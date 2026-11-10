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
use super::{avatar, legacy, loopback, service, valid_email, Accounts, SignUpOutcome};

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
