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

/// Variáveis do LiveKit: ambiente e, se houver, o arquivo de `BRIKS_ENV_FILE` (mesma regra do resto da configuração).
fn net_env(k: &str) -> Option<String> {
    std::env::var(k).ok().filter(|v| !v.trim().is_empty()).or_else(|| {
        let f = std::env::var("BRIKS_ENV_FILE").ok()?;
        read_env_file(&f).get(k).cloned().filter(|v| !v.trim().is_empty())
    })
}

/// Tenta entrar na sala pelo SFU: token do Supabase (sessão do usuário), depois sinalização LiveKit. Erro = texto para o diagnóstico.
pub(crate) async fn try_sfu(sc: &crate::sfu_cfg::SfuConfig, accounts: &crate::accounts::service::Runtime, ns: &str, room: &str, audio: Option<(Arc<Audio>, mpsc::Receiver<crate::audio::AudioEvent>)>, data: Option<crate::sfudata::DataCfg>) -> Result<crate::sfu::SfuHandle, String> {
    let acc = accounts.acc.clone().ok_or("entre na sua conta Briks para usar o servidor de chamadas")?;
    let access = acc.access_token().await.map_err(|e| format!("sessão: {e}"))?;
    let lk_room = format!("{ns}:{}", &secure_room_id(ns, room)[..32]);
    let (url, room2) = (sc.token_url.clone(), lk_room.clone());
    let (token, told_url) = tokio::task::spawn_blocking(move || crate::sfu::fetch_token_ex(&url, &access, &room2)).await.map_err(|e| e.to_string())?.map_err(|e| e.msg)?;
    // O serviço de tokens diz para qual servidor o token vale. Se for outro que não o configurado aqui, vale o do serviço (é ele que
    // tem a chave); se a URL que ele mandou não for aceitável (esquema), erro claro em vez de uma recusa sem explicação.
    let norm = |u: &str| u.trim().trim_end_matches('/').to_ascii_lowercase();
    let server_url = match told_url {
        Some(u) if norm(&u) != norm(&sc.url) => { crate::sfu_cfg::check_server_url(&u).map_err(|e| format!("URL do servidor divergente (o serviço de tokens informou outra, inválida): {e}"))?; u }
        _ => sc.url.clone(),
    };
    // reconexão: token novo pela mesma função, com a sessão da conta renovada se preciso (o token da sala vale ~1 h)
    let token_url = sc.token_url.clone();
    let renew: crate::sfu::TokenFn = Arc::new(move || {
        let (acc, url, room) = (acc.clone(), token_url.clone(), lk_room.clone());
        Box::pin(async move {
            let access = acc.access_token().await.map_err(|e| crate::sfu::TokenFail::from(format!("sessão: {e}")))?;
            tokio::task::spawn_blocking(move || crate::sfu::fetch_token_ex(&url, &access, &room).map(|t| t.0)).await.map_err(|e| crate::sfu::TokenFail::from(e.to_string()))?
        })
    });
    crate::sfu::connect(crate::sfu::SfuJoin { url: server_url, token, audio, data, renew: Some(renew) }).await.map_err(|e| e.to_string())
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
    serve_with(l, token, cfg, accounts).await
}

