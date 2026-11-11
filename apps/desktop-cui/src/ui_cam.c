/* Câmera na chamada: botão liga/desliga com estado real, vídeo no bloco de cada pessoa (com avatar quando desligada), escolha da câmera e
 * teste de pré-visualização em Configurações. Captura, H264 e WebRTC são do briks-core; aqui só há comandos/eventos JSON (cam.*) e a
 * exibição: o core grava cada quadro RGBA num arquivo mapeado (seqlock) e esta tela o sobe como textura, como na transmissão de tela.
 * A câmera só abre quando ligada; sair da chamada ou desligar a solta (a luz apaga). Os blocos só decodificam o vídeo de quem aparece. */
#include "briks_ui.h"
#include "call.h"
#include "core.h"
#include "json.h"
#include <cui/cui_render.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define CAM_DEV_MAX 8
#define FV_ME BRIKS_MAX_PEERS          /* prévia do que eu envio */
#define FV_TEST (BRIKS_MAX_PEERS + 1)  /* teste das Configurações */

typedef struct { char path[300]; HANDLE file, map; const unsigned char *p; unsigned long long n; cui_texture tex; int w, h; } fview_t;
static fview_t g_fv[BRIKS_MAX_PEERS + 2];
static cui_texture g_trash[32]; static int g_ntrash;

static struct {
    bool active, starting; int w, h; char err[200];
    struct { char id[320], name[96]; } dev[CAM_DEV_MAX]; int ndev; bool dev_loaded, dev_asked;
    bool prev_on, prev_want, prev_sent; char prev_path[300], prev_err[200];
    struct { bool on, watching; char path[300]; } remote[BRIKS_MAX_PEERS];
} C;

static void prev_send(void);
static void copy(char *dst, size_t cap, const char *src) { snprintf(dst, cap, "%s", src ? src : ""); }
static int jint(const jval *j, const char *k) { const jval *v = json_get(j, k); return v && v->type == J_NUM ? (int)v->num : 0; }
static void cmd(const char *json) { if (core_state() == CORE_READY) core_send(json); }

static void fv_close(fview_t *v) {
    if (v->p) UnmapViewOfFile(v->p);
    if (v->map) CloseHandle(v->map);
    if (v->file && v->file != INVALID_HANDLE_VALUE) CloseHandle(v->file);
    if (v->tex && g_ntrash < 32) g_trash[g_ntrash++] = v->tex;
    memset(v, 0, sizeof *v);
}

/* Abre (se preciso) e sobe o quadro mais novo. true = há textura para mostrar. */
static bool fv_update(cui_ctx *ui, app_state_t *s, fview_t *v, const char *path) {
    if (!path[0]) return false;
    if (strcmp(v->path, path)) { fv_close(v); copy(v->path, sizeof v->path, path); }
    if (!v->p) {
        v->file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        if (v->file == INVALID_HANDLE_VALUE) { v->file = NULL; return false; }
        v->map = CreateFileMappingA(v->file, NULL, PAGE_READONLY, 0, 0, NULL);
        if (v->map) v->p = MapViewOfFile(v->map, FILE_MAP_READ, 0, 0, 0);
        if (!v->p) { if (v->map) CloseHandle(v->map); CloseHandle(v->file); v->map = v->file = NULL; return false; }
    }
    const volatile unsigned *seq = (const volatile unsigned *)(v->p + 4);
    unsigned a = *seq;
    if (memcmp(v->p, "BKFR", 4) || (a & 1)) return v->tex != 0;
    int w = *(const int *)(v->p + 8), h = *(const int *)(v->p + 12);
    unsigned long long n = *(const unsigned long long *)(v->p + 16);
    if (w <= 0 || h <= 0 || w > 3840 || h > 2160 || n == 0 || n == v->n) return v->tex != 0;
    cui_gl_renderer *gl = cui_app_gl_renderer(s->app);
    if (v->tex && w == v->w && h == v->h && gl) { cui_renderer_iface r = cui_gl_iface(gl); r.texture_update(r.user, v->tex, 0, 0, w, h, v->p + 64, w * 4); }
    else { if (v->tex && g_ntrash < 32) g_trash[g_ntrash++] = v->tex; v->tex = cui_image_create(ui, w, h, v->p + 64); v->w = w; v->h = h; }
    if (*seq == a) v->n = n;
    return v->tex != 0;
}

