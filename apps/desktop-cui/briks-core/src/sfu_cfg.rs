//! Configuração do modo SFU (LiveKit) e escolha do modo de rede (Automático / SFU / P2P).
//! Fontes, nesta ordem: variáveis `BRIKS_LIVEKIT_URL` / `BRIKS_LIVEKIT_TOKEN_URL` (ou as `VITE_*` do .env.example), depois o
//! arquivo `livekit.json` da pasta de dados: {"url":"wss://...","token_url":"https://.../functions/v1/livekit-token"}.
//! Nenhum segredo mora aqui: a URL do servidor e a do serviço de tokens são públicas; o token sai do Supabase com a sessão do usuário.
//! Esquemas: `wss://` e `https://`. `ws://` e `http://` só valem para loopback (servidor de desenvolvimento local).
use std::path::Path;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NetMode {
    Auto,
    Sfu,
    P2p,
}

impl NetMode {
    pub fn parse(s: &str) -> Option<NetMode> {
        match s.trim().to_ascii_lowercase().as_str() {
            "auto" | "automatico" | "automático" => Some(NetMode::Auto),
            "sfu" => Some(NetMode::Sfu),
            "p2p" => Some(NetMode::P2p),
            _ => None,
        }
    }
    pub fn name(self) -> &'static str {
        match self { NetMode::Auto => "auto", NetMode::Sfu => "sfu", NetMode::P2p => "p2p" }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SfuConfig {
    pub url: String,
    pub token_url: String,
}

fn host_of(u: &str) -> &str {
    let rest = u.split_once("://").map(|x| x.1).unwrap_or(u);
    let auth = rest.split(['/', '?', '#']).next().unwrap_or("");
    let auth = auth.rsplit('@').next().unwrap_or(auth);
    if let Some(r) = auth.strip_prefix('[') { return r.split(']').next().unwrap_or(""); }   // IPv6 literal
    auth.split(':').next().unwrap_or("")
}

pub fn is_loopback(u: &str) -> bool {
    let h = host_of(u).to_ascii_lowercase();
    h == "localhost" || h == "127.0.0.1" || h == "::1" || h.ends_with(".localhost")
}

fn check(u: &str, secure: &str, plain: &str, what: &str) -> Result<(), String> {
    if u.len() > 512 || u.chars().any(|c| c.is_whitespace() || c.is_control()) { return Err(format!("{what} inválida")); }
    if u.starts_with(secure) && host_of(u).len() > 0 { return Ok(()); }
    if u.starts_with(plain) && is_loopback(u) { return Ok(()); }
    Err(format!("{what}: use {secure}… ({plain}… só vale para localhost)"))
}

pub fn check_server_url(u: &str) -> Result<(), String> { check(u, "wss://", "ws://", "URL do servidor LiveKit") }
pub fn check_token_url(u: &str) -> Result<(), String> { check(u, "https://", "http://", "URL do serviço de tokens") }

/// `Ok(None)` = não configurado (modo P2P puro); `Err` = configurado mas inválido (aparece no diagnóstico, não derruba nada).
pub fn resolve(env: &dyn Fn(&str) -> Option<String>, data_dir: &Path) -> Result<Option<SfuConfig>, String> {
    let get = |k: &str| env(k).map(|v| v.trim().to_string()).filter(|v| !v.is_empty());
    let file: serde_json::Value = std::fs::read_to_string(data_dir.join("livekit.json")).ok()
        .and_then(|t| if t.len() <= 8 * 1024 { serde_json::from_str(t.trim_start_matches('\u{feff}')).ok() } else { None })
        .unwrap_or(serde_json::Value::Null);
    let fget = |k: &str| file[k].as_str().map(|v| v.trim().to_string()).filter(|v| !v.is_empty());
    let url = get("BRIKS_LIVEKIT_URL").or_else(|| get("VITE_LIVEKIT_URL")).or_else(|| fget("url"));
    let token_url = get("BRIKS_LIVEKIT_TOKEN_URL").or_else(|| get("VITE_LIVEKIT_TOKEN_URL")).or_else(|| fget("token_url"));
    match (url, token_url) {
        (None, None) => Ok(None),
        (Some(u), Some(t)) => { check_server_url(&u)?; check_token_url(&t)?; Ok(Some(SfuConfig { url: u, token_url: t })) }
        (Some(_), None) => Err("falta BRIKS_LIVEKIT_TOKEN_URL (serviço de tokens)".into()),
        (None, Some(_)) => Err("falta BRIKS_LIVEKIT_URL (servidor LiveKit)".into()),
    }
}

/// Modo escolhido pelo usuário: `BRIKS_NET_MODE` ou `net-mode.json` {"mode":"auto|sfu|p2p"} na pasta de dados (a UI grava esse arquivo
/// por `net.mode.set`). Padrão: Automático.
pub fn load_mode(env: &dyn Fn(&str) -> Option<String>, data_dir: &Path) -> NetMode {
    if let Some(m) = env("BRIKS_NET_MODE").and_then(|v| NetMode::parse(&v)) { return m; }
    std::fs::read_to_string(data_dir.join("net-mode.json")).ok()
        .and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok())
        .and_then(|v| v["mode"].as_str().and_then(NetMode::parse))
        .unwrap_or(NetMode::Auto)
}

