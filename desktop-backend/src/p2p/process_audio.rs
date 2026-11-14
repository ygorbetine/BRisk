//! Captura de áudio por processo (WASAPI process loopback, Windows 10 2004+).
//!
//! O renderer recebe PCM `f32le`, 48 kHz, estéreo, em um corpo HTTP em streaming.
//! - `window`: só o áudio da árvore de processos dona da janela escolhida.
//! - `screen`: o áudio de tudo o que está tocando, exceto os processos excluídos
//!   (Discord e o próprio Briks por padrão), mixando uma captura por processo.

use axum::{
    body::Body,
    http::{header, StatusCode},
    response::{IntoResponse, Response},
    Json,
};
use serde::Deserialize;

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CaptureRequest {
    /// "window" ou "screen".
    mode: String,
    /// HWND da janela escolhida (modo `window`).
    hwnd: Option<u64>,
    /// Nomes de executáveis extras a excluir no modo `screen`.
    #[serde(default)]
    exclude_names: Vec<String>,
    /// Modo `apps`: só o áudio destes executáveis (e dos processos filhos), por nome.
    #[serde(default)]
    include_names: Vec<String>,
}

/// Um app com sessão de áudio aberta, para o seletor de áudio da transmissão.
#[derive(Debug, serde::Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct AudioApp {
    /// Nome do executável em minúsculas (ex.: `spotify.exe`). É a chave usada em `include_names`.
    pub name: String,
    /// Nome amigável derivado do executável (ex.: `Spotify`).
    pub label: String,
    /// `true` se está tocando agora; `false` se a sessão existe, mas está em pausa.
    pub playing: bool,
    /// Caminho completo do executável, para o Electron extrair o ícone. Vazio se desconhecido.
    #[serde(skip_serializing_if = "String::is_empty")]
    pub path: String,
}

/// `spotify.exe` → `Spotify`.
pub fn friendly_app_label(executable: &str) -> String {
    let stem = executable
        .strip_suffix(".exe")
        .or_else(|| executable.strip_suffix(".EXE"))
        .unwrap_or(executable);
    let mut chars = stem.chars();
    match chars.next() {
        Some(first) => first.to_uppercase().collect::<String>() + chars.as_str(),
        None => String::new(),
    }
}

/// Junta sessões de áudio (executável, tocando?) em um item por app, tocando primeiro e depois por nome.
/// Ignora nomes vazios e os de `skip` (Briks e Discord, que nunca entram na transmissão).
pub fn group_audio_apps(sessions: Vec<(String, bool)>, skip: &[&str]) -> Vec<AudioApp> {
    let mut apps: Vec<AudioApp> = Vec::new();
    for (executable, playing) in sessions {
        let name = executable.trim().to_lowercase();
        if name.is_empty() || skip.contains(&name.as_str()) {
            continue;
        }
        match apps.iter_mut().find(|app| app.name == name) {
            Some(app) => app.playing |= playing,
            None => apps.push(AudioApp {
                label: friendly_app_label(&name),
                name,
                playing,
                path: String::new(),
            }),
        }
    }
    apps.sort_by(|left, right| {
        right
            .playing
            .cmp(&left.playing)
            .then_with(|| left.name.cmp(&right.name))
    });
    apps
}

fn error_response(status: StatusCode, message: &str) -> Response {
    (status, Json(serde_json::json!({ "error": message }))).into_response()
}

#[cfg(not(windows))]
pub async fn process_capture(Json(_request): Json<CaptureRequest>) -> Response {
    let _ = Body::empty();
    let _ = header::CONTENT_TYPE;
    error_response(
        StatusCode::NOT_IMPLEMENTED,
        "Captura por processo só está disponível no Windows.",
    )
}

#[cfg(not(windows))]
pub async fn stop_process_capture() {}

#[cfg(not(windows))]
pub fn list_audio_apps() -> Vec<AudioApp> {
    Vec::new()
}

#[cfg(windows)]
pub use windows_impl::{list_audio_apps, process_capture, stop_process_capture};

#[cfg(windows)]
mod windows_impl {
    use super::*;
    use axum::body::Bytes;
    use std::{
        collections::{HashSet, VecDeque},
        io,
        sync::{
            atomic::{AtomicBool, Ordering},
            mpsc as std_mpsc, Arc, Mutex, OnceLock,
        },
        thread,
        time::{Duration, Instant},
    };
    use sysinfo::{Pid, ProcessRefreshKind, ProcessesToUpdate, System, UpdateKind};
    use tokio::sync::{mpsc, oneshot};
    use tokio_stream::wrappers::ReceiverStream;
    use wasapi::{
        AudioClient, DeviceEnumerator, Direction, SampleType, SessionState, StreamMode, WaveFormat,
    };

