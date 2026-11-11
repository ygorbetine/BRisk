//! `room.join` de ponta a ponta pelo IPC com SFU PURO (sem malha P2P e sem Supabase Realtime): dois "apps" (dois servidores IPC do
//! core, cada um com a sua pasta de dados, identidade e conta), contas no servidor simulado, serviço de tokens simulado que confere
//! o `Bearer` da sessão e um `livekit-server --dev` REAL. Cobre o que a UI usa: modo Automático escolhendo o SFU, lista de
//! participantes com impressão digital, dono, chat, status, evento de chat, fala, mudo remoto (`room.mute`), expulsão e a volta barrada.
//! Sem GPU, sem janela, sem placa de som (`BRIKS_CORE_FAKE_AUDIO=1`). Precisa de `BRIKS_LIVEKIT_SERVER_BIN`.
use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use serde_json::{json, Value};
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};

use crate::accounts::{fake, supabase::SupabaseProvider, Accounts, SignUpOutcome};
use crate::tests_sfu::dev_token;
use crate::tests_sfurec::start_server;

/// Pedidos recebidos pela função `livekit-kick` simulada: (Bearer, corpo JSON).
type Kicks = Arc<Mutex<Vec<(String, Value)>>>;

/// Serviço de tokens simulado: `Bearer <sessão>` conhecido -> token de dev com a identidade daquele usuário; senão 401.
/// O mesmo servidor responde por `…/livekit-kick` (a irmã da função de tokens): registra o pedido e devolve 200 (ou 500 se `kick_fails`).
fn token_service(users: Arc<Mutex<HashMap<String, String>>>, kicks: Kicks, kick_fails: Arc<std::sync::atomic::AtomicBool>) -> u16 {
    use std::io::{Read, Write};
    let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let port = l.local_addr().unwrap().port();
    std::thread::spawn(move || {
        for s in l.incoming() {
            let Ok(mut s) = s else { continue };
            let mut buf = vec![0u8; 8192];
            let mut n = 0;
            while n < buf.len() { let r = s.read(&mut buf[n..]).unwrap_or(0); if r == 0 { break; } n += r; if String::from_utf8_lossy(&buf[..n]).contains("\"room\"") { break; } }
            let req = String::from_utf8_lossy(&buf[..n]).to_string();
            let bearer = req.lines().find(|l| l.to_ascii_lowercase().starts_with("authorization:")).and_then(|l| l.split_whitespace().last()).unwrap_or("").to_string();
            let room = req.split("\"room\":\"").nth(1).and_then(|r| r.split('"').next()).unwrap_or("").to_string();
            let ident = users.lock().unwrap().get(&bearer).cloned();
            if req.starts_with("POST /functions/v1/livekit-kick") {
                let body_json: Value = req.split("\r\n\r\n").nth(1).and_then(|b| serde_json::from_str(b).ok()).unwrap_or(Value::Null);
                kicks.lock().unwrap().push((bearer.clone(), body_json.clone()));
                let (status, body) = if kick_fails.load(std::sync::atomic::Ordering::Relaxed) { ("500 Internal Server Error", json!({ "message": "falha simulada" }).to_string()) } else { ("200 OK", json!({ "removed": body_json["identity"] }).to_string()) };
                let _ = write!(s, "HTTP/1.1 {status}\r\ncontent-type: application/json\r\ncontent-length: {}\r\nconnection: close\r\n\r\n{body}", body.len());
                continue;
            }
            let (status, body) = match ident {
                Some(i) if !room.is_empty() => ("200 OK", json!({ "token": dev_token(&i, &room), "ttl": 3600 }).to_string()),
                _ => ("401 Unauthorized", json!({ "message": "Entre na sua conta Briks para usar o servidor de chamadas." }).to_string()),
            };
            let _ = write!(s, "HTTP/1.1 {status}\r\ncontent-type: application/json\r\ncontent-length: {}\r\nconnection: close\r\n\r\n{body}", body.len());
        }
    });
    port
}

