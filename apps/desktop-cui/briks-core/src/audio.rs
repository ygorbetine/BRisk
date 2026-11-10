//! Áudio da chamada: captura → Opus (48 kHz mono, 20 ms, FEC/DTX, ~32 kbps) → faixa SRTP única compartilhada por todos os peers;
//! recepção: RTP → Opus (PLC/FEC) → jitter buffer por peer → mixagem → saída. Detecção de fala (RMS + histerese).
//! Com `fake = true` (BRIKS_CORE_FAKE_AUDIO=1) a captura é um tom de 440 Hz e não há dispositivo de áudio: os testes
//! chamam `mix_pull` no lugar do callback do cpal.
use std::collections::{HashMap, HashSet, VecDeque};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use bytes::Bytes;
use opus::{Application, Bitrate, Channels, Decoder, Encoder};
use tokio::sync::mpsc;
use webrtc::api::media_engine::MIME_TYPE_OPUS;
use webrtc::media::Sample;
use webrtc::rtp_transceiver::rtp_codec::RTCRtpCodecCapability;
use webrtc::track::track_local::track_local_static_sample::TrackLocalStaticSample;
use webrtc::track::track_remote::TrackRemote;

pub const RATE: u32 = 48_000;
pub const FRAME: usize = 960; // 20 ms
const PRIME: usize = FRAME * 3; // 60 ms antes de começar a tocar
const MAX_BUF: usize = FRAME * 10; // 200 ms; acima disso descarta o mais antigo

#[derive(Debug, Clone, PartialEq)]
pub enum AudioEvent {
    /// `peer` = "me" para o microfone local. `level` 0..1 (0 = parou de falar).
    Speaking { peer: String, level: f32 },
    Error(String),
}

#[derive(Default, Debug, Clone)]
pub struct RxStat {
    pub frames: u64,
    pub samples: u64,
    pub zero_cross: u64,
    pub peak: f32,
}
impl RxStat {
    /// Frequência estimada (Hz) por cruzamentos de zero; boa para um tom puro.
    pub fn freq(&self) -> f32 { if self.samples == 0 { 0.0 } else { self.zero_cross as f32 / 2.0 / (self.samples as f32 / RATE as f32) } }
}

#[derive(Default)]
struct Sink { buf: VecDeque<f32>, primed: bool }

#[derive(Default)]
struct Speaker { on: bool, quiet: u32, tick: u32 }
impl Speaker {
    /// Histerese: liga acima de 0,015 RMS, desliga após 300 ms abaixo de 0,010. Devolve o nível a emitir (~10 Hz enquanto fala).
    fn feed(&mut self, rms: f32) -> Option<f32> {
        let was = self.on;
        if rms > 0.015 { self.on = true; self.quiet = 0; }
        else if self.on && rms < 0.010 { self.quiet += 1; if self.quiet >= 15 { self.on = false; } }
        else if self.on { self.quiet = 0; }
        self.tick = (self.tick + 1) % 5;
        if self.on && (!was || self.tick == 0) { Some((rms * 5.0).clamp(0.05, 1.0)) }
        else if was && !self.on { Some(0.0) }
        else { None }
    }
}

pub struct Audio {
    pub track: Arc<TrackLocalStaticSample>,
    mic_muted: AtomicBool,
    deaf: AtomicBool,
    stop: AtomicBool,
    fake: bool,
    muted_peers: Mutex<HashSet<String>>,
    sinks: Mutex<HashMap<String, Sink>>,
    cap: Mutex<VecDeque<f32>>,
    pub stats: Mutex<HashMap<String, RxStat>>,
    pub sent_frames: AtomicU64,
    ev: mpsc::Sender<AudioEvent>,
}

pub fn rms(x: &[f32]) -> f32 { if x.is_empty() { 0.0 } else { (x.iter().map(|v| v * v).sum::<f32>() / x.len() as f32).sqrt() } }

