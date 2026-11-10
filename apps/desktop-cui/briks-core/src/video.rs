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
        // sem truncar: a UI pode estar com o arquivo mapeado (no Windows isso faria a abertura falhar)
        let f = std::fs::OpenOptions::new().read(true).write(true).create(true).truncate(false).open(path)?;
        let want = (HDR + MAX_W * MAX_H * 4) as u64;
        if f.metadata()?.len() != want { f.set_len(want)?; }
        let mut map = unsafe { MmapMut::map_mut(&f)? };
        map[0..4].copy_from_slice(b"BKFR");
        map[8..24].fill(0);   // largura/altura/nº zerados: quadro antigo de outra sessão não aparece
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
    pub fn ts(&self) -> u64 { u64::from_le_bytes(self.map[24..32].try_into().unwrap_or([0; 8])) }
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

/// Nº de threads para conversão de cor: poucas, para não disputar a CPU com o jogo/app transmitido.
fn conv_threads(pixels: usize) -> usize {
    let n = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(2);
    if pixels <= 1280 * 720 { 1 } else { (n / 2).clamp(1, 4) }
}

/// Converte um par de linhas BGRA (mesma largura `w`, par) para Y (2 linhas) e U/V (1 linha). BT.601, faixa limitada.
#[inline]
fn rows_to_yuv(r0: &[u8], r1: &[u8], y0: &mut [u8], y1: &mut [u8], u: &mut [u8], v: &mut [u8]) {
    for (((((a, b), ya), yb), uo), vo) in r0.chunks_exact(8).zip(r1.chunks_exact(8)).zip(y0.chunks_exact_mut(2)).zip(y1.chunks_exact_mut(2)).zip(u.iter_mut()).zip(v.iter_mut()) {
        let (b0, g0, c0) = (a[0] as i32, a[1] as i32, a[2] as i32);
        let (b1, g1, c1) = (a[4] as i32, a[5] as i32, a[6] as i32);
        let (b2, g2, c2) = (b[0] as i32, b[1] as i32, b[2] as i32);
        let (b3, g3, c3) = (b[4] as i32, b[5] as i32, b[6] as i32);
        ya[0] = (((66 * c0 + 129 * g0 + 25 * b0 + 128) >> 8) + 16) as u8;
        ya[1] = (((66 * c1 + 129 * g1 + 25 * b1 + 128) >> 8) + 16) as u8;
        yb[0] = (((66 * c2 + 129 * g2 + 25 * b2 + 128) >> 8) + 16) as u8;
        yb[1] = (((66 * c3 + 129 * g3 + 25 * b3 + 128) >> 8) + 16) as u8;
        let (bs, gs, rs) = (b0 + b1 + b2 + b3, g0 + g1 + g2 + g3, c0 + c1 + c2 + c3);   // soma do bloco 2x2 (x4)
        *uo = (((-38 * rs - 74 * gs + 112 * bs + 512) >> 10) + 128) as u8;
        *vo = (((112 * rs - 94 * gs - 18 * bs + 512) >> 10) + 128) as u8;
    }
}

/// BGRA (sw x sh, sem preenchimento de linha) → I420 dw x dh (pares). Se as dimensões diferem, cada pixel de destino é a média de
/// 4 amostras da origem (suaviza o serrilhado de reduções moderadas, importante para texto). Usa poucas threads, por faixas de linhas.
pub fn bgra_to_i420(src: &[u8], sw: usize, sh: usize, dw: usize, dh: usize) -> Yuv { bgra_to_i420_stride(src, sw * 4, sw, sh, dw, dh) }

