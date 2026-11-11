//! Anexos P2P pelo DataChannel: oferta assinada (nome, tamanho, SHA-256), pedaços em ordem, verificação de integridade ao fim.
//! Mensagens (JSON, cada uma <= 64 KiB):
//!   file.offer  {id,name,size,mime,sha256,fromPeerId,timestamp,signature}
//!   file.chunk  {id,seq,data(base64)}
//!   file.done   {id}
//!   file.cancel {id}
//! O receptor grava numa pasta de quarentena (`recebidos`) e nunca abre nada; abrir depende de confirmação na UI.
use crate::identity::{self, Identity, PublicJwk};
use anyhow::{anyhow, Result};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::fs::File;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};

pub const MAX_FILE_BYTES: u64 = 25 * 1024 * 1024;
pub const CHUNK_BYTES: usize = 32 * 1024;
pub const MAX_INCOMING_PER_PEER: usize = 3;
pub const OFFER_SKEW_MS: i64 = 5 * 60 * 1000;

/// Nome seguro para gravar em disco: sem pastas, sem caracteres reservados do Windows, sem nomes de dispositivo, até 100 caracteres.
pub fn sanitize_name(raw: &str) -> String {
    let base = raw.rsplit(['/', '\\']).next().unwrap_or("");
    let mut n: String = base.chars().filter(|c| !c.is_control() && !"<>:\"|?*".contains(*c)).collect();
    n = n.trim().trim_matches('.').to_string();
    let stem = n.split('.').next().unwrap_or("").to_ascii_uppercase();
    if ["CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "LPT1", "LPT2", "LPT3"].contains(&stem.as_str()) { n = format!("_{n}"); }
    if n.is_empty() { n = "arquivo".into(); }
    if n.chars().count() > 100 {
        let ext: String = n.rsplit_once('.').map(|(_, e)| e.chars().take(10).collect::<String>()).unwrap_or_default();
        let head: String = n.chars().take(80).collect();
        n = if ext.is_empty() { head } else { format!("{head}.{ext}") };
    }
    n
}

pub fn mime_for(name: &str) -> &'static str {
    match name.rsplit_once('.').map(|(_, e)| e.to_ascii_lowercase()).as_deref() {
        Some("png") => "image/png", Some("jpg") | Some("jpeg") => "image/jpeg", Some("gif") => "image/gif", Some("webp") => "image/webp",
        Some("pdf") => "application/pdf", Some("txt") => "text/plain", Some("zip") => "application/zip", Some("mp4") => "video/mp4", Some("mp3") => "audio/mpeg",
        _ => "application/octet-stream",
    }
}
pub fn is_thumbnailable(mime: &str) -> bool { mime == "image/png" || mime == "image/jpeg" }

#[derive(Clone, Debug)]
pub struct Offer { pub id: String, pub name: String, pub size: u64, pub mime: String, pub sha256: String, pub from_peer: String, pub timestamp: i64, pub signature: String }

fn canonical(o: &Offer) -> String {
    json!({ "version": 1, "type": "file.offer", "id": o.id, "name": o.name, "size": o.size, "mime": o.mime, "sha256": o.sha256, "fromPeerId": o.from_peer, "timestamp": o.timestamp }).to_string()
}
fn valid_id(s: &str) -> bool { (8..=64).contains(&s.len()) && s.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_') }

impl Offer {
    pub fn sign(id: &Identity, mut o: Offer) -> Offer { o.signature = id.sign(&canonical(&o)); o }
    pub fn to_wire(&self) -> Value {
        json!({ "version": 1, "type": "file.offer", "id": self.id, "name": self.name, "size": self.size, "mime": self.mime, "sha256": self.sha256, "fromPeerId": self.from_peer, "timestamp": self.timestamp, "signature": self.signature })
    }
    pub fn from_wire(v: &Value) -> Result<Offer> {
        let s = |k: &str| v[k].as_str().map(String::from).ok_or_else(|| anyhow!("campo {k} ausente"));
        let o = Offer { id: s("id")?, name: s("name")?, size: v["size"].as_u64().ok_or_else(|| anyhow!("size"))?, mime: s("mime")?, sha256: s("sha256")?, from_peer: s("fromPeerId")?, timestamp: v["timestamp"].as_i64().ok_or_else(|| anyhow!("timestamp"))?, signature: s("signature")? };
        if !valid_id(&o.id) { return Err(anyhow!("id inválido")); }
        if o.size == 0 || o.size > MAX_FILE_BYTES { return Err(anyhow!("tamanho fora do limite")); }
        if o.sha256.len() != 64 || !o.sha256.bytes().all(|b| b.is_ascii_hexdigit()) { return Err(anyhow!("hash inválido")); }
        if o.mime.len() > 100 || o.name.chars().count() > 300 { return Err(anyhow!("metadados longos demais")); }
        Ok(o)
    }
    pub fn verify(&self, sender: &PublicJwk, sender_fp: &str, now_ms: i64) -> Result<()> {
        if self.from_peer != sender_fp { return Err(anyhow!("remetente não confere")); }
        if (now_ms - self.timestamp).abs() > OFFER_SKEW_MS { return Err(anyhow!("oferta fora do relógio")); }
        if !identity::verify(sender, &canonical(self), &self.signature) { return Err(anyhow!("assinatura inválida")); }
        Ok(())
    }
}

/// Lê o arquivo uma vez para obter tamanho e SHA-256. Bloqueante.
pub fn describe(path: &Path) -> Result<(String, u64, String, String)> {
    let meta = std::fs::metadata(path)?;
    if !meta.is_file() { return Err(anyhow!("não é um arquivo")); }
    if meta.len() == 0 { return Err(anyhow!("arquivo vazio")); }
    if meta.len() > MAX_FILE_BYTES { return Err(anyhow!("arquivo maior que {} MB", MAX_FILE_BYTES / 1024 / 1024)); }
    let mut f = File::open(path)?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 64 * 1024];
    loop { let n = f.read(&mut buf)?; if n == 0 { break; } h.update(&buf[..n]); }
    let name = sanitize_name(&path.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_default());
    let mime = mime_for(&name).to_string();
    Ok((name, meta.len(), hex(&h.finalize()), mime))
}

