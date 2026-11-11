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

/// `livekit.json`: primeiro o da pasta de dados do usuário (o que a pessoa editou vence); sem ele, o que o instalador deixou ao lado do
/// executável (só URLs públicas, sem segredo; mesmo esquema do `supabase.json`).
fn read_file(data_dir: &Path) -> serde_json::Value {
    let exe_side = std::env::current_exe().ok().and_then(|e| e.parent().map(|d| d.join("livekit.json")));
    [Some(data_dir.join("livekit.json")), exe_side].into_iter().flatten()
        .find_map(|p| std::fs::read_to_string(p).ok().and_then(|t| if t.len() <= 8 * 1024 { serde_json::from_str::<serde_json::Value>(t.trim_start_matches('\u{feff}')).ok() } else { None }))
        .unwrap_or(serde_json::Value::Null)
}

/// `Ok(None)` = não configurado (modo P2P puro); `Err` = configurado mas inválido (aparece no diagnóstico, não derruba nada).
pub fn resolve(env: &dyn Fn(&str) -> Option<String>, data_dir: &Path) -> Result<Option<SfuConfig>, String> {
    let get = |k: &str| env(k).map(|v| v.trim().to_string()).filter(|v| !v.is_empty());
    let file = read_file(data_dir);
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

/// URL da função `livekit-kick` (expulsão de verdade no servidor de mídia): `BRIKS_LIVEKIT_KICK_URL`, `kick_url` no `livekit.json`
/// ou, sem nada disso, a irmã da função de tokens (`…/livekit-token` -> `…/livekit-kick`). None = não há como chamar (segue só o
/// aviso assinado pela camada de dados). Mesmas regras de esquema da URL de tokens.
pub fn kick_url(env: &dyn Fn(&str) -> Option<String>, data_dir: &Path, token_url: &str) -> Option<String> {
    let explicit = env("BRIKS_LIVEKIT_KICK_URL").map(|v| v.trim().to_string()).filter(|v| !v.is_empty()).or_else(|| {
        Some(read_file(data_dir)).and_then(|v| v["kick_url"].as_str().map(|s| s.trim().to_string())).filter(|v| !v.is_empty())
    });
    let u = match explicit { Some(u) => u, None => format!("{}/livekit-kick", token_url.strip_suffix("/livekit-token")?) };
    check_token_url(&u).ok().map(|_| u)
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
    let p = data_dir.join("net-mode.json");
    // preserva as outras chaves do arquivo (ex.: "fallback")
    let mut v = std::fs::read_to_string(&p).ok().and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok()).filter(|v| v.is_object()).unwrap_or_else(|| serde_json::json!({}));
    v["mode"] = serde_json::json!(m.name());
    std::fs::write(p, v.to_string())
}

/// Reserva P2P quando o SFU está configurado mas a entrada falha. DESLIGADA por padrão: quem caísse sozinho no P2P ficaria em
/// outra "sala" (a malha P2P e o SFU não se enxergam), falando com ninguém sem saber. Liga com `BRIKS_NET_FALLBACK=p2p` ou
/// `"fallback":"p2p"` no `net-mode.json` (útil só se TODOS os participantes combinarem o mesmo).
pub fn fallback_p2p(env: &dyn Fn(&str) -> Option<String>, data_dir: &Path) -> bool {
    if let Some(v) = env("BRIKS_NET_FALLBACK") { return v.trim().eq_ignore_ascii_case("p2p"); }
    std::fs::read_to_string(data_dir.join("net-mode.json")).ok()
        .and_then(|t| serde_json::from_str::<serde_json::Value>(&t).ok())
        .map(|v| v["fallback"].as_str().map(|f| f.eq_ignore_ascii_case("p2p")).unwrap_or(false)).unwrap_or(false)
}

/// Modo efetivo de uma chamada, sem salas divididas. `sfu_ready` = configuração presente e válida. `sfu_failed` = configurado, mas
/// o token não saiu ou o servidor não aceitou a entrada. `fallback` = reserva P2P ligada (ver `fallback_p2p`).
/// Devolve ("sfu" | "p2p" | "none", motivo para o diagnóstico); "none" = a chamada NÃO abre (erro visível, o usuário tenta de novo).
///
/// Regra: com o SFU configurado, a sala É do SFU, no Automático e no "SFU": só entra nele quem tem configuração válida E conseguiu
/// o token/entrada; quem não conseguiu não é jogado em silêncio no P2P. Sem configuração, tudo é P2P, como sempre.
pub fn decide(mode: NetMode, sfu_ready: bool, sfu_failed: bool, fallback: bool) -> (&'static str, String) {
    match (mode, sfu_ready, sfu_failed) {
        (NetMode::P2p, _, _) => ("p2p", "escolhido: P2P".into()),
        (NetMode::Sfu, true, false) => ("sfu", "escolhido: SFU".into()),
        (NetMode::Auto, true, false) => ("sfu", "automático: servidor de chamadas (SFU)".into()),
        (NetMode::Sfu, false, _) => ("p2p", "SFU escolhido, mas não configurado: usando P2P".into()),
        (NetMode::Auto, false, _) => ("p2p", "automático: SFU não configurado".into()),
        (_, true, true) if fallback => ("p2p", "servidor de chamadas indisponível: usando P2P (reserva ligada; só fala com quem também caiu no P2P)".into()),
        (_, true, true) => ("none", "servidor de chamadas indisponível: a chamada não foi aberta em P2P para não dividir a sala".into()),
    }
}

