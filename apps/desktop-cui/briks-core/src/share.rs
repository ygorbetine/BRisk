//! Transmissão de tela/janela: pipeline do emissor (captura → escala/cor → H264 → faixa WebRTC) e do receptor (RTP → H264 → quadro
//! compartilhado com a UI). O mesh só cria as faixas, renegocia e roteia os eventos; aqui fica o trabalho pesado, em threads próprias.
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use bytes::Bytes;
use serde_json::{json, Value};
use tokio::sync::mpsc;
use webrtc::rtp::codecs::h264::{H264Packet, H264Payloader};
use webrtc::rtp::packetizer::{Depacketizer, Packetizer};
use webrtc::track::track_local::track_local_static_rtp::TrackLocalStaticRTP;
use webrtc::track::track_local::TrackLocalWriter;
use webrtc::rtp_transceiver::rtp_codec::RTCRtpCodecCapability;
use webrtc::track::track_remote::TrackRemote;

use crate::capture::{Capture, SourceKind};
use crate::video::{self, FrameSink, H264Dec, H264Enc};
use crate::codec::Codec;

pub const VIDEO_TRACK_ID: &str = "screen-video";
/// 2ª transmissão simultânea da mesma pessoa (segundo monitor): faixa própria, com pacer, codificador e estimador próprios.
pub const VIDEO_TRACK_ID_2: &str = "screen-video-2";
/// Faixa de vídeo da câmera: um vídeo à parte por peer, ao lado da(s) transmissão(ões) de tela. O receptor distingue pelo id da faixa.
pub const CAMERA_TRACK_ID: &str = "camera-video";
/// Quantas transmissões de tela uma pessoa pode ter ao mesmo tempo.
pub const SHARE_SLOTS: usize = 2;
/// Identificador de quem transmite nos eventos do receptor: a 2ª tela leva o sufixo "#2" (vira `"slot": 1` no evento).
pub fn recv_key(peer: &str, slot: usize) -> String { if slot == 1 { format!("{peer}#2") } else { peer.to_string() } }
/// No SFU a 2ª tela de uma pessoa é outra faixa ScreenShare; o que a distingue é este prefixo no nome da faixa.
pub const SLOT2_NAME: &str = "[2] ";
pub fn slot_of_name(name: &str) -> usize { if name.starts_with(SLOT2_NAME) { 1 } else { 0 } }
pub fn strip_slot_name(name: &str) -> &str { name.strip_prefix(SLOT2_NAME).unwrap_or(name) }
pub fn video_cid(slot: usize) -> &'static str { if slot == 1 { VIDEO_TRACK_ID_2 } else { VIDEO_TRACK_ID } }
fn split_key(k: &str) -> (&str, u64) { match k.strip_suffix("#2") { Some(p) => (p, 1), None => (k, 0) } }
pub const AUDIO_TRACK_ID: &str = "screen-audio";
pub const H264_FMTP: &str = "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f";
pub const MAX_BPS: u32 = 50_000_000;
/// Só testes: transmissões com este rótulo dormem TEST_SLOW_MS antes de codificar cada quadro (codificador lento simulado).
pub const SLOW_LABEL: &str = "__lento__";
pub static TEST_SLOW_MS: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Clone)]
pub struct ShareCfg {
    pub kind: SourceKind,
    /// Altura alvo (720, 1080, 1440, 2160...); a origem nunca é ampliada.
    pub height: usize,
    pub fps: u32,
    /// 0 = automático.
    pub kbps: u32,
    pub label: String,
    pub audio: crate::procaudio::AudioMode,
    /// o que preservar quando falta banda ou CPU
    pub content: crate::adapt::Content,
    /// codec pedido (H264 = padrão). Só vale se todos os espectadores sabem decodificar; senão cai para H264 (ver `codec::negotiate`).
    pub codec: crate::codec::Codec,
    /// false = sem adaptação (bitrate/FPS/resolução fixos, como pedido)
    pub adaptive: bool,
}

/// Faixa de vídeo em RTP cru: o empacotamento H264 e o espaçamento dos pacotes (pacer) são nossos.
pub fn new_video_track() -> Arc<TrackLocalStaticRTP> { new_video_track_slot(0) }
pub fn new_video_track_slot(slot: usize) -> Arc<TrackLocalStaticRTP> { new_video_track_slot_codec(slot, Codec::H264) }
pub fn new_video_track_slot_codec(slot: usize, codec: Codec) -> Arc<TrackLocalStaticRTP> {
    new_video_track_named_codec(if slot == 1 { VIDEO_TRACK_ID_2 } else { VIDEO_TRACK_ID }, if slot == 1 { "briks-screen-2" } else { "briks-screen" }, codec)
}
pub fn new_video_track_named(id: &str, stream: &str) -> Arc<TrackLocalStaticRTP> { new_video_track_named_codec(id, stream, Codec::H264) }
/// fmtp do codec na faixa: H264 como sempre; VP8 sem parâmetros; VP9 perfil 0 (o que o libvpx gera em I420 8 bits).
pub fn video_fmtp(c: Codec) -> &'static str { match c { Codec::H264 => H264_FMTP, Codec::Vp8 => "", Codec::Vp9 => "profile-id=0" } }
pub fn new_video_track_named_codec(id: &str, stream: &str, codec: Codec) -> Arc<TrackLocalStaticRTP> {
    Arc::new(TrackLocalStaticRTP::new(
        RTCRtpCodecCapability { mime_type: codec.mime().into(), clock_rate: 90_000, channels: 0, sdp_fmtp_line: video_fmtp(codec).into(), rtcp_feedback: vec![] },
        id.into(),
        stream.into(),
    ))
}

/// Saída de vídeo de uma sala: a faixa e o pacer, que vivem enquanto a sala existir. Parar e retomar a transmissão reaproveita
/// os dois, para a numeração e o relógio RTP continuarem (um salto quebraria o SRTP do outro lado).
#[derive(Clone)]
pub struct VideoOut {
    /// (codec, faixa) atuais. A faixa é presa a UM codec; trocar de codec cria outra faixa (`set_codec`) e o chamador a coloca nos peers
    /// com `replace_track`. O pacer, a numeração de NACK e os leitores de RTCP continuam os mesmos.
    cur: Arc<Mutex<(Codec, Arc<TrackLocalStaticRTP>)>>,
    ids: (String, String),
    ctl: mpsc::UnboundedSender<(Codec, Arc<TrackLocalStaticRTP>)>,
    tx: mpsc::UnboundedSender<(Vec<u8>, u32, u32)>,
    /// pedidos de retransmissão (números de sequência) vindos do RTCP dos espectadores
    pub nack_tx: mpsc::Sender<Vec<u16>>,
    queued: Arc<AtomicU64>,
    pub rtx_sent: Arc<AtomicU64>,
    pub rtx_skipped: Arc<AtomicU64>,
    /// intervalo mínimo (ms) entre duas retransmissões do MESMO pacote. 150 na malha (o nosso receptor repete o pedido a cada 20 ms
    /// por muito tempo); o SFU baixa para perto do RTT, porque o LiveKit pede poucas vezes e desiste.
    pub rtx_gap_ms: Arc<AtomicU64>,
    /// perda alta no caminho (>= 3,5% no relatório do SFU): cada retransmissão sai em dobro, porque com 10% de perda uma em cada dez
    /// retransmissões também se perde e o LiveKit desiste do pacote depois de poucos pedidos. Só o SFU liga.
    pub rtx_twice: Arc<AtomicBool>,
    /// pacotes pedidos pela PRIMEIRA vez (perda real antes do conserto; pedidos repetidos do mesmo pacote não contam)
    pub nack_unique: Arc<AtomicU64>,
    /// para onde vão os pacotes de paridade (FEC, ver `fec.rs`): o transporte (SFU ou malha) instala; sem destino não há FEC
    fec_sink: Arc<Mutex<Option<FecSink>>>,
    /// pacotes de paridade enviados (telemetria)
    pub fec_sent: Arc<AtomicU64>,
    /// até quando (relógio `mono_ms`) o pacer gera paridade: 10 s depois do último sinal de perda
    fec_until_ms: Arc<AtomicU64>,
}
/// Relógio monotônico do processo em ms (para prazos guardados em atômicos).
pub fn mono_ms() -> u64 { static T0: std::sync::OnceLock<Instant> = std::sync::OnceLock::new(); T0.get_or_init(Instant::now).elapsed().as_millis() as u64 }
/// Por quanto tempo o FEC fica ligado depois de um sinal de perda.
const FEC_HOLD_MS: u64 = 10_000;
/// Destino dos pacotes de paridade de uma saída de vídeo (o transporte acrescenta o slot e manda pelo canal de dados sem retransmissão).
pub type FecSink = Arc<dyn Fn(Vec<u8>) + Send + Sync>;
impl VideoOut {
    /// API interna de publicação (ver docs/plans/transmissao-api-interna.md): entrega UMA unidade de acesso H264 (Annex-B, já com
    /// SPS/PPS no quadro-chave) ao pacer, que empacota (RTP, MTU 1180), espaça e guarda para retransmissão. `samples` = intervalo desde
    /// o quadro anterior em ticks de 90 kHz; `pace_bps` = ritmo de saída (convenção: 2,5 × bitrate, piso de 600 kbps).
    /// É o ponto em que um SFU/LiveKit pode receber o MESMO codificador: troque o consumidor do canal, não o laço de captura/codificação.
    pub fn publish(&self, au: Vec<u8>, samples: u32, pace_bps: u32) {
        self.queued.fetch_add(au.len() as u64, Ordering::Relaxed);
        let _ = self.tx.send((au, samples.max(1), pace_bps));
    }
    /// Liga o FEC desta saída: `sink` recebe cada pacote de paridade. O pacer só os gera enquanto há perda (pedidos de retransmissão
    /// nos últimos 10 s), para não gastar banda numa rede limpa. `BRIKS_FEC=0` desliga.
    pub fn set_fec_sink(&self, sink: FecSink) { if std::env::var("BRIKS_FEC").as_deref() != Ok("0") { *self.fec_sink.lock().unwrap_or_else(|e| e.into_inner()) = Some(sink); } }
    /// Um espectador avisou que está perdendo pacotes (no SFU o emissor não vê a perda do lado de quem assiste: é o servidor que
    /// responde aos pedidos de retransmissão). Liga o FEC pelos próximos 10 s.
    pub fn request_fec(&self) { self.fec_until_ms.store(mono_ms() + FEC_HOLD_MS, Ordering::Relaxed); }
    pub fn new() -> Self { Self::new_slot(0) }
    pub fn new_slot(slot: usize) -> Self { Self::with_ids(if slot == 1 { VIDEO_TRACK_ID_2 } else { VIDEO_TRACK_ID }, if slot == 1 { "briks-screen-2" } else { "briks-screen" }) }
    /// Saída de vídeo da câmera (faixa `camera-video`, fluxo `briks-camera`).
    pub fn new_camera() -> Self { Self::with_ids(CAMERA_TRACK_ID, "briks-camera") }
    fn with_ids(id: &str, stream: &str) -> Self { Self::with_track(new_video_track_named(id, stream), Codec::H264, (id.into(), stream.into())) }
    pub fn track(&self) -> Arc<TrackLocalStaticRTP> { self.cur.lock().unwrap_or_else(|e| e.into_inner()).1.clone() }
    pub fn codec(&self) -> Codec { self.cur.lock().unwrap_or_else(|e| e.into_inner()).0 }
    /// Troca o codec da saída. Some(nova faixa) = mudou (o chamador faz `replace_track` em cada peer e reinicia o codificador); None = já era esse.
    pub fn set_codec(&self, codec: Codec) -> Option<Arc<TrackLocalStaticRTP>> {
        let mut g = self.cur.lock().unwrap_or_else(|e| e.into_inner());
        if g.0 == codec { return None; }
        let t = new_video_track_named_codec(&self.ids.0, &self.ids.1, codec);
        *g = (codec, t.clone());
        let _ = self.ctl.send((codec, t.clone()));
        Some(t)
    }
    fn with_track(track: Arc<TrackLocalStaticRTP>, codec: Codec, ids: (String, String)) -> Self {
        let (tx, rx) = mpsc::unbounded_channel();
        // limitado: um espectador que despeja NACKs não pode encher a memória do emissor; com a fila cheia o pedido é
        // descartado (try_send) e o receptor pede de novo ou pede quadro-chave
        let (nack_tx, nack_rx) = mpsc::channel(256);
        let (queued, rtx_sent, rtx_skipped, nack_unique) = (Arc::new(AtomicU64::new(0)), Arc::new(AtomicU64::new(0)), Arc::new(AtomicU64::new(0)), Arc::new(AtomicU64::new(0)));
        let rtx_gap_ms = Arc::new(AtomicU64::new(150));
        let rtx_twice = Arc::new(AtomicBool::new(false));
        let (ctl, ctl_rx) = mpsc::unbounded_channel();
        let cur = Arc::new(Mutex::new((codec, track.clone())));
        let (fec_sink, fec_sent): (Arc<Mutex<Option<FecSink>>>, _) = (Arc::new(Mutex::new(None)), Arc::new(AtomicU64::new(0)));
        let fec_until_ms = Arc::new(AtomicU64::new(0));
        tokio::spawn(pacer_task(codec, ctl_rx, track.clone(), rx, nack_rx, queued.clone(), rtx_sent.clone(), rtx_skipped.clone(), nack_unique.clone(), rtx_gap_ms.clone(), rtx_twice.clone(), fec_sink.clone(), fec_sent.clone(), fec_until_ms.clone()));
        VideoOut { cur, ids, ctl, tx, nack_tx, queued, rtx_sent, rtx_skipped, nack_unique, rtx_gap_ms, rtx_twice, fec_sink, fec_sent, fec_until_ms }
    }
}

const HISTORY: usize = 2048;