/// Igual a `bgra_to_i420`, com passo de linha `stride` (bytes) na origem — lê direto de uma textura mapeada.
pub fn bgra_to_i420_stride(src: &[u8], stride: usize, sw: usize, sh: usize, dw: usize, dh: usize) -> Yuv {
    let mut y = vec![0u8; dw * dh];
    let mut u = vec![0u8; dw * dh / 4];
    let mut v = vec![0u8; dw * dh / 4];
    let threads = conv_threads(dw * dh);
    let pairs = dh / 2;
    let per = pairs.div_ceil(threads).max(1);
    let same = sw == dw && sh == dh;
    let (sx_step, sy_step) = (sw as f32 / dw as f32, sh as f32 / dh as f32);
    // deslocamentos (em bytes) das duas colunas de origem de cada coluna de destino
    let xs: Vec<(u32, u32)> = if same { vec![] } else { (0..dw).map(|x| {
        let a = ((x as f32 + 0.25) * sx_step) as usize; let b = ((x as f32 + 0.75) * sx_step) as usize; ((a.min(sw - 1) * 4) as u32, (b.min(sw - 1) * 4) as u32) }).collect() };
    std::thread::scope(|sc| {
        let yc = y.chunks_mut(2 * dw * per);
        let uc = u.chunks_mut(dw / 2 * per);
        let vc = v.chunks_mut(dw / 2 * per);
        for (ci, ((yy, uu), vv)) in yc.zip(uc).zip(vc).enumerate() {
            let xs = &xs;
            let mut work = move || {
                let first = ci * per;
                let rows = uu.len() / (dw / 2);
                let mut tmp = if same { vec![] } else { vec![0u8; dw * 4 * 2] };
                for pr in 0..rows {
                    let py = (first + pr) * 2;
                    let (ya, yb) = yy[pr * 2 * dw..(pr * 2 + 2) * dw].split_at_mut(dw);
                    let (uo, vo) = (&mut uu[pr * (dw / 2)..(pr + 1) * (dw / 2)], &mut vv[pr * (dw / 2)..(pr + 1) * (dw / 2)]);
                    if same {
                        rows_to_yuv(&src[py * stride..py * stride + sw * 4], &src[(py + 1) * stride..(py + 1) * stride + sw * 4], ya, yb, uo, vo);
                    } else {
                        // reduz as duas linhas para `tmp` (BGRA) e converte
                        for k in 0..2 {
                            let dyr = py + k;
                            let r0 = (((dyr as f32 + 0.25) * sy_step) as usize).min(sh - 1) * stride;
                            let r1 = (((dyr as f32 + 0.75) * sy_step) as usize).min(sh - 1) * stride;
                            let (ra, rb) = (&src[r0..r0 + sw * 4], &src[r1..r1 + sw * 4]);
                            let out = &mut tmp[k * dw * 4..(k + 1) * dw * 4];
                            for (o, &(xa, xb)) in out.chunks_exact_mut(4).zip(xs.iter()) {
                                let (xa, xb) = (xa as usize, xb as usize);
                                o[0] = ((ra[xa] as u32 + ra[xb] as u32 + rb[xa] as u32 + rb[xb] as u32) >> 2) as u8;
                                o[1] = ((ra[xa + 1] as u32 + ra[xb + 1] as u32 + rb[xa + 1] as u32 + rb[xb + 1] as u32) >> 2) as u8;
                                o[2] = ((ra[xa + 2] as u32 + ra[xb + 2] as u32 + rb[xa + 2] as u32 + rb[xb + 2] as u32) >> 2) as u8;
                            }
                        }
                        let (t0, t1) = tmp.split_at(dw * 4);
                        rows_to_yuv(t0, t1, ya, yb, uo, vo);
                    }
                }
            };
            if threads == 1 { work(); } else { sc.spawn(work); }
        }
    });
    Yuv { w: dw, h: dh, y, u, v }
}

// ---------------------------------------------------------------------------------------------------------------------
// Codificador / decodificador
// ---------------------------------------------------------------------------------------------------------------------
enum Backend { Mf(crate::mfenc::MfEnc), Open(Encoder) }

/// Codificador H264: Media Foundation (GPU quando há; senão o de software da Microsoft) e, se nada disso abrir, OpenH264.
/// `BRIKS_ENCODER` = `openh264` | `mf-soft` força um deles (testes/diagnóstico).
pub struct H264Enc {
    be: Backend,
    pub w: usize,
    pub h: usize,
    pub bps: u32,
    pub name: String,
    pub hardware: bool,
    idr: bool,
    nv12: Vec<u8>,
}

