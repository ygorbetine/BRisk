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

/// Piso de bitrate: abaixo disto 360p a 10 qps vira borrão e o quadro-chave não fecha; é melhor segurar aqui e deixar a
/// retransmissão trabalhar do que continuar descendo.
pub const FLOOR_BPS: f64 = 250_000.0;

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
    /// menor RTT já visto deste espectador (a base dele): serve para dizer se ESTE caminho está congestionado
    pub rtt_min: f32,
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
    /// Regra da maioria numa malha: se pelo menos metade dos espectadores está bem, quem está congestionado (perda alta ou RTT
    /// muito acima da própria base) deixa de ditar o bitrate — fica com a imagem pior em vez de derrubar a de todos.
    /// Devolve (pior entre os que contam, quantos foram deixados de fora).
    pub fn majority(&self) -> (Option<(f32, f32, f32)>, usize) {
        let fresh: Vec<&PeerNet> = self.peers.values().filter(|p| p.updated.elapsed() < Duration::from_millis(3500)).collect();
        if fresh.is_empty() { return (None, 0); }
        let bad = |p: &&PeerNet| p.loss > 0.08 || (p.rtt_ms > 0.0 && p.rtt_min > 0.0 && p.rtt_ms > p.rtt_min + (p.rtt_min * 0.5).max(80.0));
        let healthy: Vec<&&PeerNet> = fresh.iter().filter(|p| !bad(p)).collect();
        let worst = |v: &[&&PeerNet]| (v.iter().map(|p| p.loss).fold(0.0, f32::max), v.iter().map(|p| p.rtt_ms).fold(0.0, f32::max), v.iter().map(|p| p.jitter_ms).fold(0.0, f32::max));
        if fresh.len() >= 2 && healthy.len() < fresh.len() && healthy.len() * 2 >= fresh.len() { return (Some(worst(&healthy)), fresh.len() - healthy.len()); }
        let all: Vec<&&PeerNet> = fresh.iter().collect();
        (Some(worst(&all)), 0)
    }
    pub fn totals(&self) -> (u64, u64) { (self.peers.values().map(|p| p.nacks).sum(), self.peers.values().map(|p| p.plis).sum()) }
}

/// Estimador de banda disponível (bps). Chamado uma vez por segundo.
///
/// Regras (2ª geração, depois de medir a 1ª descer a ~0,4 Mbps onde cabiam ~5 e levar 20 s para voltar):
///   - **Cortar para o que passou, não em cascata.** Com perda, a taxa que chegou ao outro lado é `enviado × (1 − perda)`: é a
///     medida da capacidade. A estimativa vai para 90% disso de uma vez, em vez de multiplicar por 0,7 a cada segundo.
///   - **Sinal velho não corta de novo.** O RTT e a perda dos Receiver Reports chegam 1–2 s atrasados: depois de um corte eles ainda
///     descrevem o envio antigo. Só a fração de pacotes pedidos de novo (NACK, do último segundo) corta em seguida; o atraso
///     espera 2,5 s, e no máximo dois cortes seguidos — se o RTT não cede mesmo cortando, o atraso não é nosso: vira a nova base.
///   - **Memória da capacidade com sondagem.** O teto lembrado fica firme por 4 s e depois deixa subir; bater de novo no mesmo
///     teto dobra o tempo firme (até 16 s), para não ficar derrubando a imagem a cada sondagem numa rede que não mudou.
pub struct Estimator {
    pub est: f64,
    pub ceil: f64,
    rtt_hist: Vec<f32>,        // últimas medidas (base = mínimo)
    hold_until: Instant,       // sem subir até aqui (depois de uma queda)
    last_cut: Option<Instant>,
    /// cortes seguidos só por atraso, sem perda (limite 2: depois disso o atraso vira a base)
    delay_cuts: u32,
    /// RTT no corte anterior da série por atraso
    first_cut_rtt: f32,
    /// capacidade observada no último congestionamento (bps), quando, e por quantos segundos ela vale como teto firme
    cap: Option<(f64, Instant, f64)>,
    pub reason: &'static str,  // "", "perda", "atraso"
    /// Perda tolerada sem cortar (começa em 10%). Sobe quando um corte de sondagem mostra que a perda NÃO responde ao nosso
    /// bitrate (perda aleatória: Wi-Fi, rádio, enlace ruim) e volta devagar a 10% quando a perda some.
    pub loss_tol: f32,
    /// corte de sondagem em avaliação: (estimativa antes, perda antes, soma e nº de amostras da perda depois do corte)
    probe: Option<(f64, f32, f32, u32)>,
    /// atraso (ms acima da base) tolerado sem cortar: sobe quando dois cortes seguidos não fizeram o RTT ceder (atraso que não é
    /// fila nossa: rota, Wi-Fi, outro tráfego) e volta devagar a zero
    delay_tol: f32,
}

