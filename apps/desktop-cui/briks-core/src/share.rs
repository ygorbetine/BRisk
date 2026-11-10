//! Transmissão de tela/janela: pipeline do emissor (captura → escala/cor → H264 → faixa WebRTC) e do receptor (RTP → H264 → quadro
//! compartilhado com a UI). O mesh só cria as faixas, renegocia e roteia os eventos; aqui fica o trabalho pesado, em threads próprias.
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use bytes::Bytes;
use serde_json::{json, Value};
use tokio::sync::mpsc;
use webrtc::media::io::sample_builder::SampleBuilder;
use webrtc::media::Sample;
use webrtc::rtp::codecs::h264::H264Packet;
use webrtc::rtp_transceiver::rtp_codec::RTCRtpCodecCapability;
use webrtc::track::track_local::track_local_static_sample::TrackLocalStaticSample;
use webrtc::track::track_remote::TrackRemote;

use crate::capture::{Capture, SourceKind};
use crate::video::{self, FrameSink, H264Dec, H264Enc};

#[cfg(test)]
pub static DBG_TX: Mutex<Vec<Vec<u8>>> = Mutex::new(Vec::new());
pub const VIDEO_TRACK_ID: &str = "screen-video";
pub const AUDIO_TRACK_ID: &str = "screen-audio";
pub const H264_FMTP: &str = "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f";
pub const MAX_BPS: u32 = 50_000_000;

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
}

