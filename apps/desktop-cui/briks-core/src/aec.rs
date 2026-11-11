//! Cancelamento acústico de eco (AEC) do microfone: o que sai pelos alto-falantes volta ao microfone atrasado e deformado pela sala.
//! O que tocamos (referência, 48 kHz mono) alimenta um filtro adaptativo NLMS que estima esse caminho e o subtrai do microfone.
//! O atraso entre "tocar" e "captar" (placa, driver, sala: dezenas de ms) é medido por correlação cruzada em 6 kHz a cada 0,5 s;
//! o filtro cobre uma janela de 1024 amostras (~21 ms) em torno dele. Fala dos dois lados ao mesmo tempo (double-talk) congela a adaptação.
//! A origem do tempo do microfone é fixada no primeiro bloco recebido (alinhada à referência), então o atraso medido é só o físico.
use crate::audio::RATE;

const TAPS: usize = 1024;
const PRE: usize = 256;                 // o filtro começa PRE amostras antes do atraso medido (margem do estimador)
const DEC: usize = 8;                   // decimação do estimador: 48 kHz -> 6 kHz
const MAX_LAG_MS: usize = 800;
const KEEP: usize = RATE as usize * 3;  // histórico da referência
const EST_EVERY: u64 = RATE as u64 / 2;
const EST_WIN: usize = 3000;            // 0,5 s a 6 kHz

pub struct Aec {
    refbuf: Vec<f32>, base: u64,        // referência: amostra de índice absoluto i = refbuf[i - base]
    ref_n: u64, cap_n: u64, started: bool,
    w: Vec<f32>,                        // pesos, w[j] multiplica ref[r - TAPS + 1 + j]
    delay: Option<usize>,               // atraso total estimado (amostras de 48 kHz): a referência da amostra n do microfone é n - delay
    dmic: Vec<f32>,                     // microfone decimado
    acc_m: f32, acc_k: usize,
    next_est: u64,
    hold: u32,                          // amostras restantes sem adaptar (fala local detectada)
    /// energias suavizadas de entrada e saída (diagnóstico)
    pub e_in: f32, pub e_out: f32,
}

impl Aec {
    pub fn new() -> Self {
        Aec { refbuf: Vec::new(), base: 0, ref_n: 0, cap_n: 0, started: false, w: vec![0.0; TAPS], delay: None, dmic: Vec::new(), acc_m: 0.0, acc_k: 0, next_est: EST_EVERY, hold: 0, e_in: 0.0, e_out: 0.0 }
    }

    /// Recomeça do zero (troca de dispositivo: os atrasos mudam).
    pub fn reset(&mut self) { *self = Aec::new(); }

    /// O que foi enviado aos alto-falantes (mono 48 kHz).
    pub fn push_ref(&mut self, x: &[f32]) {
        self.refbuf.extend_from_slice(x);
        self.ref_n += x.len() as u64;
        if self.refbuf.len() > KEEP * 2 { let d = self.refbuf.len() - KEEP; self.refbuf.drain(..d); self.base += d as u64; }
    }

    /// Atraso medido em ms (None enquanto não achou).
    pub fn delay_ms(&self) -> Option<f32> { self.delay.map(|d| d as f32 * 1000.0 / RATE as f32) }

    fn refat(&self, i: i64) -> f32 { if i < self.base as i64 || i >= self.ref_n as i64 { 0.0 } else { self.refbuf[(i - self.base as i64) as usize] } }

    /// Remove o eco do bloco `mic` (no lugar).
    pub fn process(&mut self, mic: &mut [f32]) {
        if !self.started { self.started = true; self.cap_n = self.ref_n; self.next_est = self.cap_n + EST_EVERY; }
        for chunk in mic.chunks_mut(480) {
            self.feed_est(chunk);
            self.cap_n += chunk.len() as u64;
            if self.cap_n >= self.next_est { self.next_est = self.cap_n + EST_EVERY; self.estimate(); }
            self.run(chunk);
        }
    }

    fn feed_est(&mut self, mic: &[f32]) {
        for &m in mic {
            self.acc_m += m; self.acc_k += 1;
            if self.acc_k == DEC { self.dmic.push(self.acc_m / DEC as f32); self.acc_m = 0.0; self.acc_k = 0; }
        }
        let cap = EST_WIN + 8;
        if self.dmic.len() > cap * 4 { let d = self.dmic.len() - cap; self.dmic.drain(..d); }
    }

