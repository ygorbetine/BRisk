//! Áudio da chamada: captura → Opus (48 kHz mono, 20 ms, FEC/DTX, ~32 kbps) → faixa SRTP única compartilhada por todos os peers;
//! recepção: RTP → Opus (PLC/FEC) → jitter buffer por peer → mixagem → saída. Detecção de fala (RMS + histerese).
//! Com `fake = true` (BRIKS_CORE_FAKE_AUDIO=1) a captura é um tom de 440 Hz e não há dispositivo de áudio: os testes
//! chamam `mix_pull` no lugar do callback do cpal.
use std::collections::{HashMap, HashSet, VecDeque};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, AtomicU8, Ordering};
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
// Os buffers por peer guardam ESTÉREO intercalado (E, D): a voz (mono) entra duplicada, o áudio da transmissão entra em estéreo.
const PRIME: usize = FRAME * 3 * 2; // 60 ms antes de começar a tocar
const MAX_BUF: usize = FRAME * 10 * 2; // 200 ms; acima disso descarta o mais antigo

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
    /// maior diferença entre os canais (0 = mono): prova de que o estéreo chegou
    pub stereo_diff: f32,
    /// relógio (ms) de cada início de som detectado depois de silêncio (medição de sincronia A/V nos testes)
    pub onsets: Vec<u64>,
    quiet: bool,
}
impl RxStat {
    /// Frequência estimada (Hz) por cruzamentos de zero; boa para um tom puro.
    pub fn freq(&self) -> f32 { if self.samples == 0 { 0.0 } else { self.zero_cross as f32 / 2.0 / (self.samples as f32 / RATE as f32) } }
}

#[derive(Default)]
struct Sink { buf: VecDeque<f32>, primed: bool, gen: u64 }
static SINK_GEN: AtomicU64 = AtomicU64::new(1);

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
    /// volume do espectador para o áudio de cada transmissão (chave "peer#screen"; padrão 1.0)
    gains: Mutex<HashMap<String, f32>>,
    sinks: Mutex<HashMap<String, Sink>>,
    cap: Mutex<VecDeque<f32>>,
    /// Configurações de áudio (bits de f32): ganho de entrada, ganho de saída, limite da porta de ruído em dB (NAN = porta desligada)
    in_gain: AtomicU32,
    out_gain: AtomicU32,
    gate_db: AtomicU32,
    /// Supressão de ruído: 0 = Padrão (filtro leve), 1 = desligada
    noise: AtomicU8,
    nf: Mutex<NoiseFilter>,
    /// Dispositivos escolhidos (nome; vazio = padrão do sistema). `dev_gen` sobe quando mudam: a thread do cpal reabre os fluxos.
    mic_dev: Mutex<String>,
    out_dev: Mutex<String>,
    dev_gen: AtomicU32,
    pub stats: Mutex<HashMap<String, RxStat>>,
    pub sent_frames: AtomicU64,
    ev: mpsc::Sender<AudioEvent>,
}

/// Ganho de entrada + porta de ruído (bloco inteiro abaixo do limite em dB vira silêncio).
pub fn process_input(x: &[f32], gain: f32, gate_db: Option<f32>) -> Vec<f32> {
    let closed = match gate_db { Some(db) if !x.is_empty() => { let r = (x.iter().map(|v| v * v).sum::<f32>() / x.len() as f32).sqrt() * gain; 20.0 * r.max(1e-9).log10() < db } _ => false };
    if closed { return vec![0.0; x.len()]; }
    x.iter().map(|v| (v * gain).clamp(-1.0, 1.0)).collect()
}

/// Ajustes de áudio vindos das Configurações > Voz.
#[derive(Debug, Clone, PartialEq)]
pub struct AudioCfg { pub in_gain: f32, pub out_gain: f32, pub gate_db: Option<f32>, pub noise: u8, pub mic: String, pub out: String }
impl Default for AudioCfg { fn default() -> Self { AudioCfg { in_gain: 1.0, out_gain: 1.0, gate_db: None, noise: 0, mic: String::new(), out: String::new() } } }