/// Pacer: empacota cada unidade de acesso e solta os pacotes espaçados a `pace_bps` em vez de despejar o quadro inteiro de uma vez
/// (uma rajada de centenas de kB estoura filas e buffers no caminho e vira perda). Também responde aos NACKs: guarda os últimos
/// pacotes e retransmite pelo mesmo espaçamento, com orçamento de 25% da taxa e no máximo uma vez a cada 150 ms por pacote —
/// retransmissão sem limite, numa rede já congestionada, só aumenta o congestionamento.
async fn pacer_task(mut codec: Codec, mut ctl_rx: mpsc::UnboundedReceiver<(Codec, Arc<TrackLocalStaticRTP>)>, mut track: Arc<TrackLocalStaticRTP>, mut rx: mpsc::UnboundedReceiver<(Vec<u8>, u32, u32)>, mut nack_rx: mpsc::Receiver<Vec<u16>>, queued: Arc<AtomicU64>, rtx_sent: Arc<AtomicU64>, rtx_skipped: Arc<AtomicU64>, nack_unique: Arc<AtomicU64>, rtx_gap_ms: Arc<AtomicU64>, rtx_twice: Arc<AtomicBool>, fec_sink: Arc<Mutex<Option<FecSink>>>, fec_sent: Arc<AtomicU64>, fec_until: Arc<AtomicU64>) {
    use std::collections::{HashMap, VecDeque};
    // um sequenciador só para a vida toda da transmissão: ao trocar de codec o número de sequência CONTINUA (um início aleatório novo cai
    // fora da janela anti-replay do SRTP do receptor em metade das vezes e os pacotes do codec novo seriam descartados)
    use webrtc::rtp::sequence::Sequencer as _;
    let seq = webrtc::rtp::sequence::new_random_sequencer();
    let mk_pk = |codec: Codec| { let payloader: Box<dyn webrtc::rtp::packetizer::Payloader + Send + Sync> = crate::codec::payloader(codec).unwrap_or_else(|| Box::new(H264Payloader::default()));
        webrtc::rtp::packetizer::new_packetizer(1180, 0, 0, payloader, seq.clone_to(), 90_000) };
    let mut pk = mk_pk(codec);
    let mut budget_at = tokio::time::Instant::now();
    let mut hist: HashMap<u16, (webrtc::rtp::packet::Packet, Option<Instant>, bool, Instant, bool)> = HashMap::new();   // (pacote, última retransmissão, já pedido, enviado em)
    let mut order: VecDeque<u16> = VecDeque::new();
    let (mut pace, mut rtx_credit, mut rtx_t) = (2_000_000f64, 0f64, Instant::now());
    // FEC: só enquanto há perda (um pedido de retransmissão de pacote novo, ou o aviso de um espectador, nos últimos 10 s)
    let mut fec = crate::fec::FecTx::new();
    loop {
        // Atende um lote de pedidos de retransmissão. É macro (e não ramo único do select) porque também roda ENTRE os pacotes de um
        // quadro: um quadro-chave com a rede apertada leva 1 s ou mais para sair, e se os NACKs esperassem o quadro terminar o
        // receptor desistia (prazo de 400 ms), pedia outro quadro-chave e o ciclo se repetia — o congelamento longo das quedas bruscas.
        macro_rules! serve_nacks { ($seqs:expr) => {{
            let seqs: Vec<u16> = $seqs;
                // crédito de retransmissão: 60% da taxa do pacer (um quadro-chave grande numa rede com perda precisa de muitos reenvios
                // para fechar dentro do prazo; com 25% ele nunca completava e o receptor ficava congelado), acumulando no máximo 1 s
                rtx_credit = (rtx_credit + rtx_t.elapsed().as_secs_f64() * pace * 0.4 / 8.0).min(pace * 0.4 / 8.0 * 0.75);
                rtx_t = Instant::now();
                for sq in seqs {
                    let Some((p, last, asked, sent_at, is_key)) = hist.get_mut(&sq) else { rtx_skipped.fetch_add(1, Ordering::Relaxed); continue };
                    if !std::mem::replace(asked, true) { nack_unique.fetch_add(1, Ordering::Relaxed); fec_until.store(mono_ms() + FEC_HOLD_MS, Ordering::Relaxed); }
                    // pacote velho: o receptor já desistiu desse quadro (espera no máximo 400 ms) e vai pedir quadro-chave; reenviar só
                    // gastaria banda. O gerador de NACK do outro lado continua pedindo por muito tempo.
                    if sent_at.elapsed() > Duration::from_millis(if *is_key { 1500 } else { 600 }) { rtx_skipped.fetch_add(1, Ordering::Relaxed); continue; }
                    let size = (p.payload.len() + 12) as f64;
                    if last.map(|t| t.elapsed() < Duration::from_millis(rtx_gap_ms.load(Ordering::Relaxed))).unwrap_or(false) || rtx_credit < size { rtx_skipped.fetch_add(1, Ordering::Relaxed); continue; }
                    rtx_credit -= size; *last = Some(Instant::now());
                    let _ = track.write_rtp(p).await;
                    if rtx_twice.load(Ordering::Relaxed) && rtx_credit >= size { rtx_credit -= size; let _ = track.write_rtp(p).await; }
                    rtx_sent.fetch_add(1, Ordering::Relaxed);
                    budget_at = budget_at.max(tokio::time::Instant::now()) + Duration::from_secs_f64(size * 8.0 / pace);
                }
        }}; }
        // sem `biased`: uma enxurrada de NACKs não pode deixar o vídeo novo esperando (o select sorteia o ramo pronto)
        tokio::select! {
            n = nack_rx.recv() => {
                let Some(seqs) = n else { break };
                serve_nacks!(seqs);
            }
            c = ctl_rx.recv() => {
                // troca de codec: faixa e empacotador novos; o histórico de retransmissão é do codec antigo e não vale mais
                let Some((nc, nt)) = c else { continue };
                codec = nc; track = nt; pk = mk_pk(nc); hist.clear(); order.clear(); fec.reset();
            }
            m = rx.recv() => {
                let Some((au, samples, pace_bps)) = m else { break };
                // o select sorteia entre ramos prontos: a troca de codec pedida ANTES deste quadro tem de valer para ele (senão o 1º quadro-chave
                // do codec novo sai no empacotador/faixa antigos e o espectador fica sem imagem até pedir outro)
                while let Ok((nc, nt)) = ctl_rx.try_recv() { codec = nc; track = nt; pk = mk_pk(nc); hist.clear(); order.clear(); }
                pace = (pace_bps as f64).max(600_000.0);
                let len = au.len() as u64;
                let is_key = if codec == Codec::H264 { crate::mfenc::nals(&au).iter().any(|n| n.0 == 5) } else { crate::codec::is_key_frame(codec, &au) };   // o receptor espera mais pelos pedaços de um quadro-chave
                let Ok(pkts) = pk.packetize(&Bytes::from(au), samples) else { queued.fetch_sub(len, Ordering::Relaxed); continue };
                if tokio::time::Instant::now().saturating_duration_since(budget_at) > Duration::from_millis(500) { budget_at = tokio::time::Instant::now(); }   // ficou parado: sem crédito acumulado
                let fec_to = if mono_ms() < fec_until.load(Ordering::Relaxed) { fec_sink.lock().unwrap_or_else(|e| e.into_inner()).clone() } else { None };
                if fec_to.is_none() { fec.reset(); }
                let last_seq = pkts.last().map(|p| p.header.sequence_number);
                for p in pkts {
                    let now = tokio::time::Instant::now();
                    // espera a vez deste pacote atendendo os NACKs que chegarem nesse meio-tempo
                    let _ = now;
                    loop {
                        if budget_at <= tokio::time::Instant::now() + Duration::from_millis(2) { break; }
                        tokio::select! {
                            _ = tokio::time::sleep_until(budget_at) => break,
                            n = nack_rx.recv() => match n { Some(seqs) => { serve_nacks!(seqs); } None => break },
                        }
                    }
                    while let Ok(seqs) = nack_rx.try_recv() { serve_nacks!(seqs); }
                    let now = tokio::time::Instant::now();
                    let _ = track.write_rtp(&p).await;
                    if let Some(sink) = &fec_to {
                        if let Some(par) = fec.add(p.header.sequence_number, p.header.timestamp, p.header.marker, &p.payload, Some(p.header.sequence_number) == last_seq) { fec_sent.fetch_add(1, Ordering::Relaxed); sink(par); }
                    }
                    budget_at = budget_at.max(now - Duration::from_millis(20)) + Duration::from_secs_f64((p.payload.len() + 12) as f64 * 8.0 / pace);
                    let sq = p.header.sequence_number;
                    if hist.insert(sq, (p, None, false, Instant::now(), is_key)).is_none() { order.push_back(sq); }
                    if order.len() > HISTORY { if let Some(old) = order.pop_front() { hist.remove(&old); } }
                }
                queued.fetch_sub(len, Ordering::Relaxed);
            }
        }
    }
}

pub struct Sender {
    stop: Arc<AtomicBool>,
    pub idr: Arc<AtomicBool>,
}
impl Sender {
    pub fn stop(&self) { self.stop.store(true, Ordering::Relaxed); }
    /// Pede um quadro-chave no próximo envio (PLI/FIR dos receptores, novo espectador).
    pub fn request_idr(&self) { self.idr.store(true, Ordering::Relaxed); }
}
impl Drop for Sender { fn drop(&mut self) { self.stop(); } }

pub fn cpu_ms() -> f64 {
    use windows::Win32::Foundation::FILETIME;
    use windows::Win32::System::Threading::{GetCurrentProcess, GetProcessTimes};
    let (mut c, mut e, mut k, mut u) = (FILETIME::default(), FILETIME::default(), FILETIME::default(), FILETIME::default());
    unsafe { if GetProcessTimes(GetCurrentProcess(), &mut c, &mut e, &mut k, &mut u).is_err() { return 0.0; } }
    let v = |f: FILETIME| ((f.dwHighDateTime as u64) << 32 | f.dwLowDateTime as u64) as f64 / 10_000.0;
    v(k) + v(u)
}

/// Fração (0..1) de amostras da luminância (I420, faixa limitada: preto = 16) que são preto. Amostra esparsa: custa ~20 mil leituras
/// por quadro de 1080p.
pub fn dark_fraction(y: &[u8]) -> f32 {
    let (mut dark, mut n) = (0u32, 0u32);
    for v in y.iter().step_by(97) { if *v <= 20 { dark += 1; } n += 1; }
    if n == 0 { 0.0 } else { dark as f32 / n as f32 }
}

/// Guarda de quadro preto: ao trocar de aba no navegador (ou o app trocar de superfície) a captura entrega ~1 s de conteúdo
/// totalmente preto, com só a moldura da janela visível, antes de o site pintar. Para quem assiste isso parecia "reload". Quando
/// um quadro quase todo preto aparece logo depois de um quadro normal, ele NÃO é codificado: o último quadro bom segue na tela do
/// espectador (o reenvio de tela parada o repete) até chegar o novo, por no máximo `MAX_HOLD`; se o preto persistir, é conteúdo
/// de verdade (jogo, tela escura) e passa.
pub struct BlackoutGuard { last_good: bool, since: Option<Instant>, pub held: u64 }
impl BlackoutGuard {
    pub const MAX_HOLD: Duration = Duration::from_millis(1500);
    pub const BLACK: f32 = 0.80;
    pub fn new() -> Self { BlackoutGuard { last_good: false, since: None, held: 0 } }
    /// `dark` = `dark_fraction` do quadro novo. true = segura (não codifica este quadro).
    pub fn hold(&mut self, now: Instant, dark: f32) -> bool {
        if dark < Self::BLACK {
            self.since = None;
            if dark < 0.5 { self.last_good = true; }
            return false;
        }
        if !self.last_good { return false; }   // já estava preto (ou nunca houve quadro bom): nada a segurar
        let t = *self.since.get_or_insert(now);
        if now.saturating_duration_since(t) < Self::MAX_HOLD { self.held += 1; return true; }
        self.last_good = false; self.since = None;   // preto persistente: é o conteúdo
        false
    }
}

/// Recuperação do codificador sem derrubar a transmissão. O erro do codificador ("o codificador não pediu entrada", GPU reiniciada pelo
/// Windows, MEError...) deixava de ser fatal: o quadro é descartado (o espectador segue vendo o último), o codificador é recriado com
/// quadro-chave (backoff curto) e, se o de hardware falhar de novo e de novo, passa para o de software, com aviso discreto. Só quando
/// o de software também esgota as tentativas a transmissão termina — com mensagem em português e o HRESULT no log.
#[derive(Debug, Clone, PartialEq)]
pub enum Recovery {
    /// descarta este quadro e segue (ainda dentro da tolerância, ou esperando o backoff)
    Drop,
    /// recria o codificador (com quadro-chave); `software` = sem o MFT de hardware
    Recreate { software: bool },
    /// esgotou: encerra com esta mensagem
    GiveUp(String),
}
pub struct EncSupervisor {
    fails: u32,
    first_fail: Option<Instant>,
    retry_at: Option<Instant>,
    recreations: Vec<Instant>,
    pub software: bool,
    pub total_faults: u64,
    pub total_recreations: u64,
}
impl EncSupervisor {
    /// quadros seguidos sem entrada (ou o tempo) antes de recriar o codificador
    pub const DROPS_BEFORE_RECREATE: u32 = 3;
    pub const STALL_BEFORE_RECREATE: Duration = Duration::from_secs(2);
    /// recriações do mesmo tipo (hardware, depois software) na janela antes de trocar de codificador / desistir
    pub const ATTEMPTS: usize = 3;
    pub const WINDOW: Duration = Duration::from_secs(30);
    pub fn new() -> Self { EncSupervisor { fails: 0, first_fail: None, retry_at: None, recreations: vec![], software: false, total_faults: 0, total_recreations: 0 } }
    pub fn on_ok(&mut self) { self.fails = 0; self.first_fail = None; }
    pub fn on_error(&mut self, now: Instant, f: &crate::mfenc::EncFault) -> Recovery {
        use crate::mfenc::FaultKind;
        self.total_faults += 1;
        self.fails += 1;
        let first = *self.first_fail.get_or_insert(now);
        if self.retry_at.map(|t| now < t).unwrap_or(false) { return Recovery::Drop; }   // acabou de recriar: dá tempo
        let must = f.kind != FaultKind::Stalled || self.fails >= Self::DROPS_BEFORE_RECREATE || now.saturating_duration_since(first) >= Self::STALL_BEFORE_RECREATE;
        if !must { return Recovery::Drop; }
        self.recreations.retain(|t| now.saturating_duration_since(*t) < Self::WINDOW);
        if self.recreations.len() >= Self::ATTEMPTS {
            if !self.software {
                self.software = true; self.recreations.clear();   // o hardware não se sustenta: troca de codificador
            } else {
                return Recovery::GiveUp(Self::explain(f));
            }
        }
        self.recreations.push(now); self.total_recreations += 1;
        self.fails = 0; self.first_fail = None;
        self.retry_at = Some(now + Duration::from_millis(150 * self.recreations.len() as u64));   // backoff curto: 150, 300, 450 ms
        Recovery::Recreate { software: self.software }
    }
    /// O codificador nem abriu (depois de uma recriação): conta como tentativa e cai para o software se for o de hardware.
    pub fn on_create_error(&mut self, now: Instant, e: &str) -> Recovery {
        let f = crate::mfenc::EncFault::new(crate::mfenc::FaultKind::Other, 0, e.to_string());
        self.recreations.retain(|t| now.saturating_duration_since(*t) < Self::WINDOW);
        self.recreations.push(now); self.total_faults += 1;
        if !self.software { self.software = true; self.recreations.clear(); self.recreations.push(now); return Recovery::Recreate { software: true }; }
        if self.recreations.len() > Self::ATTEMPTS { return Recovery::GiveUp(Self::explain(&f)); }
        self.retry_at = Some(now + Duration::from_millis(150 * self.recreations.len() as u64));
        Recovery::Drop
    }
    /// Mensagem final para o usuário (português): o que houve, o que o app fez, o que fazer. O HRESULT vai no log e entre parênteses.
    pub fn explain(f: &crate::mfenc::EncFault) -> String {
        let hr = if f.hr != 0 { format!(" (código 0x{:08X})", f.hr as u32) } else { String::new() };
        let what = match f.kind {
            crate::mfenc::FaultKind::DeviceLost => "A placa de vídeo foi reiniciada pelo Windows e o codificador não voltou",
            crate::mfenc::FaultKind::Stalled => "O codificador de vídeo parou de responder (a placa de vídeo está sobrecarregada)",
            crate::mfenc::FaultKind::Other => "O codificador de vídeo falhou",
        };
        format!("{what}{hr}. O Briks tentou reiniciá-lo e usar o codificador por software, sem sucesso. Clique em Tentar de novo; se repetir, atualize o driver da placa de vídeo.")
    }
}

#[link(name = "winmm")]
extern "system" {
    fn timeBeginPeriod(period: u32) -> u32;
    fn timeEndPeriod(period: u32) -> u32;
}
/// Relógio fino do Windows (1 ms) enquanto existir. Sem ele as esperas do sistema andam em passos de ~15,6 ms: o pacer (temporizador do
/// tokio) soltava os pacotes em rajadas a cada 15 ms em vez de espaçados, e qualquer `Sleep` curto do processo arredondava para cima —
/// o que aparecia para quem assiste como quadros chegando em intervalos irregulares. O pedido vale para o processo e é contado pelo
/// próprio Windows (cada `timeBeginPeriod` tem o seu `timeEndPeriod`), então só fica ligado enquanto há transmissão no ar.
pub struct FineTimer(());
impl FineTimer { pub fn acquire() -> Self { unsafe { timeBeginPeriod(1); } FineTimer(()) } }
impl Drop for FineTimer { fn drop(&mut self) { unsafe { timeEndPeriod(1); } } }

/// O que o laço do emissor faz agora.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Tick {
    /// ainda não é hora (ou está dando um instante para a captura entregar): dorme isto e pergunta de novo
    Wait(Duration),
    /// há quadro novo da captura: codifica e envia
    Fresh,
    /// não chegou quadro novo neste período: codifica de novo o último (a cadência não cai quando a tela fica parada)
    Repeat,
    /// nada para enviar neste período
    Idle,
}

/// Cadência do emissor: uma grade fixa no FPS alvo. Antes o laço só enviava quando a captura entregava um quadro novo — e a captura
/// do Windows só entrega quando a tela MUDA. Tela parada, vídeo a 24 qps, página rolando aos trancos: o FPS enviado acompanhava o
/// conteúdo ("14 FPS" numa transmissão de 30) e o controle de taxa do codificador, que conta com o FPS configurado, errava o tamanho
/// dos quadros. Agora cada período da grade sai um quadro: o novo, se houver; senão o último de novo (para o codificador é um quadro
/// "igual ao anterior", de poucas centenas de bytes).
///
/// Tolerância (`grace`, 1/3 do período): captura e grade são dois relógios quase iguais e fora de fase; quando o quadro novo chega
/// logo DEPOIS do instante da grade, repetir na hora e deixar o novo para o período seguinte dava um quadro repetido seguido de um
/// quadro perdido (dois chegam no mesmo período, o mais velho é sobrescrito). Esperar um instante pelo quadro novo evita os dois.
pub struct Cadence { period: Duration, next: Instant,
    /// false = comportamento antigo (só reenvia a tela parada uma vez por segundo); `BRIKS_SHARE_NO_REPEAT` (diagnóstico)
    pub repeat: bool,
    /// Seguir a fonte (pedido do usuário, 2026-11-14): um filme a 24 qps numa transmissão de 30 saía com 6 quadros repetidos por
    /// segundo, encaixados na grade de 33 ms — cadência irregular e bits gastos em quadros iguais. Quando a fonte entrega quadros num
    /// ritmo REGULAR mais lento que o alvo (20 qps ou mais: vídeo/filme), cada quadro novo sai assim que chega, sem repetições, e o
    /// FPS enviado é o da fonte. Fonte irregular ou abaixo de 20 qps (página rolando aos trancos), tela parada e fonte no ritmo do
    /// alvo continuam na grade, como antes. `BRIKS_SHARE_NO_FOLLOW` desliga (diagnóstico).
    pub follow_source: bool,
    /// chegadas dos últimos quadros novos (13 = 12 intervalos)
    arrivals: std::collections::VecDeque<Instant>,
    /// o quadro novo que está esperando já foi contado em `arrivals`
    seen: bool,
    /// período médio da fonte enquanto ela está sendo seguida
    follow: Option<Duration> }
