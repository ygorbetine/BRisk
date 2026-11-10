//! Eventos de chat assinados (protocolo v3 `chat.event`): resposta, edição, exclusão, reação e fixação.
//! Formato idêntico ao do cliente TypeScript (apps/web/src/chat/MessageProtocol.ts) e à tabela `p2p_chat_events`
//! do backend local: o JSON canônico assinado tem a mesma ordem de chaves, então um peer web/antigo verifica o que o
//! core assina e vice-versa.
//!
//! Compatibilidade: um evento de versão/tipo/ação desconhecida NÃO é erro — `parse_wire` devolve `Parsed::Ignored` e o
//! peer segue funcionando (peers antigos descartam o que não entendem). Só o que é `chat.event` v3 com ação conhecida
//! e formato quebrado vira `Parsed::Invalid`.
//!
//! Regras de permissão (aplicadas na projeção, `project`):
//!   reply / edit  -> só o autor da mensagem;
//!   delete        -> o autor; em grupo, também o dono e administradores (extensão: peers antigos só aceitam o autor);
//!   pin / unpin   -> DM: qualquer um dos dois; grupo: dono e administradores;
//!   reaction.*    -> qualquer pessoa presente na conversa (um peer só mexe na própria reação).
//! Ordem: eventos são ordenados por (timestamp, id); `delete` é definitivo (edições depois dele são ignoradas).
#![allow(dead_code)]

use std::collections::{BTreeMap, BTreeSet};

use anyhow::{anyhow, Result};
use serde_json::{json, Value};

use crate::identity::{verify, Identity, PublicJwk, MAX_CHAT_CHARS};

pub const EVENT_VERSION: u32 = 3;
pub const CLOCK_SKEW_MS: u64 = 5 * 60_000;
pub const MAX_EMOJI_CHARS: usize = 16;
/// Capacidades anunciadas na negociação (mesmos nomes do cliente web).
pub const CAPABILITIES: &[&str] = &["chat-events-v1", "chat-replies-v1", "message-edit-v1", "message-delete-v1", "message-reactions-v1", "message-pins-v1"];

#[derive(Debug, Clone, PartialEq)]
pub struct ChatEvent {
    pub channel_id: String,
    pub id: String,
    pub target_message_id: String,
    pub actor_peer_id: String,
    pub action: String,
    pub content: Option<String>,
    pub reference_message_id: Option<String>,
    pub emoji: Option<String>,
    pub timestamp: u64,
    pub signature: String,
}

pub const ACTIONS: &[&str] = &["reply", "edit", "delete", "reaction.add", "reaction.remove", "pin", "unpin"];

/// JSON canônico assinado (ordem e `null` iguais a `canonicalChatEvent` do TypeScript).
pub fn canonical(e: &ChatEvent) -> String {
    json!({
        "version": EVENT_VERSION, "type": "chat.event", "channelId": e.channel_id, "id": e.id,
        "targetMessageId": e.target_message_id, "actorPeerId": e.actor_peer_id, "action": e.action,
        "content": e.content, "referenceMessageId": e.reference_message_id, "emoji": e.emoji, "timestamp": e.timestamp,
    })
    .to_string()
}

/// Forma de fio (com `signature`), campos opcionais omitidos como no cliente web.
pub fn to_wire(e: &ChatEvent) -> Value {
    let mut v = json!({
        "version": EVENT_VERSION, "type": "chat.event", "channelId": e.channel_id, "id": e.id,
        "targetMessageId": e.target_message_id, "actorPeerId": e.actor_peer_id, "action": e.action,
    });
    if let Some(c) = &e.content { v["content"] = json!(c); }
    if let Some(r) = &e.reference_message_id { v["referenceMessageId"] = json!(r); }
    if let Some(m) = &e.emoji { v["emoji"] = json!(m); }
    v["timestamp"] = json!(e.timestamp);
    v["signature"] = json!(e.signature);
    v
}

pub fn new_id() -> String { uuid::Uuid::new_v4().to_string() }