pub fn new_video_track() -> Arc<TrackLocalStaticSample> {
    Arc::new(TrackLocalStaticSample::new(
        RTCRtpCodecCapability { mime_type: "video/H264".into(), clock_rate: 90_000, channels: 0, sdp_fmtp_line: H264_FMTP.into(), rtcp_feedback: vec![] },
        VIDEO_TRACK_ID.into(),
        "briks-screen".into(),
    ))
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

/// Inicia o emissor numa thread. `ev` recebe eventos JSON ("share.local"...); `preview` = arquivo de quadro compartilhado da prévia.
pub fn start_sender(gen: u64, idr: Arc<AtomicBool>, cfg: ShareCfg, track: Arc<TrackLocalStaticSample>, ev: mpsc::Sender<Value>, preview: Option<PathBuf>, dump: Option<PathBuf>) -> Result<Sender, String> {
    let cap = Capture::start(cfg.kind.clone(), cfg.fps, cfg.height, preview.is_some(), dump.is_some()).map_err(|e| e.to_string())?;
    let stop = Arc::new(AtomicBool::new(false));
    idr.store(true, Ordering::Relaxed);
    let (tx, mut rx) = mpsc::unbounded_channel::<(Vec<u8>, Duration)>();
    let rt = tokio::runtime::Handle::current();
    let t2 = track.clone();
    rt.spawn(async move {
        while let Some((au, d)) = rx.recv().await { let _ = t2.write_sample(&Sample { data: Bytes::from(au), duration: d, ..Default::default() }).await; }
    });
    let (st, id2) = (stop.clone(), idr.clone());
    std::thread::spawn(move || {
        let fps = cfg.fps.clamp(5, 120);
        let period = Duration::from_micros(1_000_000 / fps as u64);
        let mut cap = cap;
        let mut enc: Option<H264Enc> = None;
        let mut pv = preview.and_then(|p| FrameSink::create(&p).ok());
        let (mut last_send, mut next) = (Instant::now() - Duration::from_secs(5), Instant::now());
        let mut held: Option<video::Yuv> = None;
        let (mut frames, mut bytes, mut enc_ns, mut skipped) = (0u64, 0u64, 0u128, 0u64);
        let (mut win_t, mut win_cpu) = (Instant::now(), cpu_ms());
        let mut since_idr = 0u32;
        let mut closed_reason = String::new();
        while !st.load(Ordering::Relaxed) {
            if cap.shared.closed.load(Ordering::Relaxed) { closed_reason = "A janela foi fechada".into(); break; }
            let now = Instant::now();
            if now < next { std::thread::sleep((next - now).min(Duration::from_millis(5))); continue; }
            next += period; if next < now { next = now + period; }
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
            let (tw, th) = (yuv.w, yuv.h);
            let bps = if cfg.kbps == 0 { video::auto_bitrate(th, fps) } else { (cfg.kbps * 1000).min(MAX_BPS) };
            if enc.as_ref().map(|e| (e.w, e.h, e.bps) != (tw, th, bps)).unwrap_or(true) {
                match H264Enc::new(tw, th, fps as f32, bps) {
                    Ok(e) => { id2.store(true, Ordering::Relaxed); let _ = ev.try_send(json!({ "ev": "share.local", "gen": gen, "state": "on", "w": tw, "h": th, "fps": fps, "kbps": bps / 1000, "src_w": src.0, "src_h": src.1, "label": cfg.label, "encoder": e.name, "hardware": e.hardware })); enc = Some(e); }
                    Err(e) => { closed_reason = format!("codificador: {e}"); break; }
                }
            }
            if let (Some(d), Some(raw)) = (&dump, raw) { dump_png(d, &raw, src.0, src.1, tw, th); }
            let e = enc.as_mut().unwrap();
            since_idr += 1;
            if id2.swap(false, Ordering::Relaxed) || since_idr >= fps * 4 { e.force_idr(); since_idr = 0; }
            let t0 = Instant::now();
            let au = e.encode(&yuv);
            held = Some(yuv);   // guardado para o reenvio de tela parada
            enc_ns += t0.elapsed().as_nanos();
            match au {
                Ok(list) => {
                    if list.is_empty() { skipped += 1; }
                    for au in list { if std::env::var("BRIKS_SHARE_DEBUG").is_ok() { for n in crate::mfenc::nals(&au) { if n.0 == 7 { eprintln!("[tx] SPS {:02x?}", &au[n.1..n.2]); } if n.0 == 8 { eprintln!("[tx] PPS {:02x?}", &au[n.1..n.2]); } }
                        eprintln!("[tx] {} bytes nals {:?}", au.len(), crate::mfenc::nals(&au).iter().map(|n| (n.0, n.2 - n.1)).collect::<Vec<_>>()); }
                        #[cfg(test)]
                        if std::env::var("BRIKS_SHARE_DEBUG").is_ok() { DBG_TX.lock().unwrap().push(au.clone()); }
                        frames += 1; bytes += au.len() as u64; last_send = Instant::now(); let _ = tx.send((au, period)); }
                }
                Err(e) => { closed_reason = e.to_string(); break; }
            }
            if win_t.elapsed() >= Duration::from_secs(1) {
                let secs = win_t.elapsed().as_secs_f64();
                let cpu = cpu_ms();
                let (tw, th) = enc.as_ref().map(|e| (e.w, e.h)).unwrap_or((0, 0));
                let _ = ev.try_send(json!({ "ev": "share.stats", "gen": gen, "fps": (frames as f64 / secs * 10.0).round() / 10.0, "kbps": (bytes as f64 * 8.0 / secs / 1000.0).round(),
                    "enc_ms": if frames > 0 { (enc_ns as f64 / frames as f64 / 1e6 * 10.0).round() / 10.0 } else { 0.0 }, "skipped": skipped, "w": tw, "h": th,
                    "cpu_pct": ((cpu - win_cpu) / (secs * 1000.0) * 100.0).round(), "cap_dropped": cap.shared.dropped.swap(0, Ordering::Relaxed) }));
                frames = 0; bytes = 0; enc_ns = 0; skipped = 0; win_t = Instant::now(); win_cpu = cpu;
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

/// Lê o RTP da faixa de vídeo do peer. Só decodifica enquanto `watching`; sem espectador descarta os pacotes (custo ~zero).
/// `pli` pede um quadro-chave ao emissor (início, perda).
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
            if std::env::var("BRIKS_SHARE_DEBUG").is_ok() { eprintln!("[rx] au {} bytes -> {}", au.len(), match &r { Ok(Some((w, h, _))) => format!("{w}x{h}"), Ok(None) => "nada".into(), Err(e) => format!("erro {e}") }); }
            #[cfg(test)]
            if r.is_err() && std::env::var("BRIKS_SHARE_DEBUG").is_ok() {
                let tx = DBG_TX.lock().unwrap();
                let best = tx.iter().rev().take(8).find(|t| t.len() == au.len() + 6);
                match best { Some(t) => { let t = &t[6..]; let d = t.iter().zip(au.iter()).position(|(a, b)| a != b); eprintln!("[cmp] tx {:02x?} | rx {:02x?} | 1ª diferença {:?} | tx@d {:02x?} rx@d {:02x?}", &t[..8], &au[..8], d, d.map(|d| t[d.saturating_sub(6)..(d + 10).min(t.len())].to_vec()), d.map(|d| au[d.saturating_sub(6)..(d + 10).min(au.len())].to_vec())); } None => eprintln!("[cmp] sem par para {} bytes", au.len()) }
            }
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
    let mut sb = SampleBuilder::new(4096, H264Packet::default(), 90_000);
    let mut my_epoch = 0u64;
    let (mut was, mut last_pli, mut last_frame) = (false, Instant::now() - Duration::from_secs(10), Instant::now());
    let mut seen = 0u64;
    while let Ok((pkt, _)) = track.read_rtp().await {
        let w = st.watching.load(Ordering::Relaxed);
        if !w { was = false; continue; }
        let ep = st.epoch.load(Ordering::Relaxed);
        if !was || ep != my_epoch {
            was = true; my_epoch = ep; sb = SampleBuilder::new(4096, H264Packet::default(), 90_000);
            st.frames.store(0, Ordering::Relaxed); seen = 0; last_frame = Instant::now();
            let _ = dtx.try_send(None);
            pli(); last_pli = Instant::now();
        }
        sb.push(pkt);
        while let Some(s) = sb.pop() {
            seen += 1;
            if dtx.try_send(Some(s.data.to_vec())).is_err() { pli(); last_pli = Instant::now(); }
            last_frame = Instant::now();
        }
        if st.want_pli.swap(false, Ordering::Relaxed) && last_pli.elapsed() > Duration::from_millis(400) { pli(); last_pli = Instant::now(); }
        // sem quadros há 2 s (perda do quadro-chave): pede outro, no máximo a cada 2 s
        if last_frame.elapsed() > Duration::from_secs(2) && last_pli.elapsed() > Duration::from_secs(2) { pli(); last_pli = Instant::now(); last_frame = Instant::now(); }
        let _ = seen;
    }
}
