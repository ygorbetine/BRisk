//! Modo híbrido da chamada: a MÍDIA (voz e tela) vai pelo SFU (`sfu.rs`) e o resto — chat, eventos de chat, status, presença,
//! expulsão — continua pela malha P2P só de DataChannel (`mesh.rs`, sem `Audio` e sem transmissão), que já tem identidade ECDSA,
//! outbox e dono da sala. Este módulo junta os dois num único `MeshHandle`, então o IPC e a UI não mudam.
//!
//! Reconciliação de identidades: o participante do SFU se chama pela identidade do token (id do usuário); o da malha, por um peerId
//! aleatório. Cada nó anuncia a sua identidade do SFU DENTRO do `peer.state` da malha (campo `sfuId`), que só é aceito de peer autenticado
//! pela prova de identidade — então o vínculo vale tanto quanto a prova (nome e impressão digital reais vêm da malha).
//! Eventos do SFU saem com o peer_id da malha; os que chegam antes do vínculo esperam por ele.
use std::collections::HashMap;
use std::sync::Arc;

use serde_json::Value;
use tokio::sync::mpsc;

use crate::audio::Audio;
use crate::mesh::{MeshCmd, MeshEvent, MeshHandle};
use crate::sfu::SfuHandle;

#[derive(Default)]
struct Links {
    by_ident: HashMap<String, String>,   // identidade no SFU -> peer_id da malha
    by_peer: HashMap<String, String>,    // peer_id da malha -> identidade no SFU
    fp_peer: HashMap<String, String>,    // impressão digital -> peer_id da malha
}

impl Links {
    /// `key` pode ser peer_id da malha, impressão digital ou identidade do SFU.
    fn ident_of(&self, key: &str) -> Option<String> {
        if self.by_ident.contains_key(key) { return Some(key.to_string()); }
        if let Some(i) = self.by_peer.get(key) { return Some(i.clone()); }
        self.fp_peer.get(key).and_then(|p| self.by_peer.get(p)).cloned()
    }
}

