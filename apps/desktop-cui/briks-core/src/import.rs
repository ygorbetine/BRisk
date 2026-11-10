//! Importação SOMENTE LEITURA dos dados do Briks antigo (Electron 1.x) para a pasta do app nativo.
//!
//! O que é importado: o SQLite do backend local (`risk.sqlite3`: contas locais, amigos, grupos, mensagens P2P, eventos de chat).
//! Nada na pasta de origem é alterado: a origem é copiada (banco + WAL) para uma pasta temporária e só a cópia é aberta.
//! O que NÃO é importado: a chave ECDSA e o peerId (CryptoKey não exportável do Chromium), a sessão do Supabase, o layout/preferências
//! do React, anexos e histórico de chamadas (ficam no IndexedDB/LocalStorage do Chromium).
//! Idempotente (marcador `import-electron.json`), com backup do banco anterior e reversão (`rollback`).
use anyhow::{anyhow, Context, Result};
use rusqlite::{backup::Backup, Connection, OpenFlags};
use serde_json::{json, Value};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

const DB: &str = "risk.sqlite3";
const MARKER: &str = "import-electron.json";
const DECLINED: &str = "import-electron.declined";
/// Tabelas de dados do usuário, em ordem de dependência. `_sqlx_migrations` nunca é copiada na mesclagem.
const TABLES: &[&str] = &[
    "users", "friend_requests", "friendships", "p2p_friends", "p2p_groups", "p2p_messages", "p2p_chat_events",
    "rooms", "room_members", "communities", "community_members", "channels", "messages",
];

pub fn default_source() -> PathBuf {
    PathBuf::from(std::env::var("APPDATA").unwrap_or_else(|_| ".".into())).join("@briks").join("desktop")
}
pub fn default_dest() -> PathBuf {
    PathBuf::from(std::env::var("APPDATA").unwrap_or_else(|_| ".".into())).join("Briks-CUI")
}

fn now_secs() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

/// impressão digital barata da origem: tamanho + mtime do banco e do WAL (para saber se a origem mudou desde a importação)
fn fingerprint(src: &Path) -> String {
    let f = |n: &str| {
        fs::metadata(src.join(n)).map(|m| format!("{}:{}", m.len(), m.modified().ok().and_then(|t| t.duration_since(UNIX_EPOCH).ok()).map(|d| d.as_secs()).unwrap_or(0))).unwrap_or_else(|_| "-".into())
    };
    format!("{}|{}", f(DB), f("risk.sqlite3-wal"))
}

fn table_exists(c: &Connection, schema: &str, t: &str) -> bool {
    c.query_row(&format!("SELECT 1 FROM {schema}.sqlite_master WHERE type='table' AND name=?1"), [t], |_| Ok(())).is_ok()
}
fn columns(c: &Connection, schema: &str, t: &str) -> Result<Vec<String>> {
    let mut st = c.prepare(&format!("SELECT name FROM {schema}.pragma_table_info('{t}')"))?;
    let v = st.query_map([], |r| r.get::<_, String>(0))?.collect::<std::result::Result<Vec<_>, _>>()?;
    Ok(v)
}
fn count(c: &Connection, schema: &str, t: &str) -> i64 {
    c.query_row(&format!("SELECT count(*) FROM {schema}.\"{t}\""), [], |r| r.get(0)).unwrap_or(0)
}

/// copia banco e WAL da origem para `tmp` (sem tocar na origem) e abre a cópia
fn open_copy(src: &Path, tmp: &Path) -> Result<Connection> {
    fs::create_dir_all(tmp)?;
    fs::copy(src.join(DB), tmp.join(DB)).context("não foi possível ler o banco do Briks antigo")?;
    let wal = src.join("risk.sqlite3-wal");
    if wal.exists() {
        let _ = fs::copy(&wal, tmp.join("risk.sqlite3-wal"));
    }
    let c = Connection::open_with_flags(tmp.join(DB), OpenFlags::SQLITE_OPEN_READ_WRITE)?;   // só a cópia; a recuperação do WAL acontece nela
    c.busy_timeout(Duration::from_secs(5))?;
    Ok(c)
}

