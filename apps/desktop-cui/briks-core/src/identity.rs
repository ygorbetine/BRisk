//! Identidade ECDSA P-256 do app nativo e mensagens de chat assinadas (docs/protocol.md, "Chat por DataChannel").
//! A assinatura é o formato raw r‖s (P1363) em base64url, o mesmo do WebCrypto, sobre o JSON canônico de
//! `canonicalSignedMessage` do cliente TypeScript (ordem das chaves idêntica à do `JSON.stringify`).
//! A chave do Electron é um CryptoKey não exportável: esta identidade é NOVA e não substitui aquela.
use anyhow::{anyhow, Result};
use base64::{engine::general_purpose::URL_SAFE_NO_PAD as B64, Engine};
use p256::ecdsa::{signature::Signer, signature::Verifier, Signature, SigningKey, VerifyingKey};
use p256::elliptic_curve::sec1::ToEncodedPoint;
use rand_core::OsRng;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)]
pub struct PublicJwk {
    pub kty: String,
    pub crv: String,
    pub x: String,
    pub y: String,
}

pub struct Identity {
    key: SigningKey,
}

/// Depuração sem segredo: só a impressão digital pública aparece.
impl std::fmt::Debug for Identity {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "Identity({})", self.fingerprint())
    }
}

impl Identity {
    pub fn generate() -> Self {
        Identity { key: SigningKey::random(&mut OsRng) }
    }
    pub fn from_secret_b64(s: &str) -> Result<Self> {
        let bytes = B64.decode(s.trim())?;
        Ok(Identity { key: SigningKey::from_slice(&bytes).map_err(|e| anyhow!("chave inválida: {e}"))? })
    }
    pub fn secret_b64(&self) -> String {
        B64.encode(self.key.to_bytes())
    }
    pub fn public_jwk(&self) -> PublicJwk {
        jwk_of(self.key.verifying_key())
    }
    pub fn sign(&self, canonical: &str) -> String {
        let sig: Signature = self.key.sign(canonical.as_bytes());
        B64.encode(sig.to_bytes())
    }
    /// Identificador estável da identidade pública (SHA-256 do `x.y`), usado para ligar peers a contatos.
    pub fn fingerprint(&self) -> String {
        fingerprint(&self.public_jwk())
    }
}

fn jwk_of(v: &VerifyingKey) -> PublicJwk {
    let pt = v.to_encoded_point(false);
    PublicJwk { kty: "EC".into(), crv: "P-256".into(), x: B64.encode(pt.x().expect("x")), y: B64.encode(pt.y().expect("y")) }
}

pub fn fingerprint(j: &PublicJwk) -> String {
    let d = Sha256::digest(format!("{}.{}", j.x, j.y).as_bytes());
    d.iter().map(|b| format!("{b:02x}")).collect()
}

pub fn verify(jwk: &PublicJwk, canonical: &str, signature_b64: &str) -> bool {
    if jwk.kty != "EC" || jwk.crv != "P-256" {
        return false;
    }
    let (Ok(x), Ok(y), Ok(sig)) = (B64.decode(&jwk.x), B64.decode(&jwk.y), B64.decode(signature_b64)) else { return false };
    if x.len() != 32 || y.len() != 32 || sig.len() != 64 {
        return false;
    }
    let mut sec1 = vec![0x04u8];
    sec1.extend_from_slice(&x);
    sec1.extend_from_slice(&y);
    let Ok(vk) = VerifyingKey::from_sec1_bytes(&sec1) else { return false };
    let Ok(sig) = Signature::from_slice(&sig) else { return false };
    vk.verify(canonical.as_bytes(), &sig).is_ok()
}

/// Mensagem de chat v2 (wire). A ordem dos campos é a do JSON canônico assinado.
#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct ChatWire {
    pub version: u32,
    #[serde(rename = "type")]
    pub kind: String,
    pub channel_id: String,
    pub id: String,
    pub author_peer_id: String,
    pub author: String,
    pub content: String,
    pub timestamp: u64,
    #[serde(default)]
    pub signature: String,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct Canonical<'a> {
    version: u32,
    #[serde(rename = "type")]
    kind: &'a str,
    channel_id: &'a str,
    id: &'a str,
    author_peer_id: &'a str,
    author: &'a str,
    content: &'a str,
    timestamp: u64,
}

pub fn canonical_signed_message(m: &ChatWire) -> String {
    serde_json::to_string(&Canonical {
        version: 2,
        kind: "chat.message",
        channel_id: &m.channel_id,
        id: &m.id,
        author_peer_id: &m.author_peer_id,
        author: &m.author,
        content: &m.content,
        timestamp: m.timestamp,
    })
    .expect("canônico")
}

