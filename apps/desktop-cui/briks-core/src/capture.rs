//! Captura de tela/janela (Windows Graphics Capture) e enumeração de fontes com miniaturas para o seletor.
//! Fonte `Test` = padrão sintético determinístico (sem GPU/tela), usada nos testes de ponta a ponta em loopback.
use std::path::Path;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::{anyhow, Result};
use serde_json::{json, Value};
use windows::Win32::Foundation::{HWND, RECT};
use windows::Win32::Graphics::Gdi::*;
use windows::Win32::Storage::Xps::{PrintWindow, PRINT_WINDOW_FLAGS};
use windows::Win32::UI::WindowsAndMessaging::GetWindowRect;
use windows_capture::capture::{CaptureControl, Context, GraphicsCaptureApiHandler};
use windows_capture::frame::Frame;
use windows_capture::graphics_capture_api::InternalCaptureControl;
use windows_capture::monitor::Monitor;
use windows_capture::settings::{ColorFormat, CursorCaptureSettings, DirtyRegionSettings, DrawBorderSettings, MinimumUpdateIntervalSettings, SecondaryWindowSettings, Settings};
use windows_capture::window::Window;

#[derive(Debug, Clone, PartialEq)]
pub enum SourceKind {
    Screen(isize),   // HMONITOR
    Window(isize),   // HWND
    Test,            // padrão sintético
    /// quadros totalmente pretos (como uma captura bloqueada por tela cheia exclusiva ou conteúdo protegido)
    TestBlack,
    /// ruído rolando em 1920x1080 a 30 qps: conteúdo difícil, faz o codificador usar todo o bitrate (testes de adaptação)
    TestMotion,
    TestResize,      // padrão sintético que muda de 1280x720 para 960x540 depois de ~2 s (janela redimensionada)
    /// webcam (id do dispositivo; vazio = a primeira); com `BRIKS_FAKE_CAMERA` é uma câmera sintética (ver camera.rs)
    Camera(String),
}

/// Último quadro capturado, já convertido para I420 no tamanho de envio (a conversão sai direto do buffer da captura:
/// nenhuma cópia do quadro BGRA inteiro, que numa tela 3440x1440 são 20 MB por quadro).
#[derive(Default)]
pub struct Latest {
    /// tamanho da origem
    pub w: usize,
    pub h: usize,
    pub yuv: Option<crate::video::Yuv>,
    /// prévia local RGBA (largura, altura, pixels), no máximo 1280 de largura, ~10 por segundo
    pub preview: Option<(usize, usize, Vec<u8>)>,
    /// primeiro quadro BGRA cru (só quando pedido, para provas)
    pub raw: Option<Vec<u8>>,
    /// idem, quando a conversão foi na GPU (não há BGRA na memória): o próprio I420 enviado
    pub raw_yuv: Option<crate::video::Yuv>,
    pub counter: u64,
}

pub struct Shared {
    pub latest: Mutex<Latest>,
    pub closed: AtomicBool,
    /// por que a fonte fechou, quando não é "a janela foi fechada" (câmera desconectada, por exemplo)
    pub close_reason: Mutex<String>,
    pub frames: AtomicU64,
    /// altura alvo do envio (a origem nunca é ampliada)
    pub target_h: std::sync::atomic::AtomicUsize,
    /// tempo gasto convertendo cor/escala (ns, acumulado) — entra na medida de sobrecarga local
    pub conv_ns: AtomicU64,
    pub want_preview: bool,
    pub want_raw: AtomicBool,
    pub preview_every: u64,
    /// intervalo mínimo entre quadros aceitos (a captura pode entregar mais rápido que o FPS pedido: o excesso é descartado sem converter)
    pub min_interval_us: AtomicU64,
    pub last_accept: Mutex<Option<std::time::Instant>>,
    pub dropped: AtomicU64,
}

impl Shared {
    /// Adaptação: novo FPS e nova altura de envio, valendo a partir do próximo quadro.
    pub fn retarget(&self, fps: u32, h: usize) {
        self.min_interval_us.store(800_000 / fps.clamp(1, 120) as u64, Ordering::Relaxed);
        self.target_h.store(h, Ordering::Relaxed);
    }
    /// Recebe um quadro BGRA (sem preenchimento de linha) da captura e guarda o I420 pronto para codificar.
    /// Este quadro entra? (a captura pode entregar mais rápido que o FPS pedido: o excesso é descartado ANTES de converter)
    pub fn accept(&self) -> bool {
        let mut la = self.last_accept.lock().unwrap_or_else(|e| e.into_inner());
        let now = std::time::Instant::now();
        if let Some(t) = *la { if now.duration_since(t) < Duration::from_micros(self.min_interval_us.load(Ordering::Relaxed)) { self.dropped.fetch_add(1, Ordering::Relaxed); return false; } }
        *la = Some(now);
        true
    }