impl Audio {
    pub fn new(fake: bool) -> (Arc<Audio>, mpsc::Receiver<AudioEvent>) {
        let (ev, rx) = mpsc::channel(128);
        let track = Arc::new(TrackLocalStaticSample::new(
            RTCRtpCodecCapability { mime_type: MIME_TYPE_OPUS.to_owned(), clock_rate: RATE, channels: 2, sdp_fmtp_line: "minptime=10;useinbandfec=1;usedtx=1".into(), rtcp_feedback: vec![] },
            "audio".into(),
            "briks".into(),
        ));
        let a = Arc::new(Audio { track, mic_muted: AtomicBool::new(false), deaf: AtomicBool::new(false), stop: AtomicBool::new(false), fake,
            muted_peers: Default::default(), sinks: Default::default(), cap: Default::default(), stats: Default::default(), sent_frames: AtomicU64::new(0), ev });
        (a, rx)
    }

    pub fn fake_from_env() -> bool { std::env::var("BRIKS_CORE_FAKE_AUDIO").map(|v| v == "1").unwrap_or(false) }

    pub fn set_mic_muted(&self, m: bool) { self.mic_muted.store(m, Ordering::Relaxed); }
    /// Ensurdecer implica mutar o microfone (o estado `mic_muted` do usuário é preservado e volta ao desligar).
    pub fn set_deaf(&self, d: bool) { self.deaf.store(d, Ordering::Relaxed); }
    pub fn deaf(&self) -> bool { self.deaf.load(Ordering::Relaxed) }
    pub fn mic_muted(&self) -> bool { self.mic_muted.load(Ordering::Relaxed) }
    /// Microfone efetivamente aberto?
    pub fn mic_open(&self) -> bool { !self.mic_muted() && !self.deaf() }
    pub fn mute_peer(&self, peer: &str, m: bool) {
        let mut s = self.muted_peers.lock().unwrap();
        if m { s.insert(peer.to_string()); } else { s.remove(peer); }
    }
    pub fn stop(&self) { self.stop.store(true, Ordering::Relaxed); }
    pub fn rx_stat(&self, peer: &str) -> RxStat { self.stats.lock().unwrap().get(peer).cloned().unwrap_or_default() }

    /// Inicia captura + envio (e a saída cpal, se não for o modo falso).
    pub fn start(self: &Arc<Self>) {
        if !self.fake {
            let me = self.clone();
            std::thread::spawn(move || { if let Err(e) = me.run_cpal() { let _ = me.ev.try_send(AudioEvent::Error(e)); } });
        }
        let me = self.clone();
        tokio::spawn(async move { me.capture_loop().await });
    }

    async fn capture_loop(self: Arc<Self>) {
        let mut enc = match Encoder::new(RATE, Channels::Mono, Application::Voip) { Ok(e) => e, Err(e) => { let _ = self.ev.send(AudioEvent::Error(format!("opus: {e}"))).await; return; } };
        let _ = enc.set_bitrate(Bitrate::Bits(32_000));
        let _ = enc.set_inband_fec(true);
        let _ = enc.set_packet_loss_perc(10);
        let _ = enc.set_dtx(true);
        let mut spk = Speaker::default();
        let mut phase = 0.0f32;
        let mut out = vec![0u8; 1500];
        let mut tick = tokio::time::interval(Duration::from_millis(20));
        tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
        while !self.stop.load(Ordering::Relaxed) {
            tick.tick().await;
            let mut frames: Vec<Vec<f32>> = vec![];
            if self.fake {
                let mut f = vec![0.0f32; FRAME];
                for s in f.iter_mut() { *s = 0.3 * phase.sin(); phase += 2.0 * std::f32::consts::PI * 440.0 / RATE as f32; if phase > 2.0 * std::f32::consts::PI { phase -= 2.0 * std::f32::consts::PI; } }
                frames.push(f);
            } else {
                let mut c = self.cap.lock().unwrap();
                while c.len() > FRAME * 4 { c.drain(..FRAME); } // atrasado: descarta o antigo, mantém a latência baixa
                while c.len() >= FRAME && frames.len() < 3 { frames.push(c.drain(..FRAME).collect()); }
            }
            for f in frames {
                let open = self.mic_open();
                let lvl = if open { rms(&f) } else { 0.0 };
                if let Some(l) = spk.feed(lvl) { let _ = self.ev.try_send(AudioEvent::Speaking { peer: "me".into(), level: l }); }
                if !open { continue; }
                if let Ok(n) = enc.encode_float(&f, &mut out) {
                    let _ = self.track.write_sample(&Sample { data: Bytes::copy_from_slice(&out[..n]), duration: Duration::from_millis(20), ..Default::default() }).await;
                    self.sent_frames.fetch_add(1, Ordering::Relaxed);
                }
            }
        }
    }

