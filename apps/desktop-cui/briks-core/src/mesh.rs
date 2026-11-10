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
use webrtc::api::interceptor_registry::register_default_interceptors;
use webrtc::api::media_engine::MediaEngine;
use webrtc::api::APIBuilder;
use webrtc::data_channel::data_channel_message::DataChannelMessage;
use webrtc::interceptor::registry::Registry;
use webrtc::track::track_local::TrackLocal;
use webrtc::data_channel::data_channel_state::RTCDataChannelState;
use webrtc::data_channel::RTCDataChannel;
use webrtc::ice_transport::ice_candidate::RTCIceCandidateInit;
use webrtc::ice_transport::ice_server::RTCIceServer;
use webrtc::peer_connection::configuration::RTCConfiguration;
use webrtc::peer_connection::peer_connection_state::RTCPeerConnectionState;
use webrtc::peer_connection::sdp::session_description::RTCSessionDescription;
use webrtc::peer_connection::signaling_state::RTCSignalingState;
use webrtc::peer_connection::RTCPeerConnection;

use crate::audio::{Audio, AudioEvent};
use crate::share::{self, RecvState, ShareCfg};
use webrtc::rtcp::payload_feedbacks::full_intra_request::FullIntraRequest;
use webrtc::rtcp::payload_feedbacks::picture_loss_indication::PictureLossIndication;
use webrtc::rtp_transceiver::rtp_sender::RTCRtpSender;
use webrtc::track::track_local::track_local_static_sample::TrackLocalStaticSample;
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
    /// `peer_id` = "me" para o microfone local; `level` 0..1 (0 = parou).
    Speaking { peer_id: String, level: f32 },
    /// Estado de microfone/ensurdecido anunciado pelo peer (autenticado).
    PeerState { peer_id: String, microphone: bool, deafened: bool, status: String, presence: String },
    /// Identidade provada de um peer (impressão digital e nome de exibição).
    PeerInfo { peer_id: String, fp: String, name: String },
    /// Quem é o dono da sala segundo este nó e se sou eu (quem pode expulsar).
    Owner { fp: String, me: bool },
    /// Fui expulso pelo dono: o core já fechou tudo.
    Kicked { reason: String },
    /// Alguém foi expulso (ou eu mesmo apliquei): fp banido nesta sessão.
    Banned { fp: String },
    /// Eventos da transmissão de tela (JSON já no formato do IPC: "share.local", "share.stats", "share.remote"...).
    Share(Value),
    Error(String),
}

pub enum MeshCmd {
    /// Expulsa (só o dono da sala): mensagem assinada para todos os peers autenticados e banimento nesta sessão.
    Kick { fp: String, reason: String },
    /// Só testes: expulsa sem checar se sou o dono (para provar que os outros ignoram quem não é).
    #[cfg(test)]
    KickUnchecked { fp: String },
    /// O dono permite a volta de alguém banido.
    Allow { fp: String },
    /// Reenvia o estado local (microfone/ensurdecido) a todos os peers autenticados.
    BroadcastState,
    /// Status (texto do balão) e presença (programa em uso) que os peers autenticados veem; vazio = nada.
    SetProfile { status: String, presence: String },
    /// Silencia (ganho zero) um peer por peer_id ou impressão digital.
    MutePeer { key: String, muted: bool },
    /// Envia texto para a identidade `to_fp` (impressão digital), por DM. Fica na outbox até haver DataChannel autenticado.
    Chat { to_fp: String, text: String },
    /// Só para testes: força um offer agora (colisão).
    ForceOffer { peer_id: String },
    /// Começa a transmitir (faixa de vídeo para todos os peers autenticados, com renegociação).
    ShareStart { cfg: ShareCfg, preview: Option<std::path::PathBuf>, dump: Option<std::path::PathBuf> },
    ShareStop,
    /// Troca os processos capturados durante a transmissão ("Escolher apps"); só vale se ela começou com áudio.
    ShareAudio { mode: crate::procaudio::AudioMode },
    /// Liga/desliga a decodificação do vídeo de um peer e define o arquivo de quadro compartilhado.
    ShareWatch { key: String, on: bool, path: std::path::PathBuf },
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
    name: String,
    vsender: Option<Arc<RTCRtpSender>>,
    asender: bool,   // faixa de áudio da transmissão já ligada a este peer
    recv: Arc<RecvState>,
}

