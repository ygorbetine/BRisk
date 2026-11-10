/* Ponte UI <-> briks-core: comandos e eventos JSON por linha (ver docs/plans/cui-webrtc.md). */
#include "briks_ui.h"
#include "core.h"
#include "json.h"
#include <stdio.h>
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
}

void rtc_set_mic(app_state_t *s, bool on) { (void)s; if (core_state() == CORE_READY) send_cmd("mic.set", on ? "\"on\":true" : "\"on\":false"); }
void rtc_set_deaf(app_state_t *s, bool on) { (void)s; if (core_state() == CORE_READY) send_cmd("deafen.set", on ? "\"on\":true" : "\"on\":false"); }

static void set_peer(app_state_t *s, const char *id, const char *state) {
    int n = (int)(sizeof s->rtc_peers / sizeof s->rtc_peers[0]);
    int slot = -1;
    for (int i = 0; i < n; i++) if (!strcmp(s->rtc_peers[i].id, id)) { slot = i; break; }
    if (slot < 0) for (int i = 0; i < n; i++) if (!s->rtc_peers[i].id[0]) { slot = i; break; }
    if (slot < 0) return;
    if (!strcmp(state, "fechado")) { s->rtc_peers[slot].id[0] = 0; s->rtc_peers[slot].state[0] = 0; }
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
    } else if (!strcmp(ev, "error")) {
        copy(s->rtc_msg, sizeof s->rtc_msg, json_str(j, "message", ""));
        if (!strcmp(json_str(j, "where", ""), "session.start") || !strcmp(json_str(j, "where", ""), "room.join")) copy(s->rtc_state, sizeof s->rtc_state, "erro");
    } else if (!strcmp(ev, "core.closed")) {
        copy(s->rtc_state, sizeof s->rtc_state, "erro");
        copy(s->rtc_msg, sizeof s->rtc_msg, "O briks-core encerrou.");
        s->rtc_session_ready = s->rtc_session_sent = false;
    } else changed = false;
    json_free(j);
    return changed;
}

void rtc_pump(app_state_t *s) {
    core_status_t st = core_state();
    if (st == CORE_FAILED && s->in_room && strcmp(s->rtc_state, "erro")) {
        copy(s->rtc_state, sizeof s->rtc_state, "erro");
        copy(s->rtc_msg, sizeof s->rtc_msg, core_error());
    }
    if (st == CORE_READY) {
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
