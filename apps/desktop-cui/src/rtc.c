/* Ponte UI <-> briks-core: comandos e eventos JSON por linha (ver docs/plans/cui-webrtc.md). */
#include "briks_ui.h"
#include "call_idle.h"
#include "core.h"
#include "json.h"
#include "msgui.h"
#include "attach.h"
#include "sysint.h"
#include "call.h"
#include <math.h>
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool settings_echo(void);   /* settings.c: cancelamento de eco ligado? */

static void copy(char *dst, size_t cap, const char *src) { snprintf(dst, cap, "%s", src ? src : ""); }

static void over_clear(app_state_t *s);

static void send_cmd(const char *cmd, const char *extra) {
    char line[1024];
    snprintf(line, sizeof line, "{\"cmd\":\"%s\"%s%s}", cmd, extra && extra[0] ? "," : "", extra ? extra : "");
    core_send(line);
}

void rtc_join_room(app_state_t *s, const char *room) {
    copy(s->room_name, sizeof s->room_name, room);
    s->rtc_want_room = true;
    s->in_room = true;
    fw_maybe_prompt();   /* primeira chamada: pergunta (sem bloquear) se pode liberar o Briks no Firewall */
    copy(s->rtc_state, sizeof s->rtc_state, "conectando");
    s->rtc_msg[0] = 0;
    s->rtc_peer_count = s->rtc_authed_count = 0;
    if (core_state() == CORE_STOPPED) core_start();
}

void rtc_leave_room(app_state_t *s) {
    s->rtc_want_room = false;
    s->in_room = false; s->dmoff_n = 0;   /* a malha acaba junto com a sala: chats privados fechados voltam ao normal */
    s->mic_state[0] = 0; s->mic_text[0] = 0; s->dev_notice[0] = 0;   /* avisos de áudio são da chamada que acabou */
    s->rtc_peer_count = s->rtc_authed_count = 0;
    if (core_state() == CORE_READY) send_cmd("room.leave", NULL);
    copy(s->rtc_state, sizeof s->rtc_state, "");
    s->rtc_owner_me = false; s->kick_open = false; s->net_sfu = false; s->net_failed = false; s->force_p2p = false;
    over_clear(s);
    memset(s->rtc_peers, 0, sizeof s->rtc_peers);
    share_reset(s);
    cam_reset();
    call_reset(s);
}

/* chat da sala e pedido de transmissão: só para quem está autenticado na sala (o core confere) */
void rtc_room_chat(const char *id, const char *text) {
    if (core_state() != CORE_READY) return;
    char line[2400], t[1700], i[96]; json_escape(text, t, sizeof t); json_escape(id, i, sizeof i);
    snprintf(line, sizeof line, "{\"cmd\":\"room.chat.send\",\"id\":\"%s\",\"text\":\"%s\"}", i, t);
    core_send(line);
}
void rtc_stream_request(app_state_t *s, int slot) {
    if (slot < 0 || slot >= BRIKS_MAX_PEERS || !s->rtc_peers[slot].id[0]) return;
    char extra[160], id[96]; json_escape(s->rtc_peers[slot].id, id, sizeof id);
    snprintf(extra, sizeof extra, "\"peer\":\"%s\"", id);
    if (core_state() == CORE_READY) send_cmd("stream.request", extra);
}
void rtc_stream_reply(const char *peer_id, bool accepted) {
    char extra[160], id[96]; json_escape(peer_id, id, sizeof id);
    snprintf(extra, sizeof extra, "\"peer\":\"%s\",\"accepted\":%s", id, accepted ? "true" : "false");
    if (core_state() == CORE_READY) send_cmd("stream.reply", extra);
}

/* A entrada pelo servidor de chamadas falhou e o usuário escolheu seguir em P2P: entra de novo na mesma sala com "net":"p2p".
   Só fala com quem também estiver em P2P (o aviso da tela diz isso). */
void rtc_join_p2p(app_state_t *s) {
    char room[sizeof s->room_name]; copy(room, sizeof room, s->room_name);
    s->rtc_session_sent = s->rtc_session_ready;   /* a sessão do core continua valendo: só o room.join é refeito */
    rtc_join_room(s, room);
    s->force_p2p = true; s->net_failed = false;
}

void rtc_kick_peer(app_state_t *s, int slot) {
    if (!s->rtc_owner_me || slot < 0 || slot >= BRIKS_MAX_PEERS || !s->rtc_peers[slot].fp[0]) return;
    char extra[200], fp[96]; json_escape(s->rtc_peers[slot].fp, fp, sizeof fp);
    snprintf(extra, sizeof extra, "\"fp\":\"%s\"", fp);
    if (core_state() == CORE_READY) send_cmd("peer.kick", extra);
}

/* o dono pede ao core para fechar/reabrir o microfone de alguém para todos (pedido assinado; o core recusa quem não é dono) */
void rtc_room_mute(app_state_t *s, int slot, bool muted) {
    if (!s->rtc_owner_me || !s->net_sfu || slot < 0 || slot >= BRIKS_MAX_PEERS || !s->rtc_peers[slot].fp[0]) return;
    char extra[200], fp[96]; json_escape(s->rtc_peers[slot].fp, fp, sizeof fp);
    snprintf(extra, sizeof extra, "\"fp\":\"%s\",\"muted\":%s", fp, muted ? "true" : "false");
    if (core_state() == CORE_READY) send_cmd("room.mute", extra);
}

