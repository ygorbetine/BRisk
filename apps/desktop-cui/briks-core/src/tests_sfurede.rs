//! Rede ruim sobre o SFU (não roda por padrão: `--ignored`): o proxy UDP do `netsim` fica entre UM cliente e o `livekit-server --dev`
//! real, do lado de quem transmite (emissor -> SFU) ou de quem assiste (SFU -> espectador). Mesma medição da malha (`tests_rede`):
//! perda de 1/2/5/10%, queda total de 4 s e banda limitada com a adaptação (adapt.rs) ligada.
//! Fonte sintética, codificador (`mf-soft`) e decodificador de software (sem GPU), sem janela.
//!   cargo test --release -- --ignored --nocapture --test-threads=1 sfu_network
//! O CSV vai para BRIKS_PROVAS_OUT (ou a pasta temporária).
use std::sync::atomic::Ordering::Relaxed;
use std::sync::Arc;
use std::time::{Duration, Instant};

use tokio::time::timeout;

use crate::audio::Audio;
use crate::capture::SourceKind;
use crate::mesh::{MeshCmd, MeshEvent};
use crate::netsim::NetSim;
use crate::sfu::{self, SfuJoin, SfuStats};
use crate::share::ShareCfg;
use crate::tests_rede::{out_dir, pct, Ends, Session, HEAD};
use crate::tests_sfu::dev_token;
use crate::tests_sfurec::start_server;
use crate::video::FrameSource;
use serde_json::json;

/// Sem GPU: codificador de software do Media Foundation (aceita mudar o bitrate sem recriar; o OpenH264 não dá 30 qps em 720p aqui)
/// e o decodificador de software. `BRIKS_ENCODER` já definida é respeitada.
fn soft_codecs() { if std::env::var("BRIKS_ENCODER").is_err() { std::env::set_var("BRIKS_ENCODER", "mf-soft"); } }

struct Sfu { s: Session, sim: Arc<NetSim>, a_stats: Arc<SfuStats>, b_stats: Arc<SfuStats>, _au: (Arc<Audio>, Arc<Audio>) }

async fn wait_share(e: &mut Ends, pred: impl Fn(&serde_json::Value) -> bool, secs: u64) -> Option<serde_json::Value> {
    let end = Instant::now() + Duration::from_secs(secs);
    loop {
        let left = end.saturating_duration_since(Instant::now());
        match timeout(left, e.events.recv()).await { Ok(Some(MeshEvent::Share(v))) => { if pred(&v) { return Some(v); } } Ok(Some(_)) => {} _ => return None }
    }
}

/// `sim_on`: "a" = proxy entre o emissor e o SFU; "b" = entre o SFU e o espectador.
async fn open(tag: &str, sim_on: &str, height: usize, kbps: u32, adaptive: bool) -> Sfu {
    let (pa, pb) = (format!("aaaaaaaa-1111-4111-8111-{tag:0>12}"), format!("bbbbbbbb-2222-4222-8222-{tag:0>12}"));
    let sim = crate::netsim::register(&[if sim_on == "a" { &pa } else { &pb }]);
    let room = format!("room:rede-{tag}-{}", uuid::Uuid::new_v4().simple());
    let mut hs = vec![];
    let mut aus = vec![];
    for id in [&pa, &pb] {
        let (au, au_rx) = Audio::new(true);
        let h = sfu::connect(SfuJoin { url: format!("ws://127.0.0.1:{}", crate::tests_sfu::lk_port()), token: dev_token(id, &room), audio: Some((au.clone(), au_rx)), data: None, renew: None }).await.expect("entrar no SFU");
        hs.push(h); aus.push(au);
    }
    let (hb, ha) = (hs.pop().unwrap(), hs.pop().unwrap());
    let (a_stats, b_stats) = (ha.stats.clone(), hb.stats.clone());
    let (mut a, mut b) = (Ends { cmd: ha.cmd, events: ha.events }, Ends { cmd: hb.cmd, events: hb.events });
    let dir = std::env::temp_dir().join(format!("briks-sfurede-{tag}-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    a.cmd.send(MeshCmd::ShareStart { cfg: ShareCfg { kind: SourceKind::TestMotion, height, fps: 30, kbps, label: "Rede".into(), audio: crate::procaudio::AudioMode::None, content: crate::adapt::Content::Detail, adaptive }, preview: None, dump: None }).await.unwrap();
    wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "on", 30).await.expect("on");
    let rpath = dir.join("rx.bin");
    b.cmd.send(MeshCmd::ShareWatch { key: pa.clone(), on: true, path: rpath.clone() }).await.unwrap();
    wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "frame", 30).await.expect("1º quadro");
    let _ = wait_share(&mut a, |v| v["ev"] == "share.stats", 5).await;
    let src = FrameSource::open(&rpath).unwrap();
    let w = height * 16 / 9;
    let au_b = aus.pop().unwrap(); let au_a = aus.pop().unwrap();
    Sfu { s: Session { a, b, src, w, tx: json!(null), rx: json!(null), last_n: 0, tx_sum: [0; 4] }, sim, a_stats, b_stats, _au: (au_a, au_b) }
}