    /// Chamado pelo `on_track` da conexão: decodifica o RTP do peer para o jitter buffer dele.
    pub async fn run_remote(self: Arc<Self>, peer: String, track: Arc<TrackRemote>) {
        let Ok(mut dec) = Decoder::new(RATE, Channels::Mono) else { return };
        self.sinks.lock().unwrap().insert(peer.clone(), Sink::default());
        let mut expect: Option<u16> = None;
        let mut spk = Speaker::default();
        let mut pcm = vec![0.0f32; FRAME];
        while let Ok((pkt, _)) = track.read_rtp().await {
            let seq = pkt.header.sequence_number;
            if let Some(e) = expect {
                let d = seq.wrapping_sub(e) as i16;
                if d < 0 { continue; } // atrasado/duplicado
                if d > 0 {
                    // perda de d pacotes: PLC nos primeiros, FEC (dentro deste pacote) no último
                    let lost = (d as usize).min(4);
                    for i in 0..lost {
                        let last = i == lost - 1;
                        let r = if last { dec.decode_float(&pkt.payload, &mut pcm, true) } else { dec.decode_float(&[], &mut pcm, false) };
                        if r.is_ok() { self.deliver(&peer, &pcm, &mut spk); }
                    }
                }
            }
            expect = Some(seq.wrapping_add(1));
            if let Ok(n) = dec.decode_float(&pkt.payload, &mut pcm, false) { self.deliver(&peer, &pcm[..n], &mut spk); }
        }
        self.sinks.lock().unwrap().remove(&peer);
        if !peer.contains('#') { let _ = self.ev.try_send(AudioEvent::Speaking { peer, level: 0.0 }); }
    }

    fn deliver(&self, peer: &str, pcm: &[f32], spk: &mut Speaker) {
        {
            let mut st = self.stats.lock().unwrap();
            let s = st.entry(peer.to_string()).or_default();
            s.frames += 1;
            s.samples += pcm.len() as u64;
            s.zero_cross += pcm.windows(2).filter(|w| (w[0] < 0.0) != (w[1] < 0.0)).count() as u64;
            s.peak = s.peak.max(pcm.iter().fold(0.0f32, |m, v| m.max(v.abs())));
        }
        // faixas de áudio de transmissão ("peer#screen") não contam como fala
        if !peer.contains('#') { if let Some(l) = spk.feed(rms(pcm)) { let _ = self.ev.try_send(AudioEvent::Speaking { peer: peer.to_string(), level: l }); } }
        if let Some(s) = self.sinks.lock().unwrap().get_mut(peer) {
            s.buf.extend(pcm.iter().copied());
            if s.buf.len() > MAX_BUF { let drop = s.buf.len() - PRIME; s.buf.drain(..drop); }
        }
    }

    pub fn remove_peer(&self, peer: &str) {
        self.sinks.lock().unwrap().remove(peer);
        self.sinks.lock().unwrap().remove(&format!("{peer}#screen"));
        self.muted_peers.lock().unwrap().remove(peer);
    }

    /// Mixa `out.len()` amostras de todos os peers (jitter buffer simples: espera 60 ms, depois toca; underrun reinicia).
    /// Ensurdecido ou peer silenciado = ganho zero (o buffer é consumido para não acumular atraso).
    pub fn mix_pull(&self, out: &mut [f32]) {
        out.iter_mut().for_each(|v| *v = 0.0);
        let deaf = self.deaf();
        let muted = self.muted_peers.lock().unwrap().clone();
        let mut sinks = self.sinks.lock().unwrap();
        for (peer, s) in sinks.iter_mut() {
            if !s.primed { if s.buf.len() >= PRIME { s.primed = true; } else { continue; } }
            let take = out.len().min(s.buf.len());
            let silent = deaf || muted.contains(peer) || muted.contains(peer.split('#').next().unwrap_or(""));
            for (i, v) in s.buf.drain(..take).enumerate() { if !silent { out[i] += v; } }
            if take < out.len() { s.primed = false; }
        }
        out.iter_mut().for_each(|v| *v = v.clamp(-1.0, 1.0));
    }

    fn push_capture(&self, mono48: &[f32]) {
        let mut c = self.cap.lock().unwrap();
        c.extend(mono48.iter().copied());
        if c.len() > RATE as usize { let d = c.len() - RATE as usize; c.drain(..d); }
    }

