//! Histórico de mensagens entre peers (paridade com HistoryService.ts do Electron): cada DM assinada, enviada ou recebida, é guardada
//! por canal; ao autenticar, um peer pede ao outro o que lhe falta (`chat.history.request` com os ids que já tem) e recebe o resto em
//! `chat.history.chunk` + `chat.history.complete`. Nada do que chega é confiado: cada mensagem é verificada pela assinatura do autor.
use crate::identity::ChatWire;
use std::collections::{HashMap, HashSet};
use std::io::Write;
use std::path::PathBuf;

/// Ids que um pedido anuncia como já conhecidos (as mais recentes) e tamanho máximo de uma página de resposta.
pub const MAX_KNOWN: usize = 200;
pub const PAGE: usize = 200;
/// Teto por canal guardado em memória/disco (descarta as mais antigas).
pub const MAX_PER_CHANNEL: usize = 2000;
/// Cada chunk cabe com folga no limite de 64 KiB de uma mensagem do DataChannel.
pub const CHUNK_BYTES: usize = 40 * 1024;
pub const MAX_CHUNK_MESSAGES: usize = 50;

pub struct Store {
    by_channel: HashMap<String, Vec<ChatWire>>,   // ordenado por (timestamp, id)
    ids: HashSet<String>,
    dir: Option<PathBuf>,
}

fn file_for(dir: &std::path::Path, channel: &str) -> PathBuf {
    let safe: String = channel.chars().filter(|c| c.is_ascii_alphanumeric() || *c == '-' || *c == '_').take(100).collect();
    dir.join(format!("{safe}.jsonl"))
}

impl Store {
    /// `dir` = onde guardar (um .jsonl por canal); None = só memória. Linhas inválidas são ignoradas.
    pub fn open(dir: Option<PathBuf>) -> Self {
        let mut s = Store { by_channel: HashMap::new(), ids: HashSet::new(), dir: dir.clone() };
        if let Some(d) = dir {
            let _ = std::fs::create_dir_all(&d);
            if let Ok(rd) = std::fs::read_dir(&d) {
                for e in rd.flatten() {
                    let p = e.path();
                    if p.extension().and_then(|x| x.to_str()) != Some("jsonl") { continue; }
                    let Ok(txt) = std::fs::read_to_string(&p) else { continue };
                    for line in txt.lines() { if let Ok(m) = serde_json::from_str::<ChatWire>(line) { s.insert_mem(m); } }
                }
            }
        }
        s
    }

    fn insert_mem(&mut self, m: ChatWire) -> bool {
        if !self.ids.insert(m.id.clone()) { return false; }
        let v = self.by_channel.entry(m.channel_id.clone()).or_default();
        let pos = v.partition_point(|x| (x.timestamp, &x.id) <= (m.timestamp, &m.id));
        v.insert(pos, m);
        if v.len() > MAX_PER_CHANNEL { let old = v.remove(0); self.ids.remove(&old.id); }
        true
    }

    /// Guarda uma mensagem já verificada. false = já existia.
    pub fn add(&mut self, m: &ChatWire) -> bool {
        if !self.insert_mem(m.clone()) { return false; }
        if let Some(d) = &self.dir {
            if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(file_for(d, &m.channel_id)) {
                if let Ok(line) = serde_json::to_string(m) { let _ = writeln!(f, "{line}"); }
            }
        }
        true
    }

    pub fn has(&self, id: &str) -> bool { self.ids.contains(id) }
    pub fn count(&self, channel: &str) -> usize { self.by_channel.get(channel).map_or(0, |v| v.len()) }

    /// Os ids mais recentes do canal (o que o pedido diz já ter).
    pub fn known_ids(&self, channel: &str) -> Vec<String> {
        self.by_channel.get(channel).map(|v| v.iter().rev().take(MAX_KNOWN).map(|m| m.id.clone()).collect()).unwrap_or_default()
    }

    /// Até `PAGE` mensagens mais recentes do canal que o outro lado ainda não tem, em ordem cronológica.
    pub fn missing(&self, channel: &str, known: &HashSet<String>) -> Vec<ChatWire> {
        let Some(v) = self.by_channel.get(channel) else { return Vec::new() };
        let mut out: Vec<ChatWire> = v.iter().rev().filter(|m| !known.contains(&m.id)).take(PAGE).cloned().collect();
        out.reverse();
        out
    }
}

