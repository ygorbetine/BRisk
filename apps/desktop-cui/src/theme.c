#include "briks_ui.h"
#include <math.h>
#include <stdlib.h>

static cui_color with_a(cui_color c, uint8_t a) { c.a = a; return c; }

/* HSL <-> RGB e contraste WCAG: as cores de campo, texto secundario e divisor saem do matiz do acento, sem valor fixo por tema */
static void rgb2hsl(cui_color c, float *h, float *s, float *l) {
    float r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f, mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    *l = (mx + mn) * 0.5f; *s = d < 1e-4f ? 0 : d / (1 - fabsf(2 * *l - 1));
    float hh = 0; if (d > 1e-4f) { if (mx == r) hh = fmodf((g - b) / d, 6.0f); else if (mx == g) hh = (b - r) / d + 2; else hh = (r - g) / d + 4; }
    *h = hh * 60.0f; if (*h < 0) *h += 360.0f;
}
static cui_color hsl2rgb(float h, float s, float l, uint8_t a) {
    float c = (1 - fabsf(2 * l - 1)) * s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2.0f) - 1)), m = l - c / 2, r = 0, g = 0, b = 0;
    int k = (int)(h / 60.0f) % 6;
    switch (k) { case 0: r = c; g = x; break; case 1: r = x; g = c; break; case 2: g = c; b = x; break; case 3: g = x; b = c; break; case 4: r = x; b = c; break; default: r = c; b = x; }
    cui_color o = { (uint8_t)((r + m) * 255.0f + 0.5f), (uint8_t)((g + m) * 255.0f + 0.5f), (uint8_t)((b + m) * 255.0f + 0.5f), a }; return o;
}
static float rlum(cui_color c) { float r = powf(c.r / 255.0f, 2.2f), g = powf(c.g / 255.0f, 2.2f), b = powf(c.b / 255.0f, 2.2f); return 0.2126f * r + 0.7152f * g + 0.0722f * b; }
static float contrast(cui_color a, cui_color b) { float x = rlum(a) + 0.05f, y = rlum(b) + 0.05f; return x > y ? x / y : y / x; }

static void derive_field_colors(void) {
    float h, s, l; rgb2hsl(g_th.c3, &h, &s, &l);
    if (s < 0.15f) rgb2hsl(g_th.c1, &h, &s, &l);   /* acento quase cinza: usa o outro */
    float fs = fminf(fmaxf(s, 0.35f), 0.65f);
    float bgl, t1, t2; rgb2hsl(g_th.bg, &t1, &t2, &bgl);
    float fl = fminf(0.085f, bgl * 0.7f);                 /* sempre mais escuro que o fundo ao redor */
    cui_color fld = hsl2rgb(h, fs, fl, 0xc8);
    g_th.field = fld;
    cui_color under = cui_mix(g_th.bg, fld, 0.78f);       /* aproximacao do campo sobre o vidro */
    g_th.line = hsl2rgb(h, fminf(fs, 0.45f), fminf(0.22f, fl + 0.12f), 255);
    /* secundario e placeholder: claros no matiz do tema; sobe a luminosidade ate 4.5:1 sobre o campo e sobre o fundo */
    float ml = 0.70f; cui_color m = hsl2rgb(h, 0.28f, ml, 255);
    for (int k = 0; k < 20 && (contrast(m, under) < 4.5f || contrast(m, g_th.bg) < 4.5f); k++) { ml += 0.02f; m = hsl2rgb(h, 0.28f, fminf(ml, 0.95f), 255); }
    g_th.muted = m;
    float pl = 0.58f; cui_color p = hsl2rgb(h, 0.22f, pl, 255);
    for (int k = 0; k < 25 && contrast(p, under) < 4.5f; k++) { pl += 0.02f; p = hsl2rgb(h, 0.22f, fminf(pl, 0.95f), 255); }
    g_th.ph = p;
}

void briks_apply_theme(cui_ctx *ui) {
    cui_set_theme(ui, CUI_THEME_DARK);
    cui_set_accent(ui, CUI_ACCENT_VIOLET);
    derive_field_colors();
    cui_theme *t = cui_theme_get(ui);
    t->colors[CUI_TOKEN_BACKGROUND]     = B_BG;
    t->colors[CUI_TOKEN_SURFACE]        = B_PANEL;
    t->colors[CUI_TOKEN_SURFACE_RAISED] = B_GLASS_POPUP;   /* diálogos e popovers: vidro na camada 1 (glass.c) */
    t->colors[CUI_TOKEN_SURFACE_MUTED]  = B_LINE;
    t->colors[CUI_TOKEN_FOREGROUND]     = B_TEXT;
    t->colors[CUI_TOKEN_TEXT_SECONDARY] = B_MUTED;
    t->colors[CUI_TOKEN_TEXT_MUTED]     = g_th.ph;
    t->colors[CUI_TOKEN_BORDER]         = B_LINE;
    t->colors[CUI_TOKEN_BORDER_SUBTLE]  = CUI_RGBA(0xffffff12);
    t->colors[CUI_TOKEN_BORDER_STRONG]  = with_a(cui_mix(B_C3, CUI_RGB(0xffffff), 0.15f), 0x90);   /* borda dos campos: cor do tema, mais clara que o fundo do campo */
    t->colors[CUI_TOKEN_ACCENT]         = B_C3;
    t->colors[CUI_TOKEN_ACCENT_STRONG]  = B_C2;
    t->colors[CUI_TOKEN_ACCENT_SUBTLE]  = with_a(B_C3, 0x22);
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
