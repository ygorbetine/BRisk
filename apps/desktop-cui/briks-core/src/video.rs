//! Vídeo da transmissão de tela: conversão BGRA → I420 (com escala), codificador/decodificador H264 por software (OpenH264) e a
//! memória compartilhada por onde os quadros chegam à UI (arquivo mapeado, seqlock; a UI em C sobe cada quadro como textura OpenGL).
use std::path::Path;
use std::sync::atomic::{AtomicU32, Ordering};

use anyhow::{anyhow, Result};
use memmap2::MmapMut;
use openh264::decoder::Decoder;
use openh264::encoder::{Encoder, EncoderConfig, RateControlMode, UsageType};
use openh264::formats::YUVSource;
use openh264::OpenH264API;

/// Resolução máxima suportada pelo quadro compartilhado (4K).
pub const MAX_W: usize = 3840;
pub const MAX_H: usize = 2160;
pub const HDR: usize = 64;

// ---------------------------------------------------------------------------------------------------------------------
// Memória compartilhada (core escreve, UI lê). Layout (little endian):
//   0  u32 magic 'BKFR'      4  u32 seq (ímpar = escrevendo)   8  u32 largura   12 u32 altura
//   16 u64 nº do quadro      24 u64 relógio (ms, epoch) da gravação              64.. RGBA (largura*altura*4)
// ---------------------------------------------------------------------------------------------------------------------
pub struct FrameSink {
    map: MmapMut,
    n: u64,
}

impl FrameSink {
    pub fn create(path: &Path) -> Result<Self> {
        if let Some(d) = path.parent() { std::fs::create_dir_all(d)?; }
        let f = std::fs::OpenOptions::new().read(true).write(true).create(true).truncate(true).open(path)?;
        f.set_len((HDR + MAX_W * MAX_H * 4) as u64)?;
        let mut map = unsafe { MmapMut::map_mut(&f)? };
        map[0..4].copy_from_slice(b"BKFR");
        Ok(FrameSink { map, n: 0 })
    }

    fn seq(&self) -> &AtomicU32 { unsafe { &*(self.map.as_ptr().add(4) as *const AtomicU32) } }

    /// Grava um quadro RGBA (w*h*4 bytes).
    pub fn write(&mut self, w: usize, h: usize, rgba: &[u8]) {
        if w == 0 || h == 0 || w > MAX_W || h > MAX_H || rgba.len() < w * h * 4 { return; }
        let s = self.seq().load(Ordering::Relaxed);
        self.seq().store(s.wrapping_add(1) | 1, Ordering::Release);
        self.n += 1;
        let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0);
        self.map[8..12].copy_from_slice(&(w as u32).to_le_bytes());
        self.map[12..16].copy_from_slice(&(h as u32).to_le_bytes());
        self.map[16..24].copy_from_slice(&self.n.to_le_bytes());
        self.map[24..32].copy_from_slice(&now.to_le_bytes());
        self.map[HDR..HDR + w * h * 4].copy_from_slice(&rgba[..w * h * 4]);
        self.seq().store((s.wrapping_add(1) | 1).wrapping_add(1), Ordering::Release);
    }
}

