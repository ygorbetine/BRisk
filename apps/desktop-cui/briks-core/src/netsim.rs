//! Só testes: rede ruim simulada entre dois nós na mesma máquina. Um proxy UDP fica no meio de TODO o tráfego WebRTC (os candidatos
//! ICE de cada nó são trocados por portas do proxy e os `a=candidate` saem do SDP), limitando a banda (balde furado com fila de
//! 250 ms e descarte na cauda, como um roteador), injetando perda aleatória e atraso fixo. Os limites mudam ao vivo.
use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use tokio::net::UdpSocket;

pub struct NetSim {
    pub peers: Vec<String>,
    /// bits por segundo em cada sentido (0 = sem limite)
    pub limit_bps: AtomicU64,
    /// perda aleatória em partes por milhão
    pub loss_ppm: AtomicU32,
    pub delay_ms: AtomicU64,
    maps: tokio::sync::Mutex<HashMap<SocketAddr, u16>>,
    rng: AtomicU64,
    pub forwarded: AtomicU64,
    pub dropped: AtomicU64,
}

static ACTIVE: Mutex<Vec<Arc<NetSim>>> = Mutex::new(Vec::new());

pub fn register(peers: &[&str]) -> Arc<NetSim> {
    let n = Arc::new(NetSim { peers: peers.iter().map(|s| s.to_string()).collect(), limit_bps: AtomicU64::new(0), loss_ppm: AtomicU32::new(0), delay_ms: AtomicU64::new(0),
        maps: Default::default(), rng: AtomicU64::new(0x9E3779B97F4A7C15), forwarded: AtomicU64::new(0), dropped: AtomicU64::new(0) });
    ACTIVE.lock().unwrap().push(n.clone());
    n
}
pub fn for_peer(id: &str) -> Option<Arc<NetSim>> { ACTIVE.lock().unwrap().iter().find(|n| n.peers.iter().any(|p| p == id)).cloned() }

/// Tira os candidatos do SDP (senão o outro lado aprenderia o endereço direto e contornaria o proxy).
pub fn strip_sdp(sdp: &str) -> String {
    sdp.split("\r\n").filter(|l| !l.starts_with("a=candidate") && !l.starts_with("a=end-of-candidates")).collect::<Vec<_>>().join("\r\n")
}

struct Shaper { next_free: Instant }

/// Tomada UDP com buffers grandes (os 64 kB padrão do Windows transbordam com rajadas de vídeo e criariam perda que não é da simulação).
fn big_socket() -> Option<UdpSocket> {
    let s = socket2::Socket::new(socket2::Domain::IPV4, socket2::Type::DGRAM, Some(socket2::Protocol::UDP)).ok()?;
    let _ = s.set_recv_buffer_size(4 << 20);
    let _ = s.set_send_buffer_size(4 << 20);
    s.bind(&"0.0.0.0:0".parse::<SocketAddr>().ok()?.into()).ok()?;
    s.set_nonblocking(true).ok()?;
    UdpSocket::from_std(s.into()).ok()
}

impl NetSim {
    pub fn set(&self, mbps: f64, loss_pct: f64) {
        self.limit_bps.store((mbps * 1e6) as u64, Ordering::Relaxed);
        self.loss_ppm.store((loss_pct * 10_000.0) as u32, Ordering::Relaxed);
    }
    fn rand(&self) -> u32 {
        let mut x = self.rng.load(Ordering::Relaxed);
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        self.rng.store(x, Ordering::Relaxed);
        (x >> 32) as u32
    }

