//! Áudio da transmissão: só o som dos processos escolhidos, via WASAPI *process loopback* (Windows 10 2004+).
//! Uma captura por processo (árvore de processos incluída), mixadas e codificadas em Opus estéreo numa faixa própria.
//! Modos: `Tree(pid)` = "Da fonte" (a janela transmitida); `Pids` = "Escolher apps"; `AllExcept` = tudo que toca, menos o próprio
//! Briks e o Discord (tela inteira); a lista é reavaliada a cada 2 s para pegar apps que começarem a tocar depois.
use std::collections::{HashMap, VecDeque};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use bytes::Bytes;
use serde_json::{json, Value};
use webrtc::api::media_engine::MIME_TYPE_OPUS;
use webrtc::media::Sample;
use webrtc::rtp_transceiver::rtp_codec::RTCRtpCodecCapability;
use webrtc::track::track_local::track_local_static_sample::TrackLocalStaticSample;
use windows::core::{implement, Interface, Ref, HRESULT, PWSTR};
use windows::Win32::Foundation::{CloseHandle, HANDLE, WAIT_OBJECT_0};
use windows::Win32::Media::Audio::Endpoints::IAudioMeterInformation;
use windows::Win32::Media::Audio::*;
use windows::Win32::System::Com::StructuredStorage::PROPVARIANT;
use windows::Win32::System::Com::{CoCreateInstance, CoInitializeEx, BLOB, CLSCTX_ALL, COINIT_MULTITHREADED};
use windows::Win32::System::Threading::{CreateEventW, OpenProcess, QueryFullProcessImageNameW, SetEvent, WaitForSingleObject, PROCESS_NAME_FORMAT, PROCESS_QUERY_LIMITED_INFORMATION};
use windows::Win32::System::Variant::VT_BLOB;

pub const RATE: u32 = 48_000;
const FRAME: usize = 960; // 20 ms

/// Processos nunca capturados no modo "tudo": o próprio Briks (a UI e o core tocam as vozes da chamada) e o Discord.
pub fn excluded_exe(exe: &str) -> bool {
    let e = exe.to_lowercase();
    e.starts_with("briks") || e.starts_with("discord")
}

#[derive(Debug, Clone, PartialEq)]
pub enum AudioMode {
    None,
    Tree(u32),
    Pids(Vec<u32>),
    AllExcept,
    /// Só testes: bipe sintético de 1 kHz por 100 ms no início de cada segundo do relógio, só no canal esquerdo (prova de estéreo
    /// e de sincronia A/V, sem depender de placa de som nem de outro processo).
    TestBeep,
}

pub fn new_audio_track() -> Arc<TrackLocalStaticSample> {
    Arc::new(TrackLocalStaticSample::new(
        RTCRtpCodecCapability { mime_type: MIME_TYPE_OPUS.to_owned(), clock_rate: RATE, channels: 2, sdp_fmtp_line: "minptime=10;useinbandfec=1;usedtx=1".into(), rtcp_feedback: vec![] },
        crate::share::AUDIO_TRACK_ID.into(),
        "briks-screen".into(),
    ))
}

pub fn exe_of(pid: u32) -> String {
    unsafe {
        let Ok(h) = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid) else { return String::new() };
        let mut buf = [0u16; 520];
        let mut len = buf.len() as u32;
        let ok = QueryFullProcessImageNameW(h, PROCESS_NAME_FORMAT(0), PWSTR(buf.as_mut_ptr()), &mut len).is_ok();
        let _ = CloseHandle(h);
        if !ok { return String::new(); }
        let p = String::from_utf16_lossy(&buf[..len as usize]);
        p.rsplit(['\\', '/']).next().unwrap_or("").to_string()
    }
}