    /// Quadro já convertido na GPU (I420 no tamanho de envio). `conv` = tempo gasto na conversão.
    pub fn push_yuv(&self, yuv: crate::video::Yuv, w: usize, h: usize, conv: Duration) {
        self.conv_ns.fetch_add(conv.as_nanos() as u64, Ordering::Relaxed);
        let n = self.frames.fetch_add(1, Ordering::Relaxed);
        let preview = if self.want_preview && n % self.preview_every == 0 {
            // prévia: metade do tamanho quando o envio passa de 1600 de largura
            let k = if yuv.w > 1600 { 2 } else { 1 };
            let (pw, ph) = ((yuv.w / k) & !1, (yuv.h / k) & !1);
            let mut out = vec![255u8; pw * ph * 4];
            if k == 1 { crate::video::i420_to_rgba(&yuv.y, &yuv.u, &yuv.v, (yuv.w, yuv.w / 2, yuv.w / 2), pw, ph, &mut out); }
            else { for y in 0..ph { for x in 0..pw {
                let (c, d, e) = ((yuv.y[y * 2 * yuv.w + x * 2] as i32 - 16).max(0) * 298, yuv.u[y * (yuv.w / 2) + x] as i32 - 128, yuv.v[y * (yuv.w / 2) + x] as i32 - 128);
                let o = (y * pw + x) * 4;
                out[o] = ((c + 409 * e + 128) >> 8).clamp(0, 255) as u8; out[o + 1] = ((c - 100 * d - 208 * e + 128) >> 8).clamp(0, 255) as u8; out[o + 2] = ((c + 516 * d + 128) >> 8).clamp(0, 255) as u8;
            } } }
            Some((pw, ph, out))
        } else { None };
        let raw_yuv = if self.want_raw.swap(false, Ordering::Relaxed) { Some(crate::video::Yuv { w: yuv.w, h: yuv.h, y: yuv.y.clone(), u: yuv.u.clone(), v: yuv.v.clone() }) } else { None };
        let mut l = self.latest.lock().unwrap_or_else(|e| e.into_inner());
        l.w = w; l.h = h; l.counter += 1; l.yuv = Some(yuv);
        if preview.is_some() { l.preview = preview; }
        if raw_yuv.is_some() { l.raw_yuv = raw_yuv; }
    }

    /// `stride` = bytes por linha na origem (>= w*4).
    pub fn push_bgra(&self, data: &[u8], w: usize, h: usize, stride: usize) {
        if w < 2 || h < 2 || stride < w * 4 || data.len() < stride * (h - 1) + w * 4 { return; }
        if !self.accept() { return; }
        let t0 = std::time::Instant::now();
        let (tw, th) = crate::video::fit_size(w, h, self.target_h.load(Ordering::Relaxed));
        let yuv = crate::video::bgra_to_i420_stride(data, stride, w, h, tw, th);
        self.conv_ns.fetch_add(t0.elapsed().as_nanos() as u64, Ordering::Relaxed);
        let n = self.frames.fetch_add(1, Ordering::Relaxed);
        let preview = if self.want_preview && n % self.preview_every == 0 { Some(preview_rgba(data, stride, w, h)) } else { None };
        let raw = if self.want_raw.swap(false, Ordering::Relaxed) { Some(data.chunks(stride).take(h).flat_map(|r| r[..w * 4].iter().copied()).collect()) } else { None };
        let mut l = self.latest.lock().unwrap_or_else(|e| e.into_inner());
        l.w = w; l.h = h; l.counter += 1; l.yuv = Some(yuv);
        if preview.is_some() { l.preview = preview; }
        if raw.is_some() { l.raw = raw; }
    }
}

