//! Convite de amigo P2P por código assinado (formato NOVO do Briks nativo, mesmo padrão do convite de grupo em group_sync.rs).
//!
//! Quem convida assina `{nome, impressão digital, chave pública, nonce, validade}` com a chave da própria identidade e passa o código
//! `briks-amg1.<base64url>` por um canal externo (WhatsApp, Discord...). Quem recebe cola o código: o core confere (a) a chave pública bate
//! com a impressão digital, (b) a assinatura é daquela chave, (c) não expirou, (d) não é o seu próprio convite. Aceitar grava o amigo
//! (impressão digital + chave) em `friends-p2p.json`, sem servidor. O convite NÃO é de uso único: não existe servidor para queimar o nonce,
//! então quem pegar o código dentro da validade (7 dias) também consegue se adicionar como amigo de quem o gerou; por isso o código deve ser
//! enviado só a quem se quer como amigo. Aceitar de novo o mesmo código é inofensivo (não duplica).
#![allow(dead_code)]

use std::path::Path;

use anyhow::{anyhow, Result};
use base64::{engine::general_purpose::URL_SAFE_NO_PAD as B64, Engine};
use serde::{Deserialize, Serialize};

use crate::identity::{fingerprint, verify, Identity, PublicJwk};

pub const CODE_PREFIX: &str = "briks-amg1.";
pub const FORMAT: u32 = 1;
pub const TTL_MS: u64 = 7 * 24 * 3600 * 1000;
pub const MAX_CODE_BYTES: usize = 4_000;
const FILE: &str = "friends-p2p.json";

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Invite {
    pub v: u32,
    pub name: String,
    pub fp: String,
    pub jwk: PublicJwk,
    pub nonce: String,
    pub ts: u64,
    pub exp: u64,
    #[serde(default)]
    pub signature: String,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Friend {
    pub fp: String,
    pub name: String,
    pub jwk: PublicJwk,
    pub added: u64,
}

fn canonical(i: &Invite) -> String {
    let mut c = i.clone();
    c.signature.clear();
    serde_json::to_string(&c).unwrap_or_default()
}

fn short(s: &str, max: usize) -> bool {
    let n = s.chars().count();
    n >= 1 && n <= max && !s.chars().any(|c| c.is_control())
}

/// Gera o convite assinado por `id` (válido por `TTL_MS` a partir de `now`, em ms).
pub fn create(id: &Identity, name: &str, now: u64, nonce: &str) -> Result<Invite> {
    let name = name.trim();
    if !short(name, 48) { return Err(anyhow!("escolha um nome de exibição antes de convidar")); }
    let mut i = Invite { v: FORMAT, name: name.into(), fp: id.fingerprint(), jwk: id.public_jwk(), nonce: nonce.into(), ts: now, exp: now + TTL_MS, signature: String::new() };
    i.signature = id.sign(&canonical(&i));
    Ok(i)
}

pub fn to_code(i: &Invite) -> String {
    format!("{CODE_PREFIX}{}", B64.encode(serde_json::to_vec(i).unwrap_or_default()))
}

pub fn from_code(code: &str) -> Result<Invite> {
    let code = code.trim();
    let Some(b) = code.strip_prefix(CODE_PREFIX) else { return Err(anyhow!("isso não parece um convite de amigo do Briks")) };
    if b.len() > MAX_CODE_BYTES { return Err(anyhow!("convite grande demais")); }
    let raw = B64.decode(b.trim()).map_err(|_| anyhow!("convite corrompido"))?;
    serde_json::from_slice(&raw).map_err(|_| anyhow!("convite corrompido"))
}

/// Confere o convite recebido por `my_fp` em `now` e devolve quem é o amigo. O erro é uma frase para o usuário.
pub fn check(i: &Invite, my_fp: &str, now: u64) -> Result<Friend> {
    if i.v != FORMAT { return Err(anyhow!("formato de convite desconhecido")); }
    if !short(&i.name, 48) || !short(&i.nonce, 64) { return Err(anyhow!("convite inválido")); }
    if fingerprint(&i.jwk) != i.fp { return Err(anyhow!("a chave do convite não confere")); }
    if !verify(&i.jwk, &canonical(i), &i.signature) { return Err(anyhow!("assinatura inválida: o convite foi alterado")); }
    if i.exp < i.ts || i.exp - i.ts > TTL_MS { return Err(anyhow!("convite inválido")); }
    if now > i.exp { return Err(anyhow!("esse convite expirou: peça um novo")); }
    if i.ts > now + 10 * 60 * 1000 { return Err(anyhow!("convite com data no futuro")); }
    if i.fp == my_fp { return Err(anyhow!("esse é o seu próprio convite")); }
    Ok(Friend { fp: i.fp.clone(), name: i.name.clone(), jwk: i.jwk.clone(), added: now })
}

pub fn load(dir: &Path) -> Vec<Friend> {
    std::fs::read_to_string(dir.join(FILE)).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default()
}

/// Grava o amigo (atualiza o nome se já existe). Devolve `true` se já era amigo.
pub fn save(dir: &Path, f: &Friend) -> Result<bool> {
    let mut all = load(dir);
    let existed = if let Some(x) = all.iter_mut().find(|x| x.fp == f.fp) { x.name = f.name.clone(); true } else { all.push(f.clone()); false };
    std::fs::create_dir_all(dir)?;
    std::fs::write(dir.join(FILE), serde_json::to_vec_pretty(&all)?)?;
    Ok(existed)
}

#[cfg(test)]
mod tests {
    use super::*;

    const NOW: u64 = 1_800_000_000_000;

    #[test]
    fn code_roundtrips_and_the_friend_is_the_signer() {
        let ana = Identity::generate();
        let bia = Identity::generate();
        let inv = create(&ana, "Ana", NOW, "n1").unwrap();
        let back = from_code(&to_code(&inv)).unwrap();
        assert_eq!(back, inv);
        let f = check(&back, &bia.fingerprint(), NOW + 1000).unwrap();
        assert_eq!((f.fp.as_str(), f.name.as_str()), (ana.fingerprint().as_str(), "Ana"));
        assert!(from_code("qualquer coisa").is_err());
        assert!(from_code("briks-amg1.@@@").is_err());
        assert!(from_code(&format!("briks-grp1.{}", "AAAA")).is_err());
    }

    #[test]
    fn tampering_expiry_and_own_code_are_rejected() {
        let ana = Identity::generate();
        let bia = Identity::generate();
        let mallory = Identity::generate();
        let inv = create(&ana, "Ana", NOW, "n1").unwrap();
        // trocar o nome invalida a assinatura
        let mut t = inv.clone(); t.name = "Ana (falsa)".into();
        assert!(check(&t, &bia.fingerprint(), NOW).unwrap_err().to_string().contains("assinatura"));
        // trocar a chave por outra: nem a impressão digital bate
        let mut k = inv.clone(); k.jwk = mallory.public_jwk();
        assert!(check(&k, &bia.fingerprint(), NOW).unwrap_err().to_string().contains("chave"));
        // Mallory refaz tudo com a chave dela mantendo a impressão digital da Ana: a assinatura dela não vale para a chave da Ana
        let mut f = create(&mallory, "Ana", NOW, "n1").unwrap(); f.fp = ana.fingerprint(); f.jwk = ana.public_jwk();
        assert!(check(&f, &bia.fingerprint(), NOW).is_err());
        // expirado e do futuro
        assert!(check(&inv, &bia.fingerprint(), NOW + TTL_MS + 1).unwrap_err().to_string().contains("expirou"));
        assert!(check(&inv, &bia.fingerprint(), NOW - 3_600_000).unwrap_err().to_string().contains("futuro"));
        assert!(check(&inv, &bia.fingerprint(), NOW + TTL_MS).is_ok());
        // o próprio convite
        assert!(check(&inv, &ana.fingerprint(), NOW).unwrap_err().to_string().contains("seu próprio"));
        // validade esticada pelo remetente além do limite
        let mut e = inv.clone(); e.exp = e.ts + TTL_MS * 10; e.signature = ana.sign(&canonical(&e));
        assert!(check(&e, &bia.fingerprint(), NOW).is_err());
        // sem nome
        assert!(create(&ana, "  ", NOW, "n").is_err());
    }

    #[test]
    fn accepting_stores_the_friend_once() {
        let ana = Identity::generate();
        let bia = Identity::generate();
        let dir = std::env::temp_dir().join(format!("briks-amg-test-{}-{}", std::process::id(), NOW));
        let _ = std::fs::remove_dir_all(&dir);
        let f = check(&create(&ana, "Ana", NOW, "n1").unwrap(), &bia.fingerprint(), NOW).unwrap();
        assert_eq!(save(&dir, &f).unwrap(), false);
        let mut f2 = f.clone(); f2.name = "Ana Maria".into();
        assert_eq!(save(&dir, &f2).unwrap(), true);
        let all = load(&dir);
        assert_eq!(all.len(), 1);
        assert_eq!(all[0].name, "Ana Maria");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
