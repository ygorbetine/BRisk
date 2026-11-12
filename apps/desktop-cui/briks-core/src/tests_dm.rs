//! DM fora da sala de voz de ponta a ponta pelo IPC (dmlink.rs): dois cores com o Realtime simulado, SEM nenhuma sala entre eles.
//! A escreve antes de B sequer observar a própria caixa; B abre o Briks depois, vê o toque, abre a outra ponta e a mensagem
//! chega, com os estados "saiu" (chat.sent) e "entregue" (chat.ack) de volta em A. Depois B responde.
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use serde_json::{json, Value};
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};

struct Ui { wr: tokio::net::tcp::OwnedWriteHalf, log: Arc<Mutex<Vec<Value>>>, fp: String }

impl Ui {
    async fn send(&mut self, v: Value) { let mut s = v.to_string(); s.push('\n'); self.wr.write_all(s.as_bytes()).await.unwrap(); }
    async fn wait(&self, secs: u64, what: &str, f: impl Fn(&Value) -> bool) -> Value {
        let end = Instant::now() + Duration::from_secs(secs);
        loop {
            if let Some(v) = self.log.lock().unwrap().iter().find(|v| f(v)).cloned() { return v; }
            if Instant::now() > end { let tail: Vec<String> = self.log.lock().unwrap().iter().rev().take(12).map(|v| v.to_string().chars().take(200).collect()).collect(); panic!("não chegou: {what}\núltimos eventos: {tail:#?}"); }
            tokio::time::sleep(Duration::from_millis(50)).await;
        }
    }
    fn count(&self, f: impl Fn(&Value) -> bool) -> usize { self.log.lock().unwrap().iter().filter(|v| f(v)).count() }
}