/// Perda de 1/2/5/10% e queda total de 4 s, com bitrate fixo (isola o conserto de perda), nos dois lados do SFU.
#[tokio::test(flavor = "multi_thread", worker_threads = 6)]
#[ignore]
async fn sfu_network_loss_and_outage_matrix() {
    let Some(_srv) = start_server() else { eprintln!("PULADO: BRIKS_LIVEKIT_SERVER_BIN não definido"); return };
    soft_codecs();
    let mut csv = format!("{HEAD}\n");
    let mut bad: Vec<String> = vec![];   // junta as falhas para a matriz sair inteira
    let only = std::env::var("BRIKS_SFUREDE_LADO").ok();   // "a" ou "b": mede só um lado (o CSV só é gravado na medição completa)
    for (tag, side, label) in [("c1", "a", "envio (emissor->SFU)"), ("c2", "b", "recepcao (SFU->espectador)")] {
        if only.as_deref().map(|o| o != side).unwrap_or(false) { continue; }
        let mut x = open(tag, side, 720, 3000, false).await;
        let mut base_fps = 0.0;
        for (name, loss) in [("base", 0.0), ("perda 1%", 1.0), ("perda 2%", 2.0), ("perda 5%", 5.0), ("perda 10%", 10.0)] {
            x.sim.set(0.0, loss);
            let _ = x.s.sample(1500).await;
            let r = x.s.sample(8000).await;
            let line = r.row(&format!("SFU {label}: {name}")); eprintln!("{line}"); csv += &(line + "\n");
            if loss == 0.0 { base_fps = r.fps(); assert!(base_fps > 10.0, "sem perda deveria fluir: {base_fps} qps"); assert!(x.sim.forwarded.load(Relaxed) > 100_000, "o tráfego não passou pelo proxy"); }
            // Até 5% o conserto de perda (NACK/retransmissão/PLI) tem de segurar a transmissão. A 10% NÃO há exigência de ritmo: com este
            // codificador de software (que estoura o bitrate pedido: ~6 Mbps em vez de 3) a transmissão degrada muito, no SFU e na malha
            // P2P (ver docs/plans/sfu-livekit-nativo.md); a linha sai no CSV e o que se exige é voltar ao normal quando a perda passa.
            if loss <= 5.0 {
                if r.fps() < base_fps * 0.7 { bad.push(format!("SFU {label}, {name}: {:.1} qps (base {base_fps:.1})", r.fps())); }
                if pct(&r.lats, 0.5) >= 400 { bad.push(format!("SFU {label}, {name}: latência p50 {} ms", pct(&r.lats, 0.5))); }
            }
            if std::env::var("BRIKS_RN_DEBUG").is_ok() { eprintln!("   tx {} | rx {}", x.s.tx, x.s.rx); }
        }
        x.sim.set(0.0, 0.0);
        let _ = x.s.sample(3000).await;
        let base = x.s.sample(3000).await;
        if base.fps() < base_fps * 0.8 { bad.push(format!("SFU {label}: não voltou ao normal 3-6 s depois da perda de 10%: {:.1} qps (base {base_fps:.1})", base.fps())); }
        // queda total de 4 s (100% de perda entre o cliente e o SFU); a sinalização (TCP) continua de pé
        x.sim.set(0.0, 100.0);
        let during = x.s.sample(4000).await;
        x.sim.set(0.0, 0.0);
        let after = x.s.sample(6000).await;
        let late = { let _ = x.s.sample(2000).await; x.s.sample(4000).await };
        let rec = (x.a_stats.reconnects.load(Relaxed), x.b_stats.reconnects.load(Relaxed));
        let first = after.first_ms.map(|v| v as i64).unwrap_or(-1);
        eprintln!("SFU {label}: queda total 4 s: durante {} quadros; 1º quadro novo {first} ms após a volta; {:.1} qps nos 6 s seguintes; maior intervalo {} ms; PLIs {}; latência p50 antes {} -> 8-12 s depois {} ms (p95 {}); reconexões completas emissor/espectador: {}/{}",
            during.frames, after.fps(), after.max_gap, after.plis, pct(&base.lats, 0.5), pct(&late.lats, 0.5), pct(&late.lats, 0.95), rec.0, rec.1);
        csv += &(after.row(&format!("SFU {label}: queda total 4 s (6 s após a volta; 1º quadro em {first} ms; p50 8-12 s depois {} ms; reconexões {}/{})", pct(&late.lats, 0.5), rec.0, rec.1)) + "\n");
        if during.frames > 4 { bad.push(format!("SFU {label}: a queda não cortou a rede ({} quadros)", during.frames)); }
        if !after.first_ms.map(|v| v < 3500).unwrap_or(false) { bad.push(format!("SFU {label}: não voltou em 3,5 s depois da queda de 4 s")); }
        if late.fps() < base.fps() * 0.8 { bad.push(format!("SFU {label}: ritmo não voltou depois da queda: {:.1} qps (antes {:.1})", late.fps(), base.fps())); }
        if pct(&late.lats, 0.5) >= pct(&base.lats, 0.5) + 100 { bad.push(format!("SFU {label}: latência acumulou depois da queda")); }
        x.s.close().await;
        tokio::time::sleep(Duration::from_millis(500)).await;
    }
    if only.is_none() { std::fs::write(out_dir().join("sfu-matriz-perda-queda.csv"), csv).unwrap(); }
    assert!(bad.is_empty(), "{}", bad.join("\n"));
}

