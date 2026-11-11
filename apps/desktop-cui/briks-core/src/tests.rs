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
        a.cmd.send(MeshCmd::ShareStart { cfg: ShareCfg { kind: SourceKind::Test, height: 720, fps: 30, kbps: 0, label: "Teste".into(), audio: crate::procaudio::AudioMode::None, content: crate::adapt::Content::Detail, adaptive: false }, preview: Some(dir.join("preview.bin")), dump: Some(sent_png.clone()) }).await.unwrap();
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
        let sr = crate::capture::stamp_rows(w);
        for t in 0..160 { let mut e = test_pattern(w, h, t); for q in e[..sr * w * 4].chunks_mut(4) { q[0] = 0; q[1] = 0; q[2] = 0; } let mut r = bgr.clone(); for q in r[..sr * w * 4].chunks_mut(4) { q[0] = 0; q[1] = 0; q[2] = 0; } let p = psnr(&e, &r); if p > best { best = p; } }
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
    // nunca o monitor principal: usa a 2ª tela; sem ela, não mede
    let srcs = crate::capture::list_sources(&dir);
    let Some(scr) = srcs["screens"].as_array().unwrap().iter().find(|s| s["primary"] != true) else { eprintln!("sem 2ª tela: medição pulada"); return };
    let hm: isize = scr["id"].as_str().unwrap().parse().unwrap();
    eprintln!("tela de teste: {} {}x{}; conversão: {}", scr["name"], scr["w"], scr["h"], std::env::var("BRIKS_CONVERT").unwrap_or_else(|_| "gpu".into()));
    let rpath = dir.join("recebido.bin");
    for (height, fps, kbps) in [(1080usize, 30u32, 0u32), (1080, 60, 0)] {
        a.cmd.send(MeshCmd::ShareStart { cfg: ShareCfg { kind: SourceKind::Screen(hm), height, fps, kbps, label: "Tela".into(), audio: crate::procaudio::AudioMode::None, content: crate::adapt::Content::Detail, adaptive: false }, preview: None, dump: None }).await.unwrap();
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
    a.cmd.send(MeshCmd::ShareStart { cfg: ShareCfg { kind: SourceKind::Test, height: 720, fps: 15, kbps: 0, label: "Teste".into(), audio: AudioMode::Tree(child.id()), content: crate::adapt::Content::Detail, adaptive: false }, preview: None, dump: None }).await.unwrap();
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
    a.cmd.send(MeshCmd::ShareStart { cfg: ShareCfg { kind: SourceKind::TestResize, height: 1080, fps: 30, kbps: 0, label: "Janela".into(), audio: AudioMode::None, content: crate::adapt::Content::Detail, adaptive: false }, preview: None, dump: None }).await.unwrap();
    wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "on", 20).await.expect("B: on");
    b.cmd.send(MeshCmd::ShareWatch { key: PA.into(), on: true, path: dir.join("b.bin") }).await.unwrap();
    let f = wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "frame", 20).await.expect("B: 1º quadro");
    // com a máquina carregada o 1º quadro pode chegar depois da mudança de tamanho (2 s): os dois são válidos
    assert!(matches!((f["w"].as_u64(), f["h"].as_u64()), (Some(1280), Some(720)) | (Some(960), Some(540))), "{f}");
    let already = f["w"] == 960;
    // C entra com a transmissão já no ar
    let mut c = node(port, PC, "Caio", "tela4", ic);
    let on = wait_share(&mut c, |v| v["ev"] == "share.remote" && v["state"] == "on" && v["peer_id"] == PA, 30).await.expect("C (entrou depois) não soube da transmissão");
    assert_eq!(on["label"], "Janela");
    c.cmd.send(MeshCmd::ShareWatch { key: PA.into(), on: true, path: dir.join("c.bin") }).await.unwrap();
    wait_share(&mut c, |v| v["ev"] == "share.remote" && v["state"] == "frame", 25).await.expect("C: nenhum quadro");
    // a fonte encolhe para 960x540: os dois espectadores recebem o novo tamanho
    if !already { let fb = wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "frame" && v["w"] == 960, 25).await.expect("B não recebeu o novo tamanho"); assert_eq!(fb["h"].as_u64(), Some(540)); }
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