void rtc_mic_test(app_state_t *s, bool on) {
    if (core_state() != CORE_READY) return;
    /* o core aplica as Configurações > Voz (dispositivo, supressão, ganho e porta) no teste; o audio.cfg mais recente já foi enviado */
    send_cmd("mic.test", on ? "\"on\":true" : "\"on\":false");
    (void)s;
}
void rtc_set_mic(app_state_t *s, bool on) { if (!on) s->me_level = 0; if (core_state() == CORE_READY) send_cmd("mic.set", on ? "\"on\":true" : "\"on\":false"); }
static void pv_remember_mute(const char *fp, bool muted);
/* silenciar uma pessoa = ganho zero só para ela (por peer_id); lembrado por identidade entre chamadas */
void rtc_mute_peer(app_state_t *s, int slot, bool muted) {
    if (slot < 0 || slot >= BRIKS_MAX_PEERS || !s->rtc_peers[slot].id[0]) return;
    s->rtc_peers[slot].muted_by_me = muted; pv_remember_mute(s->rtc_peers[slot].fp, muted);
    char extra[160], id[96]; json_escape(s->rtc_peers[slot].id, id, sizeof id);
    snprintf(extra, sizeof extra, "\"peer\":\"%s\",\"muted\":%s", id, muted ? "true" : "false");
    if (core_state() == CORE_READY) send_cmd("peer.mute", extra);
}
/* CHM-14: volume por pessoa lembrado entre chamadas. Chave = impressão digital da identidade (estável); arquivo de linhas "fp=ganho" em
 * %APPDATA%\Briks-CUI\peer-volumes.txt (BRIKS_PEERVOL_FILE nos testes). Só guarda o que difere de 100%. */
#define PV_MAX 128
static struct { char fp[96]; float gain; bool muted; } g_pv[PV_MAX];   /* muted: 'Silenciar para mim' também é lembrado (linha "fp=ganho,1") */ static int g_pv_n; static bool g_pv_loaded;
static bool pv_path(char *out, size_t cap) {
    const char *o = getenv("BRIKS_PEERVOL_FILE"); if (o && o[0]) { snprintf(out, cap, "%s", o); return true; }
    const char *ad = getenv("APPDATA"); if (!ad || !ad[0]) return false;
    char dir[MAX_PATH]; snprintf(dir, sizeof dir, "%s\\Briks-CUI", ad); CreateDirectoryA(dir, NULL);
    snprintf(out, cap, "%s\\peer-volumes.txt", dir); return true;
}
static void pv_load(void) {
    if (g_pv_loaded) return; g_pv_loaded = true; g_pv_n = 0;
    char p[MAX_PATH], line[200]; if (!pv_path(p, sizeof p)) return;
    FILE *f = fopen(p, "rb"); if (!f) return;
    while (g_pv_n < PV_MAX && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='); if (!eq) continue; *eq = 0;
        float g = (float)atof(eq + 1); if (g < 0 || g > 2) continue;
        const char *cm = strchr(eq + 1, ',');
        snprintf(g_pv[g_pv_n].fp, sizeof g_pv[0].fp, "%s", line); g_pv[g_pv_n].gain = g; g_pv[g_pv_n].muted = cm && atoi(cm + 1) != 0; g_pv_n++;
    }
    fclose(f);
}
static void pv_save(void) {
    char p[MAX_PATH]; if (!pv_path(p, sizeof p)) return;
    FILE *f = fopen(p, "wb"); if (!f) return;
    for (int i = 0; i < g_pv_n; i++) fprintf(f, g_pv[i].muted ? "%s=%.2f,1\n" : "%s=%.2f\n", g_pv[i].fp, g_pv[i].gain);
    fclose(f);
}
float rtc_saved_volume(const char *fp) {   /* ganho lembrado (0..2) para essa identidade; 1 = normal */
    if (!fp || !fp[0]) return 1.0f; pv_load();
    for (int i = 0; i < g_pv_n; i++) if (!strcmp(g_pv[i].fp, fp)) return g_pv[i].gain;
    return 1.0f;
}
static void pv_remember(const char *fp, float gain) {
    if (!fp || !fp[0] || strchr(fp, '=') || strchr(fp, '\n')) return; pv_load();
    int k = -1; for (int i = 0; i < g_pv_n; i++) if (!strcmp(g_pv[i].fp, fp)) k = i;
    const bool normal = gain > 0.995f && gain < 1.005f;
    if (k >= 0 && fabsf(g_pv[k].gain - gain) < 0.004f) return;
    if (k < 0 && normal) return;
    if (k < 0) { if (g_pv_n >= PV_MAX) return; k = g_pv_n++; snprintf(g_pv[k].fp, sizeof g_pv[0].fp, "%s", fp); g_pv[k].muted = false; }
    if (normal && !g_pv[k].muted) { g_pv[k] = g_pv[--g_pv_n]; } else g_pv[k].gain = gain;
    pv_save();
}
bool rtc_saved_mute(const char *fp) {   /* "Silenciar para mim" lembrado para essa identidade */
    if (!fp || !fp[0]) return false; pv_load();
    for (int i = 0; i < g_pv_n; i++) if (!strcmp(g_pv[i].fp, fp)) return g_pv[i].muted;
    return false;
}
static void pv_remember_mute(const char *fp, bool muted) {
    if (!fp || !fp[0] || strchr(fp, '=') || strchr(fp, '\n') || strchr(fp, ',')) return; pv_load();
    int k = -1; for (int i = 0; i < g_pv_n; i++) if (!strcmp(g_pv[i].fp, fp)) k = i;
    if (k < 0 && !muted) return;
    if (k < 0) { if (g_pv_n >= PV_MAX) return; k = g_pv_n++; snprintf(g_pv[k].fp, sizeof g_pv[0].fp, "%s", fp); g_pv[k].gain = 1.0f; }
    g_pv[k].muted = muted;
    if (!muted && g_pv[k].gain > 0.995f && g_pv[k].gain < 1.005f) g_pv[k] = g_pv[--g_pv_n];
    pv_save();
}

