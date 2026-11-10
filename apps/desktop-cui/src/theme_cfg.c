#include "briks_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include <shlobj.h>

briks_theme_t g_th;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static cui_color rgba_of(cui_color c, uint8_t a) { c.a = a; return c; }
static uint8_t a8(float a) { return (uint8_t)(clampf(a, 0, 1) * 255.0f + 0.5f); }
static cui_color scaled(cui_color c, float k, uint8_t a) {
    return (cui_color){ (uint8_t)(c.r * k + 0.5f), (uint8_t)(c.g * k + 0.5f), (uint8_t)(c.b * k + 0.5f), a };
}
static bool same(cui_color a, cui_color b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

void briks_theme_defaults(briks_theme_t *t) {
    memset(t, 0, sizeof *t);
    t->c1 = CUI_RGB(0xb01fcf); t->c2 = CUI_RGB(0x6d5cf6); t->c3 = CUI_RGB(0x8b5cf6);
    t->bg = CUI_RGB(0x0f0316);
    t->blob[0] = CUI_RGB(0xb01fcf); t->blob_a[0] = 0.40f;
    t->blob[1] = CUI_RGB(0x3b34d8); t->blob_a[1] = 0.55f;
    t->blob[2] = CUI_RGB(0x8b5cf6); t->blob_a[2] = 0.42f;
    t->glass_rgb = CUI_RGB(0x13051e);
    t->a_glass = 0.50f; t->a_soft = 0.34f; t->a_strong = 0.74f; t->a_popup = 0.85f;
    t->blur_offset = 1.0f; t->saturation = 1.55f; t->highlight = 0.09f;
    t->bubble_align = 0;
    t->bubble_refract = 12; t->bubble_disp = 0.08f; t->bubble_spec = 0.5f; t->bubble_tint = 0.22f;
    t->sheen = 0.06f; t->sheen_line = 0.10f; t->card_fill = 0.05f; t->card_sat = 1.2f;
    t->panel_radius = 22; t->control_radius = 12; t->chat_pad = 20;
    t->bubble_own = CUI_RGB(0x6d5cf6); t->bubble_big = 16; t->bubble_small = 2;
    briks_theme_validate(t);
}

/* luminância relativa 0..1 (aproximada, sRGB sem linearizar: basta para travar tint claro demais) */
static float lum(cui_color c) { return (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b) / 255.0f; }

void briks_theme_validate(briks_theme_t *t) {
    t->bg.a = 255;
    for (int i = 0; i < 3; i++) { t->blob[i].a = 255; t->blob_a[i] = clampf(t->blob_a[i], 0.0f, 0.8f); }
    /* texto claro sobre o vidro: o tint nunca passa de luminância 0.22, e o vidro nunca fica quase transparente nem opaco demais */
    t->glass_rgb.a = 255;
    while (lum(t->glass_rgb) > 0.22f) { t->glass_rgb.r = (uint8_t)(t->glass_rgb.r * 0.9f); t->glass_rgb.g = (uint8_t)(t->glass_rgb.g * 0.9f); t->glass_rgb.b = (uint8_t)(t->glass_rgb.b * 0.9f); }
    t->a_glass = clampf(t->a_glass, 0.20f, 0.95f); t->a_soft = clampf(t->a_soft, 0.15f, 0.95f);
    t->a_strong = clampf(t->a_strong, 0.30f, 0.97f); t->a_popup = clampf(t->a_popup, 0.50f, 0.98f);
    t->blur_offset = clampf(t->blur_offset, 0.5f, 4.0f);
    t->saturation = clampf(t->saturation, 0.5f, 2.5f);
    t->highlight = clampf(t->highlight, 0.0f, 0.2f);
    t->sheen = clampf(t->sheen, 0.0f, 0.3f); t->sheen_line = clampf(t->sheen_line, 0.0f, 0.4f);
    t->bubble_refract = clampf(t->bubble_refract, 0.0f, 30.0f); t->bubble_disp = clampf(t->bubble_disp, 0.0f, 0.6f);
    t->bubble_spec = clampf(t->bubble_spec, 0.0f, 1.0f); t->bubble_tint = clampf(t->bubble_tint, 0.0f, 0.3f);
    t->card_fill = clampf(t->card_fill, 0.0f, 0.2f); t->card_sat = clampf(t->card_sat, 1.0f, 1.6f);
    t->panel_radius = clampf(t->panel_radius, 8.0f, 32.0f);       /* >= 8: o cabeçalho usa raio-1, sempre arredondado */
    t->control_radius = clampf(t->control_radius, 4.0f, 20.0f);
    t->chat_pad = clampf(t->chat_pad, 8.0f, 40.0f);
    t->bubble_own.a = 0xbd; t->c1.a = t->c2.a = t->c3.a = 255;
    t->bubble_big = clampf(t->bubble_big, 2.0f, 28.0f);
    t->bubble_small = clampf(t->bubble_small, 2.0f, 12.0f);
    if (t->bubble_small > t->bubble_big) t->bubble_small = t->bubble_big;
    /* derivados; o vidro é achado pelo RGBA exato (glass.c), então as quatro cores precisam ser distintas entre as camadas 0 e 1 */
    t->glass = rgba_of(t->glass_rgb, a8(t->a_glass));
    t->glass_soft = rgba_of(t->glass_rgb, a8(t->a_soft));
    t->glass_strong = scaled(t->glass_rgb, 0.72f, a8(t->a_strong));
    t->glass_popup = scaled(t->glass_rgb, 0.70f, a8(t->a_popup));
    t->glass_card = t->glass_soft; t->glass_card.a = (uint8_t)(t->glass_soft.a < 250 ? t->glass_soft.a + 1 : t->glass_soft.a - 1);
    while (same(t->glass_card, t->glass) || same(t->glass_card, t->glass_strong) || same(t->glass_card, t->glass_popup)) t->glass_card.a++;
    t->glass_bubble = t->glass_soft; t->glass_bubble.a = 16;   /* a cor do estilo vem do shader; este alfa só identifica a bolha (glass.c) */
    while (same(t->glass_bubble, t->glass) || same(t->glass_bubble, t->glass_soft) || same(t->glass_bubble, t->glass_card) || same(t->glass_bubble, t->glass_strong) || same(t->glass_bubble, t->glass_popup)) t->glass_bubble.a++;
    while (same(t->glass_popup, t->glass) || same(t->glass_popup, t->glass_soft) || same(t->glass_popup, t->glass_strong)) t->glass_popup.a++;
}

/* ---------- JSON plano: {"chave": numero | "#rrggbb"} ---------- */
static const char *find_val(const char *js, const char *key) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}
static void get_f(const char *js, const char *k, float *out) { const char *v = find_val(js, k); if (v && (*v == '-' || *v == '.' || (*v >= '0' && *v <= '9'))) *out = (float)atof(v); }
static void get_c(const char *js, const char *k, cui_color *out) {
    const char *v = find_val(js, k);
    if (!v || *v != '"' || v[1] != '#') return;
    unsigned x = 0; int n = 0;
    for (const char *h = v + 2; n < 6 && ((*h >= '0' && *h <= '9') || (*h >= 'a' && *h <= 'f') || (*h >= 'A' && *h <= 'F')); h++, n++)
        x = x * 16 + (unsigned)(*h <= '9' ? *h - '0' : (*h | 32) - 'a' + 10);
    if (n == 6) *out = CUI_RGB(x);
}