/// Preferência de conexão (Configurações > Rede): 0 Automático, 1 Internet direta, 2 VPN privada. Vale para as próximas conexões.
static NET_PREF: std::sync::atomic::AtomicU8 = std::sync::atomic::AtomicU8::new(0);
pub fn set_pref(p: u8) { NET_PREF.store(p.min(2), std::sync::atomic::Ordering::Relaxed); }
pub fn pref() -> u8 { NET_PREF.load(std::sync::atomic::Ordering::Relaxed) }

/// Adaptador de VPN privada conhecido (ZeroTier, Tailscale, WireGuard e túneis genéricos), pelo nome da interface.
pub fn is_vpn_iface(name: &str) -> bool {
    let n = name.to_ascii_lowercase();
    ["zerotier", "tailscale", "wireguard", "wintun", "openvpn", "tap-windows", "vpn"].iter().any(|k| n.contains(k)) || n.starts_with("wg") || n.starts_with("zt") || n.starts_with("tun") || n.starts_with("utun")
}

/// Filtro de interfaces do ICE conforme a preferência: "Internet direta" não oferece candidatos de adaptadores de VPN (o STUN/TURN
/// seguem valendo). Automático e "VPN privada" deixam todos os adaptadores: a prioridade entre eles é do ICE.
pub fn iface_allowed(name: &str) -> bool { pref() != 1 || !is_vpn_iface(name) }

#[cfg(test)]
mod t {
    use super::*;
    #[test]
    fn direct_pref_hides_vpn_interfaces() {
        assert!(super::is_vpn_iface("ZeroTier One [8056c2e21c000001]") && super::is_vpn_iface("Tailscale") && super::is_vpn_iface("wg0"));
        assert!(!super::is_vpn_iface("Ethernet") && !super::is_vpn_iface("Wi-Fi") && !super::is_vpn_iface("Realtek PCIe GbE"));
    }
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
    }

    #[test]
    fn kick_url_is_derived_from_the_token_url_or_set_explicitly() {
        let d = std::env::temp_dir().join(format!("briks-kickurl-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir_all(&d).unwrap();
        assert_eq!(kick_url(&env_of(vec![]), &d, "https://a.supabase.co/functions/v1/livekit-token").as_deref(), Some("https://a.supabase.co/functions/v1/livekit-kick"));
        assert_eq!(kick_url(&env_of(vec![]), &d, "https://a.b/outro-nome"), None, "sem nome conhecido não adivinha");
        assert_eq!(kick_url(&env_of(vec![("BRIKS_LIVEKIT_KICK_URL", "https://k.x/kick")]), &d, "https://a.b/outro").as_deref(), Some("https://k.x/kick"));
        assert_eq!(kick_url(&env_of(vec![("BRIKS_LIVEKIT_KICK_URL", "http://fora.com/kick")]), &d, "https://a.b/livekit-token"), None, "http só em loopback");
        std::fs::write(d.join("livekit.json"), r#"{"url":"wss://f.g","token_url":"https://h.i/t","kick_url":"https://h.i/k"}"#).unwrap();
        assert_eq!(kick_url(&env_of(vec![]), &d, "https://h.i/t").as_deref(), Some("https://h.i/k"));
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn auto_rule_never_splits_a_room() {
        // SFU é o padrão quando configurado e a entrada deu certo, no Automático e no "SFU"
        assert_eq!(decide(NetMode::Auto, true, false, false).0, "sfu");
        assert_eq!(decide(NetMode::Sfu, true, false, false).0, "sfu");
        // configurado, mas sem token/entrada: NÃO cai no P2P em silêncio (ficaria sozinho em outra sala)
        assert_eq!(decide(NetMode::Auto, true, true, false).0, "none");
        assert_eq!(decide(NetMode::Sfu, true, true, false).0, "none");
        // reserva só quando ligada de propósito
        assert_eq!(decide(NetMode::Auto, true, true, true).0, "p2p");
        assert_eq!(decide(NetMode::Sfu, true, true, true).0, "p2p");
        // sem configuração (ou inválida): P2P, como antes; a escolha P2P nunca tenta o SFU
        assert_eq!(decide(NetMode::Auto, false, false, false).0, "p2p");
        assert_eq!(decide(NetMode::Sfu, false, false, false).0, "p2p");
        for (ready, failed) in [(true, false), (true, true), (false, false)] { assert_eq!(decide(NetMode::P2p, ready, failed, false).0, "p2p"); }
    }

    #[test]
    fn fallback_is_off_by_default_and_survives_mode_changes() {
        let d = std::env::temp_dir().join(format!("briks-netfb-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir_all(&d).unwrap();
        assert!(!fallback_p2p(&env_of(vec![]), &d));
        assert!(fallback_p2p(&env_of(vec![("BRIKS_NET_FALLBACK", "p2p")]), &d));
        std::fs::write(d.join("net-mode.json"), r#"{"mode":"auto","fallback":"p2p"}"#).unwrap();
        assert!(fallback_p2p(&env_of(vec![]), &d));
        assert!(!fallback_p2p(&env_of(vec![("BRIKS_NET_FALLBACK", "off")]), &d), "ambiente vence o arquivo");
        save_mode(&d, NetMode::Sfu).unwrap();
        assert_eq!(load_mode(&env_of(vec![]), &d), NetMode::Sfu);
        assert!(fallback_p2p(&env_of(vec![]), &d), "trocar o modo não apaga a reserva");
        let _ = std::fs::remove_dir_all(&d);
    }
}
