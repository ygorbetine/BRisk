//! Servidores ICE da malha WebRTC: STUN públicos por padrão; TURN (e mais STUN) por configuração, sem credenciais embutidas.
//! Fontes, nesta ordem: variável `BRIKS_ICE_SERVERS_JSON`, arquivo `ice.json` na pasta de dados, e só então o padrão.
//! Formato: [{"urls":"turn:host:3478","username":"u","credential":"c"}, {"urls":["stun:a:3478","stun:b:3478"]}]
//! O que o usuário configura é ACRESCENTADO aos STUN padrão. Sem TURN, redes simétricas/CGNAT podem não conectar.
//!
//! A tela Rede grava o `ice.json` por aqui (`save`): a senha do TURN vai cifrada com a DPAPI no campo
//! `credential_dpapi` (um arquivo escrito à mão com `credential` em texto continua valendo). Nada daqui escreve
//! credencial em log, evento ou mensagem de erro: a UI só recebe "há senha guardada".
use std::path::Path;
use std::time::Duration;

use base64::{engine::general_purpose::STANDARD as B64, Engine};
use serde_json::Value;
use webrtc::ice_transport::ice_server::RTCIceServer;

const DEFAULT_STUN: [&str; 3] = ["stun:stun.l.google.com:19302", "stun:stun1.l.google.com:19302", "stun:stun.cloudflare.com:3478"];
const MAX_SERVERS: usize = 8;
const MAX_URLS: usize = 4;
const MAX_URL_LEN: usize = 256;
const MAX_JSON: usize = 8 * 1024;

pub fn default_servers() -> Vec<RTCIceServer> {
    DEFAULT_STUN.iter().map(|u| RTCIceServer { urls: vec![u.to_string()], ..Default::default() }).collect()
}

fn valid_url(u: &str) -> bool {
    let ok_scheme = ["stun:", "stuns:", "turn:", "turns:"].iter().any(|p| u.starts_with(p));
    ok_scheme && u.len() <= MAX_URL_LEN && u.len() > 6 && !u.chars().any(|c| c.is_whitespace() || c.is_control())
}

pub fn parse(json: &str) -> Result<Vec<RTCIceServer>, String> {
    if json.len() > MAX_JSON { return Err("JSON ICE grande demais".into()); }
    let v: Value = serde_json::from_str(json.trim_start_matches('\u{feff}')).map_err(|e| format!("JSON ICE inválido: {e}"))?;
    let arr = v.as_array().ok_or("JSON ICE deve ser uma lista")?;
    if arr.len() > MAX_SERVERS { return Err("servidores ICE demais".into()); }
    let mut out = Vec::new();
    for e in arr {
        let urls: Vec<String> = match &e["urls"] {
            Value::String(s) => vec![s.clone()],
            Value::Array(a) => a.iter().filter_map(|x| x.as_str().map(String::from)).collect(),
            _ => return Err("entrada ICE sem urls".into()),
        };
        if urls.is_empty() || urls.len() > MAX_URLS || !urls.iter().all(|u| valid_url(u)) { return Err("URL ICE inválida (use stun:, turn: ou turns:)".into()); }
        let user = e["username"].as_str().unwrap_or("");
        let cred_owned = match (e["credential"].as_str(), e["credential_dpapi"].as_str()) {
            (Some(c), _) => c.to_string(),
            (None, Some(sealed)) => unseal(sealed).ok_or("a senha do TURN guardada não abre nesta conta do Windows; salve de novo na tela Rede")?,
            _ => String::new(),
        };
        let cred = cred_owned.as_str();
        if urls.iter().any(|u| u.starts_with("turn")) && (user.is_empty() || cred.is_empty()) { return Err("TURN exige username e credential".into()); }
        if user.len() > 256 || cred.len() > 512 { return Err("credencial ICE grande demais".into()); }
        out.push(RTCIceServer { urls, username: user.into(), credential: cred.into(), ..Default::default() });
    }
    Ok(out)
}

