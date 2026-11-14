//! Conversão de cor + escala na GPU: BGRA (textura da captura) → NV12 já no tamanho de envio, pelo processador de vídeo do
//! Direct3D 11. Só o quadro reduzido (1,5 byte por pixel) volta para a memória, em vez do BGRA inteiro (4 bytes por pixel da
//! origem) seguido de conversão na CPU. Qualquer falha devolve erro e quem chama cai no caminho por CPU.
use anyhow::{anyhow, Result};
use windows::core::Interface;
use windows::Win32::Foundation::TRUE;
use windows::Win32::Graphics::Direct3D11::*;
use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT_NV12, DXGI_RATIONAL, DXGI_SAMPLE_DESC};

use crate::video::Yuv;

struct Chain {
    key: (u32, u32, u32, u32, u32, u32),   // área visível, destino e tamanho real da textura da captura (muda quando a janela é redimensionada)
    input: ID3D11Texture2D,      // cópia da textura da captura (com as permissões que o processador exige)
    out: ID3D11Texture2D,
    staging: ID3D11Texture2D,
    vp: ID3D11VideoProcessor,
    iv: ID3D11VideoProcessorInputView,
    ov: ID3D11VideoProcessorOutputView,
}

#[derive(Default)]
pub struct GpuConv { chain: Option<Chain> }
unsafe impl Send for GpuConv {}

impl GpuConv {
    /// Converte `tex` (BGRA, `sw` x `sh` visíveis) para I420 `dw` x `dh` (pares).
    pub fn convert(&mut self, tex: &ID3D11Texture2D, sw: u32, sh: u32, dw: u32, dh: u32) -> Result<Yuv> {
        unsafe {
            let dev = tex.GetDevice()?;
            let ctx = dev.GetImmediateContext()?;
            let vctx: ID3D11VideoContext = ctx.cast()?;
            let mut td = D3D11_TEXTURE2D_DESC::default();
            tex.GetDesc(&mut td);
            let key = (sw, sh, dw, dh, td.Width, td.Height);
            if self.chain.as_ref().map(|c| c.key != key).unwrap_or(true) {
                self.chain = None;
                self.chain = Some(Self::build(&dev, &vctx, tex, sw, sh, dw, dh)?);
            }
            let c = self.chain.as_ref().unwrap();
            ctx.CopyResource(&c.input, tex);
            let stream = D3D11_VIDEO_PROCESSOR_STREAM { Enable: TRUE, pInputSurface: std::mem::ManuallyDrop::new(Some(c.iv.clone())), ..Default::default() };
            let mut streams = [stream];
            let r = vctx.VideoProcessorBlt(&c.vp, &c.ov, 0, &streams);
            std::mem::ManuallyDrop::drop(&mut streams[0].pInputSurface);
            r?;
            ctx.CopyResource(&c.staging, &c.out);
            let mut m = D3D11_MAPPED_SUBRESOURCE::default();
            ctx.Map(&c.staging, 0, D3D11_MAP_READ, 0, Some(&mut m))?;
            let (w, h, pitch) = (dw as usize, dh as usize, m.RowPitch as usize);
            let data = std::slice::from_raw_parts(m.pData as *const u8, pitch * h + pitch * (h / 2 - 1) + w);
            let mut y = vec![0u8; w * h];
            for (r, row) in y.chunks_exact_mut(w).enumerate() { row.copy_from_slice(&data[r * pitch..r * pitch + w]); }
            let (mut u, mut v) = (vec![0u8; w * h / 4], vec![0u8; w * h / 4]);
            let uv = &data[pitch * h..];
            for r in 0..h / 2 {
                let src = &uv[r * pitch..r * pitch + w];
                let (ur, vr) = (&mut u[r * (w / 2)..(r + 1) * (w / 2)], &mut v[r * (w / 2)..(r + 1) * (w / 2)]);
                for ((p, a), b) in src.chunks_exact(2).zip(ur.iter_mut()).zip(vr.iter_mut()) { *a = p[0]; *b = p[1]; }
            }
            ctx.Unmap(&c.staging, 0);
            Ok(Yuv { w, h, y, u, v })
        }
    }

