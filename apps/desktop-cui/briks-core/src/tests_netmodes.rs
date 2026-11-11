//! Modo da chamada (Configurações > Rede) pelo IPC, sem LiveKit e sem Realtime: o que cada modo faz quando o servidor de chamadas
//! NÃO atende (serviço de tokens responde 401). P2P nem pede token; Automático e SFU pedem e, ao falhar, mostram o erro sem abrir sala
//! P2P à parte; sem SFU configurado o Automático segue para o P2P e o "Sempre SFU" dá erro. net.mode.set grava a escolha.
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;

use serde_json::json;

use crate::accounts::fake;
use crate::tests_sfuipc::app;

/// Serviço de tokens que só conta os pedidos e recusa (401).
fn counting_token_service(hits: Arc<AtomicUsize>) -> u16 {
    use std::io::{Read, Write};
    let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let port = l.local_addr().unwrap().port();
    std::thread::spawn(move || {
        for s in l.incoming() {
            let Ok(mut s) = s else { continue };
            let mut buf = vec![0u8; 8192];
            let _ = s.read(&mut buf);
            hits.fetch_add(1, Ordering::SeqCst);
            let body = json!({ "message": "recusado no teste" }).to_string();
            let _ = write!(s, "HTTP/1.1 401 Unauthorized\r\ncontent-type: application/json\r\ncontent-length: {}\r\nconnection: close\r\n\r\n{body}", body.len());
        }
    });
    port
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn call_modes_p2p_auto_sfu_with_and_without_the_server() {
    std::env::set_var("BRIKS_CORE_FAKE_AUDIO", "1");
    std::env::remove_var("BRIKS_NET_MODE");
    std::env::remove_var("BRIKS_NET_FALLBACK");
    let f = fake::start(0).await.unwrap();
    f.state.lock().unwrap().autoconfirm = true;
    let hits = Arc::new(AtomicUsize::new(0));
    let tport = counting_token_service(hits.clone());
    let (mut a, _s, dir) = app("modos", &f.url(), Some("modos@exemplo.com"), "Modos", tport).await;
    let join = |room: &str, net: &str| json!({ "cmd": "room.join", "namespace": "room", "room": room, "net": net });

    // Só P2P, mesmo com o SFU configurado: não pede token e não abre pelo servidor (sem Realtime aqui, o P2P para no aviso de sinalização)
    a.send(join("sala-p2p", "p2p")).await;
    a.wait(10, "P2P chegou à sinalização", |v| v["ev"] == "signaling.status" && v["state"] == "erro").await;
    assert_eq!(hits.load(Ordering::SeqCst), 0, "P2P não pede token ao servidor de chamadas");
    assert_eq!(a.count(|v| v["ev"] == "net.mode" && v["mode"] == "sfu"), 0);

    // Automático com o SFU configurado: pede o token; falhou = erro visível, nunca P2P sozinho
    a.send(join("sala-auto", "auto")).await;
    let nm = a.wait(20, "net.mode do Automático", |v| v["ev"] == "net.mode" && v["mode"] == "none").await;
    assert_eq!((nm["configured"].as_bool(), nm["fallback"].as_bool()), (Some(true), Some(false)));
    assert!(hits.load(Ordering::SeqCst) >= 1, "Automático tenta o SFU primeiro");

    // Sempre SFU: igual, e também não volta ao P2P
    let before = hits.load(Ordering::SeqCst);
    let is_err = |v: &serde_json::Value| v["ev"] == "error" && v["where"] == "room.join" && v["message"].as_str().map(|m| m.contains("servidor de chamadas")).unwrap_or(false);
    let errs0 = a.count(is_err);
    a.send(join("sala-sfu", "sfu")).await;
    for _ in 0..200 { if a.count(is_err) > errs0 { break; } tokio::time::sleep(std::time::Duration::from_millis(100)).await; }
    assert!(a.count(is_err) > errs0, "Sempre SFU não deu erro");
    assert!(hits.load(Ordering::SeqCst) > before);
    assert_eq!(a.count(|v| v["ev"] == "net.mode" && v["mode"] == "p2p"), 0, "nenhum modo caiu em P2P sozinho");

    // net.mode.set grava e devolve net.config
    a.send(json!({ "cmd": "net.mode.set", "mode": "p2p", "pref": 1 })).await;
    let c = a.wait(10, "net.config", |v| v["ev"] == "net.config" && v["mode"] == "p2p").await;
    assert_eq!((c["configured"].as_bool(), c["pref"].as_u64()), (Some(true), Some(1)));
    assert!(std::fs::read_to_string(dir.join("net-mode.json")).unwrap().contains("p2p"));

    // Sem SFU configurado: "Sempre SFU" dá erro; Automático segue para o P2P (aqui, até o aviso de sinalização)
    std::fs::remove_file(dir.join("livekit.json")).unwrap();
    let n0 = a.count(|v| v["ev"] == "net.mode" && v["mode"] == "none");
    a.send(join("sala-sfu2", "sfu")).await;
    a.wait(10, "Sempre SFU sem servidor", |v| v["ev"] == "error" && v["message"].as_str().map(|m| m.contains("não está configurado")).unwrap_or(false)).await;
    assert_eq!(a.count(|v| v["ev"] == "net.mode" && v["mode"] == "none"), n0 + 1);
    let e0 = a.count(|v| v["ev"] == "signaling.status" && v["state"] == "erro");
    a.send(join("sala-auto2", "auto")).await;
    tokio::time::sleep(std::time::Duration::from_millis(800)).await;
    assert!(a.count(|v| v["ev"] == "signaling.status" && v["state"] == "erro") > e0, "Automático sem SFU configurado segue o caminho P2P");
}