/// STUN padrão + o que o usuário configurou. Erro de configuração não derruba a chamada: volta ao padrão e devolve o aviso.
pub fn load(env: &dyn Fn(&str) -> Option<String>, data_dir: &Path) -> (Vec<RTCIceServer>, Option<String>) {
    let mut servers = default_servers();
    let src = env("BRIKS_ICE_SERVERS_JSON").or_else(|| std::fs::read_to_string(data_dir.join("ice.json")).ok());
    let mut warn = None;
    if let Some(j) = src { match parse(&j) { Ok(extra) => servers.extend(extra), Err(e) => warn = Some(e) } }
    (servers, warn)
}

pub fn has_turn(s: &[RTCIceServer]) -> bool { s.iter().any(|x| x.urls.iter().any(|u| u.starts_with("turn"))) }

fn seal(plain: &str) -> Option<String> {
    use crate::accounts::store::{Dpapi, Protector};
    Dpapi.protect(plain.as_bytes()).ok().map(|b| B64.encode(b))
}
fn unseal(sealed: &str) -> Option<String> {
    use crate::accounts::store::{Dpapi, Protector};
    String::from_utf8(Dpapi.unprotect(&B64.decode(sealed).ok()?).ok()?).ok()
}

/// Confere um endereço digitado na tela Rede. Erro = frase pronta para mostrar.
pub fn check_url(u: &str) -> Result<(), String> {
    let Some((scheme, rest)) = u.split_once(':') else { return Err("O endereço precisa começar com stun:, turn: ou turns: (exemplo: turn:meuservidor.com:3478).".into()) };
    if !["stun", "stuns", "turn", "turns"].contains(&scheme) { return Err("O endereço precisa começar com stun:, turn: ou turns:.".into()); }
    if rest.starts_with("//") { return Err("Sem // no endereço: escreva turn:meuservidor.com:3478.".into()); }
    if !valid_url(u) { return Err("Endereço inválido: sem espaços e com até 256 caracteres.".into()); }
    let (addr, query) = rest.split_once('?').unwrap_or((rest, ""));
    if !query.is_empty() && !["transport=udp", "transport=tcp"].contains(&query) { return Err("Depois de ? só vale transport=udp ou transport=tcp.".into()); }
    let (host, port) = match addr.rfind(':') {
        Some(i) if !addr.ends_with(']') => (&addr[..i], Some(&addr[i + 1..])),   // [::1] sem porta termina em ]
        _ => (addr, None),
    };
    if host.is_empty() || host.contains('@') || host.contains('/') { return Err("Falta o nome do servidor (exemplo: turn:meuservidor.com:3478).".into()); }
    if let Some(p) = port { if !matches!(p.parse::<u32>(), Ok(1..=65535)) { return Err("A porta precisa ser um número entre 1 e 65535.".into()); } }
    Ok(())
}

/// Grava o servidor digitado na tela Rede em `ice.json` (substitui o que havia). `urls`: um endereço ou vários separados
/// por vírgula. Senha vazia mantém a que já estava guardada para o mesmo usuário.
pub fn save(data_dir: &Path, urls: &str, username: &str, credential: &str) -> Result<(), String> {
    let urls: Vec<String> = urls.split(|c: char| c == ',' || c.is_whitespace()).map(str::trim).filter(|u| !u.is_empty()).map(String::from).collect();
    if urls.is_empty() { return Err("Digite o endereço do servidor, por exemplo turn:meuservidor.com:3478.".into()); }
    if urls.len() > MAX_URLS { return Err("São no máximo 4 endereços.".into()); }
    for u in &urls { check_url(u)?; }
    let username = username.trim();
    let turn = urls.iter().any(|u| u.starts_with("turn"));
    let path = data_dir.join("ice.json");
    let mut entry = serde_json::json!({ "urls": urls });
    if turn {
        if username.is_empty() { return Err("Servidor TURN precisa de usuário.".into()); }
        if username.len() > 256 || credential.len() > 512 { return Err("Usuário ou senha grande demais.".into()); }
        let sealed = if credential.is_empty() {
            // editar só o endereço: reaproveita a senha já guardada para este usuário
            let old: Value = std::fs::read_to_string(&path).ok().and_then(|j| serde_json::from_str(&j).ok()).unwrap_or(Value::Null);
            let e = &old[0];
            if e["username"].as_str() != Some(username) { return Err("Servidor TURN precisa de senha.".into()); }
            match (e["credential_dpapi"].as_str(), e["credential"].as_str()) {
                (Some(s), _) => s.to_string(),
                (None, Some(c)) => seal(c).ok_or("Não foi possível guardar a senha com segurança neste computador.")?,
                _ => return Err("Servidor TURN precisa de senha.".into()),
            }
        } else {
            seal(credential).ok_or("Não foi possível guardar a senha com segurança neste computador.")?
        };
        entry["username"] = username.into();
        entry["credential_dpapi"] = sealed.into();
    }
    let text = Value::Array(vec![entry]).to_string();
    parse(&text)?;   // o que foi gravado tem de ser exatamente o que o carregador aceita
    let _ = std::fs::create_dir_all(data_dir);
    let tmp = data_dir.join("ice.json.tmp");
    std::fs::write(&tmp, text).and_then(|_| std::fs::rename(&tmp, &path)).map_err(|_| "Não foi possível gravar a configuração de rede.".to_string())
}