impl Cadence {
    pub const POLL: Duration = Duration::from_millis(1);
    pub fn new(now: Instant, fps: u32) -> Self { Cadence { period: Self::period_of(fps), next: now, repeat: true, follow_source: true, arrivals: Default::default(), seen: false, follow: None } }
    /// FPS da fonte que está sendo seguida (None = na grade do FPS alvo).
    pub fn follow_fps(&self) -> Option<u32> { self.follow.map(|p| (1.0 / p.as_secs_f64()).round() as u32) }
    /// Fonte mais lenta que isto não é seguida (não é vídeo: é tela que muda pouco, e aí vale a grade com repetição).
    const FOLLOW_MIN_FPS: f64 = 20.0;
    /// Chegou um quadro novo: guarda o instante e decide se a fonte tem ritmo regular mais lento que o alvo.
    fn note_arrival(&mut self, now: Instant) {
        self.arrivals.push_back(now);
        while self.arrivals.len() > 13 { self.arrivals.pop_front(); }
        let was = self.follow.is_some();
        self.follow = None;
        if self.follow_source && self.repeat && self.arrivals.len() == 13 {
            let iv: Vec<f64> = self.arrivals.iter().zip(self.arrivals.iter().skip(1)).map(|(a, b)| b.duration_since(*a).as_secs_f64()).collect();
            let m = iv.iter().sum::<f64>() / iv.len() as f64;
            // regular: no máximo um intervalo fora de 0,7x–1,4x da média (um filme a 24 qps num monitor de 60 Hz chega alternando
            // 33 e 50 ms; um quadro pulado pela fonte de vez em quando não derruba o modo)
            let off = iv.iter().filter(|x| **x < m * 0.7 || **x > m * 1.4).count();
            if off <= 1 && m >= self.period.as_secs_f64() * 1.12 && m <= 1.0 / Self::FOLLOW_MIN_FPS * 1.02 { self.follow = Some(Duration::from_secs_f64(m)); }
        }
        if was && self.follow.is_none() { self.next = now; }   // voltou para a grade: ela recomeça daqui
    }
    fn period_of(fps: u32) -> Duration { Duration::from_micros(1_000_000 / fps.clamp(1, 240) as u64) }
    pub fn set_fps(&mut self, fps: u32) { self.period = Self::period_of(fps); }
    pub fn period(&self) -> Duration { self.period }
    pub fn grace(&self) -> Duration { self.period / 3 }
    /// `fresh` = a captura tem quadro novo esperando; `held` = há um último quadro para repetir; `since_send` = tempo desde o último envio.
    pub fn step(&mut self, now: Instant, fresh: bool, held: bool, since_send: Duration) -> Tick {
        if !fresh { self.seen = false; } else if !self.seen { self.seen = true; self.note_arrival(now); }
        if let Some(sp) = self.follow {
            // seguindo a fonte: o quadro novo sai ao chegar; sem quadro novo, espera — até a fonte parar (2,2 períodos dela sem nada)
            if fresh { self.next = now + self.period; return Tick::Fresh; }
            let last = self.arrivals.back().copied().unwrap_or(now);
            if now.saturating_duration_since(last) <= sp.mul_f64(2.2) { return Tick::Wait(Self::POLL); }
            self.follow = None; self.arrivals.clear(); self.next = now;   // a fonte parou: de volta à grade (repete o último quadro)
        }
        if now < self.next { return Tick::Wait((self.next - now).min(Duration::from_millis(5))); }
        if fresh { self.advance(now); return Tick::Fresh; }
        // ainda sem quadro nenhum (início, janela minimizada): olha de novo em 1 ms, e a grade começa no 1º quadro que chegar —
        // assim ela nasce em fase com a captura, em vez de meio período atrasada
        if !held { self.next = now + Self::POLL; return Tick::Idle; }
        if !self.repeat { self.advance(now); return if since_send >= Duration::from_secs(1) { Tick::Repeat } else { Tick::Idle }; }
        let limit = self.next + self.grace();
        if now < limit { return Tick::Wait((limit - now).min(Self::POLL)); }
        self.advance(now);
        Tick::Repeat
    }
    /// Próximo instante da grade. Atrasou mais de um período (codificador lento, máquina travada): recomeça a grade daqui, sem
    /// rajada de quadros para "recuperar".
    fn advance(&mut self, now: Instant) { self.next += self.period; if self.next <= now { self.next = now + self.period; } }
}

/// FPS mostrado no chip de quem assiste: quadros decodificados / tempo da janela (a janela fecha entre 1,00 e ~1,05 s; contar os
/// quadros sem dividir dava 31–32 numa transmissão de 30). E, como a contagem de uma janela de 1 s sempre oscila ±1 quadro com o
/// fluxo perfeitamente regular (o quadro da borda cai de um lado ou do outro), uma diferença de até 1 mantém o número anterior:
/// o chip só muda quando a taxa muda de verdade.
pub fn steady_fps(prev: u64, frames: u64, secs: f64) -> u64 {
    let m = frames as f64 / secs.max(0.5);
    if frames > 0 && prev > 0 && (m - prev as f64).abs() <= 1.0 { prev } else { m.round() as u64 }
}

/// Bitrate do codificador para um orçamento de rede: desconta cabeçalhos RTP/UDP/IP e a folga das retransmissões.
fn enc_bps(budget: u32) -> u32 { (budget as f64 * 0.85) as u32 }

/// Teto de QP do codificador H264 conforme o conteúdo. O teto baixo (28) é o que mantém o texto nítido no quadro-chave, mas com ele o
/// NVENC IGNORA o bitrate quando o conteúdo não cabe (grão de filme pesado, jogo caótico): 67 Mbps com 2 a 6 Mbps pedidos (medido). O
/// excesso entope o pacer, o emissor pula quadros (FPS baixo) e a escada derruba a resolução. Aqui, dois segundos seguidos de estouro
/// sobem o teto um degrau (28 → 34 → 40 → 51) e o codificador é recriado com ele (a troca ao vivo é aceita e não obedecida). Depois de
/// um tempo calmo tenta descer um degrau; se estourar de novo, volta e dobra a espera (até 10 min) para não ficar sondando.
pub struct QpGovernor { pub cap: u32, over: u32, calm: u32, hold: u32, grace: u32 }
impl QpGovernor {
    const STEPS: [u32; 4] = [crate::mfenc::DEFAULT_MAX_QP, 34, 40, 51];
    pub fn new() -> Self { QpGovernor { cap: Self::STEPS[0], over: 0, calm: 0, hold: 60, grace: 0 } }
    /// Uma vez por segundo: `sent`/`asked` = bps que saíram do codificador e bps pedidos; `transient` = segundo com quadro-chave forçado
    /// ou troca de nível (o excesso é do quadro-chave, não do conteúdo). Devolve o teto novo quando ele muda (recrie o codificador).
    pub fn note(&mut self, sent: f64, asked: f64, transient: bool) -> Option<u32> {
        if std::env::var("BRIKS_ENC_MAXQP").is_ok() { return None; }   // teto fixado para diagnóstico: recriar o codificador não mudaria nada
        if self.grace > 0 { self.grace -= 1; return None; }   // codificador recém-criado: o 1º quadro-chave distorce a medida
        if transient || asked <= 0.0 { self.over = 0; return None; }
        let i = Self::STEPS.iter().position(|q| *q == self.cap).unwrap_or(0);
        if sent > asked * 1.5 {
            self.over += 1; self.calm = 0;
            // estouro de 3x ou mais não é quadro-chave nem cena difícil passageira: sobe já
            if (self.over >= 2 || sent > asked * 3.0) && i + 1 < Self::STEPS.len() { self.cap = Self::STEPS[i + 1]; self.over = 0; self.grace = 2; self.hold = (self.hold * 2).min(600); return Some(self.cap); }
        } else {
            self.over = 0; self.calm += 1;
            if i > 0 && self.calm >= self.hold { self.cap = Self::STEPS[i - 1]; self.calm = 0; self.grace = 2; return Some(self.cap); }
        }
        None
    }
}
impl Default for QpGovernor { fn default() -> Self { Self::new() } }

/// Quadro-chave periódico (rede de segurança; os de verdade saem a pedido do receptor, por PLI/FIR). Estava em 3 s para encurtar a
/// imagem corrompida no espectador; a causa dela era o receptor pular quadros (ver `DecodeFeed`), não a falta de quadro-chave. Cada
/// quadro-chave custa caro: em texto parado a 4,5 Mbps a luminância cai de 45 para 35 dB e leva mais de 1 s para voltar (uma piscada a
/// cada 3 s); em filme a 2 Mbps, −0,6 dB na média e −1,5 dB no pior quadro, com um quadro de 133 KB em vez de 106 (medido).
pub const KEY_EVERY_S: u32 = 5;