bool briks_theme_load(const char *path, briks_theme_t *t) {
    briks_theme_defaults(t);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    static char js[8192]; size_t n = fread(js, 1, sizeof js - 1, f); js[n] = 0; fclose(f);
    get_c(js, "c1", &t->c1); get_c(js, "c2", &t->c2); get_c(js, "c3", &t->c3);
    get_c(js, "bg", &t->bg);
    get_c(js, "blob1", &t->blob[0]); get_c(js, "blob2", &t->blob[1]); get_c(js, "blob3", &t->blob[2]);
    get_f(js, "blob1_a", &t->blob_a[0]); get_f(js, "blob2_a", &t->blob_a[1]); get_f(js, "blob3_a", &t->blob_a[2]);
    get_c(js, "glass_rgb", &t->glass_rgb);
    get_f(js, "glass_a", &t->a_glass); get_f(js, "soft_a", &t->a_soft); get_f(js, "strong_a", &t->a_strong); get_f(js, "popup_a", &t->a_popup);
    get_f(js, "blur_offset", &t->blur_offset); get_f(js, "saturation", &t->saturation); get_f(js, "highlight", &t->highlight);
    const bool v2 = find_val(js, "recipe") != NULL;   /* receita de vidro v2 (violeta + rim light); arquivos antigos não sobrepõem esses padrões */
    if (v2) { get_f(js, "sheen", &t->sheen); get_f(js, "sheen_line", &t->sheen_line); } get_f(js, "card_fill", &t->card_fill); get_f(js, "card_sat", &t->card_sat);
    if (v2) { get_f(js, "bubble_refract", &t->bubble_refract); get_f(js, "bubble_disp", &t->bubble_disp); get_f(js, "bubble_spec", &t->bubble_spec); get_f(js, "bubble_tint", &t->bubble_tint); }
    get_f(js, "panel_radius", &t->panel_radius); get_f(js, "control_radius", &t->control_radius); get_f(js, "chat_pad", &t->chat_pad);
    get_c(js, "bubble_own", &t->bubble_own); get_f(js, "bubble_big", &t->bubble_big); get_f(js, "bubble_small", &t->bubble_small);
    { const char *v = find_val(js, "bubble_align"); if (v && *v == '"') t->bubble_align = (v[1] == 'm' || v[1] == 'M') ? 1 : 0; }
    briks_theme_validate(t);
    return true;
}

