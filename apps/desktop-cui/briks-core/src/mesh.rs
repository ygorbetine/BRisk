//! Malha WebRTC: um RTCPeerConnection por peer remoto (até 5), Perfect Negotiation, fila de ICE, DataChannel `briks.chat`
//! com desafio de identidade ECDSA e mensagens assinadas (+ ACK/outbox). Um único ator (tarefa tokio) é dono de todo o estado;
//! os callbacks do webrtc-rs só enviam `Internal` para ele, sem travas.
//!
//! Perfect Negotiation: o peer "educado" é o de peerId MAIOR (o que não inicia). Em colisão de offers o impolido ignora a offer
//! remota; o educado faz rollback da própria offer (`set_local_description` com tipo rollback, suportado pelo webrtc-rs 0.12)
//! e responde à offer do outro na mesma conexão.
use std::collections::{HashMap, HashSet};
use std::sync::Arc;

use anyhow::Result;
use serde_json::{json, Value};
use tokio::sync::mpsc;
use webrtc::api::APIBuilder;
use webrtc::data_channel::data_channel_message::DataChannelMessage;
use webrtc::data_channel::RTCDataChannel;
use webrtc::ice_transport::ice_candidate::RTCIceCandidateInit;
use webrtc::ice_transport::ice_server::RTCIceServer;
use webrtc::peer_connection::configuration::RTCConfiguration;
use webrtc::peer_connection::peer_connection_state::RTCPeerConnectionState;
use webrtc::peer_connection::sdp::session_description::RTCSessionDescription;
use webrtc::peer_connection::signaling_state::RTCSignalingState;
use webrtc::peer_connection::RTCPeerConnection;

use crate::identity::{self, ChatWire, Identity, PublicJwk};
use crate::realtime::{Realtime, RtEvent};
use crate::signaling::{is_initiator, secure_room_id, Envelope, Reject, Validator};

pub const MAX_PEERS: usize = 5;
pub const MAX_PENDING_ICE: usize = 256;

#[derive(Debug, Clone, PartialEq)]
pub enum MeshEvent {
    /// Estado de um peer: "negociando" | "conectado" | "autenticado" | "falhou" | "fechado".
    Peer { peer_id: String, state: String },
    Chat { channel: String, from_fp: String, author: String, content: String, id: String, ts: u64 },
    Error(String),
}

pub enum MeshCmd {
    /// Envia texto para a identidade `to_fp` (impressão digital), por DM. Fica na outbox até haver DataChannel autenticado.
    Chat { to_fp: String, text: String },
    /// Só para testes: força um offer agora (colisão).
    ForceOffer { peer_id: String },
    Close,
}

enum Internal {
    Ice { peer: String, gen: u32, init: RTCIceCandidateInit },
    PcState { peer: String, gen: u32, state: RTCPeerConnectionState },
    DcOpen { peer: String, gen: u32 },
    DcAttach { peer: String, gen: u32, dc: Arc<RTCDataChannel> },
    DcMsg { peer: String, gen: u32, text: String },
}

struct Peer {
    pc: Arc<RTCPeerConnection>,
    gen: u32,
    polite: bool,
    making_offer: bool,
    ignore_offer: bool,
    pending_ice: Vec<RTCIceCandidateInit>,
    dc: Option<Arc<RTCDataChannel>>,
    fp: Option<String>,
    jwk: Option<PublicJwk>,
    my_nonce: String,
    proven: bool,   // o outro provou a identidade para mim
    sent_proof: bool,
}

pub struct MeshCfg {
    pub namespace: String,
    pub room: String,
    pub local_peer: String,
    pub display_name: String,
    pub ice_servers: Vec<RTCIceServer>,
}

pub struct MeshHandle {
    pub cmd: mpsc::Sender<MeshCmd>,
    pub events: mpsc::Receiver<MeshEvent>,
}

fn now_ms() -> i64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() as i64).unwrap_or(0)
}

pub fn spawn(cfg: MeshCfg, identity: Arc<Identity>, rt: Realtime, rt_events: mpsc::Receiver<RtEvent>) -> MeshHandle {
    let (ctx, crx) = mpsc::channel(64);
    let (etx, erx) = mpsc::channel(256);
    tokio::spawn(async move {
        let mut a = Actor::new(cfg, identity, rt, etx);
        a.run(rt_events, crx).await;
    });
    MeshHandle { cmd: ctx, events: erx }
}

