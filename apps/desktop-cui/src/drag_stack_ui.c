/* Desenho da "pilha levantada": três cartões de vidro (giro/escala/opacidade aplicados em glass.c) e a ARTE do cartão da frente,
 * uma por tipo de painel, com dados do estado real do app. Tudo é desenhado sem giro, dentro do cartão e recuado >= 12 px das bordas,
 * então nenhum retângulo encosta nos cantos curvos (raio 16). */
#include "briks_ui.h"
#include "layout.h"
#include "drag_stack.h"
#include "drag_stack_ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int demo_voice = -1, demo_servers = -1;
void drag_stack_set_demo(int voice_people, int servers) { demo_voice = voice_people; demo_servers = servers; }
static int art_override = -1;
void drag_stack_set_art(int kind) { art_override = kind; }   /* galeria de testes: força a arte de outro tipo de painel */

#define PAD 12.0f
#define WHITE(a) CUI_RGBA(0xffffff00 | (a))

static const cui_color AV[5] = { { 0x6d, 0x5c, 0xf6, 255 }, { 0xb0, 0x1f, 0xcf, 255 }, { 0x2f, 0x8f, 0x6b, 255 }, { 0x3b, 0x6e, 0xc9, 255 }, { 0xc0, 0x4f, 0x8a, 255 } };

static void ini(const char *name, char *out) {
    out[0] = name && name[0] ? (char)((name[0] >= 'a' && name[0] <= 'z') ? name[0] - 32 : name[0]) : '?'; out[1] = 0; out[2] = 0;
    const char *sp = name ? strchr(name, ' ') : NULL;
    if (sp && sp[1]) out[1] = (char)((sp[1] >= 'a' && sp[1] <= 'z') ? sp[1] - 32 : sp[1]);
}

/* texto cortado por quantidade de caracteres (largura ~ 0,54 do corpo) */
static void txt(cui_ctx *ui, cui_font f, float size, float x, float y, cui_color col, const char *s, float maxw) {
    char b[64]; int n = (int)(maxw / (size * 0.5f)); if (n < 1) n = 1; if (n > 60) n = 60;
    int len = (int)strlen(s);
    if (len > n) { snprintf(b, sizeof b, "%.*s...", n > 3 ? n - 3 : 1, s); } else snprintf(b, sizeof b, "%s", s);
    cui_draw_text(ui, f, size, (cui_vec2){ x, y }, col, b, -1);
}
static float tw(float size, const char *s) { return (float)strlen(s) * size * 0.54f; }

/* avatar redondo liso (sem relevo) com iniciais */
static void avatar(cui_ctx *ui, float cx, float cy, float r, const char *name, int hue, bool online) {
    cui_draw_circle(ui, (cui_vec2){ cx, cy }, r, AV[hue % 5]);
    char i2[3]; ini(name, i2);
    float sz = r * 0.95f; if (sz < 7) sz = 7;
    cui_draw_text(ui, CUI_FONT_MEDIUM, sz, (cui_vec2){ cx - tw(sz, i2) * 0.5f, cy - sz * 0.62f }, CUI_RGBA(0xffffffee), i2, -1);
    if (online) {
        cui_draw_circle(ui, (cui_vec2){ cx + r * 0.72f, cy + r * 0.72f }, r * 0.42f + 1.2f, CUI_RGBA(0x1a0a26ff));
        cui_draw_circle(ui, (cui_vec2){ cx + r * 0.72f, cy + r * 0.72f }, r * 0.42f, B_ONLINE);
    }
}

