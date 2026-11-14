/* Transmissão de tela/janela: seletor "Compartilhar tela — Escolha a fonte e confirme", áudio por app, qualidade independente
 * (resolução, FPS, bitrate), chip na barra da call, tile de vídeo (prévia de quem transmite) e palco do modo Transmissão.
 * O trabalho pesado (captura, H264, WebRTC, áudio por processo) é do briks-core; aqui só há comandos/eventos JSON e a exibição:
 * o core grava cada quadro RGBA num arquivo mapeado (seqlock) e esta tela o sobe como textura OpenGL.
 * Regra do produto: a chamada abre sem exibir transmissões; assistir exige o modo Transmissão (só então o core decodifica). */
#include "briks_ui.h"
#include "core.h"
#include "json.h"
#include "callhist.h"
#include "call.h"
#include "volume_ctl.h"
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
float *share_remote_volume(app_state_t *s, int i) {
    if (i < 0 || i >= BRIKS_MAX_PEERS) return NULL;
    if (!s->sh.remote[i].vol_set) { s->sh.remote[i].vol_set = true; s->sh.remote[i].volume = 1.0f; }
    return &s->sh.remote[i].volume;
}
void share_remote_volume_changed(app_state_t *s, int i) { (void)s; (void)i; }   /* o share_tick envia o ganho certo (zero fora da aba Transmissão) */

/* zoom de quem assiste, por transmissão (zerado = encaixada), e o aviso "X começou a transmitir · Assistir" */
static share_aspect_t g_aspect[2 * (BRIKS_MAX_PEERS + 1)];   /* razão de aspecto suavizada de cada tile (mesmo índice de g_fv) */
static cui_image_view g_zoom[2 * (BRIKS_MAX_PEERS + 1)];   /* mesmo índice de g_fv: cada transmissão tem o próprio zoom */
static int g_zoom_pct[2 * (BRIKS_MAX_PEERS + 1)];
static int g_stage_toggles;   /* roteiros: quantas vezes um clique em área vazia alternou grade/foco */
static int g_tiles;                                 /* blocos desenhados no palco no último quadro (roteiros de teste) */   /* o que o selo mostrou no último quadro (roteiros de teste) */
static struct { int slot; double until; } g_notice = { -1, -1 };
static bool share_viewing(const app_state_t *s) { return s->screen == SCREEN_CALL && s->call_mode == 1 && s->call_view == 0; }
static const char *const CODEC_NAME[3] = { "h264", "vp9", "vp8" };   /* 1.4.4: índice de share_state_t.codec */
static int g_qk[2];   /* teste qual-slots: kbps pedido a cada transmissão */
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
/* 1.4.1 (CHM-34): qualidade por transmissão. Os campos res/fps/br_mode/br_custom/content do estado são os da transmissão EM EDIÇÃO (qsel);
 * a outra fica guardada em qs[]. Trocar de transmissão guarda uma e carrega a outra; enviar ao core usa a qualidade da transmissão certa. */
static qual_t qual_view(const share_state_t *q) { return (qual_t){ q->res, q->fps, q->br_mode, q->content, q->br_custom }; }
static void qual_set_view(share_state_t *q, const qual_t *v) { q->res = v->res; q->fps = v->fps; q->br_mode = v->br_mode; q->content = v->content; q->br_custom = v->br_custom; }
static void qual_switch(share_state_t *q, int sel) {
    if (sel < 0 || sel > 1 || sel == q->qsel) return;
    q->qs[q->qsel] = qual_view(q); q->qs_ok[q->qsel] = true;
    q->qsel = sel;
    if (q->qs_ok[sel]) qual_set_view(q, &q->qs[sel]);   /* nunca editada: começa igual à outra */
}
static qual_t qual_enter(share_state_t *q, int slot) {   /* põe no estado a qualidade da transmissão `slot`; devolve o que estava (para qual_leave) */
    qual_t prev = qual_view(q);
    if (slot != q->qsel && q->qs_ok[slot]) qual_set_view(q, &q->qs[slot]);
    return prev;
}
static void qual_leave(share_state_t *q, qual_t prev) { qual_set_view(q, &prev); }

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
typedef struct { char path[300]; HANDLE file, map; const unsigned char *p; unsigned long long n; cui_texture tex; int w, h; double last; int want; unsigned char *ds; size_t dscap; int sw, sh, kused, blur; unsigned char cells[40 * 23 * 4]; int ccw, cch; } fview_t;   /* want: largura (px) em que a imagem aparece; ds: quadro reduzido; sw/sh: tamanho do quadro de origem */
#define FV_N (BRIKS_MAX_PEERS + 1)
#define FV2(i) (FV_N + (i))   /* a 2ª tela de cada um fica na segunda metade */
static fview_t g_fv[2 * FV_N];   /* 0..MAX-1 = slots de rtc_peers; MAX = prévia local; depois o mesmo para a 2ª tela */
static cui_texture g_trash[64]; static int g_ntrash;   /* texturas a destruir no próximo quadro (só a thread da UI mexe em GL) */

/* 1.4.2: o receptor reduzia um quadro grande (ex. 1080p) direto na GPU com filtro linear simples, o que serrilha e borra texto quando o quadro aparece muito
 * menor (tile na grade). Aqui o quadro é reduzido por média de blocos (caixa 2^k x 2^k) até ficar entre 1x e 2x do tamanho em que aparece; a GPU faz só o
 * ajuste final. Com zoom acima do encaixe (want = 0) ou quando já aparece quase no tamanho original, usa o quadro inteiro. */

static void fv_close(fview_t *v) {
    free(v->ds); v->ds = NULL; v->dscap = 0;
    if (v->p) UnmapViewOfFile(v->p);
    if (v->map) CloseHandle(v->map);
    if (v->file && v->file != INVALID_HANDLE_VALUE) CloseHandle(v->file);
    if (v->tex && g_ntrash < 64) g_trash[g_ntrash++] = v->tex;
    memset(v, 0, sizeof *v);
}

/* Abre (se preciso) e sobe o quadro mais novo. Devolve true se há textura para mostrar. */
/* desfoque de caixa (separável, bordas repetidas) no quadro minúsculo da thumb: a imagem esticada ao bloco inteiro vira só cor e luz, sem formas legíveis */
static void blur_rgba(unsigned char *p, int w, int h, int r, int passes) {
    if (r < 1 || w < 2 || h < 2) return;
    const int m = w > h ? w : h; unsigned char *tmp = (unsigned char *)malloc((size_t)m * 4); if (!tmp) return;
    for (int ps = 0; ps < passes; ps++) {
        for (int y = 0; y < h; y++) {   /* horizontal */
            unsigned char *row = p + (size_t)y * w * 4;
            for (int c = 0; c < 4; c++) {
                int sum = 0; for (int k = -r; k <= r; k++) { int x = k < 0 ? 0 : k >= w ? w - 1 : k; sum += row[x * 4 + c]; }
                for (int x = 0; x < w; x++) {
                    tmp[x * 4 + c] = (unsigned char)(sum / (2 * r + 1));
                    int a = x + r + 1; a = a >= w ? w - 1 : a; int b = x - r; b = b < 0 ? 0 : b;
                    sum += row[a * 4 + c] - row[b * 4 + c];
                }
            }
            memcpy(row, tmp, (size_t)w * 4);
        }
        for (int x = 0; x < w; x++) {   /* vertical */
            for (int c = 0; c < 4; c++) {
                int sum = 0; for (int k = -r; k <= r; k++) { int y = k < 0 ? 0 : k >= h ? h - 1 : k; sum += p[((size_t)y * w + x) * 4 + c]; }
                for (int y = 0; y < h; y++) {
                    tmp[y * 4 + c] = (unsigned char)(sum / (2 * r + 1));
                    int a = y + r + 1; a = a >= h ? h - 1 : a; int b = y - r; b = b < 0 ? 0 : b;
                    sum += p[((size_t)a * w + x) * 4 + c] - p[((size_t)b * w + x) * 4 + c];
                }
            }
            for (int y = 0; y < h; y++) memcpy(p + ((size_t)y * w + x) * 4, tmp + y * 4, 4);
        }
    }
    free(tmp);
}

static bool fv_update(cui_ctx *ui, app_state_t *s, fview_t *v, const char *path) {
    if (strcmp(v->path, path)) { const int kw = v->want, kb = v->blur; fv_close(v); v->want = kw; v->blur = kb; copy(v->path, sizeof v->path, path); }   /* o fechamento zera tudo: guarda a largura pedida e o desfoque */
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
    if (w <= 0 || h <= 0 || w > 3840 || h > 2160 || n == 0 || (n == v->n && share_ds_steps(w, v->want) == v->kused)) return v->tex != 0;   /* quadro igual e mesma redução: nada a subir */
    cui_gl_renderer *gl = cui_app_gl_renderer(s->app);
    const unsigned char *pix = v->p + 64; int ow = w, oh = h;
    const int k = share_ds_steps(w, v->want);
    if (k > 0) {
        ow = w >> k; oh = h >> k; const size_t need = (size_t)ow * (size_t)oh * 4;
        if (v->dscap < need) { free(v->ds); v->ds = (unsigned char *)malloc(need); v->dscap = v->ds ? need : 0; }
        if (v->ds) { share_box_down(pix, w, k, ow, oh, v->ds); pix = v->ds; } else { ow = w; oh = h; }
    }
    if (v->tex && v->w > 0 && (ow != v->w || oh != v->h)) {   /* a textura mudou de tamanho (redução ligou/desligou): o zoom do espectador é em px da imagem, então acompanha */
        const int gi = (int)(v - g_fv);
        if (gi >= 0 && gi < 2 * FV_N && g_zoom[gi].zoom > 0) { const float f = (float)v->w / (float)ow; g_zoom[gi].zoom *= f; g_zoom[gi].cx /= f; g_zoom[gi].cy /= f; }
    }
    v->kused = k;
    if (v->blur && (size_t)ow * (size_t)oh <= 256u * 256u) {   /* thumb do bloco: desfoca de verdade (a cópia vai para o buffer próprio: o arquivo mapeado é só leitura) */
        const size_t need = (size_t)ow * (size_t)oh * 4;
        if (v->dscap < need) { free(v->ds); v->ds = (unsigned char *)malloc(need); v->dscap = v->ds ? need : 0; }
        if (v->ds) { if (pix != v->ds) memcpy(v->ds, pix, need); pix = v->ds; blur_rgba(v->ds, ow, oh, 3, 3); }
    }
    if (v->blur) {   /* mosaico: cor media de cada celula 40x23 (a thumb ja esta pequena) */
        v->ccw = 40; v->cch = 23;
        for (int cy = 0; cy < 23; cy++) for (int cx = 0; cx < 40; cx++) {
            const int x0 = cx * ow / 40, x1 = (cx + 1) * ow / 40, y0 = cy * oh / 23, y1 = (cy + 1) * oh / 23; unsigned sum[3] = { 0, 0, 0 }, cnt = 0;
            for (int y = y0; y < (y1 > y0 ? y1 : y0 + 1) && y < oh; y++) for (int x = x0; x < (x1 > x0 ? x1 : x0 + 1) && x < ow; x++) { const unsigned char *q = pix + ((size_t)y * (size_t)ow + (size_t)x) * 4; sum[0] += q[0]; sum[1] += q[1]; sum[2] += q[2]; cnt++; }
            unsigned char *d = v->cells + ((size_t)cy * 40 + (size_t)cx) * 4; if (!cnt) cnt = 1; d[0] = (unsigned char)(sum[0] / cnt); d[1] = (unsigned char)(sum[1] / cnt); d[2] = (unsigned char)(sum[2] / cnt); d[3] = 255;
        }
    }
    if (v->tex && ow == v->w && oh == v->h && gl) {
        cui_renderer_iface r = cui_gl_iface(gl);
        r.texture_update(r.user, v->tex, 0, 0, ow, oh, pix, ow * 4);
    } else {
        if (v->tex && g_ntrash < 64) g_trash[g_ntrash++] = v->tex;
        v->tex = cui_image_create(ui, ow, oh, pix);
        v->w = ow; v->h = oh;
    }
    v->sw = w; v->sh = h;
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
    if (!q->second.active && !q->second.starting) qual_switch(q, 0);   /* sem 2ª tela no ar o seletor edita a 1ª */
    if (core_state() != CORE_READY) { q->loading = false; copy(q->err, sizeof q->err, "Entre numa sala para transmitir."); return; }
    q->loading = true;
    cmd("{\"cmd\":\"share.list\"}");
    cmd("{\"cmd\":\"share.audio.list\"}");
}

static char g_last_err[320];   /* motivo completo da última parada da transmissão (o chip mostra só um resumo) */
static void send_start(app_state_t *s, const char *kind, const char *id, const char *label) {
    share_state_t *q = &s->sh;
    char line[1024], lab[160], pids[400] = ""; size_t o = 0;
    json_escape(label, lab, sizeof lab);
    const qual_t qprev = qual_enter(q, 0);
    for (int i = 0; i < q->napps && o < sizeof pids - 16; i++) if (q->apps[i].on) o += (size_t)snprintf(pids + o, sizeof pids - o, "%s%u", o ? "," : "", q->apps[i].pid);
    snprintf(line, sizeof line, "{\"cmd\":\"share.start\",\"kind\":\"%s\",\"id\":\"%s\",\"height\":%d,\"fps\":%d,\"kbps\":%d,\"label\":\"%s\",\"audio\":\"%s\",\"pids\":[%s],\"content\":\"%s\",\"codec\":\"%s\"}",
             kind, id, RES_H[q->res], FPS_V[q->fps], chosen_kbps(q), lab, q->audio_mode == 0 ? "source" : q->audio_mode == 1 ? "apps" : "none", pids, q->content ? "motion" : "detail", CODEC_NAME[q->codec]);
    qual_leave(q, qprev);
    cmd(line);
    copy(q->last_kind, sizeof q->last_kind, kind); copy(q->last_id, sizeof q->last_id, id); copy(q->label, sizeof q->label, label);
    q->starting = true; q->err[0] = 0;
}

