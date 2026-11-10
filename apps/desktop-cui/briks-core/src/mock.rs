//! Servidor Phoenix/Realtime mínimo em loopback (Presence + Broadcast) para testar a sinalização sem Supabase real.
//! `briks-core mock-realtime [porta]` também o expõe na linha de comando (só 127.0.0.1).
use std::collections::HashMap;
use std::sync::Arc;

use futures_util::{SinkExt, StreamExt};
use serde_json::{json, Value};
use tokio::net::TcpListener;
use tokio::sync::{mpsc, Mutex};
use tokio_tungstenite::tungstenite::Message;

use crate::phoenix::{self, Frame};

type Tx = mpsc::UnboundedSender<String>;
#[derive(Default)]
struct Topic { members: HashMap<u64, (String, Option<Value>, Tx)> } // conn -> (key, meta, tx)
type Hub = Arc<Mutex<HashMap<String, Topic>>>;

/// Escuta em 127.0.0.1:porta (0 = livre). Devolve a porta; roda até o processo acabar.
pub async fn start(port: u16) -> std::io::Result<u16> {
    let l = TcpListener::bind(("127.0.0.1", port)).await?;
    let port = l.local_addr()?.port();
    let hub: Hub = Arc::new(Mutex::new(HashMap::new()));
    tokio::spawn(async move {
        let mut next = 1u64;
        while let Ok((s, _)) = l.accept().await {
            let hub = hub.clone();
            let id = next; next += 1;
            tokio::spawn(async move { let _ = conn(s, hub, id).await; });
        }
    });
    Ok(port)
}

async fn conn(s: tokio::net::TcpStream, hub: Hub, id: u64) -> anyhow::Result<()> {
    let ws = tokio_tungstenite::accept_async(s).await?;
    let (mut sink, mut stream) = ws.split();
    let (tx, mut rx) = mpsc::unbounded_channel::<String>();
    let writer = tokio::spawn(async move { while let Some(m) = rx.recv().await { if sink.send(Message::Text(m)).await.is_err() { break; } } });
    let mut my_topics: Vec<String> = vec![];
    while let Some(Ok(msg)) = stream.next().await {
        let Message::Text(t) = msg else { continue };
        let Some(f) = phoenix::decode(&t) else { continue };
        match f.event.as_str() {
            "phx_join" => {
                let key = f.payload["config"]["presence"]["key"].as_str().unwrap_or("").to_string();
                let mut h = hub.lock().await;
                let topic = h.entry(f.topic.clone()).or_default();
                topic.members.insert(id, (key, None, tx.clone()));
                my_topics.push(f.topic.clone());
                let _ = tx.send(phoenix::encode(&Frame { topic: f.topic.clone(), event: "phx_reply".into(), payload: json!({"status":"ok","response":{}}), msg_ref: f.msg_ref.clone(), join_ref: f.join_ref.clone() }));
                let state: serde_json::Map<String, Value> = topic.members.values().filter_map(|(k, m, _)| m.as_ref().map(|m| (k.clone(), json!({"metas":[m]})))).collect();
                let _ = tx.send(phoenix::encode(&Frame { topic: f.topic.clone(), event: "presence_state".into(), payload: Value::Object(state), msg_ref: None, join_ref: None }));
            }
            "presence" => {
                let mut h = hub.lock().await;
                let Some(topic) = h.get_mut(&f.topic) else { continue };
                let key = topic.members.get(&id).map(|m| m.0.clone()).unwrap_or_default();
                let (joins, leaves) = if f.payload["event"] == "track" {
                    let meta = f.payload["payload"].clone();
                    if let Some(m) = topic.members.get_mut(&id) { m.1 = Some(meta.clone()); }
                    (json!({ key.clone(): { "metas": [meta] } }), json!({}))
                } else {
                    if let Some(m) = topic.members.get_mut(&id) { m.1 = None; }
                    (json!({}), json!({ key.clone(): { "metas": [] } }))
                };
                let diff = phoenix::encode(&Frame { topic: f.topic.clone(), event: "presence_diff".into(), payload: json!({"joins": joins, "leaves": leaves}), msg_ref: None, join_ref: None });
                for (cid, m) in topic.members.iter() { if *cid != id { let _ = m.2.send(diff.clone()); } }
                // quem acabou de entrar recebe o estado atual
                let state: serde_json::Map<String, Value> = topic.members.iter().filter(|(cid, _)| **cid != id).filter_map(|(_, (k, m, _))| m.as_ref().map(|m| (k.clone(), json!({"metas":[m]})))).collect();
                if f.payload["event"] == "track" { let _ = tx.send(phoenix::encode(&Frame { topic: f.topic.clone(), event: "presence_state".into(), payload: Value::Object(state), msg_ref: None, join_ref: None })); }
            }
            "broadcast" => {
                let h = hub.lock().await;
                if let Some(topic) = h.get(&f.topic) {
                    for (cid, m) in topic.members.iter() { if *cid != id { let _ = m.2.send(t.clone()); } }
                }
            }
            "heartbeat" => { let _ = tx.send(phoenix::encode(&Frame { topic: "phoenix".into(), event: "phx_reply".into(), payload: json!({"status":"ok","response":{}}), msg_ref: f.msg_ref.clone(), join_ref: None })); }
            "phx_leave" => break,
            _ => {}
        }
    }
    let mut h = hub.lock().await;
    for tn in my_topics {
        if let Some(topic) = h.get_mut(&tn) {
            if let Some((key, meta, _)) = topic.members.remove(&id) {
                if meta.is_some() {
                    let diff = phoenix::encode(&Frame { topic: tn.clone(), event: "presence_diff".into(), payload: json!({"joins":{}, "leaves": { key: {"metas":[]} }}), msg_ref: None, join_ref: None });
                    for m in topic.members.values() { let _ = m.2.send(diff.clone()); }
                }
            }
        }
    }
    writer.abort();
    Ok(())
}