/// Apps com sessão de áudio na saída padrão: [{pid, exe, name, playing}] ("playing" = som saindo agora).
pub fn list_apps() -> Vec<Value> {
    let mut out: Vec<Value> = vec![];
    unsafe {
        let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
        let Ok(en) = CoCreateInstance::<_, IMMDeviceEnumerator>(&MMDeviceEnumerator, None, CLSCTX_ALL) else { return out };
        let Ok(dev) = en.GetDefaultAudioEndpoint(eRender, eConsole) else { return out };
        let Ok(mgr) = dev.Activate::<IAudioSessionManager2>(CLSCTX_ALL, None) else { return out };
        let Ok(list) = mgr.GetSessionEnumerator() else { return out };
        let mut seen: HashMap<u32, usize> = HashMap::new();
        for i in 0..list.GetCount().unwrap_or(0) {
            let Ok(c) = list.GetSession(i) else { continue };
            let Ok(c2) = c.cast::<IAudioSessionControl2>() else { continue };
            let pid = c2.GetProcessId().unwrap_or(0);
            if pid == 0 { continue; }
            let active = c.GetState().map(|s| s == AudioSessionStateActive).unwrap_or(false);
            let peak = c.cast::<IAudioMeterInformation>().ok().and_then(|m| m.GetPeakValue().ok()).unwrap_or(0.0);
            let playing = active && peak > 0.0005;
            let exe = exe_of(pid);
            if exe.is_empty() { continue; }
            if let Some(&k) = seen.get(&pid) { if playing { out[k]["playing"] = json!(true); } continue; }
            let name = exe.trim_end_matches(".exe").trim_end_matches(".EXE").to_string();
            seen.insert(pid, out.len());
            out.push(json!({ "pid": pid, "exe": exe, "name": name, "playing": playing, "excluded": excluded_exe(&exe) }));
        }
    }
    out
}

// ---------------------------------------------------------------------------------------------------------------------
// Captura de um processo
// ---------------------------------------------------------------------------------------------------------------------
#[implement(IActivateAudioInterfaceCompletionHandler)]
struct Done(isize);
impl IActivateAudioInterfaceCompletionHandler_Impl for Done_Impl {
    fn ActivateCompleted(&self, _op: Ref<'_, IActivateAudioInterfaceAsyncOperation>) -> windows::core::Result<()> {
        unsafe { let _ = SetEvent(HANDLE(self.0 as *mut _)); }
        Ok(())
    }
}

type Ring = Arc<Mutex<VecDeque<f32>>>; // estéreo intercalado, 48 kHz

/// Captura o áudio da árvore do processo `pid` até `stop`. Bloqueante (rode numa thread).
fn capture_pid(pid: u32, ring: Ring, stop: Arc<AtomicBool>) -> Result<(), String> {
    unsafe {
        let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
        let mut params = AUDIOCLIENT_ACTIVATION_PARAMS::default();
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.Anonymous.ProcessLoopbackParams = AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS { TargetProcessId: pid, ProcessLoopbackMode: PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE };
        let mut pv = std::mem::ManuallyDrop::new(PROPVARIANT::default());
        {
            let a = &mut *pv.Anonymous.Anonymous;
            a.vt = VT_BLOB;
            a.Anonymous.blob = BLOB { cbSize: std::mem::size_of::<AUDIOCLIENT_ACTIVATION_PARAMS>() as u32, pBlobData: &mut params as *mut _ as *mut u8 };
        }
        let ev = CreateEventW(None, false, false, None).map_err(|e| e.to_string())?;
        let handler: IActivateAudioInterfaceCompletionHandler = Done(ev.0 as isize).into();
        let op = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, &IAudioClient::IID, Some(&*pv as *const PROPVARIANT), &handler).map_err(|e| format!("loopback de processo indisponível: {e}"))?;
        if WaitForSingleObject(ev, 5000) != WAIT_OBJECT_0 { let _ = CloseHandle(ev); return Err("ativação do áudio demorou demais".into()); }
        let _ = CloseHandle(ev);
        let (mut hr, mut unk) = (HRESULT(0), None);
        op.GetActivateResult(&mut hr, &mut unk).map_err(|e| e.to_string())?;
        hr.ok().map_err(|e| format!("ativação do áudio: {e}"))?;
        let client: IAudioClient = unk.ok_or("sem cliente de áudio")?.cast().map_err(|e| e.to_string())?;
        let fmt = WAVEFORMATEX { wFormatTag: 1, nChannels: 2, nSamplesPerSec: RATE, nAvgBytesPerSec: RATE * 4, nBlockAlign: 4, wBitsPerSample: 16, cbSize: 0 };
        client.Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 0, 0, &fmt, None).map_err(|e| format!("Initialize: {e}"))?;
        let cap: IAudioCaptureClient = client.GetService().map_err(|e| e.to_string())?;
        let tick = CreateEventW(None, false, false, None).map_err(|e| e.to_string())?;
        client.SetEventHandle(tick).map_err(|e| e.to_string())?;
        client.Start().map_err(|e| e.to_string())?;
        while !stop.load(Ordering::Relaxed) {
            WaitForSingleObject(tick, 100);
            loop {
                let n = match cap.GetNextPacketSize() { Ok(n) => n, Err(_) => { stop_client(&client, tick); return Ok(()); } };
                if n == 0 { break; }
                let (mut p, mut frames, mut flags) = (std::ptr::null_mut(), 0u32, 0u32);
                if cap.GetBuffer(&mut p, &mut frames, &mut flags, None, None).is_err() { break; }
                {
                    let mut r = ring.lock().unwrap();
                    if flags & (AUDCLNT_BUFFERFLAGS_SILENT.0 as u32) != 0 || p.is_null() { r.extend(std::iter::repeat(0.0).take(frames as usize * 2)); }
                    else { let s = std::slice::from_raw_parts(p as *const i16, frames as usize * 2); r.extend(s.iter().map(|v| *v as f32 / 32768.0)); }
                    let max = RATE as usize; // 0,5 s estéreo
                    if r.len() > max { let d = r.len() - max; r.drain(..d); }
                }
                let _ = cap.ReleaseBuffer(frames);
            }
        }
        stop_client(&client, tick);
        Ok(())
    }
}
unsafe fn stop_client(c: &IAudioClient, tick: HANDLE) { let _ = c.Stop(); let _ = CloseHandle(tick); }

