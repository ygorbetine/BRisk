/* Avisos do sistema, sons e contador de não lidas (NOT-01..05, CHM-20, BAS-20..22).
 * O balão em si é do tray.c (balão nativo do Windows pelo ícone da bandeja); aqui ficam as regras (sm_toast_allowed em sysmodel.h):
 * respeita os interruptores de Configurações, não avisa com a janela em foco na conversa, durante a chamada nem logo ao abrir. */
#include "sysint.h"
#include "storage.h"
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_notify_shown, g_notify_blocked, g_sound_played;
static sm_unread g_un;
static ULONGLONG g_grace_until;
static char g_click_chan[160];
static bool g_un_loaded;
static int g_force_focus = -1;   /* roteiro: sys focus 0|1 (a janela oculta de teste nunca é a ativa) */
static int g_last_total = -1, g_last_in_call = -1;

/* ---- não lidas persistem entre aberturas do app (unread.txt na pasta de dados: só "n<TAB>canal", nunca texto de mensagem) ---- */
static void un_path(char *out, size_t cap) { char d[MAX_PATH], h[MAX_PATH], r[MAX_PATH]; storage_dirs(d, h, r, sizeof d); snprintf(out, cap, "%s\\unread.txt", d); }
static void un_load(void) {
    g_un_loaded = true; char p[MAX_PATH + 16]; un_path(p, sizeof p);
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[260];
    while (fgets(line, sizeof line, f)) {
        int n = atoi(line); char *t = strchr(line, '\t'); if (!t || n <= 0) continue;
        t++; t[strcspn(t, "\r\n")] = 0; if (!t[0]) continue;
        for (int i = 0; i < n && i < 999; i++) sm_unread_add(&g_un, t);
    }
    fclose(f);
}
static void un_save(void) {
    char p[MAX_PATH + 16]; un_path(p, sizeof p);
    FILE *f = fopen(p, "w"); if (!f) return;
    for (int i = 0; i < g_un.count; i++) fprintf(f, "%d\t%s\n", g_un.e[i].n, g_un.e[i].key);
    fclose(f);
}

void notify_set_force_focus(int v) { g_force_focus = v; }
int notify_force_focus(void) { return g_force_focus; }
void notify_set_grace_ms(unsigned ms) { g_grace_until = GetTickCount64() + ms; }

