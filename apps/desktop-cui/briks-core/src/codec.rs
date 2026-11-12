//! Codecs de vídeo da transmissão e o que este binário sabe usar.

/// Codec de vídeo da transmissão. H264 é o padrão e a reserva.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum Codec { #[default] H264, Vp8, Vp9 }
impl Codec {
    pub fn mime(self) -> &'static str { match self { Codec::H264 => "video/H264", Codec::Vp8 => "video/VP8", Codec::Vp9 => "video/VP9" } }
    pub fn name(self) -> &'static str { match self { Codec::H264 => "h264", Codec::Vp8 => "vp8", Codec::Vp9 => "vp9" } }
    pub fn from_name(s: &str) -> Option<Codec> { match s.to_ascii_lowercase().as_str() { "h264" => Some(Codec::H264), "vp8" => Some(Codec::Vp8), "vp9" => Some(Codec::Vp9), _ => None } }
    pub fn from_mime(m: &str) -> Codec { if m.eq_ignore_ascii_case("video/VP8") { Codec::Vp8 } else if m.eq_ignore_ascii_case("video/VP9") { Codec::Vp9 } else { Codec::H264 } }
    /// Este binário sabe codificar/decodificar o codec? (H264 sempre; VP8/VP9 só com a feature `vpx`)
    pub fn available(self) -> bool { self == Codec::H264 || cfg!(feature = "vpx") }
}
/// Codecs que este binário decodifica, na ordem de preferência para anunciar ao par.
pub fn local_caps() -> Vec<&'static str> { if cfg!(feature = "vpx") { vec!["h264", "vp9", "vp8"] } else { vec!["h264"] } }


// ---------------------------------------------------------------------------------------------------------------------
// RTP por codec: empacotador (emissor) e leitura do descritor (receptor). H264 continua no código original de share.rs.
// ---------------------------------------------------------------------------------------------------------------------
use bytes::{Bytes, BytesMut, BufMut};
use webrtc::rtp::codecs::vp8::{Vp8Packet, Vp8Payloader};
use webrtc::rtp::codecs::vp9::Vp9Packet;
use webrtc::rtp::packetizer::{Depacketizer, Payloader};

/// Empacotador VP9 próprio: o do webrtc-rs sempre marca "modo flexível" e nunca o bit P (quadro predito), então nenhum SFU/receptor
/// distinguiria quadro-chave de quadro predito pelo descritor. Aqui: modo não flexível, ID de imagem de 15 bits, B/E por fragmento e
/// P = 0 só no quadro-chave (detectado no cabeçalho do quadro VP9).
#[derive(Clone, Default, Debug)]
pub struct Vp9Pay { pic: u16 }
impl Payloader for Vp9Pay {
    fn payload(&mut self, mtu: usize, payload: &Bytes) -> Result<Vec<Bytes>, webrtc::rtp::Error> {
        const HDR: usize = 3;
        if payload.is_empty() || mtu <= HDR { return Ok(vec![]); }
        let max = mtu - HDR;
        let key = vp9_is_key(payload);
        let (mut out, mut at) = (vec![], 0usize);
        while at < payload.len() {
            let n = max.min(payload.len() - at);
            let mut b = BytesMut::with_capacity(HDR + n);
            let mut d0 = 0x80u8;                  // I = 1 (ID de imagem presente), F = 0
            if !key { d0 |= 0x40; }               // P: predito de outro quadro
            if at == 0 { d0 |= 0x08; }            // B
            if at + n == payload.len() { d0 |= 0x04; }   // E
            b.put_u8(d0);
            b.put_u8(0x80 | (self.pic >> 8) as u8);
            b.put_u8((self.pic & 0xFF) as u8);
            b.put_slice(&payload[at..at + n]);
            out.push(b.freeze());
            at += n;
        }
        self.pic = (self.pic + 1) & 0x7FFF;
        Ok(out)
    }
    fn clone_to(&self) -> Box<dyn Payloader + Send + Sync> { Box::new(self.clone()) }
}

/// Quadro VP9 completo é um quadro-chave? (cabeçalho não comprimido: marcador, perfil, show_existing_frame, frame_type)
pub fn vp9_is_key(f: &[u8]) -> bool {
    let Some(&b0) = f.first() else { return false };
    if b0 >> 6 != 2 { return false; }
    let profile = ((b0 >> 5) & 1) | (((b0 >> 4) & 1) << 1);
    let skip = if profile == 3 { 1 } else { 0 };
    let bit = |i: usize| (f.get(i / 8).copied().unwrap_or(0) >> (7 - i % 8)) & 1;
    let show_existing = bit(4 + skip);
    let frame_type = bit(5 + skip);
    show_existing == 0 && frame_type == 0
}
/// Quadro VP8 completo é um quadro-chave? (bit 0 do primeiro byte = 0)
pub fn vp8_is_key(f: &[u8]) -> bool { f.first().map(|b| b & 1 == 0).unwrap_or(false) }

pub fn payloader(c: Codec) -> Option<Box<dyn Payloader + Send + Sync>> {
    match c {
        Codec::H264 => None,
        Codec::Vp8 => { let mut p = Vp8Payloader::default(); p.enable_picture_id = true; Some(Box::new(p)) },
        Codec::Vp9 => Some(Box::new(Vp9Pay::default())),
    }
}
/// O quadro (já inteiro) é quadro-chave?
pub fn is_key_frame(c: Codec, f: &[u8]) -> bool { match c { Codec::Vp8 => vp8_is_key(f), Codec::Vp9 => vp9_is_key(f), Codec::H264 => false } }

/// Primeiro pacote RTP de um quadro? E, nesse caso, o quadro é chave? (VP8/VP9; H264 fica em share.rs)
pub fn rtp_start(c: Codec, payload: &Bytes) -> (bool, bool) {
    match c {
        Codec::Vp8 => { let mut d = Vp8Packet::default(); match d.depacketize(payload) { Ok(b) => { let st = d.s == 1 && d.pid == 0; (st, st && vp8_is_key(&b)) } Err(_) => (false, false) } }
        Codec::Vp9 => { let mut d = Vp9Packet::default(); match d.depacketize(payload) { Ok(b) => (d.b, d.b && !d.p && vp9_is_key(&b)), Err(_) => (false, false) } }
        Codec::H264 => (false, false),
    }
}
/// Monta o quadro a partir dos pacotes (já em ordem). None = pacote inválido.
pub fn assemble<'a>(c: Codec, pkts: impl Iterator<Item = &'a Bytes>) -> Option<Vec<u8>> {
    let mut out = Vec::new();
    match c {
        Codec::Vp8 => { let mut d = Vp8Packet::default(); for p in pkts { out.extend_from_slice(&d.depacketize(p).ok()?); } }
        Codec::Vp9 => { let mut d = Vp9Packet::default(); for p in pkts { out.extend_from_slice(&d.depacketize(p).ok()?); } }
        Codec::H264 => return None,
    }
    Some(out)
}