/// Adaptação (adapt.rs) com o SFU no lugar dos pares: banda do emissor limitada no meio da transmissão. O Receiver Report e os NACKs
/// do SFU alimentam o estimador; o bitrate tem de descer para caber e voltar a subir quando a banda volta.
#[tokio::test(flavor = "multi_thread", worker_threads = 6)]
#[ignore]
async fn sfu_network_adapts_to_limited_uplink() {
    let Some(_srv) = start_server() else { eprintln!("PULADO: BRIKS_LIVEKIT_SERVER_BIN não definido"); return };
    soft_codecs();
    let mut x = open("d1", "a", 720, 3000, true).await;
    let mut csv = String::from("fase,qps_recebido,enviado_kbps,alvo_kbps,estimativa_kbps,altura,fps_alvo,perda_pct,rtt_ms,motivo,lat_p50_ms,lat_p95_ms,congelamentos_250ms\n");
    let mut phase = |name: &str, r: &crate::tests_rede::Samp, tx: &serde_json::Value| {
        let line = format!("\"{name}\",{:.1},{},{},{},{},{},{},{},{},{},{},{}", r.fps(), tx["kbps"], tx["target_kbps"], tx["est_kbps"], tx["h"], tx["target_fps"], tx["loss_pct"], tx["rtt_ms"], tx["reason"], pct(&r.lats, 0.5), pct(&r.lats, 0.95), r.freezes);
        eprintln!("{line}"); csv.push_str(&line); csv.push('\n');
    };
    let _ = x.s.sample(4000).await;
    let free = x.s.sample(6000).await;
    let free_kbps = x.s.tx["kbps"].as_f64().unwrap_or(0.0);
    phase("livre", &free, &x.s.tx);
    // 1,2 Mbps de banda no caminho emissor <-> SFU (fila de 250 ms, descarte na cauda)
    x.sim.set(1.2, 0.0);
    let squeeze = x.s.sample(10000).await;
    phase("1,2 Mbps: primeiros 10 s", &squeeze, &x.s.tx);
    let tight = x.s.sample(10000).await;
    let tight_kbps = x.s.tx["kbps"].as_f64().unwrap_or(0.0);
    let tight_est = x.s.tx["est_kbps"].as_f64().unwrap_or(0.0);
    phase("1,2 Mbps: 10-20 s", &tight, &x.s.tx);
    x.sim.set(0.0, 0.0);
    let _ = x.s.sample(15000).await;
    let back = x.s.sample(10000).await;
    let back_kbps = x.s.tx["kbps"].as_f64().unwrap_or(0.0);
    phase("livre de novo: 15-25 s depois", &back, &x.s.tx);
    std::fs::write(out_dir().join("sfu-adaptacao-banda.csv"), csv).unwrap();
    assert!(free_kbps > 1800.0, "sem limite deveria usar o bitrate pedido: {free_kbps} kbps");
    assert!(tight_est < 1500.0 && tight_kbps < 1400.0, "não se adaptou ao enlace de 1,2 Mbps: enviando {tight_kbps} kbps, estimativa {tight_est} kbps");
    // apertado, o controlador desce para 360p a 10 qps (codificador de software): o que se exige é continuar entregando quadros
    assert!(tight.fps() >= 5.0 && tight.freezes <= 6, "adaptado, mas travando: {:.1} qps, {} congelamentos", tight.fps(), tight.freezes);
    assert!(back_kbps > tight_kbps * 1.4, "não voltou a subir quando a banda voltou: {back_kbps} kbps (apertado {tight_kbps})");
    assert_eq!(x.a_stats.reconnects.load(Relaxed) + x.b_stats.reconnects.load(Relaxed), 0, "limitar a banda não deve derrubar a sessão");
    x.s.close().await;
}

