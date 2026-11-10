//! Testes de integração em loopback: servidor Phoenix simulado + duas malhas WebRTC reais (webrtc-rs) na mesma máquina.
use std::sync::Arc;
use std::time::Duration;

use serde_json::json;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::time::timeout;

use crate::audio::Audio;
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
    mesh::spawn(MeshCfg { namespace: "room".into(), room: room.into(), local_peer: peer.into(), display_name: name.into(), ice_servers: vec![], audio: None, owner_fp: None, joined_at: 1 }, id, rt, rx)
}

fn node_audio(port: u16, peer: &str, name: &str, room: &str, id: Arc<Identity>) -> (MeshHandle, Arc<Audio>) {
    let mut cfg = RealtimeCfg::new(&format!("ws://127.0.0.1:{port}"), "chave-de-teste");
    cfg.leave_grace = Duration::from_millis(200);
    let topic = topic_name("room", &secure_room_id("room", room));
    let (rt, rx) = realtime::connect(cfg, topic, peer.into(), json!({ "peerId": peer, "joinedAt": 1, "clientVersion": "teste" }));
    let (au, au_rx) = Audio::new(true); // tom de 440 Hz no lugar do microfone, sem dispositivo
    let h = mesh::spawn(MeshCfg { namespace: "room".into(), room: room.into(), local_peer: peer.into(), display_name: name.into(), ice_servers: vec![], audio: Some((au.clone(), au_rx)), owner_fp: None, joined_at: 1 }, id, rt, rx);
    (h, au)
}

const PC: &str = "cccccccc-3333-4333-8333-000000000003";
const PB2: &str = "bbbbbbbb-2222-4222-8222-0000000000b2";

fn node_owner(port: u16, peer: &str, name: &str, room: &str, id: Arc<Identity>, owner: &str) -> MeshHandle {
    let mut cfg = RealtimeCfg::new(&format!("ws://127.0.0.1:{port}"), "chave-de-teste");
    cfg.leave_grace = Duration::from_millis(200);
    let topic = topic_name("room", &secure_room_id("room", room));
    let (rt, rx) = realtime::connect(cfg, topic, peer.into(), json!({ "peerId": peer, "joinedAt": 1, "clientVersion": "teste" }));
    mesh::spawn(MeshCfg { namespace: "room".into(), room: room.into(), local_peer: peer.into(), display_name: name.into(), ice_servers: vec![], audio: None, owner_fp: Some(owner.into()), joined_at: 1 }, id, rt, rx)
}

