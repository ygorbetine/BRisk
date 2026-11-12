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
