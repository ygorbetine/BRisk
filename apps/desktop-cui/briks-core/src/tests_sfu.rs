//! Testes do modo SFU contra um `livekit-server --dev` REAL em loopback (sem GPU, sem janela, sem placa de som).
//! O binário vem de `BRIKS_LIVEKIT_SERVER_BIN` (release oficial do GitHub do livekit/livekit); sem ele os testes avisam e passam.
//! O token é assinado aqui com as chaves de DESENVOLVIMENTO do `--dev` (devkey/secret, públicas na documentação do LiveKit):
//! em produção o token vem da função `livekit-token` do Supabase e o segredo nunca está no cliente.
use std::process::{Child, Command, Stdio};
use std::sync::Arc;
use std::time::{Duration, Instant};

use base64::{engine::general_purpose::URL_SAFE_NO_PAD as B64, Engine};
use hmac::{Hmac, Mac};
use serde_json::json;
use sha2::Sha256;
use tokio::time::timeout;

use crate::audio::Audio;
use crate::mesh::{MeshCmd, MeshEvent};
use crate::sfu::{self, SfuHandle, SfuJoin};

struct Server { child: Child, port: u16 }
impl Drop for Server { fn drop(&mut self) { let _ = self.child.kill(); let _ = self.child.wait(); } }

fn start_server() -> Option<Server> {
    let bin = std::env::var("BRIKS_LIVEKIT_SERVER_BIN").ok()?;
    if !std::path::Path::new(&bin).exists() { return None; }
    let child = Command::new(bin).args(["--dev", "--bind", "127.0.0.1"]).stdout(Stdio::null()).stderr(Stdio::null()).spawn().ok()?;
    let port = 7880;
    // espera a porta HTTP abrir
    let t0 = Instant::now();
    while t0.elapsed() < Duration::from_secs(10) {
        if std::net::TcpStream::connect(("127.0.0.1", port)).is_ok() { return Some(Server { child, port }); }
        std::thread::sleep(Duration::from_millis(100));
    }
    let mut s = Server { child, port }; let _ = s.child.kill(); None
}

