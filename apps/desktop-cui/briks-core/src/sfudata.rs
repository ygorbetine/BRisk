//! Camada de DADOS do modo SFU: chat (DM e eventos assinados), status/presença, estado de microfone, expulsão e mudo remoto pelas
//! data channels do PRÓPRIO LiveKit (publishData reliable), sem malha P2P e portanto sem o teto de 5 peers: serve a salas grandes.
//!
//! Confiança: o servidor LiveKit carimba em cada pacote a identidade do remetente (a do token, emitida pela função `livekit-token`),
//! e ninguém consegue forjá-la. A impressão digital ECDSA vem de um `hello` assinado que carrega a identidade do SFU dentro do texto
//! assinado: só vale se `sfuId == remetente carimbado` e a assinatura bate com a chave pública anunciada (que define a impressão
//! digital). Toda mensagem seguinte (estado, chat, evento, expulsão...) é verificada com a chave desse remetente; comandos de sala
//! (expulsar, permitir, mudar o mudo de outro) só valem se o remetente for o DONO. Mensagens malformadas, grandes, repetidas, fora da
//! janela de tempo, de quem ainda não fez hello ou de banidos são descartadas em silêncio (com um aviso só quando útil).
//!
//! Limite conhecido: expulsar aqui faz todos os clientes honestos ignorarem e silenciarem o banido (e o banido sai sozinho). Remover
//! de verdade do servidor de mídia exige a API de administração do LiveKit (RemoveParticipant), que fica numa função do Supabase.
//!
//! O módulo é só lógica (sem rede): `Layer` recebe pacotes/comandos e devolve `Out` (eventos para a UI e pacotes a enviar).
use std::collections::{HashMap, HashSet};
use std::sync::Arc;
use std::time::{Duration, Instant};

use serde_json::{json, Value};

use crate::identity::{self, ChatWire, Identity, PublicJwk};
use crate::mesh::{MeshCmd, MeshEvent};

/// Teto de um pacote (a data channel do LiveKit fragmenta acima de ~16 KB; abaixo disso é seguro em todos os navegadores/SDKs).
pub const MAX_PACKET: usize = 15_000;
const HELLO_WINDOW_MS: i64 = 10 * 60 * 1000;
const CMD_WINDOW_MS: i64 = 5 * 60 * 1000;
const RATE_MAX: u32 = 400;
const RATE_WIN: Duration = Duration::from_secs(10);
const FALLBACK_AFTER: Duration = Duration::from_secs(12);

pub struct DataCfg {
    pub identity: Arc<Identity>,
    pub name: String,
    /// `secure_room_id(ns, room)` inteiro (liga cada assinatura a ESTA sala)
    pub room_hash: String,
    pub owner_fp: Option<String>,
    pub joined_at: u64,
}

pub enum Out {
    Ev(MeshEvent),
    /// `to` vazio = todos
    Send { to: Vec<String>, msg: Value },
    /// o dono mandou fechar/abrir o meu microfone
    Mic(bool),
    /// silenciar/liberar localmente o áudio de um participante (banido)
    MuteLocal { ident: String, muted: bool },
    /// fui expulso: sair do SFU
    Leave,
    /// mensagem do protocolo de anexos vinda de um participante autenticado (tratada em `sfufiles`)
    File { from: String, fp: String, jwk: PublicJwk, m: Value },
}

struct Peer { fp: String, jwk: PublicJwk, name: String, joined_at: u64, last_state: i64 }
struct Present { name: String, since: Instant, tries: u32, fallback: bool }

pub struct Layer {
    cfg: DataCfg,
    me: String,
    fp: String,
    peers: HashMap<String, Peer>,        // identidade no SFU -> peer autenticado por hello
    present: HashMap<String, Present>,   // quem o SFU diz que está na sala
    banned: HashSet<String>,
    muted_idents: HashMap<String, String>,   // identidade silenciada por banimento -> fp
    outbox: HashMap<String, Vec<ChatWire>>,
    dm_off: HashSet<String>,             // fps cujo chat privado o usuário desconectou (a chamada segue)
    seen_msgs: HashSet<String>,
    sons: crate::sonsnet::Net,
    seen_events: HashSet<String>,
    ev_last_ts: u64,
    profile: (String, String),
    media: (bool, bool),
    state_ts: i64,
    rate: HashMap<String, (Instant, u32)>,
    owner_sent: Option<String>,
    /// identidade (no SFU) de quem entrou com a marca de dono no token, segundo o servidor de chamadas (`None` = ninguém marcado)
    server_owner: Option<String>,
    tick_n: u64,
    /// difusões (estado/evento) a repetir: o canal confiável do SFU pode perder um pacote para um receptor sobrecarregado; os receptores deduplicam
    resend: Vec<(Instant, u8, Value)>,
    /// histórico de DMs (o mesmo armazenamento da malha P2P) e o pedido em andamento por participante
    hist: crate::history::Store,
    hist_reqs: HashMap<String, String>,
    seen_groups: HashSet<String>,
}

/// Parte uma lista de mensagens em lotes que cabem num pacote de dados do LiveKit (o lote de 40 KB da malha não cabe).
fn hist_batches(msgs: Vec<ChatWire>) -> Vec<Vec<ChatWire>> {
    let (mut out, mut cur, mut size) = (vec![], vec![], 0usize);
    for m in msgs {
        let len = serde_json::to_string(&m).map(|s| s.len()).unwrap_or(MAX_PACKET);
        if len + 400 > MAX_PACKET { continue; }   // uma mensagem sozinha maior que o pacote não viaja
        if !cur.is_empty() && (size + len > 11_000 || cur.len() >= crate::history::MAX_CHUNK_MESSAGES) { out.push(std::mem::take(&mut cur)); size = 0; }
        size += len; cur.push(m);
    }
    if !cur.is_empty() { out.push(cur); }
    out
}

fn now_ms() -> i64 { std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() as i64).unwrap_or(0) }

/// JSON canônico assinado: array (sem ambiguidade de separador, mesmo com `|` no nome).
fn canon(parts: Vec<Value>) -> String { serde_json::to_string(&Value::Array(parts)).expect("canônico") }
fn hello_canon(room: &str, sfu: &str, fp: &str, name: &str, joined: u64, ts: i64) -> String { canon(vec![json!("briks.sfu.hello.v1"), json!(room), json!(sfu), json!(fp), json!(name), json!(joined), json!(ts)]) }
fn state_canon(room: &str, sfu: &str, fp: &str, mic: bool, deaf: bool, status: &str, presence: &str, ts: i64) -> String { canon(vec![json!("briks.sfu.state.v1"), json!(room), json!(sfu), json!(fp), json!(mic), json!(deaf), json!(status), json!(presence), json!(ts)]) }
fn kick_canon(room: &str, target: &str, ts: i64, reason: &str) -> String { format!("briks.kick.v1|{room}|{target}|{ts}|{reason}") }
fn allow_canon(room: &str, target: &str, ts: i64) -> String { format!("briks.allow.v1|{room}|{target}|{ts}") }
fn mute_canon(room: &str, target: &str, muted: bool, ts: i64) -> String { canon(vec![json!("briks.mute.v1"), json!(room), json!(target), json!(muted), json!(ts)]) }
fn short(s: &str, n: usize) -> String { s.chars().take(n).collect() }

impl Layer {
    pub fn new(cfg: DataCfg, sfu_id: String) -> Self {
        let fp = cfg.identity.fingerprint();
        let sons = crate::sonsnet::Net::new(&fp, &cfg.room_hash, crate::sons::store_for(&fp));
        Layer { cfg, me: sfu_id, fp, peers: HashMap::new(), present: HashMap::new(), banned: HashSet::new(), muted_idents: HashMap::new(), outbox: HashMap::new(), dm_off: HashSet::new(),
            seen_msgs: HashSet::new(), sons, seen_events: HashSet::new(), ev_last_ts: 0, profile: (String::new(), String::new()), media: (true, false), state_ts: 0,
            rate: HashMap::new(), owner_sent: None, server_owner: None, tick_n: 0, resend: vec![],
            hist: crate::history::Store::open(crate::mesh::hist_dir()), hist_reqs: HashMap::new(), seen_groups: HashSet::new() }
    }

