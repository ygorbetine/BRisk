/* Lógica pura do pacote "Janela e sistema" (sem Windows, sem CUI): geometria da janela salva, regras de aviso, contador de não lidas,
 * sons sintetizados (WAV em memória), ícone de contador (pixels) e validação de links. Fica em cabeçalho para o teste unitário
 * (tests/sys_test.c) usar o MESMO código do app (winstate.c, tray.c, notify.c). */
#ifndef BRIKS_SYSMODEL_H
#define BRIKS_SYSMODEL_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* ---------- geometria: a janela salva sempre volta inteira dentro de um monitor ligado ---------- */
typedef struct { int l, t, r, b; } wf_rect;
static inline int wf_w(wf_rect a) { return a.r - a.l; }
static inline int wf_h(wf_rect a) { return a.b - a.t; }
static inline long long wf_inter(wf_rect a, wf_rect b) {
    int l = a.l > b.l ? a.l : b.l, t = a.t > b.t ? a.t : b.t, r = a.r < b.r ? a.r : b.r, bo = a.b < b.b ? a.b : b.b;
    return (r > l && bo > t) ? (long long)(r - l) * (bo - t) : 0;
}
/* work[0..n-1]: áreas úteis dos monitores ligados (work[0] = principal). saved: retângulo externo da janela, em coordenadas de tela.
   Devolve o índice do monitor usado, ou -1 se o salvo é inválido (usa o padrão). *out fica sempre dentro da área útil desse monitor:
   monitor desligado/fora da tela = centraliza no principal; parcialmente fora = empurra para dentro; maior que o monitor = encolhe. */
static inline int wf_fit(const wf_rect *work, int n, wf_rect saved, int minw, int minh, wf_rect *out, bool *moved) {
    if (n < 1 || wf_w(saved) < 100 || wf_h(saved) < 100 || wf_w(saved) > 20000 || wf_h(saved) > 20000) return -1;
    int best = 0; long long ba = -1;
    for (int i = 0; i < n; i++) { long long a = wf_inter(saved, work[i]); if (a > ba) { ba = a; best = i; } }
    wf_rect W = work[best]; bool relocated = false;
    if (ba < 160LL * 80) { best = 0; W = work[0]; relocated = true; }   /* quase nada visível (monitor sumiu): principal */
    int w = wf_w(saved), h = wf_h(saved), ww = wf_w(W), wh = wf_h(W);
    if (w > ww) w = ww; if (h > wh) h = wh;
    if (w < minw && minw <= ww) w = minw; if (h < minh && minh <= wh) h = minh;
    int x = saved.l, y = saved.t;
    if (relocated) { x = W.l + (ww - w) / 2; y = W.t + (wh - h) / 2; }
    if (x + w > W.r) x = W.r - w; if (x < W.l) x = W.l;
    if (y + h > W.b) y = W.b - h; if (y < W.t) y = W.t;
    if (moved) *moved = relocated || x != saved.l || y != saved.t || w != wf_w(saved) || h != wf_h(saved);
    out->l = x; out->t = y; out->r = x + w; out->b = y + h;
    return best;
}

/* ---------- avisos: quando um aviso do sistema pode aparecer ---------- */
typedef enum { NT_MESSAGE, NT_FRIEND_ONLINE, NT_FRIEND_CALL, NT_STREAM, NT_REMINDER, NT_INFO } notify_kind_t;
typedef struct {
    bool nt_msg, nt_online, nt_stream;   /* interruptores de Configurações > Notificações */
    bool friend_alerts_off;              /* "avisos de amigo" desligados no perfil (notify_off) */
    bool focused;                        /* a janela do Briks é a que está em primeiro plano */
    bool in_call;                        /* sala de voz aberta: nada é avisado (release 1.2.0) */
    bool grace;                          /* logo depois de abrir (primeiros segundos): nada é avisado */
    bool same_chat;                      /* mensagem da conversa que está aberta na tela */
} sm_ctx;
static inline bool sm_toast_allowed(notify_kind_t k, const sm_ctx *c) {
    if (c->in_call || c->grace) return false;
    switch (k) {
    case NT_MESSAGE: return c->nt_msg && !(c->focused && c->same_chat);   /* conversa aberta e em foco: já está vendo */
    case NT_FRIEND_ONLINE: case NT_FRIEND_CALL: return c->nt_online && !c->friend_alerts_off && !c->focused;
    case NT_STREAM: return c->nt_stream && !c->friend_alerts_off && !c->focused;
    case NT_REMINDER: return true;
    default: return true;
    }
}

