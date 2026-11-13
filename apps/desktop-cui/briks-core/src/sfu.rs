//! Cliente LiveKit (SFU) do briks-core: protocolo de sinalização do LiveKit (WebSocket + protobuf, mensagens do crate `livekit-protocol`)
//! sobre os PeerConnections do webrtc-rs que o resto do core já usa. Nada de libwebrtc: os mesmos quadros H264 já codificados
//! pelo nosso encoder (MFT/NVENC com fallback), o mesmo Opus e o mesmo adapt.rs/NACK/PLI vão para o SFU. Decisão em docs/plans/sfu-livekit-nativo.md.
//!
//! Dois PeerConnections, como o LiveKit exige: o SUBSCRIBER recebe (o servidor manda a offer, nós respondemos) e o PUBLISHER envia
//! (pedimos `AddTrack`, o servidor responde `TrackPublished`, só então ligamos a faixa e mandamos a offer).
//! Eventos e comandos usam os mesmos tipos da malha (`MeshEvent`/`MeshCmd`), então a UI e o IPC não mudam por causa do modo.
use std::collections::{HashMap, HashSet};
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU64, Ordering};
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
    /// Camada de dados (chat/status/presença/expulsão pelas data channels do LiveKit). None = só voz e tela (ou malha P2P ao lado).
    pub data: Option<crate::sfudata::DataCfg>,
    /// Busca um token novo para a reconexão (o da entrada vale ~1 h). None = reconecta com o mesmo token.
    pub renew: Option<TokenFn>,
}

/// Função que devolve um token de sala novo (no app: a função `livekit-token` do Supabase com a sessão da conta).
pub type TokenFn = Arc<dyn Fn() -> std::pin::Pin<Box<dyn std::future::Future<Output = Result<String, TokenFail>> + Send>> + Send + Sync>;

/// Falha ao pedir o token. `retry_after` = segundos que o serviço mandou esperar (429): ninguém deve pedir de novo antes disso.
#[derive(Debug, Clone)]
pub struct TokenFail { pub msg: String, pub retry_after: Option<u64> }
impl From<String> for TokenFail { fn from(msg: String) -> Self { TokenFail { msg, retry_after: None } } }

/// Intervalo mínimo entre dois pedidos de token durante uma mesma queda (o serviço limita a 30 por usuário a cada 10 min).
const TOKEN_MIN_GAP: Duration = Duration::from_secs(10);

/// Reconexão: 1ª tentativa quase imediata, depois espera crescente até 5 s (com ±20% de acaso para os clientes não baterem juntos
/// num servidor que acabou de voltar); desiste após 2 min fora do ar.
const RETRY_MS: [u64; 6] = [200, 500, 1000, 2000, 4000, 5000];
const GIVE_UP: Duration = Duration::from_secs(120);
/// Depois de voltar, quem ainda não reapareceu (ou ainda não republicou a tela) só é dado como saído após este prazo:
/// numa queda do servidor todos voltam em momentos diferentes e a lista não deve piscar.
const GRACE: Duration = Duration::from_secs(20);

fn retry_delay(attempt: u32) -> Duration {
    let base = RETRY_MS[(attempt as usize).min(RETRY_MS.len() - 1)];
    let r = (uuid::Uuid::new_v4().as_u128() % 41) as u64;   // 0..=40 -> 80%..120%
    Duration::from_millis(base * (80 + r) / 100)
}

#[derive(Default)]
pub struct SfuStats {
    pub sub_connected: AtomicBool,
    pub pub_connected: AtomicBool,
    pub tracks_in: AtomicU64,
    pub tracks_out: AtomicU64,
    pub ws_msgs: AtomicU64,
    /// feedback RTCP do SFU ao publisher de vídeo (alimenta o adapt.rs): Receiver Reports, NACKs, PLIs, último RTT (µs) e perda (‰)
    pub rr: AtomicU64,
    pub nacks: AtomicU64,
    pub plis: AtomicU64,
    pub rtt_us: AtomicU64,
    pub loss_permille: AtomicU64,
    /// reconexões concluídas e tokens renovados para reconectar
    pub reconnects: AtomicU64,
    pub token_renewals: AtomicU64,
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
    recv: HashMap<String, Arc<RecvState>>,  // identity -> estado de recepção do vídeo (transmissão de tela)
    recv_cam: HashMap<String, Arc<RecvState>>,   // identity -> estado de recepção do vídeo da câmera
}

/// `epoch` = geração da sessão (sobe a cada reconexão): o que vier de PeerConnections antigos é descartado.
enum Internal {
    Data(u32, Vec<u8>),
    DcOpen(u32),
    Ice { epoch: u32, target: i32, init: RTCIceCandidateInit },
    Pc { epoch: u32, target: i32, state: RTCPeerConnectionState },
}

struct Down { since: std::time::Instant, attempt: u32, next: tokio::time::Instant, token_fresh: bool, announced: bool, reason: String, token_at: Option<std::time::Instant> }

struct Pending { track: Arc<dyn TrackLocal + Send + Sync>, video: bool, codec: crate::codec::Codec }

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
        // VP9/VP8 (libvpx): só entram na offer quando este binário sabe codificar. Cada faixa de tela restringe a m-line ao SEU codec com
        // `set_codec_preferences` (ver `on_track_published`); sem isso a offer listaria todos e o servidor registraria a faixa com o errado.
        let fb2 = vec![RTCPFeedback { typ: "goog-remb".into(), parameter: "".into() }, RTCPFeedback { typ: "ccm".into(), parameter: "fir".into() }, RTCPFeedback { typ: "nack".into(), parameter: "".into() }, RTCPFeedback { typ: "nack".into(), parameter: "pli".into() }, RTCPFeedback { typ: "transport-cc".into(), parameter: "".into() }];
        if crate::codec::Codec::Vp9.available() { m.register_codec(RTCRtpCodecParameters { capability: RTCRtpCodecCapability { mime_type: "video/VP9".into(), clock_rate: 90_000, channels: 0, sdp_fmtp_line: share::video_fmtp(crate::codec::Codec::Vp9).into(), rtcp_feedback: fb2.clone() }, payload_type: 98, ..Default::default() }, RTPCodecType::Video)?; }
        if crate::codec::Codec::Vp8.available() { m.register_codec(RTCRtpCodecParameters { capability: RTCRtpCodecCapability { mime_type: "video/VP8".into(), clock_rate: 90_000, channels: 0, sdp_fmtp_line: "".into(), rtcp_feedback: fb2 }, payload_type: 96, ..Default::default() }, RTPCodecType::Video)?; }
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
        se.set_interface_filter(Box::new(|n: &str| crate::sfu_cfg::iface_allowed(n)));
    // O webrtc-rs 0.12 anuncia AEAD_AES_256_GCM mas o SRTP dele entra em pânico com chave de 256 bits (generic-array, key_derivation.rs);
    // o LiveKit/pion prefere esse perfil. Anunciando só os de 128 bits a negociação cai num perfil que funciona.
    se.set_srtp_protection_profiles(vec![SrtpProtectionProfile::Srtp_Aead_Aes_128_Gcm, SrtpProtectionProfile::Srtp_Aes128_Cm_Hmac_Sha1_80]);
    crate::netmode::apply(&mut se);
    let api = APIBuilder::new().with_media_engine(m).with_interceptor_registry(registry).with_setting_engine(se).build();
    Ok(Arc::new(api.new_peer_connection(RTCConfiguration { ice_servers: crate::netmode::ice_servers(ice), ..Default::default() }).await?))
}

fn wire_pc(pc: &Arc<RTCPeerConnection>, epoch: u32, target: i32, itx: &mpsc::Sender<Internal>) {
    let itx1 = itx.clone();
    pc.on_ice_candidate(Box::new(move |c| { let itx = itx1.clone(); Box::pin(async move {
        if let Some(c) = c { if let Ok(init) = c.to_json() { let _ = itx.send(Internal::Ice { epoch, target, init }).await; } } }) }));
    let itx2 = itx.clone();
    pc.on_peer_connection_state_change(Box::new(move |state| { let itx = itx2.clone(); Box::pin(async move { let _ = itx.send(Internal::Pc { epoch, target, state }).await; }) }));
}

/// Falha ao abrir a sessão. `auth` = o servidor respondeu e RECUSOU (token vencido/inválido): vale buscar outro token.
struct OpenErr { auth: bool, msg: String }
impl OpenErr { fn net(m: impl std::fmt::Display) -> Self { OpenErr { auth: false, msg: redact(&m.to_string()) } } }

/// Abre o WebSocket de sinalização e espera o `JoinResponse`. Usado na entrada e em cada tentativa de reconexão.
async fn open_session(url: &str, token: &str, secs: u64) -> Result<(WsTx, WsRx, lk::JoinResponse, String), OpenErr> {
    use tokio_tungstenite::tungstenite::Error as WsErr;
    let target = crate::realtime::parse_target(url).map_err(OpenErr::net)?;
    let io = tokio::time::timeout(Duration::from_secs(secs), crate::realtime::open_io(&target)).await.map_err(|_| OpenErr::net("tempo esgotado ao conectar ao servidor de chamadas"))?.map_err(OpenErr::net)?;
    let hostport = format!("{}:{}", target.host, target.port);
    let url = ws_url(&format!("{}://{}", if target.tls { "wss" } else { "ws" }, hostport), token);
    let (ws, _) = tokio::time::timeout(Duration::from_secs(secs), tokio_tungstenite::client_async(url, io)).await.map_err(|_| OpenErr::net("servidor de chamadas não respondeu"))?
        .map_err(|e| OpenErr { auth: matches!(&e, WsErr::Http(r) if r.status().as_u16() == 401 || r.status().as_u16() == 403), msg: format!("servidor de chamadas recusou a conexão: {}{}", redact(&e.to_string()), if matches!(&e, WsErr::Http(r) if r.status().as_u16() == 401 || r.status().as_u16() == 403) { clock_hint() } else { String::new() }) })?;
    let (tx, mut rx): (WsTx, WsRx) = ws.split();
    let join = tokio::time::timeout(Duration::from_secs(secs), async {
        loop {
            match rx.next().await {
                Some(Ok(Message::Binary(b))) => {
                    if let Ok(lk::SignalResponse { message: Some(m) }) = lk::SignalResponse::decode(b.as_slice()) {
                        match m { rs::Message::Join(j) => return Ok(j), rs::Message::Leave(_) => return Err(OpenErr { auth: true, msg: "servidor encerrou a entrada".into() }), _ => {} }
                    }
                }
                Some(Ok(_)) => {}
                Some(Err(e)) => return Err(OpenErr::net(format!("WebSocket: {e}"))),
                None => return Err(OpenErr::net("servidor fechou a conexão")),
            }
        }
    }).await.map_err(|_| OpenErr::net("servidor de chamadas não respondeu"))??;
    Ok((tx, rx, join, target.host.clone()))
}

fn ice_of(join: &lk::JoinResponse) -> Vec<RTCIceServer> {
    join.ice_servers.iter().map(|s| RTCIceServer { urls: s.urls.clone(), username: s.username.clone(), credential: s.credential.clone(), ..Default::default() }).collect()
}