/// Relatório sem gravar nada: o que existe na origem e o que seria importado.
pub fn probe(src: &Path, dest: &Path) -> Value {
    if dest.join(MARKER).exists() {
        return json!({ "found": false, "state": "imported" });
    }
    if dest.join(DECLINED).exists() {
        return json!({ "found": false, "state": "declined" });
    }
    if !src.join(DB).exists() {
        return json!({ "found": false, "state": "no-source" });
    }
    let tmp = dest.join("import-tmp");
    let _ = fs::remove_dir_all(&tmp);
    let res = open_copy(src, &tmp).map(|c| {
        let tables: Vec<Value> = TABLES.iter().filter(|t| table_exists(&c, "main", t)).map(|t| json!({ "table": t, "rows": count(&c, "main", t) })).collect();
        let user: Option<String> = c.query_row("SELECT display_name FROM users LIMIT 1", [], |r| r.get(0)).ok();
        (tables, user)
    });
    let _ = fs::remove_dir_all(&tmp);
    match res {
        Ok((tables, user)) => json!({ "found": true, "state": "ready", "source": src, "user": user, "tables": tables }),
        Err(e) => json!({ "found": false, "state": "error", "error": e.to_string() }),
    }
}

/// Importa. `src` nunca é escrita. Devolve o relatório (também gravado no marcador).
pub fn apply(src: &Path, dest: &Path) -> Result<Value> {
    fs::create_dir_all(dest)?;
    let fp = fingerprint(src);
    if let Ok(t) = fs::read_to_string(dest.join(MARKER)) {
        if let Ok(v) = serde_json::from_str::<Value>(&t) {
            if v["status"] == "applied" {
                return Ok(json!({ "state": "already", "imported_at": v["imported_at"], "report": v["report"] }));
            }
        }
    }
    if !src.join(DB).exists() {
        return Err(anyhow!("banco do Briks antigo não encontrado em {}", src.display()));
    }
    let tmp = dest.join("import-tmp");
    let _ = fs::remove_dir_all(&tmp);
    let srcdb = open_copy(src, &tmp)?;
    srcdb.execute_batch("PRAGMA wal_checkpoint(TRUNCATE);").ok();

    // backup do banco atual do CUI (se existir) antes de qualquer mudança
    let ts = now_secs();
    let backup_dir = dest.join("import-backup").join(ts.to_string());
    let dest_db = dest.join(DB);
    let had_dest = dest_db.exists();
    if had_dest {
        fs::create_dir_all(&backup_dir)?;
        for n in [DB, "risk.sqlite3-wal", "risk.sqlite3-shm"] {
            if dest.join(n).exists() {
                fs::copy(dest.join(n), backup_dir.join(n)).with_context(|| format!("backup de {n}"))?;
            }
        }
    }

    let mut report: Vec<Value> = vec![];
    let mode;
    if !had_dest {
        // CUI ainda sem banco: a cópia consistente da origem vira o banco (inclui o esquema e as migrações)
        let mut d = Connection::open(&dest_db)?;
        {
            let b = Backup::new(&srcdb, &mut d)?;
            b.run_to_completion(64, Duration::from_millis(5), None)?;
        }
        for t in TABLES.iter().filter(|t| table_exists(&d, "main", t)) {
            report.push(json!({ "table": t, "imported": count(&d, "main", t) }));
        }
        mode = "copy";
    } else {
        // CUI já tem banco: mescla linha a linha sem sobrescrever nada (chaves primárias iguais são mantidas como estão)
        let d = Connection::open(&dest_db)?;
        d.busy_timeout(Duration::from_secs(5))?;
        d.execute("ATTACH DATABASE ?1 AS src", [tmp.join(DB).to_string_lossy().as_ref()])?;
        d.execute_batch("BEGIN IMMEDIATE")?;
        let r: Result<()> = (|| {
            for t in TABLES {
                if !table_exists(&d, "src", t) || !table_exists(&d, "main", t) {
                    continue;
                }
                let (a, b) = (columns(&d, "main", t)?, columns(&d, "src", t)?);
                let common: Vec<String> = a.into_iter().filter(|c| b.contains(c)).map(|c| format!("\"{c}\"")).collect();
                if common.is_empty() {
                    continue;
                }
                let cols = common.join(",");
                let n = d.execute(&format!("INSERT OR IGNORE INTO main.\"{t}\" ({cols}) SELECT {cols} FROM src.\"{t}\""), [])?;
                report.push(json!({ "table": t, "imported": n, "source_rows": count(&d, "src", t) }));
            }
            // usuário atual: só adota o do Briks antigo se o CUI não tem um
            d.execute("INSERT OR IGNORE INTO main.app_state(key,value) SELECT key,value FROM src.app_state WHERE key='current_user_id'", [])?;
            Ok(())
        })();
        match r {
            Ok(()) => d.execute_batch("COMMIT")?,
            Err(e) => {
                let _ = d.execute_batch("ROLLBACK");
                let _ = fs::remove_dir_all(&tmp);
                return Err(e);
            }
        }
        mode = "merge";
    }
    drop(srcdb);
    let _ = fs::remove_dir_all(&tmp);
    let full = json!({
        "status": "applied", "version": 1, "imported_at": ts, "mode": mode, "source": src, "source_fingerprint": fp,
        "had_dest_db": had_dest, "backup": if had_dest { json!(backup_dir) } else { Value::Null }, "report": report,
        "not_imported": ["identidade ECDSA e peerId (chave não exportável do Chromium: o app nativo cria identidade nova)", "sessão do Supabase (entre de novo)", "layout e preferências do React", "anexos e histórico de chamadas (IndexedDB)"],
    });
    fs::write(dest.join(MARKER), serde_json::to_string_pretty(&full)?)?;
    Ok(full)
}