/// Inicia o emissor numa thread. `ev` recebe eventos JSON ("share.local"...); `preview` = arquivo de quadro compartilhado da prévia.
pub fn start_sender(gen: u64, idr: Arc<AtomicBool>, net: Arc<Mutex<crate::adapt::NetShared>>, cfg: ShareCfg, out: VideoOut, ev: mpsc::Sender<Value>, preview: Option<PathBuf>, dump: Option<PathBuf>) -> Result<Sender, String> {
    let cap = Capture::start(cfg.kind.clone(), cfg.fps, cfg.height, preview.is_some(), dump.is_some()).map_err(|e| e.to_string())?;
    let stop = Arc::new(AtomicBool::new(false));
    idr.store(true, Ordering::Relaxed);
    let (tx, queued) = (out.tx.clone(), out.queued.clone());   // bytes esperando no pacer
    let out_codec = out.codec();
    let out_rtx = (out.rtx_sent.clone(), out.rtx_skipped.clone(), out.nack_unique.clone());
    let out_fec = out.fec_sent.clone();
    let (st, id2) = (stop.clone(), idr.clone());
    std::thread::spawn(move || {
        // Um pânico aqui dentro não pode deixar a UI mostrando "transmitindo" para sempre: o laço roda protegido e, se cair,
        // a transmissão termina com aviso.
        let ev_panic = ev.clone();
        let run = std::panic::AssertUnwindSafe(move || {
        use crate::adapt::{Controller, Estimator};
        let max_fps = cfg.fps.clamp(5, 120);
        let no_cpu = std::env::var("BRIKS_ADAPT_NO_CPU").is_ok();   // testes de rede: isola o laço de rede da carga da máquina
        let mut ctl = Controller::new(cfg.content, cfg.height, max_fps, cfg.kbps.min(MAX_BPS / 1000));
        let mut est = Estimator::new(ctl.top_ceil());
        let mut dec = ctl.decide(est.est);
        let mut fps = dec.level.fps;
        let _fine = FineTimer::acquire();   // esperas de 1 ms (pacer e este laço) enquanto a transmissão está no ar
        let mut cad = Cadence::new(Instant::now(), fps);
        cad.repeat = std::env::var("BRIKS_SHARE_NO_REPEAT").is_err();
        cad.follow_source = std::env::var("BRIKS_SHARE_NO_FOLLOW").is_err();
        let mut enc_fps = fps;   // FPS informado ao codificador (o da fonte, quando ela está sendo seguida)
        let mut cap = cap;
        let mut enc: Option<H264Enc> = None;
        let mut pv = preview.and_then(|p| FrameSink::create(&p).ok());
        let mut last_send = Instant::now() - Duration::from_secs(5);
        let mut held: Option<video::Yuv> = None;
        let mut repeated = 0u64;   // quadros repetidos na janela (telemetria: FPS enviado − repetidos = o que a tela de fato mudou)
        let (mut frames, mut bytes, mut enc_ns, mut skipped) = (0u64, 0u64, 0u128, 0u64);
        let (mut win_t, mut win_cpu) = (Instant::now(), cpu_ms());
        let (mut cap_frames0, mut conv_ns0, mut nacks0) = (0u64, 0u64, 0u64);
        let mut since_idr = 0u32;
        let mut idrs0 = 0u64;
        let mut trans_win = false;   // nível/codificador mudou na janela atual
        let mut idrs = 0u64;   // quadros-chave forçados (telemetria: rajada de IDR = "reload" no espectador)
        let mut pacer_drops = 0u64;
        let mut just_changed = false;
        let mut recreated = Instant::now() - Duration::from_secs(4);
        let (mut black_t, mut black_run, mut black_said) = (Instant::now(), 0u32, false);
        let mut last_idr = Instant::now() - Duration::from_secs(5);
        let mut announce = false;
        let mut guard = BlackoutGuard::new();
        let mut sup = EncSupervisor::new();
        let mut qpg = QpGovernor::new();
        let mut recreated_for_fault = false;
        let mut held_win = 0u64;   // quadros pretos segurados na janela (telemetria)
        let mut closed_reason = String::new();
        while !st.load(Ordering::Relaxed) {
            if cap.shared.closed.load(Ordering::Relaxed) {
                closed_reason = { let r = cap.shared.close_reason.lock().unwrap_or_else(|e| e.into_inner()).clone(); if r.is_empty() { "A janela foi fechada".into() } else { r } };
                break;
            }
            // grade fixa no FPS alvo: quadro novo se houver; senão, passado um instante de tolerância, o último de novo
            let has_fresh = { let l = cap.shared.latest.lock().unwrap_or_else(|e| e.into_inner()); l.w != 0 && l.yuv.is_some() };
            let tick = cad.step(Instant::now(), has_fresh, held.is_some(), last_send.elapsed());
            if let Tick::Wait(d) = tick { std::thread::sleep(d); continue; }
            // ---- uma vez por segundo: mede, estima a banda, decide o nível e publica a telemetria ----
            if win_t.elapsed() >= Duration::from_secs(1) {
                let secs = win_t.elapsed().as_secs_f64();
                if secs > 1.5 && std::env::var("BRIKS_EST_DEBUG").is_ok() { eprintln!("[lento] janela de medição durou {secs:.2} s"); }
                let cpu = cpu_ms();
                let sent_bps = bytes as f64 * 8.0 / secs;
                let sent_fps = frames as f64 / secs;
                let capf = cap.shared.frames.load(Ordering::Relaxed);
                let dropped = cap.shared.dropped.swap(0, Ordering::Relaxed);
                let offered = (capf - cap_frames0 + dropped) as f64 / secs;
                let convn = cap.shared.conv_ns.load(Ordering::Relaxed);
                let work_ms = if frames > 0 { ((convn - conv_ns0) as f64 + enc_ns as f64) / frames as f64 / 1e6 } else { 0.0 };
                // numa malha, quem está congestionado sozinho não dita o bitrate (regra da maioria)
                let ((worst, left_out), (nacks, plis)) = { let n = net.lock().unwrap(); (n.majority(), n.totals()) };
                let lost_new = out_rtx.2.swap(0, Ordering::Relaxed);
                // os pedidos de retransmissão não dizem de quem vieram: com alguém deixado de fora, o sinal de perda fica só com os relatórios
                let nack_ratio = if bytes > 0 && left_out == 0 { lost_new as f32 / (bytes as f32 / 1100.0).max(1.0) } else { 0.0 };
                let (tw, th) = enc.as_ref().map(|e| (e.w, e.h)).unwrap_or((0, 0));
                if cfg.adaptive {
                    est.set_ceil(ctl.top_ceil());
                    est.update(Instant::now(), sent_bps, worst, nack_ratio);
                    if std::env::var("BRIKS_EST_DEBUG").is_ok() { eprintln!("[est] enviado {:.0} kbps | relatorio {:?} | nack {:.3} (perdidos {lost_new}) | est {:.0} kbps motivo {} tol {:.2}", sent_bps / 1000.0, worst, nack_ratio, est.est / 1000.0, est.reason, est.loss_tol); }
                    // o segundo logo depois de uma troca de nível mistura os dois níveis (e a recriação do codificador): não conta
                    if !no_cpu && !std::mem::take(&mut just_changed) { ctl.note_cpu(work_ms, offered, sent_fps, (pacer_drops + skipped) as f64 / secs, ((cpu - win_cpu) / (secs * 1000.0) * 100.0).min(100.0)); }
                    if let Some(e) = enc.as_ref() { if frames >= 1 { ctl.note_encoder(sent_bps, e.bps as f64, sent_fps, e.w, e.h, idrs != idrs0 || trans_win); } }
                    let d = ctl.decide(est.est);
                    if d.level != dec.level {
                        just_changed = true;
                        trans_win = true;
                        fps = d.level.fps; cad.set_fps(fps);
                        cap.shared.retarget(fps, d.level.h);
                        if let Some(e) = enc.as_mut() { if !e.set_fps(fps) { enc = None; } }
                        announce = true;
                    }
                    if let Some(e) = enc.as_mut() {
                        let diff = (enc_bps(d.bps) as f64 - e.bps as f64).abs() / e.bps.max(1) as f64;
                        let want = enc_bps(d.bps);
                        // Queda grande: recria o codificador. O ajuste ao vivo no NVENC leva ~2 s para valer (medido), e nesses 2 s o
                        // envio continua acima do que a rede aguenta. Subidas e ajustes pequenos vão ao vivo, sem quadro-chave.
                        // Cada criação do MFT da NVIDIA deixa ~4 handles para trás (medido: 40 ciclos, +4,1 por ciclo, mesmo com
                        // Shutdown do MFT e do objeto de ativação), então só recria em queda para menos de 70% e no máximo uma vez
                        // a cada 2 s; o resto vai ao vivo.
                        if (want as f64) < e.bps as f64 * 0.7 && recreated.elapsed() >= Duration::from_secs(2) { enc = None; trans_win = true; recreated = Instant::now(); }
                        else if diff > 0.05 && !e.set_bitrate(want) { enc = None; }
                    }
                    dec = d;
                }
                // o codificador ignorou o bitrate (conteúdo que não cabe com o teto de QP atual): recria com o teto um degrau acima.
                // Vale também sem adaptação (bitrate fixo pelo usuário).
                if let Some(e) = enc.as_ref() { if frames >= 1 && e.has_qp_cap() { if let Some(q) = qpg.note(sent_bps, e.bps as f64, idrs != idrs0 || trans_win) {
                    crate::netlog::write(&format!("transmissao: teto de QP {q} (o codificador enviou {:.0} kbps com {} pedidos)", sent_bps / 1000.0, e.bps / 1000));
                    enc = None; recreated = Instant::now();
                } } }
                let (loss, rtt, jit) = worst.unwrap_or((0.0, 0.0, 0.0));
                // por que o nível está abaixo do configurado (prova nos logs): "cpu" = sobrecarga local, "conteudo" = o codificador não
                // obedece o bitrate neste conteúdo (piso de bits por pixel), "rede" = banda estimada
                let why = if !cfg.adaptive || ctl.idx == 0 { "" } else if dec.cpu_limited || (ctl.cpu_idx > 0 && ctl.cpu_idx >= ctl.idx) { "cpu" } else if ctl.floor_driven { "conteudo" } else { "rede" };
                let adapting = if !cfg.adaptive { "" } else if dec.cpu_limited || (ctl.cpu_idx > 0 && ctl.cpu_idx >= ctl.idx) { "cpu" } else if ctl.idx > 0 || (dec.bps as f64) < ctl.top_ceil() * 0.9 { "rede" } else { "" };
                {   // diagnóstico de desempenho: uma linha a cada ~5 s no rede.log (codificador, FPS enviado x alvo, CPU do Briks, por que o nível caiu)
                    static LOGN: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
                    if LOGN.fetch_add(1, Ordering::Relaxed) % 5 == 0 {
                        let (en, hw) = enc.as_ref().map(|e| (e.name.to_string(), e.hardware)).unwrap_or_default();
                        crate::netlog::write(&format!("transmissao: {en} ({}) {tw}x{th} enviou {:.1} fps (alvo {}{}) {} kbps, captura ofereceu {:.0} fps, cpu do briks {:.0}% de 1 nucleo, tempo por quadro {:.1} ms (codificar {:.1} ms), nivel {} motivo '{}'",
                            if hw { "hardware" } else { "software" }, sent_fps, dec.level.fps, cad.follow_fps().map(|f| format!(", seguindo a fonte a {f}")).unwrap_or_default(), (sent_bps / 1000.0).round(), offered, (cpu - win_cpu) / (secs * 1000.0) * 100.0,
                            work_ms, if frames > 0 { enc_ns as f64 / frames as f64 / 1e6 } else { 0.0 }, ctl.idx, why));
                    }
                }
                let _ = ev.try_send(json!({ "ev": "share.stats", "gen": gen, "fps": (sent_fps * 10.0).round() / 10.0, "kbps": (sent_bps / 1000.0).round(),
                    "enc_ms": if frames > 0 { (enc_ns as f64 / frames as f64 / 1e6 * 10.0).round() / 10.0 } else { 0.0 }, "work_ms": (work_ms * 10.0).round() / 10.0,
                    "skipped": skipped, "w": tw, "h": th, "cpu_pct": ((cpu - win_cpu) / (secs * 1000.0) * 100.0).round(), "cap_dropped": dropped, "offered_fps": offered.round(),
                    "target_kbps": enc_bps(dec.bps) / 1000, "est_kbps": (est.est / 1000.0).round(), "target_fps": dec.level.fps, "target_h": dec.level.h, "level": ctl.idx,
                    "loss_pct": (loss * 1000.0).round() / 10.0, "rtt_ms": rtt.round(), "jitter_ms": jit.round(), "nacks": nacks.saturating_sub(nacks0), "plis": plis, "idrs": idrs,
                    "blackout_held": std::mem::take(&mut held_win), "bpp_floor": (ctl.bpp_floor * 1000.0).round() / 1000.0, "cpu_idx": ctl.cpu_idx, "why": why,
                    "adapting": adapting, "reason": est.reason, "left_out": left_out, "overshoot": (ctl.overshoot * 100.0).round() / 100.0, "pacer_drops": std::mem::take(&mut pacer_drops), "repeated": std::mem::take(&mut repeated), "lost_pkts": lost_new, "rtx": out_rtx.0.swap(0, Ordering::Relaxed), "rtx_skipped": out_rtx.1.swap(0, Ordering::Relaxed), "pacer_queue_kb": queued.load(Ordering::Relaxed) / 1000, "fec_sent": out_fec.swap(0, Ordering::Relaxed), "source_fps": cad.follow_fps().unwrap_or(0) }));
                idrs0 = idrs; trans_win = false; frames = 0; bytes = 0; enc_ns = 0; skipped = 0; win_t = Instant::now(); win_cpu = cpu; cap_frames0 = capf; conv_ns0 = convn; nacks0 = nacks;
            }
            // quadro novo, ou o último de novo quando a tela não mudou neste período (a cadência fica no FPS alvo)
            let (yuv, pvw, raw, src) = {
                let mut l = cap.shared.latest.lock().unwrap_or_else(|e| e.into_inner());
                if l.w == 0 { continue; }
                let fresh = l.yuv.take();
                if let (Some(d), Some(y)) = (&dump, l.raw_yuv.take()) { let mut rgba = vec![255u8; y.w * y.h * 4]; video::i420_to_rgba(&y.y, &y.u, &y.v, (y.w, y.w / 2, y.w / 2), y.w, y.h, &mut rgba); if let Some(img) = image::RgbaImage::from_raw(y.w as u32, y.h as u32, rgba) { let _ = img.save(d); } }
                (fresh, l.preview.take(), l.raw.take(), (l.w, l.h))
            };
            if let (Some(p), Some((pw, ph, px))) = (pv.as_mut(), pvw) { p.write(pw, ph, &px); }
            let (yuv, is_repeat) = match yuv {
                Some(y) => (y, false),   // (um quadro que chegou entre a consulta e aqui também vale como novo)
                None if tick == Tick::Repeat => match held.take() { Some(y) => { repeated += 1; (y, true) } None => continue },
                None => continue,
            };
            // troca de aba / repintura: não deixa o preto passageiro chegar ao espectador (segura o último quadro bom). O quadro
            // repetido já passou por aqui quando era novo.
            if !is_repeat && guard.hold(Instant::now(), dark_fraction(&yuv.y)) { held_win += 1; continue; }
            // fila do pacer acima de ~0,3 s do bitrate atual: a rede não dá vazão. Pula este quadro SEM codificar (a cadeia de
            // referência fica intacta; nada de quadro-chave, que só pioraria o congestionamento)
            if let Some(e) = enc.as_ref() { if queued.load(Ordering::Relaxed) * 8 > (e.bps as u64 * 3 / 10).max(250_000) { pacer_drops += 1; held = Some(yuv); continue; } }
            // Fonte saindo preta: jogo em tela cheia exclusiva ou conteúdo protegido (DRM) entregam quadros pretos à captura.
            // Amostra o brilho uma vez por segundo; 3 s seguidos de preto uniforme avisam a UI (uma vez), e o aviso some se voltar.
            if black_t.elapsed() >= Duration::from_secs(1) {
                black_t = Instant::now();
                let (mut sum, mut max, mut n) = (0u64, 0u8, 0u64);
                for v in yuv.y.iter().step_by(97) { sum += *v as u64; max = max.max(*v); n += 1; }
                let dark = n > 0 && sum / n <= 18 && max <= 24;
                black_run = if dark { black_run + 1 } else { 0 };
                if black_run == 3 { let _ = ev.try_send(json!({ "ev": "share.hint", "gen": gen, "hint": "black" })); }
                if !dark && black_said { let _ = ev.try_send(json!({ "ev": "share.hint", "gen": gen, "hint": "" })); }
                black_said = black_run >= 3;
            }
            let (tw, th) = (yuv.w, yuv.h);
            if enc.as_ref().map(|e| (e.w, e.h) != (tw, th)).unwrap_or(true) {
                let bps = if cfg.adaptive { enc_bps(dec.bps) } else if cfg.kbps == 0 { video::auto_bitrate(th, fps) } else { (cfg.kbps * 1000).min(MAX_BPS) };
                let tc = Instant::now();
                let made = H264Enc::new_codec_qp(out_codec, tw, th, fps as f32, bps, !sup.software, qpg.cap);
                if tc.elapsed() > Duration::from_millis(200) && std::env::var("BRIKS_EST_DEBUG").is_ok() { eprintln!("[lento] criar codificador {tw}x{th}: {} ms", tc.elapsed().as_millis()); }
                match made {
                    Ok(e) => {
                        id2.store(false, Ordering::Relaxed); since_idr = 0; last_idr = Instant::now(); enc = Some(e); announce = true; enc_fps = fps;   // codificador novo já começa com quadro-chave
                        if recreated_for_fault {
                            recreated_for_fault = false;
                            let st = if sup.software { "software" } else { "recovered" };
                            let nm = enc.as_ref().map(|e| e.name.clone()).unwrap_or_default();
                            eprintln!("[encoder] codificador recriado ({nm}), recriações {}, falhas {}", sup.total_recreations, sup.total_faults);
                            let _ = ev.try_send(json!({ "ev": "share.encoder", "gen": gen, "state": st, "encoder": nm, "recreations": sup.total_recreations, "faults": sup.total_faults }));
                        }
                    }
                    Err(e) => {
                        eprintln!("[encoder] não foi possível criar o codificador {tw}x{th}: {e}");
                        if !recreated_for_fault && sup.total_faults == 0 {
                            // 1ª criação (início da transmissão): sem codificador nenhum não há o que recuperar
                            closed_reason = format!("O codificador de vídeo não abriu: {e}. Clique em Tentar de novo; se repetir, atualize o driver da placa de vídeo."); break;
                        }
                        match sup.on_create_error(Instant::now(), &e.to_string()) {
                            Recovery::GiveUp(m) => { closed_reason = m; break; }
                            _ => { held = Some(yuv); std::thread::sleep(Duration::from_millis(100)); continue; }   // tenta de novo com este quadro, mesmo se a tela não mudar
                        }
                    }
                }
            }
            let e = enc.as_mut().unwrap();
            // seguindo a fonte: o controle de taxa conta com o FPS informado; com 24 quadros reais e 30 informados ele gastaria só 80% do bitrate
            let want_fps = cad.follow_fps().unwrap_or(fps).clamp(1, fps);
            if want_fps != enc_fps { let _ = e.set_fps(want_fps); enc_fps = want_fps; }
            if std::mem::take(&mut announce) {
                crate::netlog::write(&format!("transmissao iniciada: codificador {} ({}) codec {} {tw}x{th} a {fps} fps, {} kbps", e.name, if e.hardware { "hardware" } else { "software" }, out_codec.name(), e.bps / 1000));
                let _ = ev.try_send(json!({ "ev": "share.local", "gen": gen, "state": "on", "w": tw, "h": th, "fps": fps, "kbps": e.bps / 1000, "src_w": src.0, "src_h": src.1,
                    "label": cfg.label, "encoder": e.name, "codec": out_codec.name(), "hardware": e.hardware, "max_h": cfg.height, "max_fps": max_fps }));
            }
            if let (Some(d), Some(raw)) = (&dump, raw) { dump_png(d, &raw, src.0, src.1, tw, th); }
            since_idr += 1;
            // quadro-chave: só a pedido (rede de segurança a cada 30 s) (PLI/FIR/novo espectador) — mas no máximo um por segundo, para uma rajada de
            // pedidos numa rede congestionada não virar uma rajada de quadros enormes
            // Um IDR custa ~1 s do orçamento (600 KB a 4,5 Mbps em 1080p de texto): rajadas de IDR são o que o espectador vê como "reload".
            let min_gap = Duration::from_millis(if est.reason != "" { 3000 } else { 1500 });   // rede apertada: quadros-chave mais espaçados
            if since_idr >= fps * KEY_EVERY_S || (id2.load(Ordering::Relaxed) && last_idr.elapsed() >= min_gap) { id2.store(false, Ordering::Relaxed); e.force_idr(); since_idr = 0; last_idr = Instant::now(); idrs += 1; }
            let t0 = Instant::now();
            let slow = if cfg.label == SLOW_LABEL { TEST_SLOW_MS.load(Ordering::Relaxed) } else { 0 };
            if slow > 0 { std::thread::sleep(Duration::from_millis(slow)); }   // só testes: simula codificador lento
            let au = e.encode(&yuv);
            if t0.elapsed() > Duration::from_millis(200) && std::env::var("BRIKS_EST_DEBUG").is_ok() { eprintln!("[lento] codificar: {} ms", t0.elapsed().as_millis()); }
            held = Some(yuv);   // guardado para o reenvio de tela parada
            enc_ns += t0.elapsed().as_nanos();
            match au {
                Ok(list) => {
                    if list.is_empty() { skipped += 1; }
                    for au in list {
                        frames += 1; bytes += au.len() as u64;
                        let cur_bps = e.bps as u64;
                        let samples = ((last_send.elapsed().as_secs_f64().min(1.0)) * 90_000.0) as u32;
                        last_send = Instant::now();
                        queued.fetch_add(au.len() as u64, Ordering::Relaxed);
                        let _ = tx.send((au, samples.max(1), (cur_bps as f64 * 2.5) as u32));
                    }
                }
                Err(er) => {
                    let f = crate::mfenc::EncFault::of(&er);
                    eprintln!("[encoder] falha {:?} em {} ({}x{}): {f}; total de falhas {}", f.kind, e.name, tw, th, sup.total_faults + 1);
                    skipped += 1;
                    match sup.on_error(Instant::now(), &f) {
                        Recovery::Drop => {}   // o último quadro segue no receptor; tenta de novo no próximo quadro
                        Recovery::Recreate { software } => {
                            enc = None; recreated_for_fault = true; idrs += 1;
                            let _ = ev.try_send(json!({ "ev": "share.encoder", "gen": gen, "state": if software { "fallback" } else { "recovering" }, "hr": format!("0x{:08X}", f.hr as u32), "detail": f.msg, "recreations": sup.total_recreations, "faults": sup.total_faults }));
                        }
                        Recovery::GiveUp(m) => { eprintln!("[encoder] desistiu: {m}"); closed_reason = m; break; }
                    }
                    continue;
                }
            }
            sup.on_ok();
        }
        cap.stop();
        let _ = ev.try_send(json!({ "ev": "share.local", "gen": gen, "state": "off", "reason": closed_reason }));
        });
        if std::panic::catch_unwind(run).is_err() { let _ = ev_panic.try_send(json!({ "ev": "share.local", "gen": gen, "state": "off", "reason": "A transmissão parou por um erro interno" })); }
    });
    Ok(Sender { stop, idr })
}

/// Só para provas: salva o primeiro quadro enviado (já no tamanho de envio) como PNG.
fn dump_png(path: &PathBuf, bgra: &[u8], w: usize, h: usize, tw: usize, th: usize) {
    let y = video::bgra_to_i420(bgra, w, h, tw, th);
    let mut rgba = vec![255u8; tw * th * 4];
    video::i420_to_rgba(&y.y, &y.u, &y.v, (tw, tw / 2, tw / 2), tw, th, &mut rgba);
    if let Some(img) = image::RgbaImage::from_raw(tw as u32, th as u32, rgba) { let _ = img.save(path); }
}

// ---------------------------------------------------------------------------------------------------------------------
// Receptor
// ---------------------------------------------------------------------------------------------------------------------
#[derive(Default)]
pub struct RecvState {
    pub watching: AtomicBool,
    pub sink_path: Mutex<Option<PathBuf>>,
    pub frames: AtomicU64,
    /// Sobe a cada "assistir": o laço de RTP reinicia o montador/decodificador e pede um quadro-chave.
    pub epoch: AtomicU64,
    /// O decodificador falhou: o laço de RTP pede um quadro-chave.
    pub want_pli: AtomicBool,
    /// telemetria do decodificador: unidades entregues, erros, chamadas sem quadro de saída
    pub fed: AtomicU64,
    pub dec_err: AtomicU64,
    pub dec_none: AtomicU64,
    /// diagnóstico (1.5.22): tempo total gasto decodificando (ns) e qual decodificador está em uso
    pub dec_ns: AtomicU64,
    pub dec_name: Mutex<String>,
    /// pacotes de paridade (FEC) deste emissor, entregues pelo transporte e consumidos pelo laço de RTP
    pub fec_in: Mutex<Vec<Vec<u8>>>,
    /// avisa o emissor de que este espectador está perdendo pacotes (liga o FEC dele); o transporte instala quando há como avisar
    pub fec_ask: Mutex<Option<Arc<dyn Fn() + Send + Sync>>>,
}
impl RecvState {
    /// Entrega um pacote de paridade (já sem o byte do slot). Sem ninguém assistindo, ou com a fila cheia, é descartado.
    pub fn push_fec(&self, parity: Vec<u8>) {
        if !self.watching.load(Ordering::Relaxed) { return; }
        let mut q = self.fec_in.lock().unwrap_or_else(|e| e.into_inner());
        if q.len() < 256 { q.push(parity); }
    }
}

