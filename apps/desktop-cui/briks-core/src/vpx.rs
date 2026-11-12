//! VP8/VP9 por software (libvpx, BSD-3) atrás da feature `vpx`. Mesma forma do `H264Enc`/`H264Dec`: I420 entra, quadros comprimidos saem
//! (um por chamada, sem Annex-B: VP8/VP9 já vêm como quadros inteiros, prontos para o empacotador RTP), e o inverso no receptor.
//! A libvpx é compilada por `scripts/build-vpx.ps1` (ver docs/vp8-vp9-viabilidade.md); sem a feature, nada disto existe e o app só fala H264.
use crate::video::Yuv;
use anyhow::{anyhow, Result};
use std::mem::MaybeUninit;
use vpx_sys as v;

pub use crate::codec::Codec;

/// ABI do codificador da libvpx 1.14.1 (16 + codec 9 + ratectrl 7 + tpl 2). Os bindings gerados são da 1.13 (30) e a struct de configuração é idêntica;
/// só este número mudou. Se a libvpx for trocada de versão, a init falha com VPX_CODEC_ABI_MISMATCH em vez de corromper memória.
const ENCODER_ABI: i32 = 34;

fn check(r: v::vpx_codec_err_t, what: &str, ctx: *mut v::vpx_codec_ctx_t) -> Result<()> {
    if r == v::vpx_codec_err_t::VPX_CODEC_OK { return Ok(()); }
    let detail = unsafe { let p = v::vpx_codec_error(ctx); if p.is_null() { String::new() } else { std::ffi::CStr::from_ptr(p).to_string_lossy().into_owned() } };
    Err(anyhow!("{what}: {r:?} {detail}"))
}

pub struct VpxEnc {
    ctx: v::vpx_codec_ctx_t,
    cfg: v::vpx_codec_enc_cfg_t,
    codec: Codec,
    pts: i64,
    ticks: u32,
    force_key: bool,
    pub w: usize,
    pub h: usize,
    pub bps: u32,
}
unsafe impl Send for VpxEnc {}