// ---------------------------------------------------------------------------------------------------------------------
// Adaptação automática com rede ruim simulada (proxy UDP no meio de todo o tráfego): banda em degraus + perda.
// Gera um CSV por política em BRIKS_ADAPT_OUT (ou na pasta temporária). ~2,5 min: não roda por padrão.
//   cargo test --release -- --ignored --nocapture adaptation_follows_the_network
// ---------------------------------------------------------------------------------------------------------------------
const NA: &str = "aaaaaaaa-1111-4111-8111-0000000000a1";
const NB: &str = "bbbbbbbb-2222-4222-8222-0000000000b1";

struct Row { rx: String, th: u64, t: u32, limit: f64, loss_in: f64, target: u64, est: u64, sent: u64, fps: f64, tfps: u64, h: u64, loss: f64, rtt: f64, nacks: u64, plis: u64, rfps: u32, lat: i32, gap: u128, adapting: String }

async fn adapt_profile(content: crate::adapt::Content, name: &str, room: &str, steps: &[(u32, f64, f64)], out: &std::path::Path) -> Vec<Row> {
    use crate::capture::{read_stamp, SourceKind};
    use crate::share::ShareCfg;
    use crate::video::FrameSource;
    let port = crate::mock::start(0).await.unwrap();
    // ids em formato UUID (a sinalização valida), distintos por política
    let tag = if name == "nitidez" { "1" } else { "2" };
    let (pa, pb) = (format!("{}{tag}", &NA[..NA.len() - 1]), format!("{}{tag}", &NB[..NB.len() - 1]));
    let sim = crate::netsim::register(&[&pa, &pb]);
    let (ia, ib) = (Arc::new(Identity::generate()), Arc::new(Identity::generate()));
    let mut a = node(port, &pa, "Ana", room, ia);
    let mut b = node(port, &pb, "Beto", room, ib);
    assert!(authed(&mut a, 1, 30).await && authed(&mut b, 1, 30).await, "não autenticou pelo proxy");
    let rpath = out.join(format!("rx-{name}.bin"));
    a.cmd.send(MeshCmd::ShareStart { cfg: ShareCfg { kind: SourceKind::TestMotion, height: 1080, fps: 30, kbps: 12_000, label: "Adaptação".into(), audio: crate::procaudio::AudioMode::None, content, adaptive: true }, preview: None, dump: None }).await.unwrap();
    wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "on", 30).await.expect("on");
    b.cmd.send(MeshCmd::ShareWatch { key: pa.clone(), on: true, path: rpath.clone() }).await.unwrap();
    if wait_share(&mut b, |v| { if v["ev"] == "share.rxstats" { eprintln!("[dbg rx] {v}"); } v["ev"] == "share.remote" && v["state"] == "frame" }, 30).await.is_none() {
        while let Ok(e) = a.events.try_recv() { if let MeshEvent::Share(v) = e { eprintln!("[dbg tx] {v}"); } }
        panic!("1º quadro não chegou (proxy: {} bytes encaminhados, {} pacotes descartados)", sim.forwarded.load(std::sync::atomic::Ordering::Relaxed), sim.dropped.load(std::sync::atomic::Ordering::Relaxed));
    }
    assert!(sim.forwarded.load(std::sync::atomic::Ordering::Relaxed) > 10_000, "o tráfego não está passando pelo proxy");
    let src = FrameSource::open(&rpath).unwrap();
    let mut rows = vec![];
    let (mut t, mut last_n, mut last_new) = (0u32, 0u64, std::time::Instant::now());
    let mut st = serde_json::Value::Null;
    let mut rx = serde_json::Value::Null;
    for (secs, mbps, loss) in steps {
        sim.set(*mbps, *loss);
        for _ in 0..*secs {
            let end = std::time::Instant::now() + Duration::from_secs(1);
            let (mut nfr, mut lats, mut gap) = (0u32, vec![], 0u128);
            while std::time::Instant::now() < end {
                while let Ok(e) = a.events.try_recv() { if let MeshEvent::Share(v) = e { if v["ev"] == "share.stats" { st = v; } } }
                while let Ok(e) = b.events.try_recv() { if let MeshEvent::Share(v) = e { if v["ev"] == "share.rxstats" { rx = v; } } }
                if let Some((w, _, n, rgba)) = src.read() {
                    if n != last_n { nfr += (n - last_n) as u32; last_n = n; gap = gap.max(last_new.elapsed().as_millis()); last_new = std::time::Instant::now();
                        let l = (src.ts() as u32).wrapping_sub(crate::capture::read_stamp_scaled(&rgba, w, 1920)) as i32; if (0..10_000).contains(&l) { lats.push(l); } }
                }
                tokio::time::sleep(Duration::from_millis(12)).await;
            }
            gap = gap.max(last_new.elapsed().as_millis());
            lats.sort();
            t += 1;
            rows.push(Row { rx: format!("{},{},{},{},{}", rx["plis"], rx["lost_frames"], rx["fed"], rx["dec_none"], rx["dec_err"]), th: st["target_h"].as_u64().unwrap_or(0), t, limit: *mbps, loss_in: *loss, target: st["target_kbps"].as_u64().unwrap_or(0), est: st["est_kbps"].as_f64().unwrap_or(0.0) as u64, sent: st["kbps"].as_f64().unwrap_or(0.0) as u64,
                fps: st["fps"].as_f64().unwrap_or(0.0), tfps: st["target_fps"].as_u64().unwrap_or(0), h: st["h"].as_u64().unwrap_or(0), loss: st["loss_pct"].as_f64().unwrap_or(0.0), rtt: st["rtt_ms"].as_f64().unwrap_or(0.0),
                nacks: st["nacks"].as_u64().unwrap_or(0), plis: st["plis"].as_u64().unwrap_or(0), rfps: nfr, lat: lats.get(lats.len() / 2).copied().unwrap_or(-1), gap, adapting: st["adapting"].as_str().unwrap_or("").into() });
        }
    }
    let mut csv = String::from("t_s,limite_mbps,perda_injetada_pct,alvo_kbps,estimativa_kbps,enviado_kbps,fps_enviado,fps_alvo,altura_alvo,altura,perda_reportada_pct,rtt_ms,nacks,plis_acum,fps_recebido,latencia_p50_ms,maior_intervalo_ms,adaptando,rx_plis,rx_quadros_perdidos,rx_entregues_dec,rx_dec_sem_saida,rx_dec_erros\n");
    for r in &rows { csv += &format!("{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n", r.t, r.limit, r.loss_in, r.target, r.est, r.sent, r.fps, r.tfps, r.th, r.h, r.loss, r.rtt, r.nacks, r.plis, r.rfps, r.lat, r.gap, r.adapting, r.rx); }
    std::fs::write(out.join(format!("adaptacao-{name}.csv")), csv).unwrap();
    let _ = a.cmd.send(MeshCmd::Close).await; let _ = b.cmd.send(MeshCmd::Close).await;
    rows
}

