//! Chat da sala (aba Chat da chamada) e pedido de transmissão ("pediu para você transmitir").
//! As duas mensagens andam pelo DataChannel já autenticado de cada peer (identidade provada, DTLS): não são retransmitidas por
//! terceiros nem gravadas em disco, então quem assina é o próprio canal. Aqui ficam só as regras puras: limpeza do texto,
//! limite de frequência por remetente e a memória de "eu pedi a transmissão para X" (uma resposta sem pedido é ignorada).
use std::collections::HashMap;
use std::sync::{Mutex, OnceLock};

pub const MAX_ROOM_CHAT_CHARS: usize = crate::identity::MAX_CHAT_CHARS;
/// Chat: no máximo 12 mensagens a cada 10 s por remetente.
const CHAT_MAX: u32 = 12;
const CHAT_WIN_MS: u64 = 10_000;
/// Pedido de transmissão: 1 a cada 10 s por remetente.
const REQ_MAX: u32 = 1;
const REQ_WIN_MS: u64 = 10_000;
/// Uma resposta só vale até 2 min depois do meu pedido.
const ASK_TTL_MS: u64 = 120_000;

/// Texto pronto para exibir: sem caracteres de controle (quebra de linha vira espaço), aparado e limitado. `None` se sobrar vazio.
pub fn clean(text: &str) -> Option<String> {
    let s: String = text.chars().map(|c| if c == '\n' || c == '\r' || c == '\t' { ' ' } else { c }).filter(|c| !c.is_control()).take(MAX_ROOM_CHAT_CHARS).collect();
    let s = s.trim();
    if s.is_empty() { None } else { Some(s.to_string()) }
}

/// Identificador de mensagem aceito: letras, números, '-' e '_', até 64. Vazio = inválido.
pub fn clean_id(id: &str) -> Option<String> {
    let s: String = id.chars().filter(|c| c.is_ascii_alphanumeric() || *c == '-' || *c == '_').take(64).collect();
    if s.is_empty() || s.len() != id.chars().count().min(64) { None } else { Some(s) }
}

/// Janela fixa por chave: até `max` eventos a cada `win_ms`.
#[derive(Default)]
pub struct Limiter { m: HashMap<String, (u64, u32)> }
impl Limiter {
    pub fn allow(&mut self, key: &str, now: u64, win_ms: u64, max: u32) -> bool {
        if self.m.len() > 4096 { self.m.clear(); }
        let e = self.m.entry(key.to_string()).or_insert((now, 0));
        if now.saturating_sub(e.0) >= win_ms { *e = (now, 0); }
        if e.1 >= max { return false; }
        e.1 += 1;
        true
    }
}

fn limiter() -> &'static Mutex<Limiter> { static L: OnceLock<Mutex<Limiter>> = OnceLock::new(); L.get_or_init(Default::default) }
fn asked() -> &'static Mutex<HashMap<String, u64>> { static A: OnceLock<Mutex<HashMap<String, u64>>> = OnceLock::new(); A.get_or_init(Default::default) }

/// `local` e `peer` são os peerIds desta sessão (a chave inclui os dois: vários nós no mesmo processo, como nos testes, não se misturam).
pub fn allow_chat(local: &str, peer: &str, now: u64) -> bool { limiter().lock().unwrap().allow(&format!("c|{local}|{peer}"), now, CHAT_WIN_MS, CHAT_MAX) }
pub fn allow_request(local: &str, peer: &str, now: u64) -> bool { limiter().lock().unwrap().allow(&format!("r|{local}|{peer}"), now, REQ_WIN_MS, REQ_MAX) }

/// Registrei um pedido de transmissão para `peer`.
pub fn mark_asked(local: &str, peer: &str, now: u64) {
    let mut a = asked().lock().unwrap();
    if a.len() > 1024 { a.clear(); }
    a.insert(format!("{local}|{peer}"), now);
}
/// Chegou uma resposta de `peer`: vale só se eu pedi há pouco (e consome o pedido).
pub fn take_asked(local: &str, peer: &str, now: u64) -> bool {
    match asked().lock().unwrap().remove(&format!("{local}|{peer}")) { Some(t) => now.saturating_sub(t) <= ASK_TTL_MS, None => false }
}

#[cfg(test)]
mod t {
    use super::*;

    #[test]
    fn clean_strips_controls_trims_and_caps() {
        assert_eq!(clean("  oi\n tudo\tbem \u{7}"), Some("oi  tudo bem".into()));
        assert_eq!(clean(" \n\t "), None);
        assert_eq!(clean(""), None);
        let long: String = "á".repeat(MAX_ROOM_CHAT_CHARS + 50);
        assert_eq!(clean(&long).unwrap().chars().count(), MAX_ROOM_CHAT_CHARS);
    }

    #[test]
    fn clean_id_rejects_odd_characters() {
        assert_eq!(clean_id("abc-123_X"), Some("abc-123_X".into()));
        assert_eq!(clean_id(""), None);
        assert_eq!(clean_id("a b"), None);
        assert_eq!(clean_id("../x"), None);
    }

    #[test]
    fn limiter_blocks_bursts_and_reopens_after_the_window() {
        let mut l = Limiter::default();
        for _ in 0..3 { assert!(l.allow("k", 1000, 10_000, 3)); }
        assert!(!l.allow("k", 1500, 10_000, 3), "o 4º dentro da janela passa");
        assert!(l.allow("outro", 1500, 10_000, 3), "chaves diferentes não se misturam");
        assert!(l.allow("k", 11_000, 10_000, 3), "janela nova não reabriu");
    }

    #[test]
    fn chat_and_request_limits_are_per_sender() {
        let (me, p) = ("eu-lim", "peer-lim");
        for i in 0..CHAT_MAX { assert!(allow_chat(me, p, 100 + i as u64)); }
        assert!(!allow_chat(me, p, 200));
        assert!(allow_chat(me, "peer-lim-2", 200));
        assert!(allow_request(me, p, 100));
        assert!(!allow_request(me, p, 5_000), "segundo pedido em menos de 10 s");
        assert!(allow_request(me, p, 100 + REQ_WIN_MS));
    }

    #[test]
    fn reply_only_counts_after_my_request_and_once() {
        let (me, p) = ("eu-ask", "peer-ask");
        assert!(!take_asked(me, p, 10), "resposta sem pedido");
        mark_asked(me, p, 1000);
        assert!(take_asked(me, p, 2000));
        assert!(!take_asked(me, p, 2500), "a mesma resposta valeu duas vezes");
        mark_asked(me, p, 1000);
        assert!(!take_asked(me, p, 1000 + ASK_TTL_MS + 1), "resposta velha demais");
    }
}
