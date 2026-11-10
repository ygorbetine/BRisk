//! Contrato do provedor de contas. O resto do app só conhece estes tipos: trocar o Supabase por outro serviço
//! é escrever outra implementação de `AuthProvider`, sem tocar em IPC, UI ou no orquestrador (`mod.rs`).
use serde::{Deserialize, Serialize};
use serde_json::Value;

#[derive(Debug, Clone, PartialEq)]
pub struct AccountError {
    /// Código estável para a UI/testes: network, invalid_credentials, email_not_confirmed, user_exists, weak_password,
    /// rate_limited, invalid_email, invalid_code, cancelled, timeout, session_expired, not_configured, provider_disabled,
    /// not_found, nick_taken, invalid_nick, server_not_ready, server…
    pub code: String,
    /// Mensagem pronta para mostrar, em português.
    pub message: String,
}

impl AccountError {
    pub fn new(code: &str, message: &str) -> Self {
        AccountError { code: code.into(), message: message.into() }
    }
    pub fn network() -> Self {
        Self::new("network", "Sem conexão com o servidor de contas. Confira a internet e tente de novo.")
    }
    pub fn session_expired() -> Self {
        Self::new("session_expired", "Sua sessão expirou. Entre de novo.")
    }
    /// Erros que significam "a sessão guardada não vale mais" (diferente de falha de rede, que não derruba a sessão).
    pub fn is_auth(&self) -> bool {
        self.code == "session_expired"
    }
}
impl std::fmt::Display for AccountError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.message)
    }
}
impl std::error::Error for AccountError {}

pub type AResult<T> = Result<T, AccountError>;

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum OAuthProvider {
    Google,
    Discord,
}
impl OAuthProvider {
    pub fn parse(s: &str) -> Option<Self> {
        match s {
            "google" => Some(Self::Google),
            "discord" => Some(Self::Discord),
            _ => None,
        }
    }
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Google => "google",
            Self::Discord => "discord",
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Default)]
pub struct AccountUser {
    pub id: String,
    pub email: String,
    pub display_name: String,
    pub avatar_url: String,
    /// google | discord | email
    pub provider: String,
}

#[derive(Debug, Clone)]
pub struct Session {
    pub access_token: String,
    pub refresh_token: String,
    /// segundos Unix em que o access token vence
    pub expires_at: u64,
    pub user: AccountUser,
}

pub enum SignUp {
    /// conta criada e já com sessão (projeto sem confirmação de e-mail)
    Session(Session),
    /// conta criada; falta confirmar o e-mail
    ConfirmEmail,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum CodeKind {
    Signup,
    Recovery,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Default)]
pub struct Profile {
    pub user_id: String,
    #[serde(default)]
    pub nick: String,
    #[serde(default)]
    pub display_name: String,
    #[serde(default)]
    pub avatar_url: Option<String>,
    #[serde(default)]
    pub fingerprint: Option<String>,
    #[serde(default)]
    pub status: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct Friend {
    pub user_id: String,
    #[serde(default)]
    pub nick: String,
    #[serde(default)]
    pub display_name: String,
    #[serde(default)]
    pub avatar_url: Option<String>,
    #[serde(default)]
    pub fingerprint: Option<String>,
    #[serde(default)]
    pub public_key: Option<Value>,
    #[serde(default)]
    pub status: Option<String>,
    /// friend | incoming | outgoing
    pub state: String,
    #[serde(default)]
    pub request_id: Option<String>,
}

/// Chamadas bloqueantes (rodam em `spawn_blocking`). Nenhuma delas guarda estado de sessão: quem guarda é o `Accounts`.
pub trait AuthProvider: Send + Sync {
    /// URL que o navegador abre para o login social com PKCE.
    fn oauth_url(&self, provider: OAuthProvider, redirect_to: &str, challenge: &str) -> String;
    fn exchange_code(&self, code: &str, verifier: &str) -> AResult<Session>;
    fn sign_in_password(&self, email: &str, password: &str) -> AResult<Session>;
    fn sign_up(&self, email: &str, password: &str, display_name: &str, redirect_to: &str, challenge: &str) -> AResult<SignUp>;
    /// Envia o e-mail de recuperação de senha. Não revela se o e-mail existe.
    fn recover(&self, email: &str, redirect_to: &str, challenge: &str) -> AResult<()>;
    /// Código de 6 dígitos do e-mail (confirmação de cadastro ou recuperação).
    fn verify_code(&self, email: &str, code: &str, kind: CodeKind) -> AResult<Session>;
    fn update_password(&self, access_token: &str, new_password: &str) -> AResult<()>;
    fn refresh(&self, refresh_token: &str) -> AResult<Session>;
    fn sign_out(&self, access_token: &str) -> AResult<()>;

    /// Cria/atualiza o perfil do usuário logado e liga a chave PÚBLICA da identidade P2P à conta.
    fn sync_profile(&self, access_token: &str, display_name: &str, avatar_url: &str, public_key: &Value, fingerprint: &str) -> AResult<Profile>;
    fn set_nick(&self, access_token: &str, nick: &str) -> AResult<Profile>;
    fn friends(&self, access_token: &str) -> AResult<Vec<Friend>>;
    fn friend_request(&self, access_token: &str, nick: &str) -> AResult<()>;
    fn friend_respond(&self, access_token: &str, request_id: &str, accept: bool) -> AResult<()>;
    fn friend_remove(&self, access_token: &str, user_id: &str) -> AResult<()>;
}