/// O pacote começa uma NAL? (FU-A só com o bit S; NAL única e STAP-A sempre.)
fn frame_start(payload: &[u8]) -> bool {
    match payload.first().map(|b| b & 0x1f) { Some(28) => payload.get(1).map(|b| b & 0x80 != 0).unwrap_or(false), Some(_) => true, None => false }
}

/// O pacote começa um quadro-chave? (SPS sozinho, STAP-A começando por SPS, IDR numa NAL única ou no início de um FU-A.)
fn key_start(p: &[u8]) -> bool {
    match p.first().map(|b| b & 0x1f) {
        Some(7) | Some(5) => true,
        Some(24) => p.get(3).map(|b| b & 0x1f == 7).unwrap_or(false),
        Some(28) => p.get(1).map(|b| b & 0x80 != 0 && b & 0x1f == 5).unwrap_or(false),
        _ => false,
    }
}

/// Buffer de reordenação/perda do receptor. Entrega as unidades de acesso em ordem; um buraco na sequência espera a
/// retransmissão (NACK) por até `WAIT`; passado o prazo, descarta até o próximo quadro-chave e sinaliza que é preciso pedi-lo (PLI).
pub struct JitterBuf {
    pkts: std::collections::BTreeMap<u64, webrtc::rtp::packet::Packet>,
    next: Option<u64>,
    hi: u64,
    /// (pacote que falta na frente da fila, desde quando): o prazo é POR PACOTE — um quadro-chave grande leva mais que o prazo
    /// para chegar, e um buraco que fechou não pode condenar o seguinte
    gap_since: Option<(u64, Instant)>,
    need_key: bool,
    pub lost_frames: u64,
    /// o próximo quadro-chave entregue vem depois de uma descontinuidade: quem decodifica deve recomeçar do zero
    pub fresh: bool,
    pub recovered: u64,   // buracos fechados por retransmissão dentro do prazo
    /// timestamp RTP (90 kHz) de cada unidade devolvida pelo último `pop`, na mesma ordem (para a reprodução suavizada, ver `Playout`)
    pub out_ts: Vec<u32>,
    codec: Codec,
}
const WAIT: Duration = Duration::from_millis(400);
/// Prazo de um buraco DENTRO do quadro-chave que vai destravar a imagem. Com a imagem já parada não há o que perder esperando a
/// retransmissão; desistir em 400 ms e pedir outro quadro-chave (mais 1 s ou mais) era o que esticava os congelamentos.
const WAIT_KEY: Duration = Duration::from_millis(1500);

impl JitterBuf {
    pub fn new() -> Self { Self::with_codec(Codec::H264) }
    pub fn with_codec(codec: Codec) -> Self { JitterBuf { pkts: Default::default(), next: None, hi: 0, gap_since: None, need_key: true, lost_frames: 0, fresh: false, recovered: 0, out_ts: vec![], codec } }
    fn pkt_key_start(&self, p: &Bytes) -> bool { if self.codec == Codec::H264 { key_start(p) } else { crate::codec::rtp_start(self.codec, p).1 } }
    fn pkt_frame_start(&self, p: &Bytes) -> bool { if self.codec == Codec::H264 { frame_start(p) } else { crate::codec::rtp_start(self.codec, p).0 } }
    fn ext(&mut self, seq: u16) -> u64 {
        if self.next.is_none() && self.pkts.is_empty() { self.hi = 1 << 16 | seq as u64; return self.hi; }
        let d = seq.wrapping_sub(self.hi as u16) as i16 as i64;
        let e = (self.hi as i64 + d).max(0) as u64;
        if e > self.hi { self.hi = e; }
        e
    }
    pub fn push(&mut self, p: webrtc::rtp::packet::Packet) {
        let e = self.ext(p.header.sequence_number);
        if self.next.is_none() { self.next = Some(e); }
        if e < self.next.unwrap() { return; }   // atrasado demais / duplicado
        self.pkts.insert(e, p);
    }
    /// Unidades de acesso prontas (Annex-B). O bool diz se é preciso pedir um quadro-chave.
    pub fn pop(&mut self, now: Instant) -> (Vec<Vec<u8>>, bool) {
        let (mut out, mut want_pli) = (vec![], false);
        self.out_ts.clear();
        loop {
            // Esperando quadro-chave: nada antes dele serve. Salta para o quadro-chave mais recente do buffer; se não há nenhum,
            // joga tudo fora. Sem isso, depois de uma rajada de perda o buffer gastaria um prazo inteiro em cada quadro velho e
            // quebrado, e nunca alcançaria o presente.
            if self.need_key {
                let cur = self.next.unwrap_or(0);
                // já está montando um quadro-chave: fica nele até fechar ou o prazo dele estourar (um quadro-chave mais novo, pedido
                // nesse meio-tempo, ainda está incompleto; trocar a cada pedido fazia nenhum dos dois terminar)
                let building_key = self.pkts.get(&cur).map(|p| self.pkt_key_start(&p.payload)).unwrap_or(false);
                if !building_key { match self.pkts.iter().rev().find(|(_, p)| self.pkt_key_start(&p.payload)).map(|(k, _)| *k) {
                    Some(k) if k > cur => { self.pkts.retain(|q, _| *q >= k); self.next = Some(k); self.gap_since = None; }
                    Some(_) => {}
                    None if !self.pkts.is_empty() => { self.pkts.clear(); self.next = Some(self.hi + 1); self.gap_since = None; }
                    None => {}
                } }
            }
            let Some(next) = self.next else { break };
            if self.pkts.contains_key(&next) {
                // O primeiro pacote tem de ser um INÍCIO de quadro. Quem entra no meio de um quadro (ou retoma depois de um
                // descarte) veria só a cauda dele; montada como se fosse um quadro inteiro, ela trava o decodificador sem erro.
                if !self.pkt_frame_start(&self.pkts[&next].payload) { self.pkts.remove(&next); self.next = Some(next + 1); self.need_key = true; continue; }
                // corrida contígua a partir de `next`, mesmo timestamp, até o marcador
                let ts = self.pkts[&next].header.timestamp;
                let mut end = None;
                let mut k = next;
                while let Some(p) = self.pkts.get(&k) {
                    if p.header.timestamp != ts { end = Some(k - 1); break; }   // quadro sem marcador (perdido no fim): fecha no pacote anterior
                    if p.header.marker { end = Some(k); break; }
                    k += 1;
                }
                let Some(end) = end else {
                    // k é o primeiro que falta. Se já há pacotes depois dele, é um buraco no meio do quadro: vale o mesmo prazo
                    if self.pkts.range(k..).next().is_none() { break; }   // só ainda chegando
                    let since = match self.gap_since { Some((q, t)) if q == k => t, _ => { self.gap_since = Some((k, now)); now } };
                    let wait = if self.need_key && self.pkt_key_start(&self.pkts[&next].payload) { WAIT_KEY } else { WAIT };
                    if now.duration_since(since) < wait && self.pkts.len() < 3000 { break; }
                    if std::env::var("BRIKS_SHARE_DEBUG").is_ok() { eprintln!("[jb] buraco no meio: falta {k}, quadro começa em {next}, ts {ts}, buffer {} pacotes [{:?}..{:?}], marcadores {:?}", self.pkts.len(), self.pkts.keys().next(), self.pkts.keys().next_back(), self.pkts.iter().filter(|(_, p)| p.header.marker).map(|(q, _)| *q).take(6).collect::<Vec<_>>()); }
                    self.gap_since = None; self.need_key = true; want_pli = true; self.lost_frames += 1;
                    let start = self.pkts.range(k..).find(|(_, p)| p.header.timestamp != ts).map(|(q, _)| *q);
                    match start { Some(st) => { self.pkts.retain(|q, _| *q >= st); self.next = Some(st); } None => { let last = *self.pkts.keys().next_back().unwrap(); self.pkts.clear(); self.next = Some(last + 1); } }
                    continue;
                };
                if self.gap_since.take().is_some() { self.recovered += 1; }
                let mut au = Vec::new();
                let mut ok = true;
                if self.codec == Codec::H264 {
                    let mut dep = H264Packet::default();
                    for q in next..=end { if let Some(p) = self.pkts.remove(&q) { match dep.depacketize(&p.payload) { Ok(b) => au.extend_from_slice(&b), Err(_) => ok = false } } }
                } else {
                    let ps: Vec<Bytes> = (next..=end).filter_map(|q| self.pkts.remove(&q)).map(|p| p.payload).collect();
                    match crate::codec::assemble(self.codec, ps.iter()) { Some(b) => au = b, None => ok = false }
                }
                self.next = Some(end + 1);
                let key = if self.codec == Codec::H264 { crate::mfenc::nals(&au).iter().any(|n| n.0 == 5) } else { crate::codec::is_key_frame(self.codec, &au) };
                if !ok || au.is_empty() { self.need_key = true; want_pli = true; self.lost_frames += 1; continue; }
                if self.need_key && !key { continue; }   // sem referência: espera o quadro-chave
                if self.need_key && self.lost_frames > 0 { self.fresh = true; }
                self.need_key = false;
                out.push(au); self.out_ts.push(ts);
            } else {
                if self.pkts.is_empty() { break; }
                let since = match self.gap_since { Some((q, t)) if q == next => t, _ => { self.gap_since = Some((next, now)); now } };
                if now.duration_since(since) < WAIT && self.pkts.len() < 3000 { break; }   // dá tempo à retransmissão
                // perda sem conserto: pula o quadro quebrado e tudo até o próximo quadro-chave
                if std::env::var("BRIKS_SHARE_DEBUG").is_ok() { eprintln!("[jb] buraco na frente: falta {next}, buffer {} pacotes [{:?}..{:?}]", self.pkts.len(), self.pkts.keys().next(), self.pkts.keys().next_back()); }
                self.gap_since = None; self.need_key = true; want_pli = true; self.lost_frames += 1;
                let (&lo, first) = self.pkts.iter().next().unwrap();
                let bad_ts = first.header.timestamp;
                let start = self.pkts.iter().find(|(_, p)| p.header.timestamp != bad_ts).map(|(k, _)| *k);
                match start { Some(st) => { self.pkts.retain(|k, _| *k >= st); self.next = Some(st); } None => { let last = *self.pkts.keys().next_back().unwrap_or(&lo); self.pkts.clear(); self.next = Some(last + 1); } }
            }
        }
        (out, want_pli)
    }
}

/// Fila entre o montador de quadros e a thread do decodificador. O canal para a thread é curto (6); antes, com ele cheio (decodificador
/// mais lento que a chegada: 27–115 ms por quadro medidos no rede.log a 30 qps), o quadro era DESCARTADO e os seguintes continuavam
/// indo ao decodificador — P-quadros sem a referência deles: blocos, "datamoshing" e legendas duplicadas até o próximo quadro-chave.
/// Agora nenhum quadro some no meio da cadeia: o excesso espera aqui; se o atraso passa de `LIMIT`, descarta TUDO até o próximo
/// quadro-chave (a última imagem boa fica parada um instante, em vez de imagem corrompida) e pede um.
pub struct DecodeFeed {
    pending: std::collections::VecDeque<Option<(Codec, Vec<u8>)>>,
    /// descartando até o próximo quadro-chave
    skipping: bool,
    /// quadros descartados por atraso do decodificador (telemetria)
    pub dropped: u64,
}
impl DecodeFeed {
    /// Quadros esperando o decodificador além dos 6 do canal: ~0,4 s a 30 qps.
    pub const LIMIT: usize = 12;
    pub fn new() -> Self { DecodeFeed { pending: Default::default(), skipping: false, dropped: 0 } }
    /// Reinício do decodificador (voltou a assistir, troca de codec, descontinuidade): o que esperava é do fluxo antigo.
    pub fn reset(&mut self) { self.pending.clear(); self.pending.push_back(None); self.skipping = false; }
    /// Entra uma unidade de acesso. Devolve true se é preciso pedir um quadro-chave (o decodificador ficou para trás).
    pub fn push(&mut self, codec: Codec, au: Vec<u8>, key: bool) -> bool {
        if self.skipping { if !key { self.dropped += 1; return false; } self.skipping = false; }
        // um quadro-chave torna dispensável tudo o que esperava antes dele: é a chance de tirar o atraso (um reinício pendente fica)
        if key && self.pending.len() > 2 { let n = self.pending.iter().filter(|m| m.is_some()).count(); self.dropped += n as u64; self.pending.retain(|m| m.is_none()); }
        if self.pending.len() >= Self::LIMIT {
            self.dropped += self.pending.iter().filter(|m| m.is_some()).count() as u64 + 1;
            self.pending.retain(|m| m.is_none());
            self.skipping = true;
            return true;
        }
        self.pending.push_back(Some((codec, au)));
        false
    }
    /// Entrega ao decodificador o que couber, em ordem. `send` devolve o item de volta quando o canal está cheio.
    pub fn drain(&mut self, mut send: impl FnMut(Option<(Codec, Vec<u8>)>) -> Result<(), Option<(Codec, Vec<u8>)>>) {
        while let Some(m) = self.pending.pop_front() { if let Err(back) = send(m) { self.pending.push_front(back); break; } }
    }
    pub fn waiting(&self) -> usize { self.pending.len() }
    /// Está descartando até o próximo quadro-chave (quem chama continua pedindo um enquanto isto for true).
    pub fn skipping(&self) -> bool { self.skipping }
}
impl Default for DecodeFeed { fn default() -> Self { Self::new() } }

/// Reprodução suavizada de quem assiste. Os quadros chegam em intervalos irregulares (variação da rede, rajada depois de uma
/// retransmissão) e eram mostrados assim que ficavam completos: travada seguida de acelerada. Aqui cada quadro tem a sua hora, tirada
/// do timestamp RTP do emissor (o ritmo em que ele foi capturado) mais um atraso que cobre a variação de chegada dos últimos 3 s (com
/// 10 ms de folga), limitado a `max`. O atraso sobe na hora em que a variação aparece e desce devagar (30 ms por segundo). Um quadro
/// que chega mais atrasado que o limite sai na hora, como antes. `max` = 0 desliga (cada quadro sai ao chegar).
pub struct Playout {
    q: std::collections::VecDeque<(Instant, Vec<u8>)>,
    /// (timestamp RTP, instante de chegada) de referência
    base: Option<(u32, Instant)>,
    /// (chegada, atraso de chegada sobre o ritmo do emissor, em s) dos últimos 3 s
    hist: std::collections::VecDeque<(Instant, f64)>,
    delay: f64,
    touched: Option<Instant>,
    last_due: Option<Instant>,
    max: f64,
}
impl Playout {
    /// Limite padrão do atraso. O áudio da transmissão não é atrasado junto, então cada ms aqui é ms de imagem atrás do som:
    /// 100 ms é o que ainda passa sem parecer dublagem fora de sincronia. `BRIKS_PLAYOUT_MAX_MS` muda (0 desliga).
    pub const MAX_MS: u64 = 100;
    pub fn new() -> Self { Self::with_max(Duration::from_millis(std::env::var("BRIKS_PLAYOUT_MAX_MS").ok().and_then(|v| v.parse().ok()).unwrap_or(Self::MAX_MS))) }
    pub fn with_max(max: Duration) -> Self { Playout { q: Default::default(), base: None, hist: Default::default(), delay: 0.0, touched: None, last_due: None, max: max.as_secs_f64() } }
    /// Descontinuidade (voltou a assistir, troca de codec, perda sem conserto): o que esperava é do fluxo antigo.
    pub fn reset(&mut self) { self.q.clear(); self.base = None; self.hist.clear(); self.delay = 0.0; self.touched = None; self.last_due = None; }
    /// Atraso em uso agora (ms), para a telemetria.
    pub fn delay_ms(&self) -> f64 { self.delay * 1000.0 }
    pub fn push(&mut self, au: Vec<u8>, ts: u32, now: Instant) {
        if self.max <= 0.0 { self.q.push_back((now, au)); return; }
        let (mut bts, mut bt) = *self.base.get_or_insert((ts, now));
        let mut dt = ts.wrapping_sub(bts) as i32 as f64 / 90_000.0;
        let mut d = now.saturating_duration_since(bt).as_secs_f64() - dt;
        if d.abs() > 1.0 {
            // salto de timestamp ou pausa do emissor (o relógio RTP dele anda no máximo 1 s por quadro): recomeça a referência daqui
            (bt, dt, d) = (now, 0.0, 0.0); self.base = Some((ts, now)); self.hist.clear();
        } else if dt > 1800.0 {
            // referência velha: traz para perto, mantendo o mesmo mapeamento (a diferença de timestamp não pode dar a volta)
            bt += Duration::from_secs_f64(dt); bts = ts; dt = 0.0; self.base = Some((bts, bt));
        }
        self.hist.push_back((now, d));
        while self.hist.front().map(|h| now.saturating_duration_since(h.0) > Duration::from_secs(3)).unwrap_or(false) { self.hist.pop_front(); }
        let (dmin, dmax) = self.hist.iter().fold((f64::MAX, f64::MIN), |a, h| (a.0.min(h.1), a.1.max(h.1)));
        let target = (dmax - dmin + 0.010).min(self.max);
        let since = self.touched.map(|t| now.saturating_duration_since(t).as_secs_f64()).unwrap_or(0.0);
        self.touched = Some(now);
        self.delay = if target >= self.delay { target } else { target.max(self.delay - 0.030 * since) };
        let off = (dt + dmin + self.delay).max(0.0);
        let mut due = (bt + Duration::from_secs_f64(off)).max(now);
        if let Some(l) = self.last_due { due = due.max(l); }   // nunca fora de ordem
        self.last_due = Some(due);
        self.q.push_back((due, au));
    }
    /// Quadros cuja hora chegou, em ordem.
    pub fn pop_due(&mut self, now: Instant) -> Vec<Vec<u8>> {
        let mut out = vec![];
        while self.q.front().map(|f| f.0 <= now).unwrap_or(false) { if let Some((_, au)) = self.q.pop_front() { out.push(au); } }
        out
    }
    /// Hora do próximo quadro em espera.
    pub fn next_due(&self) -> Option<Instant> { self.q.front().map(|f| f.0) }
    pub fn waiting(&self) -> usize { self.q.len() }
}
impl Default for Playout { fn default() -> Self { Self::new() } }

