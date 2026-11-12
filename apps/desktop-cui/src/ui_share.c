/* Transmissão de tela/janela: seletor "Compartilhar tela — Escolha a fonte e confirme", áudio por app, qualidade independente
 * (resolução, FPS, bitrate), chip na barra da call, tile de vídeo (prévia de quem transmite) e palco do modo Transmissão.
 * O trabalho pesado (captura, H264, WebRTC, áudio por processo) é do briks-core; aqui só há comandos/eventos JSON e a exibição:
 * o core grava cada quadro RGBA num arquivo mapeado (seqlock) e esta tela o sobe como textura OpenGL.
 * Regra do produto: a chamada abre sem exibir transmissões; assistir exige o modo Transmissão (só então o core decodifica). */
#include "briks_ui.h"
#include "core.h"
#include "json.h"
#include "callhist.h"
#include <cui/cui_render.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static void copy(char *dst, size_t cap, const char *src) { snprintf(dst, cap, "%s", src ? src : ""); }
static int jint(const jval *j, const char *k) { const jval *v = json_get(j, k); return v && v->type == J_NUM ? (int)v->num : 0; }
static float jnum(const jval *j, const char *k) { const jval *v = json_get(j, k); return v && v->type == J_NUM ? (float)v->num : 0; }
static bool jbool(const jval *j, const char *k) { const jval *v = json_get(j, k); return v && v->type == J_BOOL && v->b; }
static void cmd(const char *json) { if (core_state() == CORE_READY) core_send(json); }

/* zoom de quem assiste, por transmissão (zerado = encaixada), e o aviso "X começou a transmitir · Assistir" */
static cui_image_view g_zoom[2 * (BRIKS_MAX_PEERS + 1)];   /* mesmo índice de g_fv: cada transmissão tem o próprio zoom */
static int g_zoom_pct[2 * (BRIKS_MAX_PEERS + 1)];
static int g_tiles;                                 /* blocos desenhados no palco no último quadro (roteiros de teste) */   /* o que o selo mostrou no último quadro (roteiros de teste) */
static struct { int slot; double until; } g_notice = { -1, -1 };
static bool share_viewing(const app_state_t *s) { return s->screen == SCREEN_CALL && s->call_mode == 1 && s->call_view == 0; }
static bool g_dev_open_quality;   /* gancho de captura: abre o popover de qualidade/telemetria */

/* ---------- qualidade ---------- */
static const int RES_H[4] = { 720, 1080, 1440, 2160 };
static const char *const RES_LABEL[4] = { "720p", "1080p", "1440p", "4K" };
#define FPS_N 7
static const int FPS_V[FPS_N] = { 10, 15, 20, 24, 30, 45, 60 };
static const char *const FPS_LABEL[FPS_N] = { "10", "15", "20", "24", "30", "45", "60" };
#define FPS_DEFAULT 4   /* 30 */
static const char *const BR_LABEL[4] = { "Automático", "Alto", "Máximo", "Personalizado" };
#define BR_MAX_KBPS 50000

/* mesmo cálculo de video::auto_bitrate no core */
static int auto_kbps(int h, int fps) {
    double base = h <= 480 ? 1800 : h <= 720 ? 3500 : h <= 1080 ? 6000 : h <= 1440 ? 10000 : 18000;   /* perfis do Electron: 720p30 3,5 / 1080p30 6 Mbps */
    return (int)(base * pow((fps < 1 ? 1 : fps) / 30.0, 0.55));
}
static int chosen_kbps(const share_state_t *q) {   /* o que será pedido ao core (0 = automático) */
    int a = auto_kbps(RES_H[q->res], FPS_V[q->fps]), k = 0;
    if (q->br_mode == 1) k = (int)(a * 1.6);
    else if (q->br_mode == 2) k = (int)(a * 2.5);
    else if (q->br_mode == 3) k = (int)(q->br_custom * 1000);
    return k > BR_MAX_KBPS ? BR_MAX_KBPS : k;
}
static void fmt_rate(int kbps, char *out, size_t cap) {
    if (kbps >= 10000) snprintf(out, cap, "%d Mbps", (kbps + 500) / 1000);
    else if (kbps >= 1000) { int d = (kbps + 50) / 100; if (d % 10) snprintf(out, cap, "%d,%d Mbps", d / 10, d % 10); else snprintf(out, cap, "%d Mbps", d / 10); }
    else snprintf(out, cap, "%d kbps", kbps);
}
static const char *res_name(int h) { return h >= 2160 ? "4K" : h >= 1440 ? "1440p" : h >= 1080 ? "1080p" : h >= 720 ? "720p" : "480p"; }
/* "1080p · 30 FPS · 4 Mbps": o que está no ar quando transmitindo; senão o que foi escolhido */
static void quality_chip(const share_state_t *q, char *out, size_t cap) {
    char r[24];
    if (q->active && q->h) {   /* no ar: valores ATUAIS do laço de adaptação (mudam ao vivo) */
        int h = q->live_h ? q->live_h : q->h, f = q->stat_fps > 0.5f ? (int)(q->stat_fps + 0.5f) : q->cur_fps, k = q->stat_kbps > 0 ? q->stat_kbps : q->kbps;
        fmt_rate(k, r, sizeof r); snprintf(out, cap, "%s · %d FPS · %s", res_name(h), f, r); return;
    }
    int k = chosen_kbps(q); if (!k) k = auto_kbps(RES_H[q->res], FPS_V[q->fps]);
    fmt_rate(k, r, sizeof r);
    snprintf(out, cap, "%s · %d FPS · %s", RES_LABEL[q->res], FPS_V[q->fps], r);
}

/* ---------- quadro compartilhado -> textura ---------- */
typedef struct { char path[300]; HANDLE file, map; const unsigned char *p; unsigned long long n; cui_texture tex; int w, h; double last; } fview_t;
#define FV_N (BRIKS_MAX_PEERS + 1)
#define FV2(i) (FV_N + (i))   /* a 2ª tela de cada um fica na segunda metade */
static fview_t g_fv[2 * FV_N];   /* 0..MAX-1 = slots de rtc_peers; MAX = prévia local; depois o mesmo para a 2ª tela */
static cui_texture g_trash[64]; static int g_ntrash;   /* texturas a destruir no próximo quadro (só a thread da UI mexe em GL) */

static void fv_close(fview_t *v) {
    if (v->p) UnmapViewOfFile(v->p);
    if (v->map) CloseHandle(v->map);
    if (v->file && v->file != INVALID_HANDLE_VALUE) CloseHandle(v->file);
    if (v->tex && g_ntrash < 64) g_trash[g_ntrash++] = v->tex;
    memset(v, 0, sizeof *v);
}

/* Abre (se preciso) e sobe o quadro mais novo. Devolve true se há textura para mostrar. */
static bool fv_update(cui_ctx *ui, app_state_t *s, fview_t *v, const char *path) {
    if (strcmp(v->path, path)) { fv_close(v); copy(v->path, sizeof v->path, path); }
    if (!v->p) {
        v->file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        if (v->file == INVALID_HANDLE_VALUE) { v->file = NULL; return false; }
        v->map = CreateFileMappingA(v->file, NULL, PAGE_READONLY, 0, 0, NULL);
        if (v->map) v->p = MapViewOfFile(v->map, FILE_MAP_READ, 0, 0, 0);
        if (!v->p) { if (v->map) CloseHandle(v->map); CloseHandle(v->file); v->map = v->file = NULL; return false; }
    }
    /* modos de desempenho: Simplificado sobe no máximo 30 quadros/s, Mínimo 15 */
    double now = cui_time(ui), min_dt = g_perf >= PERF_MIN ? 1.0 / 15 : g_perf == PERF_SIMPLE ? 1.0 / 30 : 0;
    if (v->tex && now - v->last < min_dt) return true;
    const volatile unsigned *seq = (const volatile unsigned *)(v->p + 4);
    unsigned a = *seq;
    if (memcmp(v->p, "BKFR", 4) || (a & 1)) return v->tex != 0;
    int w = *(const int *)(v->p + 8), h = *(const int *)(v->p + 12);
    unsigned long long n = *(const unsigned long long *)(v->p + 16);
    if (w <= 0 || h <= 0 || w > 3840 || h > 2160 || n == 0 || n == v->n) return v->tex != 0;
    cui_gl_renderer *gl = cui_app_gl_renderer(s->app);
    if (v->tex && w == v->w && h == v->h && gl) {
        cui_renderer_iface r = cui_gl_iface(gl);
        r.texture_update(r.user, v->tex, 0, 0, w, h, v->p + 64, w * 4);
    } else {
        if (v->tex && g_ntrash < 64) g_trash[g_ntrash++] = v->tex;
        v->tex = cui_image_create(ui, w, h, v->p + 64);
        v->w = w; v->h = h;
    }
    if (*seq == a) v->n = n;   /* o core escreveu no meio da cópia: sobe de novo no próximo quadro */
    v->last = now;
    return v->tex != 0;
}

static void preview_path_n(char *out, size_t cap, bool second) {
    const char *d = getenv("BRIKS_CORE_DATA"), *f = second ? "preview-2.bin" : "preview.bin";
    if (d && d[0]) snprintf(out, cap, "%s\\video\\%s", d, f);
    else { const char *a = getenv("APPDATA"); snprintf(out, cap, "%s\\Briks-CUI\\video\\%s", a ? a : ".", f); }
}
static void preview_path(char *out, size_t cap) { preview_path_n(out, cap, false); }