/* 2ª tela simultânea: mesma qualidade escolhida, sem áudio (o áudio da transmissão é o da 1ª tela) */
static void send_start_second(app_state_t *s, const char *kind, const char *id, const char *label) {
    share_state_t *q = &s->sh;
    char line[700], lab[160];
    json_escape(label, lab, sizeof lab);
    const qual_t qprev = qual_enter(q, 1);
    snprintf(line, sizeof line, "{\"cmd\":\"share.start\",\"slot\":1,\"kind\":\"%s\",\"id\":\"%s\",\"height\":%d,\"fps\":%d,\"kbps\":%d,\"label\":\"%s\",\"audio\":\"none\",\"content\":\"%s\",\"codec\":\"%s\"}",
             kind, id, RES_H[q->res], FPS_V[q->fps], chosen_kbps(q), lab, q->content ? "motion" : "detail", CODEC_NAME[q->codec]);
    qual_leave(q, qprev);
    cmd(line);
    copy(q->second.label, sizeof q->second.label, label); copy(q->second.kind, sizeof q->second.kind, kind); copy(q->second.id, sizeof q->second.id, id);
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
    int res = q->res, fps = q->fps, br = q->br_mode, am = q->audio_mode, ct = q->content, qsel = q->qsel, cdc = q->codec; float bc = q->br_custom;
    qual_t qs[2] = { q->qs[0], q->qs[1] }; bool qok[2] = { q->qs_ok[0], q->qs_ok[1] };
    memset(q, 0, sizeof *q);
    q->res = res; q->fps = fps; q->br_mode = br; q->audio_mode = am; q->br_custom = bc; q->content = ct; q->codec = cdc;
    q->qs[0] = qs[0]; q->qs[1] = qs[1]; q->qs_ok[0] = qok[0]; q->qs_ok[1] = qok[1]; q->qsel = qsel;
}

bool share_peer_live(const app_state_t *s, int slot) { return slot >= 0 && slot < BRIKS_MAX_PEERS && s->sh.remote[slot].on; }

bool share_peer_eye(const app_state_t *s, int slot, int sl, int *count, char *tip, size_t cap) {
    if (slot < 0 || slot >= BRIKS_MAX_PEERS || sl < 0 || sl > 1) return false;
    const bool live = sl == 0 ? s->sh.remote[slot].on : s->sh.remote2[slot].on;
    if (!live || !s->sh.aud[slot][sl].known) return false;   /* sem transmissão ou sem dado (cliente antigo): oculto */
    if (count) *count = s->sh.aud[slot][sl].count;
    if (tip) snprintf(tip, cap, "%s", s->sh.aud[slot][sl].count > 0 && s->sh.aud[slot][sl].names[0] ? s->sh.aud[slot][sl].names : s->sh.aud[slot][sl].count > 0 ? "Pessoas assistindo" : "Ninguém assistindo agora");
    return true;
}

static int slot_of(const app_state_t *s, const char *peer) {
    for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (s->rtc_peers[i].id[0] && !strcmp(s->rtc_peers[i].id, peer)) return i;
    return -1;
}

/* Aviso "transmissão ligada" de alguém que a lista da chamada ainda não conhece (no servidor de chamadas o aviso da tela pode chegar antes do
 * "entrou"/hello da pessoa): antes era descartado e a transmissão só aparecia quando o amigo parava e retomava. Agora fica guardado e é aplicado
 * assim que a pessoa aparece (share_pending_tick); um "desligou" apaga o guardado. */
typedef struct { char peer[96], label[64]; int slot, w, h, fps, kbps; bool audio, used; double at; } pend_share_t;
static pend_share_t g_pshare[12];
static void pshare_note(const jval *j) {
    const char *peer = json_str(j, "peer_id", ""), *st = json_str(j, "state", ""); const int sl = jint(j, "slot") == 1 ? 1 : 0;
    if (!peer[0]) return;
    pend_share_t *free_slot = NULL, *same = NULL, *oldest = &g_pshare[0];
    for (int k = 0; k < 12; k++) {
        pend_share_t *e = &g_pshare[k];
        if (e->used && e->slot == sl && !strcmp(e->peer, peer)) same = e;
        if (!e->used && !free_slot) free_slot = e;
        if (e->used && (!oldest->used || e->at < oldest->at)) oldest = e;
    }
    if (!strcmp(st, "off")) { if (same) same->used = false; return; }
    if (strcmp(st, "on")) return;
    pend_share_t *e = same ? same : free_slot ? free_slot : oldest;
    memset(e, 0, sizeof *e); e->used = true; e->slot = sl; e->w = jint(j, "w"); e->h = jint(j, "h"); e->fps = jint(j, "fps"); e->kbps = jint(j, "kbps"); e->audio = jbool(j, "audio"); e->at = (double)GetTickCount64() / 1000.0;
    copy(e->peer, sizeof e->peer, peer); copy(e->label, sizeof e->label, json_str(j, "label", ""));
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
    } else if (!strcmp(ev, "share.viewers")) {   /* core: {slot, count, peers:[ids]}; nunca conta o próprio usuário */
        const int sl = jint(j, "slot") == 1 ? 1 : 0; q->vknown[sl] = true; q->viewers[sl] = jint(j, "count"); q->vnames[sl][0] = 0;
        const jval *pl = json_get(j, "peers"); const int np = json_len(pl); int shown = 0;
        q->vidn[sl] = 0;   /* ids de quem assiste: o nome é resolvido na hora de desenhar (o nome pode chegar depois do aviso) */
        for (int k = 0; k < np && q->vidn[sl] < 10; k++) { const jval *e = json_at(pl, k); if (e && e->type == J_STR && e->str) snprintf(q->vids[sl][q->vidn[sl]++], sizeof q->vids[sl][0], "%s", e->str); }
        for (int k = 0; k < np && shown < 3; k++) {
            const jval *e = json_at(pl, k); const char *pid = e && e->type == J_STR && e->str ? e->str : "";
            const char *nm = "Participante";
            for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (s->rtc_peers[i].id[0] && !strcmp(s->rtc_peers[i].id, pid) && s->rtc_peers[i].name[0]) nm = s->rtc_peers[i].name;
            size_t l = strlen(q->vnames[sl]); snprintf(q->vnames[sl] + l, sizeof q->vnames[sl] - l, "%s%s", shown ? ", " : "", nm); shown++;
        }
        if (np > shown) { size_t l = strlen(q->vnames[sl]); snprintf(q->vnames[sl] + l, sizeof q->vnames[sl] - l, " e mais %d", np - shown); }
    } else if (!strcmp(ev, "share.audience")) {   /* core: {peer_id, slot, count, names:[até 3]}: a pessoa divulga quantos assistem à tela dela */
        const int i = slot_of(s, json_str(j, "peer_id", "")), sl = jint(j, "slot") == 1 ? 1 : 0; if (i < 0) return true;
        q->aud[i][sl].known = true; q->aud[i][sl].count = jint(j, "count"); if (q->aud[i][sl].count < 0) q->aud[i][sl].count = 0; q->aud[i][sl].names[0] = 0;
        const jval *nl = json_get(j, "names"); const int nn = json_len(nl); int shown = 0;
        for (int k = 0; k < nn && shown < 3; k++) {
            const jval *e = json_at(nl, k); if (!e || e->type != J_STR || !e->str || !e->str[0]) continue;
            size_t l = strlen(q->aud[i][sl].names); snprintf(q->aud[i][sl].names + l, sizeof q->aud[i][sl].names - l, "%s%s", shown ? ", " : "", e->str); shown++;
        }
        if (q->aud[i][sl].count > shown && shown) { size_t l = strlen(q->aud[i][sl].names); snprintf(q->aud[i][sl].names + l, sizeof q->aud[i][sl].names - l, " e mais %d", q->aud[i][sl].count - shown); }
    } else if (jint(j, "slot") == 1 && (!strcmp(ev, "share.local") || !strcmp(ev, "share.stats") || !strcmp(ev, "share.hint") || !strcmp(ev, "share.rxstats") || !strcmp(ev, "share.remote"))) {
        /* 2ª tela (segundo monitor): estado próprio, mais enxuto que o da 1ª */
        if (!strcmp(ev, "share.local")) {
            if (!strcmp(json_str(j, "state", ""), "on")) { if (!q->second.active && s->screen == SCREEN_CALL) s->call_mode = 1;   /* começou a MINHA 2ª tela: vai para Transmissão (uma vez por início) */
                q->second.active = true; q->second.starting = false; q->second.w = jint(j, "w"); q->second.h = jint(j, "h"); q->second.fps = jint(j, "fps"); q->second.kbps = jint(j, "kbps"); }
            else { const char *why = json_str(j, "reason", ""); if (why[0]) copy(q->err, sizeof q->err, why); memset(&q->second, 0, sizeof q->second); q->vknown[1] = false; q->viewers[1] = 0; fv_close(&g_fv[FV2(BRIKS_MAX_PEERS)]); }
        } else if (!strcmp(ev, "share.stats")) { q->second.kbps = jint(j, "kbps"); if (jint(j, "h")) q->second.h = jint(j, "h"); }
        else if (!strcmp(ev, "share.remote")) {
            int i = slot_of(s, json_str(j, "peer_id", "")); if (i < 0) { pshare_note(j); return false; }
            const char *st = json_str(j, "state", "");
            if (!strcmp(st, "on")) {
                if (share_notice_wanted(q->remote2[i].on || q->remote[i].on, true, share_viewing(s))) { g_notice.slot = i; g_notice.until = -1; }
                q->remote2[i].on = true; q->remote2[i].w = jint(j, "w"); q->remote2[i].h = jint(j, "h"); q->remote2[i].fps = jint(j, "fps"); q->remote2[i].kbps = jint(j, "kbps");
                copy(q->remote2[i].label, sizeof q->remote2[i].label, json_str(j, "label", ""));
            } else if (!strcmp(st, "off")) { memset(&q->remote2[i], 0, sizeof q->remote2[i]); memset(&q->aud[i][1], 0, sizeof q->aud[i][1]); fv_close(&g_fv[FV2(i)]); memset(&g_zoom[FV2(i)], 0, sizeof g_zoom[0]); }
            else if (!strcmp(st, "watch")) copy(q->remote2[i].path, sizeof q->remote2[i].path, json_str(j, "path", ""));
            else if (!strcmp(st, "frame")) { q->remote2[i].has_frame = true; q->remote2[i].stalled = false; q->remote2[i].w = jint(j, "w"); q->remote2[i].h = jint(j, "h"); }
            else if (!strcmp(st, "stalled")) q->remote2[i].stalled = true;   /* 3 s sem pacote algum: tira a última imagem em vez de deixá-la parada */
            else if (!strcmp(st, "resumed")) q->remote2[i].stalled = false;
        }
    } else if (!strcmp(ev, "share.local")) {
        if (!strcmp(json_str(j, "state", ""), "on")) {
            if (!q->active && s->screen == SCREEN_CALL) s->call_mode = 1;   /* começou a MINHA transmissão: aba Transmissão, uma vez por início (o usuário pode voltar e não é puxado de novo) */
            if (!q->active) q->audio_muted = false;   /* transmissão nova: o core começa com áudio */
            q->active = true; q->starting = false;
            q->w = jint(j, "w"); q->h = jint(j, "h"); q->cur_fps = jint(j, "fps"); q->kbps = jint(j, "kbps");
            q->audio_on = jbool(j, "audio"); q->hardware = jbool(j, "hardware");
            if (q->audio_on && q->audio_muted) cmd("{\"cmd\":\"share.audio.mute\",\"muted\":true}");   /* o codificador reiniciou (mudou a qualidade): o mudo continua valendo */
            copy(q->encoder, sizeof q->encoder, json_str(j, "encoder", ""));
        } else {
            q->vknown[0] = false; q->viewers[0] = 0; q->active = false; q->starting = false; q->black = false; q->adapting[0] = 0; q->live_h = 0; q->stat_fps = 0; q->stat_kbps = 0;
            share_stop_second(s);   /* a 2ª tela só existe junto com a 1ª */
            copy(q->err, sizeof q->err, json_str(j, "reason", ""));
        }
    } else if (!strcmp(ev, "share.encoder")) {
        /* o core recriou o codificador sozinho (a transmissão continua no ar): só um aviso discreto, nunca um erro */
        const char *st = json_str(j, "state", "");
        if (!strcmp(st, "fallback") || !strcmp(st, "software")) { q->hardware = false; settings_notice("Transmissão: o codificador da placa de vídeo falhou várias vezes; usando o codificador por software (a imagem pode ficar mais pesada)."); }
        else if (!strcmp(st, "recovered")) settings_notice("Transmissão: o codificador de vídeo foi reiniciado automaticamente.");
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
        if (jint(j, "slot") == 1) { q->remote2[i].fps = jint(j, "fps"); q->remote2[i].kbps = jint(j, "kbps"); }   /* o que EU recebo (medido aqui), não o que o emissor anunciou (no servidor de chamadas vinha 0) */
        else { q->remote[i].fps = jint(j, "fps"); q->remote[i].kbps = jint(j, "kbps"); }
        if (jint(j, "fps") < 3) { if (q->remote[i].low_secs < 99) q->remote[i].low_secs++; } else q->remote[i].low_secs = 0;
        q->remote[i].poor = q->remote[i].low_secs >= 4;
    } else if (!strcmp(ev, "share.remote")) {
        int i = slot_of(s, json_str(j, "peer_id", "")); if (i < 0) { pshare_note(j); return false; }
        const char *st = json_str(j, "state", "");
        if (!strcmp(st, "on")) {
            if (share_notice_wanted(q->remote[i].on, true, share_viewing(s))) { g_notice.slot = i; g_notice.until = -1; }
            const bool was_on = q->remote[i].on;   /* reaviso (o áudio chegou depois do vídeo no servidor): mantém os números medidos */
            q->remote[i].on = true; q->remote[i].w = jint(j, "w"); q->remote[i].h = jint(j, "h");
            if (!was_on) { q->remote[i].fps = jint(j, "fps"); q->remote[i].kbps = jint(j, "kbps"); }
            q->remote[i].audio = jbool(j, "audio"); copy(q->remote[i].label, sizeof q->remote[i].label, json_str(j, "label", ""));
        } else if (!strcmp(st, "off")) { memset(&q->aud[i][0], 0, sizeof q->aud[i][0]); q->remote[i].on = false; q->remote[i].watching = false; q->remote[i].has_frame = false; q->remote[i].poor = false; q->remote[i].stalled = false; q->remote[i].low_secs = 0; fv_close(&g_fv[i]);
            memset(&g_zoom[i], 0, sizeof g_zoom[i]); if (g_notice.slot == i) g_notice.slot = -1; }
        else if (!strcmp(st, "watch")) copy(q->remote[i].path, sizeof q->remote[i].path, json_str(j, "path", ""));
        else if (!strcmp(st, "frame")) { q->remote[i].has_frame = true; q->remote[i].stalled = false; q->remote[i].w = jint(j, "w"); q->remote[i].h = jint(j, "h"); }
        else if (!strcmp(st, "stalled")) q->remote[i].stalled = true;   /* 3 s sem pacote algum: tira a última imagem em vez de deixá-la parada */
        else if (!strcmp(st, "resumed")) q->remote[i].stalled = false;
    } else return false;
    return true;
}

