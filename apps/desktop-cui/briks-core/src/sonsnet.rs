//! Painel de sons pela rede: disparo (`sound.play`), pedido e transferência do arquivo por hash (`sound.want`/`sound.data`/`sound.none`).
//!
//! É só lógica (sem rede), igual para a malha P2P (`mesh.rs`) e para as data channels do SFU (`sfudata.rs`): quem chama entrega a
//! mensagem de um par já autenticado (`PeerRef`: chave de transporte, impressão digital e chave pública provadas) e transmite o que
//! sai em `NetOut::Send` (o transporte põe o seu envelope: `type`/`version` na malha, `t`/`v` no SFU). Nenhum arquivo de som passa
//! por servidor: ele vai de um aparelho a outro, em pedaços de 8 KB, e só entra no disco se o SHA-256 bater com o hash pedido.
//!
//! Disparo: `{hash, nonce, ts, sig}`, assinado pelo remetente com a sala dentro do texto assinado (não vale em outra sala). O receptor
//! confere assinatura, relógio (±30 s), nonce repetido, lista de membros do grupo, silenciados e o intervalo mínimo por pessoa; só
//! depois toca (ou baixa e toca). Cada aparelho toca localmente; o microfone não é remixado e "ensurdecer" não toca (a UI e o core
//! descartam o evento `sound.play` nesse caso).
use std::collections::{HashMap, VecDeque};
use std::sync::{Arc, Mutex};

use base64::{engine::general_purpose::STANDARD as B64, Engine};
use serde_json::{json, Value};

use crate::identity::{self, Identity, PublicJwk};
use crate::sons::{self, Store, MAX_BYTES};

pub const CHUNK: usize = 8 * 1024;
pub const SKEW_MS: i64 = 30_000;
const MAX_CONCURRENT_FETCH: usize = 3;
const FETCH_TIMEOUT_MS: u64 = 8_000;
/// Um som baixado depois disto do disparo já não toca (ficaria fora de hora); só fica no cache.
const PLAY_LATE_MS: u64 = 6_000;
const BAD_MS: u64 = 60_000;
const WANT_GAP_MS: u64 = 5_000;
const SERVE_MAX: usize = 6;
const SERVE_WIN_MS: u64 = 10_000;
const MAX_QUEUE: usize = 64;

pub struct PeerRef<'a> { pub key: &'a str, pub fp: &'a str, pub jwk: &'a PublicJwk }

#[derive(Debug, Clone, PartialEq)]
pub enum NetOut {
    /// `to` vazio = todos os pares autenticados
    Send { to: Vec<String>, kind: &'static str, body: Value },
    Ev(Value),
}

struct Pending { cands: Vec<String>, cur: usize, started: u64, asked_at: u64, play_from: Option<String>, buf: Vec<u8>, size: usize, next_seq: u32 }

pub struct Net {
    me_fp: String,
    room: String,
    store: Arc<Mutex<Store>>,
    seen: HashMap<String, u64>,
    last_play: HashMap<String, u64>,
    pending: HashMap<String, Pending>,
    queue: VecDeque<(String, Vec<String>, Option<String>, u64)>,
    bad: HashMap<String, u64>,
    want_seen: HashMap<String, u64>,
    served: VecDeque<u64>,
}

fn canon(room: &str, hash: &str, nonce: &str, ts: i64) -> String { format!("sound.play|{room}|{hash}|{nonce}|{ts}") }
fn valid_nonce(n: &str) -> bool { (8..=32).contains(&n.len()) && n.bytes().all(|b| b.is_ascii_alphanumeric()) }
fn drop_ev(reason: &str, hash: &str, fp: &str) -> NetOut { NetOut::Ev(json!({ "ev": "sound.drop", "reason": reason, "hash": hash, "from_fp": fp })) }

impl Net {
    pub fn new(me_fp: &str, room_hash: &str, store: Arc<Mutex<Store>>) -> Net {
        Net { me_fp: me_fp.into(), room: room_hash.into(), store, seen: HashMap::new(), last_play: HashMap::new(), pending: HashMap::new(), queue: VecDeque::new(), bad: HashMap::new(), want_seen: HashMap::new(), served: VecDeque::new() }
    }