/* quem entra na sala volta com o volume que a pessoa escolheu da última vez (identidade conhecida pelo peer.info) */
static void peer_volume_restore(app_state_t *s, int slot) {
    float g = rtc_saved_volume(s->rtc_peers[slot].fp);
    if (fabsf(g - 1.0f) > 0.004f) { s->rtc_peers[slot].volume = g * 100.0f; s->rtc_peers[slot].vol_set = true; rtc_set_peer_volume(s, slot, g); }
    if (rtc_saved_mute(s->rtc_peers[slot].fp) && !s->rtc_peers[slot].muted_by_me) rtc_mute_peer(s, slot, true);   /* quem eu silenciei continua silenciado */
}

void rtc_set_peer_volume(app_state_t *s, int slot, float gain) {
    if (slot < 0 || slot >= BRIKS_MAX_PEERS || !s->rtc_peers[slot].id[0]) return;
    pv_remember(s->rtc_peers[slot].fp, gain);
    char extra[160], id[96]; json_escape(s->rtc_peers[slot].id, id, sizeof id);
    snprintf(extra, sizeof extra, "\"peer\":\"%s\",\"volume\":%.2f", id, gain);
    if (core_state() == CORE_READY) send_cmd("peer.volume", extra);
}
void rtc_set_deaf(app_state_t *s, bool on) { (void)s; if (core_state() == CORE_READY) send_cmd("deafen.set", on ? "\"on\":true" : "\"on\":false"); }

/* Quem não coube na grade (mais de BRIKS_MAX_PEERS remotos): só os ids, para o "+N" contar certo quando entram e saem. */
/* Guarda também nome, impressão digital e estado: quando abre vaga na grade, o primeiro da fila sobe já completo. */
typedef struct { char id[48], state[16], fp[72], name[64], status[64], presence[64]; bool mic_off, deafened; unsigned long order; } over_t;
static over_t g_over[256];
static unsigned long g_over_seq;
static void over_clear(app_state_t *s) { memset(g_over, 0, sizeof g_over); s->rtc_overflow = 0; }
static over_t *over_find(const char *id) { for (int i = 0; i < 256; i++) if (g_over[i].id[0] && !strcmp(g_over[i].id, id)) return &g_over[i]; return NULL; }
static void over_set(app_state_t *s, const char *id, const char *state) {
    over_t *o = over_find(id);
    if (!strcmp(state, "fechado")) { if (o) { memset(o, 0, sizeof *o); if (s->rtc_overflow > 0) s->rtc_overflow--; } return; }
    if (!o) { for (int i = 0; i < 256 && !o; i++) if (!g_over[i].id[0]) o = &g_over[i]; if (!o) return; memset(o, 0, sizeof *o); copy(o->id, sizeof o->id, id); o->order = ++g_over_seq; s->rtc_overflow++; }
    copy(o->state, sizeof o->state, state);
}
/* peer.info / peer.state de quem está fora da grade */
static void over_note(const jval *j, const char *ev) {
    if (strcmp(ev, "peer.info") && strcmp(ev, "peer.state")) return;
    over_t *o = over_find(json_str(j, "peer_id", "")); if (!o) return;
    if (!strcmp(ev, "peer.info")) { copy(o->fp, sizeof o->fp, json_str(j, "fp", "")); copy(o->name, sizeof o->name, json_str(j, "name", "")); return; }
    const jval *m = json_get(j, "microphone"), *d = json_get(j, "deafened");
    o->mic_off = m && m->type == J_BOOL && !m->b; o->deafened = d && d->type == J_BOOL && d->b;
    copy(o->status, sizeof o->status, json_str(j, "status", "")); copy(o->presence, sizeof o->presence, json_str(j, "presence", ""));
}
/* Abriu vaga na grade: sobe quem espera há mais tempo. */
static void over_promote(app_state_t *s) {
    for (;;) {
        int slot = -1; for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (!s->rtc_peers[i].id[0]) { slot = i; break; }
        over_t *o = NULL; for (int i = 0; i < 256; i++) if (g_over[i].id[0] && (!o || g_over[i].order < o->order)) o = &g_over[i];
        if (slot < 0 || !o) return;
        memset(&s->rtc_peers[slot], 0, sizeof s->rtc_peers[slot]);
        copy(s->rtc_peers[slot].id, sizeof s->rtc_peers[slot].id, o->id); copy(s->rtc_peers[slot].state, sizeof s->rtc_peers[slot].state, o->state);
        copy(s->rtc_peers[slot].fp, sizeof s->rtc_peers[slot].fp, o->fp); copy(s->rtc_peers[slot].name, sizeof s->rtc_peers[slot].name, o->name);
        copy(s->rtc_peers[slot].status, sizeof s->rtc_peers[slot].status, o->status); copy(s->rtc_peers[slot].presence, sizeof s->rtc_peers[slot].presence, o->presence);
        s->rtc_peers[slot].mic_off = o->mic_off; s->rtc_peers[slot].deafened = o->deafened;
        memset(o, 0, sizeof *o); if (s->rtc_overflow > 0) s->rtc_overflow--;
    }
}

