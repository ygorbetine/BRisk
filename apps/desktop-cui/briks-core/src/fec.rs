//! Correção de erro antecipada (FEC) da transmissão de tela, por paridade XOR.
//!
//! O webrtc-rs 0.12 não traz FEC de vídeo (só os nomes dos formatos), e um pacote de redundância na própria faixa de vídeo seria
//! descartado pelo SFU. Por isso a paridade é nossa e viaja FORA da faixa, por um canal de dados sem retransmissão (`_lossy` no SFU,
//! `briks.fec` na malha). O emissor junta os pacotes RTP de vídeo em grupos de até `GROUP_MAX` e manda 1 pacote de paridade por grupo:
//! o XOR dos conteúdos, dos tamanhos, dos timestamps e dos marcadores. Se faltar UM pacote do grupo, o receptor o refaz com a paridade
//! e os outros pacotes, sem esperar o pedido de retransmissão (NACK) ir e voltar. Faltando dois ou mais, a retransmissão cuida, como
//! sempre. Quem não conhece o canal (versão antiga) simplesmente não recebe a paridade.
//!
//! Formato do pacote de paridade: `MAGIC` (1) · 1º nº de sequência (2, big endian) · quantos pacotes (1) · XOR dos tamanhos (2) ·
//! XOR dos timestamps (4) · XOR dos marcadores (1) · XOR dos conteúdos (cada um completado com zeros até o maior).
use std::collections::{HashMap, VecDeque};

use bytes::Bytes;

pub const MAGIC: u8 = 0xF1;
/// Tamanho máximo do grupo: 1 paridade para até 10 pacotes (10% de banda a mais num quadro grande).
pub const GROUP_MAX: usize = 10;
/// Um grupo só fecha no fim do quadro se já tem este tanto; menor que isso segue para o quadro seguinte (um quadro de 2 pacotes com
/// paridade própria custaria 50% a mais).
pub const GROUP_MIN: usize = 4;
const HEAD: usize = 11;
/// Maior conteúdo de pacote aceito (o empacotador usa MTU 1180).
const MAX_PAYLOAD: usize = 1500;
const HISTORY: usize = 768;
const PARITIES: usize = 96;

/// Pacote RTP de vídeo, só com o que o montador de quadros usa.
#[derive(Debug, Clone, PartialEq)]
pub struct Pkt { pub seq: u16, pub ts: u32, pub marker: bool, pub payload: Bytes }

/// Lado do emissor: acumula os pacotes na ordem em que saem e devolve a paridade quando o grupo fecha.
#[derive(Default)]
pub struct FecTx { base: u16, n: usize, len_xor: u16, ts_xor: u32, mk_xor: u8, xor: Vec<u8> }
impl FecTx {
    pub fn new() -> Self { Self::default() }
    /// Esquece o grupo em andamento (troca de codec, FEC desligado).
    pub fn reset(&mut self) { self.n = 0; self.len_xor = 0; self.ts_xor = 0; self.mk_xor = 0; self.xor.clear(); }
    /// Entra um pacote enviado. `frame_end` = último pacote do quadro. Devolve a paridade se o grupo fechou.
    pub fn add(&mut self, seq: u16, ts: u32, marker: bool, payload: &[u8], frame_end: bool) -> Option<Vec<u8>> {
        if payload.len() > MAX_PAYLOAD { self.reset(); return None; }
        // os grupos são de números de sequência seguidos: um salto (não deveria haver) recomeça o grupo
        if self.n > 0 && seq != self.base.wrapping_add(self.n as u16) { self.reset(); }
        if self.n == 0 { self.base = seq; }
        self.n += 1;
        self.len_xor ^= payload.len() as u16;
        self.ts_xor ^= ts;
        self.mk_xor ^= marker as u8;
        if self.xor.len() < payload.len() { self.xor.resize(payload.len(), 0); }
        for (x, b) in self.xor.iter_mut().zip(payload) { *x ^= *b; }
        if self.n >= GROUP_MAX || (frame_end && self.n >= GROUP_MIN) {
            let mut out = Vec::with_capacity(HEAD + self.xor.len());
            out.push(MAGIC);
            out.extend_from_slice(&self.base.to_be_bytes());
            out.push(self.n as u8);
            out.extend_from_slice(&self.len_xor.to_be_bytes());
            out.extend_from_slice(&self.ts_xor.to_be_bytes());
            out.push(self.mk_xor);
            out.extend_from_slice(&self.xor);
            self.reset();
            return Some(out);
        }
        None
    }
}