    unsafe fn build(dev: &ID3D11Device, vctx: &ID3D11VideoContext, tex: &ID3D11Texture2D, sw: u32, sh: u32, dw: u32, dh: u32) -> Result<Chain> {
        let vdev: ID3D11VideoDevice = dev.cast().map_err(|e| anyhow!("sem ID3D11VideoDevice: {e}"))?;
        let mut d = D3D11_TEXTURE2D_DESC::default();
        tex.GetDesc(&mut d);
        let (tex_w, tex_h) = (d.Width, d.Height);
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = (D3D11_BIND_RENDER_TARGET.0 | D3D11_BIND_SHADER_RESOURCE.0) as u32; d.CPUAccessFlags = 0; d.MiscFlags = 0;
        let mut input = None;
        dev.CreateTexture2D(&d, None, Some(&mut input))?;
        let input = input.ok_or_else(|| anyhow!("textura de entrada"))?;
        let mut od = D3D11_TEXTURE2D_DESC { Width: dw, Height: dh, MipLevels: 1, ArraySize: 1, Format: DXGI_FORMAT_NV12, SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            Usage: D3D11_USAGE_DEFAULT, BindFlags: D3D11_BIND_RENDER_TARGET.0 as u32, CPUAccessFlags: 0, MiscFlags: 0 };
        let mut out = None;
        dev.CreateTexture2D(&od, None, Some(&mut out))?;
        let out = out.ok_or_else(|| anyhow!("textura NV12"))?;
        od.Usage = D3D11_USAGE_STAGING; od.BindFlags = 0; od.CPUAccessFlags = D3D11_CPU_ACCESS_READ.0 as u32;
        let mut staging = None;
        dev.CreateTexture2D(&od, None, Some(&mut staging))?;
        let staging = staging.ok_or_else(|| anyhow!("textura de leitura"))?;
        let cd = D3D11_VIDEO_PROCESSOR_CONTENT_DESC { InputFrameFormat: D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, InputFrameRate: DXGI_RATIONAL { Numerator: 60, Denominator: 1 }, InputWidth: tex_w, InputHeight: tex_h,
            OutputFrameRate: DXGI_RATIONAL { Numerator: 60, Denominator: 1 }, OutputWidth: dw, OutputHeight: dh, Usage: D3D11_VIDEO_USAGE_PLAYBACK_NORMAL };
        let en = vdev.CreateVideoProcessorEnumerator(&cd)?;
        let vp = vdev.CreateVideoProcessor(&en, 0)?;
        let ivd = D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC { FourCC: 0, ViewDimension: D3D11_VPIV_DIMENSION_TEXTURE2D, Anonymous: D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC_0 { Texture2D: D3D11_TEX2D_VPIV { MipSlice: 0, ArraySlice: 0 } } };
        let mut iv = None;
        vdev.CreateVideoProcessorInputView(&input, &en, &ivd, Some(&mut iv))?;
        let ovd = D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC { ViewDimension: D3D11_VPOV_DIMENSION_TEXTURE2D, Anonymous: D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC_0 { Texture2D: D3D11_TEX2D_VPOV { MipSlice: 0 } } };
        let mut ov = None;
        vdev.CreateVideoProcessorOutputView(&out, &en, &ovd, Some(&mut ov))?;
        // só a área visível da textura (a janela capturada pode ser menor que a textura do pool)
        let src = windows::Win32::Foundation::RECT { left: 0, top: 0, right: sw.min(tex_w) as i32, bottom: sh.min(tex_h) as i32 };
        vctx.VideoProcessorSetStreamSourceRect(&vp, 0, true, Some(&src));
        vctx.VideoProcessorSetStreamFrameFormat(&vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        // entrada RGB faixa total; saída BT.601 faixa limitada (16–235), igual ao caminho por CPU e ao que o receptor espera
        vctx.VideoProcessorSetStreamColorSpace(&vp, 0, &D3D11_VIDEO_PROCESSOR_COLOR_SPACE { _bitfield: 0 });
        vctx.VideoProcessorSetOutputColorSpace(&vp, &D3D11_VIDEO_PROCESSOR_COLOR_SPACE { _bitfield: 1 << 4 });
        Ok(Chain { key: (sw, sh, dw, dh, tex_w, tex_h), input, out, staging, vp, iv: iv.ok_or_else(|| anyhow!("vista de entrada"))?, ov: ov.ok_or_else(|| anyhow!("vista de saída"))? })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use windows::Win32::Graphics::Direct3D::{D3D_DRIVER_TYPE_HARDWARE, D3D_FEATURE_LEVEL_11_0};
    use windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT_B8G8R8A8_UNORM;

    unsafe fn device() -> (ID3D11Device, ID3D11DeviceContext) {
        let (mut dev, mut ctx) = (None, None);
        D3D11CreateDevice(None, D3D_DRIVER_TYPE_HARDWARE, windows::Win32::Foundation::HMODULE::default(), D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, Some(&[D3D_FEATURE_LEVEL_11_0]), D3D11_SDK_VERSION, Some(&mut dev), None, Some(&mut ctx)).expect("D3D11");
        (dev.unwrap(), ctx.unwrap())
    }
    unsafe fn bgra_texture(dev: &ID3D11Device, w: u32, h: u32, px: &[u8]) -> ID3D11Texture2D {
        let d = D3D11_TEXTURE2D_DESC { Width: w, Height: h, MipLevels: 1, ArraySize: 1, Format: DXGI_FORMAT_B8G8R8A8_UNORM, SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            Usage: D3D11_USAGE_DEFAULT, BindFlags: (D3D11_BIND_RENDER_TARGET.0 | D3D11_BIND_SHADER_RESOURCE.0) as u32, CPUAccessFlags: 0, MiscFlags: 0 };
        let init = D3D11_SUBRESOURCE_DATA { pSysMem: px.as_ptr() as *const _, SysMemPitch: w * 4, SysMemSlicePitch: 0 };
        let mut t = None;
        dev.CreateTexture2D(&d, Some(&init), Some(&mut t)).expect("textura");
        t.unwrap()
    }

    /// A conversão na GPU tem de dar (quase) as mesmas cores que a da CPU: mesma matriz e mesma faixa.
    #[test]
    fn gpu_conversion_matches_the_cpu_one() {
        unsafe {
            let (dev, _ctx) = device();
            let (w, h) = (640u32, 360u32);
            let mut px = vec![255u8; (w * h * 4) as usize];
            for (i, p) in px.chunks_mut(4).enumerate() { let (x, y) = (i as u32 % w, i as u32 / w); let k = (x / 160 + (y / 180) * 4) as usize;
                let c = [(40u8, 160u8, 220u8), (200, 30, 90), (255, 255, 255), (0, 0, 0), (0, 0, 255), (0, 255, 0), (255, 0, 0), (128, 128, 128)][k]; p[0] = c.0; p[1] = c.1; p[2] = c.2; }
            let tex = bgra_texture(&dev, w, h, &px);
            let mut g = GpuConv::default();
            let a = match g.convert(&tex, w, h, w, h) { Ok(a) => a, Err(e) => { eprintln!("sem GPU utilizável aqui ({e}); teste ignorado"); return; } };   // runner do CI: só adaptador de software
            let b = crate::video::bgra_to_i420(&px, w as usize, h as usize, w as usize, h as usize);
            let mut worst = 0i32;
            for k in 0..8usize { let (cx, cy) = ((k % 4) * 160 + 80, (k / 4) * 180 + 90);
                let (ya, yb) = (a.y[cy * 640 + cx] as i32, b.y[cy * 640 + cx] as i32);
                let (ua, ub) = (a.u[(cy / 2) * 320 + cx / 2] as i32, b.u[(cy / 2) * 320 + cx / 2] as i32);
                let (va, vb) = (a.v[(cy / 2) * 320 + cx / 2] as i32, b.v[(cy / 2) * 320 + cx / 2] as i32);
                worst = worst.max((ya - yb).abs()).max((ua - ub).abs()).max((va - vb).abs());
                eprintln!("cor {k}: GPU ({ya},{ua},{va}) × CPU ({yb},{ub},{vb})"); }
            assert!(worst <= 4, "diferença de até {worst} níveis");
        }
    }

    /// Medição antes/depois sem capturar tela nenhuma: textura sintética na GPU → I420. "CPU" = o caminho antigo (copia a textura
    /// inteira para a memória e converte); "GPU" = o novo. Tempo de parede e tempo de CPU do processo por quadro.
    #[test]
    #[ignore]
    fn bench_gpu_vs_cpu_conversion() {
        unsafe {
            let (dev, ctx) = device();
            for (sw, sh, dw, dh, nome) in [(1920u32, 1080u32, 1920u32, 1080u32, "1080p"), (2560, 1440, 2560, 1440, "1440p"), (3440, 1440, 2580, 1080, "ultrawide→1080p"), (3440, 1440, 3440, 1440, "ultrawide nativo")] {
                let mut px = vec![255u8; (sw * sh * 4) as usize];
                let mut x = 0x1234567u32;
                for p in px.chunks_mut(4) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; p[0] = x as u8; p[1] = (x >> 8) as u8; p[2] = (x >> 16) as u8; }
                let tex = bgra_texture(&dev, sw, sh, &px);
                let mut d = D3D11_TEXTURE2D_DESC::default(); tex.GetDesc(&mut d);
                d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ.0 as u32;
                let mut st = None; dev.CreateTexture2D(&d, None, Some(&mut st)).unwrap(); let st = st.unwrap();
                let n = 60;
                let (c0, t0) = (crate::share::cpu_ms(), std::time::Instant::now());
                for _ in 0..n {
                    ctx.CopyResource(&st, &tex);
                    let mut m = D3D11_MAPPED_SUBRESOURCE::default();
                    ctx.Map(&st, 0, D3D11_MAP_READ, 0, Some(&mut m)).unwrap();
                    let pitch = m.RowPitch as usize;
                    let data = std::slice::from_raw_parts(m.pData as *const u8, pitch * (sh as usize - 1) + sw as usize * 4);
                    let y = crate::video::bgra_to_i420_stride(data, pitch, sw as usize, sh as usize, dw as usize, dh as usize);
                    std::hint::black_box(&y);
                    ctx.Unmap(&st, 0);
                }
                let (cpu_wall, cpu_cpu) = (t0.elapsed().as_secs_f64() * 1000.0 / n as f64, (crate::share::cpu_ms() - c0) / n as f64);
                let mut g = GpuConv::default();
                g.convert(&tex, sw, sh, dw, dh).expect("GPU");
                let (c0, t0) = (crate::share::cpu_ms(), std::time::Instant::now());
                for _ in 0..n { let y = g.convert(&tex, sw, sh, dw, dh).unwrap(); std::hint::black_box(&y); }
                let (gpu_wall, gpu_cpu) = (t0.elapsed().as_secs_f64() * 1000.0 / n as f64, (crate::share::cpu_ms() - c0) / n as f64);
                eprintln!("{nome} ({sw}x{sh} → {dw}x{dh}): CPU {cpu_wall:.1} ms de parede, {cpu_cpu:.1} ms de CPU por quadro | GPU {gpu_wall:.1} ms de parede, {gpu_cpu:.1} ms de CPU por quadro");
            }
        }
    }
}
