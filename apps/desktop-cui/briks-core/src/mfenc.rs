//! Codificador H264 do Media Foundation: usa o MFT de hardware (NVENC / Quick Sync / AMF) quando existe e o codificador de software
//! da Microsoft quando não. Entrada NV12 em memória, saída Annex-B (SPS/PPS repetidos em todo quadro-chave), baixa latência, CBR.
use anyhow::{anyhow, Result};
use windows::core::{Interface, GUID};
use windows::Win32::Media::MediaFoundation::*;
use windows::Win32::System::Com::{CoInitializeEx, CoTaskMemFree, COINIT_MULTITHREADED};
use windows::Win32::System::Variant::{VARIANT, VT_BOOL, VT_UI4};

fn var_u32(v: u32) -> VARIANT { let mut x = VARIANT::default(); unsafe { let a = &mut *x.Anonymous.Anonymous; a.vt = VT_UI4; a.Anonymous.ulVal = v; } x }
fn var_bool(v: bool) -> VARIANT { let mut x = VARIANT::default(); unsafe { let a = &mut *x.Anonymous.Anonymous; a.vt = VT_BOOL; a.Anonymous.boolVal = windows::Win32::Foundation::VARIANT_BOOL(if v { -1 } else { 0 }); } x }

/// O Media Foundation é iniciado uma única vez por processo (a adaptação recria codificadores com frequência).
fn mf_startup() -> Result<()> {
    static ONCE: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    if *ONCE.get_or_init(|| unsafe { MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET).is_ok() }) { Ok(()) } else { Err(anyhow!("Media Foundation indisponível")) }
}

pub struct MfEnc {
    /// o objeto de ativação que criou o MFT: precisa de ShutdownObject no fim, senão o MFT de hardware deixa handles para trás
    act: Option<IMFActivate>,
    tr: IMFTransform,
    events: Option<IMFMediaEventGenerator>,
    codec: Option<ICodecAPI>,
    pub w: usize,
    pub h: usize,
    pub bps: u32,
    pub hardware: bool,
    pub name: String,
    fps: u32,
    need_input: u32,
    have_output: u32,
    provides_samples: bool,
    out_size: u32,
    n: i64,
    params: Vec<u8>, // SPS+PPS (Annex-B) vistos por último
}

unsafe impl Send for MfEnc {}

fn pack(a: u32, b: u32) -> u64 { ((a as u64) << 32) | b as u64 }

unsafe fn friendly(act: &IMFActivate) -> String {
    let mut p = windows::core::PWSTR::null();
    let mut len = 0u32;
    if act.GetAllocatedString(&MFT_FRIENDLY_NAME_Attribute, &mut p, &mut len).is_ok() && !p.is_null() {
        let s = p.to_string().unwrap_or_default();
        CoTaskMemFree(Some(p.0 as *const _));
        return s;
    }
    String::new()
}

