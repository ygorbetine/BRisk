//! Cliente LiveKit (SFU) do briks-core: protocolo de sinalização do LiveKit (WebSocket + protobuf, mensagens do crate `livekit-protocol`)
//! sobre os PeerConnections do webrtc-rs que o resto do core já usa. Nada de libwebrtc: os mesmos quadros H264 já codificados
//! pelo nosso encoder (MFT/NVENC com fallback), o mesmo Opus e o mesmo adapt.rs/NACK/PLI vão para o SFU. Decisão em docs/plans/sfu-livekit-nativo.md.
//!
//! Dois PeerConnections, como o LiveKit exige: o SUBSCRIBER recebe (o servidor manda a offer, nós respondemos) e o PUBLISHER envia
//! (pedimos `AddTrack`, o servidor responde `TrackPublished`, só então ligamos a faixa e mandamos a offer).
//! Eventos e comandos usam os mesmos tipos da malha (`MeshEvent`/`MeshCmd`), então a UI e o IPC não mudam por causa do modo.
use std::collections::{HashMap, HashSet};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::{anyhow, Result};
use futures_util::stream::{SplitSink, SplitStream};
use futures_util::{SinkExt, StreamExt};
use livekit_protocol as lk;
use lk::signal_request as rq;
use lk::signal_response as rs;
use prost::Message as _;
use serde_json::{json, Value};
use tokio::sync::mpsc;
use tokio_tungstenite::tungstenite::Message;
use tokio_tungstenite::WebSocketStream;
use webrtc::api::media_engine::MediaEngine;
use webrtc::api::APIBuilder;
use webrtc::dtls::extension::extension_use_srtp::SrtpProtectionProfile;
use webrtc::data_channel::data_channel_init::RTCDataChannelInit;
use webrtc::ice_transport::ice_candidate::RTCIceCandidateInit;
use webrtc::ice_transport::ice_server::RTCIceServer;
use webrtc::interceptor::registry::Registry;
use webrtc::peer_connection::configuration::RTCConfiguration;
use webrtc::peer_connection::peer_connection_state::RTCPeerConnectionState;
use webrtc::peer_connection::sdp::session_description::RTCSessionDescription;
use webrtc::peer_connection::RTCPeerConnection;
use webrtc::rtcp::payload_feedbacks::full_intra_request::FullIntraRequest;
use webrtc::rtcp::payload_feedbacks::picture_loss_indication::PictureLossIndication;
use webrtc::rtp_transceiver::rtp_codec::RTPCodecType;
use webrtc::rtp_transceiver::rtp_sender::RTCRtpSender;
use webrtc::track::track_local::TrackLocal;

use crate::audio::{Audio, AudioEvent};
use crate::mesh::{MeshCmd, MeshEvent};
use crate::realtime::Io;
use crate::share::{self, RecvState, ShareCfg};

type Ws = WebSocketStream<Box<dyn Io>>;
type WsTx = SplitSink<Ws, Message>;
type WsRx = SplitStream<Ws>;

/// Versão do protocolo de sinalização pedida ao servidor (o servidor aceita uma faixa de versões).
pub const PROTOCOL: u32 = 15;

pub struct SfuJoin {
    /// `wss://host` (ou `ws://127.0.0.1:7880` em desenvolvimento). Já validada por `sfu_cfg`.
    pub url: String,
    /// JWT de acesso à sala, emitido pela função `livekit-token` (ou, só em testes, por um segredo de desenvolvimento).
    pub token: String,
    pub audio: Option<(Arc<Audio>, mpsc::Receiver<AudioEvent>)>,
}

#[derive(Default)]
pub struct SfuStats {
    pub sub_connected: AtomicBool,
    pub pub_connected: AtomicBool,
    pub tracks_in: AtomicU64,
    pub tracks_out: AtomicU64,
    pub ws_msgs: AtomicU64,
}

pub struct SfuHandle {
    pub cmd: mpsc::Sender<MeshCmd>,
    pub events: mpsc::Receiver<MeshEvent>,
    pub stats: Arc<SfuStats>,
    /// identidade deste cliente na sala (a do token)
    pub identity: String,
    pub server: String,
}

#[derive(Clone, Debug)]
struct TrackMeta { identity: String, source: i32, ttype: i32, name: String, w: u32, h: u32, muted: bool }

#[derive(Default)]
struct Shared {
    tracks: HashMap<String, TrackMeta>,     // sid da faixa -> dono e tipo
    part_ident: HashMap<String, String>,    // sid do participante -> identity
    recv: HashMap<String, Arc<RecvState>>,  // identity -> estado de recepção do vídeo
}

enum Internal {
    Ice { target: i32, init: RTCIceCandidateInit },
    Pc { target: i32, state: RTCPeerConnectionState },
}

struct Pending { track: Arc<dyn TrackLocal + Send + Sync>, video: bool }

struct Published { sid: String, sender: Arc<RTCRtpSender> }

fn encode_req(m: rq::Message) -> Message { Message::Binary(lk::SignalRequest { message: Some(m) }.encode_to_vec()) }

fn ws_url(base: &str, token: &str) -> String {
    let b = base.trim_end_matches('/');
    let b = if let Some(r) = b.strip_prefix("https://") { format!("wss://{r}") } else if let Some(r) = b.strip_prefix("http://") { format!("ws://{r}") } else { b.to_string() };
    format!("{b}/rtc?access_token={token}&protocol={PROTOCOL}&auto_subscribe=0&sdk=rust&version=briks-0.1&adaptive_stream=0")
}