/// Prévia local: reduz para no máximo 1280 de largura (média da área coberta por cada pixel novo; o vizinho mais próximo
/// pulava pixels e deixava o texto serrilhado e quebrado) e troca BGRA → RGBA.
fn preview_rgba(bgra: &[u8], stride: usize, w: usize, h: usize) -> (usize, usize, Vec<u8>) {
    let sc = (w as f32 / 1280.0).max(1.0);
    let (pw, ph) = (((w as f32 / sc) as usize).max(2) & !1, ((h as f32 / sc) as usize).max(2) & !1);
    let mut out = vec![255u8; pw * ph * 4];
    let span = |i: usize, n: usize| { let a = (i as f32 * sc) as usize; (a.min(n - 1), (((i + 1) as f32 * sc).ceil() as usize).clamp(a + 1, n)) };
    let xs: Vec<(usize, usize)> = (0..pw).map(|x| span(x, w)).collect();
    for (y, row) in out.chunks_exact_mut(pw * 4).enumerate() {
        let (y0, y1) = span(y, h);
        for (px, &(x0, x1)) in row.chunks_exact_mut(4).zip(xs.iter()) {
            let (mut b, mut g, mut r, mut n) = (0u32, 0u32, 0u32, 0u32);
            for yy in y0..y1 { let src = &bgra[yy * stride..]; for xx in x0..x1 { let o = xx * 4; b += src[o] as u32; g += src[o + 1] as u32; r += src[o + 2] as u32; n += 1; } }
            px[0] = (r / n) as u8; px[1] = (g / n) as u8; px[2] = (b / n) as u8;
        }
    }
    (pw, ph, out)
}

enum Handle {
    Wgc(Box<dyn FnOnce() + Send>),
    Thread,
    /// webcam: `stop` espera a thread soltar o dispositivo (a luz da câmera apaga antes de a chamada ou o envio terminarem)
    Cam(std::thread::JoinHandle<()>),
}

pub struct Capture {
    pub shared: Arc<Shared>,
    stop: Arc<AtomicBool>,
    handle: Option<Handle>,
}

struct Handler { logged_start: bool, staging_dbg: (u32, u32, u32, u32), shared: Arc<Shared>, gpu: Option<crate::gpuconv::GpuConv>, staging: Option<(windows::Win32::Graphics::Direct3D11::ID3D11Texture2D, u32, u32)> }

impl GraphicsCaptureApiHandler for Handler {
    type Flags = Arc<Shared>;
    type Error = Box<dyn std::error::Error + Send + Sync>;
    fn new(ctx: Context<Self::Flags>) -> Result<Self, Self::Error> { // conversão na GPU por padrão; BRIKS_CONVERT=cpu força o caminho antigo (diagnóstico / isolar problema de driver)
        let gpu = if std::env::var("BRIKS_CONVERT").as_deref() == Ok("cpu") { None } else { Some(crate::gpuconv::GpuConv::default()) };
        Ok(Handler { logged_start: false, staging_dbg: (0, 0, 0, 0), shared: ctx.flags, gpu, staging: None }) }
    fn on_frame_arrived(&mut self, frame: &mut Frame, _c: InternalCaptureControl) -> Result<(), Self::Error> {
        use windows::Win32::Graphics::Direct3D11::*;
        let (w, h) = (frame.width(), frame.height());
        // Uma única textura de leitura reaproveitada entre quadros (a biblioteca criaria uma nova de dezenas de MB a cada quadro);
        // a conversão de cor lê direto da memória mapeada, respeitando o passo de linha.
        if let Ok(f) = std::env::var("BRIKS_CAPTURE_LOG") { if !self.logged_start { self.logged_start = true; use std::io::Write; if let Ok(mut o) = std::fs::OpenOptions::new().create(true).append(true).open(f) { let _ = writeln!(o, "captura iniciada {w}x{h}, GPU {}", self.gpu.is_some()); } } }
        if let Some(g) = self.gpu.as_mut() {
            // GPU: escala + BGRA→NV12 no processador de vídeo; só o quadro reduzido volta para a memória
            let (tw, th) = crate::video::fit_size(w as usize, h as usize, self.shared.target_h.load(Ordering::Relaxed));
            if !self.shared.accept() { return Ok(()); }
            if let Ok(f) = std::env::var("BRIKS_CAPTURE_LOG") { use std::io::Write; let mut d = D3D11_TEXTURE2D_DESC::default(); unsafe { frame.as_raw_texture().GetDesc(&mut d); }
                if self.staging_dbg != (w, h, d.Width, d.Height) { self.staging_dbg = (w, h, d.Width, d.Height); if let Ok(mut o) = std::fs::OpenOptions::new().create(true).append(true).open(f) { let _ = writeln!(o, "quadro {w}x{h} textura {}x{} -> envio {tw}x{th}", d.Width, d.Height); } } }
            let t0 = std::time::Instant::now();
            let tex = unsafe { frame.as_raw_texture().clone() };
            match g.convert(&tex, w, h, tw as u32, th as u32) {
                Ok(yuv) => { self.shared.push_yuv(yuv, w as usize, h as usize, t0.elapsed()); return Ok(()); }
                Err(e) => { eprintln!("conversão na GPU indisponível ({e}); usando a CPU");
                    if let Ok(f) = std::env::var("BRIKS_CAPTURE_LOG") { use std::io::Write; if let Ok(mut o) = std::fs::OpenOptions::new().create(true).append(true).open(f) { let _ = writeln!(o, "GPU falhou em {w}x{h}: {e}"); } } self.gpu = None; *self.shared.last_accept.lock().unwrap() = None; }
            }
        }
        unsafe {
            let tex = frame.as_raw_texture().clone();
            let dev = tex.GetDevice()?;
            let ctx = dev.GetImmediateContext()?;
            if self.staging.as_ref().map(|s| (s.1, s.2) != (w, h)).unwrap_or(true) {
                let mut d = D3D11_TEXTURE2D_DESC::default();
                tex.GetDesc(&mut d);
                d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ.0 as u32; d.MiscFlags = 0;
                let mut st = None;
                dev.CreateTexture2D(&d, None, Some(&mut st))?;
                self.staging = st.map(|t| (t, w, h));
            }
            let Some((st, _, _)) = &self.staging else { return Ok(()) };
            ctx.CopyResource(st, &tex);
            let mut m = D3D11_MAPPED_SUBRESOURCE::default();
            ctx.Map(st, 0, D3D11_MAP_READ, 0, Some(&mut m))?;
            let pitch = m.RowPitch as usize;
            let data = std::slice::from_raw_parts(m.pData as *const u8, pitch * (h as usize - 1) + w as usize * 4);
            self.shared.push_bgra(data, w as usize, h as usize, pitch);
            ctx.Unmap(st, 0);
        }
        Ok(())
    }
    fn on_closed(&mut self) -> Result<(), Self::Error> { self.shared.closed.store(true, Ordering::Relaxed); Ok(()) }
}

