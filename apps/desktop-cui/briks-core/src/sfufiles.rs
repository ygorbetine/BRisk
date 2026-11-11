//! Anexos no modo SFU: o mesmo protocolo da malha P2P (`file.offer` assinada, `file.accept`, `file.chunk` em ordem com SHA-256 no fim,
//! `file.pause`/`file.resume`/`file.cancel`; ver `filetransfer.rs` e `mesh.rs`), levado pela camada de dados do LiveKit em vez de uma
//! DataChannel direta. Diferenças impostas pelo transporte: pedaços de 8 KB (um pacote de dados do LiveKit leva até ~15 KB) e teto de
//! 100 MB por arquivo (tudo passa pelo servidor de chamadas; na malha o teto é 20 GiB porque vai direto de um par ao outro).
//! Nada é gravado em disco antes de o usuário aceitar; nome saneado, tamanho e hash conferidos como na malha.
use std::collections::{HashMap, HashSet};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering::SeqCst};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use base64::Engine;
use serde_json::{json, Value};
use tokio::sync::mpsc;

use crate::filetransfer as ft;
use crate::identity::{Identity, PublicJwk};
use crate::mesh::MeshEvent;

pub const SFU_MAX_FILE_BYTES: u64 = 100 * 1024 * 1024;
pub const SFU_CHUNK_BYTES: usize = 8 * 1024;

fn now_ms() -> i64 { std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() as i64).unwrap_or(0) }
fn file_ev(v: Value) -> MeshEvent { MeshEvent::File(v) }

#[derive(Default)]
struct Ctl {
    /// quem envia cancelou: para todos os destinatários
    cancel: AtomicBool,
    paused_all: AtomicBool,
    accepted: Mutex<HashSet<String>>,
    paused: Mutex<HashSet<String>>,
    gone: Mutex<HashSet<String>>,
    /// destinatários (fp) que cancelaram/recusaram: só o envio para ELES para; os outros seguem recebendo
    refused: Mutex<HashSet<String>>,
}

/// Pacote de arquivo a publicar: (identidade de destino no SFU, mensagem do protocolo de anexos).
pub type FileOut = (String, Value);

pub struct Files {
    identity: Arc<Identity>,
    recv_dir: PathBuf,
    ev: mpsc::Sender<MeshEvent>,
    out: mpsc::Sender<FileOut>,
    incoming: HashMap<String, (String, ft::Incoming, u64)>,   // id -> (identidade de quem envia, estado, último progresso emitido)
    pending: HashMap<String, (String, String, ft::Offer)>,     // id -> (identidade, fp, oferta ainda não aceita)
    outgoing: HashMap<String, Arc<Ctl>>,
    /// Mensagens de controle (aceitar, pausar, cancelar) a publicar já. NÃO passam pela fila dos pedaços: ela é pequena e só o ator
    /// a esvazia; se ele mesmo esperasse vaga nela (enviando um arquivo e, ao mesmo tempo, aceitando ou pausando outro), travaria.
    urgent: Vec<FileOut>,
}

impl Files {
    pub fn new(identity: Arc<Identity>, ev: mpsc::Sender<MeshEvent>, out: mpsc::Sender<FileOut>) -> Self {
        Files { identity, recv_dir: crate::mesh::recv_dir(), ev, out, incoming: HashMap::new(), pending: HashMap::new(), outgoing: HashMap::new(), urgent: vec![] }
    }
    async fn emit(&self, v: Value) { let _ = self.ev.send(file_ev(v)).await; }
    async fn send(&mut self, to: &str, m: Value) { self.urgent.push((to.to_string(), m)); }
    /// O ator chama depois de cada entrada neste módulo e publica o que vier.
    pub fn take_urgent(&mut self) -> Vec<FileOut> { std::mem::take(&mut self.urgent) }

