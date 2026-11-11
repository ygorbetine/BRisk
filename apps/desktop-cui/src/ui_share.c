/* Transmissão de tela/janela: seletor "Compartilhar tela — Escolha a fonte e confirme", áudio por app, qualidade independente
 * (resolução, FPS, bitrate), chip na barra da call, tile de vídeo (prévia de quem transmite) e palco do modo Transmissão.
 * O trabalho pesado (captura, H264, WebRTC, áudio por processo) é do briks-core; aqui só há comandos/eventos JSON e a exibição:
 * o core grava cada quadro RGBA num arquivo mapeado (seqlock) e esta tela o sobe como textura OpenGL.
 * Regra do produto: a chamada abre sem exibir transmissões; assistir exige o modo Transmissão (só então o core decodifica). */
#include "briks_ui.h"
#include "core.h"
#include "json.h"
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
    double base = h <= 480 ? 1200 : h <= 720 ? 2500 : h <= 1080 ? 4500 : h <= 1440 ? 9000 : 18000;
    return (int)(base * pow((fps < 1 ? 1 : fps) / 30.0, 0.75));
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
static fview_t g_fv[9];           /* 0..7 = slots de rtc_peers; 8 = prévia local */
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

static void preview_path(char *out, size_t cap) {
    const char *d = getenv("BRIKS_CORE_DATA");
    if (d && d[0]) snprintf(out, cap, "%s\\video\\preview.bin", d);
    else { const char *a = getenv("APPDATA"); snprintf(out, cap, "%s\\Briks-CUI\\video\\preview.bin", a ? a : "."); }
}

/* ---------- comandos ---------- */
void share_open_dialog(app_state_t *s) {
    share_state_t *q = &s->sh;
    q->dialog_open = true; q->err[0] = 0;
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

static void start_selected(app_state_t *s) {
    share_state_t *q = &s->sh;
    int t = q->tab;
    if (q->sel[t] < 0 || q->sel[t] >= q->nsrc[t]) return;
    send_start(s, t == 0 ? "screen" : "window", q->src[t][q->sel[t]].id, q->src[t][q->sel[t]].name);
    q->dialog_open = false;
}

void share_stop(app_state_t *s) { cmd("{\"cmd\":\"share.stop\"}"); s->sh.active = false; s->sh.starting = false; }

void share_reset(app_state_t *s) {
    share_state_t *q = &s->sh;
    for (int t = 0; t < 2; t++) for (int i = 0; i < q->nsrc[t]; i++) if (q->src[t][i].tex && g_ntrash < 64) g_trash[g_ntrash++] = q->src[t][i].tex;
    for (int i = 0; i < 9; i++) fv_close(&g_fv[i]);
    int res = q->res, fps = q->fps, br = q->br_mode, am = q->audio_mode; float bc = q->br_custom;
    memset(q, 0, sizeof *q);
    q->res = res; q->fps = fps; q->br_mode = br; q->audio_mode = am; q->br_custom = bc;
}

bool share_peer_live(const app_state_t *s, int slot) { return slot >= 0 && slot < 8 && s->sh.remote[slot].on; }

static int slot_of(const app_state_t *s, const char *peer) {
    for (int i = 0; i < 8; i++) if (s->rtc_peers[i].id[0] && !strcmp(s->rtc_peers[i].id, peer)) return i;
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
    } else if (!strcmp(ev, "share.local")) {
        if (!strcmp(json_str(j, "state", ""), "on")) {
            q->active = true; q->starting = false;
            q->w = jint(j, "w"); q->h = jint(j, "h"); q->cur_fps = jint(j, "fps"); q->kbps = jint(j, "kbps");
            q->audio_on = jbool(j, "audio"); q->hardware = jbool(j, "hardware");
            copy(q->encoder, sizeof q->encoder, json_str(j, "encoder", ""));
        } else {
            q->active = false; q->starting = false; q->adapting[0] = 0; q->live_h = 0; q->stat_fps = 0; q->stat_kbps = 0;
            copy(q->err, sizeof q->err, json_str(j, "reason", ""));
        }
    } else if (!strcmp(ev, "share.stats")) {
        q->stat_fps = jnum(j, "fps"); q->stat_kbps = jint(j, "kbps"); q->enc_ms = jnum(j, "enc_ms"); q->cpu = jint(j, "cpu_pct");
        q->audio_peak = jnum(j, "audio_peak"); q->audio_sources = jint(j, "audio_sources");
        q->live_h = jint(j, "h"); q->target_fps = jint(j, "target_fps"); q->target_kbps = jint(j, "target_kbps"); q->est_kbps = jint(j, "est_kbps");
        q->rtt_ms = jint(j, "rtt_ms"); q->rtx = jint(j, "rtx"); q->loss_pct = jnum(j, "loss_pct");
        copy(q->adapting, sizeof q->adapting, json_str(j, "adapting", "")); copy(q->reason, sizeof q->reason, json_str(j, "reason", ""));
    } else if (!strcmp(ev, "share.remote")) {
        int i = slot_of(s, json_str(j, "peer_id", "")); if (i < 0) return false;
        const char *st = json_str(j, "state", "");
        if (!strcmp(st, "on")) {
            q->remote[i].on = true; q->remote[i].w = jint(j, "w"); q->remote[i].h = jint(j, "h"); q->remote[i].fps = jint(j, "fps"); q->remote[i].kbps = jint(j, "kbps");
            q->remote[i].audio = jbool(j, "audio"); copy(q->remote[i].label, sizeof q->remote[i].label, json_str(j, "label", ""));
        } else if (!strcmp(st, "off")) { q->remote[i].on = false; q->remote[i].watching = false; q->remote[i].has_frame = false; fv_close(&g_fv[i]); }
        else if (!strcmp(st, "watch")) copy(q->remote[i].path, sizeof q->remote[i].path, json_str(j, "path", ""));
        else if (!strcmp(st, "frame")) { q->remote[i].has_frame = true; q->remote[i].w = jint(j, "w"); q->remote[i].h = jint(j, "h"); }
    } else return false;
    return true;
}

