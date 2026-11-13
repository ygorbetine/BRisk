/* Experimento "tijolo" (branch experimento/cui-tijolo): alusão a peças de montar nos painéis do Briks.
 * Tudo aqui é desenho 2D barato (retângulos e círculos do CUI); nada de textura. Cada função tem uma referência em docs/plans/tijolo.md. */
#include "briks_ui.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

bool brick_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("BRIKS_BRICKS"); v = !(e && e[0] == '0'); } return v == 1; }

static cui_color shade(cui_color c, float f) {   /* f < 1 escurece, f > 1 clareia */
    int r = (int)(c.r * f), g = (int)(c.g * f), b = (int)(c.b * f);
    cui_color o = c; o.r = (unsigned char)(r > 255 ? 255 : r); o.g = (unsigned char)(g > 255 ? 255 : g); o.b = (unsigned char)(b > 255 ? 255 : b); return o;
}

/* um pino (stud): corpo escuro + topo claro, como visto de cima e de lado */
static void stud(cui_ctx *ui, float cx, float cy, float rad, cui_color base) {
    cui_draw_rect(ui, (cui_rect){ cx - rad, cy - rad * 0.55f, rad * 2, rad * 1.1f }, CUI_CORNERS(rad * 0.45f), shade(base, 0.62f));
    cui_draw_rect(ui, (cui_rect){ cx - rad * 0.8f, cy - rad * 0.85f, rad * 1.6f, rad * 0.9f }, CUI_CORNERS(rad * 0.45f), shade(base, 1.18f));
}

/* um tijolo com `n` pinos em cima (n = 0: sem pinos) */
static void brick(cui_ctx *ui, cui_rect r, cui_color base, int n) {
    cui_draw_rect_border(ui, r, CUI_CORNERS(3), base, shade(base, 0.6f), 1.0f);
    cui_draw_rect(ui, (cui_rect){ r.x + 1.5f, r.y + 1.5f, r.w - 3.0f, 2.0f }, CUI_CORNERS(1), shade(base, 1.22f));   /* brilho da face de cima */
    for (int i = 0; i < n; i++) stud(ui, r.x + r.w * (i + 0.5f) / (float)n, r.y - 1.0f, fminf(r.w / (float)n * 0.28f, 4.5f), base);
}

/* ---- 1. fileira de pinos no topo de um painel ---- */
static void paint_row(cui_ctx *ui, cui_rect r, void *user) {
    (void)user;
    const float step = 26.0f; const int n = (int)(r.w / step); const float x0 = r.x + (r.w - (float)n * step) * 0.5f;
    for (int i = 0; i < n; i++) stud(ui, x0 + ((float)i + 0.5f) * step, r.y + r.h - 4.0f, 5.0f, B_C2);
}
void brick_studs_row(cui_ctx *ui) { cui_canvas(ui, "bk-row", CUI_FILL, CUI_PX(12), paint_row, NULL, 0); }

/* ---- 4. barra de progresso em tijolos: a parede sobe com a fração; o próximo tijolo cai e encaixa ---- */
typedef struct { float frac; bool indef; } prog_t;
static prog_t g_prog[4];
static void paint_prog(cui_ctx *ui, cui_rect r, void *user) {
    const prog_t *p = (const prog_t *)user;
    const int cols = (int)fmaxf(6.0f, floorf(r.w / 36.0f)); const float bw = r.w / (float)cols, bh = 15.0f, top = r.y + 8.0f;
    const double t = cui_time(ui);
    const int rows = 2; const int total = cols * rows;
    const float f = p->indef ? fmodf((float)t * 0.35f, 1.0f) : p->frac;
    const int done = (int)(f * (float)total);
    for (int k = 0; k < total; k++) {
        const int row = k / cols, col = k % cols;   /* a 1ª fileira enche primeiro; a 2ª fica deslocada meio tijolo (empilhamento real) */
        const float off = row ? bw * 0.5f : 0.0f;
        cui_rect b = { r.x + (float)col * bw + off + 1.0f, top + (float)(rows - 1 - row) * (bh + 1.5f), bw - 2.0f, bh };
        if (row && col == cols - 1) continue;
        if (k < done) brick(ui, b, row ? B_C3 : B_C2, 1);
        else if (k == done && !p->indef) {   /* o próximo cai: 0.45 s de queda por tijolo */
            const float ph = fmodf((float)t * 1.6f, 1.0f), y = ph < 0.6f ? (ph / 0.6f - 1.0f) * 18.0f : 0.0f;
            b.y += y; cui_color c = row ? B_C3 : B_C2; c.a = 210; brick(ui, b, c, 1);
        } else {
            cui_draw_rect_border(ui, b, CUI_CORNERS(3), CUI_RGBA(0xffffff0a), CUI_RGBA(0xffffff22), 0.5f);
        }
    }
}
void brick_progress(cui_ctx *ui, int idx, float frac, bool indef) {
    idx &= 3; g_prog[idx].frac = frac; g_prog[idx].indef = indef;
    cui_canvas(ui, idx ? "bk-prog1" : "bk-prog0", CUI_FILL, CUI_PX(44), paint_prog, &g_prog[idx], 0);
    cui_request_wakeup(ui, 0.04f);
}