/// 10% de perda no ENVIO com a adaptação LIGADA (como no app; a matriz acima usa bitrate fixo de propósito). Mede se baixar o
/// bitrate/resolução segura a transmissão onde o bitrate fixo desaba. Sem exigência dura: o resultado vai para o CSV e para o doc.
#[tokio::test(flavor = "multi_thread", worker_threads = 6)]
#[ignore]
async fn sfu_network_adaptive_under_uplink_loss() {
    let Some(_srv) = start_server() else { eprintln!("PULADO: BRIKS_LIVEKIT_SERVER_BIN não definido"); return };
    soft_codecs();
    let mut x = open("e1", "a", 720, 3000, true).await;
    let mut csv = String::from("fase,qps_recebido,enviado_kbps,estimativa_kbps,altura,fps_alvo,perda_pct,motivo,lat_p50_ms,lat_p95_ms,maior_intervalo_ms,congelamentos_250ms,plis\n");
    let _ = x.s.sample(5000).await;
    let mut worst10 = f64::MAX;
    for (name, loss, ms) in [("livre", 0.0, 6000u64), ("perda 5%: 0-10 s", 5.0, 10000), ("perda 5%: 10-20 s", 5.0, 10000), ("perda 10%: 0-10 s", 10.0, 10000), ("perda 10%: 10-20 s", 10.0, 10000), ("perda 10%: 20-30 s", 10.0, 10000), ("livre de novo: 5-15 s depois", 0.0, 10000)] {
        x.sim.set(0.0, loss);
        if name.starts_with("livre de novo") { let _ = x.s.sample(5000).await; }
        let r = x.s.sample(ms).await;
        let tx = &x.s.tx;
        let line = format!("\"{name}\",{:.1},{},{},{},{},{},{},{},{},{},{},{}", r.fps(), tx["kbps"], tx["est_kbps"], tx["h"], tx["target_fps"], tx["loss_pct"], tx["reason"], pct(&r.lats, 0.5), pct(&r.lats, 0.95), r.max_gap, r.freezes, r.plis);
        eprintln!("{line}"); csv.push_str(&line); csv.push('\n');
        if name.starts_with("perda 10%: 1") || name.starts_with("perda 10%: 2") { worst10 = worst10.min(r.fps()); }
    }
    std::fs::write(out_dir().join("sfu-perda-envio-adaptativo.csv"), csv).unwrap();
    eprintln!("10% de perda no envio com adaptação: pior janela depois de acomodar = {worst10:.1} qps");
    assert_eq!(x.a_stats.reconnects.load(Relaxed) + x.b_stats.reconnects.load(Relaxed), 0, "perda não deve derrubar a sessão");
    x.s.close().await;
}