pub fn dev_token(identity: &str, room: &str) -> String {
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_secs();
    let head = B64.encode(br#"{"alg":"HS256","typ":"JWT"}"#);
    let body = B64.encode(json!({ "iss": "devkey", "sub": identity, "nbf": now - 10, "exp": now + 3600,
        "video": { "room": room, "roomJoin": true, "canPublish": true, "canSubscribe": true, "canPublishData": true } }).to_string());
    let mut m = Hmac::<Sha256>::new_from_slice(b"secret").unwrap();
    m.update(format!("{head}.{body}").as_bytes());
    format!("{head}.{body}.{}", B64.encode(m.finalize().into_bytes()))
}

async fn client(port: u16, room: &str, ident: &str, muted: bool) -> (SfuHandle, Arc<Audio>) {
    let (au, au_rx) = Audio::new(true);
    au.set_mic_muted(muted);
    let h = sfu::connect(SfuJoin { url: format!("ws://127.0.0.1:{port}"), token: dev_token(ident, room), audio: Some((au.clone(), au_rx)) }).await.expect("entrar no SFU");
    (h, au)
}

async fn wait_ev(h: &mut SfuHandle, mut pred: impl FnMut(&MeshEvent) -> bool, secs: u64) -> Option<MeshEvent> {
    let end = Instant::now() + Duration::from_secs(secs);
    loop {
        let left = end.saturating_duration_since(Instant::now());
        match timeout(left, h.events.recv()).await { Ok(Some(e)) => { if pred(&e) { return Some(e); } } _ => return None }
    }
}

const A: &str = "aaaaaaaa-1111-4111-8111-000000000001";
const B: &str = "bbbbbbbb-2222-4222-8222-000000000002";

/// Marco 1 e 2: entra com token, vê o outro participante, ouve áudio sintético (440 Hz), mede latência e CPU, estado de mudo.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn sfu_join_and_audio_between_two_clients() {
    let Some(srv) = start_server() else { eprintln!("PULADO: BRIKS_LIVEKIT_SERVER_BIN não definido"); return };
    let room = format!("room:{}", uuid::Uuid::new_v4().simple());
    // A entra mudo, para podermos medir o tempo entre abrir o microfone e o 1º quadro de áudio chegar ao B
    let (mut a, au_a) = client(srv.port, &room, A, true).await;
    let (mut b, au_b) = client(srv.port, &room, B, false).await;
    assert_eq!(a.identity, A);
    wait_ev(&mut b, |e| matches!(e, MeshEvent::PeerInfo { peer_id, .. } if peer_id == A), 15).await.expect("B não viu o A na lista de participantes");
    wait_ev(&mut a, |e| matches!(e, MeshEvent::PeerInfo { peer_id, .. } if peer_id == B), 15).await.expect("A não viu o B");
    // espera as conexões de mídia (subscriber de B e publisher de A)
    let t0 = Instant::now();
    while t0.elapsed() < Duration::from_secs(15) && !(a.stats.pub_connected.load(std::sync::atomic::Ordering::Relaxed) && b.stats.sub_connected.load(std::sync::atomic::Ordering::Relaxed)) { tokio::time::sleep(Duration::from_millis(50)).await; }
    assert!(a.stats.pub_connected.load(std::sync::atomic::Ordering::Relaxed), "publisher do A não conectou");
    assert!(b.stats.sub_connected.load(std::sync::atomic::Ordering::Relaxed), "subscriber do B não conectou");
    // B vê o A com microfone fechado (publicou mudo)
    assert!(au_b.rx_stat(A).frames == 0);
    // A abre o microfone: mede o tempo até o 1º quadro chegar ao B
    let t_open = Instant::now();
    au_a.set_mic_muted(false);
    a.cmd.send(MeshCmd::BroadcastState).await.unwrap();
    while t_open.elapsed() < Duration::from_secs(10) && au_b.rx_stat(A).frames == 0 { tokio::time::sleep(Duration::from_millis(2)).await; }
    let first = t_open.elapsed();
    assert!(au_b.rx_stat(A).frames > 0, "nenhum áudio chegou ao B");
    // 4 s de áudio: conta quadros, frequência e CPU do processo (dois clientes + servidor local fora do processo)
    let (c0, f0, s0) = (crate::share::cpu_ms(), au_b.rx_stat(A).frames, au_a.sent_frames.load(std::sync::atomic::Ordering::Relaxed));
    let w0 = Instant::now();
    tokio::time::sleep(Duration::from_secs(4)).await;
    let (cpu, wall) = (crate::share::cpu_ms() - c0, w0.elapsed().as_secs_f64() * 1000.0);
    let st = au_b.rx_stat(A);
    let frames = st.frames - f0;
    // o gerador de tom do teste anda no relógio do tokio (Windows: ~31 ms por tique), então o esperado é o que o A ENVIOU
    let sent = au_a.sent_frames.load(std::sync::atomic::Ordering::Relaxed) - s0;
    eprintln!("SFU áudio: 1º quadro {first:?} após abrir o mic; recebeu {frames} de {sent} quadros enviados em 4 s ({:.1}/s); freq {:.0} Hz; pico {:.2}; CPU do processo (2 clientes) {:.1}% de 1 núcleo", frames as f64 / 4.0, st.freq(), st.peak, cpu / wall * 100.0);
    assert!(frames * 100 >= sent * 97 && sent > 80, "perdeu áudio: {frames}/{sent}");
    assert!((st.freq() - 440.0).abs() < 25.0, "tom errado: {} Hz", st.freq());
    // mudo: B recebe o estado do microfone do A; ao fechar, o fluxo para
    au_a.set_mic_muted(true);
    a.cmd.send(MeshCmd::BroadcastState).await.unwrap();
    wait_ev(&mut b, |e| matches!(e, MeshEvent::PeerState { peer_id, microphone: false, .. } if peer_id == A), 10).await.expect("B não soube que o A mutou");
    tokio::time::sleep(Duration::from_millis(500)).await;
    let n1 = au_b.rx_stat(A).frames; tokio::time::sleep(Duration::from_secs(1)).await;
    let after = au_b.rx_stat(A).frames - n1;
    eprintln!("SFU mudo: {after} quadros/s depois de mutar (o LiveKit pode repor silêncio por um tempo)");
    assert!(after <= 50, "fluxo de áudio descontrolado: {after}/s");   // a prova do mudo é o PeerState acima; o SFU repõe silêncio em ~25 quadros/s
    // sair: o outro lado vê o participante sumir
    a.cmd.send(MeshCmd::Close).await.unwrap();
    wait_ev(&mut b, |e| matches!(e, MeshEvent::Peer { peer_id, state } if peer_id == A && state == "fechado"), 15).await.expect("B não viu a saída do A");
    b.cmd.send(MeshCmd::Close).await.unwrap();
}

