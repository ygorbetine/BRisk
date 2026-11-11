//! Expulsão DE VERDADE (RemoveParticipant do LiveKit) contra um `livekit-server --dev` real, sem Supabase e sem publicar nada.
//! A regra da Edge Function `livekit-kick` (`supabase/functions/_shared/kick.ts`) e o cliente de administração dela rodam pelo
//! Node (`supabase/tests/livekit-kick-local-check.ts`), com a autenticação trocada por uma identidade de teste.
//! `BRIKS_NODE_BIN` = caminho do node 22+ (sem ele, a remoção é pedida direto ao LiveKit e só a reação do cliente é provada).
//! As chaves usadas são as PÚBLICAS do modo `--dev` (devkey/secret, da documentação do LiveKit).
use std::time::Duration;

use base64::{engine::general_purpose::URL_SAFE_NO_PAD as B64, Engine};
use hmac::{Hmac, Mac};
use serde_json::json;
use sha2::Sha256;

use crate::mesh::MeshCmd;
use crate::signaling::secure_room_id;
use crate::tests_sfurec::{client, start_server};

const A: &str = "aaaaaaaa-1111-4111-8111-00000000k1c1";
const B: &str = "bbbbbbbb-2222-4222-8222-00000000k1c2";
const C: &str = "cccccccc-3333-4333-8333-00000000k1c3";