impl H264Enc {
    pub fn new(w: usize, h: usize, fps: f32, bps: u32) -> Result<Self> {
        let pref = std::env::var("BRIKS_ENCODER").unwrap_or_default();
        if pref != "openh264" {
            match crate::mfenc::MfEnc::new(w, h, fps.round() as u32, bps, pref != "mf-soft") {
                Ok(m) => { let (name, hardware) = (m.name.clone(), m.hardware); return Ok(H264Enc { be: Backend::Mf(m), w, h, bps, name, hardware, idr: true, nv12: vec![] }); }
                Err(e) => eprintln!("codificador Media Foundation indisponível ({e}); usando OpenH264"),
            }
        }
        let cfg = EncoderConfig::new()
            .set_bitrate_bps(bps)
            .max_frame_rate(fps)
            .rate_control_mode(RateControlMode::Bitrate)
            .usage_type(UsageType::CameraVideoRealTime)
            .enable_skip_frame(true);
        let enc = Encoder::with_api_config(OpenH264API::from_source(), cfg).map_err(|e| anyhow!("h264: {e}"))?;
        Ok(H264Enc { be: Backend::Open(enc), w, h, bps, name: "OpenH264 (software)".into(), hardware: false, idr: true, nv12: vec![] })
    }
    pub fn force_idr(&mut self) { self.idr = true; }
    /// Codifica um quadro I420; devolve as unidades de acesso prontas em Annex-B (nenhuma = o codificador não soltou quadro desta vez).
    pub fn encode(&mut self, yuv: &Yuv) -> Result<Vec<Vec<u8>>> {
        let idr = std::mem::take(&mut self.idr);
        match &mut self.be {
            Backend::Mf(m) => { crate::mfenc::i420_to_nv12(&yuv.y, &yuv.u, &yuv.v, &mut self.nv12); m.encode(&self.nv12, idr) }
            Backend::Open(e) => { if idr { e.force_intra_frame(); } let bs = e.encode(yuv).map_err(|e| anyhow!("h264: {e}"))?; let v = bs.to_vec(); Ok(if v.is_empty() { vec![] } else { vec![v] }) }
        }
    }
}

enum DecBackend { Mf(crate::mfenc::MfDec), Open(Decoder) }

