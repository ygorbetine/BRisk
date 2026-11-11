//! Robustez da transmissão com fontes sintéticas (sem tela, sem GPU): sinalização malformada, emissor que cai e volta, espectador que
//! entra e sai sem parar, queda do canal de sinalização e ciclos repetidos de iniciar/parar com medida de handles/threads/memória.
//! Para rodar sem NVENC e sem decodificador de GPU (a máquina já teve tela azul com NVENC):
//!   BRIKS_ENCODER=openh264 BRIKS_DECODER=openh264 cargo test --release robust:: -- --test-threads=2
//! Os testes de ciclo (medida de vazamento) são `#[ignore]` porque medem o processo inteiro e precisam rodar sozinhos:
//!   BRIKS_ENCODER=openh264 BRIKS_DECODER=openh264 cargo test --release -- --ignored --nocapture --test-threads=1 robust::cycles
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use futures_util::{SinkExt, StreamExt};
use serde_json::{json, Value};
use tokio::net::{TcpListener, TcpStream};
use tokio::time::timeout;
use tokio_tungstenite::tungstenite::Message;

use crate::capture::SourceKind;
use crate::identity::Identity;
use crate::mesh::{self, MeshCfg, MeshCmd, MeshEvent, MeshHandle};
use crate::phoenix::{self, Frame};
use crate::procaudio::AudioMode;
use crate::realtime::{self, RealtimeCfg};
use crate::share::ShareCfg;
use crate::signaling::{secure_room_id, topic_name};

fn pid(n: u32) -> String { format!("{n:08x}-1111-4111-8111-{n:012x}") }

fn node_at(url: &str, peer: &str, name: &str, room: &str, id: Arc<Identity>, grace_ms: u64) -> MeshHandle {
    let mut cfg = RealtimeCfg::new(url, "chave-de-teste");
    cfg.leave_grace = Duration::from_millis(grace_ms);
    let topic = topic_name("room", &secure_room_id("room", room));
    let (rt, rx) = realtime::connect(cfg, topic, peer.into(), json!({ "peerId": peer, "joinedAt": 1, "clientVersion": "teste" }));
    mesh::spawn(MeshCfg { namespace: "room".into(), room: room.into(), local_peer: peer.into(), display_name: name.into(), ice_servers: vec![], audio: None, owner_fp: None, joined_at: 1 }, id, rt, rx)
}
fn node(port: u16, peer: &str, name: &str, room: &str, id: Arc<Identity>) -> MeshHandle { node_at(&format!("ws://127.0.0.1:{port}"), peer, name, room, id, 200) }

/// Espera um evento que satisfaça `f`; os outros são descartados (e o canal de eventos nunca enche, senão o ator trava).
async fn wait_ev(h: &mut MeshHandle, secs: u64, mut f: impl FnMut(&MeshEvent) -> bool) -> Option<MeshEvent> {
    let end = Instant::now() + Duration::from_secs(secs);
    loop {
        match timeout(end.saturating_duration_since(Instant::now()), h.events.recv()).await { Ok(Some(e)) => { if f(&e) { return Some(e); } } _ => return None }
    }
}
async fn wait_share(h: &mut MeshHandle, secs: u64, f: impl Fn(&Value) -> bool) -> Option<Value> {
    match wait_ev(h, secs, |e| matches!(e, MeshEvent::Share(v) if f(v))).await { Some(MeshEvent::Share(v)) => Some(v), _ => None }
}
async fn authed(h: &mut MeshHandle, n: usize, secs: u64) -> bool {
    let mut got = std::collections::HashSet::new();
    wait_ev(h, secs, |e| { if let MeshEvent::Peer { peer_id, state } = e { if state == "autenticado" { got.insert(peer_id.clone()); } } got.len() >= n }).await.is_some()
}
fn drain(h: &mut MeshHandle) { while h.events.try_recv().is_ok() {} }

fn cfg(kind: SourceKind, label: &str, audio: AudioMode) -> ShareCfg {
    ShareCfg { kind, height: 360, fps: 15, kbps: 0, label: label.into(), audio, content: crate::adapt::Content::Detail, adaptive: false }
}
async fn start_share(h: &MeshHandle, label: &str) { h.cmd.send(MeshCmd::ShareStart { cfg: cfg(SourceKind::Test, label, AudioMode::None), preview: None, dump: None }).await.unwrap(); }

