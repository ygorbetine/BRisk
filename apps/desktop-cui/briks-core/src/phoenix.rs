//! Protocolo Phoenix (vsn 1.0.0) usado pelo Supabase Realtime: frames, join, heartbeat, Presence e Broadcast.
//! Só a parte pura (codificação/decodificação e estado de Presence) vive aqui; o transporte WebSocket fica em `realtime.rs`.
use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)]
pub struct Frame {
    pub topic: String,
    pub event: String,
    pub payload: Value,
    #[serde(rename = "ref", default)]
    pub msg_ref: Option<String>,
    #[serde(default)]
    pub join_ref: Option<String>,
}

pub fn encode(f: &Frame) -> String {
    serde_json::to_string(f).expect("frame serializável")
}

pub fn decode(text: &str) -> Option<Frame> {
    serde_json::from_str(text).ok()
}

/// Corpo do `phx_join` de um canal Supabase Realtime com Broadcast (sem eco, com ack) e Presence com chave = peerId.
pub fn join_payload(peer_id: &str, access_token: &str) -> Value {
    json!({
        "config": {
            "broadcast": { "ack": true, "self": false },
            "presence": { "key": peer_id },
            "postgres_changes": []
        },
        "access_token": access_token
    })
}

pub fn heartbeat(ref_n: u64) -> Frame {
    Frame { topic: "phoenix".into(), event: "heartbeat".into(), payload: json!({}), msg_ref: Some(ref_n.to_string()), join_ref: None }
}

pub fn broadcast(topic: &str, event: &str, payload: Value, ref_n: u64, join_ref: &str) -> Frame {
    Frame {
        topic: topic.into(),
        event: "broadcast".into(),
        payload: json!({ "type": "broadcast", "event": event, "payload": payload }),
        msg_ref: Some(ref_n.to_string()),
        join_ref: Some(join_ref.into()),
    }
}

pub fn presence_track(topic: &str, meta: Value, ref_n: u64, join_ref: &str) -> Frame {
    Frame {
        topic: topic.into(),
        event: "presence".into(),
        payload: json!({ "type": "presence", "event": "track", "payload": meta }),
        msg_ref: Some(ref_n.to_string()),
        join_ref: Some(join_ref.into()),
    }
}

pub fn presence_untrack(topic: &str, ref_n: u64, join_ref: &str) -> Frame {
    Frame {
        topic: topic.into(),
        event: "presence".into(),
        payload: json!({ "type": "presence", "event": "untrack" }),
        msg_ref: Some(ref_n.to_string()),
        join_ref: Some(join_ref.into()),
    }
}

/// Se o frame é um Broadcast recebido, devolve (evento, payload).
pub fn as_broadcast(f: &Frame) -> Option<(String, Value)> {
    if f.event != "broadcast" {
        return None;
    }
    let ev = f.payload.get("event")?.as_str()?.to_string();
    let p = f.payload.get("payload")?.clone();
    Some((ev, p))
}

/// Presence: chave (peerId) -> primeira meta. Aplica `presence_state` (substitui) e `presence_diff` (joins/leaves).
#[derive(Default, Debug, Clone)]
pub struct PresenceTable {
    pub peers: BTreeMap<String, Value>,
}

#[derive(Debug, PartialEq)]
pub struct PresenceChange {
    pub joined: Vec<String>,
    pub left: Vec<String>,
}

fn first_meta(entry: &Value) -> Value {
    entry.get("metas").and_then(|m| m.as_array()).and_then(|a| a.first()).cloned().unwrap_or(Value::Null)
}

impl PresenceTable {
    pub fn apply(&mut self, f: &Frame) -> Option<PresenceChange> {
        match f.event.as_str() {
            "presence_state" => {
                let obj = f.payload.as_object()?;
                let mut now: BTreeMap<String, Value> = BTreeMap::new();
                for (k, v) in obj {
                    now.insert(k.clone(), first_meta(v));
                }
                let joined = now.keys().filter(|k| !self.peers.contains_key(*k)).cloned().collect();
                let left = self.peers.keys().filter(|k| !now.contains_key(*k)).cloned().collect();
                self.peers = now;
                Some(PresenceChange { joined, left })
            }
            "presence_diff" => {
                let mut ch = PresenceChange { joined: vec![], left: vec![] };
                if let Some(leaves) = f.payload.get("leaves").and_then(|v| v.as_object()) {
                    for k in leaves.keys() {
                        if self.peers.remove(k).is_some() {
                            ch.left.push(k.clone());
                        }
                    }
                }
                if let Some(joins) = f.payload.get("joins").and_then(|v| v.as_object()) {
                    for (k, v) in joins {
                        if self.peers.insert(k.clone(), first_meta(v)).is_none() {
                            ch.joined.push(k.clone());
                        }
                    }
                }
                Some(ch)
            }
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn frame_roundtrip_and_ref_field() {
        let f = broadcast("realtime:briks:room:abc", "webrtc.offer", json!({"x": 1}), 7, "1");
        let text = encode(&f);
        assert!(text.contains("\"ref\":\"7\"") && text.contains("\"join_ref\":\"1\""));
        let back = decode(&text).unwrap();
        assert_eq!(back, f);
        let (ev, p) = as_broadcast(&back).unwrap();
        assert_eq!(ev, "webrtc.offer");
        assert_eq!(p["x"], 1);
    }

    #[test]
    fn join_payload_has_supabase_config() {
        let p = join_payload("peer-1", "anon");
        assert_eq!(p["config"]["presence"]["key"], "peer-1");
        assert_eq!(p["config"]["broadcast"]["self"], false);
        assert_eq!(p["config"]["broadcast"]["ack"], true);
    }

    #[test]
    fn presence_state_then_diff() {
        let mut t = PresenceTable::default();
        let state = Frame { topic: "t".into(), event: "presence_state".into(), msg_ref: None, join_ref: None,
            payload: json!({ "a": { "metas": [{ "peerId": "a", "joinedAt": 1 }] } }) };
        let c = t.apply(&state).unwrap();
        assert_eq!(c.joined, vec!["a"]);
        let diff = Frame { topic: "t".into(), event: "presence_diff".into(), msg_ref: None, join_ref: None,
            payload: json!({ "joins": { "b": { "metas": [{ "peerId": "b", "joinedAt": 2 }] } }, "leaves": { "a": { "metas": [] } } }) };
        let c = t.apply(&diff).unwrap();
        assert_eq!(c.joined, vec!["b"]);
        assert_eq!(c.left, vec!["a"]);
        assert_eq!(t.peers.len(), 1);
        assert_eq!(t.peers["b"]["joinedAt"], 2);
    }

    #[test]
    fn rejects_garbage() {
        assert!(decode("nope").is_none());
        let f = Frame { topic: "t".into(), event: "x".into(), payload: json!({}), msg_ref: None, join_ref: None };
        assert!(as_broadcast(&f).is_none());
    }
}
