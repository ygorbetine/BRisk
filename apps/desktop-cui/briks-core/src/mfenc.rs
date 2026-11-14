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
pub(crate) fn mf_startup() -> Result<()> {
    static ONCE: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    if *ONCE.get_or_init(|| unsafe { MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET).is_ok() }) { Ok(()) } else { Err(anyhow!("Media Foundation indisponível")) }
}

/// Falha do codificador, com o HRESULT para o diagnóstico e a classe para a recuperação (ver `share::EncSupervisor`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FaultKind {
    /// o MFT não pediu entrada / recusou o quadro / mudou de formato: quadro perdido, o codificador costuma voltar sozinho (ou com flush)
    Stalled,
    /// GPU removida/reiniciada (TDR), driver caiu, MFT de hardware não iniciou: precisa recriar o codificador
    DeviceLost,
    /// qualquer outro erro do MFT
    Other,
}
#[derive(Debug, Clone)]
pub struct EncFault { pub kind: FaultKind, pub hr: i32, pub msg: String }
impl std::fmt::Display for EncFault {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result { write!(f, "{} (HRESULT 0x{:08X})", self.msg, self.hr as u32) }
}
impl std::error::Error for EncFault {}
impl EncFault {
    pub fn new(kind: FaultKind, hr: i32, msg: impl Into<String>) -> Self { EncFault { kind, hr, msg: msg.into() } }
    pub fn from_hr(hr: i32, what: &str) -> Self { EncFault::new(classify_hr(hr), hr, what) }
    /// Classe de um erro qualquer do codificador (EncFault, erro do Windows ou texto).
    pub fn of(e: &anyhow::Error) -> EncFault {
        if let Some(f) = e.downcast_ref::<EncFault>() { return f.clone(); }
        if let Some(w) = e.downcast_ref::<windows::core::Error>() { return EncFault::from_hr(w.code().0, &w.message()); }
        EncFault::new(FaultKind::Other, 0, e.to_string())
    }
}
/// HRESULTs de GPU perdida (DXGI_ERROR_DEVICE_REMOVED/HUNG/RESET/DRIVER_INTERNAL_ERROR, MF_E_HW_MFT_FAILED_START_STREAMING) e de "sem entrada".
pub fn classify_hr(hr: i32) -> FaultKind {
    match hr as u32 {
        0x887A0005 | 0x887A0006 | 0x887A0007 | 0x887A0020 | 0xC00D3EA2 => FaultKind::DeviceLost,
        0xC00D36B5 | 0xC00D6D61 | 0xC00D6D72 => FaultKind::Stalled,   // MF_E_NOTACCEPTING, MF_E_TRANSFORM_STREAM_CHANGE, MF_E_TRANSFORM_NEED_MORE_INPUT fora de hora
        _ => FaultKind::Other,
    }
}

/// Teto de QP de partida: nitidez de texto no quadro-chave (IDR a ~35 dB; com 34 cai para ~30 e com 40 para ~24, medido). Conteúdo que
/// não cabe no bitrate com este teto (grão pesado, jogo caótico) faz o NVENC ignorar o bitrate — 67 Mbps com 2 a 6 pedidos, medido —, e aí
/// o `share::QpGovernor` sobe o teto.
pub const DEFAULT_MAX_QP: u32 = 28;

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