/// Como `serve`, com o serviço de contas já montado (os testes entram com uma conta do servidor simulado).
pub async fn serve_with(l: TcpListener, token: String, cfg: Config, accounts: Arc<crate::accounts::service::Runtime>) -> Result<()> {
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
    let turn_rt = accounts.clone();   // cache do TURN da conta
    let ev_tx = acct_tx.clone();   // eventos de tarefas em segundo plano (teste de rede)
    let mut account = crate::accounts::service::Service::new(accounts.clone(), acct_tx);   // contas online: account.*, friends.*, import.*
    let mut identity: Option<Arc<Identity>> = None;
    let mut name = String::from("Usuário");
    let mut local_peer = uuid::Uuid::new_v4().to_string();
    let mut mesh_cmd: Option<mpsc::Sender<MeshCmd>> = None;
    let mut mesh_ev: Option<mpsc::Receiver<MeshEvent>> = None;
    let (link_tx, mut link_rx) = mpsc::channel::<Value>(16);   // respostas de pré-visualização de links (busca em threads)
    let mut rt_status: Option<mpsc::Receiver<String>> = None;
    let mut audio: Option<Arc<Audio>> = None;
    let mut audio_cfg = crate::audio::AudioCfg::default();   // última Configurações > Voz; vale também para a sala que ainda vai nascer
    let mut mic_test: Option<crate::audio::MicTest> = None;
    let mut kicked_rooms: std::collections::HashSet<String> = Default::default();   // salas de onde fui expulso nesta sessão do core
    let mut cur_room = String::new();
    let (mut mic_muted, mut deafened) = (false, false);
    let mut profile: (String, String) = (String::new(), String::new());   // status e presença locais, reenviados ao entrar numa sala
    // SFU puro: sala no LiveKit e URL da função de expulsão; impressão digital -> identidade no SFU; sou o dono?
    let mut sfu_room: Option<(String, Option<String>)> = None;
    let mut fp_ident: std::collections::HashMap<String, String> = Default::default();
    let mut owner_me = false;
    let (kick_tx, mut kick_rx) = mpsc::channel::<Value>(8);

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
                        {
                            let (ns, room) = (v["namespace"].as_str().unwrap_or("room"), v["room"].as_str().unwrap_or(""));
                            if !["room", "chat", "friend", "group"].contains(&ns) || room.is_empty() || room.len() > 200 {
                                out(&mut wr, json!({ "ev": "error", "where": "room.join", "message": "sala inválida" })).await?;
                                continue;
                            }
                            if kicked_rooms.contains(&format!("{ns}:{room}")) {
                                out(&mut wr, json!({ "ev": "error", "where": "room.join", "message": "você foi removido desta sala nesta sessão" })).await?;
                                continue;
                            }
                        }
                        // Modo de rede (Automático / SFU / P2P). Com o SFU configurado, Automático e SFU entram pelo servidor de chamadas;
                        // se o token ou a entrada falharem, a chamada NÃO abre em P2P (sala dividida), a não ser com a reserva ligada.
                        let sfu_ns = v["namespace"].as_str().map(|n| n == "room" || n == "group").unwrap_or(true);
                        let net_mode = v["net"].as_str().and_then(crate::sfu_cfg::NetMode::parse).unwrap_or_else(|| crate::sfu_cfg::load_mode(&net_env, &cfg.data_dir));
                        let mut sfu_err = String::new();
                        let mut sfu_failed = false;
                        let mut hybrid: Option<crate::sfu::SfuHandle> = None;
                        let mut sfu_au: Option<Arc<Audio>> = None;
                        let sfu_conf = crate::sfu_cfg::resolve(&net_env, &cfg.data_dir);
                        let sfu_ready = matches!(sfu_conf, Ok(Some(_)));
                        if let Err(e) = &sfu_conf { sfu_err = e.clone(); }
                        if sfu_ns && net_mode != crate::sfu_cfg::NetMode::P2p {
                            if let Ok(Some(sc)) = &sfu_conf {
                                let (ns, room) = (v["namespace"].as_str().unwrap_or("room").to_string(), v["room"].as_str().unwrap_or("").to_string());
                                if !room.is_empty() && room.len() <= 200 && !kicked_rooms.contains(&format!("{ns}:{room}")) {
                                    if let Some(c) = mesh_cmd.take() { let _ = c.send(MeshCmd::Close).await; }
                                    let audio_on = std::env::var("BRIKS_CORE_AUDIO").map(|v| v != "off").unwrap_or(true);
                                    let (au, au_rx) = if audio_on { let (a, r) = Audio::new(Audio::fake_from_env()); (Some(a), Some(r)) } else { (None, None) };
                                    if let Some(a) = &au { a.set_mic_muted(mic_muted); a.set_deaf(deafened); a.apply_cfg(&audio_cfg); }
                                    // Padrão: chat/status/presença/expulsão pelas data channels do próprio LiveKit (sem teto de peers). BRIKS_SFU_MESH=1: híbrido com a malha P2P (até 5).
                                    let with_mesh = std::env::var("BRIKS_SFU_MESH").map(|v| v == "1").unwrap_or(false);
                                    let dcfg = if with_mesh { None } else { Some(crate::sfudata::DataCfg { identity: id.clone(), name: name.clone(), room_hash: secure_room_id(&ns, &room), owner_fp: v["owner_fp"].as_str().map(String::from), joined_at: std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0) }) };
                                    match try_sfu(sc, &accounts, &ns, &room, au.clone().zip(au_rx), dcfg).await {
                                        Ok(h) if !with_mesh => {
                                            // SFU puro: sem malha nem Realtime. O peer_id local é a identidade no SFU.
                                            let (_, reason) = crate::sfu_cfg::decide(net_mode, true, false, false);
                                            out(&mut wr, json!({ "ev": "net.mode", "mode": "sfu", "reason": reason, "server": h.server, "configured": true })).await?;
                                            cur_room = format!("{ns}:{room}");
                                            sfu_room = Some((format!("{ns}:{}", &secure_room_id(&ns, &room)[..32]), crate::sfu_cfg::kick_url(&net_env, &cfg.data_dir, &sc.token_url)));
                                            fp_ident.clear(); owner_me = false;
                                            local_peer = h.identity.clone();
                                            audio = au.clone();
                                            mesh_cmd = Some(h.cmd);
                                            mesh_ev = Some(h.events);
                                            rt_status = None;
                                            out(&mut wr, json!({ "ev": "room.joining", "peer_id": local_peer })).await?;
                                            // no SFU a sinalização é a própria sessão com o servidor: já está de pé (quedas viram "reconectando")
                                            out(&mut wr, json!({ "ev": "signaling.status", "state": "conectado", "message": "" })).await?;
                                            if !profile.0.is_empty() || !profile.1.is_empty() { if let Some(c) = &mesh_cmd { let _ = c.send(MeshCmd::SetProfile { status: profile.0.clone(), presence: profile.1.clone() }).await; } }
                                            continue;
                                        }
                                        Ok(h) => { hybrid = Some(h); sfu_au = au; }
                                        Err(e) if !crate::sfu_cfg::fallback_p2p(&net_env, &cfg.data_dir) => {
                                            // sem reserva: erro visível em vez de uma sala P2P à parte
                                            mesh_ev = None;
                                            rt_status = None;
                                            let (_, reason) = crate::sfu_cfg::decide(net_mode, true, true, false);
                                            out(&mut wr, json!({ "ev": "net.mode", "mode": "none", "reason": reason, "error": e, "configured": true, "fallback": false })).await?;
                                            out(&mut wr, json!({ "ev": "error", "where": "room.join", "message": format!("Não foi possível entrar pelo servidor de chamadas: {e}") })).await?;
                                            continue;
                                        }
                                        Err(e) => { sfu_failed = true; sfu_err = e; }
                                    }
                                }
                            }
                        }
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
                        if let Some(h) = &hybrid {
                            let (_, reason) = crate::sfu_cfg::decide(net_mode, true, false, false);
                            out(&mut wr, json!({ "ev": "net.mode", "mode": "sfu", "reason": reason, "server": h.server, "configured": true })).await?;
                        } else if sfu_ns {
                            let (_, reason) = crate::sfu_cfg::decide(net_mode, sfu_ready, sfu_failed, true);
                            out(&mut wr, json!({ "ev": "net.mode", "mode": "p2p", "reason": reason, "error": sfu_err, "configured": sfu_ready, "fallback": sfu_failed })).await?;
                        }
                        cur_room = format!("{ns}:{room}");
                        sfu_room = None; fp_ident.clear(); owner_me = false;
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
                        let (au, au_rx) = if hybrid.is_some() { (sfu_au.clone(), None) } else if audio_on { let (a, r) = Audio::new(Audio::fake_from_env()); (Some(a), Some(r)) } else { (None, None) };
                        if let Some(a) = &au { a.set_mic_muted(mic_muted); a.set_deaf(deafened); a.apply_cfg(&audio_cfg); }
                        audio = au.clone();
                        let (ice_list, ice_warn) = crate::ice::load_with(&|k| std::env::var(k).ok(), &cfg.data_dir, &turn_rt.turn.servers());
                        out(&mut wr, json!({ "ev": "ice.info", "servers": ice_list.len(), "turn": crate::ice::has_turn(&ice_list), "warn": ice_warn.unwrap_or_default() })).await?;
                        let au_h = au.clone();
                        let h = mesh::spawn(MeshCfg { namespace: ns, room, local_peer: local_peer.clone(), display_name: name.clone(), ice_servers: ice_list.clone(), audio: au.zip(au_rx), owner_fp: v["owner_fp"].as_str().map(String::from), joined_at }, id, rt, fw_rx);
                        let h = match hybrid.take() { Some(sfu) => crate::hybrid::spawn(h, sfu, au_h).await, None => h };
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
                        sfu_room = None; fp_ident.clear(); owner_me = false;
                        out(&mut wr, json!({ "ev": "signaling.status", "state": "desconectado" })).await?;
                    }
                    "chat.send" => {
                        let (Some(to), Some(text)) = (v["to_fp"].as_str(), v["text"].as_str()) else { continue };
                        if let Some(c) = &mesh_cmd {
                            let text: String = text.chars().take(crate::identity::MAX_CHAT_CHARS).collect();
                            let id = v["id"].as_str().filter(|i| !i.is_empty() && i.len() <= 64 && i.chars().all(|ch| ch.is_ascii_alphanumeric() || ch == '-' || ch == '_'));
                            let _ = c.send(match id { Some(id) => MeshCmd::ChatWithId { to_fp: to.into(), text, id: id.into() }, None => MeshCmd::Chat { to_fp: to.into(), text } }).await;
                        }
                    }
                    "file.send" => {
                        let Some(path) = v["path"].as_str() else { continue };
                        if let Some(c) = &mesh_cmd { let _ = c.send(MeshCmd::SendFile { path: path.into(), to_fp: v["to_fp"].as_str().map(|x| x.to_string()) }).await; }
                    }
                    "file.cancel" => {
                        if let (Some(id), Some(c)) = (v["id"].as_str(), &mesh_cmd) { let _ = c.send(MeshCmd::CancelFile { id: id.into() }).await; }
                    }
                    "file.accept" => { if let (Some(id), Some(c)) = (v["id"].as_str(), &mesh_cmd) { let _ = c.send(MeshCmd::AcceptFile { id: id.into() }).await; } }
                    "file.pause" => { if let (Some(id), Some(c)) = (v["id"].as_str(), &mesh_cmd) { let _ = c.send(MeshCmd::PauseFile { id: id.into(), paused: v["paused"].as_bool().unwrap_or(true) }).await; } }
                    "chat.disconnect" => { if let (Some(fp), Some(c)) = (v["fp"].as_str(), &mesh_cmd) { let _ = c.send(MeshCmd::DmDisconnect { fp: fp.into() }).await; } }
                    "typing.send" => { if let Some(c) = &mesh_cmd { let _ = c.send(MeshCmd::Typing).await; } }
                    // chat da sala (aba Chat da chamada) e pedido de transmissão: só para quem está autenticado na sala
                    "room.chat.send" => {
                        if let (Some(id), Some(text), Some(c)) = (v["id"].as_str(), v["text"].as_str(), &mesh_cmd) { let _ = c.send(MeshCmd::RoomChat { id: id.into(), text: text.into() }).await; }
                    }
                    "stream.request" => { if let (Some(k), Some(c)) = (v["peer"].as_str(), &mesh_cmd) { let _ = c.send(MeshCmd::StreamRequest { key: k.into() }).await; } }
                    "stream.reply" => { if let (Some(k), Some(c)) = (v["peer"].as_str(), &mesh_cmd) { let _ = c.send(MeshCmd::StreamReply { key: k.into(), accepted: v["accepted"].as_bool().unwrap_or(false) }).await; } }
                    "link.preview" => {
                        // busca segura em thread própria (bloqueante); a resposta volta como evento `link.preview`
                        let (Some(url), Some(rid)) = (v["url"].as_str(), v["id"].as_str()) else { continue };
                        let (url, rid, tx) = (url.to_string(), rid.to_string(), link_tx.clone());
                        tokio::task::spawn_blocking(move || {
                            let r = crate::linkpreview::fetch(&url, crate::linkpreview::STRICT);
                            let ev = match r { Ok(mut j) => { j["ev"] = "link.preview".into(); j["id"] = rid.into(); j["ok"] = true.into(); j }, Err(e) => json!({ "ev": "link.preview", "id": rid, "url": url, "ok": false, "error": e.to_string() }) };
                            let _ = tx.blocking_send(ev);
                        });
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
                    "group.image" | "group.image_reset" | "group.image_get" => {
                        // imagem do grupo: só neste computador (cache em avatar-cache/grp-<id>); mesmo recorte quadrado/256px da foto de perfil
                        let gid: String = v["group_id"].as_str().unwrap_or("").chars().filter(|c| c.is_ascii_alphanumeric() || *c == '-' || *c == '_').take(64).collect();
                        let key = format!("grp-{gid}");
                        let ev = if gid.is_empty() { json!({ "ev": "group.image", "group_id": gid, "ok": false, "message": "grupo inválido" }) } else {
                            match v["cmd"].as_str().unwrap_or("") {
                                "group.image_reset" => { crate::accounts::avatar::clear(&cfg.data_dir, &key); json!({ "ev": "group.image", "group_id": gid, "ok": true, "path": Value::Null }) }
                                "group.image_get" => json!({ "ev": "group.image", "group_id": gid, "ok": true, "path": crate::accounts::avatar::cached(&cfg.data_dir, &key) }),
                                _ => {
                                    let path = PathBuf::from(v["path"].as_str().unwrap_or(""));
                                    let dir = cfg.data_dir.clone();
                                    let r = tokio::task::spawn_blocking(move || crate::accounts::avatar::prepare(&path).and_then(|j| crate::accounts::avatar::store(&dir, &key, &j))).await;
                                    match r {
                                        Ok(Ok(p)) => json!({ "ev": "group.image", "group_id": gid, "ok": true, "path": p }),
                                        Ok(Err(e)) => json!({ "ev": "group.image", "group_id": gid, "ok": false, "message": e.message }),
                                        Err(_) => json!({ "ev": "group.image", "group_id": gid, "ok": false, "message": "Não foi possível preparar a imagem." }),
                                    }
                                }
                            }
                        };
                        out(&mut wr, ev).await?;
                    }
                    "friend.invite" | "friend.accept" | "friend.list" => {
                        // amigo P2P por código assinado (friend_invite.rs): criar o convite, aceitar um código, listar quem já foi aceito
                        let op = v["cmd"].as_str().unwrap_or("").trim_start_matches("friend.").to_string();
                        let me = match &identity { Some(i) => Some(i.clone()), None => load_or_create_identity(&cfg.data_dir).ok().map(Arc::new) };
                        let Some(me) = me else { out(&mut wr, json!({ "ev": "friend.result", "op": op, "ok": false, "message": "identidade indisponível" })).await?; continue };
                        identity = Some(me.clone());
                        let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0);
                        let ev = match op.as_str() {
                            "invite" => {
                                let nonce = format!("{:016x}{:016x}", now, rand_core::RngCore::next_u64(&mut rand_core::OsRng));
                                match crate::friend_invite::create(&me, v["name"].as_str().unwrap_or(""), now, &nonce) {
                                    Ok(i) => json!({ "ev": "friend.result", "op": "invite", "ok": true, "code": crate::friend_invite::to_code(&i), "exp": i.exp }),
                                    Err(e) => json!({ "ev": "friend.result", "op": "invite", "ok": false, "message": e.to_string() }),
                                }
                            }
                            "accept" => {
                                let r = crate::friend_invite::from_code(v["code"].as_str().unwrap_or("")).and_then(|i| crate::friend_invite::check(&i, &me.fingerprint(), now))
                                    .and_then(|f| crate::friend_invite::save(&cfg.data_dir, &f).map(|existed| (f, existed)));
                                match r {
                                    Ok((f, existed)) => json!({ "ev": "friend.result", "op": "accept", "ok": true, "fp": f.fp, "name": f.name, "existed": existed }),
                                    Err(e) => json!({ "ev": "friend.result", "op": "accept", "ok": false, "message": e.to_string() }),
                                }
                            }
                            _ => json!({ "ev": "friend.result", "op": "list", "ok": true, "friends": crate::friend_invite::load(&cfg.data_dir) }),
                        };
                        out(&mut wr, ev).await?;
                    }
                    "group.code" => {
                        let ev = match serde_json::from_value::<crate::group_sync::Snapshot>(v["snapshot"].clone()) {
                            Ok(s) if crate::group_sync::check_structure(&s).is_ok() => json!({ "ev": "group.result", "op": "code", "ok": true, "group_id": s.group_id, "code": crate::group_sync::to_code(&s) }),
                            _ => json!({ "ev": "group.result", "op": "code", "ok": false, "message": "grupo inválido" }),
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
                        // No SFU, além do aviso assinado (que os apps honestos obedecem), o dono pede ao servidor para derrubar a conexão:
                        // função `livekit-kick` com a sessão da conta. Se falhar (função não publicada, sem rede), fica só o aviso.
                        if let (true, Some((lk_room, Some(url))), Some(ident), Some(acc)) = (owner_me, sfu_room.clone(), fp_ident.get(&fp).cloned(), accounts.acc.clone()) {
                            let tx = kick_tx.clone();
                            tokio::spawn(async move {
                                let r = match acc.access_token().await {
                                    Ok(access) => tokio::task::spawn_blocking(move || crate::sfu::kick_remote(&url, &access, &lk_room, &ident)).await.unwrap_or_else(|e| Err(e.to_string())),
                                    Err(e) => Err(format!("sessão: {e}")),
                                };
                                let _ = tx.send(json!({ "ev": "net.kick", "ok": r.is_ok(), "message": r.err().unwrap_or_default() })).await;
                            });
                        }
                        if let (false, Some(c)) = (fp.is_empty(), &mesh_cmd) { let _ = c.send(MeshCmd::Kick { fp, reason: v["reason"].as_str().unwrap_or("").to_string() }).await; }
                    }
                    "room.allow" => {
                        let fp = v["fp"].as_str().unwrap_or("").to_string();
                        if let (false, Some(c)) = (fp.is_empty(), &mesh_cmd) { let _ = c.send(MeshCmd::Allow { fp }).await; }
                    }
                    "room.mute" => {
                        // o dono fecha (ou reabre) o microfone de outra pessoa para TODOS: pedido assinado pela camada de dados do SFU
                        let fp = v["fp"].as_str().unwrap_or("").to_string();
                        if let (false, Some(c)) = (fp.is_empty(), &mesh_cmd) { let _ = c.send(MeshCmd::RemoteMute { fp, muted: v["muted"].as_bool().unwrap_or(true) }).await; }
                    }
                    "peer.mute" => {
                        let key = v["peer"].as_str().or(v["fp"].as_str()).unwrap_or("").to_string();
                        if let (false, Some(c)) = (key.is_empty(), &mesh_cmd) { let _ = c.send(MeshCmd::MutePeer { key, muted: v["muted"].as_bool().unwrap_or(true) }).await; }
                    }
                    "audio.cfg" => {
                        // Configurações > Voz: ganhos (1.0 = normal), porta de ruído (gate_db ausente = desligada), supressão (0 padrão, 1 desligada) e dispositivos (nome; vazio = padrão)
                        audio_cfg = crate::audio::AudioCfg { in_gain: v["in_gain"].as_f64().unwrap_or(1.0) as f32, out_gain: v["out_gain"].as_f64().unwrap_or(1.0) as f32, gate_db: v["gate_db"].as_f64().map(|d| d as f32),
                            noise: v["noise"].as_u64().unwrap_or(0).min(1) as u8, mic: v["mic"].as_str().unwrap_or("").chars().take(120).collect(), out: v["out"].as_str().unwrap_or("").chars().take(120).collect() };
                        if let Some(c) = &mesh_cmd { let _ = c.send(MeshCmd::AudioCfg(audio_cfg.clone())).await; }
                    }
                    "mic.test" => {
                        // Teste do microfone fora da chamada: on=true começa (eventos `mic.test` com o nível), on=false para.
                        if let Some(t) = mic_test.take() { t.stop(); }
                        if v["on"].as_bool().unwrap_or(false) { mic_test = Some(crate::audio::MicTest::start(audio_cfg.clone(), Audio::fake_from_env(), link_tx.clone())); }
                    }
                    "peer.volume" => {
                        let key = v["peer"].as_str().or(v["fp"].as_str()).unwrap_or("").to_string();
                        if let (false, Some(c)) = (key.is_empty(), &mesh_cmd) { let _ = c.send(MeshCmd::PeerVolume { key, gain: v["volume"].as_f64().unwrap_or(1.0) as f32 }).await; }
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
                    "net.mode.set" | "net.status" => {
                        if let Some(m) = v["mode"].as_str().and_then(crate::sfu_cfg::NetMode::parse) { let _ = crate::sfu_cfg::save_mode(&cfg.data_dir, m); }
                        let (mode, conf) = (crate::sfu_cfg::load_mode(&net_env, &cfg.data_dir), crate::sfu_cfg::resolve(&net_env, &cfg.data_dir));
                        out(&mut wr, json!({ "ev": "net.config", "mode": mode.name(), "configured": matches!(conf, Ok(Some(_))), "error": conf.err().unwrap_or_default() })).await?;
                    }
                    // tela Rede: servidor TURN/STUN próprio em ice.json. A senha entra aqui e nunca volta para a UI.
                    "ice.get" => out(&mut wr, crate::ice::summary(&|k| std::env::var(k).ok(), &cfg.data_dir, &turn_rt.turn.servers())).await?,
                    "ice.set" | "ice.clear" => {
                        let s = |k: &str| v[k].as_str().unwrap_or("");
                        let set = v["cmd"] == "ice.set";
                        let r = if set { crate::ice::save(&cfg.data_dir, s("urls"), s("username"), s("credential")) } else { crate::ice::clear(&cfg.data_dir) };
                        let mut ev = crate::ice::summary(&|k| std::env::var(k).ok(), &cfg.data_dir, &turn_rt.turn.servers());
                        ev["ok"] = r.is_ok().into();
                        ev["message"] = match r { Ok(()) if set => "Salvo. Vale a partir da próxima chamada.".into(), Ok(()) => "Removido. O Briks volta a usar só o STUN público.".into(), Err(e) => e.into() };
                        out(&mut wr, ev).await?;
                    }
                    "ice.test" => {
                        let (list, _) = crate::ice::load_with(&|k| std::env::var(k).ok(), &cfg.data_dir, &turn_rt.turn.servers());
                        let tx = ev_tx.clone();
                        tokio::spawn(async move { let _ = tx.send(crate::ice::probe(list, Duration::from_secs(8)).await).await; });
                    }
                    "ping" => out(&mut wr, json!({ "ev": "pong" })).await?,
                    other => { account.command(other, &v); }
                }
            }
            Some(ev) = acct_rx.recv() => out(&mut wr, ev).await?,
            Some(ev) = kick_rx.recv() => out(&mut wr, ev).await?,   // resultado do pedido ao servidor (diagnóstico; a UI não precisa reagir)
            Some(j) = link_rx.recv() => out(&mut wr, j).await?,
            e = async { match mesh_ev.as_mut() { Some(r) => r.recv().await, None => std::future::pending().await } } => {
                match e {
                    Some(MeshEvent::Peer { peer_id, state }) => out(&mut wr, json!({ "ev": "peer", "peer_id": peer_id, "state": state })).await?,
                    Some(MeshEvent::File(j)) => { let mut j = j; j["ev"] = "file".into(); out(&mut wr, j).await? }
                    Some(MeshEvent::Ack { id, from_fp }) => out(&mut wr, json!({ "ev": "chat.ack", "id": id, "from_fp": from_fp })).await?,
                    Some(MeshEvent::DmLink { fp, on }) => out(&mut wr, json!({ "ev": "chat.link", "fp": fp, "on": on })).await?,
                    Some(MeshEvent::Typing { from_fp }) => out(&mut wr, json!({ "ev": "chat.typing", "from_fp": from_fp })).await?,
                    Some(MeshEvent::RoomChat { peer_id, from_fp, author, content, id, ts }) => out(&mut wr, json!({ "ev": "room.chat", "peer_id": peer_id, "from_fp": from_fp, "author": author, "content": content, "id": id, "ts": ts })).await?,
                    Some(MeshEvent::StreamRequest { peer_id, name }) => out(&mut wr, json!({ "ev": "stream.request", "peer_id": peer_id, "name": name })).await?,
                    Some(MeshEvent::StreamReply { peer_id, name, accepted }) => out(&mut wr, json!({ "ev": "stream.reply", "peer_id": peer_id, "name": name, "accepted": accepted })).await?,
                    Some(MeshEvent::ChatEvent { from_fp, wire, local }) => out(&mut wr, json!({ "ev": "chat.event", "from_fp": from_fp, "local": local, "wire_raw": wire.to_string(), "wire": wire })).await?,
                    Some(MeshEvent::GroupSnapshot { from_fp, snapshot }) => out(&mut wr, json!({ "ev": "group.snapshot", "from_fp": from_fp, "snapshot": snapshot })).await?,
                    Some(MeshEvent::History { channel, from_fp, author, content, id, ts, mine }) => out(&mut wr, json!({ "ev": "chat.history", "channel": channel, "from_fp": from_fp, "author": author, "content": content, "id": id, "ts": ts, "mine": mine })).await?,
                    Some(MeshEvent::Chat { channel, from_fp, author, content, id, ts }) => out(&mut wr, json!({ "ev": "chat.message", "channel": channel, "from_fp": from_fp, "author": author, "content": content, "id": id, "ts": ts })).await?,
                    Some(MeshEvent::PeerInfo { peer_id, fp, name }) => { fp_ident.insert(fp.clone(), peer_id.clone()); out(&mut wr, json!({ "ev": "peer.info", "peer_id": peer_id, "fp": fp, "name": name })).await? }
                    Some(MeshEvent::Owner { fp, me }) => { owner_me = me; out(&mut wr, json!({ "ev": "room.owner", "fp": fp, "me": me })).await? }
                    Some(MeshEvent::Banned { fp }) => out(&mut wr, json!({ "ev": "peer.banned", "fp": fp })).await?,
                    Some(MeshEvent::Kicked { reason }) => {
                        kicked_rooms.insert(cur_room.clone());
                        out(&mut wr, json!({ "ev": "kicked", "reason": reason })).await?;
                    }
                    Some(MeshEvent::Speaking { peer_id, level }) => out(&mut wr, json!({ "ev": "speaking", "peer": peer_id, "level": level })).await?,
                    Some(MeshEvent::PeerState { peer_id, microphone, deafened, status, presence }) => out(&mut wr, json!({ "ev": "peer.state", "peer_id": peer_id, "microphone": microphone, "deafened": deafened, "status": status, "presence": presence })).await?,
                    Some(MeshEvent::Share(v)) => {
                        // mudo pedido pelo dono: o core já fechou/abriu o microfone; o estado local e a UI acompanham
                        let muted_by_owner = if v["ev"] == "room.muted" { v["muted"].as_bool() } else { None };
                        out(&mut wr, v).await?;
                        if let Some(m) = muted_by_owner {
                            mic_muted = m;
                            out(&mut wr, json!({ "ev": "mic.state", "microphone": !mic_muted && !deafened, "muted": mic_muted, "deafened": deafened })).await?;
                        }
                    }
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
