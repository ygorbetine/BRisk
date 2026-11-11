//! Camada de dados do SFU (chat/status/presença/mudo/expulsão pelas data channels do LiveKit, sem malha P2P) com VÁRIOS clientes
//! contra um `livekit-server --dev` real: o objetivo do SFU é passar de 5 pessoas. Sem GPU, sem janela, sem placa de som.
//! `BRIKS_SFU_CLIENTS` (padrão 10) muda o número de clientes. Precisa de `BRIKS_LIVEKIT_SERVER_BIN` (senão avisa e passa).
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use crate::audio::Audio;
use crate::identity::Identity;
use crate::mesh::{MeshCmd, MeshEvent};
use crate::sfu::{self, SfuHandle, SfuJoin};
use crate::sfudata::DataCfg;
use crate::signaling::secure_room_id;
use crate::tests_sfu::dev_token;

struct Server { child: Child }
impl Drop for Server { fn drop(&mut self) { let _ = self.child.kill(); let _ = self.child.wait(); } }

fn start_server() -> Option<Server> {
    let bin = std::env::var("BRIKS_LIVEKIT_SERVER_BIN").ok()?;
    if !std::path::Path::new(&bin).exists() { return None; }
    let child = Command::new(bin).args(["--dev", "--bind", "127.0.0.1"]).stdout(Stdio::null()).stderr(match std::env::var("BRIKS_LIVEKIT_LOG").ok().and_then(|p| std::fs::File::create(p).ok()) { Some(f) => Stdio::from(f), None => Stdio::null() }).spawn().ok()?;
    let t0 = Instant::now();
    while t0.elapsed() < Duration::from_secs(10) {
        if std::net::TcpStream::connect(("127.0.0.1", 7880)).is_ok() { return Some(Server { child }); }
        std::thread::sleep(Duration::from_millis(100));
    }
    let mut s = Server { child }; let _ = s.child.kill(); None
}

#[derive(Clone, Debug)]
enum Seen {
    Info { peer: String, fp: String, name: String },
    State { peer: String, mic: bool, status: String },
    Chat { content: String, from_fp: String },
    Event,
    Kicked,
    Banned(String),
    Speaking(String),
    Owner { me: bool },
    Err(String),
}

struct C {
    ident: String,
    fp: String,
    cmd: tokio::sync::mpsc::Sender<MeshCmd>,
    audio: Arc<Audio>,
    log: Arc<Mutex<Vec<Seen>>>,
    _h: tokio::task::JoinHandle<()>,
}

impl C {
    fn count(&self, f: impl Fn(&Seen) -> bool) -> usize { self.log.lock().unwrap().iter().filter(|s| f(s)).count() }
    async fn wait(&self, secs: u64, f: impl Fn(&Seen) -> bool) -> bool {
        let end = Instant::now() + Duration::from_secs(secs);
        while Instant::now() < end { if self.count(&f) > 0 { return true; } tokio::time::sleep(Duration::from_millis(100)).await; }
        false
    }
}

async fn client(room: &str, ident: &str, name: &str, joined: u64, owner_fp: Option<String>) -> C {
    let id = Arc::new(Identity::generate());
    let fp = id.fingerprint();
    let rh = secure_room_id("room", room);
    let lk_room = format!("room:{}", &rh[..32]);
    let (au, au_rx) = Audio::new(true);
    let h: SfuHandle = sfu::connect(SfuJoin { url: "ws://127.0.0.1:7880".into(), token: dev_token(ident, &lk_room), audio: Some((au.clone(), au_rx)),
        data: Some(DataCfg { identity: id, name: name.into(), room_hash: rh, owner_fp, joined_at: joined }) }).await.expect("entrar no SFU");
    let log = Arc::new(Mutex::new(vec![]));
    let (l2, mut ev) = (log.clone(), h.events);
    let task = tokio::spawn(async move {
        while let Some(e) = ev.recv().await {
            let s = match e {
                MeshEvent::PeerInfo { peer_id, fp, name } => Seen::Info { peer: peer_id, fp, name },
                MeshEvent::PeerState { peer_id, microphone, status, .. } => Seen::State { peer: peer_id, mic: microphone, status },
                MeshEvent::Chat { content, from_fp, .. } => Seen::Chat { content, from_fp },
                MeshEvent::ChatEvent { local: false, .. } => Seen::Event,
                MeshEvent::Kicked { .. } => Seen::Kicked,
                MeshEvent::Banned { fp } => Seen::Banned(fp),
                MeshEvent::Speaking { peer_id, level } if level > 0.0 => Seen::Speaking(peer_id),
                MeshEvent::Owner { me, .. } => Seen::Owner { me },
                MeshEvent::Error(m) => Seen::Err(m),
                _ => continue,
            };
            l2.lock().unwrap().push(s);
        }
    });
    C { ident: ident.into(), fp, cmd: h.cmd, audio: au, log, _h: task }
}

