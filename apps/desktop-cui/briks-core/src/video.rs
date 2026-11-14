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
enum Backend { Mf(crate::mfenc::MfEnc), Open(Encoder), #[cfg(feature = "vpx")] Vpx(crate::vpx::VpxEnc) }

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
    /// só testes: este codificador obedece as falhas injetadas (FAULT_*); os outros testes que rodam em paralelo não são afetados
    pub inject: bool,
}

/// Só testes: injeção de falhas do codificador. `FAULT_NEXT` = quantas das próximas chamadas a `encode` falham (qualquer codificador);
/// `FAULT_HW` = `encode` de codificador de HARDWARE falha sempre (o de software passa: prova o fallback); `FAULT_HR` = HRESULT devolvido
/// (0 = "não pediu entrada"). Zerados fora dos testes: custo de um load atômico por quadro.
pub static FAULT_NEXT: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
pub static FAULT_HW: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
pub static FAULT_HR: std::sync::atomic::AtomicI32 = std::sync::atomic::AtomicI32::new(0);
fn injected_fault() -> crate::mfenc::EncFault {
    use crate::mfenc::{EncFault, FaultKind};
    match FAULT_HR.load(std::sync::atomic::Ordering::Relaxed) { 0 => EncFault::new(FaultKind::Stalled, 0xC00D36B5u32 as i32, "o codificador não pediu entrada (injetado)"), hr => EncFault::from_hr(hr, "falha injetada") }
}