    const RATE: u64 = 48_000;
    const CHANNELS: usize = 2;
    const TICK_FRAMES: u64 = 480; // 10 ms
    const MAX_BUFFERED_SAMPLES: usize = 9_600 * CHANNELS; // 200 ms por fonte
    const RECONCILE_EVERY: Duration = Duration::from_millis(750);
    const DEFAULT_EXCLUDED: [&str; 3] = ["discord.exe", "discordptb.exe", "discordcanary.exe"];

    type SourceBuffer = Arc<Mutex<VecDeque<f32>>>;
    type ReadySender = std_mpsc::Sender<Result<(), String>>;

    fn current_stop() -> &'static Mutex<Option<Arc<AtomicBool>>> {
        static CURRENT: OnceLock<Mutex<Option<Arc<AtomicBool>>>> = OnceLock::new();
        CURRENT.get_or_init(|| Mutex::new(None))
    }

    pub async fn stop_process_capture() {
        if let Some(stop) = current_stop().lock().unwrap().take() {
            stop.store(true, Ordering::SeqCst);
        }
    }

    pub async fn process_capture(Json(request): Json<CaptureRequest>) -> Response {
        let window_pid = match request.mode.as_str() {
            "window" => match request.hwnd.and_then(pid_from_hwnd) {
                Some(pid) => Some(pid),
                None => {
                    return error_response(
                        StatusCode::NOT_FOUND,
                        "Não foi possível descobrir o processo da janela.",
                    )
                }
            },
            "screen" => None,
            "apps" => {
                if request
                    .include_names
                    .iter()
                    .all(|name| name.trim().is_empty())
                {
                    return error_response(
                        StatusCode::BAD_REQUEST,
                        "Escolha ao menos um aplicativo.",
                    );
                }
                None
            }
            _ => return error_response(StatusCode::BAD_REQUEST, "Modo de captura inválido."),
        };
        let included: Option<HashSet<String>> = (request.mode == "apps").then(|| {
            request
                .include_names
                .iter()
                .map(|name| name.trim().to_lowercase())
                .filter(|name| !name.is_empty())
                .collect()
        });

        // Só uma captura por vez.
        let stop = Arc::new(AtomicBool::new(false));
        if let Some(previous) = current_stop().lock().unwrap().replace(stop.clone()) {
            previous.store(true, Ordering::SeqCst);
        }

        let (tx, rx) = mpsc::channel::<Result<Bytes, io::Error>>(64);
        let (ready_tx, ready_rx) = oneshot::channel::<Result<(), String>>();
        let mut excluded: HashSet<String> = DEFAULT_EXCLUDED
            .iter()
            .map(|name| name.to_string())
            .collect();
        excluded.extend(request.exclude_names.iter().map(|name| name.to_lowercase()));

        let thread_stop = stop.clone();
        let spawned = thread::Builder::new()
            .name("briks-process-audio".into())
            .spawn(move || mixer_thread(window_pid, excluded, included, thread_stop, tx, ready_tx));
        if spawned.is_err() {
            return error_response(
                StatusCode::INTERNAL_SERVER_ERROR,
                "Falha ao iniciar a captura.",
            );
        }

        match tokio::time::timeout(Duration::from_secs(4), ready_rx).await {
            Ok(Ok(Ok(()))) => Response::builder()
                .status(StatusCode::OK)
                .header(header::CONTENT_TYPE, "application/octet-stream")
                .header(header::CACHE_CONTROL, "no-store")
                .header("x-audio-format", "f32le;rate=48000;channels=2")
                .body(Body::from_stream(ReceiverStream::new(rx)))
                .unwrap_or_else(|_| {
                    error_response(StatusCode::INTERNAL_SERVER_ERROR, "Resposta inválida.")
                }),
            Ok(Ok(Err(message))) => {
                stop.store(true, Ordering::SeqCst);
                error_response(StatusCode::INTERNAL_SERVER_ERROR, &message)
            }
            _ => {
                stop.store(true, Ordering::SeqCst);
                error_response(
                    StatusCode::GATEWAY_TIMEOUT,
                    "A captura de áudio não iniciou a tempo.",
                )
            }
        }
    }

    fn pid_from_hwnd(hwnd: u64) -> Option<u32> {
        use windows::Win32::{Foundation::HWND, UI::WindowsAndMessaging::GetWindowThreadProcessId};
        let mut pid = 0u32;
        let handle = HWND(hwnd as usize as *mut core::ffi::c_void);
        unsafe { GetWindowThreadProcessId(handle, Some(&mut pid)) };
        (pid != 0).then_some(pid)
    }

    struct Source {
        pid: u32,
        buffer: SourceBuffer,
        stop: Arc<AtomicBool>,
        alive: Arc<AtomicBool>,
    }

    fn spawn_source(pid: u32, global_stop: Arc<AtomicBool>, ready: Option<ReadySender>) -> Source {
        let buffer: SourceBuffer = Arc::new(Mutex::new(VecDeque::new()));
        let stop = Arc::new(AtomicBool::new(false));
        let alive = Arc::new(AtomicBool::new(true));
        let (b, s, a) = (buffer.clone(), stop.clone(), alive.clone());
        let _ = thread::Builder::new()
            .name(format!("briks-capture-{pid}"))
            .spawn(move || {
                if let Err(error) = capture_process(pid, b, global_stop, s, ready) {
                    tracing::warn!(pid, %error, "captura de áudio do processo encerrada");
                }
                a.store(false, Ordering::SeqCst);
            });
        Source {
            pid,
            buffer,
            stop,
            alive,
        }
    }

    fn capture_process(
        pid: u32,
        buffer: SourceBuffer,
        global_stop: Arc<AtomicBool>,
        stop: Arc<AtomicBool>,
        ready: Option<ReadySender>,
    ) -> Result<(), String> {
        let init = || -> Result<_, String> {
            wasapi::initialize_mta().ok().map_err(|e| e.to_string())?;
            let format = WaveFormat::new(32, 32, &SampleType::Float, RATE as usize, CHANNELS, None);
            let mut client =
                AudioClient::new_application_loopback_client(pid, true).map_err(|e| {
                    format!("O Windows não permitiu capturar o áudio deste processo: {e}")
                })?;
            client
                .initialize_client(
                    &format,
                    &Direction::Capture,
                    &StreamMode::EventsShared {
                        autoconvert: true,
                        buffer_duration_hns: 0,
                    },
                )
                .map_err(|e| e.to_string())?;
            let event = client.set_get_eventhandle().map_err(|e| e.to_string())?;
            let capture = client.get_audiocaptureclient().map_err(|e| e.to_string())?;
            client.start_stream().map_err(|e| e.to_string())?;
            Ok((client, event, capture))
        };
        let (client, event, capture) = match init() {
            Ok(parts) => {
                if let Some(ready) = &ready {
                    let _ = ready.send(Ok(()));
                }
                parts
            }
            Err(error) => {
                if let Some(ready) = &ready {
                    let _ = ready.send(Err(error.clone()));
                }
                return Err(error);
            }
        };

        let mut raw: VecDeque<u8> = VecDeque::new();
        while !global_stop.load(Ordering::Relaxed) && !stop.load(Ordering::Relaxed) {
            loop {
                let frames = capture
                    .get_next_packet_size()
                    .map_err(|e| e.to_string())?
                    .unwrap_or(0);
                if frames == 0 {
                    break;
                }
                capture
                    .read_from_device_to_deque(&mut raw)
                    .map_err(|e| e.to_string())?;
            }
            if raw.len() >= 4 {
                let mut samples = buffer.lock().unwrap();
                while raw.len() >= 4 {
                    let bytes = [
                        raw.pop_front().unwrap(),
                        raw.pop_front().unwrap(),
                        raw.pop_front().unwrap(),
                        raw.pop_front().unwrap(),
                    ];
                    samples.push_back(f32::from_le_bytes(bytes));
                }
                while samples.len() > MAX_BUFFERED_SAMPLES {
                    samples.pop_front();
                }
            }
            // Timeout é normal: processos em silêncio não geram eventos.
            let _ = event.wait_for_event(50);
        }
        let _ = client.stop_stream();
        Ok(())
    }

    pub(super) fn mixer_thread(
        window_pid: Option<u32>,
        excluded: HashSet<String>,
        included: Option<HashSet<String>>,
        stop: Arc<AtomicBool>,
        tx: mpsc::Sender<Result<Bytes, io::Error>>,
        ready: oneshot::Sender<Result<(), String>>,
    ) {
        let mut sources: Vec<Source> = Vec::new();

        if let Some(pid) = window_pid {
            let (ready_tx, ready_rx) = std_mpsc::channel();
            sources.push(spawn_source(pid, stop.clone(), Some(ready_tx)));
            match ready_rx.recv_timeout(Duration::from_secs(3)) {
                Ok(Ok(())) => {
                    let _ = ready.send(Ok(()));
                }
                Ok(Err(message)) => {
                    let _ = ready.send(Err(message));
                    stop.store(true, Ordering::SeqCst);
                    return;
                }
                Err(_) => {
                    let _ = ready.send(Err("A captura do processo não respondeu.".into()));
                    stop.store(true, Ordering::SeqCst);
                    return;
                }
            }
        } else {
            let _ = ready.send(Ok(()));
        }

        let Ok(mut system) = System::new() else {
            stop.store(true, Ordering::SeqCst);
            return;
        };
        let _ = wasapi::initialize_mta();
        let start = Instant::now();
        let mut sent_frames: u64 = 0;
        let mut last_reconcile = Instant::now() - RECONCILE_EVERY;

        'outer: loop {
            if stop.load(Ordering::Relaxed) || tx.is_closed() {
                break;
            }
            if window_pid.is_none() {
                if last_reconcile.elapsed() >= RECONCILE_EVERY {
                    last_reconcile = Instant::now();
                    reconcile_screen_sources(
                        &mut sources,
                        &mut system,
                        &excluded,
                        included.as_ref(),
                        &stop,
                    );
                }
                sources.retain(|source| source.alive.load(Ordering::Relaxed));
            }

            let due = (start.elapsed().as_secs_f64() * RATE as f64) as u64;
            if due.saturating_sub(sent_frames) > RATE / 2 {
                sent_frames = due - TICK_FRAMES * 10; // atrasou demais: descarta e segue ao vivo
            }
            while sent_frames + TICK_FRAMES <= due {
                let mut out = vec![0f32; TICK_FRAMES as usize * CHANNELS];
                for source in &sources {
                    let mut samples = source.buffer.lock().unwrap();
                    for slot in out.iter_mut() {
                        match samples.pop_front() {
                            Some(value) => *slot += value,
                            None => break,
                        }
                    }
                }
                let mut bytes = Vec::with_capacity(out.len() * 4);
                for value in out {
                    bytes.extend_from_slice(&value.clamp(-1.0, 1.0).to_le_bytes());
                }
                if tx.blocking_send(Ok(Bytes::from(bytes))).is_err() {
                    break 'outer;
                }
                sent_frames += TICK_FRAMES;
            }
            thread::sleep(Duration::from_millis(5));
        }
        stop.store(true, Ordering::SeqCst);
        for source in &sources {
            source.stop.store(true, Ordering::SeqCst);
        }
    }

    fn reconcile_screen_sources(
        sources: &mut Vec<Source>,
        system: &mut System,
        excluded: &HashSet<String>,
        included: Option<&HashSet<String>>,
        global_stop: &Arc<AtomicBool>,
    ) {
        system.refresh_processes(ProcessesToUpdate::All, true);
        let own_pid = std::process::id();
        let own_root = system
            .process(Pid::from_u32(own_pid))
            .and_then(|p| p.parent())
            .map(|p| p.as_u32());

        let parent_of = |pid: u32| -> Option<u32> {
            system
                .process(Pid::from_u32(pid))
                .and_then(|p| p.parent())
                .map(|p| p.as_u32())
        };
        let is_excluded = |pid: u32| -> bool {
            let mut current = Some(pid);
            for _ in 0..32 {
                let Some(id) = current else { break };
                if id == own_pid || Some(id) == own_root {
                    return true;
                }
                let Some(process) = system.process(Pid::from_u32(id)) else {
                    break;
                };
                if excluded.contains(&process.name().to_string_lossy().to_lowercase()) {
                    return true;
                }
                current = parent_of(id);
            }
            false
        };
        let ancestors = |pid: u32| -> Vec<u32> {
            let mut result = Vec::new();
            let mut current = parent_of(pid);
            while let Some(id) = current {
                if result.len() >= 32 || result.contains(&id) {
                    break;
                }
                result.push(id);
                current = parent_of(id);
            }
            result
        };
        // Modo `apps`: o processo, ou um ancestral dele, precisa estar na lista escolhida.
        let is_included = |pid: u32| -> bool {
            let Some(names) = included else { return true };
            std::iter::once(pid).chain(ancestors(pid)).any(|id| {
                system
                    .process(Pid::from_u32(id))
                    .map(|process| names.contains(&process.name().to_string_lossy().to_lowercase()))
                    .unwrap_or(false)
            })
        };

        let candidates: HashSet<u32> = active_render_pids()
            .into_iter()
            .filter(|pid| *pid != 0 && !is_excluded(*pid) && is_included(*pid))
            .collect();
        let roots: Vec<u32> = candidates
            .iter()
            .copied()
            .filter(|pid| !ancestors(*pid).iter().any(|a| candidates.contains(a)))
            .collect();

        for root in roots {
            let root_ancestors = ancestors(root);
            let covered = sources
                .iter()
                .any(|source| source.pid == root || root_ancestors.contains(&source.pid));
            if covered {
                continue;
            }
            // Um ancestral novo cobre as fontes descendentes já abertas.
            sources.retain(|source| {
                let descendant = ancestors(source.pid).contains(&root);
                if descendant {
                    source.stop.store(true, Ordering::SeqCst);
                }
                !descendant
            });
            tracing::info!(pid = root, "capturando áudio do processo");
            sources.push(spawn_source(root, global_stop.clone(), None));
        }

        // Remove fontes cujo processo não existe mais.
        sources.retain(|source| {
            let exists = system.process(Pid::from_u32(source.pid)).is_some();
            if !exists {
                source.stop.store(true, Ordering::SeqCst);
            }
            exists
        });
    }

    /// Apps com sessão de áudio aberta (tocando ou em pausa), um item por executável.
    pub fn list_audio_apps() -> Vec<AudioApp> {
        let _ = wasapi::initialize_mta();
        let mut system = match System::new() {
            Ok(system) => system,
            Err(_) => return Vec::new(),
        };
        system.refresh_processes_specifics(
            ProcessesToUpdate::All,
            true,
            ProcessRefreshKind::nothing().with_exe(UpdateKind::OnlyIfNotSet),
        );
        let own_pid = std::process::id();
        let mut sessions: Vec<(String, bool)> = Vec::new();
        let mut paths: std::collections::HashMap<String, String> = std::collections::HashMap::new();
        let Ok(enumerator) = DeviceEnumerator::new() else {
            return Vec::new();
        };
        let Ok(collection) = enumerator.get_device_collection(&Direction::Render) else {
            return Vec::new();
        };
        for device in &collection {
            let Ok(device) = device else { continue };
            let Ok(manager) = device.get_iaudiosessionmanager() else {
                continue;
            };
            let Ok(list) = manager.get_audiosessionenumerator() else {
                continue;
            };
            let count = list.get_count().unwrap_or(0);
            for index in 0..count {
                let Ok(control) = list.get_session(index) else {
                    continue;
                };
                let state = control.get_state().unwrap_or(SessionState::Expired);
                if state == SessionState::Expired {
                    continue;
                }
                let Ok(pid) = control.get_process_id() else {
                    continue;
                };
                if pid == 0 || pid == own_pid {
                    continue;
                }
                let Some(process) = system.process(Pid::from_u32(pid)) else {
                    continue;
                };
                let executable = process.name().to_string_lossy().to_string();
                if let Some(path) = process.exe() {
                    paths
                        .entry(executable.trim().to_lowercase())
                        .or_insert_with(|| path.to_string_lossy().to_string());
                }
                sessions.push((executable, state == SessionState::Active));
            }
        }
        // Discord e o próprio Briks nunca entram na transmissão, então nem aparecem na lista.
        let mut apps = group_audio_apps(
            sessions,
            &[
                "discord.exe",
                "discordptb.exe",
                "discordcanary.exe",
                "briks.exe",
                "briks-desktop-backend.exe",
            ],
        );
        for app in &mut apps {
            if let Some(path) = paths.remove(&app.name) {
                app.path = path;
            }
        }
        apps
    }

    fn active_render_pids() -> Vec<u32> {
        let mut pids = Vec::new();
        let Ok(enumerator) = DeviceEnumerator::new() else {
            return pids;
        };
        let Ok(collection) = enumerator.get_device_collection(&Direction::Render) else {
            return pids;
        };
        for device in &collection {
            let Ok(device) = device else { continue };
            let Ok(manager) = device.get_iaudiosessionmanager() else {
                continue;
            };
            let Ok(sessions) = manager.get_audiosessionenumerator() else {
                continue;
            };
            let count = sessions.get_count().unwrap_or(0);
            for index in 0..count {
                let Ok(control) = sessions.get_session(index) else {
                    continue;
                };
                let active = control
                    .get_state()
                    .map(|state| state == SessionState::Active)
                    .unwrap_or(false);
                if active {
                    if let Ok(pid) = control.get_process_id() {
                        pids.push(pid);
                    }
                }
            }
        }
        pids
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rotulo_amigavel_vem_do_executavel() {
        assert_eq!(friendly_app_label("spotify.exe"), "Spotify");
        assert_eq!(friendly_app_label("chrome.exe"), "Chrome");
        assert_eq!(friendly_app_label("semextensao"), "Semextensao");
        assert_eq!(friendly_app_label(""), "");
    }

    #[test]
    fn agrupa_por_executavel_tocando_primeiro() {
        let apps = group_audio_apps(
            vec![
                ("Chrome.exe".into(), false),
                ("spotify.exe".into(), true),
                ("chrome.exe".into(), true),
                ("discord.exe".into(), true),
                ("".into(), true),
                ("spotify.exe".into(), false),
            ],
            &["discord.exe"],
        );
        let names: Vec<_> = apps
            .iter()
            .map(|app| (app.name.as_str(), app.playing))
            .collect();
        assert_eq!(names, vec![("chrome.exe", true), ("spotify.exe", true)]);
    }

    #[test]
    fn app_em_pausa_continua_na_lista() {
        let apps = group_audio_apps(vec![("spotify.exe".into(), false)], &[]);
        assert_eq!(
            apps,
            vec![AudioApp {
                name: "spotify.exe".into(),
                label: "Spotify".into(),
                playing: false,
                path: String::new()
            }]
        );
    }

    /// Manual: imprime os apps com sessão de áudio deste PC. `cargo test -- --ignored --nocapture lista_real`.
    #[cfg(windows)]
    #[test]
    #[ignore]
    fn lista_real_de_apps_com_audio() {
        let apps = list_audio_apps();
        for app in &apps {
            println!("app: {} ({}) tocando={}", app.name, app.label, app.playing);
        }
        println!("total: {}", apps.len());
    }

    /// Manual: captura ~3 s do app em `BRIKS_TEST_APP` (ex.: spotify.exe) e imprime o pico.
    /// `BRIKS_TEST_APP=spotify.exe cargo test -- --ignored --nocapture captura_real`.
    #[cfg(windows)]
    #[test]
    #[ignore]
    fn captura_real_do_app() {
        use std::{
            collections::HashSet,
            sync::{atomic::AtomicBool, Arc},
            time::{Duration, Instant},
        };
        let app = std::env::var("BRIKS_TEST_APP").unwrap_or_else(|_| "spotify.exe".into());
        let stop = Arc::new(AtomicBool::new(false));
        let (tx, mut rx) = tokio::sync::mpsc::channel(256);
        let (ready_tx, ready_rx) = tokio::sync::oneshot::channel();
        let included: HashSet<String> = [app.to_lowercase()].into_iter().collect();
        let excluded: HashSet<String> = ["discord.exe".to_string()].into_iter().collect();
        let thread_stop = stop.clone();
        let handle = std::thread::spawn(move || {
            windows_impl::mixer_thread(None, excluded, Some(included), thread_stop, tx, ready_tx)
        });
        assert!(ready_rx.blocking_recv().is_ok());
        let start = Instant::now();
        let (mut peak, mut bytes) = (0f32, 0usize);
        while start.elapsed() < Duration::from_secs(3) {
            if let Some(Ok(chunk)) = rx.blocking_recv() {
                bytes += chunk.len();
                for i in (0..chunk.len() / 4 * 4).step_by(4) {
                    peak = peak.max(
                        f32::from_le_bytes([chunk[i], chunk[i + 1], chunk[i + 2], chunk[i + 3]])
                            .abs(),
                    );
                }
            } else {
                break;
            }
        }
        stop.store(true, std::sync::atomic::Ordering::SeqCst);
        drop(rx);
        let _ = handle.join();
        println!("app={app} bytes={bytes} pico={peak:.4}");
    }
}