impl MfEnc {
    /// `allow_hw = false` força o codificador de software.
    pub fn new(w: usize, h: usize, fps: u32, bps: u32, allow_hw: bool) -> Result<Self> {
        unsafe {
            let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
            mf_startup()?;
            let out_info = MFT_REGISTER_TYPE_INFO { guidMajorType: MFMediaType_Video, guidSubtype: MFVideoFormat_H264 };
            let mut tries: Vec<(IMFActivate, bool)> = vec![];
            for (flags, hw) in [(MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, true), (MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, false)] {
                if hw && !allow_hw { continue; }
                let mut arr: *mut Option<IMFActivate> = std::ptr::null_mut();
                let mut count = 0u32;
                if MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, None, Some(&out_info), &mut arr, &mut count).is_ok() && !arr.is_null() {
                    for i in 0..count as usize { if let Some(a) = (*arr.add(i)).take() { tries.push((a, hw)); } }
                    CoTaskMemFree(Some(arr as *const _));
                }
            }
            let mut last = anyhow!("nenhum codificador H264 do Media Foundation");
            for (act, hw) in tries {
                let name = friendly(&act);
                match Self::open(&act, w, h, fps, bps, hw, name.clone()) { Ok(e) => return Ok(e), Err(e) => { last = anyhow!("{name}: {e}"); let _ = act.ShutdownObject(); } }
            }
            Err(last)
        }
    }

    unsafe fn open(act: &IMFActivate, w: usize, h: usize, fps: u32, bps: u32, hw: bool, name: String) -> Result<Self> {
        let tr: IMFTransform = act.ActivateObject()?;
        let attrs = tr.GetAttributes().ok();
        let mut is_async = false;
        if let Some(a) = &attrs {
            is_async = a.GetUINT32(&MF_TRANSFORM_ASYNC).unwrap_or(0) == 1;
            if is_async { a.SetUINT32(&MF_TRANSFORM_ASYNC_UNLOCK, 1)?; }
            let _ = a.SetUINT32(&MF_LOW_LATENCY, 1);
        }
        let codec = tr.cast::<ICodecAPI>().ok();
        let set = |g: &GUID, v: u32| { if let Some(c) = &codec { let _ = c.SetValue(g, &var_u32(v)); } };
        set(&CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR.0 as u32);
        set(&CODECAPI_AVEncCommonMeanBitRate, bps);
        // balde furado (VBV) de 1 s: sem isso um quadro-chave pode sair com várias vezes o bitrate de um segundo e virar rajada
        set(&CODECAPI_AVEncCommonBufferSize, bps / 8);
        set(&CODECAPI_AVEncMPVGOPSize, fps * 30);   // quadro-chave só a pedido (PLI/FIR); o GOP longo é só rede de segurança
        set(&CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        if let Some(c) = &codec { let _ = c.SetValue(&CODECAPI_AVLowLatencyMode, &var_bool(true)); let _ = c.SetValue(&CODECAPI_AVEncCommonRealTime, &var_bool(true)); }

        let mut ok = false;
        for profile in [eAVEncH264VProfile_ConstrainedBase.0, eAVEncH264VProfile_Base.0, eAVEncH264VProfile_Main.0, eAVEncH264VProfile_High.0] {
            let ot = MFCreateMediaType()?;
            ot.SetGUID(&MF_MT_MAJOR_TYPE, &MFMediaType_Video)?;
            ot.SetGUID(&MF_MT_SUBTYPE, &MFVideoFormat_H264)?;
            ot.SetUINT32(&MF_MT_AVG_BITRATE, bps)?;
            ot.SetUINT64(&MF_MT_FRAME_SIZE, pack(w as u32, h as u32))?;
            ot.SetUINT64(&MF_MT_FRAME_RATE, pack(fps, 1))?;
            ot.SetUINT64(&MF_MT_PIXEL_ASPECT_RATIO, pack(1, 1))?;
            ot.SetUINT32(&MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive.0 as u32)?;
            ot.SetUINT32(&MF_MT_MPEG2_PROFILE, profile as u32)?;
            if tr.SetOutputType(0, &ot, 0).is_ok() { ok = true; break; }
        }
        if !ok { return Err(anyhow!("tipo de saída recusado")); }
        let it = MFCreateMediaType()?;
        it.SetGUID(&MF_MT_MAJOR_TYPE, &MFMediaType_Video)?;
        it.SetGUID(&MF_MT_SUBTYPE, &MFVideoFormat_NV12)?;
        it.SetUINT64(&MF_MT_FRAME_SIZE, pack(w as u32, h as u32))?;
        it.SetUINT64(&MF_MT_FRAME_RATE, pack(fps, 1))?;
        it.SetUINT64(&MF_MT_PIXEL_ASPECT_RATIO, pack(1, 1))?;
        it.SetUINT32(&MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive.0 as u32)?;
        tr.SetInputType(0, &it, 0)?;
        let info = tr.GetOutputStreamInfo(0)?;
        let provides = info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES.0 as u32 | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES.0 as u32) != 0;
        let events = if is_async { Some(tr.cast::<IMFMediaEventGenerator>()?) } else { None };
        tr.ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0).ok();
        tr.ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)?;
        tr.ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)?;
        Ok(MfEnc { act: Some(act.clone()), tr, events, codec, w, h, bps, hardware: hw, name, fps, need_input: 0, have_output: 0, provides_samples: provides, out_size: info.cbSize.max((w * h * 2) as u32), n: 0, params: vec![] })
    }

    /// Bitrate médio ao vivo (ICodecAPI). Devolve false se o codificador recusar.
    pub fn set_bitrate(&mut self, bps: u32) -> bool {
        let Some(c) = &self.codec else { return false };
        let ok = unsafe { let _ = c.SetValue(&CODECAPI_AVEncCommonBufferSize, &var_u32(bps / 8)); c.SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var_u32(bps)).is_ok() };
        if ok { self.bps = bps; }
        ok
    }
    pub fn set_fps(&mut self, fps: u32) { self.fps = fps.max(1); }

    /// Consome eventos do MFT assíncrono. `wait` = bloqueia até chegar um.
    unsafe fn pump(&mut self, wait: bool) -> bool {
        let Some(ev) = &self.events else { return false };
        match ev.GetEvent(if wait { MEDIA_EVENT_GENERATOR_GET_EVENT_FLAGS(0) } else { MF_EVENT_FLAG_NO_WAIT }) {
            Ok(e) => {
                let t = e.GetType().unwrap_or(0);
                if t == METransformNeedInput.0 as u32 { self.need_input += 1; } else if t == METransformHaveOutput.0 as u32 { self.have_output += 1; }
                true
            }
            Err(_) => false,
        }
    }

    unsafe fn take_output(&mut self) -> Result<Option<Vec<u8>>> {
        let mut sample: Option<IMFSample> = None;
        if !self.provides_samples {
            let s = MFCreateSample()?;
            let b = MFCreateMemoryBuffer(self.out_size)?;
            s.AddBuffer(&b)?;
            sample = Some(s);
        }
        let mut bufs = [MFT_OUTPUT_DATA_BUFFER { dwStreamID: 0, pSample: std::mem::ManuallyDrop::new(sample), dwStatus: 0, pEvents: std::mem::ManuallyDrop::new(None) }];
        let mut status = 0u32;
        let r = self.tr.ProcessOutput(0, &mut bufs, &mut status);
        let s = std::mem::ManuallyDrop::take(&mut bufs[0].pSample);
        let _ = std::mem::ManuallyDrop::take(&mut bufs[0].pEvents);
        match r {
            Ok(()) => {
                let Some(s) = s else { return Ok(None) };
                let b = s.ConvertToContiguousBuffer()?;
                let (mut p, mut len) = (std::ptr::null_mut(), 0u32);
                b.Lock(&mut p, None, Some(&mut len))?;
                let v = std::slice::from_raw_parts(p, len as usize).to_vec();
                b.Unlock()?;
                Ok(Some(v))
            }
            Err(e) if e.code() == MF_E_TRANSFORM_NEED_MORE_INPUT => Ok(None),
            Err(e) if e.code() == MF_E_TRANSFORM_STREAM_CHANGE => {
                if let Ok(t) = self.tr.GetOutputAvailableType(0, 0) { let _ = self.tr.SetOutputType(0, &t, 0); }
                Ok(None)
            }
            Err(e) => Err(anyhow!("ProcessOutput: {e}")),
        }
    }

    /// Codifica um quadro NV12 (w*h*3/2 bytes). Devolve as unidades de acesso prontas, em Annex-B, uma por quadro (zero, uma ou mais).
    pub fn encode(&mut self, nv12: &[u8], force_idr: bool) -> Result<Vec<Vec<u8>>> {
        unsafe {
            if force_idr { if let Some(c) = &self.codec { let _ = c.SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &var_u32(1)); } }
            let len = (self.w * self.h * 3 / 2) as u32;
            let b = MFCreateMemoryBuffer(len)?;
            let (mut p, mut _max) = (std::ptr::null_mut(), 0u32);
            b.Lock(&mut p, Some(&mut _max), None)?;
            std::ptr::copy_nonoverlapping(nv12.as_ptr(), p, len as usize);
            b.Unlock()?;
            b.SetCurrentLength(len)?;
            let s = MFCreateSample()?;
            s.AddBuffer(&b)?;
            let dur = 10_000_000i64 / self.fps.max(1) as i64;
            s.SetSampleTime(self.n)?;
            s.SetSampleDuration(dur)?;
            self.n += dur;   // relógio acumulado: o FPS pode mudar no meio
            let mut out: Vec<Vec<u8>> = vec![];
            if self.events.is_some() {
                let t0 = std::time::Instant::now();
                while self.need_input == 0 { if !self.pump(false) { if t0.elapsed().as_millis() > 500 { return Err(anyhow!("codificador não pediu entrada")); } std::thread::sleep(std::time::Duration::from_micros(300)); } }
                self.need_input -= 1;
                self.tr.ProcessInput(0, &s, 0)?;
                let t0 = std::time::Instant::now();
                while self.have_output == 0 && t0.elapsed().as_millis() < 250 { if !self.pump(false) { std::thread::sleep(std::time::Duration::from_micros(300)); } }
                while self.have_output > 0 { self.have_output -= 1; if let Some(v) = self.take_output()? { out.push(v); } }
            } else {
                self.tr.ProcessInput(0, &s, 0)?;
                while let Some(v) = self.take_output()? { out.push(v); }
            }
            Ok(out.into_iter().map(|a| self.with_params(a)).filter(|a| !a.is_empty()).collect())
        }
    }

    /// Guarda SPS/PPS e os recoloca na frente de quadros-chave que vierem sem eles (alguns codificadores só mandam uma vez).
    fn with_params(&mut self, au: Vec<u8>) -> Vec<u8> {
        if au.is_empty() { return au; }
        let (mut has_sps, mut has_idr, mut params) = (false, false, vec![]);
        for (ty, s, e) in nals(&au) {
            match ty { 7 | 8 => { if ty == 7 { has_sps = true; } params.extend_from_slice(&[0, 0, 0, 1]); params.extend_from_slice(&au[s..e]); } 5 => has_idr = true, _ => {} }
        }
        if has_sps { self.params = params; return au; }
        if has_idr && !self.params.is_empty() { let mut v = self.params.clone(); v.extend_from_slice(&au); return v; }
        au
    }
}

