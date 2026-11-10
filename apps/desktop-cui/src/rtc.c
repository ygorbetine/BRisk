/* Ponte UI <-> briks-core: comandos e eventos JSON por linha (ver docs/plans/cui-webrtc.md). */
#include "briks_ui.h"
#include "core.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy(char *dst, size_t cap, const char *src) { snprintf(dst, cap, "%s", src ? src : ""); }

static void send_cmd(const char *cmd, const char *extra) {
    char line[1024];
    snprintf(line, sizeof line, "{\"cmd\":\"%s\"%s%s}", cmd, extra && extra[0] ? "," : "", extra ? extra : "");
    core_send(line);
}

void rtc_join_room(app_state_t *s, const char *room) {
    copy(s->room_name, sizeof s->room_name, room);
    s->rtc_want_room = true;
    s->in_room = true;
    copy(s->rtc_state, sizeof s->rtc_state, "conectando");
    s->rtc_msg[0] = 0;
    s->rtc_peer_count = s->rtc_authed_count = 0;
    if (core_state() == CORE_STOPPED) core_start();
}

void rtc_leave_room(app_state_t *s) {
    s->rtc_want_room = false;
    s->in_room = false;
    s->rtc_peer_count = s->rtc_authed_count = 0;
    if (core_state() == CORE_READY) send_cmd("room.leave", NULL);
    copy(s->rtc_state, sizeof s->rtc_state, "");
    s->rtc_owner_me = false; s->kick_open = false;
    memset(s->rtc_peers, 0, sizeof s->rtc_peers);
}

void rtc_kick_peer(app_state_t *s, int slot) {
    if (!s->rtc_owner_me || slot < 0 || slot >= 8 || !s->rtc_peers[slot].fp[0]) return;
    char extra[200], fp[96]; json_escape(s->rtc_peers[slot].fp, fp, sizeof fp);
    snprintf(extra, sizeof extra, "\"fp\":\"%s\"", fp);
    if (core_state() == CORE_READY) send_cmd("peer.kick", extra);
}

void rtc_set_mic(app_state_t *s, bool on) { if (!on) s->me_level = 0; if (core_state() == CORE_READY) send_cmd("mic.set", on ? "\"on\":true" : "\"on\":false"); }
/* silenciar uma pessoa = ganho zero só para ela (por peer_id) */
void rtc_mute_peer(app_state_t *s, int slot, bool muted) {
    if (slot < 0 || slot >= 8 || !s->rtc_peers[slot].id[0]) return;
    s->rtc_peers[slot].muted_by_me = muted;
    char extra[160], id[96]; json_escape(s->rtc_peers[slot].id, id, sizeof id);
    snprintf(extra, sizeof extra, "\"peer\":\"%s\",\"muted\":%s", id, muted ? "true" : "false");
    if (core_state() == CORE_READY) send_cmd("peer.mute", extra);
}
void rtc_set_deaf(app_state_t *s, bool on) { (void)s; if (core_state() == CORE_READY) send_cmd("deafen.set", on ? "\"on\":true" : "\"on\":false"); }

static void set_peer(app_state_t *s, const char *id, const char *state) {
    int n = (int)(sizeof s->rtc_peers / sizeof s->rtc_peers[0]);
    int slot = -1;
    for (int i = 0; i < n; i++) if (!strcmp(s->rtc_peers[i].id, id)) { slot = i; break; }
    if (slot < 0) for (int i = 0; i < n; i++) if (!s->rtc_peers[i].id[0]) { slot = i; break; }
    if (slot < 0) return;
    if (!strcmp(state, "fechado")) { memset(&s->rtc_peers[slot], 0, sizeof s->rtc_peers[slot]); }
    else { copy(s->rtc_peers[slot].id, sizeof s->rtc_peers[slot].id, id); copy(s->rtc_peers[slot].state, sizeof s->rtc_peers[slot].state, state); }
    s->rtc_peer_count = s->rtc_authed_count = 0;
    for (int i = 0; i < n; i++) if (s->rtc_peers[i].id[0]) { s->rtc_peer_count++; if (!strcmp(s->rtc_peers[i].state, "autenticado")) s->rtc_authed_count++; }
}