/* balão de vidro (sem relevo): sombra suave, fundo translúcido; quina reta no lado de quem fala */
#define BUB_SHADOW CUI_RGBA(0x00000055)
static void neu_bubble(cui_ctx *ui, cui_rect r, bool left_tail) {
    cui_corners c = left_tail ? (cui_corners){ 2, 8, 8, 8 } : (cui_corners){ 8, 8, 2, 8 };
    cui_draw_shadow(ui, r, c, 5.0f, 0, (cui_vec2){ 0, 2 }, BUB_SHADOW);
    cui_draw_rect(ui, r, c, CUI_RGBA(0x2a1448f0));
    cui_draw_rect(ui, (cui_rect){ r.x + 6, r.y + r.h * 0.5f - 1.5f, r.w - 12, 3 }, CUI_CORNERS(1.5f), WHITE(0x38));   /* "texto" */
}
static void sent_bubble(cui_ctx *ui, cui_rect r) {
    cui_corners c = { 8, 8, 2, 8 };
    cui_draw_shadow(ui, r, c, 5.0f, 0, (cui_vec2){ 0, 2 }, BUB_SHADOW);
    cui_draw_rect_gradient(ui, r, c, CUI_RGBA(0x9b6cf8ff), CUI_RGBA(0x6d5cf6ff));
    cui_draw_rect(ui, (cui_rect){ r.x + 6, r.y + r.h * 0.5f - 1.5f, r.w - 12, 3 }, CUI_CORNERS(1.5f), WHITE(0x70));
}

static void header_sz(cui_ctx *ui, cui_rect c, const char *icon, const char *title, float sz) {
    cui_draw_icon(ui, icon, (cui_rect){ c.x + PAD, c.y + 11, 16, 16 }, 15, CUI_RGBA(0xedf1f7ee));
    txt(ui, CUI_FONT_MEDIUM, sz, c.x + PAD + 22, c.y + 13, CUI_RGBA(0xedf1f7f2), title, c.w - PAD * 2 - 22);
}

static void header(cui_ctx *ui, cui_rect c, const char *icon, const char *title) { header_sz(ui, c, icon, title, 11.5f); }

static const char *friend_name(const app_state_t *s, int i) {
    static const char *demo[3] = { "Marina Costa", "Rafael Lima", "Bia Nogueira" };
    return (s && i < s->friend_count) ? s->friends[i].name : demo[i % 3];
}
static int people_in_call(const app_state_t *s) {
    (void)s;
    return demo_voice > 0 ? demo_voice : 1;
}
static int community_total(const app_state_t *s) { return demo_servers >= 0 ? demo_servers : (s ? s->community_count : 0); }

/* ---- uma arte por tipo de painel ---- */
static void art_chat(cui_ctx *ui, const app_state_t *s, cui_rect c, bool group) {
    float iw = c.w - PAD * 2, y0 = c.y + 40, x0 = c.x + PAD;
    const char *name;
    if (group && s && s->active_group >= 0 && s->active_group < s->group_count) name = s->groups[s->active_group].name;
    else if (group) name = "Noite de jogos";
    else name = (s && s->active_friend >= 0 && s->active_friend < s->friend_count) ? s->friends[s->active_friend].name : friend_name(s, 0);
    header(ui, c, group ? CUI_ICON_USERS : CUI_ICON_MESSAGE_SQUARE, name);
    float off = group ? 20 : 0;
    if (group) { avatar(ui, x0 + 7, y0 + 8, 7, friend_name(s, 0), 0, false); avatar(ui, x0 + 7, y0 + 29, 7, friend_name(s, 1), 2, false); }
    neu_bubble(ui, (cui_rect){ x0 + off, y0, iw * 0.58f, 16 }, true);
    neu_bubble(ui, (cui_rect){ x0 + off, y0 + 21, iw * 0.44f, 16 }, true);
    sent_bubble(ui, (cui_rect){ x0 + iw - iw * 0.52f, y0 + 42, iw * 0.52f, 16 });
}

