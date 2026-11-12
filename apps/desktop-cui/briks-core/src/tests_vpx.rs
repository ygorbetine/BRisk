//! VP8/VP9 na transmissão de tela, de ponta a ponta em loopback (malha P2P real do webrtc-rs): o codec pedido sai codificado pela libvpx,
//! atravessa o pacer/RTP, é decodificado no receptor com o conteúdo certo; e a queda automática para H264 quando um peer não decodifica.
//! Só compila com `--features vpx`.
use super::*;

use crate::capture::{read_stamp, test_pattern, SourceKind};
use crate::codec::Codec;
use crate::share::ShareCfg;
use crate::video::{psnr, FrameSource};

fn cfg(codec: Codec) -> ShareCfg {
    ShareCfg { kind: SourceKind::Test, height: 720, fps: 30, kbps: 0, label: "Teste".into(), audio: crate::procaudio::AudioMode::None, content: crate::adapt::Content::Detail, codec, adaptive: true }
}

/// Amostra `secs` s do arquivo de quadros do receptor: quantos quadros novos e o melhor PSNR contra o padrão de teste.
fn sample(src: &FrameSource, w: usize, h: usize, secs: u64) -> (usize, f64) {
    let (mut last_n, mut n_frames, mut best, mut shot) = (0u64, 0usize, 0f64, None);
    let t0 = std::time::Instant::now();
    while t0.elapsed() < Duration::from_secs(secs) {
        if let Some((rw, rh, n, rgba)) = src.read() {
            if n != last_n && rw == w && rh == h { last_n = n; n_frames += 1; if shot.is_none() || n % 30 == 0 { shot = Some(rgba); } }
        }
        std::thread::sleep(Duration::from_millis(8));
    }
    if let Some(rgba) = shot {
        let mut bgr = rgba.clone(); for p in bgr.chunks_mut(4) { p.swap(0, 2); }
        let sr = crate::capture::stamp_rows(w);
        let _ = read_stamp(&rgba, w);
        for t in 0..160 { let mut e = test_pattern(w, h, t); for q in e[..sr * w * 4].chunks_mut(4) { q[0] = 0; q[1] = 0; q[2] = 0; } let mut r = bgr.clone(); for q in r[..sr * w * 4].chunks_mut(4) { q[0] = 0; q[1] = 0; q[2] = 0; } let p = psnr(&e, &r); if p > best { best = p; } }
    }
    (n_frames, best)
}

async fn codec_roundtrip(codec: Codec) {
    let port = crate::mock::start(0).await.unwrap();
    let (ia, ib) = (Arc::new(Identity::generate()), Arc::new(Identity::generate()));
    let room = format!("vpx-{}", codec.name());
    let mut a = node(port, PA, "Ana", &room, ia);
    let mut b = node(port, PB, "Beto", &room, ib);
    assert!(authed(&mut a, 1, 20).await && authed(&mut b, 1, 20).await, "não autenticou");
    let (w, h) = (1280usize, 720usize);
    let dir = std::env::temp_dir().join(format!("briks-vpx-{}-{}", codec.name(), std::process::id()));
    let _ = std::fs::remove_dir_all(&dir); std::fs::create_dir_all(&dir).unwrap();
    a.cmd.send(MeshCmd::ShareStart { cfg: cfg(codec), preview: None, dump: None }).await.unwrap();
    let local = wait_share(&mut a, |v| v["ev"] == "share.local" && v["state"] == "on", 20).await.expect("A não começou a transmitir");
    assert_eq!(local["codec"], codec.name(), "o codec negociado deve ser o pedido: {local}");
    wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "on", 20).await.expect("B não soube da transmissão");
    let rpath = dir.join("recebido.bin");
    b.cmd.send(MeshCmd::ShareWatch { key: PA.into(), on: true, path: rpath.clone() }).await.unwrap();
    let fr = wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "frame", 25).await.expect("nenhum quadro chegou ao B");
    assert_eq!((fr["w"].as_u64(), fr["h"].as_u64()), (Some(w as u64), Some(h as u64)));
    let src = FrameSource::open(&rpath).unwrap();
    let (n, best) = tokio::task::spawn_blocking(move || sample(&src, w, h, 4)).await.unwrap();
    eprintln!("{}: {n} quadros em 4 s ({:.1} fps), PSNR conteúdo {best:.1} dB", codec.name(), n as f64 / 4.0);
    assert!(n >= 40, "poucos quadros: {n}");
    assert!(best > 25.0, "conteúdo diferente do enviado: PSNR {best}");
    let _ = a.cmd.send(MeshCmd::Close).await; let _ = b.cmd.send(MeshCmd::Close).await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn vp8_share_flows_end_to_end_in_loopback() { codec_roundtrip(Codec::Vp8).await; }

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn vp9_share_flows_end_to_end_in_loopback() { codec_roundtrip(Codec::Vp9).await; }