/* quadro sintético para as capturas (grade de 80 px com um bloco de cor por célula; `tint` muda a paleta) */
static void demo_frame(char *path, size_t cap, const char *name, int W, int H, int tint) {
    const char *tmp = getenv("TEMP");
    snprintf(path, cap, "%s\\briks-demo-%s-%lu.bin", tmp ? tmp : ".", name, (unsigned long)GetCurrentProcessId());
    { const char *own = getenv("BRIKS_DEMO_FRAME"); if (own && own[0] && CopyFileA(own, path, FALSE)) return; }   /* captura de teste: quadro real (arquivo BKFR) no lugar da grade sintética */
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

/* ---------- miniatura da transmissão na barra "Em chamada" da lateral (1.4.4) ----------
 * Aparece fora da tela da chamada quando há transmissão no ar (de outros ou minha). Tem decodificador/arquivo de quadro PRÓPRIO (g_mini):
 * o tile do palco só existe com a chamada aberta. Sobe no máx. ~8 quadros/s, já reduzido ao tamanho da miniatura (fv_update faz a média
 * de blocos), então o custo é um decodificador a mais no core (igual à miniatura da sobreposição) e uma textura pequena. */
static fview_t g_mini;
static int g_sfocus;   /* definida (com valor inicial) mais abaixo, junto do palco */
static int g_mkey = -1, g_msel = -1;   /* chave exibida (mesma de emit_tile) e a escolhida pelo usuário (-1 = a primeira) */
static bool g_mini_on;                 /* a miniatura está sendo pedida neste quadro (tela fora da chamada) */
static int g_mini_n, g_mini_frames;    /* roteiros: quantas transmissões; quadros novos subidos */
static int live_keys(const app_state_t *s, int *keys) {
    const share_state_t *q = &s->sh; int n = 0;
    for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (s->rtc_peers[i].id[0] && q->remote[i].on) { keys[n++] = i; if (q->remote2[i].on) keys[n++] = FV2(i); }
    if (q->active) keys[n++] = BRIKS_MAX_PEERS;
    if (q->second.active) keys[n++] = FV2(BRIKS_MAX_PEERS);
    return n;
}
static bool mini_wants(int slot, bool second) { return g_mini_on && g_mkey == (second ? FV2(slot) : slot); }
static void mini_update_sel(app_state_t *s) {
    int keys[2 * FV_N], n = live_keys(s, keys);
    g_mini_on = s->in_room && s->screen != SCREEN_CALL && n > 0;
    g_mini_n = g_mini_on ? n : 0;
    g_mkey = -1;
    if (g_mini_on) { g_mkey = keys[0]; for (int i = 0; i < n; i++) if (keys[i] == g_msel) g_mkey = g_msel; }
    if (!g_mini_on) { g_msel = -1; fv_close(&g_mini); }
}
static void mini_title(const app_state_t *s, int key, char *out, size_t cap) {
    if (key == BRIKS_MAX_PEERS || key == FV2(BRIKS_MAX_PEERS)) { snprintf(out, cap, key == BRIKS_MAX_PEERS ? "Você" : "Você · 2ª tela"); return; }
    const bool two = key >= FV_N; const int i = two ? key - FV_N : key;
    const char *nm = s->rtc_peers[i].name[0] ? s->rtc_peers[i].name : "Participante";
    snprintf(out, cap, two ? "%s · 2ª" : "%s", nm);
}
/* Desenha a miniatura (largura w) acima da barra. Devolve false (e nada ocupa espaço) quando não há o que mostrar. */
bool share_mini(cui_ctx *ui, app_state_t *s, float w) {
    if (!g_mini_on || g_mkey < 0 || w < 120.0f) return false;
    const share_state_t *q = &s->sh; const int key = g_mkey;
    char path[300] = ""; bool ok = false;
    if (key == BRIKS_MAX_PEERS) { preview_path(path, sizeof path); ok = true; }
    else if (key == FV2(BRIKS_MAX_PEERS)) { preview_path_n(path, sizeof path, true); ok = true; }
    else if (key >= FV_N) { const int i = key - FV_N; ok = q->remote2[i].path[0] && q->remote2[i].watching && !q->remote2[i].stalled; if (ok) copy(path, sizeof path, q->remote2[i].path); }
    else { ok = q->remote[key].path[0] && q->remote[key].watching && !q->remote[key].stalled; if (ok) copy(path, sizeof path, q->remote[key].path); }
    const float ih = floorf(w * 9.0f / 16.0f);
    g_mini.want = (int)w * 2;
    if (ok) {
        const double now = cui_time(ui);
        if (!g_mini.tex || strcmp(g_mini.path, path) || now - g_mini.last >= 0.125) { const unsigned long long before = g_mini.n; ok = fv_update(ui, s, &g_mini, path); if (g_mini.n != before) g_mini_frames++; }
        else ok = true;
        cui_request_wakeup(ui, 0.125f);
    } else if (g_mini.tex) fv_close(&g_mini);
    const bool has = ok && g_mini.tex && g_mini.w > 0 && g_mini.h > 0;
    char title[120]; mini_title(s, key, title, sizeof title);
    bool go = false; int step = 0;
    cui_push_id_int(ui, 6420);
    cui_box(ui, .width = CUI_PX(w), .gap = 4) {
        cui_box(ui, .id = "mini", .width = CUI_PX(w), .height = CUI_PX(ih), .radius = 14, .bg = CUI_RGBA(0x1a0a2680), .border = 1, .border_color = CUI_RGBA(0x6d5cf655),
                .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .clip = true) {
            if (has) {
                const float sc = fminf((w - 4.0f) / (float)g_mini.w, (ih - 4.0f) / (float)g_mini.h);
                cui_image(ui, g_mini.tex, .width = CUI_PX(floorf((float)g_mini.w * sc)), .height = CUI_PX(floorf((float)g_mini.h * sc)), .radius = 11);   /* raio próprio e 2 px para dentro da moldura: nada reto vaza */
            } else cui_icon(ui, CUI_ICON_MONITOR, 22, B_C3);
        }
        if (briks_hover(ui, "mini")) briks_tip("Voltar à chamada e assistir esta transmissão");
        if (briks_clicked_r(ui, "mini", 14.0f)) go = true;
        cui_box(ui, .dir = CUI_ROW, .width = CUI_PX(w), .gap = 4, .align = CUI_ALIGN_CENTER, .pad_x = 2) {
            cui_icon(ui, CUI_ICON_MONITOR, 12, B_C3);
            cui_box(ui, .width = CUI_FILL, .clip = true) { cui_text(ui, title, .size = 11.5f, .color = B_MUTED, .nowrap = true); }
            if (g_mini_n > 1) {
                char cnt[16] = ""; int idx = 0, keys[2 * FV_N], n = live_keys(s, keys); for (int i = 0; i < n; i++) if (keys[i] == key) idx = i;
                snprintf(cnt, sizeof cnt, "%d/%d", idx + 1, n);
                cui_box(ui, .id = "mprev", .width = CUI_PX(22), .height = CUI_PX(22), .radius = 11, .bg = CUI_RGBA(0xffffff12), .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) { cui_icon(ui, CUI_ICON_CHEVRON_LEFT, 13, B_TEXT); }
                if (briks_hover(ui, "mprev")) briks_tip("Transmissão anterior");
                if (briks_clicked_r(ui, "mprev", 11.0f)) step = -1;
                cui_text(ui, cnt, .size = 11, .color = B_MUTED, .nowrap = true);
                cui_box(ui, .id = "mnext", .width = CUI_PX(22), .height = CUI_PX(22), .radius = 11, .bg = CUI_RGBA(0xffffff12), .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) { cui_icon(ui, CUI_ICON_CHEVRON_RIGHT, 13, B_TEXT); }
                if (briks_hover(ui, "mnext")) briks_tip("Próxima transmissão");
                if (briks_clicked_r(ui, "mnext", 11.0f)) step = 1;
            }
        }
    }
    cui_pop_id(ui);
    if (step) {
        int keys[2 * FV_N], n = live_keys(s, keys), idx = 0; for (int i = 0; i < n; i++) if (keys[i] == key) idx = i;
        if (n > 1) { g_msel = keys[(idx + step + n) % n]; fv_close(&g_mini); }
    } else if (go) {
        g_sfocus = key; s->screen = SCREEN_CALL; s->call_mode = 1; s->call_view = 0;
        if (g_mini_n > 1) g_call.layout = 1;   /* várias: o foco precisa estar no modo Foco para valer */
        g_msel = -1;
    }
    return true;
}

/* ---------- thumb desfocada de quem transmite (fundo do avatar no modo Call) ----------
 * Quadro minúsculo (~96 px de largura, reduzido por média de blocos e esticado com filtro bilinear = desfoque barato), pedido só para os primeiros
 * 3 avatares por quadro. Pede o decodificador ao core igual à miniatura da lateral; sobe poucos quadros por segundo. */
static fview_t g_thumb[BRIKS_MAX_PEERS]; static bool g_thumb_req[BRIKS_MAX_PEERS], g_thumb_prev[BRIKS_MAX_PEERS]; static int g_thumb_nreq;
static fview_t g_thumb_me; static bool g_thumb_me_req, g_thumb_me_prev;   /* thumb da PROPRIA transmissao (prévia local), no bloco "Você" */
static double thumb_dt(void) { return g_perf >= PERF_MIN ? 10.0 : g_perf == PERF_SIMPLE ? 0.25 : 0.125; }   /* fundo desfocado: Normal 8 quadros/s, Simplificado 4, Mínimo estático (um quadro a cada 10 s) */
bool share_thumb(cui_ctx *ui, app_state_t *s, int slot, cui_texture *tex, int *w, int *h) {
    if (slot == BRIKS_MAX_PEERS + 1) {   /* SLOT_ME */
        if (!s->sh.active) return false;
        g_thumb_me_req = true; fview_t *v = &g_thumb_me; v->want = 40; v->blur = 1;
        char path[300]; preview_path(path, sizeof path); const double now = cui_time(ui);
        if (!v->tex || now - v->last >= thumb_dt()) { if (!fv_update(ui, s, v, path)) return false; }
        if (!v->tex || v->w <= 0) return false;
        *tex = v->tex; *w = v->w; *h = v->h; return true;
    }
    if (slot < 0 || slot >= BRIKS_MAX_PEERS || !s->rtc_peers[slot].id[0] || !s->sh.remote[slot].on) return false;
    if (!g_thumb_req[slot]) { if (g_thumb_nreq >= 3) return false; g_thumb_req[slot] = true; g_thumb_nreq++; }
    const share_state_t *q = &s->sh;
    if (!(q->remote[slot].path[0] && q->remote[slot].watching && !q->remote[slot].stalled)) return false;
    fview_t *v = &g_thumb[slot]; v->want = 40; v->blur = 1;   /* bem pequena: esticada ao bloco inteiro vira desfoque */
    const double now = cui_time(ui);
    if (!v->tex || now - v->last >= thumb_dt()) {   /* fundo desfocado ao vivo: 8 / 4 quadros/s, estático no Mínimo */ if (!fv_update(ui, s, v, q->remote[slot].path)) return false; }
    if (!v->tex || v->w <= 0) return false;
    *tex = v->tex; *w = v->w; *h = v->h; return true;
}
bool share_any_live(const app_state_t *s) { for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (s->rtc_peers[i].id[0] && (s->sh.remote[i].on || s->sh.remote2[i].on)) return true; return false; }
bool share_thumb_cells(int slot, const unsigned char **cells, int *cw, int *ch) { if (slot == BRIKS_MAX_PEERS + 1) { if (!g_thumb_me.ccw) return false; *cells = g_thumb_me.cells; *cw = g_thumb_me.ccw; *ch = g_thumb_me.cch; return true; } if (slot < 0 || slot >= BRIKS_MAX_PEERS || !g_thumb[slot].ccw) return false; *cells = g_thumb[slot].cells; *cw = g_thumb[slot].ccw; *ch = g_thumb[slot].cch; return true; }
int share_thumb_probe(void) { int c = 0; for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (g_thumb[i].tex) c++; return c; }

bool overlay_live_wanted(int slot, bool second);   /* overlay_host.c */
void share_tick(cui_ctx *ui, app_state_t *s) {
    share_state_t *q = &s->sh;
    for (int k = 0; k < 12; k++) {   /* avisos de transmissão guardados de gente que ainda não estava na lista: aplica quando ela aparece (ou descarta após 2 min) */
        pend_share_t *e = &g_pshare[k]; if (!e->used) continue;
        if ((double)GetTickCount64() / 1000.0 - e->at > 120.0) { e->used = false; continue; }
        if (slot_of(s, e->peer) < 0) continue;
        char pj[96 * 2], lj[64 * 2], js[512]; json_escape(e->peer, pj, sizeof pj); json_escape(e->label, lj, sizeof lj);
        snprintf(js, sizeof js, "{\"ev\":\"share.remote\",\"peer_id\":\"%s\",\"slot\":%d,\"state\":\"on\",\"w\":%d,\"h\":%d,\"fps\":%d,\"kbps\":%d,\"label\":\"%s\",\"audio\":%s}", pj, e->slot, e->w, e->h, e->fps, e->kbps, lj, e->audio ? "true" : "false");
        e->used = false;
        jval *jj = json_parse(js); if (jj) { share_handle_event(s, jj, "share.remote"); json_free(jj); }
    }
    for (int i = 0; i < g_ntrash; i++) cui_image_destroy(ui, g_trash[i]);
    g_ntrash = 0;
    if (!q->fps && !q->res && !q->br_custom) { q->res = 1; q->fps = FPS_DEFAULT; q->br_custom = 8; }
    if (q->fps < 0 || q->fps >= FPS_N) q->fps = FPS_DEFAULT;   /* padrão: 1080p · 30 FPS · automático */
    /* só assiste no modo Transmissão (aba Chamada): fora dele o core nem decodifica */
    bool viewing = share_viewing(s);
    {   /* enquanto assiste a uma transmissão o computador não suspende nem apaga a tela; ao parar de assistir, volta ao normal */
        static bool awake; bool want = false;
        for (int i = 0; i < BRIKS_MAX_PEERS && !want; i++) want = (q->remote[i].on && q->remote[i].watching) || (q->remote2[i].on && q->remote2[i].watching);
        if (want != awake) { awake = want; SetThreadExecutionState(want ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED) : ES_CONTINUOUS); }
    }
    mini_update_sel(s);
    memcpy(g_thumb_prev, g_thumb_req, sizeof g_thumb_prev); memset(g_thumb_req, 0, sizeof g_thumb_req); g_thumb_nreq = 0;
    g_thumb_me_prev = g_thumb_me_req; g_thumb_me_req = false; if (!g_thumb_me_prev && g_thumb_me.p) fv_close(&g_thumb_me);
    for (int i = 0; i < BRIKS_MAX_PEERS; i++) {
        if (!g_thumb_prev[i] && g_thumb[i].p) fv_close(&g_thumb[i]);   /* ninguém pediu a thumb deste no último quadro: solta arquivo e textura */
        if (!s->rtc_peers[i].id[0]) {
            if (q->remote[i].on || q->remote[i].watching) { memset(&q->remote[i], 0, sizeof q->remote[i]); fv_close(&g_fv[i]); memset(&g_zoom[i], 0, sizeof g_zoom[i]); }
            if (q->remote2[i].on || q->remote2[i].watching) { memset(&q->remote2[i], 0, sizeof q->remote2[i]); fv_close(&g_fv[FV2(i)]); memset(&g_zoom[FV2(i)], 0, sizeof g_zoom[0]); }
            continue;
        }
        bool want2 = (viewing || overlay_live_wanted(i, true) || mini_wants(i, true)) && q->remote2[i].on;
        if (want2 != q->remote2[i].watching) {
            char line[220], id[96]; json_escape(s->rtc_peers[i].id, id, sizeof id);
            snprintf(line, sizeof line, "{\"cmd\":\"share.watch\",\"slot\":1,\"peer\":\"%s\",\"on\":%s}", id, want2 ? "true" : "false");
            cmd(line);
            q->remote2[i].watching = want2;
            if (!want2) { q->remote2[i].has_frame = false; fv_close(&g_fv[FV2(i)]); }
        }
        bool want = (viewing || overlay_live_wanted(i, false) || mini_wants(i, false) || g_thumb_prev[i]) && q->remote[i].on;   /* a miniatura do overlay (jogo na frente) também pede o decodificador */
        if (want != q->remote[i].watching) {
            char line[200], id[96]; json_escape(s->rtc_peers[i].id, id, sizeof id);
            snprintf(line, sizeof line, "{\"cmd\":\"share.watch\",\"peer\":\"%s\",\"on\":%s}", id, want ? "true" : "false");
            cmd(line);
            q->remote[i].watching = want;
            if (!want) { q->remote[i].has_frame = false; fv_close(&g_fv[i]); }
        }
        /* o SOM de uma transmissão só sai na aba Transmissão: a miniatura, o overlay e a thumb do bloco decodificam a imagem, mas ficam mudos */
        {
            static float sent[BRIKS_MAX_PEERS]; static double sent_at[BRIKS_MAX_PEERS]; static bool known[BRIKS_MAX_PEERS];
            if (q->remote[i].audio) {   /* 1.5.20: também sem assistir (o P2P entrega o áudio à força): então vale 0 até eu assistir */
                const float vol = q->remote[i].vol_set ? q->remote[i].volume : 1.0f, tgt = (viewing && q->remote[i].watching) ? vol : 0.0f;
                const double now_s = (double)GetTickCount64() / 1000.0;
                if (!known[i] || fabsf(tgt - sent[i]) > 0.004f || now_s - sent_at[i] > 2.0) {
                    char line[220], id[96]; json_escape(s->rtc_peers[i].id, id, sizeof id);
                    snprintf(line, sizeof line, "{\"cmd\":\"share.volume\",\"peer\":\"%s\",\"volume\":%.2f}", id, tgt);
                    cmd(line); sent[i] = tgt; sent_at[i] = now_s; known[i] = true;
                }
            } else known[i] = false;
        }
    }
    /* capturas sem core (BRIKS_DEMO_PEERS): BRIKS_DEV_SHARE=demo-remote faz o 1º participante "começar a transmitir" (aviso);
       demo-zoom faz o mesmo já no modo Transmissão, com um quadro sintético (grade) para provar o zoom; demo-two põe DUAS telas
       da mesma pessoa no ar */
    { static int demo; const char *dd = getenv("BRIKS_DEV_SHARE");
      if (!demo && dd && !strncmp(dd, "demo-", 5) && s->rtc_peers[0].id[0]) {
        demo = 1;
        const bool eyes = !strcmp(dd, "demo-eyes"); if (eyes) dd = "demo-many";   /* demo-many + olhinho dos OUTROS (share.audience): 1ª pessoa com 2 telas contadas, 2ª com zero, 3ª sem dado (oculto) */
        const bool aud = !strcmp(dd, "demo-audio"); if (aud) dd = "demo-many";   /* demo-many com áudio na 1ª transmissão (barra de volume no bloco) */
        if (!strcmp(dd, "demo-zoom") || !strcmp(dd, "demo-two") || !strcmp(dd, "demo-many")) { s->screen = SCREEN_CALL; s->call_mode = 1; s->call_view = 0; }
        if (!strcmp(dd, "demo-self") || !strcmp(dd, "demo-eye")) {   /* MINHA transmissão no ar com prévia local (teste do aviso de qualidade reduzida): quadro sintético no arquivo da prévia */
            s->screen = SCREEN_CALL; s->call_mode = 1; s->call_view = 0; q->active = true; snprintf(q->label, sizeof q->label, "Tela 1");
            char tmpf[300], pv[300]; demo_frame(tmpf, sizeof tmpf, "self", 1280, 720, 0); preview_path(pv, sizeof pv);
            char *sl = strrchr(pv, 92); if (sl) { *sl = 0; CreateDirectoryA(pv, NULL); *sl = 92; }
            CopyFileA(tmpf, pv, FALSE);
            if (!strcmp(dd, "demo-eye")) {   /* 2 telas minhas no ar; olho: 4 assistindo a 1ª (nomes "e mais 1"), 1 assistindo a 2ª */
                q->second.active = true; copy(q->second.label, sizeof q->second.label, "Tela 2"); q->second.w = 1280; q->second.h = 720;
                char t2[300], p2[300]; demo_frame(t2, sizeof t2, "self2", 1280, 720, 1); preview_path_n(p2, sizeof p2, true); CopyFileA(t2, p2, FALSE);
                jval *v0 = json_parse("{\"ev\":\"share.viewers\",\"slot\":0,\"count\":4,\"peers\":[\"demo-0\",\"demo-1\",\"demo-2\",\"zz\"]}");
                if (v0) { share_handle_event(s, v0, "share.viewers"); json_free(v0); }
                jval *v1 = json_parse("{\"ev\":\"share.viewers\",\"slot\":1,\"count\":1,\"peers\":[\"demo-1\"]}");
                if (v1) { share_handle_event(s, v1, "share.viewers"); json_free(v1); }
            }
        }
        if (!strcmp(dd, "demo-sheen")) { s->screen = SCREEN_CALL; s->call_mode = 0; s->call_view = 0; }   /* modo Call com alguém transmitindo: brilho e avatar com thumb */
        if (!strcmp(dd, "demo-mini")) { s->screen = SCREEN_SOCIAL; s->call_mode = 0; s->call_view = 0; }   /* miniatura da lateral: duas telas no ar, usuário no menu */
        jval *jj = json_parse("{\"ev\":\"share.remote\",\"peer_id\":\"demo-0\",\"state\":\"on\",\"w\":1280,\"h\":720,\"fps\":30,\"kbps\":4000,\"label\":\"Tela 2\"}");
        if (jj) { share_handle_event(s, jj, "share.remote"); json_free(jj); }
        q->remote[0].audio = aud;
        if (!strcmp(dd, "demo-zoom") || !strcmp(dd, "demo-two") || !strcmp(dd, "demo-many") || !strcmp(dd, "demo-mini") || !strcmp(dd, "demo-sheen")) demo_frame(q->remote[0].path, sizeof q->remote[0].path, "tela1", 1280, 720, 0);
        if (getenv("BRIKS_DEMO_SHARE_AUDIO")) q->remote[0].audio = true;
        if (!strcmp(dd, "demo-two") || !strcmp(dd, "demo-many") || !strcmp(dd, "demo-mini")) {   /* a mesma pessoa com DUAS telas no ar */
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
        if (eyes) {
            static const char *ev[] = {
                "{\"ev\":\"share.audience\",\"peer_id\":\"demo-0\",\"slot\":0,\"count\":4,\"names\":[\"Ana\",\"Beto\",\"Caio\"]}",
                "{\"ev\":\"share.audience\",\"peer_id\":\"demo-0\",\"slot\":1,\"count\":1,\"names\":[\"Duda\"]}",
                "{\"ev\":\"share.audience\",\"peer_id\":\"demo-1\",\"slot\":0,\"count\":0,\"names\":[]}" };
            for (int k = 0; k < 3; k++) { jval *je = json_parse(ev[k]); if (je) { share_handle_event(s, je, "share.audience"); json_free(je); } }
        }
      }
    }
    /* ganchos de teste/captura: BRIKS_DEV_SHARE=dialog | dialog-apps | dialog-windows | screen | test (uma vez, com o core pronto) */
    static int dev; const char *d = getenv("BRIKS_DEV_SHARE");
    if (d) cui_request_redraw(ui);
    if (d && !strcmp(d, "demo-eye") && q->vknown[0] && cui_time(ui) > 3.0) { static bool done; if (!done) { done = true; q->viewers[0] = 0; snprintf(q->vnames[0], sizeof q->vnames[0], "%s", ""); q->vknown[1] = false; } }   /* depois: 1ª sem ninguém ("0"), 2ª sem dado (oculto) */
    if (d && !strcmp(d, "qual-slots")) {   /* teste (CHM-34): duas transmissões, cada uma com a sua qualidade; trocar/mudar a 2 não mexe na 1 */
        static int step; double t = cui_time(ui);
        if (step == 0 && t > 0.5) {
            step = 1; q->res = 1; q->fps = FPS_DEFAULT; q->br_mode = 0;                 /* 1ª: 1080p 30 automático */
            q->active = true; q->second.active = true; copy(q->label, sizeof q->label, "Monitor 1"); copy(q->second.label, sizeof q->second.label, "Janela do jogo");
            qual_switch(q, 1); q->res = 0; q->fps = 1; q->br_mode = 3; q->br_custom = 3;   /* 2ª: 720p 15 personalizado 3 Mbps */
            qual_switch(q, 0);
            { qual_t p = qual_enter(q, 1); g_qk[1] = chosen_kbps(q); qual_leave(q, p); }
            { qual_t p = qual_enter(q, 0); g_qk[0] = chosen_kbps(q); qual_leave(q, p); }
            cui_request_redraw(ui);
        } else if (step == 1 && t > 1.5) { step = 2; q->quality_open = true; q->dialog_open = true; q->pick_second = true; qual_switch(q, 1); }
    }
    if (d && !strcmp(d, "fake-local")) {   /* teste sem core: começa/para/recomeça a MINHA transmissão pelo mesmo tratador de eventos do core (1 s liga, 4 s desliga, 7 s liga, 9 s repete o "on") */
        static int step; double t = cui_time(ui); const char *ev = NULL;
        if (step == 0 && t > 1) { step = 1; ev = "{\"ev\":\"share.local\",\"slot\":0,\"state\":\"on\",\"w\":1280,\"h\":720,\"fps\":30,\"kbps\":2500}"; }
        else if (step == 1 && t > 4) { step = 2; ev = "{\"ev\":\"share.local\",\"slot\":0,\"state\":\"off\",\"reason\":\"\"}"; }
        else if (step == 2 && t > 7) { step = 3; ev = "{\"ev\":\"share.local\",\"slot\":0,\"state\":\"on\",\"w\":1280,\"h\":720,\"fps\":30,\"kbps\":2500}"; }
        else if (step == 3 && t > 9) { step = 4; ev = "{\"ev\":\"share.local\",\"slot\":0,\"state\":\"on\",\"w\":1280,\"h\":720,\"fps\":30,\"kbps\":2500}"; }
        if (ev) { jval *jj = json_parse(ev); if (jj) { share_handle_event(s, jj, "share.local"); json_free(jj); } }
        cui_request_redraw(ui);
    }
    if (d && !strcmp(d, "demo-adapt")) {   /* captura: injeta, pelo MESMO tratador do core, o "no ar" e as estatísticas com adapting (formato de share.rs: "cpu" e depois "rede" com reason) e abre a janela de qualidade */
        static int step; const double t = cui_time(ui); const char *ev = NULL;
        if (step == 0 && t > 1.0) { step = 1; ev = "{\"ev\":\"share.local\",\"slot\":0,\"state\":\"on\",\"w\":1920,\"h\":1080,\"fps\":30,\"kbps\":6000}"; }
        else if (step == 1 && t > 1.3) { step = 2; ev = "{\"ev\":\"share.stats\",\"fps\":18,\"kbps\":2100,\"h\":720,\"target_fps\":30,\"target_kbps\":6000,\"est_kbps\":2600,\"enc_ms\":41.0,\"cpu_pct\":96,\"rtt_ms\":48,\"rtx\":3,\"loss_pct\":1.5,\"adapting\":\"cpu\",\"reason\":\"\"}"; g_dev_open_quality = true; }
        else if (step == 2 && t > 8.0) { step = 3; ev = "{\"ev\":\"share.stats\",\"fps\":24,\"kbps\":1800,\"h\":720,\"target_fps\":30,\"target_kbps\":6000,\"est_kbps\":1900,\"enc_ms\":9.0,\"cpu_pct\":40,\"rtt_ms\":180,\"rtx\":40,\"loss_pct\":6.0,\"adapting\":\"rede\",\"reason\":\"perda\"}"; }
        if (ev) { jval *jj = json_parse(ev); if (jj) { share_handle_event(s, jj, strstr(ev, "share.local") ? "share.local" : "share.stats"); json_free(jj); } }
        cui_request_redraw(ui);
    }
    if (d && !strcmp(d, "demo-dialog")) { static int once; if (!once && cui_time(ui) > 0.8) { once = 1; s->screen = SCREEN_CALL; q->loading = false;
            jval *js = json_parse("{\"ev\":\"share.sources\",\"screens\":[{\"id\":\"0\",\"name\":\"Monitor 1\",\"sub\":\"1920 x 1080\"},{\"id\":\"1\",\"name\":\"Monitor 2\",\"sub\":\"2560 x 1440\"}],\"windows\":[]}");
            if (js) { share_handle_event(s, js, "share.sources"); json_free(js); } q->loading = false; q->dialog_open = true; } }   /* captura sem core: diálogo de compartilhar tela aberto */
    if (d && !strcmp(d, "demo-quality")) { static int once; if (!once && cui_time(ui) > 1.0) { once = 1; g_dev_open_quality = true; } }   /* captura sem core: abre a janela de qualidade */
    if (d && core_state() == CORE_READY && s->in_room) {
        if (!strcmp(d, "adapt") && q->active) { static double t0; if (t0 == 0) t0 = cui_time(ui); if (cui_time(ui) - t0 > 3 && dev < 3) { dev = 3; g_dev_open_quality = true; } }
        if (dev == 0 && !strcmp(d, "dialog-quality")) { q->quality_open = true; q->br_mode = 3; q->br_custom = 35; q->res = 2; q->fps = FPS_N - 1; }
        if (dev == 0) { dev = 1; if (!strncmp(d, "dialog", 6) || !strcmp(d, "screen") || !strcmp(d, "adapt")) share_open_dialog(s);
            if (!strcmp(d, "dialog-apps")) q->audio_mode = 1; if (!strcmp(d, "dialog-windows")) q->tab = 1;
            if (!strcmp(d, "test")) { if (s->rtc_session_ready && !strcmp(s->rtc_state, "conectado") && cui_time(ui) > 4.0) send_start(s, "test", "0", "Padrão de teste"); else dev = 0; } }
        else if (dev == 1 && (!strcmp(d, "screen") || !strcmp(d, "adapt")) && !q->loading && q->nsrc[0] > 0 && s->rtc_authed_count > 0) { dev = 2; q->tab = 0; q->sel[0] = 0; start_selected(s); }
        if (dev == 1 && !strcmp(d, "dialog-apps") && q->napps > 0) { dev = 2; q->apps[0].on = true; }
    }
}

/* ---------- peças de interface ---------- */
static void paint_grad(cui_ctx *ui, cui_rect r, void *user) { float rad = *(const float *)user; cui_draw_rect_gradient(ui, r, CUI_CORNERS(rad), B_C1, B_C2); }

/* segmentado em vidro: ativa = pílula em degradê; devolve true se mudou */
static float g_qw;   /* largura útil do painel de qualidade neste quadro (0 = sem limite): os segmentos quebram em 2+ linhas quando não cabem */
static bool seg(cui_ctx *ui, const char *id, const char *const *labels, const int *counts, int n, int *idx) {
    static float rad = 8; bool changed = false;
    cui_push_id(ui, id);
    cui_box(ui, .dir = CUI_ROW, .gap = 3, .pad = 3, .radius = 11, .bg = B_GLASS, .border = 1, .border_color = B_GLASS_BORDER, .wrap = g_qw > 0, .max_width = g_qw > 0 ? g_qw : 0) {
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
    if (!q->second.active && !q->second.starting && !q->pick_second && q->qsel == 1) qual_switch(q, 0);   /* a 2ª terminou: volta a editar a 1ª */
    cui_column(ui, .gap = 10, .width = CUI_FILL) {
        if (q->second.active || q->second.starting || q->pick_second) {   /* duas transmissões: escolhe qual delas os ajustes abaixo valem */
            char l0[80], l1[80];
            snprintf(l0, sizeof l0, "Transmissão 1: %.16s%s", q->label[0] ? q->label : "tela", strlen(q->label) > 16 ? "…" : "");
            snprintf(l1, sizeof l1, "Transmissão 2: %.16s%s", q->second.label[0] ? q->second.label : "tela", strlen(q->second.label) > 16 ? "…" : "");
            const char *labs[2] = { l0, l1 };
            cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, "Ajustes de", .size = 13, .color = B_MUTED);
                int sel = q->qsel; if (seg(ui, "qsel", labs, NULL, 2, &sel)) qual_switch(q, sel);
            }
        }
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .gap = 6, .wrap = true, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, "Resolução", .size = 13, .color = B_MUTED);
            ch |= seg(ui, "res", RES_LABEL, NULL, 4, &q->res);
        }
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .gap = 6, .wrap = true, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, "Quadros por segundo", .size = 13, .color = B_MUTED);
            ch |= seg(ui, "fps", FPS_LABEL, NULL, FPS_N, &q->fps);
        }
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .gap = 6, .wrap = true, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
            cui_column(ui, .gap = 1, .max_width = g_qw > 0 ? fmaxf(g_qw - 190.0f, 150.0f) : 0, .width = CUI_FILL) {
                wrap_line(ui, "Se a rede ou o PC apertar", 13, B_MUTED);
                wrap_line(ui, q->content ? "Fluidez: cede resolução antes de quadros" : "Nitidez: cede quadros antes de resolução", 11, CUI_RGBA(0xa592b8aa));
            }
            static const char *const pr[2] = { "Nitidez", "Fluidez" };
            ch |= seg(ui, "pri", pr, NULL, 2, &q->content);
        }
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .gap = 6, .wrap = true, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
            cui_column(ui, .gap = 1, .width = CUI_FILL) {
                wrap_line(ui, "Codec de vídeo", 13, B_MUTED);
                wrap_line(ui, q->codec == 1 ? "VP9: imagem mais nítida com a mesma internet, mas pesa mais no processador. Vale no servidor de chamadas e no P2P; no P2P, se alguém não conseguir abrir, o Briks volta sozinho ao H.264. Depois da 1ª transmissão da chamada, trocar o codec só vale na próxima chamada."
                     : q->codec == 2 ? "VP8: mais antigo e leve, bom para PCs fracos, com qualidade um pouco menor. Vale no servidor de chamadas e no P2P; no P2P, se alguém não conseguir abrir, volta ao H.264. Depois da 1ª transmissão da chamada, trocar o codec só vale na próxima chamada."
                     : "H.264: o mais comum. Funciona em qualquer PC, usa a placa de vídeo para não pesar e vale também no servidor de chamadas. Na dúvida, fique com ele.", 11, CUI_RGBA(0xa592b8aa));
            }
            static const char *const cd[3] = { "H.264", "VP9", "VP8" };
            ch |= seg(ui, "codec", cd, NULL, 3, &q->codec);
        }
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .gap = 6, .wrap = true, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
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

