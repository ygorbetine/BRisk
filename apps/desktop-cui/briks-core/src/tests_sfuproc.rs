//! Muitos clientes no SFU em PROCESSOS SEPARADOS (não roda por padrão: `--ignored`): o teste pai sobe o `livekit-server --dev` real e
//! lança N cópias deste mesmo executável de teste, cada uma um cliente completo (camada de dados + áudio sintético) com o seu runtime.
//! Serve para separar "limite do teste num processo só" de "limite do cliente/servidor": no teste em um processo, 25 clientes
//! dividem um runtime de 4 threads e decodificam 600 fluxos de áudio juntos.
//!   BRIKS_SFU_CLIENTS=25 cargo test --release -- --ignored --nocapture --test-threads=1 sfu_many_clients_in_separate_processes
//! Roteiro por relógio (todos recebem o mesmo T0): T0 status + evento do dono e DM do 2 para o último; T0+12 s mudo remoto do 3;
//! T0+20 s expulsão do 7; T0+32 s cada filho imprime `RESULT {json}` e sai.
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use serde_json::{json, Value};

use crate::mesh::MeshCmd;
use crate::tests_sfurec::{client, start_server};

fn ident(i: usize) -> String { format!("user-{i:02}-0000-4000-8000-00000000{i:04}") }
fn now_ms() -> u64 { std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_millis() as u64 }
async fn until_ms(t: u64) { let n = now_ms(); if t > n { tokio::time::sleep(Duration::from_millis(t - n)).await; } }

/// Um cliente. Só faz algo quando lançado pelo teste pai (variáveis BRIKS_PROC_*).
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
#[ignore]
async fn sfu_proc_child() {
    let Ok(idx) = std::env::var("BRIKS_PROC_CHILD") else { return };
    let i: usize = idx.parse().unwrap();
    let n: usize = std::env::var("BRIKS_PROC_N").unwrap().parse().unwrap();
    let room = std::env::var("BRIKS_PROC_ROOM").unwrap();
    let t0: u64 = std::env::var("BRIKS_PROC_T0").unwrap().parse().unwrap();
    let started = Instant::now();
    // BRIKS_PROC_TALKERS=K: só os K primeiros ficam de microfone aberto (numa chamada real quase ninguém fala ao mesmo tempo)
    let talkers: usize = std::env::var("BRIKS_PROC_TALKERS").ok().and_then(|v| v.parse().ok()).unwrap_or(n);
    let c = client(&room, &ident(i), &format!("Pessoa {i}"), 1000 + i as u64, 3600).await;
    if i >= talkers { c.audio.set_mic_muted(true); c.cmd.send(MeshCmd::BroadcastState).await.unwrap(); }
    let fp_of = |j: usize| -> Option<String> { let pat = format!("info:{}:", ident(j)); c.log.lock().unwrap().iter().find(|s| s.starts_with(&pat)).map(|s| s[pat.len()..].to_string()) };
    let authed = || (0..n).filter(|j| *j != i && fp_of(*j).is_some()).count();
    // presença: espera autenticar todos (até T0)
    let mut auth_secs = -1.0f32;
    while now_ms() < t0 { if authed() == n - 1 { auth_secs = started.elapsed().as_secs_f32(); break; } tokio::time::sleep(Duration::from_millis(100)).await; }
    let authed_at_t0 = authed();
    until_ms(t0).await;
    if i == 0 {
        c.cmd.send(MeshCmd::SetProfile { status: "reunião".into(), presence: "Teste".into() }).await.unwrap();
        c.cmd.send(MeshCmd::Event { channel: "dm-x".into(), target: "msg-0001-aaaa".into(), action: "reaction.add".into(), content: None, reference: None, emoji: Some("👍".into()) }).await.unwrap();
    }
    if i == 2 { if let Some(fp) = fp_of(n - 1) { c.cmd.send(MeshCmd::Chat { to_fp: fp, text: "só para você".into() }).await.unwrap(); } }
    until_ms(t0 + 12_000).await;
    if i == 0 { if let Some(fp) = fp_of(3) { c.cmd.send(MeshCmd::RemoteMute { fp, muted: true }).await.unwrap(); } }
    until_ms(t0 + 20_000).await;
    if i == 0 { if let Some(fp) = fp_of(7) { c.cmd.send(MeshCmd::Kick { fp, reason: "teste".into() }).await.unwrap(); } }
    // voz: quadros recebidos do dono e do vizinho nos últimos segundos antes da expulsão já contam no total
    until_ms(t0 + 29_000).await;
    let (o0, v0) = (c.audio.rx_stat(&ident(0)).frames, c.audio.rx_stat(&ident((i + 1) % n)).frames);
    until_ms(t0 + 32_000).await;
    let (o1, v1) = (c.audio.rx_stat(&ident(0)).frames, c.audio.rx_stat(&ident((i + 1) % n)).frames);
    let heard = (0..n.min(talkers)).filter(|j| *j != i && c.audio.rx_stat(&ident(*j)).frames > 20).count();
    let errs: Vec<String> = c.log.lock().unwrap().iter().filter(|s| s.starts_with("err:")).take(4).cloned().collect();
    let r = json!({
        "i": i, "authed": authed(), "authed_at_t0": authed_at_t0, "auth_secs": auth_secs,
        "owner_me": c.log.lock().unwrap().iter().rev().find(|s| s.starts_with("owner:")).map(|s| s == "owner:true").unwrap_or(false), "status": c.count(&format!("state:{}:", ident(0))) > 0 && c.log.lock().unwrap().iter().any(|s| s.starts_with(&format!("state:{}:", ident(0))) && s.ends_with(":reunião")),
        "event": c.count("event:") > 0, "dm": c.count(":só para você"), "mic_open": c.audio.mic_open(),
        "saw_mute3": c.log.lock().unwrap().iter().any(|s| s.starts_with(&format!("state:{}:mic=false", ident(3)))),
        "kicked": c.count("kicked:") > 0, "banned": c.count("banned:"), "voice_owner_3s": o1 - o0, "voice_next_3s": v1 - v0, "heard": heard,
        "reconnects": c.stats.reconnects.load(std::sync::atomic::Ordering::Relaxed), "cpu_ms": crate::share::cpu_ms() as u64, "errs": errs,
    });
    println!("RESULT {r}");
    let _ = c.cmd.send(MeshCmd::Close).await;
    tokio::time::sleep(Duration::from_millis(300)).await;
}

