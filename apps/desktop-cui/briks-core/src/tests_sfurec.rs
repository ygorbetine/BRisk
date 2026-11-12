//! Reconexão automática do modo SFU: o `livekit-server --dev` REAL é morto e reiniciado no meio da chamada.
//! Prova que voz, transmissão assistida, chat e estado voltam sozinhos, com token renovado, sem ninguém "sair" da sala.
//! Sem GPU (codificador/decodificador de software), sem janela, sem placa de som. Precisa de `BRIKS_LIVEKIT_SERVER_BIN`.
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use crate::audio::Audio;
use crate::identity::Identity;
use crate::mesh::{MeshCmd, MeshEvent};
use crate::sfu::{self, SfuJoin, SfuStats, TokenFn};
use crate::sfudata::DataCfg;
use crate::signaling::secure_room_id;

pub struct Server { child: Child }
impl Drop for Server { fn drop(&mut self) { let _ = self.child.kill(); let _ = self.child.wait(); } }

/// Porta do `livekit-server` de teste. `BRIKS_LK_TEST_PORT` troca a 7880 (e as portas RTC seguintes) para não colidir com outro
/// servidor na máquina. `BRIKS_LIVEKIT_TEST_PORT` é o nome preferido; todos os testes de SFU usam esta porta.
pub fn lk_port() -> u16 { std::env::var("BRIKS_LIVEKIT_TEST_PORT").or_else(|_| std::env::var("BRIKS_LK_TEST_PORT")).ok().and_then(|v| v.parse().ok()).unwrap_or(7880) }