impl VpxEnc {
    pub fn new(codec: Codec, w: usize, h: usize, fps: f32, bps: u32) -> Result<Self> {
        let iface = unsafe { match codec { Codec::Vp8 => v::vpx_codec_vp8_cx(), Codec::Vp9 => v::vpx_codec_vp9_cx(), Codec::H264 => return Err(anyhow!("H264 não é da libvpx")) } };
        unsafe {
            let mut cfg: v::vpx_codec_enc_cfg_t = MaybeUninit::zeroed().assume_init();
            check(v::vpx_codec_enc_config_default(iface, &mut cfg, 0), "vpx config", std::ptr::null_mut())?;
            cfg.g_w = w as u32; cfg.g_h = h as u32;
            cfg.g_timebase = v::vpx_rational { num: 1, den: 90_000 };
            cfg.g_threads = std::thread::available_parallelism().map(|n| n.get().min(8) as u32).unwrap_or(2);
            cfg.g_lag_in_frames = 0;                 // tempo real: sem atraso de lookahead
            cfg.g_error_resilient = 0;
            cfg.rc_end_usage = v::vpx_rc_mode::VPX_CBR;
            cfg.rc_target_bitrate = (bps / 1000).max(50);
            cfg.rc_min_quantizer = 2; cfg.rc_max_quantizer = 56;
            cfg.rc_undershoot_pct = 50; cfg.rc_overshoot_pct = 50;
            cfg.rc_buf_sz = 1000; cfg.rc_buf_initial_sz = 600; cfg.rc_buf_optimal_sz = 600;
            cfg.rc_dropframe_thresh = 0;
            cfg.kf_mode = v::vpx_kf_mode::VPX_KF_DISABLED; cfg.kf_max_dist = 100_000;   // quadro-chave só a pedido (PLI/FIR), como no H264
            let mut ctx: v::vpx_codec_ctx_t = MaybeUninit::zeroed().assume_init();
            check(v::vpx_codec_enc_init_ver(&mut ctx, iface, &cfg, 0, ENCODER_ABI), "vpx init", &mut ctx)?;
            let ctl = |ctx: &mut v::vpx_codec_ctx_t, id: v::vp8e_enc_control_id, val: i32| { v::vpx_codec_control_(ctx, id as i32, val); };
            match codec {
                Codec::Vp8 => {
                    ctl(&mut ctx, v::vp8e_enc_control_id::VP8E_SET_CPUUSED, 8);
                    ctl(&mut ctx, v::vp8e_enc_control_id::VP8E_SET_STATIC_THRESHOLD, 1);
                    ctl(&mut ctx, v::vp8e_enc_control_id::VP8E_SET_SCREEN_CONTENT_MODE, 1);
                }
                _ => {
                    ctl(&mut ctx, v::vp8e_enc_control_id::VP8E_SET_CPUUSED, 7);
                    ctl(&mut ctx, v::vp8e_enc_control_id::VP9E_SET_AQ_MODE, 3);
                    ctl(&mut ctx, v::vp8e_enc_control_id::VP9E_SET_ROW_MT, 1);
                    ctl(&mut ctx, v::vp8e_enc_control_id::VP9E_SET_TILE_COLUMNS, if w >= 1280 { 2 } else { 1 });
                    ctl(&mut ctx, v::vp8e_enc_control_id::VP9E_SET_NOISE_SENSITIVITY, 0);
                    ctl(&mut ctx, v::vp8e_enc_control_id::VP9E_SET_FRAME_PARALLEL_DECODING, 1);
                    ctl(&mut ctx, v::vp8e_enc_control_id::VP9E_SET_TUNE_CONTENT, 1);   // conteúdo de tela
                }
            }
            Ok(VpxEnc { ctx, cfg, codec, pts: 0, ticks: (90_000.0 / fps.max(1.0)) as u32, force_key: true, w, h, bps })
        }
    }
    pub fn codec(&self) -> Codec { self.codec }
    pub fn force_idr(&mut self) { self.force_key = true; }
    /// Muda o bitrate sem recriar (a libvpx aceita no meio do fluxo).
    pub fn set_bitrate(&mut self, bps: u32) -> bool {
        self.cfg.rc_target_bitrate = (bps / 1000).max(50);
        let ok = unsafe { v::vpx_codec_enc_config_set(&mut self.ctx, &self.cfg) } == v::vpx_codec_err_t::VPX_CODEC_OK;
        if ok { self.bps = bps; }
        ok
    }
    pub fn set_fps(&mut self, fps: u32) { self.ticks = 90_000 / fps.max(1); }
    /// Codifica um quadro I420. Devolve o quadro VP8/VP9 e se é quadro-chave (nenhum = o codificador não soltou quadro desta vez).
    pub fn encode(&mut self, yuv: &Yuv) -> Result<Vec<(Vec<u8>, bool)>> {
        let samples = self.ticks;
        if (yuv.w, yuv.h) != (self.w, self.h) { return Err(anyhow!("vpx: tamanho mudou")); }
        let force = std::mem::take(&mut self.force_key);
        unsafe {
            let mut img: v::vpx_image_t = MaybeUninit::zeroed().assume_init();
            img.fmt = v::vpx_img_fmt::VPX_IMG_FMT_I420;
            img.w = yuv.w as u32; img.h = yuv.h as u32; img.d_w = img.w; img.d_h = img.h; img.r_w = img.w; img.r_h = img.h;
            img.bit_depth = 8; img.x_chroma_shift = 1; img.y_chroma_shift = 1; img.bps = 12;
            img.planes = [yuv.y.as_ptr() as *mut u8, yuv.u.as_ptr() as *mut u8, yuv.v.as_ptr() as *mut u8, std::ptr::null_mut()];
            img.stride = [yuv.w as i32, (yuv.w / 2) as i32, (yuv.w / 2) as i32, 0];
            self.pts += samples.max(1) as i64;
            let r = v::vpx_codec_encode(&mut self.ctx, &img, self.pts, samples.max(1) as _, if force { v::VPX_EFLAG_FORCE_KF as _ } else { 0 }, v::VPX_DL_REALTIME as _);
            check(r, "vpx encode", &mut self.ctx)?;
            let mut out = vec![];
            let mut it: v::vpx_codec_iter_t = std::ptr::null();
            loop {
                let pkt = v::vpx_codec_get_cx_data(&mut self.ctx, &mut it);
                if pkt.is_null() { break; }
                if (*pkt).kind == v::vpx_codec_cx_pkt_kind::VPX_CODEC_CX_FRAME_PKT {
                    let f = (*pkt).data.frame;
                    let data = std::slice::from_raw_parts(f.buf as *const u8, f.sz).to_vec();
                    out.push((data, f.flags & v::VPX_FRAME_IS_KEY != 0));
                }
            }
            Ok(out)
        }
    }
}
impl Drop for VpxEnc { fn drop(&mut self) { unsafe { v::vpx_codec_destroy(&mut self.ctx); } } }