    // ---------- dispositivos (cpal), numa thread própria porque o Stream do WASAPI não é Send ----------
    fn run_cpal(self: &Arc<Self>) -> Result<(), String> {
        use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
        use cpal::SampleFormat;
        let host = cpal::default_host();
        let mut keep: Vec<cpal::Stream> = vec![];
        if let Some(dev) = host.default_input_device() {
            let cfg = dev.default_input_config().map_err(|e| format!("microfone: {e}"))?;
            let (ch, rate) = (cfg.channels() as usize, cfg.sample_rate().0);
            let me = self.clone();
            let mut rs = Resampler::new(rate as f64 / RATE as f64);
            let mut push = move |mono: Vec<f32>| { let mut o = vec![]; rs.process(&mono, &mut o); me.push_capture(&o); };
            let err = |e| eprintln!("áudio (entrada): {e}");
            let st = match cfg.sample_format() {
                SampleFormat::F32 => dev.build_input_stream(&cfg.config(), move |d: &[f32], _| push(mono(d, ch, |v| v)), err, None),
                SampleFormat::I16 => dev.build_input_stream(&cfg.config(), move |d: &[i16], _| push(mono(d, ch, |v| v as f32 / 32768.0)), err, None),
                SampleFormat::U16 => dev.build_input_stream(&cfg.config(), move |d: &[u16], _| push(mono(d, ch, |v| (v as f32 - 32768.0) / 32768.0)), err, None),
                f => return Err(format!("formato de microfone não suportado: {f:?}")),
            }
            .map_err(|e| format!("microfone: {e}"))?;
            st.play().map_err(|e| format!("microfone: {e}"))?;
            keep.push(st);
        } else {
            let _ = self.ev.try_send(AudioEvent::Error("nenhum microfone encontrado".into()));
        }
        if let Some(dev) = host.default_output_device() {
            let cfg = dev.default_output_config().map_err(|e| format!("saída: {e}"))?;
            let (ch, rate) = (cfg.channels() as usize, cfg.sample_rate().0);
            let me = self.clone();
            let mut up = Upsampler::new(rate as f64 / RATE as f64);
            let err = |e| eprintln!("áudio (saída): {e}");
            let st = match cfg.sample_format() {
                SampleFormat::F32 => dev.build_output_stream(&cfg.config(), move |d: &mut [f32], _| { up.fill(&me, d.len() / ch, |i, v| for c in 0..ch { d[i * ch + c] = v; }); }, err, None),
                SampleFormat::I16 => dev.build_output_stream(&cfg.config(), move |d: &mut [i16], _| { up.fill(&me, d.len() / ch, |i, v| for c in 0..ch { d[i * ch + c] = (v * 32767.0) as i16; }); }, err, None),
                f => return Err(format!("formato de saída não suportado: {f:?}")),
            }
            .map_err(|e| format!("saída: {e}"))?;
            st.play().map_err(|e| format!("saída: {e}"))?;
            keep.push(st);
        } else {
            let _ = self.ev.try_send(AudioEvent::Error("nenhuma saída de áudio encontrada".into()));
        }
        while !self.stop.load(Ordering::Relaxed) { std::thread::sleep(Duration::from_millis(200)); }
        drop(keep);
        Ok(())
    }
}

#[cfg(test)]
mod unit {
    use super::*;

    #[test]
    fn speaker_has_hysteresis_and_10hz_events() {
        let mut s = Speaker::default();
        assert_eq!(s.feed(0.001), None);
        assert!(s.feed(0.2).is_some(), "liga ao passar do limiar");
        let ev = (0..20).filter_map(|_| s.feed(0.2)).count();
        assert!(ev >= 3 && ev <= 5, "~a cada 5 quadros: {ev}");
        // entre 0,010 e 0,015 continua falando (histerese); só desliga após 15 quadros abaixo de 0,010
        for _ in 0..30 { s.feed(0.012); }
        assert!(s.on);
        let mut off = None;
        for _ in 0..16 { if let Some(l) = s.feed(0.001) { off = Some(l); } }
        assert_eq!(off, Some(0.0));
    }

