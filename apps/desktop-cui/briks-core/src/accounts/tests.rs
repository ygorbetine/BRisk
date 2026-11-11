//! Testes do módulo de contas. Nenhum usa a internet: o cliente Supabase de verdade fala com o servidor falso em 127.0.0.1.
use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;

use serde_json::{json, Value};

use super::config::{self, AccountsConfig};
use super::fake::{self, Fake};
use super::pkce::{challenge_of, parse_query, url_decode, url_encode, Pkce};
use super::provider::*;
use super::store::{Protector, SecretFile};
use super::supabase::{map_error, SupabaseProvider};
use super::{avatar, backup, legacy, loopback, service, valid_email, Accounts, SignUpOutcome};

/// Os fluxos que abrem o loopback usam portas fixas: um de cada vez.
static FLOW: tokio::sync::Mutex<()> = tokio::sync::Mutex::const_new(());

fn tmpdir(n: &str) -> PathBuf {
    let p = std::env::temp_dir().join(format!("briks-acc-{n}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&p);
    std::fs::create_dir_all(&p).unwrap();
    p
}

fn accounts(f: &Fake, dir: &PathBuf) -> Accounts {
    let mut a = Accounts::new(Arc::new(SupabaseProvider::new(&f.url(), "chave-publica-de-teste")), dir, fake::auto_browser());
    a.oauth_timeout = Duration::from_secs(10);
    a.email_timeout = Duration::from_secs(10);
    a
}

// ---------- PKCE e utilitários ----------

#[test]
fn pkce_matches_rfc7636_vector() {
    assert_eq!(challenge_of("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"), "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
}

#[test]
fn pkce_verifier_is_random_and_well_formed() {
    let (a, b) = (Pkce::generate(), Pkce::generate());
    assert_eq!(a.verifier.len(), 43);
    assert!(a.verifier.chars().all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_'));
    assert_ne!(a.verifier, b.verifier);
    assert_eq!(a.challenge, challenge_of(&a.verifier));
    assert_ne!(a.challenge, a.verifier);
}

#[test]
fn url_encoding_roundtrip_and_query() {
    let s = "http://127.0.0.1:53682/callback?a=1&b=ç ã";
    assert_eq!(url_decode(&url_encode(s)), s);
    assert!(!url_encode(s).contains(['&', '?', ':', '/', ' ']));
    assert_eq!(parse_query("code=abc%2D1&x=&y"), vec![("code".into(), "abc-1".into()), ("x".into(), "".into()), ("y".into(), "".into())]);
    assert_eq!(url_decode("100%"), "100%");
    assert_eq!(url_decode("%zz"), "%zz");
}

#[test]
fn oauth_url_carries_challenge_but_never_the_verifier() {
    let p = SupabaseProvider::new("https://exemplo.supabase.co/", "k");
    let pk = Pkce::generate();
    let u = p.oauth_url(OAuthProvider::Discord, "http://127.0.0.1:53682/callback", &pk.challenge);
    assert!(u.starts_with("https://exemplo.supabase.co/auth/v1/authorize?provider=discord&"));
    assert!(u.contains("redirect_to=http%3A%2F%2F127.0.0.1%3A53682%2Fcallback"));
    assert!(u.contains(&format!("code_challenge={}", pk.challenge)) && u.contains("code_challenge_method=s256"));
    assert!(!u.contains(&pk.verifier));
}

#[test]
fn email_validation() {
    for ok in ["a@b.co", " voce@exemplo.com.br "] {
        assert!(valid_email(ok), "{ok}");
    }
    for bad in ["", "a", "a@b", "@b.co", "a b@c.co", "a@.co", "a@b."] {
        assert!(!valid_email(bad), "{bad}");
    }
}

// ---------- configuração ----------

#[test]
fn config_refuses_secret_keys_and_plain_http() {
    use base64::{engine::general_purpose::URL_SAFE_NO_PAD as B64, Engine};
    let jwt = |role: &str| format!("eyJhbGciOiJIUzI1NiJ9.{}.assinatura", B64.encode(json!({ "role": role, "iss": "supabase" }).to_string()));
    assert!(config::check_public_key(&jwt("anon")).is_ok());
    assert!(config::check_public_key("sb_publishable_abc123").is_ok());
    assert!(config::check_public_key(&jwt("service_role")).is_err());
    assert!(config::check_public_key("sb_secret_abc123").is_err());
    assert!(config::check_public_key("").is_err());
    assert!(config::check_url("https://x.supabase.co").is_ok());
    assert!(config::check_url("http://x.supabase.co").is_err());
    assert!(config::check_url("http://127.0.0.1:5000").is_ok());

    let dir = tmpdir("cfg");
    let none = |_: &str| None;
    assert!(matches!(config::resolve(&none, &dir, None), AccountsConfig::Missing(_)));
    std::fs::write(dir.join("supabase.json"), json!({ "url": "https://arquivo.supabase.co/", "anon_key": jwt("anon") }).to_string()).unwrap();
    assert_eq!(config::resolve(&none, &dir, None), AccountsConfig::Supabase { url: "https://arquivo.supabase.co".into(), anon_key: jwt("anon") });
    // variáveis de ambiente vencem o arquivo; o modo falso vence tudo
    let env = |k: &str| match k { "BRIKS_SUPABASE_URL" => Some("https://env.supabase.co".to_string()), "BRIKS_SUPABASE_ANON_KEY" => Some("sb_publishable_x".to_string()), _ => None };
    assert_eq!(config::resolve(&env, &dir, None), AccountsConfig::Supabase { url: "https://env.supabase.co".into(), anon_key: "sb_publishable_x".into() });
    let mock = |k: &str| (k == "BRIKS_ACCOUNTS_MOCK").then(|| "1".to_string());
    assert_eq!(config::resolve(&mock, &dir, None), AccountsConfig::Mock);
    // service_role no arquivo: recusada, com o motivo
    std::fs::write(dir.join("supabase.json"), json!({ "url": "https://arquivo.supabase.co", "anon_key": jwt("service_role") }).to_string()).unwrap();
    match config::resolve(&none, &dir, None) {
        AccountsConfig::Missing(m) => assert!(m.contains("service_role")),
        o => panic!("deveria recusar: {o:?}"),
    }
    // as variáveis do Realtime servem de reserva; ws:// de teste não
    let empty = tmpdir("cfg2");
    assert!(matches!(config::resolve(&none, &empty, Some(("https://rt.supabase.co".into(), "sb_publishable_y".into()))), AccountsConfig::Supabase { .. }));
    assert!(matches!(config::resolve(&none, &empty, Some(("ws://127.0.0.1:1".into(), "k".into()))), AccountsConfig::Missing(_)));
}

// ---------- armazenamento cifrado ----------

struct Xor;
impl Protector for Xor {
    fn protect(&self, p: &[u8]) -> anyhow::Result<Vec<u8>> {
        Ok(p.iter().map(|b| b ^ 0x5a).collect())
    }
    fn unprotect(&self, s: &[u8]) -> anyhow::Result<Vec<u8>> {
        Ok(s.iter().map(|b| b ^ 0x5a).collect())
    }
}

#[test]
fn secret_file_format_roundtrip_and_clear() {
    let dir = tmpdir("store1");
    let f = SecretFile::new(dir.join("s.bin"), Box::new(Xor));
    assert_eq!(f.load::<Value>(), None);
    f.save(&json!({ "refresh_token": "rt-segredo" })).unwrap();
    assert_eq!(f.load::<Value>().unwrap()["refresh_token"], "rt-segredo");
    std::fs::write(dir.join("s.bin"), b"lixo").unwrap();
    assert_eq!(f.load::<Value>(), None, "arquivo estranho = sem sessão");
    f.clear();
    assert!(!dir.join("s.bin").exists());
}

#[cfg(windows)]
#[test]
fn dpapi_never_writes_plaintext_and_detects_tampering() {
    let dir = tmpdir("store2");
    let f = SecretFile::dpapi(dir.join("s.bin"));
    f.save(&json!({ "refresh_token": "rt-super-secreto-123", "user": { "email": "voce@exemplo.com" } })).unwrap();
    let raw = std::fs::read(dir.join("s.bin")).unwrap();
    let hay = String::from_utf8_lossy(&raw);
    assert!(!hay.contains("rt-super-secreto-123") && !hay.contains("voce@exemplo.com") && !hay.contains("refresh_token"));
    assert_eq!(f.load::<Value>().unwrap()["refresh_token"], "rt-super-secreto-123");
    let mut bad = raw.clone();
    let n = bad.len();
    bad[n - 5] ^= 0xff;
    std::fs::write(dir.join("s.bin"), &bad).unwrap();
    assert_eq!(f.load::<Value>(), None, "blob adulterado não abre");
}

// ---------- tradução de erros ----------

#[test]
fn server_errors_become_clear_portuguese_messages() {
    let m = |status: u16, v: Value| map_error(status, &v);
    assert_eq!(m(400, json!({ "error_code": "invalid_credentials", "msg": "Invalid login credentials" })).code, "invalid_credentials");
    assert_eq!(m(400, json!({ "error": "invalid_grant", "error_description": "Invalid login credentials" })).message, "E-mail ou senha incorretos.");
    assert_eq!(m(400, json!({ "error_code": "email_not_confirmed", "msg": "Email not confirmed" })).code, "email_not_confirmed");
    assert_eq!(m(422, json!({ "error_code": "user_already_exists", "msg": "User already registered" })).code, "user_exists");
    assert_eq!(m(422, json!({ "error_code": "weak_password", "msg": "Password should be at least 8 characters" })).code, "weak_password");
    assert_eq!(m(429, json!({ "error_code": "over_email_send_rate_limit", "msg": "x" })).code, "rate_limited");
    assert_eq!(m(403, json!({ "error_code": "otp_expired", "msg": "Token has expired or is invalid" })).code, "invalid_code");
    assert_eq!(m(400, json!({ "error_code": "refresh_token_not_found", "msg": "Invalid Refresh Token: Refresh Token Not Found" })).code, "session_expired");
    assert_eq!(m(401, json!({ "code": "PGRST301", "message": "JWT expired" })).code, "session_expired");
    // formato visto no Supabase real para um refresh token desconhecido
    assert_eq!(m(400, json!({ "code": 400, "error_code": "validation_failed", "msg": "Refresh token is not valid" })).code, "session_expired");
    assert_eq!(m(404, json!({ "code": "PGRST202", "message": "Could not find the function public.friends_list" })).code, "server_not_ready");
    assert_eq!(m(400, json!({ "code": "P0001", "message": "nick_taken" })).code, "nick_taken");
    assert_eq!(m(400, json!({ "code": "P0001", "message": "not_found" })).code, "not_found");
    assert_eq!(m(500, Value::Null).code, "server");
    for e in [m(400, json!({ "error_code": "invalid_credentials" })), m(500, Value::Null), AccountError::network()] {
        assert!(!e.message.is_empty() && !e.message.contains("error"), "mensagem em português: {}", e.message);
    }
}

// ---------- loopback ----------

fn get(url: String) {
    std::thread::spawn(move || {
        let _ = ureq::AgentBuilder::new().redirects(0).timeout(Duration::from_secs(5)).build().get(&url).call();
    });
}

#[tokio::test]
async fn loopback_returns_code_and_ignores_other_paths() {
    let lb = loopback::bind_ports(&[0]).await.unwrap();
    assert!(lb.redirect_uri.starts_with("http://127.0.0.1:") && lb.redirect_uri.ends_with("/callback"));
    let base = lb.redirect_uri.trim_end_matches("/callback").to_string();
    get(format!("{base}/favicon.ico"));
    get(format!("{base}/callback"));   // sem parâmetros: continua esperando
    tokio::time::sleep(Duration::from_millis(150)).await;
    get(format!("{base}/callback?code=abc-123&x=1"));
    assert_eq!(lb.wait_code(Duration::from_secs(5)).await.unwrap(), "abc-123");
}

#[tokio::test]
async fn loopback_maps_denied_and_timeout() {
    let lb = loopback::bind_ports(&[0]).await.unwrap();
    get(format!("{}?error=access_denied&error_description=The+user+denied", lb.redirect_uri));
    assert_eq!(lb.wait_code(Duration::from_secs(5)).await.unwrap_err().code, "cancelled");
    let lb = loopback::bind_ports(&[0]).await.unwrap();
    get(format!("{}?error=access_denied&error_code=otp_expired&error_description=Email+link+is+invalid+or+has+expired", lb.redirect_uri));
    assert_eq!(lb.wait_code(Duration::from_secs(5)).await.unwrap_err().code, "invalid_code");
    let lb = loopback::bind_ports(&[0]).await.unwrap();
    assert_eq!(lb.wait_code(Duration::from_millis(100)).await.unwrap_err().code, "timeout");
}

// ---------- fluxos completos contra o servidor falso ----------

#[tokio::test]
async fn oauth_pkce_login_persists_encrypted_and_restores() {
    let _g = FLOW.lock().await;
    let f = fake::start(0).await.unwrap();
    let dir = tmpdir("oauth");
    let a = accounts(&f, &dir);
    assert_eq!(a.restore().await.unwrap(), None);
    let u = a.sign_in_oauth(OAuthProvider::Google).await.unwrap();
    assert_eq!((u.email.as_str(), u.provider.as_str(), u.display_name.as_str()), ("google@mock.briks", "google", "Conta Google"));
    let tok = a.access_token().await.unwrap();
    assert!(tok.starts_with("at-"));
    // em disco: só o blob cifrado
    let raw = std::fs::read(dir.join("account-session.bin")).unwrap();
    let hay = String::from_utf8_lossy(&raw);
    assert!(!hay.contains("rt-") && !hay.contains(&tok) && !hay.contains("google@mock.briks"));
    // "reabrir o app": outra instância retoma pela sessão guardada (refresh token rotacionado)
    let b = accounts(&f, &dir);
    assert_eq!(b.restore().await.unwrap().unwrap().id, u.id);
    assert_eq!(f.state.lock().unwrap().refresh_count, 1);
    // Discord é outra conta
    b.sign_out().await;
    assert!(!dir.join("account-session.bin").exists(), "sair apaga a sessão guardada");
    assert_eq!(b.restore().await.unwrap(), None);
    assert_eq!(b.access_token().await.unwrap_err().code, "session_expired");
    let d = b.sign_in_oauth(OAuthProvider::Discord).await.unwrap();
    assert_ne!(d.id, u.id);
}

#[tokio::test]
async fn oauth_denied_is_cancelled_and_stolen_code_is_useless() {
    let _g = FLOW.lock().await;
    let f = fake::start(0).await.unwrap();
    let dir = tmpdir("deny");
    let a = accounts(&f, &dir);
    f.state.lock().unwrap().deny_oauth = true;
    assert_eq!(a.sign_in_oauth(OAuthProvider::Google).await.unwrap_err().code, "cancelled");
    assert!(a.user().is_none() && !dir.join("account-session.bin").exists());
    f.state.lock().unwrap().deny_oauth = false;

    // um código interceptado não vira sessão sem o verificador que ficou no processo
    let p = SupabaseProvider::new(&f.url(), "k");
    let pk = Pkce::generate();
    let lb = loopback::bind_ports(&[0]).await.unwrap();
    (fake::auto_browser())(p.oauth_url(OAuthProvider::Google, &lb.redirect_uri, &pk.challenge)).unwrap();
    let code = lb.wait_code(Duration::from_secs(5)).await.unwrap();
    let (p2, c2) = (SupabaseProvider::new(&f.url(), "k"), code.clone());
    let wrong = tokio::task::spawn_blocking(move || p2.exchange_code(&c2, &Pkce::generate().verifier)).await.unwrap();
    assert_eq!(wrong.unwrap_err().code, "invalid_code");
    // e o código é de uso único: nem o verificador certo serve depois
    let again = tokio::task::spawn_blocking(move || p.exchange_code(&code, &pk.verifier)).await.unwrap();
    assert!(again.is_err());
}

#[tokio::test]
async fn email_signup_confirmation_login_and_errors() {
    let _g = FLOW.lock().await;
    let f = fake::start(0).await.unwrap();
    let a = accounts(&f, &tmpdir("email"));
    // validação local, antes de qualquer rede
    assert_eq!(a.sign_up("sem-arroba", "senha-boa-1", "Ana").await.err().unwrap().code, "invalid_email");
    assert_eq!(a.sign_up("ana@exemplo.com", "curta", "Ana").await.err().unwrap().code, "weak_password");
    assert_eq!(a.sign_up("ana@exemplo.com", "senha-boa-1", "  ").await.err().unwrap().code, "invalid_name");

    let SignUpOutcome::ConfirmEmail(w) = a.sign_up("Ana@Exemplo.com", "senha-boa-1", "Ana").await.unwrap() else { panic!("deveria pedir confirmação") };
    assert!(a.user().is_none());
    assert_eq!(a.sign_in_password("ana@exemplo.com", "senha-boa-1").await.unwrap_err().code, "email_not_confirmed");
    // clicar no link do e-mail neste computador conclui o cadastro
    assert!(fake::click_last_link(&f.state));
    let u = a.finish_email(w).await.unwrap();
    assert_eq!((u.email.as_str(), u.display_name.as_str(), u.provider.as_str()), ("ana@exemplo.com", "Ana", "email"));
    a.sign_out().await;

    assert_eq!(a.sign_in_password("ana@exemplo.com", "senha-errada").await.unwrap_err().code, "invalid_credentials");
    assert_eq!(a.sign_in_password("ninguem@exemplo.com", "senha-boa-1").await.unwrap_err().code, "invalid_credentials");
    assert_eq!(a.sign_up("ana@exemplo.com", "senha-boa-1", "Ana").await.err().unwrap().code, "user_exists");
    assert_eq!(a.sign_in_password("ana@exemplo.com", "senha-boa-1").await.unwrap().id, u.id);
    a.sign_out().await;

    // confirmação pelo código de 6 dígitos (sem link)
    let SignUpOutcome::ConfirmEmail(w) = a.sign_up("bia@exemplo.com", "senha-boa-2", "Bia").await.unwrap() else { panic!() };
    drop(w);
    assert_eq!(a.verify_code("bia@exemplo.com", "000000", CodeKind::Signup).await.unwrap_err().code, "invalid_code");
    let SignUpOutcome::ConfirmEmail(_) = a.sign_up("caio@exemplo.com", "senha-boa-3", "Caio").await.unwrap() else { panic!() };
    assert_eq!(a.verify_code("caio@exemplo.com", " 123 456 ", CodeKind::Signup).await.unwrap().display_name, "Caio");
}

#[tokio::test]
async fn password_recovery_by_link_and_by_code() {
    let _g = FLOW.lock().await;
    let f = fake::start(0).await.unwrap();
    f.state.lock().unwrap().autoconfirm = true;
    let a = accounts(&f, &tmpdir("recover"));
    let SignUpOutcome::SignedIn(u) = a.sign_up("dan@exemplo.com", "senha-antiga-1", "Dan").await.unwrap() else { panic!("autoconfirm") };
    a.sign_out().await;

    // e-mail desconhecido: mesma resposta, sem revelar que a conta não existe
    let w = a.recover("ninguem@exemplo.com").await.unwrap();
    assert!(f.state.lock().unwrap().last_link.is_none());
    drop(w);

    let w = a.recover("dan@exemplo.com").await.unwrap();
    assert!(fake::click_last_link(&f.state));
    assert_eq!(a.finish_email(w).await.unwrap().id, u.id);
    assert_eq!(a.set_password("curta").await.unwrap_err().code, "weak_password");
    a.set_password("senha-nova-22").await.unwrap();
    a.sign_out().await;
    assert_eq!(a.sign_in_password("dan@exemplo.com", "senha-antiga-1").await.unwrap_err().code, "invalid_credentials");
    assert_eq!(a.sign_in_password("dan@exemplo.com", "senha-nova-22").await.unwrap().id, u.id);
    a.sign_out().await;

    let _w = a.recover("dan@exemplo.com").await.unwrap();
    assert_eq!(a.verify_code("dan@exemplo.com", fake::FAKE_OTP, CodeKind::Recovery).await.unwrap().id, u.id);
}

#[tokio::test]
async fn token_refreshes_itself_and_dead_session_is_dropped() {
    let _g = FLOW.lock().await;
    let f = fake::start(0).await.unwrap();
    {
        let mut st = f.state.lock().unwrap();
        st.autoconfirm = true;
        st.expires_in = 30;   // menos que a folga de 60 s: todo pedido de token renova
    }
    let dir = tmpdir("refresh");
    let a = Arc::new(accounts(&f, &dir));
    let SignUpOutcome::SignedIn(_) = a.sign_up("eva@exemplo.com", "senha-boa-1", "Eva").await.unwrap() else { panic!() };
    assert_eq!(a.refresh_in(), Some(0));
    let t1 = a.access_token().await.unwrap();
    let t2 = a.access_token().await.unwrap();
    assert_ne!(t1, t2);
    assert_eq!(f.state.lock().unwrap().refresh_count, 2);
    // renovações simultâneas não queimam o refresh token de uso único
    f.state.lock().unwrap().expires_in = 3600;
    let (x, y) = tokio::join!(a.access_token(), a.access_token());
    assert_eq!(x.unwrap(), y.unwrap());
    assert_eq!(f.state.lock().unwrap().refresh_count, 3);
    assert!(a.refresh_in().unwrap() > 3000);

    // servidor fora do ar: erro de rede, sessão guardada continua lá
    let dead = Accounts::new(Arc::new(SupabaseProvider::new("http://127.0.0.1:9", "k")), &dir, fake::auto_browser());
    assert_eq!(dead.restore().await.unwrap_err().code, "network");
    assert!(dir.join("account-session.bin").exists());

    // sessão revogada no servidor: a retomada limpa o que estava guardado
    let b = accounts(&f, &dir);
    let tok = a.access_token().await.unwrap();
    let p = SupabaseProvider::new(&f.url(), "k");
    tokio::task::spawn_blocking(move || p.sign_out(&tok)).await.unwrap().unwrap();
    assert_eq!(b.restore().await.unwrap(), None);
    assert!(!dir.join("account-session.bin").exists());
}

#[tokio::test]
async fn profile_publishes_only_the_public_key_and_friends_find_each_other() {
    let _g = FLOW.lock().await;
    let f = fake::start(0).await.unwrap();
    f.state.lock().unwrap().autoconfirm = true;
    let (a, b) = (accounts(&f, &tmpdir("fr-a")), accounts(&f, &tmpdir("fr-b")));
    let SignUpOutcome::SignedIn(ua) = a.sign_up("ana@exemplo.com", "senha-boa-1", "Ana").await.unwrap() else { panic!() };
    let SignUpOutcome::SignedIn(ub) = b.sign_up("bia@exemplo.com", "senha-boa-1", "Bia").await.unwrap() else { panic!() };

    let (ida, idb) = (crate::identity::Identity::generate(), crate::identity::Identity::generate());
    let jwk = serde_json::to_value(ida.public_jwk()).unwrap();
    let mut keys: Vec<&str> = jwk.as_object().unwrap().keys().map(String::as_str).collect();
    keys.sort();
    assert_eq!(keys, ["crv", "kty", "x", "y"], "só a parte pública");
    assert!(!jwk.to_string().contains(&ida.secret_b64()));
    let pa = a.publish_identity(jwk, &ida.fingerprint()).await.unwrap();
    assert_eq!((pa.user_id.as_str(), pa.display_name.as_str(), pa.fingerprint.as_deref()), (ua.id.as_str(), "Ana", Some(ida.fingerprint().as_str())));
    assert!(!pa.nick.is_empty());
    // o servidor recusa uma chave com a parte privada
    let p = SupabaseProvider::new(&f.url(), "k");
    let tok = a.access_token().await.unwrap();
    let leak = tokio::task::spawn_blocking(move || p.sync_profile(&tok, "Ana", "", &json!({ "kty": "EC", "crv": "P-256", "x": "x", "y": "y", "d": "segredo" }), "fp")).await.unwrap();
    assert_eq!(leak.unwrap_err().code, "server");

    b.publish_identity(serde_json::to_value(idb.public_jwk()).unwrap(), &idb.fingerprint()).await.unwrap();
    assert_eq!(b.set_nick("@Bia_01").await.unwrap().nick, "bia_01");
    assert_eq!(a.set_nick("bia_01").await.unwrap_err().code, "nick_taken");
    assert_eq!(a.set_nick("x").await.unwrap_err().code, "invalid_nick");

    assert_eq!(a.friend_request("nao_existe").await.unwrap_err().code, "not_found");
    assert_eq!(a.friend_request("!!").await.unwrap_err().code, "invalid_nick");
    assert_eq!(a.friend_request(&pa.nick).await.unwrap_err().code, "self_request");
    a.friend_request("@bia_01").await.unwrap();
    assert_eq!(b.friend_request(&pa.nick).await.unwrap_err().code, "already_requested");

    // pedido pendente: quem recebe vê nome e nick, mas não a chave nem a impressão digital
    let inc = b.friends().await.unwrap();
    assert_eq!((inc.len(), inc[0].state.as_str(), inc[0].display_name.as_str()), (1, "incoming", "Ana"));
    assert!(inc[0].fingerprint.is_none() && inc[0].public_key.is_none());
    assert_eq!(a.friends().await.unwrap()[0].state, "outgoing");
    // só quem recebeu pode aceitar
    let rid = inc[0].request_id.clone().unwrap();
    assert_eq!(a.friend_respond(&rid, true).await.unwrap_err().code, "not_found");
    b.friend_respond(&rid, true).await.unwrap();
    let fa = a.friends().await.unwrap();
    assert_eq!((fa[0].state.as_str(), fa[0].user_id.as_str(), fa[0].fingerprint.as_deref()), ("friend", ub.id.as_str(), Some(idb.fingerprint().as_str())));
    assert_eq!(fa[0].public_key.as_ref().unwrap()["x"], idb.public_jwk().x.as_str());
    assert_eq!(a.friend_request("bia_01").await.unwrap_err().code, "already_friends");
    a.friend_remove(&ub.id).await.unwrap();
    assert!(b.friends().await.unwrap().is_empty());
}

#[tokio::test]
async fn blocking_removes_the_bond_hides_the_requests_and_falls_back_without_the_rpc() {
    let _g = FLOW.lock().await;
    let f = fake::start(0).await.unwrap();
    f.state.lock().unwrap().autoconfirm = true;
    let (a, b) = (accounts(&f, &tmpdir("bl-a")), accounts(&f, &tmpdir("bl-b")));
    let SignUpOutcome::SignedIn(_ua) = a.sign_up("ana@exemplo.com", "senha-boa-1", "Ana").await.unwrap() else { panic!() };
    let SignUpOutcome::SignedIn(ub) = b.sign_up("bia@exemplo.com", "senha-boa-1", "Bia").await.unwrap() else { panic!() };
    let (ida, idb) = (crate::identity::Identity::generate(), crate::identity::Identity::generate());
    let pa = a.publish_identity(serde_json::to_value(ida.public_jwk()).unwrap(), &ida.fingerprint()).await.unwrap();
    b.publish_identity(serde_json::to_value(idb.public_jwk()).unwrap(), &idb.fingerprint()).await.unwrap();
    b.set_nick("bia_01").await.unwrap();

    a.friend_request("bia_01").await.unwrap();
    // Bia bloqueia a Ana: o pedido some e a Ana não descobre (a resposta é igual a "@nick inexistente")
    b.friend_block(&pa.user_id).await.unwrap();
    assert!(b.friends().await.unwrap().is_empty());
    let bl = b.blocks().await.unwrap();
    assert_eq!((bl.len(), bl[0].user_id.as_str(), bl[0].state.as_str()), (1, pa.user_id.as_str(), "blocked"));
    assert_eq!(a.friend_request("bia_01").await.unwrap_err().code, "not_found");
    // quem bloqueou não consegue pedir amizade ao bloqueado sem desbloquear
    assert_eq!(b.friend_request(&pa.nick).await.unwrap_err().code, "blocked_by_you");
    assert_eq!(b.friend_block(&ub.id).await.unwrap_err().code, "not_found", "não bloqueia a si mesmo (perfil inválido)");
    b.friend_unblock(&pa.user_id).await.unwrap();
    assert!(b.blocks().await.unwrap().is_empty());
    a.friend_request("bia_01").await.unwrap();

    // servidor sem a migração 0006: server_not_ready (a UI mantém a lista local)
    f.state.lock().unwrap().no_blocks_rpc = true;
    assert_eq!(b.friend_block(&pa.user_id).await.unwrap_err().code, "server_not_ready");
    assert_eq!(b.blocks().await.unwrap_err().code, "server_not_ready");
}

#[tokio::test]
async fn presence_shows_only_accepted_unblocked_visible_friends_and_expires() {
    let _g = FLOW.lock().await;
    let f = fake::start(0).await.unwrap();
    f.state.lock().unwrap().autoconfirm = true;
    let (a, b, c) = (accounts(&f, &tmpdir("pr-a")), accounts(&f, &tmpdir("pr-b")), accounts(&f, &tmpdir("pr-c")));
    let SignUpOutcome::SignedIn(_) = a.sign_up("ana@exemplo.com", "senha-boa-1", "Ana").await.unwrap() else { panic!() };
    let SignUpOutcome::SignedIn(_) = b.sign_up("bia@exemplo.com", "senha-boa-1", "Bia").await.unwrap() else { panic!() };
    let SignUpOutcome::SignedIn(_) = c.sign_up("cai@exemplo.com", "senha-boa-1", "Cai").await.unwrap() else { panic!() };
    let mut ids = vec![];
    for (acc, nick) in [(&a, "ana_01"), (&b, "bia_01"), (&c, "cai_01")] {
        let id = crate::identity::Identity::generate();
        let p = acc.publish_identity(serde_json::to_value(id.public_jwk()).unwrap(), &id.fingerprint()).await.unwrap();
        acc.set_nick(nick).await.unwrap();
        ids.push(p.user_id);
    }
    // Ana pede a Bia (aceita) e à Cai (só pedido, não aceita)
    a.friend_request("bia_01").await.unwrap();
    a.friend_request("cai_01").await.unwrap();
    let req = b.friends().await.unwrap().into_iter().find(|x| x.state == "incoming").unwrap();
    b.friend_respond(req.request_id.as_deref().unwrap(), true).await.unwrap();
    b.presence_beat(false).await.unwrap();
    c.presence_beat(false).await.unwrap();
    let l = a.presence_friends().await.unwrap();
    assert_eq!(l.len(), 1, "só amigo aceito aparece (Cai é só pedido)");
    assert_eq!((l[0].user_id.as_str(), l[0].online), (ids[1].as_str(), true));
    // invisível: some da lista
    b.presence_beat(true).await.unwrap();
    assert!(a.presence_friends().await.unwrap().is_empty());
    b.presence_beat(false).await.unwrap();
    // bloqueio (em qualquer direção) esconde
    a.friend_block(&ids[1]).await.unwrap();
    assert!(a.presence_friends().await.unwrap().is_empty());
    assert!(b.presence_friends().await.unwrap().is_empty());
    // sem batimento por mais de 2 min: continua na lista, mas offline
    a.friend_unblock(&ids[1]).await.unwrap();
    a.friend_request("bia_01").await.unwrap();
    let req = b.friends().await.unwrap().into_iter().find(|x| x.state == "incoming").unwrap();
    b.friend_respond(req.request_id.as_deref().unwrap(), true).await.unwrap();
    b.presence_beat(false).await.unwrap();
    assert!(a.presence_friends().await.unwrap()[0].online);
    f.state.lock().unwrap().clock_skew = 130;
    assert!(!a.presence_friends().await.unwrap()[0].online);
    // sair da conta: some na hora
    b.presence_clear().await.unwrap();
    assert!(a.presence_friends().await.unwrap().is_empty());
    // servidor sem a 0007
    f.state.lock().unwrap().no_presence_rpc = true;
    assert_eq!(a.presence_beat(false).await.unwrap_err().code, "server_not_ready");
    assert_eq!(a.presence_friends().await.unwrap_err().code, "server_not_ready");
}

#[tokio::test]
async fn nick_lookup_is_exact_hides_blocked_and_rate_limited() {
    let _g = FLOW.lock().await;
    let f = fake::start(0).await.unwrap();
    f.state.lock().unwrap().autoconfirm = true;
    let (a, b) = (accounts(&f, &tmpdir("nl-a")), accounts(&f, &tmpdir("nl-b")));
    let SignUpOutcome::SignedIn(_) = a.sign_up("ana@exemplo.com", "senha-boa-1", "Ana").await.unwrap() else { panic!() };
    let SignUpOutcome::SignedIn(_) = b.sign_up("bia@exemplo.com", "senha-boa-1", "Bia").await.unwrap() else { panic!() };
    let (ida, idb) = (crate::identity::Identity::generate(), crate::identity::Identity::generate());
    let pa = a.publish_identity(serde_json::to_value(ida.public_jwk()).unwrap(), &ida.fingerprint()).await.unwrap();
    let pb = b.publish_identity(serde_json::to_value(idb.public_jwk()).unwrap(), &idb.fingerprint()).await.unwrap();
    a.set_nick("ana_01").await.unwrap();
    b.set_nick("bia_01").await.unwrap();
    let hit = a.nick_lookup("@Bia_01").await.unwrap();
    assert_eq!((hit.user_id.as_str(), hit.nick.as_str(), hit.fingerprint.as_str()), (pb.user_id.as_str(), "bia_01", idb.fingerprint().as_str()));
    // exato: prefixo, inexistente e a própria pessoa respondem igual
    assert_eq!(a.nick_lookup("bia").await.unwrap_err().code, "not_found");
    assert_eq!(a.nick_lookup("ninguem_x").await.unwrap_err().code, "not_found");
    assert_eq!(a.nick_lookup("ana_01").await.unwrap_err().code, "not_found");
    // bloqueio nos dois sentidos esconde
    b.friend_block(&pa.user_id).await.unwrap();
    assert_eq!(a.nick_lookup("bia_01").await.unwrap_err().code, "not_found");
    b.friend_unblock(&pa.user_id).await.unwrap();
    a.friend_block(&pb.user_id).await.unwrap();
    assert_eq!(a.nick_lookup("bia_01").await.unwrap_err().code, "not_found");
    a.friend_unblock(&pb.user_id).await.unwrap();
    // anti-enumeração: 30 buscas por hora
    let mut last = String::new();
    for _ in 0..30 { if let Err(e) = a.nick_lookup("ninguem_x").await { last = e.code; } }
    assert_eq!(last, "rate_limited");
    // servidor sem a 0008
    f.state.lock().unwrap().no_lookup_rpc = true;
    assert_eq!(b.nick_lookup("ana_01").await.unwrap_err().code, "server_not_ready");
}

#[test]
fn group_image_is_cropped_stored_per_group_and_cleared() {
    let dir = tmpdir("grp-img");
    let src = dir.join("origem.png");
    image::RgbImage::from_pixel(300, 120, image::Rgb([200, 30, 90])).save(&src).unwrap();
    let jpeg = super::avatar::prepare(&src).unwrap();
    let p = super::avatar::store(&dir, "grp-g1", &jpeg).unwrap();
    assert!(p.exists() && p.to_string_lossy().contains("grp-g1"));
    assert_eq!(super::avatar::cached(&dir, "grp-g1"), Some(p));
    assert!(super::avatar::cached(&dir, "grp-g2").is_none(), "cada grupo tem a sua imagem");
    let img = image::open(super::avatar::cached(&dir, "grp-g1").unwrap()).unwrap();
    assert_eq!((img.width(), img.height()), (120, 120), "recorte quadrado central");
    super::avatar::clear(&dir, "grp-g1");
    assert!(super::avatar::cached(&dir, "grp-g1").is_none());
    std::fs::write(dir.join("ruim.png"), b"nao e imagem").unwrap();
    assert!(super::avatar::prepare(&dir.join("ruim.png")).is_err());
}

#[tokio::test]
async fn local_bridge_password_is_stable_per_account_and_encrypted() {
    let _g = FLOW.lock().await;
    let f = fake::start(0).await.unwrap();
    f.state.lock().unwrap().autoconfirm = true;
    let dir = tmpdir("bridge");
    let a = accounts(&f, &dir);
    assert_eq!(a.local_credentials().unwrap_err().code, "session_expired");
    let SignUpOutcome::SignedIn(u) = a.sign_up("gil@exemplo.com", "senha-boa-1", "Gil").await.unwrap() else { panic!() };
    let (email, pw) = a.local_credentials().unwrap();
    assert_eq!(email, format!("{}@conta.briks.local", u.id));
    assert_eq!(pw.len(), 48);
    assert_ne!(pw, "senha-boa-1");
    assert_eq!(a.local_credentials().unwrap().1, pw);
    assert!(!String::from_utf8_lossy(&std::fs::read(dir.join("local-bridge.bin")).unwrap()).contains(&pw));
    // sair e entrar de novo: o histórico local continua acessível
    a.sign_out().await;
    a.sign_in_password("gil@exemplo.com", "senha-boa-1").await.unwrap();
    assert_eq!(a.local_credentials().unwrap().1, pw);
}

// ---------- serviço (o que a UI enxerga pelo IPC) ----------

async fn next(rx: &mut tokio::sync::mpsc::Receiver<Value>) -> Value {
    tokio::time::timeout(Duration::from_secs(10), rx.recv()).await.expect("evento a tempo").expect("canal aberto")
}

#[tokio::test]
async fn service_emits_states_for_login_cancel_and_logout() {
    let _g = FLOW.lock().await;
    let f = fake::start(0).await.unwrap();
    let dir = tmpdir("svc");
    let rt = service::Runtime::for_tests(Some(Arc::new(accounts(&f, &dir))), dir.clone());
    let (tx, mut rx) = tokio::sync::mpsc::channel(64);
    let mut svc = service::Service::new(rt, tx);
    assert!(!svc.command("room.join", &json!({})), "comandos de outros módulos passam direto");

    assert!(svc.command("account.status", &json!({})));
    assert_eq!(next(&mut rx).await["state"], "busy");
    let e = next(&mut rx).await;
    assert_eq!((e["state"].as_str(), e["configured"].as_bool()), (Some("signed_out"), Some(true)));
    assert!(e.get("error_code").is_none());

    svc.command("account.oauth", &json!({ "provider": "discord" }));
    let e = next(&mut rx).await;
    assert_eq!((e["state"].as_str(), e["what"].as_str(), e["provider"].as_str()), (Some("busy"), Some("oauth"), Some("discord")));
    let e = next(&mut rx).await;
    assert_eq!((e["state"].as_str(), e["user"]["email"].as_str()), (Some("signed_in"), Some("discord@mock.briks")));
    assert!(e["user"].get("refresh_token").is_none() && !e.to_string().contains("at-") && !e.to_string().contains("rt-"), "tokens não vão para a UI");
    // a chave pública da identidade deste aparelho foi publicada no perfil
    let e = next(&mut rx).await;
    assert_eq!(e["ev"], "account.profile");
    let id = crate::ipc::load_or_create_identity(&dir).unwrap();
    assert_eq!(e["fingerprint"], id.fingerprint().as_str());
    let uid = f.state.lock().unwrap().users[0].id.clone();
    let prof = f.state.lock().unwrap().profiles[&uid].clone();
    assert_eq!(prof["public_key"]["x"], id.public_jwk().x.as_str());
    assert!(!prof.to_string().contains(&id.secret_b64()), "a chave privada nunca chega ao servidor");

    assert_eq!(next(&mut rx).await["ev"], "account.avatar");
    svc.command("friends.list", &json!({}));
    assert_eq!(next(&mut rx).await["ev"], "friends.list");
    svc.command("local.credentials", &json!({}));
    assert_eq!(next(&mut rx).await["ev"], "local.credentials");

    svc.command("account.logout", &json!({}));
    assert_eq!(next(&mut rx).await["state"], "signed_out");
    assert!(!dir.join("account-session.bin").exists());

    // login por e-mail com erro: volta para "fora" com a mensagem
    svc.command("account.login", &json!({ "email": "x@y.co", "password": "nada" }));
    assert_eq!(next(&mut rx).await["state"], "busy");
    let e = next(&mut rx).await;
    assert_eq!((e["state"].as_str(), e["error_code"].as_str(), e["message"].as_str()), (Some("signed_out"), Some("invalid_credentials"), Some("E-mail ou senha incorretos.")));

    // cancelar enquanto o navegador está aberto
    let slow = Accounts::with_stores(Arc::new(SupabaseProvider::new(&f.url(), "k")), SecretFile::dpapi(dir.join("x.bin")), SecretFile::dpapi(dir.join("y.bin")), Arc::new(|_| Ok(())));
    let rt = service::Runtime::for_tests(Some(Arc::new(slow)), dir.clone());
    let (tx, mut rx) = tokio::sync::mpsc::channel(64);
    let mut svc = service::Service::new(rt, tx);
    svc.command("account.oauth", &json!({ "provider": "google" }));
    assert_eq!(next(&mut rx).await["state"], "busy");
    tokio::time::sleep(Duration::from_millis(100)).await;
    svc.command("account.cancel", &json!({}));
    let e = next(&mut rx).await;
    assert_eq!((e["state"].as_str(), e["error_code"].as_str()), (Some("signed_out"), Some("cancelled")));
    // a porta do loopback foi liberada: dá para tentar de novo na hora
    tokio::time::sleep(Duration::from_millis(100)).await;
    assert!(loopback::bind().await.unwrap().redirect_uri.contains(":53682/"));

    // sem configuração: qualquer comando explica o motivo
    let rt = service::Runtime::for_tests(None, dir.clone());
    let (tx, mut rx) = tokio::sync::mpsc::channel(64);
    let mut svc = service::Service::new(rt, tx);
    svc.command("account.status", &json!({}));
    let e = next(&mut rx).await;
    assert_eq!((e["state"].as_str(), e["configured"].as_bool(), e["error_code"].as_str()), (Some("signed_out"), Some(false), Some("not_configured")));
}

// ---------- importação do Briks antigo (sem a identidade antiga) ----------

#[test]
fn legacy_import_reassigns_friends_without_old_accounts_or_keys() {
    use rusqlite::Connection;
    let (s, d) = (tmpdir("leg-s"), tmpdir("leg-d"));
    assert_eq!(legacy::probe(&s, &d)["state"], "no-source");
    let schema = "CREATE TABLE users(id BLOB PRIMARY KEY, display_name TEXT, email TEXT, password_hash TEXT, created_at INTEGER);
        CREATE TABLE p2p_friends(owner_user_id BLOB NOT NULL, peer_id TEXT NOT NULL, display_name TEXT NOT NULL, public_key_json TEXT NOT NULL, avatar TEXT, added_at INTEGER NOT NULL, PRIMARY KEY(owner_user_id, peer_id));
        CREATE TABLE p2p_groups(owner_user_id BLOB NOT NULL, group_id TEXT NOT NULL, name TEXT NOT NULL, channels_json TEXT NOT NULL, members_json TEXT NOT NULL, joined_at INTEGER NOT NULL, PRIMARY KEY(owner_user_id, group_id));
        CREATE TABLE p2p_messages(owner_user_id BLOB NOT NULL, channel_id TEXT NOT NULL, id TEXT NOT NULL, author TEXT NOT NULL, content TEXT NOT NULL, created_at TEXT NOT NULL, PRIMARY KEY(owner_user_id, channel_id, id));";
    {
        let c = Connection::open(s.join("risk.sqlite3")).unwrap();
        c.execute_batch(schema).unwrap();
        c.execute_batch("ALTER TABLE p2p_groups ADD COLUMN coluna_so_do_antigo TEXT;
            INSERT INTO users VALUES(x'01','Ygor antigo','antigo@local','hash-antigo',1);
            INSERT INTO users VALUES(x'02','Outro','outro@local','hash-2',1);
            INSERT INTO p2p_friends VALUES(x'01','p1','Ana','{}',NULL,1),(x'01','p2','Bia','{}',NULL,2),(x'02','p9','De outro perfil','{}',NULL,3);
            INSERT INTO p2p_groups VALUES(x'01','g1','Squad','[]','[]',1,NULL);
            INSERT INTO p2p_messages VALUES(x'01','g1-geral','m1','Ana','oi','2025-01-01T00:00:00Z');").unwrap();
    }
    let before = std::fs::read(s.join("risk.sqlite3")).unwrap();
    let p = legacy::probe(&s, &d);
    assert_eq!((p["state"].as_str(), p["friends"].as_i64(), p["groups"].as_i64(), p["messages"].as_i64()), (Some("ready"), Some(2), Some(1), Some(1)));
    assert!(legacy::apply(&s, &d, "novo@conta.briks.local").is_err(), "sem banco local ainda");
    {
        let c = Connection::open(d.join("risk.sqlite3")).unwrap();
        c.execute_batch(schema).unwrap();
        c.execute_batch("INSERT INTO users VALUES(x'aa','Conta nova','novo@conta.briks.local','h',1);
            INSERT INTO p2p_friends VALUES(x'aa','p1','Ana (já editada)','{}',NULL,9);").unwrap();
    }
    assert!(legacy::apply(&s, &d, "desconhecido@conta.briks.local").is_err());
    let r = legacy::apply(&s, &d, "NOVO@conta.briks.local").unwrap();
    assert_eq!((r["state"].as_str(), r["report"]["friends"].as_i64(), r["report"]["groups"].as_i64(), r["report"]["messages"].as_i64()), (Some("applied"), Some(1), Some(1), Some(1)));
    let c = Connection::open(d.join("risk.sqlite3")).unwrap();
    let q = |sql: &str| -> i64 { c.query_row(sql, [], |r| r.get(0)).unwrap() };
    assert_eq!(q("SELECT count(*) FROM p2p_friends WHERE owner_user_id=x'aa'"), 2);
    assert_eq!(q("SELECT count(*) FROM p2p_friends WHERE display_name='Ana (já editada)'"), 1, "não sobrescreve");
    assert_eq!(q("SELECT count(*) FROM p2p_friends WHERE display_name='De outro perfil'"), 0);
    assert_eq!(q("SELECT count(*) FROM p2p_groups WHERE owner_user_id=x'aa'") + q("SELECT count(*) FROM p2p_messages WHERE owner_user_id=x'aa'"), 2);
    assert_eq!(q("SELECT count(*) FROM users"), 1, "contas locais antigas (e seus hashes) não são importadas");
    assert_eq!(std::fs::read(s.join("risk.sqlite3")).unwrap(), before, "origem intacta");
    assert_eq!(legacy::apply(&s, &d, "novo@conta.briks.local").unwrap()["state"], "already");
    assert_eq!(legacy::probe(&s, &d)["state"], "imported");
    assert!(!d.join("import-social-tmp").exists());
}

// ---------- foto da conta e @nick provisório ----------

#[tokio::test]
async fn avatar_is_cached_validated_and_cleared() {
    let f = fake::start(0).await.unwrap();
    let dir = tmpdir("avatar");
    let base = f.url();
    tokio::task::spawn_blocking(move || {
        let id = "11111111-aaaa-4bbb-8ccc-222222222222";
        let path = avatar::cache(&dir, id, &format!("{base}/avatar.png")).unwrap();
        assert_eq!(path, dir.join("avatar-cache").join(format!("{id}.png")));
        assert_eq!(avatar::cached(&dir, id), Some(path.clone()));
        assert_eq!(avatar::sniff(&std::fs::read(&path).unwrap()), Some("png"));
        // conteúdo que não é imagem, arquivo grande demais, http comum e arquivo local: nada é gravado
        assert_eq!(avatar::cache(&dir, "u2", &format!("{base}/avatar.html")).unwrap_err().code, "avatar");
        assert_eq!(avatar::cache(&dir, "u3", &format!("{base}/avatar.big")).unwrap_err().code, "avatar");
        assert_eq!(avatar::fetch("http://exemplo.com/a.png").unwrap_err().code, "avatar");
        assert_eq!(avatar::fetch("file:///c:/windows/win.ini").unwrap_err().code, "avatar");
        assert!(avatar::cached(&dir, "u2").is_none() && avatar::cached(&dir, "u3").is_none());
        // um id malicioso não sai da pasta do cache
        assert_eq!(avatar::cache(&dir, "../../fora", &format!("{base}/avatar.png")).unwrap(), dir.join("avatar-cache").join("fora.png"));
        avatar::clear(&dir, id);
        assert!(!path.exists());
    }).await.unwrap();
}

#[test]
fn nick_hint_comes_from_the_name_never_the_email() {
    assert_eq!(avatar::nick_hint("Ygor Mello Betine"), "ygor_mello_betine");
    assert_eq!(avatar::nick_hint("  João  D'Ávila-Conceição "), "joao_davila_conceica");
    assert_eq!(avatar::nick_hint("李"), "");
    assert_eq!(avatar::nick_hint("Al"), "");
    assert!(avatar::nick_hint("Nome Muito Comprido Que Passa Do Limite").len() <= 20);
    // sem nome no provedor, o nome vira "Usuário": nada do e-mail aparece
    let v = json!({ "access_token": "a", "refresh_token": "r", "expires_in": 60, "user": { "id": "u1", "email": "segredo.pessoal@exemplo.com", "user_metadata": {}, "app_metadata": { "provider": "google" } } });
    let s = super::supabase::session_of(&v).unwrap();
    assert_eq!(s.user.display_name, "Usuário");
    assert!(!avatar::nick_hint(&s.user.display_name).contains("segredo"));
}

// ---------- personalizar o perfil: nome e foto ----------

fn write_png(path: &std::path::Path, w: u32, h: u32) {
    let mut img = image::RgbaImage::new(w, h);
    for (x, y, p) in img.enumerate_pixels_mut() {
        *p = image::Rgba([(x % 256) as u8, (y % 256) as u8, 120, if x < w / 2 { 255 } else { 0 }]);
    }
    img.save(path).unwrap();
}

#[test]
fn photo_is_cropped_square_resized_and_reencoded() {
    let dir = tmpdir("prep");
    write_png(&dir.join("larga.png"), 900, 400);
    let jpeg = avatar::prepare(&dir.join("larga.png")).unwrap();
    assert_eq!(avatar::sniff(&jpeg), Some("jpg"));
    let out = image::load_from_memory(&jpeg).unwrap();
    assert_eq!((out.width(), out.height()), (256, 256));
    assert!(jpeg.len() < 524288, "cabe no limite do servidor");
    // imagem pequena não é ampliada; minúscula, texto e arquivo inexistente são recusados com mensagem clara
    write_png(&dir.join("pequena.png"), 100, 80);
    let small = image::load_from_memory(&avatar::prepare(&dir.join("pequena.png")).unwrap()).unwrap();
    assert_eq!((small.width(), small.height()), (80, 80));
    write_png(&dir.join("mini.png"), 10, 10);
    assert_eq!(avatar::prepare(&dir.join("mini.png")).unwrap_err().code, "avatar");
    std::fs::write(dir.join("nota.png"), b"isto nao e imagem").unwrap();
    assert_eq!(avatar::prepare(&dir.join("nota.png")).unwrap_err().message, "Escolha uma imagem PNG ou JPEG.");
    assert_eq!(avatar::prepare(&dir.join("nao-existe.png")).unwrap_err().code, "avatar");
}

#[tokio::test]
async fn custom_name_and_photo_survive_the_next_login() {
    let _g = FLOW.lock().await;
    let f = fake::start(0).await.unwrap();
    let dir = tmpdir("custom");
    let a = accounts(&f, &dir);
    let u = a.sign_in_oauth(OAuthProvider::Google).await.unwrap();
    let id = crate::identity::Identity::generate();
    let jwk = serde_json::to_value(id.public_jwk()).unwrap();
    assert_eq!(a.publish_identity(jwk.clone(), &id.fingerprint()).await.unwrap().display_name, "Conta Google");

    assert_eq!(a.set_display_name("   ").await.unwrap_err().code, "invalid_name");
    assert_eq!(a.set_display_name("  Ygor  ").await.unwrap().display_name, "Ygor");
    assert_eq!(a.user().unwrap().display_name, "Ygor", "a sessão passa a usar o nome escolhido");

    write_png(&dir.join("foto.png"), 600, 600);
    let src = dir.join("foto.png");
    let jpeg = tokio::task::spawn_blocking(move || avatar::prepare(&src)).await.unwrap().unwrap();
    let p = a.set_photo(jpeg.clone()).await.unwrap();
    let url = p.avatar_url.clone().unwrap();
    assert!(url.contains(&format!("/storage/v1/object/public/avatars/{}/avatar.jpg?v=", u.id)));
    assert_eq!(f.state.lock().unwrap().objects[&format!("{}/avatar.jpg", u.id)], jpeg);
    // a foto publicada é baixável e é a mesma
    let (d2, id2, url2) = (dir.clone(), u.id.clone(), url.clone());
    let cached = tokio::task::spawn_blocking(move || avatar::cache(&d2, &id2, &url2)).await.unwrap().unwrap();
    assert_eq!(std::fs::read(cached).unwrap(), jpeg);

    // ninguém grava na pasta de outra pessoa
    let (prov, tok) = (SupabaseProvider::new(&f.url(), "k"), a.access_token().await.unwrap());
    let other = tokio::task::spawn_blocking(move || prov.upload_avatar(&tok, "00000000-0000-4000-8000-00000000dead", &[0xff, 0xd8, 0xff, 0])).await.unwrap();
    assert_eq!(other.unwrap_err().code, "server");
    let (prov, tok, uid) = (SupabaseProvider::new(&f.url(), "k"), a.access_token().await.unwrap(), u.id.clone());
    let big = tokio::task::spawn_blocking(move || prov.upload_avatar(&tok, &uid, &vec![0u8; 600_000])).await.unwrap();
    assert_eq!(big.unwrap_err().code, "avatar");

    // sair e entrar de novo: o login NÃO devolve o nome e a foto do provedor por cima do que a pessoa escolheu
    a.sign_out().await;
    a.sign_in_oauth(OAuthProvider::Google).await.unwrap();
    let again = a.publish_identity(jwk, &id.fingerprint()).await.unwrap();
    assert_eq!((again.display_name.as_str(), again.avatar_url.as_deref()), ("Ygor", Some(url.as_str())));

    // voltar para a foto da conta: o provedor falso não tem foto, então fica sem
    assert_eq!(a.reset_photo().await.unwrap().avatar_url, None);
}

#[tokio::test]
async fn service_applies_profile_changes_and_reports_them() {
    let _g = FLOW.lock().await;
    let f = fake::start(0).await.unwrap();
    let dir = tmpdir("svc-prof");
    let rt = service::Runtime::for_tests(Some(Arc::new(accounts(&f, &dir))), dir.clone());
    let (tx, mut rx) = tokio::sync::mpsc::channel(64);
    let mut svc = service::Service::new(rt, tx);
    svc.command("account.oauth", &json!({ "provider": "google" }));
    assert_eq!(next(&mut rx).await["state"], "busy");
    let e = next(&mut rx).await;
    assert_eq!((e["state"].as_str(), e["nick_hint"].as_str()), (Some("signed_in"), Some("conta_google")));
    assert_eq!(next(&mut rx).await["ev"], "account.profile");
    let e = next(&mut rx).await;
    assert_eq!((e["ev"].as_str(), e["path"].is_null()), (Some("account.avatar"), true), "sem foto no provedor");
    let uid = f.state.lock().unwrap().users[0].id.clone();

    svc.command("account.name", &json!({ "name": "Ygor Betine" }));
    let e = next(&mut rx).await;
    assert_eq!((e["ev"].as_str(), e["display_name"].as_str(), e["message"].as_str()), (Some("account.profile"), Some("Ygor Betine"), Some("Nome alterado.")));
    svc.command("account.name", &json!({ "name": "" }));
    assert_eq!(next(&mut rx).await["error_code"], "invalid_name");

    write_png(&dir.join("eu.png"), 300, 500);
    svc.command("account.photo", &json!({ "path": dir.join("eu.png") }));
    let e = next(&mut rx).await;
    assert_eq!(e["ev"], "account.avatar");
    let shown = std::path::PathBuf::from(e["path"].as_str().unwrap());
    assert_eq!(shown, dir.join("avatar-cache").join(format!("{uid}.jpg")));
    assert_eq!(std::fs::read(&shown).unwrap(), f.state.lock().unwrap().objects[&format!("{uid}/avatar.jpg")]);
    assert_eq!(next(&mut rx).await["message"], "Foto alterada.");
    svc.command("account.photo", &json!({ "path": dir.join("nao-existe.png") }));
    assert_eq!(next(&mut rx).await["error_code"], "avatar");
    assert!(shown.exists(), "erro ao trocar não apaga a foto atual");

    svc.command("account.photo_reset", &json!({}));
    let e = next(&mut rx).await;
    assert_eq!((e["ev"].as_str(), e["path"].is_null()), (Some("account.avatar"), true));
    assert!(!shown.exists());
    assert_eq!(next(&mut rx).await["message"], "Voltamos para a foto da sua conta.");
}

// ---------- fase 2: cópia cifrada da identidade, recuperável por frase ----------

fn fixed_identity() -> crate::identity::Identity {
    use base64::{engine::general_purpose::URL_SAFE_NO_PAD as B64, Engine};
    crate::identity::Identity::from_secret_b64(&B64.encode([7u8; 32])).unwrap()
}
fn fixed_phrase() -> String {
    bip39::Mnemonic::from_entropy_in(bip39::Language::Portuguese, &[0x5au8; 16]).unwrap().to_string()
}

#[test]
fn backup_known_vector_is_stable() {
    // vetor fixo: mesma identidade, frase, conta, sal e nonce => sempre o mesmo pacote (trava o formato v1)
    let id = fixed_identity();
    let phrase = fixed_phrase();
    assert_eq!(phrase, "envolver ringue mais panela narina cajado fita despesa envolver ringue mais palito");
    let blob = backup::seal_with(&id, &phrase, "11111111-aaaa-4bbb-8ccc-222222222222", &[1u8; 16], &[2u8; 24], backup::M_KIB, backup::T_COST, backup::P_COST).unwrap();
    assert_eq!((blob.version, blob.kdf.as_str(), blob.cipher.as_str(), blob.m_kib, blob.t, blob.p), (1, "argon2id", "xchacha20poly1305", 65536, 3, 1));
    assert_eq!(blob.salt, "AQEBAQEBAQEBAQEBAQEBAQ");
    assert_eq!(blob.nonce, "AgICAgICAgICAgICAgICAgICAgICAgIC");
    assert_eq!(blob.ciphertext, "ievtV8mhLt9KOKOpJOJ0RYBsTwRYVZunsNKxTr_q6F-BGHUIUT0RD1bMfyZWgGxR");
    assert_eq!(blob.fingerprint, id.fingerprint());
    let back = backup::open(&blob, &phrase, "11111111-aaaa-4bbb-8ccc-222222222222").unwrap();
    assert_eq!(back.secret_b64(), id.secret_b64());
    // o pacote não contém a chave nem a frase
    let text = serde_json::to_string(&blob).unwrap();
    assert!(!text.contains(&id.secret_b64()) && !text.contains(&phrase));
    assert!(text.len() < 2000, "cabe no limite do servidor");
}

#[test]
fn backup_phrase_generation_and_typing_mistakes() {
    let (a, b) = (backup::new_phrase(), backup::new_phrase());
    assert_ne!(a, b);
    assert_eq!(a.split(' ').count(), 12);
    assert!(a.chars().all(|c| c.is_ascii_lowercase() || c == ' '), "só letras sem acento: {a}");
    assert_eq!(backup::check_phrase(&a).unwrap(), a);
    // maiúsculas e espaços a mais são tolerados
    let messy = format!("  {}  ", a.to_uppercase().replace(' ', "   "));
    assert_eq!(backup::check_phrase(&messy).unwrap(), a);
    // faltando palavra, palavra inventada e palavra trocada (soma de verificação) são pegas sem ir à rede
    let words: Vec<&str> = a.split(' ').collect();
    assert_eq!(backup::check_phrase(&words[..11].join(" ")).unwrap_err().code, "invalid_phrase");
    assert_eq!(backup::check_phrase(&format!("{} xyzzy", words[..11].join(" "))).unwrap_err().code, "invalid_phrase");
    assert_eq!(backup::check_phrase("").unwrap_err().code, "invalid_phrase");
    let other: Vec<String> = b.split(' ').map(String::from).collect();
    let swapped: Vec<String> = (0..12).map(|i| if i == 3 { other[3].clone() } else { words[i].to_string() }).collect();
    if swapped[3] != words[3] {
        // 1 chance em 16 de a soma de verificação coincidir: aí a frase é válida, mas é OUTRA frase
        match backup::check_phrase(&swapped.join(" ")) {
            Err(e) => assert_eq!(e.code, "invalid_phrase"),
            Ok(p) => assert_ne!(p, a),
        }
    }
}

#[test]
fn backup_phrase_errors_say_what_to_fix_without_echoing_the_phrase() {
    let a = backup::new_phrase();
    let words: Vec<&str> = a.split(' ').collect();
    let e = backup::check_phrase("").unwrap_err();
    assert!(e.message.contains("12 palavras"));
    let e = backup::check_phrase(&words[..10].join(" ")).unwrap_err();
    assert!(e.message.contains("Faltam 2"), "{}", e.message);
    let e = backup::check_phrase(&format!("{a} casa")).unwrap_err();
    assert!(e.message.contains("Sobra"), "{}", e.message);
    let e = backup::check_phrase(&format!("{} xyzzyq", words[..11].join(" "))).unwrap_err();
    assert!(e.message.contains("palavra 12") && !e.message.contains("xyzzyq"), "{}", e.message);
    // a mensagem de frase errada orienta sobre troca de frase e conta, sem vazar nada
    let id = fixed_identity();
    let phrase = fixed_phrase();
    let blob = backup::seal(&id, &phrase, "11111111-aaaa-4bbb-8ccc-111111111111").unwrap();
    let e = backup::open(&blob, &backup::new_phrase(), "11111111-aaaa-4bbb-8ccc-111111111111").unwrap_err();
    assert!(e.message.contains("mais recente") && !e.message.contains(&phrase));
}

#[tokio::test]
async fn dropped_session_during_a_command_sends_the_user_back_to_the_entry_screen() {
    let _g = FLOW.lock().await;
    let f = fake::start(0).await.unwrap();
    let dir = tmpdir("svc-drop");
    let a = Arc::new(accounts(&f, &dir));
    let rt = service::Runtime::for_tests(Some(a.clone()), dir.clone());
    let (tx, mut rx) = tokio::sync::mpsc::channel(64);
    let mut svc = service::Service::new(rt, tx);
    svc.command("account.oauth", &json!({ "provider": "google" }));
    for _ in 0..4 { next(&mut rx).await; }   // busy, signed_in, profile, avatar
    // o servidor derruba a sessão (refresh token revogado) e o access token vence
    {
        let mut st = f.state.lock().unwrap();
        st.drop_all_sessions();
    }
    a.expire_access_for_tests();
    svc.command("account.name", &json!({ "name": "Outro" }));
    let e = next(&mut rx).await;
    assert_eq!((e["state"].as_str(), e["error_code"].as_str()), (Some("signed_out"), Some("session_expired")));
    assert!(a.user().is_none() && !dir.join("account-session.bin").exists(), "sessão guardada apagada");
    // erro de rede NÃO derruba: continua logado, só avisa
    svc.command("account.oauth", &json!({ "provider": "google" }));
    for _ in 0..4 { next(&mut rx).await; }
    let dead = Arc::new(Accounts::new(Arc::new(SupabaseProvider::new("http://127.0.0.1:9", "k")), &tmpdir("svc-net"), fake::auto_browser()));
    assert_eq!(dead.restore().await.unwrap(), None);
    assert!(a.user().is_some());
}

#[tokio::test]
async fn switching_accounts_replaces_the_session_and_forgets_the_previous_photo() {
    let _g = FLOW.lock().await;
    let f = fake::start(0).await.unwrap();
    let dir = tmpdir("switch");
    let a = accounts(&f, &dir);
    let first = a.sign_in_oauth(OAuthProvider::Google).await.unwrap();
    assert_eq!(a.take_previous_id(&first.id), None, "primeiro login: nada a esquecer");
    avatar::store(&dir, &first.id, &[0xff, 0xd8, 0xff, 0xd9]).unwrap();
    let second = a.sign_in_oauth(OAuthProvider::Discord).await.unwrap();
    assert_ne!(first.id, second.id);
    assert_eq!(a.user().unwrap().id, second.id);
    assert_eq!(a.take_previous_id(&second.id).as_deref(), Some(first.id.as_str()));
    assert_eq!(a.take_previous_id(&second.id), None, "só uma vez");
    // a sessão guardada é a da conta nova: reabrir o app entra nela
    let b = accounts(&f, &dir);
    assert_eq!(b.restore().await.unwrap().unwrap().id, second.id);
    // sair apaga a sessão guardada
    b.sign_out().await;
    assert!(!dir.join("account-session.bin").exists());
    assert_eq!(accounts(&f, &dir).restore().await.unwrap(), None);
}

#[test]
fn backup_wrong_phrase_tampering_and_other_account_all_fail_the_same_way() {
    let id = fixed_identity();
    let phrase = fixed_phrase();
    let user = "11111111-aaaa-4bbb-8ccc-222222222222";
    let blob = backup::seal(&id, &phrase, user).unwrap();
    // sal e nonce aleatórios: duas cópias da mesma chave com a mesma frase não se parecem
    let again = backup::seal(&id, &phrase, user).unwrap();
    assert!(blob.salt != again.salt && blob.nonce != again.nonce && blob.ciphertext != again.ciphertext);
    assert_eq!(backup::open(&blob, &phrase, user).unwrap().fingerprint(), id.fingerprint());

    let other_phrase = backup::new_phrase();
    let e = backup::open(&blob, &other_phrase, user).unwrap_err();
    assert_eq!(e.code, "wrong_phrase");
    assert!(!e.message.contains(&id.secret_b64()) && !e.message.contains(&blob.ciphertext));
    // pacote de outra conta (dado autenticado diferente) e pacote adulterado: mesmo erro, nada vaza
    assert_eq!(backup::open(&blob, &phrase, "99999999-aaaa-4bbb-8ccc-222222222222").unwrap_err().code, "wrong_phrase");
    let flip = |s: &str| { let mut c: Vec<char> = s.chars().collect(); c[5] = if c[5] == 'A' { 'B' } else { 'A' }; c.into_iter().collect::<String>() };
    for field in ["ciphertext", "nonce", "salt"] {
        let mut bad = blob.clone();
        match field { "ciphertext" => bad.ciphertext = flip(&bad.ciphertext), "nonce" => bad.nonce = flip(&bad.nonce), _ => bad.salt = flip(&bad.salt) }
        assert_eq!(backup::open(&bad, &phrase, user).unwrap_err().code, "wrong_phrase", "{field}");
    }
    // impressão digital trocada: a chave abre, mas não é a anunciada
    let mut bad = blob.clone();
    bad.fingerprint = "0".repeat(64);
    assert_eq!(backup::open(&bad, &phrase, user).unwrap_err().code, "invalid_backup");
    // um servidor malicioso não baixa o custo do KDF nem pede memória absurda
    for (m, t, p) in [(1024, 3, 1), (65536, 1, 1), (4_000_000, 3, 1), (65536, 3, 64)] {
        let mut bad = blob.clone();
        (bad.m_kib, bad.t, bad.p) = (m, t, p);
        assert_eq!(backup::open(&bad, &phrase, user).unwrap_err().code, "invalid_backup", "{m} {t} {p}");
    }
    let mut bad = blob.clone();
    bad.version = 2;
    assert_eq!(backup::open(&bad, &phrase, user).unwrap_err().code, "invalid_backup");
}

#[tokio::test]
async fn backup_roundtrip_through_the_server_and_phrase_rotation() {
    let _g = FLOW.lock().await;
    let f = fake::start(0).await.unwrap();
    let a = accounts(&f, &tmpdir("bk-a"));
    let u = a.sign_in_oauth(OAuthProvider::Google).await.unwrap();
    assert_eq!(a.backup_fetch().await.unwrap(), None);
    assert_eq!(a.backup_open(&backup::new_phrase()).await.unwrap_err().code, "no_backup");
    assert_eq!(a.backup_open("isto nao e uma frase").await.unwrap_err().code, "invalid_phrase");

    let id = crate::identity::Identity::generate();
    let phrase1 = backup::new_phrase();
    let saved = a.backup_save(&id, &phrase1).await.unwrap();
    // o que o servidor guardou não tem a chave nem a frase, e é exatamente o pacote
    let stored = f.state.lock().unwrap().backups[&u.id].clone();
    assert_eq!(serde_json::from_value::<backup::Blob>(stored["blob"].clone()).unwrap(), saved);
    let text = stored.to_string();
    assert!(!text.contains(&id.secret_b64()) && !text.contains(&phrase1));

    // "outro computador": outra instância, mesma conta, só a frase
    let b = accounts(&f, &tmpdir("bk-b"));
    b.sign_in_oauth(OAuthProvider::Google).await.unwrap();
    assert_eq!(b.backup_open(&phrase1).await.unwrap().secret_b64(), id.secret_b64());
    assert_eq!(b.backup_open(&backup::new_phrase()).await.unwrap_err().code, "wrong_phrase");

    // trocar a frase: a antiga deixa de abrir, a nova abre a mesma chave
    let phrase2 = backup::new_phrase();
    let rotated = a.backup_save(&id, &phrase2).await.unwrap();
    assert!(rotated.salt != saved.salt && rotated.nonce != saved.nonce && rotated.ciphertext != saved.ciphertext);
    assert_eq!(b.backup_open(&phrase1).await.unwrap_err().code, "wrong_phrase");
    assert_eq!(b.backup_open(&phrase2).await.unwrap().fingerprint(), id.fingerprint());

    // outra conta não enxerga a cópia
    let c = accounts(&f, &tmpdir("bk-c"));
    c.sign_in_oauth(OAuthProvider::Discord).await.unwrap();
    assert_eq!(c.backup_fetch().await.unwrap(), None);
    assert_eq!(c.backup_open(&phrase2).await.unwrap_err().code, "no_backup");

    // o servidor recusa pacote fora do formato
    let (prov, tok) = (SupabaseProvider::new(&f.url(), "k"), a.access_token().await.unwrap());
    let junk = tokio::task::spawn_blocking(move || prov.backup_put(&tok, &json!({ "version": 1, "secret": "em claro" }))).await.unwrap();
    assert_eq!(junk.unwrap_err().code, "invalid_backup");

    a.backup_delete().await.unwrap();
    assert_eq!(b.backup_fetch().await.unwrap(), None);
    assert_eq!(b.backup_open(&phrase2).await.unwrap_err().code, "no_backup");
}

#[tokio::test]
async fn service_backup_create_confirm_restore_on_new_computer_and_delete() {
    let _g = FLOW.lock().await;
    let f = fake::start(0).await.unwrap();
    let skip = |v: &Value| matches!(v["ev"].as_str(), Some("account.state" | "account.profile" | "account.avatar" | "account.notice"));
    // computador 1
    let dir1 = tmpdir("bk-svc1");
    let rt = service::Runtime::for_tests(Some(Arc::new(accounts(&f, &dir1))), dir1.clone());
    let (tx, mut rx1) = tokio::sync::mpsc::channel(64);
    let mut pc1 = service::Service::new(rt, tx);
    pc1.command("account.oauth", &json!({ "provider": "google" }));
    for _ in 0..4 { assert!(skip(&next(&mut rx1).await)); }
    let fp1 = crate::ipc::load_or_create_identity(&dir1).unwrap().fingerprint();

    assert!(pc1.command("backup.status", &json!({})));
    let e = next(&mut rx1).await;
    assert_eq!((e["ev"].as_str(), e["exists"].as_bool()), (Some("backup.state"), Some(false)));

    // gerar a frase não grava nada; cancelar descarta; confirmar sem frase pendente falha
    pc1.command("backup.create", &json!({}));
    let e = next(&mut rx1).await;
    assert_eq!((e["ev"].as_str(), e["words"].as_array().map(|w| w.len())), (Some("backup.phrase"), Some(12)));
    assert!(f.state.lock().unwrap().backups.is_empty(), "nada sobe antes do 'anotei'");
    pc1.command("backup.cancel", &json!({}));
    pc1.command("backup.confirm", &json!({}));
    let e = next(&mut rx1).await;
    assert_eq!((e["ev"].as_str(), e["ok"].as_bool()), (Some("backup.result"), Some(false)));
    assert_eq!(next(&mut rx1).await["exists"], false);

    pc1.command("backup.create", &json!({}));
    let phrase: String = next(&mut rx1).await["words"].as_array().unwrap().iter().map(|w| w.as_str().unwrap()).collect::<Vec<_>>().join(" ");
    pc1.command("backup.confirm", &json!({}));
    let e = next(&mut rx1).await;
    assert_eq!((e["op"].as_str(), e["ok"].as_bool()), (Some("save"), Some(true)));
    assert!(!e.to_string().contains(&phrase), "a frase não volta nos eventos seguintes");
    let e = next(&mut rx1).await;
    assert_eq!((e["exists"].as_bool(), e["matches"].as_bool()), (Some(true), Some(true)));

    // computador 2: identidade nova, mesma conta
    let dir2 = tmpdir("bk-svc2");
    let rt = service::Runtime::for_tests(Some(Arc::new(accounts(&f, &dir2))), dir2.clone());
    let (tx, mut rx2) = tokio::sync::mpsc::channel(64);
    let mut pc2 = service::Service::new(rt, tx);
    pc2.command("account.oauth", &json!({ "provider": "google" }));
    for _ in 0..4 { assert!(skip(&next(&mut rx2).await)); }
    let fp2 = crate::ipc::load_or_create_identity(&dir2).unwrap().fingerprint();
    assert_ne!(fp1, fp2);
    pc2.command("backup.status", &json!({}));
    let e = next(&mut rx2).await;
    assert_eq!((e["exists"].as_bool(), e["matches"].as_bool()), (Some(true), Some(false)), "há cópia, mas de outra identidade");

    // frase errada: nada muda neste computador
    pc2.command("backup.restore", &json!({ "phrase": backup::new_phrase() }));
    let e = next(&mut rx2).await;
    assert_eq!((e["op"].as_str(), e["ok"].as_bool(), e["error_code"].as_str()), (Some("restore"), Some(false), Some("wrong_phrase")));
    assert_eq!(next(&mut rx2).await["matches"], false);
    assert_eq!(crate::ipc::load_or_create_identity(&dir2).unwrap().fingerprint(), fp2);
    assert!(!dir2.join("core-identity.prev.json").exists());

    // frase certa: a identidade do computador 1 é instalada e anunciada no perfil
    pc2.command("backup.restore", &json!({ "phrase": format!("  {}  ", phrase.to_uppercase()) }));
    let e = next(&mut rx2).await;
    assert_eq!((e["ev"].as_str(), e["fingerprint"].as_str()), (Some("identity.changed"), Some(fp1.as_str())));
    let e = next(&mut rx2).await;
    assert_eq!((e["ev"].as_str(), e["fingerprint"].as_str()), (Some("account.profile"), Some(fp1.as_str())));
    let e = next(&mut rx2).await;
    assert_eq!((e["op"].as_str(), e["ok"].as_bool()), (Some("restore"), Some(true)));
    assert_eq!(next(&mut rx2).await["matches"], true);
    assert_eq!(crate::ipc::load_or_create_identity(&dir2).unwrap().fingerprint(), fp1);
    assert!(dir2.join("core-identity.prev.json").exists(), "a identidade anterior deste computador fica guardada");
    let uid = f.state.lock().unwrap().users[0].id.clone();
    assert_eq!(f.state.lock().unwrap().profiles[&uid]["fingerprint"], fp1.as_str());

    pc2.command("backup.delete", &json!({}));
    let e = next(&mut rx2).await;
    assert_eq!((e["op"].as_str(), e["ok"].as_bool()), (Some("delete"), Some(true)));
    assert_eq!(next(&mut rx2).await["exists"], false);
    assert!(f.state.lock().unwrap().backups.is_empty());
}

// ---------- Supabase REAL (opcional) ----------
// Só roda com `cargo test -- --ignored real_supabase` e BRIKS_SUPABASE_URL / BRIKS_SUPABASE_ANON_KEY no ambiente.
// Não cria conta nem faz login: confere que o projeto responde e que os erros reais viram os nossos códigos.
#[tokio::test]
#[ignore]
async fn real_supabase_answers_and_errors_map() {
    let (Ok(url), Ok(key)) = (std::env::var("BRIKS_SUPABASE_URL"), std::env::var("BRIKS_SUPABASE_ANON_KEY")) else { panic!("defina BRIKS_SUPABASE_URL e BRIKS_SUPABASE_ANON_KEY") };
    config::check_url(&url).unwrap();
    config::check_public_key(&key).unwrap();
    let dir = tmpdir("real");
    let a = Accounts::new(Arc::new(SupabaseProvider::new(&url, &key)), &dir, Arc::new(|_| Ok(())));
    assert_eq!(a.restore().await.unwrap(), None);
    assert_eq!(a.sign_in_password("ninguem-teste@example.com", "senha-inexistente-000").await.unwrap_err().code, "invalid_credentials");
    let run = |f: Box<dyn FnOnce(&SupabaseProvider) -> AccountError + Send>| {
        let p = SupabaseProvider::new(&url, &key);
        tokio::task::spawn_blocking(move || f(&p))
    };
    assert_eq!(run(Box::new(|p| p.refresh("nao-existe").unwrap_err())).await.unwrap().code, "session_expired");
    assert_eq!(run(Box::new(|p| p.exchange_code("00000000-0000-4000-8000-000000000000", &Pkce::generate().verifier).unwrap_err())).await.unwrap().code, "invalid_code");
    assert_eq!(run(Box::new(|p| p.friends("token-invalido").unwrap_err())).await.unwrap().code, "session_expired");
    // o endereço de login social existe e leva ao provedor (sem seguir o redirect)
    for (prov, host) in [(OAuthProvider::Google, "accounts.google.com"), (OAuthProvider::Discord, "discord.com")] {
        let u = SupabaseProvider::new(&url, &key).oauth_url(prov, "http://127.0.0.1:53682/callback", &Pkce::generate().challenge);
        let loc = tokio::task::spawn_blocking(move || ureq::AgentBuilder::new().redirects(0).timeout(Duration::from_secs(15)).build().get(&u).call().ok().and_then(|r| r.header("Location").map(String::from))).await.unwrap();
        assert!(loc.unwrap_or_default().contains(host), "{host}");
    }
}

// ---------- TURN da conta (Edge Function turn-credentials, servidor HTTP falso) ----------

#[tokio::test]
async fn account_turn_is_fetched_after_login_cleared_on_logout_and_silent_on_failure() {
    use super::turn::{self, TurnCache};
    let _g = FLOW.lock().await;
    let f = fake::start(0).await.unwrap();
    let dir = tmpdir("turn");
    let a = accounts(&f, &dir);
    let cache = TurnCache::default();

    // sem login: nada, e nenhuma chamada à função
    f.state.lock().unwrap().turn_ttl = 3600;
    turn::step(&a, &cache, &f.url(), "k").await;
    assert!(cache.servers().is_empty() && f.state.lock().unwrap().turn_hits == 0);

    // função não publicada (404): silêncio, só STUN
    f.state.lock().unwrap().turn_ttl = 0;
    a.sign_in_oauth(OAuthProvider::Google).await.unwrap();
    let wait = turn::step(&a, &cache, &f.url(), "k").await;
    assert!(cache.servers().is_empty());
    assert_eq!(wait, Duration::from_secs(300), "tenta de novo em 5 min, sem barulho");

    // publicada: busca com o JWT da sessão, guarda e agenda a renovação antes de vencer
    f.state.lock().unwrap().turn_ttl = 3600;
    let wait = turn::step(&a, &cache, &f.url(), "k").await;
    assert_eq!(wait, Duration::from_secs(3300));
    let s = cache.servers();
    assert!(crate::ice::has_turn(&s) && s.len() == 2, "STUN + TURN da conta");
    assert_eq!(s[1].credential, "cred-falsa-do-turn");
    assert_eq!(f.state.lock().unwrap().turn_hits, 1);

    // o TURN da conta entra no ICE junto com o STUN padrão; o que o usuário configurou manda mais
    let noenv = |_: &str| None;
    let (list, w) = crate::ice::load_with(&noenv, &dir, &s);
    assert!(w.is_none() && crate::ice::has_turn(&list) && list.len() == 3 + 2);
    let sum = crate::ice::summary(&noenv, &dir, &s);
    assert_eq!((sum["source"].as_str(), sum["turn"].as_bool(), sum["urls"].as_str(), sum["has_credential"].as_bool()), (Some("account"), Some(true), Some(""), Some(false)));
    assert!(!sum.to_string().contains("cred-falsa") && !sum.to_string().contains("turn.exemplo"), "a UI não vê endereços nem senha");
    std::fs::write(dir.join("ice.json"), r#"[{"urls":"turn:meu.example.com:3478","username":"u","credential":"c"}]"#).unwrap();
    let (list, _) = crate::ice::load_with(&noenv, &dir, &s);
    assert!(list.len() == 4 && list[3].urls[0].starts_with("turn:meu.example.com"), "ice.json substitui o TURN da conta");
    assert_eq!(crate::ice::summary(&noenv, &dir, &s)["source"], "file");
    std::fs::remove_file(dir.join("ice.json")).unwrap();

    // falha depois de ter credencial: continua usando a que ainda vale
    f.state.lock().unwrap().turn_ttl = 0;
    turn::step(&a, &cache, &f.url(), "k").await;
    assert_eq!(cache.servers().len(), 2, "credencial ainda válida continua em uso");

    // sair limpa o cache e para de pedir
    a.sign_out().await;
    let hits = f.state.lock().unwrap().turn_hits;
    turn::step(&a, &cache, &f.url(), "k").await;
    assert!(cache.servers().is_empty() && f.state.lock().unwrap().turn_hits == hits);
}

#[test]
fn account_turn_renewal_is_early_and_responses_are_validated() {
    use super::turn;
    assert_eq!(turn::renew_after(3600), Duration::from_secs(3300));
    assert_eq!(turn::renew_after(900), Duration::from_secs(600));
    assert_eq!(turn::renew_after(300), Duration::from_secs(150));
    // credencial vencida não é entregue
    let c = turn::TurnCache::default();
    c.set(crate::ice::default_servers(), 1);
    assert!(c.servers().is_empty());
    c.set(crate::ice::default_servers(), u64::MAX);
    assert_eq!(c.servers().len(), 3);
    c.clear();
    assert!(c.servers().is_empty());
    // servidor fora do ar: erro curto, sem endereço nem token
    let e = turn::fetch("http://127.0.0.1:9", "k", "token-secreto-123").unwrap_err();
    assert!(!e.contains("token-secreto") && !e.contains("127.0.0.1"), "{e}");
}

#[tokio::test]
async fn sign_out_of_other_computers_keeps_this_one_and_kills_the_rest() {
    let _g = FLOW.lock().await;
    let f = fake::start(0).await.unwrap();
    let (d1, d2) = (tmpdir("others-a"), tmpdir("others-b"));
    // dois "computadores" na mesma conta
    let (a, b) = (accounts(&f, &d1), accounts(&f, &d2));
    let ua = a.sign_in_oauth(OAuthProvider::Google).await.unwrap();
    let ub = b.sign_in_oauth(OAuthProvider::Google).await.unwrap();
    assert_eq!(ua.id, ub.id);
    a.sign_out_others().await.unwrap();
    // o outro computador perdeu a sessão (a renovação falha como sessão derrubada); este segue funcionando
    let c = accounts(&f, &d2);
    assert_eq!(c.restore().await.unwrap(), None, "o outro computador foi desconectado");
    assert!(!d2.join("account-session.bin").exists());
    assert!(a.access_token().await.is_ok());
    let again = accounts(&f, &d1);
    assert_eq!(again.restore().await.unwrap().unwrap().id, ua.id, "este computador reabre normalmente");
    // sem sessão: erro de sessão, não pânico
    assert!(accounts(&f, &tmpdir("others-c")).sign_out_others().await.unwrap_err().is_auth());
}

#[tokio::test]
async fn service_reports_the_result_of_signing_out_the_others() {
    let _g = FLOW.lock().await;
    let f = fake::start(0).await.unwrap();
    let dir = tmpdir("svc-others");
    let rt = service::Runtime::for_tests(Some(Arc::new(accounts(&f, &dir))), dir.clone());
    let (tx, mut rx) = tokio::sync::mpsc::channel(64);
    let mut svc = service::Service::new(rt, tx);
    svc.command("account.oauth", &json!({ "provider": "google" }));
    for _ in 0..4 { next(&mut rx).await; }
    svc.command("account.logout_others", &json!({}));
    let e = next(&mut rx).await;
    assert_eq!((e["ev"].as_str(), e["ok"].as_bool()), (Some("account.others"), Some(true)));
    assert!(e["message"].as_str().unwrap().contains("Este continua conectado"));
}

#[tokio::test]
async fn devices_list_register_revoke_and_livekit_refuses_the_revoked_peer() {
    let _g = FLOW.lock().await;
    let f = fake::start(0).await.unwrap();
    f.state.lock().unwrap().autoconfirm = true;
    let (a, a2, b) = (accounts(&f, &tmpdir("dv-a")), accounts(&f, &tmpdir("dv-a2")), accounts(&f, &tmpdir("dv-b")));
    let SignUpOutcome::SignedIn(ua) = a.sign_up("ana@exemplo.com", "senha-boa-1", "Ana").await.unwrap() else { panic!() };
    let SignUpOutcome::SignedIn(_) = b.sign_up("bia@exemplo.com", "senha-boa-1", "Bia").await.unwrap() else { panic!() };
    let id = crate::identity::Identity::generate();
    let jwk = serde_json::to_value(id.public_jwk()).unwrap();
    a.publish_identity(jwk.clone(), &id.fingerprint()).await.unwrap();
    b.publish_identity(jwk.clone(), &id.fingerprint()).await.unwrap();
    // a segunda instalação da Ana entra com as mesmas credenciais
    a2.sign_in_password("ana@exemplo.com", "senha-boa-1").await.unwrap();

    let (pc1, pc2) = ("11111111-aaaa-4bbb-8ccc-000000000001", "22222222-aaaa-4bbb-8ccc-000000000002");
    a.key_register(pc1, jwk.clone(), "PC da sala").await.unwrap();
    a2.key_register(pc2, jwk.clone(), "Notebook").await.unwrap();
    b.key_register("33333333-aaaa-4bbb-8ccc-000000000003", jwk.clone(), "PC da Bia").await.unwrap();
    assert_eq!(a.key_register(pc1, jwk.clone(), "").await.map(|_| ()), Ok(()), "registrar de novo só atualiza o último uso");
    assert_eq!(a.key_register("curto", jwk.clone(), "x").await.unwrap_err().code, "server", "peerId inválido é recusado");

    let l = a.keys_list().await.unwrap();
    assert_eq!(l.len(), 2, "só os computadores da própria conta");
    let n = l.iter().find(|d| d.peer_id == pc2).unwrap();
    assert_eq!(n.device_name, "Notebook");
    assert!(n.created_at.as_deref().unwrap().starts_with("20") && n.last_used_at.is_some());
    assert_eq!(l.iter().find(|d| d.peer_id == pc1).unwrap().device_name, "PC da sala", "nome vazio não apaga o nome");

    // a Bia não consegue revogar o computador da Ana (o SQL só mexe nas linhas do próprio usuário)
    b.key_revoke(pc2).await.unwrap();
    assert!(f.state.lock().unwrap().peer_allowed(&ua.id, pc2));
    assert_eq!(b.key_register(pc2, jwk.clone(), "x").await.unwrap_err().code, "server", "peerId de outra conta");

    // a Ana revoga o notebook: some da lista, e o livekit-token (peer_allowed) passa a recusar esse peerId
    assert!(f.state.lock().unwrap().peer_allowed(&ua.id, pc1));
    a.key_revoke(pc2).await.unwrap();
    assert_eq!(a.keys_list().await.unwrap().len(), 1);
    assert!(!f.state.lock().unwrap().peer_allowed(&ua.id, pc2));
    assert!(f.state.lock().unwrap().peer_allowed(&ua.id, pc1), "o outro computador segue valendo");
    assert_eq!(a2.key_register(pc2, jwk.clone(), "Notebook").await.unwrap_err().code, "key_revoked", "instalação revogada não volta a se registrar");

    // servidor sem a 0009: a UI recebe server_not_ready e esconde o cartão
    f.state.lock().unwrap().no_keys_rpc = true;
    assert_eq!(a.keys_list().await.unwrap_err().code, "server_not_ready");
    assert_eq!(a.key_revoke(pc1).await.unwrap_err().code, "server_not_ready");
}
