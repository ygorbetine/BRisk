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
    (void)user;   /* uma placa de montar: faixa baixa na cor do tema com os pinos em cima, ocupando a largura toda do conteúdo */
    const float step = 26.0f; const int n = (int)(r.w / step); const float x0 = r.x + (r.w - (float)n * step) * 0.5f;
    cui_color plate = B_C2; plate.a = 150;
    cui_draw_rect_border(ui, (cui_rect){ r.x, r.y + r.h - 7.0f, r.w, 7.0f }, CUI_CORNERS(3), plate, B_A(shade(B_C2, 1.3f), 170), 0.75f);
    for (int i = 0; i < n; i++) stud(ui, x0 + ((float)i + 0.5f) * step, r.y + r.h - 8.5f, 5.0f, B_C2);
}
void brick_studs_row(cui_ctx *ui) { cui_canvas(ui, "bk-row", CUI_FILL, CUI_PX(18), paint_row, NULL, 0); }

/* ---- 4. barra de progresso em tijolos: a parede sobe com a fração; o próximo tijolo cai e encaixa ---- */
typedef struct { float frac; bool indef; } prog_t;
static prog_t g_prog[4];
static void paint_prog(cui_ctx *ui, cui_rect r, void *user) {
    const prog_t *p = (const prog_t *)user;
    const int cols = (int)fmaxf(4.0f, floorf(r.w / 72.0f)); const float bw = r.w / (float)cols, bh = 18.0f, top = r.y + 9.0f;   /* poucos tijolos e grandes: o download é rápido */
    const double t = cui_time(ui);
    const int rows = 2; const int total = cols * rows;
    const float f = p->indef ? fmodf((float)t * 0.35f, 1.0f) : p->frac;
    const int done = (int)(f * (float)total);
    for (int k = 0; k < total; k++) {
        const int row = k / cols, col = k % cols;   /* a 1ª fileira enche primeiro; a 2ª fica deslocada meio tijolo (empilhamento real) */
        const float off = row ? bw * 0.5f : 0.0f;
        cui_rect b = { r.x + (float)col * bw + off + 1.0f, top + (float)(rows - 1 - row) * (bh + 1.5f), bw - 2.0f, bh };
        if (row && col == cols - 1) continue;
        if (k < done) brick(ui, b, row ? B_C3 : B_C2, 2);
        else if (k == done && !p->indef) {   /* o próximo cai: 0.45 s de queda por tijolo */
            const float ph = f * (float)total - (float)done, y = (1.0f - ph) * -(ph < 0.5f ? 18.0f : 18.0f * (1.0f - ph) * 2.0f) * 0.0f + (ph < 0.6f ? (ph / 0.6f - 1.0f) * 18.0f : 0.0f);   /* a queda acompanha o progresso: cada tijolo desce enquanto o percentual o alcança */
            b.y += y; cui_color c = row ? B_C3 : B_C2; c.a = 210; brick(ui, b, c, 2);
        } else {
            cui_draw_rect_border(ui, b, CUI_CORNERS(3), CUI_RGBA(0xffffff0a), CUI_RGBA(0xffffff22), 0.5f);
        }
    }
}
void brick_progress(cui_ctx *ui, int idx, float frac, bool indef) {
    idx &= 3; g_prog[idx].frac = frac; g_prog[idx].indef = indef;
    cui_canvas(ui, idx ? "bk-prog1" : "bk-prog0", CUI_FILL, CUI_PX(50), paint_prog, &g_prog[idx], 0);
    cui_request_wakeup(ui, 0.04f);
}

/* ---- 6. torre de tijolos ao lado do nome do grupo: uma peça por canal; peça amarela = mensagens novas ---- */
typedef struct { int n; bool unread; } tower_t;
static tower_t g_tw[32];
static void paint_tower(cui_ctx *ui, cui_rect r, void *user) {
    const tower_t *t = (const tower_t *)user; const int n = t->n > 4 ? 4 : t->n < 1 ? 1 : t->n;
    const float bw = 22.0f, bh = 5.0f, gap = 0.6f; const float x = r.x + r.w - bw - 2.0f; float y = r.y + r.h - 2.0f - bh;
    for (int i = 0; i < n; i++) { brick(ui, (cui_rect){ x + (i & 1 ? 3.0f : 0.0f), y, bw, bh }, B_C2, 0); y -= bh + gap; }
    if (t->unread) {   /* a peça nova no topo pulsa: mensagens não lidas */
        const float pulse = 0.6f + 0.4f * sinf((float)cui_time(ui) * 4.0f); cui_color am = CUI_RGB(0xe8c46a); am.a = (unsigned char)(255.0f * pulse);
        brick(ui, (cui_rect){ x + (n & 1 ? 3.0f : 0.0f), y, bw, bh }, am, 0); cui_request_wakeup(ui, 0.06f);
    }
}
void brick_tower(cui_ctx *ui, int idx, int channels, bool unread) {
    idx &= 31; g_tw[idx].n = channels; g_tw[idx].unread = unread;
    cui_canvas(ui, "bk-tower", CUI_PX(28), CUI_PX(30), paint_tower, &g_tw[idx], 0);
}

