//! Envelope de sinalização e validações de docs/protocol.md (versão 1). Puro: sem rede, testável.
use std::collections::{HashMap, VecDeque};

use serde::{Deserialize, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};

pub const NAMESPACES: [&str; 4] = ["room", "chat", "friend", "group"];
pub const MAX_ENVELOPE_BYTES: usize = 64 * 1024;
pub const TIMESTAMP_WINDOW_MS: i64 = 120_000;
pub const RATE_LIMIT_PER_MIN: usize = 240;
pub const DEDUPE_CAP: usize = 2048;

#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct Envelope {
    pub version: u32,
    pub room_id: String,
    pub from_peer_id: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub target_peer_id: Option<String>,
    pub message_id: String,
    pub timestamp: i64,
    #[serde(rename = "type")]
    pub kind: String,
    pub payload: Value,
}

/// SHA-256 hex de "<namespace>:<sala>" (o `secureRoomId` do cliente TypeScript).
pub fn secure_room_id(namespace: &str, room: &str) -> String {
    Sha256::digest(format!("{namespace}:{room}").as_bytes()).iter().map(|b| format!("{b:02x}")).collect()
}

/// Tópico Realtime: `realtime:briks:<namespace>:<32 primeiros hex>`.
pub fn topic_name(namespace: &str, room_hash: &str) -> String {
    format!("realtime:briks:{namespace}:{}", &room_hash[..32])
}

fn is_uuid(s: &str) -> bool {
    let b = s.as_bytes();
    s.len() == 36 && [8, 13, 18, 23].iter().all(|&i| b[i] == b'-') && s.chars().enumerate().all(|(i, c)| [8, 13, 18, 23].contains(&i) || c.is_ascii_hexdigit())
}

pub fn valid_kind(k: &str) -> bool {
    matches!(k, "webrtc.offer" | "webrtc.answer" | "webrtc.ice-candidate" | "peer.state")
}

#[derive(Debug, PartialEq, Eq)]
pub enum Reject {
    Size, Shape, Version, Kind, Uuid, Room, Target, SelfEcho, NotPresent, Clock, Duplicate, Rate, Payload,
}

/// Estado do validador de um canal: dedupe por messageId e rate limit por (peer, tipo).
pub struct Validator {
    pub room_hash: String,
    pub local_peer: String,
    seen: HashMap<String, i64>,
    order: VecDeque<String>,
    rate: HashMap<(String, String), VecDeque<i64>>,
}

impl Validator {
    pub fn new(room_hash: &str, local_peer: &str) -> Self {
        Validator { room_hash: room_hash.into(), local_peer: local_peer.into(), seen: HashMap::new(), order: VecDeque::new(), rate: HashMap::new() }
    }

    /// `present` diz se o remetente está na Presence atual; `now_ms` é injetado para teste.
    pub fn check(&mut self, raw: &Value, present: impl Fn(&str) -> bool, now_ms: i64) -> Result<Envelope, Reject> {
        if serde_json::to_vec(raw).map(|v| v.len()).unwrap_or(usize::MAX) > MAX_ENVELOPE_BYTES {
            return Err(Reject::Size);
        }
        let e: Envelope = serde_json::from_value(raw.clone()).map_err(|_| Reject::Shape)?;
        if e.version != 1 {
            return Err(Reject::Version);
        }
        if !valid_kind(&e.kind) {
            return Err(Reject::Kind);
        }
        if !is_uuid(&e.message_id) || !is_uuid(&e.from_peer_id) || e.target_peer_id.as_deref().map(|t| !is_uuid(t)).unwrap_or(false) {
            return Err(Reject::Uuid);
        }
        if e.room_id != self.room_hash {
            return Err(Reject::Room);
        }
        if e.from_peer_id == self.local_peer {
            return Err(Reject::SelfEcho);
        }
        if let Some(t) = &e.target_peer_id {
            if t != &self.local_peer {
                return Err(Reject::Target);
            }
        }
        if !present(&e.from_peer_id) {
            return Err(Reject::NotPresent);
        }
        // saturating: o timestamp vem do peer; i64::MIN/MAX estouraria a subtração (pânico em debug, valor errado em release)
        if now_ms.saturating_sub(e.timestamp).saturating_abs() > TIMESTAMP_WINDOW_MS {
            return Err(Reject::Clock);
        }
        if self.seen.contains_key(&e.message_id) {
            return Err(Reject::Duplicate);
        }
        let q = self.rate.entry((e.from_peer_id.clone(), e.kind.clone())).or_default();
        while q.front().map(|t| now_ms.saturating_sub(*t) > 60_000).unwrap_or(false) {
            q.pop_front();
        }
        if q.len() >= RATE_LIMIT_PER_MIN {
            return Err(Reject::Rate);
        }
        if !payload_ok(&e) {
            return Err(Reject::Payload);
        }
        q.push_back(now_ms);
        self.seen.insert(e.message_id.clone(), now_ms);
        self.order.push_back(e.message_id.clone());
        while self.order.len() > DEDUPE_CAP {
            if let Some(old) = self.order.pop_front() {
                self.seen.remove(&old);
            }
        }
        Ok(e)
    }
}