static void set_peer(app_state_t *s, const char *id, const char *state) {
    int n = (int)(sizeof s->rtc_peers / sizeof s->rtc_peers[0]);
    int slot = -1;
    if (!id[0]) return;
    for (int i = 0; i < n; i++) if (!strcmp(s->rtc_peers[i].id, id)) { slot = i; break; }
    if (slot < 0 && strcmp(state, "fechado")) for (int i = 0; i < n; i++) if (!s->rtc_peers[i].id[0]) { slot = i; break; }
    if (slot < 0) { over_set(s, id, state); return; }
    if (!strcmp(state, "fechado")) { share_peer_gone(s, slot); memset(&s->rtc_peers[slot], 0, sizeof s->rtc_peers[slot]); over_promote(s); }   /* saiu: some o que ela transmitia */
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
    over_note(j, ev);
    if (!strcmp(ev, "session.ready")) {
        s->rtc_session_ready = true;
        copy(s->rtc_fp, sizeof s->rtc_fp, json_str(j, "fingerprint", ""));
        const jval *idn = json_get(j, "identity");
        if (idn && json_str(idn, "x", "")[0]) snprintf(s->my_jwk, sizeof s->my_jwk, "{\"kty\":\"EC\",\"crv\":\"P-256\",\"x\":\"%s\",\"y\":\"%s\"}", json_str(idn, "x", ""), json_str(idn, "y", ""));
    } else if (!strcmp(ev, "signaling.status")) {
        if (!s->in_room) { json_free(j); return false; }   /* "desconectado" atrasado de uma sala já deixada não pode sujar a próxima */
        copy(s->rtc_state, sizeof s->rtc_state, json_str(j, "state", ""));
        net_friendly_error(json_str(j, "message", ""), s->rtc_msg, sizeof s->rtc_msg);   /* erro técnico de rede vira frase compreensível */
    } else if (!strcmp(ev, "peer")) {
        set_peer(s, json_str(j, "peer_id", ""), json_str(j, "state", ""));
        if (!strcmp(json_str(j, "state", ""), "autenticado")) groups_resend(s);
    } else if (!strcmp(ev, "speaking")) {
        const jval *lv = json_get(j, "level");
        float l = lv && lv->type == J_NUM ? (float)lv->num : 0;
        const char *p = json_str(j, "peer", "");
        if (!strcmp(p, "me")) s->me_level = l;
        else for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (s->rtc_peers[i].id[0] && !strcmp(s->rtc_peers[i].id, p)) s->rtc_peers[i].level = l;
    } else if (!strcmp(ev, "mic.test")) {
        const jval *lv = json_get(j, "level"); const char *er = json_str(j, "error", "");
        if (er[0]) {
            s->mic_test_on = false; s->mic_test_level = 0;
            if (json_str(j, "code", "")[0]) { copy(s->mic_state, sizeof s->mic_state, json_str(j, "code", "")); copy(s->mic_text, sizeof s->mic_text, er); s->mic_test_msg[0] = 0; }   /* motivo conhecido: o aviso do microfone cuida do texto */
            else copy(s->mic_test_msg, sizeof s->mic_test_msg, er);
        }
        else if (s->mic_test_on) { s->mic_test_level = lv && lv->type == J_NUM ? (float)lv->num : 0; if (!strcmp(s->mic_state, "denied") || !strcmp(s->mic_state, "no_device") || !strcmp(s->mic_state, "busy")) s->mic_state[0] = 0; }   /* mediu nível: o microfone funciona */
    } else if (!strcmp(ev, "audio.mic")) {
        const char *st = json_str(j, "state", "");
        copy(s->mic_state, sizeof s->mic_state, st);
        copy(s->mic_text, sizeof s->mic_text, json_str(j, "text", ""));
        if (strcmp(st, "ok") && st[0]) call_toast(!strcmp(st, "denied") ? "Sem acesso ao microfone: o Windows bloqueou. Veja o aviso em Configurações > Voz e vídeo." : !strcmp(st, "no_device") ? "Nenhum microfone encontrado neste computador." : !strcmp(st, "busy") ? "Outro aplicativo está usando o microfone." : "Não foi possível abrir o microfone.");
    } else if (!strcmp(ev, "audio.device")) {
        copy(s->dev_notice, sizeof s->dev_notice, json_str(j, "text", ""));
        if (s->dev_notice[0]) call_toast(s->dev_notice);
    } else if (!strcmp(ev, "peer.info")) {
        const char *p = json_str(j, "peer_id", "");
        for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (s->rtc_peers[i].id[0] && !strcmp(s->rtc_peers[i].id, p)) {
            copy(s->rtc_peers[i].fp, sizeof s->rtc_peers[i].fp, json_str(j, "fp", ""));
            copy(s->rtc_peers[i].name, sizeof s->rtc_peers[i].name, json_str(j, "name", ""));
            peer_volume_restore(s, i);   /* CHM-14 */
        }
    } else if (!strcmp(ev, "room.owner")) {
        const jval *m = json_get(j, "me");
        s->rtc_owner_me = m && m->type == J_BOOL && m->b;
    } else if (!strcmp(ev, "kicked")) {
        /* o core já fechou tudo: volta à tela social com o aviso */
        s->in_room = false; s->rtc_want_room = false; s->rtc_owner_me = false; s->kick_open = false; over_clear(s);
        memset(s->rtc_peers, 0, sizeof s->rtc_peers);
        s->rtc_peer_count = s->rtc_authed_count = 0;
        copy(s->rtc_state, sizeof s->rtc_state, "");
        s->kicked_open = true;
        s->screen = SCREEN_SOCIAL;
        call_reset(s);
    } else if (!strcmp(ev, "room.muted")) {
        /* o dono fechou (ou reabriu) o meu microfone: o core já aplicou; o botão acompanha e eu posso reabrir depois */
        const jval *m = json_get(j, "muted");
        bool muted = m && m->type == J_BOOL && m->b;
        s->mic_on = !muted;
        if (muted) s->me_level = 0;
        copy(s->rtc_msg, sizeof s->rtc_msg, muted ? "O dono da chamada silenciou o seu microfone." : "O dono da chamada reabriu o seu microfone.");
    } else if (!strcmp(ev, "peer.state")) {
        const char *p = json_str(j, "peer_id", "");
        const jval *m = json_get(j, "microphone"), *d = json_get(j, "deafened");
        for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (s->rtc_peers[i].id[0] && !strcmp(s->rtc_peers[i].id, p)) {
            s->rtc_peers[i].mic_off = m && m->type == J_BOOL && !m->b;
            s->rtc_peers[i].deafened = d && d->type == J_BOOL && d->b;
            copy(s->rtc_peers[i].status, sizeof s->rtc_peers[i].status, json_str(j, "status", ""));
            copy(s->rtc_peers[i].presence, sizeof s->rtc_peers[i].presence, json_str(j, "presence", ""));
        }
    } else if (!strcmp(ev, "hello.ok")) {   /* o core diz o nível do protocolo: abaixo do exigido é um core de outra versão (órfão ou briks-core.exe velho) */
        const jval *pv = json_get(j, "core_proto"); const int p = pv && pv->type == J_NUM ? (int)pv->num : 0; core_set_proto(p);
        if (p < BRIKS_CORE_PROTO_MIN) {
            snprintf(s->notice, sizeof s->notice, "O motor do Briks em execução é de outra versão (nível %d, o programa pede %d). Feche o Briks por completo (e briks-core.exe no Gerenciador de Tarefas) e abra de novo; se continuar, reinstale.", p, BRIKS_CORE_PROTO_MIN);
            settings_notice(s->notice); changed = true;
        }
    } else if (!strcmp(ev, "outbox.state")) {   /* fila de envio guardada no PC (pendentes/entregues/falhas) */
        changed = chat_outbox_event(s, j);
    } else if (!strcmp(ev, "file") || !strcmp(ev, "link.preview") || !strcmp(ev, "chat.typing") || !strcmp(ev, "chat.ack") || !strcmp(ev, "chat.sent")) {
        if (!strcmp(ev, "chat.ack")) chat_outbox_acked(json_str(j, "id", ""));
        changed = attach_event(s, ev, j);
    } else if (!strcmp(ev, "chat.link")) {   /* chat privado fechado/reaberto pelo core (AMI-16) */
        const jval *on = json_get(j, "on"); friends_dm_link(s, json_str(j, "fp", ""), on && on->type == J_BOOL && on->b); changed = true;
    } else if (!strcmp(ev, "chat.event")) {
        const jval *lc = json_get(j, "local");
        changed = msgui_on_event(s, json_str(j, "from_fp", ""), lc && lc->type == J_BOOL && lc->b, json_str(j, "wire_raw", ""));
    } else if (!strcmp(ev, "chat.message")) {   /* mensagem nova de outra pessoa: aviso do sistema e contador de não lidas (notify.c) */
        changed = chat_in_message(s, j, false);   /* DM: grava no histórico local, mostra na conversa aberta e avisa (chat.c) */
    } else if (!strcmp(ev, "chat.history")) {   /* mensagens que faltavam, vindas da sincronização com o amigo */
        changed = chat_in_message(s, j, true);
    } else if (!strcmp(ev, "dm.wake")) {   /* um amigo quer falar comigo e eu não estou em sala com ele: abre a minha ponta do canal direto */
        chat_dm_wake(s, json_str(j, "fp", "")); changed = false;
    } else if (!strcmp(ev, "dm.state")) {
        changed = false;
    } else if (!strcmp(ev, "room.chat")) {
        const jval *ts = json_get(j, "ts");
        call_chat_add(json_str(j, "id", ""), json_str(j, "author", "Participante"), json_str(j, "content", ""), ts && ts->type == J_NUM ? (long long)ts->num : 0, false);
        if (!(s->screen == SCREEN_CALL && s->call_view == 1)) g_call.unread++;
    } else if (!strcmp(ev, "stream.request")) {
        call_request_in(json_str(j, "peer_id", ""), json_str(j, "name", ""));
        { const char *nm = json_str(j, "name", ""); char t[140]; snprintf(t, sizeof t, "%s pediu para você transmitir a tela", nm[0] ? nm : "Alguém"); notify_post(s, NT_REQUEST, NULL, "Briks", t); }   /* NOT-04: aviso do sistema com o app em segundo plano */
    } else if (!strcmp(ev, "stream.reply")) {
        const jval *a = json_get(j, "accepted"); const char *nm = json_str(j, "name", ""); char t[160];
        if (a && a->type == J_BOOL && a->b) snprintf(t, sizeof t, "%s vai transmitir.", nm[0] ? nm : "A pessoa");
        else snprintf(t, sizeof t, "%s não pode transmitir agora.", nm[0] ? nm : "A pessoa");
        call_toast(t);
    } else if (!strcmp(ev, "ice.info")) {
        const jval *tn = json_get(j, "turn"); const char *w = json_str(j, "warn", "");
        snprintf(s->ice_note, sizeof s->ice_note, "%s%s%s", (tn && tn->type == J_BOOL && tn->b) ? "STUN + TURN" : "Somente STUN: redes restritivas podem não conectar sem TURN", w[0] ? " · config ICE ignorada: " : "", w);
        changed = true;
    } else if (!strcmp(ev, "group.roster")) {
        groups_roster_event(s, j);
    } else if (!strcmp(ev, "net.config")) {
        const jval *c = json_get(j, "configured");
        s->net_cfg_known = true; s->net_cfg_sfu = c && c->type == J_BOOL && c->b;
        copy(s->net_cfg_err, sizeof s->net_cfg_err, json_str(j, "error", ""));
    } else if (!strcmp(ev, "net.mode")) {
        const char *md = json_str(j, "mode", ""), *er = json_str(j, "error", "");
        s->net_sfu = !strcmp(md, "sfu");
        s->net_failed = !strcmp(md, "none");
        if (s->net_failed) { char nt[120]; snprintf(nt, sizeof nt, "Rede: sem chamada%s%s", er[0] ? " · " : "", er); settings_notice(nt); }
        snprintf(s->net_note, sizeof s->net_note, "%s · %s%s%s%s%s", !strcmp(md, "sfu") ? "SFU" : !strcmp(md, "none") ? "Sem chamada" : "P2P", json_str(j, "reason", ""),
                 json_str(j, "server", "")[0] ? " · servidor " : "", json_str(j, "server", ""), er[0] ? " · " : "", er);
        changed = true;
    } else if (!strncmp(ev, "share.", 6)) {
        if (!strcmp(ev, "share.local") && !strcmp(json_str(j, "state", ""), "off") && json_str(j, "reason", "")[0]) { char nt[120]; snprintf(nt, sizeof nt, "Transmissão: %s", json_str(j, "reason", "")); settings_notice(nt); }   /* a transmissão caiu/não iniciou */
        changed = share_handle_event(s, j, ev);
    } else if (!strncmp(ev, "cam.", 4)) {
        changed = cam_handle_event(s, j, ev);
    } else if (!strcmp(ev, "error")) {
        net_friendly_error(json_str(j, "message", ""), s->rtc_msg, sizeof s->rtc_msg);
        { const char *w = json_str(j, "where", ""); char nt[120];   /* CFG-26: o erro também vai para a aba Avisos, com a área */
          const char *area = !strncmp(w, "share", 5) || !strncmp(w, "stream", 6) || !strncmp(w, "cam", 3) ? "Transmissão" : !strncmp(w, "file", 4) || !strncmp(w, "chat", 4) ? "Arquivo" : !strncmp(w, "account", 7) ? "Conta" : !strncmp(w, "net", 3) || !strncmp(w, "mesh", 4) || !strncmp(w, "group", 5) ? "Rede" : "Chamada";
          snprintf(nt, sizeof nt, "%s: %s", area, s->rtc_msg); settings_notice(nt); }
        if (!strcmp(json_str(j, "where", ""), "session.start") || !strcmp(json_str(j, "where", ""), "room.join")) copy(s->rtc_state, sizeof s->rtc_state, "erro");
    } else if (!strcmp(ev, "core.closed")) {
        copy(s->rtc_state, sizeof s->rtc_state, "erro");
        copy(s->rtc_msg, sizeof s->rtc_msg, "O serviço de chamadas encerrou.");
        settings_notice("Chamada: o serviço de chamadas encerrou.");
        s->rtc_session_ready = s->rtc_session_sent = false; groups_roster_reset();
    } else changed = account_handle_event(s, line);   /* account.*, friends.*, local.*, import.* */
    json_free(j);
    return changed;
}