pub fn window_pid(hwnd: isize) -> u32 { Window::from_raw_hwnd(hwnd as *mut std::ffi::c_void).process_id().unwrap_or(0) }

/// Quadro sintético: degradê + xadrez + barra móvel + contador em blocos (determinístico por `t`). BGRA.
/// Trava dos testes automáticos: com `BRIKS_SHARE_NO_PRIMARY` definida, o MONITOR PRINCIPAL do usuário nunca é capturado (o core
/// recusa o `share.start` e `Capture::start`), qualquer que seja o roteiro ou o clique que tenha escolhido a fonte.
pub fn refuses_primary(hm: isize) -> bool {
    if std::env::var_os("BRIKS_SHARE_NO_PRIMARY").is_none() { return false; }
    use windows::Win32::Graphics::Gdi::{GetMonitorInfoW, HMONITOR, MONITORINFO};
    let mut mi = MONITORINFO { cbSize: std::mem::size_of::<MONITORINFO>() as u32, ..Default::default() };
    unsafe { !GetMonitorInfoW(HMONITOR(hm as *mut std::ffi::c_void), &mut mi).as_bool() || mi.dwFlags & 1 == 1 }   // não deu para saber = recusa
}

pub fn test_pattern(w: usize, h: usize, t: usize) -> Vec<u8> {
    let mut f = vec![255u8; w * h * 4];
    for y in 0..h { for x in 0..w {
        let o = (y * w + x) * 4;
        f[o] = (x * 255 / w) as u8; f[o + 1] = (y * 255 / h) as u8; f[o + 2] = 128;
        if (x / 64 + y / 64) % 2 == 0 { f[o + 2] = 220; }
        let bx = (t * 8) % w;
        if x >= bx && x < bx + 40 { f[o] = 255; f[o + 1] = 255; f[o + 2] = 0; }
    } }
    f
}

/// Carimba o relógio (ms, 32 bits) como 32 blocos preto/branco de lado w/40 no topo (medição de latência nos testes). O lado
/// proporcional à largura mantém o carimbo legível depois de reduzido (a 360p cada bloco ainda tem 16 px).
pub fn stamp_ms(f: &mut [u8], w: usize) {
    let ms = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() as u32).unwrap_or(0);
    let b = (w / 40).max(4);
    for i in 0..32 { let v = if (ms >> (31 - i)) & 1 == 1 { 255 } else { 0 };
        for y in 0..b { for x in 0..b { let o = (y * w + i * b + x) * 4; f[o] = v; f[o + 1] = v; f[o + 2] = v; } } }
}
/// Lê o carimbo de um quadro RGBA (inverso de `stamp_ms`).
pub fn read_stamp(rgba: &[u8], w: usize) -> u32 { read_stamp_scaled(rgba, w, w) }
/// O lado do bloco na fonte, para os testes que mascaram a faixa do carimbo.
pub fn stamp_rows(w: usize) -> usize { (w / 40).max(4) }
/// Igual, para um quadro recebido em tamanho diferente do original (`src_w` = largura em que o carimbo foi desenhado).
pub fn read_stamp_scaled(rgba: &[u8], w: usize, src_w: usize) -> u32 {
    let _ = src_w;   // o bloco é proporcional à largura: a escala já está embutida
    let b = w as f32 / 40.0;
    let mut ms = 0u32;
    for i in 0..32 { let (x, y) = (((i as f32 + 0.5) * b) as usize, (0.5 * b) as usize); let o = (y * w + x.min(w - 1)) * 4; ms = (ms << 1) | (rgba[o + 1] > 127) as u32; }
    ms
}

