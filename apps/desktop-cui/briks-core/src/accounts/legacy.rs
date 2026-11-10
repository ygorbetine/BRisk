//! "Importar amigos do Briks antigo": traz contatos, grupos e mensagens P2P do banco local do Briks 1.x (Electron)
//! para o usuário local da conta logada. SEM a identidade antiga: a chave e o peerId do Electron não são importados
//! (não são exportáveis) e as contas locais antigas (tabela `users`, com hash de senha) também não.
//! A pasta de origem nunca é escrita: o banco (e o WAL) é copiado para uma pasta temporária e só a cópia é aberta.
//! Complementa `import.rs` (cópia integral do banco, do instalador); aqui a importação é seletiva e reatribuída.
use anyhow::{anyhow, Context, Result};
use rusqlite::{Connection, OpenFlags};
use serde_json::{json, Value};
use std::fs;
use std::path::Path;
use std::time::Duration;

const DB: &str = "risk.sqlite3";
const MARKER: &str = "import-social.json";
/// tabelas com dono (`owner_user_id`) que fazem sentido sem a identidade antiga
const TABLES: &[&str] = &["p2p_friends", "p2p_groups", "p2p_messages"];

fn table_exists(c: &Connection, schema: &str, t: &str) -> bool {
    c.query_row(&format!("SELECT 1 FROM {schema}.sqlite_master WHERE type='table' AND name=?1"), [t], |_| Ok(())).is_ok()
}
fn columns(c: &Connection, schema: &str, t: &str) -> Result<Vec<String>> {
    let mut st = c.prepare(&format!("SELECT name FROM {schema}.pragma_table_info('{t}')"))?;
    let v = st.query_map([], |r| r.get::<_, String>(0))?.collect::<std::result::Result<Vec<_>, _>>()?;
    Ok(v)
}

fn copy_source(src: &Path, tmp: &Path) -> Result<()> {
    let _ = fs::remove_dir_all(tmp);
    fs::create_dir_all(tmp)?;
    fs::copy(src.join(DB), tmp.join(DB)).context("não foi possível ler o banco do Briks antigo")?;
    let wal = src.join("risk.sqlite3-wal");
    if wal.exists() {
        let _ = fs::copy(&wal, tmp.join("risk.sqlite3-wal"));
    }
    Ok(())
}

/// O perfil principal da origem: o dono com mais amigos e grupos (o Briks antigo tinha, na prática, uma conta local por PC).
fn main_owner(c: &Connection, schema: &str) -> Option<rusqlite::types::Value> {
    let mut parts = vec![];
    for t in ["p2p_friends", "p2p_groups"] {
        if table_exists(c, schema, t) {
            parts.push(format!("SELECT owner_user_id AS o FROM {schema}.{t}"));
        }
    }
    if parts.is_empty() {
        return None;
    }
    c.query_row(&format!("SELECT o FROM ({}) GROUP BY o ORDER BY count(*) DESC LIMIT 1", parts.join(" UNION ALL ")), [], |r| r.get(0)).ok()
}

/// O que existe para importar, sem gravar nada no destino (só a cópia temporária, apagada em seguida).
pub fn probe(src: &Path, dest: &Path) -> Value {
    if dest.join(MARKER).exists() {
        return json!({ "state": "imported" });
    }
    if !src.join(DB).exists() {
        return json!({ "state": "no-source" });
    }
    let tmp = dest.join("import-social-tmp");
    let r = copy_source(src, &tmp).and_then(|_| {
        let c = Connection::open_with_flags(tmp.join(DB), OpenFlags::SQLITE_OPEN_READ_WRITE)?;
        let owner = main_owner(&c, "main");
        let n = |t: &str| -> i64 {
            match (&owner, table_exists(&c, "main", t)) {
                (Some(o), true) => c.query_row(&format!("SELECT count(*) FROM {t} WHERE owner_user_id=?1"), [o], |r| r.get(0)).unwrap_or(0),
                _ => 0,
            }
        };
        Ok(json!({ "state": "ready", "friends": n("p2p_friends"), "groups": n("p2p_groups"), "messages": n("p2p_messages") }))
    });
    let _ = fs::remove_dir_all(&tmp);
    r.unwrap_or_else(|e| json!({ "state": "error", "error": e.to_string() }))
}

/// Importa para o usuário local cujo e-mail é `local_email` (o da ponte da conta logada). Não sobrescreve nada que já exista.
pub fn apply(src: &Path, dest: &Path, local_email: &str) -> Result<Value> {
    if let Ok(t) = fs::read_to_string(dest.join(MARKER)) {
        if let Ok(v) = serde_json::from_str::<Value>(&t) {
            return Ok(json!({ "state": "already", "report": v["report"] }));
        }
    }
    if !src.join(DB).exists() {
        return Err(anyhow!("Não encontramos o Briks antigo neste computador."));
    }
    if !dest.join(DB).exists() {
        return Err(anyhow!("O banco local do Briks ainda não foi criado. Abra o app, entre na conta e tente de novo."));
    }
    let tmp = dest.join("import-social-tmp");
    copy_source(src, &tmp)?;
    let run = || -> Result<Value> {
        let d = Connection::open(dest.join(DB))?;
        d.busy_timeout(Duration::from_secs(10))?;
        let me: rusqlite::types::Value = d.query_row("SELECT id FROM users WHERE email=?1 COLLATE NOCASE", [local_email], |r| r.get(0))
            .map_err(|_| anyhow!("O usuário local desta conta ainda não existe. Entre na conta e tente de novo."))?;
        d.execute("ATTACH DATABASE ?1 AS src", [tmp.join(DB).to_string_lossy().as_ref()])?;
        let Some(owner) = main_owner(&d, "src") else { return Ok(json!({ "friends": 0, "groups": 0, "messages": 0 })) };
        d.execute_batch("BEGIN IMMEDIATE")?;
        let r = (|| -> Result<Value> {
            let mut rep = serde_json::Map::new();
            for t in TABLES {
                if !table_exists(&d, "src", t) || !table_exists(&d, "main", t) {
                    continue;
                }
                let (a, b) = (columns(&d, "main", t)?, columns(&d, "src", t)?);
                let common: Vec<String> = a.into_iter().filter(|c| c != "owner_user_id" && b.contains(c)).map(|c| format!("\"{c}\"")).collect();
                let cols = common.join(",");
                let n = d.execute(&format!("INSERT OR IGNORE INTO main.\"{t}\" (owner_user_id,{cols}) SELECT ?1,{cols} FROM src.\"{t}\" WHERE owner_user_id=?2"), rusqlite::params![me, owner])?;
                rep.insert(t.trim_start_matches("p2p_").to_string(), json!(n));
            }
            Ok(Value::Object(rep))
        })();
        match r {
            Ok(v) => {
                d.execute_batch("COMMIT")?;
                Ok(v)
            }
            Err(e) => {
                let _ = d.execute_batch("ROLLBACK");
                Err(e)
            }
        }
    };
    let r = run();
    let _ = fs::remove_dir_all(&tmp);
    let report = r?;
    let full = json!({ "version": 1, "report": report,
        "not_imported": ["identidade e peerId do Briks antigo (o app novo usa a identidade da sua conta)", "contas locais antigas e senhas", "anexos e histórico de chamadas"] });
    fs::write(dest.join(MARKER), serde_json::to_string_pretty(&full)?)?;
    Ok(json!({ "state": "applied", "report": report }))
}
