//! Modo "somente local" para testes: `BRIKS_NET_LOOPBACK_ONLY=1` (ou `netmode::set_loopback_only(true)` dentro de um teste).
//!
//! Objetivo: nenhum processo de teste escuta em 0.0.0.0 / IP de rede nem fala com a internet, para o Firewall do Windows não perguntar
//! "permitir acesso?" a cada exe novo. Em produção (variável ausente) nada aqui muda o comportamento.
//!
//! Com o modo ligado:
//!   - ICE (webrtc-rs): só candidatos host em 127.0.0.1/::1 (filtro de IP + loopback incluído), mDNS desligado (o padrão `QueryOnly`
//!     abre 0.0.0.0:5353), e os servidores STUN/TURN que não sejam loopback são descartados (coletar srflx/relay abriria 0.0.0.0:porta);
//!   - HTTP/WebSocket de saída (Realtime, contas/Supabase, turn-credentials, livekit-token/kick, foto de perfil, prévia de link): só
//!     destinos loopback (`deny_url`);
//!   - o resto dos sockets do core já vive em 127.0.0.1 (IPC, mocks, loopback OAuth, transferência de arquivo pelo DataChannel).
//!
//! Teste que precisa de rede de verdade: `#[ignore]` e/ou `BRIKS_TEST_REAL_NET=1` (veja `real_net_wanted`).

use std::sync::atomic::{AtomicU8, Ordering};
use webrtc::api::setting_engine::SettingEngine;
use webrtc::ice::mdns::MulticastDnsMode;
use webrtc::ice_transport::ice_server::RTCIceServer;

/// 0 = segue a variável de ambiente; 1 = forçado ligado; 2 = forçado desligado (só testes).
static FORCE: AtomicU8 = AtomicU8::new(0);

pub fn loopback_only() -> bool {
    match FORCE.load(Ordering::Relaxed) {
        1 => true,
        2 => false,
        // `cargo test` liga sozinho (o binário de teste nunca deve tocar a rede); BRIKS_NET_LOOPBACK_ONLY=0 ou BRIKS_TEST_REAL_NET=1 desligam
        _ => match std::env::var("BRIKS_NET_LOOPBACK_ONLY") {
            Ok(v) => v == "1",
            Err(_) => cfg!(test) && std::env::var("BRIKS_TEST_REAL_NET").map(|v| v != "1").unwrap_or(true),
        },
    }
}

#[allow(dead_code)]
pub fn set_loopback_only(on: bool) { FORCE.store(if on { 1 } else { 2 }, Ordering::Relaxed); }

/// Testes que exigem STUN/TURN/internet de verdade só rodam com `BRIKS_TEST_REAL_NET=1` (e nunca junto com o modo local).
#[allow(dead_code)]
pub fn real_net_wanted() -> bool { !loopback_only() && std::env::var("BRIKS_TEST_REAL_NET").map(|v| v == "1").unwrap_or(false) }

/// Aplica o modo ao `SettingEngine` do webrtc-rs (no-op fora do modo).
pub fn apply(se: &mut SettingEngine) {
    if !loopback_only() { return; }
    se.set_ip_filter(Box::new(|ip| ip.is_loopback()));
    se.set_include_loopback_candidate(true);
    se.set_ice_multicast_dns_mode(MulticastDnsMode::Disabled);
}

fn host_is_loopback(host: &str) -> bool {
    let h = host.trim_start_matches('[').trim_end_matches(']').to_ascii_lowercase();
    h == "localhost" || h == "::1" || h.parse::<std::net::IpAddr>().map(|i| i.is_loopback()).unwrap_or(false)
}

/// Host de uma URL (`https://h:443/x`, `turn:h:3478?x`, `stun:[::1]:3478`).
fn url_host(u: &str) -> &str {
    let rest = u.split_once("://").map(|(_, r)| r).or_else(|| u.split_once(':').map(|(_, r)| r)).unwrap_or(u);
    let rest = rest.split(['/', '?']).next().unwrap_or("");
    let rest = rest.rsplit('@').next().unwrap_or(rest);
    if let Some(r) = rest.strip_prefix('[') { return r.split(']').next().unwrap_or(""); }
    rest.split(':').next().unwrap_or("")
}

/// `Some(motivo)` se o modo local proíbe falar com `url` (host que não é loopback). Sempre `None` fora do modo.
pub fn deny_url(url: &str) -> Option<String> {
    if !loopback_only() || host_is_loopback(url_host(url)) { return None; }
    Some("rede externa bloqueada (BRIKS_NET_LOOPBACK_ONLY=1)".into())
}

/// Servidores STUN/TURN que o modo local deixa usar: só os de loopback (todos, fora do modo).
pub fn ice_servers(list: &[RTCIceServer]) -> Vec<RTCIceServer> {
    if !loopback_only() { return list.to_vec(); }
    list.iter().filter(|s| !s.urls.is_empty() && s.urls.iter().all(|u| host_is_loopback(url_host(u)))).cloned().collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn hosts_e_filtros() {
        assert_eq!(url_host("https://a.supabase.co/functions/v1/x"), "a.supabase.co");
        assert_eq!(url_host("http://127.0.0.1:7880"), "127.0.0.1");
        assert_eq!(url_host("ws://[::1]:9/x"), "::1");
        assert_eq!(url_host("turn:t.example.com:3478?transport=udp"), "t.example.com");
        assert_eq!(url_host("turn:[::1]:3478"), "::1");
        assert!(host_is_loopback("localhost") && host_is_loopback("127.0.0.2") && host_is_loopback("::1"));
        assert!(!host_is_loopback("10.0.0.2") && !host_is_loopback("0.0.0.0") && !host_is_loopback("a.com"));
    }
    #[test]
    fn modo_local_bloqueia_o_externo() {
        set_loopback_only(true);
        assert!(deny_url("https://x.supabase.co/").is_some() && deny_url("http://127.0.0.1:1/").is_none() && deny_url("wss://[::1]:2/").is_none());
        let mk = |u: &str| RTCIceServer { urls: vec![u.into()], ..Default::default() };
        let r = ice_servers(&[mk("stun:stun.l.google.com:19302"), mk("turn:127.0.0.1:9?transport=udp")]);
        assert_eq!(r.len(), 1);
        set_loopback_only(false);
        assert!(deny_url("https://x.supabase.co/").is_none());
        assert_eq!(ice_servers(&[mk("stun:a:1"), mk("stun:b:1")]).len(), 2);
        FORCE.store(0, Ordering::Relaxed);
    }
}