struct Kids(Vec<std::process::Child>);
impl Drop for Kids { fn drop(&mut self) { for k in &mut self.0 { let _ = k.kill(); let _ = k.wait(); } } }

#[test]
#[ignore]
fn sfu_many_clients_in_separate_processes() {
    let Some(_srv) = start_server() else { eprintln!("PULADO: BRIKS_LIVEKIT_SERVER_BIN não definido"); return };
    let n: usize = std::env::var("BRIKS_SFU_CLIENTS").ok().and_then(|v| v.parse().ok()).unwrap_or(25).clamp(9, 60);
    let talkers: usize = std::env::var("BRIKS_PROC_TALKERS").ok().and_then(|v| v.parse().ok()).unwrap_or(n).clamp(8, n);
    let room = format!("procs-{}", uuid::Uuid::new_v4().simple());
    let exe = std::env::current_exe().unwrap();
    // janela para todos conectarem e se autenticarem antes do roteiro
    let window = 20_000 + n as u64 * 600;
    let t0 = now_ms() + window;
    let started = Instant::now();
    let mut kids = Kids(vec![]);
    for i in 0..n {
        let k = Command::new(&exe).args(["--ignored", "--exact", "tests_sfuproc::sfu_proc_child", "--nocapture", "--test-threads=1"])
            .env("BRIKS_PROC_CHILD", i.to_string()).env("BRIKS_PROC_N", n.to_string()).env("BRIKS_PROC_ROOM", &room).env("BRIKS_PROC_T0", t0.to_string()).env("BRIKS_PROC_TALKERS", talkers.to_string())
            .stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().expect("lançar o filho");
        kids.0.push(k);
        std::thread::sleep(Duration::from_millis(150));
    }
    eprintln!("{n} processos lançados em {:.1} s; roteiro começa em {:.0} s", started.elapsed().as_secs_f32(), (t0 - now_ms()) as f32 / 1000.0);
    let mut rs: Vec<Value> = vec![];
    let mut missing = vec![];
    for (i, k) in std::mem::take(&mut kids.0).into_iter().enumerate() {
        let out = k.wait_with_output().expect("saída do filho");
        let txt = String::from_utf8_lossy(&out.stdout);
        match txt.lines().find_map(|l| l.find("RESULT ").map(|p| &l[p + 7..])).and_then(|j| serde_json::from_str::<Value>(j).ok()) { Some(v) => rs.push(v), None => { if missing.is_empty() { let e = String::from_utf8_lossy(&out.stderr); let tail: String = e.lines().filter(|l| l.contains("panicked") || l.contains("src")).take(4).collect::<Vec<_>>().join(" | "); eprintln!("filho {i} sem resultado: {}", tail.chars().take(500).collect::<String>()); } missing.push(i) } }
    }
    assert!(missing.is_empty(), "filhos sem resultado (travaram ou caíram): {missing:?}");
    rs.sort_by_key(|v| v["i"].as_u64());
    let mut bad: Vec<String> = vec![];
    let mut chk = |ok: bool, msg: String| if !ok { bad.push(msg) };
    let secs: Vec<f64> = rs.iter().filter_map(|v| v["auth_secs"].as_f64()).filter(|s| *s >= 0.0).collect();
    for v in &rs {
        let i = v["i"].as_u64().unwrap() as usize;
        chk(v["authed_at_t0"] == n - 1, format!("cliente {i}: autenticou {} de {} antes do roteiro", v["authed_at_t0"], n - 1));
        chk(v["owner_me"] == (i == 0), format!("cliente {i}: dono errado"));
        if i != 0 { chk(v["status"] == true, format!("cliente {i}: não viu o status do dono")); chk(v["event"] == true, format!("cliente {i}: não viu o evento de chat")); }
        chk(v["dm"].as_u64() == Some((i == n - 1) as u64), format!("cliente {i}: DM = {} (só o último deve receber, uma vez)", v["dm"]));
        chk(v["mic_open"] == (i != 3 && i < talkers), format!("cliente {i}: microfone aberto = {}", v["mic_open"]));
        if i != 3 && i != 0 && i != 7 { chk(v["saw_mute3"] == true, format!("cliente {i}: não viu o microfone do 3 fechar")); }
        chk(v["kicked"] == (i == 7), format!("cliente {i}: expulso = {}", v["kicked"]));
        if i != 7 && i != 0 { chk(v["banned"].as_u64().unwrap_or(0) >= 1, format!("cliente {i}: não registrou o banimento do 7")); }
        if i != 7 {
            // nos 3 s medidos o 7 já saiu e o 3 está mudo: o dono fala sempre; o vizinho só conta se não for nenhum dos dois
            if i != 0 { chk(v["voice_owner_3s"].as_u64().unwrap_or(0) > 60, format!("cliente {i}: {} quadros de voz do dono em 3 s", v["voice_owner_3s"])); }
            let nx = (i + 1) % n;
            if nx != 3 && nx != 7 && nx < talkers { chk(v["voice_next_3s"].as_u64().unwrap_or(0) > 60, format!("cliente {i}: {} quadros de voz do vizinho em 3 s", v["voice_next_3s"])); }
            let want = (talkers - (i < talkers) as usize) as u64;
            chk(v["heard"].as_u64().unwrap_or(0) == want, format!("cliente {i}: ouviu {} de {want} pessoas que falam", v["heard"]));
        }
        chk(v["reconnects"] == 0, format!("cliente {i}: reconectou {} vez(es)", v["reconnects"]));
    }
    let cpu: u64 = rs.iter().map(|v| v["cpu_ms"].as_u64().unwrap_or(0)).sum();
    let wall = started.elapsed().as_secs_f64();
    eprintln!("{n} clientes ({talkers} falando) em processos separados: presença completa em {:.1}–{:.1} s (de {} que mediram); CPU somada dos clientes {:.0}% de 1 núcleo em média ({:.1} s de relógio); falhas: {}",
        secs.iter().cloned().fold(f64::MAX, f64::min), secs.iter().cloned().fold(0.0, f64::max), secs.len(), cpu as f64 / (wall * 1000.0) * 100.0, wall, bad.len());
    if !bad.is_empty() { for v in &rs { if !v["errs"].as_array().map(|a| a.is_empty()).unwrap_or(true) { eprintln!("erros do cliente {}: {}", v["i"], v["errs"]); } } }
    assert!(bad.is_empty(), "{}", bad.join("\n"));
}
