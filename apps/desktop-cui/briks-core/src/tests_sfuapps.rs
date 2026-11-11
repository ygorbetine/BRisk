//! O que a malha P2P faz além de voz e chat, agora pela camada de dados do SFU, com 3 clientes contra um `livekit-server --dev` real:
//! "digitando", confirmação de entrega, manifesto de grupo (só para membros; forjado é recusado), histórico de DMs entre os dois lados
//! (sem forjados nem outras conversas), volume por pessoa e anexos (oferta assinada, aceitar, progresso, conteúdo idêntico, envio
//! dirigido, limite de tamanho e falha visível quando quem recebe sai). Sem GPU, sem janela. Precisa de `BRIKS_LIVEKIT_SERVER_BIN`.
use std::time::{Duration, Instant};

use serde_json::Value;

use crate::identity::{self, ChatWire, Identity};
use crate::mesh::MeshCmd;
use crate::tests_sfurec::{client, start_server, C};

const A: &str = "aaaaaaaa-1111-4111-8111-0000000app01";
const B: &str = "bbbbbbbb-2222-4222-8222-0000000app02";
const D: &str = "dddddddd-3333-4333-8333-0000000app03";

/// Eventos de anexo (`file:{json}`) de um cliente que satisfazem `f`.
fn files(c: &C, f: impl Fn(&Value) -> bool) -> Vec<Value> {
    c.log.lock().unwrap().iter().filter_map(|s| s.strip_prefix("file:")).filter_map(|j| serde_json::from_str::<Value>(j).ok()).filter(|v| f(v)).collect()
}
async fn wait_file(c: &C, secs: u64, what: &str, f: impl Fn(&Value) -> bool) -> Value {
    let end = Instant::now() + Duration::from_secs(secs);
    loop {
        if let Some(v) = files(c, &f).into_iter().next() { return v; }
        if Instant::now() > end { panic!("{}: não chegou o evento de anexo: {what}; últimos: {:?}", c.ident, c.log.lock().unwrap().iter().rev().take(5).map(|s| s.chars().take(160).collect::<String>()).collect::<Vec<_>>()); }
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
}
fn rms(x: &[f32]) -> f32 { (x.iter().map(|v| v * v).sum::<f32>() / x.len().max(1) as f32).sqrt() }

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn sfu_typing_group_history_volume_and_files_over_the_data_layer() {
    use crate::group_sync::{Channel, Member, Snapshot};
    let Some(_srv) = start_server() else { eprintln!("PULADO: BRIKS_LIVEKIT_SERVER_BIN não definido"); return };
    let dir = std::env::temp_dir().join(format!("briks-sfuapps-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir); std::fs::create_dir_all(dir.join("recebidos")).unwrap();
    std::env::set_var("BRIKS_RECV_DIR", dir.join("recebidos"));
    let room = format!("apps-{}", uuid::Uuid::new_v4().simple());
    let a = client(&room, A, "Ana", 1000, 3600).await;
    let b = client(&room, B, "Beto", 1001, 3600).await;
    let d = client(&room, D, "Duda", 1002, 3600).await;
    let cs = [&a, &b, &d];
    for c in cs { for o in cs { if c.ident != o.ident { assert!(c.wait(30, &format!("info:{}:{}", o.ident, o.fp)).await, "{} não autenticou {}", c.ident, o.ident); } } }

    // 1) digitando: chega a todos os autenticados, com a impressão digital de quem digita
    a.cmd.send(MeshCmd::Typing).await.unwrap();
    assert!(b.wait(10, &format!("typing:{}", a.fp)).await && d.wait(10, &format!("typing:{}", a.fp)).await, "digitando não chegou");

    // 1b) chat da chamada (para todos, autor = identidade provada) e pedido de transmissão com resposta
    b.cmd.send(MeshCmd::RoomChat { id: "sala-msg-0001".into(), text: "alguém me ouve?".into() }).await.unwrap();
    for c in [&a, &d] { assert!(c.wait(10, &format!("roomchat:{B}:{}:Beto:sala-msg-0001:alguém me ouve?", b.fp)).await, "{}: chat da chamada não chegou", c.ident); }
    assert_eq!(b.count("roomchat:"), 0, "eco do próprio chat da chamada");
    a.cmd.send(MeshCmd::StreamRequest { key: b.fp.clone() }).await.unwrap();
    assert!(b.wait(10, &format!("streamreq:{A}:Ana")).await, "pedido de transmissão não chegou");
    b.cmd.send(MeshCmd::StreamReply { key: a.fp.clone(), accepted: true }).await.unwrap();
    assert!(a.wait(10, &format!("streamreply:{B}:Beto:true")).await, "resposta ao pedido de transmissão não chegou");
    // resposta que ninguém pediu é ignorada
    d.cmd.send(MeshCmd::StreamReply { key: a.fp.clone(), accepted: true }).await.unwrap();
    tokio::time::sleep(Duration::from_millis(500)).await;
    assert_eq!(a.count("streamreply:") + d.count("streamreq:"), 1, "resposta não pedida passou");

    // 2) DM com o id da UI e confirmação de entrega (uma vez)
    b.cmd.send(MeshCmd::ChatWithId { to_fp: a.fp.clone(), text: "com id da UI".into(), id: "msg-ui-0001".into() }).await.unwrap();
    assert!(a.wait(10, &format!("chat:{}:com id da UI", b.fp)).await, "DM não chegou");
    assert!(b.wait(10, &format!("ack:{}:msg-ui-0001", a.fp)).await, "confirmação de entrega não chegou");
    tokio::time::sleep(Duration::from_secs(4)).await;   // passa pelos reenvios
    assert_eq!(b.count("ack:"), 1, "confirmação repetida");
    assert_eq!(d.count("chat:"), 0, "DM vazou");

    // 3) manifesto de grupo: só membros recebem; assinado por quem não é o remetente é recusado
    let mem = |i: &Identity, n: &str, admin: bool| Member { fp: i.fingerprint(), name: n.into(), admin, jwk: if admin { Some(i.public_jwk()) } else { None } };
    let mk = |by: &Identity, name: &str| {
        let mut s = Snapshot { v: 1, group_id: "g1".into(), name: name.into(), owner_fp: a.fp.clone(), version: 1, ts: 1,
            members: vec![mem(&a.id, "Ana", true), mem(&b.id, "Beto", true)],
            channels: vec![Channel { id: "c1".into(), name: "geral".into(), kind: "text".into() }], signer_fp: String::new(), signature: String::new() };
        crate::group_sync::sign(by, &mut s).unwrap();
        s
    };
    a.cmd.send(MeshCmd::GroupSend { snapshot: serde_json::to_value(mk(&a.id, "Grupo da Ana")).unwrap() }).await.unwrap();
    assert!(b.wait(10, &format!("group:{}:Grupo da Ana", a.fp)).await, "membro não recebeu o grupo");
    // a Duda (fora do grupo) repassa um manifesto assinado pela Ana como se fosse dela: os membros recusam
    d.cmd.send(MeshCmd::GroupSend { snapshot: serde_json::to_value(mk(&a.id, "Repassado")).unwrap() }).await.unwrap();
    assert!(b.wait(10, "err:grupo rejeitado").await, "manifesto repassado por terceiro devia ser recusado");
    tokio::time::sleep(Duration::from_millis(500)).await;
    assert_eq!(d.count("group:"), 0, "quem não é membro recebeu o grupo");
    assert_eq!(b.count("group:"), 1);

    // 4) histórico de DMs: o Beto pede à Ana o que falta da conversa dos dois
    let ch = identity::dm_channel(&a.fp, &b.fp);
    let msg = |who: &Identity, ch: &str, n: u64, text: &str| identity::sign_message(who, ChatWire { version: 2, kind: "chat.message".into(), channel_id: ch.into(), id: format!("hist-{n:08}"), author_peer_id: who.fingerprint(), author: "x".into(), content: text.into(), timestamp: 1_700_000_000_000 + n, signature: String::new() });
    let (m1, m2, m3) = (msg(&a.id, &ch, 1, "oi, sou a Ana"), msg(&a.id, &ch, 2, "segunda da Ana"), msg(&b.id, &ch, 3, "resposta do Beto, escrita antes"));
    let mut forged = msg(&a.id, &ch, 4, "o Beto nunca disse isto"); forged.author_peer_id = b.fp.clone();
    let other = msg(&a.id, &identity::dm_channel(&a.fp, &d.fp), 5, "conversa com a Duda");
    // 60 mensagens longas: o lote tem de ser repartido para caber nos pacotes de dados
    let many: Vec<ChatWire> = (100..160).map(|n| msg(&a.id, &ch, n, &"texto comprido ".repeat(40))).collect();
    let mut seed = vec![m1, m2.clone(), m3, forged, other]; seed.extend(many);
    a.cmd.send(MeshCmd::HistSeed { msgs: seed }).await.unwrap();
    b.cmd.send(MeshCmd::HistSeed { msgs: vec![m2] }).await.unwrap();
    b.cmd.send(MeshCmd::HistAsk).await.unwrap();
    assert!(b.wait(15, &format!("hist:hist-00000001:{}:false:oi, sou a Ana", a.fp)).await, "histórico da Ana não chegou");
    assert!(b.wait(15, &format!("hist:hist-00000003:{}:true:", b.fp)).await, "a minha mensagem antiga não voltou como minha");
    assert!(b.wait(15, "hist:hist-00000159:").await, "histórico grande não chegou inteiro");
    tokio::time::sleep(Duration::from_millis(500)).await;
    // 62 novas + a "com id da UI" trocada no passo 2 (que o Beto já tinha) = nada duplicado, nada forjado, nada de outra conversa
    assert_eq!(b.count("hist:hist-"), 62, "quantidade de histórico errada");
    assert_eq!(b.count("hist:hist-00000002"), 0, "voltou o que eu já tinha");
    assert_eq!(b.count("nunca disse") + b.count("conversa com a Duda") + d.count("hist:"), 0, "forjada ou de outra conversa passou");

    // 5) volume por pessoa: o Beto zera a Ana e a Duda (pela impressão digital, como a UI manda) e a mixagem dele fica muda
    let mut buf = vec![0.0f32; 4800];
    tokio::time::sleep(Duration::from_millis(600)).await;
    b.audio.mix_pull(&mut buf);
    let loud = rms(&buf);
    b.cmd.send(MeshCmd::PeerVolume { key: a.fp.clone(), gain: 0.0 }).await.unwrap();
    b.cmd.send(MeshCmd::PeerVolume { key: d.fp.clone(), gain: 0.0 }).await.unwrap();
    tokio::time::sleep(Duration::from_millis(600)).await;
    b.audio.mix_pull(&mut buf);   // um só: dois seguidos esvaziam o buffer e o segundo sai mudo por falta de áudio, não por volume
    let quiet = rms(&buf);
    b.cmd.send(MeshCmd::PeerVolume { key: a.fp.clone(), gain: 1.0 }).await.unwrap();
    tokio::time::sleep(Duration::from_millis(600)).await;
    b.audio.mix_pull(&mut buf);   // um só: dois seguidos esvaziam o buffer e o segundo sai mudo por falta de áudio, não por volume
    let back = rms(&buf);
    eprintln!("volume por pessoa: mixagem do Beto rms {loud:.3} -> {quiet:.4} (todos em zero) -> {back:.3} (Ana de volta)");
    assert!(loud > 0.05 && quiet < 0.005 && back > 0.05, "volume por pessoa não mudou a mixagem");

    // 6) anexo para todos: 3 MB de bytes variados; o Beto aceita, a Duda só vê a oferta
    let src = dir.join("relatorio final.bin");
    let bytes: Vec<u8> = { let mut x = 0x2545F491u32; (0..3 * 1024 * 1024).map(|_| { x ^= x << 13; x ^= x >> 17; x ^= x << 5; x as u8 }).collect() };
    std::fs::write(&src, &bytes).unwrap();
    let t0 = Instant::now();
    a.cmd.send(MeshCmd::SendFile { path: src.to_string_lossy().into(), to_fp: None }).await.unwrap();
    let off = wait_file(&b, 15, "oferta", |v| v["kind"] == "offer" && v["mine"] == false).await;
    assert_eq!((off["name"].as_str(), off["size"].as_u64(), off["peer_fp"].as_str()), (Some("relatorio final.bin"), Some(bytes.len() as u64), Some(a.fp.as_str())));
    wait_file(&d, 15, "oferta para a Duda", |v| v["kind"] == "offer" && v["mine"] == false).await;
    let id = off["id"].as_str().unwrap().to_string();
    assert!(std::fs::read_dir(dir.join("recebidos")).unwrap().next().is_none(), "gravou antes de o usuário aceitar");
    b.cmd.send(MeshCmd::AcceptFile { id: id.clone() }).await.unwrap();
    let done = wait_file(&b, 90, "anexo concluído", |v| v["kind"] == "done" && v["mine"] == false && v["id"] == id.as_str()).await;
    let secs = t0.elapsed().as_secs_f64();
    assert_eq!(std::fs::read(done["path"].as_str().unwrap()).unwrap(), bytes, "o arquivo recebido difere do enviado");
    assert!(files(&b, |v| v["kind"] == "progress" && v["mine"] == false).len() >= 5, "sem progresso para quem recebe");
    wait_file(&a, 10, "concluído para quem enviou", |v| v["kind"] == "done" && v["mine"] == true && v["peer_fp"] == b.fp.as_str()).await;
    assert!(files(&a, |v| v["kind"] == "progress" && v["mine"] == true).len() >= 5, "sem progresso para quem envia");
    assert!(files(&d, |v| v["kind"] == "done" || v["kind"] == "progress").is_empty(), "quem não aceitou recebeu dados");
    eprintln!("anexo de 3 MB pelo SFU: {:.1} s ({:.0} kB/s), conteúdo idêntico", secs, bytes.len() as f64 / 1024.0 / secs);

    // 7) envio dirigido: só a Duda vê a oferta
    let small = dir.join("so-para-a-duda.txt");
    std::fs::write(&small, b"segredo da Duda").unwrap();
    let nb = files(&b, |v| v["kind"] == "offer").len();
    a.cmd.send(MeshCmd::SendFile { path: small.to_string_lossy().into(), to_fp: Some(d.fp.clone()) }).await.unwrap();
    let off = wait_file(&d, 15, "oferta dirigida", |v| v["kind"] == "offer" && v["name"] == "so-para-a-duda.txt").await;
    d.cmd.send(MeshCmd::AcceptFile { id: off["id"].as_str().unwrap().into() }).await.unwrap();
    let done = wait_file(&d, 30, "anexo dirigido concluído", |v| v["kind"] == "done" && v["name"] == "so-para-a-duda.txt").await;
    assert_eq!(std::fs::read(done["path"].as_str().unwrap()).unwrap(), b"segredo da Duda");
    assert_eq!(files(&b, |v| v["kind"] == "offer").len(), nb, "oferta dirigida vazou para outro participante");

    // 8) limite de tamanho: acima de 100 MB nem é oferecido, com erro visível para quem tentou
    let big = dir.join("grande.bin");
    std::fs::File::create(&big).unwrap().set_len(crate::sfufiles::SFU_MAX_FILE_BYTES + 1).unwrap();
    a.cmd.send(MeshCmd::SendFile { path: big.to_string_lossy().into(), to_fp: None }).await.unwrap();
    let f = wait_file(&a, 30, "falha por tamanho", |v| v["kind"] == "failed" && v["id"] == "").await;
    eprintln!("acima do limite: {}", f["error"]);
    tokio::time::sleep(Duration::from_millis(500)).await;
    assert_eq!(files(&b, |v| v["name"] == "grande.bin").len(), 0);

    // 9) falha visível: quem recebe sai no meio; quem envia é avisado e o pedaço gravado some
    let mid = dir.join("interrompido.bin");
    std::fs::write(&mid, &bytes).unwrap();
    a.cmd.send(MeshCmd::SendFile { path: mid.to_string_lossy().into(), to_fp: Some(b.fp.clone()) }).await.unwrap();
    let off = wait_file(&b, 15, "oferta do interrompido", |v| v["kind"] == "offer" && v["name"] == "interrompido.bin").await;
    let id = off["id"].as_str().unwrap().to_string();
    b.cmd.send(MeshCmd::AcceptFile { id: id.clone() }).await.unwrap();
    wait_file(&b, 30, "1º progresso", |v| v["kind"] == "progress" && v["id"] == id.as_str()).await;
    b.cmd.send(MeshCmd::Close).await.unwrap();
    let f = wait_file(&a, 30, "falha avisada a quem envia", |v| v["kind"] == "failed" && v["id"] == id.as_str()).await;
    assert!(f["error"].as_str().unwrap().contains("saiu"), "{f}");
    assert!(files(&a, |v| v["kind"] == "done" && v["id"] == id.as_str()).is_empty(), "transferência interrompida dada como concluída");
    eprintln!("anexos: dirigido, limite de 100 MB e saída de quem recebe no meio ({}) conferidos", f["error"]);
    for c in [&a, &d] { let _ = c.cmd.send(MeshCmd::Close).await; }
    tokio::time::sleep(Duration::from_millis(300)).await;
    let _ = std::fs::remove_dir_all(&dir);
}
