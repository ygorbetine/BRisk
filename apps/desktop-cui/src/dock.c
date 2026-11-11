/* Dock: desenha a árvore de painéis (layout.c), as divisórias redimensionáveis, o arrastar-e-encaixar estilo FancyZones,
 * o menu "+ Painel" e a persistência em %APPDATA%\Briks-CUI\layout.json. Os painéis animam quando o layout muda e a pré-visualização é ao vivo no arraste; parado, não redesenha. */
#include "briks_ui.h"
#include "layout.h"
#include "drag_stack.h"
#include "drag_stack_ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>
#include <shlobj.h>

static layout_t L;
static bool inited, demo_mode, demo_hover_div;
static char file_path[MAX_PATH];

typedef struct { bool active; int kind; bool from_menu; } drag_t;
static drag_t drag;
static int resizing = -1;          /* nó de divisão sendo arrastado */
static bool prev_down;
static bool drag_cancelled;
static bool menu_open;
static float menu_ax, menu_ay;     /* onde o usuário clicou para adicionar: o menu nasce ancorado aqui */
static int menu_age;               /* quadros desde que abriu (o clique que abriu não pode fechar) */
static int hdr_press = -1;         /* painel cujo cabeçalho está apertado (ainda sem passar do limiar) */
static float hdr_px, hdr_py;
#define HDR_DRAG_PX 5.0f           /* limiar: clique simples no cabeçalho não arrasta */
#define MENU_W 300.0f
static bool fake_mouse; static cui_vec2 fake_pos;
static lrect last_r[LAYOUT_MAX_NODES];   /* retângulos do último quadro (para o roteiro) */
static float last_vp_w = 1280;

/* ---- pilha levantada: prévia que acompanha o cursor (dinâmica em drag_stack.c, desenho em drag_stack_ui.c, vidro/giro em glass.c) ---- */
static pile_t pile;
static bool prev_drag_active;
static int pile_kind = -1;
static float pile_w = 170, pile_h = 104, pile_ox, pile_oy;   /* tamanho do cartão e origem (centro ao pegar) */

static void save_layout(void) {
    if (demo_mode || !file_path[0]) return;
    if ((getenv("BRIKS_SCRIPT") || getenv("BRIKS_HIDDEN")) && !getenv("BRIKS_LAYOUT_FILE")) return;   /* teste automatizado nunca grava no layout real do usuário */
    char js[4096];
    if (!layout_to_json(&L, js, sizeof js)) return;
    FILE *f = fopen(file_path, "wb");
    if (f) { fputs(js, f); fclose(f); }
}

static void load_layout(float width) {
    layout_app_default(&L, width);
    const char *over = getenv("BRIKS_LAYOUT_FILE");
    if (over && over[0]) snprintf(file_path, sizeof file_path, "%s", over);
    else {
        char appdata[MAX_PATH];
        if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) == S_OK) {
            char dir[MAX_PATH]; snprintf(dir, sizeof dir, "%s\\Briks-CUI", appdata);
            CreateDirectoryA(dir, NULL);
            snprintf(file_path, sizeof file_path, "%s\\layout.json", dir);
        }
    }
    FILE *f = file_path[0] ? fopen(file_path, "rb") : NULL;
    if (f) {
        char js[4096]; size_t n = fread(js, 1, sizeof js - 1, f); js[n] = 0; fclose(f);
        layout_t tmp;
        if (layout_from_json(&tmp, js)) L = tmp;   /* arquivo inválido: segue com o padrão */
    }
}

/* BRIKS_DOCK=default|resize|added|moved|closed|drag|place: layouts de demonstração para as capturas (não gravam arquivo). */
static void apply_demo(const char *d, float width) {
    demo_mode = true;
    layout_default(&L, width);
    if (!strcmp(d, "default")) return;
    if (!strcmp(d, "resize")) {
        int nav = layout_find(&L, P_NAV), inner = L.n[nav].parent;
        L.n[inner].ratio = 0.36f; demo_hover_div = true; return;
    }
    if (!strcmp(d, "stacked")) { layout_move(&L, P_CHAT, P_NAV, Z_BOTTOM); return; }   /* conversa abaixo do menu lateral */
    if (!strcmp(d, "moved")) { layout_move(&L, P_CHAT, P_NAV, Z_LEFT); return; }
    layout_split(&L, P_CHAT, P_MEMBERS, Z_RIGHT);
    layout_split(&L, P_NAV, P_VOICE, Z_BOTTOM);
    if (!strcmp(d, "added")) return;
    if (!strcmp(d, "closed")) { layout_remove(&L, P_VOICE); return; }
    if (!strcmp(d, "drag")) {   /* arrastando "Amigos" para a esquerda da conversa */
        drag = (drag_t){ true, P_FRIENDS, true };
        lrect area = { 10, 10, width - 20, 800 - 3 - 20 }, r[LAYOUT_MAX_NODES];
        layout_compute(&L, area, r);
        lrect c = r[layout_find(&L, P_CHAT)];
        fake_mouse = true; fake_pos = (cui_vec2){ c.x + c.w * 0.1f, c.y + c.h * 0.5f };
    }
}