    /// Eu disparei um som: mensagem assinada para todos + evento local (eu também ouço). Erro = frase para a UI.
    pub fn play(&mut self, id: &Identity, hash: &str, now: u64) -> Result<Vec<NetOut>, String> {
        let mut st = self.store.lock().unwrap();
        if !st.has(hash) { return Err("esse som não está neste aparelho".into()); }
        let cd = st.prefs.cooldown_ms;
        if let Some(&t) = self.last_play.get(&self.me_fp) { if now.saturating_sub(t) < cd { return Err(format!("aguarde {} s entre os sons", (cd + 999) / 1000)); } }
        self.last_play.insert(self.me_fp.clone(), now);
        st.note_use(hash);
        let name = st.items().iter().find(|i| i.hash == hash).map(|i| i.name.clone()).unwrap_or_default();
        drop(st);
        let nonce: String = uuid::Uuid::new_v4().simple().to_string()[..16].to_string();
        let ts = now as i64;
        let sig = id.sign(&canon(&self.room, hash, &nonce, ts));
        Ok(vec![
            NetOut::Send { to: vec![], kind: "sound.play", body: json!({ "hash": hash, "nonce": nonce, "ts": ts, "sig": sig }) },
            NetOut::Ev(json!({ "ev": "sound.play", "hash": hash, "from_fp": self.me_fp, "name": name, "self": true })),
        ])
    }

    /// Pede o arquivo a estes pares (pré-busca do catálogo de um grupo). Fila de baixa prioridade: poucos pedidos por vez.
    pub fn fetch(&mut self, hash: &str, cands: Vec<String>, now: u64) -> Vec<NetOut> {
        if !sons::valid_hash(hash) || cands.is_empty() || self.store.lock().unwrap().has(hash) || self.pending.contains_key(hash) || self.queue.iter().any(|q| q.0 == hash) { return vec![]; }
        if self.bad.get(hash).map(|t| now.saturating_sub(*t) < BAD_MS).unwrap_or(false) { return vec![]; }
        if self.queue.len() >= MAX_QUEUE { return vec![]; }
        self.queue.push_back((hash.into(), cands, None, now));
        self.pump(now)
    }

    fn pump(&mut self, now: u64) -> Vec<NetOut> {
        let mut out = vec![];
        while self.pending.len() < MAX_CONCURRENT_FETCH {
            let Some((hash, cands, play_from, at)) = self.queue.pop_front() else { break };
            out.push(NetOut::Send { to: vec![cands[0].clone()], kind: "sound.want", body: json!({ "hash": hash }) });
            out.push(NetOut::Ev(json!({ "ev": "sound.fetching", "hash": hash })));
            self.pending.insert(hash, Pending { cands, cur: 0, started: at.min(now), asked_at: now, play_from, buf: vec![], size: 0, next_seq: 0 });
        }
        out
    }

    /// Falhou com o candidato atual: tenta o próximo ou desiste.
    fn next_cand(&mut self, hash: &str, now: u64, why: &str) -> Vec<NetOut> {
        let Some(p) = self.pending.get_mut(hash) else { return vec![] };
        p.cur += 1; p.buf.clear(); p.size = 0; p.next_seq = 0; p.asked_at = now;
        if p.cur < p.cands.len() {
            return vec![NetOut::Send { to: vec![p.cands[p.cur].clone()], kind: "sound.want", body: json!({ "hash": hash }) }];
        }
        let fp = p.play_from.clone().unwrap_or_default();
        self.pending.remove(hash);
        self.bad.insert(hash.into(), now);
        let mut out = vec![NetOut::Ev(json!({ "ev": "sound.fail", "hash": hash, "message": why, "from_fp": fp }))];
        out.extend(self.pump(now));
        out
    }

    pub fn tick(&mut self, now: u64) -> Vec<NetOut> {
        let late: Vec<String> = self.pending.iter().filter(|(_, p)| now.saturating_sub(p.asked_at) > FETCH_TIMEOUT_MS).map(|(h, _)| h.clone()).collect();
        let mut out = vec![];
        for h in late { out.extend(self.next_cand(&h, now, "ninguém respondeu com esse som")); }
        if self.seen.len() > 2048 { self.seen.retain(|_, t| now.saturating_sub(*t) < 120_000); }
        if self.want_seen.len() > 2048 { self.want_seen.clear(); }
        if self.bad.len() > 512 { self.bad.clear(); }
        out
    }