/* ---------- não lidas por conversa ---------- */
#define SM_UNREAD_MAX 64
typedef struct { char key[64]; int n; } sm_unread_e;
typedef struct { sm_unread_e e[SM_UNREAD_MAX]; int count; } sm_unread;
static inline void sm_unread_add(sm_unread *u, const char *key) {
    if (!key || !key[0]) return;
    for (int i = 0; i < u->count; i++) if (!strcmp(u->e[i].key, key)) { u->e[i].n++; return; }
    if (u->count >= SM_UNREAD_MAX) { u->e[0].n++; return; }   /* cheio: soma na primeira (o total continua certo) */
    snprintf(u->e[u->count].key, sizeof u->e[0].key, "%s", key); u->e[u->count].n = 1; u->count++;
}
static inline int sm_unread_clear(sm_unread *u, const char *key) {   /* devolve quantas havia */
    for (int i = 0; i < u->count; i++) if (!strcmp(u->e[i].key, key)) { int n = u->e[i].n; u->e[i] = u->e[--u->count]; return n; }
    return 0;
}
static inline int sm_unread_get(const sm_unread *u, const char *key) { for (int i = 0; i < u->count; i++) if (!strcmp(u->e[i].key, key)) return u->e[i].n; return 0; }
static inline int sm_unread_total(const sm_unread *u) { int t = 0; for (int i = 0; i < u->count; i++) t += u->e[i].n; return t; }

/* ---------- texto curto (UTF-8) para o balão ---------- */
static inline void sm_trunc(char *dst, size_t cap, const char *src, int maxchars) {
    size_t o = 0; int ch = 0; const unsigned char *p = (const unsigned char *)(src ? src : "");
    while (*p && ch < maxchars && o + 4 < cap) {
        size_t len = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : (*p >> 3) == 30 ? 4 : 1;
        for (size_t k = 0; k < len && p[k]; k++) dst[o++] = (char)p[k];
        p += len; ch++;
        if (*p == 0) break;
    }
    if (*p && o + 4 < cap) { dst[o++] = '\xe2'; dst[o++] = '\x80'; dst[o++] = '\xa6'; }   /* … */
    dst[o] = 0;
}

/* ---------- links: só http(s), como o openExternal do Electron ---------- */
static inline bool sm_url_ok(const char *u) {
    if (!u) return false;
    size_t n = strlen(u); if (n < 9 || n > 2000) return false;
    if (strncmp(u, "http://", 7) && strncmp(u, "https://", 8)) return false;
    for (size_t i = 0; i < n; i++) if ((unsigned char)u[i] <= 0x20 || u[i] == '"' || u[i] == '<' || u[i] == '>') return false;
    const char *host = u + (u[4] == 's' ? 8 : 7);
    return host[0] && host[0] != '/' && host[0] != '?' && host[0] != '#';
}

/* ---------- iniciar com o Windows: comando gravado na chave Run ---------- */
static inline void sm_autostart_cmd(const char *exe, char *out, size_t cap) { snprintf(out, cap, "\"%s\" --minimizado", exe); }