struct Actor {
    cfg: MeshCfg,
    identity: Arc<Identity>,
    rt: Realtime,
    ev: mpsc::Sender<MeshEvent>,
    room_hash: String,
    validator: Validator,
    present: HashSet<String>,
    peers: HashMap<String, Peer>,
    itx: mpsc::Sender<Internal>,
    irx: Option<mpsc::Receiver<Internal>>,
    next_gen: u32,
    outbox: HashMap<String, Vec<ChatWire>>,   // por fp de destino
    seen_msgs: HashSet<String>,
}

impl Actor {
    fn new(cfg: MeshCfg, identity: Arc<Identity>, rt: Realtime, ev: mpsc::Sender<MeshEvent>) -> Self {
        let room_hash = secure_room_id(&cfg.namespace, &cfg.room);
        let (itx, irx) = mpsc::channel(512);
        let validator = Validator::new(&room_hash, &cfg.local_peer);
        Actor { cfg, identity, rt, ev, room_hash, validator, present: HashSet::new(), peers: HashMap::new(), itx, irx: Some(irx), next_gen: 1, outbox: HashMap::new(), seen_msgs: HashSet::new() }
    }

    async fn run(&mut self, mut rt_events: mpsc::Receiver<RtEvent>, mut cmds: mpsc::Receiver<MeshCmd>) {
        let mut irx = self.irx.take().expect("irx");
        let mut retry = tokio::time::interval(std::time::Duration::from_secs(2));
        loop {
            tokio::select! {
                _ = retry.tick() => {
                    // mensagens sem ACK são reenviadas (o receptor deduplica por id)
                    let fps: Vec<String> = self.outbox.iter().filter(|(_, l)| !l.is_empty()).map(|(k, _)| k.clone()).collect();
                    for fp in fps { self.flush_outbox(&fp).await; }
                },
                e = rt_events.recv() => match e {
                    Some(e) => self.on_rt(e).await,
                    None => break,
                },
                c = cmds.recv() => match c {
                    Some(MeshCmd::Close) | None => break,
                    Some(c) => self.on_cmd(c).await,
                },
                i = irx.recv() => if let Some(i) = i { self.on_internal(i).await },
            }
        }
        for (_, p) in self.peers.drain() { let _ = p.pc.close().await; }
        self.rt.close().await;
    }

    async fn emit(&self, e: MeshEvent) { let _ = self.ev.send(e).await; }

    async fn on_rt(&mut self, e: RtEvent) {
        match e {
            RtEvent::Status(s) => { if let Some(msg) = s.strip_prefix("error: ") { self.emit(MeshEvent::Error(msg.to_string())).await; } }
            RtEvent::PeerJoined { peer_id, .. } => {
                self.present.insert(peer_id.clone());
                if self.peers.len() >= MAX_PEERS || self.peers.contains_key(&peer_id) { return; }
                if let Err(e) = self.add_peer(&peer_id).await { self.emit(MeshEvent::Error(format!("peer {peer_id}: {e}"))).await; return; }
                if is_initiator(&self.cfg.local_peer, &peer_id) { self.start_offer(&peer_id, true).await; }
            }
            RtEvent::PeerLeft(p) => {
                self.present.remove(&p);
                if let Some(peer) = self.peers.remove(&p) { let _ = peer.pc.close().await; self.emit(MeshEvent::Peer { peer_id: p, state: "fechado".into() }).await; }
            }
            RtEvent::Broadcast { event: _, payload } => {
                let present = self.present.clone();
                match self.validator.check(&payload, |p| present.contains(p), now_ms()) {
                    Ok(env) => self.on_signal(env).await,
                    Err(Reject::Duplicate) | Err(Reject::SelfEcho) | Err(Reject::Target) | Err(Reject::Room) => {}
                    Err(_) => {}
                }
            }
        }
    }

    fn rtc_config(&self) -> RTCConfiguration {
        RTCConfiguration { ice_servers: self.cfg.ice_servers.clone(), ..Default::default() }
    }

    async fn new_pc(&mut self, peer_id: &str, gen: u32) -> Result<Arc<RTCPeerConnection>> {
        let api = APIBuilder::new().build();
        let pc = Arc::new(api.new_peer_connection(self.rtc_config()).await?);
        let (itx, id) = (self.itx.clone(), peer_id.to_string());
        { let (itx, id) = (itx.clone(), id.clone());
          pc.on_ice_candidate(Box::new(move |c| { let (itx, id) = (itx.clone(), id.clone()); Box::pin(async move {
              if let Some(c) = c { if let Ok(init) = c.to_json() { let _ = itx.send(Internal::Ice { peer: id, gen, init }).await; } } }) })); }
        { let (itx, id) = (itx.clone(), id.clone());
          pc.on_peer_connection_state_change(Box::new(move |state| { let (itx, id) = (itx.clone(), id.clone()); Box::pin(async move { let _ = itx.send(Internal::PcState { peer: id, gen, state }).await; }) })); }
        { let (itx, id) = (itx.clone(), id.clone());
          pc.on_data_channel(Box::new(move |dc| { let (itx, id) = (itx.clone(), id.clone()); Box::pin(async move { let _ = itx.send(Internal::DcAttach { peer: id, gen, dc }).await; }) })); }
        Ok(pc)
    }