/* Painéis vivos: o retângulo exibido de cada painel (por tipo) persegue o alvo com interpolação exponencial (~200 ms).
 * Durante o arraste o alvo vem da árvore de PRÉ-VISUALIZAÇÃO (layout_preview), calculada a cada quadro. */
static lrect anim[P_COUNT];
static bool anim_on[P_COUNT];
static zone_hold hold;
static float last_vp_h = 800;
#define ANIM_TAU_SIMPLE 0.014f   /* Simplificado: assenta em ~40 ms, sem rastro elástico */
#define ANIM_TAU 0.055f     /* 95% em ~165 ms */
#define HOLD_S   0.10f      /* histerese: tempo na nova zona antes de trocar a prévia */

typedef struct { bool slot; lrect slot_r; } over_t;

static void paint_over(cui_ctx *ui, cui_rect r, void *user) {
    (void)r;
    const over_t *o = (const over_t *)user;
    if (!o->slot) return;
    /* só um contorno sutil, com o MESMO retângulo e raio (22) do painel: segue a curva, sem cantos retos */
    cui_rect z = { o->slot_r.x, o->slot_r.y, o->slot_r.w, o->slot_r.h };
    /* halo azul (3 anéis que somem para fora) + contorno azul nítido: o encaixe fica evidente, sempre com o raio do painel */
    for (int i = 3; i >= 1; i--) {
        float g = (float)i * 3.0f; cui_rect h = { z.x - g, z.y - g, z.w + 2 * g, z.h + 2 * g };
        cui_draw_rect_border(ui, h, CUI_CORNERS(g_th.panel_radius + g), CUI_RGBA(0x00000000), CUI_RGBA(0x5aa0ff00 | (0x14 * (4 - i))), 3.0f);
    }
    cui_draw_rect_border(ui, z, CUI_CORNERS(g_th.panel_radius), CUI_RGBA(0x00000000), CUI_RGBA(0x7db4ffee), 2.0f);
}

static float servers_target_w(float vw) {
    float t = fminf(fmaxf((vw - 560.0f) / 340.0f, 0.0f), 1.0f), icon_w = 36.0f + 20.0f * t + 20.0f;   /* só ícones: 56..76 */
    if (vw <= 900.0f) return icon_w;
    float u = fminf((vw - 900.0f) / 40.0f, 1.0f), big = 180.0f + 90.0f * fminf((vw - 900.0f) / 500.0f, 1.0f);   /* larga: 180..270 */
    return 76.0f + (big - 76.0f) * u;
}
static bool inside(lrect r, float x, float y) { return x >= r.x && y >= r.y && x <= r.x + r.w && y <= r.y + r.h; }

static void tool_button(cui_ctx *ui, int id, const char *icon, bool *hit) {
    cui_push_id_int(ui, id);
    bool hot = briks_hover_top(ui, "b");
    cui_box(ui, .id = "b", .width = CUI_PX(24), .height = CUI_PX(24), .radius = 9, .bg = hot ? CUI_RGBA(0x2a1b4fee) : B_A(B_PANEL, 0xe6), .border = 1,
            .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
        cui_icon(ui, icon, 13, B_TEXT);
    }
    *hit = briks_clicked_top(ui, "b");
    cui_pop_id(ui);
}

typedef struct { bool hot; } grip_t;
static void paint_grip(cui_ctx *ui, cui_rect r, void *user) {
    const grip_t *g = (const grip_t *)user;
    float bw = fminf(34.0f, r.w - 24.0f); cui_rect b = { r.x + r.w * 0.5f - bw * 0.5f, r.y + (r.w < 110.0f ? 4.0f : 8.0f), bw, 4.0f };
    cui_draw_rect(ui, b, CUI_CORNERS(2), g->hot ? CUI_RGBA(0xc4b0ffcc) : CUI_RGBA(0xffffff24));
}

