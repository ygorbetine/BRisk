//! Grupos P2P do Briks nativo (formato NOVO, só entre instalações do app nativo; não conversa com o grupo do Electron 1.2.2).
//!
//! Um grupo é um *snapshot assinado* por quem pode alterá-lo (o dono ou um administrador). Não há servidor: o snapshot viaja por
//! (a) convite colado (`briks-grp1.<base64url>`, endereçado a uma pessoa específica) e (b) DataChannel entre peers autenticados da mesma sala.
//! A confiança nasce no convite (canal externo escolhido pelo dono) e depois só avança por assinaturas de quem já era autorizado.
//!
//! Regras (`accept`):
//!   * estrutura: 1 a 48 membros, 1 a 16 canais (ao menos um de texto), nomes curtos, ids únicos; o dono é membro e administrador;
//!     o dono e todo administrador carregam a chave pública (`jwk`) e ela tem de bater com a impressão digital (`fp`);
//!   * convite (nada confiado antes): assinado pelo DONO e o seu `fp` tem de estar na lista de membros ("esse convite não é para você");
//!   * atualização: mesmo grupo e mesmo dono; versão MAIOR (ou igual quando o dono sobrescreve uma edição de administrador);
//!     o signatário tem de ser dono ou administrador no estado JÁ CONFIADO (nunca no estado novo: ninguém se promove sozinho);
//!   * administrador não mexe em administradores: o conjunto de administradores e o dono ficam iguais, e nenhum deles sai da lista;
//!   * revogação: o dono tira alguém da lista de administradores; a partir do momento em que um nó aplica essa versão, assinaturas
//!     do revogado deixam de valer. Quem recebe uma versão sem o seu `fp` apaga o grupo (`Decision::Removed`).
//!
//! Limite conhecido: um nó que ainda não recebeu a revogação aceita uma edição do administrador revogado com versão maior até a revogação chegar
//! (sem servidor central não há relógio comum); o dono ganha empate de versão e a revogação posterior de qualquer forma prevalece.
#![allow(dead_code)]

use std::collections::BTreeSet;

use anyhow::{anyhow, Result};
use base64::{engine::general_purpose::URL_SAFE_NO_PAD as B64, Engine};
use serde::{Deserialize, Serialize};

use crate::identity::{fingerprint, verify, Identity, PublicJwk};