/* ---- 3. anel de pinos em volta do avatar que fala (acendem com o volume) ---- */
void brick_ring(cui_ctx *ui, cui_vec2 c, float radius, float k, float lvl) {
    const int n = 14; const int lit = (int)ceilf(lvl * (float)n * 0.9f) < 2 ? 2 : (int)ceilf(lvl * (float)n * 0.9f);
    for (int i = 0; i < n; i++) {
        const float a = 6.2831853f * (float)i / (float)n - 1.5707963f;
        const cui_color base = i < lit ? CUI_RGB(0x3fae6f) : CUI_RGB(0x3a2a55);
        stud(ui, c.x + cosf(a) * radius, c.y + sinf(a) * radius, fmaxf(3.0f, 4.6f * k), base);
    }
}

/* ---- 6. torre de tijolos ao lado do nome do grupo: uma peça por canal; peça amarela = mensagens novas ---- */
typedef struct { int n; bool unread; } tower_t;
static tower_t g_tw[32];
static void paint_tower(cui_ctx *ui, cui_rect r, void *user) {
    const tower_t *t = (const tower_t *)user; const int n = t->n > 4 ? 4 : t->n < 1 ? 1 : t->n;
    const float bw = 22.0f, bh = 6.5f; const float x = r.x + r.w - bw - 2.0f; float y = r.y + r.h - 2.0f - bh;
    for (int i = 0; i < n; i++) { brick(ui, (cui_rect){ x + (i & 1 ? 3.0f : 0.0f), y, bw, bh }, B_C2, i == n - 1 && !t->unread ? 2 : 0); y -= bh + 0.5f; }
    if (t->unread) brick(ui, (cui_rect){ x + (n & 1 ? 3.0f : 0.0f), y, bw, bh }, CUI_RGB(0xe8c46a), 2);
}
void brick_tower(cui_ctx *ui, int idx, int channels, bool unread) {
    idx &= 31; g_tw[idx].n = channels; g_tw[idx].unread = unread;
    cui_canvas(ui, "bk-tower", CUI_PX(30), CUI_PX(30), paint_tower, &g_tw[idx], 0);
}

/* ---- 8. estado vazio: tijolos soltos esperando montagem ---- */
static void paint_loose(cui_ctx *ui, cui_rect r, void *user) {
    (void)user; const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    brick(ui, (cui_rect){ cx - 34, cy + 2, 30, 17 }, B_C2, 2);
    brick(ui, (cui_rect){ cx + 2, cy + 6, 30, 17 }, CUI_RGB(0x2f7d9a), 2);
    brick(ui, (cui_rect){ cx - 14, cy - 18, 22, 17 }, B_C3, 1);
}
void brick_loose(cui_ctx *ui) { cui_canvas(ui, "bk-loose", CUI_PX(80), CUI_PX(52), paint_loose, NULL, 0); }

