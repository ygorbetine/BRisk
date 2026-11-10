//! Servidor de contas FALSO, só em 127.0.0.1: imita as rotas do Supabase Auth e as nossas funções RPC o bastante para
//! exercitar o cliente de verdade (`SupabaseProvider`) sem rede. Usado nos testes e no modo de desenvolvimento
//! (`BRIKS_ACCOUNTS_MOCK=1` ou `briks-core mock-accounts`). Nada aqui é segurança real: tudo vive na memória.
use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use anyhow::Result;
use serde_json::{json, Value};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};

use super::pkce::{challenge_of, parse_query};
use super::provider::{AResult, AccountError};
use super::Opener;

#[derive(Clone)]
pub struct FakeUser {
    pub id: String,
    pub email: String,
    pub password: Option<String>,
    pub confirmed: bool,
    pub name: String,
    pub provider: String,
}

#[derive(Default)]
pub struct FakeState {
    pub users: Vec<FakeUser>,
    codes: HashMap<String, (String, String)>,      // code -> (challenge, user_id)
    access: HashMap<String, String>,               // access token -> user_id
    refresh: HashMap<String, String>,              // refresh token -> user_id (uso único)
    otps: HashMap<String, String>,                 // "tipo:email" -> código
    pub profiles: HashMap<String, Value>,
    pub objects: HashMap<String, Vec<u8>>,
    pub backups: HashMap<String, Value>,           // cópia cifrada da identidade por conta         // Storage: "<id>/avatar.jpg" -> bytes
    requests: Vec<(String, String, String, bool)>, // id, de, para, aceito
    /// validade do access token em segundos (testes de renovação usam valores baixos)
    pub expires_in: u64,
    pub autoconfirm: bool,
    /// o provedor social devolve "access_denied" (a pessoa cancelou)
    pub deny_oauth: bool,
    /// último link "enviado por e-mail" (confirmação/recuperação) e contagem de renovações
    pub last_link: Option<String>,
    pub refresh_count: u32,
    /// amigos de exemplo ao criar o primeiro perfil (modo de desenvolvimento)
    pub demo: bool,
}

pub struct Fake {
    pub port: u16,
    pub state: Arc<Mutex<FakeState>>,
}
impl Fake {
    pub fn url(&self) -> String {
        format!("http://127.0.0.1:{}", self.port)
    }
}

pub const FAKE_OTP: &str = "123456";

fn uid() -> String {
    uuid::Uuid::new_v4().to_string()
}

pub async fn start(port: u16) -> Result<Fake> {
    let l = TcpListener::bind(("127.0.0.1", port)).await?;
    let port = l.local_addr()?.port();
    let state = Arc::new(Mutex::new(FakeState { expires_in: 3600, ..Default::default() }));
    let st = state.clone();
    tokio::spawn(async move {
        while let Ok((s, _)) = l.accept().await {
            let st = st.clone();
            tokio::spawn(async move {
                let _ = serve(s, st).await;
            });
        }
    });
    Ok(Fake { port, state })
}