pub async fn spawn(mesh: MeshHandle, sfu: SfuHandle, audio: Option<Arc<Audio>>) -> MeshHandle {
    let (ctx, mut crx) = mpsc::channel::<MeshCmd>(64);
    let (etx, erx) = mpsc::channel::<MeshEvent>(256);
    let (mesh_cmd, mut mesh_ev) = (mesh.cmd, mesh.events);
    let (sfu_cmd, sfu_ev, sfu_id) = (sfu.cmd, sfu.events, sfu.identity.clone());
    let mut sfu_ev = Some(sfu_ev);
    let _stats = sfu.stats;
    let state = |a: &Option<Arc<Audio>>| a.as_ref().map(|a| (a.mic_open(), a.deaf())).unwrap_or((true, false));
    let (mic, deaf) = state(&audio);
    let _ = mesh_cmd.send(MeshCmd::SetHybrid { mic, deaf, sfu_id: sfu_id.clone() }).await;
    tokio::spawn(async move {
        let mut links = Links::default();
        let mut pending_share: HashMap<String, Value> = HashMap::new();   // identidade -> último share.remote sem vínculo ainda
        loop {
            tokio::select! {
                c = crx.recv() => {
                    let Some(c) = c else { break };
                    match c {
                        MeshCmd::Close => break,
                        MeshCmd::BroadcastState => {
                            let _ = sfu_cmd.send(MeshCmd::BroadcastState).await;
                            let (mic, deaf) = state(&audio);
                            let _ = mesh_cmd.send(MeshCmd::SetHybrid { mic, deaf, sfu_id: sfu_id.clone() }).await;
                        }
                        MeshCmd::MutePeer { key, muted } => { let k = links.ident_of(&key).unwrap_or(key); let _ = sfu_cmd.send(MeshCmd::MutePeer { key: k, muted }).await; }
                        MeshCmd::ShareVolume { key, gain } => { let k = links.ident_of(&key).unwrap_or(key); let _ = sfu_cmd.send(MeshCmd::ShareVolume { key: k, gain }).await; }
                        MeshCmd::ShareWatch { key, on, path } => { let k = links.ident_of(&key).unwrap_or(key); let _ = sfu_cmd.send(MeshCmd::ShareWatch { key: k, on, path }).await; }
                        MeshCmd::CamWatch { key, on, path } => { let k = links.ident_of(&key).unwrap_or(key); let _ = sfu_cmd.send(MeshCmd::CamWatch { key: k, on, path }).await; }
                        c @ (MeshCmd::ShareStart { .. } | MeshCmd::ShareStop | MeshCmd::ShareAudio { .. } | MeshCmd::CamStart { .. } | MeshCmd::CamStop) => { let _ = sfu_cmd.send(c).await; }
                        c => { let _ = mesh_cmd.send(c).await; }   // chat, eventos, status/presença, expulsar/permitir
                    }
                }
                e = mesh_ev.recv() => match e {
                    None => break,
                    Some(MeshEvent::Share(v)) if v["ev"] == "sfu.link" => {
                        let (peer, ident) = (v["peer_id"].as_str().unwrap_or("").to_string(), v["identity"].as_str().unwrap_or("").to_string());
                        if peer.is_empty() || ident.is_empty() { continue; }
                        links.by_ident.insert(ident.clone(), peer.clone());
                        links.by_peer.insert(peer.clone(), ident.clone());
                        for key in [ident.clone(), format!("{ident}#cam")] {
                            if let Some(mut s) = pending_share.remove(&key) { s["peer_id"] = Value::String(peer.clone()); let _ = etx.send(MeshEvent::Share(s)).await; }
                        }
                    }
                    Some(MeshEvent::PeerInfo { peer_id, fp, name }) => {
                        links.fp_peer.insert(fp.clone(), peer_id.clone());
                        let _ = etx.send(MeshEvent::PeerInfo { peer_id, fp, name }).await;
                    }
                    Some(MeshEvent::Peer { peer_id, state }) => {
                        if state == "fechado" || state == "falhou" {
                            if let Some(i) = links.by_peer.remove(&peer_id) { links.by_ident.remove(&i); }
                            links.fp_peer.retain(|_, p| *p != peer_id);
                        }
                        let _ = etx.send(MeshEvent::Peer { peer_id, state }).await;
                    }
                    Some(MeshEvent::Kicked { reason }) => {
                        // expulso pelo dono (mensagem assinada na malha): sai também do SFU
                        let _ = etx.send(MeshEvent::Kicked { reason }).await;
                        break;
                    }
                    Some(e) => { let _ = etx.send(e).await; }
                },
                e = async { match sfu_ev.as_mut() { Some(r) => r.recv().await, None => std::future::pending().await } } => match e {
                    None => { sfu_ev = None; let _ = etx.send(MeshEvent::Error("conexão com o servidor de chamadas encerrada".into())).await; }
                    // quem está na sala, nome, fingerprint e mic/ensurdecido vêm da malha autenticada
                    Some(MeshEvent::Peer { .. } | MeshEvent::PeerInfo { .. } | MeshEvent::PeerState { .. }) => {}
                    // a queda do SFU não é expulsão: vira aviso (o chat continua pela malha)
                    Some(MeshEvent::Kicked { reason }) => { let _ = etx.send(MeshEvent::Error(reason)).await; }
                    Some(MeshEvent::Speaking { peer_id, level }) => {
                        if peer_id == "me" { let _ = etx.send(MeshEvent::Speaking { peer_id, level }).await; }
                        else if let Some(p) = links.by_ident.get(&peer_id) { let _ = etx.send(MeshEvent::Speaking { peer_id: p.clone(), level }).await; }
                    }
                    Some(MeshEvent::Share(mut v)) => {
                        if let Some(ident) = v["peer_id"].as_str().map(String::from) {
                            match links.by_ident.get(&ident) {
                                Some(p) => { v["peer_id"] = Value::String(p.clone()); let _ = etx.send(MeshEvent::Share(v)).await; }
                                None => {
                                    if (v["ev"] == "share.remote" || v["ev"] == "cam.remote") && (v["state"] == "on" || v["state"] == "off") {
                                        let key = if v["ev"] == "cam.remote" { format!("{ident}#cam") } else { ident };
                                        pending_share.insert(key, v);
                                    }
                                }
                            }
                        } else { let _ = etx.send(MeshEvent::Share(v)).await; }
                    }
                    Some(e) => { let _ = etx.send(e).await; }
                },
            }
        }
        let _ = sfu_cmd.send(MeshCmd::Close).await;
        let _ = mesh_cmd.send(MeshCmd::Close).await;
    });
    MeshHandle { cmd: ctx, events: erx }
}