/// Uma "UI": conexão IPC com um core, com todos os eventos guardados em ordem.
pub(crate) struct Ui { wr: tokio::net::tcp::OwnedWriteHalf, log: Arc<Mutex<Vec<Value>>>, fp: String }

impl Ui {
    pub(crate) async fn send(&mut self, v: Value) { let mut s = v.to_string(); s.push('\n'); self.wr.write_all(s.as_bytes()).await.unwrap(); }
    pub(crate) fn count(&self, f: impl Fn(&Value) -> bool) -> usize { self.log.lock().unwrap().iter().filter(|v| f(v)).count() }
    pub(crate) async fn wait(&self, secs: u64, what: &str, f: impl Fn(&Value) -> bool) -> Value {
        let end = Instant::now() + Duration::from_secs(secs);
        loop {
            if let Some(v) = self.log.lock().unwrap().iter().find(|v| f(v)).cloned() { return v; }
            if Instant::now() > end { let tail: Vec<String> = self.log.lock().unwrap().iter().rev().take(12).map(|v| v.to_string().chars().take(200).collect()).collect(); panic!("não chegou: {what}\núltimos eventos: {tail:#?}"); }
            tokio::time::sleep(Duration::from_millis(50)).await;
        }
    }
}

/// Sobe um core (IPC) com pasta de dados própria, conta `email` já logada e o SFU configurado em `livekit.json`. Devolve a UI e a sessão.
pub(crate) async fn app(tag: &str, accounts_url: &str, email: Option<&str>, name: &str, tport: u16) -> (Ui, String, std::path::PathBuf) {
    let dir = std::env::temp_dir().join(format!("briks-sfuipc-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir); std::fs::create_dir_all(&dir).unwrap();
    std::fs::write(dir.join("livekit.json"), json!({ "url": "ws://127.0.0.1:7880", "token_url": format!("http://127.0.0.1:{tport}/functions/v1/livekit-token") }).to_string()).unwrap();
    let (acc, session) = match email {
        Some(email) => {
            let acc = Accounts::new(Arc::new(SupabaseProvider::new(accounts_url, "chave-publica-de-teste")), &dir, fake::auto_browser());
            let SignUpOutcome::SignedIn(_) = acc.sign_up(email, "senha-boa-1", name).await.unwrap() else { panic!("cadastro") };
            let session = acc.access_token().await.unwrap();
            (Some(Arc::new(acc)), session)
        }
        None => (None, String::new()),
    };
    let rt = crate::accounts::service::Runtime::for_tests(acc, dir.clone());
    let (l, port) = crate::ipc::bind().await.unwrap();
    // SEM Realtime: o SFU puro não pode depender dele
    let cfg = crate::ipc::Config { data_dir: dir.clone(), realtime_url: None, realtime_key: None };
    tokio::spawn(crate::ipc::serve_with(l, "segredo".into(), cfg, rt));
    let s = tokio::net::TcpStream::connect(("127.0.0.1", port)).await.unwrap();
    let (rd, wr) = s.into_split();
    let log = Arc::new(Mutex::new(vec![]));
    let l2 = log.clone();
    tokio::spawn(async move { let mut lines = BufReader::new(rd).lines(); while let Ok(Some(l)) = lines.next_line().await { if let Ok(v) = serde_json::from_str::<Value>(&l) { l2.lock().unwrap().push(v); } } });
    let mut ui = Ui { wr, log, fp: String::new() };
    ui.send(json!({ "cmd": "hello", "token": "segredo" })).await;
    ui.wait(10, "hello.ok", |v| v["ev"] == "hello.ok").await;
    ui.send(json!({ "cmd": "session.start", "display_name": name })).await;
    ui.fp = ui.wait(10, "session.ready", |v| v["ev"] == "session.ready").await["fingerprint"].as_str().unwrap().to_string();
    (ui, session, dir)
}

const IA: &str = "aaaaaaaa-1111-4111-8111-00000000e2e1";
const IB: &str = "bbbbbbbb-2222-4222-8222-00000000e2e2";

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn ipc_room_join_pure_sfu_end_to_end_with_remote_mute_and_kick() {
    let Some(_srv) = start_server() else { eprintln!("PULADO: BRIKS_LIVEKIT_SERVER_BIN não definido"); return };
    std::env::set_var("BRIKS_CORE_FAKE_AUDIO", "1");
    let f = fake::start(0).await.unwrap();
    f.state.lock().unwrap().autoconfirm = true;
    let users = Arc::new(Mutex::new(HashMap::new()));
    let kicks: Kicks = Default::default();
    let kick_fails = Arc::new(std::sync::atomic::AtomicBool::new(false));
    let tport = token_service(users.clone(), kicks.clone(), kick_fails.clone());
    let (mut a, sa, da) = app("a", &f.url(), Some("ana@exemplo.com"), "Ana", tport).await;
    let (mut b, sb, db) = app("b", &f.url(), Some("beto@exemplo.com"), "Beto", tport).await;
    users.lock().unwrap().insert(sa.clone(), IA.into());
    users.lock().unwrap().insert(sb, IB.into());
    let room = format!("sala-e2e-{}", uuid::Uuid::new_v4().simple());
    let t0 = Instant::now();

    // 1) Automático (a UI não manda "net"): com o SFU configurado e o token obtido, entra pelo SFU
    a.send(json!({ "cmd": "room.join", "namespace": "room", "room": room })).await;
    let nm = a.wait(20, "net.mode de A", |v| v["ev"] == "net.mode").await;
    eprintln!("A: {nm}");
    assert_eq!((nm["mode"].as_str(), nm["configured"].as_bool(), nm["server"].as_str()), (Some("sfu"), Some(true), Some("127.0.0.1")));
    assert!(nm["reason"].as_str().unwrap().contains("automático"), "{nm}");
    assert_eq!(a.wait(10, "room.joining de A", |v| v["ev"] == "room.joining").await["peer_id"], IA, "o peer_id local é a identidade no SFU");
    a.wait(10, "sinalização conectada", |v| v["ev"] == "signaling.status" && v["state"] == "conectado").await;
    tokio::time::sleep(Duration::from_millis(300)).await;   // A é a mais antiga (dona)
    b.send(json!({ "cmd": "room.join", "namespace": "room", "room": room, "net": "auto" })).await;
    assert_eq!(b.wait(20, "net.mode de B", |v| v["ev"] == "net.mode").await["mode"], "sfu");
    assert_eq!(b.wait(10, "room.joining de B", |v| v["ev"] == "room.joining").await["peer_id"], IB);

    // 2) participantes com a impressão digital REAL (hello assinado), nome e dono
    let (fa, fb) = (a.fp.clone(), b.fp.clone());
    a.wait(30, "A vê o B", |v| v["ev"] == "peer.info" && v["peer_id"] == IB && v["fp"] == fb.as_str() && v["name"] == "Beto").await;
    b.wait(30, "B vê a A", |v| v["ev"] == "peer.info" && v["peer_id"] == IA && v["fp"] == fa.as_str() && v["name"] == "Ana").await;
    a.wait(10, "A autenticou B", |v| v["ev"] == "peer" && v["peer_id"] == IB && v["state"] == "autenticado").await;
    a.wait(10, "A é a dona", |v| v["ev"] == "room.owner" && v["me"] == true).await;
    b.wait(10, "B sabe quem é a dona", |v| v["ev"] == "room.owner" && v["me"] == false && v["fp"] == fa.as_str()).await;
    eprintln!("entrada: os dois apps se autenticaram pelo SFU em {:.1} s", t0.elapsed().as_secs_f32());

    // 3) voz (áudio sintético): indicador de fala do outro chega pela UI
    a.wait(15, "A ouve o B falar", |v| v["ev"] == "speaking" && v["peer"] == IB && v["level"].as_f64().unwrap_or(0.0) > 0.0).await;
    b.wait(15, "B ouve a A falar", |v| v["ev"] == "speaking" && v["peer"] == IA && v["level"].as_f64().unwrap_or(0.0) > 0.0).await;

    // 4) chat (DM assinada), status e evento de chat
    b.send(json!({ "cmd": "chat.send", "to_fp": fa, "text": "oi pelo SFU" })).await;
    a.wait(15, "DM do B", |v| v["ev"] == "chat.message" && v["content"] == "oi pelo SFU" && v["from_fp"] == fb.as_str() && v["author"] == "Beto").await;
    a.send(json!({ "cmd": "profile.set", "status": "em reunião", "presence": "Briks" })).await;
    b.wait(15, "status da A", |v| v["ev"] == "peer.state" && v["peer_id"] == IA && v["status"] == "em reunião" && v["presence"] == "Briks").await;
    a.send(json!({ "cmd": "chat.event.send", "channel": "dm-x", "target": "msg-0001-aaaa", "action": "reaction.add", "emoji": "👍" })).await;
    b.wait(15, "evento de chat da A", |v| v["ev"] == "chat.event" && v["local"] == false && v["from_fp"] == fa.as_str()).await;

    // 5) mudo remoto: quem não é dono é recusado; a dona fecha e reabre o microfone do B para todos
    b.send(json!({ "cmd": "room.mute", "fp": fa, "muted": true })).await;
    b.wait(10, "recusa do mudo remoto de quem não é dono", |v| v["ev"] == "error" && v["message"].as_str().unwrap_or("").contains("dono")).await;
    tokio::time::sleep(Duration::from_millis(500)).await;
    assert_eq!(a.count(|v| v["ev"] == "room.muted"), 0, "não-dono silenciou a dona");
    a.send(json!({ "cmd": "room.mute", "fp": fb, "muted": true })).await;
    let rm = b.wait(15, "B é silenciado pela dona", |v| v["ev"] == "room.muted" && v["muted"] == true).await;
    assert_eq!(rm["by"], fa.as_str());
    b.wait(10, "estado do microfone do B", |v| v["ev"] == "mic.state" && v["muted"] == true && v["microphone"] == false).await;
    a.wait(15, "A vê o microfone do B fechado", |v| v["ev"] == "peer.state" && v["peer_id"] == IB && v["microphone"] == false).await;
    a.send(json!({ "cmd": "room.mute", "fp": fb, "muted": false })).await;
    b.wait(15, "B é reaberto pela dona", |v| v["ev"] == "mic.state" && v["muted"] == false && v["microphone"] == true).await;
    // o estado local acompanha: ensurdecer e voltar não "desfaz" nem "refaz" o mudo da dona
    a.send(json!({ "cmd": "room.mute", "fp": fb, "muted": true })).await;
    b.wait(15, "B silenciado de novo", |v| v["ev"] == "room.muted" && v["muted"] == true).await;
    tokio::time::sleep(Duration::from_millis(300)).await;
    let n0 = b.count(|v| v["ev"] == "mic.state");
    b.send(json!({ "cmd": "deafen.set", "on": false })).await;
    let end = Instant::now() + Duration::from_secs(10);
    while b.count(|v| v["ev"] == "mic.state") == n0 && Instant::now() < end { tokio::time::sleep(Duration::from_millis(50)).await; }
    let last = b.log.lock().unwrap().iter().rev().find(|v| v["ev"] == "mic.state").cloned().unwrap();
    assert_eq!((last["muted"].as_bool(), last["microphone"].as_bool()), (Some(true), Some(false)), "o core esqueceu o mudo pedido pela dona: {last}");
    eprintln!("mudo remoto (room.mute): recusado para não-dono, aplicado e desfeito pela dona, estado local coerente");

    // 6) expulsão: o B é avisado e sai; a dona registra o banimento; o B não volta nesta sessão.
    // Quem não é dono não chama a função do servidor (e o pedido assinado dele é recusado pelos outros)
    b.send(json!({ "cmd": "peer.kick", "fp": fa, "reason": "abuso" })).await;
    tokio::time::sleep(Duration::from_millis(800)).await;
    assert!(kicks.lock().unwrap().is_empty(), "não-dono chamou a função de expulsão");
    assert_eq!(a.count(|v| v["ev"] == "kicked"), 0);
    a.send(json!({ "cmd": "peer.kick", "fp": fb, "reason": "teste" })).await;
    b.wait(15, "B expulso", |v| v["ev"] == "kicked").await;
    // a dona também pediu ao servidor (função livekit-kick): sessão DELA no Bearer, sala do LiveKit e identidade do B no corpo
    let nk = a.wait(15, "resultado do pedido ao servidor", |v| v["ev"] == "net.kick").await;
    assert_eq!(nk["ok"], true, "{nk}");
    {
        let k = kicks.lock().unwrap();
        assert_eq!(k.len(), 1, "a função de expulsão devia ser chamada uma vez: {k:?}");
        assert_eq!(k[0].0, sa, "Bearer não é a sessão da dona");
        let lk_room = format!("room:{}", &crate::signaling::secure_room_id("room", &room)[..32]);
        assert_eq!(k[0].1, json!({ "room": lk_room, "identity": IB }));
    }
    // falha do serviço (função não publicada, 500, sem rede) não é erro para o usuário: o aviso por dados já valeu
    kick_fails.store(true, std::sync::atomic::Ordering::Relaxed);
    let url = format!("http://127.0.0.1:{tport}/functions/v1/livekit-kick");
    let e = tokio::task::spawn_blocking(move || crate::sfu::kick_remote(&url, "x", "room:a", IB)).await.unwrap().err().expect("500 devia virar erro");
    assert!(e.contains("500") && e.contains("falha simulada"), "{e}");
    assert!(crate::sfu::kick_remote("http://exemplo.com/k", "x", "room:a", IB).is_err(), "http fora de loopback: a sessão nem sai");
    assert_eq!(a.count(|v| v["ev"] == "error" && v["message"].as_str().unwrap_or("").contains("expuls")), 0);
    a.wait(15, "A registra o banimento", |v| v["ev"] == "peer.banned" && v["fp"] == fb.as_str()).await;
    b.send(json!({ "cmd": "room.join", "namespace": "room", "room": room })).await;
    b.wait(10, "volta barrada", |v| v["ev"] == "error" && v["where"] == "room.join" && v["message"].as_str().unwrap_or("").contains("removido")).await;

    // 7) sem conta: o token não sai e a chamada NÃO abre (nem em P2P)
    let (mut c, _, dc) = app("c", &f.url(), None, "Caio", tport).await;
    c.send(json!({ "cmd": "room.join", "namespace": "room", "room": room })).await;
    let nm = c.wait(15, "net.mode sem conta", |v| v["ev"] == "net.mode").await;
    assert_eq!((nm["mode"].as_str(), nm["fallback"].as_bool()), (Some("none"), Some(false)), "{nm}");
    c.wait(10, "erro visível sem conta", |v| v["ev"] == "error" && v["where"] == "room.join" && v["message"].as_str().unwrap_or("").contains("conta")).await;
    tokio::time::sleep(Duration::from_millis(500)).await;
    assert_eq!(c.count(|v| v["ev"] == "room.joining"), 0, "abriu uma sala à parte");

    a.send(json!({ "cmd": "room.leave" })).await;
    a.wait(10, "A saiu", |v| v["ev"] == "signaling.status" && v["state"] == "desconectado").await;
    eprintln!("room.join de ponta a ponta com SFU puro: ok em {:.1} s (sem Realtime, sem malha)", t0.elapsed().as_secs_f32());
    tokio::time::sleep(Duration::from_millis(300)).await;
    for d in [da, db, dc] { let _ = std::fs::remove_dir_all(d); }
}
