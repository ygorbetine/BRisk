//! Câmera: enumeração e captura da webcam (Media Foundation, leitor de amostras) e uma câmera FALSA para testes.
//!
//! A captura entrega quadros BGRA ao mesmo `capture::Shared` da transmissão de tela (`SourceKind::Camera`), então o resto do
//! caminho (escala, H264, pacer, adaptação por perda, prévia) é o da transmissão — só com 360p/30 como teto.
//!
//! Câmera falsa (nunca abre a webcam real; é o que os testes automáticos usam), por `BRIKS_FAKE_CAMERA`:
//!   `1`      duas câmeras sintéticas (1280x720 e 640x480), com carimbo de relógio e marcas de canto para provar orientação
//!   `none`   nenhuma câmera (testa o aviso "não encontramos uma câmera")
//!   `denied` uma câmera cujo acesso o Windows nega (testa o aviso de permissão)
//!   `busy`   uma câmera que outro aplicativo já está usando
//! Sem a variável, vale a webcam real.
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::mpsc::Sender;
use std::sync::Arc;
use std::time::Duration;

use serde_json::{json, Value};

use crate::capture::Shared;

#[derive(Debug, Clone, PartialEq)]
pub struct CamInfo { pub id: String, pub name: String }

/// Por que a câmera não abriu. O texto já é para o usuário; o código (`code()`) é estável para a interface.
#[derive(Debug, Clone, PartialEq)]
pub enum CamError { NoDevice, Denied, Busy, Other(String) }
impl CamError {
    pub fn code(&self) -> &'static str { match self { CamError::NoDevice => "no_camera", CamError::Denied => "denied", CamError::Busy => "busy", CamError::Other(_) => "error" } }
    pub fn text(&self) -> String {
        match self {
            CamError::NoDevice => "Não encontramos uma câmera neste computador.".into(),
            CamError::Denied => "O Windows não deixou o Briks usar a câmera. Em Configurações > Privacidade e segurança > Câmera, permita o acesso de aplicativos de área de trabalho.".into(),
            CamError::Busy => "Outro aplicativo está usando a câmera. Feche-o e tente de novo.".into(),
            CamError::Other(m) => format!("Não foi possível abrir a câmera: {m}"),
        }
    }
}
impl std::fmt::Display for CamError {
    /// "[código] texto": o código viaja junto na mensagem de erro de `Capture::start` e a malha o separa de volta (`split_code`).
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result { write!(f, "[{}] {}", self.code(), self.text()) }
}
impl std::error::Error for CamError {}
/// Inverso do `Display` de `CamError`: ("código", "texto").
pub fn split_code(msg: &str) -> (&str, &str) {
    if let Some(r) = msg.strip_prefix('[') { if let Some((c, t)) = r.split_once("] ") { if !c.contains(' ') { return (c, t); } } }
    ("error", msg)
}

/// Câmeras abertas agora (a luz da câmera acende com isto > 0). Os testes conferem que volta a 0.
static OPEN: AtomicUsize = AtomicUsize::new(0);
pub fn open_count() -> usize { OPEN.load(Ordering::SeqCst) }
struct OpenGuard;
impl OpenGuard { fn new() -> Self { OPEN.fetch_add(1, Ordering::SeqCst); OpenGuard } }
impl Drop for OpenGuard { fn drop(&mut self) { OPEN.fetch_sub(1, Ordering::SeqCst); } }

fn fake_mode() -> Option<String> { std::env::var("BRIKS_FAKE_CAMERA").ok().map(|v| v.trim().to_ascii_lowercase()).filter(|v| !v.is_empty() && v != "0") }

