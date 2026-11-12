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
    /// false = sem adaptação (bitrate/FPS/resolução fixos, como pedido)
    pub adaptive: bool,
}

/// Faixa de vídeo em RTP cru: o empacotamento H264 e o espaçamento dos pacotes (pacer) são nossos.
pub fn new_video_track() -> Arc<TrackLocalStaticRTP> { new_video_track_slot(0) }
pub fn new_video_track_slot(slot: usize) -> Arc<TrackLocalStaticRTP> {
    new_video_track_named(if slot == 1 { VIDEO_TRACK_ID_2 } else { VIDEO_TRACK_ID }, if slot == 1 { "briks-screen-2" } else { "briks-screen" })
}
pub fn new_video_track_named(id: &str, stream: &str) -> Arc<TrackLocalStaticRTP> {
    Arc::new(TrackLocalStaticRTP::new(
        RTCRtpCodecCapability { mime_type: "video/H264".into(), clock_rate: 90_000, channels: 0, sdp_fmtp_line: H264_FMTP.into(), rtcp_feedback: vec![] },
        id.into(),
        stream.into(),
    ))
}

/// Saída de vídeo de uma sala: a faixa e o pacer, que vivem enquanto a sala existir. Parar e retomar a transmissão reaproveita
/// os dois, para a numeração e o relógio RTP continuarem (um salto quebraria o SRTP do outro lado).
#[derive(Clone)]
pub struct VideoOut {
    pub track: Arc<TrackLocalStaticRTP>,
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
}
impl VideoOut {
    /// API interna de publicação (ver docs/plans/transmissao-api-interna.md): entrega UMA unidade de acesso H264 (Annex-B, já com
    /// SPS/PPS no quadro-chave) ao pacer, que empacota (RTP, MTU 1180), espaça e guarda para retransmissão. `samples` = intervalo desde
    /// o quadro anterior em ticks de 90 kHz; `pace_bps` = ritmo de saída (convenção: 2,5 × bitrate, piso de 600 kbps).
    /// É o ponto em que um SFU/LiveKit pode receber o MESMO codificador: troque o consumidor do canal, não o laço de captura/codificação.
    pub fn publish(&self, au: Vec<u8>, samples: u32, pace_bps: u32) {
        self.queued.fetch_add(au.len() as u64, Ordering::Relaxed);
        let _ = self.tx.send((au, samples.max(1), pace_bps));
    }
    pub fn new() -> Self { Self::new_slot(0) }
    pub fn new_slot(slot: usize) -> Self { Self::with_track(new_video_track_slot(slot)) }
    /// Saída de vídeo da câmera (faixa `camera-video`, fluxo `briks-camera`).
    pub fn new_camera() -> Self { Self::with_track(new_video_track_named(CAMERA_TRACK_ID, "briks-camera")) }
    fn with_track(track: Arc<TrackLocalStaticRTP>) -> Self {
        let (tx, rx) = mpsc::unbounded_channel();
        // limitado: um espectador que despeja NACKs não pode encher a memória do emissor; com a fila cheia o pedido é
        // descartado (try_send) e o receptor pede de novo ou pede quadro-chave
        let (nack_tx, nack_rx) = mpsc::channel(256);
        let (queued, rtx_sent, rtx_skipped, nack_unique) = (Arc::new(AtomicU64::new(0)), Arc::new(AtomicU64::new(0)), Arc::new(AtomicU64::new(0)), Arc::new(AtomicU64::new(0)));
        let rtx_gap_ms = Arc::new(AtomicU64::new(150));
        let rtx_twice = Arc::new(AtomicBool::new(false));
        tokio::spawn(pacer_task(track.clone(), rx, nack_rx, queued.clone(), rtx_sent.clone(), rtx_skipped.clone(), nack_unique.clone(), rtx_gap_ms.clone(), rtx_twice.clone()));
        VideoOut { track, tx, nack_tx, queued, rtx_sent, rtx_skipped, nack_unique, rtx_gap_ms, rtx_twice }
    }
}

const HISTORY: usize = 2048;

