//! Modo híbrido (mídia no SFU + malha só de DataChannel para chat/status/presença/expulsão) contra `livekit-server --dev` real
//! e o Realtime simulado. Sem GPU, sem janela, sem placa de som. Precisa de `BRIKS_LIVEKIT_SERVER_BIN` (senão avisa e passa).
use std::process::{Child, Command, Stdio};
use std::sync::Arc;
use std::time::{Duration, Instant};

use serde_json::json;
use tokio::time::timeout;

use crate::audio::Audio;
use crate::identity::Identity;
use crate::mesh::{self, MeshCfg, MeshCmd, MeshEvent, MeshHandle};
use crate::realtime::{self, RealtimeCfg};
use crate::signaling::{secure_room_id, topic_name};
use crate::sfu::{self, SfuJoin};
use crate::tests_sfu::dev_token;

struct Server { child: Child }
impl Drop for Server { fn drop(&mut self) { let _ = self.child.kill(); let _ = self.child.wait(); } }

fn start_server() -> Option<Server> {
    let bin = std::env::var("BRIKS_LIVEKIT_SERVER_BIN").ok()?;
    if !std::path::Path::new(&bin).exists() { return None; }
    let child = Command::new(bin).args(crate::tests_sfu::lk_args()).stdout(Stdio::null()).stderr(Stdio::null()).spawn().ok()?;
    let t0 = Instant::now();
    while t0.elapsed() < Duration::from_secs(10) {
        if std::net::TcpStream::connect(("127.0.0.1", crate::tests_sfu::lk_port())).is_ok() { return Some(Server { child }); }
        std::thread::sleep(Duration::from_millis(100));
    }
    let mut s = Server { child }; let _ = s.child.kill(); None
}

struct Node { h: MeshHandle, audio: Arc<Audio>, fp: String }

async fn node(rt_port: u16, room: &str, peer: &str, sfu_ident: &str, name: &str, joined: u64) -> Node {
    let id = Arc::new(Identity::generate());
    let fp = id.fingerprint();
    let mut cfg = RealtimeCfg::new(&format!("ws://127.0.0.1:{rt_port}"), "chave-de-teste");
    cfg.leave_grace = Duration::from_millis(200);
    let topic = topic_name("room", &secure_room_id("room", room));
    let (rt, rx) = realtime::connect(cfg, topic, peer.into(), json!({ "peerId": peer, "joinedAt": joined, "clientVersion": "teste" }));
    let lk_room = format!("room:{}", &secure_room_id("room", room)[..32]);
    let (au, au_rx) = Audio::new(true);
    let sh = sfu::connect(SfuJoin { url: crate::tests_sfu::lk_ws(), token: dev_token(sfu_ident, &lk_room), audio: Some((au.clone(), au_rx)), data: None, renew: None }).await.expect("entrar no SFU");
    let m = mesh::spawn(MeshCfg { namespace: "room".into(), room: room.into(), local_peer: peer.into(), display_name: name.into(), ice_servers: vec![], audio: None, owner_fp: None, joined_at: joined }, id, rt, rx);
    let h = crate::hybrid::spawn(m, sh, Some(au.clone())).await;
    Node { h, audio: au, fp }
}

async fn wait<T>(n: &mut Node, secs: u64, mut f: impl FnMut(&MeshEvent) -> Option<T>) -> Option<T> {
    let end = Instant::now() + Duration::from_secs(secs);
    loop {
        let left = end.saturating_duration_since(Instant::now());
        match timeout(left, n.h.events.recv()).await { Ok(Some(e)) => { if let Some(t) = f(&e) { return Some(t); } } _ => return None }
    }
}

const PA: &str = "aaaaaaaa-1111-4111-8111-00000000000a";
const PB: &str = "bbbbbbbb-2222-4222-8222-00000000000b";
const IA: &str = "11111111-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
const IB: &str = "22222222-bbbb-4bbb-8bbb-bbbbbbbbbbbb";