/// (memória de trabalho MB, memória privada MB, handles, threads) do processo de teste.
fn proc_stats() -> (f64, f64, u32, usize) {
    use windows::Win32::Foundation::CloseHandle;
    use windows::Win32::System::Diagnostics::ToolHelp::{CreateToolhelp32Snapshot, Thread32First, Thread32Next, TH32CS_SNAPTHREAD, THREADENTRY32};
    use windows::Win32::System::ProcessStatus::{GetProcessMemoryInfo, PROCESS_MEMORY_COUNTERS};
    use windows::Win32::System::Threading::{GetCurrentProcess, GetProcessHandleCount};
    unsafe {
        let mut m = PROCESS_MEMORY_COUNTERS::default();
        let _ = GetProcessMemoryInfo(GetCurrentProcess(), &mut m, std::mem::size_of::<PROCESS_MEMORY_COUNTERS>() as u32);
        let mut h = 0u32; let _ = GetProcessHandleCount(GetCurrentProcess(), &mut h);
        let mut threads = 0usize;
        if let Ok(snap) = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0) {
            let mut e = THREADENTRY32 { dwSize: std::mem::size_of::<THREADENTRY32>() as u32, ..Default::default() };
            let me = std::process::id();
            if Thread32First(snap, &mut e).is_ok() { loop { if e.th32OwnerProcessID == me { threads += 1; } if Thread32Next(snap, &mut e).is_err() { break; } } }
            let _ = CloseHandle(snap);
        }
        (m.WorkingSetSize as f64 / 1048576.0, m.PagefileUsage as f64 / 1048576.0, h, threads)
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 1. Sinalização malformada / hostil
// ---------------------------------------------------------------------------------------------------------------------
/// Um cliente WebSocket cru que entra na sala como mais um peer (Presence) e despeja lixo no Broadcast.
struct Raw { tx: futures_util::stream::SplitSink<tokio_tungstenite::WebSocketStream<tokio_tungstenite::MaybeTlsStream<TcpStream>>, Message>, topic: String, n: u64 }
impl Raw {
    async fn join(port: u16, room: &str, peer: &str) -> Raw {
        let (ws, _) = tokio_tungstenite::connect_async(format!("ws://127.0.0.1:{port}/realtime/v1/websocket?apikey=k&vsn=1.0.0")).await.unwrap();
        let (mut tx, mut rx) = ws.split();
        tokio::spawn(async move { while let Some(Ok(_)) = rx.next().await {} });   // drena o que o servidor manda
        let topic = topic_name("room", &secure_room_id("room", room));
        let j = Frame { topic: topic.clone(), event: "phx_join".into(), payload: phoenix::join_payload(peer, "k"), msg_ref: Some("1".into()), join_ref: Some("1".into()) };
        tx.send(Message::Text(phoenix::encode(&j))).await.unwrap();
        tokio::time::sleep(Duration::from_millis(300)).await;
        tx.send(Message::Text(phoenix::encode(&phoenix::presence_track(&topic, json!({ "peerId": peer, "joinedAt": 2, "clientVersion": "lixo" }), 2, "1")))).await.unwrap();
        Raw { tx, topic, n: 3 }
    }
    async fn bcast(&mut self, event: &str, payload: Value) {
        self.n += 1;
        let _ = self.tx.send(Message::Text(phoenix::encode(&phoenix::broadcast(&self.topic, event, payload, self.n, "1")))).await;
    }
    async fn text(&mut self, t: &str) { let _ = self.tx.send(Message::Text(t.into())).await; }
}

fn env(room: &str, from: &str, to: &str, kind: &str, payload: Value, ts: i64, n: u32) -> Value {
    json!({ "version": 1, "roomId": secure_room_id("room", room), "fromPeerId": from, "targetPeerId": to, "messageId": format!("deadbeef-0000-4000-8000-{n:012}"), "timestamp": ts, "type": kind, "payload": payload })
}
fn now_ms() -> i64 { std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_millis() as i64 }

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn malformed_and_spoofed_signaling_does_not_break_a_live_share() {
    let port = crate::mock::start(0).await.unwrap();
    let (ia, ib) = (Arc::new(Identity::generate()), Arc::new(Identity::generate()));
    let (fa, fb) = (ia.fingerprint(), ib.fingerprint());
    let (pa, pb, px) = (pid(0xa1), pid(0xb1), pid(0xc1));
    let mut a = node(port, &pa, "Ana", "lixo1", ia);
    let mut b = node(port, &pb, "Beto", "lixo1", ib);
    assert!(authed(&mut a, 1, 20).await && authed(&mut b, 1, 20).await, "não autenticou");
    start_share(&a, "viva").await;
    wait_share(&mut b, 20, |v| v["ev"] == "share.remote" && v["state"] == "on").await.expect("on");
    let dir = std::env::temp_dir().join(format!("briks-robust-lixo-{}", std::process::id())); std::fs::create_dir_all(&dir).unwrap();
    b.cmd.send(MeshCmd::ShareWatch { key: pa.clone(), on: true, path: dir.join("b.bin") }).await.unwrap();
    wait_share(&mut b, 25, |v| v["ev"] == "share.remote" && v["state"] == "frame").await.expect("quadro antes do ataque");

    let mut x = Raw::join(port, "lixo1", &px).await;
    let big = "x".repeat(70_000);
    let ts = now_ms();
    let garbage_sdp = |t: &str, s: &str| json!({ "sdp": { "type": t, "sdp": s } });
    let mut n = 0u32;
    let mut nx = || { n += 1; n };
    // 1) corpo que nem é objeto, nem JSON do Phoenix
    for p in [json!(null), json!("texto"), json!(42), json!([1, 2, 3]), json!({}), json!({ "version": 1 }), json!(true)] { x.bcast("webrtc.offer", p).await; }
    x.text("isto não é JSON").await; x.text("{\"topic\":5}").await; x.text("[]").await; x.text("").await;
    // 2) números extremos e tipos trocados (a validação do timestamp estourava com i64::MIN)
    for t in [i64::MIN, i64::MAX, 0, -1] { x.bcast("webrtc.offer", env("lixo1", &px, &pb, "webrtc.offer", garbage_sdp("offer", "v=0"), t, nx())).await; }
    x.bcast("webrtc.offer", json!({ "version": u32::MAX, "roomId": 1, "fromPeerId": 2, "timestamp": 1e300, "type": ["x"], "payload": 7 })).await;
    // 3) SDP inválido de um peer presente, de todos os tipos, e tamanho acima do limite
    for t in ["offer", "answer", "pranswer", "rollback", "inexistente"] {
        for s in ["", "lixo", "v=0\r\nm=video 0 RTP/AVP 0\r\n", "\u{0}\u{1}\u{2}"] { x.bcast("webrtc.offer", env("lixo1", &px, &pb, "webrtc.offer", garbage_sdp(t, s), ts, nx())).await; x.bcast("webrtc.answer", env("lixo1", &px, &pa, "webrtc.answer", garbage_sdp(t, s), ts, nx())).await; }
    }
    x.bcast("webrtc.offer", env("lixo1", &px, &pb, "webrtc.offer", garbage_sdp("offer", &big), ts, nx())).await;
    // 4) candidatos ICE inválidos
    for c in [json!("x"), json!({}), json!({ "candidate": 5 }), json!({ "candidate": "candidate:lixo", "sdpMid": null }), json!({ "candidate": "candidate:1 1 udp 1 999.999.0.1 99999999 typ host", "sdpMLineIndex": 70000 })] {
        x.bcast("webrtc.ice-candidate", env("lixo1", &px, &pb, "webrtc.ice-candidate", json!({ "candidate": c }), ts, nx())).await;
    }
    // 5) FALSIFICA o remetente: oferta/resposta/ICE "de A" para B com lixo, no meio de uma conexão viva
    x.bcast("webrtc.offer", env("lixo1", &pa, &pb, "webrtc.offer", garbage_sdp("offer", "lixo"), ts, nx())).await;
    x.bcast("webrtc.answer", env("lixo1", &pa, &pb, "webrtc.answer", garbage_sdp("answer", "lixo"), ts, nx())).await;
    x.bcast("webrtc.ice-candidate", env("lixo1", &pa, &pb, "webrtc.ice-candidate", json!({ "candidate": { "candidate": "candidate:lixo" } }), ts, nx())).await;
    // 6) inundação: mais que o limite de 240 por minuto por (peer, tipo) e mensagens repetidas
    for _ in 0..400 { x.bcast("webrtc.ice-candidate", env("lixo1", &px, &pb, "webrtc.ice-candidate", json!({ "candidate": { "candidate": "candidate:lixo" } }), ts, nx())).await; }
    let dup = env("lixo1", &px, &pb, "webrtc.offer", garbage_sdp("offer", "lixo"), ts, 999_999);
    for _ in 0..50 { x.bcast("webrtc.offer", dup.clone()).await; }
    tokio::time::sleep(Duration::from_secs(2)).await;
    drain(&mut a); drain(&mut b);

    // a conexão viva sobreviveu: o vídeo continua chegando e o chat assinado continua entregando nos dois sentidos
    let n0 = crate::video::FrameSource::open(&dir.join("b.bin")).unwrap().read().map(|r| r.2).unwrap_or(0);
    a.cmd.send(MeshCmd::Chat { to_fp: fb.clone(), text: "depois do ataque".into() }).await.unwrap();
    b.cmd.send(MeshCmd::Chat { to_fp: fa.clone(), text: "resposta".into() }).await.unwrap();
    let got_b = wait_ev(&mut b, 15, |e| matches!(e, MeshEvent::Chat { content, .. } if content == "depois do ataque")).await.is_some();
    let got_a = wait_ev(&mut a, 15, |e| matches!(e, MeshEvent::Chat { content, .. } if content == "resposta")).await.is_some();
    tokio::time::sleep(Duration::from_secs(2)).await;
    let n1 = crate::video::FrameSource::open(&dir.join("b.bin")).unwrap().read().map(|r| r.2).unwrap_or(0);
    eprintln!("depois do ataque: chat A→B {got_b}, B→A {got_a}, quadros novos em ~2 s: {}", n1 - n0);
    assert!(got_b && got_a, "chat parou depois da sinalização malformada");
    assert!(n1 > n0 + 5, "o vídeo parou depois da sinalização malformada: {n0} → {n1}");
    // e os atores ainda respondem a comandos
    a.cmd.send(MeshCmd::ShareStop).await.unwrap();
    wait_share(&mut b, 15, |v| v["ev"] == "share.remote" && v["state"] == "off").await.expect("B não soube que parou: ator travado?");
    let _ = a.cmd.send(MeshCmd::Close).await; let _ = b.cmd.send(MeshCmd::Close).await;
}

// ---------------------------------------------------------------------------------------------------------------------
// 2. Emissor que cai e reconecta; espectador que entra e sai sem parar
// ---------------------------------------------------------------------------------------------------------------------
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn sender_crashes_and_comes_back_three_times() {
    let port = crate::mock::start(0).await.unwrap();
    let ida = Arc::new(Identity::generate());
    let mut b = node(port, &pid(0xb2), "Beto", "cai1", Arc::new(Identity::generate()));
    let dir = std::env::temp_dir().join(format!("briks-robust-cai-{}", std::process::id())); std::fs::create_dir_all(&dir).unwrap();
    for round in 0..3u32 {
        let pa = pid(0xa200 + round);   // o app gera um peerId novo a cada entrada na sala
        let mut a = node(port, &pa, "Ana", "cai1", ida.clone());
        assert!(authed(&mut a, 1, 20).await && authed(&mut b, 1, 20).await, "rodada {round}: não autenticou");
        start_share(&a, &format!("rodada {round}")).await;
        let on = wait_share(&mut b, 20, |v| v["ev"] == "share.remote" && v["state"] == "on" && v["peer_id"] == pa.as_str()).await.expect("B não viu a transmissão");
        assert_eq!(on["label"], format!("rodada {round}"));
        let t0 = Instant::now();
        b.cmd.send(MeshCmd::ShareWatch { key: pa.clone(), on: true, path: dir.join(format!("r{round}.bin")) }).await.unwrap();
        wait_share(&mut b, 25, |v| v["ev"] == "share.remote" && v["state"] == "frame").await.expect("nenhum quadro");
        eprintln!("rodada {round}: 1º quadro {} ms depois de assistir", t0.elapsed().as_millis());
        // o emissor "cai": os comandos e a conexão somem sem ShareStop antes
        drop(a);
        let closed = wait_ev(&mut b, 20, |e| matches!(e, MeshEvent::Peer { peer_id, state } if peer_id == &pa && state == "fechado")).await;
        assert!(closed.is_some(), "rodada {round}: B não deu o emissor antigo como saído (fantasma ocupando vaga)");
    }
    let _ = b.cmd.send(MeshCmd::Close).await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn viewer_toggles_watch_and_rejoins_repeatedly() {
    let port = crate::mock::start(0).await.unwrap();
    let mut a = node(port, &pid(0xa3), "Ana", "churn1", Arc::new(Identity::generate()));
    let mut b = node(port, &pid(0xb3), "Beto", "churn1", Arc::new(Identity::generate()));
    assert!(authed(&mut a, 1, 20).await && authed(&mut b, 1, 20).await);
    start_share(&a, "churn").await;
    wait_share(&mut b, 20, |v| v["ev"] == "share.remote" && v["state"] == "on").await.expect("on");
    let dir = std::env::temp_dir().join(format!("briks-robust-churn-{}", std::process::id())); std::fs::create_dir_all(&dir).unwrap();
    // assistir / parar de assistir 15 vezes: cada "assistir" tem de render um quadro (quadro-chave pedido na hora)
    let mut first = vec![];
    for i in 0..15 {
        let t0 = Instant::now();
        b.cmd.send(MeshCmd::ShareWatch { key: pid(0xa3), on: true, path: dir.join("b.bin") }).await.unwrap();
        wait_share(&mut b, 20, |v| v["ev"] == "share.remote" && v["state"] == "frame").await.unwrap_or_else(|| panic!("toggle {i}: sem quadro"));
        first.push(t0.elapsed().as_millis());
        b.cmd.send(MeshCmd::ShareWatch { key: pid(0xa3), on: false, path: dir.join("b.bin") }).await.unwrap();
        tokio::time::sleep(Duration::from_millis(150)).await;
    }
    first.sort();
    eprintln!("assistir/parar x15: 1º quadro p50 {} ms, máx {} ms", first[first.len() / 2], first[first.len() - 1]);
    drop(b);
    // um espectador novo (peerId novo) entra e sai 5 vezes com a transmissão no ar
    let mut firsts = vec![];
    for i in 0..5u32 {
        let pv = pid(0xb300 + i);
        let mut v = node(port, &pv, "Visita", "churn1", Arc::new(Identity::generate()));
        let t0 = Instant::now();
        wait_share(&mut v, 30, |e| e["ev"] == "share.remote" && e["state"] == "on").await.unwrap_or_else(|| panic!("visita {i}: não soube da transmissão"));
        v.cmd.send(MeshCmd::ShareWatch { key: pid(0xa3), on: true, path: dir.join(format!("v{i}.bin")) }).await.unwrap();
        wait_share(&mut v, 30, |e| e["ev"] == "share.remote" && e["state"] == "frame").await.unwrap_or_else(|| panic!("visita {i}: sem quadro"));
        firsts.push(t0.elapsed().as_millis());
        let _ = v.cmd.send(MeshCmd::Close).await;
        wait_ev(&mut a, 15, |e| matches!(e, MeshEvent::Peer { peer_id, state } if peer_id == &pv && state == "fechado")).await.unwrap_or_else(|| panic!("visita {i}: A não deu a saída"));
    }
    eprintln!("espectador novo x5: entrar → 1º quadro {firsts:?} ms");
    // a transmissão seguiu viva para quem sobrou
    assert!(wait_share(&mut a, 5, |e| e["ev"] == "share.stats").await.is_some(), "o emissor parou de reportar");
    let _ = a.cmd.send(MeshCmd::Close).await;
}

// ---------------------------------------------------------------------------------------------------------------------
// 3. Canal de sinalização que cai: a transmissão P2P segue, e quem saiu no intervalo não vira fantasma
// ---------------------------------------------------------------------------------------------------------------------
struct Proxy { port: u16, block: Arc<AtomicBool>, conns: Arc<std::sync::Mutex<Vec<tokio::task::JoinHandle<()>>>> }
impl Proxy {
    async fn start(target: u16) -> Proxy {
        let l = TcpListener::bind(("127.0.0.1", 0)).await.unwrap();
        let (block, conns) = (Arc::new(AtomicBool::new(false)), Arc::new(std::sync::Mutex::new(vec![])));
        let (b2, c2) = (block.clone(), conns.clone());
        let port = l.local_addr().unwrap().port();
        tokio::spawn(async move {
            while let Ok((mut c, _)) = l.accept().await {
                if b2.load(Ordering::Relaxed) { drop(c); continue; }
                let jh = tokio::spawn(async move { if let Ok(mut s) = TcpStream::connect(("127.0.0.1", target)).await { let _ = tokio::io::copy_bidirectional(&mut c, &mut s).await; } });
                c2.lock().unwrap().push(jh);
            }
        });
        Proxy { port, block, conns }
    }
    /// Derruba as conexões abertas (o servidor vê o socket cair, sem phx_leave).
    fn sever(&self) { for j in self.conns.lock().unwrap().drain(..) { j.abort(); } }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn signaling_outage_keeps_the_share_and_does_not_leave_ghost_peers() {
    let port = crate::mock::start(0).await.unwrap();
    let px = Proxy::start(port).await;
    let grace = 4000;   // como em produção: uma queda curta do canal não derruba ninguém
    let (pa, pb, pc) = (pid(0xa4), pid(0xb4), pid(0xc4));
    let mut a = node_at(&format!("ws://127.0.0.1:{port}"), &pa, "Ana", "queda1", Arc::new(Identity::generate()), grace);
    let mut b = node_at(&format!("ws://127.0.0.1:{}", px.port), &pb, "Beto", "queda1", Arc::new(Identity::generate()), grace);
    let mut c = node_at(&format!("ws://127.0.0.1:{port}"), &pc, "Caio", "queda1", Arc::new(Identity::generate()), grace);
    assert!(authed(&mut a, 2, 30).await && authed(&mut b, 2, 30).await && authed(&mut c, 2, 30).await, "não autenticou");
    start_share(&a, "queda").await;
    wait_share(&mut b, 20, |v| v["ev"] == "share.remote" && v["state"] == "on").await.expect("on");
    let dir = std::env::temp_dir().join(format!("briks-robust-queda-{}", std::process::id())); std::fs::create_dir_all(&dir).unwrap();
    b.cmd.send(MeshCmd::ShareWatch { key: pa.clone(), on: true, path: dir.join("b.bin") }).await.unwrap();
    wait_share(&mut b, 25, |v| v["ev"] == "share.remote" && v["state"] == "frame").await.expect("quadro");
    // o canal de B cai e fica fora do ar; enquanto isso C sai da sala
    px.block.store(true, Ordering::Relaxed); px.sever();
    tokio::time::sleep(Duration::from_millis(500)).await;
    let _ = c.cmd.send(MeshCmd::Close).await;
    tokio::time::sleep(Duration::from_millis(1500)).await;
    px.block.store(false, Ordering::Relaxed);   // B volta a conseguir conectar (reconexão com espera exponencial)
    // B tem de dar C como saído (só soube disso no presence_state novo) e seguir recebendo o vídeo de A
    let gone = wait_ev(&mut b, 25, |e| matches!(e, MeshEvent::Peer { peer_id, state } if peer_id == &pc && state == "fechado")).await;
    assert!(gone.is_some(), "B nunca deu C como saído: fantasma depois da reconexão do canal");
    let n0 = crate::video::FrameSource::open(&dir.join("b.bin")).unwrap().read().map(|r| r.2).unwrap_or(0);
    tokio::time::sleep(Duration::from_secs(2)).await;
    let n1 = crate::video::FrameSource::open(&dir.join("b.bin")).unwrap().read().map(|r| r.2).unwrap_or(0);
    eprintln!("depois da queda do canal: {} quadros novos em 2 s", n1 - n0);
    assert!(n1 > n0 + 5, "o vídeo parou depois da queda do canal de sinalização: {n0} → {n1}");
    let _ = a.cmd.send(MeshCmd::Close).await; let _ = b.cmd.send(MeshCmd::Close).await;
}

// ---------------------------------------------------------------------------------------------------------------------
// 4. Ciclos de iniciar/parar: handles, threads e memória (medem o processo todo: rodam sozinhos, `--ignored`)
// ---------------------------------------------------------------------------------------------------------------------
async fn measure_cycles(label: &str, cycles: usize, mk: impl Fn(usize) -> ShareCfg, expect_on: bool) {
    let port = crate::mock::start(0).await.unwrap();
    let mut a = node(port, &pid(0xa5), "Ana", &format!("ciclos-{label}"), Arc::new(Identity::generate()));
    let mut b = node(port, &pid(0xb5), "Beto", &format!("ciclos-{label}"), Arc::new(Identity::generate()));
    assert!(authed(&mut a, 1, 20).await && authed(&mut b, 1, 20).await);
    let dir = std::env::temp_dir().join(format!("briks-robust-ciclos-{}", std::process::id())); std::fs::create_dir_all(&dir).unwrap();
    b.cmd.send(MeshCmd::ShareWatch { key: pid(0xa5), on: true, path: dir.join("b.bin") }).await.unwrap();
    let mut rows: Vec<(usize, f64, f64, u32, usize)> = vec![];
    let t0 = Instant::now();
    let sample = |i: usize, rows: &mut Vec<(usize, f64, f64, u32, usize)>| { let (w, p, h, t) = proc_stats(); rows.push((i, w, p, h, t)); eprintln!("[{label}] ciclo {i:>3}: trabalho {w:6.1} MB  privada {p:6.1} MB  handles {h:4}  threads {t:3}"); };
    sample(0, &mut rows);
    for i in 1..=cycles {
        a.cmd.send(MeshCmd::ShareStart { cfg: mk(i), preview: None, dump: None }).await.unwrap();
        if expect_on { wait_share(&mut a, 20, |v| v["ev"] == "share.local" && v["state"] == "on").await.unwrap_or_else(|| panic!("ciclo {i}: não subiu")); }
        else { wait_share(&mut a, 20, |v| v["ev"] == "share.local" && v["state"] == "off").await.unwrap_or_else(|| panic!("ciclo {i}: sem evento de falha")); }
        a.cmd.send(MeshCmd::ShareStop).await.unwrap();
        if expect_on { wait_share(&mut a, 20, |v| v["ev"] == "share.local" && v["state"] == "off").await.unwrap_or_else(|| panic!("ciclo {i}: a thread do emissor não terminou")); }
        drain(&mut b);
        if i == 10 || i % 50 == 0 || i == cycles { tokio::time::sleep(Duration::from_millis(1500)).await; drain(&mut a); drain(&mut b); sample(i, &mut rows); }
    }
    eprintln!("[{label}] {cycles} ciclos em {:.1} s", t0.elapsed().as_secs_f64());
    if let Ok(out) = std::env::var("BRIKS_ROBUST_OUT") {
        let mut csv = String::from("ciclo,memoria_trabalho_mb,memoria_privada_mb,handles,threads\n");
        for r in &rows { csv += &format!("{},{:.1},{:.1},{},{}\n", r.0, r.1, r.2, r.3, r.4); }
        let _ = std::fs::write(std::path::Path::new(&out).join(format!("ciclos-{label}.csv")), csv);
    }
    // referência = depois do aquecimento (ciclo 10); fim = último
    let (base, last) = (rows.iter().find(|r| r.0 >= 10).unwrap_or(&rows[0]).clone(), rows.last().unwrap().clone());
    assert!(last.3 <= base.3 + 40, "[{label}] handles cresceram: {} → {}", base.3, last.3);
    assert!(last.4 <= base.4 + 8, "[{label}] threads cresceram: {} → {}", base.4, last.4);
    assert!(last.2 <= base.2 + 150.0, "[{label}] memória privada cresceu: {:.0} → {:.0} MB", base.2, last.2);
    let _ = a.cmd.send(MeshCmd::Close).await; let _ = b.cmd.send(MeshCmd::Close).await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
#[ignore]
async fn cycles_start_stop_video_and_audio() {
    let n: usize = std::env::var("BRIKS_CYCLES").ok().and_then(|v| v.parse().ok()).unwrap_or(200);
    // metade dos ciclos com o áudio sintético do processo (thread de áudio + codificador Opus), metade só vídeo
    measure_cycles("normal", n, |i| cfg(SourceKind::Test, "ciclo", if i % 2 == 0 { AudioMode::TestBeep } else { AudioMode::None }), true).await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
#[ignore]
async fn cycles_failed_starts() {
    let n: usize = std::env::var("BRIKS_CYCLES").ok().and_then(|v| v.parse().ok()).unwrap_or(200);
    // janela que não existe: a captura recusa na hora (nada a parar, nada pode vazar)
    measure_cycles("falha", n, |_| cfg(SourceKind::Window(1), "fantasma", AudioMode::None), false).await;
}