/// Lista as câmeras. Não abre nenhuma (a luz não acende).
pub fn list() -> Vec<CamInfo> {
    match fake_mode().as_deref() {
        Some("none") => vec![],
        Some("denied") => vec![CamInfo { id: "fake:denied".into(), name: "Câmera de teste (acesso negado)".into() }],
        Some("busy") => vec![CamInfo { id: "fake:busy".into(), name: "Câmera de teste (em uso)".into() }],
        Some(_) => vec![CamInfo { id: "fake:0".into(), name: "Câmera de teste 1280x720".into() }, CamInfo { id: "fake:1".into(), name: "Câmera de teste 640x480".into() }],
        None => real::list(),
    }
}

/// Evento `cam.devices` para a UI.
pub fn devices_event() -> Value {
    let l = list();
    json!({ "ev": "cam.devices", "devices": l.iter().map(|c| json!({ "id": c.id, "name": c.name })).collect::<Vec<_>>() })
}

/// Qual câmera abrir: a pedida (id), senão a primeira.
fn pick(id: &str, all: &[CamInfo]) -> Result<CamInfo, CamError> {
    if all.is_empty() { return Err(CamError::NoDevice); }
    Ok(all.iter().find(|c| !id.is_empty() && c.id == id).or_else(|| all.first()).cloned().unwrap())
}

/// Corpo da thread de captura. Avisa por `otx` se abriu (uma vez); depois empurra quadros BGRA em `sh` até `st` ou erro.
pub fn run(id: &str, fps: u32, sh: Arc<Shared>, st: Arc<AtomicBool>, otx: Sender<Result<(), CamError>>) {
    let all = list();
    let dev = match pick(id, &all) { Ok(d) => d, Err(e) => { let _ = otx.send(Err(e)); return; } };
    if fake_mode().is_some() { fake_run(&dev, fps, sh, st, otx); } else { real::run(&dev, sh, st, otx); }
}

// ---------------------------------------------------------------------------------------------------------------------
// Câmera falsa
// ---------------------------------------------------------------------------------------------------------------------
/// Quadro sintético: degradê que anda, faixa móvel, carimbo de relógio no topo e marcas de canto (vermelho em cima à esquerda,
/// azul em cima à direita, verde embaixo à esquerda) para a orientação poder ser conferida depois de codificar.
pub fn fake_frame(w: usize, h: usize, t: usize) -> Vec<u8> {
    let mut f = fake_base(w, h);
    fake_decorate(&mut f, w, h, t);
    f
}

/// Parte fixa do quadro sintético (degradê e marcas de canto); calculada uma vez por câmera.
fn fake_base(w: usize, h: usize) -> Vec<u8> {
    let mut f = vec![255u8; w * h * 4];
    for y in 0..h { for x in 0..w {
        let o = (y * w + x) * 4;
        f[o] = (x * 200 / w) as u8; f[o + 1] = (y * 200 / h) as u8; f[o + 2] = 90;
    } }
    let m = (h / 9).max(8);
    let mut mark = |x0: usize, y0: usize, b: u8, g: u8, r: u8| { for y in y0..(y0 + m).min(h) { for x in x0..(x0 + m).min(w) { let o = (y * w + x) * 4; f[o] = b; f[o + 1] = g; f[o + 2] = r; } } };
    mark(0, (w / 40).max(4) + 2, 20, 20, 235);       // vermelho (BGR), logo abaixo do carimbo
    mark(w - m, (w / 40).max(4) + 2, 235, 20, 20);   // azul
    mark(0, h - m, 20, 235, 20);                     // verde
    f
}

/// Parte que muda a cada quadro: faixa móvel e carimbo de relógio.
fn fake_decorate(f: &mut [u8], w: usize, h: usize, t: usize) {
    let bx = (t * 6) % w;
    let bw = (w / 16).min(w - bx);
    let top = (w / 40).max(4) + 2 + (h / 9).max(8) + 2;   // abaixo das marcas
    for y in top..h.saturating_sub((h / 9).max(8) + 2) { let o = (y * w + bx) * 4; for p in f[o..o + bw * 4].chunks_exact_mut(4) { p[0] = 40; p[1] = 220; p[2] = 230; } }
    crate::capture::stamp_ms(f, w);
}

