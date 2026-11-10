//! Testes de integração em loopback: servidor Phoenix simulado + duas malhas WebRTC reais (webrtc-rs) na mesma máquina.
use std::sync::Arc;
use std::time::Duration;

use serde_json::json;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::time::timeout;

use crate::identity::Identity;
use crate::mesh::{self, MeshCfg, MeshCmd, MeshEvent, MeshHandle};
use crate::realtime::{self, RealtimeCfg};
use crate::signaling::{secure_room_id, topic_name};

const PA: &str = "aaaaaaaa-1111-4111-8111-000000000001";
const PB: &str = "bbbbbbbb-2222-4222-8222-000000000002";

fn node(port: u16, peer: &str, name: &str, room: &str, id: Arc<Identity>) -> MeshHandle {
    let mut cfg = RealtimeCfg::new(&format!("ws://127.0.0.1:{port}"), "chave-de-teste");
    cfg.leave_grace = Duration::from_millis(200);
    let topic = topic_name("room", &secure_room_id("room", room));
    let (rt, rx) = realtime::connect(cfg, topic, peer.into(), json!({ "peerId": peer, "joinedAt": 1, "clientVersion": "teste" }));
    mesh::spawn(MeshCfg { namespace: "room".into(), room: room.into(), local_peer: peer.into(), display_name: name.into(), ice_servers: vec![] }, id, rt, rx)
}