    pub fn set_media(&mut self, m: (bool, bool)) { self.media = m; }

    pub fn is_authed(&self, ident: &str) -> bool { self.peers.contains_key(ident) }

    fn hello(&self) -> Value { self.hello_ack(false) }

    /// `ack` = já autentiquei o destinatário (ele não precisa responder). Sem isso, uma resposta perdida nunca se repararia.
    fn hello_ack(&self, ack: bool) -> Value {
        let ts = now_ms();
        let sig = self.cfg.identity.sign(&hello_canon(&self.cfg.room_hash, &self.me, &self.fp, &self.cfg.name, self.cfg.joined_at, ts));
        json!({ "v": 1, "t": "hello", "sfuId": self.me, "identity": self.cfg.identity.public_jwk(), "name": self.cfg.name, "joinedAt": self.cfg.joined_at, "ts": ts, "sig": sig, "ack": ack })
    }

    fn state_msg(&mut self) -> Value {
        self.state_ts = now_ms().max(self.state_ts + 1);
        let (mic, deaf) = self.media;
        let sig = self.cfg.identity.sign(&state_canon(&self.cfg.room_hash, &self.me, &self.fp, mic, deaf, &self.profile.0, &self.profile.1, self.state_ts));
        json!({ "v": 1, "t": "state", "mic": mic, "deaf": deaf, "status": self.profile.0, "presence": self.profile.1, "ts": self.state_ts, "sig": sig })
    }

    fn greet(&mut self, to: &str) -> Vec<Out> {
        let (h, s) = (self.hello_ack(true), self.state_msg());
        vec![Out::Send { to: vec![to.into()], msg: h }, Out::Send { to: vec![to.into()], msg: s }]
    }

    /// Pede ao participante o que ele tem da nossa DM e eu não (mesmo protocolo da malha: ids conhecidos -> lotes -> fim).
    fn request_history(&mut self, ident: &str, fp: &str) -> Vec<Out> {
        let ch = identity::dm_channel(&self.fp, fp);
        let rid = uuid::Uuid::new_v4().to_string();
        self.hist_reqs.insert(ident.to_string(), rid.clone());
        vec![Out::Send { to: vec![ident.into()], msg: json!({ "v": 1, "t": "hist.req", "channelId": ch, "requestId": rid, "knownIds": self.hist.known_ids(&ch) }) }]
    }

    /// Dados do participante autenticado (para os anexos, que rodam fora desta camada).
    pub fn peer_key(&self, ident: &str) -> Option<(String, PublicJwk)> { self.peers.get(ident).map(|p| (p.fp.clone(), p.jwk.clone())) }
    pub fn ident_for(&self, fp: &str) -> Option<String> { self.ident_of_fp(fp) }
    pub fn authed_idents(&self) -> Vec<(String, String)> { self.peers.iter().map(|(k, p)| (k.clone(), p.fp.clone())).collect() }
    pub fn my_fp(&self) -> &str { &self.fp }
    pub fn identity(&self) -> Arc<Identity> { self.cfg.identity.clone() }

    fn ident_of_fp(&self, fp: &str) -> Option<String> { self.peers.iter().find(|(_, p)| p.fp == fp).map(|(k, _)| k.clone()) }

    /// Dono segundo este nó, nesta ordem: o informado pelo app; quem o servidor de chamadas diz que entrou com a marca de dono no
    /// token (dono fixo: não muda com a ordem de volta depois de uma queda); sem ninguém marcado, o mais antigo (joinedAt declarado
    /// e assinado no hello) entre os autenticados. Vazio = há um dono marcado, mas ele ainda não se autenticou comigo (ninguém
    /// passa por dono nesse intervalo).
    fn owner(&self) -> String {
        if let Some(o) = &self.cfg.owner_fp { return o.clone(); }
        if let Some(id) = &self.server_owner {
            if *id == self.me { return self.fp.clone(); }
            return self.peers.get(id).map(|p| p.fp.clone()).unwrap_or_default();
        }
        let mut best = (self.cfg.joined_at, self.fp.clone());
        for p in self.peers.values() { if (p.joined_at, p.fp.as_str()) < (best.0, best.1.as_str()) { best = (p.joined_at, p.fp.clone()); } }
        best.1
    }

    /// O servidor de chamadas informou quem tem a marca de dono (identidade no SFU), ou que ninguém tem.
    pub fn set_server_owner(&mut self, ident: Option<String>) -> Vec<Out> {
        if self.server_owner == ident { return vec![]; }
        self.server_owner = ident;
        self.owner_ev().into_iter().collect()
    }

    fn owner_ev(&mut self) -> Option<Out> {
        let o = self.owner();
        if o.is_empty() || self.owner_sent.as_deref() == Some(o.as_str()) { return None; }
        self.owner_sent = Some(o.clone());
        let me = o == self.fp;
        Some(Out::Ev(MeshEvent::Owner { fp: o, me }))
    }

    /// O SFU anunciou um participante (entrou ou mudou).
    pub fn present(&mut self, ident: &str, name: &str) {
        if ident == self.me { return; }
        self.present.entry(ident.to_string()).or_insert_with(|| Present { name: name.to_string(), since: Instant::now(), tries: 0, fallback: false });
    }

    /// O SFU anunciou a saída de um participante.
    pub fn left(&mut self, ident: &str) {
        self.present.remove(ident);
        self.peers.remove(ident);
        self.rate.remove(ident);
    }

    /// Chamado a cada ~2 s: reenvia hello a quem ainda não respondeu, reenvia chat sem ack e promove os que nunca fizeram hello.
    pub fn tick(&mut self) -> Vec<Out> {
        self.tick_n += 1;
        let mut out = vec![];
        let todo: Vec<String> = self.present.iter().filter(|(k, p)| !self.peers.contains_key(*k) && (p.tries < 8 || self.tick_n % 10 == 0)).map(|(k, _)| k.clone()).collect();
        for k in todo {
            if let Some(p) = self.present.get_mut(&k) { p.tries += 1; }
            let h = self.hello();
            out.push(Out::Send { to: vec![k], msg: h });
        }
        // participantes que nunca fizeram hello (cliente antigo ou sem identidade): aparecem como "não verificados", fp = identidade do SFU
        let late: Vec<(String, String)> = self.present.iter().filter(|(k, p)| !p.fallback && !self.peers.contains_key(*k) && p.since.elapsed() > FALLBACK_AFTER).map(|(k, p)| (k.clone(), p.name.clone())).collect();
        for (k, name) in late {
            if let Some(p) = self.present.get_mut(&k) { p.fallback = true; }
            out.push(Out::Ev(MeshEvent::Peer { peer_id: k.clone(), state: "autenticado".into() }));
            out.push(Out::Ev(MeshEvent::PeerInfo { peer_id: k.clone(), fp: k.clone(), name: if name.is_empty() { k } else { name } }));
        }
        let fps: Vec<String> = self.outbox.iter().filter(|(_, l)| !l.is_empty()).map(|(k, _)| k.clone()).collect();
        // repete difusões recentes (3 s e 9 s depois); estado repetido é ignorado pelo receptor (ts não cresce) e evento pelo id
        let now = Instant::now();
        self.resend.retain(|(t, stage, _)| now.duration_since(*t) < Duration::from_secs(12) || *stage < 2);
        for (t, stage, msg) in self.resend.iter_mut() {
            let age = now.duration_since(*t);
            if (*stage == 0 && age >= Duration::from_secs(3)) || (*stage == 1 && age >= Duration::from_secs(9)) { *stage += 1; out.push(Out::Send { to: vec![], msg: msg.clone() }); }
        }
        self.resend.retain(|(_, stage, _)| *stage < 2);
        if self.tick_n % 3 == 0 { for fp in fps { out.extend(self.flush(&fp)); } }
        self.rate.retain(|_, (t, _)| t.elapsed() < RATE_WIN);
        let so = self.sons.tick(now_ms() as u64); out.extend(self.sons_out(so));
        out
    }

