#include "briks_ui.h"
#include <math.h>
#include <stdlib.h>

void briks_apply_theme(cui_ctx *ui) {
    cui_set_theme(ui, CUI_THEME_DARK);
    cui_set_accent(ui, CUI_ACCENT_VIOLET);
    cui_theme *t = cui_theme_get(ui);
    t->colors[CUI_TOKEN_BACKGROUND]     = B_BG;
    t->colors[CUI_TOKEN_SURFACE]        = B_PANEL;
    t->colors[CUI_TOKEN_SURFACE_RAISED] = B_GLASS_POPUP;   /* diálogos e popovers: vidro na camada 1 (glass.c) */
    t->colors[CUI_TOKEN_SURFACE_MUTED]  = B_LINE;
    t->colors[CUI_TOKEN_FOREGROUND]     = B_TEXT;
    t->colors[CUI_TOKEN_TEXT_SECONDARY] = B_MUTED;
    t->colors[CUI_TOKEN_TEXT_MUTED]     = CUI_RGB(0x8472a0);
    t->colors[CUI_TOKEN_BORDER]         = B_LINE;
    t->colors[CUI_TOKEN_BORDER_SUBTLE]  = CUI_RGBA(0xffffff12);
    t->colors[CUI_TOKEN_BORDER_STRONG]  = CUI_RGBA(0x8b5cf666);
    t->colors[CUI_TOKEN_ACCENT]         = B_C3;
    t->colors[CUI_TOKEN_ACCENT_STRONG]  = B_C2;
    t->colors[CUI_TOKEN_ACCENT_SUBTLE]  = CUI_RGBA(0x8b5cf622);
    t->colors[CUI_TOKEN_ACCENT_FOREGROUND] = CUI_RGB(0xffffff);
    t->colors[CUI_TOKEN_DANGER]         = B_DANGER;
    t->colors[CUI_TOKEN_SUCCESS]        = B_ONLINE;
    t->colors[CUI_TOKEN_HOVER]          = CUI_RGBA(0xffffff0f);
    t->colors[CUI_TOKEN_FOCUS_RING]     = B_C1;
    t->radius_control = g_th.control_radius;
    t->radius_panel   = 16;
    t->radius_surface = 20;
}

typedef struct { float strength; } ambient_t;

/* radial-gradient(rx ry at fx fy, cor a%, transparent 62%) do CSS: uma sombra muito desfocada de um retângulo pequeno
   vira uma mancha elíptica suave. rx/ry em unidades de vmax. */
static void blob(cui_ctx *ui, cui_rect area, float vmax, float fx, float fy, float rx, float ry, cui_color c, float a) {
    float cx = area.x + area.w * fx, cy = area.y + area.h * fy;
    float hx = rx * vmax * 0.62f, hy = ry * vmax * 0.62f;      /* alcance visível do degradê */
    float mn = hx < hy ? hx : hy;
    a *= 2.0f; if (a > 1.0f) a = 1.0f;
    c.a = (uint8_t)(a * 255.0f);
    cui_draw_shadow(ui, (cui_rect){ cx - hx * 0.12f, cy - hy * 0.12f, hx * 0.24f, hy * 0.24f },
                    CUI_CORNERS(mn * 0.12f), mn * 0.85f, 0, (cui_vec2){ 0, 0 }, c);
}