    #[test]
    fn resampler_44k1_to_48k_keeps_duration_and_tone() {
        let mut rs = Resampler::new(44100.0 / 48000.0);
        let input: Vec<f32> = (0..4410).map(|i| (2.0 * std::f32::consts::PI * 440.0 * i as f32 / 44100.0).sin()).collect();
        let mut out = vec![];
        rs.process(&input, &mut out);
        assert!((out.len() as i32 - 4800).abs() <= 2, "{} amostras", out.len());
        let zc = out.windows(2).filter(|w| (w[0] < 0.0) != (w[1] < 0.0)).count() as f32 / 2.0 / 0.1;
        assert!((zc - 440.0).abs() < 25.0, "{zc} Hz");
    }

    #[test]
    fn opus_roundtrip_with_loss_concealment() {
        let mut enc = Encoder::new(RATE, Channels::Mono, Application::Voip).unwrap();
        enc.set_inband_fec(true).unwrap();
        enc.set_packet_loss_perc(10).unwrap();
        let mut dec = Decoder::new(RATE, Channels::Mono).unwrap();
        let mut ph = 0.0f32;
        let mut pk: Vec<Vec<u8>> = vec![];
        for _ in 0..10 {
            let f: Vec<f32> = (0..FRAME).map(|_| { let v = 0.3 * ph.sin(); ph += 2.0 * std::f32::consts::PI * 440.0 / RATE as f32; v }).collect();
            pk.push(enc.encode_vec_float(&f, 400).unwrap());
        }
        let mut out = vec![0.0f32; FRAME];
        for (i, p) in pk.iter().enumerate() {
            if i == 5 { continue; } // pacote 5 perdido
            if i == 6 { assert_eq!(dec.decode_float(p, &mut out, true).unwrap(), FRAME); } // FEC do 5 dentro do 6
            assert_eq!(dec.decode_float(p, &mut out, false).unwrap(), FRAME);
        }
        assert_eq!(dec.decode_float(&[], &mut out, false).unwrap(), FRAME); // PLC puro
    }
}

fn mono<T: Copy>(d: &[T], ch: usize, conv: impl Fn(T) -> f32) -> Vec<f32> {
    d.chunks(ch.max(1)).map(|f| f.iter().map(|v| conv(*v)).sum::<f32>() / f.len() as f32).collect()
}

/// Reamostragem linear (entrada `ratio` = taxa_do_dispositivo / 48000).
pub struct Resampler { ratio: f64, pos: f64, last: f32 }
impl Resampler {
    pub fn new(ratio: f64) -> Self { Resampler { ratio, pos: 0.0, last: 0.0 } }
    pub fn process(&mut self, input: &[f32], out: &mut Vec<f32>) {
        if (self.ratio - 1.0).abs() < 1e-9 { out.extend_from_slice(input); return; }
        // posição fracionária em amostras de entrada; `last` é a amostra -1
        while self.pos < input.len() as f64 {
            let i = self.pos.floor() as isize;
            let f = (self.pos - self.pos.floor()) as f32;
            let a = if i < 0 { self.last } else { input[i as usize] };
            let b = if i + 1 < input.len() as isize { input[(i + 1) as usize] } else { a };
            out.push(a + (b - a) * f);
            self.pos += self.ratio;
        }
        self.pos -= input.len() as f64;
        self.last = *input.last().unwrap_or(&self.last);
    }
}

/// Saída: 48 kHz mono → taxa do dispositivo, puxando do mixer sob demanda.
struct Upsampler { step: f64, pos: f64, pend: VecDeque<f32> }
impl Upsampler {
    fn new(dev_over_48k: f64) -> Self { Upsampler { step: 1.0 / dev_over_48k, pos: 0.0, pend: VecDeque::new() } }
    fn fill(&mut self, a: &Audio, frames: usize, mut put: impl FnMut(usize, f32)) {
        for i in 0..frames {
            let idx = self.pos.floor() as usize;
            while self.pend.len() < idx + 2 { let mut c = [0.0f32; 480]; a.mix_pull(&mut c); self.pend.extend(c); }
            let f = (self.pos - self.pos.floor()) as f32;
            let (x, y) = (self.pend[idx], self.pend[idx + 1]);
            put(i, x + (y - x) * f);
            self.pos += self.step;
        }
        let drop = (self.pos.floor() as usize).min(self.pend.len());
        self.pend.drain(..drop);
        self.pos -= drop as f64;
    }
}