pub fn save_mode(data_dir: &Path, m: NetMode) -> std::io::Result<()> {
    std::fs::create_dir_all(data_dir)?;
    std::fs::write(data_dir.join("net-mode.json"), serde_json::json!({ "mode": m.name() }).to_string())
}

/// Modo efetivo de uma chamada. `sfu_ready` = configurado e válido. `sfu_failed` = a tentativa de entrar no SFU falhou (reserva P2P).
/// Devolve ("sfu" | "p2p", motivo para o diagnóstico).
pub fn decide(mode: NetMode, sfu_ready: bool, sfu_failed: bool) -> (&'static str, String) {
    match (mode, sfu_ready, sfu_failed) {
        (NetMode::P2p, _, _) => ("p2p", "escolhido: P2P".into()),
        (NetMode::Sfu, true, false) => ("sfu", "escolhido: SFU".into()),
        (NetMode::Sfu, false, _) => ("p2p", "SFU escolhido, mas não configurado: usando P2P".into()),
        (NetMode::Sfu, true, true) => ("p2p", "SFU escolhido, mas indisponível: usando P2P".into()),
        (NetMode::Auto, true, false) => ("sfu", "automático: SFU configurado".into()),
        (NetMode::Auto, true, true) => ("p2p", "automático: SFU indisponível, reserva P2P".into()),
        (NetMode::Auto, false, _) => ("p2p", "automático: SFU não configurado".into()),
    }
}

#[cfg(test)]
mod t {
    use super::*;
    fn env_of(v: Vec<(&'static str, &'static str)>) -> impl Fn(&str) -> Option<String> { move |k| v.iter().find(|(a, _)| *a == k).map(|(_, b)| b.to_string()) }

    #[test]
    fn schemes_are_validated() {
        assert!(check_server_url("wss://x.livekit.cloud").is_ok());
        assert!(check_server_url("ws://127.0.0.1:7880").is_ok());
        assert!(check_server_url("ws://[::1]:7880").is_ok());
        assert!(check_server_url("ws://exemplo.com").is_err(), "ws só em loopback");
        assert!(check_server_url("ws://localhost.evil.com").is_err());
        assert!(check_server_url("https://x").is_err());
        assert!(check_server_url("wss://").is_err());
        assert!(check_token_url("https://a.supabase.co/functions/v1/livekit-token").is_ok());
        assert!(check_token_url("http://a.supabase.co/x").is_err());
        assert!(check_token_url("http://localhost:9/x").is_ok());
        assert!(check_token_url("https://a b").is_err());
    }

    #[test]
    fn resolves_from_env_then_file_and_needs_both() {
        let d = std::env::temp_dir().join(format!("briks-sfucfg-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir_all(&d).unwrap();
        assert_eq!(resolve(&env_of(vec![]), &d), Ok(None));
        assert!(resolve(&env_of(vec![("BRIKS_LIVEKIT_URL", "wss://a.b")]), &d).is_err());
        let c = resolve(&env_of(vec![("BRIKS_LIVEKIT_URL", "wss://a.b"), ("VITE_LIVEKIT_TOKEN_URL", "https://c.d/t")]), &d).unwrap().unwrap();
        assert_eq!(c, SfuConfig { url: "wss://a.b".into(), token_url: "https://c.d/t".into() });
        std::fs::write(d.join("livekit.json"), r#"{"url":"wss://f.g","token_url":"https://h.i/t"}"#).unwrap();
        assert_eq!(resolve(&env_of(vec![]), &d).unwrap().unwrap().url, "wss://f.g");
        assert_eq!(resolve(&env_of(vec![("BRIKS_LIVEKIT_URL", "wss://env.x")]), &d).unwrap().unwrap().url, "wss://env.x", "ambiente vence o arquivo");
        std::fs::write(d.join("livekit.json"), r#"{"url":"http://a","token_url":"https://h.i/t"}"#).unwrap();
        assert!(resolve(&env_of(vec![]), &d).is_err());
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn mode_persistence_and_decision() {
        let d = std::env::temp_dir().join(format!("briks-netmode-{}", uuid::Uuid::new_v4()));
        assert_eq!(load_mode(&env_of(vec![]), &d), NetMode::Auto);
        save_mode(&d, NetMode::Sfu).unwrap();
        assert_eq!(load_mode(&env_of(vec![]), &d), NetMode::Sfu);
        assert_eq!(load_mode(&env_of(vec![("BRIKS_NET_MODE", "p2p")]), &d), NetMode::P2p);
        let _ = std::fs::remove_dir_all(&d);
        assert_eq!(decide(NetMode::Auto, true, false).0, "sfu");
        assert_eq!(decide(NetMode::Auto, true, true).0, "p2p");
        assert_eq!(decide(NetMode::Auto, false, false).0, "p2p");
        assert_eq!(decide(NetMode::Sfu, false, false).0, "p2p");
        assert_eq!(decide(NetMode::P2p, true, false).0, "p2p");
    }
}