#[tokio::test(flavor = "multi_thread", worker_threads = 6)]
#[ignore]
async fn adaptation_follows_the_network() {
    let out = std::env::var("BRIKS_ADAPT_OUT").map(std::path::PathBuf::from).unwrap_or_else(|_| std::env::temp_dir().join("briks-adapt"));
    std::fs::create_dir_all(&out).unwrap();
    // (segundos, banda Mbps, perda %)
    let steps = [(12u32, 20.0, 0.0), (14, 6.0, 0.0), (14, 2.0, 2.0), (14, 0.8, 5.0), (18, 6.0, 0.0)];
    let mut policy = vec![];
    for (content, name) in [(crate::adapt::Content::Detail, "nitidez"), (crate::adapt::Content::Motion, "fluidez")] {
        let rows = adapt_profile(content, name, &format!("adapt-{name}"), &steps, &out).await;
        let avg = |a: usize, b: usize, f: &dyn Fn(&Row) -> f64| rows[a..b].iter().map(f).sum::<f64>() / (b - a) as f64;
        let sent = |r: &Row| r.sent as f64;
        let (free, m6, m2, m08, back) = (avg(6, 12, &sent), avg(19, 26, &sent), avg(33, 40, &sent), avg(47, 54, &sent), avg(66, 72, &sent));
        let worst_gap = rows.iter().map(|r| r.gap).max().unwrap();
        let (h2, f2) = (avg(33, 40, &|r| r.h as f64), avg(33, 40, &|r| r.tfps as f64));
        let (h08, f08) = (avg(47, 54, &|r| r.h as f64), avg(47, 54, &|r| r.tfps as f64));
        eprintln!("[{name}] enviado (kbps): livre {free:.0} | 6 Mbps {m6:.0} | 2 Mbps+2% {m2:.0} | 0,8 Mbps+5% {m08:.0} | volta a 6 Mbps {back:.0}");
        eprintln!("[{name}] nível: 2 Mbps → {h2:.0}p a {f2:.0} qps; 0,8 Mbps → {h08:.0}p a {f08:.0} qps; maior intervalo sem quadro novo no receptor {worst_gap} ms");
        eprintln!("[{name}] latência p50 (ms): livre {:.0} | 6 Mbps {:.0} | 2 Mbps {:.0} | 0,8 Mbps {:.0}", avg(6, 12, &|r| r.lat as f64), avg(19, 26, &|r| r.lat as f64), avg(33, 40, &|r| r.lat as f64), avg(47, 54, &|r| r.lat as f64));
        assert!(free > 4500.0, "sem limite deveria usar bem mais que os degraus apertados: {free}");
        assert!(m6 < 6300.0 && m2 < 2200.0 && m08 < 950.0, "o envio não respeitou a banda: {m6} {m2} {m08}");
        assert!(back > m08 * 2.5, "não se recuperou quando a banda voltou: {back}");
        policy.push((name, h2, f2, h08, f08));
        assert!(worst_gap < 7000, "receptor congelado por {worst_gap} ms");
    }
    // política por conteúdo, com a MESMA rede: a nitidez segura mais resolução (e cede FPS); a fluidez segura mais FPS (e cede resolução)
    let (n, f) = (policy[0], policy[1]);
    eprintln!("política a 2 Mbps: nitidez {:.0}p a {:.0} qps × fluidez {:.0}p a {:.0} qps; a 0,8 Mbps: nitidez {:.0}p a {:.0} qps × fluidez {:.0}p a {:.0} qps", n.1, n.2, f.1, f.2, n.3, n.4, f.3, f.4);
    assert!(n.1 >= f.1 && f.2 >= n.2 && (n.1 > f.1 || f.2 > n.2), "as duas políticas deveriam divergir no aperto");
    eprintln!("CSV em {}", out.display());
}