/// Tamanho do VBV em bits. Padrão: bps/8 (= 125 ms de bitrate). `BRIKS_ENC_VBV_MS` testa outros (diagnóstico).
fn vbv_bits(bps: u32) -> u32 {
    match std::env::var("BRIKS_ENC_VBV_MS").ok().and_then(|v| v.parse::<u64>().ok()) { Some(ms) => (bps as u64 * ms / 1000).min(u32::MAX as u64) as u32, None => bps / 8 }
}
/// Perfis H264 na ordem de tentativa; `BRIKS_ENC_PROFILE` = cbase | base | main | high põe o escolhido primeiro (diagnóstico).
fn profile_order() -> Vec<i32> {
    // High primeiro (medido no NVENC, 1080p de tela: +4,6 dB a 3,5 Mbps e +6,4 dB a 6 Mbps sobre o Constrained Baseline); cai para os mais simples se o MFT recusar
    let mut v = vec![eAVEncH264VProfile_High.0, eAVEncH264VProfile_Main.0, eAVEncH264VProfile_Base.0, eAVEncH264VProfile_ConstrainedBase.0];
    let first = match std::env::var("BRIKS_ENC_PROFILE").as_deref() { Ok("cbase") => Some(eAVEncH264VProfile_ConstrainedBase.0), Ok("base") => Some(eAVEncH264VProfile_Base.0), Ok("main") => Some(eAVEncH264VProfile_Main.0), Ok("high") => Some(eAVEncH264VProfile_High.0), _ => None };
    if let Some(f) = first { v.retain(|x| *x != f); v.insert(0, f); }
    v
}