static void video_dir(char *out, size_t cap) {
    const char *d = getenv("BRIKS_CORE_DATA");
    if (d && d[0]) snprintf(out, cap, "%s\\video", d);
    else { const char *a = getenv("APPDATA"); snprintf(out, cap, "%s\\Briks-CUI\\video", a ? a : "."); }
}

/* ---------- estado ---------- */
bool cam_active(void) { return C.active; }
bool cam_starting(void) { return C.starting; }
bool cam_remote_on(int slot) { return slot >= 0 && slot < BRIKS_MAX_PEERS && C.remote[slot].on; }
int cam_dev_count(void) { return C.ndev; }
const char *cam_dev_name(int i) { return i >= 0 && i < C.ndev ? C.dev[i].name : ""; }

void cam_reset(void) {
    for (int i = 0; i < BRIKS_MAX_PEERS + 2; i++) fv_close(&g_fv[i]);
    bool want = C.prev_want, loaded = C.dev_loaded; int nd = C.ndev; char devs[sizeof C.dev]; memcpy(devs, C.dev, sizeof C.dev);
    memset(&C, 0, sizeof C);
    C.prev_want = want; C.dev_loaded = loaded; C.ndev = nd; memcpy(C.dev, devs, sizeof C.dev); C.dev_asked = true;
}

static const char *friendly(const char *code, const char *reason) {
    if (!strcmp(code, "no_camera")) return "Não encontramos uma câmera neste computador.";
    if (!strcmp(code, "denied")) return "O Windows bloqueou a câmera. Libere em Configurações > Privacidade > Câmera.";
    if (!strcmp(code, "busy")) return "Outro aplicativo está usando a câmera. Feche-o e tente de novo.";
    if (!strcmp(code, "no_room")) return "Entre numa sala para ligar a câmera.";
    return reason && reason[0] ? reason : "Não foi possível ligar a câmera.";
}

static int slot_of(const app_state_t *s, const char *peer) {
    for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (s->rtc_peers[i].id[0] && !strcmp(s->rtc_peers[i].id, peer)) return i;
    return -1;
}

/* Eventos cam.* do core. */
bool cam_handle_event(app_state_t *s, const jval *j, const char *ev) {
    if (!strcmp(ev, "cam.devices")) {
        const jval *arr = json_get(j, "devices"); C.ndev = 0;
        for (int i = 0; arr && i < json_len(arr) && C.ndev < CAM_DEV_MAX; i++) { const jval *e = json_at(arr, i); copy(C.dev[C.ndev].id, sizeof C.dev[0].id, json_str(e, "id", "")); copy(C.dev[C.ndev].name, sizeof C.dev[0].name, json_str(e, "name", "Câmera")); C.ndev++; }
        C.dev_loaded = true;
    } else if (!strcmp(ev, "cam.local")) {
        if (!strcmp(json_str(j, "state", ""), "on")) { C.active = true; C.starting = false; C.w = jint(j, "w"); C.h = jint(j, "h"); C.err[0] = 0; }
        else {
            const bool was = C.active || C.starting;
            C.active = false; C.starting = false;
            copy(C.err, sizeof C.err, friendly(json_str(j, "code", ""), json_str(j, "reason", "")));
            if (was || json_str(j, "reason", "")[0]) call_toast(C.err);
            fv_close(&g_fv[FV_ME]);
        }
    } else if (!strcmp(ev, "cam.preview")) {
        if (!strcmp(json_str(j, "state", ""), "on")) { C.prev_on = true; copy(C.prev_path, sizeof C.prev_path, json_str(j, "path", "")); C.prev_err[0] = 0; }
        else {
            C.prev_on = false; C.prev_want = false; fv_close(&g_fv[FV_TEST]);
            const char *code = json_str(j, "code", ""), *rs = json_str(j, "reason", "");
            copy(C.prev_err, sizeof C.prev_err, code[0] || rs[0] ? friendly(code, rs) : "");
        }
    } else if (!strcmp(ev, "cam.remote")) {
        int i = slot_of(s, json_str(j, "peer_id", "")); if (i < 0) return false;
        const char *st = json_str(j, "state", "");
        if (!strcmp(st, "on")) C.remote[i].on = true;
        else if (!strcmp(st, "off")) { C.remote[i].on = false; C.remote[i].watching = false; fv_close(&g_fv[i]); }
        else if (!strcmp(st, "watch")) copy(C.remote[i].path, sizeof C.remote[i].path, json_str(j, "path", ""));
    } else return false;
    return true;
}