    async fn add_peer(&mut self, peer_id: &str) -> Result<()> {
        let gen = self.next_gen; self.next_gen += 1;
        let pc = self.new_pc(peer_id, gen).await?;
        let polite = !is_initiator(&self.cfg.local_peer, peer_id);
        self.peers.insert(peer_id.into(), Peer { pc, gen, polite, making_offer: false, ignore_offer: false, pending_ice: vec![], dc: None, fp: None, jwk: None, my_nonce: String::new(), proven: false, sent_proof: false });
        Ok(())
    }

    fn wire_dc(&self, peer: &str, gen: u32, dc: &Arc<RTCDataChannel>) {
        let (itx, id) = (self.itx.clone(), peer.to_string());
        { let (itx, id) = (itx.clone(), id.clone());
          dc.on_open(Box::new(move || { let (itx, id) = (itx.clone(), id.clone()); Box::pin(async move { let _ = itx.send(Internal::DcOpen { peer: id, gen }).await; }) })); }
        dc.on_message(Box::new(move |m: DataChannelMessage| { let (itx, id) = (itx.clone(), id.clone());
            Box::pin(async move { if m.data.len() <= identity::MAX_PAYLOAD_BYTES { let _ = itx.send(Internal::DcMsg { peer: id, gen, text: String::from_utf8_lossy(&m.data).to_string() }).await; } }) }));
    }

    async fn send_signal(&self, kind: &str, target: &str, payload: Value) {
        let env = Envelope { version: 1, room_id: self.room_hash.clone(), from_peer_id: self.cfg.local_peer.clone(), target_peer_id: Some(target.into()),
            message_id: uuid::Uuid::new_v4().to_string(), timestamp: now_ms(), kind: kind.into(), payload };
        if let Ok(v) = serde_json::to_value(&env) { let _ = self.rt.send_broadcast(kind, v).await; }
    }

    async fn start_offer(&mut self, peer_id: &str, create_dc: bool) {
        let Some(p) = self.peers.get_mut(peer_id) else { return };
        p.making_offer = true;
        let pc = p.pc.clone();
        let gen = p.gen;
        let mut dc_new = None;
        if create_dc && p.dc.is_none() {
            if let Ok(dc) = pc.create_data_channel("briks.chat", None).await { p.dc = Some(dc.clone()); dc_new = Some(dc); }
        }
        let res: Result<RTCSessionDescription> = async { let o = pc.create_offer(None).await?; pc.set_local_description(o.clone()).await?; Ok(o) }.await;
        if let Some(p) = self.peers.get_mut(peer_id) { p.making_offer = false; }
        if let Some(dc) = dc_new { self.wire_dc(peer_id, gen, &dc); }
        match res {
            Ok(o) => self.send_signal("webrtc.offer", peer_id, json!({ "sdp": o })).await,
            Err(e) => self.emit(MeshEvent::Error(format!("offer: {e}"))).await,
        }
    }

