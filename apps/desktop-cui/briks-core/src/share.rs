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
pub fn new_video_track() -> Arc<TrackLocalStaticRTP> {
    Arc::new(TrackLocalStaticRTP::new(
        RTCRtpCodecCapability { mime_type: "video/H264".into(), clock_rate: 90_000, channels: 0, sdp_fmtp_line: H264_FMTP.into(), rtcp_feedback: vec![] },
        VIDEO_TRACK_ID.into(),
        "briks-screen".into(),
    ))
}

/// Saída de vídeo de uma sala: a faixa e o pacer, que vivem enquanto a sala existir. Parar e retomar a transmissão reaproveita
/// os dois, para a numeração e o relógio RTP continuarem (um salto quebraria o SRTP do outro lado).
#[derive(Clone)]
pub struct VideoOut {
    pub track: Arc<TrackLocalStaticRTP>,
    tx: mpsc::UnboundedSender<(Vec<u8>, u32, u32)>,
    /// pedidos de retransmissão (números de sequência) vindos do RTCP dos espectadores
    pub nack_tx: mpsc::UnboundedSender<Vec<u16>>,
    queued: Arc<AtomicU64>,
    pub rtx_sent: Arc<AtomicU64>,
    pub rtx_skipped: Arc<AtomicU64>,
    /// pacotes pedidos pela PRIMEIRA vez (perda real antes do conserto; pedidos repetidos do mesmo pacote não contam)
    pub nack_unique: Arc<AtomicU64>,
}
impl VideoOut {
    pub fn new() -> Self {
        let track = new_video_track();
        let (tx, rx) = mpsc::unbounded_channel();
        let (nack_tx, nack_rx) = mpsc::unbounded_channel();
        let (queued, rtx_sent, rtx_skipped, nack_unique) = (Arc::new(AtomicU64::new(0)), Arc::new(AtomicU64::new(0)), Arc::new(AtomicU64::new(0)), Arc::new(AtomicU64::new(0)));
        tokio::spawn(pacer_task(track.clone(), rx, nack_rx, queued.clone(), rtx_sent.clone(), rtx_skipped.clone(), nack_unique.clone()));
        VideoOut { track, tx, nack_tx, queued, rtx_sent, rtx_skipped, nack_unique }
    }
}

const HISTORY: usize = 2048;