/// PeerConnection de RECEPÇÃO (o servidor oferece): faixas recebidas vão para o áudio/vídeo de sempre; as data channels do servidor
/// (_reliable/_lossy) entregam cada DataPacket ao ator.
async fn make_sub(ice: &[RTCIceServer], epoch: u32, itx: &mpsc::Sender<Internal>, audio: Option<Arc<Audio>>, shared: Arc<Mutex<Shared>>, etx: mpsc::Sender<MeshEvent>, sev_tx: mpsc::Sender<Value>, stats: Arc<SfuStats>) -> Result<Arc<RTCPeerConnection>> {
    let sub = build_pc(ice, false).await?;
    wire_pc(&sub, epoch, 1, itx);
    {
        let (audio, shared, etx, wk, sev, stats) = (audio.clone(), shared.clone(), etx.clone(), Arc::downgrade(&sub), sev_tx.clone(), stats.clone());
        sub.on_track(Box::new(move |track, rtp_rx, _tr| {
            let (audio, shared, etx, wk, sev, stats) = (audio.clone(), shared.clone(), etx.clone(), wk.clone(), sev.clone(), stats.clone());
            Box::pin(async move {
                let pts = share::payload_codecs(&rtp_rx).await;
                tokio::spawn(async move { while rtp_rx.read_rtcp().await.is_ok() {} });
                stats.tracks_in.fetch_add(1, Ordering::Relaxed);
                // o id da faixa é o sid; os metadados chegam por ParticipantUpdate (normalmente antes da offer)
                let sid = track.id();
                let mut meta = None;
                for _ in 0..30 { meta = shared.lock().unwrap().tracks.get(&sid).cloned(); if meta.is_some() { break; } tokio::time::sleep(Duration::from_millis(100)).await; }
                let Some(meta) = meta else { let _ = etx.send(MeshEvent::Error(format!("faixa {sid} sem dono"))).await; return; };
                if track.kind() == RTPCodecType::Video {
                    let is_cam = meta.source == lk::TrackSource::Camera as i32;
                    let slot = if meta.source == lk::TrackSource::ScreenShare as i32 { share::slot_of_name(&meta.name) } else { 0 };   // 2ª tela da mesma pessoa
                    let rkey = share::recv_key(&meta.identity, slot);
                    let recv = { let mut sh = shared.lock().unwrap(); if is_cam { sh.recv_cam.entry(meta.identity.clone()).or_default().clone() } else { sh.recv.entry(rkey.clone()).or_default().clone() } };
                    let ssrc = track.ssrc();
                    let pli: Arc<dyn Fn() + Send + Sync> = Arc::new(move || {
                        if let Some(pc) = wk.upgrade() { tokio::spawn(async move { let _ = pc.write_rtcp(&[Box::new(PictureLossIndication { sender_ssrc: 0, media_ssrc: ssrc })]).await; }); }
                    });
                    // Faixa nova (1ª assinatura ou reassinatura depois de uma reconexão): o receptor pede um quadro-chave ao começar,
                    // mas esse 1º pedido pode chegar ao servidor antes de a faixa de quem transmite estar pronta e se perder; o
                    // seguinte só sairia depois de 2 s sem quadros. Repete o pedido algumas vezes, até o 1º quadro aparecer.
                    if !(cfg!(test) && std::env::var("BRIKS_SFU_TEST_REKEY_OLD").is_ok()) {
                        let (pli2, recv2) = (pli.clone(), recv.clone());
                        tokio::spawn(async move {
                            for ms in [250u64, 350, 500, 700] {
                                tokio::time::sleep(Duration::from_millis(ms)).await;
                                if recv2.frames.load(Ordering::Relaxed) > 0 { break; }
                                if recv2.watching.load(Ordering::Relaxed) { pli2(); }
                            }
                        });
                    }
                    if is_cam { tokio::spawn(share::run_remote_video_as("cam", meta.identity.clone(), track, recv, sev, pli, pts)); }
                    else { tokio::spawn(share::run_remote_video(rkey, track, recv, sev, pli, pts)); }
                } else if let Some(a) = audio {
                    let key = if meta.source == lk::TrackSource::ScreenShareAudio as i32 { format!("{}#screen", meta.identity) } else { meta.identity.clone() };
                    tokio::spawn(a.run_remote(key, track));
                }
            })
        }));
    }
    {
        let itx = itx.clone();
        sub.on_data_channel(Box::new(move |dc| {
            let itx = itx.clone();
            Box::pin(async move {
                dc.on_message(Box::new(move |msg| { let itx = itx.clone(); Box::pin(async move { let _ = itx.send(Internal::Data(epoch, msg.data.to_vec())).await; }) }));
            })
        }));
    }
    Ok(sub)
}

/// Conecta, entra na sala e só devolve quando o servidor respondeu `Join` (para o chamador poder cair no P2P se falhar).
/// Depois disso a sessão se mantém sozinha: se o WebSocket ou a mídia caírem, o ator reconecta (ver `Actor::try_rejoin`).
pub async fn connect(j: SfuJoin) -> Result<SfuHandle> {
    let (tx, rx, join, server) = open_session(&j.url, &j.token, 10).await.map_err(|e| anyhow!(e.msg))?;
    let me = join.participant.clone().unwrap_or_default();
    let ice = ice_of(&join);
    let (itx, irx) = mpsc::channel::<Internal>(512);
    let shared = Arc::new(Mutex::new(Shared::default()));
    let stats = Arc::new(SfuStats::default());
    let (etx, erx) = mpsc::channel::<MeshEvent>(256);
    let (ctx, crx) = mpsc::channel::<MeshCmd>(64);
    let (sev_tx, sev_rx) = mpsc::channel::<Value>(256);
    let (audio, audio_rx) = match j.audio { Some((a, r)) => (Some(a), Some(r)), None => (None, None) };
    let sub = make_sub(&ice, 0, &itx, audio.clone(), shared.clone(), etx.clone(), sev_tx.clone(), stats.clone()).await?;
    let identity = me.identity.clone();
    let (ftx, frx) = mpsc::channel::<crate::sfufiles::FileOut>(8);
    let files = j.data.as_ref().map(|c| crate::sfufiles::Files::new(c.identity.clone(), etx.clone(), ftx.clone()));
    let data = j.data.map(|c| crate::sfudata::Layer::new(c, me.identity.clone()));
    let mut actor = Actor {
        files, _ftx: ftx,
        data, dc_pub: None, dc_queue: vec![], leave: false,
        url: j.url.clone(), token: j.token, renew: j.renew, epoch: 0, down: None, stale: HashMap::new(), audio_started: false, last_rx: std::time::Instant::now(),
        ping_timeout: Duration::from_secs(if join.ping_timeout > 0 { (join.ping_timeout as u64).clamp(5, 60) } else { 15 }),
        owner_marks: HashMap::new(), republish_video: false,
        tx, ice, sub, publisher: None, itx, shared, stats: stats.clone(), ev: etx, audio, sev_tx, me: me.clone(),
        pending: HashMap::new(), published: HashMap::new(), subscribed: HashSet::new(), known: HashMap::new(), pub_offering: false, pub_dirty: false,
        pend_ice: [vec![], vec![]], share: [None, None], share_gens: [0, 0], cam: None, cam_gen: 0, cam_vout: None, cam_net: Default::default(), cam_idr: Arc::new(AtomicBool::new(true)), vout: [None, None], atrack: None, share_gen: 0, net: Default::default(), idr: [Arc::new(AtomicBool::new(true)), Arc::new(AtomicBool::new(true))], mic_sid: None, last_mic_open: true,
        ping_ms: if join.ping_interval > 0 { join.ping_interval as u64 * 1000 } else { 10_000 }, subscriber_primary: join.subscriber_primary,
    };
    remember_token(&actor.token);
    actor.handle_join(join, false).await;
    tokio::spawn(async move { actor.run(rx, crx, irx, sev_rx, audio_rx, frx).await; forget_token(&actor.token); });
    Ok(SfuHandle { cmd: ctx, events: erx, stats, identity, server })
}

/// Pede o token de acesso à sala à função `livekit-token` do Supabase, autenticado com a sessão do usuário (Bearer). Bloqueante:
/// chame de `spawn_blocking`. O segredo do LiveKit fica só no servidor; aqui só passa o token curto (1 h) da sala.
/// A identidade do token é o usuário (a função não aceita identidade livre).
pub fn fetch_token(token_url: &str, access_token: &str, room: &str) -> Result<String, String> {
    fetch_token_ex(token_url, access_token, room).map(|t| t.0).map_err(|e| e.msg)
}

/// Como `fetch_token`, devolvendo também a URL do servidor que o serviço informa (`url`, quando vem) e, numa recusa por excesso
/// de pedidos (429), quantos segundos esperar (`Retry-After`).
pub fn fetch_token_ex(token_url: &str, access_token: &str, room: &str) -> Result<(String, Option<String>), TokenFail> {
    fetch_token_inner(token_url, access_token, room)
}

/// Diferença (segundos) entre o relógio do servidor de tokens e o deste PC, lida do cabeçalho `Date` da última resposta. Um relógio fora
/// de hora faz a sessão expirada parecer válida e o servidor de chamadas recusar o token da sala (401): a mensagem de erro cita isto.
static CLOCK_SKEW_S: AtomicI64 = AtomicI64::new(0);
pub fn clock_skew_s() -> i64 { CLOCK_SKEW_S.load(Ordering::Relaxed) }
/// "Wed, 12 Nov 2026 21:30:42 GMT" -> segundos Unix (UTC).
pub(crate) fn parse_http_date(s: &str) -> Option<i64> {
    let p: Vec<&str> = s.split_whitespace().collect();
    if p.len() < 5 { return None; }
    let (d, y): (i64, i64) = (p[1].parse().ok()?, p[3].parse().ok()?);
    let m = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"].iter().position(|x| x.eq_ignore_ascii_case(p[2]))? as i64 + 1;
    let hms: Vec<i64> = p[4].split(':').filter_map(|v| v.parse().ok()).collect();
    if hms.len() != 3 { return None; }
    let (yy, mm) = if m <= 2 { (y - 1, m + 9) } else { (y, m - 3) };   // dias desde 1970 (algoritmo civil)
    let era = yy.div_euclid(400); let yoe = yy - era * 400;
    let doy = (153 * mm + 2) / 5 + d - 1; let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    Some((era * 146097 + doe - 719468) * 86400 + hms[0] * 3600 + hms[1] * 60 + hms[2])
}
fn note_clock(r: &ureq::Response) {
    if let Some(srv) = r.header("date").and_then(parse_http_date) {
        let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(srv);
        CLOCK_SKEW_S.store(srv - now, Ordering::Relaxed);
    }
}
/// Complemento para mensagens de recusa (401/403): cita o relógio quando ele está a mais de 60 s do servidor.
pub fn clock_hint() -> String {
    let s = clock_skew_s();
    if s.abs() > 60 { format!(" O relógio deste PC difere do servidor em {} min: ajuste a data e a hora.", s.abs() / 60) } else { String::new() }
}