void share_tick(cui_ctx *ui, app_state_t *s) {
    share_state_t *q = &s->sh;
    for (int i = 0; i < g_ntrash; i++) cui_image_destroy(ui, g_trash[i]);
    g_ntrash = 0;
    if (!q->fps && !q->res && !q->br_custom) { q->res = 1; q->fps = FPS_DEFAULT; q->br_custom = 8; }
    if (q->fps < 0 || q->fps >= FPS_N) q->fps = FPS_DEFAULT;   /* padrão: 1080p · 30 FPS · automático */
    /* só assiste no modo Transmissão (aba Chamada): fora dele o core nem decodifica */
    bool viewing = s->screen == SCREEN_CALL && s->call_mode == 1 && s->call_view == 0;
    for (int i = 0; i < 8; i++) {
        if (!s->rtc_peers[i].id[0]) { if (q->remote[i].on || q->remote[i].watching) { memset(&q->remote[i], 0, sizeof q->remote[i]); fv_close(&g_fv[i]); } continue; }
        bool want = viewing && q->remote[i].on;
        if (want != q->remote[i].watching) {
            char line[200], id[96]; json_escape(s->rtc_peers[i].id, id, sizeof id);
            snprintf(line, sizeof line, "{\"cmd\":\"share.watch\",\"peer\":\"%s\",\"on\":%s}", id, want ? "true" : "false");
            cmd(line);
            q->remote[i].watching = want;
            if (!want) { q->remote[i].has_frame = false; fv_close(&g_fv[i]); }
        }
    }
    /* ganchos de teste/captura: BRIKS_DEV_SHARE=dialog | dialog-apps | dialog-windows | screen | test (uma vez, com o core pronto) */
    static int dev; const char *d = getenv("BRIKS_DEV_SHARE");
    if (d) cui_request_redraw(ui);
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
            if (briks_clicked(ui, "s") && !on) { *idx = i; changed = true; }
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
            cui_column(ui, .gap = 1) {
                cui_text(ui, "Se a rede ou o PC apertar", .size = 13, .color = B_MUTED);
                cui_text(ui, q->content ? "Fluidez: cede resolução antes de quadros" : "Nitidez: cede quadros antes de resolução", .size = 11, .color = CUI_RGBA(0xa592b8aa), .nowrap = true);
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
            cui_text(ui, line, .size = 12, .color = B_MUTED, .nowrap = true);
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
    cui_dialog(ui, "Compartilhar tela", &q->dialog_open, .description = "Escolha a fonte e confirme", .width = fminf(inner + 48, fmaxf(vp.x - 48, 520))) {
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
            cui_text(ui, q->starting ? "Iniciando…" : q->active ? "Parar transmissão" : "Compartilhar", .font = CUI_FONT_DISPLAY, .size = 15, .color = fg);
        }
        /* com o seletor aberto a barra fica por baixo dele: um clique no diálogo não pode valer também aqui */
        bool modal = q->dialog_open || s->kick_open;
        if (!modal && briks_clicked(ui, "b")) { if (on) share_stop(s); else share_open_dialog(s); }
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
        cui_box(ui, .id = "q", .dir = CUI_ROW, .gap = 6, .pad_x = 14, .height = CUI_FILL, .align = CUI_ALIGN_CENTER) {
            bool adapting = q->active && q->adapting[0];   /* a rede ou o PC apertou: o chip mostra o que está saindo de fato */
            if (failed) cui_icon(ui, CUI_ICON_TRIANGLE_ALERT, 13, B_DANGER);
            else if (adapting) cui_icon(ui, !strcmp(q->adapting, "cpu") ? CUI_ICON_CPU : CUI_ICON_TRENDING_DOWN, 13, CUI_RGB(0xf5b942));
            cui_text(ui, failed ? q->err : chip, .size = 12, .color = failed ? CUI_RGB(0xffb4b4) : adapting ? CUI_RGB(0xf5d9a0) : B_TEXT, .nowrap = true);
        }
        if ((!modal && briks_clicked(ui, "q")) || g_dev_open_quality) { g_dev_open_quality = false; q->err[0] = 0; cui_popover_open(ui, "shq"); }
        cui_popover(ui, "shq", .placement = CUI_PLACE_TOP_END, .width = 520, .pad = 16, .gap = 10) {
            cui_text(ui, "Qualidade da transmissão", .font = CUI_FONT_DISPLAY, .size = 15);
            static bool dirty;
            if (quality_controls(ui, q)) dirty = true;
            if (q->active) {
                /* telemetria local, ao vivo */
                char st[240];
                snprintf(st, sizeof st, "No ar: %s · %.0f FPS · %d kbps (alvo %d FPS · %d kbps)", res_name(q->live_h ? q->live_h : q->h), q->stat_fps, q->stat_kbps, q->target_fps, q->target_kbps);
                cui_text(ui, st, .size = 12, .color = B_TEXT, .nowrap = true);
                snprintf(st, sizeof st, "Rede: estimada %d kbps · perda %.1f%% · RTT %d ms · reenvios %d/s", q->est_kbps, q->loss_pct, q->rtt_ms, q->rtx);
                cui_text(ui, st, .size = 12, .color = B_MUTED, .nowrap = true);
                snprintf(st, sizeof st, "Codificador: %s%s · %.1f ms/quadro · CPU %d%%", q->encoder[0] ? q->encoder : "H264", q->hardware ? " (GPU)" : " (software)", q->enc_ms, q->cpu);
                cui_text(ui, st, .size = 12, .color = B_MUTED, .nowrap = true);
                if (q->adapting[0]) {
                    snprintf(st, sizeof st, !strcmp(q->adapting, "cpu") ? "Adaptando: o PC não deu conta; qualidade reduzida até caber." : "Adaptando à rede%s%s: qualidade reduzida, volta sozinha.", q->reason[0] ? " (" : "", q->reason[0] ? (!strcmp(q->reason, "perda") ? "perda)" : "atraso)") : "");
                    cui_text(ui, st, .size = 12, .color = CUI_RGB(0xf5d9a0), .nowrap = true);
                }
                if (cui_button(ui, "Aplicar na transmissão", .variant = CUI_BUTTON_PRIMARY, .disabled = !dirty)) { dirty = false; send_start(s, q->last_kind, q->last_id, q->label); cui_popover_close(ui, "shq"); }
            } else dirty = false;
        }
    }
    cui_pop_id(ui);
}