struct ActiveShare { audio: Option<crate::procaudio::ProcAudio>, has_audio: bool, gen: u64, track: Arc<TrackLocalStaticSample>, sender: share::Sender, info: Option<Value> }

pub struct MeshCfg {
    pub namespace: String,
    pub room: String,
    pub local_peer: String,
    pub display_name: String,
    pub ice_servers: Vec<RTCIceServer>,
    /// Áudio da chamada (None = só DataChannel, como nos testes de chat).
    pub audio: Option<(Arc<Audio>, mpsc::Receiver<AudioEvent>)>,
    /// Dono da sala (impressão digital). Sem isso vale o mais antigo (joinedAt) entre os autenticados: mais fraco (joinedAt é declarado
    /// pelo próprio peer), então o app deve passar o dono real quando o conhecer (criador do grupo/sala).
    pub owner_fp: Option<String>,
    pub joined_at: u64,
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
    audio: Option<Arc<Audio>>,
    audio_rx: Option<mpsc::Receiver<AudioEvent>>,
    joined: HashMap<String, u64>,   // peerId -> joinedAt da presença
    banned: HashSet<String>,        // fps expulsos nesta sessão
    profile: (String, String),      // (status, presença) anunciados aos peers autenticados
    dead: bool,                     // fui expulso: encerra o ator
    blocked: HashSet<String>,       // peerIds de banidos: sinalização deles é ignorada
    share: Option<ActiveShare>,
    share_gen: u64,
    net: Arc<std::sync::Mutex<crate::adapt::NetShared>>,   // perda/RTT/NACK por espectador (RTCP), lidos pelo laço de adaptação
    idr: Arc<std::sync::atomic::AtomicBool>,   // pedido de quadro-chave (PLI/FIR/novo espectador), único para a sala toda
    vtrack: Option<Arc<TrackLocalStaticSample>>,
    atrack: Option<Arc<TrackLocalStaticSample>>,   // áudio da transmissão (só do processo), também persistente   // criada na 1ª transmissão e mantida: parar/retomar não renegocia nem troca de SSRC
    sev_tx: mpsc::Sender<Value>,
    sev_rx: Option<mpsc::Receiver<Value>>,
}

impl Actor {
    fn new(mut cfg: MeshCfg, identity: Arc<Identity>, rt: Realtime, ev: mpsc::Sender<MeshEvent>) -> Self {
        let (audio, audio_rx) = match cfg.audio.take() { Some((a, r)) => (Some(a), Some(r)), None => (None, None) };
        let room_hash = secure_room_id(&cfg.namespace, &cfg.room);
        let (itx, irx) = mpsc::channel(512);
        let validator = Validator::new(&room_hash, &cfg.local_peer);
        let (sev_tx, sev_rx) = mpsc::channel::<Value>(256);
        Actor { cfg, identity, rt, ev, room_hash, validator, present: HashSet::new(), peers: HashMap::new(), itx, irx: Some(irx), next_gen: 1, outbox: HashMap::new(), seen_msgs: HashSet::new(), audio, audio_rx, joined: HashMap::new(), banned: HashSet::new(), profile: (String::new(), String::new()), dead: false, blocked: HashSet::new(), share: None, share_gen: 0, net: Default::default(), idr: Arc::new(std::sync::atomic::AtomicBool::new(true)), vtrack: None, atrack: None, sev_tx, sev_rx: Some(sev_rx) }
    }

