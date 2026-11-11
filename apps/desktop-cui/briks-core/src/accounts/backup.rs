//! Cópia cifrada da identidade P2P, recuperável por frase. Tudo aqui roda NO APARELHO: o servidor recebe só o pacote
//! cifrado (`Blob`) e não tem como abri-lo. Sem a frase não há recuperação, nem por nós.
//!
//!   frase (12 palavras, 128 bits, gerada pelo app)
//!     └─ Argon2id (64 MiB, 3 passadas, sal aleatório de 16 bytes) ──► chave de 32 bytes
//!          └─ XChaCha20-Poly1305 (nonce aleatório de 24 bytes; dado autenticado = versão + id da conta)
//!               └─ texto cifrado da chave privada P-256 (32 bytes + etiqueta de 16)
//!
//! Frase errada, pacote adulterado ou pacote de outra conta falham do mesmo jeito (a etiqueta não confere), sem
//! revelar nada sobre a chave. Trocar a frase gera sal e nonce novos: a frase antiga deixa de abrir a cópia.
use argon2::{Algorithm, Argon2, Params, Version};
use base64::{engine::general_purpose::URL_SAFE_NO_PAD as B64, Engine};
use bip39::{Language, Mnemonic};
use chacha20poly1305::aead::{Aead, KeyInit, Payload};
use chacha20poly1305::{XChaCha20Poly1305, XNonce};
use rand_core::{OsRng, RngCore};
use serde::{Deserialize, Serialize};

use super::provider::{AResult, AccountError};
use crate::identity::Identity;

pub const VERSION: u32 = 1;
/// Custo padrão do Argon2id (recomendação da RFC 9106 para ambientes com memória limitada).
pub const M_KIB: u32 = 65536;
pub const T_COST: u32 = 3;
pub const P_COST: u32 = 1;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct Blob {
    pub version: u32,
    pub kdf: String,
    pub cipher: String,
    pub m_kib: u32,
    pub t: u32,
    pub p: u32,
    pub salt: String,
    pub nonce: String,
    pub ciphertext: String,
    /// impressão digital PÚBLICA da identidade guardada (para saber de qual chave é a cópia, sem abri-la)
    pub fingerprint: String,
}

fn err(code: &str, msg: &str) -> AccountError {
    AccountError::new(code, msg)
}
fn wrong_phrase() -> AccountError {
    err("wrong_phrase", "Essa frase não abre a cópia desta conta. Confira se é a frase da cópia mais recente (se você trocou a frase, vale só a última) e se a conta é a mesma.")
}

/// Frase nova: 128 bits do gerador do sistema, em 12 palavras da lista BIP39 em português (com soma de verificação).
pub fn new_phrase() -> String {
    let mut entropy = [0u8; 16];
    OsRng.fill_bytes(&mut entropy);
    Mnemonic::from_entropy_in(Language::Portuguese, &entropy).expect("16 bytes de entropia").to_string()
}

/// Minúsculas, um espaço entre as palavras. Confere as 12 palavras e a soma de verificação (pega erro de digitação
/// antes de qualquer acesso à rede).
pub fn check_phrase(input: &str) -> AResult<String> {
    let norm = input.split_whitespace().map(|w| w.to_lowercase()).collect::<Vec<_>>().join(" ");
    let words = norm.split(' ').filter(|w| !w.is_empty()).count();
    if words == 0 {
        return Err(err("invalid_phrase", "Digite as 12 palavras da sua frase de recuperação."));
    }
    if words != 12 {
        let msg = if words < 12 { format!("Faltam {} palavras: a frase tem 12 e você digitou {words}.", 12 - words) } else { format!("Sobram {} palavras: a frase tem 12 e você digitou {words}.", words - 12) };
        return Err(err("invalid_phrase", &msg));
    }
    match Mnemonic::parse_in_normalized(Language::Portuguese, &norm) {
        Ok(m) => Ok(m.to_string()),
        // diz QUAL posição está errada, nunca a palavra (a frase não vai para mensagens)
        Err(bip39::Error::UnknownWord(i)) => Err(err("invalid_phrase", &format!("A palavra {} não está na lista de palavras do Briks. Confira como ela está escrita (acentos incluídos).", i + 1))),
        Err(_) => Err(err("invalid_phrase", "As 12 palavras existem, mas não formam uma frase válida: alguma está trocada ou fora de ordem.")),
    }
}