/* ---------- canal de áudio ao vivo (popover da barra) ---------- */
static void send_audio_live(app_state_t *s) {
    share_state_t *q = &s->sh;
    char line[640], pids[400] = ""; size_t o = 0;
    for (int i = 0; i < q->napps && o < sizeof pids - 16; i++) if (q->apps[i].on) o += (size_t)snprintf(pids + o, sizeof pids - o, "%s%u", o ? "," : "", q->apps[i].pid);
    snprintf(line, sizeof line, "{\"cmd\":\"share.audio.set\",\"audio\":\"%s\",\"pids\":[%s],\"kind\":\"%s\",\"id\":\"%s\"}", q->audio_mode == 1 ? "apps" : q->audio_mode == 0 ? "source" : "none", pids, q->last_kind, q->last_id);
    cmd(line);
}
static void share_audio_live(cui_ctx *ui, app_state_t *s) {
    share_state_t *q = &s->sh;
    static double last_poll; if (cui_time(ui) - last_poll > 2.0) { last_poll = cui_time(ui); cmd("{\"cmd\":\"share.audio.list\"}"); }
    cui_request_wakeup(ui, 1.0f);
    cui_text(ui, "Som da transmissão", .font = CUI_FONT_DISPLAY, .size = 15);
    static const char *const am[2] = { "Da fonte", "Escolher apps" };
    int mode = q->audio_mode == 1 ? 1 : 0;
    if (cui_segmented(ui, "amlive", am, 2, &mode, .full_width = true, .size = CUI_SIZE_SM)) { q->audio_mode = mode; send_audio_live(s); }
    if (q->audio_mode == 1) {
        int marked = 0;
        if (!q->napps) cui_text(ui, "Nenhum app com áudio aberto agora.", .size = 13, .color = B_MUTED);
        cui_box(ui, .width = CUI_FILL, .gap = 6, .height = q->napps > 4 ? CUI_PX(4 * 50 + 18) : CUI_FIT, .scroll_y = q->napps > 4) {
            for (int i = 0; i < q->napps; i++) {
                share_app_t *a = &q->apps[i]; if (a->on) marked++;
                cui_push_id_int(ui, 600 + i);
                cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(46), .pad_x = 12, .gap = 10, .radius = 10, .bg = B_GLASS_SOFT, .border = 1, .border_color = B_GLASS_BORDER_SOFT, .align = CUI_ALIGN_CENTER) {
                    cui_icon(ui, CUI_ICON_MUSIC, 14, B_C3);
                    cui_column(ui, .gap = 1, .width = CUI_FILL) {
                        cui_text(ui, a->name, .size = 13, .nowrap = true);
                        cui_text(ui, a->playing ? "Tocando agora" : "Em silêncio", .size = 11, .color = a->playing ? B_ONLINE : B_MUTED);
                    }
                    if (cui_switch(ui, "##on", &a->on)) send_audio_live(s);
                }
                cui_pop_id(ui);
            }
        }
        cui_text(ui, marked ? "Só o som dos apps marcados sai na transmissão. O Briks e o Discord nunca entram." : "Nenhum app marcado: a transmissão fica sem som até marcar um.", .size = 12, .color = B_MUTED);
    } else cui_text(ui, "Entra o som da fonte (tela: tudo menos o Briks e o Discord; janela: só o programa dela).", .size = 12, .color = B_MUTED);
}