/// `publish` = PeerConnection de ENVIO: só Opus e H264 (o nosso codificador). Com todos os codecs padrão a offer lista VP8 primeiro e o
/// LiveKit registra a faixa como VP8, mesmo com a mídia H264 (visto no teste real: mime_type "video/VP8" e ninguém recebia o vídeo).
async fn build_pc(ice: &[RTCIceServer], publish: bool) -> Result<Arc<RTCPeerConnection>> {
    let mut m = MediaEngine::default();
    if publish {
        use webrtc::api::media_engine::MIME_TYPE_OPUS;
        use webrtc::rtp_transceiver::rtp_codec::{RTCRtpCodecCapability, RTCRtpCodecParameters};
        use webrtc::rtp_transceiver::RTCPFeedback;
        m.register_codec(RTCRtpCodecParameters { capability: RTCRtpCodecCapability { mime_type: MIME_TYPE_OPUS.into(), clock_rate: 48_000, channels: 2, sdp_fmtp_line: "minptime=10;useinbandfec=1".into(), rtcp_feedback: vec![] }, payload_type: 111, ..Default::default() }, RTPCodecType::Audio)?;
        let fb = vec![RTCPFeedback { typ: "goog-remb".into(), parameter: "".into() }, RTCPFeedback { typ: "ccm".into(), parameter: "fir".into() }, RTCPFeedback { typ: "nack".into(), parameter: "".into() }, RTCPFeedback { typ: "nack".into(), parameter: "pli".into() }, RTCPFeedback { typ: "transport-cc".into(), parameter: "".into() }];
        m.register_codec(RTCRtpCodecParameters { capability: RTCRtpCodecCapability { mime_type: "video/H264".into(), clock_rate: 90_000, channels: 0, sdp_fmtp_line: share::H264_FMTP.into(), rtcp_feedback: fb }, payload_type: 102, ..Default::default() }, RTPCodecType::Video)?;
    } else {
        m.register_default_codecs()?;
    }
    for parameter in ["", "pli"] { m.register_feedback(webrtc::rtp_transceiver::RTCPFeedback { typ: "nack".into(), parameter: parameter.into() }, RTPCodecType::Video); }
    let mut registry = Registry::new();
    registry.add(Box::new(webrtc::interceptor::nack::generator::Generator::builder().with_interval(Duration::from_millis(20)).with_log2_size_minus_6(5)));
    let registry = webrtc::api::interceptor_registry::configure_rtcp_reports(registry);
    let registry = webrtc::api::interceptor_registry::configure_twcc_receiver_only(registry, &mut m)?;
    let mut se = webrtc::api::setting_engine::SettingEngine::default();
    se.set_srtp_replay_protection_window(8192);
    // O webrtc-rs 0.12 anuncia AEAD_AES_256_GCM mas o SRTP dele entra em pânico com chave de 256 bits (generic-array, key_derivation.rs);
    // o LiveKit/pion prefere esse perfil. Anunciando só os de 128 bits a negociação cai num perfil que funciona.
    se.set_srtp_protection_profiles(vec![SrtpProtectionProfile::Srtp_Aead_Aes_128_Gcm, SrtpProtectionProfile::Srtp_Aes128_Cm_Hmac_Sha1_80]);
    let api = APIBuilder::new().with_media_engine(m).with_interceptor_registry(registry).with_setting_engine(se).build();
    Ok(Arc::new(api.new_peer_connection(RTCConfiguration { ice_servers: ice.to_vec(), ..Default::default() }).await?))
}

fn wire_pc(pc: &Arc<RTCPeerConnection>, target: i32, itx: &mpsc::Sender<Internal>) {
    let itx1 = itx.clone();
    pc.on_ice_candidate(Box::new(move |c| { let itx = itx1.clone(); Box::pin(async move {
        if let Some(c) = c { if let Ok(init) = c.to_json() { let _ = itx.send(Internal::Ice { target, init }).await; } } }) }));
    let itx2 = itx.clone();
    pc.on_peer_connection_state_change(Box::new(move |state| { let itx = itx2.clone(); Box::pin(async move { let _ = itx.send(Internal::Pc { target, state }).await; }) }));
}