bool briks_theme_save(const char *path, const briks_theme_t *t) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
#define HEX(c) (c).r, (c).g, (c).b
    fprintf(f, "{\n"
        "  \"recipe\": 2,\n"
        "  \"_receita_vidro\": \"v2 violeta: tinta nos baloes (bubble_tint), rim light fino e continuo na borda (bubble_spec, sheen_line), sem risco especular\",\n"
        "  \"_sheen\": \"reflexo difuso dos cartoes, baixo contraste\", \"_bubble_refract\": \"lente do balao de status; bubble_disp = separacao de cor, minima\",\n"
        "  \"_c1_c2_c3\": \"acento derivado do gradiente (aba Temas); c3 legivel sobre o fundo\",\n"
        "  \"c1\": \"#%02x%02x%02x\", \"c2\": \"#%02x%02x%02x\", \"c3\": \"#%02x%02x%02x\",\n"
        "  \"bg\": \"#%02x%02x%02x\",\n"
        "  \"blob1\": \"#%02x%02x%02x\", \"blob1_a\": %.2f,\n"
        "  \"blob2\": \"#%02x%02x%02x\", \"blob2_a\": %.2f,\n"
        "  \"blob3\": \"#%02x%02x%02x\", \"blob3_a\": %.2f,\n"
        "  \"glass_rgb\": \"#%02x%02x%02x\",\n"
        "  \"glass_a\": %.2f, \"soft_a\": %.2f, \"strong_a\": %.2f, \"popup_a\": %.2f,\n"
        "  \"blur_offset\": %.2f, \"saturation\": %.2f, \"highlight\": %.3f,\n"
        "  \"sheen\": %.3f, \"sheen_line\": %.3f, \"card_fill\": %.3f, \"card_sat\": %.2f,\n"
        "  \"panel_radius\": %.1f, \"control_radius\": %.1f, \"chat_pad\": %.1f,\n"
        "  \"bubble_own\": \"#%02x%02x%02x\", \"bubble_big\": %.1f, \"bubble_small\": %.1f,\n"
        "  \"bubble_align\": \"%s\",\n"
        "  \"bubble_refract\": %.1f, \"bubble_disp\": %.2f, \"bubble_spec\": %.2f, \"bubble_tint\": %.2f\n"
        "}\n",
        HEX(t->c1), HEX(t->c2), HEX(t->c3), HEX(t->bg), HEX(t->blob[0]), t->blob_a[0], HEX(t->blob[1]), t->blob_a[1], HEX(t->blob[2]), t->blob_a[2],
        HEX(t->glass_rgb), t->a_glass, t->a_soft, t->a_strong, t->a_popup,
        t->blur_offset, t->saturation, t->highlight, t->sheen, t->sheen_line, t->card_fill, t->card_sat, t->panel_radius, t->control_radius, t->chat_pad,
        HEX(t->bubble_own), t->bubble_big, t->bubble_small, t->bubble_align ? "middle" : "top", t->bubble_refract, t->bubble_disp, t->bubble_spec, t->bubble_tint);