/// Pacer: empacota cada unidade de acesso e solta os pacotes espaçados a `pace_bps` em vez de despejar o quadro inteiro de uma vez
/// (uma rajada de centenas de kB estoura filas e buffers no caminho e vira perda). Também responde aos NACKs: guarda os últimos
/// pacotes e retransmite pelo mesmo espaçamento, com orçamento de 25% da taxa e no máximo uma vez a cada 150 ms por pacote —
/// retransmissão sem limite, numa rede já congestionada, só aumenta o congestionamento.
async fn pacer_task(track: Arc<TrackLocalStaticRTP>, mut rx: mpsc::UnboundedReceiver<(Vec<u8>, u32, u32)>, mut nack_rx: mpsc::Receiver<Vec<u16>>, queued: Arc<AtomicU64>, rtx_sent: Arc<AtomicU64>, rtx_skipped: Arc<AtomicU64>, nack_unique: Arc<AtomicU64>, rtx_gap_ms: Arc<AtomicU64>, rtx_twice: Arc<AtomicBool>) {
    use std::collections::{HashMap, VecDeque};
    let mut pk = webrtc::rtp::packetizer::new_packetizer(1180, 0, 0, Box::new(H264Payloader::default()), Box::new(webrtc::rtp::sequence::new_random_sequencer()), 90_000);
    let mut budget_at = tokio::time::Instant::now();
    let mut hist: HashMap<u16, (webrtc::rtp::packet::Packet, Option<Instant>, bool, Instant, bool)> = HashMap::new();   // (pacote, última retransmissão, já pedido, enviado em)
    let mut order: VecDeque<u16> = VecDeque::new();
    let (mut pace, mut rtx_credit, mut rtx_t) = (2_000_000f64, 0f64, Instant::now());
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
                    if !std::mem::replace(asked, true) { nack_unique.fetch_add(1, Ordering::Relaxed); }
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
            m = rx.recv() => {
                let Some((au, samples, pace_bps)) = m else { break };
                pace = (pace_bps as f64).max(600_000.0);
                let len = au.len() as u64;
                let is_key = crate::mfenc::nals(&au).iter().any(|n| n.0 == 5);   // o receptor espera mais pelos pedaços de um quadro-chave
                let Ok(pkts) = pk.packetize(&Bytes::from(au), samples) else { queued.fetch_sub(len, Ordering::Relaxed); continue };
                if tokio::time::Instant::now().saturating_duration_since(budget_at) > Duration::from_millis(500) { budget_at = tokio::time::Instant::now(); }   // ficou parado: sem crédito acumulado
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

/// Bitrate do codificador para um orçamento de rede: desconta cabeçalhos RTP/UDP/IP e a folga das retransmissões.
fn enc_bps(budget: u32) -> u32 { (budget as f64 * 0.85) as u32 }

/// Inicia o emissor numa thread. `ev` recebe eventos JSON ("share.local"...); `preview` = arquivo de quadro compartilhado da prévia.
pub fn start_sender(gen: u64, idr: Arc<AtomicBool>, net: Arc<Mutex<crate::adapt::NetShared>>, cfg: ShareCfg, out: VideoOut, ev: mpsc::Sender<Value>, preview: Option<PathBuf>, dump: Option<PathBuf>) -> Result<Sender, String> {
    let cap = Capture::start(cfg.kind.clone(), cfg.fps, cfg.height, preview.is_some(), dump.is_some()).map_err(|e| e.to_string())?;
    let stop = Arc::new(AtomicBool::new(false));
    idr.store(true, Ordering::Relaxed);
    let (tx, queued) = (out.tx.clone(), out.queued.clone());   // bytes esperando no pacer
    let out_rtx = (out.rtx_sent.clone(), out.rtx_skipped.clone(), out.nack_unique.clone());
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
        let mut period = Duration::from_micros(1_000_000 / fps as u64);
        let mut cap = cap;
        let mut enc: Option<H264Enc> = None;
        let mut pv = preview.and_then(|p| FrameSink::create(&p).ok());
        let (mut last_send, mut next) = (Instant::now() - Duration::from_secs(5), Instant::now());
        let mut held: Option<video::Yuv> = None;
        let (mut frames, mut bytes, mut enc_ns, mut skipped) = (0u64, 0u64, 0u128, 0u64);
        let (mut win_t, mut win_cpu) = (Instant::now(), cpu_ms());
        let (mut cap_frames0, mut conv_ns0, mut nacks0) = (0u64, 0u64, 0u64);
        let mut since_idr = 0u32;
        let mut idrs = 0u64;   // quadros-chave forçados (telemetria: rajada de IDR = "reload" no espectador)
        let mut pacer_drops = 0u64;
        let mut just_changed = false;
        let mut recreated = Instant::now() - Duration::from_secs(4);
        let (mut black_t, mut black_run, mut black_said) = (Instant::now(), 0u32, false);
        let mut last_idr = Instant::now() - Duration::from_secs(5);
        let mut announce = false;
        let mut closed_reason = String::new();
        while !st.load(Ordering::Relaxed) {
            if cap.shared.closed.load(Ordering::Relaxed) {
                closed_reason = { let r = cap.shared.close_reason.lock().unwrap_or_else(|e| e.into_inner()).clone(); if r.is_empty() { "A janela foi fechada".into() } else { r } };
                break;
            }
            let now = Instant::now();
            if now < next { std::thread::sleep((next - now).min(Duration::from_millis(5))); continue; }
            next += period; if next < now { next = now + period; }
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
                    if !no_cpu && !std::mem::take(&mut just_changed) { ctl.note_cpu(work_ms, offered, sent_fps); }
                    if let Some(e) = enc.as_ref() { if frames >= 1 { ctl.note_encoder(sent_bps, e.bps as f64, sent_fps, e.w, e.h); } }
                    let d = ctl.decide(est.est);
                    if d.level != dec.level {
                        just_changed = true;
                        fps = d.level.fps; period = Duration::from_micros(1_000_000 / fps as u64);
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
                        if (want as f64) < e.bps as f64 * 0.7 && recreated.elapsed() >= Duration::from_secs(2) { enc = None; recreated = Instant::now(); }
                        else if diff > 0.05 && !e.set_bitrate(want) { enc = None; }
                    }
                    dec = d;
                }
                let (loss, rtt, jit) = worst.unwrap_or((0.0, 0.0, 0.0));
                let adapting = if !cfg.adaptive { "" } else if dec.cpu_limited || (ctl.cpu_idx > 0 && ctl.cpu_idx >= ctl.idx) { "cpu" } else if ctl.idx > 0 || (dec.bps as f64) < ctl.top_ceil() * 0.9 { "rede" } else { "" };
                let _ = ev.try_send(json!({ "ev": "share.stats", "gen": gen, "fps": (sent_fps * 10.0).round() / 10.0, "kbps": (sent_bps / 1000.0).round(),
                    "enc_ms": if frames > 0 { (enc_ns as f64 / frames as f64 / 1e6 * 10.0).round() / 10.0 } else { 0.0 }, "work_ms": (work_ms * 10.0).round() / 10.0,
                    "skipped": skipped, "w": tw, "h": th, "cpu_pct": ((cpu - win_cpu) / (secs * 1000.0) * 100.0).round(), "cap_dropped": dropped, "offered_fps": offered.round(),
                    "target_kbps": enc_bps(dec.bps) / 1000, "est_kbps": (est.est / 1000.0).round(), "target_fps": dec.level.fps, "target_h": dec.level.h, "level": ctl.idx,
                    "loss_pct": (loss * 1000.0).round() / 10.0, "rtt_ms": rtt.round(), "jitter_ms": jit.round(), "nacks": nacks.saturating_sub(nacks0), "plis": plis, "idrs": idrs,
                    "adapting": adapting, "reason": est.reason, "left_out": left_out, "overshoot": (ctl.overshoot * 100.0).round() / 100.0, "pacer_drops": std::mem::take(&mut pacer_drops), "lost_pkts": lost_new, "rtx": out_rtx.0.swap(0, Ordering::Relaxed), "rtx_skipped": out_rtx.1.swap(0, Ordering::Relaxed), "pacer_queue_kb": queued.load(Ordering::Relaxed) / 1000 }));
                frames = 0; bytes = 0; enc_ns = 0; skipped = 0; win_t = Instant::now(); win_cpu = cpu; cap_frames0 = capf; conv_ns0 = convn; nacks0 = nacks;
            }
            // quadro novo (ou reenvio de 1 s numa tela parada, para o decodificador não secar)
            let (yuv, pvw, raw, src) = {
                let mut l = cap.shared.latest.lock().unwrap_or_else(|e| e.into_inner());
                if l.w == 0 { continue; }
                let fresh = l.yuv.take();
                if let (Some(d), Some(y)) = (&dump, l.raw_yuv.take()) { let mut rgba = vec![255u8; y.w * y.h * 4]; video::i420_to_rgba(&y.y, &y.u, &y.v, (y.w, y.w / 2, y.w / 2), y.w, y.h, &mut rgba); if let Some(img) = image::RgbaImage::from_raw(y.w as u32, y.h as u32, rgba) { let _ = img.save(d); } }
                (fresh, l.preview.take(), l.raw.take(), (l.w, l.h))
            };
            if let (Some(p), Some((pw, ph, px))) = (pv.as_mut(), pvw) { p.write(pw, ph, &px); }
            let yuv = match (yuv, &held) {
                (Some(y), _) => y,
                (None, Some(_)) if last_send.elapsed() >= Duration::from_millis(1000) => held.take().unwrap(),
                _ => continue,
            };
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
                let made = H264Enc::new(tw, th, fps as f32, bps);
                if tc.elapsed() > Duration::from_millis(200) && std::env::var("BRIKS_EST_DEBUG").is_ok() { eprintln!("[lento] criar codificador {tw}x{th}: {} ms", tc.elapsed().as_millis()); }
                match made {
                    Ok(e) => { id2.store(false, Ordering::Relaxed); since_idr = 0; last_idr = Instant::now(); enc = Some(e); announce = true; }   // codificador novo já começa com quadro-chave
                    Err(e) => { closed_reason = format!("codificador: {e}"); break; }
                }
            }
            let e = enc.as_mut().unwrap();
            if std::mem::take(&mut announce) {
                let _ = ev.try_send(json!({ "ev": "share.local", "gen": gen, "state": "on", "w": tw, "h": th, "fps": fps, "kbps": e.bps / 1000, "src_w": src.0, "src_h": src.1,
                    "label": cfg.label, "encoder": e.name, "hardware": e.hardware, "max_h": cfg.height, "max_fps": max_fps }));
            }
            if let (Some(d), Some(raw)) = (&dump, raw) { dump_png(d, &raw, src.0, src.1, tw, th); }
            since_idr += 1;
            // quadro-chave: só a pedido (rede de segurança a cada 30 s) (PLI/FIR/novo espectador) — mas no máximo um por segundo, para uma rajada de
            // pedidos numa rede congestionada não virar uma rajada de quadros enormes
            // Um IDR custa ~1 s do orçamento (600 KB a 4,5 Mbps em 1080p de texto): rajadas de IDR são o que o espectador vê como "reload".
            let min_gap = Duration::from_millis(if est.reason != "" { 3000 } else { 1500 });   // rede apertada: quadros-chave mais espaçados
            if since_idr >= fps * 30 || (id2.load(Ordering::Relaxed) && last_idr.elapsed() >= min_gap) { id2.store(false, Ordering::Relaxed); e.force_idr(); since_idr = 0; last_idr = Instant::now(); idrs += 1; }
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
                Err(e) => { closed_reason = e.to_string(); break; }
            }
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
}
const WAIT: Duration = Duration::from_millis(400);
/// Prazo de um buraco DENTRO do quadro-chave que vai destravar a imagem. Com a imagem já parada não há o que perder esperando a
/// retransmissão; desistir em 400 ms e pedir outro quadro-chave (mais 1 s ou mais) era o que esticava os congelamentos.
const WAIT_KEY: Duration = Duration::from_millis(1500);