impl Estimator {
    pub fn new(ceil: f64) -> Self { Estimator { est: ceil, ceil, rtt_hist: vec![], hold_until: Instant::now(), last_cut: None, delay_cuts: 0, first_cut_rtt: 0.0, cap: None, reason: "", loss_tol: 0.10, probe: None, delay_tol: 0.0 } }
    pub fn set_ceil(&mut self, ceil: f64) { self.ceil = ceil; if self.est > ceil { self.est = ceil; } }
    pub fn rtt_base(&self) -> f32 { self.rtt_hist.iter().copied().fold(f32::MAX, f32::min) }

    fn remember_cap(&mut self, now: Instant, c: f64) { self.cap = Some((c, now, 3.0)); }

    /// `sent` = bps realmente enviados no último segundo; `net` = pior espectador (None = sem relatórios);
    /// `nack_ratio` = pacotes pedidos de novo pela 1ª vez / pacotes enviados no último segundo.
    pub fn update(&mut self, now: Instant, sent: f64, net: Option<(f32, f32, f32)>, nack_ratio: f32) -> f64 {
        let Some((loss, rtt, _)) = net else { self.reason = ""; return self.est; };
        if rtt > 0.0 { self.rtt_hist.push(rtt); if self.rtt_hist.len() > 30 { self.rtt_hist.remove(0); } }
        let base = if self.rtt_hist.is_empty() { 0.0 } else { self.rtt_base() };
        let delayed = rtt > 0.0 && base > 0.0 && rtt > base + (base * 0.5).max(80.0) + self.delay_tol;
        let since_cut = self.last_cut.map(|t| now.saturating_duration_since(t).as_secs_f64()).unwrap_or(f64::MAX);
        // a perda dos relatórios é atrasada: logo depois de um corte ela ainda é a do envio antigo, então só vale a dos NACKs
        let loss_eff = if since_cut < 2.5 { nack_ratio.min(1.0) } else { loss.max(nack_ratio.min(1.0)) };
        let using = sent > FLOOR_BPS;
        // Avaliação do corte de sondagem, pela MÉDIA da perda nos 2 s depois dele (a perda aleatória oscila em torno do limite: um
        // instante abaixo dele não prova que o corte adiantou). Não caiu → aleatória: devolve o bitrate e passa a tolerá-la.
        if let Some((before, l0, sum, n)) = self.probe {
            let (sum, n) = (sum + loss_eff, n + 1);
            if since_cut >= 2.0 {
                self.probe = None;
                let after = sum / n as f32;
                if after > l0 * 0.6 {
                    self.est = before.min(self.ceil);
                    self.loss_tol = (after.max(l0) * 1.5 + 0.04).min(0.45);
                    self.hold_until = now;
                    self.reason = "";
                    return self.est;
                }
                // respondeu: era congestionamento sem fila (policiamento); o corte de 20% fica e o laço normal segue
            } else { self.probe = Some((before, l0, sum, n)); }
        }
        // Perda × congestionamento. Perda com a fila enchendo (RTT subindo) ou perda muito alta é congestionamento: corta. Perda
        // SEM aumento de atraso pode ser aleatória, e cortar não a diminui — só piora a imagem (medido no SFU: 10% de perda
        // aleatória levava 1316 → 575 → 223 kbps). Nesse caso faz UM corte de sondagem de 20% e olha 2 s depois: se a perda caiu,
        // era congestionamento sem fila (policiamento) e o corte fica; se não caiu, é aleatória: devolve o bitrate e passa a
        // tolerar essa perda (a retransmissão é que cuida dela).
        let rising = rtt > 0.0 && base > 0.0 && rtt > base + (base * 0.25).max(25.0) + self.delay_tol;
        if loss_eff > self.loss_tol {
            let evidence = rising || delayed || loss_eff > (self.loss_tol + 0.12).max(0.25);
            let cut_now = evidence;
            if evidence { self.probe = None; }
            else if self.probe.is_none() && since_cut >= 2.5 {
                self.probe = Some((self.est, loss_eff, 0.0, 0));
                self.est = (self.est * 0.8).max(FLOOR_BPS);
                self.last_cut = Some(now);
            }
            // Quanto cortar: o `enviado` do segundo logo depois de um corte NÃO mede a capacidade (o emissor parou para se
            // recuperar: 0 qps), então ele nunca derruba a estimativa abaixo de metade num corte só, e dentro de 2,5 s do corte
            // anterior só uma perda extrema (> 30%) corta de novo, e só 25%.
            if cut_now && (since_cut >= 2.5 || (since_cut >= 0.9 && loss_eff > 0.30)) {
                let passed = if using { sent * (1.0 - loss_eff as f64) } else { self.est * (1.0 - 0.5 * loss_eff as f64) };
                let cut = if since_cut < 2.5 { self.est * 0.75 } else { (passed * 0.9).max(self.est * 0.35) };
                self.est = self.est.min(cut).max(FLOOR_BPS);
                if using { self.remember_cap(now, self.est); }
                self.last_cut = Some(now);
            }
            self.delay_cuts = 0;
            self.hold_until = now + Duration::from_secs(2);
            self.reason = "perda";
        } else if delayed {
            if since_cut >= 2.5 {
                if self.delay_cuts >= 2 && rtt <= self.first_cut_rtt * 1.15 {
                    // dois cortes e o atraso continua SEM crescer: não é fila nossa (rota mudou, Wi-Fi, outro tráfego). Passa a
                    // tolerar esse tanto de atraso em vez de cortar a cada vez que ele aparece.
                    self.delay_tol = self.delay_tol.max((rtt - base) * 1.2); self.delay_cuts = 0;
                } else {
                    self.first_cut_rtt = rtt;   // RTT no corte anterior: se o próximo não for maior, o atraso não está crescendo
                    let from = if using { self.est.min(sent.max(self.est * 0.5)) } else { self.est };
                    self.est = (from * 0.85).max(FLOOR_BPS);
                    if using { self.remember_cap(now, self.est); }
                    self.last_cut = Some(now); self.delay_cuts += 1;
                }
            }
            self.hold_until = now + Duration::from_secs(2);
            self.reason = "atraso";
        } else {
            // a série de cortes por atraso só zera depois de 10 s sem corte (um segundo limpo no meio não prova nada)
            if since_cut > 10.0 { self.delay_cuts = 0; }
            self.delay_tol *= 0.995;
            // perda leve (até 6%) é consertada por retransmissão e não impede de subir; entre 6 e 10% só segura
            if loss_eff < self.loss_tol * 0.5 { self.loss_tol = (self.loss_tol - 0.005).max(0.10); }
            if loss_eff < (self.loss_tol - 0.04).max(0.06) && now >= self.hold_until {
                // Sobe só quando a estimativa está sendo usada (senão cresceria sem ser testada): rápido (25%/s) enquanto está
                // abaixo de metade do teto, devagar (10%/s) perto dele.
                if sent >= self.est * 0.7 || self.est < self.ceil * 0.25 {
                    let mut k: f64 = if self.est < self.ceil * 0.5 { 1.25 } else { 1.10 };
                    // teto da capacidade lembrada: firme por 3 s; depois a subida continua, mas só 8%/s enquanto está na altura dele
                    // (sondagem cuidadosa: bater de novo custa um corte; passar bem acima dele já é capacidade nova, sobe rápido)
                    let mut firm_cap = None;
                    if let Some((c, t, firm)) = self.cap {
                        if now.saturating_duration_since(t).as_secs_f64() < firm { firm_cap = Some(c); } else if self.est >= c * 0.9 && self.est < c * 1.3 { k = k.min(1.08); } else if self.est >= c * 1.3 { self.cap = None; }
                    }
                    let mut next = (self.est * k + 40_000.0).min(self.ceil);
                    if let Some(c) = firm_cap { next = next.min(c.max(self.est)); }
                    self.est = next.max(FLOOR_BPS);
                }
                if self.est >= self.ceil * 0.98 { self.reason = ""; }
            }
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
    cpu_relaxed: bool,
    /// enviado / pedido no último segundo (telemetria). Conteúdo muito difícil (ruído, jogo caótico) não comprime até o alvo em
    /// resolução alta; o piso de bits por pixel abaixo é o que faz a escada pular direto para a resolução que cabe.
    pub overshoot: f64,
    /// piso medido de bits por pixel por quadro (0 = sem piso conhecido) e proporção largura/altura do que está sendo codificado
    pub bpp_floor: f64,
    bpp_prev: Option<f64>,
    aspect: f64,
    /// teto de bitrate do modo manual (None = automático: o teto é o "automático" do nível em uso)
    pub manual_ceil: Option<f64>,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Decision { pub level: Level, pub bps: u32, pub net_limited: bool, pub cpu_limited: bool }

impl Controller {
    pub fn new(content: Content, max_h: usize, max_fps: u32, manual_kbps: u32) -> Self {
        Controller { content, ladder: ladder(content, max_h, max_fps), idx: 0, cpu_idx: 0, up_ticks: 0, cpu_bad: 0, cpu_good: 0, cpu_relaxed: false, overshoot: 1.0, bpp_floor: 0.0, bpp_prev: None, aspect: 16.0 / 9.0, manual_ceil: if manual_kbps > 0 { Some(manual_kbps as f64 * 1000.0) } else { None } }
    }
    /// Teto de bitrate do nível de topo (o que o estimador nunca passa).
    pub fn top_ceil(&self) -> f64 { self.manual_ceil.unwrap_or(auto_bitrate(self.ladder[0].h, self.ladder[0].fps) as f64) }
    fn ceil_for(&self, l: Level) -> f64 { self.manual_ceil.unwrap_or(auto_bitrate(l.h, l.fps) as f64) }
    /// Bitrate necessário para o nível: o mínimo de qualidade ou, se maior, o que o codificador comprovadamente não consegue
    /// baixar neste conteúdo (piso medido de bits por pixel).
    fn need(&self, l: Level) -> f64 {
        let pixels = l.h as f64 * l.h as f64 * self.aspect;
        need_bps(l).max(self.bpp_floor * pixels * l.fps as f64 * 1.1)
    }
    fn net_index(&self, budget: f64) -> usize { self.ladder.iter().position(|l| self.need(*l) <= budget).unwrap_or(self.ladder.len() - 1) }
    /// Chamado uma vez por segundo com quadros saindo: `sent`/`asked` = bps que saíram do codificador e bps pedidos; `fps` = quadros
    /// codificados por segundo; `w`x`h` = tamanho codificado. Se o codificador passa do pedido, aprende o piso de bits por pixel.
    pub fn note_encoder(&mut self, sent: f64, asked: f64, fps: f64, w: usize, h: usize) {
        if asked <= 0.0 || sent <= 0.0 || fps < 0.5 || w == 0 || h == 0 { return; }
        self.aspect = w as f64 / h as f64;
        let bpp = sent / fps / (w * h) as f64;
        if sent > asked * 1.15 {
            // vale o menor de dois segundos seguidos de excesso: um quadro-chave isolado infla a medida de um segundo só
            if let Some(prev) = self.bpp_prev { let c = prev.min(bpp); self.bpp_floor = if c > self.bpp_floor { c } else { self.bpp_floor * 0.7 + c * 0.3 }; }
            self.bpp_prev = Some(bpp);
        } else { self.bpp_prev = None; self.bpp_floor *= 0.9; }   // obedeceu: o conteúdo ficou mais fácil, esquece aos poucos
        self.overshoot = (sent / asked).clamp(0.0, 8.0);
    }

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
                if self.cpu_good >= 3 { self.cpu_idx -= 1; self.cpu_good = 0; self.cpu_relaxed = true; }
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
        // a folga local voltou e era ela que segurava o nível: sobe já até onde a rede deixa (sem esperar os 3 s da rede)
        if std::mem::take(&mut self.cpu_relaxed) && want > net_now.max(self.cpu_idx) { want = net_now.max(self.cpu_idx); }
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

    /// Enlace simulado, um passo por segundo: capacidade `cap` (bps), fila de 250 ms, perda aleatória `rand`, RTT base `base`
    /// (+ `wobble` de variação que não depende do envio). O RTT e a perda dos relatórios chegam ao estimador 2 s atrasados; a
    /// fração de NACK é a do segundo. O codificador obedece à estimativa com 1 s de atraso. Devolve a estimativa de cada segundo.
    fn sim(e: &mut Estimator, t0: Instant, start: u64, steps: &[(u64, f64, f64)], base: f32, wobble: f32) -> Vec<f64> {
        let (mut out, mut backlog, mut hist) = (vec![], 0.0f64, vec![(0.0f32, base); 3]);
        let mut sent = e.est;
        let mut t = start;
        for (secs, cap, rand) in steps {
            for _ in 0..*secs {
                let arrive = sent + backlog;
                let passed = arrive.min(*cap);
                let over = (arrive - passed - cap * 0.25).max(0.0);
                backlog = (arrive - passed).min(cap * 0.25);
                let lost = (over / sent.max(1.0)) as f32 + *rand as f32;
                let rtt = base + (backlog / cap * 1000.0) as f32 + if t % 3 == 0 { wobble } else { 0.0 };
                hist.push((lost, rtt));
                let old = hist[hist.len() - 3];
                let est = e.update(t0 + Duration::from_secs(t), sent * (1.0 - *rand), Some((old.0, old.1, 1.0)), lost);
                if std::env::var("BRIKS_EST_DEBUG").is_ok() { eprintln!("t={t} sent={:.0} rtt_rep={:.0} lost={:.3} est={:.0} reason={} tol={:.0} cuts={}", sent/1000.0, old.1, lost, est/1000.0, e.reason, e.delay_tol, e.delay_cuts); }
                out.push(est);
                sent = est;
                t += 1;
            }
        }
        out
    }
    fn avg(v: &[f64]) -> f64 { v.iter().sum::<f64>() / v.len() as f64 }

    #[test]
    fn sharp_drop_lands_near_the_new_capacity_instead_of_collapsing() {
        let mut e = Estimator::new(9_000_000.0);
        let v = sim(&mut e, Instant::now(), 0, &[(8, 20e6, 0.0), (20, 6e6, 0.0)], 20.0, 0.0);
        let after = &v[8..];
        let low = after.iter().copied().fold(f64::MAX, f64::min);
        assert!(low > 3_500_000.0, "desceu demais numa queda de 20 para 6 Mbps: mínimo {low:.0} ({after:?})");
        assert!(avg(&after[3..]) > 4_500_000.0 && avg(&after[3..]) < 6_100_000.0, "média fora da capacidade: {:.0}", avg(&after[3..]));
    }

    #[test]
    fn tight_lossy_link_is_used_and_recovery_is_quick_when_capacity_returns() {
        let mut e = Estimator::new(9_000_000.0);
        let v = sim(&mut e, Instant::now(), 0, &[(6, 20e6, 0.0), (14, 6e6, 0.0), (14, 2e6, 0.02), (14, 0.8e6, 0.05), (18, 6e6, 0.0)], 20.0, 0.0);
        let (m2, m08) = (avg(&v[26..34]), avg(&v[40..48]));
        assert!(m2 > 1_300_000.0 && m2 < 2_050_000.0, "2 Mbps com 2% de perda: {m2:.0}");
        assert!(m08 > 450_000.0 && m08 < 830_000.0, "0,8 Mbps com 5% de perda: {m08:.0}");
        // a banda volta a 6 Mbps no segundo 48: em 14 s tem de estar acima de 4 Mbps
        assert!(v[48 + 14] > 4_000_000.0, "recuperação lenta: {:?}", &v[48..]);
    }

    #[test]
    fn delay_that_does_not_answer_to_our_rate_does_not_spiral_down() {
        // RTT base alto com variação que não é fila nossa (rota longa, Wi-Fi) e 2% de perda aleatória em 4 Mbps
        let mut e = Estimator::new(6_000_000.0);
        let v = sim(&mut e, Instant::now(), 0, &[(40, 4e6, 0.02)], 150.0, 260.0);
        let low = v[10..].iter().copied().fold(f64::MAX, f64::min);
        assert!(low > 1_300_000.0 && avg(&v[10..]) > 2_000_000.0, "espiral por atraso alheio: mínimo {low:.0}, média {:.0}, série {:?}", avg(&v[10..]), v.iter().map(|x| (x / 1000.0) as u32).collect::<Vec<_>>());
    }

    #[test]
    fn deep_buffer_without_loss_still_backs_off_on_delay() {
        // fila funda (sem perda): só o atraso avisa. 8 Mbps num enlace de 5: tem de descer para perto de 5 e ficar
        let mut e = Estimator::new(8_000_000.0);
        let t0 = Instant::now();
        for i in 0..5 { e.update(t0 + Duration::from_secs(i), 8_000_000.0, Some((0.0, 20.0, 1.0)), 0.0); }
        let mut rtt = 20.0f32;
        let mut v = vec![];
        for i in 5..30 { let sent = e.est; rtt = (rtt + ((sent - 5e6) / 5e6 * 1000.0) as f32).clamp(20.0, 3000.0); v.push(e.update(t0 + Duration::from_secs(i), sent, Some((0.0, rtt, 1.0)), 0.0)); }
        // dente de serra em torno da capacidade (sobe sondando, corta quando o atraso cresce): vale a média
        let m = avg(&v[8..]);
        assert!(m < 5_900_000.0 && m > 3_500_000.0, "média {m:.0}: {v:?}");
    }

    #[test]
    fn random_loss_without_delay_does_not_drag_the_bitrate_down() {
        // 10% de perda aleatória num enlace folgado (20 Mbps): a estimativa tem de ficar perto do teto, não descer sem parar
        for rand in [0.05, 0.10, 0.15] {
            let mut e = Estimator::new(3_000_000.0);
            let v = sim(&mut e, Instant::now(), 0, &[(5, 20e6, 0.0), (40, 20e6, rand)], 20.0, 0.0);
            let low = v[5..].iter().copied().fold(f64::MAX, f64::min);
            assert!(low >= 2_300_000.0, "perda aleatória de {rand}: desceu a {low:.0} ({:?})", v.iter().map(|x| (x / 1000.0) as u32).collect::<Vec<_>>());
            assert!(avg(&v[15..]) > 2_800_000.0, "perda aleatória de {rand}: média {:.0}", avg(&v[15..]));
        }
    }

    #[test]
    fn noisy_random_loss_around_the_threshold_does_not_ratchet_down() {
        // como no sistema real: 10% de perda aleatória medida em poucos pacotes oscila entre ~6% e ~20% de um segundo para o outro
        let mut e = Estimator::new(3_000_000.0);
        let t0 = Instant::now();
        let noise = [0.10f32, 0.06, 0.16, 0.09, 0.12, 0.20, 0.07, 0.11, 0.15, 0.08];
        let mut v = vec![];
        for i in 0..5 { e.update(t0 + Duration::from_secs(i), 3_000_000.0, Some((0.0, 30.0, 3.0)), 0.0); }
        for i in 5..65u64 { let l = noise[(i % 10) as usize]; let sent = e.est; v.push(e.update(t0 + Duration::from_secs(i), sent, Some((0.01, 30.0, 20.0)), l)); }
        let low = v[10..].iter().copied().fold(f64::MAX, f64::min);
        assert!(low >= 2_300_000.0 && avg(&v[20..]) > 2_700_000.0, "desceu a {low:.0}, média {:.0}: {:?}", avg(&v[20..]), v.iter().map(|x| (x / 1000.0) as u32).collect::<Vec<_>>());
    }

    #[test]
    fn policer_loss_without_queue_still_converges() {
        // policiamento: tudo acima de 3 Mbps é descartado, sem fila (o RTT não sobe). A perda responde ao bitrate: tem de descer.
        let mut e = Estimator::new(4_000_000.0);
        let t0 = Instant::now();
        let mut v = vec![];
        let mut sent = e.est;
        for i in 0..30 {
            let lost = ((sent - 3e6).max(0.0) / sent) as f32;
            let est = e.update(t0 + Duration::from_secs(i), sent * (1.0 - lost as f64), Some((lost, 20.0, 1.0)), lost);
            v.push(est); sent = est;
        }
        assert!(avg(&v[10..]) < 3_500_000.0 && avg(&v[10..]) > 2_200_000.0, "policiamento: {:?}", v.iter().map(|x| (x / 1000.0) as u32).collect::<Vec<_>>());
    }

    #[test]
    fn congestion_on_top_of_random_loss_is_still_detected() {
        // 5% aleatória o tempo todo; no segundo 20 a banda cai de 20 para 2 Mbps: tem de descer para perto de 2
        let mut e = Estimator::new(4_000_000.0);
        let v = sim(&mut e, Instant::now(), 0, &[(20, 20e6, 0.05), (20, 2e6, 0.05)], 20.0, 0.0);
        assert!(avg(&v[5..20]) > 3_500_000.0, "antes da queda: {:.0}", avg(&v[5..20]));
        assert!(avg(&v[28..]) < 2_300_000.0 && avg(&v[28..]) > 1_000_000.0, "depois da queda: {:?}", v[20..].iter().map(|x| (x / 1000.0) as u32).collect::<Vec<_>>());
    }

    #[test]
    fn light_random_loss_does_not_cut() {
        let mut e = Estimator::new(8_000_000.0);
        let t0 = Instant::now();
        for i in 0..20 { e.update(t0 + Duration::from_secs(i), 8_000_000.0, Some((0.05, 20.0, 1.0)), 0.05); }
        assert_eq!(e.est, 8_000_000.0);
    }

    #[test]
    fn one_congested_viewer_does_not_drag_the_majority() {
        let peer = |loss: f32, rtt: f32| PeerNet { loss, rtt_ms: rtt, jitter_ms: 1.0, updated: Instant::now(), nacks: 0, plis: 0, rtt_min: 20.0 };
        let mut n = NetShared::default();
        n.peers.insert("bom1".into(), peer(0.0, 25.0));
        n.peers.insert("ruim".into(), peer(0.30, 400.0));
        // 1 bom + 1 ruim: metade está bem → o ruim fica de fora
        assert_eq!(n.majority(), (Some((0.0, 25.0, 1.0)), 1));
        n.peers.insert("bom2".into(), peer(0.01, 30.0));
        assert_eq!(n.majority(), (Some((0.01, 30.0, 1.0)), 1));
        // a maioria está mal: aí o problema é do emissor ou da rede de todos → vale o pior
        n.peers.insert("ruim2".into(), peer(0.2, 300.0));
        n.peers.insert("ruim3".into(), peer(0.5, 500.0));
        assert_eq!(n.majority(), (Some((0.5, 500.0, 1.0)), 0));
    }

    #[test]
    fn incompressible_content_jumps_straight_to_a_resolution_that_fits() {
        let mut c = Controller::new(Content::Detail, 1080, 30, 12_000);
        c.decide(12_000_000.0);
        // ruído em 1080p: pediu 1,5 Mbps, saíram 4,3 Mbps a 3 qps → ~0,7 bit por pixel por quadro
        c.note_encoder(4_300_000.0, 1_500_000.0, 3.0, 1920, 1080);
        c.note_encoder(4_300_000.0, 1_500_000.0, 3.0, 1920, 1080);
        let d = c.decide(1_500_000.0);
        assert!(d.level.h <= 360 && d.level.fps == 10, "desce de uma vez: {d:?}");
        // conteúdo fácil de novo: o piso some e a resolução volta
        for _ in 0..40 { c.note_encoder(900_000.0, 1_200_000.0, 10.0, 640, 360); }
        for _ in 0..8 { c.decide(1_500_000.0); }
        assert!(c.decide(1_500_000.0).level.h >= 1080, "voltou a 1080p");
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
        for _ in 0..3 { c.note_cpu(4.0, 60.0, 60.0); }
        assert_eq!(c.decide(20_000_000.0).level, Level { h: 1080, fps: 60 });
    }
}