impl Capture {
    /// `target_h` = altura de envio; `preview` = gerar a prévia local; `raw` = guardar o 1º quadro cru (provas).
    pub fn start(kind: SourceKind, fps: u32, target_h: usize, preview: bool, raw: bool) -> Result<Capture> {
        let shared = Arc::new(Shared { latest: Mutex::new(Latest::default()), closed: AtomicBool::new(false), close_reason: Mutex::new(String::new()), frames: AtomicU64::new(0), target_h: std::sync::atomic::AtomicUsize::new(target_h), conv_ns: AtomicU64::new(0), want_preview: preview,
            want_raw: AtomicBool::new(raw), preview_every: if matches!(kind, SourceKind::Camera(_)) { 2 } else { (fps as u64 / 10).max(1) },
            min_interval_us: AtomicU64::new(800_000 / fps.clamp(1, 120) as u64), last_accept: Mutex::new(None), dropped: AtomicU64::new(0) });
        let stop = Arc::new(AtomicBool::new(false));
        let interval = MinimumUpdateIntervalSettings::Custom(Duration::from_millis((1000 / fps.clamp(1, 120)) as u64));
        let handle = match kind {
            SourceKind::TestMotion => {
                let (sh, st) = (shared.clone(), stop.clone());
                std::thread::spawn(move || {
                    let (w, h) = (1920usize, 1080usize);
                    // "Área de trabalho" sintética: ladrilhos lisos com linhas finas (comprime bem, como uma interface) e, por cima,
                    // uma janela de 960x540 com ruído que muda a cada quadro (como um vídeo/jogo numa janela: custa bits de verdade).
                    let mut base = vec![255u8; w * h * 4];
                    for y in 0..h { for x in 0..w {
                        let o = (y * w + x) * 4; let (tx, ty) = (x / 96, y / 96);
                        let line = x % 96 < 2 || y % 96 < 2;
                        base[o] = if line { 230 } else { (40 + (tx * 37 + ty * 11) % 80) as u8 };
                        base[o + 1] = if line { 230 } else { (30 + (tx * 13 + ty * 29) % 70) as u8 };
                        base[o + 2] = if line { 230 } else { (50 + (tx * 7 + ty * 43) % 90) as u8 };
                    } }
                    let mut x = 0x2545F491u32;
                    let mut f = vec![255u8; w * h * 4];
                    let mut t = 0usize;
                    let mut next = std::time::Instant::now();
                    while !st.load(Ordering::Relaxed) {
                        let shift = (t * 2 % w) * 4;   // a "interface" rola devagar
                        for y in 0..h { let row = &base[y * w * 4..(y + 1) * w * 4]; let out = &mut f[y * w * 4..(y + 1) * w * 4]; out[..w * 4 - shift].copy_from_slice(&row[shift..]); out[w * 4 - shift..].copy_from_slice(&row[..shift]); }
                        for by in 0..135 { for bx in 0..240 {   // janela ruidosa em blocos 4x4, nova a cada quadro
                            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                            let (b, g, r) = ((x & 255) as u8, ((x >> 8) & 255) as u8, ((x >> 16) & 255) as u8);
                            for dy in 0..4 { let o = ((270 + by * 4 + dy) * w + 480 + bx * 4) * 4; for dx in 0..4 { f[o + dx * 4] = b; f[o + dx * 4 + 1] = g; f[o + dx * 4 + 2] = r; } }
                        } }
                        stamp_ms(&mut f, w);
                        sh.push_bgra(&f, w, h, w * 4);
                        t += 1;
                        next += Duration::from_micros(1_000_000 / 60);   // oferece 60 qps: quem limita é o FPS alvo
                        let now = std::time::Instant::now();
                        if next > now { std::thread::sleep(next - now); } else { next = now; }
                    }
                });
                Handle::Thread
            }
            SourceKind::Test | SourceKind::TestResize | SourceKind::TestBlack => {
                let resize = kind == SourceKind::TestResize;
                let black = kind == SourceKind::TestBlack;
                let (sh, st) = (shared.clone(), stop.clone());
                let (w, h) = std::env::var("BRIKS_TEST_SOURCE_SIZE").ok().and_then(|v| v.split_once('x').map(|(a, b)| (a.parse().unwrap_or(1280), b.parse().unwrap_or(720)))).unwrap_or((1280usize, 720usize));
                std::thread::spawn(move || {
                    let mut t = 0usize;
                    while !st.load(Ordering::Relaxed) {
                        let (w, h) = if resize && t > fps as usize * 2 { (960, 540) } else { (w, h) };
                        let mut f = if black { let mut v = vec![0u8; w * h * 4]; for p in v.chunks_mut(4) { p[3] = 255; } v } else { test_pattern(w, h, t / 10) };
                        if !black { stamp_ms(&mut f, w); }
                        sh.push_bgra(&f, w, h, w * 4);
                        t += 1;
                        std::thread::sleep(Duration::from_millis((1000 / fps.clamp(1, 120)) as u64));
                    }
                });
                Handle::Thread
            }
            SourceKind::Camera(id) => {
                let (sh, st) = (shared.clone(), stop.clone());
                let (otx, orx) = std::sync::mpsc::channel::<Result<(), crate::camera::CamError>>();
                let jh = std::thread::spawn(move || crate::camera::run(&id, fps, sh, st, otx));
                match orx.recv_timeout(Duration::from_secs(10)) {
                    Ok(Ok(())) => Handle::Cam(jh),
                    Ok(Err(e)) => { let _ = jh.join(); return Err(anyhow!(e.to_string())); }
                    Err(_) => { stop.store(true, Ordering::Relaxed); return Err(anyhow!(crate::camera::CamError::Other("a câmera não respondeu".into()).to_string())); }
                }
            }
            SourceKind::Screen(h) => {
                if refuses_primary(h) { return Err(anyhow!("teste: a tela principal está bloqueada (BRIKS_SHARE_NO_PRIMARY)")); }
                let m = Monitor::from_raw_hmonitor(h as *mut std::ffi::c_void);
                let mk = |b: DrawBorderSettings| Settings::new(m, CursorCaptureSettings::WithCursor, b, SecondaryWindowSettings::Default, interval, DirtyRegionSettings::Default, ColorFormat::Bgra8, shared.clone());
                let c: CaptureControl<Handler, _> = match Handler::start_free_threaded(mk(DrawBorderSettings::WithoutBorder)) {
                    Ok(c) => c,
                    Err(e) => { eprintln!("captura sem contorno amarelo indisponível ({e}); usando o contorno padrão"); Handler::start_free_threaded(mk(DrawBorderSettings::Default)).map_err(|e| anyhow!("captura de tela: {e}"))? }
                };
                Handle::Wgc(Box::new(move || { let _ = c.stop(); }))
            }
            SourceKind::Window(h) => {
                let w = Window::from_raw_hwnd(h as *mut std::ffi::c_void);
                if !w.is_valid() { return Err(anyhow!("janela não existe mais")); }
                let mk = |b: DrawBorderSettings| Settings::new(w, CursorCaptureSettings::WithCursor, b, SecondaryWindowSettings::Default, interval, DirtyRegionSettings::Default, ColorFormat::Bgra8, shared.clone());
                let c: CaptureControl<Handler, _> = match Handler::start_free_threaded(mk(DrawBorderSettings::WithoutBorder)) {
                    Ok(c) => c,
                    Err(e) => { eprintln!("captura sem contorno amarelo indisponível ({e}); usando o contorno padrão"); Handler::start_free_threaded(mk(DrawBorderSettings::Default)).map_err(|e| anyhow!("captura de janela: {e}"))? }
                };
                Handle::Wgc(Box::new(move || { let _ = c.stop(); }))
            }
        };
        Ok(Capture { shared, stop, handle: Some(handle) })
    }