    /// Converte o que o `sonsnet` mandou: pacotes da camada de dados (`v`/`t`) e eventos para a UI.
    fn sons_out(&self, outs: Vec<crate::sonsnet::NetOut>) -> Vec<Out> {
        outs.into_iter().map(|o| match o {
            crate::sonsnet::NetOut::Ev(e) => Out::Ev(MeshEvent::Sound(e)),
            crate::sonsnet::NetOut::Send { to, kind, mut body } => { body["v"] = json!(1); body["t"] = json!(kind); Out::Send { to, msg: body } }
        }).collect()
    }

    fn flush(&mut self, fp: &str) -> Vec<Out> {
        if self.dm_off.contains(fp) { return vec![]; }
        let Some(ident) = self.ident_of_fp(fp) else { return vec![] };
        let Some(list) = self.outbox.get(fp) else { return vec![] };
        list.iter().filter_map(|m| serde_json::to_value(m).ok()).map(|m| Out::Send { to: vec![ident.clone()], msg: json!({ "v": 1, "t": "chat", "msg": m }) }).collect()
    }

    fn ban(&mut self, fp: &str) -> Vec<Out> {
        self.banned.insert(fp.to_string());
        let mut out = vec![Out::Ev(MeshEvent::Banned { fp: fp.to_string() })];
        let idents: Vec<String> = self.peers.iter().filter(|(_, p)| p.fp == fp).map(|(k, _)| k.clone()).collect();
        for i in idents {
            self.peers.remove(&i);
            self.muted_idents.insert(i.clone(), fp.to_string());
            out.push(Out::MuteLocal { ident: i.clone(), muted: true });
            out.push(Out::Ev(MeshEvent::Peer { peer_id: i, state: "fechado".into() }));
        }
        out
    }

    fn rate_ok(&mut self, from: &str) -> bool {
        let e = self.rate.entry(from.to_string()).or_insert((Instant::now(), 0));
        if e.0.elapsed() > RATE_WIN { *e = (Instant::now(), 0); }
        e.1 += 1;
        e.1 <= RATE_MAX
    }

    /// Pacote recebido; `from` é a identidade carimbada pelo servidor.
    pub fn on_packet(&mut self, from: &str, payload: &[u8]) -> Vec<Out> {
        if from.is_empty() || from == self.me || payload.len() > MAX_PACKET { return vec![]; }
        let Ok(v) = serde_json::from_slice::<Value>(payload) else { self.rate_ok(from); return vec![] };
        let t = v["t"].as_str().unwrap_or("");
        // Pedaços de arquivo não entram no limite de mensagens (um anexo são milhares de pacotes): só valem de quem já fez hello e
        // para uma transferência que o usuário aceitou; o resto é descartado sem custo em `sfufiles`.
        if !(t == "file" && self.peers.contains_key(from)) && !self.rate_ok(from) { return vec![]; }
        if v["v"] != 1 { return vec![]; }
        if t == "hello" { return self.on_hello(from, &v); }
        // tudo abaixo exige hello válido anterior
        let Some(p) = self.peers.get(from) else { return vec![] };
        let (jwk, fp) = (p.jwk.clone(), p.fp.clone());
        let room = self.cfg.room_hash.clone();
        match t {
            "state" => {
                let (Some(mic), Some(deaf), Some(ts), Some(sig)) = (v["mic"].as_bool(), v["deaf"].as_bool(), v["ts"].as_i64(), v["sig"].as_str()) else { return vec![] };
                let (status, presence) = (short(v["status"].as_str().unwrap_or(""), 64), short(v["presence"].as_str().unwrap_or(""), 64));
                if v["status"].as_str().map(|s| s.chars().count() > 64).unwrap_or(false) || v["presence"].as_str().map(|s| s.chars().count() > 64).unwrap_or(false) { return vec![]; }
                if ts <= p.last_state || !identity::verify(&jwk, &state_canon(&room, from, &fp, mic, deaf, &status, &presence, ts), sig) { return vec![]; }
                if let Some(p) = self.peers.get_mut(from) { p.last_state = ts; }
                vec![Out::Ev(MeshEvent::PeerState { peer_id: from.into(), microphone: mic, deafened: deaf, status, presence })]
            }
            "chat" => {
                let Ok(m) = serde_json::from_value::<ChatWire>(v["msg"].clone()) else { return vec![] };
                let expected = identity::dm_channel(&self.fp, &fp);
                if m.author_peer_id != fp || identity::verify_message(&jwk, &m, &expected).is_err() { return vec![Out::Ev(MeshEvent::Error("mensagem rejeitada (assinatura/canal)".into()))]; }
                let mut out = vec![Out::Send { to: vec![from.into()], msg: json!({ "v": 1, "t": "ack", "id": m.id }) }];
                if self.dm_off.remove(&fp) { out.push(Out::Ev(MeshEvent::DmLink { fp: fp.clone(), on: true })); }   // a pessoa escreveu: reconecta
                if self.seen_msgs.len() > 8192 { self.seen_msgs.clear(); }
                self.hist.add(&m);
                if self.seen_msgs.insert(m.id.clone()) { out.push(Out::Ev(MeshEvent::Chat { channel: m.channel_id, from_fp: fp, author: short(&m.author, 64), content: m.content, id: m.id, ts: m.timestamp })); }
                out
            }
            "ack" => {
                let id = v["id"].as_str().unwrap_or("");
                let had = self.outbox.get(&fp).map(|l| l.iter().any(|m| m.id == id)).unwrap_or(false);
                if let Some(l) = self.outbox.get_mut(&fp) { l.retain(|m| m.id != id); }
                if had { vec![Out::Ev(MeshEvent::Ack { id: id.to_string(), from_fp: fp })] } else { vec![] }   // "entregue" na UI, uma vez só
            }
            "typing" if self.dm_off.contains(&fp) => vec![],
            "typing" => vec![Out::Ev(MeshEvent::Typing { from_fp: fp })],
            "room.chat" => {
                // chat da chamada: só de quem fez hello, com limite de frequência; o autor é a identidade provada (nada do que vem no JSON)
                let author = self.peers.get(from).map(|p| p.name.clone()).unwrap_or_default();
                let (Some(id), Some(content)) = (v["id"].as_str().and_then(crate::roomchat::clean_id), v["content"].as_str().and_then(crate::roomchat::clean)) else { return vec![] };
                let now = now_ms() as u64;
                if !crate::roomchat::allow_chat(&self.me, from, now) { return vec![]; }
                if self.seen_msgs.len() > 8192 { self.seen_msgs.clear(); }
                if self.seen_msgs.insert(format!("room:{id}")) { vec![Out::Ev(MeshEvent::RoomChat { peer_id: from.into(), from_fp: fp, author, content, id, ts: now })] } else { vec![] }
            }
            "stream.request" | "stream.reply" => {
                let name = self.peers.get(from).map(|p| p.name.clone()).unwrap_or_default();
                let now = now_ms() as u64;
                if t == "stream.request" {
                    if crate::roomchat::allow_request(&self.me, from, now) { vec![Out::Ev(MeshEvent::StreamRequest { peer_id: from.into(), name })] } else { vec![] }
                } else if crate::roomchat::take_asked(&self.me, from, now) {
                    vec![Out::Ev(MeshEvent::StreamReply { peer_id: from.into(), name, accepted: v["accepted"].as_bool().unwrap_or(false) })]
                } else { vec![] }
            }
            "sound.play" | "sound.want" | "sound.data" | "sound.none" => {
                let o = self.sons.on_msg(&crate::sonsnet::PeerRef { key: from, fp: &fp, jwk: &jwk }, t, &v, now_ms() as u64);
                self.sons_out(o)
            }
            "file" => vec![Out::File { from: from.to_string(), fp, jwk, m: v["m"].clone() }],
            "group" => {
                let Ok(s) = serde_json::from_value::<crate::group_sync::Snapshot>(v["snapshot"].clone()) else { return vec![] };   // formato desconhecido: ignora
                if let Err(why) = crate::group_sync::verify_wire(&s, &fp) { return vec![Out::Ev(MeshEvent::Error(format!("grupo rejeitado: {why}")))]; }
                if self.seen_groups.len() > 1024 { self.seen_groups.clear(); }
                if self.seen_groups.insert(s.signature.clone()) { vec![Out::Ev(MeshEvent::GroupSnapshot { from_fp: fp, snapshot: v["snapshot"].clone() })] } else { vec![] }
            }
            "hist.req" => {
                let ch = identity::dm_channel(&self.fp, &fp);
                if v["channelId"].as_str() != Some(&ch) { return vec![]; }   // só a DM entre nós dois; nunca o histórico de outro canal
                let Some(rid) = v["requestId"].as_str().filter(|r| !r.is_empty() && r.len() <= 64) else { return vec![] };
                let known: HashSet<String> = v["knownIds"].as_array().map(|a| a.iter().filter_map(|x| x.as_str()).take(crate::history::MAX_KNOWN).map(String::from).collect()).unwrap_or_default();
                let mut out: Vec<Out> = hist_batches(self.hist.missing(&ch, &known)).into_iter()
                    .map(|b| Out::Send { to: vec![from.into()], msg: json!({ "v": 1, "t": "hist.chunk", "channelId": ch, "requestId": rid, "messages": b }) }).collect();
                out.push(Out::Send { to: vec![from.into()], msg: json!({ "v": 1, "t": "hist.done", "channelId": ch, "requestId": rid }) });
                out
            }
            "hist.chunk" => {
                if v["requestId"].as_str() != self.hist_reqs.get(from).map(String::as_str) { return vec![]; }   // só o que eu pedi, deste participante
                let ch = identity::dm_channel(&self.fp, &fp);
                if v["channelId"].as_str() != Some(&ch) { return vec![]; }
                let Some(arr) = v["messages"].as_array() else { return vec![] };
                let my_jwk = self.cfg.identity.public_jwk();
                let mut out = vec![];
                for raw in arr.iter().take(crate::history::MAX_CHUNK_MESSAGES) {
                    let Ok(m) = serde_json::from_value::<ChatWire>(raw.clone()) else { continue };
                    if self.hist.has(&m.id) { continue; }
                    // o autor tem de ser um dos dois lados desta DM, e a assinatura tem de ser dele (ninguém forja o que "eu" disse)
                    let mine = m.author_peer_id == self.fp;
                    let key = if mine { &my_jwk } else if m.author_peer_id == fp { &jwk } else { continue };
                    if identity::verify_message(key, &m, &ch).is_err() { continue; }
                    if self.hist.add(&m) { out.push(Out::Ev(MeshEvent::History { channel: m.channel_id, from_fp: m.author_peer_id, author: m.author, content: m.content, id: m.id, ts: m.timestamp, mine })); }
                }
                out
            }
            "hist.done" => vec![],
            "event" => {
                let wire = v["wire"].to_string();
                match crate::chat_events::parse_wire(&wire) {
                    crate::chat_events::Parsed::Ignored(_) => vec![],
                    crate::chat_events::Parsed::Invalid(why) => vec![Out::Ev(MeshEvent::Error(format!("evento rejeitado: {why}")))],
                    crate::chat_events::Parsed::Event(e) => {
                        let ch = e.channel_id.clone();
                        if e.actor_peer_id != fp || crate::chat_events::verify_event(&jwk, &e, &ch, now_ms() as u64).is_err() { return vec![Out::Ev(MeshEvent::Error("evento rejeitado (autor/assinatura)".into()))]; }
                        if self.seen_events.len() > 8192 { self.seen_events.clear(); }
                        if self.seen_events.insert(e.id.clone()) { vec![Out::Ev(MeshEvent::ChatEvent { from_fp: fp, wire: crate::chat_events::to_wire(&e), local: false })] } else { vec![] }
                    }
                }
            }
            "kick" | "allow" | "mute" => self.on_room_cmd(t, &fp, &jwk, &v),
            _ => vec![],
        }
    }