pub fn hex(b: &[u8]) -> String { b.iter().map(|x| format!("{x:02x}")).collect() }

pub struct Incoming { file: Option<File>, pub path: PathBuf, pub offer: Offer, pub got: u64, next_seq: u64, hasher: Sha256 }

impl Incoming {
    pub fn start(dir: &Path, offer: Offer) -> Result<Incoming> {
        std::fs::create_dir_all(dir)?;
        let name = sanitize_name(&offer.name);
        let mut path = dir.join(format!("{}-{}", &offer.id[..offer.id.len().min(8)], name));
        let mut k = 1;
        while path.exists() { path = dir.join(format!("{}-{}-{}", &offer.id[..offer.id.len().min(8)], k, name)); k += 1; }
        let file = File::create(&path)?;
        Ok(Incoming { file: Some(file), path, offer, got: 0, next_seq: 0, hasher: Sha256::new() })
    }
    /// Pedaços chegam em ordem (DataChannel confiável e ordenado): qualquer salto, excesso ou pedaço grande demais aborta.
    pub fn chunk(&mut self, seq: u64, data: &[u8]) -> Result<()> {
        if seq != self.next_seq { return Err(anyhow!("pedaço fora de ordem")); }
        if data.is_empty() || data.len() > CHUNK_BYTES { return Err(anyhow!("pedaço com tamanho inválido")); }
        if self.got + data.len() as u64 > self.offer.size { return Err(anyhow!("mais dados que o declarado")); }
        self.file.as_mut().ok_or_else(|| anyhow!("encerrado"))?.write_all(data)?;
        self.hasher.update(data);
        self.got += data.len() as u64;
        self.next_seq += 1;
        Ok(())
    }
    pub fn finish(mut self) -> Result<PathBuf> {
        if self.got != self.offer.size { self.abort_inner(); return Err(anyhow!("arquivo incompleto")); }
        let got = hex(&self.hasher.clone().finalize());
        if !got.eq_ignore_ascii_case(&self.offer.sha256) { self.abort_inner(); return Err(anyhow!("hash não confere")); }
        if let Some(mut f) = self.file.take() { f.flush()?; }
        Ok(self.path.clone())
    }
    fn abort_inner(&mut self) { self.file = None; let _ = std::fs::remove_file(&self.path); }
    pub fn abort(mut self) { self.abort_inner(); }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(tag: &str) -> PathBuf { let d = std::env::temp_dir().join(format!("briks-ft-{tag}-{}", uuid::Uuid::new_v4())); std::fs::create_dir_all(&d).unwrap(); d }

    #[test]
    fn names_are_sanitized() {
        assert_eq!(sanitize_name("..\\..\\Windows\\system32\\evil.exe"), "evil.exe");
        assert_eq!(sanitize_name("a/b/c.png"), "c.png");
        assert_eq!(sanitize_name("CON.txt"), "_CON.txt");
        assert_eq!(sanitize_name("rel<a>tó:rio?.pdf"), "relatório.pdf");
        assert_eq!(sanitize_name("..."), "arquivo");
        let long = format!("{}.zip", "x".repeat(300));
        assert!(sanitize_name(&long).chars().count() <= 100 && sanitize_name(&long).ends_with(".zip"));
    }

    fn offer_for(idn: &Identity, size: u64, sha: &str) -> Offer {
        Offer::sign(idn, Offer { id: "ft-0000-aaaa".into(), name: "foto.png".into(), size, mime: "image/png".into(), sha256: sha.into(), from_peer: idn.fingerprint(), timestamp: 1_000_000, signature: String::new() })
    }