/// Supressão de ruído "Padrão": filtro passa-altas (~80 Hz, tira ronco e DC) + expansor suave que abaixa até 12 dB o que fica
/// perto do piso de ruído estacionário (ventilador, chiado). Voz bem acima do piso passa intacta. Não é RNNoise.
pub struct NoiseFilter { hx: f32, hy: f32, floor: f32, gain: f32 }
impl NoiseFilter {
    pub fn new() -> Self { NoiseFilter { hx: 0.0, hy: 0.0, floor: 0.0, gain: 1.0 } }
    pub fn process(&mut self, x: &mut [f32]) {
        if x.is_empty() { return; }
        const A: f32 = 0.9895;
        for v in x.iter_mut() { let y = A * (self.hy + *v - self.hx); self.hx = *v; self.hy = y; *v = y; }
        let r = rms(x).max(1e-5);
        let ms = x.len() as f32 * 1000.0 / RATE as f32;
        self.floor = if self.floor == 0.0 || r < self.floor { r } else { (self.floor * 1.00005f32.powf(ms)).min(r) };
        let target = if r < self.floor * 2.0 { 0.25 } else { 1.0 };
        for v in x.iter_mut() { self.gain += (target - self.gain) * 0.004; *v *= self.gain; }
    }
}

/// Dispositivo cujo nome combina com o pedido (o Windows corta nomes em 31 caracteres: aceita prefixo; depois, trecho).
pub fn match_device(names: &[String], want: &str) -> Option<usize> {
    let w = want.trim().to_lowercase();
    if w.is_empty() { return None; }
    let low: Vec<String> = names.iter().map(|n| n.to_lowercase()).collect();
    low.iter().position(|n| *n == w).or_else(|| low.iter().position(|n| n.starts_with(&w))).or_else(|| low.iter().position(|n| n.contains(&w)))
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
            muted_peers: Default::default(), gains: Default::default(), sinks: Default::default(), cap: Default::default(), in_gain: AtomicU32::new(1.0f32.to_bits()), out_gain: AtomicU32::new(1.0f32.to_bits()), gate_db: AtomicU32::new(f32::NAN.to_bits()), noise: AtomicU8::new(0), nf: Mutex::new(NoiseFilter::new()), mic_dev: Default::default(), out_dev: Default::default(), dev_gen: AtomicU32::new(0), stats: Default::default(), sent_frames: AtomicU64::new(0), ev });
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
    /// Volume (0..2) do áudio da transmissão de um peer, só para este espectador.
    pub fn set_screen_gain(&self, peer: &str, gain: f32) { self.gains.lock().unwrap().insert(format!("{peer}#screen"), gain.clamp(0.0, 2.0)); }
    /// Volume (0..2) da voz de um peer, só para quem escuta (1.0 = normal).
    pub fn set_peer_gain(&self, peer: &str, gain: f32) { self.gains.lock().unwrap().insert(peer.to_string(), gain.clamp(0.0, 2.0)); }
    /// Ajustes de áudio das Configurações: ganhos 0..2 (1 = normal) e porta de ruído (`gate_db` = None desliga).
    pub fn set_audio_cfg(&self, in_gain: f32, out_gain: f32, gate_db: Option<f32>) {
        self.in_gain.store(in_gain.clamp(0.0, 2.0).to_bits(), Ordering::Relaxed);
        self.out_gain.store(out_gain.clamp(0.0, 2.0).to_bits(), Ordering::Relaxed);
        self.gate_db.store(gate_db.map(|d| d.clamp(-90.0, 0.0)).unwrap_or(f32::NAN).to_bits(), Ordering::Relaxed);
    }
    /// Aplica todas as Configurações > Voz (ganhos, porta, supressão e dispositivos). Mudar dispositivo reabre os fluxos.
    pub fn apply_cfg(&self, c: &AudioCfg) {
        self.set_audio_cfg(c.in_gain, c.out_gain, c.gate_db);
        self.noise.store(c.noise.min(1), Ordering::Relaxed);
        let mut changed = false;
        { let mut m = self.mic_dev.lock().unwrap(); if *m != c.mic { *m = c.mic.clone(); changed = true; } }
        { let mut o = self.out_dev.lock().unwrap(); if *o != c.out { *o = c.out.clone(); changed = true; } }
        if changed { self.dev_gen.fetch_add(1, Ordering::Relaxed); }
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
        // a faixa de áudio de uma transmissão ("peer#screen") é decodificada e tocada em estéreo; a voz, em mono
        let stereo = peer.contains('#');
        let ch = if stereo { 2 } else { 1 };
        let Ok(mut dec) = Decoder::new(RATE, if stereo { Channels::Stereo } else { Channels::Mono }) else { return };
        // geração: numa reconexão a faixa nova do mesmo peer pode começar antes de a antiga terminar; a antiga não pode apagar o sink da nova
        let gen = SINK_GEN.fetch_add(1, Ordering::Relaxed);
        self.sinks.lock().unwrap().insert(peer.clone(), Sink { gen, ..Default::default() });
        let mut expect: Option<u16> = None;
        let mut spk = Speaker::default();
        let mut pcm = vec![0.0f32; FRAME * ch];
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
                        if r.is_ok() { self.deliver(&peer, &pcm, ch, &mut spk); }
                    }
                }
            }
            expect = Some(seq.wrapping_add(1));
            if let Ok(n) = dec.decode_float(&pkt.payload, &mut pcm, false) { self.deliver(&peer, &pcm[..n * ch], ch, &mut spk); }
        }
        { let mut s = self.sinks.lock().unwrap(); if s.get(&peer).map(|k| k.gen == gen).unwrap_or(false) { s.remove(&peer); } else { return; } }
        if !peer.contains('#') { let _ = self.ev.try_send(AudioEvent::Speaking { peer, level: 0.0 }); }
    }

    /// `ch` = 1 (voz, mono) ou 2 (áudio de transmissão, estéreo intercalado).
    fn deliver(&self, peer: &str, pcm_in: &[f32], ch: usize, spk: &mut Speaker) {
        let mono: Vec<f32>;
        let pcm: &[f32] = if ch == 2 { mono = pcm_in.chunks_exact(2).map(|p| (p[0] + p[1]) * 0.5).collect(); &mono } else { pcm_in };
        {
            let mut st = self.stats.lock().unwrap();
            let s = st.entry(peer.to_string()).or_default();
            s.frames += 1;
            s.samples += pcm.len() as u64;
            s.zero_cross += pcm.windows(2).filter(|w| (w[0] < 0.0) != (w[1] < 0.0)).count() as u64;
            s.peak = s.peak.max(pcm.iter().fold(0.0f32, |m, v| m.max(v.abs())));
            if ch == 2 { s.stereo_diff = s.stereo_diff.max(pcm_in.chunks_exact(2).fold(0.0f32, |m, p| m.max((p[0] - p[1]).abs()))); }
            // início de som depois de silêncio: marca o relógio no ponto exato da amostra dentro do quadro
            let level = rms(pcm);
            if level < 0.004 { s.quiet = true; }
            else if s.quiet && level > 0.02 {
                s.quiet = false;
                let first = pcm.iter().position(|v| v.abs() > 0.02).unwrap_or(0);
                let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0);
                let back = ((pcm.len() - first) as u64 * 1000) / RATE as u64;
                if s.onsets.len() < 64 { s.onsets.push(now.saturating_sub(back)); }
            }
        }
        // faixas de áudio de transmissão ("peer#screen") não contam como fala
        if !peer.contains('#') { if let Some(l) = spk.feed(rms(pcm)) { let _ = self.ev.try_send(AudioEvent::Speaking { peer: peer.to_string(), level: l }); } }
        if let Some(s) = self.sinks.lock().unwrap().get_mut(peer) {
            if ch == 2 { s.buf.extend(pcm_in.iter().copied()); } else { s.buf.extend(pcm.iter().flat_map(|v| [*v, *v])); }
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
        let mut st = vec![0.0f32; out.len() * 2];
        self.mix_pull_stereo(&mut st);
        for (o, p) in out.iter_mut().zip(st.chunks_exact(2)) { *o = (p[0] + p[1]) * 0.5; }
    }

    /// Igual, em estéreo intercalado (E, D): `out.len()` = 2 x nº de amostras. É o que vai para a placa de som.
    pub fn mix_pull_stereo(&self, out: &mut [f32]) {
        out.iter_mut().for_each(|v| *v = 0.0);
        let deaf = self.deaf();
        let muted = self.muted_peers.lock().unwrap().clone();
        let gains = self.gains.lock().unwrap().clone();
        let mut sinks = self.sinks.lock().unwrap();
        for (peer, s) in sinks.iter_mut() {
            if !s.primed { if s.buf.len() >= PRIME { s.primed = true; } else { continue; } }
            let take = out.len().min(s.buf.len()) & !1;
            let g = gains.get(peer).copied().unwrap_or(1.0);
            let silent = deaf || muted.contains(peer) || muted.contains(peer.split('#').next().unwrap_or(""));
            for (i, v) in s.buf.drain(..take).enumerate() { if !silent { out[i] += v * g; } }
            if take < out.len() { s.primed = false; }
        }
        let og = f32::from_bits(self.out_gain.load(Ordering::Relaxed));
        out.iter_mut().for_each(|v| *v = (*v * og).clamp(-1.0, 1.0));
    }

    fn push_capture(&self, mono48: &[f32]) {
        let gate = f32::from_bits(self.gate_db.load(Ordering::Relaxed));
        let tmp;
        let mono48 = if self.noise.load(Ordering::Relaxed) == 0 { let mut t = mono48.to_vec(); self.nf.lock().unwrap().process(&mut t); tmp = t; &tmp[..] } else { mono48 };
        let proc = process_input(mono48, f32::from_bits(self.in_gain.load(Ordering::Relaxed)), if gate.is_nan() { None } else { Some(gate) });
        let mut c = self.cap.lock().unwrap();
        c.extend(proc.iter().copied());
        if c.len() > RATE as usize { let d = c.len() - RATE as usize; c.drain(..d); }
    }

    // ---------- dispositivos (cpal), numa thread própria porque o Stream do WASAPI não é Send ----------
    fn run_cpal(self: &Arc<Self>) -> Result<(), String> {
        // Reabre os fluxos quando a pessoa troca de microfone/saída nas Configurações (dev_gen sobe).
        while !self.stop.load(Ordering::Relaxed) {
            let gen = self.dev_gen.load(Ordering::Relaxed);
            let keep = self.open_streams()?;
            while !self.stop.load(Ordering::Relaxed) && self.dev_gen.load(Ordering::Relaxed) == gen { std::thread::sleep(Duration::from_millis(100)); }
            drop(keep);
        }
        Ok(())
    }

    /// Escolhe o dispositivo pelo nome salvo; se sumiu, usa o padrão e avisa.
    fn pick(&self, host: &cpal::Host, input: bool, want: &str) -> Option<cpal::Device> {
        use cpal::traits::{DeviceTrait, HostTrait};
        if !want.trim().is_empty() {
            let list: Vec<cpal::Device> = if input { host.input_devices().map(|d| d.collect()).unwrap_or_default() } else { host.output_devices().map(|d| d.collect()).unwrap_or_default() };
            let names: Vec<String> = list.iter().map(|d| d.name().unwrap_or_default()).collect();
            if let Some(i) = match_device(&names, want) { return list.into_iter().nth(i); }
            let _ = self.ev.try_send(AudioEvent::Error(format!("{} \"{}\" não encontrado; usando o padrão do sistema", if input { "microfone" } else { "saída" }, want)));
        }
        if input { host.default_input_device() } else { host.default_output_device() }
    }

    fn open_streams(self: &Arc<Self>) -> Result<Vec<cpal::Stream>, String> {
        use cpal::traits::{DeviceTrait, StreamTrait};
        use cpal::SampleFormat;
        let host = cpal::default_host();
        let mut keep: Vec<cpal::Stream> = vec![];
        let (want_in, want_out) = (self.mic_dev.lock().unwrap().clone(), self.out_dev.lock().unwrap().clone());
        if let Some(dev) = self.pick(&host, true, &want_in) {
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
        if let Some(dev) = self.pick(&host, false, &want_out) {
            let cfg = dev.default_output_config().map_err(|e| format!("saída: {e}"))?;
            let (ch, rate) = (cfg.channels() as usize, cfg.sample_rate().0);
            let me = self.clone();
            let mut up = Upsampler::new(rate as f64 / RATE as f64);
            let err = |e| eprintln!("áudio (saída): {e}");
            let st = match cfg.sample_format() {
                // 2+ canais: esquerda e direita nos dois primeiros (demais recebem a média); 1 canal: a média
                SampleFormat::F32 => dev.build_output_stream(&cfg.config(), move |d: &mut [f32], _| { up.fill(&me, d.len() / ch, |i, l, r| for c in 0..ch { d[i * ch + c] = if ch == 1 { (l + r) * 0.5 } else if c == 0 { l } else if c == 1 { r } else { (l + r) * 0.5 }; }); }, err, None),
                SampleFormat::I16 => dev.build_output_stream(&cfg.config(), move |d: &mut [i16], _| { up.fill(&me, d.len() / ch, |i, l, r| for c in 0..ch { d[i * ch + c] = ((if ch == 1 { (l + r) * 0.5 } else if c == 0 { l } else if c == 1 { r } else { (l + r) * 0.5 }) * 32767.0) as i16; }); }, err, None),
                f => return Err(format!("formato de saída não suportado: {f:?}")),
            }
            .map_err(|e| format!("saída: {e}"))?;
            st.play().map_err(|e| format!("saída: {e}"))?;
            keep.push(st);
        } else {
            let _ = self.ev.try_send(AudioEvent::Error("nenhuma saída de áudio encontrada".into()));
        }
        Ok(keep)
    }
}