/* ---------- diálogo ---------- */
static bool g_pop_req; static int g_pop_grace; static cui_id g_shqd_id;   /* painel lateral de qualidade: pedido de abertura, tolerância de quadros e id (resolvido dentro do escopo do diálogo) */
void ui_share_dialog(cui_ctx *ui, app_state_t *s) {
    share_state_t *q = &s->sh;
    if (!q->dialog_open) { if (g_shqd_id) cui_popover_set_id(ui, g_shqd_id, false); g_pop_req = false; return; }   /* fechar o diálogo fecha o painel */
    static double last_poll;   /* "Tocando agora" atualiza sozinho enquanto o diálogo está aberto */
    if (q->audio_mode == 1 && cui_time(ui) - last_poll > 2.0) { last_poll = cui_time(ui); cmd("{\"cmd\":\"share.audio.list\"}"); cui_request_redraw(ui); }
    cui_vec2 vp = cui_viewport_size(ui);
    float inner = 3 * CARD_W + 2 * 12;
    /* ajustes de qualidade ao LADO do diálogo (painel separado, mesma altura de topo) quando a janela comporta os dois lado a lado; senão, expandidos dentro como antes */
    const float dlg_w = fminf(inner + 48, fmaxf(vp.x - 48, 520));
    const float side_room = vp.x - dlg_w - 12.0f - 32.0f;
    const bool side = q->quality_open && side_room >= 300.0f;
    const float side_w = fminf(side_room, 460.0f);
    static float off_x;   /* o diálogo desliza para a esquerda para o par ficar centrado na janela */
    { const float tgt = side ? -(side_w + 12.0f) * 0.5f : 0.0f; if (fabsf(tgt - off_x) > 0.5f) { off_x += (tgt - off_x) * 0.35f; cui_request_redraw(ui); } else off_x = tgt; }
    const cui_id pan = cui_dialog_panel_id(ui, "Compartilhar tela (beta)");   /* no mesmo escopo de ids de quem chama o diálogo */
    cui_dialog(ui, "Compartilhar tela (beta)", &q->dialog_open, .offset_x = off_x, .description = q->pick_second ? "Escolha a 2ª tela: ela vai ao ar junto com a primeira" : "Escolha a fonte e confirme", .width = fminf(inner + 48, fmaxf(vp.x - 48, 520))) {
        int t = q->tab, n = q->nsrc[t];
        /* com os ajustes de qualidade abertos, fontes e áudio viram uma linha de resumo: o diálogo tem de caber em 800 px de altura */
        if (!q->quality_open || side) {
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
        if (q->quality_open && !side) { g_qw = 0; quality_controls(ui, q); }
        cui_separator(ui);
        beta_note(ui, "Em queda brusca de rede a imagem pode cair bastante ou travar por alguns segundos antes de voltar.");
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, q->audio_mode == 2 ? "Sem áudio" : q->audio_mode == 1 ? "Áudio: só os apps marcados" : "Áudio: da fonte", .size = 12, .color = B_MUTED);
            char b[140]; bool ok = n > 0 && q->sel[t] < n && !q->loading;
            if (ok) snprintf(b, sizeof b, t == 0 ? "Transmitir %s" : "Transmitir janela", q->src[t][q->sel[t]].name); else snprintf(b, sizeof b, "Transmitir");
            if (cui_button(ui, b, .variant = CUI_BUTTON_PRIMARY, .disabled = !ok)) start_selected(s);
        }
        /* painel lateral: grudado à direita do diálogo, topo alinhado, rola por dentro; Esc fecha só ele; clique no diálogo não o fecha (sticky) */
        g_shqd_id = cui_make_id(ui, "shqd");
        { const bool isop = cui_popover_is_open(ui, "shqd");
          if (!side) { g_pop_req = false; if (isop) cui_popover_set_id(ui, g_shqd_id, false); }
          else if (!g_pop_req) { g_pop_req = true; g_pop_grace = 3; cui_popover_open(ui, "shqd"); }
          else if (!isop) { if (g_pop_grace > 0) g_pop_grace--; else { q->quality_open = false; g_pop_req = false; } } }   /* Esc fechou só o painel */
        {
            const cui_rect pr = cui_get_rect(ui, pan);
            float ph = pr.h > 0 ? pr.h : 560.0f;
            g_qw = side_w - 32.0f - 8.0f;
            cui_popover(ui, "shqd", .placement = CUI_PLACE_RIGHT_START, .width = side_w, .pad = 16, .gap = 12, .anchor = pan, .sticky = true, .offset_x = off_x) {
                cui_text(ui, "Qualidade da transmissão", .font = CUI_FONT_DISPLAY, .size = 15);
                const cui_rect qin = cui_get_rect(ui, cui_make_id(ui, "shqd-in"));
                const float maxh = fmaxf(ph - 32.0f - 24.0f - 10.0f, 120.0f);
                cui_scroll(ui, .id = "shqd-body", .width = CUI_FILL, .height = CUI_PX(qin.h > 0 ? fminf(maxh, qin.h + 2.0f) : maxh), .gap = 0, .fade_edges = true) {
                    cui_box(ui, .id = "shqd-in", .width = CUI_FILL, .gap = 10, .pad_right = 8) { quality_controls(ui, q); }
                }
            }
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
            if (!modal && briks_clicked_r(ui, "b2", 25.0f)) { if (on2) share_stop_second(s); else { share_open_dialog(s); q->pick_second = true; qual_switch(q, 1); } }
        }
        if (q->active || q->second.active) {   /* olho: quantas pessoas assistem às minhas telas e quem são (P2P e servidor de chamadas); sem ninguém mostra 0 */
            vsep(ui);
            static char ebuf[40], etip[640]; const bool two = q->second.active && q->active;
            if (two) snprintf(ebuf, sizeof ebuf, "%d · %d", q->viewers[0], q->viewers[1]); else snprintf(ebuf, sizeof ebuf, "%d", q->active ? q->viewers[0] : q->viewers[1]);
            etip[0] = 0;
            for (int sl = 0; sl < 2; sl++) if ((sl == 0 && q->active) || (sl == 1 && q->second.active)) {
                size_t l = strlen(etip); snprintf(etip + l, sizeof etip - l, "%s%s", l ? "\n" : "", two ? (sl ? "Tela 2: " : "Tela 1: ") : "Assistindo: ");
                if (q->viewers[sl] <= 0) { l = strlen(etip); snprintf(etip + l, sizeof etip - l, "ninguém"); continue; }
                int shown = 0;
                for (int k = 0; k < q->vidn[sl]; k++) {
                    const char *nm = "Participante";
                    for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (s->rtc_peers[i].id[0] && !strcmp(s->rtc_peers[i].id, q->vids[sl][k]) && s->rtc_peers[i].name[0]) nm = s->rtc_peers[i].name;
                    l = strlen(etip); snprintf(etip + l, sizeof etip - l, "%s%s", shown ? ", " : "", nm); shown++;
                }
                if (q->viewers[sl] > shown) { l = strlen(etip); snprintf(etip + l, sizeof etip - l, " e mais %d", q->viewers[sl] - shown); }
            }
            cui_box(ui, .id = "bey", .dir = CUI_ROW, .gap = 6, .pad_x = 12, .height = CUI_FILL, .align = CUI_ALIGN_CENTER) {
                cui_icon(ui, CUI_ICON_EYE, 14, B_TEXT); cui_text(ui, ebuf, .size = 12, .nowrap = true);
            }
            cui_tooltip(ui, etip);
        }
        if (q->active) {   /* "Com áudio" / "Sem áudio": liga e desliga o áudio da transmissão AO VIVO (a faixa continua aberta: nada é renegociado) */
            vsep(ui);
            const bool snd = q->audio_on && !q->audio_muted;   /* só a 1ª tela leva áudio; sem áudio desde o início não há faixa para ligar */
            const cui_color ac = snd ? CUI_RGB(0x7be8a8) : q->audio_on ? CUI_RGB(0xe88a8a) : B_MUTED;
            cui_box(ui, .id = "ba", .dir = CUI_ROW, .gap = 6, .pad_x = 12, .height = CUI_FILL, .align = CUI_ALIGN_CENTER) {   /* sem fundo próprio: nada reto vaza no contorno arredondado */
                cui_icon(ui, snd ? CUI_ICON_VOLUME_2 : CUI_ICON_VOLUME_X, 14, ac);
                cui_text(ui, snd ? "Com áudio" : "Sem áudio", .size = 12, .color = ac, .nowrap = true);
            }
            cui_tooltip(ui, !q->audio_on ? "Esta transmissão começou sem áudio. Pare e compartilhe de novo escolhendo o áudio para poder ligar aqui."
                                         : snd ? "Desligar o áudio da transmissão (a imagem continua)" : "Ligar o áudio da transmissão");
            if (!modal && q->audio_on && briks_clicked_r(ui, "ba", 25.0f)) {
                q->audio_muted = !q->audio_muted;
                cmd(q->audio_muted ? "{\"cmd\":\"share.audio.mute\",\"muted\":true}" : "{\"cmd\":\"share.audio.mute\",\"muted\":false}");
            }
        }
        if (q->active && q->audio_on) {   /* 1.4.3: escolher de quais apps vem o som, AO VIVO (o core troca a mistura sem renegociar nada) */
            vsep(ui);
            cui_box(ui, .dir = CUI_STACK, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_START) {
                cui_box(ui, .id = "bc", .dir = CUI_ROW, .gap = 6, .pad_x = 12, .height = CUI_FILL, .align = CUI_ALIGN_CENTER) {
                    cui_icon(ui, CUI_ICON_MUSIC, 14, q->audio_mode == 1 ? CUI_RGB(0x7be8a8) : B_TEXT);
                    cui_text(ui, q->audio_mode == 1 ? "Apps" : "Canal", .size = 12, .color = q->audio_mode == 1 ? CUI_RGB(0x7be8a8) : B_TEXT, .nowrap = true);
                }
                cui_tooltip(ui, "Escolher de quais apps vem o som da transmissão (ex.: só o Spotify), sem parar a transmissão");
                if (!modal && briks_clicked_r(ui, "bc", 25.0f)) { cui_popover_open(ui, "shaud"); cmd("{\"cmd\":\"share.audio.list\"}"); }
                cui_canvas(ui, "shaudanchor", CUI_FILL, CUI_PX(1), NULL, NULL, 0);
                cui_popover(ui, "shaud", .placement = CUI_PLACE_TOP, .width = 380, .pad = 14, .gap = 10) { share_audio_live(ui, s); }
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
            cui_text(ui, failed ? (strlen(q->err) > 46 ? "A transmissão parou · toque para ver o motivo" : q->err) : (q->active && q->black) ? "Imagem preta" : chip, .size = 12, .color = failed ? CUI_RGB(0xffb4b4) : adapting ? CUI_RGB(0xf5d9a0) : B_TEXT, .nowrap = true);
            if (failed && q->last_kind[0] && !q->starting && cui_button(ui, "Tentar de novo", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) { copy(g_last_err, sizeof g_last_err, q->err); q->err[0] = 0; send_start(s, q->last_kind, q->last_id, q->label); }
        }
        if ((!modal && briks_clicked_r(ui, "q", 25.0f)) || g_dev_open_quality) { g_dev_open_quality = false; if (q->err[0]) copy(g_last_err, sizeof g_last_err, q->err); q->err[0] = 0; cui_popover_open(ui, "shq"); }
            { const cui_rect cr = cui_get_rect(ui, cui_make_id(ui, "q")); cui_canvas(ui, "shqanchor", CUI_FILL, CUI_PX((cr.h > 0 ? cr.h : 40.0f) + 24.0f), NULL, NULL, 0); }   /* 12 px acima do topo do chip: o painel sobe para fora da barra, sem encostar nela */   /* âncora: topo do chip, com a largura dele */
        /* painel responsivo: largura até 520 mas nunca mais que a janela, altura limitada ao espaço ACIMA da barra (o cui limita a 420) com rolagem
           interna; os textos quebram e os segmentos descem de linha. O cui já mantém o popover dentro da janela nos 4 lados. */
        const cui_vec2 qvp = cui_viewport_size(ui); const cui_rect qar = cui_get_rect(ui, cui_make_id(ui, "shqanchor"));
        const float qpw = fminf(520.0f, fmaxf(qvp.x - 24.0f, 240.0f));
        float qh = 560.0f; if (qar.w > 0) qh = qar.y - (titlebar_height() + 4.0f) - 10.0f - 32.0f - 8.0f - 28.0f;   /* menos gap, padding, margem e o título */
        qh = fminf(fmaxf(qh, 90.0f), 640.0f);
        g_qw = qpw - 32.0f - 8.0f;
        cui_popover(ui, "shq", .placement = CUI_PLACE_TOP, .width = qpw, .pad = 16, .gap = 10) {
            cui_text(ui, "Qualidade da transmissão", .font = CUI_FONT_DISPLAY, .size = 15);
            const cui_rect qin = cui_get_rect(ui, cui_make_id(ui, "shq-in"));   /* altura do conteúdo no quadro anterior: o painel não fica com vazio sobrando, e rola quando passa do limite */
            cui_scroll(ui, .id = "shq-body", .width = CUI_FILL, .height = CUI_PX(qin.h > 0 ? fminf(qh, qin.h + 2.0f) : qh), .gap = 0, .fade_edges = true) {
            cui_box(ui, .id = "shq-in", .width = CUI_FILL, .gap = 10, .pad_right = 8) {
            beta_note(ui, "Em queda brusca de rede a imagem pode cair bastante ou travar por alguns segundos. Com mais de um espectador, quem estiver com a rede ruim pode ficar sem imagem.");
            /* as escolhas valem sozinhas: transmitindo, ~0,4 s depois da última mudança o codificador é reiniciado com elas (um clique
               seguido de outro não reinicia duas vezes); fora da transmissão ficam salvas para a próxima */
            static double apply_at = -1;
            if (quality_controls(ui, q) && q->active) apply_at = cui_time(ui) + 0.4;
            if (apply_at >= 0) { if (!q->active) apply_at = -1; else if (cui_time(ui) >= apply_at) { apply_at = -1; if (q->qsel == 1 && q->second.active) send_start_second(s, q->second.kind, q->second.id, q->second.label); else send_start(s, q->last_kind, q->last_id, q->label); } else cui_request_wakeup(ui, 0.2f); }
            if (q->active) g_last_err[0] = 0;
            else if (g_last_err[0]) wrap_line(ui, g_last_err, 12.5f, CUI_RGB(0xffb4b4));   /* por que a transmissão parou (o chip só mostra um resumo) */
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
            }   /* shq-in */
            }   /* shq-body */
        }
        }
    }
    cui_pop_id(ui);
}