fn payload_ok(e: &Envelope) -> bool {
    match e.kind.as_str() {
        "webrtc.offer" | "webrtc.answer" => e.payload.get("sdp").and_then(|s| s.get("sdp")).map(|v| v.is_string()).unwrap_or(false),
        "webrtc.ice-candidate" => e.payload.get("candidate").map(|c| c.is_object()).unwrap_or(false),
        "peer.state" => e.payload.get("state").map(|s| s.is_object()).unwrap_or(false),
        _ => false,
    }
}

/// Quem inicia a conexão na descoberta: o menor peerId (ordem lexical), como no cliente TypeScript.
pub fn is_initiator(local: &str, remote: &str) -> bool {
    local < remote
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    const A: &str = "aaaaaaaa-0000-4000-8000-000000000001";
    const B: &str = "bbbbbbbb-0000-4000-8000-000000000002";
    const M1: &str = "11111111-0000-4000-8000-000000000001";
    const M2: &str = "11111111-0000-4000-8000-000000000002";

    fn env(id: &str, from: &str, to: Option<&str>, room: &str, ts: i64) -> Value {
        let mut v = json!({ "version": 1, "roomId": room, "fromPeerId": from, "messageId": id, "timestamp": ts, "type": "webrtc.offer", "payload": { "sdp": { "type": "offer", "sdp": "v=0" } } });
        if let Some(t) = to { v["targetPeerId"] = json!(t); }
        v
    }

    #[test]
    fn room_hash_and_topic() {
        let h = secure_room_id("room", "sala-1");
        assert_eq!(h.len(), 64);
        assert_eq!(h, secure_room_id("room", "sala-1"));
        assert_ne!(h, secure_room_id("chat", "sala-1"));
        assert_eq!(topic_name("room", &h), format!("realtime:briks:room:{}", &h[..32]));
    }

    #[test]
    fn accepts_valid_and_dedupes() {
        let h = secure_room_id("room", "s");
        let mut v = Validator::new(&h, B);
        let e = env(M1, A, Some(B), &h, 1000);
        assert!(v.check(&e, |p| p == A, 1000).is_ok());
        assert_eq!(v.check(&e, |p| p == A, 1000), Err(Reject::Duplicate));
    }

    #[test]
    fn rejects_every_bad_case() {
        let h = secure_room_id("room", "s");
        let mut v = Validator::new(&h, B);
        let pa = |p: &str| p == A;
        assert_eq!(v.check(&env(M1, A, Some(B), "outro", 1000), pa, 1000), Err(Reject::Room));
        assert_eq!(v.check(&env(M1, A, Some(A), &h, 1000), pa, 1000), Err(Reject::Target));
        assert_eq!(v.check(&env(M1, B, None, &h, 1000), |_| true, 1000), Err(Reject::SelfEcho));
        assert_eq!(v.check(&env(M1, A, Some(B), &h, 1000), |_| false, 1000), Err(Reject::NotPresent));
        assert_eq!(v.check(&env(M1, A, Some(B), &h, 1000), pa, 1000 + TIMESTAMP_WINDOW_MS + 1), Err(Reject::Clock));
        assert_eq!(v.check(&env("nao-uuid", A, Some(B), &h, 1000), pa, 1000), Err(Reject::Uuid));
        let mut bad = env(M2, A, Some(B), &h, 1000); bad["version"] = json!(2);
        assert_eq!(v.check(&bad, pa, 1000), Err(Reject::Version));
        let mut bad = env(M2, A, Some(B), &h, 1000); bad["type"] = json!("webrtc.hack");
        assert_eq!(v.check(&bad, pa, 1000), Err(Reject::Kind));
        let mut bad = env(M2, A, Some(B), &h, 1000); bad["payload"] = json!({});
        assert_eq!(v.check(&bad, pa, 1000), Err(Reject::Payload));
        let mut big = env(M2, A, Some(B), &h, 1000); big["payload"]["sdp"]["sdp"] = json!("x".repeat(MAX_ENVELOPE_BYTES));
        assert_eq!(v.check(&big, pa, 1000), Err(Reject::Size));
    }

    #[test]
    fn rate_limit_per_peer_and_kind() {
        let h = secure_room_id("room", "s");
        let mut v = Validator::new(&h, B);
        for i in 0..RATE_LIMIT_PER_MIN {
            let id = format!("22222222-0000-4000-8000-{i:012}");
            assert!(v.check(&env(&id, A, Some(B), &h, 1000), |p| p == A, 1000).is_ok(), "{i}");
        }
        let id = "33333333-0000-4000-8000-000000000000";
        assert_eq!(v.check(&env(id, A, Some(B), &h, 1000), |p| p == A, 1000), Err(Reject::Rate));
    }

    #[test]
    fn extreme_numbers_and_shapes_never_panic() {
        let h = secure_room_id("room", "s");
        let mut v = Validator::new(&h, B);
        for ts in [i64::MIN, i64::MAX, i64::MIN + 1, -1, 0] {
            let r = v.check(&env(&format!("44444444-0000-4000-8000-{:012}", ts.unsigned_abs() % 1_000_000_000_000), A, Some(B), &h, ts), |p| p == A, 1000);
            assert!(r.is_err() || ts.abs() < TIMESTAMP_WINDOW_MS * 2, "ts {ts}: {r:?}");
        }
        assert_eq!(v.check(&env(M1, A, Some(B), &h, i64::MIN), |p| p == A, i64::MAX), Err(Reject::Clock));
        assert_eq!(v.check(&env(M2, A, Some(B), &h, i64::MAX), |p| p == A, i64::MIN), Err(Reject::Clock));
        // formas erradas: nada de pânico, sempre um motivo de rejeição
        for raw in [json!(null), json!("x"), json!(7), json!([]), json!([1, 2]), json!({}), json!({ "version": "1" }), json!({ "version": 1, "timestamp": 1.5 }),
                    json!({ "version": 1, "roomId": 5, "fromPeerId": [], "messageId": {}, "timestamp": "x", "type": 3, "payload": 1 })] {
            assert!(v.check(&raw, |_| true, 1000).is_err(), "{raw}");
        }
        // UUID com bytes multibyte e tamanho de 36 bytes: não pode indexar fora
        let multi = "é".repeat(18);
        assert_eq!(v.check(&env(&multi, A, Some(B), &h, 1000), |p| p == A, 1000), Err(Reject::Uuid));
        // ordem dos campos aninhada profundamente
        let mut deep = json!(1); for _ in 0..100 { deep = json!({ "a": deep }); }
        let mut e = env("55555555-0000-4000-8000-000000000001", A, Some(B), &h, 1000); e["payload"]["sdp"]["extra"] = deep;
        assert!(v.check(&e, |p| p == A, 1000).is_ok());
    }

    #[test]
    fn initiator_is_lowest_peer_id() {
        assert!(is_initiator(A, B) && !is_initiator(B, A));
    }
}