    fn on_hello(&mut self, from: &str, v: &Value) -> Vec<Out> {
        let (Some(sfu), Some(ts), Some(sig)) = (v["sfuId"].as_str(), v["ts"].as_i64(), v["sig"].as_str()) else { return vec![] };
        let Ok(jwk) = serde_json::from_value::<PublicJwk>(v["identity"].clone()) else { return vec![] };
        let name = v["name"].as_str().unwrap_or("");
        let joined = v["joinedAt"].as_u64().unwrap_or(u64::MAX);
        if sfu != from || name.chars().count() > 64 || (now_ms() - ts).abs() > HELLO_WINDOW_MS { return vec![]; }
        let fp = identity::fingerprint(&jwk);
        if !identity::verify(&jwk, &hello_canon(&self.cfg.room_hash, from, &fp, name, joined, ts), sig) { return vec![Out::Ev(MeshEvent::Error("hello rejeitado (assinatura)".into()))]; }
        if self.banned.contains(&fp) {
            self.muted_idents.insert(from.to_string(), fp);
            return vec![Out::MuteLocal { ident: from.to_string(), muted: true }];
        }
        // já autenticado: a 1ª identidade vale até ele sair; se ele ainda não me conhece (resposta minha perdida), responde de novo
        if let Some(same) = self.peers.get(from).map(|p| p.fp == fp) { return if same && v["ack"] != true { self.greet(from) } else { vec![] }; }
        if fp == self.fp { return vec![]; }   // eco da minha própria identidade vindo de outro participante
        self.peers.insert(from.to_string(), Peer { fp: fp.clone(), jwk, name: name.to_string(), joined_at: joined, last_state: 0 });
        self.present.entry(from.to_string()).or_insert_with(|| Present { name: name.to_string(), since: Instant::now(), tries: 0, fallback: false });
        // a UI só guarda o nome de quem já está na grade: o participante entra primeiro, o nome vem logo depois
        let mut out = vec![Out::Ev(MeshEvent::Peer { peer_id: from.into(), state: "autenticado".into() })];
        out.push(Out::Ev(MeshEvent::PeerInfo { peer_id: from.into(), fp: fp.clone(), name: name.to_string() }));
        out.extend(self.owner_ev());
        if v["ack"] != true { out.extend(self.greet(from)); }
        out.extend(self.flush(&fp));
        out.extend(self.request_history(from, &fp));
        out
    }

    fn on_room_cmd(&mut self, t: &str, from_fp: &str, jwk: &PublicJwk, v: &Value) -> Vec<Out> {
        let (Some(target), Some(sig), Some(ts)) = (v["target_fp"].as_str(), v["sig"].as_str(), v["ts"].as_i64()) else { return vec![] };
        if self.owner() != from_fp { return vec![Out::Ev(MeshEvent::Error("comando de sala ignorado: remetente não é o dono".into()))]; }
        if (now_ms() - ts).abs() > CMD_WINDOW_MS { return vec![]; }
        let room = self.cfg.room_hash.clone();
        let reason = short(v["reason"].as_str().unwrap_or(""), 120);
        let muted = v["muted"].as_bool().unwrap_or(true);
        let canon = match t { "kick" => kick_canon(&room, target, ts, &reason), "allow" => allow_canon(&room, target, ts), _ => mute_canon(&room, target, muted, ts) };
        if !identity::verify(jwk, &canon, sig) { return vec![Out::Ev(MeshEvent::Error("assinatura de comando de sala inválida".into()))]; }
        let me = target == self.fp;
        match t {
            "kick" if me => { self.banned.insert(self.fp.clone()); vec![Out::Ev(MeshEvent::Kicked { reason }), Out::Leave] }
            "kick" => self.ban(target),
            "allow" => {
                self.banned.remove(target);
                let idents: Vec<String> = self.muted_idents.iter().filter(|(_, f)| f.as_str() == target).map(|(k, _)| k.clone()).collect();
                idents.into_iter().map(|i| { self.muted_idents.remove(&i); Out::MuteLocal { ident: i, muted: false } }).collect()
            }
            _ if me => vec![Out::Mic(!muted), Out::Ev(MeshEvent::Share(json!({ "ev": "room.muted", "muted": muted, "by": from_fp })))],
            _ => vec![],
        }
    }