static void paint_ambient(cui_ctx *ui, cui_rect r, void *user) {
    const ambient_t *a = (const ambient_t *)user;
    cui_draw_rect(ui, r, CUI_CORNERS(0), g_th.bg);
    float vmax = r.w > r.h ? r.w : r.h;
    if (a->strength >= 1.0f) {   /* tela de login (html .auth) */
        blob(ui, r, vmax, 0.12f, 0.18f, 0.46f, 0.36f, B_C1, 0.45f);
        blob(ui, r, vmax, 0.88f, 0.22f, 0.44f, 0.38f, B_C2, 0.40f);
        blob(ui, r, vmax, 0.50f, 1.08f, 0.40f, 0.34f, B_C3, 0.34f);
    } else {                     /* corpo do app (body) */
        /* referência do app original (Electron): magenta no topo esquerdo (barra lateral e canto da conversa), índigo largo no
           topo direito descendo pela direita, roxo subindo da base central, centro/esquerda do meio mais escuro */
        blob(ui, r, vmax, 0.17f, -0.02f, 0.66f, 0.34f, g_th.blob[0], g_th.blob_a[0]);
        blob(ui, r, vmax, 0.95f, 0.10f, 0.60f, 0.52f, g_th.blob[1], g_th.blob_a[1]);
        blob(ui, r, vmax, 0.55f, 1.06f, 0.55f, 0.28f, g_th.blob[2], g_th.blob_a[2]);
    }
    /* BRIKS_GLASS_TEST=1: formas nítidas atrás dos painéis para ver o desfoque funcionando */
    static int test = -1;
    if (test < 0) { const char *e = getenv("BRIKS_GLASS_TEST"); test = e ? e[0] - '0' : 0; }
    if (test == 2) {   /* listras em toda a janela: aparecem atrás de cabeçalho, botões e barra de controles */
        for (int i = 0; i < 64; i++) {
            cui_color c = (i % 3 == 0) ? CUI_RGB(0xff5577) : (i % 3 == 1) ? CUI_RGB(0x55ddff) : CUI_RGB(0xffe066);
            cui_draw_rect(ui, (cui_rect){ r.x + 6 + i * 20, r.y, 8, r.h }, CUI_CORNERS(0), c);
        }
    } else if (test) {
        for (int i = 0; i < 14; i++) {
            cui_color c = (i % 3 == 0) ? CUI_RGB(0xff5577) : (i % 3 == 1) ? CUI_RGB(0x55ddff) : CUI_RGB(0xffe066);
            cui_draw_rect(ui, (cui_rect){ r.x + 330 + i * 46, r.y + 80, 22, r.h - 160 }, CUI_CORNERS(4), c);
        }
        cui_draw_circle(ui, (cui_vec2){ r.x + r.w * 0.5f, r.y + r.h * 0.5f }, 120, CUI_RGB(0xffffff));
    }
}

void briks_ambient(cui_ctx *ui, float strength) {
    ambient_t a = { strength };
    cui_canvas(ui, "ambient", CUI_FILL, CUI_FILL, paint_ambient, &a, sizeof a);
}

static void paint_bar(cui_ctx *ui, cui_rect r, void *user) {
    (void)user;
    /* linear-gradient(90deg, c1, c2 60%, c3): três trechos horizontais por retângulos de 1px de largura seriam caros;
       usamos 3 faixas com gradiente vertical nulo e cores intermediárias. */
    float w1 = r.w * 0.6f;
    int steps = 48;
    for (int i = 0; i < steps; i++) {
        float t = (float)i / (steps - 1);
        cui_color c;
        cui_color a = t < 0.6f ? B_C1 : B_C2, b = t < 0.6f ? B_C2 : B_C3;
        float k = t < 0.6f ? t / 0.6f : (t - 0.6f) / 0.4f;
        c = cui_mix(a, b, k);
        cui_draw_rect(ui, (cui_rect){ r.x + r.w * i / steps, r.y, r.w / steps + 1, r.h }, CUI_CORNERS(0), c);
    }
    (void)w1;
}

void briks_gradient_bar(cui_ctx *ui) {
    cui_canvas(ui, "topbar", CUI_FILL, CUI_PX(3), paint_bar, NULL, 0);
}

void briks_brand(cui_ctx *ui, float size) {
    cui_row(ui, .gap = 9, .align = CUI_ALIGN_CENTER) {
        cui_box(ui, .width = CUI_PX(size + 6), .height = CUI_PX(size + 6), .radius = (size + 6) * 0.3f,
                .bg = B_C2, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
            cui_text(ui, "B", .font = CUI_FONT_DISPLAY, .size = size * 0.8f, .color = CUI_RGB(0xffffff));
        }
        cui_text(ui, "Briks", .font = CUI_FONT_DISPLAY, .size = 19, .color = CUI_RGB(0xe6dcff));
    }
}
