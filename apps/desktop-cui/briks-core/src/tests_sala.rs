//! Chat da sala e pedido de transmissão entre duas malhas reais em loopback (regras puras em roomchat.rs).
use super::*;

const SA: &str = "aaaaaaaa-5a1a-4111-8111-0000000005a1";
const SB: &str = "bbbbbbbb-5a1a-4222-8222-0000000005b2";

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn room_chat_reaches_authenticated_peers_with_proven_author() {
    let port = crate::mock::start(0).await.unwrap();
    let (ia, ib) = (Arc::new(Identity::generate()), Arc::new(Identity::generate()));
    let mut a = node(port, SA, "Ana", "sala-chat", ia.clone());
    let mut b = node(port, SB, "Beto", "sala-chat", ib.clone());
    assert!(authed(&mut a, 1, 30).await && authed(&mut b, 1, 30).await, "não autenticaram");

    a.cmd.send(MeshCmd::RoomChat { id: "m-1".into(), text: "  oi, sala\n".into() }).await.unwrap();
    let got = wait_for(&mut b, 10, |e| matches!(e, MeshEvent::RoomChat { .. })).await.expect("B não recebeu o chat da sala");
    let MeshEvent::RoomChat { peer_id, from_fp, author, content, id, .. } = got else { unreachable!() };
    assert_eq!(peer_id, SA);
    assert_eq!(from_fp, ia.fingerprint(), "o autor tem de ser a identidade provada do canal");
    assert_eq!(author, "Ana");
    assert_eq!(content, "oi, sala", "texto não foi limpo");
    assert_eq!(id, "m-1");

    // o autor e o nome que vêm no JSON são ignorados; id repetido não aparece duas vezes; vazio e id inválido são descartados
    b.cmd.send(MeshCmd::RawDc { text: json!({ "version": 1, "type": "room.chat", "id": "m-2", "content": "sou a Ana", "author": "Ana", "from_fp": ia.fingerprint() }).to_string() }).await.unwrap();
    let got = wait_for(&mut a, 10, |e| matches!(e, MeshEvent::RoomChat { .. })).await.expect("A não recebeu");
    let MeshEvent::RoomChat { from_fp, author, .. } = got else { unreachable!() };
    assert_eq!((from_fp, author.as_str()), (ib.fingerprint(), "Beto"), "aceitou autor forjado");
    for bad in [json!({ "version": 1, "type": "room.chat", "id": "m-2", "content": "repetida" }), json!({ "version": 1, "type": "room.chat", "id": "m-3", "content": "   " }),
                json!({ "version": 1, "type": "room.chat", "id": "../m", "content": "id ruim" }), json!({ "version": 1, "type": "room.chat", "content": "sem id" })] {
        b.cmd.send(MeshCmd::RawDc { text: bad.to_string() }).await.unwrap();
    }
    assert!(wait_for(&mut a, 2, |e| matches!(e, MeshEvent::RoomChat { .. })).await.is_none(), "mensagem inválida ou repetida passou");

    // rajada: o limite por remetente segura o excesso (12 a cada 10 s; 1 já foi)
    for i in 0..40 { b.cmd.send(MeshCmd::RawDc { text: json!({ "version": 1, "type": "room.chat", "id": format!("r-{i}"), "content": "spam" }).to_string() }).await.unwrap(); }
    let mut n = 0;
    while wait_for(&mut a, 2, |e| matches!(e, MeshEvent::RoomChat { .. })).await.is_some() { n += 1; }
    assert!(n >= 1 && n <= 11, "limite de frequência não segurou: {n}");
    let _ = a.cmd.send(MeshCmd::Close).await;
    let _ = b.cmd.send(MeshCmd::Close).await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn stream_request_and_reply_only_between_authenticated_peers() {
    let port = crate::mock::start(0).await.unwrap();
    let (ia, ib) = (Arc::new(Identity::generate()), Arc::new(Identity::generate()));
    let (pa, pb) = ("aaaaaaaa-5a2a-4111-8111-0000000005a3", "bbbbbbbb-5a2a-4222-8222-0000000005b4");
    let mut a = node(port, pa, "Ana", "sala-pedido", ia.clone());
    let mut b = node(port, pb, "Beto", "sala-pedido", ib.clone());
    assert!(authed(&mut a, 1, 30).await && authed(&mut b, 1, 30).await, "não autenticaram");

    // resposta sem pedido: ignorada
    b.cmd.send(MeshCmd::StreamReply { key: pa.into(), accepted: true }).await.unwrap();
    assert!(wait_for(&mut a, 2, |e| matches!(e, MeshEvent::StreamReply { .. })).await.is_none(), "resposta sem pedido chegou à UI");

    // A pede pela impressão digital de B; B vê quem pediu e recusa; A recebe a recusa uma vez só
    a.cmd.send(MeshCmd::StreamRequest { key: ib.fingerprint() }).await.unwrap();
    let got = wait_for(&mut b, 10, |e| matches!(e, MeshEvent::StreamRequest { .. })).await.expect("B não viu o pedido");
    assert_eq!(got, MeshEvent::StreamRequest { peer_id: pa.into(), name: "Ana".into() });
    b.cmd.send(MeshCmd::StreamReply { key: pa.into(), accepted: false }).await.unwrap();
    let got = wait_for(&mut a, 10, |e| matches!(e, MeshEvent::StreamReply { .. })).await.expect("A não viu a resposta");
    assert_eq!(got, MeshEvent::StreamReply { peer_id: pb.into(), name: "Beto".into(), accepted: false });
    b.cmd.send(MeshCmd::StreamReply { key: pa.into(), accepted: true }).await.unwrap();
    assert!(wait_for(&mut a, 2, |e| matches!(e, MeshEvent::StreamReply { .. })).await.is_none(), "a resposta valeu duas vezes");

    // insistir em menos de 10 s não gera outro aviso do lado de lá
    a.cmd.send(MeshCmd::StreamRequest { key: pb.into() }).await.unwrap();
    assert!(wait_for(&mut b, 2, |e| matches!(e, MeshEvent::StreamRequest { .. })).await.is_none(), "pedido repetido passou do limite");

    // pedir a quem não está na sala: erro local, nada sai
    a.cmd.send(MeshCmd::StreamRequest { key: "ninguem".into() }).await.unwrap();
    assert!(wait_for(&mut a, 5, |e| matches!(e, MeshEvent::Error(m) if m.contains("não está na sala"))).await.is_some());
    let _ = a.cmd.send(MeshCmd::Close).await;
    let _ = b.cmd.send(MeshCmd::Close).await;
}
