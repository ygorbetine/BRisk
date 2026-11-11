//! Testes de rede ruim e de latência vidro a vidro (não rodam por padrão: `--ignored`). Fontes sintéticas, proxy UDP local
//! (`netsim`), sem captura de tela. Para não tocar na GPU: `BRIKS_ENCODER=mf-soft BRIKS_CONVERT=cpu`.
//!   cargo test --release -- --ignored --nocapture --test-threads=1 network_impairment_matrix
//!   cargo test --release -- --ignored --nocapture --test-threads=1 glass_to_glass_latency
//! O CSV vai para BRIKS_PROVAS_OUT (ou a pasta temporária).
use std::sync::Arc;
use std::time::{Duration, Instant};

use tokio::time::timeout;

use crate::capture::{read_stamp_scaled, SourceKind};
use crate::identity::Identity;
use crate::mesh::{self, MeshCfg, MeshCmd, MeshEvent, MeshHandle};
use crate::realtime::{self, RealtimeCfg};
use crate::share::ShareCfg;
use crate::signaling::{secure_room_id, topic_name};
use crate::video::FrameSource;
use serde_json::json;

fn node(port: u16, peer: &str, name: &str, room: &str) -> MeshHandle {
    let mut cfg = RealtimeCfg::new(&format!("ws://127.0.0.1:{port}"), "chave-de-teste");
    cfg.leave_grace = Duration::from_millis(200);
    let topic = topic_name("room", &secure_room_id("room", room));
    let (rt, rx) = realtime::connect(cfg, topic, peer.into(), json!({ "peerId": peer, "joinedAt": 1, "clientVersion": "teste" }));
    mesh::spawn(MeshCfg { namespace: "room".into(), room: room.into(), local_peer: peer.into(), display_name: name.into(), ice_servers: vec![], audio: None, owner_fp: None, joined_at: 1 }, Arc::new(Identity::generate()), rt, rx)
}
async fn authed(h: &mut MeshHandle) -> bool {
    let end = Instant::now() + Duration::from_secs(30);
    loop {
        let left = end.saturating_duration_since(Instant::now());
        match timeout(left, h.events.recv()).await { Ok(Some(MeshEvent::Peer { state, .. })) if state == "autenticado" => return true, Ok(Some(_)) => {} _ => return false }
    }
}
async fn wait_share(h: &mut MeshHandle, pred: impl Fn(&serde_json::Value) -> bool, secs: u64) -> Option<serde_json::Value> {
    let end = Instant::now() + Duration::from_secs(secs);
    loop {
        let left = end.saturating_duration_since(Instant::now());
        match timeout(left, h.events.recv()).await { Ok(Some(MeshEvent::Share(v))) => { if pred(&v) { return Some(v); } } Ok(Some(_)) => {} _ => return None }
    }
}

pub(crate) fn pct(v: &[i32], p: f64) -> i32 { if v.is_empty() { -1 } else { v[((v.len() as f64 * p) as usize).min(v.len() - 1)] } }

/// As duas pontas de comando/eventos de um nó: iguais na malha P2P e no SFU, então a mesma medição serve aos dois.
pub(crate) struct Ends { pub cmd: tokio::sync::mpsc::Sender<MeshCmd>, pub events: tokio::sync::mpsc::Receiver<MeshEvent> }
impl From<MeshHandle> for Ends { fn from(h: MeshHandle) -> Self { Ends { cmd: h.cmd, events: h.events } } }

pub(crate) struct Session { pub a: Ends, pub b: Ends, pub src: FrameSource, pub w: usize, pub tx: serde_json::Value, pub rx: serde_json::Value, pub last_n: u64, pub tx_sum: [u64; 4] }