/// Conecta, entra na sala e só devolve quando o servidor respondeu `Join` (para o chamador poder cair no P2P se falhar).
pub async fn connect(j: SfuJoin) -> Result<SfuHandle> {
    let target = crate::realtime::parse_target(&j.url)?;
    let io = tokio::time::timeout(Duration::from_secs(10), crate::realtime::open_io(&target)).await.map_err(|_| anyhow!("tempo esgotado ao conectar ao servidor de chamadas"))??;
    let hostport = format!("{}:{}", target.host, target.port);
    let url = ws_url(&format!("{}://{}", if target.tls { "wss" } else { "ws" }, hostport), &j.token);
    let (ws, _) = tokio_tungstenite::client_async(url, io).await.map_err(|e| anyhow!("servidor de chamadas recusou a conexão: {}", redact(&e.to_string())))?;
    let (mut tx, mut rx): (WsTx, WsRx) = ws.split();
    // espera o JoinResponse
    let join = tokio::time::timeout(Duration::from_secs(10), async {
        loop {
            match rx.next().await {
                Some(Ok(Message::Binary(b))) => {
                    if let Ok(lk::SignalResponse { message: Some(m) }) = lk::SignalResponse::decode(b.as_slice()) {
                        match m { rs::Message::Join(j) => return Ok(j), rs::Message::Leave(_) => return Err(anyhow!("servidor encerrou a entrada")), _ => {} }
                    }
                }
                Some(Ok(_)) => {}
                Some(Err(e)) => return Err(anyhow!("WebSocket: {}", redact(&e.to_string()))),
                None => return Err(anyhow!("servidor fechou a conexão")),
            }
        }
    }).await.map_err(|_| anyhow!("servidor de chamadas não respondeu"))??;
    let me = join.participant.clone().unwrap_or_default();
    let ice: Vec<RTCIceServer> = join.ice_servers.iter().map(|s| RTCIceServer { urls: s.urls.clone(), username: s.username.clone(), credential: s.credential.clone(), ..Default::default() }).collect();
    let sub = build_pc(&ice, false).await?;
    let (itx, irx) = mpsc::channel::<Internal>(512);
    wire_pc(&sub, 1, &itx);
    let shared = Arc::new(Mutex::new(Shared::default()));
    let stats = Arc::new(SfuStats::default());
    let (etx, erx) = mpsc::channel::<MeshEvent>(256);
    let (ctx, crx) = mpsc::channel::<MeshCmd>(64);
    let (sev_tx, sev_rx) = mpsc::channel::<Value>(256);
    let (audio, audio_rx) = match j.audio { Some((a, r)) => (Some(a), Some(r)), None => (None, None) };
    // faixas recebidas
    {
        let (audio, shared, etx, wk, sev, stats) = (audio.clone(), shared.clone(), etx.clone(), Arc::downgrade(&sub), sev_tx.clone(), stats.clone());
        sub.on_track(Box::new(move |track, rtp_rx, _tr| {
            let (audio, shared, etx, wk, sev, stats) = (audio.clone(), shared.clone(), etx.clone(), wk.clone(), sev.clone(), stats.clone());
            Box::pin(async move {
                tokio::spawn(async move { while rtp_rx.read_rtcp().await.is_ok() {} });
                stats.tracks_in.fetch_add(1, Ordering::Relaxed);
                // o id da faixa é o sid; os metadados chegam por ParticipantUpdate (normalmente antes da offer)
                let sid = track.id();
                let mut meta = None;
                for _ in 0..30 { meta = shared.lock().unwrap().tracks.get(&sid).cloned(); if meta.is_some() { break; } tokio::time::sleep(Duration::from_millis(100)).await; }
                let Some(meta) = meta else { let _ = etx.send(MeshEvent::Error(format!("faixa {sid} sem dono"))).await; return; };
                if track.kind() == RTPCodecType::Video {
                    let recv = shared.lock().unwrap().recv.entry(meta.identity.clone()).or_default().clone();
                    let ssrc = track.ssrc();
                    let pli: Arc<dyn Fn() + Send + Sync> = Arc::new(move || {
                        if let Some(pc) = wk.upgrade() { tokio::spawn(async move { let _ = pc.write_rtcp(&[Box::new(PictureLossIndication { sender_ssrc: 0, media_ssrc: ssrc })]).await; }); }
                    });
                    tokio::spawn(share::run_remote_video(meta.identity.clone(), track, recv, sev, pli));
                } else if let Some(a) = audio {
                    let key = if meta.source == lk::TrackSource::ScreenShareAudio as i32 { format!("{}#screen", meta.identity) } else { meta.identity.clone() };
                    tokio::spawn(a.run_remote(key, track));
                }
            })
        }));
    }
    sub.on_data_channel(Box::new(|_dc| Box::pin(async {})));   // as duas data channels do servidor (_reliable/_lossy): ainda sem uso
    let server = target.host.clone();
    let identity = me.identity.clone();
    let mut actor = Actor {
        tx, ice, sub, publisher: None, itx, shared, stats: stats.clone(), ev: etx, audio, sev_tx, me: me.clone(),
        pending: HashMap::new(), published: HashMap::new(), subscribed: HashSet::new(), known: HashMap::new(), pub_offering: false, pub_dirty: false,
        pend_ice: [vec![], vec![]], share: None, share_gen: 0, net: Default::default(), idr: Arc::new(AtomicBool::new(true)), mic_sid: None, last_mic_open: true,
        ping_ms: if join.ping_interval > 0 { join.ping_interval as u64 * 1000 } else { 10_000 }, subscriber_primary: join.subscriber_primary,
    };
    actor.handle_join(join).await;
    tokio::spawn(async move { actor.run(rx, crx, irx, sev_rx, audio_rx).await; });
    Ok(SfuHandle { cmd: ctx, events: erx, stats, identity, server })
}

/// Pede o token de acesso à sala à função `livekit-token` do Supabase, autenticado com a sessão do usuário (Bearer). Bloqueante:
/// chame de `spawn_blocking`. O segredo do LiveKit fica só no servidor; aqui só passa o token curto (1 h) da sala.
/// A identidade do token é o usuário (a função não aceita identidade livre).
pub fn fetch_token(token_url: &str, access_token: &str, room: &str) -> Result<String, String> {
    crate::sfu_cfg::check_token_url(token_url)?;
    let resp = ureq::AgentBuilder::new().timeout(Duration::from_secs(10)).build()
        .post(token_url).set("authorization", &format!("Bearer {access_token}")).set("content-type", "application/json")
        .send_json(json!({ "room": room }));
    match resp {
        Ok(r) => {
            let v: Value = r.into_json().map_err(|e| format!("resposta do serviço de tokens inválida: {e}"))?;
            v["token"].as_str().filter(|t| !t.is_empty() && t.len() < 4096).map(String::from).ok_or_else(|| "serviço de tokens não devolveu token".to_string())
        }
        Err(ureq::Error::Status(code, r)) => {
            let msg = r.into_json::<Value>().ok().and_then(|v| v["message"].as_str().map(String::from)).unwrap_or_default();
            Err(format!("serviço de tokens recusou ({code}){}{}", if msg.is_empty() { "" } else { ": " }, msg.chars().take(160).collect::<String>()))
        }
        Err(e) => Err(format!("serviço de tokens inacessível: {}", e.to_string().chars().take(160).collect::<String>())),
    }
}