/* ---------- palco do modo Transmissão ---------- */
static int g_sfocus = -1;   /* transmissão fixada no Foco (chave de emit_tile); -1 = a primeira da lista */
static bool s_modal_hint;   /* com um diálogo aberto por cima, o clique não vale para a miniatura */
void share_peer_gone(app_state_t *s, int slot) {
    if (slot < 0 || slot >= BRIKS_MAX_PEERS) return;
    share_state_t *q = &s->sh;
    memset(&q->remote[slot], 0, sizeof q->remote[slot]); memset(&q->remote2[slot], 0, sizeof q->remote2[slot]); memset(q->aud[slot], 0, sizeof q->aud[slot]);
    fv_close(&g_fv[slot]); fv_close(&g_fv[FV2(slot)]); fv_close(&g_thumb[slot]);
    memset(&g_zoom[slot], 0, sizeof g_zoom[slot]); memset(&g_zoom[FV2(slot)], 0, sizeof g_zoom[0]);
    if (g_notice.slot == slot) g_notice.slot = -1;
    if (g_sfocus == slot || g_sfocus == FV2(slot)) g_sfocus = -1;
}
static bool g_prev_note_off;   /* o usuário dispensou o aviso da prévia (vale até fechar o app) */
static cui_rect g_trect[16];   /* retângulos das transmissões do último quadro */
static bool g_tg_press, g_tg_release, g_tile_grid, g_tg_down; static cui_vec2 g_tg_at; static int g_tg_key = -1, g_tile_opens;   /* clique no bloco da Grade abre em Foco */
static bool pt_in(cui_rect r, cui_vec2 p);
static int g_eye = -1; static char g_eye_tip[220];   /* olho + número de quem assiste à minha tela neste bloco (-1 = oculto) */
static app_state_t *g_vt_s;   /* estado do app para o menu do botão direito do bloco (volume da pessoa e da transmissão) */
static bool video_tile(cui_ctx *ui, int key, cui_texture tex, int tw, int th, const char *title, const char *chip, const char *wait, float *volume, cui_image_view *zv, bool compact) {
    int zoom_pct = 100; bool clicked = false;
    if (compact) { chip = NULL; volume = NULL; zv = NULL; }   /* miniatura da faixa do Foco: só a imagem e o nome; clicar traz para o destaque */
    if (key >= 0 && key < 2 * FV_N) g_zoom_pct[key] = 100;
    cui_push_id_int(ui, 800 + key);
    cui_rect r = cui_get_rect(ui, cui_make_id(ui, "vt"));   /* tamanho do quadro anterior: o vídeo cabe inteiro, sem distorcer */
    if (g_tiles < 16) g_trect[g_tiles] = r;   /* onde cada transmissão está: o que sobra do palco é área vazia (clique alterna grade/foco) */
    g_tiles++;
    /* 1.5.20: nome, olhinho, qualidade e volume só aparecem com o mouse sobre a transmissão (a miniatura compacta mantém o nome) */
    const bool hov = compact || (r.w > 0 && pt_in(r, cui_mouse_pos(ui)) && !s_modal_hint);
    cui_box(ui, .id = "vt", .dir = CUI_STACK, .width = CUI_FILL, .height = CUI_FILL, .radius = 20, .bg = CUI_RGBA(0x1a0a2680), .border = 1, .border_color = CUI_RGBA(0x6d5cf655)) {
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .gap = 10) {
            if (tex && tw > 0 && th > 0 && r.w > 60 && r.h > 60) {
                /* O retângulo exibido sai do container e da razão de aspecto SUAVIZADA, nunca do tamanho do quadro decodificado: trocar a
                   resolução da fonte (1080p <-> 720p), a redução ou a textura não faz a imagem diminuir/aumentar (a textura antiga fica até a nova chegar) */
                float aw = r.w - 16, ah = r.h - 16, dw, dh;
                const float ar = (key >= 0 && key < 2 * FV_N) ? share_aspect_update(&g_aspect[key], tw, th) : (float)tw / (float)th;
                share_fit_rect(aw, ah, ar, &dw, &dh);
                const float sc = dw / (float)tw;
                if (key >= 0 && key < 2 * FV_N) g_fv[key].want = (zv && zv->zoom > sc * 1.01f) ? 0 : (int)floorf(dw);   /* com zoom, o quadro inteiro; senão, o tamanho em que aparece */
                /* a imagem tem o próprio raio e fica 8 px para dentro da moldura (raio 20): cantos sempre curvos, nada reto vaza */
                if (zv) {
                    /* quem assiste: roda do mouse ou pinça ampliam em torno do ponteiro, arrastar move; a moldura fica do tamanho
                       da imagem encaixada (cantos curvos, nada vaza) e o zoom nunca fica abaixo do encaixe */
                    cui_box(ui, .width = CUI_PX(floorf(dw)), .height = CUI_PX(floorf(dh)), .radius = 13, .clip = true) {
                        cui_image_viewer_ex(ui, "zv", tex, tw, th, zv, &(cui_image_viewer_opts){ .width = CUI_FILL, .height = CUI_PX(floorf(dh)), .max_zoom = sc * 8, .no_toolbar = true });
                    }
                    if (share_zoom_clamp(&zv->zoom, &zv->cx, &zv->cy, sc, 8)) zoom_pct = share_zoom_pct(zv->zoom, sc);
                    if (key >= 0 && key < 2 * FV_N) g_zoom_pct[key] = zoom_pct;
                } else cui_image(ui, tex, .width = CUI_PX(floorf(dw)), .height = CUI_PX(floorf(dh)), .radius = 13);
            } else {
                if (key >= 0 && key < 2 * FV_N && !tex) share_aspect_reset(&g_aspect[key]);   /* tile sem imagem: a próxima transmissão começa do zero */
                cui_icon(ui, CUI_ICON_MONITOR, 34, B_C3);
                cui_text(ui, wait, .size = 14, .color = B_MUTED);
            }
        }
        if (hov) cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_END, .pad = compact ? 8 : 18) {
            cui_card_begin(ui, &(cui_card_opts){ .id = "lab", .dir = CUI_ROW, .gap = 7, .pad_x = compact ? 8 : 12, .pad_y = compact ? 4 : 8, .align = CUI_ALIGN_CENTER });
            if (!compact) cui_icon(ui, CUI_ICON_MONITOR, 14, B_C3);
            cui_text(ui, title, .size = compact ? 11 : 14, .nowrap = true);
            if (g_eye >= 0 && !compact) {
                cui_box(ui, .id = "eyec", .dir = CUI_ROW, .gap = 4, .pad_left = 8, .pad_right = 6, .pad_y = 2, .radius = 10, .bg = CUI_RGBA(0xffffff12), .align = CUI_ALIGN_CENTER) {
                    cui_icon(ui, CUI_ICON_EYE, 13, B_C3); cui_textf(ui, "%d", g_eye);
                }
                if (briks_hover(ui, "eyec")) briks_tip(g_eye_tip);
            }
            cui_card_end(ui);
        }
        if (hov && chip && chip[0]) cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_END, .justify = CUI_JUSTIFY_START, .pad = 18) { briks_status_pill(ui, chip); }
        if (zv && zoom_pct > 100) {   /* selo do zoom: clicar volta a 100% (duplo clique na imagem e a tecla 0 também) */
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_START, .pad = 18) {
                char z[48]; snprintf(z, sizeof z, "%d%% · voltar a 100%%", zoom_pct);
                cui_box(ui, .id = "zrst") { if (cui_button(ui, z, .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) memset(zv, 0, sizeof *zv); }
            }
        }
        if (!compact && !g_prev_note_off && (key == BRIKS_MAX_PEERS || key == FV2(BRIKS_MAX_PEERS)) && tex) {   /* só no tile da PRÓPRIA transmissão: a prévia local é reduzida, quem assiste vê em boa qualidade */
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_END, .justify = CUI_JUSTIFY_END, .pad = 18) {
                cui_box(ui, .id = "pnote", .dir = CUI_ROW, .gap = 6, .pad_left = 10, .pad_right = 4, .pad_y = 3, .radius = 12, .bg = CUI_RGBA(0x0a0414b0), .align = CUI_ALIGN_CENTER, .max_width = fmaxf(r.w - 300.0f, 170.0f)) {
                    cui_text(ui, "Prévia com qualidade reduzida: quem assiste vê a imagem em boa qualidade.", .size = 11, .color = B_MUTED);
                    cui_box(ui, .id = "pnx", .width = CUI_PX(20), .height = CUI_PX(20), .radius = 10, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) { cui_icon(ui, CUI_ICON_X, 11, B_MUTED); }
                    if (briks_hover(ui, "pnx")) briks_tip("Dispensar este aviso");
                    if (briks_clicked_r(ui, "pnx", 10.0f)) g_prev_note_off = true;
                }
            }
        }
        if (hov && volume) {   /* volume do áudio desta transmissão, só para quem assiste */
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_END, .justify = CUI_JUSTIFY_END, .pad = 18) {
                cui_box(ui, .id = "volbar", .dir = CUI_ROW, .gap = 8, .pad_x = 12, .height = CUI_PX(36), .width = CUI_PX(190), .radius = 18, .bg = B_GLASS_STRONG, .border = 1, .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER) {
                    briks_volume(ui, "vol", volume, 0.0f, 2.0f, 0.01f, NULL, false);   /* 1.4.4: ícone muta/desmuta, roda ajusta (volume_ctl.c) */
                }
            }
        }
        {   /* botão direito na transmissão de alguém: o mesmo menu do bloco da chamada, com volume/mudo da PESSOA e da TRANSMISSÃO, cada um no seu controle */
            const int peer = (key >= 0 && key < BRIKS_MAX_PEERS) ? key : (key >= FV_N && key < FV_N + BRIKS_MAX_PEERS) ? key - FV_N : -1;
            if (!compact && g_vt_s && peer >= 0 && g_vt_s->rtc_peers[peer].id[0]) {
                static int g_svm_key = -2; static cui_vec2 g_svm_at;
                app_state_t *as = g_vt_s; const cui_rect vr =cui_get_rect(ui, cui_make_id(ui, "vt")); const cui_vec2 mp = cui_mouse_pos(ui);
                if (!s_modal_hint && !briks_blocked() && cui_mouse_clicked(ui, CUI_MOUSE_RIGHT) && vr.w > 0 && pt_in(vr, mp)) { g_svm_key = key; g_svm_at = mp; cui_popover_open(ui, "svmenu"); }
                /* 1.5.21: o menu abre onde o botão direito foi clicado e com o tamanho normal (âncora de 1 px nesse ponto), não esticado no topo da transmissão */
                if (g_svm_key == key && vr.w > 0) cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_START, .pad_left = fmaxf(g_svm_at.x - vr.x, 0.0f), .pad_top = fmaxf(g_svm_at.y - vr.y, 0.0f)) {
                    cui_canvas(ui, "svpos", CUI_PX(1), CUI_PX(1), NULL, NULL, 0);
                }
                cui_popover(ui, "svmenu", .anchor = cui_make_id(ui, "svpos"), .placement = CUI_PLACE_BOTTOM_START, .pad = 6, .gap = 4) {
                    cui_menu_label(ui, as->rtc_peers[peer].name[0] ? as->rtc_peers[peer].name : "Participante");
                    cui_menu_label(ui, "Voz");
                    if (!as->rtc_peers[peer].vol_set) { as->rtc_peers[peer].vol_set = true; as->rtc_peers[peer].volume = 100.0f; }
                    cui_box(ui, .dir = CUI_ROW, .width = CUI_PX(220), .gap = 8, .pad_x = 10, .pad_y = 4, .align = CUI_ALIGN_CENTER) {
                        bool mt = as->rtc_peers[peer].muted_by_me;
                        const int ch = briks_volume(ui, "pvol", &as->rtc_peers[peer].volume, 0.0f, 200.0f, 1.0f, &mt, true);
                        if (ch & VOLCTL_MUTE) rtc_mute_peer(as, peer, mt);
                        if (ch & VOLCTL_VOL) rtc_set_peer_volume(as, peer, as->rtc_peers[peer].volume / 100.0f);
                    }
                    if (key < BRIKS_MAX_PEERS && as->sh.remote[peer].audio) {
                        cui_menu_label(ui, "Transmissão");
                        float *sv = share_remote_volume(as, peer);
                        cui_box(ui, .dir = CUI_ROW, .width = CUI_PX(220), .gap = 8, .pad_x = 10, .pad_y = 4, .align = CUI_ALIGN_CENTER) {
                            float pct = *sv * 100.0f;
                            if (briks_volume(ui, "svol", &pct, 0.0f, 200.0f, 1.0f, NULL, true) & VOLCTL_VOL) { *sv = pct / 100.0f; share_remote_volume_changed(as, peer); }
                        }
                    }
                }
            }
        }
    }
    if (compact && !s_modal_hint && briks_clicked_r(ui, "vt", 20.0f)) clicked = true;
    if (g_tile_grid && !compact && !s_modal_hint && zoom_pct <= 100) {
        /* Grade: clique CURTO no bloco (não arrasto, não nos controles do bloco) abre esta transmissão em Foco; Enter com o ponteiro sobre o bloco também */
        const cui_vec2 mp = cui_mouse_pos(ui); const cui_rect vr = cui_get_rect(ui, cui_make_id(ui, "vt"));
        bool inside = vr.w > 0 && pt_in(vr, mp) && !briks_blocked() && !briks_covered(ui);
        const char *kids[] = { "volbar", "zrst", "pnote", "lab" };
        for (int k = 0; k < 4 && inside; k++) { cui_rect kr = cui_get_rect(ui, cui_make_id(ui, kids[k])); if (kr.w > 0 && pt_in(kr, mp)) inside = false; }
        bool open = false;
        if (g_tg_press && inside) { g_tg_at = mp; g_tg_key = key; }
        if (g_tg_release && inside && g_tg_key == key) {   /* soltou no mesmo bloco, quase onde apertou */
            const float dx = mp.x - g_tg_at.x, dy = mp.y - g_tg_at.y;
            if (dx * dx + dy * dy <= 36.0f) open = true;
        }
        if (inside && cui_key_pressed(ui, CUI_KEY_ENTER)) open = true;
        if (open) { g_sfocus = key; g_call.layout = 1; g_tile_opens++; }
    }
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
    if (!strcmp(n, "share_vol_0")) return (int)(s->sh.remote[0].volume * 100.0f + 0.5f);   /* 1.4.4: volume de quem assiste a 1ª transmissão (%) */
    if (!strcmp(n, "share_zoomed")) return g_zoom_pct[0] > 100;
    if (!strcmp(n, "share_cx")) return (int)g_zoom[0].cx;   /* ponto da imagem no meio da moldura: muda ao arrastar */
    if (!strcmp(n, "share_cy")) return (int)g_zoom[0].cy;
    if (!strcmp(n, "share_mode")) return s->call_mode;
    if (!strcmp(n, "share_tiles")) return g_tiles;
    if (!strcmp(n, "prev_note_off")) return g_prev_note_off;   /* aviso "Prévia com qualidade reduzida" dispensado */
    if (!strcmp(n, "thumbs")) return share_thumb_probe();   /* avatares com thumb desfocada */
    if (!strcmp(n, "mini_on")) return g_mini_on;         /* miniatura da barra "Em chamada" pedida */
    if (!strcmp(n, "mini_n")) return g_mini_n;           /* transmissões que ela alterna */
    if (!strcmp(n, "mini_key")) return g_mkey;
    if (!strcmp(n, "mini_frames")) return g_mini_frames; /* quadros novos subidos (taxa baixa) */
    if (!strcmp(n, "share_mine")) return s->sh.active;
    if (!strcmp(n, "share_remote_on")) { int c = 0; for (int i = 0; i < BRIKS_MAX_PEERS; i++) c += s->sh.remote[i].on + s->sh.remote2[i].on; return c; }   /* transmissões dos outros no ar */
    if (!strcmp(n, "share_stalled")) { int c = 0; for (int i = 0; i < BRIKS_MAX_PEERS; i++) c += s->sh.remote[i].stalled + s->sh.remote2[i].stalled; return c; }
    if (!strcmp(n, "share_muted")) return s->sh.audio_muted;
    if (!strcmp(n, "share_qsel")) return s->sh.qsel;
    if (!strcmp(n, "share_q0_res")) return s->sh.qsel == 0 ? s->sh.res : s->sh.qs[0].res;
    if (!strcmp(n, "share_q1_res")) return s->sh.qsel == 1 ? s->sh.res : s->sh.qs[1].res;
    if (!strcmp(n, "share_q0_fps")) return s->sh.qsel == 0 ? s->sh.fps : s->sh.qs[0].fps;
    if (!strcmp(n, "share_q1_fps")) return s->sh.qsel == 1 ? s->sh.fps : s->sh.qs[1].fps;
    if (!strcmp(n, "share_q0_kbps")) return g_qk[0];
    if (!strcmp(n, "share_q1_kbps")) return g_qk[1];
    if (!strcmp(n, "share_active")) return s->sh.active;
    if (!strcmp(n, "share_amode")) return s->sh.audio_mode;   /* 0 da fonte, 1 apps, 2 sem audio */
    if (!strcmp(n, "share_audio")) return s->sh.audio_on && !s->sh.audio_muted;   /* "Com áudio" aceso */
    if (!strcmp(n, "stage_toggles")) return g_stage_toggles;
    if (!strcmp(n, "tile_opens")) return g_tile_opens;
    if (!strcmp(n, "dlg_quality")) return s->sh.quality_open ? 1 : 0;
    if (!strcmp(n, "dlg_res")) return s->sh.res;
    if (!strcmp(n, "dlg_fps")) return s->sh.fps;
    if (!strcmp(n, "dlg_open")) return s->sh.dialog_open ? 1 : 0;
    if (!strcmp(n, "eye0")) return s->sh.vknown[0] ? s->sh.viewers[0] : -1;   /* -1 = sem dado (oculto) */
    if (!strcmp(n, "eye1")) return s->sh.vknown[1] ? s->sh.viewers[1] : -1;
    if (!strncmp(n, "paud", 4) && n[4] >= '0' && n[4] <= '9' && (n[5] == '0' || n[5] == '1') && !n[6]) { int c = -1; return share_peer_eye(s, n[4] - '0', n[5] - '0', &c, NULL, 0) ? c : -1; }   /* paudPS: olhinho que o participante P divulgou para a tela S (-1 = oculto) */
    if (!strcmp(n, "share_vol0")) return (int)(s->sh.remote[0].volume * 100.0f + 0.5f);
    if (!strcmp(n, "stage_layout")) return g_call.layout;          /* blocos de vídeo no palco */
    if (!strcmp(n, "share_focus")) return g_sfocus;          /* chave da transmissão em destaque no Foco (-1 = a primeira) */
    if (!strcmp(n, "share_zoom2")) return g_zoom_pct[FV2(0)];   /* % da 2ª tela do 1º participante */
    return -9999;
}