/// Marco 3: vídeo de tela (fonte sintética, codificador de software: sem GPU) publicado por A e assistido por B, com parar/retomar.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn sfu_screen_share_published_and_watched() {
    use crate::capture::{read_stamp, SourceKind};
    use crate::share::ShareCfg;
    use crate::video::FrameSource;
    let Some(srv) = start_server() else { eprintln!("PULADO: BRIKS_LIVEKIT_SERVER_BIN não definido"); return };
    // BRIKS_SFU_TEST_GPU=1 (só com o lock A:\BRiskCall\.gpu-test.lock): usa o codificador padrão (Media Foundation/NVENC) no lugar do software
    if std::env::var("BRIKS_SFU_TEST_GPU").is_err() { std::env::set_var("BRIKS_ENCODER", "openh264"); }
    std::env::set_var("BRIKS_DECODER", "openh264");
    let room = format!("room:{}", uuid::Uuid::new_v4().simple());
    let (mut a, _au_a) = client(srv.port, &room, A, false).await;
    let (mut b, _au_b) = client(srv.port, &room, B, false).await;
    wait_ev(&mut b, |e| matches!(e, MeshEvent::PeerInfo { peer_id, .. } if peer_id == A), 15).await.expect("B não viu o A");
    let dir = std::env::temp_dir().join(format!("briks-sfu-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir); std::fs::create_dir_all(&dir).unwrap();
    let (w, h) = (1280usize, 720usize);
    for round in 0..2 {
        a.cmd.send(MeshCmd::ShareStart { cfg: ShareCfg { kind: SourceKind::Test, height: 720, fps: 30, kbps: 0, label: "Teste".into(), audio: crate::procaudio::AudioMode::None, content: crate::adapt::Content::Detail, adaptive: false }, preview: None, dump: None }).await.unwrap();
        if let Some(MeshEvent::Share(v)) = wait_ev(&mut a, |e| matches!(e, MeshEvent::Share(v) if v["ev"] == "share.local" && v["state"] == "on"), 20).await { eprintln!("SFU codificador: {} (hardware: {})", v["encoder"], v["hardware"]); }
        let on = wait_ev(&mut b, |e| matches!(e, MeshEvent::Share(v) if v["ev"] == "share.remote" && v["state"] == "on"), 20).await.expect("B não soube da transmissão");
        if let MeshEvent::Share(v) = on { assert_eq!(v["label"], "Teste"); }
        let rpath = dir.join(format!("recebido-{round}.bin"));
        b.cmd.send(MeshCmd::ShareWatch { key: A.into(), on: true, path: rpath.clone() }).await.unwrap();
        let fr = wait_ev(&mut b, |e| matches!(e, MeshEvent::Share(v) if v["ev"] == "share.remote" && v["state"] == "frame"), 25).await.expect("nenhum quadro chegou ao B");
        if let MeshEvent::Share(v) = fr { assert_eq!((v["w"].as_u64(), v["h"].as_u64()), (Some(w as u64), Some(h as u64))); }
        let src = FrameSource::open(&rpath).unwrap();
        let (mut last_n, mut lats) = (0u64, vec![]);
        let t0 = Instant::now();
        let (c0, wall0) = (crate::share::cpu_ms(), Instant::now());
        while t0.elapsed() < Duration::from_secs(4) {
            if let Some((rw, rh, n, rgba)) = src.read() {
                if n != last_n && rw == w && rh == h { last_n = n; lats.push((src.ts() as u32).wrapping_sub(read_stamp(&rgba, w)) as i32); }
            }
            tokio::time::sleep(Duration::from_millis(8)).await;
        }
        let cpu = (crate::share::cpu_ms() - c0) / (wall0.elapsed().as_secs_f64() * 1000.0) * 100.0;
        lats.sort();
        let (n, p50, p95) = (lats.len(), lats[lats.len() / 2], lats[lats.len() * 95 / 100]);
        eprintln!("SFU vídeo, rodada {round}: {n} quadros em 4 s ({:.1} fps), latência captura→quadro p50 {p50} ms p95 {p95} ms; CPU do processo (codificação + decodificação software + 2 clientes) {cpu:.0}% de 1 núcleo", n as f64 / 4.0);
        assert!(n >= 12, "poucos quadros: {n}");   // só prova o fluxo; o desempenho se mede em --release
        // parar: B é avisado
        a.cmd.send(MeshCmd::ShareStop).await.unwrap();
        wait_ev(&mut b, |e| matches!(e, MeshEvent::Share(v) if v["ev"] == "share.remote" && v["state"] == "off"), 15).await.expect("B não soube que parou");
    }
    use std::sync::atomic::Ordering::Relaxed;
    let (rr, rtt, loss, nk, pl) = (a.stats.rr.load(Relaxed), a.stats.rtt_us.load(Relaxed), a.stats.loss_permille.load(Relaxed), a.stats.nacks.load(Relaxed), a.stats.plis.load(Relaxed));
    eprintln!("SFU feedback ao publisher (alimenta o adapt.rs): {rr} Receiver Reports, RTT {:.1} ms, perda {loss}‰, {nk} NACKs, {pl} PLIs", rtt as f64 / 1000.0);
    assert!(rr > 0, "o SFU não mandou Receiver Reports: o adapt.rs ficaria cego");
    a.cmd.send(MeshCmd::Close).await.unwrap();
    b.cmd.send(MeshCmd::Close).await.unwrap();
}

/// Token errado / servidor inexistente: falha rápido com erro legível (para o Automático cair no P2P) e sem vazar o token.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn sfu_connect_failures_are_fast_and_do_not_leak_the_token() {
    // nada ouvindo nessa porta
    let t0 = Instant::now();
    let r = sfu::connect(SfuJoin { url: "ws://127.0.0.1:9".into(), token: "eyJsegredo.abc.def".into(), audio: None }).await;
    let e = r.err().expect("devia falhar").to_string();
    assert!(!e.contains("eyJsegredo"), "{e}");
    assert!(t0.elapsed() < Duration::from_secs(12));
    let Some(srv) = start_server() else { eprintln!("PULADO (servidor): BRIKS_LIVEKIT_SERVER_BIN não definido"); return };
    let r = sfu::connect(SfuJoin { url: format!("ws://127.0.0.1:{}", srv.port), token: "eyJsegredo.abc.def".into(), audio: None }).await;
    let e = r.err().expect("token inválido devia falhar").to_string();
    assert!(!e.contains("eyJsegredo"), "{e}");
    eprintln!("erro com token inválido: {e}");
}