    pub fn on_msg(&mut self, p: &PeerRef, kind: &str, v: &Value, now: u64) -> Vec<NetOut> {
        match kind {
            "sound.play" => self.on_play(p, v, now),
            "sound.want" => self.on_want(p, v, now),
            "sound.data" => self.on_data(p, v, now),
            "sound.none" => {
                let Some(h) = v["hash"].as_str() else { return vec![] };
                if self.pending.get(h).and_then(|x| x.cands.get(x.cur)).map(|c| c == p.key).unwrap_or(false) { self.next_cand(h, now, "ninguém tem esse som agora") } else { vec![] }
            }
            _ => vec![],
        }
    }

    fn on_play(&mut self, p: &PeerRef, v: &Value, now: u64) -> Vec<NetOut> {
        let (Some(hash), Some(nonce), Some(ts), Some(sig)) = (v["hash"].as_str().filter(|h| sons::valid_hash(h)), v["nonce"].as_str().filter(|n| valid_nonce(n)), v["ts"].as_i64(), v["sig"].as_str()) else { return vec![] };
        if p.fp == self.me_fp { return vec![]; }
        if !identity::verify(p.jwk, &canon(&self.room, hash, nonce, ts), sig) { return vec![drop_ev("assinatura", hash, p.fp)]; }
        if (now as i64 - ts).abs() > SKEW_MS { return vec![drop_ev("relogio", hash, p.fp)]; }
        let key = format!("{}|{nonce}", p.fp);
        if self.seen.contains_key(&key) { return vec![drop_ev("repetido", hash, p.fp)]; }
        self.seen.insert(key, now);
        let (cooldown, name, have) = {
            let st = self.store.lock().unwrap();
            if let Some(r) = &st.roster { if !r.contains(p.fp) { return vec![drop_ev("nao-membro", hash, p.fp)]; } }
            if st.prefs.all_muted || st.prefs.muted.contains(p.fp) { return vec![drop_ev("silenciado", hash, p.fp)]; }
            (st.prefs.cooldown_ms, st.items().iter().find(|i| i.hash == hash).map(|i| i.name.clone()).unwrap_or_default(), st.has(hash))
        };
        if let Some(&t) = self.last_play.get(p.fp) { if now.saturating_sub(t) < cooldown { return vec![drop_ev("intervalo", hash, p.fp)]; } }
        self.last_play.insert(p.fp.to_string(), now);
        if self.last_play.len() > 512 { self.last_play.retain(|_, t| now.saturating_sub(*t) < 60_000); }
        if have {
            self.store.lock().unwrap().note_use(hash);
            return vec![NetOut::Ev(json!({ "ev": "sound.play", "hash": hash, "from_fp": p.fp, "name": name, "self": false }))];
        }
        if self.bad.get(hash).map(|t| now.saturating_sub(*t) < BAD_MS).unwrap_or(false) { return vec![drop_ev("indisponivel", hash, p.fp)]; }
        if let Some(pd) = self.pending.get_mut(hash) {
            if pd.play_from.is_none() { pd.play_from = Some(p.fp.to_string()); pd.started = now; }
            return vec![];
        }
        if self.pending.len() >= MAX_CONCURRENT_FETCH { return vec![drop_ev("ocupado", hash, p.fp)]; }
        self.queue.push_front((hash.into(), vec![p.key.to_string()], Some(p.fp.to_string()), now));
        self.pump(now)
    }

