//! Presença por sala de voz do grupo (GRP-17): quem está em cada sala sem entrar nela.
//!
//! Cada grupo tem um canal de presença próprio no Realtime, cujo nome vem de um hash do id do grupo (só os membros sabem o id).
//! Quem está numa sala de voz do grupo anuncia `{fp, room, live}` nesse canal; quem observa recebe o estado de todos.
//! Privacidade: a presença nunca carrega nome. O nome mostrado vem da LISTA DE MEMBROS que a interface entregou (`members`), casada
//! pela impressão digital; entradas cuja impressão digital não é de um membro são descartadas (não aparecem nem contam), e um nome
//! declarado por terceiros nunca é usado. Limites: 8 pessoas por sala no evento (`more` conta as demais), 16 salas, 64 membros.
use crate::realtime::{self, Realtime, RealtimeCfg, RtEvent};
use crate::signaling::{secure_room_id, topic_name};
use serde_json::{json, Value};
use std::collections::{BTreeMap, HashMap};
use tokio::sync::mpsc;

pub const MAX_SHOWN: usize = 8;
pub const MAX_ROOMS: usize = 16;
pub const MAX_MEMBERS: usize = 64;
pub const MAX_GROUPS: usize = 4;

pub fn topic(group_id: &str) -> String { topic_name("group", &secure_room_id("group", &format!("roster:{group_id}"))) }

pub fn valid_id(s: &str) -> bool { !s.is_empty() && s.len() <= 64 && s.chars().all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_') }

/// Meta anunciado por quem está (ou deixou de estar) numa sala. `room` vazio = não está em sala nenhuma.
pub fn meta(fp: &str, room: &str, live: bool) -> Value { json!({ "fp": fp, "room": room, "live": live }) }

type RoomList = (usize, Vec<(String, String, bool)>);

/// Agrupa por sala quem é membro do grupo. `peers`: meta de cada peer presente no canal (sem o próprio).
/// Devolve sala -> (total de membros, os primeiros MAX_SHOWN como (fp, nome, ao vivo)).
pub fn compute(members: &[(String, String)], peers: &HashMap<String, Value>) -> BTreeMap<String, RoomList> {
    let names: HashMap<&str, &str> = members.iter().take(MAX_MEMBERS).map(|(f, n)| (f.as_str(), n.as_str())).collect();
    let mut by_room: BTreeMap<String, BTreeMap<String, bool>> = BTreeMap::new();   // sala -> fp -> ao vivo (sem duplicar quem tem duas abas)
    for m in peers.values() {
        let (fp, room) = (m["fp"].as_str().unwrap_or(""), m["room"].as_str().unwrap_or(""));
        if room.is_empty() || room.len() > 96 || !names.contains_key(fp) { continue; }   // não é membro: não vaza nem conta
        if by_room.len() >= MAX_ROOMS && !by_room.contains_key(room) { continue; }
        let live = m["live"].as_bool().unwrap_or(false);
        let e = by_room.entry(room.to_string()).or_default();
        let l = e.entry(fp.to_string()).or_insert(false);
        *l |= live;
    }
    by_room.into_iter().map(|(room, set)| {
        let total = set.len();
        let list = set.into_iter().take(MAX_SHOWN).map(|(fp, live)| { let n = names[fp.as_str()].to_string(); (fp, n, live) }).collect();
        (room, (total, list))
    }).collect()
}

fn event(group: &str, room: &str, total: usize, list: &[(String, String, bool)]) -> Value {
    json!({ "ev": "group.roster", "group": group, "room": room, "total": total, "more": total.saturating_sub(list.len()),
            "members": list.iter().map(|(fp, n, l)| json!({ "fp": fp, "name": n, "live": l })).collect::<Vec<_>>() })
}

/// Abre o canal do grupo e emite `group.roster` (uma vez por sala que mudou; sala que esvaziou sai com `members: []`).
pub fn spawn(cfg: RealtimeCfg, group_id: String, my_fp: String, members: Vec<(String, String)>, tx: mpsc::Sender<Value>) -> Realtime {
    let me = uuid::Uuid::new_v4().to_string();
    let (rt, mut rx) = realtime::connect(cfg, topic(&group_id), me, meta(&my_fp, "", false));
    tokio::spawn(async move {
        let mut peers: HashMap<String, Value> = HashMap::new();
        let mut last: BTreeMap<String, RoomList> = BTreeMap::new();
        while let Some(e) = rx.recv().await {
            match e {
                RtEvent::PeerJoined { peer_id, meta } => { peers.insert(peer_id, meta); }
                RtEvent::PeerLeft(p) => { peers.remove(&p); }
                RtEvent::Status(s) if s == "disconnected" || s.starts_with("error") => { peers.clear(); }
                _ => continue,
            }
            let now = compute(&members, &peers);
            for (room, v) in &now { if last.get(room) != Some(v) && tx.send(event(&group_id, room, v.0, &v.1)).await.is_err() { return; } }
            for room in last.keys() { if !now.contains_key(room) && tx.send(event(&group_id, room, 0, &[])).await.is_err() { return; } }
            last = now;
        }
    });
    rt
}

#[cfg(test)]
mod t {
    use super::*;
    fn peers(v: &[(&str, Value)]) -> HashMap<String, Value> { v.iter().map(|(k, m)| (k.to_string(), m.clone())).collect() }
    fn mem() -> Vec<(String, String)> { vec![("fa".into(), "Ana".into()), ("fb".into(), "Beto".into())] }

    #[test]
    fn only_group_members_appear_with_the_groups_own_names() {
        let p = peers(&[("1", meta("fa", "g-sala1", true)), ("2", meta("fb", "g-sala1", false)), ("3", meta("intruso", "g-sala1", false)), ("4", meta("fb", "", false))]);
        let r = compute(&mem(), &p);
        assert_eq!(r.len(), 1, "peer fora da lista e quem não está em sala não geram nada");
        let (total, list) = &r["g-sala1"];
        assert_eq!(*total, 2);
        assert_eq!(list[0], ("fa".into(), "Ana".into(), true));
        assert_eq!(list[1].1, "Beto");
    }

    #[test]
    fn a_name_claimed_in_presence_is_ignored_and_duplicates_collapse() {
        let mut m = meta("fa", "g-x", false);
        m["name"] = "Falso".into();
        let r = compute(&mem(), &peers(&[("1", m), ("2", meta("fa", "g-x", true))]));
        assert_eq!(r["g-x"].0, 1);
        assert_eq!(r["g-x"].1[0].1, "Ana");
        assert!(r["g-x"].1[0].2, "ao vivo em qualquer aba conta");
    }

    #[test]
    fn rooms_and_people_are_capped() {
        let many: Vec<(String, String)> = (0..40).map(|i| (format!("f{i}"), format!("P{i}"))).collect();
        let p: HashMap<String, Value> = (0..40).map(|i| (i.to_string(), meta(&format!("f{i}"), "g-big", false))).collect();
        let r = compute(&many, &p);
        assert_eq!(r["g-big"].0, 40);
        assert_eq!(r["g-big"].1.len(), MAX_SHOWN);
        assert_eq!(event("g", "g-big", 40, &r["g-big"].1)["more"], 32);
        let p2: HashMap<String, Value> = (0..30).map(|i| (i.to_string(), meta("f1", &format!("r{i}"), false))).collect();
        assert!(compute(&many, &p2).len() <= MAX_ROOMS);
        assert!(valid_id("abc-123_x") && !valid_id("") && !valid_id("a b") && !valid_id(&"x".repeat(65)));
    }
}