    pub fn stop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        match self.handle.take() { Some(Handle::Wgc(f)) => f(), Some(Handle::Cam(j)) => { let _ = j.join(); } _ => {} }
    }
}
impl Drop for Capture { fn drop(&mut self) { self.stop(); } }

// ---------------------------------------------------------------------------------------------------------------------
// Enumeração + miniaturas
// ---------------------------------------------------------------------------------------------------------------------
const THUMB_W: i32 = 320;
const THUMB_H: i32 = 180;

/// Copia `src` (DC) para uma miniatura THUMB_W x THUMB_H preservando a proporção (fundo escuro) e devolve RGBA.
unsafe fn thumb_from(src_w: i32, src_h: i32, draw: impl FnOnce(HDC, i32, i32)) -> Option<Vec<u8>> {
    if src_w <= 0 || src_h <= 0 { return None; }
    let screen = GetDC(None);
    let mem = CreateCompatibleDC(Some(screen));
    // 1) bitmap do tamanho original onde a fonte é desenhada
    let big = CreateCompatibleBitmap(screen, src_w, src_h);
    let old = SelectObject(mem, big.into());
    draw(mem, src_w, src_h);
    // 2) miniatura
    let sc = (THUMB_W as f32 / src_w as f32).min(THUMB_H as f32 / src_h as f32);
    let (tw, th) = (((src_w as f32 * sc) as i32).max(1), ((src_h as f32 * sc) as i32).max(1));
    let dst = CreateCompatibleDC(Some(screen));
    let small = CreateCompatibleBitmap(screen, THUMB_W, THUMB_H);
    let old2 = SelectObject(dst, small.into());
    let br = CreateSolidBrush(windows::Win32::Foundation::COLORREF(0x00260c10));
    FillRect(dst, &RECT { left: 0, top: 0, right: THUMB_W, bottom: THUMB_H }, br);
    SetStretchBltMode(dst, HALFTONE);
    let _ = SetBrushOrgEx(dst, 0, 0, None);
    let _ = StretchBlt(dst, (THUMB_W - tw) / 2, (THUMB_H - th) / 2, tw, th, Some(mem), 0, 0, src_w, src_h, SRCCOPY);
    let mut bi = BITMAPINFO::default();
    bi.bmiHeader = BITMAPINFOHEADER { biSize: std::mem::size_of::<BITMAPINFOHEADER>() as u32, biWidth: THUMB_W, biHeight: -THUMB_H, biPlanes: 1, biBitCount: 32, biCompression: 0, ..Default::default() };
    let mut px = vec![0u8; (THUMB_W * THUMB_H * 4) as usize];
    let ok = GetDIBits(dst, small, 0, THUMB_H as u32, Some(px.as_mut_ptr() as *mut _), &mut bi, DIB_RGB_COLORS);
    SelectObject(dst, old2); SelectObject(mem, old);
    let _ = DeleteObject(small.into()); let _ = DeleteObject(big.into()); let _ = DeleteObject(br.into());
    let _ = DeleteDC(dst); let _ = DeleteDC(mem); ReleaseDC(None, screen);
    if ok == 0 { return None; }
    for p in px.chunks_mut(4) { p.swap(0, 2); p[3] = 255; }
    Some(px)
}

