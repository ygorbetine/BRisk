//! Contas online do Briks. Camada própria: o resto do core fala com `Accounts` e com os tipos de `provider.rs`;
//! a API do Supabase fica isolada em `supabase.rs`.
//!
//! O que fica onde:
//!  - refresh token: em disco, cifrado com a DPAPI do Windows (`account-session.bin`); nunca em texto aberto;
//!  - access token: só na memória deste processo, renovado sozinho antes de vencer;
//!  - chave privada da identidade P2P: NUNCA vai ao servidor; só a chave pública e a impressão digital são publicadas no perfil.
pub mod avatar;
pub mod backup;
pub mod config;
pub mod fake;
pub mod invite_code;
pub mod legacy;
pub mod loopback;
pub mod pkce;
pub mod provider;
pub mod service;
pub mod store;
pub mod supabase;
pub mod turn;
#[cfg(test)]
mod tests;

use std::collections::HashMap;
use std::path::Path;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use serde::{Deserialize, Serialize};
use serde_json::Value;

use loopback::Loopback;
use pkce::Pkce;
pub use provider::*;
use store::SecretFile;

pub type Opener = Arc<dyn Fn(String) -> AResult<()> + Send + Sync>;

#[derive(Serialize, Deserialize)]
struct Stored {
    version: u32,
    refresh_token: String,
    user: AccountUser,
}

pub struct Accounts {
    provider: Arc<dyn AuthProvider>,
    store: SecretFile,
    bridge: SecretFile,
    session: Mutex<Option<Session>>,
    refresh_lock: tokio::sync::Mutex<()>,
    replaced: Mutex<Option<String>>,
    opener: Opener,
    pub oauth_timeout: Duration,
    pub email_timeout: Duration,
}

/// Espera pelo clique no link do e-mail (confirmação de cadastro ou recuperação de senha) aberto neste computador.
pub struct EmailWait {
    lb: Loopback,
    verifier: String,
}

pub enum SignUpOutcome {
    SignedIn(AccountUser),
    ConfirmEmail(EmailWait),
}

fn now() -> u64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

async fn blk<T: Send + 'static>(f: impl FnOnce() -> AResult<T> + Send + 'static) -> AResult<T> {
    tokio::task::spawn_blocking(f).await.unwrap_or_else(|_| Err(AccountError::new("server", "Falha interna no módulo de contas.")))
}

/// Abre a URL no navegador padrão do Windows. Sem shell no meio: a URL vai como argumento único.
pub fn system_browser() -> Opener {
    Arc::new(|url: String| {
        if !(url.starts_with("https://") || url.starts_with("http://127.0.0.1") || url.starts_with("http://localhost")) {
            return Err(AccountError::new("browser", "Endereço de login inválido."));
        }
        std::process::Command::new("rundll32").args(["url.dll,FileProtocolHandler", &url]).spawn().map(|_| ())
            .map_err(|_| AccountError::new("browser", "Não foi possível abrir o navegador."))
    })
}

pub fn valid_email(e: &str) -> bool {
    let e = e.trim();
    match e.split_once('@') {
        Some((a, b)) => !a.is_empty() && b.contains('.') && !b.starts_with('.') && !b.ends_with('.') && e.len() <= 254 && !e.contains(char::is_whitespace),
        None => false,
    }
}

fn check_email(e: &str) -> AResult<String> {
    if valid_email(e) { Ok(e.trim().to_lowercase()) } else { Err(AccountError::new("invalid_email", "Digite um e-mail válido, como voce@exemplo.com.")) }
}
fn check_password(p: &str) -> AResult<()> {
    if p.chars().count() < 8 {
        return Err(AccountError::new("weak_password", "A senha precisa ter pelo menos 8 caracteres."));
    }
    if p.len() > 72 {
        return Err(AccountError::new("weak_password", "A senha pode ter no máximo 72 caracteres."));
    }
    Ok(())
}