pub const FORMAT: u32 = 1;
pub const CODE_PREFIX: &str = "briks-grp1.";
pub const MAX_MEMBERS: usize = 48;
pub const MAX_CHANNELS: usize = 16;
pub const MAX_NAME: usize = 48;
pub const MAX_CODE_BYTES: usize = 60_000;

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Member {
    pub fp: String,
    pub name: String,
    #[serde(default)]
    pub admin: bool,
    /// obrigatória para o dono e para administradores (quem assina); omitida para membros comuns
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub jwk: Option<PublicJwk>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Channel {
    pub id: String,
    pub name: String,
    /// "text" | "voice"
    pub kind: String,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Snapshot {
    pub v: u32,
    pub group_id: String,
    pub name: String,
    pub owner_fp: String,
    pub version: u64,
    pub ts: u64,
    pub members: Vec<Member>,
    pub channels: Vec<Channel>,
    pub signer_fp: String,
    #[serde(default)]
    pub signature: String,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Decision {
    /// primeiro snapshot do grupo (convite do dono para mim)
    Invite,
    Update,
    /// a nova versão não me inclui mais: apagar o grupo daqui
    Removed,
}

fn canonical(s: &Snapshot) -> String {
    let mut c = s.clone();
    c.signature.clear();
    serde_json::to_string(&c).unwrap_or_default()
}

/// Assina como `id` (que precisa ser o dono ou um administrador listado). Preenche `signer_fp` e `signature`.
pub fn sign(id: &Identity, s: &mut Snapshot) -> Result<()> {
    let me = id.fingerprint();
    let Some(m) = s.members.iter().find(|m| m.fp == me) else { return Err(anyhow!("você não está na lista de membros")) };
    if !m.admin {
        return Err(anyhow!("só o dono ou um administrador assina alterações do grupo"));
    }
    s.signer_fp = me;
    s.signature = id.sign(&canonical(s));
    check_structure(s)
}

pub fn to_code(s: &Snapshot) -> String {
    format!("{CODE_PREFIX}{}", B64.encode(serde_json::to_vec(s).unwrap_or_default()))
}

pub fn from_code(code: &str) -> Result<Snapshot> {
    let code = code.trim();
    let Some(b) = code.strip_prefix(CODE_PREFIX) else { return Err(anyhow!("isso não parece um convite de grupo do Briks")) };
    if b.len() > MAX_CODE_BYTES {
        return Err(anyhow!("convite grande demais"));
    }
    let raw = B64.decode(b.trim()).map_err(|_| anyhow!("convite corrompido"))?;
    serde_json::from_slice(&raw).map_err(|_| anyhow!("convite corrompido"))
}

fn short(s: &str, max: usize) -> bool {
    let n = s.chars().count();
    n >= 1 && n <= max && !s.chars().any(|c| c.is_control())
}

/// Estrutura e coerência interna (sem olhar para o que já era confiado).
pub fn check_structure(s: &Snapshot) -> Result<()> {
    if s.v != FORMAT { return Err(anyhow!("formato de grupo desconhecido")); }
    if !short(&s.group_id, 64) || !short(&s.name, MAX_NAME) { return Err(anyhow!("nome ou id do grupo inválido")); }
    if s.version < 1 { return Err(anyhow!("versão inválida")); }
    if s.members.is_empty() || s.members.len() > MAX_MEMBERS { return Err(anyhow!("número de membros inválido")); }
    if s.channels.is_empty() || s.channels.len() > MAX_CHANNELS { return Err(anyhow!("número de canais inválido")); }
    let mut seen = BTreeSet::new();
    for m in &s.members {
        if !short(&m.fp, 80) || !short(&m.name, 64) || !seen.insert(m.fp.clone()) { return Err(anyhow!("membro inválido ou repetido")); }
        if m.admin || m.fp == s.owner_fp {
            let Some(j) = &m.jwk else { return Err(anyhow!("administrador sem chave pública")) };
            if fingerprint(j) != m.fp { return Err(anyhow!("chave pública não confere com o membro")); }
        }
    }
    match s.members.iter().find(|m| m.fp == s.owner_fp) {
        Some(o) if o.admin => {}
        _ => return Err(anyhow!("o dono precisa estar na lista como administrador")),
    }
    let mut ids = BTreeSet::new();
    for c in &s.channels {
        if !short(&c.id, 64) || !short(&c.name, MAX_NAME) || !["text", "voice"].contains(&c.kind.as_str()) || !ids.insert(c.id.clone()) { return Err(anyhow!("canal inválido ou repetido")); }
    }
    if !s.channels.iter().any(|c| c.kind == "text") { return Err(anyhow!("o grupo precisa de ao menos um canal de texto")); }
    if !s.members.iter().any(|m| m.fp == s.signer_fp && m.admin) { return Err(anyhow!("o signatário não é dono nem administrador")); }
    Ok(())
}

fn admins(s: &Snapshot) -> BTreeSet<&str> {
    s.members.iter().filter(|m| m.admin).map(|m| m.fp.as_str()).collect()
}

/// Valida `incoming` contra o que já é confiado (`trusted`, None = ainda não conheço o grupo) e diz o que fazer. O erro é uma frase para o usuário.
pub fn accept(trusted: Option<&Snapshot>, incoming: &Snapshot, my_fp: &str) -> Result<Decision> {
    check_structure(incoming)?;
    let signer = incoming.members.iter().find(|m| m.fp == incoming.signer_fp).and_then(|m| m.jwk.clone()).ok_or_else(|| anyhow!("assinatura sem chave"))?;
    if !verify(&signer, &canonical(incoming), &incoming.signature) {
        return Err(anyhow!("assinatura inválida"));
    }
    let mine = incoming.members.iter().any(|m| m.fp == my_fp);
    let Some(t) = trusted else {
        if incoming.signer_fp != incoming.owner_fp { return Err(anyhow!("um convite precisa ser assinado pelo dono do grupo")); }
        if !mine { return Err(anyhow!("esse convite não é para você")); }
        return Ok(Decision::Invite);
    };
    if incoming.group_id != t.group_id || incoming.owner_fp != t.owner_fp { return Err(anyhow!("o grupo ou o dono não conferem")); }
    // quem assina precisa ter autoridade no estado que eu já confio
    let by_owner = incoming.signer_fp == t.owner_fp;
    if !by_owner && !t.members.iter().any(|m| m.fp == incoming.signer_fp && m.admin) { return Err(anyhow!("quem assinou não é administrador (ou foi revogado)")); }
    let newer = incoming.version > t.version;
    let owner_overrides = incoming.version == t.version && by_owner && t.signer_fp != t.owner_fp && canonical(incoming) != canonical(t);
    if !newer && !owner_overrides { return Err(anyhow!("versão antiga ou repetida")); }
    if !by_owner {
        // administrador comum: sem mexer em administradores nem no dono
        if admins(incoming) != admins(t) { return Err(anyhow!("só o dono muda administradores")); }
    }
    if !mine { return Ok(Decision::Removed); }
    Ok(Decision::Update)
}

/// Checagem que o mesh faz ao receber um snapshot pelo DataChannel: estrutura + assinatura + o signatário é quem está do outro lado (já autenticado).
/// A autorização de verdade (versão, administrador, dono) é de `accept`, que precisa do estado já confiado e roda quando a UI chama `group.verify`.
pub fn verify_wire(s: &Snapshot, peer_fp: &str) -> Result<()> {
    check_structure(s)?;
    if s.signer_fp != peer_fp { return Err(anyhow!("o snapshot não foi assinado por quem o enviou")); }
    let signer = s.members.iter().find(|m| m.fp == s.signer_fp).and_then(|m| m.jwk.clone()).ok_or_else(|| anyhow!("assinatura sem chave"))?;
    if !verify(&signer, &canonical(s), &s.signature) { return Err(anyhow!("assinatura inválida")); }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn mem(id: &Identity, name: &str, admin: bool) -> Member {
        Member { fp: id.fingerprint(), name: name.into(), admin, jwk: if admin { Some(id.public_jwk()) } else { None } }
    }
    fn base(owner: &Identity, members: Vec<Member>) -> Snapshot {
        Snapshot { v: 1, group_id: "g1".into(), name: "Amigos".into(), owner_fp: owner.fingerprint(), version: 1, ts: 1, members,
            channels: vec![Channel { id: "c1".into(), name: "geral".into(), kind: "text".into() }, Channel { id: "c2".into(), name: "Sala".into(), kind: "voice".into() }],
            signer_fp: String::new(), signature: String::new() }
    }
    fn signed(by: &Identity, mut s: Snapshot) -> Snapshot { sign(by, &mut s).unwrap(); s }

    struct W { owner: Identity, adm: Identity, bia: Identity, cai: Identity }
    fn world() -> W { W { owner: Identity::generate(), adm: Identity::generate(), bia: Identity::generate(), cai: Identity::generate() } }

    #[test]
    fn invite_roundtrips_through_the_code_and_only_works_for_the_invited() {
        let w = world();
        let s = signed(&w.owner, base(&w.owner, vec![mem(&w.owner, "Ana", true), mem(&w.bia, "Bia", false)]));
        let back = from_code(&to_code(&s)).unwrap();
        assert_eq!(back, s);
        assert_eq!(accept(None, &back, &w.bia.fingerprint()).unwrap(), Decision::Invite);
        assert!(accept(None, &back, &w.cai.fingerprint()).unwrap_err().to_string().contains("não é para você"));
        assert!(from_code("qualquer coisa").is_err());
        assert!(from_code("briks-grp1.@@@").is_err());
    }

    #[test]
    fn tampering_and_forged_signers_are_rejected() {
        let w = world();
        let s = signed(&w.owner, base(&w.owner, vec![mem(&w.owner, "Ana", true), mem(&w.bia, "Bia", false)]));
        let mut t = s.clone(); t.name = "Outro".into();
        assert_eq!(accept(None, &t, &w.bia.fingerprint()).unwrap_err().to_string(), "assinatura inválida");
        // convite assinado por um administrador (não pelo dono) não vale como primeiro contato
        let mut a = base(&w.owner, vec![mem(&w.owner, "Ana", true), mem(&w.adm, "Adm", true), mem(&w.bia, "Bia", false)]);
        a.signer_fp = w.adm.fingerprint(); a.signature = w.adm.sign(&canonical(&a));
        assert!(accept(None, &a, &w.bia.fingerprint()).unwrap_err().to_string().contains("dono"));
        // jwk que não bate com o fp do membro
        let mut f = base(&w.owner, vec![mem(&w.owner, "Ana", true), mem(&w.bia, "Bia", false)]);
        f.members[0].jwk = Some(w.cai.public_jwk());
        assert!(sign(&w.owner, &mut f).is_err());
        // quem não é admin não assina
        let mut n = base(&w.owner, vec![mem(&w.owner, "Ana", true), mem(&w.bia, "Bia", false)]);
        assert!(sign(&w.bia, &mut n).is_err());
        // estrutura: sem canal de texto, membro repetido
        let mut nt = base(&w.owner, vec![mem(&w.owner, "Ana", true)]); nt.channels.retain(|c| c.kind == "voice");
        assert!(sign(&w.owner, &mut nt).is_err());
        let mut dup = base(&w.owner, vec![mem(&w.owner, "Ana", true), mem(&w.owner, "Ana", true)]);
        assert!(sign(&w.owner, &mut dup).is_err());
    }

    #[test]
    fn updates_follow_version_signer_and_admin_rules() {
        let w = world();
        let v1 = signed(&w.owner, base(&w.owner, vec![mem(&w.owner, "Ana", true), mem(&w.adm, "Adm", true), mem(&w.bia, "Bia", false)]));
        let me = w.bia.fingerprint();
        // dono edita: ok; versão repetida/antiga não
        let mut v2 = v1.clone(); v2.version = 2; v2.name = "Novo nome".into();
        let v2 = signed(&w.owner, v2);
        assert_eq!(accept(Some(&v1), &v2, &me).unwrap(), Decision::Update);
        assert!(accept(Some(&v2), &v2, &me).is_err());
        assert!(accept(Some(&v2), &v1, &me).is_err());
        // administrador adiciona membro comum: ok
        let mut a2 = v2.clone(); a2.version = 3; a2.members.push(mem(&w.cai, "Cai", false));
        let a2 = signed(&w.adm, a2);
        assert_eq!(accept(Some(&v2), &a2, &me).unwrap(), Decision::Update);
        // administrador não promove ninguém, não tira o dono nem outro administrador
        let mut p = v2.clone(); p.version = 3; p.members[2].admin = true; p.members[2].jwk = Some(w.bia.public_jwk());
        assert!(accept(Some(&v2), &signed(&w.adm, p), &me).unwrap_err().to_string().contains("dono"));
        let mut r = v2.clone(); r.version = 3; r.members.retain(|m| m.fp != w.owner.fingerprint());
        assert!(accept(Some(&v2), &{ let mut x = r.clone(); x.owner_fp = w.owner.fingerprint(); x.signer_fp = w.adm.fingerprint(); x.signature = w.adm.sign(&canonical(&x)); x }, &me).is_err());
        // membro comum não assina
        let mut c = v2.clone(); c.version = 3;
        c.signer_fp = w.bia.fingerprint(); c.signature = w.bia.sign(&canonical(&c));
        assert!(accept(Some(&v2), &c, &me).is_err());
        // grupo ou dono diferente
        let mut other = v2.clone(); other.version = 3; other.group_id = "g2".into();
        assert!(accept(Some(&v2), &signed(&w.owner, other), &me).is_err());
        let mut hijack = base(&w.adm, vec![mem(&w.adm, "Adm", true), mem(&w.bia, "Bia", false)]); hijack.version = 9;
        assert!(accept(Some(&v2), &signed(&w.adm, hijack), &me).unwrap_err().to_string().contains("não conferem"));
    }

    #[test]
    fn admin_edits_channels_and_wire_check_binds_the_sender() {
        let w = world();
        let v1 = signed(&w.owner, base(&w.owner, vec![mem(&w.owner, "Ana", true), mem(&w.adm, "Adm", true), mem(&w.bia, "Bia", false)]));
        let me = w.bia.fingerprint();
        // administrador cria, renomeia e apaga canais (o manifesto muda, os cargos não)
        let mut v2 = v1.clone(); v2.version = 2;
        v2.channels.push(Channel { id: "c3".into(), name: "novo".into(), kind: "text".into() });
        v2.channels[1].name = "Sala 2".into();
        let v2 = signed(&w.adm, v2);
        assert_eq!(accept(Some(&v1), &v2, &me).unwrap(), Decision::Update);
        let mut v3 = v2.clone(); v3.version = 3; v3.channels.retain(|c| c.id != "c3");
        assert_eq!(accept(Some(&v2), &signed(&w.adm, v3), &me).unwrap(), Decision::Update);
        // pelo DataChannel: o signatário tem de ser quem enviou
        assert!(verify_wire(&v2, &w.adm.fingerprint()).is_ok());
        assert!(verify_wire(&v2, &w.cai.fingerprint()).unwrap_err().to_string().contains("quem o enviou"));
        // sem canal de texto sobrando, o próprio assinar recusa
        let mut v4 = v2.clone(); v4.version = 4; v4.channels.retain(|c| c.kind == "voice");
        assert!(sign(&w.adm, &mut v4).is_err());
    }

    #[test]
    fn revoked_admin_loses_the_power_and_removed_member_drops_the_group() {
        let w = world();
        let v1 = signed(&w.owner, base(&w.owner, vec![mem(&w.owner, "Ana", true), mem(&w.adm, "Adm", true), mem(&w.bia, "Bia", false)]));
        let me = w.bia.fingerprint();
        // o dono revoga o administrador
        let mut v2 = v1.clone(); v2.version = 2; v2.members[1].admin = false; v2.members[1].jwk = None;
        let v2 = signed(&w.owner, v2);
        assert_eq!(accept(Some(&v1), &v2, &me).unwrap(), Decision::Update);
        // depois da revogação, o revogado não consegue mais assinar nada
        let mut late = v2.clone(); late.version = 3; late.name = "Golpe".into();
        late.members[1].admin = true; late.members[1].jwk = Some(w.adm.public_jwk());
        late.signer_fp = w.adm.fingerprint(); late.signature = w.adm.sign(&canonical(&late));
        assert!(accept(Some(&v2), &late, &me).unwrap_err().to_string().contains("revogado"));
        // empate: o dono vence uma edição concorrente de administrador na mesma versão
        let mut adm_edit = v1.clone(); adm_edit.version = 2; adm_edit.name = "do adm".into();
        let adm_edit = signed(&w.adm, adm_edit);
        let mut own_edit = v1.clone(); own_edit.version = 2; own_edit.name = "do dono".into();
        let own_edit = signed(&w.owner, own_edit);
        assert_eq!(accept(Some(&v1), &adm_edit, &me).unwrap(), Decision::Update);
        assert_eq!(accept(Some(&adm_edit), &own_edit, &me).unwrap(), Decision::Update);
        assert!(accept(Some(&own_edit), &adm_edit, &me).is_err());
        // remoção de membro: quem some recebe Removed
        let mut v3 = v2.clone(); v3.version = 3; v3.members.retain(|m| m.fp != me);
        assert_eq!(accept(Some(&v2), &signed(&w.owner, v3), &me).unwrap(), Decision::Removed);
    }
}
