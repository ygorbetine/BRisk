//! Ponte entre os comandos do IPC (`account.*`, `friends.*`, `local.credentials`, `import.*`) e o `Accounts`.
//! Eventos emitidos para a UI:
//!   account.state   {state: signed_out|busy|email_sent|signed_in, what?, provider?, kind?, email?, user?, recovery?, error_code?, message?, configured, mock}
//!   account.avatar  {path|null}                   foto do perfil no cache local (PNG/JPEG); null = sem foto
//!   comandos de personalização: account.name {name}, account.photo {path}, account.photo_reset, account.nick {nick}
//!   account.profile {nick, fingerprint}            perfil publicado (chave pública ligada à conta)
//!   account.notice  {error_code, message}          falha que não derruba a sessão
//!   backup.state    {exists, matches, updated_at}  cópia cifrada da identidade: existe? é a deste aparelho?
//!   backup.phrase   {words:[12]}                   frase recém-gerada, mostrada uma vez (comandos: backup.create/confirm/cancel)
//!   backup.result   {op: save|restore|delete, ok, message}   (comandos: backup.restore {phrase}, backup.delete, backup.status)
//!   identity.changed {fingerprint}                 a identidade deste aparelho foi trocada pela recuperada
//!   friends.list    {friends:[...]} | friends.result {ok, message}
//!   presence.beat {invisible} / presence.friends / presence.clear -> presence.list {remote, friends:[{user_id,online,last_seen}]}   (0007)
//!   friends.blocks  {remote, blocks:[{user_id,nick,display_name}]}   (friends.block/unblock/blocks {user_id}); remote=false: servidor sem a 0006, vale a lista local da UI
//!   local.credentials {email, password, name}      ponte com o banco local (nunca a senha da conta)
//!   import.result   {state, friends, groups, messages, message}
use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;

use serde_json::{json, Value};
use tokio::sync::mpsc;
use tokio::task::JoinHandle;

use super::config::{self, AccountsConfig};
use super::supabase::SupabaseProvider;
use super::{avatar, backup, fake, legacy, AccountError, Accounts, CodeKind, OAuthProvider, SignUpOutcome};

/// Criado uma vez por processo.
pub struct Runtime {
    pub acc: Option<Arc<Accounts>>,
    pub reason: String,
    pub mock: bool,
    pub data_dir: PathBuf,
    _fake: Option<fake::Fake>,
}

impl Runtime {
    #[cfg(test)]
    pub fn for_tests(acc: Option<Arc<Accounts>>, data_dir: PathBuf) -> Arc<Runtime> {
        let reason = if acc.is_none() { "Servidor de contas não configurado.".to_string() } else { String::new() };
        Arc::new(Runtime { acc, reason, mock: false, data_dir, _fake: None })
    }
    pub async fn build(data_dir: PathBuf, realtime: Option<(String, String)>) -> Arc<Runtime> {
        let env = |k: &str| std::env::var(k).ok().filter(|v| !v.trim().is_empty());
        match config::resolve(&env, &data_dir, realtime) {
            AccountsConfig::Supabase { url, anon_key } => {
                let acc = Accounts::new(Arc::new(SupabaseProvider::new(&url, &anon_key)), &data_dir, super::system_browser());
                Arc::new(Runtime { acc: Some(Arc::new(acc)), reason: String::new(), mock: false, data_dir, _fake: None })
            }
            AccountsConfig::Mock => match fake::start(0).await {
                Ok(f) => {
                    {
                        let mut st = f.state.lock().unwrap();
                        st.demo = true;
                        st.autoconfirm = std::env::var("BRIKS_ACCOUNTS_MOCK_CONFIRM").is_err();   // =1: exige confirmar o e-mail (código 123456)
                        st.deny_oauth = std::env::var("BRIKS_ACCOUNTS_MOCK_DENY").is_ok();
                    }
                    // sessão do modo falso fica numa pasta própria: nunca se mistura com a sessão real
                    let dir = data_dir.join("mock-accounts");
                    let acc = Accounts::new(Arc::new(SupabaseProvider::new(&f.url(), "chave-publica-falsa")), &dir, fake::auto_browser());
                    Arc::new(Runtime { acc: Some(Arc::new(acc)), reason: String::new(), mock: true, data_dir, _fake: Some(f) })
                }
                Err(e) => Arc::new(Runtime { acc: None, reason: format!("Servidor de contas falso não iniciou: {e}"), mock: true, data_dir, _fake: None }),
            },
            AccountsConfig::Missing(reason) => Arc::new(Runtime { acc: None, reason, mock: false, data_dir, _fake: None }),
        }
    }
}