/* CHM-16: 1 h sem falar desconecta; falar, transmitir a tela ou "Continuar" zeram a contagem */
static void idle_tick(app_state_t *s) {
    const ULONGLONG now = GetTickCount64();
    if (!s->in_room) { s->idle_last = 0; s->idle_phase_now = IDLE_ACTIVE; return; }
    bool watching = false; if (s->call_mode == 1) for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (share_peer_live(s, i)) watching = true;   /* assistir a uma transmissão também conta como presença */
    if (!s->idle_last || (s->mic_on && s->me_level > 0.05f) || s->sh.active || watching) s->idle_last = now;
    const long long lim = idle_limit_for(settings_idle_min());
    long long rem = 0; int ph = idle_phase_lim((long long)(now - s->idle_last), lim, &rem);
    s->idle_phase_now = ph; s->idle_left_ms = rem;
    if (ph == IDLE_EXPIRED) {
        rtc_leave_room(s); s->idle_last = 0; s->idle_phase_now = IDLE_ACTIVE;
        { char m[120]; const long long mn = lim / 60000; if (mn >= 60 && mn % 60 == 0) snprintf(m, sizeof m, "Você saiu da chamada por inatividade (%lld %s sem falar).", mn / 60, mn == 60 ? "hora" : "horas"); else snprintf(m, sizeof m, "Você saiu da chamada por inatividade (%lld min sem falar).", mn > 0 ? mn : 1); copy(s->idle_notice, sizeof s->idle_notice, m); }
        if (s->screen == SCREEN_CALL) s->screen = SCREEN_SOCIAL;
        return;
    }
    static ULONGLONG last_wake;   /* sem quadros o relógio não andaria: acorda a tela a cada 1 s no aviso e a cada 20 s fora dele */
    if (now - last_wake >= (ph == IDLE_WARNING ? 1000u : 20000u)) { last_wake = now; if (s->app) cui_request_redraw(cui_app_ui(s->app)); }
}

