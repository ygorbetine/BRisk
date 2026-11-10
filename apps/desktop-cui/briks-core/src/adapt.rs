//! Adaptação automática da transmissão (laço fechado): estimativa de banda a partir do RTCP dos espectadores e escolha de
//! bitrate / FPS / resolução conforme a rede, o tipo de conteúdo e a folga da máquina.
//!
//! O webrtc-rs 0.12 não traz controle de congestionamento (nem goog-cc nem REMB): só gera Receiver Reports, NACK e o feedback
//! TWCC no lado receptor. Por isso a estimativa aqui é própria, no estilo AIMD (sobe devagar, cai rápido), com dois sinais:
//!   - perda (fração perdida dos Receiver Reports): < 3% deixa subir, 3–10% segura, > 10% derruba;
//!   - atraso (RTT acima da base recente): fila enchendo no caminho → derruba antes de haver perda.
//! Numa malha com vários espectadores vale o pior deles (há um só codificador).
use std::collections::HashMap;
use std::time::{Duration, Instant};

use crate::video::auto_bitrate;

pub const FLOOR_BPS: f64 = 150_000.0;

/// O que preservar quando falta banda ou CPU.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Content {
    /// Tela, texto, janela: nitidez. Cai o FPS antes da resolução (piso de 10 FPS).
    Detail,
    /// Jogo, vídeo, movimento: fluidez. Cai a resolução antes do FPS (piso de 30 FPS enquanto houver resolução para cortar).
    Motion,
}

/// Medidas de rede de um espectador (atualizadas pela leitura do RTCP do remetente).
#[derive(Debug, Clone)]
pub struct PeerNet {
    pub loss: f32,       // 0..1, do último Receiver Report
    pub rtt_ms: f32,     // 0 = ainda sem medida
    pub jitter_ms: f32,
    pub updated: Instant,
    pub nacks: u64,      // pacotes pedidos de novo (acumulado)
    pub plis: u64,       // pedidos de quadro-chave (acumulado)
}

#[derive(Default)]
pub struct NetShared { pub peers: HashMap<String, PeerNet> }

impl NetShared {
    /// Pior espectador com relatório recente: (perda, rtt_ms, jitter_ms). None = ninguém reportando.
    pub fn worst(&self) -> Option<(f32, f32, f32)> {
        let fresh: Vec<&PeerNet> = self.peers.values().filter(|p| p.updated.elapsed() < Duration::from_millis(3500)).collect();
        if fresh.is_empty() { return None; }
        Some((fresh.iter().map(|p| p.loss).fold(0.0, f32::max), fresh.iter().map(|p| p.rtt_ms).fold(0.0, f32::max), fresh.iter().map(|p| p.jitter_ms).fold(0.0, f32::max)))
    }
    pub fn totals(&self) -> (u64, u64) { (self.peers.values().map(|p| p.nacks).sum(), self.peers.values().map(|p| p.plis).sum()) }
}

/// Estimador de banda disponível (bps). Chamado uma vez por segundo.
pub struct Estimator {
    pub est: f64,
    pub ceil: f64,
    rtt_hist: Vec<f32>,        // últimas medidas (base = mínimo)
    hold_until: Instant,       // sem subir até aqui (depois de uma queda)
    last_drop: Option<Instant>,
    pub reason: &'static str,  // "", "perda", "atraso"
}

impl Estimator {
    pub fn new(ceil: f64) -> Self { Estimator { est: ceil, ceil, rtt_hist: vec![], hold_until: Instant::now(), last_drop: None, reason: "" } }
    pub fn set_ceil(&mut self, ceil: f64) { self.ceil = ceil; if self.est > ceil { self.est = ceil; } }
    pub fn rtt_base(&self) -> f32 { self.rtt_hist.iter().copied().fold(f32::MAX, f32::min) }