/// Decodificador H264: Media Foundation (aceita o fluxo de qualquer codificador) e, se não abrir, OpenH264.
pub struct H264Dec { be: DecBackend, buf: Vec<u8>, pub name: &'static str }
impl H264Dec {
    pub fn new() -> Result<Self> {
        if std::env::var("BRIKS_DECODER").as_deref() != Ok("openh264") {
            match crate::mfenc::MfDec::new() { Ok(m) => return Ok(H264Dec { be: DecBackend::Mf(m), buf: vec![], name: "Media Foundation" }), Err(e) => eprintln!("decodificador Media Foundation indisponível ({e}); usando OpenH264") }
        }
        Ok(H264Dec { be: DecBackend::Open(Decoder::new().map_err(|e| anyhow!("h264: {e}"))?), buf: vec![], name: "OpenH264" })
    }
    /// Decodifica uma unidade de acesso; devolve (largura, altura, RGBA) se saiu um quadro.
    pub fn decode(&mut self, au: &[u8]) -> Result<Option<(usize, usize, &[u8])>> {
        match &mut self.be {
            DecBackend::Mf(m) => {
                let buf = &mut self.buf;
                let mut dims = None;
                m.decode(au, |w, h, y, uv, stride| { buf.resize(w * h * 4, 255); nv12_to_rgba(y, uv, stride, w, h, buf); dims = Some((w, h)); })?;
                Ok(dims.map(|(w, h)| (w, h, &self.buf[..w * h * 4])))
            }
            DecBackend::Open(dec) => {
                let d = dec.decode(au).map_err(|e| anyhow!("h264: {e}"))?;
                let Some(d) = d else { return Ok(None) };
                let (w, h) = d.dimensions();
                self.buf.resize(w * h * 4, 255);
                let (sy, su, sv) = d.strides();
                i420_to_rgba(d.y(), d.u(), d.v(), (sy, su, sv), w, h, &mut self.buf);
                Ok(Some((w, h, &self.buf[..w * h * 4])))
            }
        }
    }
}

/// NV12 (BT.601, faixa limitada) → RGBA, em poucas threads.
pub fn nv12_to_rgba(y: &[u8], uv: &[u8], stride: usize, w: usize, h: usize, out: &mut [u8]) {
    let threads = conv_threads(w * h);
    let per = h.div_ceil(threads).max(2) & !1;
    std::thread::scope(|sc| {
        for (ci, chunk) in out[..w * h * 4].chunks_mut(per * w * 4).enumerate() {
            let mut work = move || {
                for (r, row) in chunk.chunks_mut(w * 4).enumerate() {
                    let yy = ci * per + r;
                    let (yr, uvr) = (&y[yy * stride..yy * stride + w], &uv[(yy / 2) * stride..]);
                    for ((px, ys), c) in row.chunks_exact_mut(8).zip(yr.chunks_exact(2)).zip(uvr.chunks_exact(2)) {
                        let (d, e) = (c[0] as i32 - 128, c[1] as i32 - 128);
                        let (rr, gg, bb) = (409 * e + 128, -100 * d - 208 * e + 128, 516 * d + 128);
                        let c0 = (ys[0] as i32 - 16) * 298;
                        let c1 = (ys[1] as i32 - 16) * 298;
                        px[0] = ((c0 + rr) >> 8).clamp(0, 255) as u8; px[1] = ((c0 + gg) >> 8).clamp(0, 255) as u8; px[2] = ((c0 + bb) >> 8).clamp(0, 255) as u8; px[3] = 255;
                        px[4] = ((c1 + rr) >> 8).clamp(0, 255) as u8; px[5] = ((c1 + gg) >> 8).clamp(0, 255) as u8; px[6] = ((c1 + bb) >> 8).clamp(0, 255) as u8; px[7] = 255;
                    }
                }
            };
            if threads == 1 { work(); } else { sc.spawn(work); }
        }
    });
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

    pub(crate) fn pattern(w: usize, h: usize, t: usize) -> Vec<u8> {
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
            for au in enc.encode(&yuv).unwrap() {
            if let Some((dw, dh, rgba)) = dec.decode(&au).unwrap() {
                assert_eq!((dw, dh), (w, h));
                let mut expect = f.clone(); for p in expect.chunks_mut(4) { p.swap(0, 2); }
                let p = psnr(&expect, rgba); best = f64::max(best, p);
                sink.write(dw, dh, rgba);
                let (rw, rh, n, d) = src.read().unwrap();
                assert_eq!((rw, rh), (w, h)); assert!(n >= 1); assert_eq!(d, rgba);
            } }
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
            for _ in 0..3 { enc.force_idr(); for au in enc.encode(&bgra_to_i420(&f, w, h, w, h)).unwrap() { if let Some((_, _, d)) = dec.decode(&au).unwrap() { got = Some(d.to_vec()); } } }
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

#[cfg(test)]
mod bench {
    use super::*;
    #[test]
    #[ignore]
    fn bench_convert_and_encode() {
        for (sw, sh, dw, dh) in [(1920usize, 1080usize, 1920usize, 1080usize), (3440, 1440, 2580, 1080), (1920, 1080, 1280, 720)] {
            let f = super::tests::pattern(sw, sh, 3);
            let c0 = crate::share::cpu_ms();
            let t = std::time::Instant::now();
            let mut y = bgra_to_i420(&f, sw, sh, dw, dh);
            for _ in 0..9 { y = bgra_to_i420(&f, sw, sh, dw, dh); }
            let conv = t.elapsed().as_secs_f64() * 100.0;
            let ccpu = (crate::share::cpu_ms() - c0) / 10.0;
            let mut enc = H264Enc::new(dw, dh, 30.0, 6_000_000).unwrap();
            let mut dec = H264Dec::new().unwrap();
            let mut aus = vec![]; let mut et = 0.0f64;
            let t = std::time::Instant::now();
            for i in 0..60 { let f2 = super::tests::pattern(sw, sh, i * 3); let y2 = bgra_to_i420(&f2, sw, sh, dw, dh); if i == 10 || i == 30 { enc.force_idr(); } let t1 = std::time::Instant::now(); let o = enc.encode(&y2).unwrap(); if o.len() != 1 { eprintln!("quadro {i}: {} saídas", o.len()); } aus.extend(o); et += t1.elapsed().as_secs_f64(); }
            let _ = t; let e = et * 50.0;
            let t = std::time::Instant::now();
            let mut okd = 0; for a in &aus { if a.is_empty() { continue; } match dec.decode(a) { Ok(Some(_)) => okd += 1, Ok(None) => {}, Err(e) => eprintln!("erro em au de {} bytes: {e}", a.len()) } } eprintln!("decodificou {okd} de {}", aus.len());
            let d = t.elapsed().as_secs_f64() * 50.0;
            eprintln!("[{}] {sw}x{sh}->{dw}x{dh}: conversão {conv:.1} ms (CPU {ccpu:.1} ms), codificação (quadro parado) {e:.1} ms, decodificação+RGBA {d:.1} ms", enc.name);
        }
    }
}