/* ---------- comandos ---------- */
void share_open_dialog(app_state_t *s) {
    share_state_t *q = &s->sh;
    q->dialog_open = true; q->err[0] = 0; q->pick_second = false;
    if (core_state() != CORE_READY) { q->loading = false; copy(q->err, sizeof q->err, "Entre numa sala para transmitir."); return; }
    q->loading = true;
    cmd("{\"cmd\":\"share.list\"}");
    cmd("{\"cmd\":\"share.audio.list\"}");
}

static void send_start(app_state_t *s, const char *kind, const char *id, const char *label) {
    share_state_t *q = &s->sh;
    char line[1024], lab[160], pids[400] = ""; size_t o = 0;
    json_escape(label, lab, sizeof lab);
    for (int i = 0; i < q->napps && o < sizeof pids - 16; i++) if (q->apps[i].on) o += (size_t)snprintf(pids + o, sizeof pids - o, "%s%u", o ? "," : "", q->apps[i].pid);
    snprintf(line, sizeof line, "{\"cmd\":\"share.start\",\"kind\":\"%s\",\"id\":\"%s\",\"height\":%d,\"fps\":%d,\"kbps\":%d,\"label\":\"%s\",\"audio\":\"%s\",\"pids\":[%s],\"content\":\"%s\"}",
             kind, id, RES_H[q->res], FPS_V[q->fps], chosen_kbps(q), lab, q->audio_mode == 0 ? "source" : q->audio_mode == 1 ? "apps" : "none", pids, q->content ? "motion" : "detail");
    cmd(line);
    copy(q->last_kind, sizeof q->last_kind, kind); copy(q->last_id, sizeof q->last_id, id); copy(q->label, sizeof q->label, label);
    q->starting = true; q->err[0] = 0;
}

/* 2ª tela simultânea: mesma qualidade escolhida, sem áudio (o áudio da transmissão é o da 1ª tela) */
static void send_start_second(app_state_t *s, const char *kind, const char *id, const char *label) {
    share_state_t *q = &s->sh;
    char line[700], lab[160];
    json_escape(label, lab, sizeof lab);
    snprintf(line, sizeof line, "{\"cmd\":\"share.start\",\"slot\":1,\"kind\":\"%s\",\"id\":\"%s\",\"height\":%d,\"fps\":%d,\"kbps\":%d,\"label\":\"%s\",\"audio\":\"none\",\"content\":\"%s\"}",
             kind, id, RES_H[q->res], FPS_V[q->fps], chosen_kbps(q), lab, q->content ? "motion" : "detail");
    cmd(line);
    copy(q->second.label, sizeof q->second.label, label);
    q->second.starting = true; q->err[0] = 0;
}

static void start_selected(app_state_t *s) {
    share_state_t *q = &s->sh;
    int t = q->tab;
    if (q->sel[t] < 0 || q->sel[t] >= q->nsrc[t]) return;
    /* trava de teste: com BRIKS_SHARE_NO_PRIMARY os roteiros automáticos nunca transmitem o monitor principal do usuário */
    if (t == 0 && getenv("BRIKS_SHARE_NO_PRIMARY") && strstr(q->src[t][q->sel[t]].sub, "principal")) { copy(q->err, sizeof q->err, "Teste: a tela principal está bloqueada (BRIKS_SHARE_NO_PRIMARY)"); return; }
    if (q->pick_second && q->active) send_start_second(s, t == 0 ? "screen" : "window", q->src[t][q->sel[t]].id, q->src[t][q->sel[t]].name);
    else send_start(s, t == 0 ? "screen" : "window", q->src[t][q->sel[t]].id, q->src[t][q->sel[t]].name);
    q->dialog_open = false; q->pick_second = false;
}

void share_stop_second(app_state_t *s) {
    if (s->sh.second.active || s->sh.second.starting) cmd("{\"cmd\":\"share.stop\",\"slot\":1}");
    memset(&s->sh.second, 0, sizeof s->sh.second); fv_close(&g_fv[FV2(BRIKS_MAX_PEERS)]);
}

/* parar a transmissão para as duas telas */
void share_stop(app_state_t *s) { share_stop_second(s); cmd("{\"cmd\":\"share.stop\"}"); s->sh.active = false; s->sh.starting = false; }

void share_reset(app_state_t *s) {
    share_state_t *q = &s->sh;
    for (int t = 0; t < 2; t++) for (int i = 0; i < q->nsrc[t]; i++) if (q->src[t][i].tex && g_ntrash < 64) g_trash[g_ntrash++] = q->src[t][i].tex;
    for (int i = 0; i < 2 * FV_N; i++) fv_close(&g_fv[i]);
    memset(g_zoom, 0, sizeof g_zoom); g_notice.slot = -1;
    int res = q->res, fps = q->fps, br = q->br_mode, am = q->audio_mode; float bc = q->br_custom;
    memset(q, 0, sizeof *q);
    q->res = res; q->fps = fps; q->br_mode = br; q->audio_mode = am; q->br_custom = bc;
}

bool share_peer_live(const app_state_t *s, int slot) { return slot >= 0 && slot < BRIKS_MAX_PEERS && s->sh.remote[slot].on; }

static int slot_of(const app_state_t *s, const char *peer) {
    for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (s->rtc_peers[i].id[0] && !strcmp(s->rtc_peers[i].id, peer)) return i;
    return -1;
}

/* ---------- eventos do core ---------- */
static void fill_sources(share_state_t *q, int t, const jval *arr) {
    char keep[24] = ""; if (q->sel[t] >= 0 && q->sel[t] < q->nsrc[t]) copy(keep, sizeof keep, q->src[t][q->sel[t]].id);
    for (int i = 0; i < q->nsrc[t]; i++) if (q->src[t][i].tex && g_ntrash < 64) g_trash[g_ntrash++] = q->src[t][i].tex;
    memset(q->src[t], 0, sizeof q->src[t]); q->nsrc[t] = 0; q->sel[t] = 0;
    for (int i = 0; i < json_len(arr) && q->nsrc[t] < SH_MAX_SRC; i++) {
        const jval *e = json_at(arr, i); share_src_t *c = &q->src[t][q->nsrc[t]];
        copy(c->id, sizeof c->id, json_str(e, "id", ""));
        c->w = jint(e, "w"); c->h = jint(e, "h");
        copy(c->thumb, sizeof c->thumb, json_str(e, "thumb", ""));
        if (t == 0) { copy(c->name, sizeof c->name, json_str(e, "name", "Tela")); snprintf(c->sub, sizeof c->sub, "%d × %d%s", c->w, c->h, jbool(e, "primary") ? " · principal" : ""); }
        else { copy(c->name, sizeof c->name, json_str(e, "title", "Janela")); copy(c->sub, sizeof c->sub, json_str(e, "exe", "")); }
        if (keep[0] && !strcmp(keep, c->id)) q->sel[t] = q->nsrc[t];
        q->nsrc[t]++;
    }
}