#undef HEX
    fclose(f);
    return true;
}

static bool ed_open; static briks_theme_t ed, g_pending; static bool g_has_pending;
static char g_path[MAX_PATH];
const char *briks_theme_path(void) {
    if (g_path[0]) return g_path;
    const char *o = getenv("BRIKS_THEME_FILE");
    if (o && o[0]) { snprintf(g_path, sizeof g_path, "%s", o); return g_path; }
    char appdata[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) == S_OK) {
        char dir[MAX_PATH]; snprintf(dir, sizeof dir, "%s\\Briks-CUI", appdata);
        CreateDirectoryA(dir, NULL);
        snprintf(g_path, sizeof g_path, "%s\\theme.json", dir);
    }
    return g_path;
}

static FILETIME g_mt; static bool g_have_mt;
static bool stat_mt(FILETIME *out) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(briks_theme_path(), GetFileExInfoStandard, &d)) return false;
    *out = d.ftLastWriteTime; return true;
}

/* Carrega na partida e depois só confere a data do arquivo (uma chamada leve por 0,4 s); sem arquivo, usa os padrões. */
bool briks_theme_poll(void) {
    static bool init; static ULONGLONG last;
    if (g_has_pending) { g_th = g_pending; g_has_pending = false; return true; }
    ULONGLONG now = GetTickCount64();
    if (!init) {
        init = true; last = now;
        FILETIME m;
        if (stat_mt(&m) && briks_theme_load(briks_theme_path(), &g_th)) { g_mt = m; g_have_mt = true; }
        else briks_theme_defaults(&g_th);
        return true;
    }
    if (now - last < 400) return false;
    last = now;
    FILETIME m;
    if (!stat_mt(&m)) return false;
    if (g_have_mt && m.dwLowDateTime == g_mt.dwLowDateTime && m.dwHighDateTime == g_mt.dwHighDateTime) return false;
    briks_theme_t t;
    if (!briks_theme_load(briks_theme_path(), &t)) return false;
    g_mt = m; g_have_mt = true; g_th = t;
    return true;
}

/* ---------- editor (Ctrl+Shift+T, só com BRIKS_DEV=1) ---------- */
static bool ed_open;
static float ed_msg_t = -10; static const char *ed_msg = "";

static bool fld(cui_ctx *ui, const char *label, float *v, float lo, float hi) {
    char id[64]; snprintf(id, sizeof id, "%s", label);
    return cui_slider(ui, id, v, lo, hi);
}
static bool col(cui_ctx *ui, const char *label, cui_color *c) {
    bool ch = false;
    cui_row(ui, .gap = 10, .align = CUI_ALIGN_CENTER) {
        char id[64]; snprintf(id, sizeof id, "col-%s", label);
        ch = cui_color_picker(ui, id, c);
        cui_text(ui, label, .size = 12);
    }
    return ch;
}

void briks_theme_editor_open(void) { ed_open = true; ed = g_th; }
void briks_theme_reset(void) { briks_theme_t d; briks_theme_defaults(&d); briks_theme_validate(&d); g_th = d; briks_theme_save(briks_theme_path(), &g_th); }

