//! Onde o app acha o servidor de contas. Só a URL do projeto e a chave PÚBLICA (anon/publishable) entram aqui.
//! Ordem: variáveis `BRIKS_SUPABASE_URL` + `BRIKS_SUPABASE_ANON_KEY`  →  `<pasta de dados>\supabase.json`
//! (`{"url":"https://xxxx.supabase.co","anon_key":"..."}`)  →  as mesmas variáveis do Realtime (`VITE_SUPABASE_*`).
//! `BRIKS_ACCOUNTS_MOCK=1` liga o modo de desenvolvimento: servidor de contas falso dentro do processo, sem rede.
use base64::{engine::general_purpose::URL_SAFE_NO_PAD as B64, Engine};
use serde_json::Value;
use std::path::Path;

#[derive(Debug, Clone, PartialEq)]
pub enum AccountsConfig {
    Supabase { url: String, anon_key: String },
    Mock,
    /// Nada configurado (ou configuração recusada): a UI mostra o motivo.
    Missing(String),
}

/// Recusa chaves secretas: a service_role (JWT com role=service_role) e as `sb_secret_...` dão acesso total ao banco
/// e não podem ficar num app distribuído.
pub fn check_public_key(key: &str) -> Result<(), String> {
    let k = key.trim();
    if k.is_empty() {
        return Err("chave pública vazia".into());
    }
    if k.starts_with("sb_secret_") {
        return Err("a chave configurada é SECRETA (sb_secret_...). Use a chave publishable/anon.".into());
    }
    if let Some(payload) = k.split('.').nth(1) {
        if let Ok(bytes) = B64.decode(payload.trim_end_matches('=')) {
            if let Ok(v) = serde_json::from_slice::<Value>(&bytes) {
                if v["role"].as_str().is_some_and(|r| r != "anon") {
                    return Err("a chave configurada é a service_role (secreta). Use a chave anon/publishable.".into());
                }
            }
        }
    }
    Ok(())
}

pub fn check_url(url: &str) -> Result<(), String> {
    let u = url.trim();
    let local = u.starts_with("http://127.0.0.1") || u.starts_with("http://localhost");
    if !(u.starts_with("https://") || local) {
        return Err("a URL do servidor de contas precisa ser https://".into());
    }
    Ok(())
}

pub fn resolve(env: &dyn Fn(&str) -> Option<String>, data_dir: &Path, fallback: Option<(String, String)>) -> AccountsConfig {
    if env("BRIKS_ACCOUNTS_MOCK").is_some_and(|v| v == "1") {
        return AccountsConfig::Mock;
    }
    let from_env = env("BRIKS_SUPABASE_URL").zip(env("BRIKS_SUPABASE_ANON_KEY"));
    let from_file = || {
        // 1) pasta de dados do usuário; 2) ao lado do executável (instalador leva a chave publishable para outros PCs)
        let exe_side = std::env::current_exe().ok().and_then(|e| e.parent().map(|d| d.join("supabase.json")));
        let t = std::fs::read_to_string(data_dir.join("supabase.json")).ok()
            .or_else(|| exe_side.and_then(|p| std::fs::read_to_string(p).ok()))?;
        let v: Value = serde_json::from_str(t.trim_start_matches('\u{feff}')).ok()?;
        let key = v["anon_key"].as_str().or(v["publishable_key"].as_str()).or(v["key"].as_str())?;
        Some((v["url"].as_str()?.to_string(), key.to_string()))
    };
    // http(s) do Realtime vem como URL do projeto; ws:// de teste não serve para contas
    let fallback = fallback.filter(|(u, _)| u.starts_with("http"));
    let Some((url, key)) = from_env.or_else(from_file).or(fallback) else {
        return AccountsConfig::Missing("Servidor de contas não configurado. Crie o arquivo supabase.json na pasta de dados do Briks (veja docs/plans/contas-online.md).".into());
    };
    if let Err(e) = check_url(&url).and_then(|_| check_public_key(&key)) {
        return AccountsConfig::Missing(format!("Configuração do servidor de contas recusada: {e}"));
    }
    AccountsConfig::Supabase { url: url.trim().trim_end_matches('/').to_string(), anon_key: key.trim().to_string() }
}
