//! Implementação do `AuthProvider` sobre o Supabase: Auth (GoTrue) em `/auth/v1` e funções RPC em `/rest/v1/rpc`.
//! É o ÚNICO arquivo que conhece as rotas e os formatos do Supabase. Só usa a URL do projeto e a chave pública
//! (anon/publishable); a service_role é recusada em `config.rs` e nunca passa por aqui.
use std::time::Duration;

use serde_json::{json, Value};

use super::pkce::url_encode;
use super::provider::*;

pub struct SupabaseProvider {
    base: String,
    key: String,
    agent: ureq::Agent,
}

fn now() -> u64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

impl SupabaseProvider {
    pub fn new(url: &str, anon_key: &str) -> Self {
        let agent = ureq::AgentBuilder::new().timeout_connect(Duration::from_secs(10)).timeout(Duration::from_secs(20)).redirects(0).build();
        SupabaseProvider { base: url.trim().trim_end_matches('/').to_string(), key: anon_key.trim().to_string(), agent }
    }

    fn call(&self, method: &str, path: &str, bearer: Option<&str>, body: Option<Value>) -> AResult<Value> {
        let req = self.agent.request(method, &format!("{}{}", self.base, path))
            .set("apikey", &self.key)
            .set("Authorization", &format!("Bearer {}", bearer.unwrap_or(&self.key)))
            .set("Accept", "application/json");
        let res = match body {
            Some(b) => req.send_json(b),
            None => req.call(),
        };
        match res {
            Ok(r) => {
                let text = r.into_string().map_err(|_| AccountError::network())?;
                Ok(if text.trim().is_empty() { Value::Null } else { serde_json::from_str(&text).unwrap_or(Value::Null) })
            }
            Err(ureq::Error::Status(status, r)) => {
                let text = r.into_string().unwrap_or_default();
                Err(map_error(status, &serde_json::from_str::<Value>(&text).unwrap_or(Value::Null)))
            }
            Err(ureq::Error::Transport(_)) => Err(AccountError::network()),
        }
    }

    fn rpc(&self, access: &str, name: &str, args: Value) -> AResult<Value> {
        self.call("POST", &format!("/rest/v1/rpc/{name}"), Some(access), Some(args))
    }
}