/* ---- sons (WAV sintetizado, tocado da memória) ---- */
void sound_play(sound_kind_t k) {
    if (!g_sys.sounds || k < 0 || k >= SND_COUNT) return;
    g_sound_played++;
    if (sys_is_headless() && !getenv("BRIKS_SOUND_PLAY")) return;   /* teste: conta, mas não toca no alto-falante de ninguém */
    static unsigned char buf[SND_COUNT][SM_WAV_CAP]; static int vol[SND_COUNT] = { -1, -1, -1, -1, -1, -1 };
    if (vol[k] != g_sys.sound_vol) { if (!sm_wav(k, g_sys.sound_vol, buf[k], sizeof buf[k])) return; vol[k] = g_sys.sound_vol; }
    PlaySoundA((LPCSTR)buf[k], NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

/* ---- conversa por canal ---- */
static void dm_chan(const app_state_t *s, const char *peer, char *out, size_t cap) {   /* mesma regra do chat.c */
    const char *a = s->acct_user_id[0] ? s->acct_user_id : s->user_id, *b = peer;
    if (strcmp(a, b) > 0) { const char *t = a; a = b; b = t; }
    snprintf(out, cap, "dm-%s-%s", a, b);
}
static const char *group_of(const app_state_t *s, const char *chan, int *gi) {
    for (int i = 0; i < s->group_count; i++) {
        const group_t *g = &s->groups[i];
        bool hit = !strcmp(g->channel, chan);
        for (int k = 0; !hit && k < g->nch; k++) if (!strcmp(g->ch[k].id, chan)) hit = true;
        if (hit) { if (gi) *gi = i; return g->name; }
    }
    return NULL;
}

void notify_open_channel(app_state_t *s, const char *chan) {
    if (!chan || !chan[0] || s->screen == SCREEN_AUTH) return;
    int gi; if (group_of(s, chan, &gi)) { app_open_group(s, gi); return; }
    for (int i = 0; i < s->friend_count; i++) { char c[160]; dm_chan(s, s->friends[i].id, c, sizeof c); if (!strcmp(c, chan)) { app_open_friend(s, i); return; } }
}
bool notify_take_click(char *chan, size_t cap) {
    if (!g_click_chan[0]) return false;
    snprintf(chan, cap, "%s", g_click_chan); g_click_chan[0] = 0; return true;
}

int notify_unread_total(void) { return sm_unread_total(&g_un); }
int notify_unread_for(const char *chan) { return sm_unread_get(&g_un, chan); }

static bool in_chat_view(const app_state_t *s, const char *chan) {
    return s->screen == SCREEN_SOCIAL && chan && chan[0] && !strcmp(chan, s->chat_channel) && (s->active_friend >= 0 || s->active_group >= 0);
}

void notify_chat_opened(app_state_t *s) {
    if (s->screen == SCREEN_SOCIAL && s->chat_channel[0] && (s->active_friend >= 0 || s->active_group >= 0) && sys_window_focused()) sm_unread_clear(&g_un, s->chat_channel);
}

void notify_post(app_state_t *s, notify_kind_t kind, const char *chan, const char *title, const char *text) {
    sm_ctx c = { .nt_msg = g_sys.nt_msg, .nt_online = settings_nt_online(), .nt_stream = settings_nt_stream(), .friend_alerts_off = s->notify_off,
                 .focused = sys_window_focused(), .in_call = s->in_room, .grace = GetTickCount64() < g_grace_until, .same_chat = in_chat_view(s, chan) };
    if (kind == NT_MESSAGE && chan && chan[0] && !(c.focused && c.same_chat)) sm_unread_add(&g_un, chan);   /* conta mesmo sem aviso (badge) */
    if (!sm_toast_allowed(kind, &c)) { g_notify_blocked++; return; }
    char t[80], x[200]; sm_trunc(t, sizeof t, title && title[0] ? title : "Briks", 50); sm_trunc(x, sizeof x, text ? text : "", 120);
    if (chan && chan[0]) snprintf(g_click_chan, sizeof g_click_chan, "%s", chan); else g_click_chan[0] = 0;
    g_notify_shown++;
    tray_balloon(t, x);
    sound_play(SND_NOTIFY);
}

void notify_chat_message(app_state_t *s, const char *channel, const char *author, const char *content) {
    if (!channel || !channel[0]) return;
    if (author && s->user_name[0] && !strcmp(author, s->user_name)) return;   /* a minha própria */
    const char *gname = group_of(s, channel, NULL);
    char title[100], text[200];
    if (gname) { snprintf(title, sizeof title, "%s", gname); snprintf(text, sizeof text, "%s: %s", author && author[0] ? author : "?", content ? content : ""); }
    else { snprintf(title, sizeof title, "%s", author && author[0] ? author : "Nova mensagem"); snprintf(text, sizeof text, "%s", content ? content : ""); }
    notify_post(s, NT_MESSAGE, channel, title, text);
}

void notify_friend(app_state_t *s, const char *text, bool in_call) { notify_post(s, in_call ? NT_FRIEND_CALL : NT_FRIEND_ONLINE, NULL, "Briks", text); }

/* a cada quadro: sons da chamada, transmissão iniciada, contador na bandeja e na barra de tarefas */
void notify_tick(app_state_t *s) {
    static bool init; static bool prev_room; static int prev_peers = -1; static bool prev_share[BRIKS_MAX_PEERS];
    if (!init) { init = true; const char *g = getenv("BRIKS_NOTIFY_GRACE"); g_grace_until = GetTickCount64() + (g ? (unsigned)atoi(g) : 15000u); }   /* logo ao abrir: nada é avisado */
    int peers = 0; for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (s->rtc_peers[i].id[0] && (!strcmp(s->rtc_peers[i].state, "autenticado") || !strcmp(s->rtc_peers[i].state, "conectado"))) peers++;
    if (s->in_room != prev_room) { sound_play(s->in_room ? SND_JOIN : SND_LEAVE); prev_room = s->in_room; prev_peers = peers; }
    else if (s->in_room && peers != prev_peers) { if (prev_peers >= 0) sound_play(peers > prev_peers ? SND_PEER_JOIN : SND_PEER_LEAVE); prev_peers = peers; }
    for (int i = 0; i < BRIKS_MAX_PEERS; i++) {
        bool on = s->rtc_peers[i].id[0] && s->sh.remote[i].on;
        if (on && !prev_share[i]) { if (GetTickCount64() >= g_grace_until) sound_play(SND_STREAM); char t[140]; snprintf(t, sizeof t, "%s começou a transmitir", s->rtc_peers[i].name[0] ? s->rtc_peers[i].name : "Alguém"); notify_post(s, NT_STREAM, NULL, "Briks", t); }
        prev_share[i] = on;
    }
    if (!g_un_loaded) { un_load(); g_last_total = -1; }
    notify_chat_opened(s);
    int total = sm_unread_total(&g_un), in_call = s->in_room ? 1 : 0;
    if (total != g_last_total || in_call != g_last_in_call) {
        if (total != g_last_total) un_save();
        g_last_total = total; g_last_in_call = in_call;
        char tip[100]; int n = snprintf(tip, sizeof tip, "Briks");
        if (in_call) n += snprintf(tip + n, sizeof tip - (size_t)n, " · em chamada");
        if (total) snprintf(tip + n, sizeof tip - (size_t)n, " · %d não lida%s", total, total == 1 ? "" : "s");
        tray_update(tip, total);
    }
    tray_taskbar_badge(sys_hwnd(), total, (uint32_t)(((unsigned)g_th.c1.r << 16) | ((unsigned)g_th.c1.g << 8) | g_th.c1.b));   /* sai cedo se nada mudou */
    tray_menu_state(s->in_room, s->mic_on);
}