/// Voz e tela pelo SFU E chat/evento/status/expulsão pela malha de dados ao mesmo tempo, com os peer_ids da malha nos eventos do SFU.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn hybrid_chat_presence_voice_share_and_kick() {
    use crate::capture::SourceKind;
    use crate::share::ShareCfg;
    let Some(_srv) = start_server() else { eprintln!("PULADO: BRIKS_LIVEKIT_SERVER_BIN não definido"); return };
    if std::env::var("BRIKS_SFU_TEST_GPU").is_err() { std::env::set_var("BRIKS_ENCODER", "openh264"); }
    std::env::set_var("BRIKS_DECODER", "openh264");
    let rt_port = crate::mock::start(0).await.unwrap();
    let room = format!("hibrida-{}", uuid::Uuid::new_v4().simple());
    let mut a = node(rt_port, &room, PA, IA, "Ana", 1).await;   // A entrou primeiro: é a dona
    let mut b = node(rt_port, &room, PB, IB, "Beto", 2).await;

    // presença autenticada pela malha
    let info = wait(&mut b, 25, |e| match e { MeshEvent::PeerInfo { peer_id, fp, name } if peer_id == PA => Some((fp.clone(), name.clone())), _ => None }).await.expect("B não autenticou o A");
    assert_eq!(info, (a.fp.clone(), "Ana".to_string()));
    wait(&mut a, 25, |e| matches!(e, MeshEvent::PeerInfo { peer_id, .. } if peer_id == PB).then_some(())).await.expect("A não autenticou o B");

    // chat: DM de B para A e evento assinado de A para todos, pela malha (nada disso passa pelo SFU)
    b.h.cmd.send(MeshCmd::Chat { to_fp: a.fp.clone(), text: "oi pelo híbrido".into() }).await.unwrap();
    let got = wait(&mut a, 20, |e| match e { MeshEvent::Chat { content, from_fp, .. } => Some((content.clone(), from_fp.clone())), _ => None }).await.expect("A não recebeu o chat");
    assert_eq!(got, ("oi pelo híbrido".to_string(), b.fp.clone()));
    a.h.cmd.send(MeshCmd::Event { channel: "dm-x".into(), target: "msg-0001-aaaa".into(), action: "reaction.add".into(), content: None, reference: None, emoji: Some("👍".into()) }).await.unwrap();
    wait(&mut b, 20, |e| matches!(e, MeshEvent::ChatEvent { local: false, .. }).then_some(())).await.expect("B não recebeu o evento de chat");

    // status/presença
    a.h.cmd.send(MeshCmd::SetProfile { status: "jogando".into(), presence: "Teste".into() }).await.unwrap();
    wait(&mut b, 20, |e| matches!(e, MeshEvent::PeerState { peer_id, status, .. } if peer_id == PA && status == "jogando").then_some(())).await.expect("B não viu o status do A");

    // voz: pelo SFU (a malha não tem áudio) e o nível de fala sai com o peer_id da malha
    wait(&mut b, 25, |e| matches!(e, MeshEvent::Speaking { peer_id, level } if peer_id == PA && *level > 0.0).then_some(())).await.expect("B não viu o A falando (peer_id da malha)");
    let n = b.audio.rx_stat(IA).frames;
    tokio::time::sleep(Duration::from_secs(2)).await;
    let d = b.audio.rx_stat(IA).frames - n;
    eprintln!("híbrido: {d} quadros de voz do A em 2 s pelo SFU");
    assert!(d > 40, "voz não chegou pelo SFU: {d}");

    // mudo do A: o estado vai pela malha
    a.audio.set_mic_muted(true);
    a.h.cmd.send(MeshCmd::BroadcastState).await.unwrap();
    wait(&mut b, 15, |e| matches!(e, MeshEvent::PeerState { peer_id, microphone: false, .. } if peer_id == PA).then_some(())).await.expect("B não soube do mudo");

    // tela: publicada pelo SFU, anunciada com o peer_id da malha
    a.h.cmd.send(MeshCmd::ShareStart { cfg: ShareCfg { kind: SourceKind::Test, height: 720, fps: 30, kbps: 0, label: "Teste".into(), audio: crate::procaudio::AudioMode::None, content: crate::adapt::Content::Detail, adaptive: false }, preview: None, dump: None }).await.unwrap();
    wait(&mut b, 25, |e| matches!(e, MeshEvent::Share(v) if v["ev"] == "share.remote" && v["state"] == "on" && v["peer_id"] == PA).then_some(())).await.expect("B não soube da transmissão do A com o peer_id da malha");
    a.h.cmd.send(MeshCmd::ShareStop).await.unwrap();

    // expulsão pela dona (mensagem assinada na malha): B é expulso e sai do SFU também
    a.h.cmd.send(MeshCmd::Kick { fp: b.fp.clone(), reason: "teste".into() }).await.unwrap();
    wait(&mut b, 20, |e| matches!(e, MeshEvent::Kicked { .. }).then_some(())).await.expect("B não foi expulso");
    a.h.cmd.send(MeshCmd::Close).await.unwrap();
}