    #[test]
    fn offer_signature_sender_and_clock_are_enforced() {
        let (a, b) = (Identity::generate(), Identity::generate());
        let sha = "0".repeat(64);
        let o = offer_for(&a, 10, &sha);
        let wire = o.to_wire();
        let back = Offer::from_wire(&wire).unwrap();
        assert!(back.verify(&a.public_jwk(), &a.fingerprint(), 1_000_000).is_ok());
        assert!(back.verify(&b.public_jwk(), &a.fingerprint(), 1_000_000).is_err(), "chave errada");
        assert!(back.verify(&a.public_jwk(), &b.fingerprint(), 1_000_000).is_err(), "remetente diferente do autor da oferta");
        assert!(back.verify(&a.public_jwk(), &a.fingerprint(), 1_000_000 + OFFER_SKEW_MS + 1).is_err(), "oferta velha");
        let mut tampered = wire.clone(); tampered["name"] = json!("outro.png");
        assert!(Offer::from_wire(&tampered).unwrap().verify(&a.public_jwk(), &a.fingerprint(), 1_000_000).is_err(), "adulterada");
        let mut big = wire.clone(); big["size"] = json!(MAX_FILE_BYTES + 1);
        assert!(Offer::from_wire(&big).is_err(), "acima do limite");
        let mut zero = wire.clone(); zero["size"] = json!(0);
        assert!(Offer::from_wire(&zero).is_err());
        let mut badhash = wire; badhash["sha256"] = json!("xyz");
        assert!(Offer::from_wire(&badhash).is_err());
    }

    #[test]
    fn transfer_roundtrip_verifies_hash_and_rejects_bad_streams() {
        let src_dir = tmp("src"); let dst = tmp("dst");
        let data: Vec<u8> = (0..100_000u32).map(|i| (i % 251) as u8).collect();
        let sp = src_dir.join("dados.bin"); std::fs::write(&sp, &data).unwrap();
        let (name, size, sha, mime) = describe(&sp).unwrap();
        assert_eq!((name.as_str(), size, mime.as_str()), ("dados.bin", 100_000, "application/octet-stream"));
        let idn = Identity::generate();
        let offer = Offer::sign(&idn, Offer { id: "ft-1111-bbbb".into(), name, size, mime, sha256: sha.clone(), from_peer: idn.fingerprint(), timestamp: 1, signature: String::new() });

        let mut inc = Incoming::start(&dst, offer.clone()).unwrap();
        for (i, c) in data.chunks(CHUNK_BYTES).enumerate() { inc.chunk(i as u64, c).unwrap(); }
        let out = inc.finish().unwrap();
        assert_eq!(std::fs::read(&out).unwrap(), data);
        assert!(out.starts_with(&dst));

        // fora de ordem, pedaço grande, excesso, hash errado e incompleto: abortam e apagam
        let mut inc = Incoming::start(&dst, offer.clone()).unwrap();
        assert!(inc.chunk(1, &data[..10]).is_err());
        assert!(inc.chunk(0, &vec![1u8; CHUNK_BYTES + 1]).is_err());
        let p = inc.path.clone(); inc.abort(); assert!(!p.exists());
        let mut inc = Incoming::start(&dst, offer.clone()).unwrap();
        for (i, c) in data.chunks(CHUNK_BYTES).enumerate() { inc.chunk(i as u64, c).unwrap(); }
        assert!(inc.chunk(4, &[1]).is_err(), "excesso sobre o declarado");
        let mut bad = offer.clone(); bad.sha256 = "1".repeat(64);
        let mut inc = Incoming::start(&dst, bad).unwrap(); let p = inc.path.clone();
        for (i, c) in data.chunks(CHUNK_BYTES).enumerate() { inc.chunk(i as u64, c).unwrap(); }
        assert!(inc.finish().is_err()); assert!(!p.exists(), "arquivo com hash errado é apagado");
        let mut inc = Incoming::start(&dst, offer).unwrap(); let p = inc.path.clone();
        inc.chunk(0, &data[..1000]).unwrap();
        assert!(inc.finish().is_err()); assert!(!p.exists());
    }

    #[test]
    fn describe_enforces_limits() {
        let d = tmp("lim");
        std::fs::write(d.join("vazio.txt"), b"").unwrap();
        assert!(describe(&d.join("vazio.txt")).is_err());
        assert!(describe(&d).is_err(), "pasta");
        assert!(describe(&d.join("nao-existe")).is_err());
        let f = File::create(d.join("grande.bin")).unwrap(); f.set_len(MAX_FILE_BYTES + 1).unwrap();
        assert!(describe(&d.join("grande.bin")).is_err());
    }
}
