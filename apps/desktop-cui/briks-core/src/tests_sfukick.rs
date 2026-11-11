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
pub(crate) fn kick_function(node: &str, room: &str, requester: &str, target: &str) -> (u16, String) {
    let script = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../supabase/tests/livekit-kick-local-check.ts");
    let out = std::process::Command::new(node).args(["--experimental-transform-types", "--no-warnings"]).arg(&script)
        .env("LK_URL", crate::tests_sfu::lk_ws()).env("LK_KEY", "devkey").env("LK_SECRET", "secret").env("LK_ROOM", room).env("LK_REQUESTER", requester).env("LK_TARGET", target)
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
    let lp = { let (r, t) = (lk_room.clone(), admin_token(&lk_room)); tokio::task::spawn_blocking(move || ureq::post(&format!("http://127.0.0.1:{}/twirp/livekit.RoomService/ListParticipants", crate::tests_sfu::lk_port())).set("authorization", &format!("Bearer {t}")).send_json(json!({ "room": r })).unwrap().into_json::<serde_json::Value>().unwrap()).await.unwrap() };
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
            tokio::task::spawn_blocking(move || ureq::post(&format!("http://127.0.0.1:{}/twirp/livekit.RoomService/RemoveParticipant", crate::tests_sfu::lk_port())).set("authorization", &format!("Bearer {t}")).send_json(json!({ "room": r, "identity": B })).unwrap()).await.unwrap();
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

/// A regra do dono fixo da função `livekit-token` e da expulsão que a respeita (supabase/tests/livekit-owner.test.ts), rodada
/// com o Node em simulações: sem Supabase, sem servidor de chamadas, sem publicar nada.
#[test]
fn owner_rule_of_the_token_function() {
    let Some(node) = std::env::var("BRIKS_NODE_BIN").ok().filter(|p| std::path::Path::new(p).exists()) else { eprintln!("PULADO: BRIKS_NODE_BIN não definido"); return };
    let script = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../supabase/tests/livekit-owner.test.ts");
    let out = std::process::Command::new(node).args(["--experimental-transform-types", "--no-warnings", "--test"]).arg(&script).output().expect("rodar o node");
    let txt = String::from_utf8_lossy(&out.stdout).to_string();
    let n = |k: &str| txt.lines().find_map(|l| l.strip_prefix(&format!("# {k} ")).and_then(|v| v.trim().parse::<u32>().ok())).unwrap_or(u32::MAX);
    eprintln!("regra do dono na função (Node): {} testes, {} passaram, {} falharam", n("tests"), n("pass"), n("fail"));
    assert!(out.status.success() && n("fail") == 0 && n("pass") >= 9 && n("pass") == n("tests"), "{txt}\n{}", String::from_utf8_lossy(&out.stderr));
}

/// Dono fixo de ponta a ponta contra o `livekit-server --dev` real: a marca de dono vai no token (aqui emitido pelo teste com as
/// chaves de dev, no mesmo formato da função), o servidor a repassa a todos e a devolve em ListParticipants. A dona marcada entra
/// POR ÚLTIMO (pelo relógio do servidor e pelo joinedAt dos apps a mais antiga é outra pessoa): ainda assim é ela que os apps
/// reconhecem, é só ela que silencia e expulsa, e continua sendo depois de o servidor morrer e voltar duas vezes.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn sfu_owner_is_the_one_marked_in_the_token_even_after_restarts() {
    use crate::tests_sfurec::client_owner;
    use std::sync::atomic::Ordering;
    const OA: &str = "aaaaaaaa-1111-4111-8111-00000000d0a1";
    const OB: &str = "bbbbbbbb-2222-4222-8222-00000000d0b2";
    const OD: &str = "dddddddd-3333-4333-8333-00000000d0d3";
    let Some(srv) = start_server() else { eprintln!("PULADO: BRIKS_LIVEKIT_SERVER_BIN não definido"); return };
    let mut srv = Some(srv);
    let room = format!("dono-{}", uuid::Uuid::new_v4().simple());
    let lk_room = format!("room:{}", &secure_room_id("room", &room)[..32]);
    // Beto entra primeiro (o mais antigo para o servidor e para os apps), depois a Duda; a Ana, marcada, entra por último
    let b = client_owner(&room, OB, "Beto", 1000, 3600, false).await;
    tokio::time::sleep(Duration::from_millis(400)).await;
    let d = client_owner(&room, OD, "Duda", 1001, 3600, false).await;
    for (x, o) in [(&b, &d), (&d, &b)] { assert!(x.wait(30, &format!("info:{}:{}", o.ident, o.fp)).await, "{} não autenticou {}", x.ident, o.ident); }
    // sem ninguém marcado vale o mais antigo: o Beto
    assert!(b.wait(10, &format!("owner:true:{}", b.fp)).await && d.wait(10, &format!("owner:false:{}", b.fp)).await, "sem marca, o mais antigo devia ser o dono");
    tokio::time::sleep(Duration::from_millis(400)).await;
    let a = client_owner(&room, OA, "Ana", 1002, 3600, true).await;
    let cs = [&a, &b, &d];
    for c in cs { for o in cs { if c.ident != o.ident { assert!(c.wait(30, &format!("info:{}:{}", o.ident, o.fp)).await, "{} não autenticou {}", c.ident, o.ident); } } }
    assert!(a.wait(10, &format!("owner:true:{}", a.fp)).await, "a marcada não se viu como dona");
    for c in [&b, &d] { assert!(c.wait(10, &format!("owner:false:{}", a.fp)).await, "{} não reconheceu a dona marcada", c.ident); }
    // só a dona silencia: o Beto (o mais antigo) é recusado, a Ana consegue
    let errs = b.count("err:");
    b.cmd.send(MeshCmd::RemoteMute { fp: d.fp.clone(), muted: true }).await.unwrap();
    tokio::time::sleep(Duration::from_millis(800)).await;
    assert!(b.count("err:") > errs && d.audio.mic_open(), "o mais antigo silenciou sem ser o dono");
    a.cmd.send(MeshCmd::RemoteMute { fp: d.fp.clone(), muted: true }).await.unwrap();
    let t0 = std::time::Instant::now();
    while d.audio.mic_open() && t0.elapsed() < Duration::from_secs(10) { tokio::time::sleep(Duration::from_millis(50)).await; }
    assert!(!d.audio.mic_open(), "a dona marcada não conseguiu silenciar");
    a.cmd.send(MeshCmd::RemoteMute { fp: d.fp.clone(), muted: false }).await.unwrap();

    // o servidor morre e volta, duas vezes: a dona continua a mesma, e em nenhum momento outro foi anunciado como dono
    let marks = |c: &crate::tests_sfurec::C| c.log.lock().unwrap().iter().filter(|s| s.starts_with("owner:")).cloned().collect::<Vec<_>>();
    let before: Vec<Vec<String>> = cs.iter().map(|c| marks(c)).collect();
    for round in 1..=2u64 {
        srv = None;
        for c in cs { assert!(c.wait(20, r#""state":"reconnecting""#).await, "{} não percebeu a queda", c.ident); }
        tokio::time::sleep(Duration::from_secs(1)).await;
        srv = Some(start_server().expect("reiniciar o livekit-server"));
        for c in cs {
            let end = std::time::Instant::now() + Duration::from_secs(40);
            while std::time::Instant::now() < end && c.stats.reconnects.load(Ordering::Relaxed) < round { tokio::time::sleep(Duration::from_millis(50)).await; }
            assert_eq!(c.stats.reconnects.load(Ordering::Relaxed), round, "{} não reconectou", c.ident);
        }
        tokio::time::sleep(Duration::from_secs(4)).await;
    }
    for (c, was) in cs.iter().zip(&before) { assert_eq!(&marks(c), was, "{}: o dono mudou durante as reconexões", c.ident); }
    // e a dona ainda manda depois de voltar
    a.cmd.send(MeshCmd::RemoteMute { fp: b.fp.clone(), muted: true }).await.unwrap();
    let t0 = std::time::Instant::now();
    while b.audio.mic_open() && t0.elapsed() < Duration::from_secs(15) { tokio::time::sleep(Duration::from_millis(50)).await; }
    assert!(!b.audio.mic_open(), "a dona não conseguiu silenciar depois das reconexões");

    // o servidor de mídia concorda: ListParticipants traz a marca, e a função de expulsão só atende a dona marcada
    let lp = { let (r, t) = (lk_room.clone(), admin_token(&lk_room)); tokio::task::spawn_blocking(move || ureq::post(&format!("http://127.0.0.1:{}/twirp/livekit.RoomService/ListParticipants", crate::tests_sfu::lk_port())).set("authorization", &format!("Bearer {t}")).send_json(json!({ "room": r })).unwrap().into_json::<serde_json::Value>().unwrap()).await.unwrap() };
    let meta = |id: &str| lp["participants"].as_array().and_then(|ps| ps.iter().find(|p| p["identity"] == id)).map(|p| p["metadata"].as_str().unwrap_or("").to_string()).unwrap_or_default();
    assert!(meta(OA).contains("\"owner\":true") && meta(OB).is_empty() && meta(OD).is_empty(), "ListParticipants sem a marca esperada: {lp}");
    match std::env::var("BRIKS_NODE_BIN").ok().filter(|p| std::path::Path::new(p).exists()) {
        Some(node) => {
            let (n2, r2) = (node.clone(), lk_room.clone());
            let (st, body) = tokio::task::spawn_blocking(move || kick_function(&n2, &r2, OB, OD)).await.unwrap();
            eprintln!("função como Beto (o mais antigo, sem marca) expulsando a Duda: {st} {body}");
            assert_eq!(st, 403, "{body}");
            let (n2, r2) = (node, lk_room.clone());
            let (st, body) = tokio::task::spawn_blocking(move || kick_function(&n2, &r2, OA, OD)).await.unwrap();
            eprintln!("função como Ana (dona marcada, a última a entrar) expulsando a Duda: {st} {body}");
            assert_eq!(st, 200, "{body}");
            assert!(d.wait(15, "kicked:").await, "a expulsa não recebeu o aviso do servidor");
        }
        None => eprintln!("BRIKS_NODE_BIN não definido: a função de expulsão com dono marcado NÃO foi exercitada contra o servidor"),
    }
    // a dona sai de vez: sem marcado na sala, o mais antigo (Beto) passa a ser o dono
    a.cmd.send(MeshCmd::Close).await.unwrap();
    assert!(b.wait(40, &format!("peer:{OA}:fechado")).await, "a saída da dona não foi vista");
    assert!(b.wait(10, &format!("owner:true:{}", b.fp)).await, "sem a dona marcada, o mais antigo devia assumir");
    eprintln!("dono fixo: marcado no token vence o mais antigo, sobrevive a 2 reinícios do servidor, vale na função de expulsão, e o mais antigo assume quando ele sai");
    for c in [&b, &d] { let _ = c.cmd.send(MeshCmd::Close).await; }
    tokio::time::sleep(Duration::from_millis(300)).await;
    drop(srv);
}