fn fake_run(dev: &CamInfo, fps: u32, sh: Arc<Shared>, st: Arc<AtomicBool>, otx: Sender<Result<(), CamError>>) {
    match dev.id.as_str() {
        "fake:denied" => { let _ = otx.send(Err(CamError::Denied)); return; }
        "fake:busy" => { let _ = otx.send(Err(CamError::Busy)); return; }
        _ => {}
    }
    let (w, h) = if dev.id == "fake:1" { (640usize, 480usize) } else { (1280, 720) };
    let _open = OpenGuard::new();
    let _ = otx.send(Ok(()));
    let mut t = 0usize;
    let base = fake_base(w, h);
    let period = Duration::from_micros(1_000_000 / fps.clamp(1, 60) as u64);
    let mut next = std::time::Instant::now();
    while !st.load(Ordering::Relaxed) {
        let mut f = base.clone();
        fake_decorate(&mut f, w, h, t);
        sh.push_bgra(&f, w, h, w * 4);
        t += 1;
        next += period;
        let now = std::time::Instant::now();
        if next > now { std::thread::sleep(next - now); } else { next = now; }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Webcam real (Media Foundation)
// ---------------------------------------------------------------------------------------------------------------------
mod real {
    use super::*;
    use windows::core::{Interface, GUID};
    use windows::Win32::Media::MediaFoundation::*;
    use windows::Win32::System::Com::{CoInitializeEx, CoTaskMemFree, COINIT_MULTITHREADED};

    /// Índice do "primeiro fluxo de vídeo" do leitor (MF_SOURCE_READER_FIRST_VIDEO_STREAM = 0xFFFFFFFC).
    const VIDEO: u32 = 0xFFFF_FFFC;

    fn init() -> Result<(), CamError> {
        unsafe { let _ = CoInitializeEx(None, COINIT_MULTITHREADED); }
        crate::mfenc::mf_startup().map_err(|e| CamError::Other(e.to_string()))
    }

    /// E_ACCESSDENIED = privacidade do Windows; os demais códigos comuns de "dispositivo em uso".
    fn map_err(e: &windows::core::Error) -> CamError {
        match e.code().0 as u32 {
            0x8007_0005 => CamError::Denied,
            0x8007_001F | 0x8007_0020 | 0xC00D_3704 | 0xC00D_3EA2 | 0x800700AA => CamError::Busy,
            0xC00D_36D5 | 0x8007_048F => CamError::NoDevice,
            c => CamError::Other(format!("{} (0x{c:08X})", e.message().trim())),
        }
    }

    fn str_attr(a: &IMFActivate, key: &GUID) -> String {
        unsafe {
            let (mut p, mut n) = (windows::core::PWSTR::null(), 0u32);
            if a.GetAllocatedString(key, &mut p, &mut n).is_err() || p.is_null() { return String::new(); }
            let s = p.to_string().unwrap_or_default();
            CoTaskMemFree(Some(p.0 as *const std::ffi::c_void));
            s
        }
    }

    fn enum_activates() -> Result<Vec<IMFActivate>, CamError> {
        init()?;
        unsafe {
            let mut attrs: Option<IMFAttributes> = None;
            MFCreateAttributes(&mut attrs, 1).map_err(|e| map_err(&e))?;
            let attrs = attrs.ok_or_else(|| CamError::Other("atributos".into()))?;
            attrs.SetGUID(&MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID).map_err(|e| map_err(&e))?;
            let (mut arr, mut n): (*mut Option<IMFActivate>, u32) = (std::ptr::null_mut(), 0);
            MFEnumDeviceSources(&attrs, &mut arr, &mut n).map_err(|e| map_err(&e))?;
            let mut out = Vec::new();
            for i in 0..n as usize { if let Some(a) = (*arr.add(i)).take() { out.push(a); } }
            if !arr.is_null() { CoTaskMemFree(Some(arr as *const std::ffi::c_void)); }
            Ok(out)
        }
    }

    pub fn list() -> Vec<CamInfo> {
        let Ok(acts) = enum_activates() else { return vec![] };
        acts.iter().map(|a| { let id = str_attr(a, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK); let name = str_attr(a, &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
            CamInfo { name: if name.is_empty() { "Câmera".into() } else { name }, id } }).filter(|c| !c.id.is_empty()).collect()
    }

    /// Preferência de formato nativo: o que o processador de vídeo do leitor converte para RGB32 sem decodificador (NV12/YUY2) vem antes do MJPEG.
    fn fmt_rank(g: &GUID) -> u32 { if *g == MFVideoFormat_NV12 { 0 } else if *g == MFVideoFormat_YUY2 { 1 } else if *g == MFVideoFormat_MJPG { 3 } else { 2 } }

    pub fn run(dev: &CamInfo, sh: Arc<Shared>, st: Arc<AtomicBool>, otx: Sender<Result<(), CamError>>) {
        match open(dev) {
            Ok((reader, w, h, _src)) => {
                let _open = OpenGuard::new();
                let _ = otx.send(Ok(()));
                let r = unsafe { read_loop(&reader, w, h, &sh, &st) };
                if let Err(e) = r { *sh.close_reason.lock().unwrap_or_else(|e| e.into_inner()) = e.text(); }
                sh.closed.store(true, Ordering::Relaxed);
                // o leitor e a fonte são liberados aqui (Drop): a luz apaga
            }
            Err(e) => { let _ = otx.send(Err(e)); }
        }
    }

    struct Src { source: IMFMediaSource }
    impl Drop for Src { fn drop(&mut self) { unsafe { let _ = self.source.Shutdown(); } } }

    /// Abre a câmera e escolhe o formato: o menor que tenha altura >= 360 (senão o maior abaixo disso), com pelo menos 24 qps e sem
    /// passar de 720p; o leitor converte para RGB32.
    fn open(dev: &CamInfo) -> Result<(IMFSourceReader, usize, usize, Src), CamError> {
        let acts = enum_activates()?;
        let act = acts.into_iter().find(|a| str_attr(a, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK) == dev.id).ok_or(CamError::NoDevice)?;
        unsafe {
            let source: IMFMediaSource = act.ActivateObject().map_err(|e| map_err(&e))?;
            let src = Src { source: source.clone() };
            let mut attrs: Option<IMFAttributes> = None;
            MFCreateAttributes(&mut attrs, 2).map_err(|e| map_err(&e))?;
            let attrs = attrs.ok_or_else(|| CamError::Other("atributos".into()))?;
            let _ = attrs.SetUINT32(&MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, 1);
            let reader = MFCreateSourceReaderFromMediaSource(&source, &attrs).map_err(|e| map_err(&e))?;
            // escolhe entre os formatos nativos
            let mut best: Option<((u32, u32, u32, u32, u32), IMFMediaType, usize, usize)> = None;
            let mut i = 0u32;
            while let Ok(mt) = reader.GetNativeMediaType(VIDEO, i) {
                i += 1;
                let (Ok(fs), Ok(sub)) = (mt.GetUINT64(&MF_MT_FRAME_SIZE), mt.GetGUID(&MF_MT_SUBTYPE)) else { continue };
                let (w, h) = ((fs >> 32) as usize, (fs & 0xFFFF_FFFF) as usize);
                if w < 160 || h < 120 { continue; }
                let fr = mt.GetUINT64(&MF_MT_FRAME_RATE).unwrap_or(30 << 32 | 1);
                let fps = ((fr >> 32) as f64 / ((fr & 0xFFFF_FFFF).max(1)) as f64).round() as u32;
                let score = ((fps < 24) as u32, (h > 720) as u32, (h < 360) as u32, (h as i32 - 360).unsigned_abs(), fmt_rank(&sub));
                if best.as_ref().map(|b| score < b.0).unwrap_or(true) { best = Some((score, mt, w, h)); }
            }
            let (_, native, _, _) = best.ok_or_else(|| CamError::Other("a câmera não informou formatos de vídeo".into()))?;
            reader.SetCurrentMediaType(VIDEO, None, &native).map_err(|e| map_err(&e))?;
            let out = MFCreateMediaType().map_err(|e| map_err(&e))?;
            out.SetGUID(&MF_MT_MAJOR_TYPE, &MFMediaType_Video).map_err(|e| map_err(&e))?;
            out.SetGUID(&MF_MT_SUBTYPE, &MFVideoFormat_RGB32).map_err(|e| map_err(&e))?;
            out.SetUINT32(&MF_MT_INTERLACE_MODE, 2).map_err(|e| map_err(&e))?;
            reader.SetCurrentMediaType(VIDEO, None, &out).map_err(|e| map_err(&e))?;
            let cur = reader.GetCurrentMediaType(VIDEO).map_err(|e| map_err(&e))?;
            let fs = cur.GetUINT64(&MF_MT_FRAME_SIZE).map_err(|e| map_err(&e))?;
            let (w, h) = ((fs >> 32) as usize, (fs & 0xFFFF_FFFF) as usize);
            if w < 2 || h < 2 { return Err(CamError::Other("tamanho de quadro inválido".into())); }
            Ok((reader, w, h, src))
        }
    }

    unsafe fn read_loop(reader: &IMFSourceReader, w: usize, h: usize, sh: &Shared, st: &AtomicBool) -> Result<(), CamError> {
        let default_stride = reader.GetCurrentMediaType(VIDEO).ok().and_then(|m| m.GetUINT32(&MF_MT_DEFAULT_STRIDE).ok()).map(|s| s as i32).unwrap_or(w as i32 * 4);
        let mut frame = vec![0u8; w * h * 4];
        let mut quiet = 0u32;
        while !st.load(Ordering::Relaxed) {
            let (mut flags, mut sample): (u32, Option<IMFSample>) = (0, None);
            reader.ReadSample(VIDEO, 0, None, Some(&mut flags), None, Some(&mut sample)).map_err(|e| map_err(&e))?;
            if flags & (MF_SOURCE_READERF_ENDOFSTREAM.0 as u32) != 0 { return Err(CamError::Other("a câmera foi desconectada".into())); }
            if flags & (MF_SOURCE_READERF_ERROR.0 as u32) != 0 { return Err(CamError::Other("erro de leitura da câmera".into())); }
            let Some(sample) = sample else { quiet += 1; if quiet > 600 { return Err(CamError::Other("a câmera parou de enviar imagem".into())); } continue };
            quiet = 0;
            let buf = sample.ConvertToContiguousBuffer().map_err(|e| map_err(&e))?;
            if let Ok(b2) = buf.cast::<IMF2DBuffer>() {
                let (mut p0, mut pitch) = (std::ptr::null_mut::<u8>(), 0i32);
                b2.Lock2D(&mut p0, &mut pitch).map_err(|e| map_err(&e))?;
                for r in 0..h { let row = std::slice::from_raw_parts(p0.offset(r as isize * pitch as isize), w * 4); frame[r * w * 4..(r + 1) * w * 4].copy_from_slice(row); }
                let _ = b2.Unlock2D();
            } else {
                let (mut p, mut maxl, mut curl) = (std::ptr::null_mut::<u8>(), 0u32, 0u32);
                buf.Lock(&mut p, Some(&mut maxl), Some(&mut curl)).map_err(|e| map_err(&e))?;
                let stride = default_stride.unsigned_abs() as usize;
                if (curl as usize) >= stride * (h - 1) + w * 4 {
                    for r in 0..h { let src_r = if default_stride < 0 { h - 1 - r } else { r }; let row = std::slice::from_raw_parts(p.add(src_r * stride), w * 4); frame[r * w * 4..(r + 1) * w * 4].copy_from_slice(row); }
                }
                let _ = buf.Unlock();
            }
            sh.push_bgra(&frame, w, h, w * 4);
        }
        Ok(())
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Teste da câmera fora da chamada (Configurações): captura só para mostrar a imagem; nada é codificado nem enviado.
// ---------------------------------------------------------------------------------------------------------------------
pub struct PreviewHandle { stop: Arc<AtomicBool> }
impl PreviewHandle { pub fn stop(&self) { self.stop.store(true, Ordering::Relaxed); } }
impl Drop for PreviewHandle { fn drop(&mut self) { self.stop(); } }

/// Abre a câmera numa thread e grava a prévia em `path` (arquivo de quadros compartilhado, como o da transmissão). Eventos `cam.preview`
/// (`state` on/off, `code` e `reason` se não abriu) saem por `tx`. A câmera fecha ao parar o teste, ao sair da tela ou ao ligar a câmera da chamada.
pub fn start_preview(device: String, path: std::path::PathBuf, tx: tokio::sync::mpsc::Sender<Value>) -> PreviewHandle {
    let stop = Arc::new(AtomicBool::new(false));
    let st = stop.clone();
    std::thread::spawn(move || {
        let cap = crate::capture::Capture::start(crate::capture::SourceKind::Camera(device), 30, 360, true, false);
        let mut cap = match cap {
            Ok(c) => c,
            Err(e) => { let es = e.to_string(); let (code, text) = split_code(&es); let _ = tx.try_send(json!({ "ev": "cam.preview", "state": "off", "code": code, "reason": text })); return; }
        };
        let mut sink = crate::video::FrameSink::create(&path).ok();
        let _ = tx.try_send(json!({ "ev": "cam.preview", "state": "on", "path": path.to_string_lossy() }));
        let mut reason = String::new();
        while !st.load(Ordering::Relaxed) {
            if cap.shared.closed.load(Ordering::Relaxed) { reason = cap.shared.close_reason.lock().unwrap_or_else(|e| e.into_inner()).clone(); break; }
            std::thread::sleep(Duration::from_millis(25));
            let pv = cap.shared.latest.lock().unwrap_or_else(|e| e.into_inner()).preview.take();
            if let (Some(s), Some((w, h, px))) = (sink.as_mut(), pv) { s.write(w, h, &px); }
        }
        cap.stop();   // espera a thread soltar o dispositivo: a luz apaga antes do evento abaixo
        let _ = tx.try_send(json!({ "ev": "cam.preview", "state": "off", "code": "", "reason": reason }));
    });
    PreviewHandle { stop }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn split_code_roundtrips_error_text() {
        for e in [CamError::NoDevice, CamError::Denied, CamError::Busy, CamError::Other("x".into())] {
            let s = e.to_string();
            let (c, t) = split_code(&s);
            assert_eq!(c, e.code());
            assert_eq!(t, e.text());
        }
        assert_eq!(split_code("sem código"), ("error", "sem código"));
    }

    #[test]
    fn fake_frame_has_corner_marks_and_changes_over_time() {
        let a = fake_frame(640, 360, 0);
        let b = fake_frame(640, 360, 5);
        assert_ne!(a, b);
        let m = (360 / 9usize).max(8);
        let px = |f: &Vec<u8>, x: usize, y: usize| { let o = (y * 640 + x) * 4; (f[o], f[o + 1], f[o + 2]) };
        let y0 = (640 / 40usize).max(4) + 2;
        assert_eq!(px(&a, 2, y0 + 2), (20, 20, 235), "vermelho em cima à esquerda");
        assert_eq!(px(&a, 640 - 3, y0 + 2), (235, 20, 20), "azul em cima à direita");
        assert_eq!(px(&a, 2, 360 - 3), (20, 235, 20), "verde embaixo à esquerda");
        let _ = m;
    }
}
