//! Servidores ICE da malha WebRTC: STUN públicos por padrão; TURN (e mais STUN) por configuração, sem credenciais embutidas.
//! Fontes, nesta ordem: variável `BRIKS_ICE_SERVERS_JSON`, arquivo `ice.json` na pasta de dados, e só então o padrão.
//! Formato: [{"urls":"turn:host:3478","username":"u","credential":"c"}, {"urls":["stun:a:3478","stun:b:3478"]}]
//! O que o usuário configura é ACRESCENTADO aos STUN padrão. Sem TURN, redes simétricas/CGNAT podem não conectar.
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
        let (user, cred) = (e["username"].as_str().unwrap_or(""), e["credential"].as_str().unwrap_or(""));
        if urls.iter().any(|u| u.starts_with("turn")) && (user.is_empty() || cred.is_empty()) { return Err("TURN exige username e credential".into()); }
        if user.len() > 256 || cred.len() > 512 { return Err("credencial ICE grande demais".into()); }
        out.push(RTCIceServer { urls, username: user.into(), credential: cred.into(), ..Default::default() });
    }
    Ok(out)
}

/// STUN padrão + o que o usuário configurou. Erro de configuração não derruba a chamada: volta ao padrão e devolve o aviso.
pub fn load(env: &dyn Fn(&str) -> Option<String>, data_dir: &std::path::Path) -> (Vec<RTCIceServer>, Option<String>) {
    let mut servers = default_servers();
    let src = env("BRIKS_ICE_SERVERS_JSON").or_else(|| std::fs::read_to_string(data_dir.join("ice.json")).ok());
    let mut warn = None;
    if let Some(j) = src { match parse(&j) { Ok(extra) => servers.extend(extra), Err(e) => warn = Some(e) } }
    (servers, warn)
}

pub fn has_turn(s: &[RTCIceServer]) -> bool { s.iter().any(|x| x.urls.iter().any(|u| u.starts_with("turn"))) }

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
}