bool share_handle_event(app_state_t *s, const jval *j, const char *ev) {
    share_state_t *q = &s->sh;
    if (!strcmp(ev, "share.sources")) {
        fill_sources(q, 0, json_get(j, "screens")); fill_sources(q, 1, json_get(j, "windows"));
        q->loading = false;
    } else if (!strcmp(ev, "share.audio.apps")) {
        share_app_t old[SH_MAX_APPS]; int nold = q->napps; memcpy(old, q->apps, sizeof old);
        const jval *arr = json_get(j, "apps"); q->napps = 0;
        for (int i = 0; i < json_len(arr) && q->napps < SH_MAX_APPS; i++) {
            const jval *e = json_at(arr, i);
            if (jbool(e, "excluded")) continue;          /* o próprio Briks e o Discord nunca entram */
            share_app_t *a = &q->apps[q->napps++]; memset(a, 0, sizeof *a);
            a->pid = (unsigned)jnum(e, "pid"); copy(a->name, sizeof a->name, json_str(e, "name", "")); a->playing = jbool(e, "playing");
            if (a->name[0] >= 'a' && a->name[0] <= 'z') a->name[0] -= 32;
            for (int k = 0; k < nold; k++) if (old[k].pid == a->pid) a->on = old[k].on;
        }
    } else if (jint(j, "slot") == 1 && (!strcmp(ev, "share.local") || !strcmp(ev, "share.stats") || !strcmp(ev, "share.hint") || !strcmp(ev, "share.rxstats") || !strcmp(ev, "share.remote"))) {
        /* 2ª tela (segundo monitor): estado próprio, mais enxuto que o da 1ª */
        if (!strcmp(ev, "share.local")) {
            if (!strcmp(json_str(j, "state", ""), "on")) { if (!q->second.active && s->screen == SCREEN_CALL) s->call_mode = 1;   /* começou a MINHA 2ª tela: vai para Transmissão (uma vez por início) */
                q->second.active = true; q->second.starting = false; q->second.w = jint(j, "w"); q->second.h = jint(j, "h"); q->second.fps = jint(j, "fps"); q->second.kbps = jint(j, "kbps"); }
            else { const char *why = json_str(j, "reason", ""); if (why[0]) copy(q->err, sizeof q->err, why); memset(&q->second, 0, sizeof q->second); fv_close(&g_fv[FV2(BRIKS_MAX_PEERS)]); }
        } else if (!strcmp(ev, "share.stats")) { q->second.kbps = jint(j, "kbps"); if (jint(j, "h")) q->second.h = jint(j, "h"); }
        else if (!strcmp(ev, "share.remote")) {
            int i = slot_of(s, json_str(j, "peer_id", "")); if (i < 0) return false;
            const char *st = json_str(j, "state", "");
            if (!strcmp(st, "on")) {
                if (share_notice_wanted(q->remote2[i].on || q->remote[i].on, true, share_viewing(s))) { g_notice.slot = i; g_notice.until = -1; }
                q->remote2[i].on = true; q->remote2[i].w = jint(j, "w"); q->remote2[i].h = jint(j, "h"); q->remote2[i].fps = jint(j, "fps"); q->remote2[i].kbps = jint(j, "kbps");
                copy(q->remote2[i].label, sizeof q->remote2[i].label, json_str(j, "label", ""));
            } else if (!strcmp(st, "off")) { memset(&q->remote2[i], 0, sizeof q->remote2[i]); fv_close(&g_fv[FV2(i)]); memset(&g_zoom[FV2(i)], 0, sizeof g_zoom[0]); }
            else if (!strcmp(st, "watch")) copy(q->remote2[i].path, sizeof q->remote2[i].path, json_str(j, "path", ""));
            else if (!strcmp(st, "frame")) { q->remote2[i].has_frame = true; q->remote2[i].w = jint(j, "w"); q->remote2[i].h = jint(j, "h"); }
        }
    } else if (!strcmp(ev, "share.local")) {
        if (!strcmp(json_str(j, "state", ""), "on")) {
            if (!q->active && s->screen == SCREEN_CALL) s->call_mode = 1;   /* começou a MINHA transmissão: aba Transmissão, uma vez por início (o usuário pode voltar e não é puxado de novo) */
            q->active = true; q->starting = false;
            q->w = jint(j, "w"); q->h = jint(j, "h"); q->cur_fps = jint(j, "fps"); q->kbps = jint(j, "kbps");
            q->audio_on = jbool(j, "audio"); q->hardware = jbool(j, "hardware");
            copy(q->encoder, sizeof q->encoder, json_str(j, "encoder", ""));
        } else {
            q->active = false; q->starting = false; q->black = false; q->adapting[0] = 0; q->live_h = 0; q->stat_fps = 0; q->stat_kbps = 0;
            share_stop_second(s);   /* a 2ª tela só existe junto com a 1ª */
            copy(q->err, sizeof q->err, json_str(j, "reason", ""));
        }
    } else if (!strcmp(ev, "share.hint")) {
        q->black = !strcmp(json_str(j, "hint", ""), "black");
    } else if (!strcmp(ev, "share.stats")) {
        q->stat_fps = jnum(j, "fps"); q->stat_kbps = jint(j, "kbps"); q->enc_ms = jnum(j, "enc_ms"); q->cpu = jint(j, "cpu_pct");
        q->audio_peak = jnum(j, "audio_peak"); q->audio_sources = jint(j, "audio_sources");
        q->live_h = jint(j, "h"); q->target_fps = jint(j, "target_fps"); q->target_kbps = jint(j, "target_kbps"); q->est_kbps = jint(j, "est_kbps");
        q->rtt_ms = jint(j, "rtt_ms"); q->rtx = jint(j, "rtx"); q->loss_pct = jnum(j, "loss_pct");
        copy(q->adapting, sizeof q->adapting, json_str(j, "adapting", "")); copy(q->reason, sizeof q->reason, json_str(j, "reason", ""));
    } else if (!strcmp(ev, "share.rxstats")) {
        /* quem assiste: com menos de 3 quadros por segundo por 4 s seguidos, a conexão não está dando conta (o emissor não baixa a
           qualidade por um espectador só quando há outros bem; avisa em vez de deixar a imagem parada sem explicação) */
        int i = slot_of(s, json_str(j, "peer_id", "")); if (i < 0 || !q->remote[i].watching) return false;
        if (jint(j, "fps") < 3) { if (q->remote[i].low_secs < 99) q->remote[i].low_secs++; } else q->remote[i].low_secs = 0;
        q->remote[i].poor = q->remote[i].low_secs >= 4;
    } else if (!strcmp(ev, "share.remote")) {
        int i = slot_of(s, json_str(j, "peer_id", "")); if (i < 0) return false;
        const char *st = json_str(j, "state", "");
        if (!strcmp(st, "on")) {
            if (share_notice_wanted(q->remote[i].on, true, share_viewing(s))) { g_notice.slot = i; g_notice.until = -1; }
            q->remote[i].on = true; q->remote[i].w = jint(j, "w"); q->remote[i].h = jint(j, "h"); q->remote[i].fps = jint(j, "fps"); q->remote[i].kbps = jint(j, "kbps");
            q->remote[i].audio = jbool(j, "audio"); copy(q->remote[i].label, sizeof q->remote[i].label, json_str(j, "label", ""));
        } else if (!strcmp(st, "off")) { q->remote[i].on = false; q->remote[i].watching = false; q->remote[i].has_frame = false; q->remote[i].poor = false; q->remote[i].low_secs = 0; fv_close(&g_fv[i]);
            memset(&g_zoom[i], 0, sizeof g_zoom[i]); if (g_notice.slot == i) g_notice.slot = -1; }
        else if (!strcmp(st, "watch")) copy(q->remote[i].path, sizeof q->remote[i].path, json_str(j, "path", ""));
        else if (!strcmp(st, "frame")) { q->remote[i].has_frame = true; q->remote[i].w = jint(j, "w"); q->remote[i].h = jint(j, "h"); }
    } else return false;
    return true;
}

/* quadro sintético para as capturas (grade de 80 px com um bloco de cor por célula; `tint` muda a paleta) */
static void demo_frame(char *path, size_t cap, const char *name, int W, int H, int tint) {
    const char *tmp = getenv("TEMP");
    snprintf(path, cap, "%s\\briks-demo-%s-%lu.bin", tmp ? tmp : ".", name, (unsigned long)GetCurrentProcessId());
    FILE *f = fopen(path, "wb"); if (!f) return;
    unsigned char hd[64] = { 'B', 'K', 'F', 'R' }; unsigned long long n = 1;
    memcpy(hd + 8, &W, 4); memcpy(hd + 12, &H, 4); memcpy(hd + 16, &n, 8); fwrite(hd, 1, 64, f);
    unsigned char *row = malloc((size_t)W * 4);
    for (int y = 0; y < H && row; y++) {
        for (int x = 0; x < W; x++) {
            int cx = x / 80, cy = y / 80; bool line = x % 80 < 2 || y % 80 < 2, dot = x % 80 > 30 && x % 80 < 50 && y % 80 > 30 && y % 80 < 50;
            unsigned char *p = row + x * 4;
            unsigned char r = line ? 235 : dot ? (unsigned char)(40 + cx * 13) : (unsigned char)(24 + ((cx + cy) & 1) * 22);
            unsigned char g = line ? 235 : dot ? (unsigned char)(220 - cy * 20) : (unsigned char)(18 + ((cx + cy) & 1) * 16);
            unsigned char bl = line ? 245 : dot ? 200 : (unsigned char)(48 + ((cx + cy) & 1) * 30);
            p[0] = tint == 1 ? g : tint == 2 ? bl : r; p[1] = tint == 1 ? bl : tint == 2 ? r : g; p[2] = tint == 1 ? r : tint == 2 ? g : bl; p[3] = 255;
        }
        fwrite(row, 1, (size_t)W * 4, f);
    }
    free(row); fclose(f);
}