/* ---------- sons: WAV mono 16 bits sintetizado (nada de arquivo extra) ---------- */
typedef enum { SND_NOTIFY, SND_JOIN, SND_LEAVE, SND_PEER_JOIN, SND_PEER_LEAVE, SND_COUNT } sound_kind_t;
#define SM_WAV_RATE 22050
#define SM_WAV_CAP 26000
static inline size_t sm_wav(sound_kind_t kind, int vol, unsigned char *buf, size_t cap) {
    static const struct { int f, ms; } seq[SND_COUNT][3] = {
        { { 880, 110 }, { 1175, 160 }, { 0, 0 } },      /* aviso: dois toques agudos */
        { { 523, 90 }, { 784, 150 }, { 0, 0 } },        /* eu entrei: sobe */
        { { 784, 90 }, { 523, 150 }, { 0, 0 } },        /* eu saí: desce */
        { { 659, 90 }, { 0, 0 }, { 0, 0 } },            /* alguém entrou: um toque */
        { { 440, 100 }, { 0, 0 }, { 0, 0 } },           /* alguém saiu: um toque grave */
    };
    if (kind < 0 || kind >= SND_COUNT || cap < 64) return 0;
    if (vol < 0) vol = 0; if (vol > 100) vol = 100;
    size_t ns = 0; short *pcm = (short *)(buf + 44); size_t maxs = (cap - 44) / 2;
    for (int k = 0; k < 3 && seq[kind][k].f; k++) {
        size_t len = (size_t)seq[kind][k].ms * SM_WAV_RATE / 1000;
        for (size_t i = 0; i < len && ns < maxs; i++, ns++) {
            double t = (double)i / SM_WAV_RATE, env = (i < 110 ? (double)i / 110.0 : 1.0) * exp(-4.0 * (double)i / (double)len);
            double v = sin(6.283185307179586 * seq[kind][k].f * t) * 0.8 + sin(6.283185307179586 * seq[kind][k].f * 2 * t) * 0.15;
            pcm[ns] = (short)(v * env * 0.6 * (vol / 100.0) * 32767.0);
        }
    }
    size_t db = ns * 2; uint32_t rate = SM_WAV_RATE, brate = SM_WAV_RATE * 2, riff = (uint32_t)(36 + db), dsz = (uint32_t)db, fmt = 16;
    uint16_t pcmfmt = 1, ch = 1, align = 2, bits = 16;
    memcpy(buf, "RIFF", 4); memcpy(buf + 4, &riff, 4); memcpy(buf + 8, "WAVEfmt ", 8); memcpy(buf + 16, &fmt, 4);
    memcpy(buf + 20, &pcmfmt, 2); memcpy(buf + 22, &ch, 2); memcpy(buf + 24, &rate, 4); memcpy(buf + 28, &brate, 4);
    memcpy(buf + 32, &align, 2); memcpy(buf + 34, &bits, 2); memcpy(buf + 36, "data", 4); memcpy(buf + 40, &dsz, 4);
    return 44 + db;
}

/* ---------- contador: círculo de acento com número (BGRA, sem pré-multiplicar), 1..9 ou "9+" ---------- */
static inline void sm_badge_bgra(int n, uint32_t rgb, int size, uint32_t *px) {
    static const char *G[11] = { "111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001", "111100111001111",
                                 "111100111101111", "111001001001001", "111101111101111", "111101111001111", "000010111010000" };   /* 0..9, '+' (3x5) */
    char txt[3] = { 0 };
    if (n > 9) { txt[0] = '9'; txt[1] = '+'; } else txt[0] = (char)('0' + (n < 0 ? 0 : n));
    int sc = size >= 28 ? 3 : size >= 18 ? 2 : 1, nch = (int)strlen(txt), tw = nch * 3 * sc + (nch - 1) * sc, th = 5 * sc;
    int ox = (size - tw) / 2, oy = (size - th) / 2;
    float rad = size / 2.0f - 0.5f, c = (size - 1) / 2.0f;
    for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) {
        float d = sqrtf((x - c) * (x - c) + (y - c) * (y - c)), cov = rad + 0.5f - d; cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
        uint32_t col = rgb;
        int lx = x - ox, ly = y - oy;
        if (lx >= 0 && ly >= 0 && lx < tw && ly < th) {
            int gi = lx / (3 * sc + sc), gx = (lx % (4 * sc)) / sc, gy = ly / sc;
            if (gi < nch && gx < 3) { int di = txt[gi] == '+' ? 10 : txt[gi] - '0'; if (G[di][gy * 3 + gx] == '1') col = 0xffffff; }
        }
        px[y * size + x] = ((uint32_t)(cov * 255.0f) << 24) | col;
    }
}

#endif