impl Drop for MfEnc {
    fn drop(&mut self) {
        unsafe {
            let _ = self.tr.ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            let _ = self.tr.ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            let _ = self.tr.ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            self.events = None; self.codec = None;
            if let Ok(s) = self.tr.cast::<IMFShutdown>() { let _ = s.Shutdown(); }
            if let Some(a) = self.act.take() { let _ = a.ShutdownObject(); }
        }
    }
}

/// NALs de um fluxo Annex-B: (tipo, início, fim) do corpo de cada uma.
pub fn nals(d: &[u8]) -> Vec<(u8, usize, usize)> {
    let mut starts = vec![];
    let mut i = 0;
    while i + 3 <= d.len() {
        if d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 { starts.push((i, i + 3)); i += 3; } else { i += 1; }
    }
    let mut out = vec![];
    for (k, (_, body)) in starts.iter().enumerate() {
        let mut end = if k + 1 < starts.len() { starts[k + 1].0 } else { d.len() };
        while end > *body && d[end - 1] == 0 && k + 1 < starts.len() { end -= 1; }
        if *body < end { out.push((d[*body] & 0x1f, *body, end)); }
    }
    out
}

pub fn i420_to_nv12(y: &[u8], u: &[u8], v: &[u8], out: &mut Vec<u8>) {
    out.clear();
    out.reserve(y.len() + u.len() * 2);
    out.extend_from_slice(y);
    for (a, b) in u.iter().zip(v) { out.push(*a); out.push(*b); }
}