/// Leitor (usado pelos testes; a UI faz o mesmo em C).
pub struct FrameSource { map: memmap2::Mmap }
impl FrameSource {
    pub fn open(path: &Path) -> Result<Self> {
        let f = std::fs::File::open(path)?;
        Ok(FrameSource { map: unsafe { memmap2::Mmap::map(&f)? } })
    }
    /// (largura, altura, nº do quadro, RGBA) do último quadro estável.
    pub fn read(&self) -> Option<(usize, usize, u64, Vec<u8>)> {
        let seq = unsafe { &*(self.map.as_ptr().add(4) as *const AtomicU32) };
        for _ in 0..8 {
            let a = seq.load(Ordering::Acquire);
            if a & 1 == 1 { std::thread::yield_now(); continue; }
            let w = u32::from_le_bytes(self.map[8..12].try_into().ok()?) as usize;
            let h = u32::from_le_bytes(self.map[12..16].try_into().ok()?) as usize;
            let n = u64::from_le_bytes(self.map[16..24].try_into().ok()?);
            if w == 0 || h == 0 || w > MAX_W || h > MAX_H { return None; }
            let data = self.map[HDR..HDR + w * h * 4].to_vec();
            if seq.load(Ordering::Acquire) == a { return Some((w, h, n, data)); }
        }
        None
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Conversão de cor + escala
// ---------------------------------------------------------------------------------------------------------------------
pub struct Yuv { pub w: usize, pub h: usize, pub y: Vec<u8>, pub u: Vec<u8>, pub v: Vec<u8> }
impl YUVSource for Yuv {
    fn dimensions(&self) -> (usize, usize) { (self.w, self.h) }
    fn strides(&self) -> (usize, usize, usize) { (self.w, self.w / 2, self.w / 2) }
    fn y(&self) -> &[u8] { &self.y }
    fn u(&self) -> &[u8] { &self.u }
    fn v(&self) -> &[u8] { &self.v }
}

/// Maior tamanho par com a proporção da origem que cabe em `max_h` de altura (e MAX_W de largura); nunca amplia.
pub fn fit_size(sw: usize, sh: usize, max_h: usize) -> (usize, usize) {
    if sw == 0 || sh == 0 { return (2, 2); }
    let th = sh.min(max_h.max(2));
    let mut tw = (sw as f64 * th as f64 / sh as f64).round() as usize;
    let mut th = th;
    if tw > MAX_W { th = (th as f64 * MAX_W as f64 / tw as f64) as usize; tw = MAX_W; }
    ((tw & !1).max(2), (th & !1).max(2))
}

#[inline]
fn yuv_px(b: i32, g: i32, r: i32) -> (u8, u8, u8) {
    // BT.601, faixa limitada (o que o OpenH264/Chrome esperam por padrão)
    let y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
    let u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
    let v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
    (y.clamp(0, 255) as u8, u.clamp(0, 255) as u8, v.clamp(0, 255) as u8)
}

/// BGRA (sw x sh, sem preenchimento de linha) → I420 dw x dh (pares). Se as dimensões diferem, faz amostragem 2x2 (suaviza o serrilhado
/// de reduções moderadas, importante para texto). Usa várias threads por faixas de linhas.
pub fn bgra_to_i420(src: &[u8], sw: usize, sh: usize, dw: usize, dh: usize) -> Yuv {
    let mut y = vec![0u8; dw * dh];
    let mut u = vec![0u8; dw * dh / 4];
    let mut v = vec![0u8; dw * dh / 4];
    let threads = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(2).clamp(1, 8);
    let pairs = dh / 2;
    let per = pairs.div_ceil(threads).max(1);
    let same = sw == dw && sh == dh;
    let sx_step = sw as f32 / dw as f32;
    let sy_step = sh as f32 / dh as f32;
    let xs: Vec<(usize, usize)> = if same { vec![] } else { (0..dw).map(|x| {
        let a = ((x as f32 + 0.25) * sx_step) as usize; let b = ((x as f32 + 0.75) * sx_step) as usize; (a.min(sw - 1), b.min(sw - 1)) }).collect() };
    std::thread::scope(|sc| {
        let yc = y.chunks_mut(2 * dw * per);
        let uc = u.chunks_mut(dw / 2 * per);
        let vc = v.chunks_mut(dw / 2 * per);
        for (ci, ((yy, uu), vv)) in yc.zip(uc).zip(vc).enumerate() {
            let xs = &xs;
            sc.spawn(move || {
                let first = ci * per;
                let rows = uu.len() / (dw / 2);
                for pr in 0..rows {
                    let py = first + pr; // par de linhas de destino
                    let mut ur = [0i32; 4]; let _ = &mut ur;
                    for x in 0..dw / 2 {
                        let (mut su, mut sv, mut n) = (0i32, 0i32, 0i32);
                        for dy in 0..2 {
                            let dyr = py * 2 + dy;
                            for dx in 0..2 {
                                let dxc = x * 2 + dx;
                                let (b, g, r) = if same {
                                    let o = (dyr * sw + dxc) * 4; (src[o] as i32, src[o + 1] as i32, src[o + 2] as i32)
                                } else {
                                    let ya = (((dyr as f32 + 0.25) * sy_step) as usize).min(sh - 1);
                                    let yb = (((dyr as f32 + 0.75) * sy_step) as usize).min(sh - 1);
                                    let (xa, xb) = xs[dxc];
                                    let mut acc = [0i32; 3];
                                    for (yy2, xx2) in [(ya, xa), (ya, xb), (yb, xa), (yb, xb)] {
                                        let o = (yy2 * sw + xx2) * 4; acc[0] += src[o] as i32; acc[1] += src[o + 1] as i32; acc[2] += src[o + 2] as i32;
                                    }
                                    (acc[0] >> 2, acc[1] >> 2, acc[2] >> 2)
                                };
                                let (yv, uv, vv2) = yuv_px(b, g, r);
                                yy[(pr * 2 + dy) * dw + dxc] = yv;
                                su += uv as i32; sv += vv2 as i32; n += 1;
                            }
                        }
                        uu[pr * (dw / 2) + x] = (su / n) as u8;
                        vv[pr * (dw / 2) + x] = (sv / n) as u8;
                    }
                }
            });
        }
    });
    Yuv { w: dw, h: dh, y, u, v }
}

// ---------------------------------------------------------------------------------------------------------------------
// Codificador / decodificador
// ---------------------------------------------------------------------------------------------------------------------
pub struct H264Enc {
    enc: Encoder,
    pub w: usize,
    pub h: usize,
    pub fps: f32,
    pub bps: u32,
}

impl H264Enc {
    pub fn new(w: usize, h: usize, fps: f32, bps: u32) -> Result<Self> {
        let cfg = EncoderConfig::new()
            .set_bitrate_bps(bps)
            .max_frame_rate(fps)
            .rate_control_mode(RateControlMode::Bitrate)
            .usage_type(UsageType::ScreenContentRealTime)
            .enable_skip_frame(true);
        let enc = Encoder::with_api_config(OpenH264API::from_source(), cfg).map_err(|e| anyhow!("h264: {e}"))?;
        Ok(H264Enc { enc, w, h, fps, bps })
    }
    pub fn force_idr(&mut self) { self.enc.force_intra_frame(); }
    /// Codifica um quadro I420; devolve a unidade de acesso em Annex-B.
    pub fn encode(&mut self, yuv: &Yuv) -> Result<Vec<u8>> {
        let bs = self.enc.encode(yuv).map_err(|e| anyhow!("h264: {e}"))?;
        Ok(bs.to_vec())
    }
}

pub struct H264Dec { dec: Decoder, buf: Vec<u8> }
impl H264Dec {
    pub fn new() -> Result<Self> { Ok(H264Dec { dec: Decoder::new().map_err(|e| anyhow!("h264: {e}"))?, buf: vec![] }) }
    /// Decodifica uma unidade de acesso; devolve (largura, altura, RGBA) se saiu um quadro.
    pub fn decode(&mut self, au: &[u8]) -> Result<Option<(usize, usize, &[u8])>> {
        let d = self.dec.decode(au).map_err(|e| anyhow!("h264: {e}"))?;
        let Some(d) = d else { return Ok(None) };
        let (w, h) = d.dimensions();
        self.buf.resize(w * h * 4, 255);
        let (sy, su, sv) = d.strides();
        i420_to_rgba(d.y(), d.u(), d.v(), (sy, su, sv), w, h, &mut self.buf);
        Ok(Some((w, h, &self.buf)))
    }
}

/// I420 (BT.601, faixa limitada, como o resto da rede espera) → RGBA, em várias threads.
pub fn i420_to_rgba(y: &[u8], u: &[u8], v: &[u8], strides: (usize, usize, usize), w: usize, h: usize, out: &mut [u8]) {
    let threads = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(2).clamp(1, 8);
    let per = h.div_ceil(threads).max(2) & !1;
    std::thread::scope(|sc| {
        for (ci, chunk) in out[..w * h * 4].chunks_mut(per * w * 4).enumerate() {
            sc.spawn(move || {
                for (r, row) in chunk.chunks_mut(w * 4).enumerate() {
                    let yy = ci * per + r;
                    for x in 0..w {
                        let c = (y[yy * strides.0 + x] as i32 - 16).max(0) * 298;
                        let d = u[(yy / 2) * strides.1 + x / 2] as i32 - 128;
                        let e = v[(yy / 2) * strides.2 + x / 2] as i32 - 128;
                        let o = x * 4;
                        row[o] = ((c + 409 * e + 128) >> 8).clamp(0, 255) as u8;
                        row[o + 1] = ((c - 100 * d - 208 * e + 128) >> 8).clamp(0, 255) as u8;
                        row[o + 2] = ((c + 516 * d + 128) >> 8).clamp(0, 255) as u8;
                        row[o + 3] = 255;
                    }
                }
            });
        }
    });
}

/// Alvo de bitrate "automático" (bps) por resolução (altura) e FPS.
pub fn auto_bitrate(h: usize, fps: u32) -> u32 {
    let base = match h { 0..=480 => 1_200_000.0, 481..=720 => 2_500_000.0, 721..=1080 => 4_500_000.0, 1081..=1440 => 9_000_000.0, _ => 18_000_000.0 };
    let f = (fps.max(1) as f64 / 30.0).powf(0.75);
    (base * f) as u32
}

pub fn psnr(a: &[u8], b: &[u8]) -> f64 {
    let n = a.len().min(b.len());
    let mut se = 0f64; let mut c = 0usize;
    for i in 0..n { if i % 4 == 3 { continue; } let d = a[i] as f64 - b[i] as f64; se += d * d; c += 1; }
    if se == 0.0 { return 99.0 }
    10.0 * (255.0 * 255.0 / (se / c as f64)).log10()
}

#[cfg(test)]
mod tests {
    use super::*;

    pub fn pattern(w: usize, h: usize, t: usize) -> Vec<u8> {
        // BGRA: degradê + quadrados coloridos + faixa móvel
        let mut f = vec![255u8; w * h * 4];
        for y in 0..h { for x in 0..w {
            let o = (y * w + x) * 4;
            f[o] = (x * 255 / w) as u8; f[o + 1] = (y * 255 / h) as u8; f[o + 2] = 128;
            if (x / 64 + y / 64) % 2 == 0 { f[o + 2] = 220; }
            if x >= (t * 8) % w && x < (t * 8) % w + 40 { f[o] = 255; f[o + 1] = 255; f[o + 2] = 0; }
        } }
        f
    }

    #[test]
    fn fit_size_keeps_aspect_and_never_upscales() {
        assert_eq!(fit_size(1920, 1080, 720), (1280, 720));
        assert_eq!(fit_size(1280, 720, 1080), (1280, 720));
        assert_eq!(fit_size(2560, 1600, 1080), (1728, 1080));
        assert_eq!(fit_size(3440, 1440, 1080), (2580, 1080));
    }

    #[test]
    fn h264_roundtrip_has_high_psnr_and_sink_roundtrips() {
        let (w, h) = (640usize, 360usize);
        let mut enc = H264Enc::new(w, h, 30.0, 4_000_000).unwrap();
        let mut dec = H264Dec::new().unwrap();
        let dir = std::env::temp_dir().join(format!("briks-vid-{}", std::process::id()));
        let mut sink = FrameSink::create(&dir.join("t.bin")).unwrap();
        let src = FrameSource::open(&dir.join("t.bin")).unwrap();
        let mut best = 0.0;
        for t in 0..10 {
            let f = pattern(w, h, t);
            let yuv = bgra_to_i420(&f, w, h, w, h);
            let au = enc.encode(&yuv).unwrap();
            assert!(!au.is_empty());
            if let Some((dw, dh, rgba)) = dec.decode(&au).unwrap() {
                assert_eq!((dw, dh), (w, h));
                let mut expect = f.clone(); for p in expect.chunks_mut(4) { p.swap(0, 2); }
                let p = psnr(&expect, rgba); best = f64::max(best, p);
                sink.write(dw, dh, rgba);
                let (rw, rh, n, d) = src.read().unwrap();
                assert_eq!((rw, rh), (w, h)); assert!(n >= 1); assert_eq!(d, rgba);
            }
        }
        assert!(best > 25.0, "PSNR {best}");
    }

    #[test]
    fn flat_colors_survive_the_codec_within_a_few_levels() {
        let (w, h) = (320usize, 180usize);
        let mut enc = H264Enc::new(w, h, 30.0, 2_000_000).unwrap();
        let mut dec = H264Dec::new().unwrap();
        for (b, g, r) in [(40u8, 160u8, 220u8), (200, 30, 90)] {
            let mut f = vec![255u8; w * h * 4];
            for p in f.chunks_mut(4) { p[0] = b; p[1] = g; p[2] = r; }
            let mut got = None;
            for _ in 0..3 { enc.force_idr(); let au = enc.encode(&bgra_to_i420(&f, w, h, w, h)).unwrap(); if let Some((_, _, d)) = dec.decode(&au).unwrap() { got = Some(d.to_vec()); } }
            let d = got.expect("quadro");
            let o = (90 * w + 160) * 4;
            for (a, e) in [(d[o], r), (d[o + 1], g), (d[o + 2], b)] { assert!((a as i32 - e as i32).abs() <= 8, "{a} vs {e}"); }
        }
    }

    #[test]
    fn downscale_produces_requested_size() {
        let f = pattern(1920, 1080, 0);
        let y = bgra_to_i420(&f, 1920, 1080, 1280, 720);
        assert_eq!((y.y.len(), y.u.len()), (1280 * 720, 640 * 360));
    }
}