/* ---- 8. estado vazio: tijolos soltos esperando montagem ---- */
static void paint_loose(cui_ctx *ui, cui_rect r, void *user) {
    (void)user; const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    brick(ui, (cui_rect){ cx - 34, cy + 2, 30, 17 }, B_C2, 2);
    brick(ui, (cui_rect){ cx + 2, cy + 6, 30, 17 }, CUI_RGB(0x2f7d9a), 2);
    brick(ui, (cui_rect){ cx - 14, cy - 18, 22, 17 }, B_C3, 1);
}
void brick_loose(cui_ctx *ui) { cui_canvas(ui, "bk-loose", CUI_PX(80), CUI_PX(52), paint_loose, NULL, 0); }

/* ---- 7. mosaico da thumb da transmissão: cada célula é uma peça 1x1 com a cor média do pedaço da tela ---- */
typedef struct { const unsigned char *cells; int cw, ch, radius; } mos_t;
static mos_t g_mos[BRIKS_MAX_PEERS + 2];   /* +1: o tile 'Você' usa o slot BRIKS_MAX_PEERS+1 */
static bool inside_round(float x, float y, cui_rect r, float rad) {   /* o ponto cai dentro do retângulo arredondado? */
    if (x < r.x || y < r.y || x > r.x + r.w || y > r.y + r.h) return false;
    const float cx = x < r.x + rad ? r.x + rad : x > r.x + r.w - rad ? r.x + r.w - rad : x, cy = y < r.y + rad ? r.y + rad : y > r.y + r.h - rad ? r.y + r.h - rad : y;
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= rad * rad;
}
static unsigned hash2(unsigned a, unsigned b) { unsigned h = a * 73856093u ^ b * 19349663u; h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15; return h; }
static void paint_mos(cui_ctx *ui, cui_rect r, void *user) {
    /* Lego visto de cima: peças de 1x1 a 2x4 encaixadas sem lacunas, pinos redondos; a cor de cada peça é a média da versão pixelada (cells) da tela sob ela */
    const mos_t *m = (const mos_t *)user; if (!m->cells) return;
    float u = r.w / 34.0f; if (u < 14.0f) u = 14.0f; if (u > 30.0f) u = 30.0f;
    int cols = (int)ceilf(r.w / u), rows = (int)ceilf(r.h / u); if (cols > 96) cols = 96; if (rows > 64) rows = 64;
    static unsigned char used[64][96]; memset(used, 0, sizeof used);
    static const int SZ[][2] = { {2,1},{2,1},{1,1},{2,2},{2,2},{3,1},{4,1},{2,3},{1,2},{3,2},{4,2} }; const int NS = (int)(sizeof SZ / sizeof SZ[0]);
    for (int y = 0; y < rows; y++) for (int x = 0; x < cols; x++) {
        if (used[y][x]) continue;
        int w = 1, h = 1; const unsigned h0 = hash2((unsigned)x, (unsigned)y);
        for (int k = 0; k < NS; k++) {
            const int *s2 = SZ[(h0 + (unsigned)k) % (unsigned)NS]; bool ok = x + s2[0] <= cols && y + s2[1] <= rows;
            for (int j = 0; ok && j < s2[1]; j++) for (int i = 0; i < s2[0]; i++) if (used[y + j][x + i]) { ok = false; break; }
            if (ok) { w = s2[0]; h = s2[1]; break; }
        }
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) used[y + j][x + i] = 1;
        cui_rect c = { r.x + (float)x * u, r.y + (float)y * u, (float)w * u, (float)h * u };
        if (c.x + c.w > r.x + r.w) c.w = r.x + r.w - c.x;
        if (c.y + c.h > r.y + r.h) c.h = r.y + r.h - c.y;
        if (c.w < u * 0.6f || c.h < u * 0.6f) continue;
        if (!inside_round(c.x, c.y, r, (float)m->radius) || !inside_round(c.x + c.w, c.y, r, (float)m->radius) || !inside_round(c.x, c.y + c.h, r, (float)m->radius) || !inside_round(c.x + c.w, c.y + c.h, r, (float)m->radius)) continue;   /* nada vaza pelos cantos redondos */
        /* média das células cobertas pela peça */
        const int sx0 = (int)((c.x - r.x) / r.w * (float)m->cw), sx1 = (int)((c.x + c.w - r.x) / r.w * (float)m->cw), sy0 = (int)((c.y - r.y) / r.h * (float)m->ch), sy1 = (int)((c.y + c.h - r.y) / r.h * (float)m->ch);
        unsigned sum[3] = { 0, 0, 0 }, cnt = 0;
        for (int yy = sy0; yy <= (sy1 > sy0 ? sy1 - 1 : sy0); yy++) for (int xx = sx0; xx <= (sx1 > sx0 ? sx1 - 1 : sx0); xx++) {
            const int cx = xx < 0 ? 0 : xx >= m->cw ? m->cw - 1 : xx, cy = yy < 0 ? 0 : yy >= m->ch ? m->ch - 1 : yy; const unsigned char *p = m->cells + ((size_t)cy * (size_t)m->cw + (size_t)cx) * 4;
            sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2]; cnt++;
        }
        if (!cnt) cnt = 1;
        cui_color base; base.r = (unsigned char)fminf(255.0f, 24.0f + (float)(sum[0] / cnt) * 1.25f); base.g = (unsigned char)fminf(255.0f, 20.0f + (float)(sum[1] / cnt) * 1.2f); base.b = (unsigned char)fminf(255.0f, 30.0f + (float)(sum[2] / cnt) * 1.3f); base.a = 215;
        cui_color edge = shade(base, 0.55f); edge.a = 230;
        cui_draw_rect_border(ui, c, CUI_CORNERS(2), base, edge, 1.0f);
        const float sr = u * 0.31f;
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
            const float cx = c.x + ((float)i + 0.5f) * u, cy = c.y + ((float)j + 0.5f) * u; if (cx + sr > c.x + c.w || cy + sr > c.y + c.h) continue;
            cui_color sh = shade(base, 0.6f); sh.a = 200; cui_draw_rect(ui, (cui_rect){ cx - sr + 1.5f, cy - sr + 2.0f, sr * 2, sr * 2 }, CUI_CORNERS(sr), sh);
            cui_color top = shade(base, 1.1f); top.a = 235; cui_draw_rect_border(ui, (cui_rect){ cx - sr, cy - sr, sr * 2, sr * 2 }, CUI_CORNERS(sr), top, shade(base, 0.72f), 1.0f);
            cui_color hi = shade(base, 1.45f); hi.a = 170; cui_draw_rect(ui, (cui_rect){ cx - sr * 0.55f, cy - sr * 0.65f, sr * 0.7f, sr * 0.55f }, CUI_CORNERS(sr * 0.3f), hi);
        }
    }
}
void brick_mosaic(cui_ctx *ui, int slot, const unsigned char *cells, int cw, int ch, float w, float h, int radius) {
    if (slot < 0 || slot > BRIKS_MAX_PEERS + 1) return;
    g_mos[slot] = (mos_t){ cells, cw, ch, radius };
    cui_canvas(ui, "bk-mos", CUI_PX(w), CUI_PX(h), paint_mos, &g_mos[slot], 0);
}