/// 10 clientes na mesma sala pelo SFU: todos se enxergam (hello assinado), ouvem voz, veem status, trocam chat assinado, mudo remoto e expulsão
/// pela dona; mensagens forjadas e lixo são descartados. Nada passa por malha P2P nem por Supabase Realtime.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn sfu_data_layer_scales_past_five_clients() {
    let Some(_srv) = start_server() else { eprintln!("PULADO: BRIKS_LIVEKIT_SERVER_BIN não definido"); return };
    let n: usize = std::env::var("BRIKS_SFU_CLIENTS").ok().and_then(|v| v.parse().ok()).unwrap_or(10).clamp(6, 40);
    let room = format!("grande-{}", uuid::Uuid::new_v4().simple());
    let t0 = Instant::now();
    let mut cs: Vec<C> = vec![];
    for i in 0..n { cs.push(client(&room, &format!("user-{i:02}-0000-4000-8000-00000000{i:04}"), &format!("Pessoa {i}"), 1000 + i as u64, None).await); }
    eprintln!("{n} clientes conectados em {:.1} s", t0.elapsed().as_secs_f32());
    let owner = &cs[0];   // a mais antiga (joinedAt menor) é a dona

    // 1) presença/identidade: todos autenticam todos pelo hello assinado, com o fp REAL
    for (i, c) in cs.iter().enumerate() {
        for (j, o) in cs.iter().enumerate() {
            if i == j { continue; }
            let ok = c.wait(40, |s| matches!(s, Seen::Info { peer, fp, name } if *peer == o.ident && *fp == o.fp && *name == format!("Pessoa {j}"))).await;
            assert!(ok, "cliente {i} não autenticou o {j}");
        }
    }
    assert!(cs[1].wait(10, |s| matches!(s, Seen::Owner { me: false })).await && owner.wait(10, |s| matches!(s, Seen::Owner { me: true })).await, "dono");
    eprintln!("presença: {n}x{} hellos assinados verificados em {:.1} s", n - 1, t0.elapsed().as_secs_f32());

    // 2) voz: todo cliente recebe áudio do dono e de outro participante, pelo SFU
    tokio::time::sleep(Duration::from_secs(3)).await;
    for (i, c) in cs.iter().enumerate() {
        if i != 0 { assert!(c.audio.rx_stat(&cs[0].ident).frames > 20, "cliente {i} sem voz do dono"); }
        let other = &cs[(i + 1) % n];
        assert!(c.audio.rx_stat(&other.ident).frames > 20, "cliente {i} sem voz do {}", (i + 1) % n);
    }
    assert!(cs[3].wait(10, |s| matches!(s, Seen::Speaking(p) if *p == cs[5].ident)).await, "indicador de fala com a identidade do SFU");
    eprintln!("voz: todos ouvem o dono e o vizinho");

    // 3) status/presença: dono muda; todos veem
    owner.cmd.send(MeshCmd::SetProfile { status: "reunião".into(), presence: "Teste".into() }).await.unwrap();
    for (i, c) in cs.iter().enumerate().skip(1) { assert!(c.wait(30, |s| matches!(s, Seen::State { peer, status, .. } if *peer == owner.ident && status == "reunião")).await, "cliente {i} não viu o status"); }

    // 4) chat assinado: evento para todos; DM só para o destinatário
    owner.cmd.send(MeshCmd::Event { channel: "dm-x".into(), target: "msg-0001-aaaa".into(), action: "reaction.add".into(), content: None, reference: None, emoji: Some("👍".into()) }).await.unwrap();
    for (i, c) in cs.iter().enumerate().skip(1) { let ok = c.wait(30, |s| matches!(s, Seen::Event)).await; if !ok { let errs: Vec<Seen> = c.log.lock().unwrap().iter().filter(|s| matches!(s, Seen::Err(_))).cloned().collect(); eprintln!("erros do cliente {i}: {errs:?}"); } assert!(ok, "cliente {i} não viu o evento"); }
    cs[2].cmd.send(MeshCmd::Chat { to_fp: cs[n - 1].fp.clone(), text: "só para você".into() }).await.unwrap();
    assert!(cs[n - 1].wait(15, |s| matches!(s, Seen::Chat { content, from_fp } if content == "só para você" && *from_fp == cs[2].fp)).await, "DM não chegou");
    tokio::time::sleep(Duration::from_millis(800)).await;
    for (i, c) in cs.iter().enumerate() { if i != n - 1 { assert_eq!(c.count(|s| matches!(s, Seen::Chat { .. })), 0, "DM vazou para o cliente {i}"); } }

    // 5) forjado/malformado: o cliente 4 (não dono) tenta expulsar o 2 e manda lixo; ninguém cai nem reage
    let victim = &cs[2];
    let ts = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_millis() as i64;
    for raw in [
        "lixo{{{".to_string(), "[]".to_string(), r#"{"v":1,"t":"kick"}"#.to_string(), r#"{"v":1,"t":"hello","sfuId":"outro"}"#.to_string(),
        format!(r#"{{"v":1,"t":"kick","target_fp":"{}","reason":"x","ts":{ts},"sig":"AAAA"}}"#, victim.fp),
        format!(r#"{{"v":1,"t":"mute","target_fp":"{}","muted":true,"ts":{ts},"sig":"AAAA"}}"#, victim.fp),
        format!(r#"{{"v":1,"t":"state","mic":false,"deaf":false,"status":"hack","ts":{ts},"sig":"AAAA"}}"#),
        "x".repeat(20_000),
    ] { cs[4].cmd.send(MeshCmd::RawDc { text: raw }).await.unwrap(); }
    tokio::time::sleep(Duration::from_secs(2)).await;
    assert_eq!(victim.count(|s| matches!(s, Seen::Kicked)), 0, "forjado expulsou");
    assert!(victim.audio.mic_open(), "forjado silenciou");
    for c in &cs { assert_eq!(c.count(|s| matches!(s, Seen::State { status, .. } if status == "hack")), 0); }

    // 6) mudo remoto (só a dona): cliente 3 é silenciado; todos veem o microfone fechado
    owner.cmd.send(MeshCmd::RemoteMute { fp: cs[3].fp.clone(), muted: true }).await.unwrap();
    for _ in 0..150 { if !cs[3].audio.mic_open() { break; } tokio::time::sleep(Duration::from_millis(100)).await; }
    assert!(!cs[3].audio.mic_open(), "alvo não fechou o microfone");
    for (i, c) in cs.iter().enumerate() { if i != 3 && i != 0 { assert!(c.wait(15, |s| matches!(s, Seen::State { peer, mic: false, .. } if *peer == cs[3].ident)).await, "cliente {i} não viu o mudo"); } }
    // um não-dono tentando silenciar é recusado localmente
    cs[1].cmd.send(MeshCmd::RemoteMute { fp: cs[5].fp.clone(), muted: true }).await.unwrap();
    tokio::time::sleep(Duration::from_millis(500)).await;
    assert!(cs[5].audio.mic_open());
    assert!(cs[1].wait(5, |s| matches!(s, Seen::Err(m) if m.contains("dono"))).await);

    // 7) expulsão pela dona: o 7 recebe Kicked e sai; os outros ban e silenciam
    owner.cmd.send(MeshCmd::Kick { fp: cs[7].fp.clone(), reason: "teste".into() }).await.unwrap();
    assert!(cs[7].wait(15, |s| matches!(s, Seen::Kicked)).await, "alvo não foi expulso");
    for (i, c) in cs.iter().enumerate() { if i != 7 && i != 0 { assert!(c.wait(15, |s| matches!(s, Seen::Banned(f) if *f == cs[7].fp)).await, "cliente {i} não registrou o banimento"); } }
    eprintln!("dados: status, chat, mudo remoto e expulsão verificados com {n} clientes em {:.1} s no total", t0.elapsed().as_secs_f32());
    for c in &cs { let _ = c.cmd.send(MeshCmd::Close).await; }
}
