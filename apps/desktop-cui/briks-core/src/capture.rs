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
}

/// Último quadro capturado (BGRA sem preenchimento de linha).
#[derive(Default)]
pub struct Latest {
    pub w: usize,
    pub h: usize,
    pub data: Vec<u8>,
    pub counter: u64,
}

pub struct Shared {
    pub latest: Mutex<Latest>,
    pub closed: AtomicBool,
    pub frames: AtomicU64,
}

enum Handle {
    Wgc(Box<dyn FnOnce() + Send>),
    Thread,
}

pub struct Capture {
    pub shared: Arc<Shared>,
    stop: Arc<AtomicBool>,
    handle: Option<Handle>,
}

struct Handler { shared: Arc<Shared> }

impl GraphicsCaptureApiHandler for Handler {
    type Flags = Arc<Shared>;
    type Error = Box<dyn std::error::Error + Send + Sync>;
    fn new(ctx: Context<Self::Flags>) -> Result<Self, Self::Error> { Ok(Handler { shared: ctx.flags }) }
    fn on_frame_arrived(&mut self, frame: &mut Frame, _c: InternalCaptureControl) -> Result<(), Self::Error> {
        let (w, h) = (frame.width() as usize, frame.height() as usize);
        let mut fb = frame.buffer()?;
        let data = fb.as_nopadding_buffer()?;
        let mut l = self.shared.latest.lock().unwrap();
        l.w = w; l.h = h; l.counter += 1;
        l.data.clear(); l.data.extend_from_slice(&data[..w * h * 4]);
        self.shared.frames.fetch_add(1, Ordering::Relaxed);
        Ok(())
    }
    fn on_closed(&mut self) -> Result<(), Self::Error> { self.shared.closed.store(true, Ordering::Relaxed); Ok(()) }
}

/// Quadro sintético: degradê + xadrez + barra móvel + contador em blocos (determinístico por `t`). BGRA.
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

/// Carimba o relógio (ms, 32 bits) como 32 blocos 16x16 preto/branco no canto superior esquerdo (medição de latência nos testes).
pub fn stamp_ms(f: &mut [u8], w: usize) {
    let ms = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() as u32).unwrap_or(0);
    for i in 0..32 { let v = if (ms >> (31 - i)) & 1 == 1 { 255 } else { 0 };
        for y in 0..16 { for x in 0..16 { let o = (y * w + i * 16 + x) * 4; f[o] = v; f[o + 1] = v; f[o + 2] = v; } } }
}
/// Lê o carimbo de um quadro RGBA (inverso de `stamp_ms`).
pub fn read_stamp(rgba: &[u8], w: usize) -> u32 {
    let mut ms = 0u32;
    for i in 0..32 { let o = (8 * w + i * 16 + 8) * 4; ms = (ms << 1) | (rgba[o + 1] > 127) as u32; }
    ms
}

impl Capture {
    pub fn start(kind: SourceKind, fps: u32) -> Result<Capture> {
        let shared = Arc::new(Shared { latest: Mutex::new(Latest::default()), closed: AtomicBool::new(false), frames: AtomicU64::new(0) });
        let stop = Arc::new(AtomicBool::new(false));
        let interval = MinimumUpdateIntervalSettings::Custom(Duration::from_millis((1000 / fps.clamp(1, 120)) as u64));
        let handle = match kind {
            SourceKind::Test => {
                let (sh, st) = (shared.clone(), stop.clone());
                let (w, h) = std::env::var("BRIKS_TEST_SOURCE_SIZE").ok().and_then(|v| v.split_once('x').map(|(a, b)| (a.parse().unwrap_or(1280), b.parse().unwrap_or(720)))).unwrap_or((1280usize, 720usize));
                std::thread::spawn(move || {
                    let mut t = 0usize;
                    while !st.load(Ordering::Relaxed) {
                        {
                            let mut l = sh.latest.lock().unwrap();
                            l.w = w; l.h = h; l.counter += 1; l.data = test_pattern(w, h, t / 10); stamp_ms(&mut l.data, w);
                        }
                        sh.frames.fetch_add(1, Ordering::Relaxed);
                        t += 1;
                        std::thread::sleep(Duration::from_millis((1000 / fps.clamp(1, 120)) as u64));
                    }
                });
                Handle::Thread
            }
            SourceKind::Screen(h) => {
                let m = Monitor::from_raw_hmonitor(h as *mut std::ffi::c_void);
                let s = Settings::new(m, CursorCaptureSettings::WithCursor, DrawBorderSettings::Default, SecondaryWindowSettings::Default, interval, DirtyRegionSettings::Default, ColorFormat::Bgra8, shared.clone());
                let c: CaptureControl<Handler, _> = Handler::start_free_threaded(s).map_err(|e| anyhow!("captura de tela: {e}"))?;
                Handle::Wgc(Box::new(move || { let _ = c.stop(); }))
            }
            SourceKind::Window(h) => {
                let w = Window::from_raw_hwnd(h as *mut std::ffi::c_void);
                if !w.is_valid() { return Err(anyhow!("janela não existe mais")); }
                let s = Settings::new(w, CursorCaptureSettings::WithCursor, DrawBorderSettings::Default, SecondaryWindowSettings::Default, interval, DirtyRegionSettings::Default, ColorFormat::Bgra8, shared.clone());
                let c: CaptureControl<Handler, _> = Handler::start_free_threaded(s).map_err(|e| anyhow!("captura de janela: {e}"))?;
                Handle::Wgc(Box::new(move || { let _ = c.stop(); }))
            }
        };
        Ok(Capture { shared, stop, handle: Some(handle) })
    }

    pub fn stop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(Handle::Wgc(f)) = self.handle.take() { f(); }
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
        let id: isize = screens[0]["id"].as_str().unwrap().parse().unwrap();
        assert!(std::path::Path::new(screens[0]["thumb"].as_str().unwrap()).exists());
        let mut c = Capture::start(SourceKind::Screen(id), 30).unwrap();
        std::thread::sleep(Duration::from_millis(1500));
        let n = c.shared.frames.load(Ordering::Relaxed);
        let (w, h) = { let l = c.shared.latest.lock().unwrap(); (l.w, l.h) };
        c.stop();
        eprintln!("quadros em 1,5 s: {n} ({w}x{h})");
        assert!(n >= 1 && w > 0 && h > 0);
    }
}