fn fetch_token_inner(token_url: &str, access_token: &str, room: &str) -> Result<(String, Option<String>), TokenFail> {
    crate::sfu_cfg::check_token_url(token_url)?;
    if let Some(m) = crate::netmode::deny_url(token_url) { return Err(m.into()); }
    let resp = ureq::AgentBuilder::new().timeout(Duration::from_secs(10)).build()
        .post(token_url).set("authorization", &format!("Bearer {access_token}")).set("content-type", "application/json")
        .send_json(match previous_token(room, access_token) { Some(p) => json!({ "room": room, "previous": p }), None => json!({ "room": room }) });
    match &resp { Ok(r) | Err(ureq::Error::Status(_, r)) => note_clock(r), _ => {} }
    match resp {
        Ok(r) => {
            let v: Value = r.into_json().map_err(|e| format!("resposta do serviço de tokens inválida: {e}"))?;
            let token = v["token"].as_str().filter(|t| !t.is_empty() && t.len() < 4096).map(String::from).ok_or_else(|| "serviço de tokens não devolveu token".to_string())?;
            Ok((token, v["url"].as_str().map(|u| u.trim().to_string()).filter(|u| !u.is_empty())))
        }
        Err(ureq::Error::Status(429, r)) => {
            // limite de pedidos do serviço: diz quanto esperar em vez de um "recusou" genérico
            let secs = r.header("retry-after").and_then(|v| v.trim().parse::<u64>().ok()).unwrap_or(30).clamp(1, 600);
            Err(TokenFail { msg: format!("muitas tentativas no serviço de tokens: aguarde {secs} s"), retry_after: Some(secs) })
        }
        Err(ureq::Error::Status(code, r)) => {
            let msg = r.into_json::<Value>().ok().and_then(|v| v["message"].as_str().map(String::from)).unwrap_or_default();
            Err(format!("serviço de tokens recusou ({code}){}{}{}", if msg.is_empty() { "" } else { ": " }, msg.chars().take(160).collect::<String>(), if code == 401 || code == 403 { clock_hint() } else { String::new() }).into())
        }
        Err(e) => Err(format!("serviço de tokens inacessível: {}", e.to_string().chars().take(160).collect::<String>()).into()),
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Dono fixo: a função `livekit-token` põe a marca de dono no próprio token (metadata do participante, que o servidor de chamadas
// repassa a todos e o cliente não consegue alterar). Para o dono continuar o mesmo depois de uma queda, o pedido de token feito
// DURANTE uma chamada leva o token em uso (`previous`), que a função confere pela assinatura. Regra em
// supabase/functions/_shared/owner.ts.
// ---------------------------------------------------------------------------------------------------------------------
/// Tokens das chamadas em curso neste processo: (sala do LiveKit, identidade, token). Só existem enquanto a chamada está aberta.
static ACTIVE_TOKENS: Mutex<Vec<(String, String, String)>> = Mutex::new(Vec::new());

/// Corpo (claims) de um JWT, sem conferir a assinatura (quem confere é o servidor).
fn jwt_claims(token: &str) -> Option<Value> {
    use base64::Engine;
    let body = token.split('.').nth(1)?;
    serde_json::from_slice(&base64::engine::general_purpose::URL_SAFE_NO_PAD.decode(body.trim_end_matches('=')).ok()?).ok()
}
fn token_key(token: &str) -> Option<(String, String)> {
    let c = jwt_claims(token)?;
    Some((c["video"]["room"].as_str()?.to_string(), c["sub"].as_str()?.to_string()))
}
pub(crate) fn remember_token(token: &str) {
    let Some((room, ident)) = token_key(token) else { return };
    let mut t = ACTIVE_TOKENS.lock().unwrap();
    t.retain(|(r, i, _)| !(*r == room && *i == ident));
    t.push((room, ident, token.to_string()));
}
pub(crate) fn forget_token(token: &str) { ACTIVE_TOKENS.lock().unwrap().retain(|(_, _, t)| t != token); }
/// O token em uso NESTA sala por ESTE usuário (a identidade no servidor de chamadas é o `sub` da sessão da conta), se houver
/// uma chamada em curso: é o que prova a reentrada.
fn previous_token(room: &str, access_token: &str) -> Option<String> {
    let user = jwt_claims(access_token)?["sub"].as_str()?.to_string();
    ACTIVE_TOKENS.lock().unwrap().iter().find(|(r, i, _)| r == room && *i == user).map(|(_, _, t)| t.clone())
}

/// A metadata do participante (vinda do token) traz a marca de dono? Mesmo formato de `hasOwnerMark` da função.
fn has_owner_mark(metadata: &str) -> bool {
    if metadata.is_empty() || metadata.len() > 2048 { return false; }
    serde_json::from_str::<Value>(metadata).map(|v| v["briks"]["owner"] == json!(true)).unwrap_or(false)
}
fn joined_ms(p: &lk::ParticipantInfo) -> i64 { if p.joined_at_ms > 0 { p.joined_at_ms } else { p.joined_at.saturating_mul(1000) } }

/// Pede à função `livekit-kick` do Supabase para tirar `identity` da sala no servidor de mídia (só o dono consegue; a função confere).
/// Autenticado com a sessão do usuário, como o pedido de token. Bloqueante: chame de `spawn_blocking`.
pub fn kick_remote(kick_url: &str, access_token: &str, room: &str, identity: &str) -> Result<(), String> {
    crate::sfu_cfg::check_token_url(kick_url)?;
    if let Some(m) = crate::netmode::deny_url(kick_url) { return Err(m); }
    let resp = ureq::AgentBuilder::new().timeout(Duration::from_secs(8)).build()
        .post(kick_url).set("authorization", &format!("Bearer {access_token}")).set("content-type", "application/json")
        .send_json(json!({ "room": room, "identity": identity }));
    match resp {
        Ok(_) => Ok(()),
        Err(ureq::Error::Status(code, r)) => {
            let msg = r.into_json::<Value>().ok().and_then(|v| v["message"].as_str().map(String::from)).unwrap_or_default();
            Err(format!("serviço de expulsão recusou ({code}){}{}", if msg.is_empty() { "" } else { ": " }, msg.chars().take(160).collect::<String>()))
        }
        Err(e) => Err(format!("serviço de expulsão inacessível: {}", e.to_string().chars().take(160).collect::<String>())),
    }
}

/// A URL de conexão carrega o token: tira da mensagem de erro qualquer trecho `access_token=…`.
fn redact(s: &str) -> String {
    match s.find("access_token=") { Some(i) => { let end = s[i..].find(|c: char| c == '&' || c.is_whitespace()).map(|e| i + e).unwrap_or(s.len()); format!("{}access_token=***{}", &s[..i], &s[end..]) } None => s.to_string() }
}

struct ActiveShare { gen: u64, sender: share::Sender, audio: Option<crate::procaudio::ProcAudio>, info: Option<Value>, label: String, h: u32 }
struct ActiveCam { gen: u64, sender: share::Sender, info: Option<Value> }

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
    share: [Option<ActiveShare>; 2],   // por slot: até duas telas ao mesmo tempo (0 = 1ª, 1 = 2ª)
    share_gens: [u64; 2],
    cam: Option<ActiveCam>,
    cam_gen: u64,
    cam_vout: Option<share::VideoOut>,
    cam_net: Arc<Mutex<crate::adapt::NetShared>>,   // RTCP dos espectadores da câmera: não mistura com a adaptação da tela
    cam_idr: Arc<AtomicBool>,
    vout: [Option<share::VideoOut>; 2],
    atrack: Option<Arc<webrtc::track::track_local::track_local_static_sample::TrackLocalStaticSample>>,
    share_gen: u64,
    net: [Arc<Mutex<crate::adapt::NetShared>>; 2],
    idr: [Arc<AtomicBool>; 2],
    mic_sid: Option<String>,
    last_mic_open: bool,
    ping_ms: u64,
    subscriber_primary: bool,
    data: Option<crate::sfudata::Layer>,
    dc_pub: Option<Arc<webrtc::data_channel::RTCDataChannel>>,
    dc_queue: Vec<Vec<u8>>,
    leave: bool,
    /// anexos pela camada de dados (só com ela ligada); `_ftx` mantém a fila aberta mesmo sem transferências
    files: Option<crate::sfufiles::Files>,
    _ftx: mpsc::Sender<crate::sfufiles::FileOut>,
    // reconexão
    url: String,
    token: String,
    renew: Option<TokenFn>,
    epoch: u32,
    down: Option<Down>,
    /// identidade -> prazo: conhecidos antes da queda que ainda podem estar voltando (não anuncia saída/fim de transmissão antes do prazo)
    stale: HashMap<String, std::time::Instant>,
    audio_started: bool,
    last_rx: std::time::Instant,
    ping_timeout: Duration,
    /// identidade -> instante de entrada (ms, do servidor) de quem entrou com a marca de dono no token (eu incluído)
    owner_marks: HashMap<String, i64>,
    /// a faixa de vídeo está sendo REpublicada depois de uma reconexão (ver `on_track_published`)
    republish_video: bool,
}

#[derive(Default, Clone)]
struct KnownPeer { sid: String, mic: Option<bool>, share: [Option<(String, u32, u32)>; 2], cam: Option<(u32, u32)>, name: String, saudio: bool }

impl Actor {
    async fn emit(&self, e: MeshEvent) {
        if let (MeshEvent::Error(m), true) = (&e, std::env::var("BRIKS_SFU_DEBUG").is_ok()) { eprintln!("[sfu {}] ERRO {m}", &self.me.identity[..4.min(self.me.identity.len())]); }
        let _ = self.ev.send(e).await;
    }
    async fn send(&mut self, m: rq::Message) {
        if std::env::var("BRIKS_SFU_DEBUG").is_ok() { let d = format!("{m:?}"); eprintln!("[sfu {}] -> {}", &self.me.identity[..4.min(self.me.identity.len())], &d[..d.len().min(1500)]); }
        let _ = self.tx.send(encode_req(m)).await; }

    async fn handle_join(&mut self, j: lk::JoinResponse, rejoin: bool) {
        self.emit(MeshEvent::Share(json!({ "ev": "net.sfu", "state": if rejoin { "rejoined" } else { "joined" }, "server_version": j.server_version, "region": j.server_region, "room": j.room.as_ref().map(|r| r.name.clone()).unwrap_or_default() }))).await;
        if rejoin {
            // sessão nova no servidor: sids antigos não valem; quem eu conhecia ganha um prazo para reaparecer
            let deadline = std::time::Instant::now() + GRACE;
            for id in self.known.keys() { self.stale.insert(id.clone(), deadline); }
            for k in self.known.values_mut() { k.mic = None; }
            { let mut sh = self.shared.lock().unwrap(); sh.tracks.clear(); sh.part_ident.clear(); }
            self.subscribed.clear();
            self.pending.clear();
            self.published.clear();
            self.last_mic_open = true;
        }
        // marca de dono no MEU token (a dos outros vem em `on_participants`; quem eu conhecia antes de uma queda mantém a marca
        // até reaparecer ou o prazo vencer, para o dono não "trocar" enquanto ele ainda está voltando)
        let me = self.me.clone();
        if has_owner_mark(&me.metadata) { self.owner_marks.insert(me.identity.clone(), joined_ms(&me)); } else { self.owner_marks.remove(&me.identity); }
        self.on_participants(&j.other_participants).await;
        self.sync_owner().await;
        // áudio do microfone: publica agora (o publisher só conecta quando há o que enviar)
        if let Some(a) = self.audio.clone() {
            if !std::mem::replace(&mut self.audio_started, true) { a.start(); }
            self.publish("audio", a.track.clone() as Arc<dyn TrackLocal + Send + Sync>, false, lk::TrackType::Audio, lk::TrackSource::Microphone, "microfone", 0, 0).await;
        } else if self.data.is_some() {
            // sem áudio nada abriria o publisher, e é dele a data channel por onde saem chat/estado
            match self.ensure_publisher().await { Ok(_) => self.negotiate_publisher().await, Err(e) => self.emit(MeshEvent::Error(format!("publisher: {e}"))).await }
        }
        if rejoin {
            // transmissão em curso: republica as mesmas faixas (o codificador e o pacer nem pararam)
            for slot in 0..2usize {
                let sh = self.share[slot].as_ref().map(|s| (s.label.clone(), s.h, s.audio.is_some()));
                if let (Some((label, h, has_audio)), Some(vout)) = (sh, self.vout[slot].clone()) {
                    self.republish_video = true;
                    let name = track_name(&label, slot);
                    self.publish(share::video_cid(slot), vout.track() as Arc<dyn TrackLocal + Send + Sync>, true, lk::TrackType::Video, lk::TrackSource::ScreenShare, &name, h * 16 / 9, h).await;
                    if let (true, Some(at)) = (has_audio, self.atrack.clone()) {
                        self.publish(share::AUDIO_TRACK_ID, at as Arc<dyn TrackLocal + Send + Sync>, false, lk::TrackType::Audio, lk::TrackSource::ScreenShareAudio, "audio da tela", 0, 0).await;
                    }
                }
            }
            // câmera ligada: republica a faixa dela também
            if let (Some(_), Some(vout)) = (self.cam.as_ref(), self.cam_vout.clone()) {
                self.publish(share::CAMERA_TRACK_ID, vout.track() as Arc<dyn TrackLocal + Send + Sync>, true, lk::TrackType::Video, lk::TrackSource::Camera, "câmera", 640, 360).await;
            }
            // meu estado (microfone/status) de novo para todos: quem voltou depois de mim pode ter perdido a última mudança
            if self.data.is_some() {
                let media = self.media();
                let outs = self.data.as_mut().and_then(|l| l.on_cmd(MeshCmd::BroadcastState, media)).unwrap_or_default();
                self.apply(outs).await;
            }
        }
    }

    /// A sessão caiu (WebSocket fechado, servidor mudo ou mídia falhou): entra no estado "fora do ar"; o laço tenta voltar.
    fn lost(&mut self, reason: &str) {
        if self.down.is_some() || self.leave { return; }
        self.down = Some(Down { since: std::time::Instant::now(), attempt: 0, next: tokio::time::Instant::now() + retry_delay(0), token_fresh: false, announced: false, reason: reason.to_string(), token_at: None });
    }

    /// Fim definitivo que NÃO é expulsão (a sala não fica bloqueada nesta sessão): a UI mostra o erro e o usuário pode entrar de novo.
    async fn terminal(&mut self, msg: &str) {
        self.emit(MeshEvent::Error(msg.to_string())).await;
        self.emit(MeshEvent::Share(json!({ "ev": "net.sfu", "state": "lost", "message": msg }))).await;
        self.emit(MeshEvent::Share(json!({ "ev": "signaling.status", "state": "erro", "message": msg }))).await;
        self.leave = true;
    }

    /// Desmonta a sessão que caiu: fecha os PeerConnections antigos (em segundo plano) e zera o que dependia deles.
    /// Ficam: áudio (faixa e captura), transmissão (codificador, pacer, faixa), camada de dados (pares autenticados, fila de envio),
    /// lista de participantes conhecidos e quem eu estava assistindo.
    async fn teardown(&mut self) {
        self.epoch = self.epoch.wrapping_add(1);
        let _ = tokio::time::timeout(Duration::from_secs(1), self.tx.close()).await;
        let (old_sub, old_pub) = (self.sub.clone(), self.publisher.take());
        tokio::spawn(async move { if let Some(p) = old_pub { let _ = tokio::time::timeout(Duration::from_secs(3), p.close()).await; } let _ = tokio::time::timeout(Duration::from_secs(3), old_sub.close()).await; });
        self.dc_pub = None;
        self.pending.clear();
        self.published.clear();
        self.pub_offering = false;
        self.pub_dirty = false;
        self.pend_ice = [vec![], vec![]];
        self.mic_sid = None;
        self.stats.sub_connected.store(false, Ordering::Relaxed);
        self.stats.pub_connected.store(false, Ordering::Relaxed);
        // transferências em curso não sobrevivem à queda (pacotes perdidos quebrariam a sequência): falham de forma visível
        let peers = self.data.as_ref().map(|l| l.authed_idents()).unwrap_or_default();
        if let Some(f) = self.files.as_mut() { for (ident, fp) in peers { f.peer_gone(&ident, &fp, "a conexão com o servidor de chamadas caiu").await; } }
        let reason = self.down.as_ref().map(|d| d.reason.clone()).unwrap_or_default();
        self.emit(MeshEvent::Share(json!({ "ev": "net.sfu", "state": "reconnecting", "reason": reason }))).await;
        self.emit(MeshEvent::Share(json!({ "ev": "signaling.status", "state": "reconectando", "message": reason }))).await;
    }

    /// Uma tentativa de voltar: token novo (uma vez por queda, ou de novo se o servidor recusar), sinalização do zero (o servidor pode
    /// ter reiniciado e esquecido a sessão), PeerConnections novos e as mesmas faixas republicadas.
    async fn try_rejoin(&mut self) -> Option<WsRx> {
        let (attempt, since, fresh) = { let d = self.down.as_mut()?; d.attempt += 1; (d.attempt, d.since, d.token_fresh) };
        if since.elapsed() > GIVE_UP { self.terminal("conexão com o servidor de chamadas perdida (sem resposta por 2 minutos)").await; return None; }
        let fail = |a: &mut Actor, e: String| { if let Some(d) = a.down.as_mut() { d.next = tokio::time::Instant::now() + retry_delay(attempt); } e };
        // Token novo: uma vez por queda; de novo só se o servidor recusar, e nunca mais de um pedido a cada 10 s (com o token
        // recusado e o pedido ainda cedo demais, a tentativa segue com o token que há: não gasta a cota do serviço em laço).
        let too_soon = self.down.as_ref().and_then(|d| d.token_at).map(|t| t.elapsed() < TOKEN_MIN_GAP).unwrap_or(false);
        if !fresh && !too_soon {
            if let Some(f) = self.renew.clone() {
                if let Some(d) = self.down.as_mut() { d.token_at = Some(std::time::Instant::now()); }
                match tokio::time::timeout(Duration::from_secs(12), f()).await.unwrap_or_else(|_| Err(TokenFail::from("serviço de tokens não respondeu".to_string()))) {
                    Ok(t) => { forget_token(&self.token); remember_token(&t); self.token = t; self.stats.token_renewals.fetch_add(1, Ordering::Relaxed); if let Some(d) = self.down.as_mut() { d.token_fresh = true; } }
                    Err(e) => {
                        let msg = fail(self, e.msg);
                        // 429: o serviço disse quanto esperar; a próxima tentativa (e o próximo pedido de token) só depois disso
                        if let (Some(secs), Some(d)) = (e.retry_after, self.down.as_mut()) { d.next = tokio::time::Instant::now() + Duration::from_secs(secs.clamp(1, 110)); }
                        self.emit(MeshEvent::Share(json!({ "ev": "net.sfu", "state": "retry", "attempt": attempt, "error": format!("token: {msg}"), "retry_after": e.retry_after }))).await;
                        if e.retry_after.is_some() { self.emit(MeshEvent::Share(json!({ "ev": "signaling.status", "state": "reconectando", "message": msg }))).await; }
                        return None;
                    }
                }
            }
        }
        let opened = open_session(&self.url, &self.token, 6).await;
        let (tx, rx, join, _) = match opened {
            Ok(x) => x,
            Err(e) => {
                // recusa do servidor = token vencido/inválido: a próxima tentativa busca outro
                if e.auth { if let Some(d) = self.down.as_mut() { d.token_fresh = false; } }
                let e = fail(self, e.msg);
                self.emit(MeshEvent::Share(json!({ "ev": "net.sfu", "state": "retry", "attempt": attempt, "error": e }))).await;
                return None;
            }
        };
        let ice = ice_of(&join);
        let sub = match make_sub(&ice, self.epoch, &self.itx, self.audio.clone(), self.shared.clone(), self.ev.clone(), self.sev_tx.clone(), self.stats.clone()).await {
            Ok(s) => s,
            Err(e) => { let e = fail(self, e.to_string()); self.emit(MeshEvent::Share(json!({ "ev": "net.sfu", "state": "retry", "attempt": attempt, "error": e }))).await; return None; }
        };
        self.tx = tx; self.ice = ice; self.sub = sub;
        if let Some(p) = join.participant.clone() { self.me = p; }
        self.down = None;
        self.last_rx = std::time::Instant::now();
        self.stats.reconnects.fetch_add(1, Ordering::Relaxed);
        self.handle_join(join, true).await;
        self.emit(MeshEvent::Share(json!({ "ev": "signaling.status", "state": "conectado", "message": "" }))).await;
        self.emit(MeshEvent::Share(json!({ "ev": "net.sfu", "state": "reconnected", "attempt": attempt, "down_ms": since.elapsed().as_millis() as u64 }))).await;
        Some(rx)
    }

    async fn peer_gone(&mut self, identity: &str, sid: &str) {
        // 1.4.1: o que a pessoa transmitia (tela nos dois slots e câmera) acabou com ela: avisa a UI para tirar o bloco e a última imagem
        if let Some(k) = self.known.remove(identity) {
            for ev in gone_events(identity, &[k.share[0].is_some(), k.share[1].is_some()], k.cam.is_some()) { self.emit(MeshEvent::Share(ev)).await; }
        }
        self.stale.remove(identity);
        { let mut sh = self.shared.lock().unwrap(); if !sid.is_empty() { sh.part_ident.remove(sid); } sh.tracks.retain(|_, t| t.identity != identity); sh.recv.remove(identity); sh.recv_cam.remove(identity); sh.recv.remove(&share::recv_key(identity, 1)); }
        if let Some(a) = &self.audio { a.remove_peer(identity); }
        let gone_fp = self.data.as_ref().and_then(|l| l.peer_key(identity)).map(|k| k.0);
        if let (Some(f), Some(fp)) = (self.files.as_mut(), gone_fp) { f.peer_gone(identity, &fp, "a pessoa saiu da chamada").await; }
        let left_outs = self.data.as_mut().map(|l| l.left(identity)).unwrap_or_default();
        self.apply(left_outs).await;
        self.emit(MeshEvent::Peer { peer_id: identity.to_string(), state: "fechado".into() }).await;
        if self.owner_marks.remove(identity).is_some() { self.sync_owner().await; }
    }

    /// Diz à camada de dados quem o servidor de chamadas marcou como dono: entre os marcados, o que entrou primeiro (empate: menor
    /// identidade). É a mesma regra da função `livekit-kick`, para o app e o servidor concordarem sobre quem pode expulsar.
    async fn sync_owner(&mut self) {
        let best = self.owner_marks.iter().min_by(|a, b| (a.1, a.0).cmp(&(b.1, b.0))).map(|(id, _)| id.clone());
        let outs = self.data.as_mut().map(|l| l.set_server_owner(best)).unwrap_or_default();
        if !outs.is_empty() { self.apply(outs).await; }
    }

    /// Prazo vencido depois de uma reconexão: quem não voltou saiu; quem voltou sem a transmissão parou de transmitir.
    async fn expire_stale(&mut self) {
        if self.down.is_some() || self.stale.is_empty() { return; }
        let now = std::time::Instant::now();
        let due: Vec<String> = self.stale.iter().filter(|(_, t)| **t <= now).map(|(k, _)| k.clone()).collect();
        for id in due {
            self.stale.remove(&id);
            let (here, sharing) = { let sh = self.shared.lock().unwrap(); (sh.part_ident.values().any(|v| *v == id), [0usize, 1].map(|s| sh.tracks.values().any(|t| t.identity == id && t.source == lk::TrackSource::ScreenShare as i32 && !t.muted && share::slot_of_name(&t.name) == s))) };
            if !here { self.peer_gone(&id, "").await; continue; }
            let cam_on = { let sh = self.shared.lock().unwrap(); sh.tracks.values().any(|t| t.identity == id && t.source == lk::TrackSource::Camera as i32 && !t.muted) };
            if !cam_on && self.known.get(&id).map(|k| k.cam.is_some()).unwrap_or(false) {
                if let Some(k) = self.known.get_mut(&id) { k.cam = None; }
                let r = self.shared.lock().unwrap().recv_cam.get(&id).cloned(); if let Some(r) = r { r.watching.store(false, Ordering::Relaxed); }
                self.emit(MeshEvent::Share(json!({ "ev": "cam.remote", "peer_id": id, "state": "off" }))).await;
            }
            for slot in 0..2usize {
                if !sharing[slot] && self.known.get(&id).map(|k| k.share[slot].is_some()).unwrap_or(false) {
                    if let Some(k) = self.known.get_mut(&id) { k.share[slot] = None; }
                    let r = self.shared.lock().unwrap().recv.get(&share::recv_key(&id, slot)).cloned(); if let Some(r) = r { r.watching.store(false, Ordering::Relaxed); }
                    self.emit(MeshEvent::Share(json!({ "ev": "share.remote", "peer_id": id, "slot": slot, "state": "off" }))).await;
                }
            }
        }
    }

    async fn publish(&mut self, cid: &str, track: Arc<dyn TrackLocal + Send + Sync>, video: bool, ttype: lk::TrackType, source: lk::TrackSource, name: &str, w: u32, h: u32) {
        let codec = if !video { crate::codec::Codec::H264 } else if cid == share::CAMERA_TRACK_ID { crate::codec::Codec::H264 } else { self.vout[if cid == share::VIDEO_TRACK_ID_2 { 1 } else { 0 }].as_ref().map(|v| v.codec()).unwrap_or_default() };
        self.pending.insert(cid.to_string(), Pending { track, video, codec });
        let req = lk::AddTrackRequest { cid: cid.into(), name: name.into(), r#type: ttype as i32, width: w, height: h, source: source as i32, simulcast_codecs: if ttype == lk::TrackType::Video { vec![lk::SimulcastCodec { codec: codec.name().into(), cid: cid.into(), ..Default::default() }] } else { vec![] }, stereo: ttype == lk::TrackType::Audio && source == lk::TrackSource::ScreenShareAudio, disable_dtx: false, ..Default::default() };
        self.send(rq::Message::AddTrack(req)).await;
    }

    async fn ensure_publisher(&mut self) -> Result<Arc<RTCPeerConnection>> {
        if let Some(p) = &self.publisher { return Ok(p.clone()); }
        let pc = build_pc(&self.ice, true).await?;
        wire_pc(&pc, self.epoch, 0, &self.itx);
        // o servidor espera as duas data channels na offer do publisher
        let dc = pc.create_data_channel("_reliable", Some(RTCDataChannelInit { ordered: Some(true), ..Default::default() })).await?;
        { let (itx, ep) = (self.itx.clone(), self.epoch); dc.on_open(Box::new(move || { let itx = itx.clone(); Box::pin(async move { let _ = itx.send(Internal::DcOpen(ep)).await; }) })); }
        self.dc_pub = Some(dc);
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
            #[cfg(test)]
            let offer = { let mut o = offer; if crate::netsim::for_peer(&self.me.identity).is_some() { o.sdp = crate::netsim::strip_sdp(&o.sdp); } o };
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
                if p.video {   // a m-line desta faixa só oferece o codec dela (H264, VP9 ou VP8)
                    for t in pc.get_transceivers().await {
                        if Arc::ptr_eq(&t.sender().await, &sender) {
                            let cap = webrtc::rtp_transceiver::rtp_codec::RTCRtpCodecCapability { mime_type: p.codec.mime().into(), clock_rate: 90_000, channels: 0, sdp_fmtp_line: share::video_fmtp(p.codec).into(), rtcp_feedback: vec![] };
                            let pt = match p.codec { crate::codec::Codec::H264 => 102, crate::codec::Codec::Vp9 => 98, crate::codec::Codec::Vp8 => 96 };
                            if let Err(e) = t.set_codec_preferences(vec![webrtc::rtp_transceiver::rtp_codec::RTCRtpCodecParameters { capability: cap, payload_type: pt, ..Default::default() }]).await { self.emit(MeshEvent::Error(format!("codec da faixa {}: {e}", r.cid))).await; }
                            break;
                        }
                    }
                }
                if p.video && r.cid == share::CAMERA_TRACK_ID {
                    self.cam_idr.store(true, Ordering::Relaxed);
                    let nack = self.cam_vout.as_ref().map(|v| (v.nack_tx.clone(), v.rtx_gap_ms.clone(), v.rtx_twice.clone()));
                    spawn_rtcp_reader(sender.clone(), self.cam_net.clone(), self.cam_idr.clone(), nack, self.stats.clone());
                } else if p.video {
                    let slot = if r.cid == share::VIDEO_TRACK_ID_2 { 1 } else { 0 };   // cada tela tem pacer, estimador e pedido de quadro-chave próprios
                    // Publicação nova: pede um quadro-chave. REpublicação depois de uma reconexão: NÃO pede aqui. O codificador
                    // nem parou, e um quadro-chave agora sairia antes de o transporte novo existir (perdido); pior, o pedido do
                    // servidor quando o espectador reassina (PLI) teria de esperar o intervalo mínimo entre quadros-chave (1 a 2 s)
                    // contado a partir desse quadro desperdiçado. Sem ele, o PLI do servidor é atendido na hora.
                    let rekey = !std::mem::take(&mut self.republish_video) || (cfg!(test) && std::env::var("BRIKS_SFU_TEST_REKEY_OLD").is_ok());
                    if rekey { self.idr[slot].store(true, Ordering::Relaxed); }
                    else {
                        // rede de segurança: se em 1,5 s nenhum pedido de quadro-chave chegou do servidor (ninguém assistindo ainda,
                        // ou o pedido se perdeu), manda um mesmo assim
                        let (idr, stats, plis0) = (self.idr[slot].clone(), self.stats.clone(), self.stats.plis.load(Ordering::Relaxed));
                        tokio::spawn(async move { tokio::time::sleep(Duration::from_millis(1500)).await; if stats.plis.load(Ordering::Relaxed) == plis0 { idr.store(true, Ordering::Relaxed); } });
                    }
                    let nack = self.vout[slot].as_ref().map(|v| (v.nack_tx.clone(), v.rtx_gap_ms.clone(), v.rtx_twice.clone()));
                    spawn_rtcp_reader(sender.clone(), self.net[slot].clone(), self.idr[slot].clone(), nack, self.stats.clone());
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

    /// Só testes (rede ruim simulada entre ESTE cliente e o SFU): os candidatos do servidor que vêm dentro do SDP passam a apontar
    /// para o proxy UDP do `netsim` (os que ele não sabe encaminhar, como TCP e IPv6, saem), para nenhum pacote contornar o proxy.
    #[cfg(test)]
    async fn sim_in_sdp(&self, sdp: String) -> String {
        let Some(ns) = crate::netsim::for_peer(&self.me.identity) else { return sdp };
        let mut out = vec![];
        for l in sdp.split("\r\n") {
            if let Some(c) = l.strip_prefix("a=") .filter(|c| c.starts_with("candidate:")) { if let Some(n) = ns.rewrite(c).await { out.push(format!("a={n}")); } }
            else if !l.starts_with("a=end-of-candidates") { out.push(l.to_string()); }
        }
        out.join("\r\n")
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
                #[cfg(test)]
                let o = { let mut o = o; o.sdp = self.sim_in_sdp(o.sdp).await; o };
                let r: Result<()> = async {
                    self.sub.set_remote_description(RTCSessionDescription::offer(o.sdp)?).await?;
                    self.flush_ice(1).await;
                    let ans = self.sub.create_answer(None).await?;
                    self.sub.set_local_description(ans.clone()).await?;
                    #[cfg(test)]
                    let ans = { let mut a = ans; if crate::netsim::for_peer(&self.me.identity).is_some() { a.sdp = crate::netsim::strip_sdp(&a.sdp); } a };
                    self.send(rq::Message::Answer(lk::SessionDescription { r#type: "answer".into(), sdp: ans.sdp, id: o.id, mid_to_track_id: Default::default() })).await;
                    Ok(())
                }.await;
                if let Err(e) = r { self.emit(MeshEvent::Error(format!("resposta ao SFU: {e}"))).await; }
            }
            rs::Message::Answer(a) => {
                #[cfg(test)]
                let a = { let mut a = a; a.sdp = self.sim_in_sdp(a.sdp).await; a };
                if let Some(pc) = self.publisher.clone() {
                    match RTCSessionDescription::answer(a.sdp) { Ok(d) => { if let Err(e) = pc.set_remote_description(d).await { self.emit(MeshEvent::Error(format!("answer do SFU: {e}"))).await; } } Err(e) => self.emit(MeshEvent::Error(format!("answer inválida: {e}"))).await }
                    self.flush_ice(0).await;
                }
                self.pub_offering = false;
                if std::mem::take(&mut self.pub_dirty) { self.negotiate_publisher().await; }
            }
            rs::Message::Trickle(t) => {
                if let Ok(init) = serde_json::from_str::<RTCIceCandidateInit>(&t.candidate_init) {
                    #[cfg(test)]
                    let init = { let mut init = init; if let Some(ns) = crate::netsim::for_peer(&self.me.identity) { match ns.rewrite(&init.candidate).await { Some(c) => init.candidate = c, None => return } } init };
                    self.add_ice(t.target, init).await;
                }
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
            rs::Message::Leave(l) => {
                use lk::DisconnectReason as R;
                match R::try_from(l.reason).unwrap_or(R::UnknownReason) {
                    // só a remoção pela administração (expulsão feita pelo dono, via função do Supabase) bloqueia a sala nesta sessão
                    R::ParticipantRemoved => { self.emit(MeshEvent::Kicked { reason: "removido da chamada pelo dono".into() }).await; self.leave = true; }
                    R::DuplicateIdentity => self.terminal("esta conta entrou na chamada em outro aparelho").await,
                    R::RoomDeleted => self.terminal("a chamada foi encerrada").await,
                    // reinício/migração do servidor, estado dessincronizado etc.: volta sozinho
                    other => self.lost(&format!("o servidor pediu reconexão ({})", other.as_str_name())),
                }
            }
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
                // saída de uma sessão antiga de quem já voltou com outro sid: ignora
                if self.known.get(&p.identity).map(|k| !k.sid.is_empty() && k.sid != p.sid).unwrap_or(false) { continue; }
                self.peer_gone(&p.identity, &p.sid).await;
                continue;
            }
            let first = !self.known.contains_key(&p.identity);
            let mut k = self.known.get(&p.identity).cloned().unwrap_or_default();
            {
                let mut sh = self.shared.lock().unwrap();
                sh.part_ident.insert(p.sid.clone(), p.identity.clone());
                sh.recv.entry(p.identity.clone()).or_default();
                sh.recv_cam.entry(p.identity.clone()).or_default();
                for t in &p.tracks { sh.tracks.insert(t.sid.clone(), TrackMeta { identity: p.identity.clone(), source: t.source, ttype: t.r#type, name: t.name.clone(), w: t.width, h: t.height, muted: t.muted }); }
            }
            k.sid = p.sid.clone();
            if has_owner_mark(&p.metadata) { self.owner_marks.insert(p.identity.clone(), joined_ms(p)); } else { self.owner_marks.remove(&p.identity); }
            if first {
                let name = if p.name.is_empty() { p.identity.clone() } else { p.name.clone() };
                k.name = name.clone();
                // com a camada de dados, quem autentica é o hello assinado (fp real); sem ela, a identidade do token faz as vezes de fp
                if let Some(l) = self.data.as_mut() { l.present(&p.identity, &name); }
                else {
                    self.emit(MeshEvent::Peer { peer_id: p.identity.clone(), state: "autenticado".into() }).await;
                    self.emit(MeshEvent::PeerInfo { peer_id: p.identity.clone(), fp: p.identity.clone(), name }).await;
                }
            }
            // microfone
            let mic = p.tracks.iter().find(|t| t.source == lk::TrackSource::Microphone as i32).map(|t| t.muted);
            if mic != k.mic {
                k.mic = mic;
                if self.data.is_none() { self.emit(MeshEvent::PeerState { peer_id: p.identity.clone(), microphone: mic == Some(false), deafened: false, status: String::new(), presence: String::new() }).await; }
            }
            // áudio de voz: assina sempre
            for t in p.tracks.iter().filter(|t| t.source == lk::TrackSource::Microphone as i32) {
                if self.subscribed.insert(t.sid.clone()) { want_sub.push((p.sid.clone(), t.sid.clone())); }
            }
            // transmissão que eu estava assistindo (antes de uma reconexão, minha ou dele): assina de novo sozinho
            for slot in 0..2usize {
                if self.shared.lock().unwrap().recv.get(&share::recv_key(&p.identity, slot)).map(|r| r.watching.load(Ordering::Relaxed)).unwrap_or(false) {
                    for t in p.tracks.iter().filter(|t| (t.source == lk::TrackSource::ScreenShare as i32 && share::slot_of_name(&t.name) == slot) || (slot == 0 && t.source == lk::TrackSource::ScreenShareAudio as i32)) {
                        if self.subscribed.insert(t.sid.clone()) { want_sub.push((p.sid.clone(), t.sid.clone())); }
                    }
                }
            }
            // câmera que eu estava mostrando (antes de uma reconexão): assina de novo sozinho
            if self.shared.lock().unwrap().recv_cam.get(&p.identity).map(|r| r.watching.load(Ordering::Relaxed)).unwrap_or(false) {
                for t in p.tracks.iter().filter(|t| t.source == lk::TrackSource::Camera as i32 && !t.muted) {
                    if self.subscribed.insert(t.sid.clone()) { want_sub.push((p.sid.clone(), t.sid.clone())); }
                }
            }
            // câmera ligada/desligada (faixa publicada e não silenciada)
            {
                let cam = p.tracks.iter().find(|t| t.source == lk::TrackSource::Camera as i32 && !t.muted).map(|t| (t.width, t.height));
                if cam != k.cam && !(cam.is_none() && self.stale.contains_key(&p.identity)) {
                    k.cam = cam;
                    if cam.is_none() { let r = self.shared.lock().unwrap().recv_cam.get(&p.identity).cloned(); if let Some(r) = r { r.watching.store(false, Ordering::Relaxed); } }
                    let ev = match cam {
                        Some((w, h)) => json!({ "ev": "cam.remote", "peer_id": p.identity, "state": "on", "w": w, "h": h, "fps": 0, "kbps": 0 }),
                        None => json!({ "ev": "cam.remote", "peer_id": p.identity, "state": "off" }),
                    };
                    self.emit(MeshEvent::Share(ev)).await;
                }
            }
            // tela
            let has_audio = p.tracks.iter().any(|t| t.source == lk::TrackSource::ScreenShareAudio as i32);
            for slot in 0..2usize {   // até duas telas por pessoa: a 2ª é a faixa ScreenShare cujo nome começa com o prefixo de slot
                let scr = p.tracks.iter().find(|t| t.source == lk::TrackSource::ScreenShare as i32 && !t.muted && share::slot_of_name(&t.name) == slot);
                let now = scr.map(|t| (share::strip_slot_name(&t.name).to_string(), t.width, t.height));
                // dentro do prazo pós-reconexão, "sem tela" pode ser só "ainda não republicou": espera (expire_stale decide)
                // o áudio da tela pode chegar DEPOIS do vídeo: sem reanunciar, o app nunca soube que há áudio (sem barra de volume)
                let audio_changed = slot == 0 && now.is_some() && now == k.share[slot] && has_audio != k.saudio;
                if (now != k.share[slot] || audio_changed) && !(now.is_none() && self.stale.contains_key(&p.identity)) {
                    k.share[slot] = now.clone();
                    if slot == 0 { k.saudio = has_audio; }
                    if now.is_none() { let r = self.shared.lock().unwrap().recv.get(&share::recv_key(&p.identity, slot)).cloned(); if let Some(r) = r { r.watching.store(false, Ordering::Relaxed); } }
                    let ev = match &now {
                        Some((label, w, h)) => json!({ "ev": "share.remote", "peer_id": p.identity, "slot": slot, "state": "on", "w": w, "h": h, "fps": 0, "kbps": 0, "label": label, "audio": has_audio && slot == 0 }),
                        None => json!({ "ev": "share.remote", "peer_id": p.identity, "slot": slot, "state": "off" }),
                    };
                    self.emit(MeshEvent::Share(ev)).await;
                }
            }
            self.known.insert(p.identity.clone(), k);
        }
        self.subscribe(want_sub, true).await;
        self.sync_owner().await;
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

    async fn run(&mut self, rx: WsRx, mut cmds: mpsc::Receiver<MeshCmd>, mut irx: mpsc::Receiver<Internal>, mut srx: mpsc::Receiver<Value>, mut arx: Option<mpsc::Receiver<AudioEvent>>, mut frx: mpsc::Receiver<crate::sfufiles::FileOut>) {
        let mut rx = Some(rx);   // None = fora do ar, esperando a próxima tentativa
        let mut ping = tokio::time::interval(Duration::from_millis(self.ping_ms));
        let mut dtick = tokio::time::interval(Duration::from_secs(1));
        loop {
            let retry_at = self.down.as_ref().map(|d| d.next);
            tokio::select! {
                m = async { match rx.as_mut() { Some(r) => r.next().await, None => std::future::pending().await } } => match m {
                    Some(Ok(Message::Binary(b))) => {
                        self.last_rx = std::time::Instant::now();
                        self.stats.ws_msgs.fetch_add(1, Ordering::Relaxed);
                        if let Ok(lk::SignalResponse { message: Some(m) }) = lk::SignalResponse::decode(b.as_slice()) { self.on_signal(m).await; }
                    }
                    Some(Ok(Message::Close(_))) | None => self.lost("conexão com o servidor de chamadas caiu"),
                    Some(Ok(_)) => { self.last_rx = std::time::Instant::now(); }
                    Some(Err(e)) => self.lost(&format!("WebSocket: {}", redact(&e.to_string()))),
                },
                c = cmds.recv() => match c { Some(MeshCmd::Close) | None => break, Some(c) => self.on_cmd(c).await },
                i = irx.recv() => if let Some(i) = i { self.on_internal(i).await },
                v = srx.recv() => if let Some(v) = v { self.on_share_event(v).await },
                // pacotes de anexo: só saem com a sessão de pé e com folga no canal de dados (é isso que freia quem envia)
                f = frx.recv(), if self.down.is_none() => if let Some((to, m)) = f {
                    self.publish_data(vec![to], json!({ "v": 1, "t": "file", "m": m })).await;
                    if let Some(dc) = self.dc_pub.clone() { let t0 = std::time::Instant::now(); while dc.buffered_amount().await > 256 * 1024 && t0.elapsed() < Duration::from_secs(2) { tokio::time::sleep(Duration::from_millis(5)).await; } }
                },
                ae = async { match arx.as_mut() { Some(r) => r.recv().await, None => std::future::pending().await } } => match ae {
                    Some(AudioEvent::Speaking { peer, level }) => self.emit(MeshEvent::Speaking { peer_id: peer, level }).await,
                    Some(AudioEvent::Error(m)) => self.emit(MeshEvent::Error(format!("áudio: {m}"))).await,
                    Some(AudioEvent::Notice(v)) => self.emit(MeshEvent::Share(v)).await,
                    None => { arx = None; }
                },
                _ = dtick.tick() => {
                    if self.data.is_some() && self.down.is_none() { self.set_media(); let outs = self.data.as_mut().map(|l| l.tick()).unwrap_or_default(); self.apply(outs).await; }
                    self.expire_stale().await;
                }
                _ = ping.tick() => {
                    if self.down.is_none() {
                        // o TCP pode ficar "aberto" por minutos numa queda de rede: sem nenhuma mensagem do servidor pelo prazo dele, reconecta
                        if self.last_rx.elapsed() > self.ping_timeout + Duration::from_millis(self.ping_ms) { self.lost("o servidor de chamadas parou de responder"); }
                        else { let t = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() as i64).unwrap_or(0); self.send(rq::Message::Ping(t)).await; }
                    }
                }
                _ = async { match retry_at { Some(t) => tokio::time::sleep_until(t).await, None => std::future::pending().await } } => {
                    if let Some(nrx) = self.try_rejoin().await { rx = Some(nrx); }
                }
            }
            if self.leave { break; }
            if self.down.as_ref().map(|d| !d.announced).unwrap_or(false) {
                if let Some(d) = self.down.as_mut() { d.announced = true; }
                rx = None;
                self.teardown().await;
            }
        }
        // saída limpa: avisa o servidor, para captura/áudio e fecha os PeerConnections
        self.send(rq::Message::Leave(lk::LeaveRequest { can_reconnect: false, reason: lk::DisconnectReason::ClientInitiated as i32, action: lk::leave_request::Action::Disconnect as i32, ..Default::default() })).await;
        let _ = self.tx.close().await;
        if let Some(a) = &self.audio { a.stop(); }
        for s in 0..2 { if let Some(sh) = self.share[s].take() { sh.sender.stop(); if let Some(a) = &sh.audio { a.stop(); } } }
        if let Some(c) = self.cam.take() { c.sender.stop(); }   // fechar a chamada desliga a captura
        if let Some(p) = self.publisher.take() { let _ = p.close().await; }
        let _ = self.sub.close().await;
    }

    async fn on_internal(&mut self, i: Internal) {
        match i {
            Internal::Data(ep, b) => { if ep == self.epoch { self.on_data(b).await } }
            Internal::DcOpen(ep) => { if ep == self.epoch { for b in std::mem::take(&mut self.dc_queue) { self.dc_send_raw(b).await; } } }
            Internal::Ice { epoch, target, init } => {
                if epoch != self.epoch { return; }
                #[cfg(test)]
                let init = { let mut init = init; if let Some(ns) = crate::netsim::for_peer(&self.me.identity) { match ns.rewrite(&init.candidate).await { Some(c) => init.candidate = c, None => return } } init };
                if let Ok(s) = serde_json::to_string(&init) { self.send(rq::Message::Trickle(lk::TrickleRequest { candidate_init: s, target, r#final: false })).await; }
            }
            Internal::Pc { epoch, target, state } => {
                if epoch != self.epoch || self.down.is_some() { return; }
                let up = state == RTCPeerConnectionState::Connected;
                if target == 1 { self.stats.sub_connected.store(up, Ordering::Relaxed); } else { self.stats.pub_connected.store(up, Ordering::Relaxed); }
                self.emit(MeshEvent::Share(json!({ "ev": "net.sfu", "state": format!("{}:{}", if target == 1 { "sub" } else { "pub" }, state) }))).await;
                if state == RTCPeerConnectionState::Failed { self.lost("conexão de mídia com o servidor de chamadas falhou"); }
            }
        }
    }

    async fn on_cmd(&mut self, c: MeshCmd) {
        if self.data.is_some() && matches!(c, MeshCmd::Chat { .. } | MeshCmd::ChatWithId { .. } | MeshCmd::ChatInvite { .. } | MeshCmd::SoundPlay { .. } | MeshCmd::SoundFetch { .. } | MeshCmd::DmDisconnect { .. } | MeshCmd::Typing | MeshCmd::GroupSend { .. } | MeshCmd::RoomChat { .. } | MeshCmd::StreamRequest { .. } | MeshCmd::StreamReply { .. } | MeshCmd::Event { .. } | MeshCmd::Kick { .. } | MeshCmd::Allow { .. } | MeshCmd::RemoteMute { .. } | MeshCmd::SetProfile { .. }) {
            let media = self.media();
            let outs = self.data.as_mut().and_then(|l| l.on_cmd(c, media)).unwrap_or_default();
            self.apply(outs).await;
            return;
        }
        if self.files.is_some() && matches!(c, MeshCmd::SendFile { .. } | MeshCmd::AcceptFile { .. } | MeshCmd::PauseFile { .. } | MeshCmd::CancelFile { .. }) {
            let peers = self.data.as_ref().map(|l| l.authed_idents()).unwrap_or_default();
            if let Some(f) = self.files.as_mut() {
                match c {
                    MeshCmd::SendFile { path, to_fp } => f.send_file(path, peers.into_iter().filter(|(_, fp)| to_fp.as_ref().map_or(true, |t| t == fp)).collect()).await,
                    MeshCmd::AcceptFile { id } => f.accept(id).await,
                    MeshCmd::PauseFile { id, paused } => f.pause(id, paused).await,
                    MeshCmd::CancelFile { id } => f.cancel(id).await,
                    _ => {}
                }
            }
            self.flush_files().await;
            return;
        }
        #[cfg(test)]
        if self.data.is_some() && matches!(c, MeshCmd::HistSeed { .. } | MeshCmd::HistAsk) {
            let media = self.media();
            let outs = self.data.as_mut().and_then(|l| l.on_cmd(c, media)).unwrap_or_default();
            self.apply(outs).await;
            return;
        }
        if matches!(c, MeshCmd::BroadcastState) && self.data.is_some() {
            let media = self.media();
            let outs = self.data.as_mut().and_then(|l| l.on_cmd(MeshCmd::BroadcastState, media)).unwrap_or_default();
            self.apply(outs).await;
        }
        match c {
            MeshCmd::BroadcastState => self.sync_mic().await,
            MeshCmd::SetProfile { .. } => {}   // status/presença ainda não viajam pelo SFU
            MeshCmd::SoundPlay { hash } => self.emit(MeshEvent::Sound(json!({ "ev": "sound.denied", "hash": hash, "message": "sons exigem a camada de dados da chamada" }))).await,
            MeshCmd::SoundFetch { .. } => {}
            MeshCmd::MutePeer { key, muted } => { if let Some(a) = &self.audio { a.mute_peer(&key, muted); } }
            MeshCmd::ShareVolume { key, gain } => { if let Some(a) = &self.audio { a.set_screen_gain(&key, gain); } }
            MeshCmd::ShareStart { cfg, preview, dump } => self.share_start(cfg, preview, dump, 0).await,
            MeshCmd::ShareStop => self.share_stop(0).await,
            MeshCmd::Share2Start { cfg, preview, dump } => self.share_start(cfg, preview, dump, 1).await,
            MeshCmd::Share2Stop => self.share_stop(1).await,
            MeshCmd::CamStart { device, preview } => self.cam_start(device, preview).await,
            MeshCmd::CamStop => self.cam_stop().await,
            MeshCmd::CamWatch { key, on, path } => self.watch_cam(&key, on, path).await,
            MeshCmd::ShareAudio { mode } => { if let Some(a) = self.share[0].as_ref().and_then(|s| s.audio.as_ref()) { a.set_mode(mode); } }
            MeshCmd::ShareAudioMute { muted } => { if let Some(a) = self.share[0].as_ref().and_then(|s| s.audio.as_ref()) { a.set_muted(muted); } }
            MeshCmd::ShareWatch { key, on, path } => self.watch(&key, on, path, 0).await,
            MeshCmd::Share2Watch { key, on, path } => self.watch(&key, on, path, 1).await,
            MeshCmd::AudioCfg(c) => { if let Some(a) = &self.audio { a.apply_cfg(&c); } }
            MeshCmd::PeerVolume { key, gain } => {
                // a UI manda a impressão digital (ou o id do participante): no SFU o áudio é indexado pela identidade
                let id = self.data.as_ref().and_then(|l| l.ident_for(&key)).unwrap_or(key);
                if let Some(a) = &self.audio { a.set_peer_gain(&id, gain); }
            }
            // Ainda só na malha P2P: anexos, "digitando", grupos e histórico não viajam pela camada de dados do SFU.
            MeshCmd::SendFile { .. } => self.emit(MeshEvent::Error("anexos ainda não estão disponíveis em chamadas pelo servidor".into())).await,
            MeshCmd::CancelFile { .. } | MeshCmd::AcceptFile { .. } | MeshCmd::PauseFile { .. } | MeshCmd::Typing | MeshCmd::GroupSend { .. } | MeshCmd::RoomChat { .. } | MeshCmd::StreamRequest { .. } | MeshCmd::StreamReply { .. } => {}
            #[cfg(test)]
            MeshCmd::HistSeed { .. } | MeshCmd::HistAsk => {}
            MeshCmd::Chat { .. } | MeshCmd::ChatWithId { .. } | MeshCmd::ChatInvite { .. } | MeshCmd::Event { .. } => self.emit(MeshEvent::Error("chat não disponível no modo SFU (use uma sala P2P)".into())).await,
            MeshCmd::Kick { .. } | MeshCmd::Allow { .. } => self.emit(MeshEvent::Error("expulsar exige a API de administração do LiveKit: ainda não disponível".into())).await,
            MeshCmd::RemoteMute { .. } => self.emit(MeshEvent::Error("mudo remoto exige a camada de dados do SFU".into())).await,
            MeshCmd::ForceOffer { .. } | MeshCmd::SetHybrid { .. } | MeshCmd::DmDisconnect { .. } => {}
            #[cfg(test)]
            MeshCmd::KickUnchecked { .. } => {}
            // só testes: publica bytes crus na data channel (provar que forjados/malformados são descartados pelos outros)
            #[cfg(test)]
            MeshCmd::RawDc { text } => {
                #[allow(deprecated)]
                let pkt = lk::DataPacket { kind: 0, participant_identity: self.me.identity.clone(), value: Some(lk::data_packet::Value::User(lk::UserPacket { payload: text.into_bytes(), topic: Some("briks".into()), ..Default::default() })), ..Default::default() };
                self.dc_send_raw(pkt.encode_to_vec()).await;
            }
            MeshCmd::Close => {}
            #[cfg(test)]
            MeshCmd::TestVcodecs(_) => {}
        }
    }

    /// Reflete no servidor o estado atual do microfone (faixa muda/aberta).
    async fn sync_mic(&mut self) {
        if let (Some(a), Some(sid)) = (self.audio.clone(), self.mic_sid.clone()) {
            let open = a.mic_open();
            if open != self.last_mic_open { self.last_mic_open = open; self.send(rq::Message::Mute(lk::MuteTrackRequest { sid, muted: !open })).await; }
        }
    }

    fn media(&self) -> (bool, bool) { self.audio.as_ref().map(|a| (a.mic_open(), a.deaf())).unwrap_or((true, false)) }
    fn set_media(&mut self) { let m = self.media(); if let Some(l) = self.data.as_mut() { l.set_media(m); } }

    /// Pacote de dados do LiveKit (DataPacket) recebido: o servidor carimba a identidade do remetente; só o tópico "briks" interessa.
    #[allow(deprecated)]
    async fn on_data(&mut self, bytes: Vec<u8>) {
        if self.data.is_none() { return; }
        let Ok(p) = lk::DataPacket::decode(bytes.as_slice()) else { return };
        let Some(lk::data_packet::Value::User(u)) = p.value else { return };
        if u.topic.as_deref() != Some("briks") { return; }
        let from = if !p.participant_identity.is_empty() { p.participant_identity } else { u.participant_identity };
        self.set_media();
        let outs = self.data.as_mut().map(|l| l.on_packet(&from, &u.payload)).unwrap_or_default();
        self.apply(outs).await;
    }

    #[allow(deprecated)]
    /// Publica as mensagens de controle dos anexos (aceitar, pausar, cancelar) sem passar pela fila dos pedaços.
    async fn flush_files(&mut self) {
        let urgent = self.files.as_mut().map(|f| f.take_urgent()).unwrap_or_default();
        for (to, m) in urgent { self.publish_data(vec![to], json!({ "v": 1, "t": "file", "m": m })).await; }
    }

    async fn publish_data(&mut self, to: Vec<String>, msg: Value) {
        let payload = msg.to_string().into_bytes();
        if payload.len() > crate::sfudata::MAX_PACKET { self.emit(MeshEvent::Error("mensagem grande demais para o canal de dados".into())).await; return; }
        let pkt = lk::DataPacket {
            kind: lk::data_packet::Kind::Reliable as i32, participant_identity: self.me.identity.clone(), destination_identities: to.clone(),
            value: Some(lk::data_packet::Value::User(lk::UserPacket { payload, topic: Some("briks".into()), destination_identities: to, ..Default::default() })), ..Default::default()
        };
        self.dc_send_raw(pkt.encode_to_vec()).await;
    }

    async fn dc_send_raw(&mut self, b: Vec<u8>) {
        use webrtc::data_channel::data_channel_state::RTCDataChannelState;
        match &self.dc_pub {
            Some(dc) if dc.ready_state() == RTCDataChannelState::Open => { let _ = dc.send(&bytes::Bytes::from(b)).await; }
            _ => { if self.dc_queue.len() < 512 { self.dc_queue.push(b); } }
        }
    }

    async fn apply(&mut self, outs: Vec<crate::sfudata::Out>) {
        use crate::sfudata::Out;
        for o in outs {
            match o {
                Out::Ev(e) => self.emit(e).await,
                Out::Send { to, msg } => self.publish_data(to, msg).await,
                Out::Mic(open) => {
                    if let Some(a) = &self.audio { a.set_mic_muted(!open); }
                    self.sync_mic().await;
                    self.set_media();
                    let media = self.media();
                    let outs = self.data.as_mut().and_then(|l| l.on_cmd(MeshCmd::BroadcastState, media)).unwrap_or_default();
                    for o in outs { if let Out::Send { to, msg } = o { self.publish_data(to, msg).await; } }
                }
                Out::MuteLocal { ident, muted } => { if let Some(a) = &self.audio { a.mute_peer(&ident, muted); } }
                Out::Leave => self.leave = true,
                Out::File { from, fp, jwk, m } => {
                    if let Some(f) = self.files.as_mut() { f.on_msg(&from, &fp, &jwk, &m).await; }
                    let urgent = self.files.as_mut().map(|f| f.take_urgent()).unwrap_or_default();
                    for (to, m) in urgent { self.publish_data(vec![to], json!({ "v": 1, "t": "file", "m": m })).await; }
                }
            }
        }
    }

    async fn watch(&mut self, key: &str, on: bool, path: std::path::PathBuf, slot: usize) {
        let (recv, sids): (Option<Arc<RecvState>>, Vec<(String, String)>) = {
            let mut sh = self.shared.lock().unwrap();
            let sids = sh.tracks.iter().filter(|(_, t)| t.identity == key && ((t.source == lk::TrackSource::ScreenShare as i32 && share::slot_of_name(&t.name) == slot) || (slot == 0 && t.source == lk::TrackSource::ScreenShareAudio as i32)))
                .map(|(sid, _)| (self.known.get(key).map(|k| k.sid.clone()).unwrap_or_default(), sid.clone())).collect();
            // a recepção da 2ª tela só nasce quando a faixa chega, e ela só chega depois de assinada: cria aqui para quem é conhecido
            let recv = if sh.recv.contains_key(key) { Some(sh.recv.entry(share::recv_key(key, slot)).or_default().clone()) } else { None };
            (recv, sids)
        };
        let Some(r) = recv else { self.emit(MeshEvent::Error("assistir: peer desconhecido".into())).await; return };
        if on { *r.sink_path.lock().unwrap() = Some(path.clone()); r.epoch.fetch_add(1, Ordering::Relaxed); r.frames.store(0, Ordering::Relaxed); }
        r.watching.store(on, Ordering::Relaxed);
        // só baixa o vídeo (e o áudio da transmissão) de quem está sendo assistido
        let todo: Vec<(String, String)> = sids.into_iter().filter(|(_, s)| if on { self.subscribed.insert(s.clone()) } else { self.subscribed.remove(s) }).collect();
        self.subscribe(todo, on).await;
        self.emit(MeshEvent::Share(json!({ "ev": "share.remote", "peer_id": key, "slot": slot, "state": if on { "watch" } else { "unwatch" }, "path": path.to_string_lossy() }))).await;
        let view_outs = self.data.as_ref().map(|l| l.viewing_out(key, slot, on)).unwrap_or_default();   // o emissor conta quem assiste (como na malha P2P)
        self.apply(view_outs).await;
    }

    /// A faixa de vídeo (e a de áudio da tela) é publicada uma vez e reaproveitada: parar = silenciar a faixa no servidor, retomar = reabrir.
    /// (Remover o transceiver e publicar de novo não funciona no webrtc-rs 0.12: o transceiver parado perde as codificações e o
    /// `add_track` seguinte falha com "same envelope as previous".) Os outros veem a faixa `muted` e tratam como "parou".
    async fn share_start(&mut self, mut cfg: ShareCfg, preview: Option<std::path::PathBuf>, dump: Option<std::path::PathBuf>, slot: usize) {
        self.share_stop(slot).await;
        if slot == 1 { cfg.audio = crate::procaudio::AudioMode::None; }   // o áudio da transmissão é um só, no slot 0
        self.share_gen += 1;
        let gen = self.share_gen;
        self.share_gens[slot] = gen;
        let cid = share::video_cid(slot);
        let first = !self.published.contains_key(cid) && !self.pending.contains_key(cid);
        let vout = self.vout[slot].get_or_insert_with(|| share::VideoOut::new_slot(slot)).clone();
        // o codec de cada tela é decidido na 1ª publicação da chamada (a faixa fica publicada entre uma transmissão e outra); depois dela, segue o mesmo
        if first { let want = if cfg.codec.available() { cfg.codec } else { crate::codec::Codec::H264 }; let _ = vout.set_codec(want); }
        let (amode, label, h) = (cfg.audio.clone(), cfg.label.clone(), cfg.height as u32);
        match share::start_sender(gen, self.idr[slot].clone(), self.net[slot].clone(), cfg, vout.clone(), self.sev_tx.clone(), preview, dump) {
            Ok(sender) => {
                let has_audio = amode != crate::procaudio::AudioMode::None;
                let atrack = self.atrack.get_or_insert_with(crate::procaudio::new_audio_track).clone();
                let audio = if has_audio { Some(crate::procaudio::start(amode, atrack.clone())) } else { None };
                self.share[slot] = Some(ActiveShare { gen, sender, audio, info: None, label: label.clone(), h });
                if first {
                    let name = track_name(&label, slot);
                    self.publish(cid, vout.track() as Arc<dyn TrackLocal + Send + Sync>, true, lk::TrackType::Video, lk::TrackSource::ScreenShare, &name, h * 16 / 9, h).await;
                } else { self.set_muted(cid, false).await; }
                if has_audio {
                    if !self.published.contains_key(share::AUDIO_TRACK_ID) && !self.pending.contains_key(share::AUDIO_TRACK_ID) {
                        self.publish(share::AUDIO_TRACK_ID, atrack as Arc<dyn TrackLocal + Send + Sync>, false, lk::TrackType::Audio, lk::TrackSource::ScreenShareAudio, "audio da tela", 0, 0).await;
                    } else { self.set_muted(share::AUDIO_TRACK_ID, false).await; }
                }
            }
            Err(e) => self.emit(MeshEvent::Share(json!({ "ev": "share.local", "slot": slot, "state": "off", "reason": e }))).await,
        }
    }

    async fn set_muted(&mut self, cid: &str, muted: bool) {
        if let Some(sid) = self.published.get(cid).map(|p| p.sid.clone()) { self.send(rq::Message::Mute(lk::MuteTrackRequest { sid, muted })).await; }
    }

    async fn share_stop(&mut self, slot: usize) {
        let Some(sh) = self.share[slot].take() else { return };
        sh.sender.stop();
        if let Some(a) = &sh.audio { a.stop(); }
        self.set_muted(share::video_cid(slot), true).await;
        if slot == 0 { self.set_muted(share::AUDIO_TRACK_ID, true).await; }
        let clr = self.data.as_mut().map(|l| l.clear_viewers(slot)).unwrap_or_default();   // parou: ninguém assiste mais
        self.apply(clr).await;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Câmera: faixa própria (`camera-video`, fonte Camera), publicada uma vez e silenciada/reaberta como a da tela.
    // ------------------------------------------------------------------------------------------------------------------
    async fn cam_start(&mut self, device: String, preview: Option<std::path::PathBuf>) {
        self.cam_stop().await;
        self.cam_gen += 1;
        let gen = self.cam_gen;
        let first = !self.published.contains_key(share::CAMERA_TRACK_ID) && !self.pending.contains_key(share::CAMERA_TRACK_ID);
        let vout = self.cam_vout.get_or_insert_with(share::VideoOut::new_camera).clone();
        let cfg = ShareCfg { kind: crate::capture::SourceKind::Camera(device), height: 360, fps: 30, kbps: 0, label: "Câmera".into(), audio: crate::procaudio::AudioMode::None, content: crate::adapt::Content::Motion, codec: Default::default(), adaptive: true };
        // os eventos do emissor chegam como "share.*"; o encaminhador os renomeia para "cam.*" antes de irem ao laço
        let (ctx, mut crx) = mpsc::channel::<Value>(64);
        let sev = self.sev_tx.clone();
        tokio::spawn(async move { while let Some(mut v) = crx.recv().await { if let Some(e) = v["ev"].as_str().map(|e| e.replacen("share.", "cam.", 1)) { v["ev"] = json!(e); } if sev.send(v).await.is_err() { break; } } });
        self.cam_idr.store(true, Ordering::Relaxed);
        match share::start_sender(gen, self.cam_idr.clone(), self.cam_net.clone(), cfg, vout.clone(), ctx, preview, None) {
            Ok(sender) => {
                self.cam = Some(ActiveCam { gen, sender, info: None });
                if first { self.publish(share::CAMERA_TRACK_ID, vout.track() as Arc<dyn TrackLocal + Send + Sync>, true, lk::TrackType::Video, lk::TrackSource::Camera, "câmera", 640, 360).await; }
                else { self.set_muted(share::CAMERA_TRACK_ID, false).await; }
            }
            Err(e) => {
                let (code, text) = crate::camera::split_code(&e);
                self.emit(MeshEvent::Share(json!({ "ev": "cam.local", "state": "off", "code": code, "reason": text }))).await;
            }
        }
    }

    async fn cam_stop(&mut self) {
        let Some(c) = self.cam.take() else { return };
        c.sender.stop();
        self.set_muted(share::CAMERA_TRACK_ID, true).await;
    }

    async fn on_cam_event(&mut self, mut v: Value) {
        if let Some(g) = v["gen"].as_u64() { if self.cam.as_ref().map(|c| c.gen) != Some(g) && !(v["ev"] == "cam.local" && v["state"] == "off" && self.cam.is_none()) { return; } }
        if v["ev"] == "cam.hint" { return; }
        if v["ev"] == "cam.local" {
            if v["state"] == "on" { if let Some(c) = self.cam.as_mut() { c.info = Some(v.clone()); } }
            else {
                let (code, text) = crate::camera::split_code(v["reason"].as_str().unwrap_or(""));
                let (code, text) = (code.to_string(), text.to_string());
                v["code"] = json!(code); v["reason"] = json!(text);
                self.cam_stop().await;
            }
        }
        self.emit(MeshEvent::Share(v)).await;
    }

    async fn watch_cam(&mut self, key: &str, on: bool, path: std::path::PathBuf) {
        let (recv, sids): (Option<Arc<RecvState>>, Vec<(String, String)>) = {
            let sh = self.shared.lock().unwrap();
            let sids = sh.tracks.iter().filter(|(_, t)| t.identity == key && t.source == lk::TrackSource::Camera as i32)
                .map(|(sid, _)| (self.known.get(key).map(|k| k.sid.clone()).unwrap_or_default(), sid.clone())).collect();
            (sh.recv_cam.get(key).cloned(), sids)
        };
        let Some(r) = recv else { self.emit(MeshEvent::Error("câmera: peer desconhecido".into())).await; return };
        if on { *r.sink_path.lock().unwrap() = Some(path.clone()); r.epoch.fetch_add(1, Ordering::Relaxed); r.frames.store(0, Ordering::Relaxed); }
        r.watching.store(on, Ordering::Relaxed);
        let todo: Vec<(String, String)> = sids.into_iter().filter(|(_, s)| if on { self.subscribed.insert(s.clone()) } else { self.subscribed.remove(s) }).collect();
        self.subscribe(todo, on).await;
        self.emit(MeshEvent::Share(json!({ "ev": "cam.remote", "peer_id": key, "state": if on { "watch" } else { "unwatch" }, "path": path.to_string_lossy() }))).await;
    }

    async fn on_share_event(&mut self, mut v: Value) {
        if v["ev"].as_str().map(|e| e.starts_with("cam.")).unwrap_or(false) { return self.on_cam_event(v).await; }
        let slot = match v["gen"].as_u64() { Some(g) if g == self.share_gens[1] && self.share_gens[1] != 0 => 1, Some(_) => 0, None => v["slot"].as_u64().unwrap_or(0).min(1) as usize };
        if let Some(g) = v["gen"].as_u64() {
            if self.share[slot].as_ref().map(|s| s.gen) != Some(g) && !(v["ev"] == "share.local" && v["state"] == "off" && self.share[slot].is_none()) { return; }
            v["slot"] = json!(slot);
        }
        if v["ev"] == "share.local" {
            if v["state"] == "on" {
                if let Some(sh) = self.share[slot].as_mut() { v["audio"] = json!(sh.audio.is_some()); sh.info = Some(v.clone()); }
            } else { self.share_stop(slot).await; }
        }
        self.emit(MeshEvent::Share(v)).await;
    }
}

/// Intervalo mínimo (ms) entre duas retransmissões do MESMO pacote para o SFU. Normal: 1,5 x RTT + 15 ms (30 a 150). Com perda alta
/// no enlace de envio (`high_loss` = retransmissão em dobro ligada), a própria retransmissão se perde com frequência e o servidor
/// pede de novo logo em seguida, poucas vezes, e desiste: o intervalo cai para RTT + 5 ms (10 a 150) para o 2º e o 3º pedido não
/// serem recusados por "acabou de sair". O custo é alguma retransmissão repetida, dentro do mesmo orçamento do pacer.
fn rtx_gap_ms(rtt_ms: f32, high_loss: bool) -> u64 {
    if high_loss { (rtt_ms as u64 + 5).clamp(10, 150) } else { ((rtt_ms * 1.5) as u64 + 15).clamp(30, 150) }
}

/// Nome da faixa de tela no SFU: o rótulo, com o prefixo que marca a 2ª tela.
fn track_name(label: &str, slot: usize) -> String {
    let base = if label.is_empty() { "tela" } else { label };
    if slot == 1 { format!("{}{}", share::SLOT2_NAME, base) } else { base.to_string() }
}

/// Lê o RTCP que o SFU manda ao publisher de vídeo: PLI/FIR (quadro-chave), NACK (retransmissão pelo pacer) e Receiver Reports
/// (perda/jitter/RTT para o adapt.rs). É a mesma leitura da malha, com a chave "sfu" no lugar do peer.
fn spawn_rtcp_reader(sender: Arc<RTCRtpSender>, net: Arc<Mutex<crate::adapt::NetShared>>, idr: Arc<AtomicBool>, nack_tx: Option<(mpsc::Sender<Vec<u16>>, Arc<AtomicU64>, Arc<AtomicBool>)>, stats: Arc<SfuStats>) {
    tokio::spawn(async move {
        use webrtc::rtcp::receiver_report::ReceiverReport;
        use webrtc::rtcp::transport_feedbacks::transport_layer_nack::TransportLayerNack;
        let ssrc = sender.get_parameters().await.encodings.first().map(|e| e.ssrc).unwrap_or(0);
        // O LiveKit pede cada pacote perdido poucas vezes, em intervalos curtos, e desiste: com os 150 ms da malha a 2ª retransmissão
        // (quando a 1ª também se perde) era sempre recusada e o quadro morria. Aqui o intervalo acompanha o RTT medido (30..150 ms).
        if let Some((_, gap, _)) = &nack_tx { gap.store(40, Ordering::Relaxed); }
        while let Ok((pkts, _)) = sender.read_rtcp().await {
            for pk in pkts {
                let any = pk.as_any();
                let mut n = net.lock().unwrap();
                let e = n.peers.entry("sfu".into()).or_insert(crate::adapt::PeerNet { loss: 0.0, rtt_ms: 0.0, jitter_ms: 0.0, updated: std::time::Instant::now(), nacks: 0, plis: 0, rtt_min: 0.0 });
                if any.downcast_ref::<PictureLossIndication>().is_some() || any.downcast_ref::<FullIntraRequest>().is_some() {
                    e.plis += 1; stats.plis.fetch_add(1, Ordering::Relaxed); idr.store(true, Ordering::Relaxed);
                } else if let Some(rr) = any.downcast_ref::<ReceiverReport>() {
                    for r in rr.reports.iter().filter(|r| r.ssrc == ssrc) {
                        e.loss = r.fraction_lost as f32 / 256.0;
                        // histerese: liga a retransmissão em dobro com 3,5% no relatório, desliga abaixo de 1,5% (o relatório do SFU mostrou cerca de
                        // metade da perda real do enlace nos testes: 5% relatados com 10% reais)
                        if let Some((_, gap, twice)) = &nack_tx {
                            let was = twice.load(Ordering::Relaxed);
                            if e.loss >= 0.035 { twice.store(true, Ordering::Relaxed); } else if e.loss < 0.015 { twice.store(false, Ordering::Relaxed); }
                            // a perda mudou de faixa: o intervalo entre retransmissões acompanha já, sem esperar o próximo RTT medido
                            let now = twice.load(Ordering::Relaxed);
                            if now != was { gap.store(rtx_gap_ms(e.rtt_ms, now), Ordering::Relaxed); }
                        }
                        e.jitter_ms = r.jitter as f32 / 90.0;
                        if r.last_sender_report != 0 {
                            let now = (webrtc::rtp::extension::abs_send_time_extension::unix2ntp(std::time::SystemTime::now()) >> 16) as u32;
                            let d = now.wrapping_sub(r.last_sender_report).wrapping_sub(r.delay);
                            if d < 10 * 65536 {
                                e.rtt_ms = d as f32 * 1000.0 / 65536.0; if e.rtt_min == 0.0 || e.rtt_ms < e.rtt_min { e.rtt_min = e.rtt_ms.max(1.0); }
                                if let Some((_, gap, twice)) = &nack_tx { gap.store(rtx_gap_ms(e.rtt_ms, twice.load(Ordering::Relaxed)), Ordering::Relaxed); }
                            }
                        }
                        e.updated = std::time::Instant::now();
                        stats.rr.fetch_add(1, Ordering::Relaxed); stats.rtt_us.store((e.rtt_ms * 1000.0) as u64, Ordering::Relaxed); stats.loss_permille.store((e.loss * 1000.0) as u64, Ordering::Relaxed);
                    }
                } else if let Some(nk) = any.downcast_ref::<TransportLayerNack>() {
                    if nk.media_ssrc == ssrc {
                        let mut seqs = vec![];
                        for p in &nk.nacks { seqs.push(p.packet_id); for b in 0..16 { if p.lost_packets & (1 << b) != 0 { seqs.push(p.packet_id.wrapping_add(b + 1)); } } }
                        seqs.truncate(512);
                        e.nacks += seqs.len() as u64; stats.nacks.fetch_add(seqs.len() as u64, Ordering::Relaxed);
                        if let Some((t, _, _)) = &nack_tx { let _ = t.try_send(seqs); }
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
    fn retransmission_gap_follows_rtt_and_shrinks_under_high_loss() {
        assert_eq!((rtx_gap_ms(1.0, false), rtx_gap_ms(40.0, false), rtx_gap_ms(400.0, false)), (30, 75, 150));
        assert_eq!((rtx_gap_ms(1.0, true), rtx_gap_ms(40.0, true), rtx_gap_ms(400.0, true)), (10, 45, 150));
        for rtt in [0.0f32, 5.0, 20.0, 60.0, 120.0] { assert!(rtx_gap_ms(rtt, true) <= rtx_gap_ms(rtt, false)); }
    }

    #[test]
    fn url_building() {
        assert_eq!(ws_url("ws://127.0.0.1:7880/", "T"), "ws://127.0.0.1:7880/rtc?access_token=T&protocol=15&auto_subscribe=0&sdk=rust&version=briks-0.1&adaptive_stream=0");
        assert!(ws_url("https://x.livekit.cloud", "T").starts_with("wss://x.livekit.cloud/rtc?"));
    }
}

/// Eventos "off" para quando alguém sai: um por tela no ar (slots 0 e 1) e um para a câmera, nessa ordem.
pub(crate) fn gone_events(identity: &str, share: &[bool; 2], cam: bool) -> Vec<Value> {
    let mut v: Vec<Value> = (0..2usize).filter(|s| share[*s]).map(|s| json!({ "ev": "share.remote", "peer_id": identity, "slot": s, "state": "off", "reason": "saiu" })).collect();
    if cam { v.push(json!({ "ev": "cam.remote", "peer_id": identity, "state": "off", "reason": "saiu" })); }
    v
}

#[cfg(test)]
mod gone_tests {
    use super::*;
    #[test]
    fn leaving_ends_every_stream_the_person_had() {
        let v = gone_events("ana", &[true, true], true);
        assert_eq!(v.len(), 3);
        assert!(v[0]["ev"] == "share.remote" && v[0]["slot"] == 0 && v[0]["state"] == "off");
        assert!(v[1]["slot"] == 1 && v[2]["ev"] == "cam.remote" && v[2]["state"] == "off");
        assert_eq!(gone_events("bia", &[false, true], false).len(), 1);
        assert!(gone_events("cris", &[false, false], false).is_empty(), "quem não transmitia não gera evento");
    }
}

#[cfg(test)]
mod clock_tests {
    #[test]
    fn http_date_vira_epoch() {
        assert_eq!(super::parse_http_date("Thu, 01 Jan 1970 00:00:10 GMT"), Some(10));
        assert_eq!(super::parse_http_date("Thu, 12 Nov 2026 21:30:42 GMT"), Some(1_794_519_042));
        assert_eq!(super::parse_http_date("lixo"), None);
    }
}