/// Teste do microfone fora da chamada: captura curta só para medir o nível (nada é gravado nem enviado).
/// Aplica o mesmo caminho da chamada (supressão, ganho, porta) para o medidor refletir as Configurações.
/// `fake` (BRIKS_CORE_FAKE_AUDIO=1): tom de 440 Hz no lugar do microfone, para os testes sem dispositivo.
pub struct MicTest { stop: Arc<AtomicBool> }
impl MicTest {
    pub fn start(cfg: AudioCfg, fake: bool, tx: mpsc::Sender<serde_json::Value>) -> MicTest {
        let stop = Arc::new(AtomicBool::new(false));
        let st = stop.clone();
        std::thread::spawn(move || {
            let send = |level: f32, error: Option<String>| { let mut j = serde_json::json!({ "ev": "mic.test", "level": level }); if let Some(e) = error { j["error"] = e.into(); } let _ = tx.try_send(j); };
            let buf: Arc<Mutex<VecDeque<f32>>> = Default::default();
            let mut nf = NoiseFilter::new();
            let mut keep: Option<cpal::Stream> = None;
            if !fake {
                use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
                use cpal::SampleFormat;
                let host = cpal::default_host();
                let list: Vec<cpal::Device> = host.input_devices().map(|d| d.collect()).unwrap_or_default();
                let names: Vec<String> = list.iter().map(|d| d.name().unwrap_or_default()).collect();
                let dev = match_device(&names, &cfg.mic).and_then(|i| list.into_iter().nth(i)).or_else(|| host.default_input_device());
                let Some(dev) = dev else { send(0.0, Some("nenhum microfone encontrado".into())); return };
                let r = dev.default_input_config().map_err(|e| e.to_string()).and_then(|c| {
                    let (ch, rate) = (c.channels() as usize, c.sample_rate().0);
                    let b = buf.clone();
                    let mut rs = Resampler::new(rate as f64 / RATE as f64);
                    let mut push = move |m: Vec<f32>| { let mut o = vec![]; rs.process(&m, &mut o); let mut q = b.lock().unwrap(); q.extend(o); if q.len() > RATE as usize { let d = q.len() - RATE as usize; q.drain(..d); } };
                    let err = |e| eprintln!("teste do microfone: {e}");
                    let s = match c.sample_format() {
                        SampleFormat::F32 => dev.build_input_stream(&c.config(), move |d: &[f32], _| push(mono(d, ch, |v| v)), err, None),
                        SampleFormat::I16 => dev.build_input_stream(&c.config(), move |d: &[i16], _| push(mono(d, ch, |v| v as f32 / 32768.0)), err, None),
                        SampleFormat::U16 => dev.build_input_stream(&c.config(), move |d: &[u16], _| push(mono(d, ch, |v| (v as f32 - 32768.0) / 32768.0)), err, None),
                        f => return Err(format!("formato não suportado: {f:?}")),
                    }.map_err(|e| e.to_string())?;
                    s.play().map_err(|e| e.to_string())?;
                    Ok(s)
                });
                match r { Ok(s) => keep = Some(s), Err(e) => { send(0.0, Some(format!("não foi possível abrir o microfone: {e}"))); return } }
            }
            let mut phase = 0.0f32;
            let started = std::time::Instant::now();
            while !st.load(Ordering::Relaxed) && started.elapsed() < Duration::from_secs(60) {   // trava de segurança: encerra sozinho em 60 s
                std::thread::sleep(Duration::from_millis(50));
                let mut x: Vec<f32> = if fake {
                    (0..2400).map(|_| { let v = 0.3 * phase.sin(); phase += 2.0 * std::f32::consts::PI * 440.0 / RATE as f32; v }).collect()
                } else { buf.lock().unwrap().drain(..).collect() };
                if x.is_empty() { continue; }
                if cfg.noise == 0 { nf.process(&mut x); }
                let p = process_input(&x, cfg.in_gain, cfg.gate_db);
                send((rms(&p) * 5.0).clamp(0.0, 1.0), None);
            }
            drop(keep);
            send(0.0, None);
        });
        MicTest { stop }
    }
    pub fn stop(&self) { self.stop.store(true, Ordering::Relaxed); }
}
impl Drop for MicTest { fn drop(&mut self) { self.stop(); } }

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
    fn input_gain_and_noise_gate() {
        let tone: Vec<f32> = (0..480).map(|i| 0.2 * (2.0 * std::f32::consts::PI * 440.0 * i as f32 / 48000.0).sin()).collect();
        let r0 = rms(&tone);
        assert!((rms(&process_input(&tone, 0.5, None)) - r0 * 0.5).abs() < 1e-4, "ganho 0,5");
        assert!(rms(&process_input(&tone, 1.0, Some(-50.0))) > r0 * 0.99, "porta aberta (voz forte)");
        let hiss: Vec<f32> = tone.iter().map(|v| v * 0.005).collect();   // ~-60 dB
        assert_eq!(rms(&process_input(&hiss, 1.0, Some(-50.0))), 0.0, "porta fecha no ruído baixo");
        assert!(rms(&process_input(&hiss, 1.0, None)) > 0.0, "sem porta o ruído passa");
    }

    #[test]
    fn noise_filter_cuts_steady_hiss_keeps_voice() {
        let mut seed = 1u32;
        let mut noise = |n: usize, amp: f32| -> Vec<f32> { (0..n).map(|_| { seed = seed.wrapping_mul(1664525).wrapping_add(1013904223); ((seed >> 8) as f32 / 8388608.0 - 1.0) * amp }).collect() };
        let mut nf = NoiseFilter::new();
        let mut last = vec![];
        for _ in 0..300 { let mut c = noise(480, 0.01); let before = rms(&c); nf.process(&mut c); last = vec![before, rms(&c)]; }
        assert!(last[1] < last[0] * 0.5, "chiado cai pelo menos 6 dB: {:?}", last);
        let mk = || -> Vec<f32> { (0..480).map(|i| 0.3 * (2.0 * std::f32::consts::PI * 300.0 * i as f32 / 48000.0).sin()).collect() };
        let before = rms(&mk());
        let mut v = mk();
        for _ in 0..40 { v = mk(); nf.process(&mut v); }
        assert!(rms(&v) > before * 0.9, "voz passa: {} vs {}", rms(&v), before);
        let mut nf2 = NoiseFilter::new();
        let mut dc = vec![0.2f32; 9600]; nf2.process(&mut dc);
        assert!(rms(&dc[8000..]) < 0.02, "DC removido");
    }

    #[test]
    fn device_name_matching() {
        let n: Vec<String> = ["Alto-falantes (Realtek High Definition Audio)", "Microfone (USB Audio Device)", "Headset (Realtek High Definition Audio)"].iter().map(|s| s.to_string()).collect();
        assert_eq!(match_device(&n, "Microfone (USB Audio Device)"), Some(1));
        assert_eq!(match_device(&n, "Headset (Realtek High Definit"), Some(2), "nome cortado em 31 caracteres do Windows");
        assert_eq!(match_device(&n, "usb audio"), Some(1));
        assert_eq!(match_device(&n, ""), None);
        assert_eq!(match_device(&n, "Inexistente"), None);
    }

    #[test]
    fn apply_cfg_bumps_device_generation_only_on_change() {
        let (a, _rx) = Audio::new(true);
        let g0 = a.dev_gen.load(Ordering::Relaxed);
        let c = AudioCfg { mic: "USB".into(), ..Default::default() };
        a.apply_cfg(&c);
        assert_eq!(a.dev_gen.load(Ordering::Relaxed), g0 + 1);
        a.apply_cfg(&c);
        assert_eq!(a.dev_gen.load(Ordering::Relaxed), g0 + 1, "mesma config não reabre");
        a.apply_cfg(&AudioCfg { mic: "USB".into(), out: "Fone".into(), noise: 1, ..Default::default() });
        assert_eq!(a.dev_gen.load(Ordering::Relaxed), g0 + 2);
        assert_eq!(a.noise.load(Ordering::Relaxed), 1);
    }

    #[tokio::test]
    async fn mic_test_fake_reports_levels_and_stops() {
        let (tx, mut rx) = mpsc::channel(64);
        let t = MicTest::start(AudioCfg { noise: 1, ..Default::default() }, true, tx);
        let j = tokio::time::timeout(Duration::from_secs(2), rx.recv()).await.unwrap().unwrap();
        assert_eq!(j["ev"], "mic.test");
        assert!(j["level"].as_f64().unwrap() > 0.5, "tom de 440 Hz a 0,3 mede alto: {j}");
        t.stop();
        let mut last = None;
        while let Ok(Some(j)) = tokio::time::timeout(Duration::from_secs(2), rx.recv()).await { let z = j["level"] == 0.0; last = Some(j); if z { break; } }
        assert_eq!(last.unwrap()["level"], 0.0, "ao parar, o medidor volta a zero");
        let (tx, mut rx) = mpsc::channel(64);
        let _t = MicTest::start(AudioCfg { noise: 1, gate_db: Some(0.0), ..Default::default() }, true, tx);
        let j = tokio::time::timeout(Duration::from_secs(2), rx.recv()).await.unwrap().unwrap();
        assert_eq!(j["level"], 0.0, "porta fechada: {j}");
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
struct Upsampler { step: f64, pos: f64, pend: VecDeque<(f32, f32)> }
impl Upsampler {
    fn new(dev_over_48k: f64) -> Self { Upsampler { step: 1.0 / dev_over_48k, pos: 0.0, pend: VecDeque::new() } }
    fn fill(&mut self, a: &Audio, frames: usize, mut put: impl FnMut(usize, f32, f32)) {
        for i in 0..frames {
            let idx = self.pos.floor() as usize;
            while self.pend.len() < idx + 2 { let mut c = [0.0f32; 960]; a.mix_pull_stereo(&mut c); self.pend.extend(c.chunks_exact(2).map(|p| (p[0], p[1]))); }
            let f = (self.pos - self.pos.floor()) as f32;
            let (x, y) = (self.pend[idx], self.pend[idx + 1]);
            put(i, x.0 + (y.0 - x.0) * f, x.1 + (y.1 - x.1) * f);
            self.pos += self.step;
        }
        let drop = (self.pos.floor() as usize).min(self.pend.len());
        self.pend.drain(..drop);
        self.pos -= drop as f64;
    }
}