    fn estimate(&mut self) {
        if self.dmic.len() < EST_WIN { return; }
        let max_lag = MAX_LAG_MS * RATE as usize / 1000 / DEC;
        // referência decimada cobrindo [fim - (EST_WIN + max_lag) * DEC, fim), onde fim = amostras do microfone já recebidas (índice absoluto igual)
        let end = self.cap_n as i64;
        let tot = EST_WIN + max_lag;
        let start = end - (tot * DEC) as i64;
        let mut dr = Vec::with_capacity(tot);
        for k in 0..tot { let b = start + (k * DEC) as i64; let mut a = 0.0; for j in 0..DEC as i64 { a += self.refat(b + j); } dr.push(a / DEC as f32); }
        let dm = &self.dmic[self.dmic.len() - EST_WIN..];
        let em: f32 = dm.iter().map(|v| v * v).sum();
        let er_all: f32 = dr[max_lag..].iter().map(|v| v * v).sum();
        if em < 1e-3 || er_all < 1e-3 { return; }   // sem som para correlacionar
        // lag l: mic[i] ~ ref[i - l]  =>  dr[max_lag + i - l]
        let (mut best, mut best_l, mut sum) = (0.0f32, 0usize, 0.0f32);
        for l in 0..max_lag {
            let seg = &dr[max_lag - l..max_lag - l + EST_WIN];
            let c: f32 = seg.iter().zip(dm).map(|(a, b)| a * b).sum();
            let e: f32 = seg.iter().map(|v| v * v).sum::<f32>().max(1e-9);
            let n = c.abs() / (e * em).sqrt();
            sum += n;
            if n > best { best = n; best_l = l; }
        }
        let mean = sum / max_lag as f32;
        if best > 0.25 && best > mean * 4.0 {
            let d = best_l * DEC;
            let cur = self.delay.unwrap_or(usize::MAX);
            if cur == usize::MAX || (d as i64 - cur as i64).abs() > 48 { self.w.iter_mut().for_each(|v| *v = 0.0); self.delay = Some(d); }
        }
    }

    fn run(&mut self, d: &mut [f32]) {
        let Some(delay) = self.delay else { return };
        let (mut ein, mut eout) = (0.0f32, 0.0f32);
        let n0 = self.cap_n - d.len() as u64;
        let mut px = -1.0f32;
        let mut prev_s = 0i64;
        // Geigel: o microfone mais alto que a referência recente (x0,7) só se há voz local por cima do eco; congela a adaptação por 50 ms
        let lo = (n0 as i64 - delay as i64 + PRE as i64 - TAPS as i64 + 1).max(self.base as i64);
        let hi = (n0 as i64 + d.len() as i64 - delay as i64 + PRE as i64).min(self.ref_n as i64);
        let xmax = if hi > lo { self.refbuf[(lo - self.base as i64) as usize..(hi - self.base as i64) as usize].iter().fold(0.0f32, |m, v| m.max(v.abs())) } else { 0.0 };
        if self.hold == 0 { for w in self.w.iter_mut() { *w *= 0.9995; } }   // vazamento leve (só adaptando): a deriva dos pesos não cresce sem limite
        for (k, v) in d.iter_mut().enumerate() {
            // a amostra n do microfone corresponde à referência r = n - delay + PRE; o filtro olha ref[r-TAPS+1 ..= r]
            let n = n0 + k as u64;
            let r = n as i64 - delay as i64 + PRE as i64;
            let s = r - TAPS as i64 + 1;
            if s < self.base as i64 || r >= self.ref_n as i64 { ein += *v * *v; eout += *v * *v; px = -1.0; continue; }
            let o = (s - self.base as i64) as usize;
            let x = &self.refbuf[o..o + TAPS];
            if px < 0.0 { px = x.iter().map(|a| a * a).sum(); }
            else { let old = self.refbuf[(prev_s - self.base as i64) as usize]; let new = x[TAPS - 1]; px = (px - old * old + new * new).max(0.0); }
            prev_s = s;
            let y: f32 = x.iter().zip(&self.w).map(|(a, b)| a * b).sum();
            let e = *v - y;
            // adapta só com a referência ativa e sem fala local por cima (double-talk: o filtro não explica o sinal e o erro passa do microfone)
            let ref_active = px > 1e-2;
            if v.abs() > 0.7 * xmax && v.abs() > 0.01 { self.hold = 2400; }
            if self.hold > 0 { self.hold -= 1; }
            if ref_active && self.hold == 0 { let g = 0.35 * e / (px + 1e-3); for (w, a) in self.w.iter_mut().zip(x) { *w += g * a; } }
            ein += *v * *v; eout += e * e;
            if ref_active { *v = e; }
        }
        self.e_in = self.e_in * 0.9 + ein * 0.1; self.e_out = self.e_out * 0.9 + eout * 0.1;
    }
}

impl Default for Aec { fn default() -> Self { Self::new() } }