static void art_nav(cui_ctx *ui, const app_state_t *s, cui_rect c) {
    header(ui, c, CUI_ICON_MENU, "Menu lateral");
    float x0 = c.x + PAD, iw = c.w - PAD * 2, y = c.y + 38;
    cui_draw_icon(ui, CUI_ICON_USERS, (cui_rect){ x0, y, 12, 12 }, 11, CUI_RGBA(0xa592b8ff));
    txt(ui, CUI_FONT_BODY, 10.5f, x0 + 18, y, CUI_RGBA(0xc9bbdcff), "Amigos", iw - 18);
    y += 16;
    cui_draw_icon(ui, CUI_ICON_HISTORY, (cui_rect){ x0, y, 12, 12 }, 11, CUI_RGBA(0xa592b8ff));
    txt(ui, CUI_FONT_BODY, 10.5f, x0 + 18, y, CUI_RGBA(0xc9bbdcff), "Histórico de chamadas", iw - 18);
    y += 19;
    const char *act = (s && s->active_friend >= 0 && s->active_friend < s->friend_count) ? s->friends[s->active_friend].name : friend_name(s, 0);
    cui_rect hl = { x0 - 3, y, iw + 6, 20 };
    cui_draw_rect_border(ui, hl, CUI_CORNERS(8), CUI_RGBA(0x8b5cf64a), CUI_RGBA(0xc4b0ff50), 1.0f);
    avatar(ui, x0 + 8, y + 10, 7, act, 3, false);
    txt(ui, CUI_FONT_MEDIUM, 10.5f, x0 + 21, y + 4, CUI_RGBA(0xffffffee), act, iw - 24);
    y += 25;
    const char *other = friend_name(s, 1);
    avatar(ui, x0 + 8, y + 7, 7, other, 2, true);
    txt(ui, CUI_FONT_BODY, 10.5f, x0 + 21, y + 1, CUI_RGBA(0xc9bbdcff), other, iw - 24);
}

static void dashed_box(cui_ctx *ui, cui_rect t, cui_color col) {   /* "tracejado": traços só nos trechos retos, longe dos cantos */
    float m = 5, d = 3.5f; cui_color k = col;
    for (int e = 0; e < 2; e++) {
        float x = t.x + m + e * (t.w - 2 * m - d);
        cui_draw_rect(ui, (cui_rect){ x, t.y, d, 1.2f }, CUI_CORNERS(0.6f), k);
        cui_draw_rect(ui, (cui_rect){ x, t.y + t.h - 1.2f, d, 1.2f }, CUI_CORNERS(0.6f), k);
        float y = t.y + m + e * (t.h - 2 * m - d);
        cui_draw_rect(ui, (cui_rect){ t.x, y, 1.2f, d }, CUI_CORNERS(0.6f), k);
        cui_draw_rect(ui, (cui_rect){ t.x + t.w - 1.2f, y, 1.2f, d }, CUI_CORNERS(0.6f), k);
    }
}

static void art_servers(cui_ctx *ui, const app_state_t *s, cui_rect c) {
    int n = community_total(s);
    char title[64]; snprintf(title, sizeof title, "Servidores · %d comunidade%s", n, n == 1 ? "" : "s");
    header_sz(ui, c, CUI_ICON_USERS, title, 10.5f);
    stack_grid g = stack_servers_grid(n);
    float iw = c.w - PAD * 2, gap = 4, side = (iw - 5 * gap) / 6.0f;
    float gh = g.rows * side + (g.rows - 1) * gap, by = c.y + 36, bh = c.h - 36 - PAD;
    float y0 = by + (bh - gh) * 0.5f, x0 = c.x + PAD;
    for (int i = 0; i < g.cells; i++) {
        cui_rect t = { x0 + (i % 6) * (side + gap), y0 + (i / 6) * (side + gap), side, side };
        bool last = i == g.cells - 1;
        cui_corners r7 = CUI_CORNERS(7);
        if (i == 0) {
            cui_draw_rect_gradient(ui, t, r7, CUI_RGBA(0xb01fcfff), CUI_RGBA(0x6d5cf6ff));
            cui_draw_text(ui, CUI_FONT_MEDIUM, side * 0.5f, (cui_vec2){ t.x + side * 0.5f - tw(side * 0.5f, "B") * 0.5f, t.y + side * 0.24f }, CUI_RGBA(0xffffffff), "B", -1);
        } else if (last && g.plus_n > 0) {
            char b[12]; snprintf(b, sizeof b, "+%d", g.plus_n);
            cui_draw_rect_border(ui, t, r7, CUI_RGBA(0x3a1d56ff), WHITE(0x1c), 1.0f);
            cui_draw_text(ui, CUI_FONT_MEDIUM, side * 0.42f, (cui_vec2){ t.x + side * 0.5f - tw(side * 0.42f, b) * 0.5f, t.y + side * 0.28f }, CUI_RGBA(0xe6dcf5ff), b, -1);
        } else if (last) {   /* "+" tracejado */
            cui_draw_rect(ui, t, r7, WHITE(0x08));
            dashed_box(ui, t, CUI_RGBA(0xc4b0ffb0));
            cui_draw_icon(ui, CUI_ICON_PLUS, (cui_rect){ t.x + side * 0.25f, t.y + side * 0.25f, side * 0.5f, side * 0.5f }, side * 0.5f, CUI_RGBA(0xc4b0ffdd));
        } else {
            char nm[96]; if (s && demo_servers < 0 && i - 1 < s->community_count) snprintf(nm, sizeof nm, "%s", s->communities[i - 1].name);
            else snprintf(nm, sizeof nm, "%c%c", 'T' + (i % 5), 'A' + (i * 3) % 20);
            char i2[3]; ini(nm, i2);
            if (!i2[1]) { /* uma letra só: ok */ }
            cui_draw_rect_border(ui, t, r7, CUI_RGBA(0x2c1546ff), WHITE(0x14), 1.0f);
            cui_draw_text(ui, CUI_FONT_MEDIUM, side * 0.38f, (cui_vec2){ t.x + side * 0.5f - tw(side * 0.38f, i2) * 0.5f, t.y + side * 0.3f }, CUI_RGBA(0xe6dcf5ff), i2, -1);
        }
    }
}