/// Recuperação de perda isolada: 5% de perda aleatória, sem limite de banda, bitrate fixo. Conta no receptor quantos buracos a
/// retransmissão (NACK) fechou dentro do prazo e quantos viraram quadro perdido + pedido de quadro-chave.
#[tokio::test(flavor = "multi_thread", worker_threads = 6)]
#[ignore]
async fn loss_is_repaired_by_retransmission() {
    use crate::capture::SourceKind;
    use crate::share::ShareCfg;
    let port = crate::mock::start(0).await.unwrap();
    let (pa, pb) = ("aaaaaaaa-1111-4111-8111-0000000000c1", "bbbbbbbb-2222-4222-8222-0000000000c1");
    let sim = crate::netsim::register(&[pa, pb]);
    let (ia, ib) = (Arc::new(Identity::generate()), Arc::new(Identity::generate()));
    let mut a = node(port, pa, "Ana", "perda1", ia);
    let mut b = node(port, pb, "Beto", "perda1", ib);
    assert!(authed(&mut a, 1, 30).await && authed(&mut b, 1, 30).await);
    let dir = std::env::temp_dir().join(format!("briks-perda-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    a.cmd.send(MeshCmd::ShareStart { cfg: ShareCfg { kind: SourceKind::TestMotion, height: 720, fps: 30, kbps: 3000, label: "Perda".into(), audio: crate::procaudio::AudioMode::None, content: crate::adapt::Content::Detail, adaptive: false }, preview: None, dump: None }).await.unwrap();
    wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "on", 30).await.expect("on");
    b.cmd.send(MeshCmd::ShareWatch { key: pa.into(), on: true, path: dir.join("rx.bin") }).await.unwrap();
    wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "frame", 30).await.expect("quadro");
    for loss in [0.0, 2.0, 5.0, 10.0] {
        sim.set(0.0, loss);
        tokio::time::sleep(Duration::from_millis(1500)).await;
        let base = wait_share(&mut b, |v| v["ev"] == "share.rxstats", 5).await.expect("rxstats");
        let (mut fps, mut n) = (0u64, 0u64);
        let mut last = base.clone();
        let mut tx = serde_json::Value::Null;
        let t0 = std::time::Instant::now();
        while t0.elapsed() < Duration::from_secs(8) {
            if let Some(v) = wait_share(&mut b, |v| v["ev"] == "share.rxstats", 3).await { fps += v["fps"].as_u64().unwrap_or(0); n += 1; last = v; }
            while let Ok(e) = a.events.try_recv() { if let MeshEvent::Share(v) = e { if v["ev"] == "share.stats" { tx = v; } } }
        }
        let d = |k: &str| last[k].as_u64().unwrap_or(0) - base[k].as_u64().unwrap_or(0);
        eprintln!("perda {loss}%: {:.1} qps no receptor; buracos consertados por retransmissão {}, quadros perdidos {}, pedidos de quadro-chave {}; emissor: retransmitidos/s {}, recusados/s {}, perda real/s {} pacotes",
            fps as f64 / n.max(1) as f64, d("recovered"), d("lost_frames"), d("plis"), tx["rtx"], tx["rtx_skipped"], tx["lost_pkts"]);
    }
}