    /// Comandos da UI. `media` = (microfone aberto, ensurdecido) atuais. `None` = não é desta camada.
    pub fn on_cmd(&mut self, c: MeshCmd, media: (bool, bool)) -> Option<Vec<Out>> {
        self.media = media;
        match c {
            MeshCmd::BroadcastState => { let s = self.state_msg(); self.resend.push((Instant::now(), 0, s.clone())); Some(vec![Out::Send { to: vec![], msg: s }]) }
            MeshCmd::SetProfile { status, presence } => {
                self.profile = (short(&status, 64), short(&presence, 64));
                let s = self.state_msg();
                self.resend.push((Instant::now(), 0, s.clone()));
                Some(vec![Out::Send { to: vec![], msg: s }])
            }
            MeshCmd::DmDisconnect { fp } => Some(if self.dm_off.insert(fp.clone()) { vec![Out::Ev(MeshEvent::DmLink { fp, on: false })] } else { vec![] }),
            MeshCmd::Chat { .. } | MeshCmd::ChatWithId { .. } | MeshCmd::ChatInvite { .. } => {
                // a UI pode mandar o id da mensagem (para casar com o estado "enviada/entregue" dela); sem id, o core cria um
                let (to_fp, text, id) = match c { MeshCmd::ChatWithId { to_fp, text, id } | MeshCmd::ChatInvite { to_fp, text, id, .. } => (to_fp, text, id), MeshCmd::Chat { to_fp, text } => (to_fp, text, uuid::Uuid::new_v4().to_string()), _ => unreachable!() };
                let m = identity::sign_message(&self.cfg.identity, ChatWire {
                    version: 2, kind: "chat.message".into(), channel_id: identity::dm_channel(&self.fp, &to_fp), id,
                    author_peer_id: self.fp.clone(), author: self.cfg.name.clone(), content: text, timestamp: now_ms() as u64, signature: String::new(), package: None });
                self.hist.add(&m);
                self.outbox.entry(to_fp.clone()).or_default().push(m);
                let mut out = if self.dm_off.remove(&to_fp) { vec![Out::Ev(MeshEvent::DmLink { fp: to_fp.clone(), on: true })] } else { vec![] };   // escrever reconecta
                let flushed = self.flush(&to_fp);
                if flushed.iter().any(|o| matches!(o, Out::Send { .. })) { if let Some(m) = self.outbox.get(&to_fp).and_then(|l| l.last()) { out.push(Out::Ev(MeshEvent::Sent { id: m.id.chars().take(64).collect(), to_fp: to_fp.clone() })); } }   // saiu pelo servidor de chamadas
                out.extend(flushed);
                Some(out)
            }
            MeshCmd::Event { channel, target, action, content, reference, emoji } => {
                match crate::chat_events::create(&self.cfg.identity, &channel, &target, &self.fp, &action, content.as_deref(), reference.as_deref(), emoji.as_deref(), now_ms() as u64, &mut self.ev_last_ts) {
                    Ok(e) => {
                        let wire = crate::chat_events::to_wire(&e);
                        self.seen_events.insert(e.id.clone());
                        let msg = json!({ "v": 1, "t": "event", "wire": wire.clone() });
                        self.resend.push((Instant::now(), 0, msg.clone()));
                        Some(vec![Out::Send { to: vec![], msg }, Out::Ev(MeshEvent::ChatEvent { from_fp: self.fp.clone(), wire, local: true })])
                    }
                    Err(er) => Some(vec![Out::Ev(MeshEvent::Error(format!("evento inválido: {er}")))]),
                }
            }
            MeshCmd::Kick { fp, reason } => {
                if self.owner() != self.fp { return Some(vec![Out::Ev(MeshEvent::Error("só o dono da sala pode expulsar".into()))]); }
                let (ts, reason) = (now_ms(), short(&reason, 120));
                let sig = self.cfg.identity.sign(&kick_canon(&self.cfg.room_hash, &fp, ts, &reason));
                let mut out = vec![Out::Send { to: vec![], msg: json!({ "v": 1, "t": "kick", "target_fp": fp, "reason": reason, "ts": ts, "sig": sig }) }];
                out.extend(self.ban(&fp));
                Some(out)
            }
            MeshCmd::Allow { fp } => {
                if self.owner() != self.fp { return Some(vec![Out::Ev(MeshEvent::Error("só o dono da sala pode permitir".into()))]); }
                let ts = now_ms();
                let sig = self.cfg.identity.sign(&allow_canon(&self.cfg.room_hash, &fp, ts));
                self.banned.remove(&fp);
                let mut out = vec![Out::Send { to: vec![], msg: json!({ "v": 1, "t": "allow", "target_fp": fp, "ts": ts, "sig": sig }) }];
                let idents: Vec<String> = self.muted_idents.iter().filter(|(_, f)| f.as_str() == fp).map(|(k, _)| k.clone()).collect();
                for i in idents { self.muted_idents.remove(&i); out.push(Out::MuteLocal { ident: i, muted: false }); }
                Some(out)
            }
            MeshCmd::SoundPlay { hash } => {
                let id = self.cfg.identity.clone();
                match self.sons.play(&id, &hash, now_ms() as u64) {
                    Ok(o) => Some(self.sons_out(o)),
                    Err(m) => Some(vec![Out::Ev(MeshEvent::Sound(json!({ "ev": "sound.denied", "hash": hash, "message": m })))]),
                }
            }
            MeshCmd::SoundFetch { hash } => {
                let cands: Vec<String> = self.peers.keys().take(3).cloned().collect();
                let o = self.sons.fetch(&hash, cands, now_ms() as u64);
                Some(self.sons_out(o))
            }
            MeshCmd::RoomChat { id, text } => {
                let (Some(id), Some(text)) = (crate::roomchat::clean_id(&id), crate::roomchat::clean(&text)) else { return Some(vec![]) };
                let to: Vec<String> = self.peers.keys().cloned().collect();
                Some(if to.is_empty() { vec![] } else { vec![Out::Send { to, msg: json!({ "v": 1, "t": "room.chat", "id": id, "content": text, "ts": now_ms() }) }] })
            }
            MeshCmd::StreamRequest { key } | MeshCmd::StreamReply { key, .. } if !self.peers.iter().any(|(k, p)| *k == key || p.fp == key) => {
                Some(vec![Out::Ev(MeshEvent::Error("pedido de transmissão: essa pessoa não está na sala".into()))])
            }
            MeshCmd::StreamRequest { key } => {
                let id = self.peers.iter().find(|(k, p)| **k == key || p.fp == key).map(|(k, _)| k.clone()).unwrap_or(key);
                crate::roomchat::mark_asked(&self.me, &id, now_ms() as u64);
                Some(vec![Out::Send { to: vec![id], msg: json!({ "v": 1, "t": "stream.request" }) }])
            }
            MeshCmd::StreamReply { key, accepted } => {
                let id = self.peers.iter().find(|(k, p)| **k == key || p.fp == key).map(|(k, _)| k.clone()).unwrap_or(key);
                Some(vec![Out::Send { to: vec![id], msg: json!({ "v": 1, "t": "stream.reply", "accepted": accepted }) }])
            }
            MeshCmd::Typing => { let to: Vec<String> = self.peers.keys().cloned().collect(); Some(if to.is_empty() { vec![] } else { vec![Out::Send { to, msg: json!({ "v": 1, "t": "typing" }) }] }) }
            MeshCmd::GroupSend { snapshot } => {
                // só quem é membro do grupo recebe (a chamada pode ter gente de fora): nome, membros e chaves não vazam
                let members: HashSet<String> = snapshot["members"].as_array().map(|a| a.iter().filter_map(|m| m["fp"].as_str().map(String::from)).collect()).unwrap_or_default();
                let to: Vec<String> = self.peers.iter().filter(|(_, p)| members.contains(&p.fp)).map(|(k, _)| k.clone()).collect();
                if to.is_empty() { Some(vec![]) } else { Some(vec![Out::Send { to, msg: json!({ "v": 1, "t": "group", "snapshot": snapshot }) }]) }
            }
            #[cfg(test)]
            MeshCmd::HistSeed { msgs } => { for m in msgs { self.hist.add(&m); } Some(vec![]) }
            #[cfg(test)]
            MeshCmd::HistAsk => { let ps: Vec<(String, String)> = self.peers.iter().map(|(k, p)| (k.clone(), p.fp.clone())).collect(); Some(ps.into_iter().flat_map(|(i, f)| self.request_history(&i, &f)).collect()) }
            MeshCmd::RemoteMute { fp, muted } => {
                if self.owner() != self.fp { return Some(vec![Out::Ev(MeshEvent::Error("só o dono da sala pode silenciar outros".into()))]); }
                let Some(ident) = self.ident_of_fp(&fp) else { return Some(vec![Out::Ev(MeshEvent::Error("silenciar: participante desconhecido".into()))]) };
                let ts = now_ms();
                let sig = self.cfg.identity.sign(&mute_canon(&self.cfg.room_hash, &fp, muted, ts));
                Some(vec![Out::Send { to: vec![ident], msg: json!({ "v": 1, "t": "mute", "target_fp": fp, "muted": muted, "ts": ts, "sig": sig }) }])
            }
            _ => None,
        }
    }
}