impl JitterBuf {
    pub fn new() -> Self { JitterBuf { pkts: Default::default(), next: None, hi: 0, gap_since: None, need_key: true, lost_frames: 0, fresh: false, recovered: 0 } }
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
        loop {
            // Esperando quadro-chave: nada antes dele serve. Salta para o quadro-chave mais recente do buffer; se não há nenhum,
            // joga tudo fora. Sem isso, depois de uma rajada de perda o buffer gastaria um prazo inteiro em cada quadro velho e
            // quebrado, e nunca alcançaria o presente.
            if self.need_key {
                let cur = self.next.unwrap_or(0);
                // já está montando um quadro-chave: fica nele até fechar ou o prazo dele estourar (um quadro-chave mais novo, pedido
                // nesse meio-tempo, ainda está incompleto; trocar a cada pedido fazia nenhum dos dois terminar)
                let building_key = self.pkts.get(&cur).map(|p| key_start(&p.payload)).unwrap_or(false);
                if !building_key { match self.pkts.iter().rev().find(|(_, p)| key_start(&p.payload)).map(|(k, _)| *k) {
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
                if !frame_start(&self.pkts[&next].payload) { self.pkts.remove(&next); self.next = Some(next + 1); self.need_key = true; continue; }
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
                    let wait = if self.need_key && key_start(&self.pkts[&next].payload) { WAIT_KEY } else { WAIT };
                    if now.duration_since(since) < wait && self.pkts.len() < 3000 { break; }
                    if std::env::var("BRIKS_SHARE_DEBUG").is_ok() { eprintln!("[jb] buraco no meio: falta {k}, quadro começa em {next}, ts {ts}, buffer {} pacotes [{:?}..{:?}], marcadores {:?}", self.pkts.len(), self.pkts.keys().next(), self.pkts.keys().next_back(), self.pkts.iter().filter(|(_, p)| p.header.marker).map(|(q, _)| *q).take(6).collect::<Vec<_>>()); }
                    self.gap_since = None; self.need_key = true; want_pli = true; self.lost_frames += 1;
                    let start = self.pkts.range(k..).find(|(_, p)| p.header.timestamp != ts).map(|(q, _)| *q);
                    match start { Some(st) => { self.pkts.retain(|q, _| *q >= st); self.next = Some(st); } None => { let last = *self.pkts.keys().next_back().unwrap(); self.pkts.clear(); self.next = Some(last + 1); } }
                    continue;
                };
                if self.gap_since.take().is_some() { self.recovered += 1; }
                let mut dep = H264Packet::default();
                let mut au = Vec::new();
                let mut ok = true;
                for q in next..=end { if let Some(p) = self.pkts.remove(&q) { match dep.depacketize(&p.payload) { Ok(b) => au.extend_from_slice(&b), Err(_) => ok = false } } }
                self.next = Some(end + 1);
                let key = crate::mfenc::nals(&au).iter().any(|n| n.0 == 5);
                if !ok || au.is_empty() { self.need_key = true; want_pli = true; self.lost_frames += 1; continue; }
                if self.need_key && !key { continue; }   // sem referência: espera o quadro-chave
                if self.need_key && self.lost_frames > 0 { self.fresh = true; }
                self.need_key = false;
                out.push(au);
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

/// Lê o RTP da faixa de vídeo do peer. Só decodifica enquanto `watching`; sem espectador descarta os pacotes (custo ~zero).
/// `pli` pede um quadro-chave ao emissor (início, perda sem conserto, erro de decodificação).
pub async fn run_remote_video(peer: String, track: Arc<TrackRemote>, st: Arc<RecvState>, ev: mpsc::Sender<Value>, pli: Arc<dyn Fn() + Send + Sync>) {
    run_remote_video_as("share", peer, track, st, ev, pli).await
}

/// Igual, com o prefixo dos eventos ("share" para a tela, "cam" para a câmera): `<prefixo>.remote` e `<prefixo>.rxstats`.
pub async fn run_remote_video_as(evp: &'static str, peer: String, track: Arc<TrackRemote>, st: Arc<RecvState>, ev: mpsc::Sender<Value>, pli: Arc<dyn Fn() + Send + Sync>) {
    let (ev_remote, ev_rx) = (format!("{evp}.remote"), format!("{evp}.rxstats"));
    let (dtx, drx) = std::sync::mpsc::sync_channel::<Option<Vec<u8>>>(6);
    let (st2, ev2, peer2, ev_remote2) = (st.clone(), ev.clone(), peer.clone(), ev_remote.clone());
    // decodificação numa thread: não trava o runtime
    std::thread::spawn(move || {
        let mut dec: Option<H264Dec> = None;
        let mut sink: Option<(PathBuf, FrameSink)> = None;
        let mut dims = (0usize, 0usize);
        let mut dry = 0u32;
        while let Ok(m) = drx.recv() {
            let Some(au) = m else { dec = None; continue };   // reset (voltou a assistir)
            if dec.is_none() { dec = H264Dec::new().ok(); }
            let Some(d) = dec.as_mut() else { continue };
            let r = d.decode(&au);
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
    let mut jb = JitterBuf::new();
    let mut my_epoch = 0u64;
    let (mut was, mut last_pli, mut last_frame) = (false, Instant::now() - Duration::from_secs(10), Instant::now());
    let (mut plis, mut win, mut frames0) = (0u64, Instant::now(), 0u64);
    let mut ask = |last_pli: &mut Instant, plis: &mut u64, min: Duration| { if last_pli.elapsed() > min { pli(); *last_pli = Instant::now(); *plis += 1; } };
    loop {
        // acorda a cada 50 ms mesmo sem pacotes, para o prazo do buraco valer
        let pkt = match tokio::time::timeout(Duration::from_millis(50), track.read_rtp()).await { Ok(Ok((p, _))) => Some(p), Ok(Err(_)) => break, Err(_) => None };
        let w = st.watching.load(Ordering::Relaxed);
        if !w { was = false; continue; }
        let ep = st.epoch.load(Ordering::Relaxed);
        if !was || ep != my_epoch {
            was = true; my_epoch = ep; jb = JitterBuf::new();
            st.frames.store(0, Ordering::Relaxed); frames0 = 0; last_frame = Instant::now();
            let _ = dtx.try_send(None);
            ask(&mut last_pli, &mut plis, Duration::ZERO);
        }
        if let Some(p) = pkt { jb.push(p); }
        let (aus, want) = jb.pop(Instant::now());
        if std::mem::take(&mut jb.fresh) { let _ = dtx.try_send(None); }   // descontinuidade: decodificador novo para o quadro-chave
        for au in aus { if dtx.try_send(Some(au)).is_err() { ask(&mut last_pli, &mut plis, Duration::from_millis(800)); } last_frame = Instant::now(); }
        if want || st.want_pli.swap(false, Ordering::Relaxed) { ask(&mut last_pli, &mut plis, Duration::from_millis(800)); }
        // sem quadros há 2 s (o quadro-chave pedido se perdeu): pede outro
        if last_frame.elapsed() > Duration::from_secs(2) { ask(&mut last_pli, &mut plis, Duration::from_secs(2)); }
        if win.elapsed() >= Duration::from_secs(1) {
            let f = st.frames.load(Ordering::Relaxed);
            let (pid, slot) = split_key(&peer);
            let _ = ev.try_send(json!({ "ev": ev_rx, "peer_id": pid, "slot": slot, "fps": f.saturating_sub(frames0), "plis": plis, "lost_frames": jb.lost_frames, "recovered": jb.recovered,
                "fed": st.fed.load(Ordering::Relaxed), "dec_err": st.dec_err.load(Ordering::Relaxed), "dec_none": st.dec_none.load(Ordering::Relaxed) }));
            frames0 = f; win = Instant::now();
        }
    }
}

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