    fn on_want(&mut self, p: &PeerRef, v: &Value, now: u64) -> Vec<NetOut> {
        let Some(hash) = v["hash"].as_str().filter(|h| sons::valid_hash(h)) else { return vec![] };
        let k = format!("{}|{hash}", p.key);
        if self.want_seen.get(&k).map(|t| now.saturating_sub(*t) < WANT_GAP_MS).unwrap_or(false) { return vec![]; }
        self.want_seen.insert(k, now);
        while self.served.front().map(|t| now.saturating_sub(*t) > SERVE_WIN_MS).unwrap_or(false) { self.served.pop_front(); }
        let bytes = self.store.lock().unwrap().read(hash).filter(|b| !b.is_empty() && b.len() as u64 <= MAX_BYTES);
        let Some(bytes) = bytes else { return vec![NetOut::Send { to: vec![p.key.to_string()], kind: "sound.none", body: json!({ "hash": hash }) }] };
        if self.served.len() >= SERVE_MAX { return vec![NetOut::Send { to: vec![p.key.to_string()], kind: "sound.none", body: json!({ "hash": hash, "busy": true }) }]; }
        self.served.push_back(now);
        let n = bytes.chunks(CHUNK).count();
        bytes.chunks(CHUNK).enumerate().map(|(i, c)| NetOut::Send { to: vec![p.key.to_string()], kind: "sound.data", body: json!({ "hash": hash, "seq": i, "size": bytes.len(), "last": i + 1 == n, "data": B64.encode(c) }) }).collect()
    }