typedef struct { cui_ctx *ui; app_state_t *s; lrect r[LAYOUT_MAX_NODES]; float mx, my; bool down; } rc_t;

/* Painel na posição absoluta r dentro do contêiner de pilha. */
/* Faixa do cabeçalho (arrastável como a barra de título de uma janela). A rail de comunidades não tem título: só a folga do topo. */
static float header_h(int kind) { return kind == P_SERVERS ? 12.0f : 56.0f; }
static bool in_header(int kind, lrect r, float x, float y) {
    if (!inside(r, x, y) || y > r.y + header_h(kind)) return false;
    int n = (r.w >= 120 ? 1 : 0) + (layout_count(&L) > 1 ? 1 : 0);
    float tw = 24.0f * n + 4.0f * (n - 1);
    if (n > 0 && kind != P_SERVERS && x >= r.x + r.w - 10 - tw - 4 && y < r.y + 38) return false;   /* sob os botões de atalho */
    return true;
}

static void render_leaf(rc_t *c, int kind, lrect r, bool tools, int hot) {
    cui_ctx *ui = c->ui;
    cui_push_id_int(ui, 100 + kind);
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_START, .pad_left = r.x, .pad_top = r.y) {
        cui_box(ui, .dir = CUI_STACK, .width = CUI_PX(r.w), .height = CUI_PX(r.h)) {
            g_panel_w = r.w;
            { int nb = (r.w >= 120 ? 1 : 0) + (layout_count(&L) > 1 ? 1 : 0); g_tools_w = (kind == P_SERVERS || !nb) ? 0.0f : 28.0f * (float)nb; }   /* reserva o canto dos botões + e × */
            ui_panel(ui, c->s, kind);
            if (hot) {   /* alça de arrasto: só aparece com o mouse sobre a faixa que move o painel, em QUALQUER painel; pintada em coordenadas do painel (não mexe no layout) */
                grip_t gp = { true };
                cui_canvas(ui, "grip", CUI_FILL, CUI_FILL, paint_grip, &gp, sizeof gp);
            }
            if (tools && !(kind == P_SERVERS && r.w < 76)) {
                { int nb = (r.w >= 120 ? 1 : 0) + (layout_count(&L) > 1 ? 1 : 0); if (nb) briks_cover(r.x + r.w - 10 - 28 * nb + 4 - 2, r.y + 8, 28 * nb, 28); }
                cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_END, .justify = CUI_JUSTIFY_START, .pad = 10) {
                    cui_box(ui, .dir = CUI_ROW, .gap = 4) {
                        bool p = false, x = false;
                        if (r.w >= 120) tool_button(ui, 2, CUI_ICON_PLUS, &p);
                        if (layout_count(&L) > 1) tool_button(ui, 3, CUI_ICON_X, &x);
                        if (p) { menu_open = true; menu_age = 0; menu_ax = c->mx; menu_ay = c->my; }
                        if (x) { layout_remove(&L, kind); save_layout(); }
                    }
                }
            }
        }
    }
    cui_pop_id(ui);
}

static void add_menu(cui_ctx *ui, cui_vec2 vp, cui_vec2 mp);