/* ---------- palco do modo Transmissão ---------- */
static void video_tile(cui_ctx *ui, int key, cui_texture tex, int tw, int th, const char *title, const char *chip, const char *wait) {
    cui_push_id_int(ui, 800 + key);
    cui_rect r = cui_get_rect(ui, cui_make_id(ui, "vt"));   /* tamanho do quadro anterior: o vídeo cabe inteiro, sem distorcer */
    cui_box(ui, .id = "vt", .dir = CUI_STACK, .width = CUI_FILL, .height = CUI_FILL, .radius = 20, .bg = CUI_RGBA(0x1a0a2680), .border = 1, .border_color = CUI_RGBA(0x6d5cf655)) {
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .gap = 10) {
            if (tex && tw > 0 && th > 0 && r.w > 60 && r.h > 60) {
                float aw = r.w - 16, ah = r.h - 16, sc = fminf(aw / tw, ah / th);
                /* a imagem tem o próprio raio e fica 8 px para dentro da moldura (raio 20): cantos sempre curvos, nada reto vaza */
                cui_image(ui, tex, .width = CUI_PX(floorf(tw * sc)), .height = CUI_PX(floorf(th * sc)), .radius = 13);
            } else {
                cui_icon(ui, CUI_ICON_MONITOR, 34, B_C3);
                cui_text(ui, wait, .size = 14, .color = B_MUTED);
            }
        }
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_END, .pad = 18) {
            cui_card_begin(ui, &(cui_card_opts){ .id = "lab", .dir = CUI_ROW, .gap = 7, .pad_x = 12, .pad_y = 8, .align = CUI_ALIGN_CENTER });
            cui_icon(ui, CUI_ICON_MONITOR, 14, B_C3);
            cui_text(ui, title, .size = 14);
            cui_card_end(ui);
        }
        if (chip && chip[0]) cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_END, .justify = CUI_JUSTIFY_START, .pad = 18) { briks_status_pill(ui, chip); }
    }
    cui_pop_id(ui);
}

