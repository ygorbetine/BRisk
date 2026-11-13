//! Fila LOCAL de anexos das DMs (neste PC; nada vai para o servidor). Um anexo escolhido para um amigo fica aqui, só como caminho
//! + metadados (cifrados com a chave do usuário, como a caixa de saída), até o canal direto com ele autenticar; aí o core oferece o
//! arquivo pelo mesmo protocolo da sala (`file.offer` assinada, pedaços em ordem, SHA-256 no fim; ver `filetransfer.rs`). O amigo
//! precisa clicar em "Baixar" (nada é gravado antes) e o arquivo cai em quarentena (`recebidos`). Sai da fila quando o envio termina,
//! quando o amigo recusa ou o usuário cancela; se a conexão cair no meio, volta para a fila e segue na próxima vez que ele ficar online.
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock};

/// Teto por arquivo na DM (a sala continua com o limite do `filetransfer`).
pub const DM_MAX_BYTES: u64 = 2 * 1024 * 1024 * 1024;
pub const MAX_QUEUE: usize = 20;
pub const EXPIRE_MS: u64 = 30 * 24 * 3600 * 1000;

#[derive(Clone, Debug, PartialEq)]
pub struct Entry { pub qid: String, pub fp: String, pub path: String, pub name: String, pub size: u64, pub ts: u64, pub mid: Option<String> }

#[derive(Default)]
pub struct AttachQ { dir: Option<PathBuf>, items: Vec<Entry> }

fn file(d: &Path) -> PathBuf { d.join("attachq.dat") }

/// Confere o arquivo antes de entrar na fila. Erro = (código, texto em português para o usuário).
pub fn check(path: &str) -> Result<(String, u64), (&'static str, String)> {
    let p = Path::new(path);
    let meta = std::fs::metadata(p).map_err(|_| ("missing", "Não encontrei o arquivo (foi movido ou apagado?).".to_string()))?;
    if !meta.is_file() { return Err(("notfile", "Isso não é um arquivo (pastas não podem ser enviadas).".into())); }
    if meta.len() == 0 { return Err(("empty", "O arquivo está vazio.".into())); }
    if meta.len() > DM_MAX_BYTES { return Err(("toobig", format!("O arquivo tem {:.1} GB; o limite pela conversa direta é {} GB.", meta.len() as f64 / 1073741824.0, DM_MAX_BYTES / 1073741824))); }
    let name = crate::filetransfer::sanitize_name(&p.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_default());
    Ok((name, meta.len()))
}

impl AttachQ {
    pub fn open(dir: Option<PathBuf>) -> Self {
        let mut q = AttachQ { dir: dir.clone(), items: vec![] };
        if let Some(d) = dir {
            if let Ok(txt) = std::fs::read_to_string(file(&d)) {
                for line in txt.lines() {
                    let Some((json, _)) = crate::atrest::open_line(line) else { continue };
                    let Ok(v) = serde_json::from_str::<serde_json::Value>(&json) else { continue };
                    let s = |k: &str| v[k].as_str().map(String::from);
                    if let (Some(qid), Some(fp), Some(path), Some(name)) = (s("qid"), s("fp"), s("path"), s("name")) {
                        q.items.push(Entry { qid, fp, path, name, size: v["size"].as_u64().unwrap_or(0), ts: v["ts"].as_u64().unwrap_or(0), mid: None });
                    }
                }
            }
        }
        q
    }

    fn save(&self) {
        let Some(d) = &self.dir else { return };
        let mut out = String::new();
        for e in &self.items {
            let j = serde_json::json!({ "qid": e.qid, "fp": e.fp, "path": e.path, "name": e.name, "size": e.size, "ts": e.ts });
            match crate::atrest::seal_line(&j.to_string()) { Some(l) => { out.push_str(&l); out.push('\n'); } None => return }
        }
        let (f, tmp) = (file(d), d.join("attachq.tmp"));
        let _ = std::fs::create_dir_all(d);
        if std::fs::write(&tmp, out).is_ok() { let _ = std::fs::rename(&tmp, &f); }
    }