#[cfg(test)]
mod t {
    use super::*;

    fn layer(name: &str, sfu: &str, owner: Option<String>, joined: u64) -> Layer {
        Layer::new(DataCfg { identity: Arc::new(Identity::generate()), name: name.into(), room_hash: "sala-x".into(), owner_fp: owner, joined_at: joined }, sfu.into())
    }
    fn sends(o: &[Out]) -> Vec<Value> { o.iter().filter_map(|x| if let Out::Send { msg, .. } = x { Some(msg.clone()) } else { None }).collect() }
    fn bytes(v: &Value) -> Vec<u8> { v.to_string().into_bytes() }
    fn evs(o: &[Out]) -> usize { o.iter().filter(|x| matches!(x, Out::Ev(_))).count() }
    /// A e B se conhecem pelo hello.
    fn pair() -> (Layer, Layer) {
        let (mut a, mut b) = (layer("Ana", "ida", None, 1), layer("Beto", "idb", None, 2));
        let ha = a.hello(); let hb = b.hello();
        a.on_packet("idb", &bytes(&hb)); b.on_packet("ida", &bytes(&ha));
        assert!(a.is_authed("idb") && b.is_authed("ida"));
        (a, b)
    }

    #[test]
    fn hello_binds_signed_identity_to_the_stamped_sender() {
        let (mut a, mut b) = (layer("Ana", "ida", None, 1), layer("Beto", "idb", None, 2));
        let hb = b.hello();
        // remetente carimbado diferente do sfuId assinado: forjado (outro participante repetindo o hello de B)
        assert!(a.on_packet("idx", &bytes(&hb)).is_empty() && !a.is_authed("idx"));
        // assinatura adulterada (nome trocado)
        let mut bad = hb.clone(); bad["name"] = json!("Outro");
        a.on_packet("idb", &bytes(&bad));
        assert!(!a.is_authed("idb"));
        // chave trocada por outra (a assinatura não bate com ela)
        let mut bad = hb.clone(); bad["identity"] = json!(Identity::generate().public_jwk());
        a.on_packet("idb", &bytes(&bad)); assert!(!a.is_authed("idb"));
        // antigo demais
        let mut old = b.hello(); old["ts"] = json!(now_ms() - 3_600_000); a.on_packet("idb", &bytes(&old)); assert!(!a.is_authed("idb"));
        // o certo
        let out = a.on_packet("idb", &bytes(&hb));
        assert!(a.is_authed("idb"));
        assert!(out.iter().any(|o| matches!(o, Out::Ev(MeshEvent::PeerInfo { peer_id, fp, name }) if peer_id == "idb" && *fp == b.fp && name == "Beto")));
        assert_eq!(sends(&out).iter().filter(|m| m["t"] == "hello").count(), 1, "responde com o próprio hello");
        // a 1ª identidade vale: outra chave com o mesmo sfuId não a substitui
        let mut intruder = layer("Falso", "idb", None, 1);
        assert!(a.on_packet("idb", &bytes(&intruder.hello())).is_empty());
        assert_eq!(a.peers["idb"].fp, b.fp);
        let _ = &mut intruder;
    }

    #[test]
    fn lost_reply_is_repaired_by_the_next_hello() {
        let (mut a, mut b) = (layer("Ana", "ida", None, 1), layer("Beto", "idb", None, 2));
        // B chega a A, mas a resposta de A (greet) se perde: B nunca autentica A
        let hb = b.hello();
        let _lost = a.on_packet("idb", &bytes(&hb));
        assert!(a.is_authed("idb") && !b.is_authed("ida"));
        // B tenta de novo (tick) com ack=false: A, que já conhece B, responde de novo
        let again = a.on_packet("idb", &bytes(&b.hello()));
        let h = sends(&again).into_iter().find(|m| m["t"] == "hello").expect("A responde de novo");
        assert_eq!(h["ack"], true);
        b.on_packet("ida", &bytes(&h));
        assert!(b.is_authed("ida"));
        // com ack=true ninguém responde (sem ping-pong)
        assert!(a.on_packet("idb", &bytes(&b.hello_ack(true))).is_empty());
        assert!(sends(&b.on_packet("ida", &bytes(&h))).is_empty());
    }

    #[test]
    fn messages_before_hello_or_garbage_are_dropped() {
        let (mut a, mut b) = (layer("Ana", "ida", None, 1), layer("Beto", "idb", None, 2));
        let st = b.state_msg();
        assert!(a.on_packet("idb", &bytes(&st)).is_empty(), "sem hello");
        a.on_packet("idb", &bytes(&b.hello()));
        for junk in [&b"\xff\xfe"[..], b"{", b"[]", b"{\"v\":2,\"t\":\"state\"}", b"{\"v\":1}", b"{\"v\":1,\"t\":\"state\"}", b"{\"v\":1,\"t\":\"chat\",\"msg\":5}", b"{\"v\":1,\"t\":\"kick\"}"] {
            assert_eq!(evs(&a.on_packet("idb", junk)), 0, "{:?}", String::from_utf8_lossy(junk));
        }
        assert!(a.on_packet("idb", &vec![b'x'; MAX_PACKET + 1]).is_empty(), "grande demais");
        assert!(a.on_packet("", &bytes(&st)).is_empty() && a.on_packet("ida", &bytes(&st)).is_empty(), "sem remetente / eco");
    }

    #[test]
    fn sound_play_and_file_transfer_work_over_the_data_layer() {
        let (mut a, mut b) = pair();
        let mk = || { let dir = std::env::temp_dir().join(format!("briks-sfusons-{}", uuid::Uuid::new_v4())); std::fs::create_dir_all(&dir).unwrap(); Arc::new(std::sync::Mutex::new(crate::sons::Store::open(&dir))) };
        let (sa, sb) = (mk(), mk());
        a.sons = crate::sonsnet::Net::new(&a.fp, "sala-x", sa.clone());
        b.sons = crate::sonsnet::Net::new(&b.fp, "sala-x", sb.clone());
        let it = sa.lock().unwrap().import_bytes(&crate::sons::testutil::wav(1200, 0.4), "Buzina", "📯", "me", "").unwrap();
        let mut q: std::collections::VecDeque<(bool, Out)> = a.on_cmd(MeshCmd::SoundPlay { hash: it.hash.clone() }, (true, false)).unwrap().into_iter().map(|o| (true, o)).collect();
        let (mut evs_a, mut evs_b) = (vec![], vec![]);
        while let Some((from_a, o)) = q.pop_front() {
            match o {
                Out::Ev(MeshEvent::Sound(e)) => if from_a { evs_a.push(e) } else { evs_b.push(e) },
                Out::Send { msg, .. } => {
                    let r = if from_a { b.on_packet("ida", &bytes(&msg)) } else { a.on_packet("idb", &bytes(&msg)) };
                    for x in r { q.push_back((!from_a, x)); }
                }
                _ => {}
            }
        }
        assert!(evs_a.iter().any(|e| e["ev"] == "sound.play" && e["self"] == true), "quem dispara ouve: {evs_a:?}");
        assert!(evs_b.iter().any(|e| e["ev"] == "sound.play" && e["self"] == false), "B ouve depois de baixar: {evs_b:?}");
        assert!(sb.lock().unwrap().has(&it.hash), "arquivo verificado por hash chegou a B");
        // intervalo mínimo: o segundo disparo de A em seguida é barrado em A mesmo
        let again = a.on_cmd(MeshCmd::SoundPlay { hash: it.hash.clone() }, (true, false)).unwrap();
        assert!(again.iter().any(|o| matches!(o, Out::Ev(MeshEvent::Sound(e)) if e["ev"] == "sound.denied")));
        // um pacote de som de quem não fez hello é descartado
        let (mut x, mut y) = (layer("Xis", "idx", None, 1), layer("Yps", "idy", None, 2));
        x.sons = crate::sonsnet::Net::new(&x.fp, "sala-x", sa.clone());
        let o = x.on_cmd(MeshCmd::SoundPlay { hash: it.hash.clone() }, (true, false)).unwrap();
        for out in o { if let Out::Send { msg, .. } = out { assert!(y.on_packet("idx", &bytes(&msg)).is_empty()); } }
    }

