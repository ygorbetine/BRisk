//! IPC local com a UI: TCP em 127.0.0.1 (porta livre), uma conexão por vez, linhas JSON.
//! A primeira linha deve ser {"cmd":"hello","token":"<BRIKS_CORE_TOKEN>"}; token errado fecha a conexão.
use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;

use anyhow::Result;
use serde_json::{json, Value};
use tokio::io::{AsyncBufReadExt, AsyncRead, AsyncReadExt, AsyncWriteExt, BufReader};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::mpsc;

use crate::audio::Audio;
use crate::identity::Identity;
use crate::mesh::{self, MeshCfg, MeshCmd, MeshEvent};
use crate::realtime::{self, RealtimeCfg};
use crate::signaling::{secure_room_id, topic_name};

pub struct Config {
    pub data_dir: PathBuf,
    pub realtime_url: Option<String>,
    pub realtime_key: Option<String>,
}

fn read_env_file(path: &str) -> std::collections::HashMap<String, String> {
    let mut m = std::collections::HashMap::new();
    if let Ok(t) = std::fs::read_to_string(path) {
        for l in t.lines() {
            let l = l.trim();
            if l.starts_with('#') {
                continue;
            }
            if let Some((k, v)) = l.split_once('=') {
                m.insert(k.trim().to_string(), v.trim().trim_matches('"').to_string());
            }
        }
    }
    m
}

/// URL/chave do Realtime: `BRIKS_REALTIME_URL`/`BRIKS_REALTIME_KEY` (loopback/testes) ou `VITE_SUPABASE_URL`/`VITE_SUPABASE_ANON_KEY`
/// (as variáveis do .env.example do repositório), do ambiente ou do arquivo apontado por `BRIKS_ENV_FILE`.
pub fn config_from_env() -> Config {
    let file = std::env::var("BRIKS_ENV_FILE").map(|p| read_env_file(&p)).unwrap_or_default();
    let get = |k: &str| std::env::var(k).ok().filter(|v| !v.trim().is_empty()).or_else(|| file.get(k).cloned().filter(|v| !v.trim().is_empty()));
    let data_dir = std::env::var("BRIKS_CORE_DATA")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from(std::env::var("APPDATA").unwrap_or_else(|_| ".".into())).join("Briks-CUI"));
    Config {
        data_dir,
        realtime_url: get("BRIKS_REALTIME_URL").or_else(|| get("VITE_SUPABASE_URL")),
        realtime_key: get("BRIKS_REALTIME_KEY").or_else(|| get("VITE_SUPABASE_ANON_KEY")),
    }
}

pub fn load_or_create_identity(dir: &std::path::Path) -> Result<Identity> {
    std::fs::create_dir_all(dir)?;
    let p = dir.join("core-identity.json");
    if let Ok(t) = std::fs::read_to_string(&p) {
        if let Ok(v) = serde_json::from_str::<Value>(&t) {
            if let Some(s) = v["secret"].as_str() {
                if let Ok(id) = Identity::from_secret_b64(s) {
                    return Ok(id);
                }
            }
        }
    }
    let id = Identity::generate();
    std::fs::write(&p, json!({ "version": 1, "secret": id.secret_b64(), "note": "identidade ECDSA P-256 do app nativo; não é a chave do Electron" }).to_string())?;
    Ok(id)
}

/// Troca a identidade deste aparelho (recuperação pela frase). A anterior fica em `core-identity.prev.json`.
pub fn install_identity(dir: &std::path::Path, id: &Identity) -> Result<()> {
    std::fs::create_dir_all(dir)?;
    let p = dir.join("core-identity.json");
    if p.exists() {
        let _ = std::fs::copy(&p, dir.join("core-identity.prev.json"));
    }
    let tmp = dir.join("core-identity.tmp");
    std::fs::write(&tmp, json!({ "version": 1, "secret": id.secret_b64(), "note": "identidade ECDSA P-256 do app nativo; recuperada da cópia da conta" }).to_string())?;
    std::fs::rename(&tmp, &p).or_else(|_| {
        let _ = std::fs::remove_file(&p);
        std::fs::rename(&tmp, &p)
    })?;
    Ok(())
}