    /// `targets` = (identidade no SFU, impressão digital) de quem deve receber (todos os autenticados ou só um).
    pub async fn send_file(&mut self, path: String, targets: Vec<(String, String)>) {
        let p2 = PathBuf::from(&path);
        let desc = tokio::task::spawn_blocking(move || ft::describe_max(&p2, SFU_MAX_FILE_BYTES)).await;
        let (name, size, sha, mime) = match desc {
            Ok(Ok(d)) => d,
            Ok(Err(e)) => { self.emit(json!({ "kind": "failed", "id": "", "name": path, "error": e.to_string() })).await; return; }
            Err(_) => return,
        };
        if targets.is_empty() { self.emit(json!({ "kind": "failed", "id": "", "name": name, "error": "ninguém conectado para receber" })).await; return; }
        let id = uuid::Uuid::new_v4().to_string();
        let offer = ft::Offer::sign(&self.identity, ft::Offer { id: id.clone(), name: name.clone(), size, mime: mime.clone(), sha256: sha, from_peer: self.identity.fingerprint(), timestamp: now_ms(), signature: String::new() });
        let ctl = Arc::new(Ctl::default());
        self.outgoing.insert(id.clone(), ctl.clone());
        self.emit(json!({ "kind": "offer", "id": id, "name": name, "size": size, "mime": mime, "mine": true, "path": path })).await;
        let wire = offer.to_wire();
        for (ident, fp) in targets {
            let (wire, ctl, ev, out, path, id) = (wire.clone(), ctl.clone(), self.ev.clone(), self.out.clone(), path.clone(), id.clone());
            tokio::spawn(async move {
                let fail = |why: &str| file_ev(json!({ "kind": "failed", "id": id, "peer_fp": fp, "error": why }));
                let cancel_msg = json!({ "version": 1, "type": "file.cancel", "id": id });
                if out.send((ident.clone(), wire)).await.is_err() { let _ = ev.send(fail("falha ao enviar a oferta")).await; return; }
                // espera o "Baixar" do outro lado (até 15 min); cancelar ou a pessoa sair antes disso encerra
                let waited = std::time::Instant::now();
                loop {
                    if ctl.cancel.load(SeqCst) { let _ = out.send((ident.clone(), cancel_msg.clone())).await; let _ = ev.send(file_ev(json!({ "kind": "cancelled", "id": id, "peer_fp": fp }))).await; return; }
                    if ctl.gone.lock().unwrap().contains(&fp) { let _ = ev.send(fail("a pessoa saiu da chamada")).await; return; }
                    if ctl.refused.lock().unwrap().contains(&fp) { let _ = ev.send(file_ev(json!({ "kind": "cancelled", "id": id, "peer_fp": fp, "by_peer": true }))).await; return; }
                    if ctl.accepted.lock().unwrap().contains(&fp) { break; }
                    if waited.elapsed() > Duration::from_secs(15 * 60) { let _ = ev.send(fail("ninguém aceitou a tempo")).await; return; }
                    tokio::time::sleep(Duration::from_millis(50)).await;
                }
                let Ok(mut f) = tokio::fs::File::open(&path).await else { let _ = ev.send(fail("não consegui abrir o arquivo")).await; return };
                let mut buf = vec![0u8; SFU_CHUNK_BYTES];
                let (mut seq, mut sent, mut last) = (0u64, 0u64, 0u64);
                loop {
                    while (ctl.paused_all.load(SeqCst) || ctl.paused.lock().unwrap().contains(&fp)) && !ctl.cancel.load(SeqCst) && !ctl.gone.lock().unwrap().contains(&fp) && !ctl.refused.lock().unwrap().contains(&fp) { tokio::time::sleep(Duration::from_millis(50)).await; }
                    if ctl.cancel.load(SeqCst) { let _ = out.send((ident.clone(), cancel_msg.clone())).await; let _ = ev.send(file_ev(json!({ "kind": "cancelled", "id": id, "peer_fp": fp }))).await; return; }
                    if ctl.gone.lock().unwrap().contains(&fp) { let _ = ev.send(fail("a pessoa saiu da chamada")).await; return; }
                    if ctl.refused.lock().unwrap().contains(&fp) { let _ = ev.send(file_ev(json!({ "kind": "cancelled", "id": id, "peer_fp": fp, "by_peer": true }))).await; return; }
                    let mut n = 0;
                    while n < buf.len() {
                        match tokio::io::AsyncReadExt::read(&mut f, &mut buf[n..]).await { Ok(0) => break, Ok(k) => n += k, Err(_) => { let _ = ev.send(fail("erro de leitura")).await; return; } }
                    }
                    if n == 0 { break; }
                    // a fila é pequena e o ator só a esvazia quando o canal de dados tem folga: é o freio do envio
                    let msg = json!({ "version": 1, "type": "file.chunk", "id": id, "seq": seq, "data": base64::engine::general_purpose::STANDARD.encode(&buf[..n]) });
                    if out.send((ident.clone(), msg)).await.is_err() { let _ = ev.send(fail("conexão perdida")).await; return; }
                    seq += 1; sent += n as u64;
                    if sent - last >= 256 * 1024 || sent == size { last = sent; let _ = ev.send(file_ev(json!({ "kind": "progress", "id": id, "peer_fp": fp, "done": sent, "size": size, "mine": true }))).await; }
                }
                let _ = out.send((ident.clone(), json!({ "version": 1, "type": "file.done", "id": id }))).await;
                let _ = ev.send(file_ev(json!({ "kind": "done", "id": id, "peer_fp": fp, "size": size, "mine": true }))).await;
            });
        }
    }