async fn serve(mut s: TcpStream, st: Arc<Mutex<FakeState>>) -> Result<()> {
    let mut buf = Vec::new();
    let mut tmp = [0u8; 4096];
    let (head_end, len) = loop {
        let n = s.read(&mut tmp).await?;
        if n == 0 {
            return Ok(());
        }
        buf.extend_from_slice(&tmp[..n]);
        if let Some(p) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
            let head = String::from_utf8_lossy(&buf[..p]).to_lowercase();
            let len = head.lines().find_map(|l| l.strip_prefix("content-length:")).and_then(|v| v.trim().parse::<usize>().ok()).unwrap_or(0);
            break (p + 4, len);
        }
        if buf.len() > 1 << 20 {
            return Ok(());
        }
    };
    while buf.len() < head_end + len {
        let n = s.read(&mut tmp).await?;
        if n == 0 {
            break;
        }
        buf.extend_from_slice(&tmp[..n]);
    }
    let head = String::from_utf8_lossy(&buf[..head_end]).to_string();
    // Storage falso: POST /storage/v1/object/avatars/<id>/<arquivo> (só o dono) e GET .../object/public/avatars/...
    {
        let mut it = head.lines().next().unwrap_or("").split_whitespace();
        let (m, t) = (it.next().unwrap_or(""), it.next().unwrap_or(""));
        let t = t.split('?').next().unwrap_or("");
        let auth = head.lines().find_map(|l| { let (k, v) = l.split_once(':')?; k.eq_ignore_ascii_case("authorization").then(|| v.trim().trim_start_matches("Bearer ").to_string()) }).unwrap_or_default();
        let reply = if let (true, Some(key)) = (m == "POST", t.strip_prefix("/storage/v1/object/avatars/")) {
            let mut g = st.lock().unwrap();
            let owner = g.access.get(&auth).cloned();
            let body = buf[head_end..].to_vec();
            Some(match owner {
                Some(o) if key.starts_with(&format!("{o}/")) && body.len() <= 524288 => { g.objects.insert(key.to_string(), body); ("200 OK", br#"{"Key":"ok"}"#.to_vec()) }
                Some(_) if body.len() > 524288 => ("400 Bad Request", br#"{"statusCode":"413","error":"Payload too large","message":"The object exceeded the maximum allowed size"}"#.to_vec()),
                _ => ("400 Bad Request", br#"{"statusCode":"403","error":"Unauthorized","message":"new row violates row-level security policy"}"#.to_vec()),
            })
        } else if let (true, Some(key)) = (m == "GET", t.strip_prefix("/storage/v1/object/public/avatars/")) {
            Some(match st.lock().unwrap().objects.get(key).cloned() { Some(b) => ("200 OK", b), None => ("404 Not Found", b"{}".to_vec()) })
        } else { None };
        if let Some((status, body)) = reply {
            s.write_all(format!("HTTP/1.1 {status}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", body.len()).as_bytes()).await?;
            s.write_all(&body).await?;
            s.shutdown().await.ok();
            return Ok(());
        }
    }
    // fotos de teste: /avatar.png (PNG mínimo), /avatar.html (não é imagem), /avatar.big (acima do limite)
    if let Some(kind) = head.lines().next().and_then(|l| l.split_whitespace().nth(1)).and_then(|t| t.strip_prefix("/avatar.")) {
        let body: Vec<u8> = match kind {
            "png" => [&[0x89u8, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a][..], &[0u8; 64][..]].concat(),
            "big" => [&[0xffu8, 0xd8, 0xff][..], &vec![0u8; 3 * 1024 * 1024][..]].concat(),
            _ => b"<html>nao sou imagem</html>".to_vec(),
        };
        s.write_all(format!("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", body.len()).as_bytes()).await?;
        s.write_all(&body).await?;
        s.shutdown().await.ok();
        return Ok(());
    }
    let mut first = head.lines().next().unwrap_or("").split_whitespace();
    let (method, target) = (first.next().unwrap_or("").to_string(), first.next().unwrap_or("").to_string());
    let bearer = head.lines().find_map(|l| {
        let (k, v) = l.split_once(':')?;
        k.eq_ignore_ascii_case("authorization").then(|| v.trim().trim_start_matches("Bearer ").to_string())
    }).unwrap_or_default();
    let body: Value = serde_json::from_slice(&buf[head_end..]).unwrap_or(Value::Null);
    let (path, query) = target.split_once('?').unwrap_or((&target, ""));
    let q: HashMap<String, String> = parse_query(query).into_iter().collect();
    let (status, extra, out) = route(&mut st.lock().unwrap(), &method, path, &q, &bearer, &body);
    let text = if out.is_null() { String::new() } else { out.to_string() };
    let r = format!("HTTP/1.1 {status}\r\nContent-Type: application/json\r\nContent-Length: {}\r\n{extra}Connection: close\r\n\r\n{text}", text.len());
    s.write_all(r.as_bytes()).await?;
    s.shutdown().await.ok();
    Ok(())
}

type Resp = (&'static str, String, Value);
fn ok(v: Value) -> Resp {
    ("200 OK", String::new(), v)
}
fn err(status: &'static str, code: &str, msg: &str) -> Resp {
    (status, String::new(), json!({ "code": 400, "error_code": code, "msg": msg }))
}
fn pg(msg: &str) -> Resp {
    ("400 Bad Request", String::new(), json!({ "code": "P0001", "message": msg, "details": null, "hint": null }))
}

fn user_json(u: &FakeUser) -> Value {
    json!({ "id": u.id, "email": u.email, "app_metadata": { "provider": u.provider }, "user_metadata": { "display_name": u.name } })
}

fn issue(st: &mut FakeState, user_id: &str) -> Value {
    let (a, r) = (format!("at-{}", uid()), format!("rt-{}", uid()));
    st.access.insert(a.clone(), user_id.to_string());
    st.refresh.insert(r.clone(), user_id.to_string());
    let u = st.users.iter().find(|u| u.id == user_id).cloned().expect("usuário");
    json!({ "access_token": a, "refresh_token": r, "token_type": "bearer", "expires_in": st.expires_in, "user": user_json(&u) })
}

fn new_code(st: &mut FakeState, challenge: &str, user_id: &str) -> String {
    let c = uid();
    st.codes.insert(c.clone(), (challenge.to_string(), user_id.to_string()));
    c
}

fn join_url(redirect: &str, param: &str) -> String {
    format!("{redirect}{}{param}", if redirect.contains('?') { '&' } else { '?' })
}

fn route(st: &mut FakeState, method: &str, path: &str, q: &HashMap<String, String>, bearer: &str, b: &Value) -> Resp {
    let s = |k: &str| b[k].as_str().unwrap_or("").to_string();
    let qs = |k: &str| q.get(k).cloned().unwrap_or_default();
    match (method, path) {
        ("GET", "/auth/v1/authorize") => {
            let (provider, redirect) = (qs("provider"), qs("redirect_to"));
            if provider != "google" && provider != "discord" {
                return err("400 Bad Request", "validation_failed", "Unsupported provider: provider is not enabled");
            }
            if !redirect.starts_with("http://127.0.0.1:") {
                return err("400 Bad Request", "validation_failed", "redirect not allowed");
            }
            let loc = if st.deny_oauth {
                join_url(&redirect, "error=access_denied&error_description=The+user+denied+the+request")
            } else {
                let email = format!("{provider}@mock.briks");
                let id = match st.users.iter().find(|u| u.email == email) {
                    Some(u) => u.id.clone(),
                    None => {
                        let id = uid();
                        st.users.push(FakeUser { id: id.clone(), email, password: None, confirmed: true, name: format!("Conta {}", if provider == "google" { "Google" } else { "Discord" }), provider });
                        id
                    }
                };
                let code = new_code(st, &qs("code_challenge"), &id);
                join_url(&redirect, &format!("code={code}"))
            };
            ("302 Found", format!("Location: {loc}\r\n"), Value::Null)
        }
        ("POST", "/auth/v1/token") => match qs("grant_type").as_str() {
            "pkce" => match st.codes.remove(&s("auth_code")) {
                Some((challenge, user)) if challenge_of(&s("code_verifier")) == challenge => {
                    if let Some(u) = st.users.iter_mut().find(|u| u.id == user) {
                        u.confirmed = true;   // abrir o link do e-mail confirma o endereço
                    }
                    ok(issue(st, &user))
                }
                Some(_) => err("400 Bad Request", "bad_code_verifier", "code challenge does not match previously saved code verifier"),
                None => err("404 Not Found", "flow_state_not_found", "invalid flow state, no valid flow state found"),
            },
            "password" => {
                let email = s("email").to_lowercase();
                match st.users.iter().find(|u| u.email == email && u.password.as_deref() == Some(&s("password"))).cloned() {
                    Some(u) if !u.confirmed => err("400 Bad Request", "email_not_confirmed", "Email not confirmed"),
                    Some(u) => ok(issue(st, &u.id)),
                    None => err("400 Bad Request", "invalid_credentials", "Invalid login credentials"),
                }
            }
            "refresh_token" => match st.refresh.remove(&s("refresh_token")) {
                Some(user) => {
                    st.refresh_count += 1;
                    ok(issue(st, &user))
                }
                None => err("400 Bad Request", "refresh_token_not_found", "Invalid Refresh Token: Refresh Token Not Found"),
            },
            _ => err("400 Bad Request", "validation_failed", "unsupported grant"),
        },
        ("POST", "/auth/v1/signup") => {
            let (email, pw) = (s("email").to_lowercase(), s("password"));
            if pw.len() < 8 {
                return err("422 Unprocessable Entity", "weak_password", "Password should be at least 8 characters");
            }
            if st.users.iter().any(|u| u.email == email) {
                return err("422 Unprocessable Entity", "user_already_exists", "User already registered");
            }
            let u = FakeUser { id: uid(), email: email.clone(), password: Some(pw), confirmed: st.autoconfirm, name: b["data"]["display_name"].as_str().unwrap_or("").to_string(), provider: "email".into() };
            st.users.push(u.clone());
            if st.autoconfirm {
                return ok(issue(st, &u.id));
            }
            let code = new_code(st, &s("code_challenge"), &u.id);
            st.last_link = Some(join_url(&qs("redirect_to"), &format!("code={code}")));
            st.otps.insert(format!("signup:{email}"), FAKE_OTP.into());
            ok(user_json(&u))
        }
        ("POST", "/auth/v1/recover") => {
            let email = s("email").to_lowercase();
            if let Some(u) = st.users.iter().find(|u| u.email == email).cloned() {
                let code = new_code(st, &s("code_challenge"), &u.id);
                st.last_link = Some(join_url(&qs("redirect_to"), &format!("code={code}")));
                st.otps.insert(format!("recovery:{email}"), FAKE_OTP.into());
            }
            ok(json!({}))
        }
        ("POST", "/auth/v1/verify") => {
            let email = s("email").to_lowercase();
            match st.otps.remove(&format!("{}:{email}", s("type"))) {
                Some(code) if code == s("token") => {
                    let Some(i) = st.users.iter().position(|u| u.email == email) else { return err("403 Forbidden", "otp_expired", "Token has expired or is invalid") };
                    st.users[i].confirmed = true;
                    let id = st.users[i].id.clone();
                    ok(issue(st, &id))
                }
                _ => err("403 Forbidden", "otp_expired", "Token has expired or is invalid"),
            }
        }
        ("PUT", "/auth/v1/user") => {
            let Some(user) = st.access.get(bearer).cloned() else { return err("401 Unauthorized", "bad_jwt", "invalid JWT") };
            if s("password").len() < 8 {
                return err("422 Unprocessable Entity", "weak_password", "Password should be at least 8 characters");
            }
            let u = st.users.iter_mut().find(|u| u.id == user).expect("usuário");
            u.password = Some(s("password"));
            u.confirmed = true;
            ok(user_json(u))
        }
        ("POST", "/auth/v1/logout") => {
            if let Some(user) = st.access.remove(bearer) {
                st.refresh.retain(|_, u| *u != user);
                st.access.retain(|_, u| *u != user);
            }
            ("204 No Content", String::new(), Value::Null)
        }
        ("POST", p) if p.starts_with("/rest/v1/rpc/") => {
            let Some(me) = st.access.get(bearer).cloned() else { return ("401 Unauthorized", String::new(), json!({ "code": "PGRST301", "message": "JWT expired" })) };
            rpc(st, &p["/rest/v1/rpc/".len()..], &me, b)
        }
        _ => err("404 Not Found", "not_found", "rota desconhecida"),
    }
}

fn friend_view(st: &FakeState, other: &str, state: &str, request_id: Option<&str>) -> Value {
    let p = st.profiles.get(other).cloned().unwrap_or(json!({ "user_id": other }));
    let accepted = state == "friend";
    json!({ "user_id": other, "nick": p["nick"], "display_name": p["display_name"], "avatar_url": p["avatar_url"],
        "fingerprint": if accepted { p["fingerprint"].clone() } else { Value::Null }, "public_key": if accepted { p["public_key"].clone() } else { Value::Null },
        "status": if accepted { p["status"].clone() } else { Value::Null }, "state": state, "request_id": request_id })
}

fn rpc(st: &mut FakeState, name: &str, me: &str, b: &Value) -> Resp {
    match name {
        "account_sync_profile" => {
            let name = b["p_display_name"].as_str().unwrap_or("").trim().to_string();
            if name.is_empty() || name.chars().count() > 64 {
                return pg("invalid_display_name");
            }
            if b["p_public_key"]["kty"] != "EC" || b["p_public_key"].get("d").is_some() {
                return pg("invalid_public_key");
            }
            let first = !st.profiles.contains_key(me);
            let nick = st.profiles.get(me).and_then(|p| p["nick"].as_str().map(String::from)).unwrap_or_else(|| {
                let base: String = name.to_lowercase().chars().filter(|c| c.is_ascii_alphanumeric()).take(12).collect();
                format!("{}_{}", if base.len() < 2 { "briks".into() } else { base }, &me[..4])
            });
            let old = st.profiles.get(me).cloned().unwrap_or(Value::Null);
            let keep = |k: &str, new: Value| if old[k].is_null() { new } else { old[k].clone() };   // perfil existente: nome e foto ficam como estavam
            let p = json!({ "user_id": me, "nick": nick, "display_name": keep("display_name", json!(name)), "avatar_url": keep("avatar_url", b["p_avatar_url"].clone()), "fingerprint": b["p_fingerprint"], "public_key": b["p_public_key"], "status": null });
            st.profiles.insert(me.to_string(), p.clone());
            if first && st.demo {
                for (i, (n, nk, accepted)) in [("Marina Costa", "marina", true), ("Rafael Lima", "rafa_lima", false)].iter().enumerate() {
                    let id = format!("00000000-0000-4000-8000-00000000000{i}");
                    st.profiles.insert(id.clone(), json!({ "user_id": id, "nick": nk, "display_name": n, "avatar_url": null, "fingerprint": format!("fp-demo-{i}"), "public_key": null, "status": if *accepted { json!("Bora jogar?") } else { Value::Null } }));
                    st.requests.push((uid(), id, me.to_string(), *accepted));
                }
            }
            ok(p)
        }
        "identity_backup_put" => {
            let blob = &b["p_blob"];
            let b64 = |k: &str, n: usize| blob[k].as_str().is_some_and(|s| s.len() == n && s.chars().all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_'));
            let range = |k: &str, lo: u64, hi: u64| blob[k].as_u64().is_some_and(|v| (lo..=hi).contains(&v));
            let fields = blob.as_object().map(|o| o.len()).unwrap_or(0);
            if !(blob["version"] == 1 && blob["kdf"] == "argon2id" && blob["cipher"] == "xchacha20poly1305" && range("m_kib", 19456, 262144) && range("t", 2, 10) && range("p", 1, 4)
                && b64("salt", 22) && b64("nonce", 32) && b64("ciphertext", 64) && blob["fingerprint"].as_str().is_some_and(|f| f.len() == 64) && fields == 10) {
                return pg("invalid_backup");
            }
            st.backups.insert(me.to_string(), json!({ "blob": blob, "updated_at": "2026-11-10T22:00:00+00:00" }));
            ok(Value::Null)
        }
        "identity_backup_get" => ok(st.backups.get(me).cloned().unwrap_or(Value::Null)),
        "identity_backup_delete" => {
            st.backups.remove(me);
            ok(Value::Null)
        }
        "account_update_profile" => {
            let Some(p) = st.profiles.get_mut(me) else { return pg("no_profile") };
            if let Some(n) = b["p_display_name"].as_str() {
                let n = n.trim();
                if n.is_empty() || n.chars().count() > 64 {
                    return pg("invalid_display_name");
                }
                p["display_name"] = json!(n);
            }
            if let Some(a) = b["p_avatar_url"].as_str() {
                if !(a.is_empty() || a.starts_with("https://") || a.starts_with("http://127.0.0.1:")) {
                    return pg("invalid_avatar");
                }
                p["avatar_url"] = if a.is_empty() { Value::Null } else { json!(a) };
            }
            ok(p.clone())
        }
        "account_set_nick" => {
            let n = b["p_nick"].as_str().unwrap_or("").to_string();
            if n.len() < 3 || n.len() > 20 || !n.chars().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_') {
                return pg("invalid_nick");
            }
            if st.profiles.iter().any(|(id, p)| id != me && p["nick"] == n.as_str()) {
                return pg("nick_taken");
            }
            let Some(p) = st.profiles.get_mut(me) else { return pg("not_found") };
            p["nick"] = json!(n);
            ok(p.clone())
        }
        "friends_list" => {
            let v: Vec<Value> = st.requests.iter().filter(|r| r.1 == me || r.2 == me).map(|(id, from, to, accepted)| {
                let other = if from == me { to } else { from };
                let state = if *accepted { "friend" } else if from == me { "outgoing" } else { "incoming" };
                friend_view(st, other, state, Some(id))
            }).collect();
            ok(json!(v))
        }
        "friend_request" => {
            let n = b["p_nick"].as_str().unwrap_or("");
            let Some(other) = st.profiles.iter().find(|(_, p)| p["nick"] == n).map(|(id, _)| id.clone()) else { return pg("not_found") };
            if other == me {
                return pg("self_request");
            }
            if let Some(r) = st.requests.iter().find(|r| (r.1 == me && r.2 == other) || (r.1 == other && r.2 == me)) {
                return pg(if r.3 { "already_friends" } else { "already_requested" });
            }
            st.requests.push((uid(), me.to_string(), other, false));
            ok(Value::Null)
        }
        "friend_respond" => {
            let id = b["p_request"].as_str().unwrap_or("");
            let Some(i) = st.requests.iter().position(|r| r.0 == id && r.2 == me && !r.3) else { return pg("not_found") };
            if b["p_accept"].as_bool().unwrap_or(false) { st.requests[i].3 = true } else { st.requests.remove(i); }
            ok(Value::Null)
        }
        "friend_remove" => {
            let other = b["p_user"].as_str().unwrap_or("");
            st.requests.retain(|r| !((r.1 == me && r.2 == other) || (r.1 == other && r.2 == me)));
            ok(Value::Null)
        }
        _ => ("404 Not Found", String::new(), json!({ "code": "PGRST202", "message": "Could not find the function" })),
    }
}

fn http_get(url: &str) -> Option<ureq::Response> {
    ureq::AgentBuilder::new().redirects(0).timeout(std::time::Duration::from_secs(5)).build().get(url).call().ok()
}

/// "Navegador" automático para o modo de desenvolvimento e os testes: segue o redirect do servidor falso até o loopback,
/// sem abrir janela nenhuma. Só aceita endereços 127.0.0.1.
pub fn auto_browser() -> Opener {
    Arc::new(|url: String| -> AResult<()> {
        if !url.starts_with("http://127.0.0.1:") {
            return Err(AccountError::new("browser", "O navegador automático só abre endereços locais."));
        }
        std::thread::spawn(move || {
            if let Some(loc) = http_get(&url).and_then(|r| r.header("Location").map(String::from)) {
                if loc.starts_with("http://127.0.0.1:") {
                    let _ = http_get(&loc);
                }
            }
        });
        Ok(())
    })
}

/// "Clica" no último link de e-mail enviado pelo servidor falso.
pub fn click_last_link(state: &Arc<Mutex<FakeState>>) -> bool {
    let Some(link) = state.lock().unwrap().last_link.take() else { return false };
    std::thread::spawn(move || {
        let _ = http_get(&link);
    });
    true
}