/* ---- 2. encaixe entre painéis: peças de ligação cruzam o vão entre dois painéis vizinhos (o vão tem LAYOUT_GAP px); os painéis não mudam de forma ---- */
typedef struct { bool horiz; } link_t;   /* horiz = o vão é uma faixa horizontal (painéis um sobre o outro) */
static link_t g_link[64];
static void paint_link(cui_ctx *ui, cui_rect r, void *user) {
    const link_t *l = (const link_t *)user; const float len = l->horiz ? r.w : r.h, mid = l->horiz ? r.y + r.h * 0.5f : r.x + r.w * 0.5f;
    if (len < 110.0f) return;
    const int n = (int)((len - 80.0f) / 110.0f) + 1; const float step = (len - 80.0f) / (float)n;
    for (int i = 0; i < n; i++) {
        const float p = 40.0f + step * ((float)i + 0.5f);   /* longe dos cantos arredondados */
        const float cx = l->horiz ? r.x + p : mid, cy = l->horiz ? mid : r.y + p;
        const cui_color base = B_C2;
        cui_rect plate = l->horiz ? (cui_rect){ cx - 13.0f, cy - (r.h * 0.5f + 3.0f), 26.0f, r.h + 6.0f } : (cui_rect){ cx - (r.w * 0.5f + 3.0f), cy - 13.0f, r.w + 6.0f, 26.0f };
        cui_color f = base; f.a = 120;
        cui_draw_rect_border(ui, plate, CUI_CORNERS(4), f, B_A(shade(base, 1.3f), 150), 0.75f);
        stud(ui, cx - (l->horiz ? 0.0f : 0.0f), cy, 3.2f, base);
    }
}
void brick_link(cui_ctx *ui, int idx, float w, float h, bool horiz) {
    idx &= 63; g_link[idx].horiz = horiz;
    cui_canvas(ui, "bk-link", CUI_PX(w), CUI_PX(h), paint_link, &g_link[idx], 0);
}