/* Devolve true se o evento mudou algo visível. */
bool rtc_handle_event(app_state_t *s, const char *line) {
    jval *j = json_parse(line);
    if (!j) return false;
    const char *ev = json_str(j, "ev", "");
    bool changed = true;
    if (!strcmp(ev, "session.ready")) {
        s->rtc_session_ready = true;
        copy(s->rtc_fp, sizeof s->rtc_fp, json_str(j, "fingerprint", ""));
    } else if (!strcmp(ev, "signaling.status")) {
        copy(s->rtc_state, sizeof s->rtc_state, json_str(j, "state", ""));
        copy(s->rtc_msg, sizeof s->rtc_msg, json_str(j, "message", ""));
    } else if (!strcmp(ev, "peer")) {
        set_peer(s, json_str(j, "peer_id", ""), json_str(j, "state", ""));
    } else if (!strcmp(ev, "speaking")) {
        const jval *lv = json_get(j, "level");
        float l = lv && lv->type == J_NUM ? (float)lv->num : 0;
        const char *p = json_str(j, "peer", "");
        if (!strcmp(p, "me")) s->me_level = l;
        else for (int i = 0; i < 8; i++) if (s->rtc_peers[i].id[0] && !strcmp(s->rtc_peers[i].id, p)) s->rtc_peers[i].level = l;
    } else if (!strcmp(ev, "peer.info")) {
        const char *p = json_str(j, "peer_id", "");
        for (int i = 0; i < 8; i++) if (s->rtc_peers[i].id[0] && !strcmp(s->rtc_peers[i].id, p)) {
            copy(s->rtc_peers[i].fp, sizeof s->rtc_peers[i].fp, json_str(j, "fp", ""));
            copy(s->rtc_peers[i].name, sizeof s->rtc_peers[i].name, json_str(j, "name", ""));
        }
    } else if (!strcmp(ev, "room.owner")) {
        const jval *m = json_get(j, "me");
        s->rtc_owner_me = m && m->type == J_BOOL && m->b;
    } else if (!strcmp(ev, "kicked")) {
        /* o core já fechou tudo: volta à tela social com o aviso */
        s->in_room = false; s->rtc_want_room = false; s->rtc_owner_me = false; s->kick_open = false;
        memset(s->rtc_peers, 0, sizeof s->rtc_peers);
        s->rtc_peer_count = s->rtc_authed_count = 0;
        copy(s->rtc_state, sizeof s->rtc_state, "");
        s->kicked_open = true;
        s->screen = SCREEN_SOCIAL;
    } else if (!strcmp(ev, "peer.state")) {
        const char *p = json_str(j, "peer_id", "");
        const jval *m = json_get(j, "microphone"), *d = json_get(j, "deafened");
        for (int i = 0; i < 8; i++) if (s->rtc_peers[i].id[0] && !strcmp(s->rtc_peers[i].id, p)) {
            s->rtc_peers[i].mic_off = m && m->type == J_BOOL && !m->b;
            s->rtc_peers[i].deafened = d && d->type == J_BOOL && d->b;
            copy(s->rtc_peers[i].status, sizeof s->rtc_peers[i].status, json_str(j, "status", ""));
            copy(s->rtc_peers[i].presence, sizeof s->rtc_peers[i].presence, json_str(j, "presence", ""));
        }
    } else if (!strcmp(ev, "error")) {
        copy(s->rtc_msg, sizeof s->rtc_msg, json_str(j, "message", ""));
        if (!strcmp(json_str(j, "where", ""), "session.start") || !strcmp(json_str(j, "where", ""), "room.join")) copy(s->rtc_state, sizeof s->rtc_state, "erro");
    } else if (!strcmp(ev, "core.closed")) {
        copy(s->rtc_state, sizeof s->rtc_state, "erro");
        copy(s->rtc_msg, sizeof s->rtc_msg, "O briks-core encerrou.");
        s->rtc_session_ready = s->rtc_session_sent = false;
    } else changed = account_handle_event(s, line);   /* account.*, friends.*, local.*, import.* */
    json_free(j);
    return changed;
}