impl H264Enc {
    pub fn new(w: usize, h: usize, fps: f32, bps: u32) -> Result<Self> { Self::new_with(w, h, fps, bps, true) }
    /// Codificador do codec pedido: H264 como sempre; VP8/VP9 pela libvpx (feature `vpx`). `allow_hw` só vale para H264.
    pub fn new_codec(codec: crate::codec::Codec, w: usize, h: usize, fps: f32, bps: u32, allow_hw: bool) -> Result<Self> { Self::new_codec_qp(codec, w, h, fps, bps, allow_hw, crate::mfenc::DEFAULT_MAX_QP) }
    /// Igual, com o teto de QP do codificador H264 do Media Foundation (os outros não têm teto).
    pub fn new_codec_qp(codec: crate::codec::Codec, w: usize, h: usize, fps: f32, bps: u32, allow_hw: bool, max_qp: u32) -> Result<Self> {
        match codec {
            crate::codec::Codec::H264 => Self::new_with_qp(w, h, fps, bps, allow_hw, max_qp),
            #[cfg(feature = "vpx")]
            c => { let e = crate::vpx::VpxEnc::new(c, w, h, fps, bps)?; Ok(H264Enc { be: Backend::Vpx(e), w, h, bps, name: format!("libvpx {} (software)", c.name().to_uppercase()), hardware: false, idr: true, nv12: vec![], inject: false }) }
            #[cfg(not(feature = "vpx"))]
            c => Err(anyhow!("{} indisponível nesta build", c.name())),
        }
    }
    /// `allow_hw = false`: pula o MFT de hardware (fallback depois de falhas repetidas do NVENC/GPU) e usa o de software da Microsoft ou o OpenH264.
    pub fn new_with(w: usize, h: usize, fps: f32, bps: u32, allow_hw: bool) -> Result<Self> { Self::new_with_qp(w, h, fps, bps, allow_hw, crate::mfenc::DEFAULT_MAX_QP) }
    pub fn new_with_qp(w: usize, h: usize, fps: f32, bps: u32, allow_hw: bool, max_qp: u32) -> Result<Self> {
        let pref = std::env::var("BRIKS_ENCODER").unwrap_or_default();
        if pref != "openh264" {
            match crate::mfenc::MfEnc::new_qp(w, h, fps.round() as u32, bps, allow_hw && pref != "mf-soft", max_qp) {
                Ok(m) => { let (name, hardware) = (m.name.clone(), m.hardware); return Ok(H264Enc { be: Backend::Mf(m), w, h, bps, name, hardware, idr: true, nv12: vec![], inject: false }); }
                Err(e) => eprintln!("codificador Media Foundation indisponível ({e}); usando OpenH264"),
            }
        }
        let cfg = EncoderConfig::new()
            .set_bitrate_bps(bps)
            .max_frame_rate(fps)
            .rate_control_mode(RateControlMode::Bitrate)
            .usage_type(if std::env::var("BRIKS_OH264_USAGE").as_deref() == Ok("screen") { UsageType::ScreenContentRealTime } else { UsageType::CameraVideoRealTime })   // medido: "screen" é estável nos saltos mas fica ~7 dB abaixo na rolagem lenta
            .enable_skip_frame(true);
        let enc = Encoder::with_api_config(OpenH264API::from_source(), cfg).map_err(|e| anyhow!("h264: {e}"))?;
        Ok(H264Enc { be: Backend::Open(enc), w, h, bps, name: "OpenH264 (software)".into(), hardware: false, idr: true, nv12: vec![], inject: false })
    }
    pub fn force_idr(&mut self) { self.idr = true; }
    /// Muda o bitrate alvo sem recriar o codificador. false = este codificador não aceita (recrie).
    pub fn set_bitrate(&mut self, bps: u32) -> bool {
        match &mut self.be { Backend::Mf(m) => { if m.set_bitrate(bps) { self.bps = bps; true } else { false } } Backend::Open(_) => false, #[cfg(feature = "vpx")] Backend::Vpx(e) => { if e.set_bitrate(bps) { self.bps = bps; true } else { false } } }
    }
    /// Informa o novo FPS (duração dos quadros para o controle de taxa). false = recrie.
    pub fn set_fps(&mut self, fps: u32) -> bool { match &mut self.be { Backend::Mf(m) => { m.set_fps(fps); true } Backend::Open(_) => false, #[cfg(feature = "vpx")] Backend::Vpx(e) => { e.set_fps(fps); true } } }
    /// Codifica um quadro I420; devolve as unidades de acesso prontas em Annex-B (nenhuma = o codificador não soltou quadro desta vez).
    pub fn encode(&mut self, yuv: &Yuv) -> Result<Vec<Vec<u8>>> {
        let idr = std::mem::take(&mut self.idr);
        if self.inject {
            use std::sync::atomic::Ordering::Relaxed;
            if FAULT_NEXT.load(Relaxed) > 0 && FAULT_NEXT.fetch_sub(1, Relaxed) > 0 { self.idr = true; return Err(anyhow::Error::new(injected_fault())); }
            if self.hardware && FAULT_HW.load(Relaxed) { self.idr = true; return Err(anyhow::Error::new(injected_fault())); }
        }
        match &mut self.be {
            Backend::Mf(m) => { crate::mfenc::i420_to_nv12(&yuv.y, &yuv.u, &yuv.v, &mut self.nv12); m.encode(&self.nv12, idr) }
            #[cfg(feature = "vpx")]
            Backend::Vpx(e) => { if idr { e.force_idr(); } Ok(e.encode(yuv)?.into_iter().map(|(d, _)| d).collect()) }
            Backend::Open(e) => { if idr { e.force_intra_frame(); } let bs = e.encode(yuv).map_err(|e| anyhow!("h264: {e}"))?; let v = bs.to_vec(); Ok(if v.is_empty() { vec![] } else { vec![v] }) }
        }
    }
}

enum DecBackend { Mf(crate::mfenc::MfDec), Open(Decoder), #[cfg(feature = "vpx")] Vpx(crate::vpx::VpxDec) }

/// Decodificador H264: Media Foundation (aceita o fluxo de qualquer codificador) e, se não abrir, OpenH264.
pub struct H264Dec { be: DecBackend, buf: Vec<u8>, pub name: &'static str }
impl H264Dec {
    /// Decodificador do codec do fluxo recebido (H264 como sempre; VP8/VP9 pela libvpx).
    pub fn new_codec(codec: crate::codec::Codec) -> Result<Self> {
        match codec {
            crate::codec::Codec::H264 => Self::new(),
            #[cfg(feature = "vpx")]
            c => Ok(H264Dec { be: DecBackend::Vpx(crate::vpx::VpxDec::new(c)?), buf: vec![], name: "libvpx" }),
            #[cfg(not(feature = "vpx"))]
            c => Err(anyhow!("{} indisponível nesta build", c.name())),
        }
    }
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
            #[cfg(feature = "vpx")]
            DecBackend::Vpx(d) => d.decode(au),
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

/// Alvo de bitrate "automático" (bps) por resolução (altura) e FPS. Âncoras: 360p 0,8 · 480p 1,2 · 720p 2,5 · 1080p 4,5 ·
/// 1440p 9 · 4K 18 Mbps a 30 FPS, interpoladas em escala logarítmica entre elas.
pub fn auto_bitrate(h: usize, fps: u32) -> u32 {
    // perfis do Electron (screen-quality.ts): 720p30 = 3,5 Mbps, 1080p30 = 6; a 60 FPS 720p = 5 e 1080p = 9
    const A: [(f64, f64); 6] = [(360.0, 1_000_000.0), (480.0, 1_800_000.0), (720.0, 3_500_000.0), (1080.0, 6_000_000.0), (1440.0, 10_000_000.0), (2160.0, 18_000_000.0)];
    let hh = (h as f64).clamp(A[0].0, A[5].0);
    let mut base = A[5].1;
    for w in A.windows(2) {
        if hh <= w[1].0 { let t = (hh / w[0].0).ln() / (w[1].0 / w[0].0).ln(); base = w[0].1 * (w[1].1 / w[0].1).powf(t); break; }
    }
    let f = (fps.max(1) as f64 / 30.0).powf(0.55);
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

    /// A janela transmitida muda de tamanho no meio: o decodificador tem de entregar o quadro novo inteiro, no tamanho novo.
    #[test]
    fn decoder_follows_a_mid_stream_resolution_change() {
        let mut dec = H264Dec::new().unwrap();
        for (w, h) in [(626usize, 392usize), (886, 512), (1280, 720), (626, 392)] {
            let mut enc = H264Enc::new(w, h, 30.0, 3_000_000).unwrap();
            let f = pattern(w, h, 2);
            let mut expect = f.clone(); for p in expect.chunks_mut(4) { p.swap(0, 2); }
            let mut last = None;
            for _ in 0..6 { for au in enc.encode(&bgra_to_i420(&f, w, h, w, h)).unwrap() { if let Some((dw, dh, rgba)) = dec.decode(&au).unwrap() { last = Some((dw, dh, rgba.to_vec())); } } }
            let (dw, dh, rgba) = last.expect("nenhum quadro depois da troca de tamanho");
            assert_eq!((dw, dh), (w, h), "tamanho errado depois da troca");
            let p = psnr(&expect, &rgba);
            eprintln!("{w}x{h}: PSNR {p:.1} dB ({})", dec.name);
            assert!(p > 22.0, "{w}x{h}: quadro errado depois da troca de tamanho (PSNR {p:.1})");
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
    /// Onde vai o tempo de quem assiste: decodificação pura (Media Foundation, sem converter) x NV12→RGBA x cópia para a memória
    /// compartilhada, a 1080p e 720p, com conteúdo em movimento. `BRIKS_DECODER=openh264` compara o decodificador de software.
    #[test]
    #[ignore]
    fn bench_decode_split() {
        for (w, h) in [(1920usize, 1080usize), (1280, 720)] {
            let mut enc = H264Enc::new(w, h, 30.0, 6_000_000).unwrap();
            let mut aus = vec![];
            for i in 0..90 { let f = super::tests::pattern(w, h, i * 3); aus.extend(enc.encode(&bgra_to_i420(&f, w, h, w, h)).unwrap()); }
            let n = aus.len() as f64;
            let mut raw = crate::mfenc::MfDec::new().unwrap();
            let (mut keep, mut frames) = (None, 0);
            let t = std::time::Instant::now();
            for a in &aus { let _ = raw.decode(a, |w, h, y, uv, stride| { frames += 1; if keep.is_none() { keep = Some((w, h, y.to_vec(), uv.to_vec(), stride)); } }); }
            let dec_ms = t.elapsed().as_secs_f64() * 1000.0 / n;
            let (kw, kh, y, uv, stride) = keep.unwrap();
            let mut out = vec![255u8; kw * kh * 4];
            let t = std::time::Instant::now();
            for _ in 0..60 { nv12_to_rgba(&y, &uv, stride, kw, kh, &mut out); }
            let conv_ms = t.elapsed().as_secs_f64() * 1000.0 / 60.0;
            let dir = std::env::temp_dir().join(format!("briks-bench-{}", std::process::id()));
            let mut sink = FrameSink::create(&dir.join("b.bin")).unwrap();
            let t = std::time::Instant::now();
            for _ in 0..60 { sink.write(kw, kh, &out); }
            let sink_ms = t.elapsed().as_secs_f64() * 1000.0 / 60.0;
            let mut full = H264Dec::new().unwrap();
            let t = std::time::Instant::now();
            for a in &aus { let _ = full.decode(a); }
            let full_ms = t.elapsed().as_secs_f64() * 1000.0 / n;
            eprintln!("[decodificar {w}x{h}] {frames} quadros de {n}: Media Foundation puro {dec_ms:.1} ms, NV12→RGBA {conv_ms:.1} ms, gravar no quadro compartilhado {sink_ms:.1} ms, H264Dec completo ({}) {full_ms:.1} ms", full.name);
        }
    }

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
            let _ = t; let e = et * 1000.0 / 60.0;   // ms por quadro (antes multiplicava por 50: mostrava 3x o tempo real)
            let t = std::time::Instant::now();
            let mut okd = 0; for a in &aus { if a.is_empty() { continue; } match dec.decode(a) { Ok(Some(_)) => okd += 1, Ok(None) => {}, Err(e) => eprintln!("erro em au de {} bytes: {e}", a.len()) } } eprintln!("decodificou {okd} de {}", aus.len());
            let d = t.elapsed().as_secs_f64() * 1000.0 / aus.len().max(1) as f64;
            eprintln!("[{}] {sw}x{sh}->{dw}x{dh}: conversão {conv:.1} ms (CPU {ccpu:.1} ms), codificação (quadro parado) {e:.1} ms, decodificação+RGBA {d:.1} ms", enc.name);
        }
    }
}

/// Medição de qualidade de tela (ignorado; rode com `--ignored --nocapture`): quadro 1080p com "texto de interface" e uma faixa que
/// anda (como a linha do tempo do Premiere); PSNR do que sai do decodificador contra a fonte, por bitrate. Variáveis de diagnóstico:
/// BRIKS_ENC_VBV_MS, BRIKS_ENC_PROFILE, BRIKS_ENCODER.
#[cfg(test)]
mod qualidade {
    use super::*;

    fn glyph(cell: u32) -> [u8; 7] { let mut x = cell.wrapping_mul(2654435761).wrapping_add(12345); let mut g = [0u8; 7]; for r in &mut g { x ^= x << 13; x ^= x >> 17; x ^= x << 5; *r = (x & 0x1f) as u8 | 0x04; } g }
    fn tela(w: usize, h: usize, t: usize) -> Vec<u8> {
        let mut f = vec![0u8; w * h * 4];
        for y in 0..h { for x in 0..w { let o = (y * w + x) * 4; let (b, g, r) = if y < 40 { (60, 40, 40) } else if x < 300 { (45, 40, 38) } else if y > h - 300 { (30, 28, 28) } else { (38, 36, 36) }; f[o] = b; f[o + 1] = g; f[o + 2] = r; f[o + 3] = 255; } }
        fn put(f: &mut [u8], w: usize, h: usize, x0: usize, y0: usize, cell: u32, col: (u8, u8, u8)) { let gl = glyph(cell); for (ry, bits) in gl.iter().enumerate() { for rx in 0..5 { if bits >> rx & 1 == 1 { for (dy, dx) in [(0, 0), (0, 1), (1, 0), (1, 1)] { let (px, py) = (x0 + rx * 2 + dx, y0 + ry * 2 + dy); if px < w && py < h { let o = (py * w + px) * 4; f[o] = col.0; f[o + 1] = col.1; f[o + 2] = col.2; } } } } } }
        // texto: linhas de "palavras" em todos os painéis (menus, propriedades, lista de arquivos)
        for ln in 0..70 { for c in 0..(w / 14) { if (c * 7 + ln * 3) % 11 < 8 { put(&mut f, w, h, c * 14 + 6, 56 + ln * 14, (ln * 977 + c * 31) as u32, (225, 225, 225)); } } }
        // a parte que anda: faixa de "clipes" que desliza 6 px por quadro e um relógio que muda
        let off = (t * 6) % 600;
        for k in 0..30 { let x0: usize = 320 + k * 60; let xx = x0.saturating_sub(off % 60); if xx + 40 < w { for y in (h - 280)..(h - 40) { for x in xx..xx + 40 { let o = (y * w + x) * 4; f[o] = 140; f[o + 1] = 90 + (k * 5) as u8; f[o + 2] = 60; } } } }
        for c in 0..8 { put(&mut f, w, h, w - 140 + c * 14, 10, (t * 13 + c * 7) as u32, (255, 255, 255)); }
        f
    }
    /// PSNR só da luminância (BT.601): é o que decide se o texto é legível; a croma 4:2:0 é igual em qualquer codificador H264.
    fn luma_psnr(bgra: &[u8], rgba: &[u8]) -> f64 {
        let mut se = 0f64; let n = bgra.len() / 4;
        for i in 0..n { let (b, g, r) = (bgra[i * 4] as f64, bgra[i * 4 + 1] as f64, bgra[i * 4 + 2] as f64); let (r2, g2, b2) = (rgba[i * 4] as f64, rgba[i * 4 + 1] as f64, rgba[i * 4 + 2] as f64);
            let d = (0.299 * r + 0.587 * g + 0.114 * b) - (0.299 * r2 + 0.587 * g2 + 0.114 * b2); se += d * d; }
        let mse = se / n as f64; if mse < 1e-9 { 99.0 } else { 10.0 * (255.0f64 * 255.0 / mse).log10() }
    }
    fn media(rgba: &[u8], f: &[u8]) -> f64 { let mut e = f.to_vec(); for p in e.chunks_mut(4) { p.swap(0, 2); } psnr(&e, rgba) }

    /// amplia RGBA (sw x sh) para (dw x dh) com filtro bilinear, como a GPU faz ao desenhar a imagem no palco
    fn amplia(src: &[u8], sw: usize, sh: usize, dw: usize, dh: usize) -> Vec<u8> {
        let mut o = vec![255u8; dw * dh * 4];
        for y in 0..dh { let fy = ((y as f32 + 0.5) * sh as f32 / dh as f32 - 0.5).clamp(0.0, sh as f32 - 1.001); let (y0, ty) = (fy as usize, fy.fract());
            for x in 0..dw { let fx = ((x as f32 + 0.5) * sw as f32 / dw as f32 - 0.5).clamp(0.0, sw as f32 - 1.001); let (x0, tx) = (fx as usize, fx.fract());
                for c in 0..3 { let g = |xx: usize, yy: usize| src[(yy * sw + xx) * 4 + c] as f32; let v = (g(x0, y0) * (1.0 - tx) + g(x0 + 1, y0) * tx) * (1.0 - ty) + (g(x0, y0 + 1) * (1.0 - tx) + g(x0 + 1, y0 + 1) * tx) * ty; o[(y * dw + x) * 4 + c] = v as u8; } } }
        o
    }

    /// Página de código: texto de traço de 1 px (como IDE/navegador de verdade), 14 px por linha, cores de sintaxe, começando na linha
    /// absoluta `top_px / 14` e rolada `top_px % 14` px. Barra lateral e barra de título fixas.
    fn pagina(w: usize, h: usize, top_px: usize, t: usize) -> Vec<u8> {
        let mut f = vec![0u8; w * h * 4];
        for y in 0..h { for x in 0..w { let o = (y * w + x) * 4; let (b, g, r) = if y < 36 { (58, 40, 40) } else if x < 280 { (44, 40, 37) } else { (30, 30, 30) }; f[o] = b; f[o + 1] = g; f[o + 2] = r; f[o + 3] = 255; } }
        let cores = [(230u8, 230u8, 230u8), (86, 156, 214), (206, 145, 120), (78, 201, 176), (220, 220, 170)];
        let mut put = |x0: usize, y0: usize, cell: u32, col: (u8, u8, u8)| {
            let gl = glyph(cell);
            for (ry, bits) in gl.iter().enumerate() { for rx in 0..5 { if bits >> rx & 1 == 1 { let (px, py) = (x0 + rx, y0 + ry); if px < w && py < h { let o = (py * w + px) * 4; f[o] = col.2; f[o + 1] = col.1; f[o + 2] = col.0; } } } }
        };
        let first = top_px / 14; let off = top_px % 14;
        for k in 0..((h - 36) / 14 + 2) {
            let ln = first + k; let y0 = (36 + k * 14).wrapping_sub(off);
            if y0 + 8 > h || y0 < 30 { continue; }
            let indent = (ln * 7 % 4) * 28;
            let mut c = 0;
            while 300 + indent + c * 7 < w - 20 { if (c * 5 + ln * 3) % 9 < 7 { put(300 + indent + c * 7, y0, (ln * 977 + c * 31) as u32, cores[(ln + c / 6) % 5]); } c += 1; }
            if ln % 3 == 0 { for c in 0..30 { put(14 + c * 7, y0, (ln * 131 + c * 17) as u32, (200, 200, 200)); } }   // árvore de arquivos
        }
        for c in 0..8 { put(w - 140 + c * 7, 12, (t * 13 + c * 7) as u32, (255, 255, 255)); }   // relógio
        f
    }

    /// Texto rolando (IDE): 2 s a 3 px/quadro, 1 s parado, 2 s a 14 px/quadro (salto de linha), 1 s parado. Mede PSNR por trecho, bitrate e
    /// fps reais e guarda quadro original x decodificado. Variáveis: BRIKS_Q_CASOS (mbps:altura), BRIKS_Q_FPS, BRIKS_PROVAS_OUT.
    #[test]
    #[ignore]
    fn rolagem_de_texto() {
        let (w, h) = (1920usize, 1080usize);
        let fps: usize = std::env::var("BRIKS_Q_FPS").ok().and_then(|v| v.parse().ok()).unwrap_or(24);
        let lista: Vec<(u32, usize)> = std::env::var("BRIKS_Q_CASOS").unwrap_or("6.6:1080".into()).split(',').map(|v| { let (a, b) = v.split_once(':').unwrap(); ((a.parse::<f64>().unwrap() * 1e6) as u32, b.parse().unwrap()) }).collect();
        let out = std::env::var("BRIKS_PROVAS_OUT").ok().map(std::path::PathBuf::from);
        if let Some(o) = &out { let _ = std::fs::create_dir_all(o); }
        for (bps, eh) in lista {
            let ew = eh * 16 / 9 / 2 * 2;
            let mut enc = H264Enc::new(ew, eh, fps as f32, bps).unwrap();
            let mut dec = H264Dec::new().unwrap();
            { let f = pagina(w, h, 100, 1); let y = bgra_to_i420(&f, w, h, w, h); let mut rgba = vec![255u8; w * h * 4]; i420_to_rgba(&y.y, &y.u, &y.v, (w, w / 2, w / 2), w, h, &mut rgba);
              eprintln!("teto da subamostragem 4:2:0 (sem codificar): PSNR {:.1} dB, luma {:.1} dB", media(&rgba, &f), luma_psnr(&f, &rgba)); }
            // fases: (quadros, px por quadro)
            let fases = [(2 * fps, 0usize), (3 * fps, 3usize), (fps, 0), (2 * fps, 14), (fps, 0)];   // a 1ª é aquecimento (quadro-chave + refino) e não entra no resumo
            let (mut t, mut top) = (0usize, 0usize);
            let mut res: Vec<String> = vec![];
            for (fi, (n, v)) in fases.iter().enumerate() {
                let (mut bytes, mut psum, mut pmin, mut got, mut maxq, mut lsum) = (0usize, 0.0f64, 99.0f64, 0usize, 0usize, 0.0f64);
                let t0 = std::time::Instant::now();
                for k in 0..*n {
                    top += v; t += 1;
                    let f = pagina(w, h, top, t);
                    let yuv = bgra_to_i420(&f, w, h, ew, eh);
                    for au in enc.encode(&yuv).unwrap() {
                        bytes += au.len(); maxq = maxq.max(au.len());
                        if let Some((dw, dh, rgba)) = dec.decode(&au).unwrap() {
                            let up = if (dw, dh) == (w, h) { rgba.to_vec() } else { amplia(rgba, dw, dh, w, h) };
                            let p = media(&up, &f); psum += p; pmin = pmin.min(p); got += 1; lsum += luma_psnr(&f, &up);
                            if k == n - 1 { if let Some(o) = &out {
                                let tag = format!("{}-{:.1}M-{}p-fase{}", enc.name.replace(' ', "_"), bps as f64 / 1e6, eh, fi);
                                let save = |name: &str, rgba: &[u8]| { if let Some(img) = image::RgbaImage::from_raw(300, 150, { let mut c = Vec::with_capacity(300 * 150 * 4); for y in 300..450 { c.extend_from_slice(&rgba[(y * w + 400) * 4..(y * w + 700) * 4]); } c }) { let _ = image::imageops::resize(&img, 900, 450, image::imageops::FilterType::Nearest).save(o.join(format!("{tag}-{name}.png"))); } };
                                let mut orig = f.clone(); for p in orig.chunks_mut(4) { p.swap(0, 2); }
                                save("original", &orig); save("recebido", &up);
                            } }
                        }
                    }
                }
                let dur = (*n as f64) / fps as f64;
                if fi > 0 { res.push(format!("fase{fi} ({}px/q): {:.2} Mbps, PSNR médio {:.1} mín {:.1} dB, luma {:.1} dB, maior AU {} KB, {:.0} ms de CPU/quadro", v, bytes as f64 * 8.0 / dur / 1e6, psum / got.max(1) as f64, pmin, lsum / got.max(1) as f64, maxq / 1024, t0.elapsed().as_secs_f64() * 1000.0 / *n as f64)); }
            }
            eprintln!("[{}] {:.1} Mbps {}p {} fps: {}", enc.name, bps as f64 / 1e6, eh, fps, res.join(" | "));
        }
    }

    /// "Pulso" do quadro-chave: PSNR da luminância quadro a quadro em volta de um IDR forçado, com a página parada e com ela rolando.
    /// Um IDR pior (ou melhor) que os quadros vizinhos aparece na tela como piscada. BRIKS_Q_CASOS = mbps:altura.
    #[test]
    #[ignore]
    fn pulso_do_quadro_chave() {
        let (w, h) = (1920usize, 1080usize);
        let (bps, eh): (u32, usize) = { let v = std::env::var("BRIKS_Q_CASOS").unwrap_or("4.5:1080".into()); let (a, b) = v.split(',').next().unwrap().split_once(':').unwrap(); ((a.parse::<f64>().unwrap() * 1e6) as u32, b.parse().unwrap()) };
        let ew = eh * 16 / 9 / 2 * 2;
        for (nome, vel) in [("parada", 0usize), ("rolando 3 px/q", 3)] {
            let mut enc = H264Enc::new(ew, eh, 24.0, bps).unwrap();
            let mut dec = H264Dec::new().unwrap();
            let (mut top, mut linha) = (0usize, vec![]);
            for t in 0..120usize {
                if t == 60 { enc.force_idr(); }
                top += vel;
                let f = pagina(w, h, top, t);
                let yuv = bgra_to_i420(&f, w, h, ew, eh);
                for au in enc.encode(&yuv).unwrap() { if let Some((dw, dh, rgba)) = dec.decode(&au).unwrap() { let up = if (dw, dh) == (w, h) { rgba.to_vec() } else { amplia(rgba, dw, dh, w, h) }; if t >= 52 && t <= 72 { linha.push(format!("{}:{:.1}({}KB)", t, luma_psnr(&f, &up), au.len() / 1024)); } } }
            }
            eprintln!("[{}] {} a {:.1} Mbps, IDR forçado no quadro 60 — quadro:luma PSNR(tamanho): {}", enc.name, nome, bps as f64 / 1e6, linha.join(" "));
        }
    }

    /// Quadro de "filme": fundo em degradê que se desloca (panorâmica), formas que andam, grão leve que muda a cada quadro, corte de
    /// cena a cada 4 s e uma legenda (texto claro sobre a imagem) que troca a cada 2 s. É o conteúdo das queixas de blocos/"fantasmas".
    fn filme(w: usize, h: usize, t: usize, fps: usize) -> Vec<u8> {
        let mut f = vec![255u8; w * h * 4];
        let cena = t / (4 * fps);
        // amplitude do grão em níveis (BRIKS_Q_GRAO; 4 = filme limpo, 15 = grão pesado / jogo caótico)
        let grao: u32 = std::env::var("BRIKS_Q_GRAO").ok().and_then(|v| v.parse().ok()).unwrap_or(6);
        let (pan, mut rng) = (t * 3, (t as u32).wrapping_mul(747796405).wrapping_add(2891336453));
        for y in 0..h { for x in 0..w {
            let o = (y * w + x) * 4;
            let (xx, yy) = (x + pan + cena * 517, y + cena * 211);
            let base = ((xx as f32 * 0.011).sin() * 40.0 + (yy as f32 * 0.017).cos() * 40.0 + ((xx + yy) as f32 * 0.004).sin() * 30.0) as i32;
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            let g = (rng % (2 * grao + 1)) as i32 - grao as i32;   // grão de filme (o que mais custa bits e o que vira blocos quando o bitrate falta)
            let tex = (((xx / 3) ^ (yy / 3)) & 7) as i32 * 4 - 14;   // textura fina que anda com a panorâmica
            let c = |k: i32| (110 + base * k / 3 + g + tex + (cena as i32 * 23) % 40).clamp(0, 255) as u8;
            f[o] = c(2); f[o + 1] = c(3); f[o + 2] = c(1);
        } }
        // formas que andam (personagens)
        for k in 0..4usize { let cx = (200 + k * 420 + t * (4 + k)) % w; let cy = h / 3 + k * 90; for y in cy.saturating_sub(70)..(cy + 70).min(h) { for x in cx.saturating_sub(50)..(cx + 50).min(w) { let o = (y * w + x) * 4; f[o] = 40 + (k * 50) as u8; f[o + 1] = 60; f[o + 2] = 200 - (k * 40) as u8; } } }
        // legenda: duas linhas de "texto" de traço de 3 px, troca a cada 2 s
        let fala = t / (2 * fps);
        for ln in 0..2usize { for c in 0..36usize { if (c * 5 + fala * 7 + ln) % 8 < 6 { let gl = glyph((fala * 131 + ln * 17 + c) as u32); let (x0, y0) = (w / 2 - 36 * 13 + c * 26, h - 150 + ln * 46);
            for (ry, bits) in gl.iter().enumerate() { for rx in 0..5 { if bits >> rx & 1 == 1 { for dy in 0..4 { for dx in 0..4 { let (px, py) = (x0 + rx * 4 + dx, y0 + ry * 4 + dy); if px < w && py < h { let o = (py * w + px) * 4; f[o] = 245; f[o + 1] = 245; f[o + 2] = 245; } } } } } } } } }
        f
    }

    /// Teto de QP x obediência ao bitrate: grão pesado a 3 Mbps em 1080p, um codificador por teto (o NVENC não obedece a troca ao vivo).
    /// Com 28 o codificador ignora o bitrate; a partir de 34 obedece. É o que o `share::QpGovernor` usa.
    #[test]
    #[ignore]
    fn teto_de_qp_e_bitrate() {
        std::env::set_var("BRIKS_Q_GRAO", "15");
        let (w, h, fps, bps) = (1920usize, 1080usize, 30usize, 3_000_000u32);
        let mut linha = vec![];
        let mut nome = String::new();
        for qp in [28u32, 34, 40, 51] {
            let mut enc = H264Enc::new_with_qp(w, h, fps as f32, bps, true, qp).unwrap();
            let mut bytes = 0usize;
            for t in 0..3 * fps { let f = filme(w, h, t, fps); for au in enc.encode(&bgra_to_i420(&f, w, h, w, h)).unwrap() { if t >= fps { bytes += au.len(); } } }
            linha.push(format!("teto {qp}: {:.1} Mbps", bytes as f64 * 8.0 / 2.0 / 1e6));
            nome = enc.name.clone();
        }
        eprintln!("[{nome}] grão pesado, pedido {:.1} Mbps: {}", bps as f64 / 1e6, linha.join(" | "));
    }

    /// Filme a 1080p: PSNR médio e mínimo, PSNR só da faixa da legenda logo depois de cada troca de fala (o "fantasma" da legenda
    /// antiga aparece aqui), bitrate real e maior quadro, com quadro-chave periódico (BRIKS_Q_IDR_S, 0 = só o inicial).
    /// BRIKS_Q_CASOS = mbps:altura; BRIKS_Q_FPS; mais as variáveis de diagnóstico do codificador (BRIKS_ENC_*).
    #[test]
    #[ignore]
    fn filme_por_bitrate() {
        let (w, h) = (1920usize, 1080usize);
        let fps: usize = std::env::var("BRIKS_Q_FPS").ok().and_then(|v| v.parse().ok()).unwrap_or(30);
        let idr_s: usize = std::env::var("BRIKS_Q_IDR_S").ok().and_then(|v| v.parse().ok()).unwrap_or(3);
        let lista: Vec<(u32, usize)> = std::env::var("BRIKS_Q_CASOS").unwrap_or("1.5:1080,3:1080,6:1080".into()).split(',').map(|v| { let (a, b) = v.split_once(':').unwrap(); ((a.parse::<f64>().unwrap() * 1e6) as u32, b.parse().unwrap()) }).collect();
        for (bps, eh) in lista {
            let ew = eh * 16 / 9 / 2 * 2;
            let mut enc = H264Enc::new(ew, eh, fps as f32, bps).unwrap();
            let mut dec = H264Dec::new().unwrap();
            let n = 10 * fps;
            let (mut bytes, mut psum, mut pmin, mut got, mut maxq, mut leg_sum, mut leg_min, mut leg_n, mut enc_ms, mut win_max) = (0usize, 0.0f64, 99.0f64, 0usize, 0usize, 0.0f64, 99.0f64, 0usize, 0.0f64, 0usize);
            let mut win: std::collections::VecDeque<usize> = Default::default();
            for t in 0..n {
                let f = filme(w, h, t, fps);
                let yuv = bgra_to_i420(&f, w, h, ew, eh);
                if idr_s > 0 && t > 0 && t % (idr_s * fps) == 0 { enc.force_idr(); }
                let t0 = std::time::Instant::now();
                let aus = enc.encode(&yuv).unwrap();
                enc_ms += t0.elapsed().as_secs_f64() * 1000.0;
                let mut q = 0;
                for au in aus {
                    bytes += au.len(); maxq = maxq.max(au.len()); q += au.len();
                    if let Some((dw, dh, rgba)) = dec.decode(&au).unwrap() {
                        let up = if (dw, dh) == (w, h) { rgba.to_vec() } else { amplia(rgba, dw, dh, w, h) };
                        if t >= fps { let p = luma_psnr(&f, &up); psum += p; pmin = pmin.min(p); got += 1; }   // o 1º segundo é aquecimento
                        // faixa da legenda nos 6 quadros depois de cada troca de fala
                        if t >= fps && t % (2 * fps) < 6 { let (a, b) = ((h - 160) * w * 4, (h - 50) * w * 4); let p = luma_psnr(&f[a..b], &up[a..b]); leg_sum += p; leg_min = leg_min.min(p); leg_n += 1; }
                    }
                }
                // maior volume em qualquer janela de 1 s (o que o pacer e a rede têm de aguentar)
                win.push_back(q); if win.len() > fps { win.pop_front(); } if t >= fps { win_max = win_max.max(win.iter().sum()); }
            }
            eprintln!("[{}] filme {:.1} Mbps {}p {} fps, IDR a cada {} s: real {:.2} Mbps (pico de 1 s {:.2}), luma PSNR médio {:.1} mín {:.1} dB, legenda pós-troca média {:.1} mín {:.1} dB, maior quadro {} KB, codificar {:.1} ms/quadro",
                enc.name, bps as f64 / 1e6, eh, fps, idr_s, bytes as f64 * 8.0 / (n as f64 / fps as f64) / 1e6, win_max as f64 * 8.0 / 1e6, psum / got.max(1) as f64, pmin, leg_sum / leg_n.max(1) as f64, leg_min, maxq / 1024, enc_ms / n as f64);
        }
    }

    #[test]
    #[ignore]
    fn psnr_da_tela_por_bitrate() {
        let (w, h) = (1920usize, 1080usize);
        let lista: Vec<(u32, usize)> = std::env::var("BRIKS_Q_CASOS").unwrap_or("2.5:1080,4.5:1080,6:1080,10:1080".into()).split(',').map(|v| { let (a, b) = v.split_once(':').unwrap(); ((a.parse::<f64>().unwrap() * 1e6) as u32, b.parse::<usize>().unwrap()) }).collect();
        for (bps, eh) in lista {
            let ew = eh * 16 / 9 / 2 * 2;
            let mut enc = H264Enc::new(ew, eh, 30.0, bps).unwrap();
            let mut dec = H264Dec::new().unwrap();
            let (mut bytes, mut psnr_idr, mut psnr_fim, mut kb_idr) = (0usize, 0.0f64, 0.0f64, 0usize);
            for t in 0..90 {
                let f = tela(w, h, t);
                let yuv = bgra_to_i420(&f, w, h, ew, eh);
                for au in enc.encode(&yuv).unwrap() {
                    bytes += au.len();
                    if t == 0 { kb_idr += au.len(); }
                    if let Some((dw, dh, rgba)) = dec.decode(&au).unwrap() { let up = if (dw, dh) == (w, h) { rgba.to_vec() } else { amplia(rgba, dw, dh, w, h) }; let p = media(&up, &f); if t == 0 { psnr_idr = p; } psnr_fim = p; }
                }
            }
            eprintln!("[{}] {:.1} Mbps em {}p: real {:.2} Mbps, IDR {} KB PSNR {:.1} dB, PSNR no quadro 90 (ampliado a 1080p) {:.1} dB", enc.name, bps as f64 / 1e6, eh, bytes as f64 * 8.0 / 3.0 / 1e6, kb_idr / 1024, psnr_idr, psnr_fim);
        }
    }
}