pub fn clear(data_dir: &Path) -> Result<(), String> {
    match std::fs::remove_file(data_dir.join("ice.json")) {
        Ok(()) => Ok(()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(_) => Err("Não foi possível remover a configuração de rede.".into()),
    }
}

/// Situação para a tela Rede (evento `ice.state`). Nunca inclui a senha: só "há senha guardada".
pub fn summary(env: &dyn Fn(&str) -> Option<String>, data_dir: &Path) -> Value {
    let from_env = env("BRIKS_ICE_SERVERS_JSON").is_some();
    let has_file = data_dir.join("ice.json").exists();
    let (servers, warn) = load(env, data_dir);
    let extra = servers.get(DEFAULT_STUN.len());
    serde_json::json!({
        "ev": "ice.state",
        "turn": has_turn(&servers),
        "source": if from_env { "env" } else if has_file { "file" } else { "default" },
        "urls": extra.map(|e| e.urls.join(", ")).unwrap_or_default(),
        "username": extra.map(|e| e.username.clone()).unwrap_or_default(),
        "has_credential": extra.map(|e| !e.credential.is_empty()).unwrap_or(false),
        "servers": servers.len(),
        "warn": warn.unwrap_or_default(),
    })
}

/// Teste de conectividade: junta candidatos ICE de verdade com os servidores em uso e conta o que voltou.
/// srflx = um STUN respondeu (endereço público descoberto); relay = o TURN aceitou usuário e senha e cedeu um relé.
/// Não devolve endereços (nem o IP público) e não fala com ninguém além dos próprios servidores.
pub async fn probe(servers: Vec<RTCIceServer>, timeout: Duration) -> Value {
    use webrtc::ice_transport::ice_candidate_type::RTCIceCandidateType as T;
    let turn = has_turn(&servers);
    let t0 = std::time::Instant::now();
    let counts = std::sync::Arc::new(std::sync::Mutex::new([0u32; 3]));   // host, srflx, relay
    let done = std::sync::Arc::new(tokio::sync::Notify::new());
    let run = async {
        let api = webrtc::api::APIBuilder::new().build();
        let pc = api.new_peer_connection(webrtc::peer_connection::configuration::RTCConfiguration { ice_servers: servers, ..Default::default() }).await?;
        let (c2, d2) = (counts.clone(), done.clone());
        pc.on_ice_candidate(Box::new(move |c| {
            match c {
                Some(c) => {
                    let mut g = c2.lock().unwrap();
                    match c.typ { T::Host => g[0] += 1, T::Srflx | T::Prflx => g[1] += 1, T::Relay => g[2] += 1, _ => {} }
                    if (turn && g[2] > 0) || (!turn && g[1] > 0) { d2.notify_one(); }   // já dá para responder
                }
                None => d2.notify_one(),   // coleta terminada
            }
            Box::pin(async {})
        }));
        pc.create_data_channel("teste", None).await?;
        let offer = pc.create_offer(None).await?;
        pc.set_local_description(offer).await?;
        let _ = tokio::time::timeout(timeout, done.notified()).await;
        let _ = pc.close().await;
        Ok::<(), webrtc::Error>(())
    };
    let failed = run.await.is_err();
    let [host, srflx, relay] = *counts.lock().unwrap();
    let (level, message) = if failed {
        ("none", "O teste não pôde ser iniciado neste computador.")
    } else if relay > 0 {
        ("relay", "O TURN respondeu: as chamadas conectam mesmo em redes restritivas.")
    } else if turn && srflx > 0 {
        ("stun", "O TURN não respondeu (endereço, usuário ou senha podem estar errados), mas o STUN sim: redes comuns conectam.")
    } else if turn {
        ("none", "Nem o TURN nem o STUN responderam. Confira a internet, o firewall e os dados do servidor.")
    } else if srflx > 0 {
        ("stun", "O STUN respondeu: redes comuns conectam. Sem TURN, redes restritivas (CGNAT, empresa, 4G) podem não conectar.")
    } else {
        ("none", "Nenhum servidor respondeu. Confira a internet ou o firewall (UDP bloqueado impede as chamadas).")
    };
    serde_json::json!({ "ev": "ice.test", "level": level, "message": message, "turn": turn, "host": host, "srflx": srflx, "relay": relay, "ms": t0.elapsed().as_millis() as u64 })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn defaults_are_stun_only() { let d = default_servers(); assert_eq!(d.len(), 3); assert!(!has_turn(&d)); }
    #[test]
    fn parses_turn_and_stun() {
        let s = parse(r#"[{"urls":"turn:t.example.com:3478","username":"u","credential":"c"},{"urls":["stun:a:3478","stuns:b:5349"]}]"#).unwrap();
        assert_eq!(s.len(), 2); assert!(has_turn(&s));
    }
    #[test]
    fn rejects_bad_schemes_missing_credentials_and_size() {
        assert!(parse(r#"[{"urls":"http://x"}]"#).is_err());
        assert!(parse(r#"[{"urls":"turn:t:3478"}]"#).is_err());
        assert!(parse(r#"{"urls":"stun:a:1"}"#).is_err());
        assert!(parse(r#"[{"urls":"stun:a b:1"}]"#).is_err());
        let many = format!("[{}]", vec![r#"{"urls":"stun:a:1"}"#; 9].join(","));
        assert!(parse(&many).is_err());
        assert!(parse(&"x".repeat(MAX_JSON + 1)).is_err());
    }
    #[test]
    fn load_adds_to_defaults_and_bad_config_falls_back() {
        let dir = std::env::temp_dir();
        let env = |k: &str| (k == "BRIKS_ICE_SERVERS_JSON").then(|| r#"[{"urls":"turn:t:3478","username":"u","credential":"c"}]"#.to_string());
        let (s, w) = load(&env, &dir); assert_eq!(s.len(), 4); assert!(w.is_none() && has_turn(&s));
        let bad = |k: &str| (k == "BRIKS_ICE_SERVERS_JSON").then(|| "lixo".to_string());
        let (s, w) = load(&bad, &dir); assert_eq!(s.len(), 3); assert!(w.is_some());
    }
    fn tmp(n: &str) -> std::path::PathBuf {
        let p = std::env::temp_dir().join(format!("briks-ice-{n}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&p); std::fs::create_dir_all(&p).unwrap(); p
    }
    fn noenv(_: &str) -> Option<String> { None }
    #[test]
    fn form_urls_get_clear_errors() {
        for ok in ["turn:t.example.com:3478", "turns:t.example.com:5349?transport=tcp", "stun:s.example.com", "turn:[2001:db8::1]:3478", "turn:10.0.0.2:3478?transport=udp"] { assert_eq!(check_url(ok), Ok(()), "{ok}"); }
        for bad in ["t.example.com", "http://t.example.com", "turn://t.example.com:3478", "turn:", "turn::3478", "turn:t.example.com:0", "turn:t.example.com:99999", "turn:t.example.com:abc", "turn:u@t.example.com:3478", "turn:t.example.com:3478?x=1", "turn:a b:1"] {
            let e = check_url(bad).unwrap_err(); assert!(!e.is_empty() && e.ends_with('.'), "{bad}: {e}");
        }
    }
    #[test]
    fn saved_turn_is_loaded_back_and_the_password_never_sits_in_clear() {
        let d = tmp("save");
        assert!(save(&d, "", "u", "c").is_err());
        assert!(save(&d, "turn:t.example.com:3478", "", "c").unwrap_err().contains("usuário"));
        assert!(save(&d, "turn:t.example.com:3478", "ana", "").unwrap_err().contains("senha"));
        assert!(!d.join("ice.json").exists());
        save(&d, "turn:t.example.com:3478, turns:t.example.com:5349", "ana", "senha-do-turn-123").unwrap();
        let raw = std::fs::read_to_string(d.join("ice.json")).unwrap();
        assert!(!raw.contains("senha-do-turn-123") && raw.contains("credential_dpapi") && !raw.contains("\"credential\""));
        let (s, w) = load(&noenv, &d);
        assert!(w.is_none() && has_turn(&s) && s.len() == 4);
        assert_eq!((s[3].urls.len(), s[3].username.as_str(), s[3].credential.as_str()), (2, "ana", "senha-do-turn-123"));
        // a UI só fica sabendo que há senha; trocar o endereço sem digitar a senha de novo mantém a guardada
        let v = summary(&noenv, &d);
        assert_eq!((v["turn"].as_bool(), v["source"].as_str(), v["username"].as_str(), v["has_credential"].as_bool()), (Some(true), Some("file"), Some("ana"), Some(true)));
        assert!(!v.to_string().contains("senha-do-turn-123"));
        save(&d, "turn:outro.example.com:3478", "ana", "").unwrap();
        let (s, _) = load(&noenv, &d);
        assert_eq!((s[3].urls[0].as_str(), s[3].credential.as_str()), ("turn:outro.example.com:3478", "senha-do-turn-123"));
        assert!(save(&d, "turn:outro.example.com:3478", "bia", "").is_err(), "outro usuário exige senha");
        // só STUN extra: sem credencial alguma
        save(&d, "stun:s.example.com:3478", "", "").unwrap();
        let v = summary(&noenv, &d);
        assert_eq!((v["turn"].as_bool(), v["urls"].as_str(), v["has_credential"].as_bool()), (Some(false), Some("stun:s.example.com:3478"), Some(false)));
        clear(&d).unwrap(); clear(&d).unwrap();
        assert_eq!(summary(&noenv, &d)["source"], "default");
    }
    #[test]
    fn hand_written_file_and_env_still_work_and_a_foreign_sealed_password_is_reported() {
        let d = tmp("hand");
        std::fs::write(d.join("ice.json"), r#"[{"urls":"turn:t:3478","username":"u","credential":"c"}]"#).unwrap();
        assert!(has_turn(&load(&noenv, &d).0));
        std::fs::write(d.join("ice.json"), r#"[{"urls":"turn:t:3478","username":"u","credential_dpapi":"bGl4bw=="}]"#).unwrap();
        let (s, w) = load(&noenv, &d);
        assert!(!has_turn(&s) && w.unwrap().contains("salve de novo"));
        let env = |k: &str| (k == "BRIKS_ICE_SERVERS_JSON").then(|| r#"[{"urls":"stun:a:1"}]"#.to_string());
        assert_eq!(summary(&env, &d)["source"], "env");
    }
    #[tokio::test]
    async fn probe_reports_a_dead_turn_without_hanging_or_leaking_the_password() {
        // porta 9 em loopback: ninguém responde. O teste termina no prazo e diz que o TURN não respondeu.
        let dead = vec![RTCIceServer { urls: vec!["turn:127.0.0.1:9?transport=udp".into()], username: "u".into(), credential: "segredo-do-teste".into(), ..Default::default() }];
        let t0 = std::time::Instant::now();
        let v = probe(dead, Duration::from_millis(1500)).await;
        assert!(t0.elapsed() < Duration::from_secs(8));
        assert_eq!((v["ev"].as_str(), v["turn"].as_bool(), v["relay"].as_u64()), (Some("ice.test"), Some(true), Some(0)));
        assert_ne!(v["level"], "relay");
        assert!(!v.to_string().contains("segredo-do-teste") && !v["message"].as_str().unwrap().is_empty());
    }
}