fn admin_token(room: &str) -> String {
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_secs();
    let head = B64.encode(br#"{"alg":"HS256","typ":"JWT"}"#);
    let body = B64.encode(json!({ "iss": "devkey", "sub": "teste", "nbf": now - 10, "exp": now + 60, "video": { "room": room, "roomAdmin": true } }).to_string());
    let mut m = Hmac::<Sha256>::new_from_slice(b"secret").unwrap();
    m.update(format!("{head}.{body}").as_bytes());
    format!("{head}.{body}.{}", B64.encode(m.finalize().into_bytes()))
}

/// Roda a regra da função (Node) como `requester` pedindo a saída de `target`. Devolve (status HTTP, corpo).
fn kick_function(node: &str, room: &str, requester: &str, target: &str) -> (u16, String) {
    let script = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../supabase/tests/livekit-kick-local-check.ts");
    let out = std::process::Command::new(node).args(["--experimental-transform-types", "--no-warnings"]).arg(&script)
        .env("LK_URL", "ws://127.0.0.1:7880").env("LK_KEY", "devkey").env("LK_SECRET", "secret").env("LK_ROOM", room).env("LK_REQUESTER", requester).env("LK_TARGET", target)
        .output().expect("rodar o node");
    let txt = String::from_utf8_lossy(&out.stdout).to_string();
    let line = txt.lines().find(|l| l.starts_with("KICK ")).unwrap_or_else(|| panic!("a função não respondeu: {} {}", txt, String::from_utf8_lossy(&out.stderr)));
    let mut it = line.splitn(3, ' ');
    it.next();
    (it.next().unwrap().parse().unwrap(), it.next().unwrap_or("").to_string())
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn sfu_kick_function_removes_participant_for_real() {
    let Some(_srv) = start_server() else { eprintln!("PULADO: BRIKS_LIVEKIT_SERVER_BIN não definido"); return };
    let room = format!("kick-{}", uuid::Uuid::new_v4().simple());
    let lk_room = format!("room:{}", &secure_room_id("room", &room)[..32]);
    // A entra primeiro: é a dona para os apps (joinedAt declarado) e para o servidor (instante de entrada registrado por ele)
    let a = client(&room, A, "Ana", 1000, 3600).await;
    tokio::time::sleep(Duration::from_millis(400)).await;
    let b = client(&room, B, "Beto", 1001, 3600).await;
    tokio::time::sleep(Duration::from_millis(400)).await;
    let c = client(&room, C, "Caio", 1002, 3600).await;
    for (x, o) in [(&a, &b), (&a, &c), (&b, &a), (&c, &a), (&c, &b)] { assert!(x.wait(30, &format!("info:{}:{}", o.ident, o.fp)).await, "{} não autenticou {}", x.ident, o.ident); }

    // o que o LiveKit devolve em ListParticipants (é disso que a função tira o dono)
    let lp = { let (r, t) = (lk_room.clone(), admin_token(&lk_room)); tokio::task::spawn_blocking(move || ureq::post("http://127.0.0.1:7880/twirp/livekit.RoomService/ListParticipants").set("authorization", &format!("Bearer {t}")).send_json(json!({ "room": r })).unwrap().into_json::<serde_json::Value>().unwrap()).await.unwrap() };
    let ps = lp["participants"].as_array().expect("participants");
    let joined = |id: &str| ps.iter().find(|p| p["identity"] == id).map(|p| { let v = if p["joined_at_ms"].is_null() { &p["joinedAtMs"] } else { &p["joined_at_ms"] }; v.as_str().and_then(|s| s.parse::<u64>().ok()).or(v.as_u64()).unwrap_or(0) }).unwrap_or(0);
    eprintln!("ListParticipants: {} participantes; joined_at_ms A/B/C = {}/{}/{}", ps.len(), joined(A), joined(B), joined(C));
    assert!(ps.len() == 3 && joined(A) > 0 && joined(A) < joined(B) && joined(B) < joined(C), "o servidor não registrou a ordem de entrada: {lp}");

    match std::env::var("BRIKS_NODE_BIN").ok().filter(|p| std::path::Path::new(p).exists()) {
        Some(node) => {
            // 1) quem não é o dono é recusado pela função e nada acontece
            let (n2, r2) = (node.clone(), lk_room.clone());
            let (st, body) = tokio::task::spawn_blocking(move || kick_function(&n2, &r2, B, A)).await.unwrap();
            eprintln!("função como B (não dono) expulsando A: {st} {body}");
            assert_eq!(st, 403, "{body}");
            assert!(body.contains("dono"));
            // 2) quem nem está na sala também
            let (n2, r2) = (node.clone(), lk_room.clone());
            let (st, body) = tokio::task::spawn_blocking(move || kick_function(&n2, &r2, "dddddddd-4444-4444-8444-00000000k1c4", B)).await.unwrap();
            assert_eq!(st, 403, "{body}");
            tokio::time::sleep(Duration::from_millis(800)).await;
            for x in [&a, &b, &c] { assert_eq!(x.count("kicked:") + x.count(":fechado"), 0, "pedido recusado derrubou alguém"); }
            // 3) a dona expulsa o B: a função chama RemoveParticipant no LiveKit real
            let (n2, r2) = (node, lk_room.clone());
            let (st, body) = tokio::task::spawn_blocking(move || kick_function(&n2, &r2, A, B)).await.unwrap();
            eprintln!("função como A (dona) expulsando B: {st} {body}");
            assert_eq!(st, 200, "{body}");
            assert!(body.contains(B));
        }
        None => {
            eprintln!("BRIKS_NODE_BIN não definido: a regra da função NÃO foi exercitada; RemoveParticipant pedido direto ao LiveKit");
            let (r, t) = (lk_room.clone(), admin_token(&lk_room));
            tokio::task::spawn_blocking(move || ureq::post("http://127.0.0.1:7880/twirp/livekit.RoomService/RemoveParticipant").set("authorization", &format!("Bearer {t}")).send_json(json!({ "room": r, "identity": B })).unwrap()).await.unwrap();
        }
    }
    // o B é derrubado pelo SERVIDOR: o core avisa "removido" (a sala fica bloqueada nesta sessão) e NÃO tenta reconectar
    assert!(b.wait(15, "kicked:removido").await, "o cliente removido não foi avisado: {:?}", b.log.lock().unwrap().iter().rev().take(6).collect::<Vec<_>>());
    assert!(a.wait(15, &format!("peer:{B}:fechado")).await && c.wait(15, &format!("peer:{B}:fechado")).await, "os outros não viram a saída do removido");
    tokio::time::sleep(Duration::from_secs(3)).await;
    assert_eq!(b.stats.reconnects.load(std::sync::atomic::Ordering::Relaxed), 0, "o removido voltou sozinho");
    assert_eq!(b.count(r#""state":"reconnecting""#), 0, "o removido tentou reconectar");
    // a chamada continua para quem ficou
    let f0 = c.audio.rx_stat(A).frames; tokio::time::sleep(Duration::from_secs(1)).await;
    assert!(c.audio.rx_stat(A).frames - f0 > 25, "a chamada parou para quem ficou");
    assert_eq!(a.count("kicked:") + c.count("kicked:"), 0);
    eprintln!("expulsão pelo servidor: o removido recebeu o aviso e não voltou; os outros viram a saída e seguem na chamada");
    for x in [&a, &c] { let _ = x.cmd.send(MeshCmd::Close).await; }
}