// ---------------------------------------------------------------------------------------------------------------------
// Mixagem + Opus
// ---------------------------------------------------------------------------------------------------------------------
#[derive(Default)]
pub struct AudioStats {
    pub frames: AtomicU64,
    /// pico (x1000) do último segundo
    pub peak_milli: AtomicU64,
    pub sources: AtomicU64,
}

pub struct ProcAudio {
    stop: Arc<AtomicBool>,
    mode: Arc<Mutex<AudioMode>>,
    pub stats: Arc<AudioStats>,
}
impl ProcAudio {
    pub fn stop(&self) { self.stop.store(true, Ordering::Relaxed); }
    pub fn set_mode(&self, m: AudioMode) { *self.mode.lock().unwrap() = m; }
}
impl Drop for ProcAudio { fn drop(&mut self) { self.stop(); } }

struct Src { ring: Ring, stop: Arc<AtomicBool>, dead: Arc<AtomicBool> }

fn wanted(mode: &AudioMode) -> Vec<u32> {
    match mode {
        AudioMode::None | AudioMode::TestBeep => vec![],
        // "Da fonte" numa janela: a árvore do processo da janela e também as sessões de áudio do MESMO executável que vivem fora
        // dela (apps que tocam por um processo irmão)
        AudioMode::Tree(p) => { let exe = exe_of(*p).to_lowercase(); let mut v = vec![*p];
            if !exe.is_empty() { for a in list_apps() { if a["exe"].as_str().map(|e| e.to_lowercase()) == Some(exe.clone()) { if let Some(q) = a["pid"].as_u64() { if !v.contains(&(q as u32)) { v.push(q as u32); } } } } }
            v }
        AudioMode::Pids(v) => v.clone(),
        AudioMode::AllExcept => list_apps().iter().filter(|a| a["excluded"] != true).filter_map(|a| a["pid"].as_u64().map(|p| p as u32)).collect(),
    }
}