    async fn on_signal(&mut self, env: Envelope) {
        let from = env.from_peer_id.clone();
        if !self.peers.contains_key(&from) {
            if self.peers.len() >= MAX_PEERS || self.add_peer(&from).await.is_err() { return; }
        }
        match env.kind.as_str() {
            "webrtc.offer" => {
                let Ok(sdp) = serde_json::from_value::<RTCSessionDescription>(env.payload["sdp"].clone()) else { return };
                let (collision, polite) = { let p = &self.peers[&from]; (p.making_offer || p.pc.signaling_state() != RTCSignalingState::Stable, p.polite) };
                if collision && !polite { self.peers.get_mut(&from).unwrap().ignore_offer = true; return; }
                if collision && polite {
                    // rollback da offer local (Perfect Negotiation) e segue como respondedor na MESMA conexão
                    let pc = self.peers[&from].pc.clone();
                    if let Ok(rb) = serde_json::from_value::<RTCSessionDescription>(json!({ "type": "rollback", "sdp": "" })) {
                        if let Err(e) = pc.set_local_description(rb).await { self.emit(MeshEvent::Error(format!("rollback: {e}"))).await; return; }
                    }
                }
                let p = self.peers.get_mut(&from).unwrap();
                p.ignore_offer = false;
                let pc = p.pc.clone();
                let r: Result<RTCSessionDescription> = async { pc.set_remote_description(sdp).await?; let a = pc.create_answer(None).await?; pc.set_local_description(a.clone()).await?; Ok(a) }.await;
                self.flush_ice(&from).await;
                match r { Ok(a) => self.send_signal("webrtc.answer", &from, json!({ "sdp": a })).await, Err(e) => self.emit(MeshEvent::Error(format!("answer: {e}"))).await }
            }
            "webrtc.answer" => {
                let Ok(sdp) = serde_json::from_value::<RTCSessionDescription>(env.payload["sdp"].clone()) else { return };
                let pc = self.peers[&from].pc.clone();
                if pc.signaling_state() != RTCSignalingState::HaveLocalOffer { return; }
                if let Err(e) = pc.set_remote_description(sdp).await { self.emit(MeshEvent::Error(format!("set answer: {e}"))).await; return; }
                self.flush_ice(&from).await;
            }
            "webrtc.ice-candidate" => {
                let Ok(init) = serde_json::from_value::<RTCIceCandidateInit>(env.payload["candidate"].clone()) else { return };
                let p = self.peers.get_mut(&from).unwrap();
                if p.ignore_offer { return; }
                if p.pc.remote_description().await.is_none() {
                    if p.pending_ice.len() < MAX_PENDING_ICE { p.pending_ice.push(init); }
                } else { let _ = p.pc.add_ice_candidate(init).await; }
            }
            _ => {}
        }
    }

    async fn flush_ice(&mut self, peer: &str) {
        let Some(p) = self.peers.get_mut(peer) else { return };
        let q = std::mem::take(&mut p.pending_ice);
        for c in q { let _ = p.pc.add_ice_candidate(c).await; }
    }

    async fn on_cmd(&mut self, c: MeshCmd) {
        match c {
            MeshCmd::ForceOffer { peer_id } => self.start_offer(&peer_id, false).await,
            MeshCmd::Chat { to_fp, text } => {
                let my_fp = self.identity.fingerprint();
                let m = identity::sign_message(&self.identity, ChatWire {
                    version: 2, kind: "chat.message".into(), channel_id: identity::dm_channel(&my_fp, &to_fp), id: uuid::Uuid::new_v4().to_string(),
                    author_peer_id: my_fp, author: self.cfg.display_name.clone(), content: text, timestamp: now_ms() as u64, signature: String::new() });
                self.outbox.entry(to_fp.clone()).or_default().push(m);
                self.flush_outbox(&to_fp).await;
            }
            MeshCmd::Close => {}
        }
    }

    async fn flush_outbox(&mut self, fp: &str) {
        let Some(list) = self.outbox.get(fp) else { return };
        let Some(peer) = self.peers.values().find(|p| p.fp.as_deref() == Some(fp) && p.sent_proof && p.proven) else { return };
        let Some(dc) = peer.dc.clone() else { return };
        for m in list { if let Ok(t) = serde_json::to_string(m) { let _ = dc.send_text(t).await; } }
    }

    async fn on_internal(&mut self, i: Internal) {
        match i {
            Internal::Ice { peer, gen, init } => { if self.gen_ok(&peer, gen) { self.send_signal("webrtc.ice-candidate", &peer, json!({ "candidate": init })).await; } }
            Internal::PcState { peer, gen, state } => {
                if !self.gen_ok(&peer, gen) { return; }
                let s = match state { RTCPeerConnectionState::Connected => "conectado", RTCPeerConnectionState::Failed => "falhou", RTCPeerConnectionState::Closed => "fechado", RTCPeerConnectionState::Disconnected => "falhou", _ => "negociando" };
                self.emit(MeshEvent::Peer { peer_id: peer, state: s.into() }).await;
            }
            Internal::DcAttach { peer, gen, dc } => {
                if !self.gen_ok(&peer, gen) { return; }
                self.wire_dc(&peer, gen, &dc);
                if let Some(p) = self.peers.get_mut(&peer) { p.dc = Some(dc); }
            }
            Internal::DcOpen { peer, gen } => { if self.gen_ok(&peer, gen) { self.send_challenge(&peer).await; } }
            Internal::DcMsg { peer, gen, text } => { if self.gen_ok(&peer, gen) { self.on_dc_text(&peer, &text).await; } }
        }
    }

    fn gen_ok(&self, peer: &str, gen: u32) -> bool { self.peers.get(peer).map(|p| p.gen == gen).unwrap_or(false) }

    async fn dc_send(&self, peer: &str, v: Value) {
        if let Some(dc) = self.peers.get(peer).and_then(|p| p.dc.clone()) { let _ = dc.send_text(v.to_string()).await; }
    }