    async fn run(&mut self, mut rt_events: mpsc::Receiver<RtEvent>, mut cmds: mpsc::Receiver<MeshCmd>) {
        let mut irx = self.irx.take().expect("irx");
        let mut retry = tokio::time::interval(std::time::Duration::from_secs(2));
        let mut arx = self.audio_rx.take();
        let mut srx = self.sev_rx.take().expect("sev_rx");
        if let Some(a) = &self.audio { a.start(); }
        loop {
            tokio::select! {
                ae = async { match arx.as_mut() { Some(r) => r.recv().await, None => std::future::pending().await } } => match ae {
                    Some(AudioEvent::Speaking { peer, level }) => self.emit(MeshEvent::Speaking { peer_id: peer, level }).await,
                    Some(AudioEvent::Error(m)) => self.emit(MeshEvent::Error(format!("áudio: {m}"))).await,
                    None => { arx = None; }
                },
                sv = srx.recv() => if let Some(v) = sv { self.on_share_event(v).await },
                _ = retry.tick() => {
                    // mensagens sem ACK são reenviadas (o receptor deduplica por id)
                    let fps: Vec<String> = self.outbox.iter().filter(|(_, l)| !l.is_empty()).map(|(k, _)| k.clone()).collect();
                    for fp in fps { self.flush_outbox(&fp).await; }
                    self.retry_auth().await;
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
            if self.dead { break; }
        }
        if let Some(a) = &self.audio { a.stop(); }
        if let Some(sh) = self.share.take() { sh.sender.stop(); if let Some(a) = &sh.audio { a.stop(); } }
        for (_, p) in self.peers.drain() { let _ = p.pc.close().await; }
        self.rt.close().await;
    }

    async fn emit(&self, e: MeshEvent) { let _ = self.ev.send(e).await; }

    async fn on_rt(&mut self, e: RtEvent) {
        match e {
            RtEvent::Status(s) => { if let Some(msg) = s.strip_prefix("error: ") { self.emit(MeshEvent::Error(msg.to_string())).await; } }
            RtEvent::PeerJoined { peer_id, meta } => {
                self.present.insert(peer_id.clone());
                self.joined.insert(peer_id.clone(), meta["joinedAt"].as_u64().unwrap_or(u64::MAX));
                if self.blocked.contains(&peer_id) || self.peers.len() >= MAX_PEERS || self.peers.contains_key(&peer_id) { return; }
                if let Err(e) = self.add_peer(&peer_id).await { self.emit(MeshEvent::Error(format!("peer {peer_id}: {e}"))).await; return; }
                if is_initiator(&self.cfg.local_peer, &peer_id) { self.start_offer(&peer_id, true).await; }
            }
            RtEvent::PeerLeft(p) => {
                self.present.remove(&p);
                self.net.lock().unwrap().peers.remove(&p);
                if let Some(a) = &self.audio { a.remove_peer(&p); }
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

    async fn new_pc(&mut self, peer_id: &str, gen: u32, recv: Arc<RecvState>) -> Result<Arc<RTCPeerConnection>> {
        let mut m = MediaEngine::default();
        m.register_default_codecs()?;
        let registry = register_default_interceptors(Registry::new(), &mut m)?;
        let api = APIBuilder::new().with_media_engine(m).with_interceptor_registry(registry).build();
        let pc = Arc::new(api.new_peer_connection(self.rtc_config()).await?);
        {
            let audio = self.audio.clone();
            if let Some(a) = &audio { pc.add_track(a.track.clone() as Arc<dyn TrackLocal + Send + Sync>).await?; }
            let (id, sev) = (peer_id.to_string(), self.sev_tx.clone());
            let wk = Arc::downgrade(&pc);
            pc.on_track(Box::new(move |track, _rx, _tr| {
                let (audio, id, sev, recv, wk) = (audio.clone(), id.clone(), sev.clone(), recv.clone(), wk.clone());
                Box::pin(async move {
                    use webrtc::rtp_transceiver::rtp_codec::RTPCodecType;
                    if track.kind() == RTPCodecType::Video {
                        let ssrc = track.ssrc();
                        let pli: Arc<dyn Fn() + Send + Sync> = Arc::new(move || {
                            if let Some(pc) = wk.upgrade() { tokio::spawn(async move { let _ = pc.write_rtcp(&[Box::new(PictureLossIndication { sender_ssrc: 0, media_ssrc: ssrc })]).await; }); }
                        });
                        tokio::spawn(share::run_remote_video(id, track, recv, sev, pli));
                    } else if let Some(a) = audio {
                        let key = if track.id() == share::AUDIO_TRACK_ID { format!("{id}#screen") } else { id };
                        tokio::spawn(a.run_remote(key, track));
                    }
                })
            }));
        }
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
        let recv = Arc::new(RecvState::default());
        let pc = self.new_pc(peer_id, gen, recv.clone()).await?;
        let polite = !is_initiator(&self.cfg.local_peer, peer_id);
        self.peers.insert(peer_id.into(), Peer { pc, gen, polite, making_offer: false, ignore_offer: false, pending_ice: vec![], dc: None, fp: None, jwk: None, my_nonce: String::new(), proven: false, sent_proof: false, name: String::new(), vsender: None, asender: false, recv });
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
        #[cfg(test)]
        let payload = { let mut p = payload; if crate::netsim::for_peer(&self.cfg.local_peer).is_some() { if let Some(s) = p["sdp"]["sdp"].as_str().map(crate::netsim::strip_sdp) { p["sdp"]["sdp"] = json!(s); } } p };
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
        if self.blocked.contains(&from) { return; }
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
            MeshCmd::Kick { fp, reason } => {
                if self.owner().as_deref() != Some(self.identity.fingerprint().as_str()) { self.emit(MeshEvent::Error("só o dono da sala pode expulsar".into())).await; return; }
                self.do_kick(&fp, &reason).await;
            }
            #[cfg(test)]
            MeshCmd::KickUnchecked { fp } => self.do_kick(&fp, "teste").await,
            MeshCmd::Allow { fp } => {
                if self.banned.remove(&fp) {
                    // propaga a permissão para que os outros também aceitem a volta (só o dono tem o direito; os demais verificam a assinatura)
                    let ts = now_ms();
                    let canon = self.allow_canon(&fp, ts);
                    let sig = self.identity.sign(&canon);
                    let ids: Vec<String> = self.peers.iter().filter(|(_, p)| p.sent_proof && p.proven).map(|(k, _)| k.clone()).collect();
                    for id in ids { self.dc_send(&id, json!({ "version": 2, "type": "room.allow", "target_fp": fp, "timestamp": ts, "signature": sig })).await; }
                }
            }
            MeshCmd::BroadcastState => {
                let ids: Vec<String> = self.peers.iter().filter(|(_, p)| p.sent_proof && p.proven).map(|(k, _)| k.clone()).collect();
                for id in ids { self.send_state(&id).await; }
            }
            MeshCmd::SetProfile { status, presence } => {
                self.profile = (status.chars().take(64).collect(), presence.chars().take(64).collect());
                let ids: Vec<String> = self.peers.iter().filter(|(_, p)| p.sent_proof && p.proven).map(|(k, _)| k.clone()).collect();
                for id in ids { self.send_state(&id).await; }
            }
            MeshCmd::MutePeer { key, muted } => {
                if let Some(a) = &self.audio {
                    let id = self.peers.iter().find(|(k, p)| *k == &key || p.fp.as_deref() == Some(key.as_str())).map(|(k, _)| k.clone()).unwrap_or(key);
                    a.mute_peer(&id, muted);
                }
            }
            MeshCmd::ShareStart { cfg, preview, dump } => self.share_start(cfg, preview, dump).await,
            MeshCmd::ShareStop => self.share_stop().await,
            MeshCmd::ShareAudio { mode } => { if let Some(a) = self.share.as_ref().and_then(|s| s.audio.as_ref()) { a.set_mode(mode); } }
            MeshCmd::ShareWatch { key, on, path } => {
                let found = self.peers.iter().find(|(k, p)| *k == &key || p.fp.as_deref() == Some(key.as_str())).map(|(k, p)| (k.clone(), p.recv.clone()));
                match found {
                    Some((id, r)) => {
                        if on { *r.sink_path.lock().unwrap() = Some(path.clone()); r.epoch.fetch_add(1, std::sync::atomic::Ordering::Relaxed); r.frames.store(0, std::sync::atomic::Ordering::Relaxed); }
                        r.watching.store(on, std::sync::atomic::Ordering::Relaxed);
                        self.emit(MeshEvent::Share(json!({ "ev": "share.remote", "peer_id": id, "state": if on { "watch" } else { "unwatch" }, "path": path.to_string_lossy() }))).await;
                    }
                    None => self.emit(MeshEvent::Error("assistir: peer desconhecido".into())).await,
                }
            }
            MeshCmd::Close => {}
        }
    }

    fn authed_ids(&self) -> Vec<String> { self.peers.iter().filter(|(_, p)| p.sent_proof && p.proven).map(|(k, _)| k.clone()).collect() }

    async fn share_start(&mut self, cfg: ShareCfg, preview: Option<std::path::PathBuf>, dump: Option<std::path::PathBuf>) {
        self.share_stop().await;
        let track = self.vtrack.get_or_insert_with(share::new_video_track).clone();
        self.share_gen += 1;
        let gen = self.share_gen;
        let amode = cfg.audio.clone();
        let atrack = self.atrack.get_or_insert_with(crate::procaudio::new_audio_track).clone();
        match share::start_sender(gen, self.idr.clone(), self.net.clone(), cfg, track.clone(), self.sev_tx.clone(), preview, dump) {
            Ok(sender) => {
                let has_audio = amode != crate::procaudio::AudioMode::None;
                let audio = if has_audio { Some(crate::procaudio::start(amode, atrack)) } else { None };
                self.share = Some(ActiveShare { audio, has_audio, gen, track, sender, info: None });
            }
            Err(e) => { self.emit(MeshEvent::Share(json!({ "ev": "share.local", "state": "off", "reason": e }))).await; return; }
        }
        for id in self.authed_ids() { self.attach_share(&id).await; }
    }

    /// Adiciona a faixa de vídeo ao peer (se ainda não tem), lê o RTCP (PLI/FIR) e renegocia.
    async fn attach_share(&mut self, id: &str) {
        let Some(sh) = &self.share else { return };
        let (track, idr) = (sh.track.clone(), sh.sender.idr.clone());
        let Some(p) = self.peers.get_mut(id) else { return };
        if p.vsender.is_some() {
            idr.store(true, std::sync::atomic::Ordering::Relaxed);
            if let Some(info) = self.share.as_ref().and_then(|s| s.info.clone()) { self.send_share_state(id, &info).await; }
            return;
        }
        if !p.asender { if let Some(at) = self.atrack.clone() { if p.pc.add_track(at as Arc<dyn TrackLocal + Send + Sync>).await.is_ok() { p.asender = true; } } }
        match p.pc.add_track(track as Arc<dyn TrackLocal + Send + Sync>).await {
            Ok(sender) => {
                let (s2, idr2, net, pid) = (sender.clone(), idr.clone(), self.net.clone(), id.to_string());
                tokio::spawn(async move {
                    use webrtc::rtcp::receiver_report::ReceiverReport;
                    use webrtc::rtcp::transport_feedbacks::transport_layer_nack::TransportLayerNack;
                    let ssrc = s2.get_parameters().await.encodings.first().map(|e| e.ssrc).unwrap_or(0);
                    while let Ok((pkts, _)) = s2.read_rtcp().await {
                        for pk in pkts {
                            let any = pk.as_any();
                            let mut n = net.lock().unwrap();
                            let e = n.peers.entry(pid.clone()).or_insert(crate::adapt::PeerNet { loss: 0.0, rtt_ms: 0.0, jitter_ms: 0.0, updated: std::time::Instant::now(), nacks: 0, plis: 0 });
                            if any.downcast_ref::<PictureLossIndication>().is_some() || any.downcast_ref::<FullIntraRequest>().is_some() {
                                e.plis += 1; idr2.store(true, std::sync::atomic::Ordering::Relaxed);
                            } else if let Some(rr) = any.downcast_ref::<ReceiverReport>() {
                                for r in rr.reports.iter().filter(|r| r.ssrc == ssrc) {
                                    e.loss = r.fraction_lost as f32 / 256.0;
                                    e.jitter_ms = r.jitter as f32 / 90.0;
                                    if r.last_sender_report != 0 {
                                        // RTT = agora − LSR − DLSR, em unidades de 1/65536 s (32 bits do meio do NTP)
                                        let now = (webrtc::rtp::extension::abs_send_time_extension::unix2ntp(std::time::SystemTime::now()) >> 16) as u32;
                                        let d = now.wrapping_sub(r.last_sender_report).wrapping_sub(r.delay);
                                        if d < 10 * 65536 { e.rtt_ms = d as f32 * 1000.0 / 65536.0; }
                                    }
                                    e.updated = std::time::Instant::now();
                                }
                            } else if let Some(nk) = any.downcast_ref::<TransportLayerNack>() {
                                if nk.media_ssrc == ssrc { e.nacks += nk.nacks.iter().map(|p| 1 + p.lost_packets.count_ones() as u64).sum::<u64>(); }
                            }
                        }
                    }
                    net.lock().unwrap().peers.remove(&pid);
                });
                p.vsender = Some(sender);
                idr.store(true, std::sync::atomic::Ordering::Relaxed);
            }
            Err(e) => { self.emit(MeshEvent::Error(format!("faixa de vídeo: {e}"))).await; return; }
        }
        self.start_offer(id, false).await;
        if let Some(info) = self.share.as_ref().and_then(|s| s.info.clone()) { self.send_share_state(id, &info).await; }
    }

    /// Para o envio. A faixa continua ligada aos peers (retomar não renegocia); os espectadores são avisados pelo DataChannel.
    async fn share_stop(&mut self) {
        let Some(sh) = self.share.take() else { return };
        sh.sender.stop();
        if let Some(a) = &sh.audio { a.stop(); }
        for id in self.authed_ids() { self.dc_send(&id, json!({ "version": 2, "type": "share.state", "on": false })).await; }
    }

    async fn send_share_state(&self, peer: &str, info: &Value) {
        let mut m = json!({ "version": 2, "type": "share.state", "on": true });
        for k in ["w", "h", "fps", "kbps", "label", "audio"] { if let Some(v) = info.get(k) { m[k] = v.clone(); } }
        self.dc_send(peer, m).await;
    }

    /// Eventos do emissor (thread de captura/codificação) e do receptor.
    async fn on_share_event(&mut self, mut v: Value) {
        // eventos de um emissor antigo (já parado) chegam atrasados: ignora
        if let Some(g) = v["gen"].as_u64() { if self.share.as_ref().map(|s| s.gen) != Some(g) && !(v["ev"] == "share.local" && v["state"] == "off" && self.share.is_none()) { return; } }
        if let Some(sh) = &self.share {
            if v["ev"] == "share.local" && v["state"] == "on" { v["audio"] = json!(sh.has_audio); }
            if v["ev"] == "share.stats" { if let Some(a) = &sh.audio {
                use std::sync::atomic::Ordering::Relaxed;
                v["audio_peak"] = json!(a.stats.peak_milli.load(Relaxed) as f64 / 1000.0); v["audio_sources"] = json!(a.stats.sources.load(Relaxed)); v["audio_frames"] = json!(a.stats.frames.load(Relaxed));
            } }
        }
        if v["ev"] == "share.local" {
            if v["state"] == "on" {
                if let Some(sh) = self.share.as_mut() { sh.info = Some(v.clone()); }
                for id in self.authed_ids() { self.send_share_state(&id, &v).await; }
            } else { self.share_stop().await; }
        }
        self.emit(MeshEvent::Share(v)).await;
    }

    /// O desafio pode chegar antes de o outro lado ligar o on_message (corrida com 3+ peers): reenvia, com o MESMO nonce, a quem ainda não provou.
    async fn retry_auth(&mut self) {
        let ids: Vec<String> = self.peers.iter().filter(|(_, p)| !p.proven && p.dc.as_ref().map(|d| d.ready_state() == RTCDataChannelState::Open).unwrap_or(false)).map(|(k, _)| k.clone()).collect();
        for id in ids {
            let nonce = self.peers.get(&id).map(|p| p.my_nonce.clone()).unwrap_or_default();
            if nonce.is_empty() { self.send_challenge(&id).await; }
            else { self.dc_send(&id, json!({ "version": 2, "type": "chat.identity.challenge", "channelId": "native", "fromPeerId": self.cfg.local_peer, "nonce": nonce, "timestamp": now_ms(), "capabilities": caps() })).await; }
        }
    }

    /// Dono da sala segundo este nó (ver `MeshCfg::owner_fp`).
    fn owner(&self) -> Option<String> {
        if let Some(o) = &self.cfg.owner_fp { return Some(o.clone()); }
        let mut best: (u64, String) = (self.cfg.joined_at, self.identity.fingerprint());
        for (id, p) in &self.peers {
            if let (true, Some(fp)) = (p.proven && p.sent_proof, p.fp.clone()) {
                let j = self.joined.get(id).copied().unwrap_or(u64::MAX);
                if (j, fp.as_str()) < (best.0, best.1.as_str()) { best = (j, fp); }
            }
        }
        Some(best.1)
    }

    fn kick_canon(&self, target_fp: &str, ts: i64, reason: &str) -> String { format!("briks.kick.v1|{}|{}|{}|{}", self.room_hash, target_fp, ts, reason) }
    fn allow_canon(&self, target_fp: &str, ts: i64) -> String { format!("briks.allow.v1|{}|{}|{}", self.room_hash, target_fp, ts) }

    async fn do_kick(&mut self, fp: &str, reason: &str) {
        let reason: String = reason.chars().take(120).collect();
        let ts = now_ms();
        let sig = self.identity.sign(&self.kick_canon(fp, ts, &reason));
        let ids: Vec<String> = self.peers.iter().filter(|(_, p)| p.sent_proof && p.proven).map(|(k, _)| k.clone()).collect();
        for id in ids { self.dc_send(&id, json!({ "version": 2, "type": "room.kick", "target_fp": fp, "reason": reason, "timestamp": ts, "signature": sig })).await; }
        tokio::time::sleep(std::time::Duration::from_millis(300)).await;   // dá tempo de a mensagem sair antes de fechar a conexão com o alvo
        self.ban(fp).await;
    }

    /// Bane o fp nesta sessão e fecha a(s) conexão(ões) com ele.
    async fn ban(&mut self, fp: &str) {
        self.banned.insert(fp.to_string());
        let ids: Vec<String> = self.peers.iter().filter(|(_, p)| p.fp.as_deref() == Some(fp)).map(|(k, _)| k.clone()).collect();
        for id in ids { self.blocked.insert(id.clone()); self.drop_peer(&id).await; }
        self.emit(MeshEvent::Banned { fp: fp.to_string() }).await;
    }

    async fn drop_peer(&mut self, id: &str) {
        self.net.lock().unwrap().peers.remove(id);
        if let Some(a) = &self.audio { a.remove_peer(id); }
        if let Some(p) = self.peers.remove(id) { let _ = p.pc.close().await; self.emit(MeshEvent::Peer { peer_id: id.into(), state: "fechado".into() }).await; }
    }

    async fn send_state(&self, peer: &str) {
        let (mic, deaf) = self.audio.as_ref().map(|a| (a.mic_open(), a.deaf())).unwrap_or((true, false));
        self.dc_send(peer, json!({ "version": 2, "type": "peer.state", "microphone": mic, "deafened": deaf, "status": self.profile.0, "presence": self.profile.1 })).await;
    }

    async fn flush_outbox(&mut self, fp: &str) {
        let Some(list) = self.outbox.get(fp) else { return };
        let Some(peer) = self.peers.values().find(|p| p.fp.as_deref() == Some(fp) && p.sent_proof && p.proven) else { return };
        let Some(dc) = peer.dc.clone() else { return };
        for m in list { if let Ok(t) = serde_json::to_string(m) { let _ = dc.send_text(t).await; } }
    }

    async fn on_internal(&mut self, i: Internal) {
        match i {
            Internal::Ice { peer, gen, init } => {
                if !self.gen_ok(&peer, gen) { return; }
                #[cfg(test)]
                let init = { let mut init = init; if let Some(ns) = crate::netsim::for_peer(&self.cfg.local_peer) { match ns.rewrite(&init.candidate).await { Some(c) => init.candidate = c, None => return } } init };
                self.send_signal("webrtc.ice-candidate", &peer, json!({ "candidate": init })).await;
            }
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
                p.name = v["displayName"].as_str().unwrap_or("").chars().take(64).collect();
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
            "room.kick" | "room.allow" => {
                let kick = v["type"] == "room.kick";
                let Some(p) = self.peers.get(peer) else { return };
                let (true, Some(jwk), Some(from_fp)) = (p.proven && p.sent_proof, p.jwk.clone(), p.fp.clone()) else { return };
                let (Some(target), Some(sig), Some(ts)) = (v["target_fp"].as_str(), v["signature"].as_str(), v["timestamp"].as_i64()) else { return };
                if self.owner().as_deref() != Some(from_fp.as_str()) { self.emit(MeshEvent::Error("comando de sala ignorado: remetente não é o dono".into())).await; return; }
                if (now_ms() - ts).abs() > 5 * 60 * 1000 { return; }   // antigo demais (replay)
                let reason: String = v["reason"].as_str().unwrap_or("").chars().take(120).collect();
                let canon = if kick { self.kick_canon(target, ts, &reason) } else { self.allow_canon(target, ts) };
                if !identity::verify(&jwk, &canon, sig) { self.emit(MeshEvent::Error("assinatura de comando de sala inválida".into())).await; return; }
                let target = target.to_string();
                if !kick { self.banned.remove(&target); return; }
                if target == self.identity.fingerprint() {
                    // fui expulso: fecha tudo e encerra o ator; a UI mostra "Você foi removido da chamada"
                    self.banned.insert(target);
                    let ids: Vec<String> = self.peers.keys().cloned().collect();
                    for id in ids { self.drop_peer(&id).await; }
                    self.emit(MeshEvent::Kicked { reason }).await;
                    self.dead = true;
                } else { self.ban(&target).await; }
            }
            "peer.state" => {
                if self.peers.get(peer).map(|p| p.proven && p.sent_proof) != Some(true) { return; }
                self.emit(MeshEvent::PeerState { peer_id: peer.into(), microphone: v["microphone"].as_bool().unwrap_or(true), deafened: v["deafened"].as_bool().unwrap_or(false), status: v["status"].as_str().unwrap_or("").chars().take(64).collect(), presence: v["presence"].as_str().unwrap_or("").chars().take(64).collect() }).await;
            }
            "share.state" => {
                if self.peers.get(peer).map(|p| p.proven && p.sent_proof) != Some(true) { return; }
                let on = v["on"].as_bool().unwrap_or(false);
                let mut m = json!({ "ev": "share.remote", "peer_id": peer, "state": if on { "on" } else { "off" } });
                for k in ["w", "h", "fps", "kbps"] { m[k] = json!(v[k].as_u64().unwrap_or(0)); }
                m["label"] = json!(v["label"].as_str().unwrap_or("").chars().take(64).collect::<String>());
                m["audio"] = json!(v["audio"].as_bool().unwrap_or(false));
                if !on { if let Some(p) = self.peers.get(peer) { p.recv.watching.store(false, std::sync::atomic::Ordering::Relaxed); } }
                self.emit(MeshEvent::Share(m)).await;
            }
            "chat.message.ack" => {
                let id = v["messageId"].as_str().unwrap_or("");
                if let Some(fp) = self.peers.get(peer).and_then(|p| p.fp.clone()) { if let Some(l) = self.outbox.get_mut(&fp) { l.retain(|m| m.id != id); } }
            }
            _ => {}
        }
    }

    async fn after_auth(&mut self, peer: &str) {
        let (ok, fp, name) = match self.peers.get(peer) { Some(p) => (p.sent_proof && p.proven, p.fp.clone(), p.name.clone()), None => return };
        if let Some(f) = &fp { if self.banned.contains(f) { self.blocked.insert(peer.to_string()); self.drop_peer(peer).await; return; } }   // banido tentando voltar na mesma sessão
        if ok {
            if let Some(f) = &fp { self.emit(MeshEvent::PeerInfo { peer_id: peer.into(), fp: f.clone(), name }).await; }
            let me = self.identity.fingerprint();
            if let Some(o) = self.owner() { let mine = o == me; self.emit(MeshEvent::Owner { fp: o, me: mine }).await; }
            self.emit(MeshEvent::Peer { peer_id: peer.into(), state: "autenticado".into() }).await;
            if let Some(fp) = fp { self.flush_outbox(&fp).await; }
            self.send_state(peer).await;
            if self.share.is_some() { self.attach_share(peer).await; }
        }
    }
}

fn peer_remote(v: &Value) -> &str { v["fromPeerId"].as_str().unwrap_or("") }
fn caps() -> Value { json!({ "protocol": 2, "client": "native-0.1", "capabilities": [] }) }