void dock_render(cui_ctx *ui, app_state_t *s) {
    cui_vec2 vp = cui_viewport_size(ui);
    bool scripted = getenv("BRIKS_SCRIPT") != NULL;
    if (!inited) {
        inited = true;
        const char *d = getenv("BRIKS_DOCK");
        load_layout(vp.x);
        if (d && d[0]) apply_demo(d, vp.x);
    }
    float dt = scripted ? 1.0f / 30.0f : cui_delta_time(ui);
    if (dt <= 0) dt = 1.0f / 60.0f;
    if (dt > 0.05f) dt = 0.05f;
    bool snap_all = g_perf >= PERF_MIN || (demo_mode && !scripted) || vp.x != last_vp_w || vp.y != last_vp_h;

    cui_vec2 mp = fake_mouse ? fake_pos : cui_mouse_pos(ui);
    bool down = cui_mouse_down(ui, CUI_MOUSE_LEFT);
    rc_t c = { ui, s, {{0}}, mp.x, mp.y, down };
    lrect area = { 10, 10, vp.x - 20, vp.y - 3 - 20 };
    layout_compute(&L, area, c.r);
    /* barra de grupos responsiva: a largura acompanha a janela (ícones 36..56 px; larga, ~180..270 px com nome) */
    { int si = layout_find(&L, P_SERVERS), sp = si >= 0 ? L.n[si].parent : -1;
      if (sp >= 0 && !L.n[sp].vertical && L.n[sp].a == si && c.r[sp].w > LAYOUT_GAP + 100) {
          float want = servers_target_w(vp.x), nr = want / (c.r[sp].w - LAYOUT_GAP);
          if (nr > 0.02f && nr < 0.6f && fabsf(nr - L.n[sp].ratio) > 1e-4f) { L.n[sp].ratio = nr; layout_compute(&L, area, c.r); }
      } }

    /* redimensionar: acompanha o mouse enquanto o botão está apertado (sem animação) */
    if (resizing >= 0) {
        if (down) { layout_resize(&L, resizing, c.r[resizing], L.n[resizing].vertical ? mp.y : mp.x); layout_compute(&L, area, c.r); }
        else { resizing = -1; save_layout(); }
    }

    /* arrastar pelo cabeçalho: aperta no cabeçalho (fora dos botões), passa do limiar e levanta o painel como a barra de título de uma janela */
    int hdr_hot = -1;
    bool modal = menu_open || briks_modal_open();   /* regra central: qualquer diálogo/menu aberto */
    if (!drag.active && resizing < 0 && !modal) {
        int hk = -1;
        for (int k = 0; k < P_COUNT; k++) if (anim_on[k] && in_header(k, anim[k], mp.x, mp.y)) hk = k;
        if (hdr_press < 0 && hk >= 0 && cui_mouse_clicked(ui, CUI_MOUSE_LEFT)) { hdr_press = hk; hdr_px = mp.x; hdr_py = mp.y; }
        if (hdr_press >= 0 && !down) hdr_press = -1;
        if (hdr_press >= 0 && hypotf(mp.x - hdr_px, mp.y - hdr_py) >= HDR_DRAG_PX) { drag = (drag_t){ true, hdr_press, false }; hdr_press = -1; }
        hdr_hot = hdr_press >= 0 ? hdr_press : hk;
        if (hdr_hot >= 0 && !drag.active) cui_set_cursor(ui, CUI_CURSOR_MOVE);
    } else hdr_press = -1;

    /* arrastar: a zona sob o cursor (medida no layout ATUAL, estável) passa pela histerese e gera a árvore de pré-visualização */
    layout_t P = L; bool previewing = false;
    if (!drag.active) zone_hold_reset(&hold);
    else {
        cui_set_cursor(ui, CUI_CURSOR_MOVE);
        int tk = -1; zone_t z = Z_NONE;
        for (int i = 0; i < LAYOUT_MAX_NODES; i++) if (L.n[i].used && L.n[i].leaf && inside(c.r[i], mp.x, mp.y)) {
            if (!drag.from_menu && L.n[i].panel == drag.kind) { tk = drag.kind; z = Z_CENTER; }   /* de volta à origem: desfaz a prévia */
            else { tk = L.n[i].panel; z = layout_zone_at(c.r[i], mp.x, mp.y); }
        }
        zone_hold_update(&hold, tk, z, dt, HOLD_S);
        previewing = layout_preview(&L, drag.kind, drag.from_menu, hold.cur_target, hold.cur_zone, &P);
        bool release = drag.from_menu ? cui_mouse_clicked(ui, CUI_MOUSE_LEFT) : (prev_down && !down && !fake_mouse);
        if (release && getenv("BRIKS_DBG")) fprintf(stderr, "release: down=%d prev=%d fake=%d from_menu=%d\n", down, prev_down, fake_mouse, drag.from_menu);
        if (release) { if (previewing) { L = P; save_layout(); } drag.active = false; drag_cancelled = false; zone_hold_reset(&hold); }
        else if (cui_mouse_clicked(ui, CUI_MOUSE_RIGHT) || cui_key_pressed(ui, CUI_KEY_ESCAPE)) { drag_cancelled = true; if (getenv("BRIKS_DBG")) fprintf(stderr, "cancel: right=%d esc=%d\n", cui_mouse_clicked(ui, CUI_MOUSE_RIGHT), cui_key_pressed(ui, CUI_KEY_ESCAPE)); drag.active = false; zone_hold_reset(&hold); }
        if (!drag.active) { P = L; previewing = false; layout_compute(&L, area, c.r); }
    }
    /* pilha: nasce ao começar o arraste; ao soltar desce até o slot, no Esc volta à origem */
    int pile_end = 0;   /* 1 = soltou, 2 = cancelou */
    if (prev_drag_active && !drag.active) pile_end = drag_cancelled ? 2 : 1;
    if (drag.active && !prev_drag_active) {
        lrect src = (drag.kind >= 0 && anim_on[drag.kind]) ? anim[drag.kind] : (lrect){ 0, 0, 300, 200 };
        stack_card_size(drag.kind, src.w, src.h, &pile_w, &pile_h);
        pile_kind = drag.kind;
        pile_ox = fminf(fmaxf(mp.x + 16 + pile_w * 0.5f, pile_w * 0.5f + 8), vp.x - pile_w * 0.5f - 8);
        pile_oy = fminf(fmaxf(mp.y + 16 + pile_h * 0.5f, pile_h * 0.5f + 8), vp.y - pile_h * 0.5f - 8);
        pile_grab(&pile, pile_ox, pile_oy);
    }
    prev_drag_active = drag.active;
    if (drag.active) {
        float fx = fminf(fmaxf(mp.x + 16 + pile_w * 0.5f, pile_w * 0.5f + 8), vp.x - pile_w * 0.5f - 8);
        float fy = fminf(fmaxf(mp.y + 16 + pile_h * 0.5f, pile_h * 0.5f + 8), vp.y - pile_h * 0.5f - 8);
        pile_follow(&pile, fx, fy, dt);
    }
    if (getenv("BRIKS_DBG")) { static int fr; fprintf(stderr, "f%d mp=%.0f,%.0f down=%d drag=%d prev=%d prv=%d\n", fr++, mp.x, mp.y, down, drag.active, prev_down, previewing); }
    prev_down = down;
    memcpy(last_r, c.r, sizeof last_r);

    /* alvos = retângulos da árvore exibida (prévia durante o arraste); depois interpola */
    lrect pr[LAYOUT_MAX_NODES], tgt[P_COUNT]; bool present[P_COUNT] = { false };
    layout_compute(&P, area, pr);
    for (int i = 0; i < LAYOUT_MAX_NODES; i++) if (P.n[i].used && P.n[i].leaf) { tgt[P.n[i].panel] = pr[i]; present[P.n[i].panel] = true; }
    bool any_before = false; for (int k = 0; k < P_COUNT; k++) any_before |= anim_on[k];
    bool moving = false;
    for (int k = 0; k < P_COUNT; k++) {
        if (!present[k]) { anim_on[k] = false; continue; }
        if (!anim_on[k]) {
            anim_on[k] = true; anim[k] = tgt[k];
            if (any_before && !snap_all) { anim[k].x += tgt[k].w * 0.08f; anim[k].y += tgt[k].h * 0.08f; anim[k].w *= 0.84f; anim[k].h *= 0.84f; }
        }
        if (snap_all || resizing >= 0) anim[k] = tgt[k];
        else moving |= lrect_step(&anim[k], tgt[k], dt, g_perf == PERF_SIMPLE ? ANIM_TAU_SIMPLE : ANIM_TAU);
    }
    if (pile_end == 1) {   /* desce até o centro do slot onde o painel se acomoda */
        int k = pile_kind;
        if (k >= 0 && present[k]) pile_release(&pile, tgt[k].x + tgt[k].w * 0.5f, tgt[k].y + tgt[k].h * 0.5f);
        else pile_cancel(&pile, pile_ox, pile_oy);
        if (pile.phase == PILE_HELD) pile_cancel(&pile, pile_ox, pile_oy);
    } else if (pile_end == 2) pile_cancel(&pile, pile_ox, pile_oy);
    pile_simple = g_perf >= PERF_MIN ? 2 : g_perf == PERF_SIMPLE ? 1 : 0;
    pile_step(&pile, dt);
    if (moving || drag.active || pile_active(&pile)) cui_request_redraw(ui);   /* parado = nenhum redesenho */
    last_vp_w = vp.x; last_vp_h = vp.y;
    (void)previewing;

    over_t o; memset(&o, 0, sizeof o);
    int dk = drag.active ? drag.kind : -1;
    if (drag.active && present[dk]) { o.slot = true; o.slot_r = anim[dk]; }

    cui_box(ui, .dir = CUI_STACK, .width = CUI_FILL, .height = CUI_FILL) {
        for (int k = 0; k < P_COUNT; k++) {
            if (!present[k]) continue;
            bool hover = !drag.active && resizing < 0 && !modal && inside(anim[k], mp.x, mp.y);
            render_leaf(&c, k, anim[k], hover && !(drag.active && k == dk), hdr_hot == k);
        }
        /* divisórias: alça visível e redimensionável (só com o layout parado) */
        if (!moving && !drag.active && !modal) {
            for (int i = 0; i < LAYOUT_MAX_NODES; i++) if (L.n[i].used && !L.n[i].leaf) {
                const lnode *n = &L.n[i]; lrect r = c.r[i], a = c.r[n->a];
                lrect d = n->vertical ? (lrect){ r.x, a.y + a.h, r.w, LAYOUT_GAP } : (lrect){ a.x + a.w, r.y, LAYOUT_GAP, r.h };
                bool ndiv = inside((lrect){ d.x - 3, d.y - 3, d.w + 6, d.h + 6 }, mp.x, mp.y);
                bool hot = resizing == i || (resizing < 0 && ndiv) || demo_hover_div;
                if (hot) {
                    cui_push_id_int(ui, 300 + i);
                    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_START, .pad_left = d.x, .pad_top = d.y) {
                        cui_box(ui, .dir = CUI_STACK, .width = CUI_PX(d.w), .height = CUI_PX(d.h), .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                            float len = (n->vertical ? d.w : d.h) * 0.12f; if (len < 56) len = 56;
                            cui_box(ui, .width = CUI_PX(n->vertical ? len : 4), .height = CUI_PX(n->vertical ? 4 : len), .radius = 2, .bg = B_A(B_C3, 0xee)) {}
                        }
                    }
                    cui_pop_id(ui);
                    cui_set_cursor(ui, n->vertical ? CUI_CURSOR_RESIZE_NS : CUI_CURSOR_RESIZE_EW);
                }
                if (resizing < 0 && ndiv && cui_mouse_clicked(ui, CUI_MOUSE_LEFT)) resizing = i;
            }
        }
        if (drag.active) {
            cui_canvas(ui, "dock-over", CUI_FILL, CUI_FILL, paint_over, &o, sizeof o);
            if (drag.from_menu) {
                cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_START, .pad = 18, .pad_top = 64) {
                    cui_box(ui, .pad_x = 16, .pad_y = 9, .radius = 14, .bg = B_A(B_PANEL, 0xf2), .border = 1, .border_color = B_GLASS_BORDER) {
                        cui_text(ui, "Clique para encaixar o painel · botão direito ou Esc cancela", .size = 13);
                    }
                }
            }
        }
        if (pile_active(&pile)) {
            drag_stack_draw pd; memset(&pd, 0, sizeof pd); pd.s = s;
            float sc = pile_scale(&pile), ax = pile.x[PILE_LAYERS - 1].x, ay = pile.y[PILE_LAYERS - 1].x;
            for (int i = 0; i < PILE_LAYERS; i++) {   /* afastamentos entre os cartões escalam com a pilha */
                pd.cx[i] = ax + (pile.x[i].x - ax) * sc; pd.cy[i] = ay + (pile.y[i].x - ay) * sc; pd.ang[i] = pile.rot[i].x;
            }
            pd.scale = sc; pd.alpha = pile_alpha(&pile); pd.lift = pile.lift.x; pd.w = pile_w; pd.h = pile_h; pd.kind = pile_kind;
            cui_canvas(ui, "dock-pile", CUI_FILL, CUI_FILL, drag_stack_paint, &pd, sizeof pd);
        }
        add_menu(ui, vp, mp);
    }
}