    async fn send_challenge(&mut self, peer: &str) {
        let nonce = uuid::Uuid::new_v4().to_string();
        if let Some(p) = self.peers.get_mut(peer) { p.my_nonce = nonce.clone(); }
        self.dc_send(peer, json!({ "version": 2, "type": "chat.identity.challenge", "channelId": "native", "fromPeerId": self.cfg.local_peer, "nonce": nonce, "timestamp": now_ms(), "capabilities": caps() })).await;
    }

    async fn on_dc_text(&mut self, peer: &str, text: &str) {
        let Ok(v) = serde_json::from_str::<Value>(text) else { return };
        match v["type"].as_str().unwrap_or("") {
            "chat.identity.challenge" => {
                let nonce = v["nonce"].as_str().unwrap_or("").to_string();
                let ts = now_ms();
                let canon = identity::canonical_identity_proof(v["channelId"].as_str().unwrap_or("native"), &self.cfg.local_peer, peer_remote(&v), &nonce, ts as u64, &caps());
                let sig = self.identity.sign(&canon);
                self.dc_send(peer, json!({ "version": 2, "type": "chat.identity.proof", "channelId": v["channelId"], "fromPeerId": self.cfg.local_peer, "toPeerId": peer_remote(&v), "nonce": nonce, "timestamp": ts, "capabilities": caps(), "signature": sig, "identity": self.identity.public_jwk(), "displayName": self.cfg.display_name })).await;
                if let Some(p) = self.peers.get_mut(peer) { p.sent_proof = true; }
                self.after_auth(peer).await;
            }
            "chat.identity.proof" => {
                let (Some(nonce), Some(sig), Ok(jwk)) = (v["nonce"].as_str(), v["signature"].as_str(), serde_json::from_value::<PublicJwk>(v["identity"].clone())) else { return };
                let Some(p) = self.peers.get_mut(peer) else { return };
                if p.my_nonce.is_empty() || nonce != p.my_nonce { return; }
                let canon = identity::canonical_identity_proof(v["channelId"].as_str().unwrap_or("native"), v["fromPeerId"].as_str().unwrap_or(""), v["toPeerId"].as_str().unwrap_or(""), nonce, v["timestamp"].as_u64().unwrap_or(0), &v["capabilities"]);
                if !identity::verify(&jwk, &canon, sig) { self.emit(MeshEvent::Error("prova de identidade inválida".into())).await; return; }
                p.fp = Some(identity::fingerprint(&jwk)); p.jwk = Some(jwk); p.proven = true; p.my_nonce.clear();
                self.after_auth(peer).await;
            }
            "chat.message" => {
                let Ok(m) = serde_json::from_value::<ChatWire>(v.clone()) else { return };
                let Some(p) = self.peers.get(peer) else { return };
                let (Some(jwk), Some(fp)) = (p.jwk.clone(), p.fp.clone()) else { return };   // sem identidade provada, ignora
                let expected = identity::dm_channel(&self.identity.fingerprint(), &fp);
                if m.author_peer_id != fp || identity::verify_message(&jwk, &m, &expected).is_err() { self.emit(MeshEvent::Error("mensagem rejeitada (assinatura/canal)".into())).await; return; }
                self.dc_send(peer, json!({ "version": 2, "type": "chat.message.ack", "channelId": m.channel_id, "messageId": m.id })).await;
                if self.seen_msgs.insert(m.id.clone()) {
                    self.emit(MeshEvent::Chat { channel: m.channel_id, from_fp: fp, author: m.author, content: m.content, id: m.id, ts: m.timestamp }).await;
                }
            }
            "chat.message.ack" => {
                let id = v["messageId"].as_str().unwrap_or("");
                if let Some(fp) = self.peers.get(peer).and_then(|p| p.fp.clone()) { if let Some(l) = self.outbox.get_mut(&fp) { l.retain(|m| m.id != id); } }
            }
            _ => {}
        }
    }

    async fn after_auth(&mut self, peer: &str) {
        let (ok, fp) = match self.peers.get(peer) { Some(p) => (p.sent_proof && p.proven, p.fp.clone()), None => return };
        if ok {
            self.emit(MeshEvent::Peer { peer_id: peer.into(), state: "autenticado".into() }).await;
            if let Some(fp) = fp { self.flush_outbox(&fp).await; }
        }
    }
}

fn peer_remote(v: &Value) -> &str { v["fromPeerId"].as_str().unwrap_or("") }
fn caps() -> Value { json!({ "protocol": 2, "client": "native-0.1", "capabilities": [] }) }