impl Accounts {
    pub fn new(provider: Arc<dyn AuthProvider>, data_dir: &Path, opener: Opener) -> Self {
        Self::with_stores(provider, SecretFile::dpapi(data_dir.join("account-session.bin")), SecretFile::dpapi(data_dir.join("local-bridge.bin")), opener)
    }
    pub fn with_stores(provider: Arc<dyn AuthProvider>, store: SecretFile, bridge: SecretFile, opener: Opener) -> Self {
        Accounts { provider, store, bridge, session: Mutex::new(None), refresh_lock: tokio::sync::Mutex::new(()), replaced: Mutex::new(None), opener,
            oauth_timeout: Duration::from_secs(300), email_timeout: Duration::from_secs(900) }
    }

    #[cfg(test)]
    pub fn expire_access_for_tests(&self) {
        if let Some(s) = self.session.lock().unwrap().as_mut() { s.expires_at = 0; }
    }

    pub fn user(&self) -> Option<AccountUser> {
        self.session.lock().unwrap().as_ref().map(|s| s.user.clone())
    }

    fn adopt(&self, s: Session) -> AccountUser {
        // falha ao cifrar/gravar não impede o uso: a sessão fica só na memória (e nunca vai para o disco em texto aberto)
        let _ = self.store.save(&Stored { version: 1, refresh_token: s.refresh_token.clone(), user: s.user.clone() });
        let u = s.user.clone();
        let old = self.session.lock().unwrap().replace(s).map(|o| o.user.id).filter(|id| *id != u.id);
        if old.is_some() {
            *self.replaced.lock().unwrap() = old;
        }
        u
    }

    /// Se a sessão que acabou de entrar substituiu a de OUTRA conta, devolve o id da anterior (uma vez só).
    pub fn take_previous_id(&self, current: &str) -> Option<String> {
        self.replaced.lock().unwrap().take().filter(|p| p != current)
    }

    fn drop_session(&self) {
        *self.session.lock().unwrap() = None;
        self.store.clear();
    }

    /// Retoma a sessão guardada (se houver). `Ok(None)` = ninguém logado. Falha de rede NÃO apaga a sessão guardada.
    pub async fn restore(&self) -> AResult<Option<AccountUser>> {
        if let Some(u) = self.user() {
            return Ok(Some(u));
        }
        let Some(st) = self.store.load::<Stored>() else { return Ok(None) };
        let p = self.provider.clone();
        match blk(move || p.refresh(&st.refresh_token)).await {
            Ok(s) => Ok(Some(self.adopt(s))),
            Err(e) if e.is_auth() => {
                self.drop_session();
                Ok(None)
            }
            Err(e) => Err(e),
        }
    }

    /// Login social: abre o navegador padrão e espera o retorno no loopback. Cancelar = abortar esta future.
    pub async fn sign_in_oauth(&self, provider: OAuthProvider) -> AResult<AccountUser> {
        let lb = loopback::bind().await?;
        let pk = Pkce::generate();
        let url = self.provider.oauth_url(provider, &lb.redirect_uri, &pk.challenge);
        (self.opener)(url)?;
        let code = lb.wait_code(self.oauth_timeout).await?;
        let (p, verifier) = (self.provider.clone(), pk.verifier);
        let s = blk(move || p.exchange_code(&code, &verifier)).await?;
        Ok(self.adopt(s))
    }

    pub async fn sign_in_password(&self, email: &str, password: &str) -> AResult<AccountUser> {
        let email = check_email(email)?;
        if password.is_empty() {
            return Err(AccountError::new("invalid_credentials", "Digite sua senha."));
        }
        let (p, pw) = (self.provider.clone(), password.to_string());
        let s = blk(move || p.sign_in_password(&email, &pw)).await?;
        Ok(self.adopt(s))
    }