/// Cria e assina um evento. `last_ts` mantém o relógio do autor monotônico (`max(agora, ultimo+1)`).
pub fn create(id: &Identity, channel: &str, target: &str, actor_peer: &str, action: &str, content: Option<&str>, reference: Option<&str>, emoji: Option<&str>, now_ms: u64, last_ts: &mut u64) -> Result<ChatEvent> {
    let ts = now_ms.max(*last_ts + 1);
    let mut e = ChatEvent {
        channel_id: channel.into(), id: new_id(), target_message_id: target.into(), actor_peer_id: actor_peer.into(), action: action.into(),
        content: content.map(Into::into), reference_message_id: reference.map(Into::into), emoji: emoji.map(|m| m.trim().to_string()), timestamp: ts, signature: String::new(),
    };
    check_payload(&e)?;
    e.signature = id.sign(&canonical(&e));
    *last_ts = ts;
    Ok(e)
}

fn valid_id(s: &str) -> bool { (8..=64).contains(&s.len()) && s.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_') }

/// Forma do payload por ação (as mesmas regras do `save_chat_event` do backend local).
pub fn check_payload(e: &ChatEvent) -> Result<()> {
    if !valid_id(&e.id) || !valid_id(&e.target_message_id) || !valid_id(&e.actor_peer_id) || e.timestamp == 0 { return Err(anyhow!("ids/relógio inválidos")); }
    let (c, r, m) = (e.content.as_deref(), e.reference_message_id.as_deref(), e.emoji.as_deref());
    let ok = match e.action.as_str() {
        "edit" => c.is_some_and(|t| !t.trim().is_empty() && t.chars().count() <= MAX_CHAT_CHARS) && r.is_none() && m.is_none(),
        "reply" => r.is_some_and(valid_id) && c.is_none() && m.is_none(),
        "reaction.add" | "reaction.remove" => m.is_some_and(|t| !t.trim().is_empty() && t.chars().count() <= MAX_EMOJI_CHARS) && c.is_none() && r.is_none(),
        "delete" | "pin" | "unpin" => c.is_none() && r.is_none() && m.is_none(),
        _ => false,
    };
    if ok { Ok(()) } else { Err(anyhow!("payload inválido para a ação {}", e.action)) }
}

#[derive(Debug, PartialEq)]
pub enum Parsed {
    Event(ChatEvent),
    /// Versão, tipo ou ação que este peer não conhece: descartado em silêncio (compatibilidade).
    Ignored(&'static str),
    /// `chat.event` v3 de ação conhecida, mas malformado.
    Invalid(String),
}

pub fn parse_wire(raw: &str) -> Parsed {
    let Ok(v) = serde_json::from_str::<Value>(raw) else { return Parsed::Invalid("JSON".into()) };
    if v["type"] != "chat.event" { return Parsed::Ignored("tipo desconhecido"); }
    if v["version"].as_u64() != Some(EVENT_VERSION as u64) { return Parsed::Ignored("versão desconhecida"); }
    let Some(action) = v["action"].as_str() else { return Parsed::Invalid("ação ausente".into()) };
    if !ACTIONS.contains(&action) { return Parsed::Ignored("ação desconhecida"); }
    let s = |k: &str| v[k].as_str().map(String::from);
    let (Some(channel_id), Some(id), Some(target), Some(actor), Some(ts), Some(sig)) =
        (s("channelId"), s("id"), s("targetMessageId"), s("actorPeerId"), v["timestamp"].as_u64(), s("signature")) else { return Parsed::Invalid("campos obrigatórios".into()) };
    if !(16..=256).contains(&sig.len()) || !sig.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_') { return Parsed::Invalid("assinatura mal formada".into()); }
    let e = ChatEvent { channel_id, id, target_message_id: target, actor_peer_id: actor, action: action.into(), content: s("content"), reference_message_id: s("referenceMessageId"), emoji: s("emoji"), timestamp: ts, signature: sig };
    match check_payload(&e) { Ok(()) => Parsed::Event(e), Err(x) => Parsed::Invalid(x.to_string()) }
}

/// Verifica canal, relógio, formato e assinatura. `sender` é a identidade já confiada para `actor_peer_id`.
pub fn verify_event(sender: &PublicJwk, e: &ChatEvent, expected_channel: &str, now_ms: u64) -> Result<()> {
    if e.channel_id != expected_channel { return Err(anyhow!("canal inesperado")); }
    if e.timestamp > now_ms + CLOCK_SKEW_MS { return Err(anyhow!("relógio do evento no futuro")); }
    check_payload(e)?;
    if !verify(sender, &canonical(e), &e.signature) { return Err(anyhow!("assinatura inválida")); }
    Ok(())
}

/// Quem pode o quê numa conversa.
#[derive(Debug, Default, Clone)]
pub struct Perms {
    pub group: bool,
    pub owner: Option<String>,
    pub admins: BTreeSet<String>,
}
impl Perms {
    pub fn dm() -> Self { Perms::default() }
    pub fn group(owner: &str, admins: &[&str]) -> Self { Perms { group: true, owner: Some(owner.into()), admins: admins.iter().map(|s| s.to_string()).collect() } }
    fn moderator(&self, peer: &str) -> bool { self.owner.as_deref() == Some(peer) || self.admins.contains(peer) }
    /// O evento é permitido para `actor` sobre uma mensagem de `author`?
    pub fn allows(&self, action: &str, actor: &str, author: Option<&str>) -> bool {
        let is_author = author == Some(actor);
        match action {
            "reply" | "edit" => is_author,
            "delete" => is_author || (self.group && self.moderator(actor)),
            "pin" | "unpin" => !self.group || self.moderator(actor),
            "reaction.add" | "reaction.remove" => true,
            _ => false,
        }
    }
}

#[derive(Debug, Default, PartialEq)]
pub struct Projection {
    pub reply_to: Option<String>,
    pub edited: Option<(String, u64)>,
    pub deleted_at: Option<u64>,
    pub deleted_by: Option<String>,
    pub pinned: Option<(u64, String)>,
    pub reactions: BTreeMap<String, BTreeSet<String>>,
    pub rejected: usize,
}

/// Aplica os eventos de UMA mensagem na ordem (timestamp, id), descartando os que o ator não pode fazer. Idempotente
/// (ids repetidos contam uma vez) e independente da ordem de chegada.
pub fn project(author: Option<&str>, perms: &Perms, events: &[ChatEvent]) -> Projection {
    let mut evs: Vec<&ChatEvent> = events.iter().collect();
    evs.sort_by(|a, b| (a.timestamp, &a.id).cmp(&(b.timestamp, &b.id)));
    evs.dedup_by(|a, b| a.id == b.id);
    let mut p = Projection::default();
    for e in evs {
        if p.deleted_at.is_some() && e.action != "reaction.remove" && e.action != "delete" { p.rejected += 1; continue; }
        if !perms.allows(&e.action, &e.actor_peer_id, author) { p.rejected += 1; continue; }
        match e.action.as_str() {
            "reply" => p.reply_to = e.reference_message_id.clone(),
            "edit" => p.edited = e.content.clone().map(|c| (c, e.timestamp)),
            "delete" => if p.deleted_at.is_none() { p.deleted_at = Some(e.timestamp); p.deleted_by = Some(e.actor_peer_id.clone()); },
            "pin" => p.pinned = Some((e.timestamp, e.actor_peer_id.clone())),
            "unpin" => p.pinned = None,
            "reaction.add" => { p.reactions.entry(e.emoji.clone().unwrap_or_default()).or_default().insert(e.actor_peer_id.clone()); }
            "reaction.remove" => { if let Some(s) = p.reactions.get_mut(e.emoji.as_deref().unwrap_or("")) { s.remove(&e.actor_peer_id); } }
            _ => {}
        }
    }
    p.reactions.retain(|_, s| !s.is_empty());
    p
}

#[cfg(test)]
mod tests {
    use super::*;

    const CH: &str = "dm-canal-de-teste-0001";
    const MSG: &str = "msg-0000-0001";
    const A: &str = "peer-aaaa-0001";
    const B: &str = "peer-bbbb-0002";
    const ADM: &str = "peer-admin-0003";

    fn ev(id: &Identity, actor: &str, action: &str, c: Option<&str>, r: Option<&str>, m: Option<&str>, ts: &mut u64, now: u64) -> ChatEvent {
        create(id, CH, MSG, actor, action, c, r, m, now, ts).unwrap()
    }

    #[test]
    fn canonical_matches_typescript_key_order_and_nulls() {
        let e = ChatEvent { channel_id: "c".into(), id: "i".into(), target_message_id: "t".into(), actor_peer_id: "a".into(), action: "delete".into(), content: None, reference_message_id: None, emoji: None, timestamp: 5, signature: String::new() };
        assert_eq!(canonical(&e), r#"{"version":3,"type":"chat.event","channelId":"c","id":"i","targetMessageId":"t","actorPeerId":"a","action":"delete","content":null,"referenceMessageId":null,"emoji":null,"timestamp":5}"#);
    }

    #[test]
    fn sign_verify_roundtrip_and_tamper() {
        let id = Identity::generate();
        let mut last = 0;
        let e = ev(&id, A, "edit", Some("novo texto"), None, None, &mut last, 1_000_000);
        assert!(verify_event(&id.public_jwk(), &e, CH, 1_000_000).is_ok());
        let mut t = e.clone(); t.content = Some("outro".into());
        assert!(verify_event(&id.public_jwk(), &t, CH, 1_000_000).is_err(), "conteúdo adulterado");
        let mut t = e.clone(); t.actor_peer_id = B.into();
        assert!(verify_event(&id.public_jwk(), &t, CH, 1_000_000).is_err(), "ator adulterado");
        let other = Identity::generate();
        assert!(verify_event(&other.public_jwk(), &e, CH, 1_000_000).is_err(), "chave de outra pessoa");
        assert!(verify_event(&id.public_jwk(), &e, "dm-outro-canal-0002", 1_000).is_err(), "canal trocado");
        assert!(verify_event(&id.public_jwk(), &e, CH, 1_000_000 - CLOCK_SKEW_MS - 5).is_err(), "evento do futuro");
    }

    #[test]
    fn wire_roundtrip_and_clock_is_monotonic() {
        let id = Identity::generate();
        let mut last = 0;
        let a = ev(&id, A, "pin", None, None, None, &mut last, 10);
        let b = ev(&id, A, "unpin", None, None, None, &mut last, 10);
        assert!(b.timestamp > a.timestamp);
        let raw = to_wire(&a).to_string();
        assert_eq!(parse_wire(&raw), Parsed::Event(a));
    }

    #[test]
    fn payload_shapes_are_enforced() {
        let id = Identity::generate();
        let mut last = 0;
        assert!(create(&id, CH, MSG, A, "edit", Some("  "), None, None, 1, &mut last).is_err());
        assert!(create(&id, CH, MSG, A, "reaction.add", None, None, Some(""), 1, &mut last).is_err());
        assert!(create(&id, CH, MSG, A, "reaction.add", None, None, Some("12345678901234567"), 1, &mut last).is_err());
        assert!(create(&id, CH, MSG, A, "reply", None, Some("curto"), None, 1, &mut last).is_err());
        assert!(create(&id, CH, MSG, A, "delete", Some("x"), None, None, 1, &mut last).is_err());
        assert!(create(&id, CH, MSG, A, "inventada", None, None, None, 1, &mut last).is_err());
        assert!(create(&id, CH, MSG, A, "reaction.add", None, None, Some("👍"), 1, &mut last).is_ok());
    }

    #[test]
    fn unknown_versions_types_and_actions_are_ignored_not_errors() {
        assert!(matches!(parse_wire(r#"{"version":4,"type":"chat.event","action":"edit"}"#), Parsed::Ignored(_)));
        assert!(matches!(parse_wire(r#"{"version":3,"type":"chat.event.novo","action":"edit"}"#), Parsed::Ignored(_)));
        assert!(matches!(parse_wire(r#"{"version":3,"type":"chat.event","action":"thread.create"}"#), Parsed::Ignored(_)));
        assert!(matches!(parse_wire(r#"{"version":2,"type":"chat.message"}"#), Parsed::Ignored(_)));
        assert!(matches!(parse_wire(r#"{"version":3,"type":"chat.event","action":"edit"}"#), Parsed::Invalid(_)));
        assert!(matches!(parse_wire("não é json"), Parsed::Invalid(_)));
        // campos extras de versões futuras não quebram um evento v3 conhecido
        let id = Identity::generate();
        let mut last = 0;
        let mut w = to_wire(&ev(&id, A, "pin", None, None, None, &mut last, 9));
        w["extraFuturo"] = json!({"x": 1});
        assert!(matches!(parse_wire(&w.to_string()), Parsed::Event(_)));
    }

    #[test]
    fn dm_permissions_only_author_edits_and_deletes() {
        let ida = Identity::generate();
        let idb = Identity::generate();
        let (mut la, mut lb) = (0, 0);
        let p = Perms::dm();
        let by_b = vec![ev(&idb, B, "edit", Some("hack"), None, None, &mut lb, 10), ev(&idb, B, "delete", None, None, None, &mut lb, 11), ev(&idb, B, "reply", None, Some("msg-ref-0001"), None, &mut lb, 12)];
        let r = project(Some(A), &p, &by_b);
        assert_eq!((r.edited, r.deleted_at, r.reply_to, r.rejected), (None, None, None, 3));
        let by_a = vec![ev(&ida, A, "edit", Some("certo"), None, None, &mut la, 20), ev(&ida, A, "reply", None, Some("msg-ref-0001"), None, &mut la, 21)];
        let r = project(Some(A), &p, &by_a);
        assert_eq!(r.edited.as_ref().map(|e| e.0.as_str()), Some("certo"));
        assert_eq!(r.reply_to.as_deref(), Some("msg-ref-0001"));
        // fixar em DM: qualquer um dos dois
        let r = project(Some(A), &p, &[ev(&idb, B, "pin", None, None, None, &mut lb, 30)]);
        assert_eq!(r.pinned.map(|p| p.1), Some(B.to_string()));
    }

    #[test]
    fn group_admin_can_delete_and_pin_but_not_edit() {
        let idm = Identity::generate();
        let idb = Identity::generate();
        let (mut lm, mut lb) = (0, 0);
        let p = Perms::group(ADM, &[]);
        let r = project(Some(A), &p, &[ev(&idm, ADM, "delete", None, None, None, &mut lm, 5), ev(&idm, ADM, "edit", Some("x"), None, None, &mut lm, 6)]);
        assert_eq!(r.deleted_by.as_deref(), Some(ADM));
        assert!(r.edited.is_none());
        // membro comum: não apaga mensagem alheia nem fixa
        let r = project(Some(A), &p, &[ev(&idb, B, "delete", None, None, None, &mut lb, 7), ev(&idb, B, "pin", None, None, None, &mut lb, 8)]);
        assert_eq!((r.deleted_at, r.pinned, r.rejected), (None, None, 2));
        // administrador (não dono) também modera
        let p2 = Perms::group(A, &[ADM]);
        assert!(project(Some(B), &p2, &[ev(&idm, ADM, "pin", None, None, None, &mut lm, 40)]).pinned.is_some());
    }

    #[test]
    fn reactions_are_per_peer_idempotent_and_order_independent() {
        let ida = Identity::generate();
        let idb = Identity::generate();
        let (mut la, mut lb) = (0, 0);
        let p = Perms::dm();
        let e1 = ev(&ida, A, "reaction.add", None, None, Some("👍"), &mut la, 1);
        let e2 = ev(&idb, B, "reaction.add", None, None, Some("👍"), &mut lb, 2);
        let e3 = ev(&ida, A, "reaction.remove", None, None, Some("👍"), &mut la, 3);
        let fwd = project(Some(A), &p, &[e1.clone(), e2.clone(), e3.clone(), e1.clone()]);
        let rev = project(Some(A), &p, &[e3, e2, e1]);
        assert_eq!(fwd, rev);
        assert_eq!(fwd.reactions.get("👍").map(|s| s.len()), Some(1));
        assert!(fwd.reactions["👍"].contains(B));
    }

    #[test]
    fn delete_is_final_and_blocks_later_edits_and_pins() {
        let id = Identity::generate();
        let mut l = 0;
        let r = project(Some(A), &Perms::dm(), &[ev(&id, A, "delete", None, None, None, &mut l, 5), ev(&id, A, "edit", Some("volta"), None, None, &mut l, 6), ev(&id, A, "pin", None, None, None, &mut l, 7)]);
        assert!(r.deleted_at.is_some() && r.edited.is_none() && r.pinned.is_none() && r.rejected == 2);
    }

    #[test]
    fn edit_conflicts_resolve_by_timestamp_then_id() {
        let id = Identity::generate();
        let mut l = 0;
        let a = ev(&id, A, "edit", Some("primeira"), None, None, &mut l, 100);
        let b = ev(&id, A, "edit", Some("segunda"), None, None, &mut l, 100);
        assert!(b.timestamp > a.timestamp);
        assert_eq!(project(Some(A), &Perms::dm(), &[b.clone(), a.clone()]).edited.unwrap().0, "segunda");
    }

    #[test]
    fn unknown_author_means_author_only_actions_are_rejected() {
        let id = Identity::generate();
        let mut l = 0;
        let r = project(None, &Perms::dm(), &[ev(&id, A, "edit", Some("x"), None, None, &mut l, 1)]);
        assert!(r.edited.is_none());
    }
}