    /// Entra na fila (já conferido com `check`). Erro = texto para o usuário (fila cheia, ou o mesmo arquivo já espera esse amigo).
    pub fn add(&mut self, fp: &str, path: &str, name: &str, size: u64, now_ms: u64) -> Result<Entry, String> {
        if self.items.iter().any(|e| e.fp == fp && e.path == path) { return Err("Esse arquivo já está na fila para esse amigo.".into()); }
        if self.items.len() >= MAX_QUEUE { return Err(format!("Já há {MAX_QUEUE} anexos esperando na fila deste computador. Espere alguns seguirem ou cancele algum.")); }
        let e = Entry { qid: format!("q-{}", uuid::Uuid::new_v4().simple()), fp: fp.into(), path: path.into(), name: name.into(), size, ts: now_ms, mid: None };
        self.items.push(e.clone());
        self.save();
        Ok(e)
    }

    pub fn list(&self) -> Vec<Entry> { self.items.clone() }
    pub fn has_fp(&self, fp: &str) -> bool { self.items.iter().any(|e| e.fp == fp) }

    /// Entradas de `fp` que ainda não estão sendo oferecidas; marca como "em andamento" (mid vazio-mas-reservado) para não duplicar a oferta.
    pub fn take_ready(&mut self, fp: &str) -> Vec<Entry> {
        self.items.iter_mut().filter(|e| e.fp == fp && e.mid.is_none()).map(|e| { e.mid = Some(String::new()); e.clone() }).collect()
    }

    /// O mesh começou a oferecer `path` a `fp` com o id `mid`: guarda a ligação e devolve o qid.
    pub fn bind(&mut self, fp: &str, path: &str, mid: &str) -> Option<String> {
        let e = self.items.iter_mut().find(|e| e.fp == fp && e.path == path && e.mid.as_deref() == Some(""))?;
        e.mid = Some(mid.into());
        Some(e.qid.clone())
    }
    pub fn qid_of(&self, mid: &str) -> Option<String> { self.items.iter().find(|e| e.mid.as_deref() == Some(mid)).map(|e| e.qid.clone()) }

    /// Terminou (enviado, recusado ou cancelado): sai da fila.
    pub fn finish(&mut self, mid: &str) -> bool {
        let n = self.items.len();
        self.items.retain(|e| e.mid.as_deref() != Some(mid));
        let ch = self.items.len() != n; if ch { self.save(); } ch
    }
    pub fn remove(&mut self, qid: &str) -> Option<Entry> {
        let i = self.items.iter().position(|e| e.qid == qid)?;
        let e = self.items.remove(i); self.save(); Some(e)
    }

    /// A conexão caiu / a oferta falhou: o item volta a esperar (devolve o qid e o id antigo, se havia).
    pub fn requeue_mid(&mut self, mid: &str) -> Option<Entry> {
        let e = self.items.iter_mut().find(|e| e.mid.as_deref() == Some(mid))?;
        e.mid = None; Some(e.clone())
    }
    pub fn requeue_fp(&mut self, fp: &str) -> Vec<(Entry, String)> {
        self.items.iter_mut().filter(|e| e.fp == fp && e.mid.is_some()).map(|e| { let old = e.mid.take().unwrap_or_default(); (e.clone(), old) }).collect()
    }

    /// Vence o que passou de `EXPIRE_MS`.
    pub fn expire(&mut self, now_ms: u64) -> Vec<Entry> {
        let (keep, gone): (Vec<_>, Vec<_>) = std::mem::take(&mut self.items).into_iter().partition(|e| now_ms.saturating_sub(e.ts) < EXPIRE_MS);
        self.items = keep;
        if !gone.is_empty() { self.save(); }
        gone
    }
}

/// Instância compartilhada por pasta (uma por PC; nos testes sem pasta, uma por processo).
pub fn shared() -> Arc<Mutex<AttachQ>> {
    static MAP: OnceLock<Mutex<HashMap<String, Arc<Mutex<AttachQ>>>>> = OnceLock::new();
    let dir = crate::mesh::hist_dir();
    let key = dir.as_ref().map(|d| d.to_string_lossy().to_string()).unwrap_or_default();
    let mut m = MAP.get_or_init(Default::default).lock().unwrap();
    m.entry(key).or_insert_with(|| Arc::new(Mutex::new(AttachQ::open(dir)))).clone()
}