/// Sobrecarga local ("a taxa de quadros cai"): o codificador é atrasado artificialmente em 60 ms por quadro — não dá para manter
/// 30 qps. O controlador tem de baixar o FPS alvo até caber e voltar quando a folga reaparece. Sem rede no meio.
/// Mede a folga real da máquina: rode sozinho (`--ignored local_overload`); em paralelo com a suíte a CPU fica de fato ocupada
/// e o FPS, corretamente, não volta.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
#[ignore]
async fn local_overload_lowers_fps_and_recovers() {
    use crate::capture::SourceKind;
    use crate::share::{ShareCfg, SLOW_LABEL, TEST_SLOW_MS};
    use std::sync::atomic::Ordering;
    let port = crate::mock::start(0).await.unwrap();
    let mut a = node(port, "aaaaaaaa-1111-4111-8111-0000000000d1", "Ana", "lento1", Arc::new(Identity::generate()));
    a.cmd.send(MeshCmd::ShareStart { cfg: ShareCfg { kind: SourceKind::TestMotion, height: 1080, fps: 30, kbps: 20_000, label: SLOW_LABEL.into(), audio: crate::procaudio::AudioMode::None, content: crate::adapt::Content::Detail, adaptive: true }, preview: None, dump: None }).await.unwrap();
    let mut csv = String::from("t_s,atraso_ms,fps_alvo,fps_enviado,ms_por_quadro,altura,adaptando\n");
    let mut t = 0;
    let mut rows: Vec<(u64, u64, f64, f64, String)> = vec![];
    for (slow, secs) in [(0u64, 5u32), (60, 12), (0, 22)] {
        TEST_SLOW_MS.store(slow, Ordering::Relaxed);
        for _ in 0..secs {
            let st = wait_share(&mut a, |v| v["ev"] == "share.stats", 5).await.expect("stats");
            t += 1;
            csv += &format!("{t},{slow},{},{},{},{},{}\n", st["target_fps"], st["fps"], st["work_ms"], st["h"], st["adapting"].as_str().unwrap_or(""));
            rows.push((slow, st["target_fps"].as_u64().unwrap_or(0), st["fps"].as_f64().unwrap_or(0.0), st["work_ms"].as_f64().unwrap_or(0.0), st["adapting"].as_str().unwrap_or("").to_string()));
        }
    }
    TEST_SLOW_MS.store(0, Ordering::Relaxed);
    let out = std::env::var("BRIKS_ADAPT_OUT").map(std::path::PathBuf::from).unwrap_or_else(|_| std::env::temp_dir().join("briks-adapt"));
    let _ = std::fs::create_dir_all(&out);
    let _ = std::fs::write(out.join("sobrecarga.csv"), &csv);
    let before = rows[3].1;
    let during = rows[5..17].iter().map(|r| r.1).min().unwrap();
    let during_work = rows[14].3;
    let cpu_flag = rows[5..17].iter().any(|r| r.4 == "cpu");
    let after = rows.last().unwrap().1;
    eprintln!("sobrecarga local: FPS alvo antes {before}, durante (60 ms/quadro) {during} com {during_work:.0} ms por quadro, depois {after}; marcou 'cpu': {cpu_flag}");
    assert_eq!(before, 30);
    assert!(during <= 15 && cpu_flag, "não reduziu o FPS sob sobrecarga: {during}");
    assert!(after >= 20, "não voltou quando a folga reapareceu: {after}");
    let _ = a.cmd.send(MeshCmd::Close).await;
}