/// Inicia a captura/mixagem/codificação. `sink`: se dado, recebe os quadros mixados (testes) em vez de só a faixa.
pub fn start(mode: AudioMode, track: Arc<TrackLocalStaticSample>) -> ProcAudio {
    let stop = Arc::new(AtomicBool::new(false));
    let stats = Arc::new(AudioStats::default());
    let mode = Arc::new(Mutex::new(mode));
    let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel::<Vec<u8>>();
    tokio::spawn(async move { while let Some(p) = rx.recv().await { let _ = track.write_sample(&Sample { data: Bytes::from(p), duration: Duration::from_millis(20), ..Default::default() }).await; } });
    let (st, sts, md) = (stop.clone(), stats.clone(), mode.clone());
    std::thread::spawn(move || {
        let mut enc = match opus::Encoder::new(RATE, opus::Channels::Stereo, opus::Application::Audio) { Ok(e) => e, Err(_) => return };
        let _ = enc.set_bitrate(opus::Bitrate::Bits(128_000));
        let _ = enc.set_inband_fec(true);   // FEC interno do Opus: um pacote perdido é reconstruído a partir do seguinte
        let _ = enc.set_packet_loss_perc(5);
        let beep = *md.lock().unwrap() == AudioMode::TestBeep;
        let mut beep_phase = 0.0f32;
        let mut srcs: HashMap<u32, Src> = HashMap::new();
        let mut last_scan = Instant::now() - Duration::from_secs(10);
        let mut next = Instant::now();
        let (mut out, mut mix) = (vec![0u8; 4000], vec![0.0f32; FRAME * 2]);
        let (mut peak, mut peak_t) = (0.0f32, Instant::now());
        while !st.load(Ordering::Relaxed) {
            if last_scan.elapsed() > Duration::from_secs(2) {
                last_scan = Instant::now();
                let want = wanted(&md.lock().unwrap());
                srcs.retain(|pid, s| { let keep = want.contains(pid) && !s.dead.load(Ordering::Relaxed); if !keep { s.stop.store(true, Ordering::Relaxed); } keep });
                for pid in want {
                    if srcs.contains_key(&pid) { continue; }
                    let s = Src { ring: Arc::new(Mutex::new(VecDeque::new())), stop: Arc::new(AtomicBool::new(false)), dead: Arc::new(AtomicBool::new(false)) };
                    let (ring, sp, dead) = (s.ring.clone(), s.stop.clone(), s.dead.clone());
                    std::thread::spawn(move || { if let Err(e) = capture_pid(pid, ring, sp) { if std::env::var("BRIKS_SHARE_DEBUG").is_ok() { eprintln!("[áudio] pid {pid}: {e}"); } } dead.store(true, Ordering::Relaxed); });
                    srcs.insert(pid, s);
                }
                sts.sources.store(srcs.len() as u64, Ordering::Relaxed);
            }
            let now = Instant::now();
            if now < next { std::thread::sleep((next - now).min(Duration::from_millis(5))); continue; }
            next += Duration::from_millis(20);
            if next + Duration::from_millis(200) < now { next = now; }
            mix.iter_mut().for_each(|v| *v = 0.0);
            let mut any = false;
            if beep {
                // o quadro cobre os 20 ms que acabaram de passar (como numa captura de verdade)
                let ms0 = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_micros() as u64).unwrap_or(0).saturating_sub(20_000);
                for i in 0..FRAME {
                    let t_ms = (ms0 + i as u64 * 1_000_000 / RATE as u64) / 1000 % 1000;
                    if t_ms < 100 { mix[i * 2] = 0.5 * beep_phase.sin(); beep_phase += 2.0 * std::f32::consts::PI * 1000.0 / RATE as f32; any = true; } else { beep_phase = 0.0; }
                }
                any = true;   // envia também o silêncio: o receptor precisa vê-lo para detectar o início do bipe
            }
            for s in srcs.values() {
                let mut r = s.ring.lock().unwrap();
                if r.len() < FRAME * 2 { continue; }
                while r.len() > FRAME * 2 * 6 { r.drain(..FRAME * 2); } // atrasado: descarta o antigo
                for (m, v) in mix.iter_mut().zip(r.drain(..FRAME * 2)) { *m += v; }
                any = true;
            }
            if !any { continue; } // silêncio: não envia nada (DTX de fato)
            for v in mix.iter_mut() { *v = v.clamp(-1.0, 1.0); peak = peak.max(v.abs()); }
            if peak_t.elapsed() > Duration::from_secs(1) { sts.peak_milli.store((peak * 1000.0) as u64, Ordering::Relaxed); peak = 0.0; peak_t = Instant::now(); }
            if let Ok(n) = enc.encode_float(&mix, &mut out) { let _ = tx.send(out[..n].to_vec()); sts.frames.fetch_add(1, Ordering::Relaxed); }
        }
        for s in srcs.values() { s.stop.store(true, Ordering::Relaxed); }
    });
    ProcAudio { stop, mode, stats }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn exclusion_list_covers_briks_and_discord() {
        assert!(excluded_exe("Briks.exe") && excluded_exe("briks-core.exe") && excluded_exe("Discord.exe") && excluded_exe("DiscordPTB.exe"));
        assert!(!excluded_exe("Spotify.exe") && !excluded_exe("chrome.exe"));
    }

    /// Lista real das sessões de áudio desta máquina (não toca nada): mostra quem seria capturado e quem fica de fora.
    #[test]
    fn lists_audio_sessions_and_marks_briks_and_discord_as_excluded() {
        let apps = list_apps();
        for a in &apps { eprintln!("sessão de áudio: {} (pid {}) tocando={} excluído={}", a["exe"], a["pid"], a["playing"], a["excluded"]); }
        for a in &apps { let e = a["exe"].as_str().unwrap_or("").to_lowercase(); assert_eq!(a["excluded"] == true, e.starts_with("briks") || e.starts_with("discord"), "{a}"); }
        let all: Vec<u32> = wanted(&AudioMode::AllExcept);
        for a in apps.iter().filter(|a| a["excluded"] == true) { assert!(!all.contains(&(a["pid"].as_u64().unwrap() as u32)), "excluído entrou na captura: {a}"); }
    }

    /// Toca um WAV do Windows num processo filho (PowerShell) e mede: capturando o PID dele chega som; capturando outro PID
    /// (um irmão mudo) não chega nada. Emite som de verdade por ~3 s: não roda por padrão.
    #[test]
    #[ignore]
    fn captures_only_the_target_process() {
        let wav = r"C:\Windows\Media\Alarm01.wav";
        let mut child = std::process::Command::new("powershell").args(["-NoProfile", "-Command", &format!("$p = New-Object Media.SoundPlayer '{wav}'; $p.PlayLooping(); Start-Sleep -Seconds 5")]).spawn().expect("powershell");
        std::thread::sleep(Duration::from_millis(1500));
        let apps = list_apps();
        eprintln!("apps com áudio: {}", serde_json::to_string(&apps).unwrap());
        let measure = |pid: u32| {
            let ring: Ring = Arc::new(Mutex::new(VecDeque::new()));
            let stop = Arc::new(AtomicBool::new(false));
            let (r2, s2) = (ring.clone(), stop.clone());
            let t = std::thread::spawn(move || capture_pid(pid, r2, s2));
            let (mut peak, mut n, mut sq) = (0.0f32, 0usize, 0.0f64);
            let t0 = Instant::now();
            while t0.elapsed() < Duration::from_millis(1500) {
                std::thread::sleep(Duration::from_millis(50));
                for v in ring.lock().unwrap().drain(..) { peak = peak.max(v.abs()); sq += (v * v) as f64; n += 1; }
            }
            stop.store(true, Ordering::Relaxed);
            let r = t.join().unwrap();
            (r, n, peak, if n > 0 { (sq / n as f64).sqrt() } else { 0.0 })
        };
        let (r1, n1, peak1, rms1) = measure(child.id());
        // controle: um processo irmão que não toca nada (o próprio teste não serve: o PowerShell é filho dele e entraria na árvore)
        let mut quiet = std::process::Command::new("ping").args(["-n", "8", "127.0.0.1"]).stdout(std::process::Stdio::null()).spawn().expect("ping");
        let (r2, n2, peak2, rms2) = measure(quiet.id());
        let _ = child.kill(); let _ = quiet.kill();
        eprintln!("processo alvo (pid {}): {:?}, {n1} amostras, pico {peak1:.3}, RMS {rms1:.4}", child.id(), r1);
        eprintln!("outro processo (mudo): {:?}, {n2} amostras, pico {peak2:.3}, RMS {rms2:.4}", r2);
        assert!(r1.is_ok() && peak1 > 0.01, "não capturou o processo alvo");
        assert!(peak2 < 0.001, "vazou áudio de outro processo");
    }
}