// ---------------------------------------------------------------------------------------------------------------------
// Decodificador H264 do Media Foundation (MFT síncrono da Microsoft; aceita Baseline/Main/High de qualquer codificador)
// ---------------------------------------------------------------------------------------------------------------------
pub struct MfDec {
    tr: IMFTransform,
    out_size: u32,
    /// largura/altura visíveis, altura armazenada (múltipla de 16) e passo de linha da saída NV12
    pub w: usize,
    pub h: usize,
    stored_h: usize,
    stride: usize,
    n: i64,
}
unsafe impl Send for MfDec {}

impl MfDec {
    pub fn new() -> Result<Self> {
        unsafe {
            let _ = CoInitializeEx(None, COINIT_MULTITHREADED);
            mf_startup()?;
            let tr: IMFTransform = windows::Win32::System::Com::CoCreateInstance(&CLSID_MSH264DecoderMFT, None, windows::Win32::System::Com::CLSCTX_INPROC_SERVER)?;
            if let Ok(a) = tr.GetAttributes() { let _ = a.SetUINT32(&MF_LOW_LATENCY, 1); }
            if let Ok(c) = tr.cast::<ICodecAPI>() { let _ = c.SetValue(&CODECAPI_AVLowLatencyMode, &var_bool(true)); }
            let it = MFCreateMediaType()?;
            it.SetGUID(&MF_MT_MAJOR_TYPE, &MFMediaType_Video)?;
            it.SetGUID(&MF_MT_SUBTYPE, &MFVideoFormat_H264)?;
            it.SetUINT32(&MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive.0 as u32)?;
            tr.SetInputType(0, &it, 0)?;
            let mut d = MfDec { tr, out_size: 0, w: 0, h: 0, stored_h: 0, stride: 0, n: 0 };
            d.pick_output()?;
            d.tr.ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)?;
            d.tr.ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)?;
            Ok(d)
        }
    }

    unsafe fn pick_output(&mut self) -> Result<()> {
        let mut i = 0;
        loop {
            let t = self.tr.GetOutputAvailableType(0, i).map_err(|e| anyhow!("decodificador sem saída NV12: {e}"))?;
            if t.GetGUID(&MF_MT_SUBTYPE)? == MFVideoFormat_NV12 {
                self.tr.SetOutputType(0, &t, 0)?;
                let fs = t.GetUINT64(&MF_MT_FRAME_SIZE).unwrap_or(0);
                let (fw, fh) = ((fs >> 32) as usize, (fs & 0xffff_ffff) as usize);
                self.stored_h = fh;
                self.stride = t.GetUINT32(&MF_MT_DEFAULT_STRIDE).map(|s| (s as i32).unsigned_abs() as usize).unwrap_or(fw);
                if self.stride == 0 { self.stride = fw; }
                (self.w, self.h) = (fw, fh);
                let mut area = MFVideoArea::default();
                if t.GetBlob(&MF_MT_MINIMUM_DISPLAY_APERTURE, std::slice::from_raw_parts_mut(&mut area as *mut _ as *mut u8, std::mem::size_of::<MFVideoArea>()), None).is_ok() && area.Area.cx > 0 {
                    (self.w, self.h) = (area.Area.cx as usize, area.Area.cy as usize);
                }
                self.out_size = self.tr.GetOutputStreamInfo(0).map(|i| i.cbSize).unwrap_or(0).max((self.stride * self.stored_h * 3 / 2) as u32).max(1024);
                return Ok(());
            }
            i += 1;
        }
    }

    /// Decodifica uma unidade de acesso Annex-B. Chama `f(largura, altura, y, uv, passo)` para cada quadro que sair (NV12).
    pub fn decode(&mut self, au: &[u8], mut f: impl FnMut(usize, usize, &[u8], &[u8], usize)) -> Result<usize> {
        unsafe {
            let b = MFCreateMemoryBuffer(au.len() as u32)?;
            let mut p = std::ptr::null_mut();
            b.Lock(&mut p, None, None)?;
            std::ptr::copy_nonoverlapping(au.as_ptr(), p, au.len());
            b.Unlock()?;
            b.SetCurrentLength(au.len() as u32)?;
            let s = MFCreateSample()?;
            s.AddBuffer(&b)?;
            s.SetSampleTime(self.n * 333_333)?;
            s.SetSampleDuration(333_333)?;
            self.n += 1;
            self.tr.ProcessInput(0, &s, 0).map_err(|e| anyhow!("ProcessInput: {e}"))?;
            let mut got = 0;
            for _ in 0..8 {
                let os = MFCreateSample()?;
                let ob = MFCreateMemoryBuffer(self.out_size)?;
                os.AddBuffer(&ob)?;
                let mut bufs = [MFT_OUTPUT_DATA_BUFFER { dwStreamID: 0, pSample: std::mem::ManuallyDrop::new(Some(os)), dwStatus: 0, pEvents: std::mem::ManuallyDrop::new(None) }];
                let mut status = 0u32;
                let r = self.tr.ProcessOutput(0, &mut bufs, &mut status);
                let smp = std::mem::ManuallyDrop::take(&mut bufs[0].pSample);
                let _ = std::mem::ManuallyDrop::take(&mut bufs[0].pEvents);
                match r {
                    Ok(()) => {
                        let Some(smp) = smp else { break };
                        let cb = smp.ConvertToContiguousBuffer()?;
                        let (mut p, mut len) = (std::ptr::null_mut(), 0u32);
                        cb.Lock(&mut p, None, Some(&mut len))?;
                        let d = std::slice::from_raw_parts(p, len as usize);
                        let ysz = self.stride * self.stored_h;
                        if d.len() >= ysz + self.stride * self.h / 2 && self.w > 0 { f(self.w, self.h, &d[..ysz], &d[ysz..], self.stride); got += 1; }
                        cb.Unlock()?;
                    }
                    Err(e) if e.code() == MF_E_TRANSFORM_NEED_MORE_INPUT => break,
                    Err(e) if e.code() == MF_E_TRANSFORM_STREAM_CHANGE => { self.pick_output()?; }
                    Err(e) => return Err(anyhow!("ProcessOutput: {e}")),
                }
            }
            Ok(got)
        }
    }
}
impl Drop for MfDec { fn drop(&mut self) { unsafe { let _ = self.tr.ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0); } } }