/* Botão da barra: liga (se estiver desligada) ou desliga. */
void cam_toggle(app_state_t *s) {
    if (C.active || C.starting) { cmd("{\"cmd\":\"cam.stop\"}"); C.active = false; C.starting = false; fv_close(&g_fv[FV_ME]); return; }
    if (!s->in_room) { copy(C.err, sizeof C.err, friendly("no_room", "")); call_toast(C.err); return; }
    if (C.dev_loaded && C.ndev == 0) { copy(C.err, sizeof C.err, friendly("no_camera", "")); call_toast(C.err); return; }
    const char *want = settings_cam_name(); char id[330] = "";
    for (int i = 0; want[0] && i < C.ndev; i++) if (!strcmp(C.dev[i].name, want)) { json_escape(C.dev[i].id, id, sizeof id); break; }
    char line[420]; snprintf(line, sizeof line, "{\"cmd\":\"cam.start\",\"device\":\"%s\"}", id);
    cmd(line); C.starting = true; C.err[0] = 0;
}

/* Todo quadro: lista as câmeras uma vez, sobe texturas velhas ao lixo, e assiste só quem aparece nos blocos (a decodificação do vídeo
 * é do core e só roda enquanto a pessoa está na grade da aba Call). */
void cam_tick(cui_ctx *ui, app_state_t *s) {
    for (int i = 0; i < g_ntrash; i++) cui_image_destroy(ui, g_trash[i]);
    g_ntrash = 0;
    /* abrindo Configurações > Voz e vídeo fora da chamada: sobe o core (uma vez) para listar as câmeras */
    if (!C.dev_loaded && settings_cam_tab_visible(s) && core_state() == CORE_STOPPED) core_start();
    if (!C.dev_asked && core_state() == CORE_READY) { C.dev_asked = true; cmd("{\"cmd\":\"cam.list\"}"); }
    if (C.prev_want && !C.prev_sent && core_state() == CORE_READY) prev_send();
    if (C.prev_want && !C.dev_loaded) cui_request_redraw(ui);
    const bool viewing = s->screen == SCREEN_CALL && s->call_mode == 0 && s->call_view == 0;
    for (int i = 0; i < BRIKS_MAX_PEERS; i++) {
        if (!s->rtc_peers[i].id[0]) { if (C.remote[i].on || C.remote[i].watching) { memset(&C.remote[i], 0, sizeof C.remote[i]); fv_close(&g_fv[i]); } continue; }
        const bool want = viewing && C.remote[i].on;
        if (want != C.remote[i].watching) {
            char line[220], id[96]; json_escape(s->rtc_peers[i].id, id, sizeof id);
            snprintf(line, sizeof line, "{\"cmd\":\"cam.watch\",\"peer\":\"%s\",\"on\":%s}", id, want ? "true" : "false");
            cmd(line); C.remote[i].watching = want;
            if (!want) fv_close(&g_fv[i]);
        }
    }
    if (!s->in_room && (C.active || C.starting)) { C.active = C.starting = false; fv_close(&g_fv[FV_ME]); }
    /* teste das Configurações: só com a tela aberta na aba Voz e vídeo; ao sair, para (a luz apaga) */
    if (C.prev_want && !settings_cam_tab_visible(s)) cam_preview_set(false);
    if (C.active || C.prev_on) cui_request_redraw(ui);
}