async fn wait_for<F: FnMut(&MeshEvent) -> bool>(h: &mut MeshHandle, secs: u64, mut f: F) -> Option<MeshEvent> {
    timeout(Duration::from_secs(secs), async {
        while let Some(e) = h.events.recv().await {
            if f(&e) { return Some(e); }
        }
        None
    })
    .await
    .ok()
    .flatten()
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn two_peers_authenticate_and_exchange_signed_chat() {
    let port = crate::mock::start(0).await.unwrap();
    let (ia, ib) = (Arc::new(Identity::generate()), Arc::new(Identity::generate()));
    let mut a = node(port, PA, "Ana", "sala-teste", ia.clone());
    let mut b = node(port, PB, "Beto", "sala-teste", ib.clone());

    // ambos chegam a "autenticado" (DataChannel aberto + prova ECDSA nos dois sentidos)
    assert!(wait_for(&mut a, 30, |e| matches!(e, MeshEvent::Peer { state, .. } if state == "autenticado")).await.is_some(), "A não autenticou");
    assert!(wait_for(&mut b, 30, |e| matches!(e, MeshEvent::Peer { state, .. } if state == "autenticado")).await.is_some(), "B não autenticou");

    a.cmd.send(MeshCmd::Chat { to_fp: ib.fingerprint(), text: "oi Beto, açaí ✓".into() }).await.unwrap();
    let got = wait_for(&mut b, 15, |e| matches!(e, MeshEvent::Chat { .. })).await.expect("B não recebeu a mensagem");
    if let MeshEvent::Chat { content, author, from_fp, .. } = got {
        assert_eq!(content, "oi Beto, açaí ✓");
        assert_eq!(author, "Ana");
        assert_eq!(from_fp, ia.fingerprint());
    }
    b.cmd.send(MeshCmd::Chat { to_fp: ia.fingerprint(), text: "oi Ana".into() }).await.unwrap();
    assert!(wait_for(&mut a, 15, |e| matches!(e, MeshEvent::Chat { content, .. } if content == "oi Ana")).await.is_some(), "A não recebeu a resposta");
    let _ = a.cmd.send(MeshCmd::Close).await;
    let _ = b.cmd.send(MeshCmd::Close).await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn simultaneous_offers_still_converge() {
    let port = crate::mock::start(0).await.unwrap();
    let (ia, ib) = (Arc::new(Identity::generate()), Arc::new(Identity::generate()));
    let mut a = node(port, PA, "Ana", "sala-colisao", ia.clone());
    let mut b = node(port, PB, "Beto", "sala-colisao", ib.clone());
    assert!(wait_for(&mut a, 30, |e| matches!(e, MeshEvent::Peer { state, .. } if state == "autenticado")).await.is_some());
    assert!(wait_for(&mut b, 30, |e| matches!(e, MeshEvent::Peer { state, .. } if state == "autenticado")).await.is_some());
    // renegociação cruzada: os dois oferecem ao mesmo tempo
    a.cmd.send(MeshCmd::ForceOffer { peer_id: PB.into() }).await.unwrap();
    b.cmd.send(MeshCmd::ForceOffer { peer_id: PA.into() }).await.unwrap();
    tokio::time::sleep(Duration::from_secs(3)).await;
    // depois da colisão o canal de chat ainda funciona (possivelmente numa conexão nova)
    let mut ok = false;
    for _ in 0..6 {
        a.cmd.send(MeshCmd::Chat { to_fp: ib.fingerprint(), text: "depois da colisão".into() }).await.unwrap();
        if wait_for(&mut b, 8, |e| matches!(e, MeshEvent::Chat { content, .. } if content == "depois da colisão")).await.is_some() { ok = true; break; }
    }
    assert!(ok, "a conversa não se recuperou da colisão de offers");
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn outbox_waits_for_peer_then_delivers() {
    let port = crate::mock::start(0).await.unwrap();
    let (ia, ib) = (Arc::new(Identity::generate()), Arc::new(Identity::generate()));
    let a = node(port, PA, "Ana", "sala-outbox", ia.clone());
    // A manda ANTES de B existir: a mensagem fica na caixa de saída
    a.cmd.send(MeshCmd::Chat { to_fp: ib.fingerprint(), text: "mensagem antiga".into() }).await.unwrap();
    tokio::time::sleep(Duration::from_millis(500)).await;
    let mut b = node(port, PB, "Beto", "sala-outbox", ib.clone());
    let got = wait_for(&mut b, 30, |e| matches!(e, MeshEvent::Chat { .. })).await;
    assert!(matches!(got, Some(MeshEvent::Chat { content, .. }) if content == "mensagem antiga"), "a outbox não entregou");
}

async fn ipc_line(rd: &mut tokio::io::Lines<BufReader<tokio::net::tcp::OwnedReadHalf>>) -> serde_json::Value {
    let l = timeout(Duration::from_secs(10), rd.next_line()).await.expect("tempo").expect("io").expect("fim");
    serde_json::from_str(&l).unwrap()
}

#[tokio::test]
async fn ipc_requires_token_and_reports_missing_config() {
    let dir = std::env::temp_dir().join(format!("briks-core-test-{}", std::process::id()));
    let (l, port) = crate::ipc::bind().await.unwrap();
    let cfg = crate::ipc::Config { data_dir: dir.clone(), realtime_url: None, realtime_key: None };
    tokio::spawn(crate::ipc::serve(l, "segredo".into(), cfg));

    // token errado: recusado
    let s = tokio::net::TcpStream::connect(("127.0.0.1", port)).await.unwrap();
    let (rd, mut wr) = s.into_split();
    let mut lines = BufReader::new(rd).lines();
    wr.write_all(b"{\"cmd\":\"hello\",\"token\":\"errado\"}\n").await.unwrap();
    let r = ipc_line(&mut lines).await;
    assert_eq!(r["ev"], "error");

    // token certo: sessão, identidade estável entre execuções, e sala sem configuração do Supabase = erro visível
    let s = tokio::net::TcpStream::connect(("127.0.0.1", port)).await.unwrap();
    let (rd, mut wr) = s.into_split();
    let mut lines = BufReader::new(rd).lines();
    wr.write_all(b"{\"cmd\":\"hello\",\"token\":\"segredo\"}\n").await.unwrap();
    assert_eq!(ipc_line(&mut lines).await["ev"], "hello.ok");
    wr.write_all(b"{\"cmd\":\"session.start\",\"display_name\":\"Teste\"}\n").await.unwrap();
    let ready = ipc_line(&mut lines).await;
    assert_eq!(ready["ev"], "session.ready");
    let fp = ready["fingerprint"].as_str().unwrap().to_string();
    assert_eq!(fp.len(), 64);
    wr.write_all(b"{\"cmd\":\"room.join\",\"room\":\"x\"}\n").await.unwrap();
    let st = ipc_line(&mut lines).await;
    assert_eq!(st["ev"], "signaling.status");
    assert_eq!(st["state"], "erro");
    assert!(st["message"].as_str().unwrap().contains("não configurado"));
    // a identidade persiste no disco
    let again = crate::ipc::load_or_create_identity(&dir).unwrap();
    assert_eq!(again.fingerprint(), fp);
    let _ = std::fs::remove_dir_all(dir);
}

#[tokio::test]
async fn realtime_reports_error_when_server_is_down() {
    // porta fechada: o cliente informa o estado e não derruba nada
    let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let port = l.local_addr().unwrap().port();
    drop(l);
    let cfg = RealtimeCfg::new(&format!("ws://127.0.0.1:{port}"), "k");
    let (rt, mut rx) = realtime::connect(cfg, "realtime:briks:room:x".into(), PA.into(), json!({}));
    let mut saw_reconnecting = false;
    let _ = timeout(Duration::from_secs(5), async {
        while let Some(e) = rx.recv().await {
            if matches!(e, realtime::RtEvent::Status(ref s) if s == "reconnecting") { saw_reconnecting = true; break; }
        }
    })
    .await;
    assert!(saw_reconnecting, "deveria tentar reconectar");
    rt.close().await;
}