fn save_png(path: &Path, rgba: Vec<u8>) -> Result<()> {
    let img = image::RgbaImage::from_raw(THUMB_W as u32, THUMB_H as u32, rgba).ok_or_else(|| anyhow!("png"))?;
    img.save_with_format(path, image::ImageFormat::Png)?;
    Ok(())
}

/// Telas e janelas capturáveis, com miniaturas PNG em `dir`. `me_exe` = nomes de processo a esconder (o próprio Briks).
pub fn list_sources(dir: &Path) -> Value {
    let _ = std::fs::create_dir_all(dir);
    let mut screens = vec![];
    if let Ok(ms) = Monitor::enumerate() {
        for (i, m) in ms.iter().enumerate() {
            let hm = m.as_raw_hmonitor() as isize;
            let (w, h) = (m.width().unwrap_or(0) as i32, m.height().unwrap_or(0) as i32);
            let mut mi = MONITORINFO { cbSize: std::mem::size_of::<MONITORINFO>() as u32, ..Default::default() };
            let (mut left, mut top, mut primary) = (0, 0, false);
            unsafe { if GetMonitorInfoW(HMONITOR(m.as_raw_hmonitor()), &mut mi).as_bool() { left = mi.rcMonitor.left; top = mi.rcMonitor.top; primary = mi.dwFlags & 1 == 1; } }
            let thumb = unsafe { thumb_from(w, h, |dc, w, h| { let sc = GetDC(None); let _ = BitBlt(dc, 0, 0, w, h, Some(sc), left, top, SRCCOPY); ReleaseDC(None, sc); }) };
            let tp = dir.join(format!("screen-{hm}.png"));
            let has = thumb.map(|t| save_png(&tp, t).is_ok()).unwrap_or(false);
            screens.push(json!({ "id": hm.to_string(), "name": format!("Tela {}", i + 1), "w": w, "h": h, "primary": primary, "thumb": if has { tp.to_string_lossy().to_string() } else { String::new() } }));
        }
    }
    let mut windows = vec![];
    if let Ok(ws) = Window::enumerate() {
        for w in ws {
            let Ok(title) = w.title() else { continue };
            if title.trim().is_empty() { continue; }
            let exe = w.process_name().unwrap_or_default();
            if exe.to_lowercase().starts_with("briks") { continue; }
            let hw = w.as_raw_hwnd() as isize;
            let mut r = RECT::default();
            unsafe { if GetWindowRect(HWND(hw as *mut _), &mut r).is_err() { continue; } }
            let (ww, wh) = (r.right - r.left, r.bottom - r.top);
            if ww < 120 || wh < 80 { continue; }
            let thumb = unsafe { thumb_from(ww, wh, |dc, _, _| { let _ = PrintWindow(HWND(hw as *mut _), dc, PRINT_WINDOW_FLAGS(2)); }) };
            let tp = dir.join(format!("win-{hw}.png"));
            let has = thumb.map(|t| save_png(&tp, t).is_ok()).unwrap_or(false);
            windows.push(json!({ "id": hw.to_string(), "title": title.chars().take(90).collect::<String>(), "exe": exe, "pid": w.process_id().unwrap_or(0), "w": ww, "h": wh, "thumb": if has { tp.to_string_lossy().to_string() } else { String::new() } }));
        }
    }
    json!({ "screens": screens, "windows": windows })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lists_screens_and_captures_real_frames() {
        let dir = std::env::temp_dir().join(format!("briks-thumbs-{}", std::process::id()));
        let v = list_sources(&dir);
        let screens = v["screens"].as_array().unwrap();
        eprintln!("telas: {} janelas: {}", screens.len(), v["windows"].as_array().unwrap().len());
        assert!(!screens.is_empty(), "sem telas");
        // captura de verdade só na 2ª tela (nunca o monitor principal do usuário); sem ela, o teste fica só na enumeração
        let Some(scr) = screens.iter().find(|s| s["primary"] != true) else { eprintln!("sem 2ª tela: captura real pulada"); return };
        let id: isize = scr["id"].as_str().unwrap().parse().unwrap();
        assert!(std::path::Path::new(scr["thumb"].as_str().unwrap()).exists());
        let mut c = Capture::start(SourceKind::Screen(id), 30, 1080, true, false).unwrap();
        std::thread::sleep(Duration::from_millis(1500));
        let n = c.shared.frames.load(Ordering::Relaxed);
        let (w, h) = { let l = c.shared.latest.lock().unwrap(); (l.w, l.h) };
        c.stop();
        eprintln!("quadros em 1,5 s: {n} ({w}x{h})");
        assert!(n >= 1 && w > 0 && h > 0);
    }
}