/// A URL de conexão carrega o token: tira da mensagem de erro qualquer trecho `access_token=…`.
fn redact(s: &str) -> String {
    match s.find("access_token=") { Some(i) => { let end = s[i..].find(|c: char| c == '&' || c.is_whitespace()).map(|e| i + e).unwrap_or(s.len()); format!("{}access_token=***{}", &s[..i], &s[end..]) } None => s.to_string() }
}

struct ActiveShare { gen: u64, sender: share::Sender, audio: Option<crate::procaudio::ProcAudio>, vout: share::VideoOut, info: Option<Value> }

struct Actor {
    tx: WsTx,
    ice: Vec<RTCIceServer>,
    sub: Arc<RTCPeerConnection>,
    publisher: Option<Arc<RTCPeerConnection>>,
    itx: mpsc::Sender<Internal>,
    shared: Arc<Mutex<Shared>>,
    stats: Arc<SfuStats>,
    ev: mpsc::Sender<MeshEvent>,
    audio: Option<Arc<Audio>>,
    sev_tx: mpsc::Sender<Value>,
    me: lk::ParticipantInfo,
    pending: HashMap<String, Pending>,
    published: HashMap<String, Published>,   // cid -> publicação
    subscribed: HashSet<String>,
    known: HashMap<String, KnownPeer>,       // identity -> estado já anunciado
    pub_offering: bool,
    pub_dirty: bool,
    pend_ice: [Vec<RTCIceCandidateInit>; 2],
    share: Option<ActiveShare>,
    share_gen: u64,
    net: Arc<Mutex<crate::adapt::NetShared>>,
    idr: Arc<AtomicBool>,
    mic_sid: Option<String>,
    last_mic_open: bool,
    ping_ms: u64,
    subscriber_primary: bool,
}

#[derive(Default, Clone)]
struct KnownPeer { sid: String, mic: Option<bool>, share: Option<(String, u32, u32)>, name: String }

impl Actor {
    async fn emit(&self, e: MeshEvent) { let _ = self.ev.send(e).await; }
    async fn send(&mut self, m: rq::Message) {
        if std::env::var("BRIKS_SFU_DEBUG").is_ok() { let d = format!("{m:?}"); eprintln!("[sfu {}] -> {}", &self.me.identity[..4.min(self.me.identity.len())], &d[..d.len().min(1500)]); }
        let _ = self.tx.send(encode_req(m)).await; }

    async fn handle_join(&mut self, j: lk::JoinResponse) {
        self.emit(MeshEvent::Share(json!({ "ev": "net.sfu", "state": "joined", "server_version": j.server_version, "region": j.server_region, "room": j.room.as_ref().map(|r| r.name.clone()).unwrap_or_default() }))).await;
        self.on_participants(&j.other_participants).await;
        // áudio do microfone: publica agora (o publisher só conecta quando há o que enviar)
        if let Some(a) = self.audio.clone() {
            a.start();
            self.publish("audio", a.track.clone() as Arc<dyn TrackLocal + Send + Sync>, false, lk::TrackType::Audio, lk::TrackSource::Microphone, "microfone", 0, 0).await;
        }
    }

