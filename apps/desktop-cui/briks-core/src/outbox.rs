//! Caixa de saída PERSISTENTE das DMs (neste PC, cifrada com a chave do usuário; nada vai para o servidor).
//! Uma mensagem escrita com o amigo offline fica aqui, já assinada, até chegar o ACK dele; sobrevive a reinício e a canal direto
//! fechado. Quem entrega é o canal direto (`mesh::flush_outbox`), que reenvia sozinho enquanto houver pendência; o receptor deduplica
//! por id, então reenviar nunca duplica. Passados `EXPIRE_MS` sem entrega a mensagem vira "falhou" (o usuário pode reenviar).
//! Também lembra os ids já entregues/que falharam (para a UI refazer os tiques depois de reabrir o app).
use crate::identity::ChatWire;
use std::collections::{HashMap, VecDeque};
use std::path::PathBuf;
use std::sync::{Mutex, OnceLock};

pub const MAX_PENDING: usize = 500;
pub const MAX_KEEP: usize = 500;
pub const EXPIRE_MS: u64 = 30 * 24 * 3600 * 1000;

#[derive(Default)]
pub struct Outbox {
    dir: Option<PathBuf>,
    pending: Vec<(String, ChatWire)>, // (fp de destino, mensagem), na ordem de escrita
    delivered: VecDeque<String>,
    failed: VecDeque<String>,
}

fn file(d: &std::path::Path) -> PathBuf { d.join("outbox.dat") }

impl Outbox {
    pub fn open(dir: Option<PathBuf>) -> Self {
        let mut o = Outbox { dir: dir.clone(), ..Default::default() };
        if let Some(d) = dir {
            if let Ok(txt) = std::fs::read_to_string(file(&d)) {
                for line in txt.lines() {
                    let Some((json, _)) = crate::atrest::open_line(line) else { continue };
                    let Ok(v) = serde_json::from_str::<serde_json::Value>(&json) else { continue };
                    match v["t"].as_str() {
                        Some("p") => {
                            if let (Some(fp), Ok(m)) = (v["fp"].as_str(), serde_json::from_value::<ChatWire>(v["m"].clone())) { o.pending.push((fp.into(), m)); }
                        }
                        Some("d") => { if let Some(i) = v["id"].as_str() { o.delivered.push_back(i.into()); } }
                        Some("f") => { if let Some(i) = v["id"].as_str() { o.failed.push_back(i.into()); } }
                        _ => {}
                    }
                }
            }
        }
        o
    }

    fn save(&self) {
        let Some(d) = &self.dir else { return };
        let mut out = String::new();
        let mut put = |v: serde_json::Value| -> bool {
            match crate::atrest::seal_line(&v.to_string()) { Some(l) => { out.push_str(&l); out.push('\n'); true } None => false }
        };
        for (fp, m) in &self.pending { if !put(serde_json::json!({ "t": "p", "fp": fp, "m": m })) { return; } }
        for i in &self.delivered { put(serde_json::json!({ "t": "d", "id": i })); }
        for i in &self.failed { put(serde_json::json!({ "t": "f", "id": i })); }
        let (f, tmp) = (file(d), d.join("outbox.tmp"));
        let _ = std::fs::create_dir_all(d);
        if std::fs::write(&tmp, out).is_ok() { let _ = std::fs::rename(&tmp, &f); }
    }

    /// Entra na fila. false = esse id já está (pendente ou entregue): nada muda (idempotente). Um id que FALHOU volta para a fila
    /// (é o "reenviar" da UI: mesmo id, então o amigo nunca recebe duas vezes).
    pub fn add(&mut self, to_fp: &str, m: &ChatWire) -> bool {
        self.failed.retain(|i| i != &m.id);
        if self.is_pending(&m.id) || self.delivered.iter().any(|i| i == &m.id) { return false; }
        if self.pending.len() >= MAX_PENDING { let (_, old) = self.pending.remove(0); push_cap(&mut self.failed, old.id); }
        self.pending.push((to_fp.into(), m.clone()));
        self.save();
        true
    }
    pub fn is_pending(&self, id: &str) -> bool { self.pending.iter().any(|(_, m)| m.id == id) }

    /// ACK do amigo: sai da fila e passa a "entregue". false = não era pendente.
    pub fn ack(&mut self, id: &str) -> bool {
        let Some(p) = self.pending.iter().position(|(_, m)| m.id == id) else { return false };
        self.pending.remove(p);
        push_cap(&mut self.delivered, id.into());
        self.save();
        true
    }