    pub async fn sign_up(&self, email: &str, password: &str, display_name: &str) -> AResult<SignUpOutcome> {
        let email = check_email(email)?;
        check_password(password)?;
        let name: String = display_name.trim().chars().take(64).collect();
        if name.is_empty() {
            return Err(AccountError::new("invalid_name", "Diga como devemos chamar você."));
        }
        let lb = loopback::bind().await?;
        let pk = Pkce::generate();
        let (p, pw, redirect, challenge) = (self.provider.clone(), password.to_string(), lb.redirect_uri.clone(), pk.challenge.clone());
        match blk(move || p.sign_up(&email, &pw, &name, &redirect, &challenge)).await? {
            SignUp::Session(s) => Ok(SignUpOutcome::SignedIn(self.adopt(s))),
            SignUp::ConfirmEmail => Ok(SignUpOutcome::ConfirmEmail(EmailWait { lb, verifier: pk.verifier })),
        }
    }

    /// Pede o e-mail de recuperação. A resposta é a mesma exista ou não a conta.
    pub async fn recover(&self, email: &str) -> AResult<EmailWait> {
        let email = check_email(email)?;
        let lb = loopback::bind().await?;
        let pk = Pkce::generate();
        let (p, redirect, challenge) = (self.provider.clone(), lb.redirect_uri.clone(), pk.challenge.clone());
        blk(move || p.recover(&email, &redirect, &challenge)).await?;
        Ok(EmailWait { lb, verifier: pk.verifier })
    }

    /// Espera o link do e-mail ser aberto neste computador e entra.
    pub async fn finish_email(&self, w: EmailWait) -> AResult<AccountUser> {
        let code = w.lb.wait_code(self.email_timeout).await?;
        let (p, verifier) = (self.provider.clone(), w.verifier);
        let s = blk(move || p.exchange_code(&code, &verifier)).await?;
        Ok(self.adopt(s))
    }

    /// Alternativa ao link: o código de 6 dígitos que vem no e-mail.
    pub async fn verify_code(&self, email: &str, code: &str, kind: CodeKind) -> AResult<AccountUser> {
        let email = check_email(email)?;
        let code: String = code.chars().filter(|c| c.is_ascii_digit()).collect();
        if code.len() < 6 || code.len() > 10 {
            return Err(AccountError::new("invalid_code", "Digite o código que chegou no e-mail."));
        }
        let p = self.provider.clone();
        let s = blk(move || p.verify_code(&email, &code, kind)).await?;
        Ok(self.adopt(s))
    }

    pub async fn set_password(&self, new_password: &str) -> AResult<()> {
        check_password(new_password)?;
        let tok = self.access_token().await?;
        let (p, pw) = (self.provider.clone(), new_password.to_string());
        blk(move || p.update_password(&tok, &pw)).await
    }

    /// Sai da conta: invalida a sessão no servidor (melhor esforço) e apaga a sessão guardada neste computador.
    /// A chave do banco local (`local-bridge.bin`) fica: não é credencial da conta e sem ela o histórico local se perderia.
    pub async fn sign_out(&self) {
        let tok = self.session.lock().unwrap().as_ref().map(|s| s.access_token.clone());
        self.drop_session();
        if let Some(tok) = tok {
            let p = self.provider.clone();
            let _ = tokio::time::timeout(Duration::from_secs(5), blk(move || p.sign_out(&tok))).await;
        }
    }

    /// "Sair dos outros computadores": o servidor derruba as outras sessões desta conta; esta continua.
    pub async fn sign_out_others(&self) -> AResult<()> {
        let tok = self.access_token().await?;
        let p = self.provider.clone();
        blk(move || p.sign_out_others(&tok)).await
    }

    /// Segundos até a próxima renovação (60 s antes do vencimento). `None` sem sessão.
    pub fn refresh_in(&self) -> Option<u64> {
        self.session.lock().unwrap().as_ref().map(|s| s.expires_at.saturating_sub(now() + 60))
    }