void briks_theme_editor(cui_ctx *ui) {
    static int dev = -1;
    if (dev < 0) { const char *e = getenv("BRIKS_DEV"); dev = e && e[0] == '1'; }
    if (dev) {
    { static bool once; if (!once) { once = true; const char *o = getenv("BRIKS_DEV_OPEN"); if (o && !strcmp(o, "theme")) { ed_open = true; ed = g_th; } } }
    if (cui_shortcut(ui, CUI_MOD_CTRL | CUI_MOD_SHIFT, CUI_KEY_T)) { ed_open = !ed_open; if (ed_open) ed = g_th; }
    }
    if (!ed_open) return;
    briks_theme_t before = g_th;
    cui_sheet(ui, "Tema (desenvolvimento)", &ed_open, .side = CUI_SIDE_RIGHT, .size = 380) {
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .scroll_y = true, .gap = 8) {
            cui_text(ui, "Fundo", .font = CUI_FONT_MEDIUM);
            col(ui, "base", &ed.bg);
            col(ui, "mancha topo esquerdo", &ed.blob[0]); fld(ui, "opacidade 1", &ed.blob_a[0], 0, 0.8f);
            col(ui, "mancha topo direito", &ed.blob[1]); fld(ui, "opacidade 2", &ed.blob_a[1], 0, 0.8f);
            col(ui, "mancha base", &ed.blob[2]); fld(ui, "opacidade 3", &ed.blob_a[2], 0, 0.8f);
            cui_text(ui, "Vidro", .font = CUI_FONT_MEDIUM);
            col(ui, "cor do tint", &ed.glass_rgb);
            fld(ui, "tint painéis", &ed.a_glass, 0.2f, 0.95f); fld(ui, "tint suave", &ed.a_soft, 0.15f, 0.95f);
            fld(ui, "tint forte", &ed.a_strong, 0.3f, 0.97f); fld(ui, "tint diálogos", &ed.a_popup, 0.5f, 0.98f);
            fld(ui, "desfoque", &ed.blur_offset, 0.5f, 4.0f); fld(ui, "saturação", &ed.saturation, 0.5f, 2.5f);
            fld(ui, "luz de borda", &ed.highlight, 0.0f, 0.2f);
            fld(ui, "reflexo (cartões)", &ed.sheen, 0.0f, 0.3f); fld(ui, "fio de luz", &ed.sheen_line, 0.0f, 0.4f);
            fld(ui, "tinta (cartões)", &ed.card_fill, 0.0f, 0.2f); fld(ui, "saturação extra", &ed.card_sat, 1.0f, 1.6f);
            fld(ui, "bolha: refração", &ed.bubble_refract, 0.0f, 30.0f); fld(ui, "bolha: dispersão", &ed.bubble_disp, 0.0f, 0.6f);
            fld(ui, "bolha: especular", &ed.bubble_spec, 0.0f, 1.0f); fld(ui, "bolha: tinta", &ed.bubble_tint, 0.0f, 0.3f);
            cui_text(ui, "Painéis e controles", .font = CUI_FONT_MEDIUM);
            fld(ui, "raio dos painéis", &ed.panel_radius, 8, 32); fld(ui, "raio dos controles", &ed.control_radius, 4, 20);
            fld(ui, "espaço da lista de mensagens", &ed.chat_pad, 8, 40);
            cui_text(ui, "Balões", .font = CUI_FONT_MEDIUM);
            col(ui, "balão enviado", &ed.bubble_own);
            fld(ui, "raio do balão", &ed.bubble_big, 2, 28); fld(ui, "raio da quina reta", &ed.bubble_small, 2, 12);
            cui_row(ui, .gap = 8) {
                if (cui_button(ui, "Salvar", .variant = CUI_BUTTON_PRIMARY)) {
                    briks_theme_validate(&ed);
                    ed_msg = briks_theme_save(briks_theme_path(), &ed) ? "Salvo em theme.json" : "Não foi possível salvar";
                    ed_msg_t = (float)cui_time(ui);
                    FILETIME m; if (stat_mt(&m)) { g_mt = m; g_have_mt = true; }
                }
                if (cui_button(ui, "Restaurar padrão", .variant = CUI_BUTTON_SECONDARY)) { briks_theme_defaults(&ed); ed_msg = "Padrão restaurado (use Salvar para gravar)"; ed_msg_t = (float)cui_time(ui); }
            }
            if ((float)cui_time(ui) - ed_msg_t < 3.0f) cui_text(ui, ed_msg, .size = 12, .color = B_ONLINE);
        }
    }
    if (memcmp(&before, &ed, sizeof g_th) != 0) { briks_theme_validate(&ed); briks_apply_theme(ui); }
}