    /// Aplica perda/limite e devolve quando o pacote pode sair (None = descartado).
    fn shape(&self, sh: &Mutex<Shaper>, len: usize) -> Option<Instant> {
        if self.rand() % 1_000_000 < self.loss_ppm.load(Ordering::Relaxed) { self.dropped.fetch_add(1, Ordering::Relaxed); return None; }
        let now = Instant::now();
        let limit = self.limit_bps.load(Ordering::Relaxed);
        let mut at = now;
        if limit > 0 {
            let mut s = sh.lock().unwrap();
            at = s.next_free.max(now);
            if at - now > Duration::from_millis(250) { self.dropped.fetch_add(1, Ordering::Relaxed); return None; }   // fila cheia
            s.next_free = at + Duration::from_secs_f64((len as f64 + 28.0) * 8.0 / limit as f64);
        }
        self.forwarded.fetch_add(len as u64, Ordering::Relaxed);
        Some(at + Duration::from_millis(self.delay_ms.load(Ordering::Relaxed)))
    }

    /// Reescreve um candidato ICE "candidate:… udp prio IP PORTA typ host …" para apontar para o proxy. None = descarte-o.
    pub async fn rewrite(self: &Arc<Self>, cand: &str) -> Option<String> {
        let mut f: Vec<String> = cand.split(' ').map(String::from).collect();
        if f.len() < 8 || !f[2].eq_ignore_ascii_case("udp") || f[7] != "host" { return None; }
        let ip: std::net::Ipv4Addr = f[4].parse().ok()?;
        let port: u16 = f[5].parse().ok()?;
        let target = SocketAddr::from((ip, port));
        let p = self.map(target).await?;
        f[5] = p.to_string();
        Some(f.join(" "))
    }

    /// Porta do proxy que encaminha para `target` (cria na primeira vez).
    async fn map(self: &Arc<Self>, target: SocketAddr) -> Option<u16> {
        let mut maps = self.maps.lock().await;
        if let Some(p) = maps.get(&target) { return Some(*p); }
        let front = Arc::new(big_socket()?);
        let port = front.local_addr().ok()?.port();
        maps.insert(target, port);
        let me = self.clone();
        tokio::spawn(async move {
            // uma tomada de saída por origem: o alvo responde para ela e o proxy devolve pela porta da frente.
            // Cada sentido tem UMA fila com uma tarefa que solta os pacotes em ordem (um roteador não reordena).
            let mut backs: HashMap<SocketAddr, tokio::sync::mpsc::UnboundedSender<(Instant, Vec<u8>)>> = HashMap::new();
            let (up, down) = (Arc::new(Mutex::new(Shaper { next_free: Instant::now() })), Arc::new(Mutex::new(Shaper { next_free: Instant::now() })));
            let mut buf = vec![0u8; 2048];
            while let Ok((n, src)) = front.recv_from(&mut buf).await {
                let q = match backs.get(&src) {
                    Some(q) => q.clone(),
                    None => {
                        let Some(b) = big_socket() else { continue };
                        let b = Arc::new(b);
                        let (qtx, mut qrx) = tokio::sync::mpsc::unbounded_channel::<(Instant, Vec<u8>)>();
                        backs.insert(src, qtx.clone());
                        let b3 = b.clone();
                        tokio::spawn(async move { while let Some((at, pkt)) = qrx.recv().await { tokio::time::sleep_until(at.into()).await; let _ = b3.send_to(&pkt, target).await; } });
                        let (dtx, mut drx) = tokio::sync::mpsc::unbounded_channel::<(Instant, Vec<u8>)>();
                        let front3 = front.clone();
                        tokio::spawn(async move { while let Some((at, pkt)) = drx.recv().await { tokio::time::sleep_until(at.into()).await; let _ = front3.send_to(&pkt, src).await; } });
                        let (b2, me2, down2) = (b.clone(), me.clone(), down.clone());
                        tokio::spawn(async move {
                            let mut buf = vec![0u8; 2048];
                            while let Ok((n, _)) = b2.recv_from(&mut buf).await { if let Some(at) = me2.shape(&down2, n) { let _ = dtx.send((at, buf[..n].to_vec())); } }
                        });
                        qtx
                    }
                };
                if let Some(at) = me.shape(&up, n) { let _ = q.send((at, buf[..n].to_vec())); }
            }
        });
        Some(port)
    }
}