/// Divide em chunks que cabem em `CHUNK_BYTES` (e em `MAX_CHUNK_MESSAGES` mensagens), sem perder nenhuma.
pub fn chunks(msgs: Vec<ChatWire>) -> Vec<Vec<ChatWire>> {
    let mut out: Vec<Vec<ChatWire>> = Vec::new();
    let mut cur: Vec<ChatWire> = Vec::new();
    let mut size = 0usize;
    for m in msgs {
        let n = serde_json::to_string(&m).map_or(0, |s| s.len());
        if !cur.is_empty() && (size + n > CHUNK_BYTES || cur.len() >= MAX_CHUNK_MESSAGES) { out.push(std::mem::take(&mut cur)); size = 0; }
        size += n; cur.push(m);
    }
    if !cur.is_empty() { out.push(cur); }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::identity::{self, Identity};

    fn msg(id: &Identity, ch: &str, n: u64, text: &str) -> ChatWire {
        identity::sign_message(id, ChatWire { version: 2, kind: "chat.message".into(), channel_id: ch.into(), id: format!("m-{n:08}"), author_peer_id: id.fingerprint(), author: "A".into(), content: text.into(), timestamp: 1000 + n, signature: String::new() })
    }

    #[test]
    fn store_dedupes_orders_and_reports_what_is_missing() {
        let id = Identity::generate();
        let mut s = Store::open(None);
        for n in [3, 1, 2] { assert!(s.add(&msg(&id, "dm-x", n, "oi"))); }
        assert!(!s.add(&msg(&id, "dm-x", 2, "oi")), "mesmo id não entra duas vezes");
        assert_eq!(s.count("dm-x"), 3);
        let known: HashSet<String> = ["m-00000002".to_string()].into();
        let miss = s.missing("dm-x", &known);
        assert_eq!(miss.iter().map(|m| m.id.as_str()).collect::<Vec<_>>(), ["m-00000001", "m-00000003"], "ordem cronológica, sem o que o outro já tem");
        assert!(s.missing("dm-outro", &HashSet::new()).is_empty());
        assert_eq!(s.known_ids("dm-x").first().map(String::as_str), Some("m-00000003"), "mais recentes primeiro");
    }

    #[test]
    fn store_is_capped_and_pages_are_bounded() {
        let id = Identity::generate();
        let mut s = Store::open(None);
        for n in 0..(MAX_PER_CHANNEL as u64 + 50) { s.add(&msg(&id, "dm-x", n, "x")); }
        assert_eq!(s.count("dm-x"), MAX_PER_CHANNEL);
        assert!(!s.has("m-00000000") && s.has(&format!("m-{:08}", MAX_PER_CHANNEL + 49)), "descarta as mais antigas");
        assert_eq!(s.missing("dm-x", &HashSet::new()).len(), PAGE);
        assert_eq!(s.known_ids("dm-x").len(), MAX_KNOWN);
    }

    #[test]
    fn chunks_respect_size_and_count_and_keep_everything() {
        let id = Identity::generate();
        let big = "y".repeat(3000);
        let msgs: Vec<ChatWire> = (0..60).map(|n| msg(&id, "dm-x", n, &big)).collect();
        let total = msgs.len();
        let cs = chunks(msgs);
        assert!(cs.len() > 1 && cs.iter().all(|c| c.len() <= MAX_CHUNK_MESSAGES && serde_json::to_string(c).unwrap().len() <= CHUNK_BYTES + 2000));
        assert_eq!(cs.iter().map(Vec::len).sum::<usize>(), total);
        let small: Vec<ChatWire> = (0..120).map(|n| msg(&id, "dm-x", n, "oi")).collect();
        assert!(chunks(small).iter().all(|c| c.len() <= MAX_CHUNK_MESSAGES));
        assert!(chunks(Vec::new()).is_empty());
    }

    #[test]
    fn store_persists_to_disk_and_ignores_garbage_lines() {
        let id = Identity::generate();
        let dir = std::env::temp_dir().join(format!("briks-hist-{}", uuid::Uuid::new_v4()));
        { let mut s = Store::open(Some(dir.clone())); s.add(&msg(&id, "dm-persist", 1, "primeira")); s.add(&msg(&id, "dm-persist", 2, "segunda")); }
        let f = file_for(&dir, "dm-persist");
        { let mut o = std::fs::OpenOptions::new().append(true).open(&f).unwrap(); writeln!(o, "isto não é json").unwrap(); }
        let s2 = Store::open(Some(dir.clone()));
        assert_eq!(s2.count("dm-persist"), 2);
        assert!(s2.has("m-00000001") && s2.has("m-00000002"));
        let _ = std::fs::remove_dir_all(&dir);
    }
}