    /// `sent` = bps realmente enviados no último segundo; `net` = pior espectador (None = sem relatórios);
    /// `nack_ratio` = pacotes pedidos de novo / pacotes enviados no último segundo.
    pub fn update(&mut self, now: Instant, sent: f64, net: Option<(f32, f32, f32)>, nack_ratio: f32) -> f64 {
        let Some((loss, rtt, _)) = net else { self.reason = ""; return self.est; };
        if rtt > 0.0 { self.rtt_hist.push(rtt); if self.rtt_hist.len() > 30 { self.rtt_hist.remove(0); } }
        let base = if self.rtt_hist.is_empty() { 0.0 } else { self.rtt_base() };
        let delayed = rtt > 0.0 && base > 0.0 && rtt > base + (base * 0.5).max(80.0);
        // os Receiver Reports contam como recebido o que a retransmissão consertou; a fração de pacotes pedidos pela primeira vez
        // é a perda real do caminho, e chega antes do relatório
        let loss_eff = loss.max(nack_ratio.min(1.0));
        if loss_eff > 0.10 || delayed {
            // Cai rápido, mas com freios: (1) os relatórios do segundo seguinte a uma queda ainda refletem o envio antigo, então
            // só cai de novo depois de 2 s (ou com perda acima de 30%); (2) parte do que realmente passou, porém nunca abaixo de
            // 60% da estimativa — o envio do último segundo já vem reduzido pelos nossos próprios descartes.
            let settling = self.last_drop.map(|t| now.duration_since(t) < Duration::from_millis(1900)).unwrap_or(false);
            if !settling || loss_eff > 0.30 {
                let from = if sent > FLOOR_BPS { self.est.min(sent.max(self.est * 0.6)) } else { self.est };
                let factor = if loss_eff > 0.10 { (1.0 - 0.5 * loss_eff as f64).clamp(0.6, 0.9) } else { 0.8 };
                self.est = (from * factor).max(FLOOR_BPS);
                self.last_drop = Some(now);
            }
            self.hold_until = now + Duration::from_secs(3);
            self.reason = if loss_eff > 0.10 { "perda" } else { "atraso" };
        } else if loss_eff < 0.03 && now >= self.hold_until {
            // Sobe só quando a estimativa está sendo usada (senão cresceria sem ser testada): rápido (25%/s) enquanto está abaixo
            // de metade do teto, devagar (8%/s) perto dele.
            if sent >= self.est * 0.7 || self.est < self.ceil * 0.25 {
                let k = if self.est < self.ceil * 0.5 { 1.25 } else { 1.08 };
                self.est = (self.est * k + 40_000.0).min(self.ceil);
            }
            if self.est >= self.ceil * 0.98 { self.reason = ""; }
        }
        self.est
    }
}

/// Estado de envio: altura e FPS.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Level { pub h: usize, pub fps: u32 }

/// Bitrate mínimo para o nível ficar aceitável (metade do "automático" daquela resolução/FPS).
pub fn need_bps(l: Level) -> f64 { auto_bitrate(l.h, l.fps) as f64 * 0.5 }

const HEIGHTS: [usize; 7] = [2160, 1440, 1080, 900, 720, 540, 360];

/// Escada do melhor para o pior, conforme a política do conteúdo. `max_h`/`max_fps` = o que o usuário pediu (teto).
pub fn ladder(content: Content, max_h: usize, max_fps: u32) -> Vec<Level> {
    let mut hs: Vec<usize> = HEIGHTS.iter().copied().filter(|h| *h < max_h).collect();
    hs.insert(0, max_h);
    let low_fps = |floor: u32| -> Vec<u32> { [20u32, 15, 10].into_iter().filter(|f| *f < max_fps && *f >= floor).collect() };
    let mut out = vec![];
    match content {
        Content::Detail => {
            out.push(Level { h: max_h, fps: max_fps });
            if max_fps > 30 { out.push(Level { h: max_h, fps: 30 }); }
            for f in low_fps(10) { out.push(Level { h: max_h, fps: f }); }
            let fmin = out.last().unwrap().fps;
            for h in hs.iter().skip(1) { out.push(Level { h: *h, fps: fmin }); }
        }
        Content::Motion => {
            let f_hi = max_fps;
            for h in &hs { out.push(Level { h: *h, fps: f_hi }); }
            let hmin = *hs.last().unwrap();
            if f_hi > 30 { out.push(Level { h: hmin, fps: 30 }); }
            for f in low_fps(15) { out.push(Level { h: hmin, fps: f }); }
        }
    }
    out
}