void share_tick(cui_ctx *ui, app_state_t *s) {
    share_state_t *q = &s->sh;
    for (int i = 0; i < g_ntrash; i++) cui_image_destroy(ui, g_trash[i]);
    g_ntrash = 0;
    if (!q->fps && !q->res && !q->br_custom) { q->res = 1; q->fps = FPS_DEFAULT; q->br_custom = 8; }
    if (q->fps < 0 || q->fps >= FPS_N) q->fps = FPS_DEFAULT;   /* padrão: 1080p · 30 FPS · automático */
    /* só assiste no modo Transmissão (aba Chamada): fora dele o core nem decodifica */
    bool viewing = share_viewing(s);
    for (int i = 0; i < BRIKS_MAX_PEERS; i++) {
        if (!s->rtc_peers[i].id[0]) {
            if (q->remote[i].on || q->remote[i].watching) { memset(&q->remote[i], 0, sizeof q->remote[i]); fv_close(&g_fv[i]); memset(&g_zoom[i], 0, sizeof g_zoom[i]); }
            if (q->remote2[i].on || q->remote2[i].watching) { memset(&q->remote2[i], 0, sizeof q->remote2[i]); fv_close(&g_fv[FV2(i)]); memset(&g_zoom[FV2(i)], 0, sizeof g_zoom[0]); }
            continue;
        }
        bool want2 = viewing && q->remote2[i].on;
        if (want2 != q->remote2[i].watching) {
            char line[220], id[96]; json_escape(s->rtc_peers[i].id, id, sizeof id);
            snprintf(line, sizeof line, "{\"cmd\":\"share.watch\",\"slot\":1,\"peer\":\"%s\",\"on\":%s}", id, want2 ? "true" : "false");
            cmd(line);
            q->remote2[i].watching = want2;
            if (!want2) { q->remote2[i].has_frame = false; fv_close(&g_fv[FV2(i)]); }
        }
        bool want = viewing && q->remote[i].on;
        if (want != q->remote[i].watching) {
            char line[200], id[96]; json_escape(s->rtc_peers[i].id, id, sizeof id);
            snprintf(line, sizeof line, "{\"cmd\":\"share.watch\",\"peer\":\"%s\",\"on\":%s}", id, want ? "true" : "false");
            cmd(line);
            q->remote[i].watching = want;
            if (!want) { q->remote[i].has_frame = false; fv_close(&g_fv[i]); }
        }
    }
    /* capturas sem core (BRIKS_DEMO_PEERS): BRIKS_DEV_SHARE=demo-remote faz o 1º participante "começar a transmitir" (aviso);
       demo-zoom faz o mesmo já no modo Transmissão, com um quadro sintético (grade) para provar o zoom; demo-two põe DUAS telas
       da mesma pessoa no ar */
    { static int demo; const char *dd = getenv("BRIKS_DEV_SHARE");
      if (!demo && dd && !strncmp(dd, "demo-", 5) && s->rtc_peers[0].id[0]) {
        demo = 1;
        if (!strcmp(dd, "demo-zoom") || !strcmp(dd, "demo-two") || !strcmp(dd, "demo-many")) { s->screen = SCREEN_CALL; s->call_mode = 1; s->call_view = 0; }
        jval *jj = json_parse("{\"ev\":\"share.remote\",\"peer_id\":\"demo-0\",\"state\":\"on\",\"w\":1280,\"h\":720,\"fps\":30,\"kbps\":4000,\"label\":\"Tela 2\"}");
        if (jj) { share_handle_event(s, jj, "share.remote"); json_free(jj); }
        if (!strcmp(dd, "demo-zoom") || !strcmp(dd, "demo-two") || !strcmp(dd, "demo-many")) demo_frame(q->remote[0].path, sizeof q->remote[0].path, "tela1", 1280, 720, 0);
        if (!strcmp(dd, "demo-two") || !strcmp(dd, "demo-many")) {   /* a mesma pessoa com DUAS telas no ar */
            jval *j2 = json_parse("{\"ev\":\"share.remote\",\"peer_id\":\"demo-0\",\"slot\":1,\"state\":\"on\",\"w\":960,\"h\":540,\"fps\":30,\"kbps\":2500,\"label\":\"Tela 3\"}");
            if (j2) { share_handle_event(s, j2, "share.remote"); json_free(j2); }
            demo_frame(q->remote2[0].path, sizeof q->remote2[0].path, "tela2", 960, 540, 1);
        }
        if (!strcmp(dd, "demo-many")) {   /* mais duas pessoas transmitindo: 4 blocos no palco (grade / foco) */
            for (int k = 1; k <= 2; k++) if (s->rtc_peers[k].id[0]) {
                char js[260]; snprintf(js, sizeof js, "{\"ev\":\"share.remote\",\"peer_id\":\"demo-%d\",\"state\":\"on\",\"w\":1280,\"h\":720,\"fps\":30,\"kbps\":3000,\"label\":\"Tela %d\"}", k, k + 3);
                jval *jk = json_parse(js); if (jk) { share_handle_event(s, jk, "share.remote"); json_free(jk); }
                char nm[16]; snprintf(nm, sizeof nm, "tela-p%d", k);
                demo_frame(q->remote[k].path, sizeof q->remote[k].path, nm, 1280, 720, k == 1 ? 2 : 1);
            }
        }
      }
    }
    /* ganchos de teste/captura: BRIKS_DEV_SHARE=dialog | dialog-apps | dialog-windows | screen | test (uma vez, com o core pronto) */
    static int dev; const char *d = getenv("BRIKS_DEV_SHARE");
    if (d) cui_request_redraw(ui);
    if (d && !strcmp(d, "fake-local")) {   /* teste sem core: começa/para/recomeça a MINHA transmissão pelo mesmo tratador de eventos do core (1 s liga, 4 s desliga, 7 s liga, 9 s repete o "on") */
        static int step; double t = cui_time(ui); const char *ev = NULL;
        if (step == 0 && t > 1) { step = 1; ev = "{\"ev\":\"share.local\",\"slot\":0,\"state\":\"on\",\"w\":1280,\"h\":720,\"fps\":30,\"kbps\":2500}"; }
        else if (step == 1 && t > 4) { step = 2; ev = "{\"ev\":\"share.local\",\"slot\":0,\"state\":\"off\",\"reason\":\"\"}"; }
        else if (step == 2 && t > 7) { step = 3; ev = "{\"ev\":\"share.local\",\"slot\":0,\"state\":\"on\",\"w\":1280,\"h\":720,\"fps\":30,\"kbps\":2500}"; }
        else if (step == 3 && t > 9) { step = 4; ev = "{\"ev\":\"share.local\",\"slot\":0,\"state\":\"on\",\"w\":1280,\"h\":720,\"fps\":30,\"kbps\":2500}"; }
        if (ev) { jval *jj = json_parse(ev); if (jj) { share_handle_event(s, jj, "share.local"); json_free(jj); } }
        cui_request_redraw(ui);
    }
    if (d && !strcmp(d, "demo-quality")) { static int once; if (!once && cui_time(ui) > 1.0) { once = 1; g_dev_open_quality = true; } }   /* captura sem core: abre a janela de qualidade */
    if (d && core_state() == CORE_READY && s->in_room) {
        if (!strcmp(d, "adapt") && q->active) { static double t0; if (t0 == 0) t0 = cui_time(ui); if (cui_time(ui) - t0 > 3 && dev < 3) { dev = 3; g_dev_open_quality = true; } }
        if (dev == 0 && !strcmp(d, "dialog-quality")) { q->quality_open = true; q->br_mode = 3; q->br_custom = 35; q->res = 2; q->fps = FPS_N - 1; }
        if (dev == 0) { dev = 1; if (!strncmp(d, "dialog", 6) || !strcmp(d, "screen") || !strcmp(d, "adapt")) share_open_dialog(s);
            if (!strcmp(d, "dialog-apps")) q->audio_mode = 1; if (!strcmp(d, "dialog-windows")) q->tab = 1;
            if (!strcmp(d, "test")) send_start(s, "test", "0", "Padrão de teste"); }
        else if (dev == 1 && (!strcmp(d, "screen") || !strcmp(d, "adapt")) && !q->loading && q->nsrc[0] > 0 && s->rtc_authed_count > 0) { dev = 2; q->tab = 0; q->sel[0] = 0; start_selected(s); }
        if (dev == 1 && !strcmp(d, "dialog-apps") && q->napps > 0) { dev = 2; q->apps[0].on = true; }
    }
}

/* ---------- peças de interface ---------- */
static void paint_grad(cui_ctx *ui, cui_rect r, void *user) { float rad = *(const float *)user; cui_draw_rect_gradient(ui, r, CUI_CORNERS(rad), B_C1, B_C2); }

/* segmentado em vidro: ativa = pílula em degradê; devolve true se mudou */
static bool seg(cui_ctx *ui, const char *id, const char *const *labels, const int *counts, int n, int *idx) {
    static float rad = 8; bool changed = false;
    cui_push_id(ui, id);
    cui_box(ui, .dir = CUI_ROW, .gap = 3, .pad = 3, .radius = 11, .bg = B_GLASS, .border = 1, .border_color = B_GLASS_BORDER) {
        for (int i = 0; i < n; i++) {
            bool on = *idx == i;
            cui_push_id_int(ui, i);
            cui_box(ui, .id = "s", .dir = CUI_STACK, .height = CUI_PX(28), .radius = 8, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                if (on) cui_canvas(ui, "bg", CUI_FILL, CUI_FILL, paint_grad, &rad, 0);
                cui_box(ui, .dir = CUI_ROW, .gap = 6, .pad_x = 12, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                    cui_text(ui, labels[i], .size = 13, .color = on ? CUI_RGB(0xffffff) : B_MUTED, .nowrap = true);
                    if (counts) { char c[12]; snprintf(c, sizeof c, "%d", counts[i]); cui_text(ui, c, .size = 11, .color = on ? CUI_RGBA(0xffffffb0) : CUI_RGBA(0xa592b888)); }
                }
            }
            if (briks_clicked_r(ui, "s", 8.0f) && !on) { *idx = i; changed = true; }
            cui_pop_id(ui);
        }
    }
    cui_pop_id(ui);
    return changed;
}