static void art_members(cui_ctx *ui, const app_state_t *s, cui_rect c) {
    int n = 1 + (s ? s->friend_count : 3);
    char title[48]; snprintf(title, sizeof title, "Membros · %d", n);
    header(ui, c, CUI_ICON_USERS, title);
    float x0 = c.x + PAD, iw = c.w - PAD * 2, y = c.y + 40;
    for (int i = 0; i < 3; i++) {
        const char *nm = i == 0 ? ((s && s->user_name[0]) ? s->user_name : "Você") : friend_name(s, i - 1);
        avatar(ui, x0 + 8, y + 8, 8, nm, i, false);
        txt(ui, CUI_FONT_MEDIUM, 10.5f, x0 + 24, y + 2, CUI_RGBA(0xedf1f7e8), nm, iw - 44);
        cui_draw_circle(ui, (cui_vec2){ x0 + iw - 5, y + 8 }, 3.2f, i == 2 ? CUI_RGBA(0x6b5a80ff) : B_ONLINE);
        y += 22;
    }
}

static void art_voice(cui_ctx *ui, const app_state_t *s, cui_rect c) {
    int n = people_in_call(s);
    char title[64]; snprintf(title, sizeof title, "Sala geral · %d na call", n);
    header(ui, c, CUI_ICON_VOLUME_2, title);
    stack_grid g = stack_voice_grid(n);
    float iw = c.w - PAD * 2, gap = 6, top = c.y + 36, ctl_h = 14, bottom = c.y + c.h - PAD - ctl_h - 6;
    float bh = bottom - top;
    float side = fminf((iw - (g.cols - 1) * gap) / g.cols, (bh - (g.rows - 1) * gap) / g.rows);
    float gw = g.cols * side + (g.cols - 1) * gap, gh = g.rows * side + (g.rows - 1) * gap;
    float y0 = top + (bh - gh) * 0.5f;
    cui_corners r9 = CUI_CORNERS(9);
    for (int i = 0; i < g.cells; i++) {
        int row = i / g.cols, in_row = g.cells - row * g.cols; if (in_row > g.cols) in_row = g.cols;
        float rx = c.x + (c.w - (in_row * side + (in_row - 1) * gap)) * 0.5f;   /* cada linha centralizada */
        cui_rect t = { rx + (i % g.cols) * (side + gap), y0 + row * (side + gap), side, side };
        bool speaking = i == (n > 1 ? 1 : 0);
        bool plus = g.plus_n > 0 && i == g.cells - 1;
        if (speaking) cui_draw_shadow(ui, t, r9, 9.0f, 1.0f, (cui_vec2){ 0, 0 }, CUI_RGBA(0x57d28b90));
        cui_draw_rect_gradient(ui, t, r9, plus ? CUI_RGBA(0x3a1d56ff) : CUI_RGBA(0x9b6cf8ff), plus ? CUI_RGBA(0x2c1546ff) : CUI_RGBA(0x5b3fd0ff));
        cui_draw_rect_border(ui, t, r9, CUI_RGBA(0x00000000), speaking ? CUI_RGBA(0x57d28bff) : WHITE(0x24), speaking ? 1.6f : 1.0f);
        cui_draw_rect(ui, (cui_rect){ t.x + 9, t.y + 1, t.w - 18, 1 }, CUI_CORNERS(0.5f), WHITE(0x40));   /* brilho interno de 1 px, longe dos cantos */
        char lab[12];
        if (plus) snprintf(lab, sizeof lab, "+%d", g.plus_n);
        else { const char *nm = i == 0 ? ((s && s->user_name[0]) ? s->user_name : "Você") : friend_name(s, i - 1); ini(nm, lab); if (i >= 3 && i > 2) { char b[3]; ini(i >= 3 ? "Convidado" : nm, b); (void)b; } }
        float sz = side * 0.36f; if (sz > 15) sz = 15;
        cui_draw_text(ui, CUI_FONT_MEDIUM, sz, (cui_vec2){ t.x + side * 0.5f - tw(sz, lab) * 0.5f, t.y + side * 0.5f - sz * 0.62f }, CUI_RGBA(0xffffffee), lab, -1);
    }
    /* barra de controles em pílulas; a última, vermelha, sai da call */
    float pw = 22, ph = 14, pg = 5, total = 4 * pw + 3 * pg, px = c.x + (c.w - total) * 0.5f, py = c.y + c.h - PAD - ph;
    const char *ic[4] = { CUI_ICON_MIC, CUI_ICON_CAMERA, CUI_ICON_MONITOR, CUI_ICON_LOG_OUT };
    for (int i = 0; i < 4; i++) {
        cui_rect p = { px + i * (pw + pg), py, pw, ph };
        cui_draw_rect_border(ui, p, CUI_CORNERS(7), i == 3 ? CUI_RGBA(0xe95555ff) : WHITE(0x1c), i == 3 ? CUI_RGBA(0xffffff30) : WHITE(0x20), 1.0f);
        cui_draw_icon(ui, ic[i], (cui_rect){ p.x + 6, p.y + 2, 10, 10 }, 9, CUI_RGBA(0xffffffee));
    }
}