/// Traduz as respostas de erro do GoTrue e do PostgREST para códigos nossos com mensagem em português.
pub fn map_error(status: u16, v: &Value) -> AccountError {
    let code = v["error_code"].as_str().or(v["error"].as_str()).or(v["code"].as_str()).unwrap_or("");
    let msg = v["msg"].as_str().or(v["error_description"].as_str()).or(v["message"].as_str()).unwrap_or("");
    let low = msg.to_lowercase();
    let e = AccountError::new;
    // exceções levantadas pelas nossas funções SQL (raise exception '<código>')
    match msg {
        "not_authenticated" => return AccountError::session_expired(),
        "nick_taken" => return e("nick_taken", "Esse @nick já está em uso."),
        "invalid_nick" => return e("invalid_nick", "O @nick deve ter de 3 a 20 caracteres: letras minúsculas, números e _."),
        "reserved_nick" => return e("invalid_nick", "Esse @nick é reservado. Escolha outro."),
        "nick_change_too_soon" => return e("nick_change_too_soon", "Você só pode trocar o @nick uma vez a cada 30 dias."),
        "not_found" => return e("not_found", "Não encontramos ninguém com esse @nick."),
        "no_profile" => return e("no_profile", "Seu perfil ainda não foi criado no servidor. Saia e entre de novo."),
        "self_request" => return e("self_request", "Esse é o seu próprio @nick."),
        "already_friends" => return e("already_friends", "Vocês já são amigos."),
        "already_requested" => return e("already_requested", "Já existe um pedido de amizade pendente com essa pessoa."),
        "too_many_requests" => return e("rate_limited", "Você tem pedidos de amizade demais em aberto. Aguarde as respostas."),
        "invalid_public_key" | "invalid_fingerprint" => return e("server", "O servidor recusou a chave pública deste aparelho."),
        "invalid_display_name" => return e("invalid_name", "O nome deve ter de 1 a 64 caracteres."),
        _ => {}
    }
    if code == "PGRST202" || (status == 404 && low.contains("could not find the function")) {
        return e("server_not_ready", "O servidor de contas ainda não foi preparado (faltam as migrações do banco).");
    }
    if code.starts_with("PGRST3") || low.contains("jwt expired") || code == "bad_jwt" || code == "session_not_found" || code == "session_expired"
        || code == "refresh_token_not_found" || code == "refresh_token_already_used" || low.contains("invalid refresh token") || low.contains("refresh token not found") || low.contains("refresh token is not valid") {
        return AccountError::session_expired();
    }
    if code == "invalid_credentials" || low.contains("invalid login credentials") {
        return e("invalid_credentials", "E-mail ou senha incorretos.");
    }
    if code == "email_not_confirmed" || low.contains("email not confirmed") {
        return e("email_not_confirmed", "Confirme seu e-mail antes de entrar. Enviamos um link para a sua caixa de entrada.");
    }
    if code == "user_already_exists" || code == "email_exists" || low.contains("already registered") {
        return e("user_exists", "Já existe uma conta com esse e-mail. Entre ou recupere a senha.");
    }
    if code == "weak_password" || low.contains("password should be") {
        return e("weak_password", "Senha fraca. Use pelo menos 8 caracteres, misturando letras e números.");
    }
    if code == "same_password" {
        return e("weak_password", "A nova senha precisa ser diferente da anterior.");
    }
    if code == "email_address_invalid" || code == "validation_failed" && low.contains("email") || low.contains("unable to validate email") {
        return e("invalid_email", "Esse e-mail não parece válido.");
    }
    if code == "otp_expired" || low.contains("token has expired or is invalid") {
        return e("invalid_code", "Código inválido ou vencido. Peça um novo e-mail.");
    }
    if code == "flow_state_not_found" || code == "flow_state_expired" || code == "bad_code_verifier" || low.contains("code verifier") {
        return e("invalid_code", "Esse login expirou. Tente de novo.");
    }
    if status == 429 || code.starts_with("over_") {
        return e("rate_limited", "Muitas tentativas em pouco tempo. Aguarde um minuto e tente de novo.");
    }
    if code == "provider_disabled" || code == "oauth_provider_not_supported" || low.contains("provider is not enabled") {
        return e("provider_disabled", "Esse tipo de login ainda não está ativado no servidor.");
    }
    if code == "signup_disabled" || code == "email_provider_disabled" {
        return e("provider_disabled", "O cadastro por e-mail está desativado no servidor.");
    }
    if code == "user_banned" {
        return e("banned", "Esta conta está suspensa.");
    }
    if status == 401 || status == 403 {
        return AccountError::session_expired();
    }
    if status >= 500 {
        return e("server", "O servidor de contas está com problemas. Tente de novo em instantes.");
    }
    e("server", "Não foi possível concluir. Tente de novo.")
}

fn user_of(u: &Value) -> AccountUser {
    let meta = &u["user_metadata"];
    let s = |v: &Value| v.as_str().map(str::trim).filter(|s| !s.is_empty()).map(String::from);
    let email = u["email"].as_str().unwrap_or("").to_string();
    let display_name = s(&meta["display_name"]).or_else(|| s(&meta["custom_claims"]["global_name"])).or_else(|| s(&meta["full_name"])).or_else(|| s(&meta["name"]))
        .or_else(|| email.split('@').next().filter(|p| !p.is_empty()).map(String::from)).unwrap_or_else(|| "Usuário".into());
    AccountUser {
        id: u["id"].as_str().unwrap_or("").to_string(),
        email,
        display_name: display_name.chars().take(64).collect(),
        avatar_url: s(&meta["avatar_url"]).or_else(|| s(&meta["picture"])).filter(|a| a.starts_with("https://") && a.len() <= 500).unwrap_or_default(),
        provider: u["app_metadata"]["provider"].as_str().unwrap_or("email").to_string(),
    }
}

fn session_of(v: &Value) -> AResult<Session> {
    let (Some(access), Some(refresh)) = (v["access_token"].as_str(), v["refresh_token"].as_str()) else {
        return Err(AccountError::new("server", "Resposta inesperada do servidor de contas."));
    };
    let user = user_of(&v["user"]);
    if user.id.is_empty() {
        return Err(AccountError::new("server", "Resposta inesperada do servidor de contas."));
    }
    let expires_at = v["expires_at"].as_u64().unwrap_or_else(|| now() + v["expires_in"].as_u64().unwrap_or(3600));
    Ok(Session { access_token: access.into(), refresh_token: refresh.into(), expires_at, user })
}