#define CARD_W 232.0f
#define CARD_IMG_W 226.0f
#define CARD_IMG_H 127.0f
static bool src_card(cui_ctx *ui, share_src_t *c, bool selected, int idx) {
    if (!c->tex && !c->tex_tried && c->thumb[0]) { c->tex_tried = true; c->tex = cui_image_load(ui, c->thumb, NULL, NULL); }
    cui_push_id_int(ui, idx);
    cui_column(ui, .gap = 6, .width = CUI_PX(CARD_W)) {
        /* moldura arredondada; a miniatura tem o próprio raio e fica 3 px para dentro: nenhum canto reto encosta na borda curva */
        cui_box(ui, .id = "c", .dir = CUI_STACK, .width = CUI_PX(CARD_W), .height = CUI_PX(CARD_IMG_H + 6), .radius = 12, .bg = CUI_RGBA(0x0f0316b0),
                .border = selected ? 2 : 1, .border_color = selected ? CUI_RGB(0x8b5cf6) : B_GLASS_BORDER) {
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                if (c->tex) cui_image(ui, c->tex, .width = CUI_PX(CARD_IMG_W), .height = CUI_PX(CARD_IMG_H), .radius = 9);
                else cui_icon(ui, CUI_ICON_MONITOR, 26, B_MUTED);
            }
            if (selected) cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_END, .justify = CUI_JUSTIFY_START, .pad = 8) {
                cui_box(ui, .width = CUI_PX(20), .height = CUI_PX(20), .radius = 10, .bg = CUI_RGB(0x8b5cf6), .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                    cui_icon(ui, CUI_ICON_CHECK, 12, CUI_RGB(0xffffff));
                }
            }
        }
        cui_column(ui, .gap = 1, .pad_x = 4, .width = CUI_PX(CARD_W)) {
            cui_text(ui, c->name, .size = 13, .color = selected ? CUI_RGB(0xffffff) : B_TEXT, .nowrap = true);
            if (c->sub[0]) cui_text(ui, c->sub, .size = 11, .color = B_MUTED, .nowrap = true);
        }
    }
    bool hit = briks_clicked(ui, "c");
    cui_pop_id(ui);
    return hit;
}

/* Resolução, FPS e bitrate, independentes. Devolve true se algo mudou. */
/* Aviso de limite conhecido (recurso em beta): fica visível no seletor e no painel de qualidade, não escondido. */
static void beta_note(cui_ctx *ui, const char *text) {
    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .gap = 10, .align = CUI_ALIGN_CENTER) {
        briks_status_pill(ui, "Beta");
        cui_box(ui, .width = CUI_FILL) { cui_text(ui, text, .size = 12, .color = CUI_RGB(0xf5d9a0)); }
    }
}

/* linha de texto que quebra dentro da largura do painel em vez de passar da borda */
static void wrap_line(cui_ctx *ui, const char *t, float size, cui_color col) {
    cui_box(ui, .width = CUI_FILL) { cui_text(ui, t, .size = size, .color = col); }
}

static bool quality_controls(cui_ctx *ui, share_state_t *q) {
    bool ch = false;
    cui_column(ui, .gap = 10, .width = CUI_FILL) {
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, "Resolução", .size = 13, .color = B_MUTED);
            ch |= seg(ui, "res", RES_LABEL, NULL, 4, &q->res);
        }
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, "Quadros por segundo", .size = 13, .color = B_MUTED);
            ch |= seg(ui, "fps", FPS_LABEL, NULL, FPS_N, &q->fps);
        }
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
            cui_column(ui, .gap = 1, .width = CUI_FILL) {
                wrap_line(ui, "Se a rede ou o PC apertar", 13, B_MUTED);
                wrap_line(ui, q->content ? "Fluidez: cede resolução antes de quadros" : "Nitidez: cede quadros antes de resolução", 11, CUI_RGBA(0xa592b8aa));
            }
            static const char *const pr[2] = { "Nitidez", "Fluidez" };
            ch |= seg(ui, "pri", pr, NULL, 2, &q->content);
        }
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, "Bitrate", .size = 13, .color = B_MUTED);
            ch |= seg(ui, "br", BR_LABEL, NULL, 4, &q->br_mode);
        }
        if (q->br_mode == 3) {
            cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_CENTER) {
                cui_box(ui, .width = CUI_FILL) { ch |= cui_slider(ui, "##brc", &q->br_custom, 1, 50, .step = 1); }
                char t[32]; snprintf(t, sizeof t, "%d Mbps", (int)(q->br_custom + 0.5f));
                cui_text(ui, t, .size = 13, .nowrap = true);
            }
        }
        int k = chosen_kbps(q); char r[24], line[200];
        fmt_rate(k ? k : auto_kbps(RES_H[q->res], FPS_V[q->fps]), r, sizeof r);
        if (k > 20000) {
            cui_box(ui, .dir = CUI_ROW, .gap = 8, .pad_x = 10, .pad_y = 8, .radius = 10, .bg = CUI_RGBA(0xf5a52422), .border = 1, .border_color = CUI_RGBA(0xf5a52466), .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
                cui_icon(ui, CUI_ICON_TRIANGLE_ALERT, 14, CUI_RGB(0xf5b942));
                snprintf(line, sizeof line, "%s é muito: quem assiste precisa de uma conexão rápida e o envio pesa na sua rede.", r);
                cui_text(ui, line, .size = 12, .color = CUI_RGB(0xf5d9a0));
            }
        } else {
            snprintf(line, sizeof line, q->br_mode == 0 ? "Automático: até %s; baixa e volta sozinho conforme a rede." : "Teto de %s; o ajuste automático trabalha abaixo dele.", r);
            wrap_line(ui, line, 12, B_MUTED);
        }
    }
    return ch;
}

/* ---------- diálogo ---------- */
void ui_share_dialog(cui_ctx *ui, app_state_t *s) {
    share_state_t *q = &s->sh;
    if (!q->dialog_open) return;
    static double last_poll;   /* "Tocando agora" atualiza sozinho enquanto o diálogo está aberto */
    if (q->audio_mode == 1 && cui_time(ui) - last_poll > 2.0) { last_poll = cui_time(ui); cmd("{\"cmd\":\"share.audio.list\"}"); cui_request_redraw(ui); }
    cui_vec2 vp = cui_viewport_size(ui);
    float inner = 3 * CARD_W + 2 * 12;
    cui_dialog(ui, "Compartilhar tela (beta)", &q->dialog_open, .description = q->pick_second ? "Escolha a 2ª tela: ela vai ao ar junto com a primeira" : "Escolha a fonte e confirme", .width = fminf(inner + 48, fmaxf(vp.x - 48, 520))) {
        int t = q->tab, n = q->nsrc[t];
        /* com os ajustes de qualidade abertos, fontes e áudio viram uma linha de resumo: o diálogo tem de caber em 800 px de altura */
        if (!q->quality_open) {
        static const char *const tabs[2] = { "Telas", "Janelas" };
        cui_row(ui) { if (seg(ui, "tab", tabs, q->nsrc, 2, &q->tab)) { t = q->tab; n = q->nsrc[t]; } }
        if (q->err[0]) cui_text(ui, q->err, .size = 13, .color = B_DANGER);
        else if (q->loading) cui_text(ui, "Procurando telas e janelas…", .size = 13, .color = B_MUTED);
        else if (!n) cui_text(ui, t == 0 ? "Nenhuma tela encontrada." : "Nenhuma janela aberta para compartilhar.", .size = 13, .color = B_MUTED);
        else {
            int rows = (n + 2) / 3;
            float row_h = CARD_IMG_H + 6 + 6 + 34;
            cui_box(ui, .width = CUI_FILL, .height = CUI_PX(rows > 1 ? row_h * 1.75f : row_h), .gap = 12, .scroll_y = rows > 1) {
                for (int r = 0; r < rows; r++) {
                    cui_push_id_int(ui, 100 + r);
                    cui_row(ui, .gap = 12) {
                        for (int i = r * 3; i < n && i < r * 3 + 3; i++) if (src_card(ui, &q->src[t][i], q->sel[t] == i, i)) q->sel[t] = i;
                    }
                    cui_pop_id(ui);
                }
            }
        }
        cui_separator(ui);
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, "Áudio da transmissão", .font = CUI_FONT_DISPLAY, .size = 14);
            static const char *const am[3] = { "Da fonte", "Escolher apps", "Sem áudio" };
            if (seg(ui, "am", am, NULL, 3, &q->audio_mode) && q->audio_mode == 1) cmd("{\"cmd\":\"share.audio.list\"}");
        }
        if (q->audio_mode == 1) {
            int marked = 0;
            if (!q->napps) cui_text(ui, "Nenhum app com áudio aberto agora.", .size = 13, .color = B_MUTED);
            cui_box(ui, .width = CUI_FILL, .gap = 6, .height = q->napps > 3 ? CUI_PX(3 * 50 + 12) : CUI_FIT, .scroll_y = q->napps > 3) {
                for (int i = 0; i < q->napps; i++) {
                    share_app_t *a = &q->apps[i]; if (a->on) marked++;
                    cui_push_id_int(ui, 300 + i);
                    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(46), .pad_x = 12, .gap = 10, .radius = 10, .bg = B_GLASS_SOFT, .border = 1, .border_color = B_GLASS_BORDER_SOFT, .align = CUI_ALIGN_CENTER) {
                        cui_box(ui, .width = CUI_PX(28), .height = CUI_PX(28), .radius = 8, .bg = CUI_RGBA(0x8b5cf633), .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) { cui_icon(ui, CUI_ICON_MUSIC, 14, B_C3); }
                        cui_column(ui, .gap = 1, .width = CUI_FILL) {
                            cui_text(ui, a->name, .size = 13, .nowrap = true);
                            cui_text(ui, a->playing ? "Tocando agora" : "Em silêncio", .size = 11, .color = a->playing ? B_ONLINE : B_MUTED);
                        }
                        cui_switch(ui, "##on", &a->on);
                    }
                    cui_pop_id(ui);
                }
            }
            cui_text(ui, marked ? "Só o som dos apps marcados entra na transmissão. O Briks e o Discord nunca entram."
                                : "Nenhum app marcado: a transmissão sai sem áudio. O Briks e o Discord nunca entram.", .size = 12, .color = B_MUTED);
        } else if (q->audio_mode == 0) {
            cui_text(ui, t == 0 ? "Entra o som de tudo que toca no PC, menos o Briks e o Discord." : "Entra só o som do programa dono da janela.", .size = 12, .color = B_MUTED);
        } else cui_text(ui, "A transmissão sai sem áudio.", .size = 12, .color = B_MUTED);
        cui_separator(ui);
        } else {
            char sum[220];
            snprintf(sum, sizeof sum, "Fonte: %s · Áudio: %s", n > 0 && q->sel[t] < n ? q->src[t][q->sel[t]].name : "nenhuma",
                     q->audio_mode == 2 ? "sem áudio" : q->audio_mode == 1 ? "só os apps marcados" : "da fonte");
            cui_text(ui, sum, .size = 13, .color = B_MUTED, .nowrap = true);
        }
        char chip[80]; { share_state_t tmp = *q; tmp.active = false; quality_chip(&tmp, chip, sizeof chip); }
        /* qualidade recolhida por padrão (o chip resume); "Ajustar" abre resolução, FPS e bitrate */
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
            cui_row(ui, .gap = 12, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, "Qualidade", .font = CUI_FONT_DISPLAY, .size = 14);
                briks_status_pill(ui, chip);
            }
            if (cui_button(ui, q->quality_open ? "Fechar ajustes" : "Ajustar", .size = CUI_SIZE_SM)) q->quality_open = !q->quality_open;
        }
        if (q->quality_open) quality_controls(ui, q);
        cui_separator(ui);
        beta_note(ui, "Em queda brusca de rede a imagem pode cair bastante ou travar por alguns segundos antes de voltar.");
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, q->audio_mode == 2 ? "Sem áudio" : q->audio_mode == 1 ? "Áudio: só os apps marcados" : "Áudio: da fonte", .size = 12, .color = B_MUTED);
            char b[140]; bool ok = n > 0 && q->sel[t] < n && !q->loading;
            if (ok) snprintf(b, sizeof b, t == 0 ? "Transmitir %s" : "Transmitir janela", q->src[t][q->sel[t]].name); else snprintf(b, sizeof b, "Transmitir");
            if (cui_button(ui, b, .variant = CUI_BUTTON_PRIMARY, .disabled = !ok)) start_selected(s);
        }
    }
}