/// Serviço de tokens simulado (HTTP em loopback): confere o cabeçalho Bearer e o corpo, devolve um token de dev ou 401.
fn token_service(expect_bearer: &'static str, room: String) -> u16 {
    use std::io::{Read, Write};
    let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let port = l.local_addr().unwrap().port();
    std::thread::spawn(move || {
        for s in l.incoming().take(2) {
            let mut s = s.unwrap();
            let mut buf = vec![0u8; 8192];
            let mut n = 0;
            // lê até ter cabeçalho + corpo (o corpo é pequeno)
            while n < buf.len() { let r = s.read(&mut buf[n..]).unwrap_or(0); if r == 0 { break; } n += r; if String::from_utf8_lossy(&buf[..n]).contains("\"room\"") { break; } }
            let req = String::from_utf8_lossy(&buf[..n]).to_string();
            let ok = req.to_ascii_lowercase().contains(&format!("authorization: bearer {expect_bearer}")) && req.contains(&room);
            let body = if ok { json!({ "token": dev_token(A, &room), "ttl": 3600 }).to_string() } else { json!({ "message": "Entre na sua conta Briks para usar o servidor de chamadas." }).to_string() };
            let _ = write!(s, "HTTP/1.1 {}\r\ncontent-type: application/json\r\ncontent-length: {}\r\nconnection: close\r\n\r\n{}", if ok { "200 OK" } else { "401 Unauthorized" }, body.len(), body);
        }
    });
    port
}