impl MfEnc {
    /// `allow_hw = false` força o codificador de software.
    pub fn new(w: usize, h: usize, fps: u32, bps: u32, allow_hw: bool) -> Result<Self> { Self::new_qp(w, h, fps, bps, allow_hw, DEFAULT_MAX_QP) }
    /// Igual a `new`, com o teto de QP escolhido (ver `share::QpGovernor`). O NVENC aceita trocar o teto ao vivo mas NÃO obedece
    /// (medido: 67 Mbps antes e depois da troca), por isso o teto só muda recriando o codificador.
    pub fn new_qp(w: usize, h: usize, fps: u32, bps: u32, allow_hw: bool, max_qp: u32) -> Result<Self> {
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
                match Self::open(&act, w, h, fps, bps, hw, name.clone(), max_qp) { Ok(e) => return Ok(e), Err(e) => { last = anyhow!("{name}: {e}"); let _ = act.ShutdownObject(); } }
            }
            Err(last)
        }
    }

    unsafe fn open(act: &IMFActivate, w: usize, h: usize, fps: u32, bps: u32, hw: bool, name: String, max_qp: u32) -> Result<Self> {
        let tr: IMFTransform = act.ActivateObject()?;
        let attrs = tr.GetAttributes().ok();
        let mut is_async = false;
        if let Some(a) = &attrs {
            is_async = a.GetUINT32(&MF_TRANSFORM_ASYNC).unwrap_or(0) == 1;
            if is_async { a.SetUINT32(&MF_TRANSFORM_ASYNC_UNLOCK, 1)?; }
            let _ = a.SetUINT32(&MF_LOW_LATENCY, 1);
        }
        let codec = tr.cast::<ICodecAPI>().ok();
        let log = std::env::var("BRIKS_ENC_LOG").is_ok();
        let set = |nome: &str, g: &GUID, v: u32| { if let Some(c) = &codec { let r = c.SetValue(g, &var_u32(v)); if log { eprintln!("[mfenc] {nome} = {v}: {}", if r.is_ok() { "ok".to_string() } else { format!("RECUSADO {r:?}") }); } } };
        let envu = |k: &str| std::env::var(k).ok().and_then(|v| v.parse::<u32>().ok());
        // Modo de controle de taxa (BRIKS_ENC_RC = cbr | pvbr | vbr | quality; diagnóstico). Padrão: CBR.
        let rc = match std::env::var("BRIKS_ENC_RC").as_deref() { Ok("pvbr") => eAVEncCommonRateControlMode_PeakConstrainedVBR, Ok("vbr") => eAVEncCommonRateControlMode_UnconstrainedVBR, Ok("quality") => eAVEncCommonRateControlMode_Quality, _ => eAVEncCommonRateControlMode_CBR };
        set("RateControlMode", &CODECAPI_AVEncCommonRateControlMode, rc.0 as u32);
        set("MeanBitRate", &CODECAPI_AVEncCommonMeanBitRate, bps);
        if rc == eAVEncCommonRateControlMode_PeakConstrainedVBR { set("MaxBitRate", &CODECAPI_AVEncCommonMaxBitRate, (bps as f64 * envu("BRIKS_ENC_PEAK_PCT").unwrap_or(150) as f64 / 100.0) as u32); }
        // balde furado (VBV) curto: sem isso um quadro-chave pode sair com várias vezes o bitrate de um segundo e virar rajada
        set("BufferSize", &CODECAPI_AVEncCommonBufferSize, vbv_bits(bps));
        set("GOPSize", &CODECAPI_AVEncMPVGOPSize, fps * 30);   // quadro-chave só a pedido (PLI/FIR); o GOP longo é só rede de segurança
        set("BPictureCount", &CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        if let Some(q) = envu("BRIKS_ENC_QUALITY") { set("CommonQuality", &CODECAPI_AVEncCommonQuality, q); }
        if let Some(q) = envu("BRIKS_ENC_QVS") { set("QualityVsSpeed", &CODECAPI_AVEncCommonQualityVsSpeed, q); }
        // Teto de QP: sem ele o NVENC em CBR faz o quadro-chave (a cada PLI/novo espectador) sair com QP altíssimo (luma ~22 dB, texto ilegível)
        // seguido de P-quadros quase vazios que refinam devagar: é o "reload/zoom" que o usuário vê. Com 28 o IDR sai ~36 dB (medido).
        set("MaxQP", &CODECAPI_AVEncVideoMaxQP, envu("BRIKS_ENC_MAXQP").unwrap_or(max_qp.clamp(10, 51)));
        if let Some(q) = envu("BRIKS_ENC_MINQP") { set("MinQP", &CODECAPI_AVEncVideoMinQP, q); }
        // Intra refresh (diagnóstico, desligado por padrão): BRIKS_ENC_INTRA_REFRESH = 1 liga a renovação gradual; ver `video::qualidade::intra_refresh_recupera`
        if let Some(v) = envu("BRIKS_ENC_INTRA_REFRESH") { set("GradualIntraRefresh", &CODECAPI_AVEncVideoGradualIntraRefresh, v); }
        if let Some(c) = &codec {
            let ll = std::env::var("BRIKS_ENC_LOWLAT").map(|v| v != "0").unwrap_or(true);
            let r1 = c.SetValue(&CODECAPI_AVLowLatencyMode, &var_bool(ll)); let r2 = c.SetValue(&CODECAPI_AVEncCommonRealTime, &var_bool(ll));
            if log { eprintln!("[mfenc] LowLatency={ll}: {:?}/{:?} (perfil, tamanho {w}x{h}, {fps} fps, {bps} bps)", r1.is_ok(), r2.is_ok()); }
        }

        let mut ok = false;
        for profile in profile_order() {
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
        let ok = unsafe { let _ = c.SetValue(&CODECAPI_AVEncCommonBufferSize, &var_u32(vbv_bits(bps))); c.SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var_u32(bps)).is_ok() };
        if ok { self.bps = bps; }
        ok
    }
    pub fn set_fps(&mut self, fps: u32) { self.fps = fps.max(1); }

    /// Consome eventos do MFT assíncrono. `wait` = bloqueia até chegar um. Ok(false) = nenhum evento. MEError vira erro (recuperável por
    /// recriação); DrainComplete/Marker e os demais são consumidos para não ficarem na fila.
    unsafe fn pump(&mut self, wait: bool) -> Result<bool> {
        let Some(ev) = &self.events else { return Ok(false) };
        match ev.GetEvent(if wait { MEDIA_EVENT_GENERATOR_GET_EVENT_FLAGS(0) } else { MF_EVENT_FLAG_NO_WAIT }) {
            Ok(e) => {
                let t = e.GetType().unwrap_or(0);
                if t == METransformNeedInput.0 as u32 { self.need_input += 1; }
                else if t == METransformHaveOutput.0 as u32 { self.have_output += 1; }
                else if t == MEError.0 as u32 {
                    let hr = e.GetStatus().map(|h| h.0).unwrap_or(0x80004005u32 as i32);
                    return Err(anyhow::Error::new(EncFault::from_hr(hr, "evento MEError do codificador")));
                }
                Ok(true)
            }
            Err(_) => Ok(false),
        }
    }

    /// Ressincroniza o MFT assíncrono sem recriá-lo: descarta o que estava em voo e zera os contadores de eventos (depois de um
    /// flush ele volta a mandar METransformNeedInput). Usado quando o codificador não pede entrada a tempo.
    unsafe fn resync(&mut self) {
        let _ = self.tr.ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        self.need_input = 0; self.have_output = 0;
        let _ = self.tr.ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        while self.pump(false).unwrap_or(false) {}   // consome os eventos que o flush deixou
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
            Err(e) => Err(anyhow::Error::new(EncFault::from_hr(e.code().0, "ProcessOutput"))),
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
                let mut t0 = std::time::Instant::now();
                let mut resynced = false;
                while self.need_input == 0 {
                    if !self.pump(false)? {
                        if t0.elapsed().as_millis() > 500 {
                            // GPU disputada, driver ocupado ou evento perdido depois de um flush/mudança de formato: uma ressincronização
                            // (flush + novo START_OF_STREAM) e mais uma espera; se ainda assim nada, o quadro é descartado (recuperável)
                            if resynced { return Err(anyhow::Error::new(EncFault::new(FaultKind::Stalled, 0xC00D36B5u32 as i32, "o codificador não pediu entrada"))); }
                            self.resync(); resynced = true; t0 = std::time::Instant::now();
                        }
                        std::thread::sleep(std::time::Duration::from_micros(300));
                    }
                }
                self.need_input -= 1;
                self.tr.ProcessInput(0, &s, 0).map_err(|e| anyhow::Error::new(EncFault::from_hr(e.code().0, "ProcessInput")))?;
                let t0 = std::time::Instant::now();
                while self.have_output == 0 && t0.elapsed().as_millis() < 250 { if !self.pump(false)? { std::thread::sleep(std::time::Duration::from_micros(300)); } }
                while self.have_output > 0 { self.have_output -= 1; if let Some(v) = self.take_output()? { out.push(v); } }
            } else {
                self.tr.ProcessInput(0, &s, 0).map_err(|e| anyhow::Error::new(EncFault::from_hr(e.code().0, "ProcessInput")))?;
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

/// I420 → NV12 (Y igual; U e V intercalados). Roda uma vez por quadro enviado, então importa: a versão antiga fazia `push` byte a
/// byte (1,4 milhão de vezes num quadro 2580x1080); esta escreve em pares, que o compilador vetoriza.
pub fn i420_to_nv12(y: &[u8], u: &[u8], v: &[u8], out: &mut Vec<u8>) {
    let n = y.len() + u.len() * 2;
    out.clear();
    out.reserve(n);
    out.extend_from_slice(y);
    out.resize(n, 0);
    for ((o, a), b) in out[y.len()..].chunks_exact_mut(2).zip(u).zip(v) { o[0] = *a; o[1] = *b; }
}

/// Medição (ignorado): o que o codificador H264 de hardware desta máquina expõe pelo ICodecAPI além do que já usamos — intra refresh,
/// quadros de referência de longo prazo, camadas temporais, B-quadros. `IsSupported` diz se a propriedade existe; `SetValue` se aceita.
#[cfg(test)]
mod sonda {
    use super::*;
    #[test]
    #[ignore]
    fn o_que_o_codificador_de_hardware_expoe() {
        let enc = MfEnc::new(1280, 720, 30, 3_000_000, true).expect("codificador");
        let Some(c) = &enc.codec else { eprintln!("[sonda] {} sem ICodecAPI", enc.name); return };
        let props: [(&str, GUID, u32); 9] = [
            ("AVEncVideoGradualIntraRefresh (intra refresh)", CODECAPI_AVEncVideoGradualIntraRefresh, 1),
            ("AVEncVideoLTRBufferControl (referência de longo prazo)", CODECAPI_AVEncVideoLTRBufferControl, 1),
            ("AVEncVideoTemporalLayerCount (camadas temporais / SVC)", CODECAPI_AVEncVideoTemporalLayerCount, 2),
            ("AVEncMPVDefaultBPictureCount (B-quadros)", CODECAPI_AVEncMPVDefaultBPictureCount, 2),
            ("AVEncVideoMaxQP", CODECAPI_AVEncVideoMaxQP, 30),
            ("AVEncVideoMaxNumRefFrame", CODECAPI_AVEncVideoMaxNumRefFrame, 2),
            ("AVEncCommonQualityVsSpeed", CODECAPI_AVEncCommonQualityVsSpeed, 50),
            ("AVEncVideoEncodeQP", CODECAPI_AVEncVideoEncodeQP, 26),
            ("AVEncH264CABACEnable", CODECAPI_AVEncH264CABACEnable, 1),
        ];
        for (nome, g, v) in props {
            let sup = unsafe { c.IsSupported(&g).is_ok() };
            let set = unsafe { c.SetValue(&g, &var_u32(v)) };
            eprintln!("[sonda] {} ({}): suportada = {sup}, SetValue({v}) = {}", nome, if enc.hardware { "hardware" } else { "software" }, if set.is_ok() { "ok".to_string() } else { format!("{:?}", set.err().map(|e| e.code())) });
        }
        eprintln!("[sonda] codificador: {}", enc.name);
    }
}

#[cfg(test)]
mod nv12_tests {
    use super::i420_to_nv12;

    fn antigo(y: &[u8], u: &[u8], v: &[u8], out: &mut Vec<u8>) {
        out.clear();
        out.reserve(y.len() + u.len() * 2);
        out.extend_from_slice(y);
        for (a, b) in u.iter().zip(v) { out.push(*a); out.push(*b); }
    }
    fn planos(w: usize, h: usize) -> (Vec<u8>, Vec<u8>, Vec<u8>) {
        ((0..w * h).map(|i| (i * 7) as u8).collect(), (0..w * h / 4).map(|i| (i * 3 + 1) as u8).collect(), (0..w * h / 4).map(|i| (i * 5 + 2) as u8).collect())
    }

    /// A versão nova produz exatamente os mesmos bytes da antiga, inclusive reaproveitando um buffer maior ou menor.
    #[test]
    fn interleaving_matches_the_old_byte_by_byte_version() {
        let mut out = vec![9u8; 5_000_000];
        for (w, h) in [(2usize, 2usize), (640, 360), (1920, 1080), (322, 182)] {
            let (y, u, v) = planos(w, h);
            let mut want = vec![];
            antigo(&y, &u, &v, &mut want);
            i420_to_nv12(&y, &u, &v, &mut out);
            assert_eq!(out, want, "{w}x{h}");
        }
    }

    /// Medição (ignorado): ms por quadro em 2580x1080 e 1920x1080, versão antiga x nova.
    #[test]
    #[ignore]
    fn bench_i420_to_nv12() {
        for (w, h) in [(2580usize, 1080usize), (1920, 1080)] {
            let (y, u, v) = planos(w, h);
            let mut out = vec![];
            let mut t = [0.0f64; 2];
            for (k, f) in [antigo as fn(&[u8], &[u8], &[u8], &mut Vec<u8>), i420_to_nv12].iter().enumerate() {
                for _ in 0..20 { f(&y, &u, &v, &mut out); }
                let t0 = std::time::Instant::now();
                for _ in 0..300 { f(&y, &u, &v, &mut out); std::hint::black_box(&out); }
                t[k] = t0.elapsed().as_secs_f64() * 1000.0 / 300.0;
            }
            eprintln!("[i420_to_nv12 {w}x{h}] antiga {:.2} ms, nova {:.2} ms por quadro", t[0], t[1]);
        }
    }
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