/* Um bloco do palco, identificado pela mesma chave de g_fv/g_zoom: i = participante, FV2(i) = a 2ª tela dele,
 * BRIKS_MAX_PEERS = minha 1ª tela, FV2(BRIKS_MAX_PEERS) = minha 2ª. Devolve true se a miniatura foi clicada. */
static bool emit_tile(cui_ctx *ui, app_state_t *s, int key, bool compact) {
    share_state_t *q = &s->sh;
    char path[300], title[200], chip[96], r[24];
    g_eye = -1; g_vt_s = s;
    if (key == BRIKS_MAX_PEERS || key == FV2(BRIKS_MAX_PEERS)) {
        const int sl = key == BRIKS_MAX_PEERS ? 0 : 1;
        if (q->vknown[sl]) { g_eye = q->viewers[sl]; snprintf(g_eye_tip, sizeof g_eye_tip, "%s", g_eye ? q->vnames[sl] : "Ninguém assistindo agora"); }
    } else {   /* transmissão de outra pessoa: o olhinho vem do que ela divulgou à sala (sem divulgação = oculto) */
        const int pi = key >= FV_N ? key - FV_N : key; int cnt = 0;
        if (share_peer_eye(s, pi, key >= FV_N ? 1 : 0, &cnt, g_eye_tip, sizeof g_eye_tip)) g_eye = cnt;
    }
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
        bool has2 = q->remote2[i].path[0] && q->remote2[i].watching && !q->remote2[i].stalled && fv_update(ui, s, v, q->remote2[i].path);
        if (q->remote2[i].label[0]) snprintf(title, sizeof title, "%s · %s", nm, q->remote2[i].label); else snprintf(title, sizeof title, "%s · 2ª tela", nm);
        fmt_rate(q->remote2[i].kbps, r, sizeof r);
        snprintf(chip, sizeof chip, "%s · %d FPS · %s", res_name(q->remote2[i].h), q->remote2[i].fps, r);
        return video_tile(ui, key, has2 ? v->tex : 0, v->w, v->h, title, chip, q->remote2[i].stalled ? "Sem sinal da transmissão…" : "Carregando a transmissão…", NULL, &g_zoom[key], compact);
    }
    bool has = q->remote[i].path[0] && q->remote[i].watching && !q->remote[i].stalled && fv_update(ui, s, &g_fv[i], q->remote[i].path);
    if (q->remote[i].label[0]) snprintf(title, sizeof title, "%s · %s", nm, q->remote[i].label); else snprintf(title, sizeof title, "%s", nm);
    fmt_rate(q->remote[i].kbps, r, sizeof r);
    snprintf(chip, sizeof chip, "%s · %d FPS · %s%s", res_name(q->remote[i].h), q->remote[i].fps, r, q->remote[i].audio ? " · com áudio" : "");
    if (q->remote[i].poor) snprintf(chip, sizeof chip, "Conexão instável: a imagem pode ficar parada");
    if (!q->remote[i].vol_set) { q->remote[i].vol_set = true; q->remote[i].volume = 1.0f; }
    float before = q->remote[i].volume;
    bool c = video_tile(ui, i, has ? g_fv[i].tex : 0, g_fv[i].w, g_fv[i].h, title, chip, q->remote[i].stalled ? "Sem sinal da transmissão…" : q->remote[i].poor ? "Sua conexão não está dando conta desta transmissão" : "Carregando a transmissão…", q->remote[i].audio ? &q->remote[i].volume : NULL, &g_zoom[i], compact);
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
static bool stage_inner(cui_ctx *ui, app_state_t *s, float W, float H, int layout);
static bool pt_in(cui_rect r, cui_vec2 p) { return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h; }
/* Clique CURTO em área vazia do palco (fora de qualquer transmissão e de diálogos) alterna grade <-> foco; arrastar não alterna;
 * clicar dentro de uma transmissão continua como antes (miniatura traz para o destaque, zoom, etc.). O foco volta a quem estava fixado. */