async fn authed(h: &mut MeshHandle, n: usize, secs: u64) -> bool {
    let mut got = std::collections::HashSet::new();
    let end = std::time::Instant::now() + Duration::from_secs(secs);
    while got.len() < n {
        let left = end.saturating_duration_since(std::time::Instant::now());
        match timeout(left, h.events.recv()).await { Ok(Some(MeshEvent::Peer { peer_id, state })) if state == "autenticado" => { got.insert(peer_id); } Ok(Some(e)) => { eprintln!("authed: {e:?}"); } r => { eprintln!("authed: fim {r:?}"); return false } }
    }
    true
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn owner_kick_removes_target_bans_it_and_blocks_return_with_three_peers() {
    let port = crate::mock::start(0).await.unwrap();
    let (ia, ib, ic) = (Arc::new(Identity::generate()), Arc::new(Identity::generate()), Arc::new(Identity::generate()));
    let owner = ia.fingerprint();
    let mut a = node_owner(port, PA, "Ana", "sala-kick", ia.clone(), &owner);
    let mut b = node_owner(port, PB, "Beto", "sala-kick", ib.clone(), &owner);
    let mut c = node_owner(port, PC, "Cida", "sala-kick", ic.clone(), &owner);
    assert!(authed(&mut a, 2, 40).await, "A não autenticou os dois");
    assert!(authed(&mut b, 2, 40).await, "B não autenticou os dois");
    assert!(authed(&mut c, 2, 40).await, "C não autenticou os dois");

    // quem não é dono não consegue: Kick de B é recusado localmente
    b.cmd.send(MeshCmd::Kick { fp: ic.fingerprint(), reason: "x".into() }).await.unwrap();
    assert!(wait_for(&mut b, 5, |e| matches!(e, MeshEvent::Error(m) if m.contains("dono"))).await.is_some(), "não-dono deveria receber erro");

    // o dono expulsa B
    a.cmd.send(MeshCmd::Kick { fp: ib.fingerprint(), reason: "spam".into() }).await.unwrap();
    let k = wait_for(&mut b, 10, |e| matches!(e, MeshEvent::Kicked { .. })).await;
    assert!(matches!(k, Some(MeshEvent::Kicked { reason }) if reason == "spam"), "B não foi avisado da expulsão");
    // C fecha a conexão com B e registra o banimento; A também
    assert!(wait_for(&mut c, 10, |e| matches!(e, MeshEvent::Banned { fp } if *fp == ib.fingerprint())).await.is_some(), "C não baniu B");
    assert!(wait_for(&mut a, 10, |e| matches!(e, MeshEvent::Banned { fp } if *fp == ib.fingerprint())).await.is_some(), "A não baniu B");

    // B volta com a mesma identidade e outro peerId: ninguém autentica
    let mut b2 = node_owner(port, PB2, "Beto", "sala-kick", ib.clone(), &owner);
    assert!(!authed(&mut a, 1, 8).await, "A aceitou o banido de volta");
    assert!(!authed(&mut c, 1, 2).await, "C aceitou o banido de volta");
    // do lado do banido a conexão é derrubada pelos outros (ele pode ver "autenticado" por instantes: o dono só sabe quem é ao receber a prova dele)
    assert!(wait_for(&mut b2, 10, |e| matches!(e, MeshEvent::Peer { state, .. } if state == "fechado" || state == "falhou")).await.is_some(), "a conexão do banido não foi derrubada");

    // o dono permite: agora volta
    a.cmd.send(MeshCmd::Allow { fp: ib.fingerprint() }).await.unwrap();
    tokio::time::sleep(Duration::from_millis(500)).await;
    let mut b3 = node_owner(port, "bbbbbbbb-2222-4222-8222-0000000000b3", "Beto", "sala-kick", ib.clone(), &owner);
    assert!(authed(&mut b3, 2, 40).await, "B não voltou depois de permitido");
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn kick_from_non_owner_is_ignored_by_everyone() {
    let port = crate::mock::start(0).await.unwrap();
    let (ia, ib, ic) = (Arc::new(Identity::generate()), Arc::new(Identity::generate()), Arc::new(Identity::generate()));
    let owner = ia.fingerprint();
    let mut a = node_owner(port, PA, "Ana", "sala-kick2", ia.clone(), &owner);
    let mut b = node_owner(port, PB, "Beto", "sala-kick2", ib.clone(), &owner);
    let mut c = node_owner(port, PC, "Cida", "sala-kick2", ic.clone(), &owner);
    assert!(authed(&mut a, 2, 40).await && authed(&mut b, 2, 40).await && authed(&mut c, 2, 40).await, "malha incompleta");
    // B forja a expulsão de C (o comando de teste pula a checagem local de dono)
    b.cmd.send(MeshCmd::KickUnchecked { fp: ic.fingerprint() }).await.unwrap();
    assert!(wait_for(&mut c, 6, |e| matches!(e, MeshEvent::Kicked { .. })).await.is_none(), "C foi expulso por quem não é dono");
    assert!(wait_for(&mut a, 3, |e| matches!(e, MeshEvent::Banned { .. })).await.is_none(), "A baniu por ordem de não-dono");
    // e a conversa A -> C continua funcionando
    a.cmd.send(MeshCmd::Chat { to_fp: ic.fingerprint(), text: "ainda aqui".into() }).await.unwrap();
    assert!(wait_for(&mut c, 15, |e| matches!(e, MeshEvent::Chat { content, .. } if content == "ainda aqui")).await.is_some(), "A não consegue falar com C depois da tentativa");
}

async fn until<F: Fn() -> bool>(secs: u64, f: F) -> bool {
    let end = std::time::Instant::now() + Duration::from_secs(secs);
    while std::time::Instant::now() < end { if f() { return true; } tokio::time::sleep(Duration::from_millis(50)).await; }
    f()
}

fn tone_in(au: &Audio) -> (f32, f32) {
    let mut out = vec![0.0f32; 4800];
    au.mix_pull(&mut out);
    let r = crate::audio::rms(&out);
    let zc = out.windows(2).filter(|w| (w[0] < 0.0) != (w[1] < 0.0)).count() as f32;
    (r, zc / 2.0 / 0.1)
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn audio_tone_travels_between_two_cores_and_mute_deafen_work() {
    let port = crate::mock::start(0).await.unwrap();
    let (ia, ib) = (Arc::new(Identity::generate()), Arc::new(Identity::generate()));
    let (mut a, au_a) = node_audio(port, PA, "Ana", "sala-audio", ia.clone());
    let (mut b, au_b) = node_audio(port, PB, "Beto", "sala-audio", ib.clone());
    assert!(wait_for(&mut a, 30, |e| matches!(e, MeshEvent::Peer { state, .. } if state == "autenticado")).await.is_some(), "A não autenticou");
    assert!(wait_for(&mut b, 30, |e| matches!(e, MeshEvent::Peer { state, .. } if state == "autenticado")).await.is_some(), "B não autenticou");

    // B recebe o tom de A: frames decodificados, ~440 Hz, nível razoável
    assert!(until(15, || au_b.rx_stat(PA).frames > 50).await, "B não recebeu áudio de A");
    let st = au_b.rx_stat(PA);
    assert!((st.freq() - 440.0).abs() < 25.0, "frequência recebida {} Hz", st.freq());
    assert!(st.peak > 0.2 && st.peak < 0.4, "pico {}", st.peak);
    assert!(until(15, || au_a.rx_stat(PB).frames > 50).await, "A não recebeu áudio de B");

    // o mixer toca o tom; depois de um pouco para o jitter buffer encher
    tokio::time::sleep(Duration::from_millis(400)).await;
    let mut got = (0.0, 0.0);
    for _ in 0..10 { got = tone_in(&au_b); if got.0 > 0.05 { break; } tokio::time::sleep(Duration::from_millis(100)).await; }
    assert!(got.0 > 0.05 && (got.1 - 440.0).abs() < 60.0, "mix sem tom: rms {} freq {}", got.0, got.1);

    // fala detectada (evento speaking do peer A em B, e "me" em A)
    assert!(wait_for(&mut b, 5, |e| matches!(e, MeshEvent::Speaking { peer_id, level } if peer_id == PA && *level > 0.0)).await.is_some(), "sem evento speaking");
    assert!(wait_for(&mut a, 5, |e| matches!(e, MeshEvent::Speaking { peer_id, level } if peer_id == "me" && *level > 0.0)).await.is_some(), "sem speaking local");

    // mutar uma pessoa = ganho zero só para ela
    b.cmd.send(MeshCmd::MutePeer { key: PA.into(), muted: true }).await.unwrap();
    tokio::time::sleep(Duration::from_millis(200)).await;
    let _ = tone_in(&au_b);
    assert!(tone_in(&au_b).0 < 0.001, "peer silenciado ainda toca");
    b.cmd.send(MeshCmd::MutePeer { key: PA.into(), muted: false }).await.unwrap();
    tokio::time::sleep(Duration::from_millis(600)).await;
    let _ = tone_in(&au_b);
    assert!(until(5, || tone_in(&au_b).0 > 0.05).await, "peer não voltou ao desmutar");

    // microfone mudo de A: B deixa de receber frames novos e é avisado (peer.state)
    au_a.set_mic_muted(true);
    a.cmd.send(MeshCmd::BroadcastState).await.unwrap();
    let ps = wait_for(&mut b, 5, |e| matches!(e, MeshEvent::PeerState { microphone: false, .. })).await;
    assert!(ps.is_some(), "B não soube que A mutou");
    tokio::time::sleep(Duration::from_millis(200)).await;
    let n1 = au_b.rx_stat(PA).frames;
    tokio::time::sleep(Duration::from_millis(500)).await;
    assert_eq!(au_b.rx_stat(PA).frames, n1, "A mudo mas ainda envia áudio");
    au_a.set_mic_muted(false);
    assert!(until(5, || au_b.rx_stat(PA).frames > n1 + 10).await, "áudio não voltou");

    // ensurdecer implica mic mudo (nada sai) e a saída fica em silêncio
    au_b.set_deaf(true);
    assert!(!au_b.mic_open());
    tokio::time::sleep(Duration::from_millis(300)).await;
    let _ = tone_in(&au_b);
    assert!(tone_in(&au_b).0 < 0.001, "ensurdecido ainda ouve");
    let sent = au_b.sent_frames.load(std::sync::atomic::Ordering::Relaxed);
    tokio::time::sleep(Duration::from_millis(400)).await;
    assert_eq!(au_b.sent_frames.load(std::sync::atomic::Ordering::Relaxed), sent, "ensurdecido ainda transmite");
    au_b.set_deaf(false);
    assert!(au_b.mic_open());

    let _ = a.cmd.send(MeshCmd::Close).await;
    let _ = b.cmd.send(MeshCmd::Close).await;
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

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn status_and_presence_reach_authenticated_peers_and_late_joiners_get_them() {
    let port = crate::mock::start(0).await.unwrap();
    let (ia, ib) = (Arc::new(Identity::generate()), Arc::new(Identity::generate()));
    let mut a = node(port, PA, "Ana", "sala-status", ia.clone());
    let mut b = node(port, PB, "Beto", "sala-status", ib.clone());
    assert!(authed(&mut a, 1, 30).await && authed(&mut b, 1, 30).await, "não autenticaram");
    a.cmd.send(MeshCmd::SetProfile { status: "Volto já".into(), presence: "Jogando Valorant".into() }).await.unwrap();
    let e = wait_for(&mut b, 10, |e| matches!(e, MeshEvent::PeerState { status, .. } if status == "Volto já")).await;
    assert!(matches!(e, Some(MeshEvent::PeerState { presence, .. }) if presence == "Jogando Valorant"), "B não recebeu status/presença de A");
    // limpar = vazio
    a.cmd.send(MeshCmd::SetProfile { status: String::new(), presence: String::new() }).await.unwrap();
    assert!(wait_for(&mut b, 10, |e| matches!(e, MeshEvent::PeerState { status, presence, .. } if status.is_empty() && presence.is_empty())).await.is_some(), "B não viu o status limpo");
}

// ---------------------------------------------------------------------------------------------------------------------
// Transmissão de tela: A transmite (fonte sintética), B assiste; conteúdo, latência, parar e retomar (renegociação)
// ---------------------------------------------------------------------------------------------------------------------
async fn wait_share(h: &mut MeshHandle, pred: impl Fn(&serde_json::Value) -> bool, secs: u64) -> Option<serde_json::Value> {
    let end = std::time::Instant::now() + Duration::from_secs(secs);
    loop {
        let left = end.saturating_duration_since(std::time::Instant::now());
        match timeout(left, h.events.recv()).await { Ok(Some(MeshEvent::Share(v))) => { if pred(&v) { return Some(v); } } Ok(Some(_)) => {} _ => return None }
    }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn screen_share_flows_end_to_end_in_loopback() {
    use crate::capture::{read_stamp, test_pattern, SourceKind};
    use crate::share::ShareCfg;
    use crate::video::{psnr, FrameSource};
    let port = crate::mock::start(0).await.unwrap();
    let (ia, ib) = (Arc::new(Identity::generate()), Arc::new(Identity::generate()));
    let mut a = node(port, PA, "Ana", "tela1", ia);
    let mut b = node(port, PB, "Beto", "tela1", ib);
    assert!(authed(&mut a, 1, 20).await && authed(&mut b, 1, 20).await, "não autenticou");
    let dir = std::env::temp_dir().join(format!("briks-share-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir); std::fs::create_dir_all(&dir).unwrap();
    let (w, h) = (1280usize, 720usize);
    for round in 0..2 {
        let sent_png = dir.join(format!("enviado-{round}.png"));
        a.cmd.send(MeshCmd::ShareStart { cfg: ShareCfg { kind: SourceKind::Test, height: 720, fps: 30, kbps: 0, label: "Teste".into(), audio: crate::procaudio::AudioMode::None }, preview: Some(dir.join("preview.bin")), dump: Some(sent_png.clone()) }).await.unwrap();
        let on = wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "on", 20).await.expect("B não soube da transmissão");
        assert_eq!((on["w"].as_u64(), on["h"].as_u64(), on["fps"].as_u64()), (Some(1280), Some(720), Some(30)), "{on}");
        let rpath = dir.join(format!("recebido-{round}.bin"));
        b.cmd.send(MeshCmd::ShareWatch { key: PA.into(), on: true, path: rpath.clone() }).await.unwrap();
        let fr = wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "frame", 25).await.expect("nenhum quadro chegou ao B");
        assert_eq!((fr["w"].as_u64(), fr["h"].as_u64()), (Some(w as u64), Some(h as u64)));
        // amostra 4 s de quadros: latência (carimbo → gravação no quadro compartilhado) e conteúdo
        let src = FrameSource::open(&rpath).unwrap();
        let (mut last_n, mut lats, mut best, mut shot) = (0u64, vec![], 0f64, None);
        let t0 = std::time::Instant::now();
        while t0.elapsed() < Duration::from_secs(4) {
            if let Some((rw, rh, n, rgba)) = src.read() {
                if n != last_n && rw == w && rh == h {
                    last_n = n;
                    let ts = src.ts() as u32; let st = read_stamp(&rgba, w);
                    lats.push(ts.wrapping_sub(st) as i32);
                    if shot.is_none() || n % 40 == 0 { shot = Some(rgba); }
                }
            }
            tokio::time::sleep(Duration::from_millis(8)).await;
        }
        let rgba = shot.expect("sem quadros");
        // conteúdo: o quadro recebido tem de bater com algum quadro do padrão (a barra anda); procura o melhor
        let mut bgr = rgba.clone(); for p in bgr.chunks_mut(4) { p.swap(0, 2); }
        for t in 0..160 { let mut e = test_pattern(w, h, t); for q in e[..16 * w * 4].chunks_mut(4) { q[0] = 0; } let mut r = bgr.clone(); for q in r[..16 * w * 4].chunks_mut(4) { q[0] = 0; } let p = psnr(&e, &r); if p > best { best = p; } }
        image::RgbaImage::from_raw(w as u32, h as u32, rgba).unwrap().save(dir.join(format!("recebido-{round}.png"))).unwrap();
        lats.sort();
        let (n, p50, p95, avg) = (lats.len(), lats[lats.len() / 2], lats[lats.len() * 95 / 100], lats.iter().sum::<i32>() as f64 / lats.len() as f64);
        eprintln!("round {round}: {n} quadros em 4 s ({:.1} fps), latência captura→quadro p50 {p50} ms p95 {p95} ms média {avg:.1} ms, PSNR conteúdo {best:.1} dB", n as f64 / 4.0);
        assert!(n >= 40, "poucos quadros: {n}");
        assert!(best > 25.0, "conteúdo diferente do enviado: PSNR {best}");
        assert!(sent_png.exists());
        // parar: B é avisado e a transmissão some; depois recomeça na rodada seguinte (renegociação nos dois sentidos)
        a.cmd.send(MeshCmd::ShareStop).await.unwrap();
        wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "off", 15).await.expect("B não soube que parou");
    }
    let _ = a.cmd.send(MeshCmd::Close).await; let _ = b.cmd.send(MeshCmd::Close).await;
    eprintln!("provas em {}", dir.display());
}

/// Medição com a tela real (não roda por padrão): `cargo test --release -- --ignored --nocapture real_screen_share_stats`.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
#[ignore]
async fn real_screen_share_stats() {
    use crate::capture::SourceKind;
    use crate::share::ShareCfg;
    use crate::video::FrameSource;
    let port = crate::mock::start(0).await.unwrap();
    let (ia, ib) = (Arc::new(Identity::generate()), Arc::new(Identity::generate()));
    let mut a = node(port, PA, "Ana", "tela2", ia);
    let mut b = node(port, PB, "Beto", "tela2", ib);
    assert!(authed(&mut a, 1, 20).await && authed(&mut b, 1, 20).await);
    let dir = std::env::temp_dir().join(format!("briks-real-{}", std::process::id()));
    let _ = std::fs::create_dir_all(&dir);
    let hm: isize = crate::capture::list_sources(&dir)["screens"][0]["id"].as_str().unwrap().parse().unwrap();
    let rpath = dir.join("recebido.bin");
    for (height, fps, kbps) in [(720usize, 30u32, 0u32), (1080, 30, 0), (1080, 60, 0), (1440, 30, 0), (2160, 30, 0)] {
        a.cmd.send(MeshCmd::ShareStart { cfg: ShareCfg { kind: SourceKind::Screen(hm), height, fps, kbps, label: "Tela".into(), audio: crate::procaudio::AudioMode::None }, preview: None, dump: None }).await.unwrap();
        wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "on", 20).await.expect("on");
        b.cmd.send(MeshCmd::ShareWatch { key: PA.into(), on: true, path: rpath.clone() }).await.unwrap();
        wait_share(&mut a, |v| v["ev"] == "share.stats", 10).await;   // descarta a 1ª janela (parcial)
        let st = wait_share(&mut a, |v| v["ev"] == "share.stats", 10).await.expect("stats");
        let st2 = wait_share(&mut a, |v| v["ev"] == "share.stats", 10).await.expect("stats2");
        let src = FrameSource::open(&rpath).ok().and_then(|s| s.read()).map(|(w, h, n, _)| (w, h, n));
        eprintln!("{height}p{fps}: enviado {}x{} {} fps {} kbps, codificação {} ms/quadro, CPU do processo {}% | {} (quadros de captura descartados/s: {}); recebido {:?}", st2["w"], st2["h"], st2["fps"], st2["kbps"], st2["enc_ms"], st2["cpu_pct"], st["cpu_pct"], st2["cap_dropped"], src);
        a.cmd.send(MeshCmd::ShareStop).await.unwrap();
        wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "off", 15).await;
    }
}

/// Áudio só do processo, de ponta a ponta: A transmite com o áudio de um PowerShell tocando um WAV; B recebe a faixa "screen-audio".
/// Emite som de verdade por alguns segundos: não roda por padrão.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
#[ignore]
async fn process_audio_reaches_the_viewer() {
    use crate::capture::SourceKind;
    use crate::procaudio::AudioMode;
    use crate::share::ShareCfg;
    let port = crate::mock::start(0).await.unwrap();
    let (ia, ib) = (Arc::new(Identity::generate()), Arc::new(Identity::generate()));
    let mut a = node(port, PA, "Ana", "tela3", ia);
    let (mut b, bau) = node_audio(port, PB, "Beto", "tela3", ib);
    assert!(authed(&mut a, 1, 20).await && authed(&mut b, 1, 20).await);
    let mut child = std::process::Command::new("powershell").args(["-NoProfile", "-Command", r"$p = New-Object Media.SoundPlayer 'C:\Windows\Media\Alarm01.wav'; $p.PlayLooping(); Start-Sleep -Seconds 9"]).spawn().expect("powershell");
    tokio::time::sleep(Duration::from_millis(1200)).await;
    a.cmd.send(MeshCmd::ShareStart { cfg: ShareCfg { kind: SourceKind::Test, height: 720, fps: 15, kbps: 0, label: "Teste".into(), audio: AudioMode::Tree(child.id()) }, preview: None, dump: None }).await.unwrap();
    let on = wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "on", 20).await.expect("on");
    assert_eq!(on["audio"], true, "{on}");
    let key = format!("{PA}#screen");
    let t0 = std::time::Instant::now();
    while t0.elapsed() < Duration::from_secs(6) && bau.rx_stat(&key).frames < 100 { tokio::time::sleep(Duration::from_millis(100)).await; }
    let st = bau.rx_stat(&key);
    let stats = wait_share(&mut a, |v| v["ev"] == "share.stats" && v["audio_frames"].as_u64().unwrap_or(0) > 0, 5).await;
    let _ = child.kill();
    eprintln!("B recebeu da faixa de áudio da tela: {} quadros Opus, pico {:.3}; emissor: {:?}", st.frames, st.peak, stats.map(|s| (s["audio_sources"].clone(), s["audio_peak"].clone(), s["audio_frames"].clone())));
    assert!(st.frames >= 50 && st.peak > 0.01, "áudio do processo não chegou: {st:?}");
    assert_eq!(bau.rx_stat(PA).frames, 0, "a faixa da tela não pode cair no canal de voz");
}

/// Item 6: quem entra depois recebe a transmissão em andamento (3 pessoas), a fonte muda de tamanho no meio (janela
/// redimensionada) e todos passam a receber o novo tamanho, e quem sai não derruba a transmissão dos outros.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn late_joiner_resize_and_leave_keep_the_share_alive() {
    use crate::capture::SourceKind;
    use crate::procaudio::AudioMode;
    use crate::share::ShareCfg;
    use crate::video::FrameSource;
    let port = crate::mock::start(0).await.unwrap();
    let (ia, ib, ic) = (Arc::new(Identity::generate()), Arc::new(Identity::generate()), Arc::new(Identity::generate()));
    let mut a = node(port, PA, "Ana", "tela4", ia);
    let mut b = node(port, PB, "Beto", "tela4", ib);
    assert!(authed(&mut a, 1, 20).await && authed(&mut b, 1, 20).await);
    let dir = std::env::temp_dir().join(format!("briks-share3-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir); std::fs::create_dir_all(&dir).unwrap();
    a.cmd.send(MeshCmd::ShareStart { cfg: ShareCfg { kind: SourceKind::TestResize, height: 1080, fps: 30, kbps: 0, label: "Janela".into(), audio: AudioMode::None }, preview: None, dump: None }).await.unwrap();
    wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "on", 20).await.expect("B: on");
    b.cmd.send(MeshCmd::ShareWatch { key: PA.into(), on: true, path: dir.join("b.bin") }).await.unwrap();
    let f = wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "frame", 20).await.expect("B: 1º quadro");
    assert_eq!((f["w"].as_u64(), f["h"].as_u64()), (Some(1280), Some(720)));
    // C entra com a transmissão já no ar
    let mut c = node(port, PC, "Caio", "tela4", ic);
    let on = wait_share(&mut c, |v| v["ev"] == "share.remote" && v["state"] == "on" && v["peer_id"] == PA, 30).await.expect("C (entrou depois) não soube da transmissão");
    assert_eq!(on["label"], "Janela");
    c.cmd.send(MeshCmd::ShareWatch { key: PA.into(), on: true, path: dir.join("c.bin") }).await.unwrap();
    wait_share(&mut c, |v| v["ev"] == "share.remote" && v["state"] == "frame", 25).await.expect("C: nenhum quadro");
    // a fonte encolhe para 960x540: os dois espectadores recebem o novo tamanho
    let fb = wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "frame" && v["w"] == 960, 25).await.expect("B não recebeu o novo tamanho");
    assert_eq!(fb["h"].as_u64(), Some(540));
    // C pode ter começado a assistir antes ou depois da mudança: confere o tamanho no próprio quadro dele
    let t0 = std::time::Instant::now();
    loop {
        let d = FrameSource::open(&dir.join("c.bin")).ok().and_then(|s| s.read()).map(|r| (r.0, r.1));
        if d == Some((960, 540)) { break; }
        assert!(t0.elapsed() < Duration::from_secs(20), "C não recebeu o novo tamanho: {d:?}");
        tokio::time::sleep(Duration::from_millis(100)).await;
    }
    // B sai da sala: C continua recebendo quadros novos
    let _ = b.cmd.send(MeshCmd::Close).await;
    tokio::time::sleep(Duration::from_millis(1500)).await;
    let src = FrameSource::open(&dir.join("c.bin")).unwrap();
    let n0 = src.read().map(|r| r.2).unwrap_or(0);
    tokio::time::sleep(Duration::from_millis(1500)).await;
    let (w, h, n1, _) = src.read().expect("C: quadro");
    eprintln!("depois de B sair: C recebeu {} quadros em 1,5 s ({w}x{h})", n1 - n0);
    assert!(n1 > n0 + 10 && (w, h) == (960, 540), "C parou de receber: {n0} -> {n1}");
    let _ = a.cmd.send(MeshCmd::Close).await; let _ = c.cmd.send(MeshCmd::Close).await;
}