impl AuthProvider for SupabaseProvider {
    fn oauth_url(&self, provider: OAuthProvider, redirect_to: &str, challenge: &str) -> String {
        format!("{}/auth/v1/authorize?provider={}&redirect_to={}&code_challenge={}&code_challenge_method=s256",
            self.base, provider.as_str(), url_encode(redirect_to), url_encode(challenge))
    }
    fn exchange_code(&self, code: &str, verifier: &str) -> AResult<Session> {
        session_of(&self.call("POST", "/auth/v1/token?grant_type=pkce", None, Some(json!({ "auth_code": code, "code_verifier": verifier })))?)
    }
    fn sign_in_password(&self, email: &str, password: &str) -> AResult<Session> {
        session_of(&self.call("POST", "/auth/v1/token?grant_type=password", None, Some(json!({ "email": email, "password": password })))?)
    }
    fn sign_up(&self, email: &str, password: &str, display_name: &str, redirect_to: &str, challenge: &str) -> AResult<SignUp> {
        let v = self.call("POST", &format!("/auth/v1/signup?redirect_to={}", url_encode(redirect_to)), None, Some(json!({
            "email": email, "password": password, "data": { "display_name": display_name },
            "code_challenge": challenge, "code_challenge_method": "s256" })))?;
        if v["access_token"].is_string() {
            return Ok(SignUp::Session(session_of(&v)?));
        }
        Ok(SignUp::ConfirmEmail)
    }
    fn recover(&self, email: &str, redirect_to: &str, challenge: &str) -> AResult<()> {
        self.call("POST", &format!("/auth/v1/recover?redirect_to={}", url_encode(redirect_to)), None,
            Some(json!({ "email": email, "code_challenge": challenge, "code_challenge_method": "s256" }))).map(|_| ())
    }
    fn verify_code(&self, email: &str, code: &str, kind: CodeKind) -> AResult<Session> {
        let t = if kind == CodeKind::Recovery { "recovery" } else { "signup" };
        session_of(&self.call("POST", "/auth/v1/verify", None, Some(json!({ "type": t, "email": email, "token": code })))?)
    }
    fn update_password(&self, access_token: &str, new_password: &str) -> AResult<()> {
        self.call("PUT", "/auth/v1/user", Some(access_token), Some(json!({ "password": new_password }))).map(|_| ())
    }
    fn refresh(&self, refresh_token: &str) -> AResult<Session> {
        session_of(&self.call("POST", "/auth/v1/token?grant_type=refresh_token", None, Some(json!({ "refresh_token": refresh_token })))?)
    }
    fn sign_out(&self, access_token: &str) -> AResult<()> {
        self.call("POST", "/auth/v1/logout?scope=local", Some(access_token), Some(json!({}))).map(|_| ())
    }

    fn sync_profile(&self, access_token: &str, display_name: &str, avatar_url: &str, public_key: &Value, fingerprint: &str) -> AResult<Profile> {
        let v = self.rpc(access_token, "account_sync_profile", json!({
            "p_display_name": display_name, "p_avatar_url": if avatar_url.is_empty() { Value::Null } else { json!(avatar_url) },
            "p_public_key": public_key, "p_fingerprint": fingerprint }))?;
        serde_json::from_value(v).map_err(|_| AccountError::new("server", "Resposta inesperada do servidor de contas."))
    }
    fn set_nick(&self, access_token: &str, nick: &str) -> AResult<Profile> {
        let v = self.rpc(access_token, "account_set_nick", json!({ "p_nick": nick }))?;
        serde_json::from_value(v).map_err(|_| AccountError::new("server", "Resposta inesperada do servidor de contas."))
    }
    fn friends(&self, access_token: &str) -> AResult<Vec<Friend>> {
        let v = self.rpc(access_token, "friends_list", json!({}))?;
        if v.is_null() {
            return Ok(vec![]);
        }
        serde_json::from_value(v).map_err(|_| AccountError::new("server", "Resposta inesperada do servidor de contas."))
    }
    fn friend_request(&self, access_token: &str, nick: &str) -> AResult<()> {
        self.rpc(access_token, "friend_request", json!({ "p_nick": nick })).map(|_| ())
    }
    fn friend_respond(&self, access_token: &str, request_id: &str, accept: bool) -> AResult<()> {
        self.rpc(access_token, "friend_respond", json!({ "p_request": request_id, "p_accept": accept })).map(|_| ())
    }
    fn friend_remove(&self, access_token: &str, user_id: &str) -> AResult<()> {
        self.rpc(access_token, "friend_remove", json!({ "p_user": user_id })).map(|_| ())
    }
}