/* Textura do vídeo de um bloco: slot de peer (0..7) ou SLOT_ME (eu). false = sem imagem (mostra o avatar). */
bool cam_frame(cui_ctx *ui, app_state_t *s, int slot, cui_texture *tex, int *w, int *h) {
    if (slot == BRIKS_MAX_PEERS + 1) {   /* SLOT_ME */
        if (!C.active) return false;
        char p[320]; video_dir(p, sizeof p); strcat(p, "\\cam-preview.bin");
        if (!fv_update(ui, s, &g_fv[FV_ME], p)) return false;
        *tex = g_fv[FV_ME].tex; *w = g_fv[FV_ME].w; *h = g_fv[FV_ME].h; return true;
    }
    if (slot < 0 || slot >= BRIKS_MAX_PEERS || !C.remote[slot].on || !C.remote[slot].watching || !C.remote[slot].path[0]) return false;
    if (!fv_update(ui, s, &g_fv[slot], C.remote[slot].path)) return false;
    *tex = g_fv[slot].tex; *w = g_fv[slot].w; *h = g_fv[slot].h; return true;
}

/* ---------- Configurações > Voz e vídeo ---------- */
static void prev_send(void) {
    const char *want = settings_cam_name(); char id[330] = "";
    for (int i = 0; want[0] && i < C.ndev; i++) if (!strcmp(C.dev[i].name, want)) { json_escape(C.dev[i].id, id, sizeof id); break; }
    char line[420]; snprintf(line, sizeof line, "{\"cmd\":\"cam.preview\",\"on\":true,\"device\":\"%s\"}", id); cmd(line);
    C.prev_sent = true;
}
/* o teste roda fora da chamada, então o core pode nem estar de pé: sobe e envia o pedido assim que ficar pronto (cam_tick) */
void cam_preview_set(bool on) {
    if (on == C.prev_want) return;
    C.prev_want = on; C.prev_err[0] = 0;
    if (!on) { if (C.prev_sent) cmd("{\"cmd\":\"cam.preview\",\"on\":false}"); C.prev_sent = false; C.prev_on = false; fv_close(&g_fv[FV_TEST]); return; }
    if (core_state() == CORE_STOPPED) core_start();
    C.prev_sent = false;
}
bool cam_devs_loaded(void) { return C.dev_loaded; }
bool cam_preview_wanted(void) { return C.prev_want; }

/* Imagem do teste (ou do que está sendo enviado, se a câmera da chamada já está ligada) com cantos arredondados; w x h = caixa. */
void cam_settings_view(cui_ctx *ui, app_state_t *s, float w, float h) {
    cui_texture tex = 0; int tw = 0, th = 0; bool has = false;
    if (C.active) has = cam_frame(ui, s, BRIKS_MAX_PEERS + 1, &tex, &tw, &th);
    else if (C.prev_on) { char p[320]; copy(p, sizeof p, C.prev_path); has = fv_update(ui, s, &g_fv[FV_TEST], p); if (has) { tex = g_fv[FV_TEST].tex; tw = g_fv[FV_TEST].w; th = g_fv[FV_TEST].h; } }
    cui_box(ui, .width = CUI_PX(w), .height = CUI_PX(h), .radius = 14, .bg = CUI_RGBA(0x1a0a2680), .border = 1, .border_color = B_GLASS_BORDER, .clip = true, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
        if (has && tw > 0 && th > 0) { float sc = fminf((w - 8) / tw, (h - 8) / th); cui_image(ui, tex, .width = CUI_PX(floorf(tw * sc)), .height = CUI_PX(floorf(th * sc)), .radius = 10); }
        else { cui_icon(ui, CUI_ICON_VIDEO, 28, B_C3); }
    }
}
const char *cam_preview_error(void) { return C.prev_err; }
bool cam_preview_on(void) { return C.prev_on; }
const char *cam_last_error(void) { return C.err; }

int cam_probe(const char *n) {
    if (!strcmp(n, "cam_active")) return C.active;
    if (!strcmp(n, "cam_starting")) return C.starting;
    if (!strcmp(n, "cam_err")) return C.err[0] != 0;
    if (!strcmp(n, "cam_devs")) return C.ndev;
    if (!strcmp(n, "cam_prev")) return C.prev_on;
    if (!strcmp(n, "cam_prev_err")) return C.prev_err[0] != 0;
    if (!strncmp(n, "cam_remote", 10)) return cam_remote_on(atoi(n + 10));
    if (!strcmp(n, "cam_tex_me")) return g_fv[FV_ME].tex != 0;
    if (!strncmp(n, "cam_tex", 7)) { int i = atoi(n + 7); return i >= 0 && i < BRIKS_MAX_PEERS && g_fv[i].tex != 0; }
    return -9999;
}