/* ---- 5. entrada na chamada: pinos caem no topo do bloco (0.7 s) ---- */
typedef struct { double t0; } join_t;
static join_t g_join[16];
static double g_last_seen[16];
static void paint_join(cui_ctx *ui, cui_rect r, void *user) {
    const join_t *j = (const join_t *)user; const float ph = (float)(cui_time(ui) - j->t0) / 0.7f; if (ph < 0 || ph > 1) return;
    const int n = (int)fmaxf(3.0f, floorf(r.w / 44.0f)); const float step = r.w / (float)n;
    for (int i = 0; i < n; i++) {
        const float p = fminf(1.0f, fmaxf(0.0f, ph * 1.4f - (float)i * 0.08f)), y = p < 0.7f ? (p / 0.7f - 1.0f) * 40.0f : -sinf((p - 0.7f) / 0.3f * 3.1416f) * 3.0f;
        cui_color c = B_C3; c.a = (unsigned char)(255.0f * (1.0f - fmaxf(0.0f, ph - 0.8f) * 5.0f));
        stud(ui, r.x + step * ((float)i + 0.5f), r.y + 7.0f + y, 5.5f, c);
    }
}
void brick_join(cui_ctx *ui, int slot) {   /* chamar a cada quadro em que o bloco aparece: detecta a entrada e toca a animação */
    if (slot < 0 || slot > 15) return;
    const double now = cui_time(ui);
    if (now - g_last_seen[slot] > 0.6) g_join[slot].t0 = now;   /* sumiu por um tempo e voltou = entrou agora */
    g_last_seen[slot] = now;
    if (now - g_join[slot].t0 < 0.8) { cui_canvas(ui, "bk-join", CUI_FILL, CUI_FILL, paint_join, &g_join[slot], 0); cui_request_wakeup(ui, 0.03f); }
}

/* ---- 7. mosaico da thumb da transmissão: cada célula é uma peça 1x1 com a cor média do pedaço da tela ---- */
typedef struct { const unsigned char *cells; int cw, ch, radius; } mos_t;
static mos_t g_mos[BRIKS_MAX_PEERS];
static bool inside_round(float x, float y, cui_rect r, float rad) {   /* o ponto cai dentro do retângulo arredondado? */
    if (x < r.x || y < r.y || x > r.x + r.w || y > r.y + r.h) return false;
    const float cx = x < r.x + rad ? r.x + rad : x > r.x + r.w - rad ? r.x + r.w - rad : x, cy = y < r.y + rad ? r.y + rad : y > r.y + r.h - rad ? r.y + r.h - rad : y;
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= rad * rad;
}
static void paint_mos(cui_ctx *ui, cui_rect r, void *user) {
    const mos_t *m = (const mos_t *)user; if (!m->cells) return;
    const float cw = r.w / (float)m->cw, chh = r.h / (float)m->ch, g = 1.0f;
    for (int y = 0; y < m->ch; y++) for (int x = 0; x < m->cw; x++) {
        cui_rect c = { r.x + (float)x * cw + g * 0.5f, r.y + (float)y * chh + g * 0.5f, cw - g, chh - g };
        if (!inside_round(c.x, c.y, r, (float)m->radius) || !inside_round(c.x + c.w, c.y, r, (float)m->radius) || !inside_round(c.x, c.y + c.h, r, (float)m->radius) || !inside_round(c.x + c.w, c.y + c.h, r, (float)m->radius)) continue;   /* nada vaza pelos cantos redondos */
        const unsigned char *p = m->cells + ((size_t)y * (size_t)m->cw + (size_t)x) * 4;
        cui_draw_rect(ui, c, CUI_CORNERS(2), CUI_RGBA(((unsigned)(p[0] * 0.72f) << 24) | ((unsigned)(p[1] * 0.72f) << 16) | ((unsigned)(p[2] * 0.72f) << 8) | 0xff));
    }
}
void brick_mosaic(cui_ctx *ui, int slot, const unsigned char *cells, int cw, int ch, float w, float h, int radius) {
    if (slot < 0 || slot >= BRIKS_MAX_PEERS) return;
    g_mos[slot] = (mos_t){ cells, cw, ch, radius };
    cui_canvas(ui, "bk-mos", CUI_PX(w), CUI_PX(h), paint_mos, &g_mos[slot], 0);
}