/// Controlador: junta a estimativa de rede e a folga local e devolve o nível + bitrate alvo.
pub struct Controller {
    pub content: Content,
    pub ladder: Vec<Level>,
    pub idx: usize,          // nível em uso
    pub cpu_idx: usize,      // pior nível imposto pela sobrecarga local (0 = sem limite)
    up_ticks: u32,
    cpu_bad: u32,
    cpu_good: u32,
    /// teto de bitrate do modo manual (None = automático: o teto é o "automático" do nível em uso)
    pub manual_ceil: Option<f64>,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Decision { pub level: Level, pub bps: u32, pub net_limited: bool, pub cpu_limited: bool }

impl Controller {
    pub fn new(content: Content, max_h: usize, max_fps: u32, manual_kbps: u32) -> Self {
        Controller { content, ladder: ladder(content, max_h, max_fps), idx: 0, cpu_idx: 0, up_ticks: 0, cpu_bad: 0, cpu_good: 0, manual_ceil: if manual_kbps > 0 { Some(manual_kbps as f64 * 1000.0) } else { None } }
    }
    /// Teto de bitrate do nível de topo (o que o estimador nunca passa).
    pub fn top_ceil(&self) -> f64 { self.manual_ceil.unwrap_or(auto_bitrate(self.ladder[0].h, self.ladder[0].fps) as f64) }
    fn ceil_for(&self, l: Level) -> f64 { self.manual_ceil.unwrap_or(auto_bitrate(l.h, l.fps) as f64) }
    fn net_index(&self, budget: f64) -> usize { self.ladder.iter().position(|l| need_bps(*l) <= budget).unwrap_or(self.ladder.len() - 1) }

    /// Sobrecarga local. `work_ms` = tempo médio de conversão + codificação por quadro; `offered`/`sent` = quadros por segundo
    /// que a captura entregou e que saíram codificados. Chamado uma vez por segundo.
    pub fn note_cpu(&mut self, work_ms: f64, offered: f64, sent: f64) {
        let cur = self.ladder[self.idx];
        let period = 1000.0 / cur.fps as f64;
        let starving = offered >= cur.fps as f64 * 0.9 && sent < cur.fps as f64 * 0.75;
        if work_ms > period * 0.9 || starving {
            self.cpu_good = 0; self.cpu_bad += 1;
            if self.cpu_bad >= 2 && self.idx + 1 < self.ladder.len() { self.cpu_idx = self.idx + 1; self.cpu_bad = 0; }
        } else {
            self.cpu_bad = 0;
            // folga: o nível logo acima caberia com sobra?
            if self.cpu_idx > 0 {
                let up = self.ladder[self.cpu_idx - 1];
                let scale = (up.h as f64 * up.fps as f64) / (cur.h as f64 * cur.fps as f64);   // custo cresce com pixels por segundo
                if work_ms * scale.max(1.0) < (1000.0 / up.fps as f64) * 0.6 { self.cpu_good += 1; } else { self.cpu_good = 0; }
                if self.cpu_good >= 5 { self.cpu_idx -= 1; self.cpu_good = 0; }
            }
        }
    }

