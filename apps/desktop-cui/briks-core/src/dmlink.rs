//! Conversa privada (DM) fora da sala de voz: um canal direto entre dois amigos, aberto pela presença, sem servidor guardando mensagem.
//!
//! Como se encontram: cada pessoa, com o Briks aberto e logada, observa a PRÓPRIA caixa de chegada: um canal de presença do Realtime
//! cujo nome é um hash do id da conta (`inbox:<id>`). Quem quer escrever (e já tem a impressão digital do amigo, vinda da lista de
//! amigos da conta) entra na caixa do amigo só para ficar visível com `{fp}` ("toque") e abre, ao mesmo tempo, a sua ponta do canal
//! direto `chat:<dm-do-par>` (a mesma malha P2P autenticada das chamadas, só com DataChannel, sem áudio). Quem observa a caixa vê o
//! toque, a interface confere que `fp` é de um amigo e abre a outra ponta; os dois se acham, provam a identidade com a chave de cada
//! um e a caixa de saída entrega o que estava esperando (mais o histórico assinado que faltar de cada lado).
//!
//! Privacidade: o toque carrega só a impressão digital de quem escreveu (nunca texto, nome ou id de conta); o conteúdo das
//! mensagens só passa pelo canal direto autenticado. Nada fica guardado no servidor: se o amigo está offline, a mensagem espera na
//! caixa de saída DESTE computador (e no histórico assinado local) e segue quando os dois estiverem online; o toque some quando o
//! canal direto abre, quando a conversa é fechada ou quando o Briks fecha.
use crate::realtime::{self, Realtime, RealtimeCfg, RtEvent};
use crate::signaling::{secure_room_id, topic_name};
use serde_json::{json, Value};
use tokio::sync::mpsc;

/// Canais diretos abertos ao mesmo tempo (o mais antigo sai quando chega mais um).
pub const MAX_LINKS: usize = 6;

pub fn valid_id(s: &str) -> bool { !s.is_empty() && s.len() <= 64 && s.chars().all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_') }
pub fn valid_fp(s: &str) -> bool { !s.is_empty() && s.len() <= 72 && s.chars().all(|c| c.is_ascii_alphanumeric()) }

/// Canal de presença onde alguém avisa que quer falar com `user_id`.
pub fn inbox_topic(user_id: &str) -> String { topic_name("friend", &secure_room_id("friend", &format!("inbox:{user_id}"))) }

/// O toque de quem quer falar: só a impressão digital.
pub fn knock_meta(my_fp: &str) -> Value { json!({ "fp": my_fp, "knock": true }) }

/// Lê o toque de um peer da caixa. Descarta o que não tem o formato certo e o próprio.
pub fn read_knock(meta: &Value, my_fp: &str) -> Option<String> {
    let fp = meta["fp"].as_str()?;
    (meta["knock"].as_bool() == Some(true) && valid_fp(fp) && fp != my_fp).then(|| fp.to_string())
}

/// Observa a minha caixa e emite `{"ev":"dm.wake","fp":...}` para cada toque.
pub fn watch_inbox(cfg: RealtimeCfg, my_user_id: &str, my_fp: String, tx: mpsc::Sender<Value>) -> Realtime {
    let (rt, mut rx) = realtime::connect(cfg, inbox_topic(my_user_id), uuid::Uuid::new_v4().to_string(), json!({ "inbox": true }));
    tokio::spawn(async move {
        while let Some(e) = rx.recv().await {
            if let RtEvent::PeerJoined { meta, .. } = e {
                if let Some(fp) = read_knock(&meta, &my_fp) { if tx.send(json!({ "ev": "dm.wake", "fp": fp })).await.is_err() { return; } }
            }
        }
    });
    rt
}

/// Toca na caixa de `friend_user_id`. Devolve a conexão: quem chama a fecha quando o canal direto abrir.
pub fn knock(cfg: RealtimeCfg, friend_user_id: &str, my_fp: &str) -> Realtime {
    let (rt, mut rx) = realtime::connect(cfg, inbox_topic(friend_user_id), uuid::Uuid::new_v4().to_string(), knock_meta(my_fp));
    tokio::spawn(async move { while rx.recv().await.is_some() {} });   // só precisa ficar visível
    rt
}

#[cfg(test)]
mod t {
    use super::*;

    #[test]
    fn inbox_names_differ_per_account_and_never_contain_the_id() {
        let (a, b) = (inbox_topic("user-a"), inbox_topic("user-b"));
        assert_ne!(a, b);
        assert!(a.starts_with("realtime:briks:friend:") && !a.contains("user-a"));
        assert_ne!(a, topic_name("friend", &secure_room_id("friend", "user-a")), "não colide com o nome simples");
    }

    #[test]
    fn a_knock_carries_only_a_valid_fingerprint_and_never_myself() {
        let fp = "ab12".repeat(16);
        assert_eq!(read_knock(&knock_meta(&fp), "outro"), Some(fp.clone()));
        assert_eq!(read_knock(&knock_meta(&fp), &fp), None, "o próprio toque não conta");
        assert_eq!(read_knock(&json!({ "fp": fp }), "x"), None, "sem a marca de toque");
        assert_eq!(read_knock(&json!({ "fp": "../etc", "knock": true }), "x"), None);
        assert_eq!(read_knock(&json!({ "fp": "a".repeat(73), "knock": true }), "x"), None);
        assert_eq!(read_knock(&json!({ "inbox": true }), "x"), None);
        assert!(valid_id("abc-123_x") && !valid_id("a b") && !valid_id(""));
    }
}