    fn on_data(&mut self, p: &PeerRef, v: &Value, now: u64) -> Vec<NetOut> {
        let (Some(hash), Some(seq), Some(data)) = (v["hash"].as_str(), v["seq"].as_u64(), v["data"].as_str()) else { return vec![] };
        let hash = hash.to_string();
        let Some(pd) = self.pending.get_mut(&hash) else { return vec![] };   // ninguém pediu: descarta
        if pd.cands.get(pd.cur).map(|c| c != p.key).unwrap_or(true) { return vec![]; }
        if seq != pd.next_seq as u64 { return self.next_cand(&hash, now, "arquivo fora de ordem"); }
        let Ok(chunk) = B64.decode(data) else { return self.next_cand(&hash, now, "arquivo corrompido") };
        if seq == 0 {
            let size = v["size"].as_u64().unwrap_or(0);
            if size == 0 || size > MAX_BYTES { return self.next_cand(&hash, now, "arquivo maior que o limite de 512 KB"); }
            pd.size = size as usize;
        }
        if chunk.is_empty() || chunk.len() > CHUNK || pd.buf.len() + chunk.len() > pd.size { return self.next_cand(&hash, now, "tamanho do arquivo não confere"); }
        pd.buf.extend_from_slice(&chunk);
        pd.next_seq += 1;
        pd.asked_at = now;
        if v["last"].as_bool() != Some(true) { return vec![]; }
        if pd.buf.len() != pd.size { return self.next_cand(&hash, now, "arquivo incompleto"); }
        let buf = std::mem::take(&mut pd.buf);
        let res = self.store.lock().unwrap().put_received(&hash, &buf);
        match res {
            Err(e) => self.next_cand(&hash, now, &e.to_string()),
            Ok(_) => {
                let pd = self.pending.remove(&hash).unwrap();
                let mut out = vec![NetOut::Ev(json!({ "ev": "sound.got", "hash": hash }))];
                if let Some(fp) = pd.play_from.filter(|_| now.saturating_sub(pd.started) <= PLAY_LATE_MS) {
                    let name = self.store.lock().unwrap().items().iter().find(|i| i.hash == hash).map(|i| i.name.clone()).unwrap_or_default();
                    self.store.lock().unwrap().note_use(&hash);
                    out.push(NetOut::Ev(json!({ "ev": "sound.play", "hash": hash, "from_fp": fp, "name": name, "self": false, "late_ms": now.saturating_sub(pd.started) })));
                }
                out.extend(self.pump(now));
                out
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::sons::testutil::wav;

    struct Node { id: Identity, fp: String, jwk: PublicJwk, net: Net, store: Arc<Mutex<Store>>, dir: std::path::PathBuf, evs: Vec<Value> }

    fn node(room: &str) -> Node {
        let id = Identity::generate();
        let dir = std::env::temp_dir().join(format!("briks-sonsnet-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir_all(&dir).unwrap();
        let store = Arc::new(Mutex::new(Store::open(&dir)));
        let fp = id.fingerprint();
        Node { jwk: id.public_jwk(), net: Net::new(&fp, room, store.clone()), id, fp, store, dir, evs: vec![] }
    }

    /// Entrega `outs` de `from` ao(s) outro(s) nós (o transporte de mentira), recursivamente, até esvaziar.
    fn pump(nodes: &mut [Node], from: usize, outs: Vec<NetOut>, now: u64) {
        let mut q: VecDeque<(usize, NetOut)> = outs.into_iter().map(|o| (from, o)).collect();
        while let Some((src, o)) = q.pop_front() {
            match o {
                NetOut::Ev(e) => nodes[src].evs.push(e),
                NetOut::Send { to, kind, body } => {
                    // a chave de transporte de cada nó é "n<índice>"
                    let targets: Vec<usize> = if to.is_empty() { (0..nodes.len()).filter(|i| *i != src).collect() } else { to.iter().filter_map(|k| k.trim_start_matches('n').parse().ok()).collect() };
                    for t in targets {
                        let (key, fp, jwk) = (format!("n{src}"), nodes[src].fp.clone(), nodes[src].jwk.clone());
                        let r = nodes[t].net.on_msg(&PeerRef { key: &key, fp: &fp, jwk: &jwk }, kind, &body, now);
                        for x in r { q.push_back((t, x)); }
                    }
                }
            }
        }
    }
    fn fire(ns: &mut Vec<Node>, i: usize, hash: &str, t: u64) -> Result<Vec<NetOut>, String> {
        let id = Identity::from_secret_b64(&ns[i].id.secret_b64()).unwrap();
        ns[i].net.play(&id, hash, t)
    }
    fn msg(ns: &mut Vec<Node>, to: usize, from: usize, kind: &str, body: &Value, t: u64) -> Vec<NetOut> {
        let (fp, jwk, key) = (ns[from].fp.clone(), ns[from].jwk.clone(), format!("n{from}"));
        ns[to].net.on_msg(&PeerRef { key: &key, fp: &fp, jwk: &jwk }, kind, body, t)
    }
    fn plays(n: &Node) -> Vec<&Value> { n.evs.iter().filter(|e| e["ev"] == "sound.play").collect() }
    fn drops(n: &Node) -> Vec<String> { n.evs.iter().filter(|e| e["ev"] == "sound.drop").map(|e| e["reason"].as_str().unwrap().to_string()).collect() }
    fn clean(ns: &[Node]) { for n in ns { let _ = std::fs::remove_dir_all(&n.dir); } }

    #[test]
    fn a_plays_b_and_c_download_by_hash_then_all_hear() {
        let mut ns = vec![node("sala"), node("sala"), node("sala")];
        let it = ns[0].store.lock().unwrap().import_bytes(&wav(1500, 0.4), "Buzina", "📯", "me", "").unwrap();
        let t0 = 1_000_000u64;
        let outs = fire(&mut ns, 0, &it.hash, t0).unwrap();
        pump(&mut ns, 0, outs, t0);
        assert_eq!(plays(&ns[0]).len(), 1, "quem dispara também ouve");
        assert_eq!(ns[0].evs[0]["self"], true);
        for i in 1..3 {
            assert!(ns[i].store.lock().unwrap().has(&it.hash), "o arquivo veio por hash e foi verificado");
            let p = plays(&ns[i]);
            assert_eq!(p.len(), 1, "{:?}", ns[i].evs);
            assert_eq!(p[0]["from_fp"].as_str().unwrap(), ns[0].fp);
            assert!(ns[i].evs.iter().any(|e| e["ev"] == "sound.fetching") && ns[i].evs.iter().any(|e| e["ev"] == "sound.got"));
        }
        // com o arquivo já no cache, o próximo disparo toca direto, sem baixar
        let t1 = t0 + 4_000;
        let outs = fire(&mut ns, 0, &it.hash, t1).unwrap();
        pump(&mut ns, 0, outs, t1);
        assert_eq!(plays(&ns[1]).len(), 2);
        assert_eq!(ns[1].evs.iter().filter(|e| e["ev"] == "sound.fetching").count(), 1);
        clean(&ns);
    }

    #[test]
    fn burst_replay_clock_signature_and_room_are_rejected() {
        let mut ns = vec![node("sala"), node("sala"), node("outra-sala")];
        let it = ns[0].store.lock().unwrap().import_bytes(&wav(1000, 0.4), "x", "", "me", "").unwrap();
        let t0 = 5_000_000u64;
        let first = fire(&mut ns, 0, &it.hash, t0).unwrap();
        pump(&mut ns, 0, first.clone(), t0);
        assert_eq!(plays(&ns[1]).len(), 1);
        assert!(drops(&ns[2]).contains(&"assinatura".to_string()), "a assinatura é presa à sala: não vale em outra");
        // o remetente barra a própria rajada
        assert!(fire(&mut ns, 0, &it.hash, t0 + 1000).unwrap_err().contains("aguarde 3 s"));
        // o receptor barra a rajada de um remetente que não se segura (assinatura boa, mas dentro dos 3 s)
        let mut rogue = fire(&mut ns, 0, &it.hash, t0 + 3000).unwrap();   // 3 s depois: válido para o remetente
        let NetOut::Send { body, .. } = rogue.remove(0) else { panic!() };
        let r = msg(&mut ns, 1, 0, "sound.play", &body, t0 + 1500);
        assert_eq!(r.len(), 1, "{r:?}");
        assert!(matches!(&r[0], NetOut::Ev(e) if e["reason"] == "relogio" || e["reason"] == "intervalo"), "{r:?}");
        // repetição da mesma mensagem
        let NetOut::Send { body: b1, .. } = first[0].clone() else { panic!() };
        let r = msg(&mut ns, 1, 0, "sound.play", &b1, t0 + 10_000);
        assert!(matches!(&r[0], NetOut::Ev(e) if e["reason"] == "repetido" || e["reason"] == "relogio"));
        // relógio: mensagem velha
        let old = { let mut b = b1.clone(); b["nonce"] = json!("abcdefgh12345678"); b["ts"] = json!(t0 as i64 - 60_000); b["sig"] = json!(ns[0].id.sign(&canon("sala", &it.hash, "abcdefgh12345678", t0 as i64 - 60_000))); b };
        assert!(matches!(&msg(&mut ns, 1, 0, "sound.play", &old, t0)[0], NetOut::Ev(e) if e["reason"] == "relogio"));
        // assinatura de outro: remetente diferente do que disse ser
        let r = msg(&mut ns, 1, 2, "sound.play", &b1, t0 + 20_000);
        assert!(matches!(&r[0], NetOut::Ev(e) if e["reason"] == "assinatura"));
        // campos faltando ou inválidos: ignorados
        assert!(msg(&mut ns, 1, 0, "sound.play", &json!({ "hash": "zz", "nonce": "x" }), t0).is_empty());
        clean(&ns);
    }

    #[test]
    fn muted_person_all_muted_and_roster_block_playback() {
        let mut ns = vec![node("sala"), node("sala")];
        let it = ns[0].store.lock().unwrap().import_bytes(&wav(1000, 0.4), "x", "", "me", "").unwrap();
        let fp0 = ns[0].fp.clone();
        ns[1].store.lock().unwrap().set_prefs(&json!({ "mute_fp": fp0 }));
        let mut t = 9_000_000u64;
        let o = fire(&mut ns, 0, &it.hash, t).unwrap(); pump(&mut ns, 0, o, t);
        assert!(plays(&ns[1]).is_empty() && drops(&ns[1]) == vec!["silenciado"]);
        assert!(!ns[1].store.lock().unwrap().has(&it.hash), "silenciado nem baixa o arquivo");
        ns[1].store.lock().unwrap().set_prefs(&json!({ "mute_fp": fp0, "muted": false }));
        t += 4000;
        let o = fire(&mut ns, 0, &it.hash, t).unwrap(); pump(&mut ns, 0, o, t);
        assert_eq!(plays(&ns[1]).len(), 1, "desfez o silenciar");
        ns[1].store.lock().unwrap().set_prefs(&json!({ "all_muted": true }));
        t += 4000;
        let o = fire(&mut ns, 0, &it.hash, t).unwrap(); pump(&mut ns, 0, o, t);
        assert_eq!(plays(&ns[1]).len(), 1);
        ns[1].store.lock().unwrap().set_prefs(&json!({ "all_muted": false }));
        ns[1].store.lock().unwrap().roster = Some(["alguem-outro".to_string()].into());
        t += 4000;
        let o = fire(&mut ns, 0, &it.hash, t).unwrap(); pump(&mut ns, 0, o, t);
        assert_eq!(drops(&ns[1]).last().unwrap(), "nao-membro");
        clean(&ns);
    }

    #[test]
    fn corrupt_oversize_unsolicited_and_wrong_hash_files_are_refused() {
        let mut ns = vec![node("sala"), node("sala")];
        let w = wav(1000, 0.4);
        let h = sons::sha256_hex(&w);
        let peer = |n: &Node| -> (String, PublicJwk) { (n.fp.clone(), n.jwk.clone()) };
        let (fp0, jwk0) = peer(&ns[0]);
        let p0 = PeerRef { key: "n0", fp: &fp0, jwk: &jwk0 };
        // dado que ninguém pediu
        let data = |seq: u64, size: usize, last: bool, bytes: &[u8]| json!({ "hash": h, "seq": seq, "size": size, "last": last, "data": B64.encode(bytes) });
        assert!(ns[1].net.on_msg(&p0, "sound.data", &data(0, w.len(), true, &w), 1000).is_empty() && !ns[1].store.lock().unwrap().has(&h), "sem pedido, nada entra");
        // pede de verdade
        let o = ns[1].net.fetch(&h, vec!["n0".into()], 1000);
        assert!(matches!(&o[0], NetOut::Send { kind: "sound.want", .. }));
        // arquivo com conteúdo diferente do hash pedido
        let other = wav(1200, 0.4);
        let mut r = vec![]; for (i, c) in other.chunks(CHUNK).enumerate() { r = ns[1].net.on_msg(&p0, "sound.data", &json!({ "hash": h, "seq": i, "size": other.len(), "last": (i + 1) * CHUNK >= other.len(), "data": B64.encode(c) }), 1100); }
        assert!(matches!(&r[0], NetOut::Ev(e) if e["ev"] == "sound.fail" && e["message"].as_str().unwrap().contains("hash")), "{r:?}");
        assert!(!ns[1].store.lock().unwrap().has(&h) && !ns[1].store.lock().unwrap().has(&sons::sha256_hex(&other)));
        // falhou: não tenta de novo na hora
        assert!(ns[1].net.fetch(&h, vec!["n0".into()], 1200).is_empty());
        // tamanho declarado acima do limite
        let h2 = sons::sha256_hex(b"outro");
        ns[1].net.fetch(&h2, vec!["n0".into()], 2000);
        let big = json!({ "hash": h2, "seq": 0, "size": MAX_BYTES + 1, "last": false, "data": B64.encode(b"x") });
        assert!(matches!(&ns[1].net.on_msg(&p0, "sound.data", &big, 2100)[0], NetOut::Ev(e) if e["message"].as_str().unwrap().contains("512 KB")));
        // pedaço fora de ordem
        let h3 = sons::sha256_hex(b"terceiro");
        ns[1].net.fetch(&h3, vec!["n0".into()], 3000);
        let skip = json!({ "hash": h3, "seq": 1, "size": 10, "last": false, "data": B64.encode(b"x") });
        assert!(matches!(&ns[1].net.on_msg(&p0, "sound.data", &skip, 3100)[0], NetOut::Ev(e) if e["ev"] == "sound.fail"));
        // hash certo mas não é som / é longo demais
        let junk = b"isto nao e audio".to_vec(); let hj = sons::sha256_hex(&junk);
        ns[1].net.fetch(&hj, vec!["n0".into()], 4000);
        assert!(matches!(&ns[1].net.on_msg(&p0, "sound.data", &data_for(&hj, &junk), 4100)[0], NetOut::Ev(e) if e["ev"] == "sound.fail"));
        let long = wav(9000, 0.3); let hl = sons::sha256_hex(&long);
        ns[1].net.fetch(&hl, vec!["n0".into()], 5000);
        let mut last = vec![];
        for (i, c) in long.chunks(CHUNK).enumerate() { last = ns[1].net.on_msg(&p0, "sound.data", &json!({ "hash": hl, "seq": i, "size": long.len(), "last": (i + 1) * CHUNK >= long.len(), "data": B64.encode(c) }), 5100); }
        assert!(last.iter().any(|o| matches!(o, NetOut::Ev(e) if e["ev"] == "sound.fail")), "som de 9 s não entra: {last:?}");
        clean(&ns);
    }
    fn data_for(h: &str, b: &[u8]) -> Value { json!({ "hash": h, "seq": 0, "size": b.len(), "last": true, "data": B64.encode(b) }) }

    #[test]
    fn fetch_falls_to_next_peer_on_none_or_timeout_and_gives_up() {
        let mut ns = vec![node("sala"), node("sala")];
        let w = wav(800, 0.3); let h = sons::sha256_hex(&w);
        let (fp0, jwk0) = (ns[0].fp.clone(), ns[0].jwk.clone());
        let o = ns[1].net.fetch(&h, vec!["nA".into(), "nB".into()], 1000);
        assert!(matches!(&o[0], NetOut::Send { to, .. } if to == &vec!["nA".to_string()]));
        let r = ns[1].net.on_msg(&PeerRef { key: "nA", fp: &fp0, jwk: &jwk0 }, "sound.none", &json!({ "hash": h }), 1100);
        assert!(matches!(&r[0], NetOut::Send { to, kind: "sound.want", .. } if to == &vec!["nB".to_string()]), "passa para o segundo par");
        let r = ns[1].net.tick(1100 + FETCH_TIMEOUT_MS + 1);
        assert!(matches!(&r[0], NetOut::Ev(e) if e["ev"] == "sound.fail"), "sem resposta: desiste");
        clean(&ns);
    }

    #[test]
    fn serving_is_rate_limited_and_answers_none_when_missing() {
        let mut ns = vec![node("sala"), node("sala")];
        let it = ns[0].store.lock().unwrap().import_bytes(&wav(1000, 0.4), "x", "", "me", "").unwrap();
        let (fp1, jwk1) = (ns[1].fp.clone(), ns[1].jwk.clone());
        let p1 = PeerRef { key: "n1", fp: &fp1, jwk: &jwk1 };
        let r = ns[0].net.on_msg(&p1, "sound.want", &json!({ "hash": it.hash }), 1000);
        assert!(r.iter().all(|o| matches!(o, NetOut::Send { kind: "sound.data", .. })) && !r.is_empty());
        assert!(matches!(r.last().unwrap(), NetOut::Send { body, .. } if body["last"] == true));
        assert!(ns[0].net.on_msg(&p1, "sound.want", &json!({ "hash": it.hash }), 1500).is_empty(), "mesmo pedido em menos de 5 s é ignorado");
        let r = ns[0].net.on_msg(&p1, "sound.want", &json!({ "hash": sons::sha256_hex(b"nao tenho") }), 1500);
        assert!(matches!(&r[0], NetOut::Send { kind: "sound.none", .. }));
        assert!(ns[0].net.on_msg(&p1, "sound.want", &json!({ "hash": "curto" }), 1500).is_empty());
        clean(&ns);
    }

    #[test]
    fn at_most_three_downloads_at_once_the_rest_wait_in_queue() {
        let mut ns = vec![node("sala")];
        let hs: Vec<String> = (0..5).map(|i| sons::sha256_hex(format!("som{i}").as_bytes())).collect();
        let mut wants = 0;
        for h in &hs { wants += ns[0].net.fetch(h, vec!["n9".into()], 1000).iter().filter(|o| matches!(o, NetOut::Send { kind: "sound.want", .. })).count(); }
        assert_eq!(wants, 3);
        let r = ns[0].net.on_msg(&PeerRef { key: "n9", fp: "x", jwk: &Identity::generate().public_jwk() }, "sound.none", &json!({ "hash": hs[0] }), 1100);
        assert!(r.iter().any(|o| matches!(o, NetOut::Send { kind: "sound.want", .. })), "vaga liberada puxa o próximo da fila: {r:?}");
        clean(&ns);
    }
}