    #[test]
    fn state_is_signed_monotonic_and_bounded() {
        let (mut a, mut b) = pair();
        b.media = (false, true); b.profile = ("jogando".into(), "Jogo".into());
        let st = b.state_msg();
        let out = a.on_packet("idb", &bytes(&st));
        assert!(matches!(&out[0], Out::Ev(MeshEvent::PeerState { peer_id, microphone: false, deafened: true, status, presence }) if peer_id == "idb" && status == "jogando" && presence == "Jogo"));
        assert!(a.on_packet("idb", &bytes(&st)).is_empty(), "repetição (replay)");
        let mut forged = b.state_msg(); forged["status"] = json!("hackeado");
        assert!(a.on_packet("idb", &bytes(&forged)).is_empty(), "conteúdo adulterado");
        let mut long = b.state_msg(); long["status"] = json!("x".repeat(65));
        assert!(a.on_packet("idb", &bytes(&long)).is_empty());
    }

    #[test]
    fn dm_chat_signed_acked_deduped_and_not_for_third_parties() {
        let (mut a, mut b) = pair();
        let out = a.on_cmd(MeshCmd::Chat { to_fp: b.fp.clone(), text: "oi".into() }, (true, false)).unwrap();
        let m = sends(&out).remove(0);
        let got = b.on_packet("ida", &bytes(&m));
        assert!(got.iter().any(|o| matches!(o, Out::Ev(MeshEvent::Chat { content, from_fp, .. }) if content == "oi" && *from_fp == a.fp)));
        let ack = sends(&got).remove(0);
        assert_eq!(ack["t"], "ack");
        assert!(evs(&b.on_packet("ida", &bytes(&m))) == 0, "duplicada não repete");
        a.on_packet("idb", &bytes(&ack));
        assert!(a.outbox[&b.fp].is_empty(), "ack limpa a outbox");
        // forjada: C tenta passar uma mensagem escrita por A (o servidor carimba C) -> author != fp de C / canal errado
        let mut c = layer("Cris", "idc", None, 3);
        b.on_packet("idc", &bytes(&c.hello())); c.on_packet("idb", &bytes(&b.hello()));
        assert!(b.on_packet("idc", &bytes(&m)).iter().all(|o| !matches!(o, Out::Ev(MeshEvent::Chat { .. }))));
        // conteúdo adulterado
        let mut bad = m.clone(); bad["msg"]["content"] = json!("pague 100");
        assert!(b.on_packet("ida", &bytes(&bad)).iter().all(|o| !matches!(o, Out::Ev(MeshEvent::Chat { .. }))));
    }

    #[test]
    fn dm_disconnect_stops_only_the_private_chat_and_reconnects_when_someone_writes() {
        let (mut a, mut b) = pair();
        // Ana desconecta o chat privado com Beto: avisa a UI; o estado/voz da chamada (state) segue normal
        let out = a.on_cmd(MeshCmd::DmDisconnect { fp: b.fp.clone() }, (true, false)).unwrap();
        assert!(out.iter().any(|o| matches!(o, Out::Ev(MeshEvent::DmLink { fp, on: false }) if *fp == b.fp)));
        assert!(a.on_cmd(MeshCmd::DmDisconnect { fp: b.fp.clone() }, (true, false)).unwrap().is_empty(), "repetir não avisa de novo");
        assert!(a.on_cmd(MeshCmd::BroadcastState, (true, false)).is_some(), "a chamada continua");
        // "digitando" de Beto não chega à UI enquanto desconectado
        assert!(evs(&a.on_packet("idb", &bytes(&json!({ "v": 1, "t": "typing" })))) == 0);
        // Beto escreve: a mensagem chega e o chat reconecta sozinho
        let m = sends(&b.on_cmd(MeshCmd::Chat { to_fp: a.fp.clone(), text: "voltei".into() }, (true, false)).unwrap()).remove(0);
        let got = a.on_packet("idb", &bytes(&m));
        assert!(got.iter().any(|o| matches!(o, Out::Ev(MeshEvent::DmLink { on: true, .. }))));
        assert!(got.iter().any(|o| matches!(o, Out::Ev(MeshEvent::Chat { content, .. }) if content == "voltei")));
        // desconecta de novo: o que Ana escreve agora fica na outbox (não sai) até ela escrever, o que reconecta e envia
        a.on_cmd(MeshCmd::DmDisconnect { fp: b.fp.clone() }, (true, false)).unwrap();
        assert!(a.flush(&b.fp).is_empty(), "desconectado: nada sai");
        let out = a.on_cmd(MeshCmd::Chat { to_fp: b.fp.clone(), text: "oi de novo".into() }, (true, false)).unwrap();
        assert!(out.iter().any(|o| matches!(o, Out::Ev(MeshEvent::DmLink { on: true, .. }))));
        assert_eq!(sends(&out).iter().filter(|m| m["t"] == "chat").count(), 1, "escrever reconecta e envia");
    }

    #[test]
    fn dm_to_unknown_peer_waits_in_outbox_until_hello() {
        let (mut a, mut b) = (layer("Ana", "ida", None, 1), layer("Beto", "idb", None, 2));
        assert!(sends(&a.on_cmd(MeshCmd::Chat { to_fp: b.fp.clone(), text: "antes".into() }, (true, false)).unwrap()).is_empty());
        let out = a.on_packet("idb", &bytes(&b.hello()));
        assert_eq!(sends(&out).iter().filter(|m| m["t"] == "chat").count(), 1, "sai no hello");
        let _ = &mut b;
    }

    #[test]
    fn chat_events_are_verified() {
        let (mut a, mut b) = pair();
        let out = a.on_cmd(MeshCmd::Event { channel: "dm-x".into(), target: "msg-0001-aaaa".into(), action: "reaction.add".into(), content: None, reference: None, emoji: Some("👍".into()) }, (true, false)).unwrap();
        let m = sends(&out).remove(0);
        assert!(b.on_packet("ida", &bytes(&m)).iter().any(|o| matches!(o, Out::Ev(MeshEvent::ChatEvent { local: false, .. }))));
        let mut bad = m.clone(); bad["wire"]["emoji"] = json!("💀");
        assert!(b.on_packet("ida", &bytes(&bad)).iter().all(|o| !matches!(o, Out::Ev(MeshEvent::ChatEvent { .. }))));
    }