/// Fluxo do app: sessão do usuário -> serviço de tokens (Bearer) -> token -> entrada no SFU real. Sem sessão válida: 401 legível.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn sfu_token_service_flow_with_user_session() {
    let Some(srv) = start_server() else { eprintln!("PULADO: BRIKS_LIVEKIT_SERVER_BIN não definido"); return };
    let room = format!("room:{}", uuid::Uuid::new_v4().simple());
    let port = token_service("sessao-do-usuario", room.clone());
    let url = format!("http://127.0.0.1:{port}/functions/v1/livekit-token");
    let (u2, r2) = (url.clone(), room.clone());
    let token = tokio::task::spawn_blocking(move || sfu::fetch_token(&u2, "sessao-do-usuario", &r2)).await.unwrap().expect("token com sessão válida");
    let h = sfu::connect(SfuJoin { url: format!("ws://127.0.0.1:{}", srv.port), token, audio: None }).await.expect("entrar com o token do serviço");
    assert_eq!(h.identity, A);
    h.cmd.send(MeshCmd::Close).await.unwrap();
    let (u3, r3) = (url, room);
    let err = tokio::task::spawn_blocking(move || sfu::fetch_token(&u3, "sessao-invalida", &r3)).await.unwrap().err().expect("sem sessão válida deve recusar");
    eprintln!("sem sessão válida: {err}");
    assert!(err.contains("401") && err.contains("Entre na sua conta"), "{err}");
    // http fora de loopback é recusado antes de qualquer envio do token
    assert!(sfu::fetch_token("http://exemplo.com/t", "x", "room:a").is_err());
}

