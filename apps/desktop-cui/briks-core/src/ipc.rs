//! IPC local com a UI: TCP em 127.0.0.1 (porta livre), uma conexão por vez, linhas JSON.
//! A primeira linha deve ser {"cmd":"hello","token":"<BRIKS_CORE_TOKEN>"}; token errado fecha a conexão.
use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;

use anyhow::Result;
use serde_json::{json, Value};
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
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

pub async fn bind() -> Result<(TcpListener, u16)> {
    let l = TcpListener::bind(("127.0.0.1", 0)).await?;
    let p = l.local_addr()?.port();
    Ok((l, p))
}

pub async fn serve(l: TcpListener, token: String, cfg: Config) -> Result<()> {
    loop {
        let (s, _) = l.accept().await?;
        let _ = handle(s, &token, &cfg).await; // uma UI por vez; ao cair, aceita a próxima
    }
}

async fn out(w: &mut tokio::net::tcp::OwnedWriteHalf, v: Value) -> Result<()> {
    let mut s = v.to_string();
    s.push('\n');
    w.write_all(s.as_bytes()).await?;
    Ok(())
}

async fn handle(s: TcpStream, token: &str, cfg: &Config) -> Result<()> {
    s.set_nodelay(true).ok();
    let (rd, mut wr) = s.into_split();
    let mut lines = BufReader::new(rd).lines();
    let first = tokio::time::timeout(Duration::from_secs(5), lines.next_line()).await??.unwrap_or_default();
    let hello: Value = serde_json::from_str(&first).unwrap_or(Value::Null);
    if hello["cmd"] != "hello" || hello["token"].as_str() != Some(token) {
        let _ = out(&mut wr, json!({ "ev": "error", "where": "hello", "message": "token inválido" })).await;
        return Ok(());
    }
    out(&mut wr, json!({ "ev": "hello.ok", "version": 1 })).await?;

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
            l = lines.next_line() => {
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
                        let h = mesh::spawn(MeshCfg { namespace: ns, room, local_peer: local_peer.clone(), display_name: name.clone(), ice_servers: vec![], audio: au.zip(au_rx), owner_fp: v["owner_fp"].as_str().map(String::from), joined_at }, id, rt, fw_rx);
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
                    "ping" => out(&mut wr, json!({ "ev": "pong" })).await?,
                    _ => {}
                }
            }
            e = async { match mesh_ev.as_mut() { Some(r) => r.recv().await, None => std::future::pending().await } } => {
                match e {
                    Some(MeshEvent::Peer { peer_id, state }) => out(&mut wr, json!({ "ev": "peer", "peer_id": peer_id, "state": state })).await?,
                    Some(MeshEvent::ChatEvent { from_fp, wire, local }) => out(&mut wr, json!({ "ev": "chat.event", "from_fp": from_fp, "local": local, "wire_raw": wire.to_string(), "wire": wire })).await?,
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