/* alfa escalado (fade de abertura dos itens/textos) */
static cui_color fade(cui_color c, float a) { c.a = (uint8_t)(c.a * a + 0.5f); return c; }

typedef struct { float a; } menu_paint_t;
/* fundo do popup: sombra suave que segue a curva, vidro (camada 1, com desfoque do que está atrás), tinta violeta leve e borda de luz fina */
static void menu_bg_paint(cui_ctx *ui, cui_rect r, void *user) {
    const menu_paint_t *mp = (const menu_paint_t *)user;
    cui_corners rad = CUI_CORNERS(22);
    if (g_perf < PERF_MIN) {
        cui_draw_shadow(ui, r, rad, 40.0f, -12.0f, (cui_vec2){ 0, 20.0f }, fade(CUI_RGBA(0x0a001977), mp->a));
        cui_draw_shadow(ui, r, rad, 14.0f, -2.0f, (cui_vec2){ 0, 6.0f }, fade(CUI_RGBA(0x0a00193c), mp->a));
    }
    cui_draw_rect_border(ui, r, rad, B_GLASS_POPUP, fade(CUI_RGBA(0xffffff30), mp->a), 1.0f);
    cui_draw_rect(ui, r, rad, fade(CUI_RGBA(0x8b5cf61c), mp->a));                                    /* tinta violeta */
    cui_rect in = { r.x + 1.5f, r.y + 1.5f, r.w - 3.0f, r.h - 3.0f };
    cui_draw_rect_border(ui, in, CUI_CORNERS(20.5f), CUI_RGBA(0x00000000), fade(CUI_RGBA(0xd9ccff26), mp->a), 1.0f);   /* rim light interno, sem listra */
}