/// Desfaz a importação: restaura o banco anterior (ou remove o banco criado pela cópia) e apaga o marcador. A origem não é tocada.
pub fn rollback(dest: &Path) -> Result<Value> {
    let t = fs::read_to_string(dest.join(MARKER)).context("nada para desfazer: marcador de importação ausente")?;
    let v: Value = serde_json::from_str(&t)?;
    for n in [DB, "risk.sqlite3-wal", "risk.sqlite3-shm"] {
        let _ = fs::remove_file(dest.join(n));
    }
    if let Some(b) = v["backup"].as_str() {
        for n in [DB, "risk.sqlite3-wal", "risk.sqlite3-shm"] {
            let p = Path::new(b).join(n);
            if p.exists() {
                fs::copy(&p, dest.join(n))?;
            }
        }
    }
    fs::remove_file(dest.join(MARKER))?;
    Ok(json!({ "state": "rolled-back", "restored_backup": v["backup"] }))
}

pub fn decline(dest: &Path) -> Result<Value> {
    fs::create_dir_all(dest)?;
    fs::write(dest.join(DECLINED), "")?;
    Ok(json!({ "state": "declined" }))
}

/// `briks-core import-electron <probe|apply|rollback|decline> [--source DIR] [--dest DIR]`; imprime JSON numa linha.
pub fn cli(args: &[String]) -> i32 {
    let mut it = args.iter();
    let action = it.next().map(String::as_str).unwrap_or("probe");
    let (mut src, mut dest) = (default_source(), default_dest());
    while let Some(a) = it.next() {
        match a.as_str() {
            "--source" => src = it.next().map(PathBuf::from).unwrap_or(src),
            "--dest" => dest = it.next().map(PathBuf::from).unwrap_or(dest),
            _ => {}
        }
    }
    let r = match action {
        "probe" => Ok(probe(&src, &dest)),
        "apply" => apply(&src, &dest),
        "rollback" => rollback(&dest),
        "decline" => decline(&dest),
        o => Err(anyhow!("ação desconhecida: {o}")),
    };
    match r {
        Ok(v) => {
            println!("{v}");
            0
        }
        Err(e) => {
            println!("{}", json!({ "state": "error", "error": e.to_string() }));
            1
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn mk_source(dir: &Path) {
        fs::create_dir_all(dir).unwrap();
        let c = Connection::open(dir.join(DB)).unwrap();
        c.execute_batch(
            "CREATE TABLE users(id TEXT PRIMARY KEY, display_name TEXT, email TEXT, password_hash TEXT, created_at TEXT);
             CREATE TABLE app_state(key TEXT PRIMARY KEY, value TEXT);
             CREATE TABLE p2p_friends(owner_user_id TEXT, peer_id TEXT, display_name TEXT, public_key_json TEXT, avatar TEXT, added_at TEXT, PRIMARY KEY(owner_user_id,peer_id));
             INSERT INTO users VALUES('u1','Ygor','y@x','hash','t');
             INSERT INTO app_state VALUES('current_user_id','u1');
             INSERT INTO p2p_friends VALUES('u1','p1','Ana','{}',NULL,'t'),('u1','p2','Bia','{}',NULL,'t');",
        )
        .unwrap();
    }
    fn tmpdir(n: &str) -> PathBuf {
        let p = std::env::temp_dir().join(format!("briks-import-{n}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&p);
        fs::create_dir_all(&p).unwrap();
        p
    }
    fn q(dir: &Path, sql: &str) -> i64 {
        Connection::open(dir.join(DB)).unwrap().query_row(sql, [], |r| r.get(0)).unwrap()
    }

    #[test]
    fn copy_mode_is_read_only_idempotent_and_reversible() {
        let (s, d) = (tmpdir("s1"), tmpdir("d1"));
        mk_source(&s);
        let before = fs::read(s.join(DB)).unwrap();
        let p = probe(&s, &d);
        assert_eq!(p["found"], true);
        assert!(!d.join(DB).exists(), "probe não grava");
        let r = apply(&s, &d).unwrap();
        assert_eq!(r["mode"], "copy");
        assert_eq!(q(&d, "SELECT count(*) FROM p2p_friends"), 2);
        assert_eq!(fs::read(s.join(DB)).unwrap(), before, "origem intacta");
        assert_eq!(apply(&s, &d).unwrap()["state"], "already");
        assert_eq!(probe(&s, &d)["state"], "imported");
        rollback(&d).unwrap();
        assert!(!d.join(DB).exists() && !d.join(MARKER).exists());
        assert_eq!(fs::read(s.join(DB)).unwrap(), before);
    }

    #[test]
    fn merge_mode_keeps_existing_rows_and_restores_backup() {
        let (s, d) = (tmpdir("s2"), tmpdir("d2"));
        mk_source(&s);
        {
            let c = Connection::open(d.join(DB)).unwrap();
            c.execute_batch(
                "CREATE TABLE users(id TEXT PRIMARY KEY, display_name TEXT, email TEXT, password_hash TEXT, created_at TEXT);
                 CREATE TABLE app_state(key TEXT PRIMARY KEY, value TEXT);
                 CREATE TABLE p2p_friends(owner_user_id TEXT, peer_id TEXT, display_name TEXT, public_key_json TEXT, avatar TEXT, added_at TEXT, PRIMARY KEY(owner_user_id,peer_id));
                 INSERT INTO users VALUES('u9','Local','l@x','h','t');
                 INSERT INTO app_state VALUES('current_user_id','u9');
                 INSERT INTO p2p_friends VALUES('u1','p1','Ana editada','{}',NULL,'t');",
            )
            .unwrap();
        }
        let r = apply(&s, &d).unwrap();
        assert_eq!(r["mode"], "merge");
        assert_eq!(q(&d, "SELECT count(*) FROM users"), 2);
        assert_eq!(q(&d, "SELECT count(*) FROM p2p_friends"), 2);
        assert_eq!(q(&d, "SELECT count(*) FROM p2p_friends WHERE display_name='Ana editada'"), 1, "não sobrescreve");
        assert_eq!(q(&d, "SELECT count(*) FROM app_state WHERE value='u9'"), 1, "mantém o usuário atual do CUI");
        rollback(&d).unwrap();
        assert_eq!(q(&d, "SELECT count(*) FROM users"), 1);
        assert_eq!(q(&d, "SELECT count(*) FROM p2p_friends"), 1);
    }

    #[test]
    fn missing_source_and_decline() {
        let (s, d) = (tmpdir("s3"), tmpdir("d3"));
        assert_eq!(probe(&s, &d)["state"], "no-source");
        assert!(apply(&s, &d).is_err());
        decline(&d).unwrap();
        assert_eq!(probe(&s, &d)["state"], "declined");
    }
}
