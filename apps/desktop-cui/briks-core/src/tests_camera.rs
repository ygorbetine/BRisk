//! Câmera na chamada, de ponta a ponta e SEM hardware: toda a captura vem de `BRIKS_FAKE_CAMERA` (nunca abre a webcam real).
//! Malha P2P real (webrtc-rs) em loopback: câmera de A para B (e de B para A), junto com a transmissão de tela, parar/religar,
//! avisos (sem câmera, acesso negado, em uso), luz da câmera (`camera::open_count`) e adaptação por perda com rede simulada.
use super::*;

use crate::camera;
use crate::capture::SourceKind;
use crate::video::FrameSource;

const CA: &str = "aaaaaaaa-ca11-4111-8111-0000000000a1";
const CB: &str = "bbbbbbbb-ca11-4222-8222-0000000000b2";

fn fake(mode: &str) { std::env::set_var("BRIKS_FAKE_CAMERA", mode); }

async fn wait_cam(h: &mut MeshHandle, pred: impl Fn(&serde_json::Value) -> bool, secs: u64) -> Option<serde_json::Value> {
    let end = std::time::Instant::now() + Duration::from_secs(secs);
    loop {
        let left = end.saturating_duration_since(std::time::Instant::now());
        match timeout(left, h.events.recv()).await { Ok(Some(MeshEvent::Share(v))) => { if pred(&v) { return Some(v); } } Ok(Some(_)) => {} _ => return None }
    }
}

async fn until_zero_open(secs: u64) -> bool {
    let end = std::time::Instant::now() + Duration::from_secs(secs);
    while std::time::Instant::now() < end { if camera::open_count() == 0 { return true; } tokio::time::sleep(Duration::from_millis(50)).await; }
    camera::open_count() == 0
}

/// Quadro RGBA recebido tem as marcas de canto da câmera falsa nos lugares certos (orientação/ordem de cores corretas).
fn corners_ok(rgba: &[u8], w: usize, h: usize) -> bool {
    let near = |x: usize, y: usize, want: (u8, u8, u8)| { let o = (y * w + x) * 4; let d = |a: u8, b: u8| (a as i32 - b as i32).abs(); d(rgba[o], want.0) < 60 && d(rgba[o + 1], want.1) < 60 && d(rgba[o + 2], want.2) < 60 };
    let m = (h / 9).max(8);
    let y0 = ((w / 40).max(4) + 2) + m / 2;
    // RGBA: vermelho (235,20,20) em cima à esquerda, azul (20,20,235) em cima à direita, verde (20,235,20) embaixo à esquerda
    near(m / 2, y0, (235, 20, 20)) && near(w - m / 2, y0, (20, 20, 235)) && near(m / 2, h - m / 2, (20, 235, 20))
}