/* item "pílula de vidro": translúcido, brilho suave ao passar o mouse */
static bool menu_pill(cui_ctx *ui, const char *label, int id, float a) {
    cui_push_id_int(ui, id);
    cui_rect lr = cui_get_rect(ui, cui_make_id(ui, "pill")); cui_vec2 m = cui_mouse_pos(ui);
    bool hot = lr.w > 0 && m.x >= lr.x && m.y >= lr.y && m.x < lr.x + lr.w && m.y < lr.y + lr.h;
    cui_box(ui, .id = "pill", .width = CUI_FILL, .height = CUI_PX(36), .radius = 18,
            .bg = fade(hot ? CUI_RGBA(0xc4b0ff3a) : CUI_RGBA(0xffffff16), a), .border = 1, .border_color = fade(hot ? CUI_RGBA(0xe9e0ff77) : CUI_RGBA(0xffffff2a), a),
            .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
        cui_text(ui, label, .font = CUI_FONT_MEDIUM, .size = 13, .color = fade(CUI_RGB(0xf6f1ff), a));
    }
    if (hot) cui_set_cursor(ui, CUI_CURSOR_HAND);
    bool hit = briks_clicked(ui, "pill");
    cui_pop_id(ui);
    return hit;
}

/* Menu "Adicionar painel": popup de vidro ancorado onde o usuário clicou, sempre inteiro dentro da janela. */
static void add_menu(cui_ctx *ui, cui_vec2 vp, cui_vec2 mp) {
    static float t;   /* abertura 0..1 */
    if (!menu_open) return;
    g_modal_now = true; g_in_modal++;
    if (menu_age == 0) t = g_perf >= PERF_MIN ? 1.0f : 0.0f;
    float dur = g_perf == PERF_SIMPLE ? 0.07f : 0.16f;
    if (t < 1.0f) { t += (dur > 0 ? 1.0f / 60.0f / dur : 1.0f); if (t > 1.0f) t = 1.0f; cui_request_redraw(ui); }
    float e = 1.0f - (1.0f - t) * (1.0f - t), a = 0.25f + 0.75f * e;
    int shown = 0; for (int k = 0; k < P_COUNT; k++) if (layout_find(&L, k) < 0) shown++;
    cui_rect lr = cui_get_rect(ui, cui_make_id(ui, "addmenu"));
    float w = MENU_W, h = lr.h > 0 ? lr.h : 150.0f + 44.0f * (shown ? shown : 1);
    float x = menu_ax, y = menu_ay;
    if (x + w + 8 > vp.x) x = menu_ax - w;      /* sem espaço à direita: abre para a esquerda do clique */
    if (y + h + 8 > vp.y) y = menu_ay - h;      /* sem espaço embaixo: abre para cima */
    x = fminf(fmaxf(x, 8), fmaxf(vp.x - w - 8, 8)); y = fminf(fmaxf(y, 8), fmaxf(vp.y - h - 8, 8));
    float lift = (1.0f - e) * 8.0f;             /* sobe 8 px ao abrir */
    bool out = menu_age > 0 && lr.w > 0 && cui_mouse_clicked(ui, CUI_MOUSE_LEFT) && !(mp.x >= lr.x && mp.y >= lr.y && mp.x < lr.x + lr.w && mp.y < lr.y + lr.h);
    if (out || (menu_age > 0 && cui_key_pressed(ui, CUI_KEY_ESCAPE))) { menu_open = false; g_in_modal--; return; }
    if (menu_age < 3) cui_request_redraw(ui);   /* o 2º quadro já mede o tamanho real e reposiciona */
    menu_age++;
    menu_paint_t pt = { a };
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_START, .pad_left = x, .pad_top = y + lift) {
        cui_box(ui, .id = "addmenu", .dir = CUI_STACK, .width = CUI_PX(w)) {
            cui_canvas(ui, "addmenu-bg", CUI_FILL, CUI_FILL, menu_bg_paint, &pt, sizeof pt);
            cui_box(ui, .width = CUI_FILL, .pad = 16, .gap = 8) {
                cui_text(ui, "Adicionar painel", .font = CUI_FONT_MEDIUM, .size = 15, .color = fade(CUI_RGB(0xfaf7ff), a));
                cui_text(ui, "Escolha o painel e leve o cursor à zona onde ele deve encaixar: os outros painéis abrem espaço ao vivo. Clique para confirmar.",
                         .size = 12, .color = fade(CUI_RGB(0xd3c8ee), a));
                for (int k = 0; k < P_COUNT; k++) {
                    if (layout_find(&L, k) >= 0) continue;
                    if (menu_pill(ui, panel_title(k), 400 + k, a)) { drag = (drag_t){ true, k, true }; menu_open = false; }
                }
                if (!shown) cui_text(ui, "Todos os painéis já estão na tela.", .size = 13, .color = fade(CUI_RGB(0xf6f1ff), a));
                if (menu_pill(ui, "Restaurar layout padrão", 499, a)) { layout_app_default(&L, vp.x); save_layout(); menu_open = false; }
            }
        }
    }
    g_in_modal--;
}

