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
    /// O chat privado com `from_fp` foi fechado (`on` = false) ou voltou (`on` = true) sem mexer na chamada.
    DmLink { fp: String, on: bool },
    /// Mensagem antiga recebida na sincronização de histórico (assinatura do autor já verificada); `mine` = eu escrevi (em outro aparelho/sessão).
    History { channel: String, from_fp: String, author: String, content: String, id: String, ts: u64, mine: bool },
    /// Evento de chat assinado (reação, edição, exclusão, resposta, fixação) já verificado; `local` = criado por mim.
    ChatEvent { from_fp: String, wire: Value, local: bool },
    /// Anexo: `kind` = offer | progress | done | cancelled | failed (campos no JSON). Nada é aberto; o arquivo recebido fica em quarentena.
    File(Value),
    /// Painel de sons: evento já pronto para a UI (`sound.play`, `sound.got`, `sound.fetching`, `sound.fail`, `sound.drop`...), ver `sonsnet.rs`.
    Sound(Value),
    /// Confirmação de entrega de uma mensagem de DM que enviei.
    Ack { id: String, from_fp: String },
    /// A mensagem de DM saiu pelo canal direto até `to_fp` (ainda não confirmada: o `Ack` vem depois). Uma vez por id.
    Sent { id: String, to_fp: String },
    /// O peer está digitando.
    Typing { from_fp: String },
    /// pacote estruturado de um convite de grupo recebido (vem logo depois do `Chat` com o texto curto)
    Invite { channel: String, from_fp: String, id: String, package: String },
    /// Mensagem do chat da sala (aba Chat da chamada): vale só durante a chamada, de um peer autenticado.
    RoomChat { peer_id: String, from_fp: String, author: String, content: String, id: String, ts: u64 },
    /// Um peer autenticado pediu para eu transmitir a tela.
    StreamRequest { peer_id: String, name: String },
    /// Resposta ao meu pedido de transmissão (só chega se eu pedi há pouco).
    StreamReply { peer_id: String, name: String, accepted: bool },
    /// Snapshot de grupo assinado recebido de um peer autenticado (estrutura e assinatura conferidas; a autorização é de `group.verify`).
    GroupSnapshot { from_fp: String, snapshot: Value },
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
    /// O dono fecha/abre o microfone de outro (mensagem assinada). Só no modo SFU (camada de dados do LiveKit).
    RemoteMute { fp: String, muted: bool },
    /// Reenvia o estado local (microfone/ensurdecido) a todos os peers autenticados.
    BroadcastState,
    /// Só no modo híbrido: estado de mídia (mic aberto, ensurdecido) e identidade no SFU a anunciar; reenvia o estado.
    SetHybrid { mic: bool, deaf: bool, sfu_id: String },
    /// Status (texto do balão) e presença (programa em uso) que os peers autenticados veem; vazio = nada.
    SetProfile { status: String, presence: String },
    /// Silencia (ganho zero) um peer por peer_id ou impressão digital.
    MutePeer { key: String, muted: bool },
    /// só testes: muda os codecs de vídeo que este nó anuncia decodificar (simula um peer sem VP8/VP9)
    #[cfg(test)]
    TestVcodecs(Vec<&'static str>),
    PeerVolume { key: String, gain: f32 },
    AudioCfg(crate::audio::AudioCfg),
    /// Envia texto para a identidade `to_fp` (impressão digital), por DM. Fica na outbox até haver DataChannel autenticado.
    Chat { to_fp: String, text: String },
    /// como `Chat`, mas com o id da mensagem escolhido pela UI (o `chat.ack` volta com o mesmo id: estado "entregue")
    ChatWithId { to_fp: String, text: String, id: String },
    /// DM com um pacote estruturado (convite de grupo) além do texto curto
    ChatInvite { to_fp: String, text: String, id: String, package: String },
    /// Assina e envia um evento de chat (`chat.event` v3) a todos os peers autenticados da sala.
    /// Envia um arquivo (caminho local) a todos os peers autenticados, ou só a `to_fp`.
    SendFile { path: String, to_fp: Option<String> },
    /// Cancela uma transferência (minha ou recebida) pelo id.
    CancelFile { id: String },
    /// Aceita uma oferta recebida (começa o download).
    AcceptFile { id: String },
    /// Pausa ou retoma uma transferência (minha ou recebida).
    PauseFile { id: String, paused: bool },
    /// AMI-16 "Desconectar P2P": fecha só o chat privado com `fp` (a chamada e a amizade ficam). O chat volta sozinho quando
    /// eu escrever para essa pessoa ou ela escrever para mim.
    DmDisconnect { fp: String },
    /// Avisa os peers autenticados que estou digitando.
    Typing,
    /// Chat da sala: texto para todos os peers autenticados (não fica gravado).
    RoomChat { id: String, text: String },
    /// Painel de sons: dispara um som que tenho (assinado, para toda a sala) / pré-busca o arquivo de um som do catálogo.
    SoundPlay { hash: String },
    SoundFetch { hash: String },
    /// Pede a um peer (peer_id ou impressão digital) que transmita a tela.
    StreamRequest { key: String },
    /// Responde a quem pediu a minha transmissão.
    StreamReply { key: String, accepted: bool },
    Event { channel: String, target: String, action: String, content: Option<String>, reference: Option<String>, emoji: Option<String> },
    /// Envia um snapshot de grupo (já assinado) a todos os peers autenticados da sala.
    GroupSend { snapshot: Value },
    /// Só para testes: envia JSON cru a todos os peers autenticados (para provar rejeições).
    #[cfg(test)]
    RawDc { text: String },
    /// Só para testes: põe uma mensagem (já assinada) no histórico daqui, sem enviá-la, e pede o histórico a todos os peers autenticados.
    #[cfg(test)]
    HistSeed { msgs: Vec<ChatWire> },
    #[cfg(test)]
    HistAsk,
    /// Só para testes: força um offer agora (colisão).
    ForceOffer { peer_id: String },
    /// Começa a transmitir (faixa de vídeo para todos os peers autenticados, com renegociação).
    ShareStart { cfg: ShareCfg, preview: Option<std::path::PathBuf>, dump: Option<std::path::PathBuf> },
    ShareStop,
    /// Liga a câmera (faixa de vídeo própria, `camera-video`, ao lado da transmissão de tela). `device` vazio = a primeira.
    CamStart { device: String, preview: Option<std::path::PathBuf> },
    CamStop,
    /// Liga/desliga a decodificação da câmera de um peer e define o arquivo de quadro compartilhado.
    CamWatch { key: String, on: bool, path: std::path::PathBuf },
    /// 2ª tela simultânea (segundo monitor): mesmos campos, slot 1. Sem áudio próprio (o áudio da transmissão é o do slot 0).
    Share2Start { cfg: ShareCfg, preview: Option<std::path::PathBuf>, dump: Option<std::path::PathBuf> },
    Share2Stop,
    Share2Watch { key: String, on: bool, path: std::path::PathBuf },
    /// Volume (0..2) do áudio da transmissão de um peer, só para mim.
    ShareVolume { key: String, gain: f32 },
    /// Troca os processos capturados durante a transmissão ("Escolher apps"); só vale se ela começou com áudio.
    ShareAudio { mode: crate::procaudio::AudioMode },
    /// "Com áudio" / "Sem áudio" ao vivo da transmissão (a faixa de áudio continua aberta)
    ShareAudioMute { muted: bool },
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
    /// refaz a conexão de um par cuja conexão WebRTC falhou (depois de uma queda longa de rede)
    Reconnect { peer: String },
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
    vsender: [Option<Arc<RTCRtpSender>>; 2],   // por slot: 0 = 1ª tela, 1 = 2ª tela (segundo monitor)
    csender: Option<Arc<RTCRtpSender>>,   // faixa da câmera já ligada a este peer
    asender: bool,   // faixa de áudio da transmissão já ligada a este peer
    reneg_pending: bool,   // pediram (ou preciso de) uma nova offer, mas havia uma em andamento: sai quando a resposta chegar
    recv_v2: bool,         // vaga de recepção extra para a 2ª tela deste peer
    recv_slots: bool,      // já criei as vagas de recepção (áudio + vídeo) para a transmissão deste peer
    recv: [Arc<RecvState>; 2],   // recepção por slot
    recv_cam: Arc<RecvState>,   // vídeo da câmera DESTE peer (a transmissão de tela dele é `recv`)
    vcodecs: Vec<String>,   // codecs de vídeo que este peer decodifica (anunciados no peer.state; sem anúncio = só h264)
    caps_known: bool,       // já recebi o peer.state dele (antes disso não ligo VP8/VP9 a ele)
}

struct ActiveCam { gen: u64, sender: share::Sender, info: Option<Value> }

struct ActiveShare { audio: Option<crate::procaudio::ProcAudio>, has_audio: bool, gen: u64, track: Arc<webrtc::track::track_local::track_local_static_rtp::TrackLocalStaticRTP>, sender: share::Sender, info: Option<Value>, launch: (ShareCfg, Option<std::path::PathBuf>, Option<std::path::PathBuf>) }

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