/// Um por conexão de UI.
pub struct Service {
    /// frase recém-gerada, esperando o "anotei" da pessoa; só existe na memória e some ao confirmar ou cancelar
    pending_phrase: Arc<std::sync::Mutex<Option<String>>>,
    rt: Arc<Runtime>,
    tx: mpsc::Sender<Value>,
    flow: Option<JoinHandle<()>>,
    refresher: Option<JoinHandle<()>>,
}

fn base(rt: &Runtime, state: &str) -> Value {
    json!({ "ev": "account.state", "state": state, "configured": rt.acc.is_some(), "mock": rt.mock })
}
fn with(mut v: Value, extra: Value) -> Value {
    if let (Some(a), Some(b)) = (v.as_object_mut(), extra.as_object()) {
        for (k, x) in b {
            a.insert(k.clone(), x.clone());
        }
    }
    v
}
fn failed(rt: &Runtime, e: &AccountError) -> Value {
    with(base(rt, "signed_out"), json!({ "error_code": e.code, "message": e.message }))
}

/// Depois de entrar: avisa a UI e publica a chave pública da identidade P2P no perfil da conta.
async fn signed_in(rt: &Runtime, acc: &Accounts, tx: &mpsc::Sender<Value>, recovery: bool) {
    let Some(u) = acc.user() else { return };
    // troca de conta (entrou com outra por cima): nada da anterior fica à mostra nem em cache
    if let Some(prev) = acc.take_previous_id(&u.id) { avatar::clear(&rt.data_dir, &prev); }
    // foto: usa o cache se houver; baixa em segundo plano e avisa quando chegar. nick_hint: provisório, derivado do nome.
    let photo = avatar::cached(&rt.data_dir, &u.id);
    let _ = tx.send(with(base(rt, "signed_in"), json!({ "user": u, "recovery": recovery, "photo": photo, "nick_hint": avatar::nick_hint(&u.display_name) }))).await;

    let id = match crate::ipc::load_or_create_identity(&rt.data_dir) {
        Ok(id) => id,
        Err(e) => {
            let _ = tx.send(json!({ "ev": "account.notice", "error_code": "identity", "message": format!("Identidade deste aparelho indisponível: {e}") })).await;
            return;
        }
    };
    let jwk = serde_json::to_value(id.public_jwk()).unwrap_or(Value::Null);   // só x/y públicos; o segredo nunca entra aqui
    // o nome e a foto que valem são os do perfil no servidor (a pessoa pode ter personalizado); sem perfil, os do provedor
    let mut photo_url = u.avatar_url.clone();
    match acc.publish_identity(jwk, &id.fingerprint()).await {
        Ok(p) => {
            acc.remember_name(&p.display_name);
            photo_url = p.avatar_url.clone().unwrap_or_default();
            let _ = tx.send(json!({ "ev": "account.profile", "nick": p.nick, "fingerprint": p.fingerprint, "display_name": p.display_name })).await;
        }
        Err(e) => {
            let _ = tx.send(json!({ "ev": "account.notice", "error_code": e.code, "message": e.message })).await;
        }
    }
    refresh_photo(rt, &u.id, photo_url, tx).await;
}

/// Baixa a foto para o cache e avisa a UI; endereço vazio = sem foto (apaga o cache e a UI volta às iniciais).
async fn refresh_photo(rt: &Runtime, user_id: &str, url: String, tx: &mpsc::Sender<Value>) {
    let (dir, id) = (rt.data_dir.clone(), user_id.to_string());
    let path = tokio::task::spawn_blocking(move || {
        if url.is_empty() {
            avatar::clear(&dir, &id);
            return None;
        }
        avatar::cache(&dir, &id, &url).ok().or_else(|| avatar::cached(&dir, &id))
    }).await.ok().flatten();
    let _ = tx.send(json!({ "ev": "account.avatar", "path": path })).await;
}