/* ---------- barra da call ---------- */
static void vsep(cui_ctx *ui) { cui_box(ui, .width = CUI_PX(1), .height = CUI_PX(26), .bg = CUI_RGBA(0xffffff1c)) {} }

void ui_share_bar(cui_ctx *ui, app_state_t *s) {
    share_state_t *q = &s->sh;
    bool on = q->active || q->starting;
    cui_color bg = on ? CUI_RGBA(0x57d28b26) : CUI_RGBA(0xffffff12), bd = on ? CUI_RGBA(0x57d28b88) : B_GLASS_BORDER, fg = on ? CUI_RGB(0x7be8a8) : B_TEXT;
    cui_push_id_int(ui, 4);
    /* um único contorno arredondado com as três partes dentro (sem fundos próprios): nada vaza nos cantos */
    cui_box(ui, .dir = CUI_ROW, .height = CUI_PX(50), .radius = 25, .bg = bg, .border = 1, .border_color = bd, .align = CUI_ALIGN_CENTER) {
        cui_box(ui, .id = "b", .dir = CUI_ROW, .gap = 8, .pad_x = 18, .height = CUI_FILL, .align = CUI_ALIGN_CENTER) {
            cui_icon(ui, CUI_ICON_MONITOR, 18, fg);
            cui_text(ui, q->starting ? "Iniciando…" : q->active ? "Parar transmissão" : "Compartilhar (beta)", .font = CUI_FONT_DISPLAY, .size = 15, .color = fg);
        }
        /* com o seletor aberto a barra fica por baixo dele: um clique no diálogo não pode valer também aqui */
        bool modal = q->dialog_open || s->kick_open;
        if (!modal && briks_clicked_r(ui, "b", 25.0f)) { if (on) share_stop(s); else share_open_dialog(s); }
        if (q->active) {   /* segundo monitor ao mesmo tempo: abre o mesmo seletor para escolher a 2ª tela */
            bool on2 = q->second.active || q->second.starting;
            vsep(ui);
            cui_box(ui, .id = "b2", .dir = CUI_ROW, .gap = 6, .pad_x = 12, .height = CUI_FILL, .align = CUI_ALIGN_CENTER) {
                cui_icon(ui, CUI_ICON_MONITOR, 14, on2 ? CUI_RGB(0x7be8a8) : B_TEXT);
                cui_text(ui, q->second.starting ? "Iniciando 2ª…" : q->second.active ? "Parar 2ª tela" : "+ 2ª tela", .size = 12, .color = on2 ? CUI_RGB(0x7be8a8) : B_TEXT, .nowrap = true);
            }
            if (!modal && briks_clicked_r(ui, "b2", 25.0f)) { if (on2) share_stop_second(s); else { share_open_dialog(s); q->pick_second = true; } }
        }
        if (q->active) {
            vsep(ui);
            cui_box(ui, .dir = CUI_ROW, .gap = 6, .pad_x = 12, .height = CUI_FILL, .align = CUI_ALIGN_CENTER) {
                cui_icon(ui, q->audio_on ? CUI_ICON_VOLUME_2 : CUI_ICON_VOLUME_X, 14, q->audio_on ? CUI_RGB(0x7be8a8) : B_MUTED);
                cui_text(ui, q->audio_on ? "Com áudio" : "Sem áudio", .size = 12, .color = q->audio_on ? CUI_RGB(0x7be8a8) : B_MUTED, .nowrap = true);
            }
        }
        vsep(ui);
        char chip[80]; quality_chip(q, chip, sizeof chip);
        bool failed = q->err[0] && !on && !q->dialog_open;   /* motivo do fim ("A janela foi fechada"...) fica no chip até a próxima ação */
        /* chip + âncora + popover num mesmo empilhamento (como o popover do Áudio): a janela abre colada ao chip, acima dele */
        cui_box(ui, .dir = CUI_STACK, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_START) {
        cui_box(ui, .id = "q", .dir = CUI_ROW, .gap = 6, .pad_x = 14, .height = CUI_FILL, .align = CUI_ALIGN_CENTER) {
            bool adapting = q->active && (q->adapting[0] || q->black);   /* a rede ou o PC apertou: o chip mostra o que está saindo de fato */
            if (failed) cui_icon(ui, CUI_ICON_TRIANGLE_ALERT, 13, B_DANGER);
            else if (adapting) cui_icon(ui, q->black ? CUI_ICON_TRIANGLE_ALERT : !strcmp(q->adapting, "cpu") ? CUI_ICON_CPU : CUI_ICON_TRENDING_DOWN, 13, CUI_RGB(0xf5b942));
            cui_text(ui, failed ? q->err : (q->active && q->black) ? "Imagem preta" : chip, .size = 12, .color = failed ? CUI_RGB(0xffb4b4) : adapting ? CUI_RGB(0xf5d9a0) : B_TEXT, .nowrap = true);
        }
        if ((!modal && briks_clicked_r(ui, "q", 25.0f)) || g_dev_open_quality) { g_dev_open_quality = false; q->err[0] = 0; cui_popover_open(ui, "shq"); }
            cui_canvas(ui, "shqanchor", CUI_FILL, CUI_PX(1), NULL, NULL, 0);   /* âncora: topo do chip, com a largura dele */
        cui_popover(ui, "shq", .placement = CUI_PLACE_TOP, .width = 520, .pad = 16, .gap = 10) {
            cui_text(ui, "Qualidade da transmissão", .font = CUI_FONT_DISPLAY, .size = 15);
            beta_note(ui, "Em queda brusca de rede a imagem pode cair bastante ou travar por alguns segundos. Com mais de um espectador, quem estiver com a rede ruim pode ficar sem imagem.");
            /* as escolhas valem sozinhas: transmitindo, ~0,4 s depois da última mudança o codificador é reiniciado com elas (um clique
               seguido de outro não reinicia duas vezes); fora da transmissão ficam salvas para a próxima */
            static double apply_at = -1;
            if (quality_controls(ui, q) && q->active) apply_at = cui_time(ui) + 0.4;
            if (apply_at >= 0) { if (!q->active) apply_at = -1; else if (cui_time(ui) >= apply_at) { apply_at = -1; send_start(s, q->last_kind, q->last_id, q->label); } else cui_request_wakeup(ui, 0.2f); }
            if (q->active) {
                /* telemetria local, ao vivo */
                char st[240];
                snprintf(st, sizeof st, "No ar: %s · %.0f FPS · %d kbps (alvo %d FPS · %d kbps)", res_name(q->live_h ? q->live_h : q->h), q->stat_fps, q->stat_kbps, q->target_fps, q->target_kbps);
                wrap_line(ui, st, 12, B_TEXT);
                snprintf(st, sizeof st, "Rede: estimada %d kbps · perda %.1f%% · RTT %d ms · reenvios %d/s", q->est_kbps, q->loss_pct, q->rtt_ms, q->rtx);
                wrap_line(ui, st, 12, B_MUTED);
                snprintf(st, sizeof st, "Codificador: %s%s · %.1f ms/quadro · CPU %d%%", q->encoder[0] ? q->encoder : "H264", q->hardware ? " (GPU)" : " (software)", q->enc_ms, q->cpu);
                wrap_line(ui, st, 12, B_MUTED);
                if (q->black) cui_text(ui, "A fonte está saindo preta. Jogos em tela cheia exclusiva e conteúdo protegido não podem ser capturados: use janela sem bordas.", .size = 12, .color = CUI_RGB(0xf5d9a0));
                if (q->adapting[0]) {
                    snprintf(st, sizeof st, !strcmp(q->adapting, "cpu") ? "Adaptando: o PC não deu conta; qualidade reduzida até caber." : "Adaptando à rede%s%s: qualidade reduzida, volta sozinha.", q->reason[0] ? " (" : "", q->reason[0] ? (!strcmp(q->reason, "perda") ? "perda)" : "atraso)") : "");
                    wrap_line(ui, st, 12, CUI_RGB(0xf5d9a0));
                }
            }
        }
        }
    }
    cui_pop_id(ui);
}