    pub async fn accept(&mut self, id: String) {
        let Some((peer, fp, off)) = self.pending.remove(&id) else { return };
        match ft::Incoming::start(&self.recv_dir, off.clone()) {
            Ok(inc) => {
                self.incoming.insert(id.clone(), (peer.clone(), inc, 0));
                self.send(&peer, json!({ "version": 1, "type": "file.accept", "id": id })).await;
                self.emit(json!({ "kind": "accepted", "id": id, "peer_fp": fp, "name": off.name, "size": off.size, "mine": false })).await;
            }
            Err(e) => {
                self.send(&peer, json!({ "version": 1, "type": "file.cancel", "id": id })).await;
                self.emit(json!({ "kind": "failed", "id": id, "peer_fp": fp, "error": e.to_string() })).await;
            }
        }
    }

    pub async fn pause(&mut self, id: String, paused: bool) {
        let kind = if paused { "paused" } else { "resumed" };
        if let Some(ctl) = self.outgoing.get(&id) { ctl.paused_all.store(paused, SeqCst); self.emit(json!({ "kind": kind, "id": id, "mine": true })).await; }
        if let Some((peer, _, _)) = self.incoming.get(&id) {
            let peer = peer.clone();
            self.send(&peer, json!({ "version": 1, "type": if paused { "file.pause" } else { "file.resume" }, "id": id })).await;
            self.emit(json!({ "kind": kind, "id": id, "mine": false })).await;
        }
    }

    pub async fn cancel(&mut self, id: String) {
        if let Some(f) = self.outgoing.get(&id) { f.cancel.store(true, SeqCst); }
        if let Some((peer, fp, off)) = self.pending.remove(&id) {   // recusar uma oferta ainda não aceita
            self.send(&peer, json!({ "version": 1, "type": "file.cancel", "id": id })).await;
            self.emit(json!({ "kind": "cancelled", "id": id, "name": off.name, "peer_fp": fp })).await;
        }
        if let Some((peer, inc, _)) = self.incoming.remove(&id) {
            let (name, fp) = (inc.offer.name.clone(), inc.offer.from_peer.clone()); inc.abort();
            self.send(&peer, json!({ "version": 1, "type": "file.cancel", "id": id })).await;
            self.emit(json!({ "kind": "cancelled", "id": id, "name": name, "peer_fp": fp })).await;
        }
    }

    /// O participante saiu (ou a sessão caiu): o que vinha dele é descartado com falha visível; o que ia para ele para.
    pub async fn peer_gone(&mut self, ident: &str, fp: &str, why: &str) {
        for ctl in self.outgoing.values() { ctl.gone.lock().unwrap().insert(fp.to_string()); }
        let ids: Vec<String> = self.incoming.iter().filter(|(_, (p, _, _))| p == ident).map(|(k, _)| k.clone()).collect();
        for id in ids { if let Some((_, inc, _)) = self.incoming.remove(&id) { inc.abort(); self.emit(json!({ "kind": "failed", "id": id, "peer_fp": fp, "error": why })).await; } }
        let ids: Vec<String> = self.pending.iter().filter(|(_, (p, _, _))| p == ident).map(|(k, _)| k.clone()).collect();
        for id in ids { self.pending.remove(&id); self.emit(json!({ "kind": "cancelled", "id": id, "peer_fp": fp })).await; }
    }