/// Pacer: empacota cada unidade de acesso e solta os pacotes espaçados a `pace_bps` em vez de despejar o quadro inteiro de uma vez
/// (uma rajada de centenas de kB estoura filas e buffers no caminho e vira perda). Também responde aos NACKs: guarda os últimos
/// pacotes e retransmite pelo mesmo espaçamento, com orçamento de 25% da taxa e no máximo uma vez a cada 150 ms por pacote —
/// retransmissão sem limite, numa rede já congestionada, só aumenta o congestionamento.
async fn pacer_task(track: Arc<TrackLocalStaticRTP>, mut rx: mpsc::UnboundedReceiver<(Vec<u8>, u32, u32)>, mut nack_rx: mpsc::UnboundedReceiver<Vec<u16>>, queued: Arc<AtomicU64>, rtx_sent: Arc<AtomicU64>, rtx_skipped: Arc<AtomicU64>, nack_unique: Arc<AtomicU64>) {
    use std::collections::{HashMap, VecDeque};
    let mut pk = webrtc::rtp::packetizer::new_packetizer(1180, 0, 0, Box::new(H264Payloader::default()), Box::new(webrtc::rtp::sequence::new_random_sequencer()), 90_000);
    let mut budget_at = tokio::time::Instant::now();
    let mut hist: HashMap<u16, (webrtc::rtp::packet::Packet, Option<Instant>, bool, Instant)> = HashMap::new();   // (pacote, última retransmissão, já pedido, enviado em)
    let mut order: VecDeque<u16> = VecDeque::new();
    let (mut pace, mut rtx_credit, mut rtx_t) = (2_000_000f64, 0f64, Instant::now());
    loop {
        tokio::select! {
            biased;
            n = nack_rx.recv() => {
                let Some(seqs) = n else { break };
                // crédito de retransmissão: 25% da taxa do pacer, acumulando no máximo 0,5 s
                rtx_credit = (rtx_credit + rtx_t.elapsed().as_secs_f64() * pace * 0.25 / 8.0).min(pace * 0.25 / 8.0 * 0.5);
                rtx_t = Instant::now();
                for sq in seqs {
                    let Some((p, last, asked, sent_at)) = hist.get_mut(&sq) else { rtx_skipped.fetch_add(1, Ordering::Relaxed); continue };
                    if !std::mem::replace(asked, true) { nack_unique.fetch_add(1, Ordering::Relaxed); }
                    // pacote velho: o receptor já desistiu desse quadro (espera no máximo 400 ms) e vai pedir quadro-chave; reenviar só
                    // gastaria banda. O gerador de NACK do outro lado continua pedindo por muito tempo.
                    if sent_at.elapsed() > Duration::from_millis(600) { rtx_skipped.fetch_add(1, Ordering::Relaxed); continue; }
                    let size = (p.payload.len() + 12) as f64;
                    if last.map(|t| t.elapsed() < Duration::from_millis(150)).unwrap_or(false) || rtx_credit < size { rtx_skipped.fetch_add(1, Ordering::Relaxed); continue; }
                    rtx_credit -= size; *last = Some(Instant::now());
                    let _ = track.write_rtp(p).await;
                    rtx_sent.fetch_add(1, Ordering::Relaxed);
                    budget_at = budget_at.max(tokio::time::Instant::now()) + Duration::from_secs_f64(size * 8.0 / pace);
                }
            }
            m = rx.recv() => {
                let Some((au, samples, pace_bps)) = m else { break };
                pace = (pace_bps as f64).max(600_000.0);
                let len = au.len() as u64;
                let Ok(pkts) = pk.packetize(&Bytes::from(au), samples) else { queued.fetch_sub(len, Ordering::Relaxed); continue };
                if tokio::time::Instant::now().saturating_duration_since(budget_at) > Duration::from_millis(500) { budget_at = tokio::time::Instant::now(); }   // ficou parado: sem crédito acumulado
                for p in pkts {
                    let now = tokio::time::Instant::now();
                    if budget_at > now + Duration::from_millis(2) { tokio::time::sleep_until(budget_at).await; }
                    let _ = track.write_rtp(&p).await;
                    budget_at = budget_at.max(now - Duration::from_millis(20)) + Duration::from_secs_f64((p.payload.len() + 12) as f64 * 8.0 / pace);
                    let sq = p.header.sequence_number;
                    if hist.insert(sq, (p, None, false, Instant::now())).is_none() { order.push_back(sq); }
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
        let mut pacer_drops = 0u64;
        let mut last_idr = Instant::now() - Duration::from_secs(5);
        let mut announce = false;
        let mut closed_reason = String::new();
        while !st.load(Ordering::Relaxed) {
            if cap.shared.closed.load(Ordering::Relaxed) { closed_reason = "A janela foi fechada".into(); break; }
            let now = Instant::now();
            if now < next { std::thread::sleep((next - now).min(Duration::from_millis(5))); continue; }
            next += period; if next < now { next = now + period; }
            // ---- uma vez por segundo: mede, estima a banda, decide o nível e publica a telemetria ----
            if win_t.elapsed() >= Duration::from_secs(1) {
                let secs = win_t.elapsed().as_secs_f64();
                let cpu = cpu_ms();
                let sent_bps = bytes as f64 * 8.0 / secs;
                let sent_fps = frames as f64 / secs;
                let capf = cap.shared.frames.load(Ordering::Relaxed);
                let dropped = cap.shared.dropped.swap(0, Ordering::Relaxed);
                let offered = (capf - cap_frames0 + dropped) as f64 / secs;
                let convn = cap.shared.conv_ns.load(Ordering::Relaxed);
                let work_ms = if frames > 0 { ((convn - conv_ns0) as f64 + enc_ns as f64) / frames as f64 / 1e6 } else { 0.0 };
                let (worst, (nacks, plis)) = { let n = net.lock().unwrap(); (n.worst(), n.totals()) };
                let lost_new = out_rtx.2.swap(0, Ordering::Relaxed);
                let nack_ratio = if bytes > 0 { lost_new as f32 / (bytes as f32 / 1100.0).max(1.0) } else { 0.0 };
                let (tw, th) = enc.as_ref().map(|e| (e.w, e.h)).unwrap_or((0, 0));
                if cfg.adaptive {
                    est.set_ceil(ctl.top_ceil());
                    est.update(Instant::now(), sent_bps, worst, nack_ratio);
                    if !no_cpu { ctl.note_cpu(work_ms, offered, sent_fps); }
                    if let Some(e) = enc.as_ref() { if frames >= 2 { ctl.note_encoder(sent_bps, e.bps as f64); } }
                    let d = ctl.decide(est.est);
                    if d.level != dec.level {
                        fps = d.level.fps; period = Duration::from_micros(1_000_000 / fps as u64);
                        cap.shared.retarget(fps, d.level.h);
                        if let Some(e) = enc.as_mut() { if !e.set_fps(fps) { enc = None; } }
                        announce = true;
                    }
                    if let Some(e) = enc.as_mut() {
                        let diff = (enc_bps(d.bps) as f64 - e.bps as f64).abs() / e.bps.max(1) as f64;
                        if diff > 0.05 && !e.set_bitrate(enc_bps(d.bps)) { enc = None; }   // sem ajuste ao vivo: recria (sai um quadro-chave)
                    }
                    dec = d;
                }
                let (loss, rtt, jit) = worst.unwrap_or((0.0, 0.0, 0.0));
                let adapting = if !cfg.adaptive { "" } else if dec.cpu_limited { "cpu" } else if ctl.idx > 0 || (dec.bps as f64) < ctl.top_ceil() * 0.9 { "rede" } else { "" };
                let _ = ev.try_send(json!({ "ev": "share.stats", "gen": gen, "fps": (sent_fps * 10.0).round() / 10.0, "kbps": (sent_bps / 1000.0).round(),
                    "enc_ms": if frames > 0 { (enc_ns as f64 / frames as f64 / 1e6 * 10.0).round() / 10.0 } else { 0.0 }, "work_ms": (work_ms * 10.0).round() / 10.0,
                    "skipped": skipped, "w": tw, "h": th, "cpu_pct": ((cpu - win_cpu) / (secs * 1000.0) * 100.0).round(), "cap_dropped": dropped, "offered_fps": offered.round(),
                    "target_kbps": enc_bps(dec.bps) / 1000, "est_kbps": (est.est / 1000.0).round(), "target_fps": dec.level.fps, "target_h": dec.level.h, "level": ctl.idx,
                    "loss_pct": (loss * 1000.0).round() / 10.0, "rtt_ms": rtt.round(), "jitter_ms": jit.round(), "nacks": nacks.saturating_sub(nacks0), "plis": plis,
                    "adapting": adapting, "reason": est.reason, "overshoot": (ctl.overshoot * 100.0).round() / 100.0, "pacer_drops": std::mem::take(&mut pacer_drops), "lost_pkts": lost_new, "rtx": out_rtx.0.swap(0, Ordering::Relaxed), "rtx_skipped": out_rtx.1.swap(0, Ordering::Relaxed), "pacer_queue_kb": queued.load(Ordering::Relaxed) / 1000 }));
                frames = 0; bytes = 0; enc_ns = 0; skipped = 0; win_t = Instant::now(); win_cpu = cpu; cap_frames0 = capf; conv_ns0 = convn; nacks0 = nacks;
            }
            // quadro novo (ou reenvio de 1 s numa tela parada, para o decodificador não secar)
            let (yuv, pvw, raw, src) = {
                let mut l = cap.shared.latest.lock().unwrap();
                if l.w == 0 { continue; }
                let fresh = l.yuv.take();
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
            let (tw, th) = (yuv.w, yuv.h);
            if enc.as_ref().map(|e| (e.w, e.h) != (tw, th)).unwrap_or(true) {
                let bps = if cfg.adaptive { enc_bps(dec.bps) } else if cfg.kbps == 0 { video::auto_bitrate(th, fps) } else { (cfg.kbps * 1000).min(MAX_BPS) };
                match H264Enc::new(tw, th, fps as f32, bps) {
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
            let min_gap = Duration::from_millis(1000);
            if since_idr >= fps * 30 || (id2.load(Ordering::Relaxed) && last_idr.elapsed() >= min_gap) { id2.store(false, Ordering::Relaxed); e.force_idr(); since_idr = 0; last_idr = Instant::now(); }
            let t0 = Instant::now();
            let slow = if cfg.label == SLOW_LABEL { TEST_SLOW_MS.load(Ordering::Relaxed) } else { 0 };
            if slow > 0 { std::thread::sleep(Duration::from_millis(slow)); }   // só testes: simula codificador lento
            let au = e.encode(&yuv);
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
}

/// Buffer de reordenação/perda do receptor. Entrega as unidades de acesso em ordem; um buraco na sequência espera a
/// retransmissão (NACK) por até `WAIT`; passado o prazo, descarta até o próximo quadro-chave e sinaliza que é preciso pedi-lo (PLI).
pub struct JitterBuf {
    pkts: std::collections::BTreeMap<u64, webrtc::rtp::packet::Packet>,
    next: Option<u64>,
    hi: u64,
    gap_since: Option<Instant>,
    need_key: bool,
    pub lost_frames: u64,
    pub recovered: u64,   // buracos fechados por retransmissão dentro do prazo
}
const WAIT: Duration = Duration::from_millis(400);

impl JitterBuf {
    pub fn new() -> Self { JitterBuf { pkts: Default::default(), next: None, hi: 0, gap_since: None, need_key: true, lost_frames: 0, recovered: 0 } }
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
            let Some(next) = self.next else { break };
            if self.pkts.contains_key(&next) {
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
                    let since = *self.gap_since.get_or_insert(now);
                    if now.duration_since(since) < WAIT && self.pkts.len() < 3000 { break; }
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
                self.need_key = false;
                out.push(au);
            } else {
                if self.pkts.is_empty() { break; }
                let since = *self.gap_since.get_or_insert(now);
                if now.duration_since(since) < WAIT && self.pkts.len() < 3000 { break; }   // dá tempo à retransmissão
                // perda sem conserto: pula o quadro quebrado e tudo até o próximo quadro-chave
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
    let (dtx, drx) = std::sync::mpsc::sync_channel::<Option<Vec<u8>>>(6);
    let (st2, ev2, peer2) = (st.clone(), ev.clone(), peer.clone());
    // decodificação numa thread: não trava o runtime
    std::thread::spawn(move || {
        let mut dec: Option<H264Dec> = None;
        let mut sink: Option<(PathBuf, FrameSink)> = None;
        let mut dims = (0usize, 0usize);
        while let Ok(m) = drx.recv() {
            let Some(au) = m else { dec = None; continue };   // reset (voltou a assistir)
            if dec.is_none() { dec = H264Dec::new().ok(); }
            let Some(d) = dec.as_mut() else { continue };
            let r = d.decode(&au);
            if r.is_err() { st2.want_pli.store(true, Ordering::Relaxed); }
            if let Ok(Some((w, h, rgba))) = r {
                let path = st2.sink_path.lock().unwrap().clone();
                let Some(path) = path else { continue };
                if sink.as_ref().map(|s| s.0 != path).unwrap_or(true) { sink = FrameSink::create(&path).ok().map(|s| (path, s)); }
                if let Some((_, s)) = sink.as_mut() { s.write(w, h, rgba); }
                let n = st2.frames.fetch_add(1, Ordering::Relaxed) + 1;
                if dims != (w, h) || n == 1 { dims = (w, h); let _ = ev2.try_send(json!({ "ev": "share.remote", "peer_id": peer2, "state": "frame", "w": w, "h": h })); }
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
        for au in aus { if dtx.try_send(Some(au)).is_err() { ask(&mut last_pli, &mut plis, Duration::from_millis(400)); } last_frame = Instant::now(); }
        if want || st.want_pli.swap(false, Ordering::Relaxed) { ask(&mut last_pli, &mut plis, Duration::from_millis(400)); }
        // sem quadros há 2 s (o quadro-chave pedido se perdeu): pede outro
        if last_frame.elapsed() > Duration::from_secs(2) { ask(&mut last_pli, &mut plis, Duration::from_secs(2)); }
        if win.elapsed() >= Duration::from_secs(1) {
            let f = st.frames.load(Ordering::Relaxed);
            let _ = ev.try_send(json!({ "ev": "share.rxstats", "peer_id": peer, "fps": f.saturating_sub(frames0), "plis": plis, "lost_frames": jb.lost_frames, "recovered": jb.recovered }));
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