/* ---------- palco do modo Transmissão ---------- */
static int g_sfocus = -1;   /* transmissão fixada no Foco (chave de emit_tile); -1 = a primeira da lista */
static bool s_modal_hint;   /* com um diálogo aberto por cima, o clique não vale para a miniatura */
static bool video_tile(cui_ctx *ui, int key, cui_texture tex, int tw, int th, const char *title, const char *chip, const char *wait, float *volume, cui_image_view *zv, bool compact) {
    int zoom_pct = 100; bool clicked = false;
    if (compact) { chip = NULL; volume = NULL; zv = NULL; }   /* miniatura da faixa do Foco: só a imagem e o nome; clicar traz para o destaque */
    if (key >= 0 && key < 2 * FV_N) g_zoom_pct[key] = 100;
    g_tiles++;
    cui_push_id_int(ui, 800 + key);
    cui_rect r = cui_get_rect(ui, cui_make_id(ui, "vt"));   /* tamanho do quadro anterior: o vídeo cabe inteiro, sem distorcer */
    cui_box(ui, .id = "vt", .dir = CUI_STACK, .width = CUI_FILL, .height = CUI_FILL, .radius = 20, .bg = CUI_RGBA(0x1a0a2680), .border = 1, .border_color = CUI_RGBA(0x6d5cf655)) {
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .gap = 10) {
            if (tex && tw > 0 && th > 0 && r.w > 60 && r.h > 60) {
                float aw = r.w - 16, ah = r.h - 16, sc = fminf(aw / tw, ah / th);
                /* a imagem tem o próprio raio e fica 8 px para dentro da moldura (raio 20): cantos sempre curvos, nada reto vaza */
                if (zv) {
                    /* quem assiste: roda do mouse ou pinça ampliam em torno do ponteiro, arrastar move; a moldura fica do tamanho
                       da imagem encaixada (cantos curvos, nada vaza) e o zoom nunca fica abaixo do encaixe */
                    cui_box(ui, .width = CUI_PX(floorf(tw * sc)), .height = CUI_PX(floorf(th * sc)), .radius = 13, .clip = true) {
                        cui_image_viewer_ex(ui, "zv", tex, tw, th, zv, &(cui_image_viewer_opts){ .width = CUI_FILL, .height = CUI_PX(floorf(th * sc)), .max_zoom = sc * 8, .no_toolbar = true });
                    }
                    if (share_zoom_clamp(&zv->zoom, &zv->cx, &zv->cy, sc, 8)) zoom_pct = share_zoom_pct(zv->zoom, sc);
                    if (key >= 0 && key < 2 * FV_N) g_zoom_pct[key] = zoom_pct;
                } else cui_image(ui, tex, .width = CUI_PX(floorf(tw * sc)), .height = CUI_PX(floorf(th * sc)), .radius = 13);
            } else {
                cui_icon(ui, CUI_ICON_MONITOR, 34, B_C3);
                cui_text(ui, wait, .size = 14, .color = B_MUTED);
            }
        }
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_END, .pad = compact ? 8 : 18) {
            cui_card_begin(ui, &(cui_card_opts){ .id = "lab", .dir = CUI_ROW, .gap = 7, .pad_x = compact ? 8 : 12, .pad_y = compact ? 4 : 8, .align = CUI_ALIGN_CENTER });
            if (!compact) cui_icon(ui, CUI_ICON_MONITOR, 14, B_C3);
            cui_text(ui, title, .size = compact ? 11 : 14, .nowrap = true);
            cui_card_end(ui);
        }
        if (chip && chip[0]) cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_END, .justify = CUI_JUSTIFY_START, .pad = 18) { briks_status_pill(ui, chip); }
        if (zv && zoom_pct > 100) {   /* selo do zoom: clicar volta a 100% (duplo clique na imagem e a tecla 0 também) */
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_START, .pad = 18) {
                char z[48]; snprintf(z, sizeof z, "%d%% · voltar a 100%%", zoom_pct);
                if (cui_button(ui, z, .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) memset(zv, 0, sizeof *zv);
            }
        }
        if (volume) {   /* volume do áudio desta transmissão, só para quem assiste */
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_END, .justify = CUI_JUSTIFY_END, .pad = 18) {
                cui_box(ui, .dir = CUI_ROW, .gap = 8, .pad_x = 12, .height = CUI_PX(36), .width = CUI_PX(190), .radius = 18, .bg = B_GLASS_STRONG, .border = 1, .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER) {
                    cui_icon(ui, *volume <= 0.01f ? CUI_ICON_VOLUME_X : CUI_ICON_VOLUME_2, 14, B_TEXT);
                    cui_box(ui, .width = CUI_FILL) { cui_slider(ui, "##vol", volume, 0, 2); }
                }
            }
        }
    }
    if (compact && !s_modal_hint && briks_clicked_r(ui, "vt", 20.0f)) clicked = true;
    cui_pop_id(ui);
    return clicked;
}

/* Aviso discreto "X começou a transmitir · Assistir". Fica 8 s; some se a pessoa parar, sair, ou se eu já estiver assistindo.
 * A chamada continua sem exibir transmissões sozinha: só o clique em Assistir troca para o modo Transmissão. */
bool share_notice_active(cui_ctx *ui, app_state_t *s) {
    int i = g_notice.slot; if (i < 0) return false;
    double now = cui_time(ui);
    if (g_notice.until < 0) g_notice.until = now + 8.0;
    if (now > g_notice.until || share_viewing(s) || !s->rtc_peers[i].id[0] || !(s->sh.remote[i].on || s->sh.remote2[i].on)) { g_notice.slot = -1; return false; }
    cui_request_wakeup(ui, 0.5f);
    return true;
}

void ui_share_notice_row(cui_ctx *ui, app_state_t *s) {
    int i = g_notice.slot; if (i < 0) return;
    cui_box(ui, .id = "sharenote", .dir = CUI_ROW, .gap = 12, .pad_x = 16, .pad_y = 9, .radius = 18, .bg = B_GLASS_POPUP, .border = 1, .border_color = B_GLASS_BORDER,
            .align = CUI_ALIGN_CENTER, .max_width = 640) {
        cui_icon(ui, CUI_ICON_MONITOR, 15, B_C3);
        char t[160]; snprintf(t, sizeof t, "%s começou a transmitir", s->rtc_peers[i].name[0] ? s->rtc_peers[i].name : "Alguém");
        cui_text(ui, t, .size = 13, .nowrap = true);
        if (cui_button(ui, "Assistir", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM)) { s->screen = SCREEN_CALL; s->call_mode = 1; s->call_view = 0; g_notice.slot = -1; }
    }
    { cui_rect r = cui_get_rect(ui, cui_make_id(ui, "sharenote")); if (r.w > 0) briks_cover(r.x, r.y, r.w, r.h); }
}

/* roteiros: share_notice (0/1), share_zoom (% da 1ª transmissão), share_zoomed (0/1), share_tiles, share_zoom2, share_mode (0 = Call, 1 = Transmissão) */
int share_probe(const char *n, const app_state_t *s) {
    if (!strcmp(n, "share_notice")) return g_notice.slot >= 0;
    if (!strcmp(n, "share_zoom")) return g_zoom_pct[0];
    if (!strcmp(n, "share_zoomed")) return g_zoom_pct[0] > 100;
    if (!strcmp(n, "share_cx")) return (int)g_zoom[0].cx;   /* ponto da imagem no meio da moldura: muda ao arrastar */
    if (!strcmp(n, "share_cy")) return (int)g_zoom[0].cy;
    if (!strcmp(n, "share_mode")) return s->call_mode;
    if (!strcmp(n, "share_tiles")) return g_tiles;          /* blocos de vídeo no palco */
    if (!strcmp(n, "share_focus")) return g_sfocus;          /* chave da transmissão em destaque no Foco (-1 = a primeira) */
    if (!strcmp(n, "share_zoom2")) return g_zoom_pct[FV2(0)];   /* % da 2ª tela do 1º participante */
    return -9999;
}