/// Lê o RTP da faixa de vídeo do peer. Só decodifica enquanto `watching`; sem espectador descarta os pacotes (custo ~zero).
/// `pli` pede um quadro-chave ao emissor (início, perda sem conserto, erro de decodificação).
/// Tabela payload type → codec negociada com o par (para o receptor reconhecer a troca de codec feita pelo emissor com `replace_track`).
pub async fn payload_codecs(rx: &webrtc::rtp_transceiver::rtp_receiver::RTCRtpReceiver) -> Vec<(u8, Codec)> {
    rx.get_parameters().await.codecs.iter().map(|c| (c.payload_type, Codec::from_mime(&c.capability.mime_type))).collect()
}

pub async fn run_remote_video(peer: String, track: Arc<TrackRemote>, st: Arc<RecvState>, ev: mpsc::Sender<Value>, pli: Arc<dyn Fn() + Send + Sync>, pts: Vec<(u8, Codec)>) {
    run_remote_video_as("share", peer, track, st, ev, pli, pts).await
}

/// Igual, com o prefixo dos eventos ("share" para a tela, "cam" para a câmera): `<prefixo>.remote` e `<prefixo>.rxstats`.
pub async fn run_remote_video_as(evp: &'static str, peer: String, track: Arc<TrackRemote>, st: Arc<RecvState>, ev: mpsc::Sender<Value>, pli: Arc<dyn Fn() + Send + Sync>, pts: Vec<(u8, Codec)>) {
    let (ev_remote, ev_rx) = (format!("{evp}.remote"), format!("{evp}.rxstats"));
    let mut rx_codec = Codec::from_mime(&track.codec().capability.mime_type);   // o que o emissor negociou (H264 / VP8 / VP9)
    let (dtx, drx) = std::sync::mpsc::sync_channel::<Option<(Codec, Vec<u8>)>>(6);
    let (st2, ev2, peer2, ev_remote2) = (st.clone(), ev.clone(), peer.clone(), ev_remote.clone());
    // decodificação numa thread: não trava o runtime
    std::thread::spawn(move || {
        let mut dec: Option<H264Dec> = None;
        let mut dec_codec = Codec::H264;
        let mut sink: Option<(PathBuf, FrameSink)> = None;
        let mut dims = (0usize, 0usize);
        let mut dry = 0u32;
        while let Ok(m) = drx.recv() {
            let Some((c, au)) = m else { dec = None; continue };   // reset (voltou a assistir)
            if dec.is_some() && c != dec_codec { dec = None; }   // o emissor trocou de codec
            if dec.is_none() { dec = H264Dec::new_codec(c).ok(); dec_codec = c; }
            let Some(d) = dec.as_mut() else { continue };
            let (t_dec, dname) = (Instant::now(), d.name);
            let r = d.decode(&au);
            st2.dec_ns.fetch_add(t_dec.elapsed().as_nanos() as u64, Ordering::Relaxed);
            if st2.fed.load(Ordering::Relaxed) == 0 || st2.dec_name.lock().map(|n| n.is_empty()).unwrap_or(false) { if let Ok(mut n) = st2.dec_name.lock() { *n = format!("{} ({})", dname, dec_codec.name()); } }
            st2.fed.fetch_add(1, Ordering::Relaxed);
            match &r { Err(_) => { st2.dec_err.fetch_add(1, Ordering::Relaxed); st2.want_pli.store(true, Ordering::Relaxed); } Ok(None) => { st2.dec_none.fetch_add(1, Ordering::Relaxed); dry += 1; } _ => { dry = 0; } }
            // rede de segurança: 15 unidades seguidas sem sair quadro = decodificador perdido; recria e pede quadro-chave
            if dry >= 15 { dry = 0; drop(r); dec = None; st2.want_pli.store(true, Ordering::Relaxed); continue; }
            if let Ok(Some((w, h, rgba))) = r {
                let path = st2.sink_path.lock().unwrap().clone();
                let Some(path) = path else { continue };
                if sink.as_ref().map(|s| s.0 != path).unwrap_or(true) { sink = FrameSink::create(&path).ok().map(|s| (path, s)); }
                if let Some((_, s)) = sink.as_mut() { s.write(w, h, rgba); }
                let n = st2.frames.fetch_add(1, Ordering::Relaxed) + 1;
                // o aviso do 1º quadro sai por try_send e se perde com o canal cheio: repete a cada 15 quadros (idempotente na UI)
                if dims != (w, h) || n == 1 || n % 15 == 0 { dims = (w, h); let (pid, slot) = split_key(&peer2); let _ = ev2.try_send(json!({ "ev": ev_remote2, "peer_id": pid, "slot": slot, "state": "frame", "w": w, "h": h })); }
            }
        }
    });
    let mut jb = JitterBuf::with_codec(rx_codec);
    let mut feed = DecodeFeed::new();
    // suavização só na transmissão de tela: na câmera (conversa) o atraso extra atrapalharia mais do que o ritmo irregular
    let mut play = if evp == "share" { Playout::new() } else { Playout::with_max(Duration::ZERO) };
    let mut fecrx = crate::fec::FecRx::new();
    let (mut loss_seen, mut fec_asked) = (0u64, Instant::now() - Duration::from_secs(10));
    let mut fine: Option<FineTimer> = None;   // esperas de 1 ms enquanto assiste (sem isso o Windows arredonda cada espera para ~15 ms)
    let to_dec = |m: Option<(Codec, Vec<u8>)>| dtx.try_send(m).map_err(|e| match e { std::sync::mpsc::TrySendError::Full(m) | std::sync::mpsc::TrySendError::Disconnected(m) => m });
    let mut my_epoch = 0u64;
    let (mut was, mut last_pli, mut last_frame) = (false, Instant::now() - Duration::from_secs(10), Instant::now());
    let (mut last_pkt, mut stalled) = (Instant::now(), false);   // rede de segurança: sem pacote nenhum por 3 s, a UI tira a última imagem
    let (mut plis, mut win, mut frames0) = (0u64, Instant::now(), 0u64);
    let mut shown_fps = 0u64;   // FPS publicado na última janela (ver `steady_fps`)
    let mut log_frames0 = 0u64;   // quadros decodificados até a última linha do rede.log
    let mut rx_bytes = 0u64;   // bytes de vídeo recebidos na janela de 1 s (vira o "kbps" que quem assiste vê)
    let mut ask = |last_pli: &mut Instant, plis: &mut u64, min: Duration| { if last_pli.elapsed() > min { pli(); *last_pli = Instant::now(); *plis += 1; } };
    loop {
        // acorda a cada 50 ms mesmo sem pacotes, para o prazo do buraco valer
        // (ou antes, na hora do próximo quadro da reprodução suavizada)
        let nap = play.next_due().map(|d| d.saturating_duration_since(Instant::now()).max(Duration::from_millis(1))).unwrap_or(Duration::from_millis(50)).min(Duration::from_millis(50));
        let pkt = match tokio::time::timeout(nap, track.read_rtp()).await { Ok(Ok((p, _))) => Some(p), Ok(Err(_)) => break, Err(_) => None };
        let w = st.watching.load(Ordering::Relaxed);
        if !w { was = false; stalled = false; last_pkt = Instant::now(); fine = None; continue; }
        if fine.is_none() && evp == "share" { fine = Some(FineTimer::acquire()); }
        if pkt.is_some() { last_pkt = Instant::now(); if stalled { stalled = false; let (pid, slot) = split_key(&peer); let _ = ev.try_send(json!({ "ev": ev_remote, "peer_id": pid, "slot": slot, "state": "resumed" })); } }
        else if !stalled && last_pkt.elapsed() > STALL_AFTER { stalled = true; let (pid, slot) = split_key(&peer); let _ = ev.try_send(json!({ "ev": ev_remote, "peer_id": pid, "slot": slot, "state": "stalled" })); }
        let ep = st.epoch.load(Ordering::Relaxed);
        if !was || ep != my_epoch {
            was = true; my_epoch = ep; jb = JitterBuf::with_codec(rx_codec);
            st.frames.store(0, Ordering::Relaxed); frames0 = 0; log_frames0 = 0; last_frame = Instant::now();
            feed.reset(); play.reset(); fecrx.reset();
            ask(&mut last_pli, &mut plis, Duration::ZERO);
        }
        if let Some(p) = pkt {
            // troca de codec no meio da transmissão (o emissor fez replace_track): outro payload type = outro empacotamento
            if let Some(c) = pts.iter().find(|(pt, _)| *pt == p.header.payload_type).map(|(_, c)| *c) {
                if c != rx_codec { rx_codec = c; jb = JitterBuf::with_codec(c); feed.reset(); play.reset(); fecrx.reset(); ask(&mut last_pli, &mut plis, Duration::ZERO); }
            }
            rx_bytes += p.payload.len() as u64;
            // FEC: este pacote pode ser o que faltava para a paridade refazer outro do mesmo grupo
            for r in fecrx.on_media(crate::fec::Pkt { seq: p.header.sequence_number, ts: p.header.timestamp, marker: p.header.marker, payload: p.payload.clone() }) { jb.push(fec_packet(r)); }
            jb.push(p);
        }
        // paridades que o transporte entregou (canal de dados): refazem o pacote perdido sem esperar a retransmissão
        let pars = std::mem::take(&mut *st.fec_in.lock().unwrap_or_else(|e| e.into_inner()));
        for par in pars { for r in fecrx.on_parity(&par) { jb.push(fec_packet(r)); } }
        // perda nova (buraco fechado por retransmissão ou pela paridade, ou quadro perdido): pede ao emissor que mantenha o FEC ligado
        let loss_now = jb.recovered + jb.lost_frames + fecrx.recovered;
        if loss_now != loss_seen { loss_seen = loss_now; if fec_asked.elapsed() > Duration::from_secs(2) { fec_asked = Instant::now(); let f = st.fec_ask.lock().unwrap_or_else(|e| e.into_inner()).clone(); if let Some(f) = f { f(); } } }
        let now = Instant::now();
        let (aus, want) = jb.pop(now);
        if std::mem::take(&mut jb.fresh) { play.reset(); feed.reset(); }   // descontinuidade: decodificador novo para o quadro-chave
        // cada quadro completo ganha a sua hora de saída (ver `Playout`); o que já venceu segue para o decodificador
        for (au, ts) in aus.into_iter().zip(jb.out_ts.iter().copied()) { play.push(au, ts, now); last_frame = now; }
        for au in play.pop_due(now) {
            let key = if rx_codec == Codec::H264 { crate::mfenc::nals(&au).iter().any(|n| n.0 == 5) } else { crate::codec::is_key_frame(rx_codec, &au) };
            // decodificador para trás: nada de pular um quadro no meio da cadeia (ver `DecodeFeed`); descarta até o quadro-chave pedido
            if feed.push(rx_codec, au, key) { ask(&mut last_pli, &mut plis, Duration::from_millis(800)); }
        }
        feed.drain(to_dec);
        if feed.skipping() { ask(&mut last_pli, &mut plis, Duration::from_millis(800)); }   // o pedido pode ter se perdido: repete até o quadro-chave chegar
        if want || st.want_pli.swap(false, Ordering::Relaxed) { ask(&mut last_pli, &mut plis, Duration::from_millis(800)); }
        // sem quadros há 2 s (o quadro-chave pedido se perdeu): pede outro
        if last_frame.elapsed() > Duration::from_secs(2) { ask(&mut last_pli, &mut plis, Duration::from_secs(2)); }
        if win.elapsed() >= Duration::from_secs(1) {
            let f = st.frames.load(Ordering::Relaxed);
            let (pid, slot) = split_key(&peer);
            let got = f.saturating_sub(frames0);
            shown_fps = steady_fps(shown_fps, got, win.elapsed().as_secs_f64());
            let _ = ev.try_send(json!({ "ev": ev_rx, "peer_id": pid, "slot": slot, "fps": shown_fps, "frames": got, "kbps": (rx_bytes as f64 * 8.0 / 1000.0 / win.elapsed().as_secs_f64().max(0.5)).round() as u64, "plis": plis, "lost_frames": jb.lost_frames, "recovered": jb.recovered,
                "fed": st.fed.load(Ordering::Relaxed), "dec_err": st.dec_err.load(Ordering::Relaxed), "dec_none": st.dec_none.load(Ordering::Relaxed), "fec_recovered": fecrx.recovered, "fec_parities": fecrx.received, "playout_ms": play.delay_ms().round() }));
            {   // diagnóstico de desempenho de quem assiste: uma linha a cada ~5 s no rede.log
                static RXLOGN: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
                if RXLOGN.fetch_add(1, Ordering::Relaxed) % 5 == 0 {
                    let (fed, ns) = (st.fed.load(Ordering::Relaxed), st.dec_ns.swap(0, Ordering::Relaxed));
                    let name = st.dec_name.lock().map(|n| n.clone()).unwrap_or_default();
                    crate::netlog::write(&format!("assistindo {pid}: decodificador {name}, decodificou {got} quadros/s (publicado {shown_fps:.1}), {} kbps, perdidos {}, recuperados {}, erros {}, sem saida {}, pedidos de quadro-chave {plis}, descartados por atraso do decodificador {}, suavizacao {:.0} ms, FEC refez {} pacotes ({} paridades), tempo medio de decodificar {:.1} ms (unidades {fed})",
                        (rx_bytes as f64 * 8.0 / 1000.0 / win.elapsed().as_secs_f64().max(0.5)).round() as u64, jb.lost_frames, jb.recovered, st.dec_err.load(Ordering::Relaxed), st.dec_none.load(Ordering::Relaxed), feed.dropped, play.delay_ms(), fecrx.recovered, fecrx.received,
                        // o tempo acumula desde a última linha (~5 s): divide pelos quadros do MESMO período. Antes dividia pelos quadros de
                        // 1 s só, e o log mostrava ~5x o tempo real ("27 ms" eram ~5,5 ms).
                        { let n5 = f.saturating_sub(log_frames0); log_frames0 = f; if n5 > 0 { ns as f64 / n5 as f64 / 1e6 } else { 0.0 } }));
                }
            }
            frames0 = f; win = Instant::now(); rx_bytes = 0;
        }
    }
    // a faixa terminou (a pessoa saiu ou a conexão fechou): sem este aviso a UI ficava com a última imagem parada
    let (pid, slot) = split_key(&peer);
    let _ = ev.send(json!({ "ev": ev_remote, "peer_id": pid, "slot": slot, "state": "off", "reason": "faixa encerrada" })).await;
}

/// Pacote RTP refeito pela paridade, no formato que o montador de quadros espera.
fn fec_packet(r: crate::fec::Pkt) -> webrtc::rtp::packet::Packet {
    let mut p = webrtc::rtp::packet::Packet::default();
    p.header.sequence_number = r.seq; p.header.timestamp = r.ts; p.header.marker = r.marker; p.payload = r.payload;
    p
}

/// Sem nenhum pacote de vídeo por este tempo, quem assiste é avisado ("stalled") para tirar a última imagem; ao voltar, "resumed".
pub const STALL_AFTER: Duration = Duration::from_secs(3);

#[cfg(test)]
mod jb_tests {
    use super::*;
    use webrtc::rtp::packet::Packet;

    fn pk(seq: u16, ts: u32, marker: bool, idr: bool) -> Packet {
        let mut p = Packet::default();
        p.header.sequence_number = seq; p.header.timestamp = ts; p.header.marker = marker;
        p.payload = Bytes::from(vec![if idr { 0x65 } else { 0x41 }, 1, 2, 3]);   // NAL único: 5 = IDR, 1 = P
        p
    }