static void art_history(cui_ctx *ui, const app_state_t *s, cui_rect c) {
    header(ui, c, CUI_ICON_HISTORY, "Histórico de chamadas");
    float x0 = c.x + PAD, iw = c.w - PAD * 2, y = c.y + 40;
    static const char *dur[3] = { "12:04", "3:21", "perdida" };
    const char *ic[3] = { CUI_ICON_ARROW_DOWN, CUI_ICON_ARROW_UP_RIGHT, CUI_ICON_PHONE };
    cui_color col[3] = { B_ONLINE, B_C3, B_DANGER };
    for (int i = 0; i < 3; i++) {
        cui_draw_circle(ui, (cui_vec2){ x0 + 8, y + 8 }, 8, (cui_color){ col[i].r, col[i].g, col[i].b, 0x38 });
        cui_draw_icon(ui, ic[i], (cui_rect){ x0 + 3, y + 3, 10, 10 }, 10, col[i]);
        txt(ui, CUI_FONT_MEDIUM, 10.5f, x0 + 22, y + 2, CUI_RGBA(0xedf1f7e8), friend_name(s, i), iw - 22 - tw(10, dur[i]) - 6);
        cui_draw_text(ui, CUI_FONT_BODY, 10, (cui_vec2){ x0 + iw - tw(10, dur[i]), y + 3 }, i == 2 ? col[2] : CUI_RGBA(0xa592b8ff), dur[i], -1);
        y += 22;
    }
}

static void art_friends(cui_ctx *ui, const app_state_t *s, cui_rect c) {
    header(ui, c, CUI_ICON_USER_PLUS, "Amigos");
    float x0 = c.x + PAD, iw = c.w - PAD * 2, y = c.y + 38;
    for (int i = 0; i < 2; i++) {
        avatar(ui, x0 + 8, y + 8, 8, friend_name(s, i), i + 1, i == 0);
        txt(ui, CUI_FONT_MEDIUM, 10.5f, x0 + 24, y + 2, CUI_RGBA(0xedf1f7e8), friend_name(s, i), iw - 24);
        y += 21;
    }
    cui_rect b = { x0, c.y + c.h - PAD - 18, 78, 18 };
    cui_draw_rect_gradient(ui, b, CUI_CORNERS(9), CUI_RGBA(0x9b6cf8ff), CUI_RGBA(0x6d5cf6ff));
    cui_draw_text(ui, CUI_FONT_MEDIUM, 10.5f, (cui_vec2){ b.x + (b.w - tw(10.5f, "Adicionar")) * 0.5f, b.y + 3.5f }, CUI_RGBA(0xffffffff), "Adicionar", -1);
}