bool ui_share_stage(cui_ctx *ui, app_state_t *s, float W, float H, int layout) {
    static bool pressed_empty; static cui_vec2 press_at; static bool was_down;
    bool shown;
    cui_box(ui, .id = "sstage", .width = CUI_FILL, .height = CUI_FILL) { shown = stage_inner(ui, s, W, H, layout); }
    if (!shown) return false;
    const cui_rect sr = cui_get_rect(ui, cui_make_id(ui, "sstage")); const cui_vec2 mp = cui_mouse_pos(ui); const bool down = cui_mouse_down(ui, CUI_MOUSE_LEFT);
    bool here_empty = !s_modal_hint && pt_in(sr, mp);
    for (int i = 0; i < g_tiles && i < 16 && here_empty; i++) if (pt_in(g_trect[i], mp)) here_empty = false;
    /* bordas do botão calculadas em stage_inner (uma vez por quadro): alterna na SOLTURA, se apertou e soltou em área vazia quase no mesmo ponto */
    if (g_tg_press) { press_at = mp; pressed_empty = here_empty; }
    if (g_tg_release) {
        const float dx = mp.x - press_at.x, dy = mp.y - press_at.y;
        if (pressed_empty && here_empty && dx * dx + dy * dy <= 36.0f) { g_call.layout = g_call.layout == 1 ? 0 : 1; g_stage_toggles++; }
        pressed_empty = false;
    }
    was_down = down;
    return true;
}
static bool stage_inner(cui_ctx *ui, app_state_t *s, float W, float H, int layout) {
    share_state_t *q = &s->sh;
    int keys[2 * FV_N], n = 0;
    for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (s->rtc_peers[i].id[0] && q->remote[i].on) { keys[n++] = i; if (q->remote2[i].on) keys[n++] = FV2(i); }
    if (q->active) keys[n++] = BRIKS_MAX_PEERS;
    if (q->second.active) keys[n++] = FV2(BRIKS_MAX_PEERS);
    g_tiles = 0; g_tile_grid = false;
    {   /* bordas do botão, calculadas UMA vez por quadro (não por bloco); cui_mouse_clicked sem botão apertado = clique de um quadro só (roteiros) */
        const bool dn = cui_mouse_down(ui, CUI_MOUSE_LEFT), ck = cui_mouse_clicked(ui, CUI_MOUSE_LEFT);
        if (g_tg_release) g_tg_key = -1;
        g_tg_press = (dn && !g_tg_down) || (ck && !dn); g_tg_release = (!dn && g_tg_down) || (ck && !dn); g_tg_down = dn;
    }
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
    g_tile_grid = n > 1 && layout == 0;
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