/// Áudio da transmissão em ESTÉREO no receptor, volume do espectador e sincronia A/V. Fontes sintéticas presas ao mesmo relógio:
/// bipe de 1 kHz no início de cada segundo (só no canal esquerdo) e vídeo com o relógio carimbado em cada quadro. No receptor:
/// atraso do áudio = instante em que o início do bipe chega ao buffer de reprodução (ms dentro do segundo); atraso do vídeo =
/// instante em que o quadro fica pronto menos o carimbo. O buffer de reprodução do áudio soma 60 ms antes de tocar.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn screen_audio_is_stereo_and_av_offset_is_measured() {
    use crate::capture::{read_stamp, SourceKind};
    use crate::procaudio::AudioMode;
    use crate::share::ShareCfg;
    use crate::video::FrameSource;
    let port = crate::mock::start(0).await.unwrap();
    let (pa, pb) = ("aaaaaaaa-1111-4111-8111-0000000000e1", "bbbbbbbb-2222-4222-8222-0000000000e1");
    let (ia, ib) = (Arc::new(Identity::generate()), Arc::new(Identity::generate()));
    let mut a = node(port, pa, "Ana", "av1", ia);
    let (mut b, bau) = node_audio(port, pb, "Beto", "av1", ib);
    assert!(authed(&mut a, 1, 20).await && authed(&mut b, 1, 20).await);
    let dir = std::env::temp_dir().join(format!("briks-av-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    a.cmd.send(MeshCmd::ShareStart { cfg: ShareCfg { kind: SourceKind::Test, height: 720, fps: 30, kbps: 0, label: "AV".into(), audio: AudioMode::TestBeep, content: crate::adapt::Content::Detail, adaptive: false }, preview: None, dump: None }).await.unwrap();
    let on = wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "on", 20).await.expect("on");
    assert_eq!(on["audio"], true);
    b.cmd.send(MeshCmd::ShareWatch { key: pa.into(), on: true, path: dir.join("rx.bin") }).await.unwrap();
    wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "frame", 25).await.expect("quadro");
    let src = FrameSource::open(&dir.join("rx.bin")).unwrap();
    let (mut last_n, mut vlat) = (0u64, vec![]);
    let t0 = std::time::Instant::now();
    while t0.elapsed() < Duration::from_secs(7) {
        if let Some((w, _, n, rgba)) = src.read() { if n != last_n { last_n = n; let l = (src.ts() as u32).wrapping_sub(read_stamp(&rgba, w)) as i32; if (0..2000).contains(&l) { vlat.push(l); } } }
        tokio::time::sleep(Duration::from_millis(8)).await;
    }
    let key = format!("{pa}#screen");
    let st = bau.rx_stat(&key);
    let mut alat: Vec<i32> = st.onsets.iter().skip(1).map(|t| (t % 1000) as i32).filter(|d| *d < 900).collect();
    vlat.sort(); alat.sort();
    assert!(st.frames > 100 && alat.len() >= 3 && vlat.len() > 60, "amostras insuficientes: áudio {} quadros, {} bipes, vídeo {}", st.frames, alat.len(), vlat.len());
    let (v50, a50) = (vlat[vlat.len() / 2], alat[alat.len() / 2]);
    eprintln!("estéreo: maior diferença entre canais {:.3} (0 = mono), pico {:.3}", st.stereo_diff, st.peak);
    eprintln!("A/V: vídeo pronto {v50} ms depois da captura (p50, {} quadros); áudio no buffer {a50} ms depois do bipe (p50, {} bipes, mín {} máx {}); com os 60 ms do buffer de reprodução o áudio sai ~{} ms DEPOIS do vídeo",
        vlat.len(), alat.len(), alat[0], alat[alat.len() - 1], a50 + 60 - v50);
    assert!(st.stereo_diff > 0.2, "o áudio da transmissão chegou em mono");
    assert!((a50 + 60 - v50).abs() < 250, "áudio e vídeo fora de sincronia: {} ms", a50 + 60 - v50);
    // volume do espectador: com ganho 0 a mixagem sai muda; com 1 volta
    // puxa a mixagem como a placa de som faria (10 ms por vez) durante 1,5 s e guarda o pico: cobre pelo menos um bipe
    async fn level(au: &Arc<Audio>) -> f32 {
        let mut peak = 0.0f32;
        for _ in 0..150 { let mut out = vec![0.0f32; 960]; au.mix_pull_stereo(&mut out); peak = out.iter().fold(peak, |m, v| m.max(v.abs())); tokio::time::sleep(Duration::from_millis(10)).await; }
        peak
    }
    let loud = level(&bau).await;
    b.cmd.send(MeshCmd::ShareVolume { key: pa.into(), gain: 0.0 }).await.unwrap();
    tokio::time::sleep(Duration::from_millis(300)).await;
    let muted = level(&bau).await;
    eprintln!("volume do espectador: pico {loud:.3} com volume 1; {muted:.3} com volume 0");
    assert!(loud > 0.1 && muted < 0.001);
}