pub struct VpxDec {
    ctx: v::vpx_codec_ctx_t,
    buf: Vec<u8>,
    pub codec: Codec,
}
unsafe impl Send for VpxDec {}
impl VpxDec {
    pub fn new(codec: Codec) -> Result<Self> {
        let iface = unsafe { match codec { Codec::Vp8 => v::vpx_codec_vp8_dx(), Codec::Vp9 => v::vpx_codec_vp9_dx(), Codec::H264 => return Err(anyhow!("H264 não é da libvpx")) } };
        unsafe {
            let mut ctx: v::vpx_codec_ctx_t = MaybeUninit::zeroed().assume_init();
            let cfg = v::vpx_codec_dec_cfg_t { threads: 2, w: 0, h: 0 };
            check(v::vpx_codec_dec_init_ver(&mut ctx, iface, &cfg, 0, v::VPX_DECODER_ABI_VERSION as i32), "vpx dec init", &mut ctx)?;
            Ok(VpxDec { ctx, buf: vec![], codec })
        }
    }
    /// Decodifica um quadro; devolve (largura, altura, RGBA) se saiu imagem.
    pub fn decode(&mut self, frame: &[u8]) -> Result<Option<(usize, usize, &[u8])>> {
        unsafe {
            check(v::vpx_codec_decode(&mut self.ctx, frame.as_ptr(), frame.len() as u32, std::ptr::null_mut(), 0), "vpx decode", &mut self.ctx)?;
            let mut it: v::vpx_codec_iter_t = std::ptr::null();
            let img = v::vpx_codec_get_frame(&mut self.ctx, &mut it);
            if img.is_null() { return Ok(None); }
            let im = &*img;
            let (w, h) = (im.d_w as usize, im.d_h as usize);
            // vpx_image tem linhas com stride próprio e a altura de croma arredondada para cima
            let (cw, ch) = (w.div_ceil(2), h.div_ceil(2));
            let (sy, su, sv) = (im.stride[0] as usize, im.stride[1] as usize, im.stride[2] as usize);
            let y = std::slice::from_raw_parts(im.planes[0], sy * h);
            let u = std::slice::from_raw_parts(im.planes[1], su * (ch - 1) + cw);
            let vv = std::slice::from_raw_parts(im.planes[2], sv * (ch - 1) + cw);
            self.buf.resize(w * h * 4, 255);
            crate::video::i420_to_rgba(y, u, vv, (sy, su, sv), w, h, &mut self.buf);
            Ok(Some((w, h, &self.buf[..w * h * 4])))
        }
    }
}
impl Drop for VpxDec { fn drop(&mut self) { unsafe { v::vpx_codec_destroy(&mut self.ctx); } } }

#[cfg(test)]
mod tests {
    use super::*;
    fn frame(w: usize, h: usize, t: usize) -> Yuv {
        let mut y = vec![0u8; w * h];
        for r in 0..h { for c in 0..w { y[r * w + c] = (((c + t * 3) * 255 / w) as u8).wrapping_add((r / 8 % 2 * 40) as u8); } }
        Yuv { w, h, y, u: vec![110; w * h / 4], v: vec![150; w * h / 4] }
    }
    fn psnr(a: &Yuv, rgba: &[u8], w: usize, h: usize) -> f64 {
        let mut back = vec![0u8; w * h * 4];
        crate::video::i420_to_rgba(&a.y, &a.u, &a.v, (w, w / 2, w / 2), w, h, &mut back);
        let mut se = 0f64;
        for (p, q) in back.iter().zip(rgba) { let d = *p as f64 - *q as f64; se += d * d; }
        10.0 * (255.0f64 * 255.0 / (se / back.len() as f64).max(1e-9)).log10()
    }
    fn roundtrip(codec: Codec) {
        let (w, h) = (640usize, 360usize);
        let mut enc = VpxEnc::new(codec, w, h, 30.0, 2_000_000).expect("enc");
        let mut dec = VpxDec::new(codec).expect("dec");
        let (mut got, mut keys, mut best) = (0, 0, 0f64);
        for t in 0..30 {
            let f = frame(w, h, t);
            if t == 15 { enc.force_idr(); }
            for (data, key) in enc.encode(&f).unwrap() {
                keys += key as u32;
                if let Some((dw, dh, rgba)) = dec.decode(&data).unwrap() { assert_eq!((dw, dh), (w, h)); got += 1; best = best.max(psnr(&f, rgba, w, h)); }
            }
        }
        assert_eq!(got, 30, "todos os quadros decodificados");
        assert_eq!(keys, 2, "quadro-chave inicial + o pedido");
        assert!(best > 30.0, "PSNR {best}");
        assert!(enc.set_bitrate(500_000));
    }
    #[test] fn vp8_roundtrip() { roundtrip(Codec::Vp8); }
    #[test] fn vp9_roundtrip() { roundtrip(Codec::Vp9); }
}
