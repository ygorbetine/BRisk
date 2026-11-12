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

/// O aviso "sua lista de amigos/pedidos mudou": sem nenhum dado (nem impressão digital); quem recebe só refaz a consulta ao servidor.
pub fn poke_meta() -> Value { json!({ "poke": true }) }
pub fn is_poke(meta: &Value) -> bool { meta["poke"].as_bool() == Some(true) }

/// Observa a minha caixa e emite `{"ev":"dm.wake","fp":...}` para cada toque e `{"ev":"friends.poke"}` para cada aviso de lista.
pub fn watch_inbox(cfg: RealtimeCfg, my_user_id: &str, my_fp: String, tx: mpsc::Sender<Value>) -> Realtime {
    let (rt, mut rx) = realtime::connect(cfg, inbox_topic(my_user_id), uuid::Uuid::new_v4().to_string(), json!({ "inbox": true }));
    tokio::spawn(async move {
        let mut last_poke: Option<std::time::Instant> = None;
        while let Some(e) = rx.recv().await {
            if let RtEvent::PeerJoined { meta, .. } = e {
                if is_poke(&meta) {
                    // no máx. 1 aviso a cada 1,5 s (quem não é amigo só consegue fazer a UI refazer uma consulta ao servidor)
                    if last_poke.map_or(true, |t| t.elapsed() >= std::time::Duration::from_millis(1500)) {
                        last_poke = Some(std::time::Instant::now());
                        if tx.send(json!({ "ev": "friends.poke" })).await.is_err() { return; }
                    }
                    continue;
                }
                if let Some(fp) = read_knock(&meta, &my_fp) { if tx.send(json!({ "ev": "dm.wake", "fp": fp })).await.is_err() { return; } }
            }
        }
    });
    rt
}

/// Avisa `user_id` que a lista dele mudou: entra na caixa dele por alguns segundos e sai (nada é guardado no servidor).
pub fn poke(cfg: RealtimeCfg, user_id: &str) {
    let (rt, mut rx) = realtime::connect(cfg, inbox_topic(user_id), uuid::Uuid::new_v4().to_string(), poke_meta());
    tokio::spawn(async move {
        let drain = async { while rx.recv().await.is_some() {} };
        let _ = tokio::time::timeout(std::time::Duration::from_secs(5), drain).await;
        rt.close().await;
    });
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

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn a_poke_reaches_the_inbox_as_friends_poke_with_no_data_and_is_rate_limited() {
        let port = crate::mock::start(0).await.unwrap();
        let cfg = || RealtimeCfg::new(&format!("ws://127.0.0.1:{port}"), "chave-de-teste");
        let (tx, mut rx) = mpsc::channel::<Value>(16);
        let _inbox = watch_inbox(cfg(), "user-b", "ab12".repeat(16), tx);
        tokio::time::sleep(std::time::Duration::from_millis(600)).await;
        poke(cfg(), "user-b");
        let ev = tokio::time::timeout(std::time::Duration::from_secs(10), rx.recv()).await.expect("o aviso não chegou").unwrap();
        assert_eq!(ev, json!({ "ev": "friends.poke" }), "o aviso não carrega nenhum dado");
        assert!(is_poke(&poke_meta()) && !is_poke(&knock_meta("ab12")) && !is_poke(&json!({ "inbox": true })));
        // um aviso colado no outro (menos de 1,5 s) não vira um segundo evento
        poke(cfg(), "user-b");
        assert!(tokio::time::timeout(std::time::Duration::from_millis(900), rx.recv()).await.is_err(), "segundo aviso em seguida deve ser descartado");
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