/* Um bloco do palco, identificado pela mesma chave de g_fv/g_zoom: i = participante, FV2(i) = a 2ª tela dele,
 * BRIKS_MAX_PEERS = minha 1ª tela, FV2(BRIKS_MAX_PEERS) = minha 2ª. Devolve true se a miniatura foi clicada. */
static bool emit_tile(cui_ctx *ui, app_state_t *s, int key, bool compact) {
    share_state_t *q = &s->sh;
    char path[300], title[200], chip[96], r[24];
    if (key == BRIKS_MAX_PEERS) {
        preview_path(path, sizeof path);
        bool has = fv_update(ui, s, &g_fv[key], path);
        snprintf(title, sizeof title, "Você · %s", q->label[0] ? q->label : "sua tela");
        quality_chip(q, chip, sizeof chip);
        return video_tile(ui, key, has ? g_fv[key].tex : 0, g_fv[key].w, g_fv[key].h, title, chip, "Preparando a prévia…", NULL, &g_zoom[key], compact);
    }
    if (key == FV2(BRIKS_MAX_PEERS)) {
        preview_path_n(path, sizeof path, true);
        fview_t *v = &g_fv[key];
        bool has = fv_update(ui, s, v, path);
        snprintf(title, sizeof title, "Você · %s (2ª tela)", q->second.label[0] ? q->second.label : "sua tela");
        fmt_rate(q->second.kbps, r, sizeof r);
        snprintf(chip, sizeof chip, "%s · %d FPS · %s", res_name(q->second.h), q->second.fps, r);
        return video_tile(ui, key, has ? v->tex : 0, v->w, v->h, title, chip, "Preparando a prévia…", NULL, &g_zoom[key], compact);
    }
    const bool two = key >= FV_N; const int i = two ? key - FV_N : key;
    const char *nm = s->rtc_peers[i].name[0] ? s->rtc_peers[i].name : "Participante";
    if (two) {
        fview_t *v = &g_fv[key];
        bool has2 = q->remote2[i].path[0] && q->remote2[i].watching && fv_update(ui, s, v, q->remote2[i].path);
        if (q->remote2[i].label[0]) snprintf(title, sizeof title, "%s · %s", nm, q->remote2[i].label); else snprintf(title, sizeof title, "%s · 2ª tela", nm);
        fmt_rate(q->remote2[i].kbps, r, sizeof r);
        snprintf(chip, sizeof chip, "%s · %d FPS · %s", res_name(q->remote2[i].h), q->remote2[i].fps, r);
        return video_tile(ui, key, has2 ? v->tex : 0, v->w, v->h, title, chip, "Carregando a transmissão…", NULL, &g_zoom[key], compact);
    }
    bool has = q->remote[i].path[0] && q->remote[i].watching && fv_update(ui, s, &g_fv[i], q->remote[i].path);
    if (q->remote[i].label[0]) snprintf(title, sizeof title, "%s · %s", nm, q->remote[i].label); else snprintf(title, sizeof title, "%s", nm);
    fmt_rate(q->remote[i].kbps, r, sizeof r);
    snprintf(chip, sizeof chip, "%s · %d FPS · %s%s", res_name(q->remote[i].h), q->remote[i].fps, r, q->remote[i].audio ? " · com áudio" : "");
    if (q->remote[i].poor) snprintf(chip, sizeof chip, "Conexão instável: a imagem pode ficar parada");
    if (!q->remote[i].vol_set) { q->remote[i].vol_set = true; q->remote[i].volume = 1.0f; }
    float before = q->remote[i].volume;
    bool c = video_tile(ui, i, has ? g_fv[i].tex : 0, g_fv[i].w, g_fv[i].h, title, chip, q->remote[i].poor ? "Sua conexão não está dando conta desta transmissão" : "Carregando a transmissão…", q->remote[i].audio ? &q->remote[i].volume : NULL, &g_zoom[i], compact);
    if (fabsf(before - q->remote[i].volume) > 0.01f) {
        char line[220], id[96]; json_escape(s->rtc_peers[i].id, id, sizeof id);
        snprintf(line, sizeof line, "{\"cmd\":\"share.volume\",\"peer\":\"%s\",\"volume\":%.2f}", id, q->remote[i].volume);
        cmd(line);
    }
    return c;
}


/* Palco do modo Transmissão. Grade: todas as transmissões lado a lado, a 2ª tela de uma pessoa é um bloco à parte (call_grid, última
 * linha centralizada). Foco: uma grande (fixada por clique, senão a primeira) e as outras numa faixa; clicar numa miniatura a traz
 * para o destaque. O seletor Grade/Foco é o mesmo do modo Call (g_call.layout). W x H = área útil do palco. */
bool ui_share_stage(cui_ctx *ui, app_state_t *s, float W, float H, int layout) {
    share_state_t *q = &s->sh;
    int keys[2 * FV_N], n = 0;
    for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (s->rtc_peers[i].id[0] && q->remote[i].on) { keys[n++] = i; if (q->remote2[i].on) keys[n++] = FV2(i); }
    if (q->active) keys[n++] = BRIKS_MAX_PEERS;
    if (q->second.active) keys[n++] = FV2(BRIKS_MAX_PEERS);
    g_tiles = 0;
    if (!n) return false;
    cui_request_redraw(ui);   /* vídeo: redesenha a cada quadro enquanto o palco está visível */
    s_modal_hint = q->dialog_open || s->kick_open || briks_modal_open();
    const float gap = 14.0f;
    int mi = -1;
    for (int i = 0; i < n; i++) if (keys[i] == g_sfocus) mi = i;
    if (mi < 0) { g_sfocus = -1; mi = 0; }   /* quem estava fixado parou: volta à primeira */
    if (layout == 1 && n > 1 && (W >= 720.0f ? H >= 150.0f : H >= 260.0f)) {
        const int k = n - 1; const bool side = W >= 720.0f; int pick = -1;
        cui_box(ui, .dir = side ? CUI_ROW : CUI_COLUMN, .width = CUI_FILL, .height = CUI_FILL, .gap = gap) {
            if (side) {
                const float sw = fminf(fmaxf(W * 0.22f, 170.0f), 280.0f), th = fminf(fmaxf((H - gap * (float)(k - 1)) / (float)k, 96.0f), sw * 0.62f);
                cui_box(ui, .id = "sfocus", .width = CUI_PX(W - gap - sw), .height = CUI_PX(H)) { emit_tile(ui, s, keys[mi], false); }
                const bool over = th * (float)k + gap * (float)(k - 1) > H + 0.5f;
                if (over) { cui_scroll(ui, .id = "sstrip", .width = CUI_PX(sw), .height = CUI_PX(H)) { cui_column(ui, .gap = gap) { for (int i = 0; i < n; i++) if (i != mi) { cui_box(ui, .width = CUI_PX(sw - 10.0f), .height = CUI_PX(th)) { if (emit_tile(ui, s, keys[i], true)) pick = keys[i]; } } } } }
                else cui_column(ui, .gap = gap, .width = CUI_PX(sw), .height = CUI_PX(H), .justify = CUI_JUSTIFY_CENTER) { for (int i = 0; i < n; i++) if (i != mi) { cui_box(ui, .width = CUI_PX(sw), .height = CUI_PX(th)) { if (emit_tile(ui, s, keys[i], true)) pick = keys[i]; } } }
            } else {
                const float sh = 110.0f, tw = fminf(fmaxf((W - gap * (float)(k - 1)) / (float)k, 130.0f), 200.0f);
                cui_box(ui, .id = "sfocus", .width = CUI_PX(W), .height = CUI_PX(H - gap - sh)) { emit_tile(ui, s, keys[mi], false); }
                cui_box(ui, .id = "sstrip-h", .dir = CUI_ROW, .width = CUI_PX(W), .height = CUI_PX(sh), .gap = gap, .scroll_x = true, .justify = CUI_JUSTIFY_CENTER) {
                    for (int i = 0; i < n; i++) if (i != mi) { cui_box(ui, .width = CUI_PX(tw), .height = CUI_PX(sh - 8.0f)) { if (emit_tile(ui, s, keys[i], true)) pick = keys[i]; } }
                }
            }
        }
        if (pick >= 0) g_sfocus = pick;
        return true;
    }
    /* Grade: blocos iguais; a última linha fica centralizada */
    int cols, rows; call_grid(n, W, H, gap, &cols, &rows);
    const float tw = floorf((W - gap * (float)(cols - 1)) / (float)cols), th = floorf((H - gap * (float)(rows - 1)) / (float)rows);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = gap, .justify = CUI_JUSTIFY_CENTER) {
        for (int r = 0; r < rows; r++) {
            cui_push_id_int(ui, 780 + r);
            cui_row(ui, .width = CUI_FILL, .gap = gap, .justify = CUI_JUSTIFY_CENTER) {
                for (int c = 0; c < cols && r * cols + c < n; c++) cui_box(ui, .width = CUI_PX(tw), .height = CUI_PX(th)) { emit_tile(ui, s, keys[r * cols + c], false); }
            }
            cui_pop_id(ui);
        }
    }
    return true;
}