void rtc_pump(app_state_t *s) {
    core_status_t st = core_state();
    static int demo_speak = -1;   /* BRIKS_DEMO_SPEAK=0.7: fixa o nível de fala local (captura de tela do halo, sem microfone) */
    if (demo_speak < 0) demo_speak = getenv("BRIKS_DEMO_SPEAK") ? 1 : 0;
    if (demo_speak) s->me_level = (float)atof(getenv("BRIKS_DEMO_SPEAK"));
    static bool demo_peers_done;   /* BRIKS_DEMO_PEERS=1: dois participantes de exemplo e eu como dono (captura do menu/diálogo de expulsar) */
    if (!demo_peers_done && getenv("BRIKS_DEMO_PEERS")) {
        demo_peers_done = true;
        const char *nm[2] = { "Marina Costa", "Rafael Lima" };
        for (int i = 0; i < 2; i++) { snprintf(s->rtc_peers[i].id, sizeof s->rtc_peers[i].id, "demo-%d", i); copy(s->rtc_peers[i].state, sizeof s->rtc_peers[i].state, "autenticado");
            copy(s->rtc_peers[i].name, sizeof s->rtc_peers[i].name, nm[i]); snprintf(s->rtc_peers[i].fp, sizeof s->rtc_peers[i].fp, "fp-demo-%d", i); }
        s->rtc_peers[1].level = 0.6f; if (getenv("BRIKS_DEMO_STATUS")) { copy(s->rtc_peers[0].status, sizeof s->rtc_peers[0].status, "Chego em 10 minutos"); copy(s->rtc_peers[1].presence, sizeof s->rtc_peers[1].presence, "Jogando Valorant"); } s->rtc_owner_me = true; s->in_room = true;
        const char *o = getenv("BRIKS_DEV_OPEN"); if (o && !strcmp(o, "kick")) { s->kick_slot = 0; s->kick_open = true; }
    }
    if (st == CORE_FAILED && s->in_room && strcmp(s->rtc_state, "erro")) {
        copy(s->rtc_state, sizeof s->rtc_state, "erro");
        copy(s->rtc_msg, sizeof s->rtc_msg, core_error());
    }
    if (st == CORE_READY) {
        {   /* status e presença (programa em uso, se ligado) seguem para o core; ele anuncia aos peers autenticados da sala */
            static char sent_st[64], sent_pr[64]; static bool sent_once;
            const char *pr = s->rp_enabled ? s->user_presence : "";
            if (!sent_once || strcmp(sent_st, s->user_status) || strcmp(sent_pr, pr)) {
                sent_once = true; copy(sent_st, sizeof sent_st, s->user_status); copy(sent_pr, sizeof sent_pr, pr);
                char extra[320], a[140], b[140]; json_escape(s->user_status, a, sizeof a); json_escape(pr, b, sizeof b);
                snprintf(extra, sizeof extra, "\"status\":\"%s\",\"presence\":\"%s\"", a, b);
                send_cmd("profile.set", extra);
            }
        }
        if (s->rtc_want_room && !s->rtc_session_sent) {
            char extra[256], nm[160];
            json_escape(s->user_name[0] ? s->user_name : "Você", nm, sizeof nm);
            snprintf(extra, sizeof extra, "\"display_name\":\"%s\"", nm);
            send_cmd("session.start", extra);
            s->rtc_session_sent = true;
        }
        if (s->rtc_want_room && s->rtc_session_ready) {
            char extra[256], rm[160];
            json_escape(s->room_name, rm, sizeof rm);
            snprintf(extra, sizeof extra, "\"namespace\":\"room\",\"room\":\"%s\"", rm);
            send_cmd("room.join", extra);
            s->rtc_want_room = false;
        }
    }
    char line[8192];
    bool any = false;
    while (core_next_event(line, sizeof line)) any |= rtc_handle_event(s, line);
    if (any || (s->in_room && (st == CORE_STARTING))) cui_request_redraw(cui_app_ui(s->app));
}

const char *rtc_status_text(const app_state_t *s, char *buf, size_t cap) {
    if (!s->in_room) { snprintf(buf, cap, "Prévia local · fora da sala"); return buf; }
    const char *st = s->rtc_state[0] ? s->rtc_state : "conectando";
    if (!strcmp(st, "conectado")) {
        if (s->rtc_peer_count) snprintf(buf, cap, "Conectado · %d pessoa%s na sala", s->rtc_peer_count, s->rtc_peer_count == 1 ? "" : "s");
        else snprintf(buf, cap, "Conectado · você está sozinho na sala");
    } else if (!strcmp(st, "erro")) snprintf(buf, cap, "Erro: %s", s->rtc_msg[0] ? s->rtc_msg : "falha na conexão");
    else if (!strcmp(st, "reconectando")) snprintf(buf, cap, "Reconectando…");
    else if (!strcmp(st, "desconectado")) snprintf(buf, cap, "Desconectado");
    else snprintf(buf, cap, "Conectando…");
    return buf;
}