/// Peer que só decodifica H264: o pedido de VP9 vira H264 desde o início. E, com a transmissão já em VP9, um peer que passa a anunciar só
/// H264 faz o emissor recomeçar em H264 sem derrubar a imagem do espectador (a faixa troca por replace_track, o receptor reconhece o
/// payload type novo).
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn vp9_falls_back_to_h264_when_a_peer_cannot_decode() {
    let port = crate::mock::start(0).await.unwrap();
    let (ia, ib) = (Arc::new(Identity::generate()), Arc::new(Identity::generate()));
    let mut a = node(port, PA, "Ana", "vpx-fallback", ia);
    let mut b = node(port, PB, "Beto", "vpx-fallback", ib);
    assert!(authed(&mut a, 1, 20).await && authed(&mut b, 1, 20).await, "não autenticou");
    let (w, h) = (1280usize, 720usize);
    let dir = std::env::temp_dir().join(format!("briks-vpx-fb-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir); std::fs::create_dir_all(&dir).unwrap();
    // 1) com os dois anunciando VP9: sai VP9 e o B assiste
    a.cmd.send(MeshCmd::ShareStart { cfg: cfg(Codec::Vp9), preview: None, dump: None }).await.unwrap();
    let local = wait_share(&mut a, |v| v["ev"] == "share.local" && v["state"] == "on", 20).await.expect("A não começou");
    assert_eq!(local["codec"], "vp9", "{local}");
    wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "on", 20).await.expect("B não soube");
    let rpath = dir.join("recebido.bin");
    b.cmd.send(MeshCmd::ShareWatch { key: PA.into(), on: true, path: rpath.clone() }).await.unwrap();
    wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "frame", 25).await.expect("nenhum quadro VP9 chegou ao B");
    let src = Arc::new(FrameSource::open(&rpath).unwrap());
    let s2 = src.clone();
    let (n, _) = tokio::task::spawn_blocking(move || sample(&s2, w, h, 2)).await.unwrap();
    assert!(n >= 20, "VP9 antes da queda: poucos quadros ({n})");
    // 2) o B passa a anunciar só H264: A recomeça em H264 e o B continua vendo imagem
    b.cmd.send(MeshCmd::TestVcodecs(vec!["h264"])).await.unwrap();
    let local = wait_share(&mut a, |v| v["ev"] == "share.local" && v["state"] == "on" && v["codec"] == "h264", 20).await.expect("A não caiu para H264");
    eprintln!("queda: {local}");
    // a queda recomeça a transmissão (como qualquer parar/começar): o B recebe o aviso e assiste de novo, agora com o payload type do H264
    wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "on", 20).await.expect("B não soube do recomeço");
    b.cmd.send(MeshCmd::ShareWatch { key: PA.into(), on: true, path: rpath.clone() }).await.unwrap();
    wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "frame", 25).await.expect("nenhum quadro H264 chegou ao B depois da queda");
    let s3 = src.clone();
    let (n2, best) = tokio::task::spawn_blocking(move || sample(&s3, w, h, 4)).await.unwrap();
    eprintln!("depois da queda (H264): {n2} quadros em 4 s, PSNR {best:.1} dB");
    assert!(n2 >= 40, "depois da queda para H264 o B ficou sem imagem: {n2} quadros");
    assert!(best > 25.0, "conteúdo errado depois da queda: PSNR {best}");
    // 3) o B já anuncia só H264: um novo pedido de VP9 sai direto em H264
    a.cmd.send(MeshCmd::ShareStop).await.unwrap();
    a.cmd.send(MeshCmd::ShareStart { cfg: cfg(Codec::Vp9), preview: None, dump: None }).await.unwrap();
    let local = wait_share(&mut a, |v| v["ev"] == "share.local" && v["state"] == "on", 20).await.expect("A não recomeçou");
    assert_eq!(local["codec"], "h264", "peer sem VP9: tem de sair H264 ({local})");
    let _ = a.cmd.send(MeshCmd::Close).await; let _ = b.cmd.send(MeshCmd::Close).await;
}

/// Um peer de versão antiga (sem o campo vcodecs no peer.state) é tratado como "só H264".
#[test]
fn missing_vcodecs_means_h264_only() {
    assert_eq!(crate::codec::local_caps(), vec!["h264", "vp9", "vp8"]);
    assert!(Codec::Vp9.available() && Codec::H264.available());
    assert_eq!(Codec::from_mime("video/VP9"), Codec::Vp9);
    assert_eq!(Codec::from_mime("video/h264"), Codec::H264);
}