    /// Access token válido; renova com o refresh token se faltar menos de 60 s. Erro `session_expired` = sessão derrubada.
    pub async fn access_token(&self) -> AResult<String> {
        let fresh = |s: &Session| s.expires_at > now() + 60;
        {
            let g = self.session.lock().unwrap();
            match g.as_ref() {
                None => return Err(AccountError::session_expired()),
                Some(s) if fresh(s) => return Ok(s.access_token.clone()),
                _ => {}
            }
        }
        let _one = self.refresh_lock.lock().await;   // uma renovação por vez: o refresh token é de uso único
        let rt = {
            let g = self.session.lock().unwrap();
            match g.as_ref() {
                None => return Err(AccountError::session_expired()),
                Some(s) if fresh(s) => return Ok(s.access_token.clone()),
                Some(s) => s.refresh_token.clone(),
            }
        };
        let p = self.provider.clone();
        match blk(move || p.refresh(&rt)).await {
            Ok(s) => {
                let t = s.access_token.clone();
                self.adopt(s);
                Ok(t)
            }
            Err(e) => {
                if e.is_auth() {
                    self.drop_session();
                }
                Err(e)
            }
        }
    }

    /// Publica no perfil a chave pública e a impressão digital da identidade P2P deste aparelho.
    pub async fn publish_identity(&self, public_key: Value, fingerprint: &str) -> AResult<Profile> {
        let u = self.user().ok_or_else(AccountError::session_expired)?;
        let tok = self.access_token().await?;
        let (p, fp) = (self.provider.clone(), fingerprint.to_string());
        blk(move || p.sync_profile(&tok, &u.display_name, &u.avatar_url, &public_key, &fp)).await
    }
    pub async fn set_nick(&self, nick: &str) -> AResult<Profile> {
        let tok = self.access_token().await?;
        let (p, n) = (self.provider.clone(), nick.trim().trim_start_matches('@').to_lowercase());
        blk(move || p.set_nick(&tok, &n)).await
    }
    /// Troca o nome de exibição (no servidor e na sessão guardada).
    pub async fn set_display_name(&self, name: &str) -> AResult<Profile> {
        let name: String = name.trim().chars().take(64).collect();
        if name.is_empty() {
            return Err(AccountError::new("invalid_name", "O nome não pode ficar vazio."));
        }
        let tok = self.access_token().await?;
        let (p, n) = (self.provider.clone(), name.clone());
        let prof = blk(move || p.update_profile(&tok, Some(&n), None)).await?;
        self.remember_name(&prof.display_name);
        Ok(prof)
    }
    /// Envia uma foto nova (JPEG já recortado por `avatar::prepare`) e passa a usá-la no perfil.
    pub async fn set_photo(&self, jpeg: Vec<u8>) -> AResult<Profile> {
        let u = self.user().ok_or_else(AccountError::session_expired)?;
        let tok = self.access_token().await?;
        let p = self.provider.clone();
        blk(move || {
            let url = p.upload_avatar(&tok, &u.id, &jpeg)?;
            p.update_profile(&tok, None, Some(&url))
        }).await
    }
    /// Volta para a foto da conta do provedor (Google/Discord); sem foto no provedor, fica só com as iniciais.
    pub async fn reset_photo(&self) -> AResult<Profile> {
        let u = self.user().ok_or_else(AccountError::session_expired)?;
        let tok = self.access_token().await?;
        let p = self.provider.clone();
        blk(move || p.update_profile(&tok, None, Some(&u.avatar_url))).await
    }
    /// O nome que vale é o do perfil no servidor (a pessoa pode ter trocado): atualiza a sessão em memória e a guardada.
    pub fn remember_name(&self, name: &str) {
        if name.is_empty() {
            return;
        }
        let mut g = self.session.lock().unwrap();
        if let Some(s) = g.as_mut() {
            if s.user.display_name != name {
                s.user.display_name = name.to_string();
                let _ = self.store.save(&Stored { version: 1, refresh_token: s.refresh_token.clone(), user: s.user.clone() });
            }
        }
    }
    /// Guarda no servidor a cópia cifrada da identidade deste aparelho. A frase não sai daqui.
    pub async fn backup_save(&self, id: &crate::identity::Identity, phrase: &str) -> AResult<backup::Blob> {
        let u = self.user().ok_or_else(AccountError::session_expired)?;
        let tok = self.access_token().await?;
        let (p, phrase, secret) = (self.provider.clone(), phrase.to_string(), id.secret_b64());
        blk(move || {
            let id = crate::identity::Identity::from_secret_b64(&secret).map_err(|_| AccountError::new("backup", "Identidade deste aparelho inválida."))?;
            let blob = backup::seal(&id, &phrase, &u.id)?;
            p.backup_put(&tok, &serde_json::to_value(&blob).unwrap_or(Value::Null))?;
            Ok(blob)
        }).await
    }
    /// A cópia guardada (sem abrir) e quando foi gravada. `None` = não há.
    pub async fn backup_fetch(&self) -> AResult<Option<(backup::Blob, String)>> {
        let tok = self.access_token().await?;
        let p = self.provider.clone();
        let Some(v) = blk(move || p.backup_get(&tok)).await? else { return Ok(None) };
        let blob: backup::Blob = serde_json::from_value(v["blob"].clone())
            .map_err(|_| AccountError::new("invalid_backup", "A cópia guardada no servidor está em um formato que este app não aceita."))?;
        Ok(Some((blob, v["updated_at"].as_str().unwrap_or("").to_string())))
    }
    /// Baixa a cópia e abre com a frase. Frase errada = erro `wrong_phrase`, sem nada instalado.
    pub async fn backup_open(&self, phrase: &str) -> AResult<crate::identity::Identity> {
        let phrase = backup::check_phrase(phrase)?;   // erro de digitação aparece antes de ir à rede
        let u = self.user().ok_or_else(AccountError::session_expired)?;
        let Some((blob, _)) = self.backup_fetch().await? else {
            return Err(AccountError::new("no_backup", "Esta conta não tem cópia de segurança guardada."));
        };
        blk(move || backup::open(&blob, &phrase, &u.id)).await
    }
    pub async fn backup_delete(&self) -> AResult<()> {
        let tok = self.access_token().await?;
        let p = self.provider.clone();
        blk(move || p.backup_delete(&tok)).await
    }
    pub async fn friends(&self) -> AResult<Vec<Friend>> {
        let tok = self.access_token().await?;
        let p = self.provider.clone();
        blk(move || p.friends(&tok)).await
    }
    pub async fn friend_request(&self, nick: &str) -> AResult<()> {
        let n = nick.trim().trim_start_matches('@').to_lowercase();
        if n.len() < 3 || n.len() > 20 || !n.chars().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_') {
            return Err(AccountError::new("invalid_nick", "Digite o @nick do seu amigo (3 a 20 caracteres: letras, números e _)."));
        }
        let tok = self.access_token().await?;
        let p = self.provider.clone();
        blk(move || p.friend_request(&tok, &n)).await
    }
    pub async fn friend_respond(&self, request_id: &str, accept: bool) -> AResult<()> {
        let tok = self.access_token().await?;
        let (p, r) = (self.provider.clone(), request_id.to_string());
        blk(move || p.friend_respond(&tok, &r, accept)).await
    }
    pub async fn friend_remove(&self, user_id: &str) -> AResult<()> {
        let tok = self.access_token().await?;
        let (p, u) = (self.provider.clone(), user_id.to_string());
        blk(move || p.friend_remove(&tok, &u)).await
    }
    pub async fn nick_lookup(&self, nick: &str) -> AResult<NickHit> {
        let tok = self.access_token().await?;
        let (p, n) = (self.provider.clone(), nick.to_string());
        blk(move || p.nick_lookup(&tok, &n)).await
    }
    /// Código curto do convite (0010): `package` é o convite longo já assinado. Devolve `{code, expires_in_hours, max_uses}`.
    pub async fn group_invite_create(&self, package: &str, ttl_hours: u32, max_uses: u32) -> AResult<serde_json::Value> {
        if !invite_code::package_ok(package) { return Err(AccountError::new("invalid_package", "Convite inválido.")); }
        let tok = self.access_token().await?;
        let (p, k) = (self.provider.clone(), package.to_string());
        blk(move || p.group_invite_create(&tok, &k, ttl_hours.clamp(1, 168), max_uses.clamp(1, 100))).await
    }
    /// Resolve o código curto para o pacote assinado (a conferência da assinatura e do destinatário é de quem chama).
    pub async fn group_invite_lookup(&self, code: &str) -> AResult<String> {
        let Some(norm) = invite_code::normalize(code) else { return Err(AccountError::new("invite_not_found", "Código inválido. Confira os 8 símbolos (BRK-XXXX-XXXX).")) };
        let tok = self.access_token().await?;
        let (p, k) = (self.provider.clone(), norm);
        let v = blk(move || p.group_invite_lookup(&tok, &k)).await?;
        let pkg = v["package"].as_str().unwrap_or("");
        if !invite_code::package_ok(pkg) { return Err(AccountError::new("server", "Resposta inesperada do servidor de contas.")); }
        Ok(pkg.to_string())
    }
    pub async fn presence_beat(&self, invisible: bool) -> AResult<()> {
        let tok = self.access_token().await?;
        let p = self.provider.clone();
        blk(move || p.presence_beat(&tok, invisible)).await
    }
    pub async fn presence_friends(&self) -> AResult<Vec<Presence>> {
        let tok = self.access_token().await?;
        let p = self.provider.clone();
        blk(move || p.presence_friends(&tok)).await
    }
    pub async fn presence_clear(&self) -> AResult<()> {
        let tok = self.access_token().await?;
        let p = self.provider.clone();
        blk(move || p.presence_clear(&tok)).await
    }
    pub async fn friend_block(&self, user_id: &str) -> AResult<()> {
        let tok = self.access_token().await?;
        let (p, u) = (self.provider.clone(), user_id.to_string());
        blk(move || p.friend_block(&tok, &u)).await
    }
    pub async fn friend_unblock(&self, user_id: &str) -> AResult<()> {
        let tok = self.access_token().await?;
        let (p, u) = (self.provider.clone(), user_id.to_string());
        blk(move || p.friend_unblock(&tok, &u)).await
    }
    pub async fn keys_list(&self) -> AResult<Vec<DeviceKey>> {
        let tok = self.access_token().await?;
        let p = self.provider.clone();
        blk(move || p.keys_list(&tok)).await
    }
    pub async fn key_register(&self, peer_id: &str, public_key: Value, device_name: &str) -> AResult<()> {
        let tok = self.access_token().await?;
        let (p, id, n) = (self.provider.clone(), peer_id.to_string(), device_name.to_string());
        blk(move || p.key_register(&tok, &id, &public_key, &n)).await
    }
    pub async fn key_revoke(&self, peer_id: &str) -> AResult<()> {
        let tok = self.access_token().await?;
        let (p, id) = (self.provider.clone(), peer_id.to_string());
        blk(move || p.key_revoke(&tok, &id)).await
    }
    pub async fn blocks(&self) -> AResult<Vec<Friend>> {
        let tok = self.access_token().await?;
        let p = self.provider.clone();
        blk(move || p.blocks(&tok)).await
    }

    /// Credencial do banco LOCAL deste computador para a conta logada (ponte de transição: mensagens e grupos ainda são
    /// guardados pelo backend local, que exige um usuário local). Senha aleatória por conta, cifrada com a DPAPI; não é a
    /// senha da conta online e nunca sai do computador.
    pub fn local_credentials(&self) -> AResult<(String, String)> {
        let u = self.user().ok_or_else(AccountError::session_expired)?;
        let mut map: HashMap<String, String> = self.bridge.load().unwrap_or_default();
        let pw = match map.get(&u.id) {
            Some(p) => p.clone(),
            None => {
                use rand_core::{OsRng, RngCore};
                let mut raw = [0u8; 24];
                OsRng.fill_bytes(&mut raw);
                let p: String = raw.iter().map(|b| format!("{b:02x}")).collect();
                map.insert(u.id.clone(), p.clone());
                self.bridge.save(&map).map_err(|_| AccountError::new("storage", "Não foi possível guardar os dados locais com segurança."))?;
                p
            }
        };
        Ok((format!("{}@conta.briks.local", u.id), pw))
    }
}