pub(crate) fn now_ms() -> i64 {
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
    sent_notified: HashSet<String>,            // ids de DM cujo "saiu" já foi avisado à UI
    dm_off: HashSet<String>,                   // fps cujo chat privado o usuário desconectou (a chamada segue)
    /// quem o usuário está assistindo (id do par → arquivo de quadros): sobrevive à recriação da conexão do par
    watch_intent: HashMap<(String, usize), std::path::PathBuf>,
    /// 1.4.4: quem está assistindo a MINHA transmissão (slot → ids dos pares que avisaram "share.viewing"); só P2P
    viewers: [HashSet<String>; 2],
    /// 1.4.1: quem está transmitindo (tela por slot / câmera) segundo o par: quando o par SAI ou a conexão cai, a UI precisa do "off" (antes ficava a última imagem parada)
    remote_live: HashSet<(String, usize)>,
    cam_live: HashSet<String>,
    hist: crate::history::Store,               // DMs assinadas, enviadas e recebidas, por canal
    hist_reqs: HashMap<String, String>,        // peer -> requestId do pedido de histórico em andamento
    seen_msgs: HashSet<String>,
    sons: crate::sonsnet::Net,
    incoming: HashMap<String, (String, crate::filetransfer::Incoming, u64)>,   // id -> (peer, estado, último progresso emitido)
    outgoing: HashMap<String, Arc<Ctl>>,                                   // id -> controle (aceitar, pausar, cancelar)
    pending: HashMap<String, (String, crate::filetransfer::Offer)>,       // ofertas recebidas aguardando "Baixar": id -> (peer, oferta)
    recv_dir: std::path::PathBuf,
    ev_last_ts: u64,                // relógio monotônico dos meus eventos de chat
    seen_events: HashSet<String>,
    seen_groups: HashSet<String>,
    audio: Option<Arc<Audio>>,
    audio_rx: Option<mpsc::Receiver<AudioEvent>>,
    joined: HashMap<String, u64>,   // peerId -> joinedAt da presença
    banned: HashSet<String>,        // fps expulsos nesta sessão
    profile: (String, String),      // (status, presença) anunciados aos peers autenticados
    /// Modo híbrido (SFU + malha só de dados): estado de mic/ensurdecido e identidade no SFU que ESTA malha anuncia, já que ela não tem `Audio`.
    media: Option<(bool, bool)>,
    sfu_id: String,
    dead: bool,                     // fui expulso: encerra o ator
    blocked: HashSet<String>,       // peerIds de banidos: sinalização deles é ignorada
    share: [Option<ActiveShare>; 2],   // por slot: até duas telas ao mesmo tempo
    share_gens: [u64; 2],              // última geração iniciada em cada slot (eventos atrasados do emissor)
    share_gen: u64,
    net: [Arc<std::sync::Mutex<crate::adapt::NetShared>>; 2],   // perda/RTT/NACK por espectador (RTCP), lidos pelo laço de adaptação
    idr: [Arc<std::sync::atomic::AtomicBool>; 2],   // pedido de quadro-chave (PLI/FIR/novo espectador), único para a sala toda
    vtrack: [Option<share::VideoOut>; 2],
    cam: Option<ActiveCam>,
    cam_gen: u64,
    cam_vout: Option<share::VideoOut>,
    vcodecs: Vec<&'static str>,   // codecs de vídeo que este nó decodifica e anuncia no peer.state
    cam_net: Arc<std::sync::Mutex<crate::adapt::NetShared>>,   // perda/RTT por espectador da CÂMERA: não mistura com a da transmissão de tela
    cam_idr: Arc<std::sync::atomic::AtomicBool>,
    cam_tx: mpsc::Sender<Value>,
    cam_rx: Option<mpsc::Receiver<Value>>,
    cam_watch: HashMap<String, std::path::PathBuf>,
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
        let sons = crate::sonsnet::Net::new(&identity.fingerprint(), &room_hash, crate::sons::store_for(&identity.fingerprint()));
        let (sev_tx, sev_rx) = mpsc::channel::<Value>(256);
        let (cam_tx, cam_rx) = mpsc::channel::<Value>(256);
        Actor { vcodecs: crate::codec::local_caps(), cfg, identity, rt, ev, room_hash, validator, present: HashSet::new(), viewers: [HashSet::new(), HashSet::new()], peers: HashMap::new(), itx, irx: Some(irx), next_gen: 1, outbox: HashMap::new(), sent_notified: HashSet::new(), dm_off: HashSet::new(), watch_intent: HashMap::new(), remote_live: HashSet::new(), cam_live: HashSet::new(), seen_msgs: HashSet::new(), sons, incoming: HashMap::new(), outgoing: HashMap::new(), pending: HashMap::new(), recv_dir: recv_dir(), ev_last_ts: 0, seen_events: HashSet::new(), audio, audio_rx, joined: HashMap::new(), banned: HashSet::new(), profile: (String::new(), String::new()), media: None, sfu_id: String::new(), dead: false, blocked: HashSet::new(), share: [None, None], share_gens: [0, 0], share_gen: 0, net: Default::default(), idr: [Arc::new(std::sync::atomic::AtomicBool::new(true)), Arc::new(std::sync::atomic::AtomicBool::new(true))], vtrack: [None, None], cam: None, cam_gen: 0, cam_vout: None, cam_net: Default::default(), cam_idr: Arc::new(std::sync::atomic::AtomicBool::new(true)), cam_tx, cam_rx: Some(cam_rx), cam_watch: HashMap::new(), atrack: None, sev_tx, sev_rx: Some(sev_rx), seen_groups: HashSet::new(), hist: crate::history::Store::open(hist_dir()), hist_reqs: HashMap::new() }
    }

    async fn run(&mut self, mut rt_events: mpsc::Receiver<RtEvent>, mut cmds: mpsc::Receiver<MeshCmd>) {
        let mut irx = self.irx.take().expect("irx");
        let mut retry = tokio::time::interval(std::time::Duration::from_secs(2));
        let mut arx = self.audio_rx.take();
        let mut srx = self.sev_rx.take().expect("sev_rx");
        let mut crx = self.cam_rx.take().expect("cam_rx");
        if let Some(a) = &self.audio { a.start(); }
        loop {
            tokio::select! {
                ae = async { match arx.as_mut() { Some(r) => r.recv().await, None => std::future::pending().await } } => match ae {
                    Some(AudioEvent::Speaking { peer, level }) => self.emit(MeshEvent::Speaking { peer_id: peer, level }).await,
                    Some(AudioEvent::Error(m)) => self.emit(MeshEvent::Error(format!("áudio: {m}"))).await,
                    Some(AudioEvent::Notice(v)) => self.emit(MeshEvent::Share(v)).await,
                    None => { arx = None; }
                },
                sv = srx.recv() => if let Some(v) = sv { self.on_share_event(v).await },
                cv = crx.recv() => if let Some(v) = cv { self.on_cam_event(v).await },
                _ = retry.tick() => {
                    // mensagens sem ACK são reenviadas (o receptor deduplica por id)
                    let fps: Vec<String> = self.outbox.iter().filter(|(_, l)| !l.is_empty()).map(|(k, _)| k.clone()).collect();   // (flush_outbox relê a fila em disco)
                    for fp in fps { self.flush_outbox(&fp).await; }
                    self.retry_auth().await;
                    let o = self.sons.tick(now_ms() as u64); self.sons_out(o).await;
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
        for s in 0..2 { if let Some(sh) = self.share[s].take() { sh.sender.stop(); if let Some(a) = &sh.audio { a.stop(); } } }
        if let Some(c) = self.cam.take() { c.sender.stop(); }   // fechar a chamada desliga a captura (a luz apaga)
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
                self.joined.remove(&p);
                for n in &self.net { n.lock().unwrap().peers.remove(&p); }
                if let Some(a) = &self.audio { a.remove_peer(&p); }
                self.end_remote_streams(&p, true).await;
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
        RTCConfiguration { ice_servers: crate::netmode::ice_servers(&self.cfg.ice_servers), ..Default::default() }
    }

    async fn new_pc(&mut self, peer_id: &str, gen: u32, recv: [Arc<RecvState>; 2], recv_cam: Arc<RecvState>) -> Result<Arc<RTCPeerConnection>> {
        let mut m = MediaEngine::default();
        m.register_default_codecs()?;
        // Interceptadores: relatórios RTCP, feedback TWCC (lado receptor) e GERADOR de NACK. O respondedor de NACK embutido fica de
        // fora: ele retransmite sem limite nem espaçamento; as retransmissões são nossas, pelo pacer (share::pacer_task).
        use webrtc::rtp_transceiver::rtp_codec::RTPCodecType;
        for parameter in ["", "pli"] { m.register_feedback(webrtc::rtp_transceiver::RTCPFeedback { typ: "nack".into(), parameter: parameter.into() }, RTPCodecType::Video); }
        let mut registry = Registry::new();
        registry.add(Box::new(webrtc::interceptor::nack::generator::Generator::builder().with_interval(std::time::Duration::from_millis(20)).with_log2_size_minus_6(5)));   // intervalo padrão de 100 ms: cada buraco esperava até 100 ms só para ser pedido; janela 2048 (padrão 8192): com perda por congestionamento o gerador repetia pedidos de milhares de pacotes velhos (34 mil por segundo medidos)
        let registry = webrtc::api::interceptor_registry::configure_rtcp_reports(registry);
        let registry = webrtc::api::interceptor_registry::configure_twcc_receiver_only(registry, &mut m)?;
        // janela anti-replay do SRTP: o padrão (64 pacotes) rejeita retransmissões — a 12 Mbps, um pacote de 100 ms atrás já está
        // mais de 100 pacotes para trás. 8192 cobre ~1 s a 50 Mbps e continua barrando replays antigos.
        let mut se = webrtc::api::setting_engine::SettingEngine::default();
        se.set_srtp_replay_protection_window(8192);
        se.set_interface_filter(Box::new(|n: &str| crate::sfu_cfg::iface_allowed(n)));
        // desiste mais cedo de uma conexão sem resposta (padrão: ~30 s para falhar): a malha refaz o par sozinha (`Reconnect`), então
        // depois de uma queda longa a transmissão volta em ~10 s em vez de ~30 s
        se.set_ice_timeouts(Some(std::time::Duration::from_secs(4)), Some(std::time::Duration::from_secs(8)), Some(std::time::Duration::from_secs(2)));
        crate::netmode::apply(&mut se);
        let api = APIBuilder::new().with_media_engine(m).with_interceptor_registry(registry).with_setting_engine(se).build();
        let pc = Arc::new(api.new_peer_connection(self.rtc_config()).await?);
        {
            let audio = self.audio.clone();
            if let Some(a) = &audio { pc.add_track(a.track.clone() as Arc<dyn TrackLocal + Send + Sync>).await?; }
            let (id, sev) = (peer_id.to_string(), self.sev_tx.clone());
            let wk = Arc::downgrade(&pc);
            pc.on_track(Box::new(move |track, rx, _tr| {
                let (audio, id, sev, recv, recv_cam, wk) = (audio.clone(), id.clone(), sev.clone(), recv.clone(), recv_cam.clone(), wk.clone());
                Box::pin(async move {
                    // o RTCP recebido só passa pelos interceptadores (Sender Reports → LSR/DLSR dos nossos Receiver Reports, base do RTT
                    // do outro lado) se alguém o ler
                    let pts = share::payload_codecs(&rx).await;   // payload type → codec negociado (o emissor pode trocar de codec no meio da transmissão)
                    tokio::spawn(async move { while rx.read_rtcp().await.is_ok() {} });
                    use webrtc::rtp_transceiver::rtp_codec::RTPCodecType;
                    if track.kind() == RTPCodecType::Video {
                        let ssrc = track.ssrc();
                        let pli: Arc<dyn Fn() + Send + Sync> = Arc::new(move || {
                            if let Some(pc) = wk.upgrade() { tokio::spawn(async move { let _ = pc.write_rtcp(&[Box::new(PictureLossIndication { sender_ssrc: 0, media_ssrc: ssrc })]).await; }); }
                        });
                        // a faixa da câmera tem id próprio: estado, arquivo de quadros e eventos separados da transmissão de tela
                        if track.id() == share::CAMERA_TRACK_ID { tokio::spawn(share::run_remote_video_as("cam", id, track, recv_cam, sev, pli, pts)); }
                        else {
                            let slot = if track.id() == share::VIDEO_TRACK_ID_2 { 1 } else { 0 };   // 2ª tela do mesmo peer: faixa e recepção próprias
                            tokio::spawn(share::run_remote_video(share::recv_key(&id, slot), track, recv[slot].clone(), sev, pli, pts));
                        }
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
        let recv = [Arc::new(RecvState::default()), Arc::new(RecvState::default())];
        let recv_cam = Arc::new(RecvState::default());
        let pc = self.new_pc(peer_id, gen, recv.clone(), recv_cam.clone()).await?;
        let polite = !is_initiator(&self.cfg.local_peer, peer_id);
        self.peers.insert(peer_id.into(), Peer { pc, gen, polite, making_offer: false, ignore_offer: false, pending_ice: vec![], dc: None, fp: None, jwk: None, my_nonce: String::new(), proven: false, sent_proof: false, name: String::new(), vsender: [None, None], recv_v2: false, csender: None, asender: false, reneg_pending: false, recv_slots: false, recv, recv_cam, vcodecs: vec!["h264".into()], caps_known: false });
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
        if std::env::var("BRIKS_SDP_DEBUG").is_ok() { if let Some(sdp) = payload["sdp"]["sdp"].as_str() {
            let mut sum = vec![]; let mut cur = String::new();
            for l in sdp.lines() { if l.starts_with("m=") { if !cur.is_empty() { sum.push(cur.clone()); } cur = l.split(' ').next().unwrap_or("").to_string(); } else if l.starts_with("a=mid:") { cur += &format!(" {}", &l[2..]); } else if ["a=sendrecv", "a=sendonly", "a=recvonly", "a=inactive"].contains(&l) { cur += &format!(" {}", &l[2..]); } else if l.starts_with("a=msid:") { cur += &format!(" {}", l.split(' ').last().unwrap_or("")); } }
            sum.push(cur);
            eprintln!("[sdp {} -> {}] {kind}: {}", self.cfg.local_peer.get(..4).unwrap_or("?"), target.get(..4).unwrap_or("?"), sum.join(" | "));
        } }
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
                let redo = collision && polite;
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
                // A minha offer foi desfeita pela colisão: o que ela levava (ex.: a faixa de vídeo de uma transmissão iniciada ao mesmo
                // tempo que a do outro) ainda não foi negociado. Reoferece agora que a conexão voltou a ficar estável.
                if redo { self.start_offer(&from, false).await; }
            }
            "webrtc.answer" => {
                let Ok(sdp) = serde_json::from_value::<RTCSessionDescription>(env.payload["sdp"].clone()) else { return };
                let pc = self.peers[&from].pc.clone();
                if pc.signaling_state() != RTCSignalingState::HaveLocalOffer { return; }
                if let Err(e) = pc.set_remote_description(sdp).await { self.emit(MeshEvent::Error(format!("set answer: {e}"))).await; return; }
                self.flush_ice(&from).await;
                if self.peers.get_mut(&from).map(|p| std::mem::take(&mut p.reneg_pending)).unwrap_or(false) { self.start_offer(&from, false).await; }
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

    /// Religa o chat privado com `fp` se estava desconectado (escrever, de qualquer lado, reconecta).
    async fn dm_reconnect(&mut self, fp: &str) {
        if self.dm_off.remove(fp) { self.emit(MeshEvent::DmLink { fp: fp.to_string(), on: true }).await; }
    }

    async fn chat_send(&mut self, to_fp: String, text: String, id: String, package: Option<String>) {
        self.dm_reconnect(&to_fp).await;
        let my_fp = self.identity.fingerprint();
        let m = identity::sign_message(&self.identity, ChatWire {
            version: 2, kind: "chat.message".into(), channel_id: identity::dm_channel(&my_fp, &to_fp), id,
            author_peer_id: my_fp, author: self.cfg.display_name.clone(), content: text, timestamp: now_ms() as u64, signature: String::new(), package: package.filter(|p| p.len() <= identity::MAX_PACKAGE_BYTES) });
        { let mut h = m.clone(); h.package = None; self.hist.add(&h); }   // o histórico sincronizado não carrega o pacote (fica na caixa de saída até a entrega)
        crate::outbox::shared().lock().unwrap().add(&to_fp, &m);   // persistente: sobrevive a reinício e a canal fechado
        self.flush_outbox(&to_fp).await;
    }

    async fn on_cmd(&mut self, c: MeshCmd) {
        match c {
            MeshCmd::ForceOffer { peer_id } => self.start_offer(&peer_id, false).await,
            MeshCmd::DmDisconnect { fp } => { if self.dm_off.insert(fp.clone()) { self.emit(MeshEvent::DmLink { fp, on: false }).await; } }
            MeshCmd::Chat { to_fp, text } => self.chat_send(to_fp, text, uuid::Uuid::new_v4().to_string(), None).await,
            MeshCmd::ChatWithId { to_fp, text, id } => self.chat_send(to_fp, text, id, None).await,
            MeshCmd::ChatInvite { to_fp, text, id, package } => self.chat_send(to_fp, text, id, Some(package)).await,
            MeshCmd::Event { channel, target, action, content, reference, emoji } => {
                let my_fp = self.identity.fingerprint();
                match crate::chat_events::create(&self.identity, &channel, &target, &my_fp, &action, content.as_deref(), reference.as_deref(), emoji.as_deref(), now_ms() as u64, &mut self.ev_last_ts) {
                    Ok(e) => {
                        let wire = crate::chat_events::to_wire(&e);
                        self.seen_events.insert(e.id.clone());
                        let ids: Vec<String> = self.peers.iter().filter(|(_, p)| p.sent_proof && p.proven).map(|(k, _)| k.clone()).collect();
                        for id in ids { self.dc_send(&id, wire.clone()).await; }
                        self.emit(MeshEvent::ChatEvent { from_fp: my_fp, wire, local: true }).await;
                    }
                    Err(er) => self.emit(MeshEvent::Error(format!("evento inválido: {er}"))).await,
                }
            }
            MeshCmd::GroupSend { snapshot } => {
                let wire = json!({ "version": 1, "type": "group.snapshot", "snapshot": snapshot });
                // só quem é membro do grupo recebe (a sala de voz pode ter gente de fora): nome, membros e chaves não vazam
                let members: HashSet<String> = snapshot["members"].as_array().map(|a| a.iter().filter_map(|m| m["fp"].as_str().map(String::from)).collect()).unwrap_or_default();
                let ids: Vec<String> = self.peers.iter().filter(|(_, p)| p.sent_proof && p.proven && p.fp.as_ref().map_or(false, |f| members.contains(f))).map(|(k, _)| k.clone()).collect();
                for id in ids { self.dc_send(&id, wire.clone()).await; }
            }
            #[cfg(test)]
            MeshCmd::HistSeed { msgs } => { for m in msgs { self.hist.add(&m); } }
            #[cfg(test)]
            MeshCmd::HistAsk => {
                let ps: Vec<(String, String)> = self.peers.iter().filter(|(_, p)| p.sent_proof && p.proven).filter_map(|(k, p)| p.fp.clone().map(|f| (k.clone(), f))).collect();
                for (id, fp) in ps { self.request_history(&id, &fp).await; }
            }
            #[cfg(test)]
            MeshCmd::RawDc { text } => {
                let ids: Vec<String> = self.peers.iter().filter(|(_, p)| p.sent_proof && p.proven).map(|(k, _)| k.clone()).collect();
                for id in ids { if let Some(dc) = self.peers.get(&id).and_then(|p| p.dc.clone()) { let _ = dc.send_text(text.clone()).await; } }
            }
            MeshCmd::Typing => {
                let ids: Vec<String> = self.peers.iter().filter(|(_, p)| p.sent_proof && p.proven).map(|(k, _)| k.clone()).collect();
                for id in ids { self.dc_send(&id, json!({ "version": 2, "type": "chat.typing", "fromPeerId": self.cfg.local_peer })).await; }
            }
            MeshCmd::RoomChat { id, text } => {
                let (Some(id), Some(text)) = (crate::roomchat::clean_id(&id), crate::roomchat::clean(&text)) else { return };
                let wire = json!({ "version": 1, "type": "room.chat", "id": id, "content": text, "timestamp": now_ms() });
                for pid in self.authed_ids() { self.dc_send(&pid, wire.clone()).await; }
            }
            MeshCmd::SoundPlay { hash } => {
                match self.sons.play(&self.identity.clone(), &hash, now_ms() as u64) {
                    Ok(o) => self.sons_out(o).await,
                    Err(m) => self.emit(MeshEvent::Sound(json!({ "ev": "sound.denied", "hash": hash, "message": m }))).await,
                }
            }
            MeshCmd::SoundFetch { hash } => {
                let cands: Vec<String> = self.authed_ids().into_iter().take(3).collect();
                let o = self.sons.fetch(&hash, cands, now_ms() as u64); self.sons_out(o).await;
            }
            MeshCmd::StreamRequest { key } | MeshCmd::StreamReply { key, .. } if !self.peers.iter().any(|(k, p)| (k == &key || p.fp.as_deref() == Some(key.as_str())) && p.sent_proof && p.proven) => {
                self.emit(MeshEvent::Error("pedido de transmissão: essa pessoa não está na sala".into())).await;
            }
            MeshCmd::StreamRequest { key } => {
                let id = self.peers.iter().find(|(k, p)| *k == &key || p.fp.as_deref() == Some(key.as_str())).map(|(k, _)| k.clone()).unwrap_or(key);
                crate::roomchat::mark_asked(&self.cfg.local_peer, &id, now_ms() as u64);
                self.dc_send(&id, json!({ "version": 1, "type": "stream.request" })).await;
            }
            MeshCmd::StreamReply { key, accepted } => {
                let id = self.peers.iter().find(|(k, p)| *k == &key || p.fp.as_deref() == Some(key.as_str())).map(|(k, _)| k.clone()).unwrap_or(key);
                self.dc_send(&id, json!({ "version": 1, "type": "stream.reply", "accepted": accepted })).await;
            }
            MeshCmd::SendFile { path, to_fp } => self.send_file(path, to_fp).await,
            MeshCmd::AcceptFile { id } => {
                let Some((peer, off)) = self.pending.remove(&id) else { return };
                let fp = self.peers.get(&peer).and_then(|p| p.fp.clone()).unwrap_or_default();
                match crate::filetransfer::Incoming::start(&self.recv_dir, off.clone()) {
                    Ok(inc) => {
                        self.incoming.insert(id.clone(), (peer.clone(), inc, 0));
                        self.dc_send(&peer, json!({ "version": 1, "type": "file.accept", "id": id })).await;
                        self.emit(MeshEvent::File(json!({ "kind": "accepted", "id": id, "peer_fp": fp, "name": off.name, "size": off.size, "mine": false }))).await;
                    }
                    Err(e) => {
                        self.dc_send(&peer, json!({ "version": 1, "type": "file.cancel", "id": id })).await;
                        self.emit(MeshEvent::File(json!({ "kind": "failed", "id": id, "peer_fp": fp, "error": e.to_string() }))).await;
                    }
                }
            }
            MeshCmd::PauseFile { id, paused } => {
                if let Some(ctl) = self.outgoing.get(&id) { ctl.paused_all.store(paused, std::sync::atomic::Ordering::SeqCst); self.emit(MeshEvent::File(json!({ "kind": if paused { "paused" } else { "resumed" }, "id": id, "mine": true }))).await; }
                if let Some((peer, _, _)) = self.incoming.get(&id) {
                    let peer = peer.clone();
                    self.dc_send(&peer, json!({ "version": 1, "type": if paused { "file.pause" } else { "file.resume" }, "id": id })).await;
                    self.emit(MeshEvent::File(json!({ "kind": if paused { "paused" } else { "resumed" }, "id": id, "mine": false }))).await;
                }
            }
            MeshCmd::CancelFile { id } => {
                if let Some(f) = self.outgoing.get(&id) { f.cancel.store(true, std::sync::atomic::Ordering::SeqCst); }
                if let Some((peer, off)) = self.pending.remove(&id) {   // recusar uma oferta ainda não aceita
                    self.dc_send(&peer, json!({ "version": 1, "type": "file.cancel", "id": id })).await;
                    let fp = self.peers.get(&peer).and_then(|p| p.fp.clone()).unwrap_or_default();
                    self.emit(MeshEvent::File(json!({ "kind": "cancelled", "id": id, "name": off.name, "peer_fp": fp }))).await;
                }
                if let Some((peer, inc, _)) = self.incoming.remove(&id) {
                    let name = inc.offer.name.clone(); inc.abort();
                    self.dc_send(&peer, json!({ "version": 1, "type": "file.cancel", "id": id })).await;
                    let fp = self.peers.get(&peer).and_then(|p| p.fp.clone()).unwrap_or_default();
                    self.emit(MeshEvent::File(json!({ "kind": "cancelled", "id": id, "name": name, "peer_fp": fp }))).await;
                }
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
            MeshCmd::SetHybrid { mic, deaf, sfu_id } => {
                self.media = Some((mic, deaf)); self.sfu_id = sfu_id;
                let ids: Vec<String> = self.peers.iter().filter(|(_, p)| p.sent_proof && p.proven).map(|(k, _)| k.clone()).collect();
                for id in ids { self.send_state(&id).await; }
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
            #[cfg(test)]
            MeshCmd::TestVcodecs(c) => { self.vcodecs = c; for id in self.authed_ids() { self.send_state(&id).await; } }
            MeshCmd::MutePeer { key, muted } => {
                if let Some(a) = &self.audio {
                    let id = self.peers.iter().find(|(k, p)| *k == &key || p.fp.as_deref() == Some(key.as_str())).map(|(k, _)| k.clone()).unwrap_or(key);
                    a.mute_peer(&id, muted);
                }
            }
            MeshCmd::AudioCfg(c) => { if let Some(a) = &self.audio { a.apply_cfg(&c); } }
            MeshCmd::PeerVolume { key, gain } => {
                if let Some(a) = &self.audio {
                    let id = self.peers.iter().find(|(k, p)| *k == &key || p.fp.as_deref() == Some(key.as_str())).map(|(k, _)| k.clone()).unwrap_or(key);
                    a.set_peer_gain(&id, gain);
                }
            }
            MeshCmd::ShareStart { cfg, preview, dump } => self.share_start(cfg, preview, dump, 0).await,
            MeshCmd::ShareStop => self.share_stop(0).await,
            MeshCmd::Share2Start { cfg, preview, dump } => self.share_start(cfg, preview, dump, 1).await,
            MeshCmd::Share2Stop => self.share_stop(1).await,
            MeshCmd::CamStart { device, preview } => self.cam_start(device, preview).await,
            MeshCmd::CamStop => self.cam_stop().await,
            MeshCmd::CamWatch { key, on, path } => {
                let found = self.peers.iter().find(|(k, p)| *k == &key || p.fp.as_deref() == Some(key.as_str())).map(|(k, p)| (k.clone(), p.recv_cam.clone()));
                match found {
                    Some((id, r)) => {
                        use std::sync::atomic::Ordering::Relaxed;
                        if on { self.cam_watch.insert(id.clone(), path.clone()); } else { self.cam_watch.remove(&id); }
                        if on { *r.sink_path.lock().unwrap() = Some(path.clone()); r.epoch.fetch_add(1, Relaxed); r.frames.store(0, Relaxed); }
                        r.watching.store(on, Relaxed);
                        self.emit(MeshEvent::Share(json!({ "ev": "cam.remote", "peer_id": id, "state": if on { "watch" } else { "unwatch" }, "path": path.to_string_lossy() }))).await;
                    }
                    None => self.emit(MeshEvent::Error("câmera: peer desconhecido".into())).await,
                }
            }
            MeshCmd::ShareVolume { key, gain } => {
                if let Some(a) = &self.audio { let id = self.peers.iter().find(|(k, p)| *k == &key || p.fp.as_deref() == Some(key.as_str())).map(|(k, _)| k.clone()).unwrap_or(key); a.set_screen_gain(&id, gain); }
            }
            MeshCmd::ShareAudio { mode } => { if let Some(a) = self.share[0].as_ref().and_then(|s| s.audio.as_ref()) { a.set_mode(mode); } }
            MeshCmd::ShareAudioMute { muted } => { if let Some(a) = self.share[0].as_ref().and_then(|s| s.audio.as_ref()) { a.set_muted(muted); } }
            c @ (MeshCmd::ShareWatch { .. } | MeshCmd::Share2Watch { .. }) => {
                let (key, on, path, slot) = match c { MeshCmd::ShareWatch { key, on, path } => (key, on, path, 0usize), MeshCmd::Share2Watch { key, on, path } => (key, on, path, 1usize), _ => unreachable!() };
                let found = self.peers.iter().find(|(k, p)| *k == &key || p.fp.as_deref() == Some(key.as_str())).map(|(k, p)| (k.clone(), p.recv[slot].clone()));
                match found {
                    Some((id, r)) => {
                        if on { self.watch_intent.insert((id.clone(), slot), path.clone()); } else { self.watch_intent.remove(&(id.clone(), slot)); }
                        if on { *r.sink_path.lock().unwrap() = Some(path.clone()); r.epoch.fetch_add(1, std::sync::atomic::Ordering::Relaxed); r.frames.store(0, std::sync::atomic::Ordering::Relaxed); }
                        r.watching.store(on, std::sync::atomic::Ordering::Relaxed);
                        self.dc_send(&id, json!({ "version": 2, "type": "share.viewing", "slot": slot, "on": on })).await;   // o emissor conta quem assiste
                        self.emit(MeshEvent::Share(json!({ "ev": "share.remote", "peer_id": id, "slot": slot, "state": if on { "watch" } else { "unwatch" }, "path": path.to_string_lossy() }))).await;
                    }
                    None => self.emit(MeshEvent::Error("assistir: peer desconhecido".into())).await,
                }
            }
            MeshCmd::RemoteMute { .. } => self.emit(MeshEvent::Error("mudo remoto só existe no modo SFU".into())).await,
            MeshCmd::Close => {}
        }
    }

    /// Entrega o que o `sonsnet` mandou: mensagens aos pares autenticados e eventos para a UI.
    async fn sons_out(&mut self, outs: Vec<crate::sonsnet::NetOut>) {
        for o in outs {
            match o {
                crate::sonsnet::NetOut::Ev(e) => self.emit(MeshEvent::Sound(e)).await,
                crate::sonsnet::NetOut::Send { to, kind, mut body } => {
                    body["version"] = json!(1); body["type"] = json!(kind);
                    let ids = if to.is_empty() { self.authed_ids() } else { to.into_iter().filter(|k| self.peers.get(k).map(|p| p.sent_proof && p.proven).unwrap_or(false)).collect() };
                    for id in ids { self.dc_send(&id, body.clone()).await; }
                }
            }
        }
    }

    fn authed_ids(&self) -> Vec<String> { self.peers.iter().filter(|(_, p)| p.sent_proof && p.proven).map(|(k, _)| k.clone()).collect() }

    /// Peer autenticado decodifica o codec? (H264 todo mundo; VP8/VP9 só quem anunciou no peer.state)
    fn peer_decodes(&self, id: &str, c: crate::codec::Codec) -> bool { c == crate::codec::Codec::H264 || self.peers.get(id).map(|p| p.vcodecs.iter().any(|x| x == c.name())).unwrap_or(false) }

    async fn share_start(&mut self, mut cfg: ShareCfg, preview: Option<std::path::PathBuf>, dump: Option<std::path::PathBuf>, slot: usize) {
        self.share_stop(slot).await;
        if slot == 1 { cfg.audio = crate::procaudio::AudioMode::None; }   // o áudio da transmissão é um só, no slot 0
        let vout = self.vtrack[slot].get_or_insert_with(|| share::VideoOut::new_slot(slot)).clone();
        // Codec: o pedido, se este binário o tem e TODOS os peers o decodificam; senão H264 (padrão e reserva). A faixa troca de codec sem
        // renegociar (`replace_track`); se algum peer recusar a troca, volta tudo para H264.
        let mut want = if cfg.codec.available() && self.authed_ids().iter().all(|id| self.peer_decodes(id, cfg.codec)) { cfg.codec } else { crate::codec::Codec::H264 };
        if let Some(track) = vout.set_codec(want) {
            let mut failed = false;
            for id in self.authed_ids() {
                if let Some(snd) = self.peers.get(&id).and_then(|p| p.vsender[slot].clone()) {
                    if let Err(e) = snd.replace_track(Some(track.clone() as Arc<dyn TrackLocal + Send + Sync>)).await { eprintln!("[codec] {id}: replace_track {}: {e}", want.name()); failed = true; break; }
                }
            }
            if failed {
                want = crate::codec::Codec::H264;
                if let Some(back) = vout.set_codec(want) {
                    for id in self.authed_ids() { if let Some(snd) = self.peers.get(&id).and_then(|p| p.vsender[slot].clone()) { let _ = snd.replace_track(Some(back.clone() as Arc<dyn TrackLocal + Send + Sync>)).await; } }
                }
            }
        }
        let track = vout.track();
        self.share_gen += 1;
        let gen = self.share_gen;
        self.share_gens[slot] = gen;
        let amode = cfg.audio.clone();
        let atrack = self.atrack.get_or_insert_with(crate::procaudio::new_audio_track).clone();
        let launch = (cfg.clone(), preview.clone(), dump.clone());
        match share::start_sender(gen, self.idr[slot].clone(), self.net[slot].clone(), cfg, vout, self.sev_tx.clone(), preview, dump) {
            Ok(sender) => {
                let has_audio = amode != crate::procaudio::AudioMode::None;
                let audio = if has_audio { Some(crate::procaudio::start(amode, atrack)) } else { None };
                self.share[slot] = Some(ActiveShare { audio, has_audio, gen, track, sender, info: None, launch });
            }
            Err(e) => { self.emit(MeshEvent::Share(json!({ "ev": "share.local", "slot": slot, "state": "off", "reason": e }))).await; return; }
        }
        for id in self.authed_ids() { self.attach_share(&id, slot).await; }
    }

    /// Algum espectador não decodifica o codec em uso: recomeça a transmissão em H264 (mesmo pedido, mesma origem).
    async fn share_fallback_h264(&mut self, slot: usize) {
        let Some(sh) = &self.share[slot] else { return };
        let (mut cfg, preview, dump) = sh.launch.clone();
        if cfg.codec == crate::codec::Codec::H264 && self.vtrack[slot].as_ref().map(|v| v.codec()) == Some(crate::codec::Codec::H264) { return; }
        cfg.codec = crate::codec::Codec::H264;
        eprintln!("[codec] espectador sem {}: voltando a H264", self.vtrack[slot].as_ref().map(|v| v.codec().name()).unwrap_or("?"));
        self.share_start(cfg, preview, dump, slot).await;
    }

    /// Adiciona a faixa de vídeo ao peer (se ainda não tem), lê o RTCP (PLI/FIR) e renegocia.
    async fn attach_share(&mut self, id: &str, slot: usize) {
        let Some(sh) = &self.share[slot] else { return };
        let (track, idr) = (sh.track.clone(), sh.sender.idr.clone());
        let Some(p) = self.peers.get_mut(id) else { return };
        if p.vsender[slot].is_some() {
            idr.store(true, std::sync::atomic::Ordering::Relaxed);
            if let Some(info) = self.share[slot].as_ref().and_then(|s| s.info.clone()) { self.send_share_state(id, &info, slot).await; }
            return;
        }
        if !p.asender { if let Some(at) = self.atrack.clone() { if p.pc.add_track(at as Arc<dyn TrackLocal + Send + Sync>).await.is_ok() { p.asender = true; } } }
        match p.pc.add_track(track as Arc<dyn TrackLocal + Send + Sync>).await {
            Ok(sender) => {
                spawn_video_rtcp(sender.clone(), idr.clone(), self.net[slot].clone(), id.to_string(), self.vtrack[slot].as_ref().map(|v| v.nack_tx.clone()));
                p.vsender[slot] = Some(sender);
                idr.store(true, std::sync::atomic::Ordering::Relaxed);
            }
            Err(e) => { self.emit(MeshEvent::Error(format!("faixa de vídeo: {e}"))).await; return; }
        }
        self.renegotiate(id).await;
        if let Some(info) = self.share[slot].as_ref().and_then(|s| s.info.clone()) { self.send_share_state(id, &info, slot).await; }
    }

    /// Renegociação sem colisão: só o lado iniciador (o de peerId menor) faz offers. O outro lado pede a ele pelo DataChannel
    /// ("share.reneg"); se já há uma offer em andamento, a nova sai quando a resposta chegar. Duas transmissões começando ao mesmo
    /// tempo geravam offers cruzadas, e o desfazer (rollback) do webrtc-rs travava a conexão.
    async fn renegotiate(&mut self, id: &str) {
        let Some(p) = self.peers.get(id) else { return };
        // "vn" = quantas faixas de vídeo eu envio a este peer (tela e/ou câmera): o iniciador cria as vagas de recepção que faltam
        // (conta as vagas de vídeo que EU tenho, não só as faixas que envio: uma faixa nova minha não ocupa uma vaga já associada a uma faixa do outro lado)
        if p.polite {
            use webrtc::rtp_transceiver::rtp_codec::RTPCodecType;
            let vn = p.pc.get_transceivers().await.iter().filter(|t| t.kind() == RTPCodecType::Video).count().max(1);
            self.dc_send(id, json!({ "version": 2, "type": "share.reneg", "vn": vn })).await; return;
        }
        if p.making_offer || p.pc.signaling_state() != RTCSignalingState::Stable { if let Some(p) = self.peers.get_mut(id) { p.reneg_pending = true; } return; }
        self.start_offer(id, false).await;
    }

    /// Para o envio. A faixa continua ligada aos peers (retomar não renegocia); os espectadores são avisados pelo DataChannel.
    async fn share_stop(&mut self, slot: usize) {
        let Some(sh) = self.share[slot].take() else { return };
        sh.sender.stop();
        if let Some(a) = &sh.audio { a.stop(); }
        for id in self.authed_ids() { self.dc_send(&id, json!({ "version": 2, "type": "share.state", "slot": slot, "on": false })).await; }
        if !self.viewers[slot].is_empty() { self.viewers[slot].clear(); self.emit_viewers(slot).await; }   // parou: ninguém assiste mais
    }

    async fn send_share_state(&self, peer: &str, info: &Value, slot: usize) {
        let mut m = json!({ "version": 2, "type": "share.state", "slot": slot, "on": true });
        for k in ["w", "h", "fps", "kbps", "label", "audio"] { if let Some(v) = info.get(k) { m[k] = v.clone(); } }
        self.dc_send(peer, m).await;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Câmera: segunda faixa de vídeo por peer (a transmissão de tela é a primeira). Mesmo codificador/pacer/adaptação da tela, com
    // 360p/30 como teto; o RTCP dos espectadores da câmera alimenta uma `NetShared` própria.
    // ------------------------------------------------------------------------------------------------------------------
    async fn cam_start(&mut self, device: String, preview: Option<std::path::PathBuf>) {
        self.cam_stop().await;
        let vout = self.cam_vout.get_or_insert_with(share::VideoOut::new_camera).clone();
        self.cam_gen += 1;
        let gen = self.cam_gen;
        let cfg = ShareCfg { kind: crate::capture::SourceKind::Camera(device), height: 360, fps: 30, kbps: 0, label: "Câmera".into(), audio: crate::procaudio::AudioMode::None, content: crate::adapt::Content::Motion, codec: Default::default(), adaptive: true };
        self.cam_idr.store(true, std::sync::atomic::Ordering::Relaxed);
        match share::start_sender(gen, self.cam_idr.clone(), self.cam_net.clone(), cfg, vout, self.cam_tx.clone(), preview, None) {
            Ok(sender) => { self.cam = Some(ActiveCam { gen, sender, info: None }); }
            Err(e) => {
                let (code, text) = crate::camera::split_code(&e);
                self.emit(MeshEvent::Share(json!({ "ev": "cam.local", "state": "off", "code": code, "reason": text }))).await;
                return;
            }
        }
        for id in self.authed_ids() { self.attach_cam(&id).await; }
    }

    /// Liga a faixa da câmera ao peer (se ainda não tem), lê o RTCP e renegocia.
    async fn attach_cam(&mut self, id: &str) {
        let Some(c) = &self.cam else { return };
        let idr = c.sender.idr.clone();
        let Some(vout) = self.cam_vout.clone() else { return };
        let Some(p) = self.peers.get_mut(id) else { return };
        if p.csender.is_some() {
            idr.store(true, std::sync::atomic::Ordering::Relaxed);
            if let Some(info) = self.cam.as_ref().and_then(|c| c.info.clone()) { self.send_cam_state(id, &info).await; }
            return;
        }
        match p.pc.add_track(vout.track() as Arc<dyn TrackLocal + Send + Sync>).await {
            Ok(sender) => {
                spawn_video_rtcp(sender.clone(), idr.clone(), self.cam_net.clone(), id.to_string(), Some(vout.nack_tx.clone()));
                p.csender = Some(sender);
                idr.store(true, std::sync::atomic::Ordering::Relaxed);
            }
            Err(e) => { self.emit(MeshEvent::Error(format!("faixa da câmera: {e}"))).await; return; }
        }
        self.renegotiate(id).await;
        if let Some(info) = self.cam.as_ref().and_then(|c| c.info.clone()) { self.send_cam_state(id, &info).await; }
    }

    /// Para a captura e o envio (a luz apaga). A faixa continua ligada aos peers: ligar de novo não renegocia.
    async fn cam_stop(&mut self) {
        let Some(c) = self.cam.take() else { return };
        c.sender.stop();
        for id in self.authed_ids() { self.dc_send(&id, json!({ "version": 2, "type": "cam.state", "on": false })).await; }
    }

    async fn send_cam_state(&self, peer: &str, info: &Value) {
        let mut m = json!({ "version": 2, "type": "cam.state", "on": true });
        for k in ["w", "h", "fps", "kbps"] { if let Some(v) = info.get(k) { m[k] = v.clone(); } }
        self.dc_send(peer, m).await;
    }

    /// Eventos do emissor da câmera (thread de captura/codificação): viram "cam.*" para a UI.
    async fn on_cam_event(&mut self, mut v: Value) {
        if let Some(g) = v["gen"].as_u64() { if self.cam.as_ref().map(|c| c.gen) != Some(g) && !(v["ev"] == "share.local" && v["state"] == "off" && self.cam.is_none()) { return; } }
        let Some(ev) = v["ev"].as_str().map(|e| e.replacen("share.", "cam.", 1)) else { return };
        v["ev"] = json!(ev);
        if ev == "cam.hint" { return; }   // "fonte preta" é um aviso da tela; a câmera tapada simplesmente aparece escura
        if ev == "cam.local" {
            if v["state"] == "on" {
                if let Some(c) = self.cam.as_mut() { c.info = Some(v.clone()); }
                for id in self.authed_ids() { self.send_cam_state(&id, &v).await; }
            } else {
                let (code, text) = crate::camera::split_code(v["reason"].as_str().unwrap_or(""));
                let (code, text) = (code.to_string(), text.to_string());
                v["code"] = json!(code); v["reason"] = json!(text);
                self.cam_stop().await;
            }
        }
        self.emit(MeshEvent::Share(v)).await;
    }

    /// Eventos do emissor (thread de captura/codificação) e do receptor.
    async fn on_share_event(&mut self, mut v: Value) {
        // eventos de um emissor antigo (já parado) chegam atrasados: ignora
        // de qual tela é o evento: pela geração do emissor (os do receptor já chegam com "slot")
        let slot = match v["gen"].as_u64() { Some(g) if g == self.share_gens[1] && self.share_gens[1] != 0 => 1, Some(_) => 0, None => v["slot"].as_u64().unwrap_or(0).min(1) as usize };
        if let Some(g) = v["gen"].as_u64() {
            if self.share[slot].as_ref().map(|s| s.gen) != Some(g) && !(v["ev"] == "share.local" && v["state"] == "off" && self.share[slot].is_none()) { return; }
            v["slot"] = json!(slot);
        }
        if let Some(sh) = &self.share[slot] {
            if v["ev"] == "share.local" && v["state"] == "on" { v["audio"] = json!(sh.has_audio); }
            if v["ev"] == "share.stats" { if let Some(a) = &sh.audio {
                use std::sync::atomic::Ordering::Relaxed;
                v["audio_peak"] = json!(a.stats.peak_milli.load(Relaxed) as f64 / 1000.0); v["audio_sources"] = json!(a.stats.sources.load(Relaxed)); v["audio_frames"] = json!(a.stats.frames.load(Relaxed));
            } }
        }
        if v["ev"] == "share.local" {
            if v["state"] == "on" {
                if let Some(sh) = self.share[slot].as_mut() { sh.info = Some(v.clone()); }
                for id in self.authed_ids() { self.send_share_state(&id, &v, slot).await; }
            } else { self.share_stop(slot).await; }
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

    /// A pessoa saiu (ou a conexão com ela caiu): o que ela transmitia (tela nos dois slots e câmera) acabou. Avisa a UI com "off" para
    /// tirar o bloco e a última imagem (antes ficava a última imagem parada). `forget`: saiu de vez, esquece também o "continuar assistindo".
    async fn emit_viewers(&mut self, slot: usize) {
        let mut ids: Vec<String> = self.viewers[slot].iter().cloned().collect(); ids.sort();
        self.emit(MeshEvent::Share(json!({ "ev": "share.viewers", "slot": slot, "count": ids.len(), "peers": ids }))).await;
    }

    async fn end_remote_streams(&mut self, peer: &str, forget: bool) {
        for slot in 0..2usize { if self.viewers[slot].remove(peer) { self.emit_viewers(slot).await; } }   // quem saiu/caiu deixa de contar
        for slot in 0..2usize {
            if self.remote_live.remove(&(peer.to_string(), slot)) {
                if let Some(p) = self.peers.get(peer) { p.recv[slot].watching.store(false, std::sync::atomic::Ordering::Relaxed); }
                self.emit(MeshEvent::Share(json!({ "ev": "share.remote", "peer_id": peer, "slot": slot, "state": "off", "reason": "saiu" }))).await;
            }
            if forget { self.watch_intent.remove(&(peer.to_string(), slot)); }
        }
        if self.cam_live.remove(peer) {
            if let Some(p) = self.peers.get(peer) { p.recv_cam.watching.store(false, std::sync::atomic::Ordering::Relaxed); }
            self.emit(MeshEvent::Share(json!({ "ev": "cam.remote", "peer_id": peer, "state": "off", "reason": "saiu" }))).await;
        }
        if forget { self.cam_watch.remove(peer); }
    }

    async fn drop_peer(&mut self, id: &str) {
        self.end_remote_streams(id, true).await;
        for n in &self.net { n.lock().unwrap().peers.remove(id); }
        if let Some(a) = &self.audio { a.remove_peer(id); }
        self.pending.retain(|_, (pe, _)| pe != id);
        let gone: Vec<String> = self.incoming.iter().filter(|(_, (pe, _, _))| pe == id).map(|(k, _)| k.clone()).collect();
        for k in gone { if let Some((_, inc, _)) = self.incoming.remove(&k) { inc.abort(); self.emit(MeshEvent::File(json!({ "kind": "failed", "id": k, "error": "conexão encerrada" }))).await; } }
        if let Some(p) = self.peers.remove(id) { let _ = p.pc.close().await; self.emit(MeshEvent::Peer { peer_id: id.into(), state: "fechado".into() }).await; }
    }

    async fn send_file(&mut self, path: String, to_fp: Option<String>) {
        use crate::filetransfer as ft;
        let p2 = std::path::PathBuf::from(&path);
        let desc = tokio::task::spawn_blocking(move || ft::describe(&p2)).await;
        let (name, size, sha, mime) = match desc {
            Ok(Ok(d)) => d,
            Ok(Err(e)) => { self.emit(MeshEvent::File(json!({ "kind": "failed", "id": "", "name": path, "error": e.to_string() }))).await; return; }
            Err(_) => return,
        };
        let targets: Vec<(Arc<RTCDataChannel>, String)> = self.peers.values()
            .filter(|p| p.proven && p.sent_proof && p.dc.is_some() && (to_fp.is_none() || p.fp == to_fp))
            .map(|p| (p.dc.clone().unwrap(), p.fp.clone().unwrap_or_default())).collect();
        if targets.is_empty() { self.emit(MeshEvent::File(json!({ "kind": "failed", "id": "", "name": name, "error": "ninguém conectado para receber" }))).await; return; }
        let id = uuid::Uuid::new_v4().to_string();
        let offer = ft::Offer::sign(&self.identity, ft::Offer { id: id.clone(), name: name.clone(), size, mime: mime.clone(), sha256: sha, from_peer: self.identity.fingerprint(), timestamp: now_ms(), signature: String::new() });
        let ctl = Arc::new(Ctl::default());
        self.outgoing.insert(id.clone(), ctl.clone());
        self.emit(MeshEvent::File(json!({ "kind": "offer", "id": id, "name": name, "size": size, "mime": mime, "mine": true, "path": path }))).await;
        let wire = offer.to_wire();
        for (dc, fp) in targets {
            let (wire, ctl, ev, path, id) = (wire.clone(), ctl.clone(), self.ev.clone(), path.clone(), id.clone());
            tokio::spawn(async move {
                use base64::Engine;
                use std::sync::atomic::Ordering::SeqCst;
                let fail = |why: &str| MeshEvent::File(json!({ "kind": "failed", "id": id, "peer_fp": fp, "error": why }));
                if dc.send_text(wire.to_string()).await.is_err() { let _ = ev.send(fail("falha ao enviar a oferta")).await; return; }
                // espera o "Baixar" do outro lado (até 15 min); cancelar antes disso encerra
                let waited = std::time::Instant::now();
                loop {
                    if ctl.cancel.load(SeqCst) { let _ = dc.send_text(json!({ "version": 1, "type": "file.cancel", "id": id }).to_string()).await; let _ = ev.send(MeshEvent::File(json!({ "kind": "cancelled", "id": id, "peer_fp": fp }))).await; return; }
                    if ctl.accepted.lock().unwrap().contains(&fp) { break; }
                    if waited.elapsed() > std::time::Duration::from_secs(15 * 60) { let _ = ev.send(fail("ninguém aceitou a tempo")).await; return; }
                    tokio::time::sleep(std::time::Duration::from_millis(50)).await;
                }
                let Ok(mut f) = tokio::fs::File::open(&path).await else { let _ = ev.send(fail("não consegui abrir o arquivo")).await; return };
                let mut buf = vec![0u8; ft::CHUNK_BYTES];
                let (mut seq, mut sent, mut last) = (0u64, 0u64, 0u64);
                loop {
                    while (ctl.paused_all.load(SeqCst) || ctl.paused.lock().unwrap().contains(&fp)) && !ctl.cancel.load(SeqCst) { tokio::time::sleep(std::time::Duration::from_millis(100)).await; }
                    if ctl.cancel.load(SeqCst) {
                        let _ = dc.send_text(json!({ "version": 1, "type": "file.cancel", "id": id }).to_string()).await;
                        let _ = ev.send(MeshEvent::File(json!({ "kind": "cancelled", "id": id, "peer_fp": fp }))).await;
                        return;
                    }
                    let mut n = 0;
                    while n < buf.len() {
                        match tokio::io::AsyncReadExt::read(&mut f, &mut buf[n..]).await { Ok(0) => break, Ok(k) => n += k, Err(_) => { let _ = ev.send(fail("erro de leitura")).await; return; } }
                    }
                    if n == 0 { break; }
                    while dc.buffered_amount().await > 1024 * 1024 { if ctl.cancel.load(SeqCst) { break; } tokio::time::sleep(std::time::Duration::from_millis(10)).await; }
                    let msg = json!({ "version": 1, "type": "file.chunk", "id": id, "seq": seq, "data": base64::engine::general_purpose::STANDARD.encode(&buf[..n]) });
                    if dc.send_text(msg.to_string()).await.is_err() { let _ = ev.send(fail("conexão perdida")).await; return; }
                    seq += 1; sent += n as u64;
                    if sent - last >= 256 * 1024 || sent == size { last = sent; let _ = ev.send(MeshEvent::File(json!({ "kind": "progress", "id": id, "peer_fp": fp, "done": sent, "size": size, "mine": true }))).await; }
                }
                let _ = dc.send_text(json!({ "version": 1, "type": "file.done", "id": id }).to_string()).await;
                let _ = ev.send(MeshEvent::File(json!({ "kind": "done", "id": id, "peer_fp": fp, "size": size, "mine": true }))).await;
            });
        }
    }

    async fn on_file_msg(&mut self, peer: &str, v: &Value) {
        use crate::filetransfer as ft;
        use base64::Engine;
        let Some(p) = self.peers.get(peer) else { return };
        let (true, Some(jwk), Some(fp)) = (p.proven && p.sent_proof, p.jwk.clone(), p.fp.clone()) else { return };   // sem identidade provada, ignora
        let id = v["id"].as_str().unwrap_or("").to_string();
        let failed = |why: String| MeshEvent::File(json!({ "kind": "failed", "id": id, "peer_fp": fp, "error": why }));
        match v["type"].as_str().unwrap_or("") {
            "file.offer" => {
                let offer = match ft::Offer::from_wire(v).and_then(|o| o.verify(&jwk, &fp, now_ms()).map(|_| o)) {
                    Ok(o) => o,
                    Err(e) => { self.emit(MeshEvent::Error(format!("anexo rejeitado: {e}"))).await; return; }
                };
                if self.incoming.contains_key(&offer.id) || self.incoming.values().filter(|(pe, _, _)| pe == peer).count() >= ft::MAX_INCOMING_PER_PEER {
                    self.dc_send(peer, json!({ "version": 1, "type": "file.cancel", "id": offer.id })).await;
                    return;
                }
                if self.pending.contains_key(&offer.id) || self.pending.values().filter(|(pe, _)| pe == peer).count() >= 8 {
                    self.dc_send(peer, json!({ "version": 1, "type": "file.cancel", "id": offer.id })).await; return;
                }
                // nada é gravado até o usuário clicar em "Baixar" (AcceptFile)
                let name = ft::sanitize_name(&offer.name);
                self.emit(MeshEvent::File(json!({ "kind": "offer", "id": offer.id, "peer_fp": fp, "name": name, "size": offer.size, "mime": offer.mime, "mine": false }))).await;
                self.pending.insert(offer.id.clone(), (peer.to_string(), offer));
            }
            "file.chunk" => {
                let Some(data) = v["data"].as_str().and_then(|d| base64::engine::general_purpose::STANDARD.decode(d).ok()) else { return };
                let Some(seq) = v["seq"].as_u64() else { return };
                let Some((pe, inc, last)) = self.incoming.get_mut(&id) else { return };
                if pe != peer { return; }
                match inc.chunk(seq, &data) {
                    Ok(()) => {
                        if inc.got - *last >= 256 * 1024 || inc.got == inc.offer.size {
                            *last = inc.got;
                            let (got, size) = (inc.got, inc.offer.size);
                            self.emit(MeshEvent::File(json!({ "kind": "progress", "id": id, "peer_fp": fp, "done": got, "size": size, "mine": false }))).await;
                        }
                    }
                    Err(e) => {
                        if let Some((_, inc, _)) = self.incoming.remove(&id) { inc.abort(); }
                        self.dc_send(peer, json!({ "version": 1, "type": "file.cancel", "id": id })).await;
                        self.emit(failed(e.to_string())).await;
                    }
                }
            }
            "file.done" => {
                let Some((pe, inc, last)) = self.incoming.remove(&id) else { return };
                if pe != peer { self.incoming.insert(id, (pe, inc, last)); return; }
                let (name, mime, size) = (inc.offer.name.clone(), inc.offer.mime.clone(), inc.offer.size);
                match inc.finish() {
                    Ok(path) => {
                        let thumb = if ft::is_thumbnailable(&mime) { std::fs::read(&path).ok().and_then(|b| crate::linkpreview::thumbnail(&b).ok()) } else { None };
                        let th = thumb.map(|(w, h, b)| json!({ "w": w, "h": h, "rgba": b })).unwrap_or(Value::Null);
                        self.emit(MeshEvent::File(json!({ "kind": "done", "id": id, "peer_fp": fp, "name": ft::sanitize_name(&name), "size": size, "mime": mime, "mine": false, "path": path.to_string_lossy(), "thumb": th }))).await;
                    }
                    Err(e) => self.emit(failed(e.to_string())).await,
                }
            }
            "file.accept" | "file.pause" | "file.resume" => {
                let Some(ctl) = self.outgoing.get(&id).cloned() else { return };
                let t = v["type"].as_str().unwrap_or("");
                if t == "file.accept" { ctl.accepted.lock().unwrap().insert(fp.clone()); }
                else if t == "file.pause" { ctl.paused.lock().unwrap().insert(fp.clone()); }
                else { ctl.paused.lock().unwrap().remove(&fp); }
                self.emit(MeshEvent::File(json!({ "kind": match t { "file.accept" => "accepted", "file.pause" => "paused", _ => "resumed" }, "id": id, "peer_fp": fp, "mine": true }))).await;
            }
            "file.cancel" => {
                if self.pending.get(&id).is_some_and(|(pe, _)| pe == peer) { self.pending.remove(&id); self.emit(MeshEvent::File(json!({ "kind": "cancelled", "id": id, "peer_fp": fp }))).await; }
                if let Some(f) = self.outgoing.get(&id) { f.cancel.store(true, std::sync::atomic::Ordering::SeqCst); }
                if let Some((pe, inc, last)) = self.incoming.remove(&id) {
                    if pe == peer { inc.abort(); self.emit(MeshEvent::File(json!({ "kind": "cancelled", "id": id, "peer_fp": fp }))).await; }
                    else { self.incoming.insert(id, (pe, inc, last)); }
                }
            }
            _ => {}
        }
    }

    async fn send_state(&self, peer: &str) {
        let (mic, deaf) = self.audio.as_ref().map(|a| (a.mic_open(), a.deaf())).or(self.media).unwrap_or((true, false));
        self.dc_send(peer, json!({ "version": 2, "type": "peer.state", "microphone": mic, "deafened": deaf, "status": self.profile.0, "presence": self.profile.1, "sfuId": self.sfu_id, "vcodecs": self.vcodecs })).await;
    }

    /// Pede ao peer recém-autenticado as mensagens desta DM que eu ainda não tenho (informa os ids que já tenho).
    async fn request_history(&mut self, peer: &str, fp: &str) {
        let ch = identity::dm_channel(&self.identity.fingerprint(), fp);
        let rid = uuid::Uuid::new_v4().to_string();
        self.hist_reqs.insert(peer.to_string(), rid.clone());
        self.dc_send(peer, json!({ "version": 2, "type": "chat.history.request", "channelId": ch, "requestId": rid, "knownIds": self.hist.known_ids(&ch) })).await;
    }

    async fn on_history_request(&mut self, peer: &str, v: &Value) {
        let Some(p) = self.peers.get(peer) else { return };
        let (true, Some(fp)) = (p.proven && p.sent_proof, p.fp.clone()) else { return };
        let ch = identity::dm_channel(&self.identity.fingerprint(), &fp);
        if v["channelId"].as_str() != Some(&ch) { return; }   // só a DM entre nós dois; nunca o histórico de outro canal
        let Some(rid) = v["requestId"].as_str().filter(|r| !r.is_empty() && r.len() <= 64) else { return };
        let known: HashSet<String> = v["knownIds"].as_array().map(|a| a.iter().filter_map(|x| x.as_str()).take(crate::history::MAX_KNOWN).map(String::from).collect()).unwrap_or_default();
        for chunk in crate::history::chunks(self.hist.missing(&ch, &known)) {
            self.dc_send(peer, json!({ "version": 2, "type": "chat.history.chunk", "channelId": ch, "requestId": rid, "messages": chunk })).await;
        }
        self.dc_send(peer, json!({ "version": 2, "type": "chat.history.complete", "channelId": ch, "requestId": rid })).await;
    }

    async fn on_history_chunk(&mut self, peer: &str, v: &Value) {
        let Some(p) = self.peers.get(peer) else { return };
        let (true, Some(jwk), Some(fp)) = (p.proven && p.sent_proof, p.jwk.clone(), p.fp.clone()) else { return };
        if v["requestId"].as_str() != self.hist_reqs.get(peer).map(String::as_str) { return; }   // só o que eu pedi, deste peer
        let me = self.identity.fingerprint();
        let ch = identity::dm_channel(&me, &fp);
        if v["channelId"].as_str() != Some(&ch) { return; }
        let Some(arr) = v["messages"].as_array() else { return };
        let my_jwk = self.identity.public_jwk();
        for raw in arr.iter().take(crate::history::MAX_CHUNK_MESSAGES) {
            let Ok(m) = serde_json::from_value::<ChatWire>(raw.clone()) else { continue };
            if self.hist.has(&m.id) { continue; }
            // o autor tem de ser um dos dois lados desta DM, e a assinatura tem de ser dele (o peer não pode forjar o que "eu" disse)
            let mine = m.author_peer_id == me;
            let key = if mine { &my_jwk } else if m.author_peer_id == fp { &jwk } else { continue };
            if identity::verify_message(key, &m, &ch).is_err() { continue; }
            if self.hist.add(&m) {
                self.emit(MeshEvent::History { channel: m.channel_id, from_fp: m.author_peer_id, author: m.author, content: m.content, id: m.id, ts: m.timestamp, mine }).await;
            }
        }
    }

    async fn flush_outbox(&mut self, fp: &str) {
        if self.dm_off.contains(fp) { return; }
        let pend = crate::outbox::shared().lock().unwrap().for_fp(fp);   // a fila em disco é a fonte da verdade (várias malhas, reinícios)
        if pend.is_empty() { self.outbox.remove(fp); return; }
        let list = self.outbox.entry(fp.to_string()).or_default();
        *list = pend;
        let list = &*list;
        let Some(peer) = self.peers.values().find(|p| p.fp.as_deref() == Some(fp) && p.sent_proof && p.proven) else { return };
        let Some(dc) = peer.dc.clone() else { return };
        let mut fresh: Vec<String> = Vec::new();
        for m in list { if let Ok(t) = serde_json::to_string(m) { if dc.send_text(t).await.is_ok() && !self.sent_notified.contains(&m.id) { fresh.push(m.id.clone()); } } }
        for id in fresh { self.sent_notified.insert(id.clone()); self.emit(MeshEvent::Sent { id: id.chars().take(64).collect(), to_fp: fp.to_string() }).await; }
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
                self.emit(MeshEvent::Peer { peer_id: peer.clone(), state: s.into() }).await;
                // Conexão FALHA (o ICE desiste depois de ~30 s sem resposta): sem isto o par ficava morto para sempre mesmo com a rede de
                // volta e os dois ainda na sala. Fecha, espera um pouco e refaz do zero (o lado iniciador oferece, o outro aceita a
                // oferta na própria `on_signal`); a transmissão em curso é anexada de novo na reautenticação (`after_auth`).
                if state == RTCPeerConnectionState::Failed {
                    self.end_remote_streams(&peer, false).await;   // a conexão voltará (Reconnect) e a transmissão reaparece com "on"; até lá nada de imagem parada
                    for n in &self.net { n.lock().unwrap().peers.remove(&peer); }
                    if let Some(a) = &self.audio { a.remove_peer(&peer); }
                    if let Some(p) = self.peers.remove(&peer) { let _ = p.pc.close().await; }
                    let (itx, id) = (self.itx.clone(), peer);
                    tokio::spawn(async move { tokio::time::sleep(std::time::Duration::from_millis(1500)).await; let _ = itx.send(Internal::Reconnect { peer: id }).await; });
                }
            }
            Internal::Reconnect { peer } => {
                if !self.present.contains(&peer) || self.blocked.contains(&peer) || self.peers.contains_key(&peer) || self.peers.len() >= MAX_PEERS { return; }
                if let Err(e) = self.add_peer(&peer).await { self.emit(MeshEvent::Error(format!("reconexão com {peer}: {e}"))).await; return; }
                if is_initiator(&self.cfg.local_peer, &peer) { self.start_offer(&peer, true).await; }
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
                self.dm_reconnect(&fp).await;   // a pessoa escreveu: o chat privado reconecta
                self.dc_send(peer, json!({ "version": 2, "type": "chat.message.ack", "channelId": m.channel_id, "messageId": m.id })).await;
                { let mut h = m.clone(); h.package = None; self.hist.add(&h); }
                if self.seen_msgs.len() > 8192 { self.seen_msgs.clear(); }   // dedupe só precisa cobrir as reenvios recentes; sem teto crescia para sempre
                if self.seen_msgs.insert(m.id.clone()) {
                    let inv = m.package.clone().filter(|p| p.len() <= identity::MAX_PACKAGE_BYTES).map(|p| (m.channel_id.clone(), fp.clone(), m.id.clone(), p));
                    self.emit(MeshEvent::Chat { channel: m.channel_id, from_fp: fp, author: m.author, content: m.content, id: m.id, ts: m.timestamp }).await;
                    if let Some((channel, from_fp, id, package)) = inv { self.emit(MeshEvent::Invite { channel, from_fp, id, package }).await; }
                }
            }
            "chat.history.request" | "chat.history.chunk" if self.peers.get(peer).and_then(|p| p.fp.as_ref()).is_some_and(|f| self.dm_off.contains(f)) => {}   // chat desconectado: sem sincronizar
            "chat.history.request" => self.on_history_request(peer, &v).await,
            "chat.history.chunk" => self.on_history_chunk(peer, &v).await,
            "chat.history.complete" => {
                if v["requestId"].as_str() == self.hist_reqs.get(peer).map(String::as_str) { self.hist_reqs.remove(peer); }
            }
            "chat.message.ack" => {
                let Some(p) = self.peers.get(peer) else { return };
                let (true, Some(fp)) = (p.proven && p.sent_proof, p.fp.clone()) else { return };
                if let Some(id) = v["messageId"].as_str() {
                    if let Some(l) = self.outbox.get_mut(&fp) { l.retain(|m| m.id != id); }   // entregue: sai da outbox
                    crate::outbox::shared().lock().unwrap().ack(id);
                    self.emit(MeshEvent::Ack { id: id.chars().take(64).collect(), from_fp: fp }).await;
                }
            }
            "chat.typing" => {
                let Some(p) = self.peers.get(peer) else { return };
                let (true, Some(fp)) = (p.proven && p.sent_proof, p.fp.clone()) else { return };
                if self.dm_off.contains(&fp) { return; }
                self.emit(MeshEvent::Typing { from_fp: fp }).await;
            }
            "room.chat" => {
                // chat da sala: só de peer autenticado, com limite de frequência; o autor é a identidade provada do canal (nada do que vem no JSON)
                let Some(p) = self.peers.get(peer) else { return };
                let (true, Some(fp)) = (p.proven && p.sent_proof, p.fp.clone()) else { return };
                let author = p.name.clone();
                let (Some(id), Some(content)) = (v["id"].as_str().and_then(crate::roomchat::clean_id), v["content"].as_str().and_then(crate::roomchat::clean)) else { return };
                let now = now_ms() as u64;
                if !crate::roomchat::allow_chat(&self.cfg.local_peer, peer, now) { return; }
                if self.seen_msgs.len() > 8192 { self.seen_msgs.clear(); }
                if self.seen_msgs.insert(format!("room:{id}")) {
                    self.emit(MeshEvent::RoomChat { peer_id: peer.into(), from_fp: fp, author, content, id, ts: now }).await;
                }
            }
            "sound.play" | "sound.want" | "sound.data" | "sound.none" => {
                // painel de sons: só de par autenticado; o autor é a identidade provada do canal (sonsnet.rs confere a assinatura do disparo)
                let Some(p) = self.peers.get(peer) else { return };
                let (true, Some(jwk), Some(fp)) = (p.proven && p.sent_proof, p.jwk.clone(), p.fp.clone()) else { return };
                let kind = v["type"].as_str().unwrap_or("").to_string();
                let o = self.sons.on_msg(&crate::sonsnet::PeerRef { key: peer, fp: &fp, jwk: &jwk }, &kind, &v, now_ms() as u64);
                self.sons_out(o).await;
            }
            "stream.request" | "stream.reply" => {
                let Some(p) = self.peers.get(peer) else { return };
                if !(p.proven && p.sent_proof) { return; }
                let name = p.name.clone();
                let now = now_ms() as u64;
                if v["type"] == "stream.request" {
                    if crate::roomchat::allow_request(&self.cfg.local_peer, peer, now) { self.emit(MeshEvent::StreamRequest { peer_id: peer.into(), name }).await; }
                } else if crate::roomchat::take_asked(&self.cfg.local_peer, peer, now) {
                    self.emit(MeshEvent::StreamReply { peer_id: peer.into(), name, accepted: v["accepted"].as_bool().unwrap_or(false) }).await;
                }
            }
            "file.offer" | "file.chunk" | "file.done" | "file.cancel" | "file.accept" | "file.pause" | "file.resume" => self.on_file_msg(peer, &v).await,
            "chat.event" => {
                let Some(p) = self.peers.get(peer) else { return };
                let (true, Some(jwk), Some(fp)) = (p.proven && p.sent_proof, p.jwk.clone(), p.fp.clone()) else { return };
                match crate::chat_events::parse_wire(text) {
                    crate::chat_events::Parsed::Ignored(_) => {}   // versão/ação desconhecida: compatibilidade
                    crate::chat_events::Parsed::Invalid(why) => self.emit(MeshEvent::Error(format!("evento rejeitado: {why}"))).await,
                    crate::chat_events::Parsed::Event(e) => {
                        let ch = e.channel_id.clone();
                        if e.actor_peer_id != fp || crate::chat_events::verify_event(&jwk, &e, &ch, now_ms() as u64).is_err() {
                            self.emit(MeshEvent::Error("evento rejeitado (autor/assinatura)".into())).await; return;
                        }
                        if self.seen_events.insert(e.id.clone()) {
                            self.emit(MeshEvent::ChatEvent { from_fp: fp, wire: crate::chat_events::to_wire(&e), local: false }).await;
                        }
                    }
                }
            }
            "group.snapshot" => {
                let Some(p) = self.peers.get(peer) else { return };
                let (true, Some(fp)) = (p.proven && p.sent_proof, p.fp.clone()) else { return };
                let Ok(s) = serde_json::from_value::<crate::group_sync::Snapshot>(v["snapshot"].clone()) else { return };   // formato desconhecido: ignora
                if let Err(why) = crate::group_sync::verify_wire(&s, &fp) { self.emit(MeshEvent::Error(format!("grupo rejeitado: {why}"))).await; return; }
                if self.seen_groups.len() > 1024 { self.seen_groups.clear(); }
                if self.seen_groups.insert(s.signature.clone()) {
                    self.emit(MeshEvent::GroupSnapshot { from_fp: fp, snapshot: v["snapshot"].clone() }).await;
                }
            }
            "room.kick" | "room.allow" => {
                let kick = v["type"] == "room.kick";
                let Some(p) = self.peers.get(peer) else { return };
                let (true, Some(jwk), Some(from_fp)) = (p.proven && p.sent_proof, p.jwk.clone(), p.fp.clone()) else { return };
                let (Some(target), Some(sig), Some(ts)) = (v["target_fp"].as_str(), v["signature"].as_str(), v["timestamp"].as_i64()) else { return };
                if self.owner().as_deref() != Some(from_fp.as_str()) { self.emit(MeshEvent::Error("comando de sala ignorado: remetente não é o dono".into())).await; return; }
                if now_ms().saturating_sub(ts).saturating_abs() > 5 * 60 * 1000 { return; }   // antigo demais (replay)
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
                // codecs de vídeo que o peer decodifica (sem o campo = versão antiga = só H264)
                let caps: Vec<String> = v["vcodecs"].as_array().map(|a| a.iter().filter_map(|x| x.as_str()).filter(|x| ["h264", "vp8", "vp9"].contains(x)).map(String::from).collect()).filter(|a: &Vec<String>| !a.is_empty()).unwrap_or_else(|| vec!["h264".into()]);
                let changed = self.peers.get(peer).map(|p| !p.caps_known || p.vcodecs != caps).unwrap_or(false);
                if let Some(p) = self.peers.get_mut(peer) { p.vcodecs = caps; p.caps_known = true; }
                if changed {
                    for s in 0..2 {
                        if self.share[s].is_none() { continue; }
                        let c = self.vtrack[s].as_ref().map(|v| v.codec()).unwrap_or_default();
                        if c == crate::codec::Codec::H264 { continue; }   // H264 já foi ligado em after_auth
                        if self.peer_decodes(peer, c) { self.attach_share(peer, s).await; } else { self.share_fallback_h264(s).await; }
                    }
                }
                // identidade do SFU anunciada por este peer (autenticado pela prova ECDSA): liga o peer da malha ao participante do SFU
                if let Some(sid) = v["sfuId"].as_str().filter(|x| !x.is_empty() && x.len() <= 128) { self.emit(MeshEvent::Share(json!({ "ev": "sfu.link", "peer_id": peer, "identity": sid }))).await; }
                self.emit(MeshEvent::PeerState { peer_id: peer.into(), microphone: v["microphone"].as_bool().unwrap_or(true), deafened: v["deafened"].as_bool().unwrap_or(false), status: v["status"].as_str().unwrap_or("").chars().take(64).collect(), presence: v["presence"].as_str().unwrap_or("").chars().take(64).collect() }).await;
            }
            "share.reneg" => {
                // o outro lado ligou faixas novas e precisa que eu (o iniciador) faça a offer. Sem faixa minha para aquele tipo de
                // mídia, crio vagas só de recepção para as dele caberem na offer.
                if self.peers.get(peer).map(|p| p.proven && p.sent_proof && !p.polite) != Some(true) { return; }
                let vn = v["vn"].as_u64().unwrap_or(1).clamp(1, 6) as usize;
                let (pc, need) = { let p = &self.peers[peer]; (p.pc.clone(), p.vsender.iter().all(|v| v.is_none()) && !p.recv_slots) };
                use webrtc::rtp_transceiver::rtp_codec::RTPCodecType;
                use webrtc::rtp_transceiver::rtp_transceiver_direction::RTCRtpTransceiverDirection;
                use webrtc::rtp_transceiver::RTCRtpTransceiverInit;
                let recvonly = || Some(RTCRtpTransceiverInit { direction: RTCRtpTransceiverDirection::Recvonly, send_encodings: vec![] });
                // vagas de vídeo: tantas quanto as faixas que o outro lado vai enviar (a minha própria faixa, se houver, já conta: é bidirecional)
                let have = pc.get_transceivers().await.iter().filter(|t| t.kind() == RTPCodecType::Video).count();
                for _ in have..vn { let _ = pc.add_transceiver_from_kind(RTPCodecType::Video, recvonly()).await; }
                if need {
                    let _ = pc.add_transceiver_from_kind(RTPCodecType::Audio, recvonly()).await;
                    if let Some(p) = self.peers.get_mut(peer) { p.recv_slots = true; }
                }
                self.renegotiate(peer).await;
            }
            "cam.state" => {
                if self.peers.get(peer).map(|p| p.proven && p.sent_proof) != Some(true) { return; }
                use std::sync::atomic::Ordering::Relaxed;
                let on = v["on"].as_bool().unwrap_or(false);
                if on { self.cam_live.insert(peer.to_string()); } else { self.cam_live.remove(peer); }
                let mut m = json!({ "ev": "cam.remote", "peer_id": peer, "state": if on { "on" } else { "off" } });
                for k in ["w", "h", "fps", "kbps"] { m[k] = json!(v[k].as_u64().unwrap_or(0)); }
                if !on { self.cam_watch.remove(peer); if let Some(p) = self.peers.get(peer) { p.recv_cam.watching.store(false, Relaxed); } }
                self.emit(MeshEvent::Share(m)).await;
                // conexão refeita com quem tem a câmera ligada e a UI estava mostrando: volta a decodificar sozinho
                if on {
                    if let (Some(path), Some(p)) = (self.cam_watch.get(peer).cloned(), self.peers.get(peer)) {
                        if !p.recv_cam.watching.load(Relaxed) {
                            *p.recv_cam.sink_path.lock().unwrap() = Some(path.clone());
                            p.recv_cam.epoch.fetch_add(1, Relaxed);
                            p.recv_cam.frames.store(0, Relaxed);
                            p.recv_cam.watching.store(true, Relaxed);
                            self.emit(MeshEvent::Share(json!({ "ev": "cam.remote", "peer_id": peer, "state": "watch", "path": path.to_string_lossy() }))).await;
                        }
                    }
                }
            }
            "share.viewing" => {   // um par começou/parou de assistir à minha tela: atualiza a contagem (nunca conta a mim mesmo: só chega de outros pares)
                if self.peers.get(peer).map(|p| p.proven && p.sent_proof) != Some(true) { return; }
                let slot = v["slot"].as_u64().unwrap_or(0).min(1) as usize;
                let changed = if v["on"].as_bool().unwrap_or(false) { self.viewers[slot].insert(peer.to_string()) } else { self.viewers[slot].remove(peer) };
                if changed { self.emit_viewers(slot).await; }
            }
            "share.state" => {
                if self.peers.get(peer).map(|p| p.proven && p.sent_proof) != Some(true) { return; }
                let on = v["on"].as_bool().unwrap_or(false);
                let slot = v["slot"].as_u64().unwrap_or(0).min(1) as usize;
                if on { self.remote_live.insert((peer.to_string(), slot)); } else { self.remote_live.remove(&(peer.to_string(), slot)); }
                let mut m = json!({ "ev": "share.remote", "peer_id": peer, "slot": slot, "state": if on { "on" } else { "off" } });
                for k in ["w", "h", "fps", "kbps"] { m[k] = json!(v[k].as_u64().unwrap_or(0)); }
                m["label"] = json!(v["label"].as_str().unwrap_or("").chars().take(64).collect::<String>());
                m["audio"] = json!(v["audio"].as_bool().unwrap_or(false));
                if !on { self.watch_intent.remove(&(peer.to_string(), slot)); if let Some(p) = self.peers.get(peer) { p.recv[slot].watching.store(false, std::sync::atomic::Ordering::Relaxed); } }
                self.emit(MeshEvent::Share(m)).await;
                // a conexão com o emissor foi refeita (queda longa) e o usuário estava assistindo: retoma sozinho, sem ele precisar fazer nada
                if on {
                    if let (Some(path), Some(p)) = (self.watch_intent.get(&(peer.to_string(), slot)).cloned(), self.peers.get(peer)) {
                        let r = &p.recv[slot];
                        if !r.watching.load(std::sync::atomic::Ordering::Relaxed) {
                            *r.sink_path.lock().unwrap() = Some(path.clone());
                            r.epoch.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                            r.frames.store(0, std::sync::atomic::Ordering::Relaxed);
                            r.watching.store(true, std::sync::atomic::Ordering::Relaxed);
                            self.dc_send(peer, json!({ "version": 2, "type": "share.viewing", "slot": slot, "on": true })).await;
                            self.emit(MeshEvent::Share(json!({ "ev": "share.remote", "peer_id": peer, "slot": slot, "state": "watch", "path": path.to_string_lossy() }))).await;
                        }
                    }
                }
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
            if let Some(fp) = fp { if !self.dm_off.contains(&fp) { self.flush_outbox(&fp).await; self.request_history(peer, &fp).await; } }
            self.send_state(peer).await;
            for s in 0..2 {
                if self.share[s].is_some() {
                    let c = self.vtrack[s].as_ref().map(|v| v.codec()).unwrap_or_default();
                    if c == crate::codec::Codec::H264 || self.peers.get(peer).map(|p| p.caps_known).unwrap_or(false) { self.attach_share(peer, s).await; }   // VP8/VP9: espera o peer.state dele
                }
            }
            if self.cam.is_some() { self.attach_cam(peer).await; }
        }
    }
}

/// Lê o RTCP que um espectador devolve para uma faixa de vídeo nossa (relatórios de perda/atraso, PLI/FIR, NACK) e alimenta a
/// adaptação (`net`), o pedido de quadro-chave (`idr2`) e as retransmissões (`nack_tx`). Serve à tela e à câmera.
fn spawn_video_rtcp(s2: Arc<RTCRtpSender>, idr2: Arc<std::sync::atomic::AtomicBool>, net: Arc<std::sync::Mutex<crate::adapt::NetShared>>, pid: String, nack_tx: Option<mpsc::Sender<Vec<u16>>>) {
    tokio::spawn(async move {
                use webrtc::rtcp::receiver_report::ReceiverReport;
                use webrtc::rtcp::transport_feedbacks::transport_layer_nack::TransportLayerNack;
                let ssrc = s2.get_parameters().await.encodings.first().map(|e| e.ssrc).unwrap_or(0);
                while let Ok((pkts, _)) = s2.read_rtcp().await {
                    for pk in pkts {
                        let any = pk.as_any();
                        let mut n = net.lock().unwrap();
                        let e = n.peers.entry(pid.clone()).or_insert(crate::adapt::PeerNet { loss: 0.0, rtt_ms: 0.0, jitter_ms: 0.0, updated: std::time::Instant::now(), nacks: 0, plis: 0, rtt_min: 0.0 });
                        if any.downcast_ref::<PictureLossIndication>().is_some() || any.downcast_ref::<FullIntraRequest>().is_some() {
                            e.plis += 1; idr2.store(true, std::sync::atomic::Ordering::Relaxed);
                        } else if let Some(rr) = any.downcast_ref::<ReceiverReport>() {
                            for r in rr.reports.iter().filter(|r| r.ssrc == ssrc) {
                                e.loss = r.fraction_lost as f32 / 256.0;
                                e.jitter_ms = r.jitter as f32 / 90.0;
                                if std::env::var("BRIKS_SHARE_DEBUG").is_ok() { eprintln!("[rr] perda {} jitter {} lsr {:x} dlsr {}", r.fraction_lost, r.jitter, r.last_sender_report, r.delay); }
                                if r.last_sender_report != 0 {
                                    // RTT = agora − LSR − DLSR, em unidades de 1/65536 s (32 bits do meio do NTP)
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
                                seqs.truncate(512);   // um RTCP legítimo não passa de ~1 MTU; o limite evita inundar o pacer
                                e.nacks += seqs.len() as u64;
                                if let Some(t) = &nack_tx { let _ = t.try_send(seqs); }
                            }
                        }
                    }
                }
                net.lock().unwrap().peers.remove(&pid);
            });
}

fn peer_remote(v: &Value) -> &str { v["fromPeerId"].as_str().unwrap_or("") }
/// Controle de um envio: quem aceitou, quem pausou e o cancelamento (compartilhado com as tarefas de envio de cada peer).
#[derive(Default)]
struct Ctl {
    cancel: std::sync::atomic::AtomicBool,
    accepted: std::sync::Mutex<HashSet<String>>,   // fps que aceitaram
    paused: std::sync::Mutex<HashSet<String>>,     // fps que pausaram
    paused_all: std::sync::atomic::AtomicBool,     // eu pausei
}

/// Onde o histórico fica guardado (um .jsonl por DM). Nos testes só em memória, a menos que BRIKS_HIST_DIR peça uma pasta.
pub(crate) fn hist_dir() -> Option<std::path::PathBuf> {
    if let Ok(d) = std::env::var("BRIKS_HIST_DIR") { return Some(d.into()); }
    if cfg!(test) { return None; }
    std::env::var("LOCALAPPDATA").ok().map(|b| std::path::PathBuf::from(b).join("Briks-CUI").join("historico"))
}

pub(crate) fn recv_dir() -> std::path::PathBuf {
    if let Ok(d) = std::env::var("BRIKS_RECV_DIR") { return d.into(); }
    let base = std::env::var("LOCALAPPDATA").map(std::path::PathBuf::from).unwrap_or_else(|_| std::env::temp_dir());
    base.join("Briks-CUI").join("recebidos")
}

fn caps() -> Value { json!({ "protocol": 2, "client": "native-0.1", "capabilities": [] }) }