    /// Dono fixo: quem o servidor de chamadas marcou é o dono, mesmo não sendo o mais antigo; enquanto o marcado não se autenticou
    /// ninguém passa por dono; sem marcado, volta a valer o mais antigo; o dono informado pelo app vence os dois.
    #[test]
    fn server_marked_owner_wins_over_the_oldest() {
        let mut a = layer("Ana", "ida", None, 1);   // a mais antiga
        let (mut b, mut c) = (layer("Beto", "idb", None, 2), layer("Cris", "idc", None, 3));
        let (ha, hb, hc) = (a.hello(), b.hello(), c.hello());
        // a Cris ainda não se autenticou com ninguém
        a.on_packet("idb", &bytes(&hb)); b.on_packet("ida", &bytes(&ha));
        assert_eq!(a.owner(), a.fp); assert_eq!(b.owner(), a.fp);
        // o servidor diz: a marcada é a Cris (idc). Ela ainda não fez hello: ninguém é dono, e a Ana não consegue expulsar
        assert!(a.set_server_owner(Some("idc".into())).is_empty(), "não anuncia dono que ainda não se autenticou");
        b.set_server_owner(Some("idc".into()));
        assert_eq!(a.owner(), ""); assert_eq!(b.owner(), "");
        assert!(matches!(&a.on_cmd(MeshCmd::Kick { fp: b.fp.clone(), reason: "x".into() }, (true, false)).unwrap()[0], Out::Ev(MeshEvent::Error(_))), "a mais antiga não é mais a dona");
        // a Cris se autentica: passa a ser a dona para todos, e ela mesma sabe que é
        let o = a.on_packet("idc", &bytes(&hc));
        assert!(o.iter().any(|x| matches!(x, Out::Ev(MeshEvent::Owner { fp, me: false }) if *fp == c.fp)), "dono marcado não foi anunciado");
        b.on_packet("idc", &bytes(&hc));
        c.on_packet("ida", &bytes(&ha)); c.on_packet("idb", &bytes(&hb));
        let o = c.set_server_owner(Some("idc".into()));
        assert!(matches!(&o[0], Out::Ev(MeshEvent::Owner { me: true, .. })));
        assert_eq!((a.owner(), b.owner(), c.owner()), (c.fp.clone(), c.fp.clone(), c.fp.clone()));
        // a expulsão assinada pela Cris vale; a mesma mensagem assinada pela Ana (a mais antiga) não
        let kick = sends(&c.on_cmd(MeshCmd::Kick { fp: b.fp.clone(), reason: "regra".into() }, (true, false)).unwrap()).remove(0);
        let ts = now_ms();
        let forged = json!({ "v": 1, "t": "kick", "target_fp": b.fp, "reason": "x", "ts": ts, "sig": a.cfg.identity.sign(&kick_canon("sala-x", &b.fp, ts, "x")) });
        assert!(b.on_packet("ida", &bytes(&forged)).iter().all(|x| !matches!(x, Out::Leave)), "a mais antiga expulsou sem ser a dona");
        assert!(b.on_packet("idc", &bytes(&kick)).iter().any(|x| matches!(x, Out::Leave)), "a dona marcada não conseguiu expulsar");
        // a marcada sai de vez e o servidor não aponta mais ninguém: volta o mais antigo
        a.left("idc");
        let o = a.set_server_owner(None);
        assert!(matches!(&o[0], Out::Ev(MeshEvent::Owner { me: true, .. })));
        assert_eq!(a.owner(), a.fp);
        // dono informado pelo app (grupo com dono conhecido) vence a marca do servidor
        let mut g = layer("Gil", "idg", Some("fp-do-dono-do-grupo".into()), 9);
        g.set_server_owner(Some("idg".into()));
        assert_eq!(g.owner(), "fp-do-dono-do-grupo");
    }

    #[test]
    fn only_the_owner_can_kick_allow_and_mute() {
        let mut a = layer("Ana", "ida", None, 1);   // dona: a mais antiga
        let (mut b, mut c) = (layer("Beto", "idb", None, 2), layer("Cris", "idc", None, 3));
        let (ha, hb, hc) = (a.hello(), b.hello(), c.hello());
        for (l, hs) in [(&mut a, vec![("idb", &hb), ("idc", &hc)]), (&mut b, vec![("ida", &ha), ("idc", &hc)]), (&mut c, vec![("ida", &ha), ("idb", &hb)])] { for (id, h) in hs { l.on_packet(id, &bytes(h)); } }
        assert_eq!(a.owner(), a.fp); assert_eq!(b.owner(), a.fp);
        // C (não dono) tenta expulsar B: o comando local é recusado e um pacote forjado é ignorado
        assert!(matches!(&c.on_cmd(MeshCmd::Kick { fp: b.fp.clone(), reason: "x".into() }, (true, false)).unwrap()[0], Out::Ev(MeshEvent::Error(_))));
        let ts = now_ms();
        let forged = json!({ "v": 1, "t": "kick", "target_fp": b.fp, "reason": "x", "ts": ts, "sig": c.cfg.identity.sign(&kick_canon("sala-x", &b.fp, ts, "x")) });
        let o = b.on_packet("idc", &bytes(&forged));
        assert!(o.iter().all(|x| !matches!(x, Out::Leave | Out::Ev(MeshEvent::Kicked { .. }))), "não-dono não expulsa");
        // dona expulsa B
        let kick = sends(&a.on_cmd(MeshCmd::Kick { fp: b.fp.clone(), reason: "regra".into() }, (true, false)).unwrap()).remove(0);
        let ob = b.on_packet("ida", &bytes(&kick));
        assert!(ob.iter().any(|x| matches!(x, Out::Leave)) && ob.iter().any(|x| matches!(x, Out::Ev(MeshEvent::Kicked { reason }) if reason == "regra")));
        let oc = c.on_packet("ida", &bytes(&kick));
        assert!(oc.iter().any(|x| matches!(x, Out::MuteLocal { ident, muted: true } if ident == "idb")) && oc.iter().any(|x| matches!(x, Out::Ev(MeshEvent::Banned { .. }))));
        // replay antigo e assinatura de outro alvo
        let mut old = kick.clone(); old["ts"] = json!(ts - 3_600_000); assert!(evs(&c.on_packet("ida", &bytes(&old))) <= 1);
        let mut other = kick.clone(); other["target_fp"] = json!(c.fp); assert!(c.on_packet("ida", &bytes(&other)).iter().all(|x| !matches!(x, Out::Leave)), "assinatura é do alvo B");
        // B banido volta com hello: C ignora e mantém silenciado
        let o = c.on_packet("idb", &bytes(&b.hello()));
        assert!(!c.is_authed("idb") && matches!(&o[0], Out::MuteLocal { muted: true, .. }));
        // dona permite: C libera
        let allow = sends(&a.on_cmd(MeshCmd::Allow { fp: b.fp.clone() }, (true, false)).unwrap()).remove(0);
        assert!(c.on_packet("ida", &bytes(&allow)).iter().any(|x| matches!(x, Out::MuteLocal { ident, muted: false } if ident == "idb")));
        c.on_packet("idb", &bytes(&b.hello())); assert!(c.is_authed("idb"));
        // mudo remoto: só a dona; alvo C fecha o microfone
        let mute = sends(&a.on_cmd(MeshCmd::RemoteMute { fp: c.fp.clone(), muted: true }, (true, false)).unwrap()).remove(0);
        assert!(c.on_packet("ida", &bytes(&mute)).iter().any(|x| matches!(x, Out::Mic(false))));
        assert!(b.on_packet("ida", &bytes(&mute)).iter().all(|x| !matches!(x, Out::Mic(_))), "outro alvo não muda");
        let ts = now_ms();
        let fake = json!({ "v": 1, "t": "mute", "target_fp": c.fp, "muted": true, "ts": ts, "sig": b.cfg.identity.sign(&mute_canon("sala-x", &c.fp, true, ts)) });
        assert!(c.on_packet("idb", &bytes(&fake)).iter().all(|x| !matches!(x, Out::Mic(_))), "não-dono não silencia");
    }

    #[test]
    fn rate_limit_drops_floods() {
        let (mut a, mut b) = pair();
        let st = b.state_msg();
        let mut n = 0;
        for _ in 0..(RATE_MAX + 50) { if !a.on_packet("idb", &bytes(&st)).is_empty() { n += 1; } }
        assert!(n <= 1);
        assert!(a.on_packet("idb", &bytes(&b.state_msg())).is_empty(), "inundação: descartado até a janela passar");
    }

    #[test]
    fn unverified_participants_appear_after_a_while() {
        let mut a = layer("Ana", "ida", None, 1);
        a.present("idz", "Zé");
        assert!(sends(&a.tick()).iter().any(|m| m["t"] == "hello"), "pede o hello");
        a.present.get_mut("idz").unwrap().since = Instant::now() - Duration::from_secs(20);
        let o = a.tick();
        assert!(o.iter().any(|x| matches!(x, Out::Ev(MeshEvent::PeerInfo { peer_id, fp, .. }) if peer_id == "idz" && fp == "idz")));
    }
}