/// Argumentos do `livekit-server` de teste, na porta de `lk_port()` (RTC nas duas seguintes).
pub fn lk_args() -> Vec<String> {
    let port = lk_port();
    let mut v: Vec<String> = ["--dev", "--bind", "127.0.0.1"].iter().map(|s| s.to_string()).collect();
    if port != 7880 { v.push("--config-body".into()); v.push(format!("port: {port}
rtc:
  tcp_port: {}
  udp_port: {}
keys:
  devkey: secret
", port + 1, port + 2)); }
    v
}

pub fn start_server() -> Option<Server> {
    let bin = std::env::var("BRIKS_LIVEKIT_SERVER_BIN").ok()?;
    if !std::path::Path::new(&bin).exists() { return None; }
    // a porta do servidor anterior pode demorar um instante para ser liberada
    for _ in 0..3 {
        let mut child = Command::new(&bin).args(crate::tests_sfu::lk_args()).stdout(Stdio::null()).stderr(Stdio::null()).spawn().ok()?;
        let t0 = Instant::now();
        while t0.elapsed() < Duration::from_secs(10) {
            if let Ok(Some(_)) = child.try_wait() { break; }   // morreu (porta ocupada): tenta de novo
            if std::net::TcpStream::connect(("127.0.0.1", crate::tests_sfu::lk_port())).is_ok() { return Some(Server { child }); }
            std::thread::sleep(Duration::from_millis(100));
        }
        let _ = child.kill(); let _ = child.wait();
        std::thread::sleep(Duration::from_millis(500));
    }
    None
}

pub struct C {
    pub ident: String,
    pub fp: String,
    pub id: Arc<Identity>,
    pub cmd: tokio::sync::mpsc::Sender<MeshCmd>,
    pub audio: Arc<Audio>,
    pub stats: Arc<SfuStats>,
    pub renewals: Arc<AtomicU64>,
    pub log: Arc<Mutex<Vec<String>>>,
}

impl C {
    pub fn count(&self, pat: &str) -> usize { self.log.lock().unwrap().iter().filter(|s| s.contains(pat)).count() }
    pub async fn wait(&self, secs: u64, pat: &str) -> bool {
        let end = Instant::now() + Duration::from_secs(secs);
        while Instant::now() < end { if self.count(pat) > 0 { return true; } tokio::time::sleep(Duration::from_millis(50)).await; }
        false
    }
}

/// Cliente com camada de dados, áudio sintético e renovação de token. `ttl` = validade (s) do token da ENTRADA; os renovados valem 1 h.
pub async fn client(room: &str, ident: &str, name: &str, joined: u64, ttl: u64) -> C { client_owner(room, ident, name, joined, ttl, false).await }

/// Como `client`; com `owner`, o token de entrada e os renovados levam a marca de dono (o que a função `livekit-token` faria).
pub async fn client_owner(room: &str, ident: &str, name: &str, joined: u64, ttl: u64, owner: bool) -> C {
    let id = Arc::new(Identity::generate());
    let fp = id.fingerprint();
    let rh = secure_room_id("room", room);
    let lk_room = format!("room:{}", &rh[..32]);
    let (au, au_rx) = Audio::new(true);
    let renewals = Arc::new(AtomicU64::new(0));
    let (r2, i2, k2) = (renewals.clone(), ident.to_string(), lk_room.clone());
    let renew: TokenFn = Arc::new(move || { let (r, i, k) = (r2.clone(), i2.clone(), k2.clone()); Box::pin(async move { r.fetch_add(1, Ordering::Relaxed); Ok(crate::tests_sfu::dev_token_owner(&i, &k, 3600, owner)) }) });
    let h = sfu::connect(SfuJoin { url: crate::tests_sfu::lk_ws(), token: crate::tests_sfu::dev_token_owner(ident, &lk_room, ttl, owner), audio: Some((au.clone(), au_rx)),
        data: Some(DataCfg { identity: id.clone(), name: name.into(), room_hash: rh, owner_fp: None, joined_at: joined }), renew: Some(renew) }).await.expect("entrar no SFU");
    let log = Arc::new(Mutex::new(vec![]));
    let (l2, mut ev) = (log.clone(), h.events);
    tokio::spawn(async move {
        while let Some(e) = ev.recv().await {
            let s = match e {
                MeshEvent::Peer { peer_id, state } => format!("peer:{peer_id}:{state}"),
                MeshEvent::PeerInfo { peer_id, fp, .. } => format!("info:{peer_id}:{fp}"),
                MeshEvent::PeerState { peer_id, microphone, status, .. } => format!("state:{peer_id}:mic={microphone}:{status}"),
                MeshEvent::Chat { content, from_fp, .. } => format!("chat:{from_fp}:{content}"),
                MeshEvent::Kicked { reason } => format!("kicked:{reason}"),
                MeshEvent::ChatEvent { local: false, from_fp, .. } => format!("event:{from_fp}"),
                MeshEvent::Banned { fp } => format!("banned:{fp}"),
                MeshEvent::Typing { from_fp } => format!("typing:{from_fp}"),
                MeshEvent::RoomChat { peer_id, from_fp, author, content, id, .. } => format!("salamsg:{peer_id}:{from_fp}:{author}:{id}:{content}"),
                MeshEvent::StreamRequest { peer_id, name } => format!("streamreq:{peer_id}:{name}"),
                MeshEvent::StreamReply { peer_id, name, accepted } => format!("streamreply:{peer_id}:{name}:{accepted}"),
                MeshEvent::GroupSnapshot { from_fp, snapshot } => format!("group:{from_fp}:{}", snapshot["name"].as_str().unwrap_or("")),
                MeshEvent::History { from_fp, content, id, mine, .. } => format!("hist:{id}:{from_fp}:{mine}:{content}"),
                MeshEvent::Ack { id, from_fp } => format!("ack:{from_fp}:{id}"),
                MeshEvent::File(v) => format!("file:{v}"),
                MeshEvent::Owner { me, fp } => format!("owner:{me}:{fp}"),
                MeshEvent::Share(v) => v.to_string(),
                MeshEvent::Error(m) => format!("err:{m}"),
                _ => continue,
            };
            l2.lock().unwrap().push(s);
        }
    });
    C { ident: ident.into(), fp, id, cmd: h.cmd, audio: au, stats: h.stats, renewals, log }
}

const A: &str = "aaaaaaaa-1111-4111-8111-000000000001";
const B: &str = "bbbbbbbb-2222-4222-8222-000000000002";
const D: &str = "dddddddd-3333-4333-8333-000000000003";

/// Quadros de voz que `c` recebe de `from` em `secs` segundos.
async fn voice(c: &C, from: &str, secs: u64) -> u64 { let f0 = c.audio.rx_stat(from).frames; tokio::time::sleep(Duration::from_secs(secs)).await; c.audio.rx_stat(from).frames - f0 }

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn sfu_reconnects_after_server_restart_with_renewed_token() {
    use crate::capture::SourceKind;
    use crate::share::ShareCfg;
    use crate::video::FrameSource;
    let Some(srv) = start_server() else { eprintln!("PULADO: BRIKS_LIVEKIT_SERVER_BIN não definido"); return };
    let mut srv = Some(srv);
    std::env::set_var("BRIKS_ENCODER", "openh264");
    std::env::set_var("BRIKS_DECODER", "openh264");
    let room = format!("rec-{}", uuid::Uuid::new_v4().simple());
    let t_tok = Instant::now();
    // token de ENTRADA com 6 s de validade: depois da queda só entra quem renovou
    let a = client(&room, A, "Ana", 1000, 6).await;
    let b = client(&room, B, "Beto", 1001, 6).await;
    let d = client(&room, D, "Duda", 1002, 6).await;
    let cs = [&a, &b, &d];
    for c in cs { for o in cs { if c.ident != o.ident { assert!(c.wait(30, &format!("info:{}:{}", o.ident, o.fp)).await, "{} não autenticou {}", c.ident, o.ident); } } }
    // A transmite (fonte sintética), B assiste
    a.cmd.send(MeshCmd::ShareStart { cfg: ShareCfg { kind: SourceKind::Test, height: 720, fps: 30, kbps: 0, label: "Teste".into(), audio: crate::procaudio::AudioMode::None, content: crate::adapt::Content::Detail, codec: Default::default(), adaptive: false }, preview: None, dump: None }).await.unwrap();
    assert!(b.wait(25, r#""state":"on""#).await, "B não soube da transmissão");
    let dir = std::env::temp_dir().join(format!("briks-sfurec-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir); std::fs::create_dir_all(&dir).unwrap();
    let rpath = dir.join("recebido.bin");
    b.cmd.send(MeshCmd::ShareWatch { key: A.into(), on: true, path: rpath.clone() }).await.unwrap();
    assert!(b.wait(25, r#""state":"frame""#).await, "nenhum quadro chegou ao B antes da queda");
    let src = FrameSource::open(&rpath).unwrap();
    // o leitor devolve None enquanto o quadro está sendo gravado: tenta de novo em vez de ler isso como "quadro 0"
    let frame_n = || { for _ in 0..200 { if let Some(x) = src.read() { return x.2; } std::thread::sleep(Duration::from_millis(1)); } 0 };
    let before = (voice(&b, A, 2).await, d.audio.rx_stat(B).frames, frame_n());
    assert!(before.0 > 40 && before.1 > 20 && before.2 > 0, "mídia antes da queda: {before:?}");

    let mut video_back: Vec<(Option<Duration>, [u64; 5])> = vec![];
    for round in 1..=2u64 {
        // garante que o token de entrada já venceu (só na 1ª rodada importa)
        while t_tok.elapsed() < Duration::from_secs(8) { tokio::time::sleep(Duration::from_millis(100)).await; }
        let n_before = frame_n();
        // ---- o servidor morre (kill, sem aviso aos clientes) ----
        let t_kill = Instant::now();
        srv = None;   // Drop: kill + wait
        for c in cs { assert!(c.wait(20, r#""state":"reconnecting""#).await, "{} não percebeu a queda", c.ident); assert!(c.count(r#""ev":"signaling.status","state":"reconectando""#) as u64 >= round); }
        eprintln!("rodada {round}: queda percebida por todos em {:.1} s", t_kill.elapsed().as_secs_f32());
        // durante a queda: B manda DM para D e A muda o status (ficam na fila e saem ao voltar)
        b.cmd.send(MeshCmd::Chat { to_fp: d.fp.clone(), text: format!("durante a queda {round}") }).await.unwrap();
        a.cmd.send(MeshCmd::SetProfile { status: format!("voltei {round}"), presence: String::new() }).await.unwrap();
        tokio::time::sleep(Duration::from_secs(if round == 1 { 4 } else { 1 })).await;
        for c in cs { assert!(c.count(r#""state":"retry""#) > 0, "{} não tentou voltar com o servidor fora", c.ident); }
        // ---- o servidor volta (processo novo, sem memória da sala) ----
        let t_up = Instant::now();
        srv = Some(start_server().expect("reiniciar o livekit-server"));
        for c in cs {
            let end = Instant::now() + Duration::from_secs(30);
            while Instant::now() < end && c.stats.reconnects.load(Ordering::Relaxed) < round { tokio::time::sleep(Duration::from_millis(50)).await; }
            assert_eq!(c.stats.reconnects.load(Ordering::Relaxed), round, "{} não reconectou", c.ident);
            assert_eq!(c.renewals.load(Ordering::Relaxed), round, "{}: um token novo por queda", c.ident);
        }
        eprintln!("rodada {round}: os 3 clientes voltaram {:.1} s depois de o servidor subir (fora do ar por {:.1} s); tokens renovados: {}", t_up.elapsed().as_secs_f32(), t_kill.elapsed().as_secs_f32(), cs.iter().map(|c| c.renewals.load(Ordering::Relaxed)).sum::<u64>());
        // vídeo do espectador logo depois de voltar: quanto demora o 1º quadro novo e a que taxa os quadros chegam, segundo a segundo
        // (o contador recomeça do zero quando o receptor é recriado; conta-se cada mudança de valor)
        let (t_rec, mut last, mut first, mut per_s) = (Instant::now(), frame_n(), None, [0u64; 5]);
        while t_rec.elapsed() < Duration::from_secs(5) {
            tokio::time::sleep(Duration::from_millis(10)).await;
            let n = frame_n();
            if n != last { last = n; let el = t_rec.elapsed(); if first.is_none() { first = Some(el); } per_s[(el.as_secs() as usize).min(4)] += 1; }
        }
        eprintln!("rodada {round}: vídeo do espectador depois de reconectar: 1º quadro novo em {} ms; quadros por segundo nos 5 s seguintes: {per_s:?}", first.map(|d| d.as_millis() as i64).unwrap_or(-1));
        video_back.push((first, per_s));
        // voz nos dois sentidos, para todos
        for c in cs { for o in cs { if c.ident != o.ident { let f = voice(c, &o.ident, 1).await; assert!(f > 25, "rodada {round}: {} sem voz de {} depois de voltar ({f} quadros/s)", c.ident, o.ident); } } }
        // a transmissão volta para quem assistia, sem novo share.watch
        let end = Instant::now() + Duration::from_secs(25);
        while Instant::now() < end && frame_n() < n_before + 20 { tokio::time::sleep(Duration::from_millis(100)).await; }
        let n_after = frame_n(); let got = n_after.saturating_sub(n_before);
        eprintln!("rodada {round}: contador de quadros do espectador {n_before} -> {n_after}");
        assert!(got >= 20, "rodada {round}: vídeo não voltou para o espectador ({got} quadros novos)");
        // o que foi enviado durante a queda chega
        assert!(d.wait(20, &format!("chat:{}:durante a queda {round}", b.fp)).await, "rodada {round}: DM enviada durante a queda não chegou");
        for c in [&b, &d] { assert!(c.wait(20, &format!("state:{A}:mic=true:voltei {round}")).await, "rodada {round}: {} não viu o status mudado durante a queda", c.ident); }
        eprintln!("rodada {round}: voz (3x2 sentidos), vídeo assistido ({got} quadros novos), DM e status da queda entregues");
    }
    // passado o prazo de tolerância, ninguém foi dado como saído nem a transmissão como encerrada, e ninguém foi "expulso"
    tokio::time::sleep(Duration::from_secs(22)).await;
    for c in cs {
        assert_eq!(c.count(":fechado"), 0, "{} viu alguém sair: {:?}", c.ident, c.log.lock().unwrap().iter().filter(|s| s.contains("fechado")).collect::<Vec<_>>());
        assert_eq!(c.count("kicked:"), 0);
        assert_eq!(c.count(r#""state":"lost""#), 0);
    }
    assert_eq!(b.count(r#""ev":"share.remote","peer_id":"aaaaaaaa-1111-4111-8111-000000000001","state":"off""#), 0, "a transmissão piscou para o espectador");
    let n1 = frame_n(); tokio::time::sleep(Duration::from_secs(2)).await; let n2 = frame_n();
    assert!(n2 > n1 + 10, "vídeo parou depois do prazo (contador {n1} -> {n2} em 2 s)");
    // saída de verdade continua sendo vista
    d.cmd.send(MeshCmd::Close).await.unwrap();
    assert!(a.wait(15, &format!("peer:{D}:fechado")).await && b.wait(15, &format!("peer:{D}:fechado")).await, "saída real não foi vista depois das reconexões");
    for c in [&a, &b] { let _ = c.cmd.send(MeshCmd::Close).await; }
    tokio::time::sleep(Duration::from_millis(300)).await;
    drop(srv);
    let _ = std::fs::remove_dir_all(&dir);
}