/// `audio`: "source" (da fonte: a árvore do processo da janela, ou tudo menos Briks/Discord numa tela), "apps" (+ `pids`), "none".
fn audio_mode(v: &Value, kind: &crate::capture::SourceKind) -> crate::procaudio::AudioMode {
    use crate::procaudio::AudioMode;
    match v["audio"].as_str().unwrap_or("none") {
        "apps" => AudioMode::Pids(v["pids"].as_array().map(|a| a.iter().filter_map(|p| p.as_u64().map(|p| p as u32)).collect()).unwrap_or_default()),
        "source" => match kind { crate::capture::SourceKind::Window(h) => match crate::capture::window_pid(*h) { 0 => AudioMode::None, p => AudioMode::Tree(p) }, _ => AudioMode::AllExcept },
        _ => AudioMode::None,
    }
}

pub async fn bind() -> Result<(TcpListener, u16)> {
    let l = TcpListener::bind(("127.0.0.1", 0)).await?;
    let p = l.local_addr()?.port();
    Ok((l, p))
}

pub async fn serve(l: TcpListener, token: String, cfg: Config) -> Result<()> {
    let realtime = cfg.realtime_url.clone().zip(cfg.realtime_key.clone());
    let accounts = crate::accounts::service::Runtime::build(cfg.data_dir.clone(), realtime).await;
    loop {
        let (s, _) = l.accept().await?;
        let _ = handle(s, &token, &cfg, accounts.clone()).await; // uma UI por vez; ao cair, aceita a próxima
    }
}

/// Maior linha aceita da UI. Sem teto, um processo local sem token mandando bytes sem quebra de linha enchia a memória.
const MAX_LINE: usize = 256 * 1024;

/// Leitor de linhas com teto. Seguro para `select!`: o que já foi lido fica em `buf` e a próxima chamada continua.
struct Lines<R> { rd: BufReader<R>, buf: Vec<u8> }
impl<R: AsyncRead + Unpin> Lines<R> {
    fn new(r: R) -> Self { Lines { rd: BufReader::new(r), buf: Vec::new() } }
    async fn next(&mut self) -> Result<Option<String>> {
        loop {
            let left = (MAX_LINE + 1).saturating_sub(self.buf.len());
            if left == 0 { anyhow::bail!("linha grande demais"); }
            let n = (&mut self.rd).take(left as u64).read_until(b'\n', &mut self.buf).await?;
            let done = self.buf.last() == Some(&b'\n');
            if done || n == 0 {
                if self.buf.is_empty() { return Ok(None); }
                let mut l = std::mem::take(&mut self.buf);
                if done { l.pop(); if l.last() == Some(&b'\r') { l.pop(); } }
                return Ok(Some(String::from_utf8_lossy(&l).into_owned()));
            }
        }
    }
}

async fn out(w: &mut tokio::net::tcp::OwnedWriteHalf, v: Value) -> Result<()> {
    let mut s = v.to_string();
    s.push('\n');
    w.write_all(s.as_bytes()).await?;
    Ok(())
}