#[cfg(test)]
mod t {
    use super::*;

    fn tmp(tag: &str) -> PathBuf { let d = std::env::temp_dir().join(format!("briks-aq-{tag}-{}", uuid::Uuid::new_v4())); std::fs::create_dir_all(&d).unwrap(); d }

    #[test]
    fn check_rejects_missing_empty_folder_and_too_big() {
        let d = tmp("chk");
        std::fs::write(d.join("a.png"), b"x").unwrap();
        std::fs::write(d.join("v.txt"), b"").unwrap();
        assert_eq!(check(d.join("a.png").to_str().unwrap()).unwrap(), ("a.png".to_string(), 1));
        assert_eq!(check(d.join("nao.png").to_str().unwrap()).unwrap_err().0, "missing");
        assert_eq!(check(d.join("v.txt").to_str().unwrap()).unwrap_err().0, "empty");
        assert_eq!(check(d.to_str().unwrap()).unwrap_err().0, "notfile");
    }

    #[test]
    fn queue_flow_dedup_limit_bind_requeue_finish() {
        let mut q = AttachQ::open(None);
        let a = q.add("fpB", "C:/x/a.png", "a.png", 10, 1).unwrap();
        assert!(q.add("fpB", "C:/x/a.png", "a.png", 10, 2).is_err(), "o mesmo arquivo não entra duas vezes");
        assert!(q.add("fpC", "C:/x/a.png", "a.png", 10, 2).is_ok(), "mas pode ir a outro amigo");
        let r = q.take_ready("fpB");
        assert_eq!(r.len(), 1);
        assert!(q.take_ready("fpB").is_empty(), "já está sendo oferecido: não duplica a oferta");
        assert_eq!(q.bind("fpB", "C:/x/a.png", "mid-1").as_deref(), Some(a.qid.as_str()));
        assert_eq!(q.qid_of("mid-1").as_deref(), Some(a.qid.as_str()));
        // conexão caiu: volta à fila e é oferecido de novo na próxima vez
        let rq = q.requeue_fp("fpB"); assert_eq!(rq.len(), 1); assert_eq!(rq[0].1, "mid-1");
        assert_eq!(q.take_ready("fpB").len(), 1);
        q.bind("fpB", "C:/x/a.png", "mid-2");
        assert!(q.finish("mid-2") && !q.has_fp("fpB") && q.has_fp("fpC"));
        for i in 0..MAX_QUEUE { let _ = q.add("fpD", &format!("C:/x/{i}"), "n", 1, 3); }
        assert!(q.add("fpD", "C:/x/zz", "n", 1, 3).unwrap_err().contains("fila"), "fila cheia avisa");
    }

    #[test]
    fn expires_old_and_removes_by_qid() {
        let mut q = AttachQ::open(None);
        let a = q.add("fpB", "p1", "n", 1, 10).unwrap(); q.add("fpB", "p2", "n", 1, 10 + EXPIRE_MS).unwrap();
        assert_eq!(q.expire(10 + EXPIRE_MS).len(), 1);
        assert!(q.remove(&a.qid).is_none() && q.list().len() == 1);
    }

    #[cfg(windows)]
    #[test]
    fn survives_restart_and_is_encrypted() {
        let d = tmp("rst");
        { let mut q = AttachQ::open(Some(d.clone())); q.add("fpB", "C:/seg/redo.pdf", "redo.pdf", 7, 5).unwrap(); }
        assert!(!std::fs::read_to_string(file(&d)).unwrap().contains("redo.pdf"), "cifrado em repouso");
        let q = AttachQ::open(Some(d.clone()));
        assert_eq!(q.list().len(), 1); assert_eq!(q.list()[0].path, "C:/seg/redo.pdf");
        assert!(q.list()[0].mid.is_none());
        let _ = std::fs::remove_dir_all(&d);
    }
}