    /// Decodificador lento: nenhum quadro some no meio da cadeia. O que não cabe espera; estourou o limite, descarta até o quadro-chave.
    #[test]
    fn decode_feed_never_skips_a_frame_inside_the_reference_chain() {
        let mut feed = DecodeFeed::new();
        let mut got: Vec<u8> = vec![];
        // canal "cheio": nada sai, tudo espera em ordem
        assert!(!feed.push(Codec::H264, vec![0], true));
        for i in 1..DecodeFeed::LIMIT as u8 { assert!(!feed.push(Codec::H264, vec![i], false)); }
        feed.drain(Err);
        assert_eq!(feed.waiting(), DecodeFeed::LIMIT);
        // o canal abre 3 vagas: saem os 3 primeiros, na ordem, e o resto continua esperando
        let mut room = 3;
        feed.drain(|m| if room > 0 { room -= 1; got.push(m.unwrap().1[0]); Ok(()) } else { Err(m) });
        assert_eq!(got, vec![0, 1, 2]);
        // enche de novo até estourar: descarta o que esperava, pede quadro-chave e ignora os P-quadros seguintes
        let mut asked = false;
        for i in 0..8u8 { asked |= feed.push(Codec::H264, vec![100 + i], false); }
        assert!(asked && feed.skipping() && feed.waiting() == 0, "estourou: pede quadro-chave e esvazia");
        assert!(!feed.push(Codec::H264, vec![200], false) && feed.waiting() == 0, "P-quadro sem referência não entra");
        // o quadro-chave retoma, e os seguintes entram normalmente
        assert!(!feed.push(Codec::H264, vec![201], true) && !feed.skipping());
        assert!(!feed.push(Codec::H264, vec![202], false));
        got.clear();
        feed.drain(|m| { got.push(m.unwrap().1[0]); Ok(()) });
        assert_eq!(got, vec![201, 202]);
        assert!(feed.dropped >= 9);
    }

    /// Um quadro-chave que chega com fila acumulada tira o atraso; o reinício do decodificador pendente não se perde.
    #[test]
    fn decode_feed_keyframe_clears_backlog_and_keeps_the_reset() {
        let mut feed = DecodeFeed::new();
        feed.reset();
        for i in 0..5u8 { feed.push(Codec::H264, vec![i], i == 0); }
        feed.push(Codec::H264, vec![50], true);
        let mut got = vec![];
        feed.drain(|m| { got.push(m.map(|x| x.1[0])); Ok(()) });
        assert_eq!(got, vec![None, Some(50)]);
    }

    /// Teto de QP: sobe com o codificador ignorando o bitrate, não reage a quadro-chave isolado, e a sondagem para descer fica mais rara.
    #[test]
    fn qp_governor_raises_on_sustained_overshoot_and_probes_down_with_backoff() {
        let mut g = QpGovernor::new();
        assert_eq!(g.cap, 28);
        // um segundo de estouro moderado (quadro-chave grande) e segundos marcados como transitórios não mudam nada
        assert_eq!(g.note(5e6, 3e6, false), None);
        assert_eq!(g.note(3e6, 3e6, false), None);
        for _ in 0..5 { assert_eq!(g.note(9e6, 3e6, true), None); }
        // dois segundos seguidos acima de 1,5x: sobe um degrau; os 2 s seguintes (codificador novo) não contam
        assert_eq!(g.note(4.6e6, 3e6, false), None);
        assert_eq!(g.note(4.6e6, 3e6, false), Some(34));
        assert_eq!(g.note(60e6, 3e6, false), None);
        assert_eq!(g.note(60e6, 3e6, false), None);
        // estouro enorme sobe na hora
        assert_eq!(g.note(60e6, 3e6, false), Some(40));
        for _ in 0..2 { g.note(3e6, 3e6, false); }
        // calmo: só tenta descer depois da espera (que dobrou duas vezes: 240 s)
        for k in 0..239 { assert_eq!(g.note(3e6, 3e6, false), None, "segundo {k}"); }
        assert_eq!(g.note(3e6, 3e6, false), Some(34));
        // nunca passa de 51
        let mut g = QpGovernor::new();
        let mut caps = vec![];
        for _ in 0..40 { if let Some(q) = g.note(90e6, 3e6, false) { caps.push(q); } }
        assert_eq!(caps, vec![34, 40, 51]);
    }

    /// Reprodução suavizada: quadros que chegam aos trancos saem no ritmo do emissor, com atraso limitado.
    #[test]
    fn playout_turns_bursty_arrivals_into_the_senders_rhythm() {
        let t0 = Instant::now();
        let ms = Duration::from_millis;
        let mut p = Playout::with_max(ms(100));
        // emissor a 30 qps (3000 ticks de 90 kHz por quadro); os quadros ímpares chegam 30 ms atrasados (intervalos de 63 e 3 ms)
        let mut out: Vec<(usize, Duration)> = vec![];
        let mut clock = t0;
        for i in 0..90usize {
            let arrive = t0 + Duration::from_micros(33_333 * i as u64) + if i % 2 == 1 { ms(30) } else { ms(0) };
            while clock < arrive { for au in p.pop_due(clock) { out.push((au[0] as usize, clock - t0)); } clock += ms(1); }
            p.push(vec![i as u8], 3000 * i as u32, arrive);
        }
        while p.waiting() > 0 { for au in p.pop_due(clock) { out.push((au[0] as usize, clock - t0)); } clock += ms(1); }
        assert_eq!(out.iter().map(|o| o.0).collect::<Vec<_>>(), (0..90).collect::<Vec<_>>(), "todos, em ordem");
        // depois de aprender a variação (1º segundo), o intervalo entre saídas fica em ~33 ms
        let gaps: Vec<i64> = out.windows(2).skip(30).map(|w| (w[1].1.as_millis() as i64) - (w[0].1.as_millis() as i64)).collect();
        assert!(gaps.iter().all(|g| (g - 33).abs() <= 3), "intervalos de saída: {gaps:?}");
        assert!(p.delay_ms() >= 30.0 && p.delay_ms() <= 60.0, "atraso cobre a variação (30 ms) com folga: {:.0} ms", p.delay_ms());
    }

    /// O atraso nunca passa do limite, a ordem nunca se inverte, `max` = 0 desliga e uma pausa longa do emissor recomeça a referência.
    #[test]
    fn playout_is_bounded_ordered_and_can_be_turned_off() {
        let t0 = Instant::now();
        let ms = Duration::from_millis;
        let mut p = Playout::with_max(ms(100));
        for i in 0..10u32 { p.push(vec![i as u8], 3000 * i, t0 + ms(33 * i as u64)); }
        // buraco de 400 ms (retransmissão) e depois a rajada dos 12 quadros que esperavam atrás dele
        let burst = t0 + ms(33 * 9 + 400);
        for i in 10..22u32 { p.push(vec![i as u8], 3000 * i, burst); }
        assert!(p.delay_ms() <= 100.0 + 1e-6);
        let (mut got, mut clock, mut last) = (vec![], t0, None);
        while p.waiting() > 0 { for au in p.pop_due(clock) { got.push(au[0]); last = Some(clock); } clock += ms(1); }
        assert_eq!(got, (0..22).collect::<Vec<u8>>());
        // o atraso é limitado: o último quadro da rajada sai no máximo 100 ms depois dela (antes do limite existir, sairia 11 quadros depois)
        let tail = last.unwrap() - burst;
        assert!(tail <= ms(110), "cauda da rajada: {tail:?}");
        let mut off = Playout::with_max(Duration::ZERO);
        off.push(vec![1], 0, t0); off.push(vec![2], 90_000, t0);
        assert_eq!(off.pop_due(t0), vec![vec![1], vec![2]], "desligado: sai ao chegar");
        let mut p = Playout::with_max(ms(100));
        p.push(vec![1], 0, t0); p.push(vec![2], 90_000, t0 + ms(5000));
        assert_eq!(p.pop_due(t0 + ms(5015)).len(), 2, "pausa longa: sai com a folga de 10 ms, sem segurar");
    }

    #[test]
    fn reorders_waits_for_retransmission_and_falls_back_to_keyframe() {
        let mut jb = JitterBuf::new();
        let t = Instant::now();
        // quadro-chave em 2 pacotes fora de ordem
        jb.push(pk(11, 100, true, true)); jb.push(pk(10, 100, false, true));
        let (f, pli) = jb.pop(t);
        assert!(f.len() == 1 && !pli, "reordenou e entregou o quadro-chave");
        // entrar no meio de um quadro: a cauda (FU-A sem bit de início) não pode ser entregue como quadro
        let mut mid = JitterBuf::new();
        let mut tail = pk(50, 900, true, true); tail.payload = Bytes::from(vec![0x7c, 0x45, 9, 9]);   // FU-A de IDR, bit E, sem S
        mid.push(tail);
        assert!(mid.pop(t).0.is_empty(), "cauda de quadro não é quadro");
        mid.push(pk(51, 1000, true, true));
        assert_eq!(mid.pop(t).0.len(), 1);
        // quadro P com buraco (12 faltando): espera; a retransmissão chega dentro do prazo
        jb.push(pk(13, 200, true, false));
        assert!(jb.pop(t).0.is_empty());
        jb.push(pk(12, 200, false, false));
        let (f, pli) = jb.pop(t + Duration::from_millis(50));
        assert!(f.len() == 1 && !pli && jb.recovered == 1, "NACK consertou");
        // buraco que não fecha (14 perdido): depois do prazo descarta, pede quadro-chave e ignora os P seguintes
        jb.push(pk(15, 300, true, false)); jb.push(pk(16, 400, true, false));
        assert!(jb.pop(t + Duration::from_millis(60)).0.is_empty());
        let (f, pli) = jb.pop(t + Duration::from_millis(600));
        assert!(f.is_empty() && pli && jb.lost_frames == 1, "prazo estourou: PLI");
        jb.push(pk(17, 500, true, false));
        assert!(jb.pop(t + Duration::from_millis(610)).0.is_empty(), "P sem referência não é entregue");
        jb.push(pk(18, 600, true, true));
        assert_eq!(jb.pop(t + Duration::from_millis(620)).0.len(), 1, "volta no quadro-chave");
    }
}

#[cfg(test)]
mod cadence_tests {
    use super::*;

    /// Roda a grade por `secs` segundos de relógio simulado. `arrivals` = instantes (ms) em que a captura entrega um quadro novo.
    /// Devolve os instantes (ms) e o tipo de cada envio.
    fn run(c: &mut Cadence, t0: Instant, secs: f64, arrivals: &[f64]) -> Vec<(f64, Tick)> {
        let (mut t, mut out, mut ai, mut fresh, mut held, mut last) = (0.0f64, vec![], 0usize, false, false, -5000.0f64);
        while t < secs * 1000.0 {
            while ai < arrivals.len() && arrivals[ai] <= t { fresh = true; ai += 1; }
            let now = t0 + Duration::from_secs_f64(t / 1000.0);
            match c.step(now, fresh, held, Duration::from_secs_f64((t - last) / 1000.0)) {
                Tick::Wait(d) => t += d.as_secs_f64() * 1000.0,
                Tick::Idle => {}
                k => { fresh = false; held = true; last = t; out.push((t, k)); }
            }
        }
        out
    }
    fn count(v: &[(f64, Tick)], k: Tick) -> usize { v.iter().filter(|x| x.1 == k).count() }
    fn worst_gap(v: &[(f64, Tick)]) -> f64 { v.windows(2).map(|w| w[1].0 - w[0].0).fold(0.0, f64::max) }

    /// Tela parada (um quadro no início e mais nada): antes saía 1 quadro por segundo; agora a cadência fica nos 30.
    #[test]
    fn a_still_screen_keeps_the_target_cadence_by_repeating_the_last_frame() {
        let t0 = Instant::now();
        let v = run(&mut Cadence::new(t0, 30), t0, 3.0, &[0.0]);
        assert!((89..=91).contains(&v.len()), "{} quadros em 3 s", v.len());
        assert_eq!(count(&v, Tick::Fresh), 1);
        assert!(worst_gap(&v[1..]) < 34.5, "repetições espaçadas por um período: {:.1} ms", worst_gap(&v[1..]));
        // comportamento antigo (diagnóstico): só o reenvio de 1 s
        let mut old = Cadence::new(t0, 30); old.repeat = false;
        let v = run(&mut old, t0, 3.0, &[0.0]);
        assert!((3..=4).contains(&v.len()), "{}", v.len());
    }

    /// Conteúdo que muda a 14 qps (o "14 FPS" do relato) numa transmissão de 30: saem 30 por segundo, 14 novos e 16 repetidos, e
    /// nenhum quadro novo é perdido.
    #[test]
    fn content_changing_at_14_fps_is_still_sent_at_30() {
        let t0 = Instant::now();
        let arr: Vec<f64> = (0..140).map(|i| 3.0 + i as f64 * 1000.0 / 14.0).collect();
        let v = run(&mut Cadence::new(t0, 30), t0, 10.0, &arr);
        assert!((298..=302).contains(&v.len()), "{} quadros em 10 s", v.len());
        assert_eq!(count(&v, Tick::Fresh), 140, "todo quadro novo é enviado");
        assert!(worst_gap(&v) < 46.0, "sem buracos: {:.1} ms", worst_gap(&v));
    }

    /// Captura no mesmo FPS da grade, mas fora de fase (o quadro chega 4 ms DEPOIS do instante da grade) e com oscilação: a
    /// tolerância espera por ele. Sem ela, cada período repetia o quadro velho e o novo ficava um período inteiro atrasado.
    #[test]
    fn a_capture_slightly_out_of_phase_is_neither_repeated_nor_dropped() {
        let t0 = Instant::now();
        let p = 1000.0 / 30.0;
        let arr: Vec<f64> = (0..300).map(|i| i as f64 * p + 4.0 + ((i * 7) % 5) as f64).collect();   // 4 a 8 ms depois da grade
        let v = run(&mut Cadence::new(t0, 30), t0, 10.0, &arr);
        assert_eq!(count(&v, Tick::Fresh), 300, "todos os quadros novos saem");
        assert_eq!(count(&v, Tick::Repeat), 0, "nenhuma repetição");
        // e o quadro sai até 1 ms depois de chegar (não espera o período seguinte)
        for (k, (t, _)) in v.iter().enumerate() { assert!(*t - arr[k] <= 1.5, "quadro {k}: chegou {:.1}, saiu {t:.1}", arr[k]); }
    }

    /// Chegadas de um filme a 24 qps capturado num monitor de 60 Hz: intervalos alternando 33,3 e 50 ms.
    fn film24(n: usize) -> Vec<f64> { let mut t = 5.0; (0..n).map(|i| { let v = t; t += if i % 2 == 0 { 100.0 / 3.0 } else { 50.0 }; v }).collect() }

    /// Seguir a fonte: filme a 24 qps numa transmissão de 30. Depois de reconhecer o ritmo (meio segundo), saem só os 24 quadros
    /// reais por segundo, cada um até 1,5 ms depois de chegar, sem nenhuma repetição.
    #[test]
    fn a_24_fps_film_is_sent_at_the_source_cadence_without_repeats() {
        let t0 = Instant::now();
        let arr = film24(240);
        let mut c = Cadence::new(t0, 30);
        let v = run(&mut c, t0, 10.0, &arr);
        assert_eq!(count(&v, Tick::Fresh), 240, "todo quadro do filme é enviado");
        assert_eq!(c.follow_fps(), Some(24));
        let late: Vec<&(f64, Tick)> = v.iter().filter(|x| x.0 > 1000.0).collect();
        assert_eq!(late.iter().filter(|x| x.1 == Tick::Repeat).count(), 0, "sem repetições depois de reconhecer o ritmo");
        let n = late.len() as f64 / 9.0;
        assert!((23.5..=24.5).contains(&n), "{n:.1} quadros por segundo");
        for x in &late { let a = arr.iter().copied().filter(|a| *a <= x.0).fold(0.0, f64::max); assert!(x.0 - a <= 1.5, "saiu {:.1}, chegou {a:.1}", x.0); }
        // com o modo desligado (diagnóstico), o comportamento de antes: 30 por segundo com repetições
        let mut old = Cadence::new(t0, 30); old.follow_source = false;
        let v = run(&mut old, t0, 10.0, &arr);
        assert!((298..=302).contains(&v.len()) && count(&v, Tick::Repeat) >= 55, "{} quadros, {} repetidos", v.len(), count(&v, Tick::Repeat));
    }