bool ui_share_stage(cui_ctx *ui, app_state_t *s) {
    share_state_t *q = &s->sh;
    int n = (q->active ? 1 : 0);
    for (int i = 0; i < 8; i++) if (s->rtc_peers[i].id[0] && q->remote[i].on) n++;
    if (!n) return false;
    cui_request_redraw(ui);   /* vídeo: redesenha a cada quadro enquanto o palco está visível */
    cui_row(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 14) {
        if (q->active) {
            char path[300], title[160], chip[96]; preview_path(path, sizeof path);
            bool has = fv_update(ui, s, &g_fv[8], path);
            snprintf(title, sizeof title, "Você · %s", q->label[0] ? q->label : "sua tela");
            quality_chip(q, chip, sizeof chip);
            video_tile(ui, 8, has ? g_fv[8].tex : 0, g_fv[8].w, g_fv[8].h, title, chip, "Preparando a prévia…");
        }
        for (int i = 0; i < 8; i++) if (s->rtc_peers[i].id[0] && q->remote[i].on) {
            char title[200], chip[96], r[24];
            bool has = q->remote[i].path[0] && q->remote[i].watching && fv_update(ui, s, &g_fv[i], q->remote[i].path);
            const char *nm = s->rtc_peers[i].name[0] ? s->rtc_peers[i].name : "Participante";
            if (q->remote[i].label[0]) snprintf(title, sizeof title, "%s · %s", nm, q->remote[i].label); else snprintf(title, sizeof title, "%s", nm);
            fmt_rate(q->remote[i].kbps, r, sizeof r);
            snprintf(chip, sizeof chip, "%s · %d FPS · %s%s", res_name(q->remote[i].h), q->remote[i].fps, r, q->remote[i].audio ? " · com áudio" : "");
            video_tile(ui, i, has ? g_fv[i].tex : 0, g_fv[i].w, g_fv[i].h, title, chip, "Carregando a transmissão…");
        }
    }
    return true;
}