/* ---- ganchos do roteiro (BRIKS_SCRIPT): pontos e ações simbólicas, para o vídeo não depender de coordenadas fixas ---- */
bool dock_script_point(const char *spec, float *x, float *y) {
    char kind_s[32] = "", what[16] = "", zone_s[16] = "";
    if (sscanf(spec, "%15[^:]:%31[^:]:%15s", what, kind_s, zone_s) < 2) return false;
    int k = panel_from_name(kind_s), i = layout_find(&L, k);
    if (i < 0) return false;
    lrect r = last_r[i];
    int cnt = layout_count(&L);
    bool has_plus = r.w >= 120, has_x = cnt > 1;
    float right = r.x + r.w - 10, cy = r.y + 22;
    float x_close = right - 12;                                   /* da direita para a esquerda: x, +, alça */
    float x_plus = right - 12 - (has_x ? 28 : 0);
        if (!strcmp(what, "grip")) { *x = r.x + r.w * 0.35f; *y = r.y + 28; return true; }   /* a alça é o próprio cabeçalho */
    if (!strcmp(what, "plus") && has_plus) { *x = x_plus; *y = cy; return true; }
    if (!strcmp(what, "close") && has_x) { *x = x_close; *y = cy; return true; }
    if (!strcmp(what, "header")) { *x = r.x + r.w * 0.35f; *y = r.y + 28; return true; }
    if (!strcmp(what, "panel")) { *x = r.x + r.w * 0.5f; *y = r.y + r.h * 0.5f; return true; }
    if (!strcmp(what, "divider")) {          /* vão entre este painel e o irmão (ou o lado de dentro) */
        int p = L.n[i].parent; if (p < 0) return false;
        lrect a = last_r[L.n[p].a];
        if (L.n[p].vertical) { *x = r.x + r.w * 0.5f; *y = a.y + a.h + LAYOUT_GAP * 0.5f; }
        else { *x = a.x + a.w + LAYOUT_GAP * 0.5f; *y = r.y + r.h * 0.5f; }
        return true;
    }
    if (!strcmp(what, "zone")) {
        *x = r.x + r.w * 0.5f; *y = r.y + r.h * 0.5f;
        if (!strcmp(zone_s, "left")) *x = r.x + r.w * 0.1f;
        else if (!strcmp(zone_s, "right")) *x = r.x + r.w * 0.9f;
        else if (!strcmp(zone_s, "top")) *y = r.y + r.h * 0.1f;
        else if (!strcmp(zone_s, "bottom")) *y = r.y + r.h * 0.9f;
        return true;
    }
    return false;
}
void dock_script_pick(const char *kind) { int k = panel_from_name(kind); if (k >= 0) { drag = (drag_t){ true, k, true }; menu_open = false; } }
void dock_script_menu(float x, float y) { menu_open = true; menu_age = 0; menu_ax = x; menu_ay = y; }
int dock_probe(const char *n) {
    if (!strcmp(n, "menu")) return menu_open;
    if (!strcmp(n, "drag")) return drag.active;
    if (!strcmp(n, "count")) return layout_count(&L);
    if (!strcmp(n, "fill_gap")) { float mr = 0; for (int k = 0; k < P_COUNT; k++) if (anim_on[k]) mr = fmaxf(mr, anim[k].x + anim[k].w); return (int)lroundf(last_vp_w - 10.0f - mr); }   /* sobra à direita (0 = preenche) */
    return -1;
}
bool dock_has_panel(int kind) { return layout_find(&L, kind) >= 0; }
void dock_script_restore(void) { layout_app_default(&L, last_vp_w); menu_open = false; }
void dock_restore_default(void) { layout_app_default(&L, last_vp_w); save_layout(); menu_open = false; }