/// Vários espectadores: A transmite para B (rede boa) e C (atrás de uma rede ruim simulada: 0,5 Mbps e 5% de perda).
/// O espectador ruim não pode derrubar a qualidade do bom: B continua recebendo perto do FPS cheio e o bitrate do emissor não
/// desce ao nível de C. Mede também o custo do emissor com 1 e com 2 espectadores.
#[tokio::test(flavor = "multi_thread", worker_threads = 6)]
#[ignore]
async fn slow_viewer_does_not_drag_the_others() {
    use crate::capture::SourceKind;
    use crate::share::ShareCfg;
    let port = crate::mock::start(0).await.unwrap();
    let (pa, pb, pc) = ("aaaaaaaa-1111-4111-8111-0000000000f1", "bbbbbbbb-2222-4222-8222-0000000000f1", "cccccccc-3333-4333-8333-0000000000f1");
    let sim = crate::netsim::register(&[pc]);   // só os candidatos de C passam pelo proxy: todo o tráfego de/para C
    let dir = std::env::temp_dir().join(format!("briks-multi-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let mut a = node(port, pa, "Ana", "multi1", Arc::new(Identity::generate()));
    let mut b = node(port, pb, "Beto", "multi1", Arc::new(Identity::generate()));
    assert!(authed(&mut a, 1, 30).await && authed(&mut b, 1, 30).await);
    a.cmd.send(MeshCmd::ShareStart { cfg: ShareCfg { kind: SourceKind::TestMotion, height: 720, fps: 30, kbps: 3000, label: "Multi".into(), audio: crate::procaudio::AudioMode::None, content: crate::adapt::Content::Detail, adaptive: true }, preview: None, dump: None }).await.unwrap();
    wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "on", 30).await.expect("B on");
    b.cmd.send(MeshCmd::ShareWatch { key: pa.into(), on: true, path: dir.join("b.bin") }).await.unwrap();
    wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "frame", 30).await.expect("B quadro");
    // mede `secs` segundos: (kbps enviado, alvo, fps do emissor, CPU do processo, fps de B, deixados de fora)
    async fn measure(a: &mut MeshHandle, b: &mut MeshHandle, secs: u64) -> (f64, f64, f64, f64, f64, u64) {
        let (mut k, mut t, mut f, mut c, mut n, mut bf, mut bn, mut out) = (0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0u64);
        let end = std::time::Instant::now() + Duration::from_secs(secs);
        while std::time::Instant::now() < end {
            tokio::select! {
                e = a.events.recv() => if let Some(MeshEvent::Share(v)) = e { if v["ev"] == "share.stats" { k += v["kbps"].as_f64().unwrap_or(0.0); t += v["target_kbps"].as_f64().unwrap_or(0.0); f += v["fps"].as_f64().unwrap_or(0.0); c += v["cpu_pct"].as_f64().unwrap_or(0.0); out = out.max(v["left_out"].as_u64().unwrap_or(0)); n += 1.0; } },
                e = b.events.recv() => if let Some(MeshEvent::Share(v)) = e { if v["ev"] == "share.rxstats" { bf += v["fps"].as_f64().unwrap_or(0.0); bn += 1.0; } },
                _ = tokio::time::sleep(Duration::from_millis(200)) => {}
            }
        }
        (k / n.max(1.0), t / n.max(1.0), f / n.max(1.0), c / n.max(1.0), bf / bn.max(1.0), out)
    }
    let one = measure(&mut a, &mut b, 8).await;
    eprintln!("1 espectador (rede boa): enviado {:.0} kbps (alvo {:.0}), {:.1} qps no emissor, CPU do processo {:.0}%, B recebe {:.1} qps", one.0, one.1, one.2, one.3, one.4);
    // C entra por uma rede ruim
    sim.set(0.5, 5.0);
    let mut c = node(port, pc, "Caio", "multi1", Arc::new(Identity::generate()));
    wait_share(&mut c, |v| v["ev"] == "share.remote" && v["state"] == "on", 40).await.expect("C on");
    c.cmd.send(MeshCmd::ShareWatch { key: pa.into(), on: true, path: dir.join("c.bin") }).await.unwrap();
    let _ = measure(&mut a, &mut b, 6).await;   // acomodação
    let two = measure(&mut a, &mut b, 12).await;
    let mut cf = (0.0, 0.0);
    while let Ok(e) = c.events.try_recv() { if let MeshEvent::Share(v) = e { if v["ev"] == "share.rxstats" { cf.0 += v["fps"].as_f64().unwrap_or(0.0); cf.1 += 1.0; } } }
    eprintln!("2 espectadores (C a 0,5 Mbps com 5% de perda): enviado {:.0} kbps (alvo {:.0}), {:.1} qps no emissor, CPU do processo {:.0}%, B recebe {:.1} qps, C recebe {:.1} qps, deixados de fora do cálculo: {}",
        two.0, two.1, two.2, two.3, two.4, cf.0 / f64::max(cf.1, 1.0), two.5);
    assert!(two.4 >= one.4 * 0.8, "o espectador bom perdeu quadros por causa do ruim: {:.1} → {:.1} qps", one.4, two.4);
    assert!(two.1 >= one.1 * 0.7, "o bitrate de todos caiu por causa de um: {:.0} → {:.0} kbps", one.1, two.1);
    assert_eq!(two.5, 1, "o espectador ruim deveria ter ficado fora do cálculo");
}