#[test]
fn fake_camera_lists_and_reports_missing_denied_and_busy() {
    fake("1");
    let l = camera::list();
    assert_eq!(l.len(), 2);
    assert_eq!(camera::open_count(), 0, "listar não abre a câmera (a luz não acende)");
    for (mode, code) in [("none", "no_camera"), ("denied", "denied"), ("busy", "busy")] {
        fake(mode);
        let e = crate::capture::Capture::start(SourceKind::Camera(String::new()), 30, 360, false, false).err().expect("devia falhar").to_string();
        let (c, text) = camera::split_code(&e);
        assert_eq!(c, code, "{e}");
        assert!(!text.is_empty());
        assert_eq!(camera::open_count(), 0, "falhou ao abrir: nada pode ficar aberto");
    }
    // abre, entrega quadros, e fechar solta o dispositivo
    fake("1");
    let mut cap = crate::capture::Capture::start(SourceKind::Camera("fake:1".into()), 30, 360, true, false).expect("câmera falsa");
    assert_eq!(camera::open_count(), 1);
    std::thread::sleep(Duration::from_millis(600));
    assert!(cap.shared.frames.load(std::sync::atomic::Ordering::Relaxed) >= 5, "poucos quadros");
    let pv = cap.shared.latest.lock().unwrap().preview.take().expect("prévia");
    assert_eq!((pv.0, pv.1), (640, 480), "a câmera 2 é 640x480");
    cap.stop();
    assert_eq!(camera::open_count(), 0, "parar a captura tem de soltar a câmera");
    std::env::remove_var("BRIKS_FAKE_CAMERA");
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn camera_flows_both_ways_next_to_a_screen_share_and_stops_cleanly() {
    fake("1");
    std::env::set_var("BRIKS_ENCODER", "openh264");
    let port = crate::mock::start(0).await.unwrap();
    let mut a = node(port, CA, "Ana", "cam1", Arc::new(Identity::generate()));
    let mut b = node(port, CB, "Beto", "cam1", Arc::new(Identity::generate()));
    assert!(authed(&mut a, 1, 20).await && authed(&mut b, 1, 20).await, "não autenticou");
    let dir = std::env::temp_dir().join(format!("briks-cam-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir); std::fs::create_dir_all(&dir).unwrap();

    // 1) A liga a câmera de 1280x720: B recebe o aviso, assiste e vê 640x360 com as marcas de canto certas
    a.cmd.send(MeshCmd::CamStart { device: "fake:0".into(), preview: Some(dir.join("a-eu.bin")) }).await.unwrap();
    let local = wait_cam(&mut a, |v| v["ev"] == "cam.local" && v["state"] == "on", 20).await.expect("A não confirmou a câmera");
    assert_eq!((local["w"].as_u64(), local["h"].as_u64()), (Some(640), Some(360)), "o padrão é 640x360: {local}");
    assert_eq!(camera::open_count(), 1, "câmera ligada = luz acesa");
    let on = wait_cam(&mut b, |v| v["ev"] == "cam.remote" && v["state"] == "on", 20).await.expect("B não soube da câmera de A");
    assert_eq!((on["w"].as_u64(), on["h"].as_u64()), (Some(640), Some(360)));
    let rpath = dir.join("b-ve-a.bin");
    b.cmd.send(MeshCmd::CamWatch { key: CA.into(), on: true, path: rpath.clone() }).await.unwrap();
    let fr = wait_cam(&mut b, |v| v["ev"] == "cam.remote" && v["state"] == "frame", 25).await.expect("nenhum quadro da câmera chegou ao B");
    assert_eq!((fr["w"].as_u64(), fr["h"].as_u64()), (Some(640), Some(360)));
    tokio::time::sleep(Duration::from_secs(2)).await;
    let src = FrameSource::open(&rpath).unwrap();
    let (rw, rh, n, rgba) = src.read().expect("quadro");
    assert!(n > 20, "poucos quadros em 2 s: {n}");
    assert!(corners_ok(&rgba, rw, rh), "a imagem chegou com cores ou orientação erradas");
    // a prévia local (o bloco "Você") também existe e tem o mesmo tamanho de origem
    let (pw, ph, pn, _) = FrameSource::open(&dir.join("a-eu.bin")).unwrap().read().expect("prévia local");
    assert!(pn > 10 && (pw, ph) == (1280, 720), "prévia local {pw}x{ph} n={pn}");

    // 2) a transmissão de tela coexiste: dois tracks de vídeo de A, B assiste aos dois, cada um no seu arquivo
    a.cmd.send(MeshCmd::ShareStart { cfg: crate::share::ShareCfg { kind: SourceKind::Test, height: 720, fps: 30, kbps: 0, label: "Tela".into(), audio: crate::procaudio::AudioMode::None, content: crate::adapt::Content::Detail, adaptive: false }, preview: None, dump: None }).await.unwrap();
    wait_cam(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "on", 25).await.expect("B não soube da tela de A");
    let spath = dir.join("b-ve-tela-de-a.bin");
    b.cmd.send(MeshCmd::ShareWatch { key: CA.into(), on: true, path: spath.clone() }).await.unwrap();
    wait_cam(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "frame", 30).await.expect("a tela não chegou com a câmera ligada");
    tokio::time::sleep(Duration::from_secs(4)).await;
    let (sw, sh, sn, _) = FrameSource::open(&spath).unwrap().read().expect("quadro da tela");
    assert_eq!((sw, sh), (1280, 720), "a tela não pode virar 640x360 nem trocar com a câmera");
    assert!(sn > 5, "poucos quadros da tela em 4 s: {sn}");
    let (cw, ch, c1, _) = FrameSource::open(&rpath).unwrap().read().unwrap();
    assert_eq!((cw, ch), (640, 360));
    tokio::time::sleep(Duration::from_secs(4)).await;   // (compilação de depuração, sem otimização: a tela de 720p e a câmera disputam a CPU)
    let c2 = FrameSource::open(&rpath).unwrap().read().unwrap().2;
    eprintln!("câmera com a tela ligada: {} quadros em 4 s", c2 - c1);
    assert!(c2 > c1 + 12, "a câmera parou de chegar quando a tela entrou: {c1} -> {c2}");

    // 3) B também liga a câmera (4:3 de 640x480, enviada em 480x360): A vê, cada um no seu sentido
    b.cmd.send(MeshCmd::CamStart { device: "fake:1".into(), preview: None }).await.unwrap();
    wait_cam(&mut b, |v| v["ev"] == "cam.local" && v["state"] == "on", 20).await.expect("B não ligou a câmera");
    wait_cam(&mut a, |v| v["ev"] == "cam.remote" && v["state"] == "on", 25).await.expect("A não soube da câmera de B");
    let apath = dir.join("a-ve-b.bin");
    a.cmd.send(MeshCmd::CamWatch { key: CB.into(), on: true, path: apath.clone() }).await.unwrap();
    let fr = wait_cam(&mut a, |v| v["ev"] == "cam.remote" && v["state"] == "frame", 30).await.expect("nenhum quadro da câmera de B chegou ao A");
    assert_eq!((fr["w"].as_u64(), fr["h"].as_u64()), (Some(480), Some(360)), "{fr}");
    assert_eq!(camera::open_count(), 2);

    // 4) A desliga: B é avisado, a luz de A apaga; ligar de novo funciona sem renegociar tudo outra vez
    a.cmd.send(MeshCmd::CamStop).await.unwrap();
    wait_cam(&mut b, |v| v["ev"] == "cam.remote" && v["state"] == "off", 15).await.expect("B não soube que a câmera de A desligou");
    tokio::time::sleep(Duration::from_millis(700)).await;
    assert_eq!(camera::open_count(), 1, "só a câmera de B continua aberta");
    a.cmd.send(MeshCmd::CamStart { device: "fake:0".into(), preview: None }).await.unwrap();
    wait_cam(&mut b, |v| v["ev"] == "cam.remote" && v["state"] == "on", 20).await.expect("B não soube da câmera religada");
    // ao ver "off" a UI para de assistir; ao ver "on" de novo ela pede para assistir (como faz o `cam_tick`)
    b.cmd.send(MeshCmd::CamWatch { key: CA.into(), on: true, path: rpath.clone() }).await.unwrap();
    wait_cam(&mut b, |v| v["ev"] == "cam.remote" && v["state"] == "frame", 25).await.expect("a câmera religada não voltou");
    let c3 = FrameSource::open(&rpath).unwrap().read().unwrap().2;
    tokio::time::sleep(Duration::from_secs(3)).await;
    let c4 = FrameSource::open(&rpath).unwrap().read().unwrap().2;
    assert!(c4 > c3 + 15, "depois de religar a imagem não voltou: {c3} -> {c4}");

    // 5) fechar a chamada (Close) desliga TODAS as capturas
    let _ = a.cmd.send(MeshCmd::Close).await; let _ = b.cmd.send(MeshCmd::Close).await;
    assert!(until_zero_open(10).await, "fechar a chamada deixou a câmera aberta: {}", camera::open_count());
    std::env::remove_var("BRIKS_FAKE_CAMERA"); std::env::remove_var("BRIKS_ENCODER");
    let _ = std::fs::remove_dir_all(&dir);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn camera_start_reports_why_it_failed_and_nothing_stays_open() {
    let port = crate::mock::start(0).await.unwrap();
    let mut a = node(port, CA, "Ana", "cam2", Arc::new(Identity::generate()));
    for (mode, code) in [("none", "no_camera"), ("denied", "denied"), ("busy", "busy")] {
        fake(mode);
        a.cmd.send(MeshCmd::CamStart { device: String::new(), preview: None }).await.unwrap();
        let v = wait_cam(&mut a, |v| v["ev"] == "cam.local", 10).await.expect("sem resposta");
        assert_eq!((v["state"].as_str(), v["code"].as_str()), (Some("off"), Some(code)), "{v}");
        assert!(v["reason"].as_str().unwrap_or("").len() > 10, "o aviso precisa de texto: {v}");
        assert_eq!(camera::open_count(), 0);
    }
    let _ = a.cmd.send(MeshCmd::Close).await;
    std::env::remove_var("BRIKS_FAKE_CAMERA");
}

/// A câmera segue a rede: com 0,3 Mbps e 5% de perda o alvo de bitrate cai muito abaixo dos ~800 kbps do 360p/30, a altura nunca passa
/// de 360 e a imagem continua chegando.
#[tokio::test(flavor = "multi_thread", worker_threads = 6)]
async fn camera_adapts_to_loss_and_never_exceeds_360p() {
    fake("1");
    std::env::set_var("BRIKS_ENCODER", "openh264");
    let port = crate::mock::start(0).await.unwrap();
    let (pa, pb) = ("aaaaaaaa-ca11-4111-8111-0000000000c1", "bbbbbbbb-ca11-4222-8222-0000000000c2");
    let sim = crate::netsim::register(&[pa, pb]);
    let mut a = node(port, pa, "Ana", "cam3", Arc::new(Identity::generate()));
    let mut b = node(port, pb, "Beto", "cam3", Arc::new(Identity::generate()));
    assert!(authed(&mut a, 1, 30).await && authed(&mut b, 1, 30).await, "não autenticou pelo proxy");
    let dir = std::env::temp_dir().join(format!("briks-cam-adapt-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir); std::fs::create_dir_all(&dir).unwrap();
    a.cmd.send(MeshCmd::CamStart { device: "fake:0".into(), preview: None }).await.unwrap();
    wait_cam(&mut b, |v| v["ev"] == "cam.remote" && v["state"] == "on", 30).await.expect("on");
    b.cmd.send(MeshCmd::CamWatch { key: pa.into(), on: true, path: dir.join("rx.bin") }).await.unwrap();
    wait_cam(&mut b, |v| v["ev"] == "cam.remote" && v["state"] == "frame", 30).await.expect("1º quadro");
    // livre: o alvo fica perto do teto de 360p/30
    let mut free = 0u64;
    let t0 = std::time::Instant::now();
    while t0.elapsed() < Duration::from_secs(12) { if let Some(v) = wait_cam(&mut a, |v| v["ev"] == "cam.stats", 2).await { free = v["target_kbps"].as_u64().unwrap_or(0); assert!(v["h"].as_u64().unwrap_or(0) <= 360, "{v}"); } }
    sim.set(0.3, 5.0);
    let (mut tight, mut th) = (u64::MAX, 360u64);
    let t0 = std::time::Instant::now();
    while t0.elapsed() < Duration::from_secs(20) { if let Some(v) = wait_cam(&mut a, |v| v["ev"] == "cam.stats", 2).await { tight = tight.min(v["target_kbps"].as_u64().unwrap_or(u64::MAX)); th = v["h"].as_u64().unwrap_or(th); assert!(th <= 360, "{v}"); } }
    eprintln!("câmera: alvo livre {free} kbps; com 0,3 Mbps + 5% de perda o alvo caiu a {tight} kbps ({th}p)");
    assert!(free > 350, "livre: o alvo devia subir perto do teto de 360p/30: {free}");
    assert!(tight < free * 6 / 10 && tight < 320, "não adaptou à perda: livre {free} -> apertado {tight}");
    let n0 = FrameSource::open(&dir.join("rx.bin")).unwrap().read().map(|r| r.2).unwrap_or(0);
    tokio::time::sleep(Duration::from_secs(4)).await;
    let n1 = FrameSource::open(&dir.join("rx.bin")).unwrap().read().map(|r| r.2).unwrap_or(0);
    assert!(n1 > n0, "a imagem parou na rede ruim: {n0} -> {n1}");
    let _ = a.cmd.send(MeshCmd::Close).await; let _ = b.cmd.send(MeshCmd::Close).await;
    assert!(until_zero_open(10).await);
    std::env::remove_var("BRIKS_FAKE_CAMERA"); std::env::remove_var("BRIKS_ENCODER");
    let _ = std::fs::remove_dir_all(&dir);
}

/// IPC: lista, teste de prévia (cam.preview) e ligar/desligar a câmera pelo mesmo caminho da UI.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn settings_preview_shows_the_image_and_releases_the_device() {
    fake("1");
    let dir = std::env::temp_dir().join(format!("briks-cam-prev-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir); std::fs::create_dir_all(&dir).unwrap();
    let (tx, mut rx) = tokio::sync::mpsc::channel::<serde_json::Value>(16);
    let h = camera::start_preview("fake:0".into(), dir.join("t.bin"), tx.clone());
    let on = timeout(Duration::from_secs(10), async { loop { let v = rx.recv().await.unwrap(); if v["ev"] == "cam.preview" { break v; } } }).await.expect("sem evento");
    assert_eq!(on["state"], "on", "{on}");
    tokio::time::sleep(Duration::from_millis(900)).await;
    let (w, hh, n, rgba) = FrameSource::open(&dir.join("t.bin")).unwrap().read().expect("quadro de prévia");
    assert!(n > 5 && (w, hh) == (1280, 720) && corners_ok(&rgba, w, hh), "prévia ruim {w}x{hh} n={n}");
    assert_eq!(camera::open_count(), 1);
    h.stop();
    let off = timeout(Duration::from_secs(10), async { loop { let v = rx.recv().await.unwrap(); if v["ev"] == "cam.preview" && v["state"] == "off" { break v; } } }).await.expect("sem evento de fim");
    assert_eq!(off["state"], "off");
    assert_eq!(camera::open_count(), 0, "o evento de fim só sai depois de soltar a câmera");
    // sem câmeras: o teste avisa
    fake("none");
    let _h = camera::start_preview(String::new(), dir.join("t2.bin"), tx);
    let v = timeout(Duration::from_secs(10), async { loop { let v = rx.recv().await.unwrap(); if v["ev"] == "cam.preview" { break v; } } }).await.expect("sem evento");
    assert_eq!((v["state"].as_str(), v["code"].as_str()), (Some("off"), Some("no_camera")), "{v}");
    std::env::remove_var("BRIKS_FAKE_CAMERA");
    let _ = std::fs::remove_dir_all(&dir);
}