impl Session {
    async fn open(tag: &str, sim_peers: bool, height: usize, kbps: u32, adaptive: bool) -> (Session, Option<Arc<crate::netsim::NetSim>>) {
        let port = crate::mock::start(0).await.unwrap();
        let (pa, pb) = (format!("aaaaaaaa-1111-4111-8111-{tag:0>12}"), format!("bbbbbbbb-2222-4222-8222-{tag:0>12}"));
        let sim = if sim_peers { Some(crate::netsim::register(&[&pa, &pb])) } else { None };
        let (mut a, mut b) = (node(port, &pa, "Ana", &format!("rede-{tag}")), node(port, &pb, "Beto", &format!("rede-{tag}")));
        assert!(authed(&mut a).await && authed(&mut b).await, "não autenticou");
        let dir = std::env::temp_dir().join(format!("briks-rede-{tag}-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        a.cmd.send(MeshCmd::ShareStart { cfg: ShareCfg { kind: SourceKind::TestMotion, height, fps: 30, kbps, label: "Rede".into(), audio: crate::procaudio::AudioMode::None, content: crate::adapt::Content::Detail, adaptive }, preview: None, dump: None }).await.unwrap();
        wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "on", 30).await.expect("on");
        let rpath = dir.join("rx.bin");
        b.cmd.send(MeshCmd::ShareWatch { key: pa.clone(), on: true, path: rpath.clone() }).await.unwrap();
        wait_share(&mut b, |v| v["ev"] == "share.remote" && v["state"] == "frame", 30).await.expect("1º quadro");
        let src = FrameSource::open(&rpath).unwrap();
        let w = height * 16 / 9;
        (Session { a: a.into(), b: b.into(), src, w, tx: json!(null), rx: json!(null), last_n: 0, tx_sum: [0; 4] }, sim)
    }

    /// Amostra `ms` de quadros recebidos.
    pub(crate) async fn sample(&mut self, ms: u64) -> Samp {
        let mut s = Samp::default();
        let (t0, mut last_new) = (Instant::now(), Instant::now());
        let mut first_gap: Option<u128> = None;
        let rx0 = self.rx.clone();
        let sum0 = self.tx_sum;
        while t0.elapsed() < Duration::from_millis(ms) {
            while let Ok(e) = self.a.events.try_recv() { if let MeshEvent::Share(v) = e { if v["ev"] == "share.stats" {
                for (i, k) in ["nacks", "rtx", "rtx_skipped", "lost_pkts"].iter().enumerate() { self.tx_sum[i] += v[k].as_u64().unwrap_or(0); }
                if std::env::var("BRIKS_RN_DEBUG").is_ok() { eprintln!("  [tx] kbps {} alvo {} est {} h {} fps {} perda {}% rtt {} nacks {} plis {} rtx {} lost {} fila {}kB", v["kbps"], v["target_kbps"], v["est_kbps"], v["h"], v["fps"], v["loss_pct"], v["rtt_ms"], v["nacks"], v["plis"], v["rtx"], v["lost_pkts"], v["pacer_queue_kb"]); }
                self.tx = v; } } }
            while let Ok(e) = self.b.events.try_recv() { if let MeshEvent::Share(v) = e { if v["ev"] == "share.rxstats" { self.rx = v; } } }
            if let Some((w, _, n, rgba)) = self.src.read() {
                if n != self.last_n && w > 0 {
                    s.frames += (n - self.last_n).min(1000) as u32; self.last_n = n;
                    let gap = last_new.elapsed().as_millis(); last_new = Instant::now();
                    if first_gap.is_none() { first_gap = Some(t0.elapsed().as_millis()); }
                    s.max_gap = s.max_gap.max(gap);
                    if gap > 250 { s.freezes += 1; }
                    let l = (self.src.ts() as u32).wrapping_sub(read_stamp_scaled(&rgba, w, 1920)) as i32;
                    if (0..20_000).contains(&l) { s.lats.push(l); }
                }
            }
            tokio::time::sleep(Duration::from_millis(6)).await;
        }
        s.max_gap = s.max_gap.max(last_new.elapsed().as_millis());
        if s.frames == 0 { s.freezes += 1; }
        s.first_ms = first_gap;
        s.ms = ms;
        let d = |k: &str| self.rx[k].as_u64().unwrap_or(0).saturating_sub(rx0[k].as_u64().unwrap_or(0));
        s.plis = d("plis"); s.lost_frames = d("lost_frames"); s.recovered = d("recovered");
        s.nacks = self.tx_sum[0] - sum0[0]; s.rtx = self.tx_sum[1] - sum0[1]; s.rtx_skipped = self.tx_sum[2] - sum0[2]; s.lost_pkts = self.tx_sum[3] - sum0[3];
        s.sent_kbps = self.tx["kbps"].as_f64().unwrap_or(0.0); s.work_ms = self.tx["work_ms"].as_f64().unwrap_or(0.0); s.enc_ms = self.tx["enc_ms"].as_f64().unwrap_or(0.0); s.rtt_ms = self.tx["rtt_ms"].as_f64().unwrap_or(0.0);
        s.lats.sort();
        s
    }
    pub(crate) async fn close(self) { let _ = self.a.cmd.send(MeshCmd::Close).await; let _ = self.b.cmd.send(MeshCmd::Close).await; }
}

#[derive(Default, Clone)]
pub(crate) struct Samp { pub ms: u64, pub frames: u32, pub max_gap: u128, pub freezes: u32, pub first_ms: Option<u128>, pub lats: Vec<i32>, pub plis: u64, pub lost_frames: u64, pub recovered: u64, pub nacks: u64, pub rtx: u64, pub rtx_skipped: u64, pub lost_pkts: u64, pub sent_kbps: f64, pub work_ms: f64, pub enc_ms: f64, pub rtt_ms: f64 }
impl Samp {
    pub(crate) fn fps(&self) -> f64 { self.frames as f64 * 1000.0 / self.ms.max(1) as f64 }
    pub(crate) fn row(&self, name: &str) -> String {
        format!("\"{name}\",{:.1},{},{},{},{},{},{},{},{},{},{},{},{}", self.fps(), self.frames, pct(&self.lats, 0.5), pct(&self.lats, 0.95), pct(&self.lats, 0.99), self.max_gap, self.freezes, self.nacks, self.rtx, self.rtx_skipped, self.lost_pkts, self.plis, self.lost_frames)
    }
}
pub(crate) const HEAD: &str = "cenario,qps_recebido,quadros,lat_p50_ms,lat_p95_ms,lat_p99_ms,maior_intervalo_ms,congelamentos_250ms,nacks_unicos,retransmitidos,retransmissoes_recusadas,pacotes_perdidos_real,plis,quadros_perdidos_rx";

pub(crate) fn out_dir() -> std::path::PathBuf {
    let d = std::env::var("BRIKS_PROVAS_OUT").map(std::path::PathBuf::from).unwrap_or_else(|_| std::env::temp_dir().join("briks-rede"));
    std::fs::create_dir_all(&d).unwrap();
    d
}

/// Perda (inclui 1%), jitter, reordenação, perda+jitter e queda total de 3 e 5 s, com bitrate fixo (isola o conserto de perda; a
/// adaptação tem teste próprio). Cada cenário: 8 s de amostra depois de 1,5 s de acomodação.
#[tokio::test(flavor = "multi_thread", worker_threads = 6)]
#[ignore]
async fn network_impairment_matrix() {
    use std::sync::atomic::Ordering::Relaxed;
    let (mut s, sim) = Session::open("a1", true, 720, 3000, false).await;
    let sim = sim.unwrap();
    let mut csv = format!("{HEAD}\n");
    // (nome, perda %, atraso ms, jitter ms, reordena)
    let scen: [(&str, f64, u64, u64, bool); 10] = [("base", 0.0, 0, 0, false), ("perda 1%", 1.0, 0, 0, false), ("perda 2%", 2.0, 0, 0, false), ("perda 5%", 5.0, 0, 0, false), ("perda 10%", 10.0, 0, 0, false),
        ("jitter 0-30 ms (em ordem)", 0.0, 20, 30, false), ("jitter 0-80 ms (em ordem)", 0.0, 20, 80, false), ("jitter 0-40 ms reordenando", 0.0, 20, 40, true), ("perda 2% + jitter 0-40 ms", 2.0, 30, 40, false), ("perda 5% + jitter 0-40 ms reordenando", 5.0, 30, 40, true)];
    for (name, loss, delay, jit, reorder) in scen {
        sim.delay_ms.store(delay, Relaxed); sim.jitter_ms.store(jit, Relaxed); sim.reorder.store(reorder, Relaxed); sim.set(0.0, loss);
        let _ = s.sample(1500).await;
        let r = s.sample(8000).await;
        let line = r.row(name); eprintln!("{line}"); csv += &(line + "\n");
    }
    sim.delay_ms.store(0, Relaxed); sim.jitter_ms.store(0, Relaxed); sim.reorder.store(false, Relaxed); sim.set(0.0, 0.0);
    let _ = s.sample(3000).await;
    let base = s.sample(3000).await;
    // queda total: 100% de perda por N s; mede quanto tempo até o 1º quadro novo depois da volta, e a latência depois
    for secs in [3u64, 5] {
        sim.set(0.0, 100.0);
        let during = s.sample(secs * 1000).await;
        sim.set(0.0, 0.0);
        let after = s.sample(6000).await;
        let late = { let _ = s.sample(2000).await; s.sample(4000).await };
        let name = format!("queda total {secs} s");
        eprintln!("{name}: durante {} quadros; 1º quadro novo {} ms após a volta; {:.1} qps nos 6 s seguintes; maior intervalo {} ms; PLIs {}; latência p50 antes {} → 8-12 s depois {} ms (p95 {})",
            during.frames, after.first_ms.map(|v| v as i64).unwrap_or(-1), after.fps(), after.max_gap, after.plis, pct(&base.lats, 0.5), pct(&late.lats, 0.5), pct(&late.lats, 0.95));
        csv += &(after.row(&format!("{name} (6 s após a volta; 1º quadro em {} ms; p50 8-12 s depois {} ms)", after.first_ms.map(|v| v as i64).unwrap_or(-1), pct(&late.lats, 0.5))) + "\n");
        assert!(during.frames <= 4, "a queda não cortou a rede");
        assert!(after.first_ms.map(|v| v < 3500).unwrap_or(false), "não voltou em 3,5 s");
        assert!(pct(&late.lats, 0.5) < pct(&base.lats, 0.5) + 100, "latência acumulou depois da queda");
    }
    std::fs::write(out_dir().join("matriz-perda-jitter-queda.csv"), csv).unwrap();
    s.close().await;
}

fn bench_stage(w: usize, h: usize, n: usize) -> (f64, f64, f64, f64) {
    use crate::video::{bgra_to_i420, H264Dec, H264Enc};
    let mut enc = H264Enc::new(w, h, 30.0, 4_000_000).unwrap();
    let mut dec = H264Dec::new().unwrap();
    let frames: Vec<Vec<u8>> = (0..8).map(|t| crate::capture::test_pattern(w, h, t * 3)).collect();
    let (mut tc, mut te, mut td, mut got, mut enc_empty, mut dec_empty) = (0f64, 0f64, 0f64, 0usize, 0usize, 0usize);
    for i in 0..n + 10 {
        let t0 = Instant::now();
        let y = bgra_to_i420(&frames[i % 8], w, h, w, h);
        let t1 = Instant::now();
        let aus = enc.encode(&y).unwrap();
        let t2 = Instant::now();
        if aus.is_empty() { enc_empty += 1; }
        for au in &aus { if dec.decode(au).unwrap().is_some() { got += 1; } else { dec_empty += 1; } }
        let t3 = Instant::now();
        if i >= 10 { tc += (t1 - t0).as_secs_f64(); te += (t2 - t1).as_secs_f64(); td += (t3 - t2).as_secs_f64(); }
    }
    let k = n as f64 / 1000.0;
    eprintln!("  atraso de pipeline ({h}p): codificador sem saída em {enc_empty} de {} quadros; decodificador sem quadro em {dec_empty} unidades", n + 10);
    (tc / k, te / k, td / k, got as f64)
}

/// Latência vidro a vidro (menos o monitor): carimbo do relógio no quadro sintético → quadro RGBA gravado para a UI. Loopback
/// puro (sem proxy), proxy sem limite e redes com atraso/banda limitados. Decomposição por etapa: conversão+codificação (telemetria
/// do emissor), rede (RTT/2 medido), decodificação+conversão para RGBA (bancada) e o resto (pacer, reordenação, gravação, intervalo
/// de amostragem de 6 ms do teste).
#[tokio::test(flavor = "multi_thread", worker_threads = 6)]
#[ignore]
async fn glass_to_glass_latency() {
    use std::sync::atomic::Ordering::Relaxed;
    let mut csv = String::from("cenario,altura,lat_p50_ms,lat_p95_ms,lat_p99_ms,lat_max_ms,qps,conv_mais_enc_ms,enc_ms,rtt_ms,quadros\n");
    let mut notes = vec![];
    // (nome, altura, kbps, usa proxy, atraso ms por sentido, jitter ms, banda Mbps, perda %)
    let cases: [(&str, usize, u32, bool, u64, u64, f64, f64); 7] = [
        ("loopback 720p30 (sem proxy)", 720, 3000, false, 0, 0, 0.0, 0.0),
        ("loopback 1080p30 (sem proxy)", 1080, 6000, false, 0, 0, 0.0, 0.0),
        ("proxy sem limite 720p30", 720, 3000, true, 0, 0, 0.0, 0.0),
        ("rede boa: 15 ms por sentido, jitter 0-5 ms, 20 Mbps, 720p30", 720, 3000, true, 15, 5, 20.0, 0.0),
        ("rede media: 40 ms por sentido, jitter 0-15 ms, 8 Mbps, 1% perda, 720p30", 720, 3000, true, 40, 15, 8.0, 1.0),
        ("rede ruim: 80 ms por sentido, jitter 0-40 ms, 4 Mbps, 2% perda, 720p30", 720, 3000, true, 80, 40, 4.0, 2.0),
        ("rede boa 1080p30: 15 ms por sentido, 20 Mbps", 1080, 6000, true, 15, 5, 20.0, 0.0),
    ];
    let only = std::env::var("BRIKS_G2G_CASE").ok().and_then(|v| v.parse::<usize>().ok());
    for (i, (name, h, kbps, proxy, delay, jit, mbps, loss)) in cases.into_iter().enumerate() {
        if only.map(|o| o != i).unwrap_or(false) { continue; }
        let (mut s, sim) = Session::open(&format!("b{i}"), proxy, h, if mbps > 0.0 { 6000 } else { kbps }, mbps > 0.0).await;   // rede limitada: adaptação ligada (como no app)
        if let Some(sim) = &sim { sim.delay_ms.store(delay, Relaxed); sim.jitter_ms.store(jit, Relaxed); sim.set(mbps, loss); }
        let _ = s.sample(if mbps > 0.0 { 10000 } else { 2500 }).await;
        let r = s.sample(15000).await;
        let max = r.lats.last().copied().unwrap_or(-1);
        eprintln!("  rx: {} | tx: {}", s.rx, s.tx);
        eprintln!("{name}: p50 {} p95 {} p99 {} máx {} ms | {:.1} qps | conv+enc {:.1} ms (enc {:.1}) | RTT {:.0} ms | {} quadros", pct(&r.lats, 0.5), pct(&r.lats, 0.95), pct(&r.lats, 0.99), max, r.fps(), r.work_ms, r.enc_ms, r.rtt_ms, r.frames);
        csv += &format!("\"{name}\",{h},{},{},{},{max},{:.1},{:.1},{:.1},{:.0},{}\n", pct(&r.lats, 0.5), pct(&r.lats, 0.95), pct(&r.lats, 0.99), r.fps(), r.work_ms, r.enc_ms, r.rtt_ms, r.frames);
        notes.push((name, pct(&r.lats, 0.95)));
        s.close().await;
    }
    for (w, h) in [(1280usize, 720usize), (1920, 1080)] {
        let (c, e, d, got) = bench_stage(w, h, 120);
        eprintln!("bancada {h}p: BGRA→I420 {c:.1} ms | codificação {e:.1} ms | decodificação+RGBA {d:.1} ms ({got:.0} unidades)");
        csv += &format!("\"bancada {h}p: BGRA->I420 / codificar / decodificar+RGBA (ms)\",{h},{c:.1},{e:.1},{d:.1},,,,,,\n");
    }
    std::fs::write(out_dir().join("latencia-vidro-a-vidro.csv"), csv).unwrap();
    // p95 < 250 ms em rede boa (meta do plano)
    for (name, p95) in &notes { if name.contains("720p") && (name.starts_with("rede boa") || name.starts_with("loopback") || name.starts_with("proxy")) { assert!(*p95 < 250, "{name}: p95 {p95} ms"); } }
}

/// Perda ALEATÓRIA sem congestionamento, com a adaptação ligada (como no app): o bitrate não pode descer sem parar. Mesmas fases da
/// medição do SFU (`sfu_network_adaptive_under_uplink_loss`). Codecs de software por padrão (sem GPU); o CSV vai para a pasta de provas.
#[tokio::test(flavor = "multi_thread", worker_threads = 6)]
#[ignore]
async fn mesh_adaptive_under_random_loss() {
    if std::env::var("BRIKS_ENCODER").is_err() { std::env::set_var("BRIKS_ENCODER", "mf-soft"); }
    let (mut s, sim) = Session::open("ea", true, 720, 3000, true).await;
    let sim = sim.unwrap();
    let mut csv = String::from("fase,qps_recebido,enviado_kbps,estimativa_kbps,altura,fps_alvo,perda_pct,motivo,lat_p50_ms,lat_p95_ms,maior_intervalo_ms,congelamentos_250ms,plis\n");
    let _ = s.sample(5000).await;
    for (name, loss, ms) in [("livre", 0.0, 6000u64), ("perda 5%: 0-10 s", 5.0, 10000), ("perda 5%: 10-20 s", 5.0, 10000), ("perda 10%: 0-10 s", 10.0, 10000), ("perda 10%: 10-20 s", 10.0, 10000), ("perda 10%: 20-30 s", 10.0, 10000), ("livre de novo: 5-15 s depois", 0.0, 10000)] {
        sim.set(0.0, loss);
        if name.starts_with("livre de novo") { let _ = s.sample(5000).await; }
        let r = s.sample(ms).await;
        let tx = &s.tx;
        let line = format!("\"{name}\",{:.1},{},{},{},{},{},{},{},{},{},{},{}", r.fps(), tx["kbps"], tx["est_kbps"], tx["h"], tx["target_fps"], tx["loss_pct"], tx["reason"], pct(&r.lats, 0.5), pct(&r.lats, 0.95), r.max_gap, r.freezes, r.plis);
        eprintln!("{line}"); csv.push_str(&line); csv.push('\n');
    }
    std::fs::write(out_dir().join("malha-perda-aleatoria-adaptativo.csv"), csv).unwrap();
    s.close().await;
}