void rtc_pump(app_state_t *s) {
    idle_tick(s);
    core_status_t st = core_state();
    static int demo_speak = -1;   /* BRIKS_DEMO_SPEAK=0.7: fixa o nível de fala local (captura de tela do halo, sem microfone) */
    if (demo_speak < 0) demo_speak = getenv("BRIKS_DEMO_SPEAK") ? 1 : 0;
    if (demo_speak) s->me_level = (float)atof(getenv("BRIKS_DEMO_SPEAK"));
    static bool demo_avisos_done;   /* BRIKS_DEMO_AVISOS=1: eventos de erro de exemplo pelo mesmo tratador do core (aba Avisos; captura e teste) */
    if (!demo_avisos_done && getenv("BRIKS_DEMO_AVISOS")) {
        demo_avisos_done = true;
        rtc_handle_event(s, "{\"ev\":\"error\",\"where\":\"room.join\",\"message\":\"sala inválida\"}");
        rtc_handle_event(s, "{\"ev\":\"share.local\",\"slot\":0,\"state\":\"off\",\"reason\":\"a captura da tela falhou\"}");
        rtc_handle_event(s, "{\"ev\":\"error\",\"where\":\"net.config\",\"message\":\"servidor de relé indisponível\"}");
    }
    static bool demo_peers_done;   /* BRIKS_DEMO_PEERS=1: dois participantes de exemplo e eu como dono (captura do menu/diálogo de expulsar) */
    if (!demo_peers_done && getenv("BRIKS_DEMO_PEERS")) {
        demo_peers_done = true;
        const char *nm[8] = { "Marina Costa", "Rafael Lima", "Bia Nogueira", "Caio Ramos", "Helena Dias", "Lucas Prado", "Duda Reis", "Téo Farias" };
        int dn = atoi(getenv("BRIKS_DEMO_PEERS")); if (dn < 2) dn = 2; if (dn > BRIKS_MAX_PEERS) dn = BRIKS_MAX_PEERS;   /* =N: N participantes (grade/foco nas capturas) */
        if (getenv("BRIKS_DEMO_REQ")) call_request_in("demo-0", nm[0]);                      /* captura do aviso "pediu para você transmitir" */
        if (getenv("BRIKS_DEMO_MIC")) { const char *m = getenv("BRIKS_DEMO_MIC"); copy(s->mic_state, sizeof s->mic_state, m); copy(s->mic_text, sizeof s->mic_text, "O Windows não deixou o Briks usar o microfone. Abra Configurações > Privacidade e segurança > Microfone e permita o acesso (inclusive para aplicativos de área de trabalho)."); }   /* captura do aviso do microfone na chamada */
        if (getenv("BRIKS_DEMO_DEVNOTICE")) { copy(s->dev_notice, sizeof s->dev_notice, !strcmp(getenv("BRIKS_DEMO_DEVNOTICE"), "1") ? "Dispositivo de áudio desconectado, usando Fone de teste" : getenv("BRIKS_DEMO_DEVNOTICE")); call_toast(s->dev_notice); }
        if (getenv("BRIKS_DEMO_ROOMCHAT")) { call_chat_add("d1", nm[0], "Cheguei! Alguém me ouve?", 0, false); call_chat_add("d2", nm[1], "Ouvindo bem daqui.", 0, false); g_call.unread = 2; }
        for (int i = 0; i < dn; i++) { snprintf(s->rtc_peers[i].id, sizeof s->rtc_peers[i].id, "demo-%d", i); copy(s->rtc_peers[i].state, sizeof s->rtc_peers[i].state, "autenticado");
            copy(s->rtc_peers[i].name, sizeof s->rtc_peers[i].name, nm[i % 8]); snprintf(s->rtc_peers[i].fp, sizeof s->rtc_peers[i].fp, "fp-demo-%d", i); }
        /* quem passa da grade entra pela fila de verdade; BRIKS_DEMO_LEAVE=K tira os K primeiros da grade (prova da promoção) */
        for (int i = BRIKS_MAX_PEERS; i < atoi(getenv("BRIKS_DEMO_PEERS")) && i < 200; i++) {
            char id[48], line[200]; snprintf(id, sizeof id, "demo-%d", i); set_peer(s, id, "autenticado");
            snprintf(line, sizeof line, "{\"ev\":\"peer.info\",\"peer_id\":\"%s\",\"fp\":\"fp-demo-%d\",\"name\":\"Fila %d\"}", id, i, i - BRIKS_MAX_PEERS + 1);
            jval *jj = json_parse(line); if (jj) { over_note(jj, "peer.info"); json_free(jj); }
        }
        if (getenv("BRIKS_DEMO_LEAVE")) for (int i = 0; i < atoi(getenv("BRIKS_DEMO_LEAVE")) && i < BRIKS_MAX_PEERS; i++) { char id[48]; snprintf(id, sizeof id, "demo-%d", i); set_peer(s, id, "fechado"); }
        if (getenv("BRIKS_DEMO_PEERVOL")) {   /* prova do CHM-14: muda o volume de um, "volta à chamada" lendo o arquivo de novo */
            rtc_set_peer_volume(s, 0, (float)atof(getenv("BRIKS_DEMO_PEERVOL")));
            s->rtc_peers[0].volume = 100.0f; s->rtc_peers[0].vol_set = false;
            g_pv_loaded = false; g_pv_n = 0;
            peer_volume_restore(s, 0);
        }
        if (getenv("BRIKS_DEMO_PEERMUTE")) {   /* prova do CHM-36: silencia um, "volta à chamada" relendo o arquivo: continua silenciado */
            rtc_mute_peer(s, 0, true);
            s->rtc_peers[0].muted_by_me = false; g_pv_loaded = false; g_pv_n = 0;
            peer_volume_restore(s, 0);
        }
        if (getenv("BRIKS_DEMO_NETFAIL")) { s->net_failed = true; copy(s->rtc_state, sizeof s->rtc_state, "erro"); copy(s->rtc_msg, sizeof s->rtc_msg, "Não foi possível entrar pelo servidor de chamadas: serviço de tokens inacessível"); }
        s->rtc_peers[1].level = 0.6f; if (getenv("BRIKS_DEMO_STATUS")) { copy(s->rtc_peers[0].status, sizeof s->rtc_peers[0].status, "Chego em 10 minutos"); copy(s->rtc_peers[1].presence, sizeof s->rtc_peers[1].presence, "Jogando Valorant"); } s->rtc_owner_me = true; s->in_room = true;
        copy(s->rtc_state, sizeof s->rtc_state, "conectado"); s->rtc_peer_count = s->rtc_authed_count = dn;   /* a demonstração mostra a sala como conectada */
        const char *o = getenv("BRIKS_DEV_OPEN"); if (o && !strcmp(o, "kick")) { s->kick_slot = 0; s->kick_open = true; }
    }
    {   /* "Conectando…" não pode ficar mudo para sempre: sem resposta da sinalização em 30 s o app diz o motivo provável */
        const bool pending = s->in_room && (!s->rtc_state[0] || !strcmp(s->rtc_state, "conectando")) && !getenv("BRIKS_DEMO_PEERS");
        if (!pending) s->rtc_conn_since = 0;
        else if (!s->rtc_conn_since) s->rtc_conn_since = GetTickCount64();
        else if (GetTickCount64() - s->rtc_conn_since > 30000ULL) {
            copy(s->rtc_state, sizeof s->rtc_state, "erro");
            copy(s->rtc_msg, sizeof s->rtc_msg, "O servidor de chamadas não respondeu em 30 segundos. Verifique a internet e entre de novo na sala.");
            s->rtc_conn_since = 0;
        }
    }
    if (st == CORE_FAILED && s->in_room && strcmp(s->rtc_state, "erro")) {
        copy(s->rtc_state, sizeof s->rtc_state, "erro");
        copy(s->rtc_msg, sizeof s->rtc_msg, core_error());
    }
    if (st == CORE_READY) {
        {   /* Configurações > Voz: ganhos, porta de ruído, supressão e dispositivos seguem para o core quando mudam ou quando entra numa sala (o áudio nasce ali) */
            static float li = -1, lo = -1, lg = -1; static bool lgo, lroom; static char lstate[16], lmic[64], lout[64]; static int lnoise = -1; static bool lcore; static int lecho = -1;
            float gi, go, gd; bool ga; settings_audio(&gi, &go, &ga, &gd);
            char mic[64], outd[64]; int noff; settings_audio_ex(mic, sizeof mic, outd, sizeof outd, &noff); const int echo = settings_echo() ? 1 : 0;
            if (gi != li || go != lo || ga != lgo || (ga && gd != lg) || s->in_room != lroom || strcmp(lstate, s->rtc_state) || strcmp(lmic, mic) || strcmp(lout, outd) || noff != lnoise || echo != lecho || !lcore) {
                snprintf(lstate, sizeof lstate, "%s", s->rtc_state); snprintf(lmic, sizeof lmic, "%s", mic); snprintf(lout, sizeof lout, "%s", outd);
                li = gi; lo = go; lgo = ga; lg = gd; lroom = s->in_room; lnoise = noff; lecho = echo; lcore = true;
                char extra[420], me[140], oe[140]; json_escape(mic, me, sizeof me); json_escape(outd, oe, sizeof oe);
                int n = snprintf(extra, sizeof extra, "\"in_gain\":%.2f,\"out_gain\":%.2f,\"noise\":%d,\"echo\":%s,\"mic\":\"%s\",\"out\":\"%s\"", gi, go, noff, echo ? "true" : "false", me, oe);
                if (ga) snprintf(extra + n, sizeof extra - (size_t)n, ",\"gate_db\":%.0f", gd);
                send_cmd("audio.cfg", extra);
            }
        }
        {   /* status e presença (programa em uso, se ligado) seguem para o core; ele anuncia aos peers autenticados da sala */
            static char sent_st[96], sent_pr[64]; static bool sent_once;
            const char *pr = s->rp_enabled ? s->user_presence : "";
            char comp[96]; if (s->user_status[0] && s->user_emoji[0]) snprintf(comp, sizeof comp, "%s %s", s->user_emoji, s->user_status); else copy(comp, sizeof comp, s->user_status);   /* o emoji viaja junto do texto */
            if (!sent_once || strcmp(sent_st, comp) || strcmp(sent_pr, pr)) {
                sent_once = true; copy(sent_st, sizeof sent_st, comp); copy(sent_pr, sizeof sent_pr, pr);
                char extra[360], a[200], b[140]; json_escape(comp, a, sizeof a); json_escape(pr, b, sizeof b);
                snprintf(extra, sizeof extra, "\"status\":\"%s\",\"presence\":\"%s\"", a, b);
                send_cmd("profile.set", extra);
            }
        }
        {   /* Configurações > Rede: o modo da chamada e a preferência de conexão seguem para o core (que grava net-mode.json) quando mudam */
            static int lcm = -1, lnp = -1;
            const int cm = settings_call_mode(), np = settings_net_pref();
            if (cm != lcm || np != lnp) {
                lcm = cm; lnp = np;
                char extra[96]; snprintf(extra, sizeof extra, "\"mode\":\"%s\",\"pref\":%d", cm == 1 ? "p2p" : cm == 2 ? "sfu" : "auto", np);
                send_cmd("net.mode.set", extra);
            }
        }
        if ((s->rtc_want_room || groups_want_roster(s) || chat_dm_wants_session(s)) && !s->rtc_session_sent) {
            char extra[256], nm[160];
            json_escape(s->user_name[0] ? s->user_name : "Você", nm, sizeof nm);
            snprintf(extra, sizeof extra, "\"display_name\":\"%s\"", nm);
            send_cmd("session.start", extra);
            s->rtc_session_sent = true;
        }
        groups_roster_tick(s);
        chat_dm_tick(s);
        if (s->rtc_want_room && s->rtc_session_ready) {
            char extra[256], rm[160];
            json_escape(s->room_name, rm, sizeof rm);
            { int cm = settings_call_mode(); snprintf(extra, sizeof extra, "\"namespace\":\"room\",\"room\":\"%s\",\"net\":\"%s\",\"pref\":%d", rm, (cm == 1 || s->force_p2p) ? "p2p" : cm == 2 ? "sfu" : "auto", settings_net_pref()); }
            send_cmd("room.join", extra);
            s->rtc_want_room = false;
        }
    }
    history_tick(s);   /* registro da chamada em andamento (ui_history.c) */
    static char line[65536];   /* a lista de fontes (share.sources) passa de 8 KB */
    bool any = false;
    while (core_next_event(line, sizeof line)) any |= rtc_handle_event(s, line);
    if (any || (s->in_room && (st == CORE_STARTING))) cui_request_redraw(cui_app_ui(s->app));
}

const char *rtc_status_text(const app_state_t *s, char *buf, size_t cap) {
    if (!s->in_room) { snprintf(buf, cap, "Você está fora da sala"); return buf; }
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

void rtc_chat_event(const char *fields) {
    if (core_state() != CORE_READY) return;
    char line[2200];
    snprintf(line, sizeof line, "{\"cmd\":\"chat.event.send\",%s}", fields);
    core_send(line);
}

bool rtc_mic_problem(const app_state_t *s) { return s->mic_state[0] && strcmp(s->mic_state, "ok") != 0; }
void rtc_open_mic_privacy(void) { ShellExecuteA(NULL, "open", "ms-settings:privacy-microphone", NULL, NULL, SW_SHOWNORMAL); }