#[cfg(test)]
mod tests {
    use super::*;
    use crate::audio::rms;

    struct Rng(u32);
    impl Rng { fn f(&mut self) -> f32 { self.0 = self.0.wrapping_mul(1664525).wrapping_add(1013904223); (self.0 >> 8) as f32 / 8388608.0 - 1.0 } }

    /// "Fala" sintética: ruído limitado em banda (passa-baixas simples) com envelope de sílabas.
    fn speech(n: usize, rng: &mut Rng) -> Vec<f32> {
        let mut lp = 0.0f32;
        (0..n).map(|i| { lp += (rng.f() - lp) * 0.35; let t = i as f32 / RATE as f32; let env = 0.55 + 0.45 * (2.0 * std::f32::consts::PI * 3.0 * t).sin(); lp * env * 1.2 }).collect()
    }

    /// Caminho acústico: atraso de `delay` amostras + resposta com alguns ecos curtos.
    fn echo_path(x: &[f32], delay: usize) -> Vec<f32> {
        let ir = [(0usize, 0.45f32), (7, 0.25), (23, -0.15), (61, 0.10), (140, -0.06)];
        (0..x.len()).map(|i| ir.iter().map(|(d, g)| if i >= delay + d { g * x[i - delay - d] } else { 0.0 }).sum()).collect()
    }

    fn db(a: f32, b: f32) -> f32 { 20.0 * (a.max(1e-9) / b.max(1e-9)).log10() }

    fn run_blocks(aec: &mut Aec, far: &[f32], mic: &[f32], near: Option<&[f32]>) -> Vec<f32> {
        let mut out = vec![];
        for (i, (f, m)) in far.chunks(480).zip(mic.chunks(480)).enumerate() {
            aec.push_ref(f);
            let mut c: Vec<f32> = m.to_vec();
            if let Some(n) = near { for (a, b) in c.iter_mut().zip(&n[i * 480..]) { *a += b; } }
            aec.process(&mut c);
            out.extend(c);
        }
        out
    }

    #[test]
    fn echo_is_attenuated_and_delay_found() {
        let mut rng = Rng(7);
        let n = RATE as usize * 8;
        let far = speech(n, &mut rng);
        let mic = echo_path(&far, 1680); // 35 ms
        let mut aec = Aec::new();
        let out = run_blocks(&mut aec, &far, &mic, None);
        let found = aec.delay_ms().expect("achou o atraso");
        assert!(found > 30.0 && found < 55.0, "atraso medido {found} ms");
        let tail = n - RATE as usize * 2;
        let erle = db(rms(&mic[tail..]), rms(&out[tail..]));
        assert!(erle > 15.0, "o eco precisa cair mais de 15 dB, caiu {erle:.1} dB");
    }

    #[test]
    fn near_end_voice_survives_double_talk() {
        let mut rng = Rng(11);
        let n = RATE as usize * 10;
        let far = speech(n, &mut rng);
        let mic = echo_path(&far, 2400);
        let mut aec = Aec::new();
        let m = RATE as usize * 5;
        run_blocks(&mut aec, &far[..m], &mic[..m], None); // primeiro só eco: converge
        // depois a pessoa fala por cima (tom de 300 Hz + harmônico)
        let near: Vec<f32> = (0..n - m).map(|i| { let t = i as f32 / RATE as f32; 0.25 * (2.0 * std::f32::consts::PI * 300.0 * t).sin() + 0.1 * (2.0 * std::f32::consts::PI * 900.0 * t).sin() }).collect();
        let out = run_blocks(&mut aec, &far[m..], &mic[m..], Some(&near));
        let s = RATE as usize;
        let (o, nr) = (rms(&out[s..]), rms(&near[s..]));
        assert!(o > nr * 0.7 && o < nr * 1.5, "voz local preservada: saída {o:.3} vs voz {nr:.3}");
        let resid: Vec<f32> = out[s..].iter().zip(&near[s..]).map(|(a, b)| a - b).collect();
        let eco = rms(&mic[m + s..]);
        assert!(db(eco, rms(&resid)) > 8.0, "eco residual na fala dos dois lados: queda de {:.1} dB", db(eco, rms(&resid)));
    }

    #[test]
    fn silent_reference_leaves_mic_untouched() {
        let mut rng = Rng(3);
        let mut aec = Aec::new();
        let n = RATE as usize * 3;
        let far = vec![0.0f32; n];
        let mic = speech(n, &mut rng);
        let out = run_blocks(&mut aec, &far, &mic, None);
        assert!((rms(&out) - rms(&mic)).abs() < 1e-4, "sem som tocando, a voz passa intacta");
    }
}
