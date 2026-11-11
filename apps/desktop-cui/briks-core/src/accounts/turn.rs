//! Credenciais TURN da conta: depois do login o app pede à Edge Function `turn-credentials` do projeto (GET com o JWT da
//! sessão) as credenciais temporárias do relay, renova antes de vencerem e as entrega ao ICE. Sem login, sem função
//! publicada ou com qualquer falha: silêncio e só STUN (a tela Rede mostra "Somente STUN"). O que o usuário configurou
//! (ice.json / BRIKS_ICE_SERVERS_JSON) tem prioridade (ver `ice::load_with`).
//! A credencial só vive na memória deste processo; nada é gravado em disco nem aparece em eventos, logs ou erros.
use std::sync::{Arc, Mutex};
use std::time::Duration;

use serde_json::Value;
use webrtc::ice_transport::ice_server::RTCIceServer;

use super::Accounts;

fn now() -> u64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

#[derive(Default)]
pub struct TurnCache {
    inner: Mutex<Option<(Vec<RTCIceServer>, u64)>>,
}

impl TurnCache {
    /// Servidores ainda válidos (com 30 s de folga) ou vazio.
    pub fn servers(&self) -> Vec<RTCIceServer> {
        match &*self.inner.lock().unwrap() {
            Some((s, exp)) if *exp > now() + 30 => s.clone(),
            _ => Vec::new(),
        }
    }
    pub fn set(&self, servers: Vec<RTCIceServer>, expires_at: u64) {
        *self.inner.lock().unwrap() = Some((servers, expires_at));
    }
    pub fn clear(&self) {
        *self.inner.lock().unwrap() = None;
    }
}

/// Pede as credenciais. Erro = frase curta sem dados sensíveis (só diagnóstico; a UI não a mostra).
pub fn fetch(base: &str, anon_key: &str, access_token: &str) -> Result<(Vec<RTCIceServer>, u64, u64), String> {
    let agent = ureq::AgentBuilder::new().timeout_connect(Duration::from_secs(5)).timeout(Duration::from_secs(10)).redirects(0).build();
    let r = agent.get(&format!("{}/functions/v1/turn-credentials", base.trim_end_matches('/')))
        .set("apikey", anon_key).set("Authorization", &format!("Bearer {access_token}")).set("Accept", "application/json")
        .call().map_err(|e| match e { ureq::Error::Status(c, _) => format!("a função respondeu {c}"), ureq::Error::Transport(_) => "sem resposta da função".to_string() })?;
    let text = r.into_string().map_err(|_| "resposta ilegível".to_string())?;
    let v: Value = serde_json::from_str(&text).map_err(|_| "resposta inválida".to_string())?;
    let servers = crate::ice::parse(&v["iceServers"].to_string())?;
    if !crate::ice::has_turn(&servers) {
        return Err("a resposta não trouxe TURN".into());
    }
    let ttl = v["ttl"].as_u64().unwrap_or(3600).clamp(300, 3600);
    let expires = v["expiresAt"].as_u64().filter(|e| *e > now()).unwrap_or_else(|| now() + ttl);
    Ok((servers, expires, ttl))
}

/// Quanto esperar até renovar: 5 min antes de vencer (ou metade do prazo, se for curto).
pub fn renew_after(ttl: u64) -> Duration {
    Duration::from_secs(if ttl > 600 { ttl - 300 } else { (ttl / 2).max(60) })
}

/// Um passo do ciclo; devolve quanto dormir. Sem sessão limpa o cache; falha é silenciosa e tenta de novo em 5 min.
pub async fn step(acc: &Accounts, cache: &TurnCache, base: &str, anon_key: &str) -> Duration {
    if acc.user().is_none() {
        cache.clear();
        return Duration::from_secs(3);
    }
    let Ok(token) = acc.access_token().await else { return Duration::from_secs(30) };
    let (base, key) = (base.to_string(), anon_key.to_string());
    match tokio::task::spawn_blocking(move || fetch(&base, &key, &token)).await {
        Ok(Ok((servers, expires, ttl))) => {
            cache.set(servers, expires);
            renew_after(ttl)
        }
        _ => {
            if cache.servers().is_empty() {
                cache.clear();
            }
            Duration::from_secs(300)
        }
    }
}

/// Ciclo permanente (uma tarefa por processo).
pub fn spawn(acc: Arc<Accounts>, cache: Arc<TurnCache>, base: String, anon_key: String) {
    tokio::spawn(async move {
        loop {
            let wait = step(&acc, &cache, &base, &anon_key).await;
            tokio::time::sleep(wait).await;
        }
    });
}
