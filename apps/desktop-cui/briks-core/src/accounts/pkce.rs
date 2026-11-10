//! PKCE (RFC 7636), método S256: o verificador nunca sai deste processo; só o desafio vai na URL aberta no navegador.
use base64::{engine::general_purpose::URL_SAFE_NO_PAD as B64, Engine};
use rand_core::{OsRng, RngCore};
use sha2::{Digest, Sha256};

pub struct Pkce {
    pub verifier: String,
    pub challenge: String,
}

/// Desafio S256 de um verificador: base64url(sha256(verificador)), sem preenchimento.
pub fn challenge_of(verifier: &str) -> String {
    B64.encode(Sha256::digest(verifier.as_bytes()))
}

impl Pkce {
    /// 32 bytes aleatórios do sistema -> verificador de 43 caracteres (o mínimo da RFC), só [A-Za-z0-9-_].
    pub fn generate() -> Self {
        let mut raw = [0u8; 32];
        OsRng.fill_bytes(&mut raw);
        let verifier = B64.encode(raw);
        let challenge = challenge_of(&verifier);
        Pkce { verifier, challenge }
    }
}

/// Codificação de componente de URL (RFC 3986: só os não reservados passam).
pub fn url_encode(s: &str) -> String {
    let mut o = String::with_capacity(s.len() * 3);
    for b in s.bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => o.push(b as char),
            _ => o.push_str(&format!("%{b:02X}")),
        }
    }
    o
}

pub fn url_decode(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        match b[i] {
            b'%' => {
                let hex = std::str::from_utf8(&b[i + 1..(i + 3).min(b.len())]).unwrap_or("");
                match (hex.len() == 2).then(|| u8::from_str_radix(hex, 16).ok()).flatten() {
                    Some(v) => {
                        out.push(v);
                        i += 3;
                    }
                    None => {
                        out.push(b'%');
                        i += 1;
                    }
                }
            }
            b'+' => {
                out.push(b' ');
                i += 1;
            }
            c => {
                out.push(c);
                i += 1;
            }
        }
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// Lê `a=1&b=2` (já sem o `?`).
pub fn parse_query(q: &str) -> Vec<(String, String)> {
    q.split('&').filter(|p| !p.is_empty()).map(|p| {
        let (k, v) = p.split_once('=').unwrap_or((p, ""));
        (url_decode(k), url_decode(v))
    }).collect()
}
