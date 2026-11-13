//! Painel de sons em loopback: três malhas WebRTC reais (servidor Phoenix simulado). A tem o arquivo; B e C o recebem por hash,
//! verificado, e os três ouvem; depois o intervalo mínimo e o silenciar por pessoa.
use std::sync::{Arc, Mutex};
use std::time::Duration;

use serde_json::{json, Value};
use tokio::time::timeout;

use crate::identity::Identity;
use crate::mesh::{self, MeshCfg, MeshCmd, MeshEvent, MeshHandle};
use crate::realtime::{self, RealtimeCfg};
use crate::signaling::{secure_room_id, topic_name};
use crate::sons::{self, testutil::wav, Store};

fn node(port: u16, peer: &str, name: &str, room: &str, id: Arc<Identity>) -> MeshHandle {
    let mut cfg = RealtimeCfg::new(&format!("ws://127.0.0.1:{port}"), "chave-de-teste");
    cfg.leave_grace = Duration::from_millis(200);
    let topic = topic_name("room", &secure_room_id("room", room));
    let (rt, rx) = realtime::connect(cfg, topic, peer.into(), json!({ "peerId": peer, "joinedAt": 1, "clientVersion": "teste" }));
    mesh::spawn(MeshCfg { namespace: "room".into(), room: room.into(), local_peer: peer.into(), display_name: name.into(), ice_servers: vec![], audio: None, owner_fp: None, joined_at: 1 }, id, rt, rx)
}

fn store(tag: &str) -> Arc<Mutex<Store>> {
    let d = std::env::temp_dir().join(format!("briks-sons-lb-{tag}-{}", uuid::Uuid::new_v4()));
    std::fs::create_dir_all(&d).unwrap();
    Arc::new(Mutex::new(Store::open(&d)))
}

async fn authed(h: &mut MeshHandle, n: usize, secs: u64) -> bool {
    let mut got = std::collections::HashSet::new();
    let end = std::time::Instant::now() + Duration::from_secs(secs);
    while got.len() < n {
        let left = end.saturating_duration_since(std::time::Instant::now());
        match timeout(left, h.events.recv()).await {
            Ok(Some(MeshEvent::Peer { peer_id, state })) if state == "autenticado" => { got.insert(peer_id); }
            Ok(Some(_)) => {}
            _ => return false,
        }
    }
    true
}

/// Espera um evento de som `ev`.
async fn sound_ev(h: &mut MeshHandle, ev: &str, secs: u64) -> Option<Value> {
    timeout(Duration::from_secs(secs), async {
        while let Some(e) = h.events.recv().await {
            if let MeshEvent::Sound(v) = e { if v["ev"] == ev { return Some(v); } }
        }
        None
    }).await.ok().flatten()
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn three_peers_a_adds_sound_b_and_c_get_it_by_hash_all_hear_then_b_mutes_a() {
    let port = crate::mock::start(0).await.unwrap();
    let (ia, ib, ic) = (Arc::new(Identity::generate()), Arc::new(Identity::generate()), Arc::new(Identity::generate()));
    let (sa, sb, sc) = (store("a"), store("b"), store("c"));
    sons::register_store(&ia.fingerprint(), &sa);
    sons::register_store(&ib.fingerprint(), &sb);
    sons::register_store(&ic.fingerprint(), &sc);
    let it = sa.lock().unwrap().import_bytes(&wav(1500, 0.4), "Buzina", "📯", "me", &ia.fingerprint()).unwrap();
    let room = "sala-sons";
    let mut a = node(port, "aaaaaaaa-1111-4111-8111-0000000000a1", "Ana", room, ia.clone());
    let mut b = node(port, "bbbbbbbb-2222-4222-8222-0000000000b1", "Beto", room, ib.clone());
    let mut c = node(port, "cccccccc-3333-4333-8333-0000000000c1", "Cida", room, ic.clone());
    assert!(authed(&mut a, 2, 40).await && authed(&mut b, 2, 40).await && authed(&mut c, 2, 40).await, "as três não se autenticaram");

    a.cmd.send(MeshCmd::SoundPlay { hash: it.hash.clone() }).await.unwrap();
    let pa = sound_ev(&mut a, "sound.play", 10).await.expect("A não ouviu o próprio disparo");
    assert_eq!(pa["self"], true);
    for (n, h) in [("B", &mut b), ("C", &mut c)] {
        let p = sound_ev(h, "sound.play", 20).await.unwrap_or_else(|| panic!("{n} não tocou"));
        assert_eq!(p["hash"].as_str().unwrap(), it.hash);
        assert_eq!(p["from_fp"].as_str().unwrap(), ia.fingerprint());
    }
    assert!(sb.lock().unwrap().has(&it.hash) && sc.lock().unwrap().has(&it.hash), "B e C receberam o arquivo, com o hash conferido");
    assert_eq!(sb.lock().unwrap().read(&it.hash).unwrap(), sa.lock().unwrap().read(&it.hash).unwrap());

    // rajada: o segundo disparo de A, dentro dos 3 s, é barrado em A (e o receptor também barraria)
    a.cmd.send(MeshCmd::SoundPlay { hash: it.hash.clone() }).await.unwrap();
    assert!(sound_ev(&mut a, "sound.denied", 5).await.is_some(), "rajada não foi barrada");

    // B silencia A: depois do intervalo, C ouve de novo e B não
    sb.lock().unwrap().set_prefs(&json!({ "mute_fp": ia.fingerprint() }));
    tokio::time::sleep(Duration::from_millis(3200)).await;
    a.cmd.send(MeshCmd::SoundPlay { hash: it.hash.clone() }).await.unwrap();
    assert!(sound_ev(&mut c, "sound.play", 10).await.is_some(), "C deveria ouvir de novo");
    let d = sound_ev(&mut b, "sound.drop", 10).await.expect("B deveria descartar");
    assert_eq!(d["reason"], "silenciado");
    for h in [&mut a, &mut b, &mut c] { let _ = h.cmd.send(MeshCmd::Close).await; }
}