static void art_generic(cui_ctx *ui, cui_rect c, int kind) {
    header(ui, c, CUI_ICON_MENU, kind >= 0 ? panel_title(kind) : "Painel");
    float iw = c.w - PAD * 2, ly = c.y + 40;
    cui_draw_rect(ui, (cui_rect){ c.x + PAD, ly, iw * 0.68f, 12 }, CUI_CORNERS(6), WHITE(0x3a));
    cui_draw_rect_gradient(ui, (cui_rect){ c.x + PAD + iw * 0.34f, ly + 18, iw * 0.66f, 12 }, CUI_CORNERS(6), CUI_RGBA(0xc4b0ffe6), CUI_RGBA(0x8b5cf6e6));
    cui_draw_rect(ui, (cui_rect){ c.x + PAD, ly + 36, iw * 0.5f, 12 }, CUI_CORNERS(6), WHITE(0x30));
}

void drag_stack_art(cui_ctx *ui, const app_state_t *s, int kind, cui_rect c) {
    switch (kind) {
    case P_CHAT: art_chat(ui, s, c, s && s->active_group >= 0 && s->active_group < s->group_count); break;
    case P_CHAT2: art_chat(ui, s, c, false); break;
    case P_NAV: art_nav(ui, s, c); break;
    case P_SERVERS: art_servers(ui, s, c); break;
    case P_MEMBERS: art_members(ui, s, c); break;
    case P_VOICE: art_voice(ui, s, c); break;
    case P_HISTORY: art_history(ui, s, c); break;
    case P_FRIENDS: art_friends(ui, s, c); break;
    default: art_generic(ui, c, kind); break;
    }
}

/* canvas: marcadores + sombras + vidro + borda das três camadas; o da frente leva a arte */
void drag_stack_paint(cui_ctx *ui, cui_rect r, void *user) {
    (void)r;
    const drag_stack_draw *d = (const drag_stack_draw *)user;
    cui_corners rad = CUI_CORNERS(16);
    /* âncora de captura: um vidro invisível ANTES das sombras, para o desfoque não conter a sombra dos próprios cartões */
    briks_pile_set(3, 0, 0, 0, 1, 0);
    cui_draw_rect(ui, (cui_rect){ 0, 0, 4, 4 }, CUI_CORNERS(2), PILE_MARK(3));
    cui_draw_rect(ui, (cui_rect){ 0, 0, 4, 4 }, CUI_CORNERS(2), B_GLASS_POPUP);
    float grow = 1.0f + 0.55f * d->lift;
    for (int i = 0; i < PILE_LAYERS; i++) {
        cui_rect c = { d->cx[i] - d->w * 0.5f, d->cy[i] - d->h * 0.5f, d->w, d->h };
        briks_pile_set(i, d->cx[i], d->cy[i], d->ang[i], d->scale, d->alpha * PILE_ALPHA[i]);
        cui_draw_rect(ui, (cui_rect){ 0, 0, 2, 2 }, CUI_CORNERS(0), PILE_MARK(i));
        cui_draw_shadow(ui, c, rad, 50.0f * grow, -14.0f, (cui_vec2){ 0, 26.0f * grow }, CUI_RGBA(0x0a001999));
        cui_draw_shadow(ui, c, rad, 18.0f * grow, -2.0f, (cui_vec2){ 0, 8.0f * grow }, CUI_RGBA(0x0a001952));
        cui_draw_rect_border(ui, c, rad, B_GLASS_POPUP, CUI_RGBA(0xffffff26), 1.0f);
        if (i == PILE_LAYERS - 1) drag_stack_art(ui, d->s, art_override >= 0 ? art_override : d->kind, c);
    }
    cui_draw_rect(ui, (cui_rect){ 0, 0, 2, 2 }, CUI_CORNERS(0), PILE_MARK_END);
}