/// Situação da cópia: existe? é da identidade deste aparelho? Nunca inclui o pacote cifrado nem a frase.
async fn backup_state(rt: &Runtime, acc: &Accounts) -> Value {
    let local = crate::ipc::load_or_create_identity(&rt.data_dir).map(|i| i.fingerprint()).unwrap_or_default();
    match acc.backup_fetch().await {
        Ok(Some((blob, updated))) => json!({ "ev": "backup.state", "exists": true, "matches": blob.fingerprint == local, "updated_at": updated }),
        Ok(None) => json!({ "ev": "backup.state", "exists": false, "matches": false }),
        Err(e) => json!({ "ev": "backup.state", "exists": false, "matches": false, "error_code": e.code, "message": e.message }),
    }
}

fn backup_result(op: &str, r: Result<&str, AccountError>) -> Value {
    match r {
        Ok(m) => json!({ "ev": "backup.result", "op": op, "ok": true, "message": m }),
        Err(e) => json!({ "ev": "backup.result", "op": op, "ok": false, "error_code": e.code, "message": e.message }),
    }
}

/// Falha de um comando já logado. Sessão derrubada (token expirado e sem renovação possível) vira "signed_out" para a UI
/// voltar à entrada; qualquer outra falha é só um aviso e a sessão continua.
fn notice(rt: &Runtime, e: &AccountError) -> Value {
    if e.is_auth() { failed(rt, e) } else { json!({ "ev": "account.notice", "error_code": e.code, "message": e.message }) }
}

fn profile_ev(p: &super::Profile, message: &str) -> Value {
    json!({ "ev": "account.profile", "nick": p.nick, "fingerprint": p.fingerprint, "display_name": p.display_name, "changed": true, "message": message })
}

impl Service {
    pub fn new(rt: Arc<Runtime>, tx: mpsc::Sender<Value>) -> Self {
        // renovação automática: acorda 60 s antes de o access token vencer; sessão inválida derruba o login na UI
        let refresher = rt.acc.clone().map(|acc| {
            let (rt2, tx2) = (rt.clone(), tx.clone());
            tokio::spawn(async move {
                loop {
                    match acc.refresh_in() {
                        None => tokio::time::sleep(Duration::from_secs(5)).await,
                        Some(secs) => {
                            tokio::time::sleep(Duration::from_secs(secs.max(1))).await;
                            match acc.access_token().await {
                                Ok(_) => {}
                                Err(e) if e.is_auth() => {
                                    if tx2.send(failed(&rt2, &e)).await.is_err() {
                                        return;
                                    }
                                }
                                Err(_) => tokio::time::sleep(Duration::from_secs(30)).await,
                            }
                        }
                    }
                }
            })
        });
        Service { pending_phrase: Default::default(), rt, tx, flow: None, refresher }
    }

    fn start_flow<F>(&mut self, what: Value, f: F)
    where
        F: std::future::Future<Output = ()> + Send + 'static,
    {
        if let Some(t) = self.flow.take() {
            t.abort();
        }
        let (tx, ev) = (self.tx.clone(), with(base(&self.rt, "busy"), what));
        self.flow = Some(tokio::spawn(async move {
            let _ = tx.send(ev).await;
            f.await;
        }));
    }