fn derive(phrase: &str, salt: &[u8], m_kib: u32, t: u32, p: u32) -> AResult<[u8; 32]> {
    let params = Params::new(m_kib, t, p, Some(32)).map_err(|_| err("invalid_backup", "A cópia guardada tem parâmetros inválidos."))?;
    let mut key = [0u8; 32];
    Argon2::new(Algorithm::Argon2id, Version::V0x13, params)
        .hash_password_into(phrase.as_bytes(), salt, &mut key)
        .map_err(|_| err("backup", "Não foi possível preparar a chave da cópia."))?;
    Ok(key)
}

fn aad(user_id: &str) -> Vec<u8> {
    format!("briks-identity-backup/v{VERSION}|{user_id}").into_bytes()
}

/// Cifra com sal e nonce dados (os testes usam valores fixos; o app usa `seal`).
pub fn seal_with(id: &Identity, phrase: &str, user_id: &str, salt: &[u8; 16], nonce: &[u8; 24], m_kib: u32, t: u32, p: u32) -> AResult<Blob> {
    let phrase = check_phrase(phrase)?;
    let secret = B64.decode(id.secret_b64()).map_err(|_| err("backup", "Identidade deste aparelho inválida."))?;
    let mut key = derive(&phrase, salt, m_kib, t, p)?;
    let ct = XChaCha20Poly1305::new((&key).into())
        .encrypt(XNonce::from_slice(nonce), Payload { msg: &secret, aad: &aad(user_id) })
        .map_err(|_| err("backup", "Não foi possível cifrar a cópia."));
    key.fill(0);
    Ok(Blob { version: VERSION, kdf: "argon2id".into(), cipher: "xchacha20poly1305".into(), m_kib, t, p,
        salt: B64.encode(salt), nonce: B64.encode(nonce), ciphertext: B64.encode(ct?), fingerprint: id.fingerprint() })
}

pub fn seal(id: &Identity, phrase: &str, user_id: &str) -> AResult<Blob> {
    let (mut salt, mut nonce) = ([0u8; 16], [0u8; 24]);
    OsRng.fill_bytes(&mut salt);
    OsRng.fill_bytes(&mut nonce);
    seal_with(id, phrase, user_id, &salt, &nonce, M_KIB, T_COST, P_COST)
}

/// Abre a cópia. Recusa parâmetros fora da faixa aceita (um servidor malicioso não consegue baixar o custo do KDF
/// nem travar o aparelho pedindo memória demais) e confere que a chave aberta é mesmo a da impressão digital anunciada.
pub fn open(blob: &Blob, phrase: &str, user_id: &str) -> AResult<Identity> {
    let phrase = check_phrase(phrase)?;
    let bad = || err("invalid_backup", "A cópia guardada no servidor está em um formato que este app não aceita.");
    if blob.version != VERSION || blob.kdf != "argon2id" || blob.cipher != "xchacha20poly1305" {
        return Err(bad());
    }
    if !(19456..=262144).contains(&blob.m_kib) || !(2..=10).contains(&blob.t) || !(1..=4).contains(&blob.p) {
        return Err(bad());
    }
    let (Ok(salt), Ok(nonce), Ok(ct)) = (B64.decode(&blob.salt), B64.decode(&blob.nonce), B64.decode(&blob.ciphertext)) else { return Err(bad()) };
    if salt.len() != 16 || nonce.len() != 24 || ct.len() != 48 {
        return Err(bad());
    }
    let mut key = derive(&phrase, &salt, blob.m_kib, blob.t, blob.p)?;
    let plain = XChaCha20Poly1305::new((&key).into()).decrypt(XNonce::from_slice(&nonce), Payload { msg: &ct, aad: &aad(user_id) });
    key.fill(0);
    let mut secret = plain.map_err(|_| wrong_phrase())?;
    let id = Identity::from_secret_b64(&B64.encode(&secret));
    secret.fill(0);
    let id = id.map_err(|_| wrong_phrase())?;
    if id.fingerprint() != blob.fingerprint {
        return Err(bad());
    }
    Ok(id)
}