    async fn publish(&mut self, cid: &str, track: Arc<dyn TrackLocal + Send + Sync>, video: bool, ttype: lk::TrackType, source: lk::TrackSource, name: &str, w: u32, h: u32) {
        self.pending.insert(cid.to_string(), Pending { track, video });
        let req = lk::AddTrackRequest { cid: cid.into(), name: name.into(), r#type: ttype as i32, width: w, height: h, source: source as i32, simulcast_codecs: if ttype == lk::TrackType::Video { vec![lk::SimulcastCodec { codec: "h264".into(), cid: cid.into(), ..Default::default() }] } else { vec![] }, stereo: ttype == lk::TrackType::Audio && source == lk::TrackSource::ScreenShareAudio, disable_dtx: false, ..Default::default() };
        self.send(rq::Message::AddTrack(req)).await;
    }

    async fn ensure_publisher(&mut self) -> Result<Arc<RTCPeerConnection>> {
        if let Some(p) = &self.publisher { return Ok(p.clone()); }
        let pc = build_pc(&self.ice, true).await?;
        wire_pc(&pc, 0, &self.itx);
        // o servidor espera as duas data channels na offer do publisher
        pc.create_data_channel("_reliable", Some(RTCDataChannelInit { ordered: Some(true), ..Default::default() })).await?;
        pc.create_data_channel("_lossy", Some(RTCDataChannelInit { ordered: Some(false), max_retransmits: Some(0), ..Default::default() })).await?;
        self.publisher = Some(pc.clone());
        Ok(pc)
    }

    async fn negotiate_publisher(&mut self) {
        if self.pub_offering { self.pub_dirty = true; return; }
        let Some(pc) = self.publisher.clone() else { return };
        let r: Result<()> = async {
            let offer = pc.create_offer(None).await?;
            pc.set_local_description(offer.clone()).await?;
            let mut mid_map = HashMap::new();
            for t in pc.get_transceivers().await {
                if let (Some(mid), Some(tr)) = (t.mid(), t.sender().await.track().await) { mid_map.insert(mid.to_string(), tr.id().to_string()); }
            }
            self.pub_offering = true;
            self.send(rq::Message::Offer(lk::SessionDescription { r#type: "offer".into(), sdp: offer.sdp, id: 0, mid_to_track_id: mid_map })).await;
            Ok(())
        }.await;
        if let Err(e) = r { self.emit(MeshEvent::Error(format!("negociação de envio: {e}"))).await; }
    }

    async fn on_track_published(&mut self, r: lk::TrackPublishedResponse) {
        let Some(p) = self.pending.remove(&r.cid) else { return };
        let sid = r.track.as_ref().map(|t| t.sid.clone()).unwrap_or_default();
        let pc = match self.ensure_publisher().await { Ok(p) => p, Err(e) => { self.emit(MeshEvent::Error(format!("publisher: {e}"))).await; return; } };
        match pc.add_track(p.track.clone()).await {
            Ok(sender) => {
                if p.video {
                    self.idr.store(true, Ordering::Relaxed);
                    let nack = self.share.as_ref().map(|s| s.vout.nack_tx.clone());
                    spawn_rtcp_reader(sender.clone(), self.net.clone(), self.idr.clone(), nack);
                } else if r.cid == "audio" {
                    self.mic_sid = Some(sid.clone());
                    // o estado de mudo pode ter mudado antes da publicação sair
                    if let Some(a) = &self.audio { if !a.mic_open() { let s = sid.clone(); self.send(rq::Message::Mute(lk::MuteTrackRequest { sid: s, muted: true })).await; self.last_mic_open = false; } }
                }
                self.stats.tracks_out.fetch_add(1, Ordering::Relaxed);
                self.published.insert(r.cid.clone(), Published { sid, sender });
                self.negotiate_publisher().await;
            }
            Err(e) => self.emit(MeshEvent::Error(format!("faixa {}: {e}", r.cid))).await,
        }
    }

    async fn add_ice(&mut self, target: i32, init: RTCIceCandidateInit) {
        let pc = if target == 1 { Some(self.sub.clone()) } else { self.publisher.clone() };
        let Some(pc) = pc else { return };
        if pc.remote_description().await.is_some() { let _ = pc.add_ice_candidate(init).await; } else { self.pend_ice[(target == 1) as usize].push(init); }
    }
    async fn flush_ice(&mut self, target: i32) {
        let pc = if target == 1 { Some(self.sub.clone()) } else { self.publisher.clone() };
        let Some(pc) = pc else { return };
        for i in std::mem::take(&mut self.pend_ice[(target == 1) as usize]) { let _ = pc.add_ice_candidate(i).await; }
    }

    async fn on_signal(&mut self, m: rs::Message) {
        if std::env::var("BRIKS_SFU_DEBUG").is_ok() {
            let d = format!("{m:?}"); eprintln!("[sfu {}] <- {}", &self.me.identity[..4.min(self.me.identity.len())], &d[..d.len().min(1500)]);
        }
        match m {
            rs::Message::Offer(o) => {
                // subscriber: o servidor oferece, nós respondemos
                let r: Result<()> = async {
                    self.sub.set_remote_description(RTCSessionDescription::offer(o.sdp)?).await?;
                    self.flush_ice(1).await;
                    let ans = self.sub.create_answer(None).await?;
                    self.sub.set_local_description(ans.clone()).await?;
                    self.send(rq::Message::Answer(lk::SessionDescription { r#type: "answer".into(), sdp: ans.sdp, id: o.id, mid_to_track_id: Default::default() })).await;
                    Ok(())
                }.await;
                if let Err(e) = r { self.emit(MeshEvent::Error(format!("resposta ao SFU: {e}"))).await; }
            }
            rs::Message::Answer(a) => {
                if let Some(pc) = self.publisher.clone() {
                    match RTCSessionDescription::answer(a.sdp) { Ok(d) => { if let Err(e) = pc.set_remote_description(d).await { self.emit(MeshEvent::Error(format!("answer do SFU: {e}"))).await; } } Err(e) => self.emit(MeshEvent::Error(format!("answer inválida: {e}"))).await }
                    self.flush_ice(0).await;
                }
                self.pub_offering = false;
                if std::mem::take(&mut self.pub_dirty) { self.negotiate_publisher().await; }
            }
            rs::Message::Trickle(t) => {
                if let Ok(init) = serde_json::from_str::<RTCIceCandidateInit>(&t.candidate_init) { self.add_ice(t.target, init).await; }
            }
            rs::Message::Update(u) => self.on_participants(&u.participants).await,
            rs::Message::TrackPublished(p) => self.on_track_published(p).await,
            rs::Message::SpeakersChanged(s) => {
                let evs: Vec<(String, f32)> = {
                    let sh = self.shared.lock().unwrap();
                    s.speakers.iter().filter_map(|k| sh.part_ident.get(&k.sid).map(|id| (id.clone(), if k.active { k.level.clamp(0.05, 1.0) } else { 0.0 }))).collect()
                };
                for (peer_id, level) in evs { self.emit(MeshEvent::Speaking { peer_id, level }).await; }
            }
            rs::Message::Leave(l) => { self.emit(MeshEvent::Error(format!("o servidor encerrou a sessão (motivo {})", l.reason))).await; self.emit(MeshEvent::Kicked { reason: "sessão encerrada pelo servidor".into() }).await; }
            rs::Message::Mute(m) => { let _ = m; }   // mudo pedido pelo servidor (moderação): ainda sem política de UI
            _ => {}
        }
    }

    /// Diferença entre o que o servidor diz e o que já anunciamos à UI; também mantém as assinaturas das faixas.
    async fn on_participants(&mut self, ps: &[lk::ParticipantInfo]) {
        let mut want_sub: Vec<(String, String)> = vec![];
        for p in ps {
            if p.identity == self.me.identity || p.sid == self.me.sid { continue; }
            let gone = p.state == lk::participant_info::State::Disconnected as i32;
            if gone {
                self.known.remove(&p.identity);
                { let mut sh = self.shared.lock().unwrap(); sh.part_ident.remove(&p.sid); sh.tracks.retain(|_, t| t.identity != p.identity); sh.recv.remove(&p.identity); }
                if let Some(a) = &self.audio { a.remove_peer(&p.identity); }
                self.emit(MeshEvent::Peer { peer_id: p.identity.clone(), state: "fechado".into() }).await;
                continue;
            }
            let first = !self.known.contains_key(&p.identity);
            let mut k = self.known.get(&p.identity).cloned().unwrap_or_default();
            {
                let mut sh = self.shared.lock().unwrap();
                sh.part_ident.insert(p.sid.clone(), p.identity.clone());
                sh.recv.entry(p.identity.clone()).or_default();
                for t in &p.tracks { sh.tracks.insert(t.sid.clone(), TrackMeta { identity: p.identity.clone(), source: t.source, ttype: t.r#type, name: t.name.clone(), w: t.width, h: t.height, muted: t.muted }); }
            }
            k.sid = p.sid.clone();
            if first {
                let name = if p.name.is_empty() { p.identity.clone() } else { p.name.clone() };
                k.name = name.clone();
                self.emit(MeshEvent::Peer { peer_id: p.identity.clone(), state: "autenticado".into() }).await;
                self.emit(MeshEvent::PeerInfo { peer_id: p.identity.clone(), fp: p.identity.clone(), name }).await;
            }
            // microfone
            let mic = p.tracks.iter().find(|t| t.source == lk::TrackSource::Microphone as i32).map(|t| t.muted);
            if mic != k.mic {
                k.mic = mic;
                self.emit(MeshEvent::PeerState { peer_id: p.identity.clone(), microphone: mic == Some(false), deafened: false, status: String::new(), presence: String::new() }).await;
            }
            // áudio de voz: assina sempre
            for t in p.tracks.iter().filter(|t| t.source == lk::TrackSource::Microphone as i32) {
                if self.subscribed.insert(t.sid.clone()) { want_sub.push((p.sid.clone(), t.sid.clone())); }
            }
            // tela
            let scr = p.tracks.iter().find(|t| t.source == lk::TrackSource::ScreenShare as i32);
            let has_audio = p.tracks.iter().any(|t| t.source == lk::TrackSource::ScreenShareAudio as i32);
            let now = scr.map(|t| (t.name.clone(), t.width, t.height));
            if now != k.share {
                k.share = now.clone();
                if now.is_none() { let r = self.shared.lock().unwrap().recv.get(&p.identity).cloned(); if let Some(r) = r { r.watching.store(false, Ordering::Relaxed); } }
                let ev = match &now {
                    Some((label, w, h)) => json!({ "ev": "share.remote", "peer_id": p.identity, "state": "on", "w": w, "h": h, "fps": 0, "kbps": 0, "label": label, "audio": has_audio }),
                    None => json!({ "ev": "share.remote", "peer_id": p.identity, "state": "off" }),
                };
                self.emit(MeshEvent::Share(ev)).await;
            }
            self.known.insert(p.identity.clone(), k);
        }
        self.subscribe(want_sub, true).await;
    }

    async fn subscribe(&mut self, items: Vec<(String, String)>, on: bool) {
        if items.is_empty() { return; }
        let mut by: HashMap<String, Vec<String>> = HashMap::new();
        for (p, t) in &items { by.entry(p.clone()).or_default().push(t.clone()); }
        let req = lk::UpdateSubscription {
            track_sids: items.iter().map(|x| x.1.clone()).collect(),
            subscribe: on,
            participant_tracks: by.into_iter().map(|(participant_sid, track_sids)| lk::ParticipantTracks { participant_sid, track_sids }).collect(),
        };
        self.send(rq::Message::Subscription(req)).await;
    }

    async fn run(&mut self, mut rx: WsRx, mut cmds: mpsc::Receiver<MeshCmd>, mut irx: mpsc::Receiver<Internal>, mut srx: mpsc::Receiver<Value>, mut arx: Option<mpsc::Receiver<AudioEvent>>) {
        let mut ping = tokio::time::interval(Duration::from_millis(self.ping_ms));
        loop {
            tokio::select! {
                m = rx.next() => match m {
                    Some(Ok(Message::Binary(b))) => {
                        self.stats.ws_msgs.fetch_add(1, Ordering::Relaxed);
                        if let Ok(lk::SignalResponse { message: Some(m) }) = lk::SignalResponse::decode(b.as_slice()) { self.on_signal(m).await; }
                    }
                    Some(Ok(Message::Close(_))) | None => { self.emit(MeshEvent::Error("conexão com o servidor de chamadas caiu".into())).await; self.emit(MeshEvent::Kicked { reason: "conexão com o servidor de chamadas caiu".into() }).await; break; }
                    Some(Ok(_)) => {}
                    Some(Err(e)) => { self.emit(MeshEvent::Error(format!("WebSocket: {}", redact(&e.to_string())))).await; break; }
                },
                c = cmds.recv() => match c { Some(MeshCmd::Close) | None => break, Some(c) => self.on_cmd(c).await },
                i = irx.recv() => if let Some(i) = i { self.on_internal(i).await },
                v = srx.recv() => if let Some(v) = v { self.on_share_event(v).await },
                ae = async { match arx.as_mut() { Some(r) => r.recv().await, None => std::future::pending().await } } => match ae {
                    Some(AudioEvent::Speaking { peer, level }) => self.emit(MeshEvent::Speaking { peer_id: peer, level }).await,
                    Some(AudioEvent::Error(m)) => self.emit(MeshEvent::Error(format!("áudio: {m}"))).await,
                    None => { arx = None; }
                },
                _ = ping.tick() => { let t = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() as i64).unwrap_or(0); self.send(rq::Message::Ping(t)).await; }
            }
        }
        // saída limpa: avisa o servidor, para captura/áudio e fecha os PeerConnections
        self.send(rq::Message::Leave(lk::LeaveRequest { can_reconnect: false, reason: lk::DisconnectReason::ClientInitiated as i32, action: lk::leave_request::Action::Disconnect as i32, ..Default::default() })).await;
        let _ = self.tx.close().await;
        if let Some(a) = &self.audio { a.stop(); }
        if let Some(sh) = self.share.take() { sh.sender.stop(); if let Some(a) = &sh.audio { a.stop(); } }
        if let Some(p) = self.publisher.take() { let _ = p.close().await; }
        let _ = self.sub.close().await;
    }

    async fn on_internal(&mut self, i: Internal) {
        match i {
            Internal::Ice { target, init } => {
                if let Ok(s) = serde_json::to_string(&init) { self.send(rq::Message::Trickle(lk::TrickleRequest { candidate_init: s, target, r#final: false })).await; }
            }
            Internal::Pc { target, state } => {
                let up = state == RTCPeerConnectionState::Connected;
                if target == 1 { self.stats.sub_connected.store(up, Ordering::Relaxed); } else { self.stats.pub_connected.store(up, Ordering::Relaxed); }
                self.emit(MeshEvent::Share(json!({ "ev": "net.sfu", "state": format!("{}:{}", if target == 1 { "sub" } else { "pub" }, state) }))).await;
                if state == RTCPeerConnectionState::Failed { self.emit(MeshEvent::Error("conexão de mídia com o SFU falhou".into())).await; }
            }
        }
    }

    async fn on_cmd(&mut self, c: MeshCmd) {
        match c {
            MeshCmd::BroadcastState => {
                if let (Some(a), Some(sid)) = (self.audio.clone(), self.mic_sid.clone()) {
                    let open = a.mic_open();
                    if open != self.last_mic_open { self.last_mic_open = open; self.send(rq::Message::Mute(lk::MuteTrackRequest { sid, muted: !open })).await; }
                }
            }
            MeshCmd::SetProfile { .. } => {}   // status/presença ainda não viajam pelo SFU
            MeshCmd::MutePeer { key, muted } => { if let Some(a) = &self.audio { a.mute_peer(&key, muted); } }
            MeshCmd::ShareVolume { key, gain } => { if let Some(a) = &self.audio { a.set_screen_gain(&key, gain); } }
            MeshCmd::ShareStart { cfg, preview, dump } => self.share_start(cfg, preview, dump).await,
            MeshCmd::ShareStop => self.share_stop().await,
            MeshCmd::ShareAudio { mode } => { if let Some(a) = self.share.as_ref().and_then(|s| s.audio.as_ref()) { a.set_mode(mode); } }
            MeshCmd::ShareWatch { key, on, path } => self.watch(&key, on, path).await,
            MeshCmd::Chat { .. } | MeshCmd::Event { .. } => self.emit(MeshEvent::Error("chat não disponível no modo SFU (use uma sala P2P)".into())).await,
            MeshCmd::Kick { .. } | MeshCmd::Allow { .. } => self.emit(MeshEvent::Error("expulsar exige a API de administração do LiveKit: ainda não disponível".into())).await,
            MeshCmd::ForceOffer { .. } => {}
            #[cfg(test)]
            MeshCmd::KickUnchecked { .. } | MeshCmd::RawDc { .. } => {}
            MeshCmd::Close => {}
        }
    }

    async fn watch(&mut self, key: &str, on: bool, path: std::path::PathBuf) {
        let (recv, sids): (Option<Arc<RecvState>>, Vec<(String, String)>) = {
            let sh = self.shared.lock().unwrap();
            let sids = sh.tracks.iter().filter(|(_, t)| t.identity == key && (t.source == lk::TrackSource::ScreenShare as i32 || t.source == lk::TrackSource::ScreenShareAudio as i32))
                .map(|(sid, _)| (self.known.get(key).map(|k| k.sid.clone()).unwrap_or_default(), sid.clone())).collect();
            (sh.recv.get(key).cloned(), sids)
        };
        let Some(r) = recv else { self.emit(MeshEvent::Error("assistir: peer desconhecido".into())).await; return };
        if on { *r.sink_path.lock().unwrap() = Some(path.clone()); r.epoch.fetch_add(1, Ordering::Relaxed); r.frames.store(0, Ordering::Relaxed); }
        r.watching.store(on, Ordering::Relaxed);
        // só baixa o vídeo (e o áudio da transmissão) de quem está sendo assistido
        let todo: Vec<(String, String)> = sids.into_iter().filter(|(_, s)| if on { self.subscribed.insert(s.clone()) } else { self.subscribed.remove(s) }).collect();
        self.subscribe(todo, on).await;
        self.emit(MeshEvent::Share(json!({ "ev": "share.remote", "peer_id": key, "state": if on { "watch" } else { "unwatch" }, "path": path.to_string_lossy() }))).await;
    }

    async fn share_start(&mut self, cfg: ShareCfg, preview: Option<std::path::PathBuf>, dump: Option<std::path::PathBuf>) {
        self.share_stop().await;
        self.share_gen += 1;
        let gen = self.share_gen;
        let vout = share::VideoOut::new();
        let (amode, label, h) = (cfg.audio.clone(), cfg.label.clone(), cfg.height as u32);
        match share::start_sender(gen, self.idr.clone(), self.net.clone(), cfg, vout.clone(), self.sev_tx.clone(), preview, dump) {
            Ok(sender) => {
                let has_audio = amode != crate::procaudio::AudioMode::None;
                let atrack = crate::procaudio::new_audio_track();
                let audio = if has_audio { Some(crate::procaudio::start(amode, atrack.clone())) } else { None };
                let track = vout.track.clone();
                self.share = Some(ActiveShare { gen, sender, audio, vout, info: None });
                self.publish(share::VIDEO_TRACK_ID, track as Arc<dyn TrackLocal + Send + Sync>, true, lk::TrackType::Video, lk::TrackSource::ScreenShare, if label.is_empty() { "tela" } else { &label }, h * 16 / 9, h).await;
                if has_audio { self.publish(share::AUDIO_TRACK_ID, atrack as Arc<dyn TrackLocal + Send + Sync>, false, lk::TrackType::Audio, lk::TrackSource::ScreenShareAudio, "audio da tela", 0, 0).await; }
            }
            Err(e) => self.emit(MeshEvent::Share(json!({ "ev": "share.local", "state": "off", "reason": e }))).await,
        }
    }

    async fn share_stop(&mut self) {
        let Some(sh) = self.share.take() else { return };
        sh.sender.stop();
        if let Some(a) = &sh.audio { a.stop(); }
        self.pending.remove(share::VIDEO_TRACK_ID);
        self.pending.remove(share::AUDIO_TRACK_ID);
        let mut changed = false;
        if let Some(pc) = self.publisher.clone() {
            for cid in [share::VIDEO_TRACK_ID, share::AUDIO_TRACK_ID] {
                if let Some(p) = self.published.remove(cid) { let _ = pc.remove_track(&p.sender).await; changed = true; }
            }
        }
        if changed { self.negotiate_publisher().await; }
    }

    async fn on_share_event(&mut self, mut v: Value) {
        if let Some(g) = v["gen"].as_u64() { if self.share.as_ref().map(|s| s.gen) != Some(g) && !(v["ev"] == "share.local" && v["state"] == "off" && self.share.is_none()) { return; } }
        if v["ev"] == "share.local" {
            if v["state"] == "on" {
                if let Some(sh) = self.share.as_mut() { v["audio"] = json!(sh.audio.is_some()); sh.info = Some(v.clone()); }
            } else { self.share_stop().await; }
        }
        self.emit(MeshEvent::Share(v)).await;
    }
}

/// Lê o RTCP que o SFU manda ao publisher de vídeo: PLI/FIR (quadro-chave), NACK (retransmissão pelo pacer) e Receiver Reports
/// (perda/jitter/RTT para o adapt.rs). É a mesma leitura da malha, com a chave "sfu" no lugar do peer.
fn spawn_rtcp_reader(sender: Arc<RTCRtpSender>, net: Arc<Mutex<crate::adapt::NetShared>>, idr: Arc<AtomicBool>, nack_tx: Option<mpsc::Sender<Vec<u16>>>) {
    tokio::spawn(async move {
        use webrtc::rtcp::receiver_report::ReceiverReport;
        use webrtc::rtcp::transport_feedbacks::transport_layer_nack::TransportLayerNack;
        let ssrc = sender.get_parameters().await.encodings.first().map(|e| e.ssrc).unwrap_or(0);
        while let Ok((pkts, _)) = sender.read_rtcp().await {
            for pk in pkts {
                let any = pk.as_any();
                let mut n = net.lock().unwrap();
                let e = n.peers.entry("sfu".into()).or_insert(crate::adapt::PeerNet { loss: 0.0, rtt_ms: 0.0, jitter_ms: 0.0, updated: std::time::Instant::now(), nacks: 0, plis: 0, rtt_min: 0.0 });
                if any.downcast_ref::<PictureLossIndication>().is_some() || any.downcast_ref::<FullIntraRequest>().is_some() {
                    e.plis += 1; idr.store(true, Ordering::Relaxed);
                } else if let Some(rr) = any.downcast_ref::<ReceiverReport>() {
                    for r in rr.reports.iter().filter(|r| r.ssrc == ssrc) {
                        e.loss = r.fraction_lost as f32 / 256.0;
                        e.jitter_ms = r.jitter as f32 / 90.0;
                        if r.last_sender_report != 0 {
                            let now = (webrtc::rtp::extension::abs_send_time_extension::unix2ntp(std::time::SystemTime::now()) >> 16) as u32;
                            let d = now.wrapping_sub(r.last_sender_report).wrapping_sub(r.delay);
                            if d < 10 * 65536 { e.rtt_ms = d as f32 * 1000.0 / 65536.0; if e.rtt_min == 0.0 || e.rtt_ms < e.rtt_min { e.rtt_min = e.rtt_ms.max(1.0); } }
                        }
                        e.updated = std::time::Instant::now();
                    }
                } else if let Some(nk) = any.downcast_ref::<TransportLayerNack>() {
                    if nk.media_ssrc == ssrc {
                        let mut seqs = vec![];
                        for p in &nk.nacks { seqs.push(p.packet_id); for b in 0..16 { if p.lost_packets & (1 << b) != 0 { seqs.push(p.packet_id.wrapping_add(b + 1)); } } }
                        seqs.truncate(512);
                        e.nacks += seqs.len() as u64;
                        if let Some(t) = &nack_tx { let _ = t.try_send(seqs); }
                    }
                }
            }
        }
        net.lock().unwrap().peers.remove("sfu");
    });
}

#[cfg(test)]
mod t {
    use super::*;
    #[test]
    fn token_never_leaks_in_errors() {
        let s = redact("HTTP error: GET /rtc?access_token=eyJabc.def&protocol=15 failed");
        assert!(!s.contains("eyJabc"), "{s}");
        assert!(s.contains("access_token=***&protocol=15"));
        assert_eq!(redact("sem token"), "sem token");
    }
    #[test]
    fn url_building() {
        assert_eq!(ws_url("ws://127.0.0.1:7880/", "T"), "ws://127.0.0.1:7880/rtc?access_token=T&protocol=15&auto_subscribe=0&sdk=rust&version=briks-0.1&adaptive_stream=0");
        assert!(ws_url("https://x.livekit.cloud", "T").starts_with("wss://x.livekit.cloud/rtc?"));
    }
}