    /// Mensagem do protocolo de anexos vinda de um participante AUTENTICADO (`peer` = identidade no SFU, `fp`/`jwk` do hello assinado).
    pub async fn on_msg(&mut self, peer: &str, fp: &str, jwk: &PublicJwk, v: &Value) {
        let id = v["id"].as_str().unwrap_or("").to_string();
        if id.is_empty() || id.len() > 64 { return; }
        let cancel = json!({ "version": 1, "type": "file.cancel", "id": id });
        match v["type"].as_str().unwrap_or("") {
            "file.offer" => {
                let offer = match ft::Offer::from_wire(v).and_then(|o| o.verify(jwk, fp, now_ms()).map(|_| o)) {
                    Ok(o) => o,
                    Err(e) => { let _ = self.ev.send(MeshEvent::Error(format!("anexo rejeitado: {e}"))).await; return; }
                };
                if offer.size > SFU_MAX_FILE_BYTES {
                    self.send(peer, cancel).await;
                    let _ = self.ev.send(MeshEvent::Error(format!("anexo recusado: {} MB passa do limite de {} MB das chamadas pelo servidor", offer.size / (1024 * 1024), SFU_MAX_FILE_BYTES / (1024 * 1024)))).await;
                    return;
                }
                if self.incoming.contains_key(&offer.id) || self.incoming.values().filter(|(pe, _, _)| pe == peer).count() >= ft::MAX_INCOMING_PER_PEER
                    || self.pending.contains_key(&offer.id) || self.pending.values().filter(|(pe, _, _)| pe == peer).count() >= 8 {
                    self.send(peer, cancel).await; return;
                }
                // nada é gravado até o usuário clicar em "Baixar" (AcceptFile)
                self.emit(json!({ "kind": "offer", "id": offer.id, "peer_fp": fp, "name": ft::sanitize_name(&offer.name), "size": offer.size, "mime": offer.mime, "mine": false })).await;
                self.pending.insert(offer.id.clone(), (peer.to_string(), fp.to_string(), offer));
            }
            "file.chunk" => {
                let Some(data) = v["data"].as_str().and_then(|d| base64::engine::general_purpose::STANDARD.decode(d).ok()) else { return };
                let Some(seq) = v["seq"].as_u64() else { return };
                let Some((pe, inc, last)) = self.incoming.get_mut(&id) else { return };
                if pe != peer { return; }
                match inc.chunk(seq, &data) {
                    Ok(()) => {
                        if inc.got - *last >= 256 * 1024 || inc.got == inc.offer.size {
                            *last = inc.got;
                            let (got, size) = (inc.got, inc.offer.size);
                            self.emit(json!({ "kind": "progress", "id": id, "peer_fp": fp, "done": got, "size": size, "mine": false })).await;
                        }
                    }
                    Err(e) => {
                        if let Some((_, inc, _)) = self.incoming.remove(&id) { inc.abort(); }
                        self.send(peer, cancel).await;
                        self.emit(json!({ "kind": "failed", "id": id, "peer_fp": fp, "error": e.to_string() })).await;
                    }
                }
            }
            "file.done" => {
                let Some((pe, inc, last)) = self.incoming.remove(&id) else { return };
                if pe != peer { self.incoming.insert(id, (pe, inc, last)); return; }
                let (name, mime, size) = (inc.offer.name.clone(), inc.offer.mime.clone(), inc.offer.size);
                match inc.finish() {
                    Ok(path) => {
                        let thumb = if ft::is_thumbnailable(&mime) { std::fs::read(&path).ok().and_then(|b| crate::linkpreview::thumbnail(&b).ok()) } else { None };
                        let th = thumb.map(|(w, h, b)| json!({ "w": w, "h": h, "rgba": b })).unwrap_or(Value::Null);
                        self.emit(json!({ "kind": "done", "id": id, "peer_fp": fp, "name": ft::sanitize_name(&name), "size": size, "mime": mime, "mine": false, "path": path.to_string_lossy(), "thumb": th })).await;
                    }
                    Err(e) => self.emit(json!({ "kind": "failed", "id": id, "peer_fp": fp, "error": e.to_string() })).await,
                }
            }
            t @ ("file.accept" | "file.pause" | "file.resume") => {
                let Some(ctl) = self.outgoing.get(&id).cloned() else { return };
                if t == "file.accept" { ctl.accepted.lock().unwrap().insert(fp.to_string()); }
                else if t == "file.pause" { ctl.paused.lock().unwrap().insert(fp.to_string()); }
                else { ctl.paused.lock().unwrap().remove(fp); }
                self.emit(json!({ "kind": match t { "file.accept" => "accepted", "file.pause" => "paused", _ => "resumed" }, "id": id, "peer_fp": fp, "mine": true })).await;
            }
            "file.cancel" => {
                if self.pending.get(&id).is_some_and(|(pe, _, _)| pe == peer) { self.pending.remove(&id); self.emit(json!({ "kind": "cancelled", "id": id, "peer_fp": fp })).await; }
                // quem recebia desistiu: para só o envio para ELE (com vários destinatários, os outros continuam recebendo)
                if let Some(f) = self.outgoing.get(&id) { f.refused.lock().unwrap().insert(fp.to_string()); }
                if let Some((pe, inc, last)) = self.incoming.remove(&id) {
                    if pe == peer { inc.abort(); self.emit(json!({ "kind": "cancelled", "id": id, "peer_fp": fp })).await; }
                    else { self.incoming.insert(id, (pe, inc, last)); }
                }
            }
            _ => {}
        }
    }
}