/// IPC: Automático com SFU configurado mas sem conta/servidor cai no P2P e diz por quê; "P2P" nem tenta; `net.mode.set` persiste.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn ipc_net_mode_auto_falls_back_to_p2p_and_reports_why() {
    use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
    let dir = std::env::temp_dir().join(format!("briks-netmode-ipc-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir); std::fs::create_dir_all(&dir).unwrap();
    std::fs::write(dir.join("livekit.json"), r#"{"url":"ws://127.0.0.1:9","token_url":"http://127.0.0.1:1/livekit-token"}"#).unwrap();
    let rt_port = crate::mock::start(0).await.unwrap();
    let (l, port) = crate::ipc::bind().await.unwrap();
    let cfg = crate::ipc::Config { data_dir: dir.clone(), realtime_url: Some(format!("ws://127.0.0.1:{rt_port}")), realtime_key: Some("k".into()) };
    tokio::spawn(crate::ipc::serve(l, "segredo".into(), cfg));
    let s = tokio::net::TcpStream::connect(("127.0.0.1", port)).await.unwrap();
    let (rd, mut wr) = s.into_split();
    let mut lines = BufReader::new(rd).lines();
    async fn until(lines: &mut tokio::io::Lines<BufReader<tokio::net::tcp::OwnedReadHalf>>, ev: &str) -> serde_json::Value {
        loop {
            let l = timeout(Duration::from_secs(15), lines.next_line()).await.expect("tempo").expect("io").expect("fim");
            let v: serde_json::Value = serde_json::from_str(&l).unwrap();
            if v["ev"] == ev { return v; }
        }
    }
    wr.write_all(b"{\"cmd\":\"hello\",\"token\":\"segredo\"}\n").await.unwrap();
    until(&mut lines, "hello.ok").await;
    wr.write_all(b"{\"cmd\":\"session.start\",\"display_name\":\"T\"}\n").await.unwrap();
    until(&mut lines, "session.ready").await;
    // automático: tenta o SFU, sem conta falha e cai no P2P
    wr.write_all(b"{\"cmd\":\"room.join\",\"namespace\":\"room\",\"room\":\"sala-a\",\"net\":\"sfu\"}\n").await.unwrap();
    let nm = until(&mut lines, "net.mode").await;
    eprintln!("net.mode (sfu manual, sem conta): {nm}");
    assert_eq!((nm["mode"].as_str(), nm["fallback"].as_bool(), nm["configured"].as_bool()), (Some("p2p"), Some(true), Some(true)));
    assert!(nm["error"].as_str().unwrap().contains("conta"), "{nm}");
    until(&mut lines, "room.joining").await;
    // P2P escolhido: não tenta o SFU
    wr.write_all(b"{\"cmd\":\"room.join\",\"namespace\":\"room\",\"room\":\"sala-b\",\"net\":\"p2p\"}\n").await.unwrap();
    let nm = until(&mut lines, "net.mode").await;
    assert_eq!((nm["mode"].as_str(), nm["fallback"].as_bool()), (Some("p2p"), Some(false)));
    assert!(nm["reason"].as_str().unwrap().contains("escolhido"), "{nm}");
    // persistência da escolha
    wr.write_all(b"{\"cmd\":\"net.mode.set\",\"mode\":\"sfu\"}\n").await.unwrap();
    let c = until(&mut lines, "net.config").await;
    assert_eq!((c["mode"].as_str(), c["configured"].as_bool()), (Some("sfu"), Some(true)));
    assert_eq!(crate::sfu_cfg::load_mode(&|_| None, &dir), crate::sfu_cfg::NetMode::Sfu);
    let _ = std::fs::remove_dir_all(&dir);
}


/// Caminho completo do app: conta (Supabase simulado) -> access token da sessão -> serviço de tokens recebe `Bearer <sessão>`
/// -> token LiveKit -> entra no SFU real. É exatamente o que o `room.join` do IPC faz (`ipc::try_sfu`).
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn ipc_try_sfu_uses_the_account_session_end_to_end() {
    use crate::accounts::{fake, supabase::SupabaseProvider, Accounts, SignUpOutcome};
    let Some(srv) = start_server() else { eprintln!("PULADO: BRIKS_LIVEKIT_SERVER_BIN não definido"); return };
    let f = fake::start(0).await.unwrap();
    f.state.lock().unwrap().autoconfirm = true;
    let dir = std::env::temp_dir().join(format!("briks-sfu-acc-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir); std::fs::create_dir_all(&dir).unwrap();
    let acc = Accounts::new(Arc::new(SupabaseProvider::new(&f.url(), "chave-publica-de-teste")), &dir, fake::auto_browser());
    let SignUpOutcome::SignedIn(_) = acc.sign_up("sfu@exemplo.com", "senha-boa-1", "Sfu").await.unwrap() else { panic!("cadastro") };
    let session = acc.access_token().await.unwrap();
    let rt = crate::accounts::service::Runtime::for_tests(Some(Arc::new(acc)), dir.clone());
    // serviço de tokens que registra o cabeçalho recebido
    let seen = Arc::new(std::sync::Mutex::new(String::new()));
    let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let tport = l.local_addr().unwrap().port();
    let seen2 = seen.clone();
    std::thread::spawn(move || {
        use std::io::{Read, Write};
        let (mut s, _) = l.accept().unwrap();
        let mut buf = vec![0u8; 8192]; let mut n = 0;
        while n < buf.len() { let r = s.read(&mut buf[n..]).unwrap_or(0); if r == 0 { break; } n += r; if String::from_utf8_lossy(&buf[..n]).contains("\"room\"") { break; } }
        let req = String::from_utf8_lossy(&buf[..n]).to_string();
        let room = req.split("\"room\":\"").nth(1).and_then(|r| r.split('"').next()).unwrap_or("").to_string();
        *seen2.lock().unwrap() = req.lines().find(|l| l.to_ascii_lowercase().starts_with("authorization:")).unwrap_or("").to_string();
        let body = json!({ "token": dev_token(A, &room), "ttl": 3600 }).to_string();
        let _ = write!(s, "HTTP/1.1 200 OK\r\ncontent-type: application/json\r\ncontent-length: {}\r\nconnection: close\r\n\r\n{}", body.len(), body);
    });
    let sc = crate::sfu_cfg::SfuConfig { url: format!("ws://127.0.0.1:{}", srv.port), token_url: format!("http://127.0.0.1:{tport}/functions/v1/livekit-token") };
    let h = crate::ipc::try_sfu(&sc, &rt, "room", "sala-do-grupo", None).await.expect("entrar no SFU com a sessão da conta");
    assert_eq!(h.identity, A);
    let hdr = seen.lock().unwrap().clone();
    assert!(hdr.to_ascii_lowercase().ends_with(&format!("bearer {}", session.to_ascii_lowercase())), "o serviço de tokens não recebeu a sessão do usuário: {}", hdr.len());
    eprintln!("ipc::try_sfu: entrou com a sessão da conta (Bearer de {} caracteres), servidor {}", session.len(), h.server);
    h.cmd.send(MeshCmd::Close).await.unwrap();
    // sem conta: erro legível, sem tentar a rede
    let rt2 = crate::accounts::service::Runtime::for_tests(None, dir.clone());
    let e = crate::ipc::try_sfu(&sc, &rt2, "room", "x", None).await.err().expect("sem conta deve falhar");
    assert!(e.contains("conta"), "{e}");
    let _ = std::fs::remove_dir_all(&dir);
}