#[cfg(test)]
mod guard_tests {
    /// A trava impede capturar o monitor principal e deixa a 2ª tela passar. Só consulta os monitores (não captura nada).
    #[test]
    fn primary_monitor_is_refused_when_the_guard_is_on() {
        let dir = std::env::temp_dir().join(format!("briks-guard-{}", std::process::id()));
        let _ = std::fs::create_dir_all(&dir);
        let list = super::list_sources(&dir);
        let _ = std::fs::remove_dir_all(&dir);   // as miniaturas da enumeração não ficam no disco
        let screens = list["screens"].as_array().unwrap().clone();
        std::env::set_var("BRIKS_SHARE_NO_PRIMARY", "1");
        let mut saw_primary = false;
        for s in &screens {
            let hm: isize = s["id"].as_str().unwrap().parse().unwrap();
            if s["primary"] == true { saw_primary = true; assert!(super::refuses_primary(hm), "o monitor principal NÃO foi recusado"); }
            else { assert!(!super::refuses_primary(hm), "uma tela secundária foi recusada"); }
        }
        assert!(saw_primary, "sem monitor principal na enumeração");
        std::env::remove_var("BRIKS_SHARE_NO_PRIMARY");
        for s in &screens { assert!(!super::refuses_primary(s["id"].as_str().unwrap().parse().unwrap()), "com a trava desligada nada é recusado"); }
    }
}

#[cfg(test)]
mod preview_tests {
    use super::*;
    /// listras de 1 px (preto/branco): a média da área dá cinza em todo o quadro, sem pixel puro (o vizinho mais próximo dava só preto/branco).
    #[test]
    fn preview_downscale_averages_instead_of_dropping_pixels() {
        let (w, h) = (1920usize, 1080usize);
        let mut f = vec![255u8; w * h * 4];
        for y in 0..h { for x in 0..w { let v = if x % 2 == 0 { 0 } else { 255 }; let o = (y * w + x) * 4; f[o] = v; f[o + 1] = v; f[o + 2] = v; } }
        let (pw, ph, out) = preview_rgba(&f, w * 4, w, h);
        assert_eq!((pw, ph), (1280, 720));
        let pure = out.chunks(4).filter(|p| p[0] == 0 || p[0] == 255).count();
        assert!(pure * 10 < out.len() / 4, "{pure} pixels puros de {}", out.len() / 4);
    }
}