    /// O filme pausa (a fonte para de entregar): a grade volta e repete o último quadro no FPS alvo; ao voltar a tocar, segue de novo.
    #[test]
    fn a_paused_film_falls_back_to_the_grid_and_follows_again_when_it_resumes() {
        let t0 = Instant::now();
        let mut arr = film24(72);                                   // 3 s de filme
        let resume = 6000.0;
        arr.extend(film24(96).into_iter().map(|a| a + resume));     // pausa até os 6 s, depois mais 4 s
        let mut c = Cadence::new(t0, 30);
        let v = run(&mut c, t0, 10.0, &arr);
        let paused: Vec<&(f64, Tick)> = v.iter().filter(|x| x.0 > 3300.0 && x.0 < 5900.0).collect();
        assert!(paused.iter().all(|x| x.1 == Tick::Repeat) && (74..=80).contains(&paused.len()), "pausado: {} repetições em 2,6 s", paused.len());
        let first_repeat = v.iter().find(|x| x.1 == Tick::Repeat && x.0 > 2900.0).unwrap().0;
        assert!(first_repeat - arr[71] < 110.0, "a repetição começa logo depois de a fonte parar: {:.0} ms", first_repeat - arr[71]);
        let after: Vec<&(f64, Tick)> = v.iter().filter(|x| x.0 > 7000.0).collect();
        assert!(after.iter().all(|x| x.1 == Tick::Fresh), "voltou a seguir a fonte");
        assert_eq!(count(&v, Tick::Fresh), 168, "nenhum quadro do filme perdido");
    }

    /// Só fonte de ritmo regular entre 20 qps e o alvo é seguida: irregular (rolagem aos trancos), lenta (14 qps) e no ritmo do alvo ficam na grade.
    #[test]
    fn only_a_steady_source_between_20_fps_and_the_target_is_followed() {
        let t0 = Instant::now();
        for (nome, arr, segue) in [
            ("25 qps", (0..250).map(|i| 3.0 + i as f64 * 40.0).collect::<Vec<f64>>(), true),
            ("14 qps", (0..140).map(|i| 3.0 + i as f64 * 1000.0 / 14.0).collect(), false),
            ("30 qps", (0..300).map(|i| 3.0 + i as f64 * 1000.0 / 30.0).collect(), false),
            ("irregular", { let mut t = 3.0; (0..220usize).map(|i| { let v = t; t += [20.0, 70.0, 35.0, 90.0, 25.0][i % 5]; v }).collect() }, false),
        ] {
            let mut c = Cadence::new(t0, 30);
            let v = run(&mut c, t0, 10.0, &arr);
            assert_eq!(c.follow_fps().is_some(), segue, "{nome}");
            if !segue { assert!((296..=302).contains(&v.len()), "{nome}: na grade saem 30 por segundo ({} em 10 s)", v.len()); }
            else { assert!((248..=262).contains(&v.len()), "{nome}: {} em 10 s", v.len()); }
        }
        // transmissão de 60: um vídeo a 50 qps é seguido; um filme a 24 (abaixo de... não: 24 >= 20) também
        let mut c = Cadence::new(t0, 60);
        run(&mut c, t0, 5.0, &(0..250).map(|i| 3.0 + i as f64 * 20.0).collect::<Vec<f64>>());
        assert_eq!(c.follow_fps(), Some(50));
    }

    /// O laço travou meio segundo (codificador recriado, máquina ocupada): a grade recomeça dali, sem rajada para compensar.
    #[test]
    fn a_stall_restarts_the_grid_without_a_burst() {
        let t0 = Instant::now();
        let mut c = Cadence::new(t0, 30);
        assert_eq!(c.step(t0, true, false, Duration::ZERO), Tick::Fresh);
        let late = t0 + Duration::from_millis(500);
        assert_eq!(c.step(late, true, true, Duration::ZERO), Tick::Fresh);
        assert!(matches!(c.step(late + Duration::from_millis(1), true, true, Duration::ZERO), Tick::Wait(_)), "o próximo só um período depois");
        // troca de FPS (adaptação): o período novo vale a partir do quadro seguinte
        c.set_fps(15);
        assert_eq!(c.period(), Duration::from_micros(66_666));
    }

    #[test]
    fn viewer_chip_fps_is_frames_over_time_and_does_not_flicker_by_one() {
        assert_eq!(steady_fps(0, 31, 1.04), 30, "31 quadros em 1,04 s são 30 FPS (antes o chip mostrava 31)");
        assert_eq!(steady_fps(30, 29, 1.0), 30, "±1 quadro na borda da janela não muda o chip");
        assert_eq!(steady_fps(30, 31, 1.0), 30);
        assert_eq!(steady_fps(30, 28, 1.0), 28, "queda de verdade aparece");
        assert_eq!(steady_fps(30, 14, 1.0), 14);
        assert_eq!(steady_fps(30, 0, 1.0), 0, "sem quadros é zero (a UI usa isso para avisar)");
        assert_eq!(steady_fps(1, 0, 1.0), 0);
    }

    /// Ponta a ponta do emissor, sem rede: a fonte de teste entrega MENOS quadros que o FPS pedido, num ritmo regular (~22 qps: ela
    /// dorme um período inteiro e ainda gasta o tempo de desenhar), como um vídeo. Até a 1.5.25 o emissor completava até 30 repetindo
    /// quadros; desde o pedido de seguir a fonte (2026-11-14) sai cada quadro real, no ritmo dela, sem repetição.
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn a_slower_source_is_sent_at_its_own_cadence_or_filled_to_the_target() {
        let out = VideoOut::new();
        let (tx, mut rx) = mpsc::channel(256);
        let cfg = ShareCfg { kind: SourceKind::Test, height: 360, fps: 30, kbps: 0, label: "Cadencia".into(), audio: crate::procaudio::AudioMode::None, content: crate::adapt::Content::Detail, codec: Default::default(), adaptive: false };
        let s = start_sender(1, Arc::new(AtomicBool::new(false)), Arc::new(Mutex::new(Default::default())), cfg, out.clone(), tx, None, None).expect("emissor");   // `out` fica vivo: é ele que mantém o pacer rodando
        let mut wins = vec![];
        let t0 = Instant::now();
        while wins.len() < 6 && t0.elapsed() < Duration::from_secs(20) {
            if let Ok(Some(v)) = tokio::time::timeout(Duration::from_secs(3), rx.recv()).await { if v["ev"] == "share.stats" { wins.push(v); } }
        }
        s.stop();
        assert!(wins.len() >= 6, "sem estatísticas do emissor");
        for w in &wins[2..] {   // as duas primeiras janelas pegam a abertura do codificador
            eprintln!("enviado {} FPS (repetidos {}, a fonte ofereceu {}), {} kbps, {} ms/quadro", w["fps"], w["repeated"], w["offered_fps"], w["kbps"], w["work_ms"]);
            let (f, o) = (w["fps"].as_f64().unwrap_or(0.0), w["offered_fps"].as_f64().unwrap_or(0.0));
            // Dois modos válidos por janela: seguindo a fonte (enviado = oferecido) ou na grade (30, repetindo). Com a suíte inteira em
            // paralelo a fonte de teste perde a regularidade e o emissor volta, corretamente, para a grade; a regra de quando seguir
            // está nos testes de `Cadence` acima, com relógio simulado.
            if w["source_fps"].as_u64().unwrap_or(0) > 0 { assert!((f - o).abs() <= 2.5, "seguindo a fonte, devia enviar o que ela oferece: {w}"); }
            else { assert!((27.0..=31.5).contains(&f), "na grade, devia ficar no FPS alvo: {w}"); }
        }
    }
}

#[cfg(test)]
mod blackout_tests {
    use super::*;

    #[test]
    fn dark_fraction_counts_black_samples() {
        assert_eq!(dark_fraction(&vec![16u8; 10_000]), 1.0);
        assert_eq!(dark_fraction(&vec![200u8; 10_000]), 0.0);
        let mut f = vec![16u8; 10_000]; for v in f.iter_mut().take(1_000) { *v = 200; }   // moldura clara + conteúdo preto
        let d = dark_fraction(&f); assert!(d > 0.85 && d < 0.95, "{d}");
    }

    /// Troca de aba: ~1 s de conteúdo preto no meio da transmissão. O preto não pode chegar ao espectador (segura o último
    /// quadro bom), mas preto de verdade (que não passa) tem de ser entregue depois do prazo.
    #[test]
    fn tab_switch_blackout_holds_last_good_frame_then_releases_real_black() {
        let (mut g, t) = (BlackoutGuard::new(), Instant::now());
        let ms = |n: u64| t + Duration::from_millis(n);
        assert!(!g.hold(ms(0), 0.02), "quadro normal passa");
        // 1 s de preto: todos segurados
        for k in 1..=25 { assert!(g.hold(ms(k * 40), 0.998), "quadro preto {k} tem de ser segurado"); }
        assert_eq!(g.held, 25);
        assert!(!g.hold(ms(1100), 0.65), "o site pintou: passa");
        // de novo preto, mas agora não passa: depois de 1,5 s é conteúdo e é entregue
        assert!(g.hold(ms(2000), 0.999));
        assert!(g.hold(ms(3000), 0.999));
        assert!(!g.hold(ms(3600), 0.999), "preto persistente é entregue");
        assert!(!g.hold(ms(3640), 0.999), "e não volta a ser segurado enquanto continua preto");
        // sem quadro bom anterior (início já preto) também passa
        let mut g = BlackoutGuard::new();
        assert!(!g.hold(ms(0), 1.0));
    }
}

#[cfg(test)]
mod enc_recovery_tests {
    use super::*;
    use crate::mfenc::{EncFault, FaultKind};
    use std::sync::atomic::Ordering::Relaxed;

    fn stalled() -> EncFault { EncFault::new(FaultKind::Stalled, 0xC00D36B5u32 as i32, "o codificador não pediu entrada") }
    fn lost() -> EncFault { EncFault::from_hr(0x887A0007u32 as i32, "GPU reiniciada") }

    #[test]
    fn stalled_encoder_drops_frames_then_recreates_and_device_loss_recreates_at_once() {
        let (mut s, t) = (EncSupervisor::new(), Instant::now());
        let ms = |n: u64| t + Duration::from_millis(n);
        assert_eq!(s.on_error(ms(0), &stalled()), Recovery::Drop, "1ª falta de entrada: só descarta o quadro");
        assert_eq!(s.on_error(ms(40), &stalled()), Recovery::Drop);
        assert_eq!(s.on_error(ms(80), &stalled()), Recovery::Recreate { software: false }, "3 seguidas: recria o codificador");
        // recém-recriado: espera o backoff em vez de recriar de novo
        assert_eq!(s.on_error(ms(100), &stalled()), Recovery::Drop);
        s.on_ok();
        // GPU perdida (TDR): recria na hora
        assert_eq!(s.on_error(ms(2000), &lost()), Recovery::Recreate { software: false });
        // um sucesso zera a contagem seguida
        let mut s = EncSupervisor::new();
        s.on_error(ms(0), &stalled()); s.on_error(ms(40), &stalled()); s.on_ok();
        assert_eq!(s.on_error(ms(80), &stalled()), Recovery::Drop);
        // sem sucesso por 2 s: recria mesmo com poucos quadros
        let mut s = EncSupervisor::new();
        s.on_error(ms(0), &stalled());
        assert_eq!(s.on_error(ms(2100), &stalled()), Recovery::Recreate { software: false });
    }

    #[test]
    fn repeated_hardware_failures_fall_back_to_software_then_give_up_with_a_clear_message() {
        let (mut s, t) = (EncSupervisor::new(), Instant::now());
        let mut now = t;
        let mut step = |s: &mut EncSupervisor, f: &EncFault| { now += Duration::from_millis(1000); s.on_error(now, f) };
        for _ in 0..3 { assert_eq!(step(&mut s, &lost()), Recovery::Recreate { software: false }); }
        assert_eq!(step(&mut s, &lost()), Recovery::Recreate { software: true }, "hardware falhou 3 vezes: troca para software");
        assert!(s.software);
        for _ in 0..2 { assert_eq!(step(&mut s, &lost()), Recovery::Recreate { software: true }); }
        match step(&mut s, &lost()) {
            Recovery::GiveUp(m) => {
                assert!(m.contains("0x887A0007") && m.contains("Tentar de novo") && m.contains("placa de vídeo"), "mensagem: {m}");
                assert!(!m.contains("MFT") && !m.contains("ProcessOutput"), "sem jargão: {m}");
            }
            r => panic!("esperava desistir: {r:?}"),
        }
        let m = EncSupervisor::explain(&stalled());
        assert!(m.contains("parou de responder") && m.contains("0xC00D36B5"), "{m}");
    }

    fn frame(w: usize, h: usize, t: usize) -> video::Yuv {
        let mut y = vec![0u8; w * h];
        for (i, v) in y.iter_mut().enumerate() { *v = 40 + (((i % w) + t * 3 + (i / w)) % 180) as u8; }
        video::Yuv { w, h, y, u: vec![128; w * h / 4], v: vec![128; w * h / 4] }
    }

    /// Mesmo laço do emissor, sem captura: codifica, e a cada erro consulta o supervisor (tempo simulado: 40 ms por quadro).
    /// Devolve (unidades codificadas, quantas vieram de codificador de hardware, quadros descartados, desistiu?, recriações).
    fn drive(frames: usize, sup: &mut EncSupervisor) -> (usize, usize, usize, bool, u64) {
        let (t0, mut enc, mut out, mut hw, mut dropped, mut gave_up) = (Instant::now(), None::<video::H264Enc>, 0, 0, 0, false);
        for i in 0..frames {
            let now = t0 + Duration::from_millis(40 * i as u64);
            if enc.is_none() { let mut e = video::H264Enc::new_with(320, 180, 25.0, 800_000, !sup.software).expect("codificador"); e.inject = true; enc = Some(e); }
            let e = enc.as_mut().unwrap();
            match e.encode(&frame(320, 180, i)) {
                Ok(list) => { out += list.len(); if e.hardware { hw += list.len(); } sup.on_ok(); }
                Err(er) => { dropped += 1; match sup.on_error(now, &EncFault::of(&er)) { Recovery::Drop => {} Recovery::Recreate { .. } => enc = None, Recovery::GiveUp(_) => { gave_up = true; break; } } }
            }
        }
        (out, hw, dropped, gave_up, sup.total_recreations)
    }

    // Os testes abaixo mexem em estáticos globais de injeção: um de cada vez (o `inject` por codificador protege os outros testes).
    static LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    /// Falhas "o codificador não pediu entrada" por ~10 quadros seguidos no meio da transmissão: ela não para, recria o codificador
    /// (com quadro-chave) e volta a produzir quadros.
    #[test]
    fn injected_stalls_do_not_stop_the_stream_it_recreates_and_keeps_producing() {
        let _g = LOCK.lock().unwrap_or_else(|e| e.into_inner());
        video::FAULT_HR.store(0, Relaxed); video::FAULT_HW.store(false, Relaxed); video::FAULT_NEXT.store(0, Relaxed);
        let mut sup = EncSupervisor::new();
        // 20 quadros normais, depois 10 falhas seguidas, depois normal
        let (t0, mut enc) = (Instant::now(), None::<video::H264Enc>);
        let (mut after_fault, mut dropped, mut idr_after) = (0usize, 0usize, false);
        for i in 0..80 {
            let now = t0 + Duration::from_millis(40 * i as u64);
            if i == 20 { video::FAULT_NEXT.store(10, Relaxed); }
            if enc.is_none() { let mut e = video::H264Enc::new_with(320, 180, 25.0, 800_000, !sup.software).expect("codificador"); e.inject = true; enc = Some(e); }
            let e = enc.as_mut().unwrap();
            match e.encode(&frame(320, 180, i)) {
                Ok(list) => { if i >= 20 { after_fault += list.len(); } if i >= 20 && !list.is_empty() { if list.iter().any(|a| crate::mfenc::nals(a).iter().any(|n| n.0 == 5)) { idr_after = true; } } sup.on_ok(); }
                Err(er) => { dropped += 1; assert_eq!(EncFault::of(&er).kind, FaultKind::Stalled); match sup.on_error(now, &EncFault::of(&er)) { Recovery::Recreate { .. } => enc = None, Recovery::GiveUp(m) => panic!("não podia desistir: {m}"), Recovery::Drop => {} } }
            }
        }
        video::FAULT_NEXT.store(0, Relaxed);
        assert_eq!(dropped, 10, "só os quadros com falha são descartados");
        assert!(sup.total_recreations >= 1, "recriou o codificador");
        assert!(after_fault >= 30, "voltou a produzir quadros: {after_fault}");
        assert!(idr_after, "o codificador novo começa com quadro-chave (espectador volta a decodificar)");
    }

    /// O codificador de HARDWARE falha sempre (driver quebrado): depois das tentativas troca para software e a transmissão segue.
    #[test]
    fn hardware_encoder_that_always_fails_falls_back_to_software_and_the_stream_continues() {
        let _g = LOCK.lock().unwrap_or_else(|e| e.into_inner());
        video::FAULT_HR.store(0x887A0007u32 as i32, Relaxed); video::FAULT_HW.store(true, Relaxed); video::FAULT_NEXT.store(0, Relaxed);
        let mut sup = EncSupervisor::new();
        let (out, hw, _dropped, gave_up, rec) = drive(120, &mut sup);
        video::FAULT_HW.store(false, Relaxed); video::FAULT_HR.store(0, Relaxed);
        assert!(!gave_up, "não pode derrubar a transmissão");
        assert!(sup.software || rec == 0, "caiu para software (ou a máquina não tem codificador de hardware: nada a recuperar)");
        assert!(out >= 60, "a transmissão continua produzindo quadros com o codificador de software: {out}");
        assert!(sup.software && hw == 0 || rec == 0, "sem quadros vindos do hardware depois do fallback");
    }
}