struct Parity { base: u16, n: usize, len_xor: u16, ts_xor: u32, mk_xor: u8, xor: Vec<u8> }
impl Parity {
    /// Lê um pacote de paridade vindo da rede (dado não confiável: tudo é conferido).
    fn parse(b: &[u8]) -> Option<Parity> {
        if b.len() < HEAD || b[0] != MAGIC { return None; }
        let n = b[3] as usize;
        if n < 2 || n > GROUP_MAX || b.len() - HEAD > MAX_PAYLOAD { return None; }
        Some(Parity { base: u16::from_be_bytes([b[1], b[2]]), n, len_xor: u16::from_be_bytes([b[4], b[5]]), ts_xor: u32::from_be_bytes([b[6], b[7], b[8], b[9]]), mk_xor: b[10], xor: b[HEAD..].to_vec() })
    }
    fn covers(&self, seq: u16) -> bool { (seq.wrapping_sub(self.base) as usize) < self.n }
}

/// Lado do receptor: guarda os últimos pacotes de vídeo e as últimas paridades e refaz o pacote que faltar.
#[derive(Default)]
pub struct FecRx {
    hist: HashMap<u16, Pkt>,
    order: VecDeque<u16>,
    parities: VecDeque<Parity>,
    /// pacotes refeitos pela paridade (telemetria)
    pub recovered: u64,
    /// paridades recebidas (telemetria)
    pub received: u64,
}
impl FecRx {
    pub fn new() -> Self { Self::default() }
    pub fn reset(&mut self) { self.hist.clear(); self.order.clear(); self.parities.clear(); }
    fn remember(&mut self, p: Pkt) {
        if self.hist.insert(p.seq, p.clone()).is_none() { self.order.push_back(p.seq); }
        while self.order.len() > HISTORY { if let Some(old) = self.order.pop_front() { self.hist.remove(&old); } }
    }
    /// Chegou um pacote de vídeo. Devolve os pacotes que ele permitiu refazer (o último que faltava num grupo com paridade).
    pub fn on_media(&mut self, p: Pkt) -> Vec<Pkt> {
        if p.payload.len() > MAX_PAYLOAD { return vec![]; }
        let seq = p.seq;
        self.remember(p);
        if self.parities.iter().any(|q| q.covers(seq)) { self.solve() } else { vec![] }
    }
    /// Chegou um pacote de paridade. Devolve o pacote refeito, se o grupo tinha exatamente um faltando.
    pub fn on_parity(&mut self, bytes: &[u8]) -> Vec<Pkt> {
        let Some(q) = Parity::parse(bytes) else { return vec![] };
        self.received += 1;
        self.parities.push_back(q);
        while self.parities.len() > PARITIES { self.parities.pop_front(); }
        self.solve()
    }
    /// Passa pelas paridades guardadas: grupo completo → paridade dispensada; faltando um → refaz; faltando mais → espera.
    fn solve(&mut self) -> Vec<Pkt> {
        let mut out = vec![];
        let mut i = 0;
        while i < self.parities.len() {
            let q = &self.parities[i];
            let missing: Vec<u16> = (0..q.n).map(|k| q.base.wrapping_add(k as u16)).filter(|s| !self.hist.contains_key(s)).collect();
            if missing.len() > 1 { i += 1; continue; }
            let q = self.parities.remove(i).unwrap();
            let Some(&seq) = missing.first() else { continue };
            let (mut len, mut ts, mut mk, mut body) = (q.len_xor, q.ts_xor, q.mk_xor, q.xor);
            for k in 0..q.n {
                let s = q.base.wrapping_add(k as u16);
                if s == seq { continue; }
                let p = &self.hist[&s];
                len ^= p.payload.len() as u16; ts ^= p.ts; mk ^= p.marker as u8;
                // um pacote maior que a paridade não bate com o grupo (paridade de outra época ou forjada): não dá para refazer
                if p.payload.len() > body.len() { len = u16::MAX; break; }
                for (x, b) in body.iter_mut().zip(p.payload.iter()) { *x ^= *b; }
            }
            let len = len as usize;
            if len == 0 || len > body.len() || mk > 1 { continue; }
            body.truncate(len);
            let p = Pkt { seq, ts, marker: mk == 1, payload: Bytes::from(body) };
            self.remember(p.clone());
            self.recovered += 1;
            out.push(p);
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pk(seq: u16, ts: u32, marker: bool, len: usize) -> Pkt {
        Pkt { seq, ts, marker, payload: Bytes::from((0..len).map(|i| (i as u16).wrapping_mul(31).wrapping_add(seq.wrapping_mul(7)) as u8).collect::<Vec<u8>>()) }
    }
    /// Um quadro de `n` pacotes a partir de `seq`: devolve os pacotes e as paridades que o emissor gerou.
    fn frame(tx: &mut FecTx, seq: u16, ts: u32, n: usize) -> (Vec<Pkt>, Vec<Vec<u8>>) {
        let (mut ps, mut par) = (vec![], vec![]);
        for k in 0..n {
            let p = pk(seq.wrapping_add(k as u16), ts, k + 1 == n, if k + 1 == n { 300 + k } else { 1168 });
            if let Some(x) = tx.add(p.seq, p.ts, p.marker, &p.payload, k + 1 == n) { par.push(x); }
            ps.push(p);
        }
        (ps, par)
    }

    /// Qualquer pacote único perdido de um grupo volta idêntico (conteúdo, tamanho, timestamp e marcador), inclusive o último do quadro.
    #[test]
    fn any_single_lost_packet_of_a_group_is_rebuilt_exactly() {
        for lost in 0..7usize {
            let mut tx = FecTx::new();
            let (ps, par) = frame(&mut tx, 65_533, 9000, 7);   // atravessa a volta do nº de sequência
            assert_eq!(par.len(), 1, "um quadro de 7 pacotes fecha um grupo");
            for order in 0..2 {
                let mut rx = FecRx::new();
                let mut got = vec![];
                if order == 0 { got.extend(rx.on_parity(&par[0])); }   // a paridade pode chegar antes ou depois dos pacotes
                for (k, p) in ps.iter().enumerate() { if k != lost { got.extend(rx.on_media(p.clone())); } }
                if order == 1 { got.extend(rx.on_parity(&par[0])); }
                assert_eq!(got, vec![ps[lost].clone()], "pacote {lost}, ordem {order}");
                assert_eq!(rx.recovered, 1);
            }
        }
    }

    /// Duas perdas no mesmo grupo não inventam nada; quando a retransmissão traz uma delas, a paridade refaz a outra.
    #[test]
    fn two_losses_wait_and_one_retransmission_lets_the_parity_finish() {
        let mut tx = FecTx::new();
        let (ps, par) = frame(&mut tx, 100, 1, 8);
        let mut rx = FecRx::new();
        let mut got = vec![];
        for (k, p) in ps.iter().enumerate() { if k != 2 && k != 5 { got.extend(rx.on_media(p.clone())); } }
        got.extend(rx.on_parity(&par[0]));
        assert!(got.is_empty(), "com dois faltando não há o que refazer");
        assert_eq!(rx.on_media(ps[5].clone()), vec![ps[2].clone()], "chegou um pela retransmissão: o outro sai da paridade");
        // sem perda nenhuma: nada a refazer e a paridade é dispensada
        let mut rx = FecRx::new();
        for p in &ps { assert!(rx.on_media(p.clone()).is_empty()); }
        assert!(rx.on_parity(&par[0]).is_empty() && rx.recovered == 0 && rx.parities.is_empty());
    }

    /// Grupos: no máximo 10 pacotes; quadros pequenos se juntam até dar 4; a sobra de um quadro grande segue para o próximo.
    #[test]
    fn groups_close_at_ten_or_at_a_frame_end_with_at_least_four() {
        let mut tx = FecTx::new();
        let (_, par) = frame(&mut tx, 0, 1, 23);   // 10 + 10 + sobra de 3 (não fecha: menos de 4)
        assert_eq!(par.iter().map(|p| p[3]).collect::<Vec<_>>(), vec![10, 10]);
        let (_, par) = frame(&mut tx, 23, 2, 2);    // 3 + 2 = 5, fecha no fim deste quadro
        assert_eq!((par.len(), par[0][3], u16::from_be_bytes([par[0][1], par[0][2]])), (1, 5, 20));
        let (_, a) = frame(&mut tx, 25, 3, 1);
        let (_, b) = frame(&mut tx, 26, 4, 1);
        let (_, c) = frame(&mut tx, 27, 5, 2);       // 1 + 1 + 2 = 4
        assert_eq!((a.len(), b.len(), c.len(), c[0][3]), (0, 0, 1, 4));
        // custo: 60 quadros de 7 pacotes → 1 paridade para cada 7 pacotes
        let mut tx = FecTx::new();
        let n: usize = (0..60u16).map(|f| frame(&mut tx, f * 7, f as u32, 7).1.len()).sum();
        assert_eq!(n, 60);
    }

    /// Lixo e paridades forjadas vindas da rede não derrubam nem produzem pacote absurdo.
    #[test]
    fn garbage_and_forged_parity_are_ignored() {
        let mut rx = FecRx::new();
        for junk in [&b""[..], &[MAGIC][..], &[0u8; 40][..], &[MAGIC, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0][..], &[MAGIC, 0, 0, 200, 0, 0, 0, 0, 0, 0, 0, 1][..]] {
            assert!(rx.on_parity(junk).is_empty());
        }
        assert!(rx.on_parity(&vec![MAGIC; 4000]).is_empty(), "grande demais");
        // paridade forjada dizendo que o pacote que falta tem 60000 bytes
        for s in 0..3u16 { rx.on_media(pk(s, 1, false, 100)); }
        let mut forged = vec![MAGIC, 0, 0, 4, 0xEA, 0x60, 0, 0, 0, 0, 0];
        forged.extend_from_slice(&[7u8; 100]);
        assert!(rx.on_parity(&forged).is_empty() && rx.recovered == 0);
        // paridade menor que os pacotes do grupo
        let mut short = vec![MAGIC, 0, 0, 4, 0, 10, 0, 0, 0, 0, 0];
        short.extend_from_slice(&[7u8; 20]);
        assert!(rx.on_parity(&short).is_empty() && rx.recovered == 0);
    }

    /// Perda aleatória de 3% em 4 Mbps a 30 qps (15 pacotes por quadro): quantos pacotes perdidos a paridade devolve.
    #[test]
    fn recovers_most_isolated_losses_at_three_percent() {
        let (mut tx, mut rx) = (FecTx::new(), FecRx::new());
        let mut rng = 0x1234_5678u32;
        let mut next = || { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; rng };
        let (mut lost, mut sent, mut par_sent) = (0u64, 0u64, 0u64);
        let mut seq = 0u16;
        for f in 0..600u32 {
            let (ps, par) = frame(&mut tx, seq, f * 3000, 15);
            seq = seq.wrapping_add(15);
            for p in ps { sent += 1; if next() % 100 < 3 { lost += 1; } else { rx.on_media(p); } }
            for x in par { par_sent += 1; if next() % 100 >= 3 { rx.on_parity(&x); } }   // a paridade também se perde
        }
        let pct = rx.recovered as f64 / lost as f64 * 100.0;
        eprintln!("3% de perda: {lost} de {sent} pacotes perdidos, {} refeitos pela paridade ({pct:.0}%), {par_sent} paridades ({:.0}% de pacotes a mais)", rx.recovered, par_sent as f64 / sent as f64 * 100.0);
        assert!(pct > 60.0, "refez só {pct:.0}%");
    }
}