async fn app(tag: &str, rt_port: u16, name: &str) -> Ui {
    let dir = std::env::temp_dir().join(format!("briks-dm-{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir); std::fs::create_dir_all(&dir).unwrap();
    let (l, port) = crate::ipc::bind().await.unwrap();
    let cfg = crate::ipc::Config { data_dir: dir, realtime_url: Some(format!("ws://127.0.0.1:{rt_port}")), realtime_key: Some("chave-de-teste".into()) };
    tokio::spawn(crate::ipc::serve(l, "segredo".into(), cfg));
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
    ui
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn dm_reaches_a_friend_with_no_shared_room_through_the_inbox_knock() {
    let port = crate::mock::start(0).await.unwrap();
    let (mut a, mut b) = (app("a", port, "Ana").await, app("b", port, "Beto").await);
    let (ua, ub) = ("11111111-aaaa-4aaa-8aaa-000000000001", "22222222-bbbb-4bbb-8bbb-000000000002");

    // A escreve primeiro: abre o canal direto, toca na caixa de B e deixa a mensagem na caixa de saída
    a.send(json!({ "cmd": "dm.open", "fp": b.fp, "user_id": ub })).await;
    a.send(json!({ "cmd": "chat.send", "to_fp": b.fp, "id": "m-ana-0001", "text": "oi, sem sala nenhuma ✓" })).await;
    a.wait(10, "dm.state de A", |v| v["ev"] == "dm.state" && v["fp"] == b.fp.as_str()).await;
    tokio::time::sleep(Duration::from_millis(800)).await;
    assert_eq!(b.count(|v| v["ev"] == "chat.message"), 0, "B ainda nem observa a caixa");

    // B abre o Briks: a caixa mostra o toque de A (que continua lá), a UI de B confere e abre a outra ponta
    b.send(json!({ "cmd": "dm.inbox", "user_id": ub })).await;
    let wake = b.wait(15, "dm.wake em B", |v| v["ev"] == "dm.wake").await;
    assert_eq!(wake["fp"], a.fp.as_str(), "o toque carrega só a impressão digital de A");
    b.send(json!({ "cmd": "dm.open", "fp": a.fp })).await;

    let m = b.wait(40, "mensagem de A em B", |v| v["ev"] == "chat.message" && v["content"] == "oi, sem sala nenhuma ✓").await;
    assert_eq!((m["from_fp"].as_str(), m["author"].as_str(), m["id"].as_str(), m["via"].as_str()), (Some(a.fp.as_str()), Some("Ana"), Some("m-ana-0001"), Some("dm")));
    a.wait(15, "chat.sent de A", |v| v["ev"] == "chat.sent" && v["id"] == "m-ana-0001").await;
    a.wait(15, "chat.ack (entregue) de A", |v| v["ev"] == "chat.ack" && v["id"] == "m-ana-0001").await;
    assert_eq!(a.count(|v| v["ev"] == "chat.sent" && v["id"] == "m-ana-0001"), 1, "o aviso de 'saiu' vem uma vez só");

    // resposta de B pelo mesmo canal direto
    b.send(json!({ "cmd": "chat.send", "to_fp": a.fp, "id": "m-beto-0001", "text": "oi Ana" })).await;
    let r = a.wait(20, "resposta de B em A", |v| v["ev"] == "chat.message" && v["content"] == "oi Ana").await;
    assert_eq!(r["from_fp"], b.fp.as_str());
    let _ = ua;

    // quem não é amigo (impressão digital qualquer) não consegue abrir nada: valores inválidos são ignorados
    b.send(json!({ "cmd": "dm.open", "fp": "../../x" })).await;
    b.send(json!({ "cmd": "dm.close", "fp": a.fp })).await;
}

/// Fila no aparelho pelo IPC: A escreve SEM canal aberto (nem sala, nem dm.open): a mensagem não se perde, aparece como pendente em
/// `outbox.state`; quando o canal abre e B está online, B recebe UMA vez e A vê "entregue" (a pendência some, o id vira entregue).
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn message_written_with_no_open_link_is_kept_pending_then_delivered_once() {
    let port = crate::mock::start(0).await.unwrap();
    let (mut a, mut b) = (app("fa", port, "Ana").await, app("fb", port, "Beto").await);
    let ub = "22222222-bbbb-4bbb-8bbb-000000000002";
    let id = format!("fila-ipc-{}", std::process::id());
    a.send(json!({ "cmd": "chat.send", "to_fp": b.fp, "id": id, "text": "escrita com o amigo fora" })).await;
    tokio::time::sleep(Duration::from_millis(400)).await;
    a.send(json!({ "cmd": "outbox.get" })).await;
    let st = a.wait(10, "outbox.state com a pendência", |v| v["ev"] == "outbox.state" && v["pending"].as_array().is_some_and(|p| p.iter().any(|x| x["id"] == id.as_str() && x["to_fp"] == b.fp.as_str()))).await;
    assert!(st["delivered"].as_array().unwrap().iter().all(|x| x != id.as_str()));
    // o amigo fica online: a UI de A reabre o canal (dm.open com toque) e B observa a caixa
    b.send(json!({ "cmd": "dm.inbox", "user_id": ub })).await;
    tokio::time::sleep(Duration::from_millis(600)).await;
    a.send(json!({ "cmd": "dm.open", "fp": b.fp, "user_id": ub })).await;
    let wake = b.wait(15, "dm.wake em B", |v| v["ev"] == "dm.wake").await;
    b.send(json!({ "cmd": "dm.open", "fp": wake["fp"] })).await;
    b.wait(40, "mensagem pendente em B", |v| v["ev"] == "chat.message" && v["id"] == id.as_str()).await;
    a.wait(20, "chat.ack em A", |v| v["ev"] == "chat.ack" && v["id"] == id.as_str()).await;
    tokio::time::sleep(Duration::from_secs(6)).await;   // dá tempo a reenvios: não pode duplicar
    assert_eq!(b.count(|v| v["ev"] == "chat.message" && v["id"] == id.as_str()), 1, "chegou uma vez só");
    a.send(json!({ "cmd": "outbox.get" })).await;
    let end = a.wait(10, "outbox.state final", |v| v["ev"] == "outbox.state" && v["delivered"].as_array().is_some_and(|d| d.iter().any(|x| x == id.as_str()))).await;
    assert!(end["pending"].as_array().unwrap().iter().all(|x| x["id"] != id.as_str()), "a pendência saiu");
}

/// Convite de grupo real (~32 KB) pela DM: o pacote viaja como campo ESTRUTURADO (`package` em chat.send -> `chat.invite`), não como texto da
/// mensagem (limite de 4000 caracteres), e chega idêntico byte a byte, uma vez só, com o texto curto assinado em `chat.message`.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_32kb_group_invite_round_trips_byte_for_byte_as_a_structured_field() {
    use base64::{engine::general_purpose::URL_SAFE_NO_PAD as B64, Engine};
    let port = crate::mock::start(0).await.unwrap();
    let (mut a, mut b) = (app("ga", port, "Ana").await, app("gb", port, "Beto").await);
    let ub = "22222222-bbbb-4bbb-8bbb-000000000002";
    // pacote do tamanho do relato (~32 KB), com todos os bytes possíveis (base64url) e emojis/quebras no meio do JSON
    let raw: Vec<u8> = (0..24_000u32).map(|i| (i.wrapping_mul(2654435761) >> 13) as u8).collect();
    let package = format!("briks-grp1.{}", B64.encode(&raw));
    assert!(package.len() > 32_000 && package.len() < crate::identity::MAX_PACKAGE_BYTES);
    let id = format!("convite-{}", std::process::id());
    let short = "briks-invite:1|grp-x|Noite de jogos|Ana";
    a.send(json!({ "cmd": "dm.open", "fp": b.fp, "user_id": ub })).await;
    a.send(json!({ "cmd": "chat.send", "to_fp": b.fp, "id": id, "text": short, "package": package })).await;
    b.send(json!({ "cmd": "dm.inbox", "user_id": ub })).await;
    let wake = b.wait(15, "dm.wake em B", |v| v["ev"] == "dm.wake").await;
    b.send(json!({ "cmd": "dm.open", "fp": wake["fp"] })).await;
    let m = b.wait(40, "texto curto em B", |v| v["ev"] == "chat.message" && v["id"] == id.as_str()).await;
    assert_eq!(m["content"], short, "o texto da mensagem é só o cartão curto");
    let inv = b.wait(20, "chat.invite em B", |v| v["ev"] == "chat.invite" && v["id"] == id.as_str()).await;
    assert_eq!(inv["package"].as_str().unwrap().as_bytes(), package.as_bytes(), "o pacote chegou idêntico, byte a byte");
    assert_eq!(inv["from_fp"], a.fp.as_str());
    a.wait(20, "chat.ack em A", |v| v["ev"] == "chat.ack" && v["id"] == id.as_str()).await;
    tokio::time::sleep(Duration::from_secs(5)).await;
    assert_eq!(b.count(|v| v["ev"] == "chat.invite" && v["id"] == id.as_str()), 1, "uma vez só");
}