    /// Pendentes para `fp`, em ordem de escrita.
    pub fn for_fp(&self, fp: &str) -> Vec<ChatWire> { self.pending.iter().filter(|(f, _)| f == fp).map(|(_, m)| m.clone()).collect() }

    /// Vence o que passou de `EXPIRE_MS`. Devolve os ids que viraram "falhou".
    pub fn expire(&mut self, now_ms: u64) -> Vec<String> {
        let (keep, gone): (Vec<_>, Vec<_>) = std::mem::take(&mut self.pending).into_iter().partition(|(_, m)| now_ms.saturating_sub(m.timestamp) < EXPIRE_MS);
        self.pending = keep;
        let ids: Vec<String> = gone.into_iter().map(|(_, m)| m.id).collect();
        for i in &ids { push_cap(&mut self.failed, i.clone()); }
        if !ids.is_empty() { self.save(); }
        ids
    }

    pub fn state(&self) -> serde_json::Value {
        serde_json::json!({
            "pending": self.pending.iter().map(|(fp, m)| serde_json::json!({ "id": m.id, "to_fp": fp })).collect::<Vec<_>>(),
            "delivered": self.delivered, "failed": self.failed,
        })
    }
}

fn push_cap(q: &mut VecDeque<String>, id: String) {
    if !q.contains(&id) { q.push_back(id); }
    while q.len() > MAX_KEEP { q.pop_front(); }
}

/// Instância compartilhada por pasta (a malha da sala e cada canal direto usam a mesma fila).
pub fn shared() -> std::sync::Arc<Mutex<Outbox>> {
    static MAP: OnceLock<Mutex<HashMap<String, std::sync::Arc<Mutex<Outbox>>>>> = OnceLock::new();
    let dir = crate::mesh::hist_dir();
    let key = dir.as_ref().map(|d| d.to_string_lossy().to_string()).unwrap_or_default();
    let mut m = MAP.get_or_init(Default::default).lock().unwrap();
    m.entry(key).or_insert_with(|| std::sync::Arc::new(Mutex::new(Outbox::open(dir)))).clone()
}

#[cfg(test)]
mod t {
    use super::*;
    use crate::identity::{self, Identity};

    fn msg(id: &Identity, n: u64, ts: u64) -> ChatWire {
        identity::sign_message(id, ChatWire { version: 2, kind: "chat.message".into(), channel_id: "dm-x".into(), id: format!("m-{n:08}"), author_peer_id: id.fingerprint(), author: "A".into(), content: "oi".into(), timestamp: ts, signature: String::new(), package: None })
    }

    #[test]
    fn idempotent_ordered_acked_and_expiring() {
        let id = Identity::generate();
        let mut o = Outbox::open(None);
        assert!(o.add("fpB", &msg(&id, 1, 10)) && o.add("fpB", &msg(&id, 2, 11)) && o.add("fpC", &msg(&id, 3, 12)));
        assert!(!o.add("fpB", &msg(&id, 1, 10)), "mesmo id não entra duas vezes");
        assert_eq!(o.for_fp("fpB").iter().map(|m| m.id.as_str()).collect::<Vec<_>>(), ["m-00000001", "m-00000002"]);
        assert!(o.ack("m-00000001") && !o.ack("m-00000001"));
        assert!(!o.add("fpB", &msg(&id, 1, 10)), "entregue também não volta");
        assert_eq!(o.state()["delivered"], serde_json::json!(["m-00000001"]));
        let gone = o.expire(11 + EXPIRE_MS); // m2 (ts 11) venceu, m3 (ts 12) não
        assert_eq!(gone, ["m-00000002"]);
        assert_eq!(o.state()["failed"], serde_json::json!(["m-00000002"]));
        assert!(o.add("fpB", &msg(&id, 2, 11)) && o.is_pending("m-00000002") && o.state()["failed"] == serde_json::json!([]), "reenviar um que falhou");
    }

    #[cfg(windows)]
    #[test]
    fn survives_restart_and_is_encrypted() {
        let id = Identity::generate();
        let dir = std::env::temp_dir().join(format!("briks-ob-{}", uuid::Uuid::new_v4()));
        {
            let mut o = Outbox::open(Some(dir.clone()));
            o.add("fpB", &msg(&id, 1, 10));
            o.add("fpB", &msg(&id, 2, 11));
            o.ack("m-00000002");
        }
        assert!(!std::fs::read_to_string(file(&dir)).unwrap().contains("fpB"));
        let o = Outbox::open(Some(dir.clone()));
        assert_eq!(o.for_fp("fpB").len(), 1);
        assert_eq!(o.state()["delivered"], serde_json::json!(["m-00000002"]));
        let _ = std::fs::remove_dir_all(&dir);
    }
}