async fn handle(s: TcpStream, token: &str, cfg: &Config, accounts: Arc<crate::accounts::service::Runtime>) -> Result<()> {
    s.set_nodelay(true).ok();
    let (rd, mut wr) = s.into_split();
    let mut lines = Lines::new(rd);
    let first = tokio::time::timeout(Duration::from_secs(5), lines.next()).await??.unwrap_or_default();
    let hello: Value = serde_json::from_str(&first).unwrap_or(Value::Null);
    if hello["cmd"] != "hello" || hello["token"].as_str() != Some(token) {
        let _ = out(&mut wr, json!({ "ev": "error", "where": "hello", "message": "token inválido" })).await;
        return Ok(());
    }
    out(&mut wr, json!({ "ev": "hello.ok", "version": 1 })).await?;

    let (acct_tx, mut acct_rx) = mpsc::channel::<Value>(64);
    let mut account = crate::accounts::service::Service::new(accounts, acct_tx);   // contas online: account.*, friends.*, import.*
    let mut identity: Option<Arc<Identity>> = None;
    let mut name = String::from("Usuário");
    let mut local_peer = uuid::Uuid::new_v4().to_string();
    let mut mesh_cmd: Option<mpsc::Sender<MeshCmd>> = None;
    let mut mesh_ev: Option<mpsc::Receiver<MeshEvent>> = None;
    let mut rt_status: Option<mpsc::Receiver<String>> = None;
    let mut audio: Option<Arc<Audio>> = None;
    let mut kicked_rooms: std::collections::HashSet<String> = Default::default();   // salas de onde fui expulso nesta sessão do core
    let mut cur_room = String::new();
    let (mut mic_muted, mut deafened) = (false, false);
    let mut profile: (String, String) = (String::new(), String::new());   // status e presença locais, reenviados ao entrar numa sala

    loop {
        tokio::select! {
            l = lines.next() => {
                let Some(l) = l? else { break };
                let Ok(v) = serde_json::from_str::<Value>(&l) else { continue };
                match v["cmd"].as_str().unwrap_or("") {
                    "session.start" => {
                        name = v["display_name"].as_str().unwrap_or("Usuário").chars().take(64).collect();
                        match load_or_create_identity(&cfg.data_dir) {
                            Ok(id) => {
                                out(&mut wr, json!({ "ev": "session.ready", "fingerprint": id.fingerprint(), "peer_id": local_peer, "identity": id.public_jwk() })).await?;
                                identity = Some(Arc::new(id));
                            }
                            Err(e) => out(&mut wr, json!({ "ev": "error", "where": "session.start", "message": e.to_string() })).await?,
                        }
                    }
                    "room.join" => {
                        let Some(id) = identity.clone() else {
                            out(&mut wr, json!({ "ev": "error", "where": "room.join", "message": "session.start primeiro" })).await?;
                            continue;
                        };
                        let (Some(url), Some(key)) = (cfg.realtime_url.clone(), cfg.realtime_key.clone()) else {
                            out(&mut wr, json!({ "ev": "signaling.status", "state": "erro", "message": "Supabase Realtime não configurado (VITE_SUPABASE_URL e VITE_SUPABASE_ANON_KEY)" })).await?;
                            continue;
                        };
                        let ns = v["namespace"].as_str().unwrap_or("room").to_string();
                        let room = v["room"].as_str().unwrap_or("").to_string();
                        if !["room", "chat", "friend", "group"].contains(&ns.as_str()) || room.is_empty() || room.len() > 200 {
                            out(&mut wr, json!({ "ev": "error", "where": "room.join", "message": "sala inválida" })).await?;
                            continue;
                        }
                        if kicked_rooms.contains(&format!("{ns}:{room}")) {
                            out(&mut wr, json!({ "ev": "error", "where": "room.join", "message": "você foi removido desta sala nesta sessão" })).await?;
                            continue;
                        }
                        cur_room = format!("{ns}:{room}");
                        if let Some(c) = mesh_cmd.take() { let _ = c.send(MeshCmd::Close).await; }
                        local_peer = uuid::Uuid::new_v4().to_string();
                        let topic = topic_name(&ns, &secure_room_id(&ns, &room));
                        let joined_at = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0);
                        let (rt, mut rt_rx) = realtime::connect(RealtimeCfg::new(&url, &key), topic, local_peer.clone(), json!({ "peerId": local_peer, "joinedAt": joined_at, "clientVersion": "native-0.1" }));
                        let (st_tx, st_rx) = mpsc::channel::<String>(16);
                        let (fw_tx, fw_rx) = mpsc::channel(256);
                        tokio::spawn(async move {
                            while let Some(e) = rt_rx.recv().await {
                                if let realtime::RtEvent::Status(s) = &e { let _ = st_tx.send(s.clone()).await; }
                                if fw_tx.send(e).await.is_err() { break; }
                            }
                        });
                        let audio_on = std::env::var("BRIKS_CORE_AUDIO").map(|v| v != "off").unwrap_or(true);
                        let (au, au_rx) = if audio_on { let (a, r) = Audio::new(Audio::fake_from_env()); (Some(a), Some(r)) } else { (None, None) };
                        if let Some(a) = &au { a.set_mic_muted(mic_muted); a.set_deaf(deafened); }
                        audio = au.clone();
                        let (ice_list, ice_warn) = crate::ice::load(&|k| std::env::var(k).ok(), &cfg.data_dir);
                        out(&mut wr, json!({ "ev": "ice.info", "servers": ice_list.len(), "turn": crate::ice::has_turn(&ice_list), "warn": ice_warn.unwrap_or_default() })).await?;
                        let h = mesh::spawn(MeshCfg { namespace: ns, room, local_peer: local_peer.clone(), display_name: name.clone(), ice_servers: ice_list.clone(), audio: au.zip(au_rx), owner_fp: v["owner_fp"].as_str().map(String::from), joined_at }, id, rt, fw_rx);
                        mesh_cmd = Some(h.cmd);
                        if !profile.0.is_empty() || !profile.1.is_empty() { if let Some(c) = &mesh_cmd { let _ = c.send(MeshCmd::SetProfile { status: profile.0.clone(), presence: profile.1.clone() }).await; } }
                        mesh_ev = Some(h.events);
                        rt_status = Some(st_rx);
                        out(&mut wr, json!({ "ev": "room.joining", "peer_id": local_peer })).await?;
                    }
                    "room.leave" => {
                        if let Some(c) = mesh_cmd.take() { let _ = c.send(MeshCmd::Close).await; }
                        mesh_ev = None;
                        rt_status = None;
                        out(&mut wr, json!({ "ev": "signaling.status", "state": "desconectado" })).await?;
                    }
                    "chat.send" => {
                        let (Some(to), Some(text)) = (v["to_fp"].as_str(), v["text"].as_str()) else { continue };
                        if let Some(c) = &mesh_cmd {
                            let _ = c.send(MeshCmd::Chat { to_fp: to.into(), text: text.chars().take(crate::identity::MAX_CHAT_CHARS).collect() }).await;
                        }
                    }
                    "chat.event.send" => {
                        let (Some(ch), Some(target), Some(action)) = (v["channel"].as_str(), v["target"].as_str(), v["action"].as_str()) else { continue };
                        let opt = |k: &str| v[k].as_str().map(|x| x.to_string());
                        if let Some(c) = &mesh_cmd {
                            let _ = c.send(MeshCmd::Event { channel: ch.into(), target: target.into(), action: action.into(), content: opt("content"), reference: opt("reference"), emoji: opt("emoji") }).await;
                        }
                    }
                    "group.sign" | "group.verify" => {
                        // grupo P2P (group_sync.rs): o core só assina e confere; quem guarda o grupo (e o estado já confiado) é a UI
                        let sign = v["cmd"] == "group.sign";
                        let me = match &identity { Some(i) => Some(i.clone()), None => load_or_create_identity(&cfg.data_dir).ok().map(Arc::new) };
                        let Some(me) = me else { out(&mut wr, json!({ "ev": "group.result", "op": if sign { "sign" } else { "verify" }, "ok": false, "message": "identidade indisponível" })).await?; continue };
                        identity = Some(me.clone());
                        let ev = if sign {
                            match serde_json::from_value::<crate::group_sync::Snapshot>(v["snapshot"].clone()) {
                                Ok(mut s) => {
                                    s.ts = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0);
                                    match crate::group_sync::sign(&me, &mut s) {
                                        Ok(_) => json!({ "ev": "group.result", "op": "sign", "ok": true, "group_id": s.group_id, "snapshot": s, "code": crate::group_sync::to_code(&s) }),
                                        Err(e) => json!({ "ev": "group.result", "op": "sign", "ok": false, "group_id": s.group_id, "message": e.to_string() }),
                                    }
                                }
                                Err(_) => json!({ "ev": "group.result", "op": "sign", "ok": false, "message": "grupo inválido" }),
                            }
                        } else {
                            let parsed = match v["code"].as_str() { Some(c) => crate::group_sync::from_code(c), None => serde_json::from_value(v["snapshot"].clone()).map_err(|_| anyhow::anyhow!("grupo inválido")) };
                            let trusted = serde_json::from_value::<crate::group_sync::Snapshot>(v["trusted"].clone()).ok();
                            match parsed.and_then(|s| crate::group_sync::accept(trusted.as_ref(), &s, &me.fingerprint()).map(|d| (s, d))) {
                                Ok((s, d)) => json!({ "ev": "group.result", "op": "verify", "ok": true, "group_id": s.group_id, "decision": format!("{d:?}").to_lowercase(), "snapshot": s, "tag": v["tag"] }),
                                Err(e) => json!({ "ev": "group.result", "op": "verify", "ok": false, "message": e.to_string(), "tag": v["tag"] }),
                            }
                        };
                        out(&mut wr, ev).await?;
                    }
                    "group.send" => {
                        if let Some(c) = &mesh_cmd { let _ = c.send(MeshCmd::GroupSend { snapshot: v["snapshot"].clone() }).await; }
                    }
                    "mic.set" | "deafen.set" => {
                        // a UI manda {"on":bool}: mic.set on=microfone ligado; deafen.set on=ensurdecido
                        if v["cmd"] == "mic.set" { mic_muted = v["on"].as_bool().map(|o| !o).or(v["muted"].as_bool()).unwrap_or(false); }
                        else { deafened = v["on"].as_bool().or(v["deafened"].as_bool()).unwrap_or(false); }
                        if let Some(a) = &audio { a.set_mic_muted(mic_muted); a.set_deaf(deafened); }
                        if let Some(c) = &mesh_cmd { let _ = c.send(MeshCmd::BroadcastState).await; }
                        // deafen implica mic mudo: o estado efetivo é o que a UI mostra
                        out(&mut wr, json!({ "ev": "mic.state", "microphone": !mic_muted && !deafened, "muted": mic_muted, "deafened": deafened })).await?;
                    }
                    "profile.set" => {
                        profile = (v["status"].as_str().unwrap_or("").chars().take(64).collect(), v["presence"].as_str().unwrap_or("").chars().take(64).collect());
                        if let Some(c) = &mesh_cmd { let _ = c.send(MeshCmd::SetProfile { status: profile.0.clone(), presence: profile.1.clone() }).await; }
                    }
                    "peer.kick" => {
                        let fp = v["fp"].as_str().unwrap_or("").to_string();
                        if let (false, Some(c)) = (fp.is_empty(), &mesh_cmd) { let _ = c.send(MeshCmd::Kick { fp, reason: v["reason"].as_str().unwrap_or("").to_string() }).await; }
                    }
                    "room.allow" => {
                        let fp = v["fp"].as_str().unwrap_or("").to_string();
                        if let (false, Some(c)) = (fp.is_empty(), &mesh_cmd) { let _ = c.send(MeshCmd::Allow { fp }).await; }
                    }
                    "peer.mute" => {
                        let key = v["peer"].as_str().or(v["fp"].as_str()).unwrap_or("").to_string();
                        if let (false, Some(c)) = (key.is_empty(), &mesh_cmd) { let _ = c.send(MeshCmd::MutePeer { key, muted: v["muted"].as_bool().unwrap_or(true) }).await; }
                    }
                    "share.list" => {
                        let dir = cfg.data_dir.join("thumbs");
                        let v = tokio::task::spawn_blocking(move || crate::capture::list_sources(&dir)).await.unwrap_or(Value::Null);
                        out(&mut wr, json!({ "ev": "share.sources", "screens": v["screens"], "windows": v["windows"] })).await?;
                    }
                    "share.start" => {
                        let Some(c) = &mesh_cmd else { out(&mut wr, json!({ "ev": "share.local", "state": "off", "reason": "entre numa sala primeiro" })).await?; continue };
                        let id: isize = v["id"].as_str().and_then(|s| s.parse().ok()).or(v["id"].as_i64().map(|n| n as isize)).unwrap_or(0);
                        let kind = match v["kind"].as_str().unwrap_or("screen") { "window" => crate::capture::SourceKind::Window(id), "test" => crate::capture::SourceKind::Test, _ => crate::capture::SourceKind::Screen(id) };
                        let kind2 = kind.clone();
                        let scfg = crate::share::ShareCfg { kind, height: v["height"].as_u64().unwrap_or(1080).clamp(240, 2160) as usize, fps: v["fps"].as_u64().unwrap_or(30).clamp(5, 60) as u32,
                            kbps: v["kbps"].as_u64().unwrap_or(0).min(50_000) as u32, label: v["label"].as_str().unwrap_or("").chars().take(64).collect(), audio: audio_mode(&v, &kind2),
                            content: if v["content"] == "motion" { crate::adapt::Content::Motion } else { crate::adapt::Content::Detail }, adaptive: v["adaptive"].as_bool().unwrap_or(true) };
                        let preview = cfg.data_dir.join("video").join("preview.bin");
                        let dump = std::env::var("BRIKS_SHARE_DUMP").ok().map(PathBuf::from);
                        let _ = c.send(MeshCmd::ShareStart { cfg: scfg, preview: Some(preview.clone()), dump }).await;
                    }
                    "share.audio.list" => {
                        let apps = tokio::task::spawn_blocking(crate::procaudio::list_apps).await.unwrap_or_default();
                        out(&mut wr, json!({ "ev": "share.audio.apps", "apps": apps })).await?;
                    }
                    "share.audio.set" => {
                        let mode = audio_mode(&v, &crate::capture::SourceKind::Test);
                        if let Some(c) = &mesh_cmd { let _ = c.send(MeshCmd::ShareAudio { mode }).await; }
                    }
                    "share.volume" => {
                        let key = v["peer"].as_str().unwrap_or("").to_string();
                        if let (false, Some(c)) = (key.is_empty(), &mesh_cmd) { let _ = c.send(MeshCmd::ShareVolume { key, gain: v["volume"].as_f64().unwrap_or(1.0) as f32 }).await; }
                    }
                    "share.stop" => { if let Some(c) = &mesh_cmd { let _ = c.send(MeshCmd::ShareStop).await; } }
                    "share.watch" => {
                        let key = v["peer"].as_str().unwrap_or("").to_string();
                        let safe: String = key.chars().filter(|c| c.is_ascii_alphanumeric() || *c == '-').collect();
                        if let (false, Some(c)) = (key.is_empty(), &mesh_cmd) { let _ = c.send(MeshCmd::ShareWatch { key, on: v["on"].as_bool().unwrap_or(true), path: cfg.data_dir.join("video").join(format!("remote-{safe}.bin")) }).await; }
                    }
                    "ping" => out(&mut wr, json!({ "ev": "pong" })).await?,
                    other => { account.command(other, &v); }
                }
            }
            Some(ev) = acct_rx.recv() => out(&mut wr, ev).await?,
            e = async { match mesh_ev.as_mut() { Some(r) => r.recv().await, None => std::future::pending().await } } => {
                match e {
                    Some(MeshEvent::Peer { peer_id, state }) => out(&mut wr, json!({ "ev": "peer", "peer_id": peer_id, "state": state })).await?,
                    Some(MeshEvent::ChatEvent { from_fp, wire, local }) => out(&mut wr, json!({ "ev": "chat.event", "from_fp": from_fp, "local": local, "wire_raw": wire.to_string(), "wire": wire })).await?,
                    Some(MeshEvent::GroupSnapshot { from_fp, snapshot }) => out(&mut wr, json!({ "ev": "group.snapshot", "from_fp": from_fp, "snapshot": snapshot })).await?,
                    Some(MeshEvent::Chat { channel, from_fp, author, content, id, ts }) => out(&mut wr, json!({ "ev": "chat.message", "channel": channel, "from_fp": from_fp, "author": author, "content": content, "id": id, "ts": ts })).await?,
                    Some(MeshEvent::PeerInfo { peer_id, fp, name }) => out(&mut wr, json!({ "ev": "peer.info", "peer_id": peer_id, "fp": fp, "name": name })).await?,
                    Some(MeshEvent::Owner { fp, me }) => out(&mut wr, json!({ "ev": "room.owner", "fp": fp, "me": me })).await?,
                    Some(MeshEvent::Banned { fp }) => out(&mut wr, json!({ "ev": "peer.banned", "fp": fp })).await?,
                    Some(MeshEvent::Kicked { reason }) => {
                        kicked_rooms.insert(cur_room.clone());
                        out(&mut wr, json!({ "ev": "kicked", "reason": reason })).await?;
                    }
                    Some(MeshEvent::Speaking { peer_id, level }) => out(&mut wr, json!({ "ev": "speaking", "peer": peer_id, "level": level })).await?,
                    Some(MeshEvent::PeerState { peer_id, microphone, deafened, status, presence }) => out(&mut wr, json!({ "ev": "peer.state", "peer_id": peer_id, "microphone": microphone, "deafened": deafened, "status": status, "presence": presence })).await?,
                    Some(MeshEvent::Share(v)) => out(&mut wr, v).await?,
                    Some(MeshEvent::Error(m)) => out(&mut wr, json!({ "ev": "error", "where": "mesh", "message": m })).await?,
                    None => { mesh_ev = None; }
                }
            }
            s = async { match rt_status.as_mut() { Some(r) => r.recv().await, None => std::future::pending().await } } => {
                if let Some(s) = s {
                    let (state, msg) = match s.as_str() {
                        "connecting" => ("conectando", String::new()),
                        "connected" => ("conectado", String::new()),
                        "reconnecting" => ("reconectando", String::new()),
                        "disconnected" => ("desconectado", String::new()),
                        e => ("erro", e.trim_start_matches("error: ").to_string()),
                    };
                    out(&mut wr, json!({ "ev": "signaling.status", "state": state, "message": msg })).await?;
                }
            }
        }
    }
    if let Some(c) = mesh_cmd.take() { let _ = c.send(MeshCmd::Close).await; }
    Ok(())
}