/// JSON canônico de `canonicalIdentityProof` (ordem de chaves do cliente TypeScript); `caps` mantém a ordem recebida (serde_json preserve_order).
pub fn canonical_identity_proof(channel: &str, from: &str, to: &str, nonce: &str, ts: u64, caps: &serde_json::Value) -> String {
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct P<'a> { version: u32, #[serde(rename = "type")] kind: &'a str, channel_id: &'a str, from_peer_id: &'a str, to_peer_id: &'a str, nonce: &'a str, timestamp: u64, capabilities: &'a serde_json::Value }
    serde_json::to_string(&P { version: 2, kind: "chat.identity.proof", channel_id: channel, from_peer_id: from, to_peer_id: to, nonce, timestamp: ts, capabilities: caps }).expect("canônico")
}

pub const MAX_CHAT_CHARS: usize = 4000;
pub const MAX_PAYLOAD_BYTES: usize = 64 * 1024;

pub fn sign_message(id: &Identity, mut m: ChatWire) -> ChatWire {
    m.version = 2;
    m.kind = "chat.message".into();
    m.signature = id.sign(&canonical_signed_message(&m));
    m
}

/// Valida estrutura e assinatura de uma mensagem recebida. `sender` é a identidade já confiada para o peer.
pub fn verify_message(sender: &PublicJwk, m: &ChatWire, expected_channel: &str) -> Result<()> {
    if m.version != 2 || m.kind != "chat.message" {
        return Err(anyhow!("versão/tipo inesperado"));
    }
    if m.channel_id != expected_channel {
        return Err(anyhow!("canal inesperado"));
    }
    if m.content.is_empty() || m.content.chars().count() > MAX_CHAT_CHARS {
        return Err(anyhow!("tamanho de conteúdo inválido"));
    }
    if m.id.len() < 8 || m.id.len() > 64 {
        return Err(anyhow!("id inválido"));
    }
    if !verify(sender, &canonical_signed_message(m), &m.signature) {
        return Err(anyhow!("assinatura inválida"));
    }
    Ok(())
}

/// Canal da DM: `dm-` + SHA-256("briks-dm-v1:" + peerIds ordenados unidos por ":") — idêntico ao cliente TypeScript.
pub fn dm_channel(a: &str, b: &str) -> String {
    let mut v = [a, b];
    v.sort();
    let d = Sha256::digest(format!("briks-dm-v1:{}", v.join(":")).as_bytes());
    format!("dm-{}", d.iter().map(|x| format!("{x:02x}")).collect::<String>())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample(id: &Identity) -> ChatWire {
        sign_message(id, ChatWire {
            version: 2, kind: "chat.message".into(), channel_id: "dm-x".into(), id: "0b1c2d3e-aaaa-bbbb-cccc-111122223333".into(),
            author_peer_id: "peer-a".into(), author: "Ygor".into(), content: "olá, açaí".into(), timestamp: 1_700_000_000_000, signature: String::new(),
        })
    }

    #[test]
    fn canonical_matches_js_json_stringify_order() {
        let id = Identity::generate();
        let m = sample(&id);
        assert_eq!(
            canonical_signed_message(&m),
            r#"{"version":2,"type":"chat.message","channelId":"dm-x","id":"0b1c2d3e-aaaa-bbbb-cccc-111122223333","authorPeerId":"peer-a","author":"Ygor","content":"olá, açaí","timestamp":1700000000000}"#
        );
    }

    #[test]
    fn sign_verify_and_tamper() {
        let id = Identity::generate();
        let m = sample(&id);
        let jwk = id.public_jwk();
        assert!(verify_message(&jwk, &m, "dm-x").is_ok());
        let mut bad = m.clone();
        bad.content = "outra coisa".into();
        assert!(verify_message(&jwk, &bad, "dm-x").is_err(), "conteúdo adulterado");
        assert!(verify_message(&jwk, &m, "dm-y").is_err(), "canal errado");
        let other = Identity::generate();
        assert!(verify_message(&other.public_jwk(), &m, "dm-x").is_err(), "chave de outro");
        let mut empty = sample(&id);
        empty.content.clear();
        assert!(verify_message(&jwk, &sign_message(&id, empty), "dm-x").is_err());
    }

    #[test]
    fn secret_roundtrip_keeps_identity() {
        let id = Identity::generate();
        let again = Identity::from_secret_b64(&id.secret_b64()).unwrap();
        assert_eq!(id.public_jwk(), again.public_jwk());
        assert_eq!(id.fingerprint(), again.fingerprint());
        assert_eq!(id.fingerprint().len(), 64);
    }

    #[test]
    fn identity_proof_canonical_order() {
        let caps = serde_json::json!({"protocol":2,"client":"x","capabilities":[]});
        assert_eq!(canonical_identity_proof("c", "a", "b", "n", 5, &caps),
            r#"{"version":2,"type":"chat.identity.proof","channelId":"c","fromPeerId":"a","toPeerId":"b","nonce":"n","timestamp":5,"capabilities":{"protocol":2,"client":"x","capabilities":[]}}"#);
    }

    #[test]
    fn dm_channel_is_symmetric() {
        assert_eq!(dm_channel("a", "b"), dm_channel("b", "a"));
        assert_ne!(dm_channel("a", "b"), dm_channel("a", "c"));
        assert!(dm_channel("a", "b").starts_with("dm-") && dm_channel("a", "b").len() == 67);
    }
}
