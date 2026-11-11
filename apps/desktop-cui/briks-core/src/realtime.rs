//! Cliente do Supabase Realtime (Phoenix sobre WebSocket): join, Presence, Broadcast, heartbeat e reconexão.
//! `ws://` (loopback, testes) e `wss://` (Supabase) pelo mesmo caminho. Só recebe URL/chave da configuração.
use std::sync::Arc;
use std::time::Duration;

use anyhow::{anyhow, Result};
use futures_util::{SinkExt, StreamExt};
use serde_json::{json, Value};
use tokio::io::{AsyncRead, AsyncWrite};
use tokio::net::TcpStream;
use tokio::sync::mpsc;
use tokio_tungstenite::tungstenite::Message;

use crate::phoenix::{self, Frame, PresenceTable};

pub trait Io: AsyncRead + AsyncWrite + Unpin + Send {}
impl<T: AsyncRead + AsyncWrite + Unpin + Send> Io for T {}

#[derive(Clone, Debug)]
pub struct RealtimeCfg {
    /// `https://<ref>.supabase.co`, `wss://...` ou `ws://127.0.0.1:porta` (testes).
    pub url: String,
    pub anon_key: String,
    pub heartbeat: Duration,
    pub leave_grace: Duration,
}

impl RealtimeCfg {
    pub fn new(url: &str, anon_key: &str) -> Self {
        RealtimeCfg { url: url.into(), anon_key: anon_key.into(), heartbeat: Duration::from_secs(30), leave_grace: Duration::from_secs(10) }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub enum RtEvent {
    /// "connecting" | "connected" | "reconnecting" | "disconnected" | "error: <msg>"
    Status(String),
    PeerJoined { peer_id: String, meta: Value },
    PeerLeft(String),
    Broadcast { event: String, payload: Value },
}

enum Out {
    Broadcast(String, Value),
    Track(Value),
    Close,
}

pub struct Realtime {
    out: mpsc::Sender<Out>,
}

impl Realtime {
    pub async fn send_broadcast(&self, event: &str, payload: Value) -> Result<()> {
        self.out.send(Out::Broadcast(event.into(), payload)).await.map_err(|_| anyhow!("canal Realtime fechado"))
    }
    pub async fn track(&self, meta: Value) -> Result<()> {
        self.out.send(Out::Track(meta)).await.map_err(|_| anyhow!("canal Realtime fechado"))
    }
    pub async fn close(&self) {
        let _ = self.out.send(Out::Close).await;
    }
}

pub(crate) struct Target { pub tls: bool, pub host: String, pub port: u16 }

pub(crate) fn parse_target(url: &str) -> Result<Target> {
    let (scheme, rest) = url.split_once("://").ok_or_else(|| anyhow!("URL inválida"))?;
    let tls = match scheme { "https" | "wss" => true, "http" | "ws" => false, _ => return Err(anyhow!("esquema não suportado: {scheme}")) };
    let hostport = rest.split('/').next().unwrap_or("");
    let (host, port) = match hostport.rsplit_once(':') {
        Some((h, p)) => (h.to_string(), p.parse::<u16>().map_err(|_| anyhow!("porta inválida"))?),
        None => (hostport.to_string(), if tls { 443 } else { 80 }),
    };
    if host.is_empty() { return Err(anyhow!("host vazio")); }
    Ok(Target { tls, host, port })
}

pub(crate) async fn open_io(t: &Target) -> Result<Box<dyn Io>> {
    if let Some(m) = crate::netmode::deny_url(&format!("{}://{}", if t.tls { "https" } else { "http" }, t.host)) { return Err(anyhow!(m)); }
    let tcp = TcpStream::connect((t.host.as_str(), t.port)).await?;
    tcp.set_nodelay(true).ok();
    if !t.tls {
        return Ok(Box::new(tcp));
    }
    let mut roots = rustls::RootCertStore::empty();
    roots.extend(webpki_roots::TLS_SERVER_ROOTS.iter().cloned());
    let cfg = rustls::ClientConfig::builder_with_provider(Arc::new(rustls::crypto::ring::default_provider()))
        .with_safe_default_protocol_versions()?
        .with_root_certificates(roots)
        .with_no_client_auth();
    let name = rustls::pki_types::ServerName::try_from(t.host.clone()).map_err(|e| anyhow!("nome TLS inválido: {e}"))?;
    let tls = tokio_rustls::TlsConnector::from(Arc::new(cfg)).connect(name, tcp).await?;
    Ok(Box::new(tls))
}

/// Abre o canal e mantém a conexão (com reconexão) até `close()`. Os eventos saem em `rx`.
pub fn connect(cfg: RealtimeCfg, topic: String, peer_id: String, meta: Value) -> (Realtime, mpsc::Receiver<RtEvent>) {
    let (otx, orx) = mpsc::channel::<Out>(256);
    let (etx, erx) = mpsc::channel::<RtEvent>(256);
    tokio::spawn(run(cfg, topic, peer_id, meta, orx, etx));
    (Realtime { out: otx }, erx)
}

async fn run(cfg: RealtimeCfg, topic: String, peer_id: String, meta: Value, mut orx: mpsc::Receiver<Out>, etx: mpsc::Sender<RtEvent>) {
    let mut attempt = 0u32;
    let mut meta = meta;
    // A tabela de Presence sobrevive às reconexões: quem saiu enquanto o canal estava caído aparece como ausente no primeiro
    // `presence_state` depois de voltar. Com uma tabela nova a cada sessão esses peers nunca eram dados como saídos (fantasmas
    // ocupando as 5 vagas da malha).
    let mut presence = PresenceTable::default();
    let mut pending_leave: Vec<(String, tokio::time::Instant)> = vec![];   // saídas ainda dentro do prazo de graça também sobrevivem à queda
    loop {
        let _ = etx.send(RtEvent::Status(if attempt == 0 { "connecting".into() } else { "reconnecting".into() })).await;
        match session(&cfg, &topic, &peer_id, &mut meta, &mut presence, &mut pending_leave, &mut orx, &etx).await {
            Ok(()) => {
                let _ = etx.send(RtEvent::Status("disconnected".into())).await;
                return;
            }
            Err(e) => {
                let first = attempt == 0;
                attempt += 1;
                if first && !is_transient(&e) {
                    let _ = etx.send(RtEvent::Status(format!("error: {e}"))).await;
                    return;
                }
                let _ = etx.send(RtEvent::Status("reconnecting".into())).await;
                let wait = Duration::from_millis((500u64 << attempt.min(5)).min(15_000));
                tokio::select! {
                    _ = tokio::time::sleep(wait) => {}
                    o = orx.recv() => { if matches!(o, Some(Out::Close) | None) { let _ = etx.send(RtEvent::Status("disconnected".into())).await; return; } }
                }
            }
        }
    }
}

fn is_transient(e: &anyhow::Error) -> bool {
    let s = e.to_string();
    !(s.contains("URL") || s.contains("esquema") || s.contains("host vazio") || s.contains("porta inválida"))
}

async fn session(cfg: &RealtimeCfg, topic: &str, peer_id: &str, meta: &mut Value, presence: &mut PresenceTable, pending_leave: &mut Vec<(String, tokio::time::Instant)>, orx: &mut mpsc::Receiver<Out>, etx: &mpsc::Sender<RtEvent>) -> Result<()> {
    let target = parse_target(&cfg.url)?;
    let io = tokio::time::timeout(Duration::from_secs(15), open_io(&target)).await.map_err(|_| anyhow!("tempo esgotado ao conectar ao Realtime"))??;
    let ws_url = format!("{}://{}:{}/realtime/v1/websocket?apikey={}&vsn=1.0.0", if target.tls { "wss" } else { "ws" }, target.host, target.port, cfg.anon_key);
    let (ws, _) = tokio_tungstenite::client_async(ws_url, io).await.map_err(|e| anyhow!("WebSocket recusado: {e}"))?;
    let (mut sink, mut stream) = ws.split();

    let mut next_ref: u64 = 1;
    let join_ref = "1".to_string();
    let join = Frame { topic: topic.into(), event: "phx_join".into(), payload: phoenix::join_payload(peer_id, &cfg.anon_key), msg_ref: Some(next_ref.to_string()), join_ref: Some(join_ref.clone()) };
    next_ref += 1;
    sink.send(Message::Text(phoenix::encode(&join))).await?;

    let mut joined = false;
    let mut hb = tokio::time::interval(cfg.heartbeat);
    hb.tick().await;
    let join_deadline = tokio::time::Instant::now() + Duration::from_secs(15);
    let mut tick = tokio::time::interval(Duration::from_millis(250));

    loop {
        tokio::select! {
            msg = stream.next() => {
                let Some(msg) = msg else { return Err(anyhow!("conexão fechada pelo servidor")); };
                let msg = msg?;
                let text = match msg { Message::Text(t) => t, Message::Close(_) => return Err(anyhow!("conexão fechada pelo servidor")), Message::Ping(p) => { sink.send(Message::Pong(p)).await?; continue; } _ => continue };
                let Some(f) = phoenix::decode(&text) else { continue };
                if f.topic != topic && f.topic != "phoenix" { continue; }
                match f.event.as_str() {
                    "phx_reply" if f.topic == topic && f.msg_ref.as_deref() == Some("1") => {
                        let status = f.payload.get("status").and_then(|s| s.as_str()).unwrap_or("");
                        if status != "ok" { return Err(anyhow!("join recusado: {}", f.payload)); }
                        joined = true;
                        let tr = phoenix::presence_track(topic, meta.clone(), next_ref, &join_ref); next_ref += 1;
                        sink.send(Message::Text(phoenix::encode(&tr))).await?;
                        let _ = etx.send(RtEvent::Status("connected".into())).await;
                    }
                    "phx_error" | "phx_close" if f.topic == topic => return Err(anyhow!("canal encerrado: {}", f.event)),
                    "presence_state" | "presence_diff" => {
                        if let Some(ch) = presence.apply(&f) {
                            for p in ch.joined {
                                pending_leave.retain(|(k, _)| k != &p);
                                if p != peer_id { let m = presence.peers.get(&p).cloned().unwrap_or(Value::Null); let _ = etx.send(RtEvent::PeerJoined { peer_id: p, meta: m }).await; }
                            }
                            for p in ch.left { if p != peer_id { pending_leave.push((p, tokio::time::Instant::now() + cfg.leave_grace)); } }
                        }
                    }
                    "broadcast" => { if let Some((event, payload)) = phoenix::as_broadcast(&f) { let _ = etx.send(RtEvent::Broadcast { event, payload }).await; } }
                    _ => {}
                }
            }
            o = orx.recv() => {
                match o {
                    None | Some(Out::Close) => {
                        let un = phoenix::presence_untrack(topic, next_ref, &join_ref); next_ref += 1;
                        let _ = sink.send(Message::Text(phoenix::encode(&un))).await;
                        let leave = Frame { topic: topic.into(), event: "phx_leave".into(), payload: json!({}), msg_ref: Some(next_ref.to_string()), join_ref: Some(join_ref.clone()) };
                        let _ = sink.send(Message::Text(phoenix::encode(&leave))).await;
                        let _ = sink.close().await;
                        return Ok(());
                    }
                    Some(Out::Broadcast(ev, payload)) => {
                        if joined { let b = phoenix::broadcast(topic, &ev, payload, next_ref, &join_ref); next_ref += 1; sink.send(Message::Text(phoenix::encode(&b))).await?; }
                    }
                    Some(Out::Track(m)) => {
                        *meta = m.clone();
                        if joined { let t = phoenix::presence_track(topic, m, next_ref, &join_ref); next_ref += 1; sink.send(Message::Text(phoenix::encode(&t))).await?; }
                    }
                }
            }
            _ = hb.tick() => { let h = phoenix::heartbeat(next_ref); next_ref += 1; sink.send(Message::Text(phoenix::encode(&h))).await?; }
            _ = tick.tick() => {
                if !joined && tokio::time::Instant::now() > join_deadline { return Err(anyhow!("tempo esgotado no join do canal")); }
                let now = tokio::time::Instant::now();
                let due: Vec<String> = pending_leave.iter().filter(|(_, t)| *t <= now).map(|(k, _)| k.clone()).collect();
                pending_leave.retain(|(_, t)| *t > now);
                for p in due { if !presence.peers.contains_key(&p) { let _ = etx.send(RtEvent::PeerLeft(p)).await; } }
            }
        }
    }
}