    /// Devolve true se o comando era deste módulo.
    pub fn command(&mut self, cmd: &str, v: &Value) -> bool {
        if !(cmd.starts_with("account.") || cmd.starts_with("backup.") || cmd.starts_with("friends.") || cmd.starts_with("import.") || cmd == "local.credentials") {
            return false;
        }
        let (rt, tx) = (self.rt.clone(), self.tx.clone());
        let s = |k: &str| v[k].as_str().unwrap_or("").to_string();
        let Some(acc) = rt.acc.clone() else {
            let ev = with(base(&rt, "signed_out"), json!({ "error_code": "not_configured", "message": rt.reason }));
            tokio::spawn(async move {
                let _ = tx.send(ev).await;
            });
            return true;
        };
        match cmd {
            "account.status" => self.start_flow(json!({ "what": "restore" }), async move {
                match acc.restore().await {
                    Ok(Some(_)) => signed_in(&rt, &acc, &tx, false).await,
                    Ok(None) => { let _ = tx.send(base(&rt, "signed_out")).await; }
                    Err(e) => { let _ = tx.send(failed(&rt, &e)).await; }
                }
            }),
            "account.oauth" => {
                let Some(p) = OAuthProvider::parse(&s("provider")) else { return true };
                self.start_flow(json!({ "what": "oauth", "provider": p.as_str() }), async move {
                    match acc.sign_in_oauth(p).await {
                        Ok(_) => signed_in(&rt, &acc, &tx, false).await,
                        Err(e) => { let _ = tx.send(failed(&rt, &e)).await; }
                    }
                })
            }
            "account.login" => {
                let (email, pw) = (s("email"), s("password"));
                self.start_flow(json!({ "what": "login" }), async move {
                    match acc.sign_in_password(&email, &pw).await {
                        Ok(_) => signed_in(&rt, &acc, &tx, false).await,
                        Err(e) => { let _ = tx.send(failed(&rt, &e)).await; }
                    }
                })
            }
            "account.signup" => {
                let (email, pw, name) = (s("email"), s("password"), s("name"));
                self.start_flow(json!({ "what": "signup" }), async move {
                    match acc.sign_up(&email, &pw, &name).await {
                        Ok(SignUpOutcome::SignedIn(_)) => signed_in(&rt, &acc, &tx, false).await,
                        Ok(SignUpOutcome::ConfirmEmail(w)) => {
                            let _ = tx.send(with(base(&rt, "email_sent"), json!({ "kind": "signup", "email": email.trim() }))).await;
                            match acc.finish_email(w).await {
                                Ok(_) => signed_in(&rt, &acc, &tx, false).await,
                                // o prazo do link neste computador acabou: a conta continua criada; basta entrar depois de confirmar
                                Err(e) if e.code == "timeout" => { let _ = tx.send(base(&rt, "signed_out")).await; }
                                Err(e) => { let _ = tx.send(failed(&rt, &e)).await; }
                            }
                        }
                        Err(e) => { let _ = tx.send(failed(&rt, &e)).await; }
                    }
                })
            }
            "account.recover" => {
                let email = s("email");
                self.start_flow(json!({ "what": "recover" }), async move {
                    match acc.recover(&email).await {
                        Ok(w) => {
                            let _ = tx.send(with(base(&rt, "email_sent"), json!({ "kind": "recovery", "email": email.trim() }))).await;
                            match acc.finish_email(w).await {
                                Ok(_) => signed_in(&rt, &acc, &tx, true).await,
                                Err(e) if e.code == "timeout" => { let _ = tx.send(base(&rt, "signed_out")).await; }
                                Err(e) => { let _ = tx.send(failed(&rt, &e)).await; }
                            }
                        }
                        Err(e) => { let _ = tx.send(failed(&rt, &e)).await; }
                    }
                })
            }
            "account.code" => {
                // código do e-mail: substitui a espera pelo link (start_flow aborta a espera anterior)
                let (email, code) = (s("email"), s("code"));
                let recovery = s("kind") == "recovery";
                self.start_flow(json!({ "what": "code" }), async move {
                    match acc.verify_code(&email, &code, if recovery { CodeKind::Recovery } else { CodeKind::Signup }).await {
                        Ok(_) => signed_in(&rt, &acc, &tx, recovery).await,
                        Err(e) => { let _ = tx.send(with(base(&rt, "email_sent"), json!({ "kind": if recovery { "recovery" } else { "signup" }, "email": email.trim(), "error_code": e.code, "message": e.message }))).await; }
                    }
                })
            }
            "account.cancel" => {
                if let Some(t) = self.flow.take() {
                    t.abort();
                }
                tokio::spawn(async move {
                    let ev = match acc.user() {
                        Some(u) => with(base(&rt, "signed_in"), json!({ "user": u })),
                        None => with(base(&rt, "signed_out"), json!({ "error_code": "cancelled", "message": "Login cancelado." })),
                    };
                    let _ = tx.send(ev).await;
                });
            }
            "account.logout" => {
                if let Some(t) = self.flow.take() {
                    t.abort();
                }
                tokio::spawn(async move {
                    if let Some(u) = acc.user() {
                        avatar::clear(&rt.data_dir, &u.id);   // sair apaga também a foto guardada
                    }
                    acc.sign_out().await;
                    let _ = tx.send(base(&rt, "signed_out")).await;
                });
            }
            "account.password" => {
                let pw = s("password");
                tokio::spawn(async move {
                    let ev = match acc.set_password(&pw).await {
                        Ok(()) => json!({ "ev": "account.password", "ok": true, "message": "Senha alterada." }),
                        Err(e) => json!({ "ev": "account.password", "ok": false, "error_code": e.code, "message": e.message }),
                    };
                    let dropped = matches!(&ev["error_code"], Value::String(c) if c == "session_expired");
                    let _ = tx.send(ev).await;
                    if dropped { let _ = tx.send(failed(&rt, &AccountError::session_expired())).await; }
                });
            }
            "account.nick" => {
                let nick = s("nick");
                tokio::spawn(async move {
                    let ev = match acc.set_nick(&nick).await {
                        Ok(p) => json!({ "ev": "account.profile", "nick": p.nick, "fingerprint": p.fingerprint, "display_name": p.display_name, "changed": true }),
                        Err(e) => notice(&rt, &e),
                    };
                    let _ = tx.send(ev).await;
                });
            }
            "account.name" => {
                let name = s("name");
                tokio::spawn(async move {
                    let ev = match acc.set_display_name(&name).await {
                        Ok(p) => profile_ev(&p, "Nome alterado."),
                        Err(e) => notice(&rt, &e),
                    };
                    let _ = tx.send(ev).await;
                });
            }
            "account.photo" | "account.photo_reset" => {
                let (reset, path) = (cmd == "account.photo_reset", PathBuf::from(s("path")));
                tokio::spawn(async move {
                    let Some(u) = acc.user() else { return };
                    let r = if reset {
                        acc.reset_photo().await.map(|p| (p, None))
                    } else {
                        match tokio::task::spawn_blocking(move || avatar::prepare(&path)).await.unwrap_or_else(|_| Err(AccountError::new("avatar", "Não foi possível preparar a imagem."))) {
                            Ok(jpeg) => acc.set_photo(jpeg.clone()).await.map(|p| (p, Some(jpeg))),
                            Err(e) => Err(e),
                        }
                    };
                    match r {
                        Ok((p, Some(jpeg))) => {
                            // a foto enviada vai direto para o cache: aparece na hora, sem baixar de volta
                            let path = avatar::store(&rt.data_dir, &u.id, &jpeg).ok();
                            let _ = tx.send(json!({ "ev": "account.avatar", "path": path })).await;
                            let _ = tx.send(profile_ev(&p, "Foto alterada.")).await;
                        }
                        Ok((p, None)) => {
                            refresh_photo(&rt, &u.id, p.avatar_url.clone().unwrap_or_default(), &tx).await;
                            let _ = tx.send(profile_ev(&p, "Voltamos para a foto da sua conta.")).await;
                        }
                        Err(e) => {
                            let _ = tx.send(notice(&rt, &e)).await;
                        }
                    }
                });
            }
            "backup.status" => {
                tokio::spawn(async move {
                    let _ = tx.send(backup_state(&rt, &acc).await).await;
                });
            }
            "backup.create" => {
                // gera a frase e mostra UMA vez; nada vai ao servidor antes do "anotei" (backup.confirm)
                let phrase = backup::new_phrase();
                *self.pending_phrase.lock().unwrap() = Some(phrase.clone());
                tokio::spawn(async move {
                    let words: Vec<&str> = phrase.split(' ').collect();
                    let _ = tx.send(json!({ "ev": "backup.phrase", "words": words })).await;
                });
            }
            "backup.cancel" => {
                *self.pending_phrase.lock().unwrap() = None;
            }
            "backup.confirm" => {
                let phrase = self.pending_phrase.lock().unwrap().take();
                tokio::spawn(async move {
                    let r = match (phrase, crate::ipc::load_or_create_identity(&rt.data_dir)) {
                        (Some(ph), Ok(id)) => acc.backup_save(&id, &ph).await.map(|_| ()),
                        (None, _) => Err(AccountError::new("backup", "A frase expirou. Gere uma nova.")),
                        (_, Err(_)) => Err(AccountError::new("backup", "Identidade deste aparelho indisponível.")),
                    };
                    let dropped = matches!(&r, Err(e) if e.is_auth());
                    let _ = tx.send(backup_result("save", r.map(|_| "Cópia de segurança guardada. Só a sua frase consegue abri-la."))).await;
                    if dropped { let _ = tx.send(failed(&rt, &AccountError::session_expired())).await; return; }
                    let _ = tx.send(backup_state(&rt, &acc).await).await;
                });
            }
            "backup.restore" => {
                let phrase = s("phrase");
                tokio::spawn(async move {
                    let r: Result<String, AccountError> = async {
                        let id = acc.backup_open(&phrase).await?;
                        crate::ipc::install_identity(&rt.data_dir, &id).map_err(|_| AccountError::new("backup", "Não foi possível gravar a identidade neste computador."))?;
                        Ok(id.fingerprint())
                    }.await;
                    match r {
                        Ok(fp) => {
                            // a identidade deste aparelho mudou: a UI reabre a sessão P2P e o perfil passa a anunciar a chave recuperada
                            let _ = tx.send(json!({ "ev": "identity.changed", "fingerprint": fp })).await;
                            if let Ok(id) = crate::ipc::load_or_create_identity(&rt.data_dir) {
                                let jwk = serde_json::to_value(id.public_jwk()).unwrap_or(Value::Null);
                                match acc.publish_identity(jwk, &id.fingerprint()).await {
                                    Ok(p) => { let _ = tx.send(json!({ "ev": "account.profile", "nick": p.nick, "fingerprint": p.fingerprint, "display_name": p.display_name })).await; }
                                    Err(e) => { let _ = tx.send(json!({ "ev": "account.notice", "error_code": e.code, "message": e.message })).await; }
                                }
                            }
                            let _ = tx.send(backup_result("restore", Ok("Identidade recuperada neste computador."))).await;
                        }
                        Err(e) => {
                            let dropped = e.is_auth();
                            let _ = tx.send(backup_result("restore", Err(e))).await;
                            if dropped { let _ = tx.send(failed(&rt, &AccountError::session_expired())).await; return; }
                        }
                    }
                    let _ = tx.send(backup_state(&rt, &acc).await).await;
                });
            }
            "backup.delete" => {
                tokio::spawn(async move {
                    let r = acc.backup_delete().await;
                    let _ = tx.send(backup_result("delete", r.map(|_| "Cópia de segurança apagada do servidor."))).await;
                    let _ = tx.send(backup_state(&rt, &acc).await).await;
                });
            }
            "friends.lookup" => {
                // busca exata por @nick (0008) para convidar para grupo; evento friends.lookup {ok, hit | error_code, message, nick}
                let nick = s("nick");
                tokio::spawn(async move {
                    let ev = match acc.nick_lookup(&nick).await {
                        Ok(h) => json!({ "ev": "friends.lookup", "ok": true, "nick": nick, "hit": h }),
                        Err(e) => json!({ "ev": "friends.lookup", "ok": false, "nick": nick, "error_code": e.code, "message": e.message }),
                    };
                    let _ = tx.send(ev).await;
                });
            }
            "presence.beat" | "presence.friends" | "presence.clear" => {
                // batimento de presença (0007): o app manda a cada ~60 s; sem a RPC (`server_not_ready`) remote=false e a UI usa só "na chamada"
                let (cmd, invisible) = (cmd.to_string(), v["invisible"].as_bool().unwrap_or(false));
                tokio::spawn(async move {
                    let r = match cmd.as_str() {
                        "presence.beat" => acc.presence_beat(invisible).await,
                        "presence.clear" => { let _ = acc.presence_clear().await; return; }
                        _ => Ok(()),
                    };
                    let list = match r {
                        Ok(_) => acc.presence_friends().await,
                        Err(e) => Err(e),
                    };
                    let ev = match list {
                        Ok(l) => json!({ "ev": "presence.list", "remote": true, "friends": l }),
                        Err(e) if e.code == "server_not_ready" => json!({ "ev": "presence.list", "remote": false, "friends": [] }),
                        Err(_) => return,   // falha de rede: mantém o que já se sabia; o próximo batimento tenta de novo
                    };
                    let _ = tx.send(ev).await;
                });
            }
            "friends.block" | "friends.unblock" | "friends.blocks" => {
                // bloqueio no servidor (0006). Sem a RPC (`server_not_ready`) a UI segue com a lista local: remote=false
                let (cmd, user) = (cmd.to_string(), s("user_id"));
                tokio::spawn(async move {
                    let r = match cmd.as_str() {
                        "friends.block" => acc.friend_block(&user).await.map(|_| ()),
                        "friends.unblock" => acc.friend_unblock(&user).await.map(|_| ()),
                        _ => Ok(()),
                    };
                    let remote = match &r { Ok(_) => true, Err(e) if e.code == "server_not_ready" => false, Err(e) => {
                        let _ = tx.send(json!({ "ev": "friends.result", "ok": false, "error_code": e.code, "message": e.message })).await;
                        return;
                    } };
                    let list = if remote { acc.blocks().await.ok() } else { None };
                    let remote = remote && list.is_some();
                    let _ = tx.send(json!({ "ev": "friends.blocks", "remote": remote, "blocks": list.unwrap_or_default() })).await;
                    if remote && cmd != "friends.blocks" {
                        if let Ok(f) = acc.friends().await { let _ = tx.send(json!({ "ev": "friends.list", "friends": f })).await; }
                    }
                });
            }
            "friends.list" | "friends.request" | "friends.respond" | "friends.remove" => {
                let (cmd, nick, req, user, accept) = (cmd.to_string(), s("nick"), s("request_id"), s("user_id"), v["accept"].as_bool().unwrap_or(true));
                tokio::spawn(async move {
                    let r = match cmd.as_str() {
                        "friends.request" => acc.friend_request(&nick).await.map(|_| "Pedido enviado."),
                        "friends.respond" => acc.friend_respond(&req, accept).await.map(|_| if accept { "Agora vocês são amigos." } else { "Pedido recusado." }),
                        "friends.remove" => acc.friend_remove(&user).await.map(|_| "Amizade desfeita."),
                        _ => Ok(""),
                    };
                    match r {
                        Ok(m) if !m.is_empty() => { let _ = tx.send(json!({ "ev": "friends.result", "ok": true, "message": m })).await; }
                        Err(e) => {
                            let _ = tx.send(json!({ "ev": "friends.result", "ok": false, "error_code": e.code, "message": e.message })).await;
                            if e.is_auth() { let _ = tx.send(failed(&rt, &e)).await; }
                            return;
                        }
                        _ => {}
                    }
                    match acc.friends().await {
                        Ok(f) => { let _ = tx.send(json!({ "ev": "friends.list", "friends": f })).await; }
                        Err(e) => {
                            let _ = tx.send(json!({ "ev": "friends.result", "ok": false, "error_code": e.code, "message": e.message })).await;
                            if e.is_auth() { let _ = tx.send(failed(&rt, &e)).await; }
                        }
                    }
                });
            }
            "local.credentials" => {
                tokio::spawn(async move {
                    let ev = match (acc.local_credentials(), acc.user()) {
                        (Ok((email, password)), Some(u)) => json!({ "ev": "local.credentials", "email": email, "password": password, "name": u.display_name }),
                        (Err(e), _) => json!({ "ev": "account.notice", "error_code": e.code, "message": e.message }),
                        _ => json!({ "ev": "account.notice", "error_code": "session_expired", "message": "Entre na conta primeiro." }),
                    };
                    let _ = tx.send(ev).await;
                });
            }
            "import.probe" | "import.apply" => {
                let apply = cmd == "import.apply";
                tokio::spawn(async move {
                    let src = std::env::var("BRIKS_IMPORT_SOURCE").map(PathBuf::from).unwrap_or_else(|_| crate::import::default_source());
                    let dest = rt.data_dir.clone();
                    let creds = acc.local_credentials();
                    let r = tokio::task::spawn_blocking(move || -> Value {
                        if !apply {
                            return legacy::probe(&src, &dest);
                        }
                        match creds {
                            Ok((email, _)) => legacy::apply(&src, &dest, &email).unwrap_or_else(|e| json!({ "state": "error", "message": e.to_string() })),
                            Err(e) => json!({ "state": "error", "message": e.message }),
                        }
                    }).await.unwrap_or_else(|_| json!({ "state": "error", "message": "Falha interna na importação." }));
                    let _ = tx.send(with(json!({ "ev": "import.result", "applied": apply }), r)).await;
                });
            }
            _ => {}
        }
        true
    }
}

impl Drop for Service {
    fn drop(&mut self) {
        if let Some(t) = self.flow.take() {
            t.abort();
        }
        if let Some(t) = self.refresher.take() {
            t.abort();
        }
    }
}