    /// Decide o nível para o orçamento de rede `budget` (bps). Cai na hora; só sobe com 30% de folga por 3 segundos seguidos.
    pub fn decide(&mut self, budget: f64) -> Decision {
        let net_now = self.net_index(budget);
        let net_up = self.net_index(budget / 1.3);
        let mut want = self.idx;
        if net_now > self.idx { want = net_now; self.up_ticks = 0; }
        else if net_up < self.idx { self.up_ticks += 1; if self.up_ticks >= 3 { want = net_up.max(self.idx.saturating_sub(3)); self.up_ticks = 0; } }   // até 3 degraus por vez
        else { self.up_ticks = 0; }
        let cpu_limited = self.cpu_idx > want;
        self.idx = want.max(self.cpu_idx).min(self.ladder.len() - 1);
        let level = self.ladder[self.idx];
        let bps = budget.min(self.ceil_for(level)).max(FLOOR_BPS) as u32;
        Decision { level, bps, net_limited: net_now > 0 && !cpu_limited || budget < self.ceil_for(self.ladder[0]) * 0.9, cpu_limited }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn detail_drops_fps_before_resolution_and_motion_the_opposite() {
        let d = ladder(Content::Detail, 1080, 30);
        assert_eq!(&d[..4], &[Level { h: 1080, fps: 30 }, Level { h: 1080, fps: 20 }, Level { h: 1080, fps: 15 }, Level { h: 1080, fps: 10 }]);
        assert_eq!(d[4], Level { h: 900, fps: 10 });
        let m = ladder(Content::Motion, 1080, 60);
        assert_eq!(&m[..3], &[Level { h: 1080, fps: 60 }, Level { h: 900, fps: 60 }, Level { h: 720, fps: 60 }]);
        assert!(m.iter().take_while(|l| l.fps == 60).count() >= 5, "a resolução cai inteira antes do FPS");
        assert_eq!(m.last().unwrap().fps, 15);
    }

    #[test]
    fn estimator_falls_fast_on_loss_or_delay_and_climbs_back_slowly() {
        let mut e = Estimator::new(8_000_000.0);
        let t0 = Instant::now();
        for i in 0..5 { e.update(t0 + Duration::from_secs(i), 8_000_000.0, Some((0.0, 20.0, 1.0)), 0.0); }
        assert_eq!(e.est, 8_000_000.0);
        // fila enchendo: RTT sobe muito acima da base → cai sem esperar perda
        let a = e.update(t0 + Duration::from_secs(6), 6_000_000.0, Some((0.0, 260.0, 5.0)), 0.0);
        assert!(a < 6_000_000.0 && e.reason == "atraso", "{a}");
        // perda pesada: cai mais
        let b = e.update(t0 + Duration::from_secs(9), 3_000_000.0, Some((0.25, 60.0, 5.0)), 0.0);
        assert!(b < 3_000_000.0 && e.reason == "perda", "{b}");
        // rede limpa: fica parado 3 s e depois sobe ~8% por segundo
        let c = e.update(t0 + Duration::from_secs(10), b, Some((0.0, 20.0, 1.0)), 0.0);
        assert_eq!(c, b);
        let mut v = c;
        for i in 13..27 { v = e.update(t0 + Duration::from_secs(i), v, Some((0.0, 20.0, 1.0)), 0.0); }
        assert!(v > b * 2.0 && v <= 8_000_000.0, "{v}");
        // perda leve aleatória (5%) não derruba
        let w = e.update(t0 + Duration::from_secs(41), v, Some((0.05, 20.0, 1.0)), 0.0);
        let _ = c;
        assert_eq!(w, v);
    }

    #[test]
    fn controller_steps_down_at_once_and_up_only_with_headroom() {
        let mut c = Controller::new(Content::Detail, 1080, 30, 0);
        assert_eq!(c.decide(4_500_000.0).level, Level { h: 1080, fps: 30 });
        let d = c.decide(900_000.0);   // 1080p10 pede ~1 Mbps: não cabe, desce a resolução
        assert!(d.level.fps == 10 && d.level.h < 1080 && d.net_limited, "{d:?}");
        let low = d.level;
        assert_eq!(c.decide(4_500_000.0).level, low, "não sobe no primeiro segundo");
        c.decide(4_500_000.0);
        assert_ne!(c.decide(4_500_000.0).level, low, "sobe depois de 3 s de folga");
        // sobrecarga local: 2 s com o quadro custando mais que o período → desce um degrau; folga por 5 s → volta
        let mut c = Controller::new(Content::Motion, 1080, 60, 0);
        c.decide(20_000_000.0);
        c.note_cpu(25.0, 60.0, 38.0); c.note_cpu(25.0, 60.0, 38.0);
        let d = c.decide(20_000_000.0);
        assert!(d.cpu_limited && d.level == Level { h: 900, fps: 60 }, "{d:?}");
        for _ in 0..5 { c.note_cpu(4.0, 60.0, 60.0); }
        for _ in 0..3 { c.decide(20_000_000.0); }
        assert_eq!(c.decide(20_000_000.0).level, Level { h: 1080, fps: 60 });
    }
}
