/* Temas por gradiente (aba Temas das Configurações).
 * Um tema é um gradiente de 2 a 4 paradas (linear ou radial, com ângulo). apply() deriva dele as cores do fundo (manchas, base), do vidro,
 * do acento (c1..c3) e do balão enviado, preservando o resto do tema (raios, desfoque, reflexos): o editor avançado (Ctrl+Shift+T) parte do
 * que o gradiente gerou. Legibilidade: a opacidade das manchas é limitada pela luminância da cor, então texto claro fica >= ~5:1.
 * Persistência: o tema derivado vai para theme.json (hot reload existente); a lista "Meus gradientes" e o tema ativo, para
 * %APPDATA%\Briks-CUI\theme-gradients.json (BRIKS_GRADIENTS_FILE troca o caminho). No modo demo nada é gravado. */
#include "briks_ui.h"
#include "json.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <shlobj.h>

typedef struct { char id[16], name[40]; int radial; float angle; int n; cui_gradient_stop st[4]; } grad_t;

#define HEX(v) CUI_RGB(v)
static grad_t PRESETS[5];   /* preenchido em init_presets(): CUI_RGB não é constante em C */
static void init_presets(void) {
    static bool done; if (done) return; done = true;
    static const struct { const char *id, *name; float angle; unsigned c[3]; } D[5] = {
        { "violeta", "Violeta noite", 135, { 0xb01fcf, 0x6d5cf6, 0x8b5cf6 } }, { "aurora", "Aurora", 135, { 0x12c9a0, 0x3b82f6, 0x9b6cf6 } },
        { "por-do-sol", "Pôr do sol", 160, { 0xff5e7e, 0xff8a3d, 0xb04fd8 } }, { "oceano", "Oceano", 135, { 0x0ea5c6, 0x2f6bff, 0x5aa9ff } },
        { "floresta", "Floresta", 135, { 0x1fbf75, 0x0e9f9a, 0x6fdc8c } } };
    for (int i = 0; i < 5; i++) {
        grad_t *g = &PRESETS[i]; snprintf(g->id, sizeof g->id, "%s", D[i].id); snprintf(g->name, sizeof g->name, "%s", D[i].name);
        g->angle = D[i].angle; g->n = 3; for (int k = 0; k < 3; k++) { g->st[k].pos = k == 0 ? 0.0f : k == 1 ? 0.5f : 1.0f; g->st[k].color = CUI_RGB(D[i].c[k]); }
    }
}
#define NPRE 5
#define MAXMY 16
static grad_t mine[MAXMY]; static int mine_n;
static char active[16] = "violeta";
static grad_t ed; static char ed_id[16]; static int ed_sel; static bool ed_init;
static char note[160]; static float note_t = -10;

static float lum(cui_color c) { float r = powf(c.r / 255.0f, 2.2f), g = powf(c.g / 255.0f, 2.2f), b = powf(c.b / 255.0f, 2.2f); return 0.2126f * r + 0.7152f * g + 0.0722f * b; }
static cui_color scale(cui_color c, float k) { cui_color o = c; o.r = (uint8_t)(c.r * k); o.g = (uint8_t)(c.g * k); o.b = (uint8_t)(c.b * k); o.a = 255; return o; }
static cui_color eval(const grad_t *g, float t) { if (t < 0) t = 0; if (t > 1) t = 1; return cui_gradient_eval(g->st, g->n, t); }
static float tpos(const grad_t *g, float x, float y) {   /* posição (0..1) do ponto no gradiente */
    if (g->radial) { float dx = x - 0.5f, dy = y - 0.35f; return sqrtf(dx * dx + dy * dy) / 0.75f; }
    float a = g->angle * 3.14159265f / 180.0f; return 0.5f + (x - 0.5f) * cosf(a) + (y - 0.5f) * sinf(a);
}

/* deriva as cores do tema a partir do gradiente; devolve o pior contraste do texto (claro) sobre o fundo e liga *capped se escureceu */
static float derive(const grad_t *g, briks_theme_t *t, bool *capped) {
    briks_theme_t d; briks_theme_defaults(&d);
    *capped = false;
    if (!strcmp(g->id, "violeta")) {
        t->bg = d.bg; for (int i = 0; i < 3; i++) { t->blob[i] = d.blob[i]; t->blob_a[i] = d.blob_a[i]; }
        t->glass_rgb = d.glass_rgb; t->bubble_own = d.bubble_own; t->c1 = d.c1; t->c2 = d.c2; t->c3 = d.c3;
    } else {
        static const float P[3][2] = { { 0.17f, 0.0f }, { 0.95f, 0.10f }, { 0.55f, 1.06f } };
        cui_color mid = eval(g, 0.5f);
        t->bg = scale(mid, 0.075f); t->glass_rgb = scale(mid, 0.09f);
        for (int i = 0; i < 3; i++) {
            cui_color c = eval(g, tpos(g, P[i][0], P[i][1])); t->blob[i] = c;
            float a = d.blob_a[i], cap = 0.13f / fmaxf(lum(c), 0.001f);
            if (cap < a) { a = cap; *capped = true; }
            t->blob_a[i] = a;
        }
        cui_color c1 = eval(g, 0), c2 = mid, c3 = eval(g, 1);
        for (int k = 0; k < 8 && lum(c1) < 0.10f; k++) c1 = cui_mix(c1, CUI_RGB(0xffffff), 0.2f);
        for (int k = 0; k < 8 && lum(c3) < 0.30f; k++) c3 = cui_mix(c3, CUI_RGB(0xffffff), 0.2f);
        for (int k = 0; k < 8 && lum(c2) > 0.25f; k++) c2 = scale(c2, 0.85f);
        t->c1 = c1; t->c2 = c2; t->c3 = c3; t->bubble_own = c2;
    }
    float worst = 0;
    for (int i = 0; i < 3; i++) { float l = t->blob_a[i] * lum(t->blob[i]) + (1 - t->blob_a[i]) * lum(t->bg); if (l > worst) worst = l; }
    return (0.88f + 0.05f) / (worst + 0.05f);
}

/* ---- persistência ---- */
static bool demo(void) { return getenv("BRIKS_DEMO") != NULL; }
static bool store_path(char *out, size_t cap) {
    const char *o = getenv("BRIKS_GRADIENTS_FILE"); if (o && o[0]) { snprintf(out, cap, "%s", o); return true; }
    char ad[MAX_PATH]; if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, ad) != S_OK) return false;
    char dir[MAX_PATH]; snprintf(dir, sizeof dir, "%s\\Briks-CUI", ad); CreateDirectoryA(dir, NULL);
    snprintf(out, cap, "%s\\theme-gradients.json", dir); return true;
}
static int write_json(char *buf, size_t cap) {
    int n = 0; char esc[96];
    n += snprintf(buf + n, cap - n, "{\"active\":\"%s\",\"items\":[", active);
    for (int i = 0; i < mine_n; i++) {
        const grad_t *g = &mine[i]; json_escape(g->name, esc, sizeof esc);
        n += snprintf(buf + n, cap - n, "%s{\"id\":\"%s\",\"name\":\"%s\",\"type\":%d,\"angle\":%.0f,\"stops\":[", i ? "," : "", g->id, esc, g->radial, g->angle);
        for (int k = 0; k < g->n; k++) n += snprintf(buf + n, cap - n, "%s{\"p\":%.3f,\"c\":\"#%02x%02x%02x\"}", k ? "," : "", g->st[k].pos, g->st[k].color.r, g->st[k].color.g, g->st[k].color.b);
        n += snprintf(buf + n, cap - n, "]}");
    }
    n += snprintf(buf + n, cap - n, "]}\n"); return n;
}
static void save_store(void) {
    if (demo()) return;
    char p[MAX_PATH]; if (!store_path(p, sizeof p)) return;
    static char buf[16384]; int n = write_json(buf, sizeof buf);
    FILE *f = fopen(p, "wb"); if (f) { fwrite(buf, 1, (size_t)n, f); fclose(f); }
}
static int parse_items(const char *text, bool replace_active) {
    jval *j = json_parse(text); if (!j) return -1;
    if (replace_active) snprintf(active, sizeof active, "%s", json_str(j, "active", "violeta"));
    const jval *it = json_get(j, "items"); int added = 0;
    for (int i = 0; it && i < json_len(it) && mine_n < MAXMY; i++) {
        const jval *o = json_at(it, i), *st = json_get(o, "stops"), *ty = json_get(o, "type"), *an = json_get(o, "angle");
        if (!st || json_len(st) < 2) continue;
        grad_t g; memset(&g, 0, sizeof g);
        snprintf(g.id, sizeof g.id, "%s", json_str(o, "id", "")); snprintf(g.name, sizeof g.name, "%s", json_str(o, "name", "Sem nome"));
        g.radial = ty && ty->num >= 1; g.angle = an ? (float)an->num : 135; g.n = json_len(st) > 4 ? 4 : json_len(st);
        for (int k = 0; k < g.n; k++) {
            const jval *s = json_at(st, k), *p = json_get(s, "p"); unsigned v = 0;
            sscanf(json_str(s, "c", "#000000") + 1, "%x", &v);
            g.st[k].pos = p ? (float)p->num : (float)k / (g.n - 1); g.st[k].color = CUI_RGB(v);
        }
        mine[mine_n++] = g; added++;
    }
    json_free(j); return added;
}
static void new_id(char *out) {
    for (int k = 1;; k++) { char t[16]; snprintf(t, sizeof t, "u%03d", k); bool used = false; for (int i = 0; i < mine_n; i++) if (!strcmp(mine[i].id, t)) used = true; if (!used) { strcpy(out, t); return; } }
}
static void load_store(void) {
    char p[MAX_PATH]; if (demo() || !store_path(p, sizeof p)) return;
    FILE *f = fopen(p, "rb"); if (!f) return;
    static char buf[16384]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    parse_items(buf, true);
}
static const grad_t *find_grad(const char *id) {
    for (int i = 0; i < NPRE; i++) if (!strcmp(PRESETS[i].id, id)) return &PRESETS[i];
    for (int i = 0; i < mine_n; i++) if (!strcmp(mine[i].id, id)) return &mine[i];
    return NULL;
}

static void apply(cui_ctx *ui, const grad_t *g) {
    init_presets();
    briks_theme_t t = g_th; bool capped; derive(g, &t, &capped);
    briks_theme_validate(&t); g_th = t;
    if (!demo()) briks_theme_save(briks_theme_path(), &g_th);
    snprintf(active, sizeof active, "%s", g->id); save_store();
    briks_apply_theme(ui); cui_request_redraw(ui);
}

void themes_tick(cui_ctx *ui) {
    init_presets();
    static bool once;
    if (once) return; once = true;
    load_store();
    const char *e = getenv("BRIKS_THEME_PRESET");   /* captura: aplica um tema pronto (0..4) ou um personalizado de demonstração (5) */
    if (e) {
        int n = atoi(e);
        if (n >= 0 && n < NPRE) apply(ui, &PRESETS[n]);
        else { grad_t g; memset(&g, 0, sizeof g); snprintf(g.id, sizeof g.id, "demo"); snprintf(g.name, sizeof g.name, "Meu gradiente"); g.angle = 60; g.n = 3; g.st[0].pos = 0; g.st[0].color = HEX(0xff3d9a); g.st[1].pos = 0.5f; g.st[1].color = HEX(0xffd23d); g.st[2].pos = 1; g.st[2].color = HEX(0x3dffb4); apply(ui, &g); }   /* claro de propósito: mostra o aviso e o escurecimento */
    }
}

/* ---- prévia ---- */
typedef struct { grad_t g; } prev_t;
static void paint_prev(cui_ctx *ui, cui_rect r, void *user) {
    const grad_t *g = &((const prev_t *)user)->g;
    briks_theme_t t = g_th; bool capped; derive(g, &t, &capped);
    cui_draw_rect(ui, r, CUI_CORNERS(0), t.bg);
    int cols = g_perf == PERF_NORMAL ? 30 : 10, rows = (int)(cols * r.h / r.w + 0.5f); if (rows < 3) rows = 3;
    float cw = r.w / cols, ch = r.h / rows;
    for (int y = 0; y < rows; y++) for (int x = 0; x < cols; x++) {
        float tt = tpos(g, (x + 0.5f) / cols, (y + 0.5f) / rows);
        cui_color c = cui_mix(t.bg, eval(g, tt), 0.50f);
        cui_draw_rect(ui, (cui_rect){ r.x + x * cw, r.y + y * ch, cw + 0.6f, ch + 0.6f }, CUI_CORNERS(0), c);
    }
}
static void preview(cui_ctx *ui, const grad_t *g, float w, float h, bool sel, const char *id) {
    prev_t p = { *g };
    cui_box(ui, .id = id, .width = w > 0 ? CUI_PX(w) : CUI_FILL, .height = CUI_PX(h), .radius = 12, .clip = true, .border = sel ? 2 : 1,
            .border_color = sel ? B_C3 : B_GLASS_BORDER) {
        cui_canvas(ui, "pv", CUI_FILL, CUI_FILL, paint_prev, &p, sizeof p);
    }
}

/* ---- área de transferência ---- */
static void clip_set(const char *text) {
    if (!OpenClipboard(NULL)) return; EmptyClipboard();
    size_t n = strlen(text) + 1; HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
    if (h) { memcpy(GlobalLock(h), text, n); GlobalUnlock(h); SetClipboardData(CF_TEXT, h); }
    CloseClipboard();
}
static bool clip_get(char *out, size_t cap) {
    if (!OpenClipboard(NULL)) return false; bool ok = false; HANDLE h = GetClipboardData(CF_TEXT);
    if (h) { const char *s = (const char *)GlobalLock(h); if (s) { snprintf(out, cap, "%s", s); ok = true; GlobalUnlock(h); } }
    CloseClipboard(); return ok;
}

static void say(cui_ctx *ui, const char *t) { snprintf(note, sizeof note, "%s", t); note_t = (float)cui_time(ui); }

#define TCARD(ui, title) CUI__SCOPE((cui_box_begin((ui), CUI__OPTS(cui_box, .width = CUI_FILL, .pad = 14, .gap = 10, .radius = 16, .bg = B_GLASS_CARD, \
    .border = 1, .border_color = B_GLASS_BORDER)), cui_text((ui), (title), .font = CUI_FONT_MEDIUM, .size = 13, .color = B_C3)), cui_box_end(ui))

void themes_tab(cui_ctx *ui) {
    init_presets();
    if (!ed_init) { ed_init = true; ed = PRESETS[0]; snprintf(ed.id, sizeof ed.id, "%s", "edit"); snprintf(ed.name, sizeof ed.name, "Meu gradiente"); ed.n = 3; }
    static const char *TYPES[] = { "Linear", "Radial" };
    const bool fresh = (float)cui_time(ui) - note_t < 4.0f;

    TCARD(ui, "TEMAS PRONTOS") {
        cui_text(ui, "Troca na hora o fundo, o acento e as cores derivadas; o vidro e a legibilidade são mantidos.", .size = 12, .color = B_MUTED);
        cui_row(ui, .gap = 10, .width = CUI_FILL, .wrap = true) {
            for (int i = 0; i < NPRE; i++) {
                cui_push_id_int(ui, 6000 + i);
                bool sel = !strcmp(active, PRESETS[i].id);
                cui_box(ui, .id = "card", .width = CUI_PX(150), .gap = 6, .pad = 6, .radius = 14, .bg = CUI_RGBA(0xffffff08)) {
                    preview(ui, &PRESETS[i], 136, 64, sel, "pv");
                    cui_row(ui, .gap = 6, .align = CUI_ALIGN_CENTER) {
                        cui_text(ui, PRESETS[i].name, .size = 13, .nowrap = true, .width = CUI_FILL);
                        if (sel) cui_icon(ui, CUI_ICON_CHECK, 14, B_ONLINE);
                    }
                }
                if (briks_clicked_r(ui, "card", 14.0f)) apply(ui, &PRESETS[i]);
                cui_pop_id(ui);
            }
        }
    }

    TCARD(ui, "CRIAR O SEU") {
        cui_input(ui, "th-name", ed.name, sizeof ed.name, .placeholder = "Nome do gradiente");
        cui_row(ui, .gap = 10, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
            cui_segmented(ui, "th-type", TYPES, 2, &ed.radial, .size = CUI_SIZE_SM);
            if (!ed.radial) { cui_textf(ui, "Ângulo: %d°", (int)ed.angle); cui_slider(ui, "##th-ang", &ed.angle, 0, 360); }
        }
        cui_text(ui, "Paradas de cor (2 a 4): clique numa parada para escolher a cor; clique na barra para adicionar.", .size = 12, .color = B_MUTED);
        cui_gradient_opts go = { 0 }; go.selected = &ed_sel;
        cui_gradient_editor_ex(ui, "th-ed", ed.st, &ed.n, 4, &go);
        if (ed.n < 2) { ed.n = 2; ed.st[0].pos = 0; ed.st[1].pos = 1; }
        preview(ui, &ed, -1, 90, false, "pv-ed");
        briks_theme_t t = g_th; bool capped; float ratio = derive(&ed, &t, &capped);
        cui_textf(ui, "Contraste do texto: %.1f:1 %s", ratio, ratio >= 7 ? "(ótimo)" : ratio >= 4.5f ? "(bom)" : "(baixo)");
        if (capped) cui_text(ui, "Gradiente muito claro: as cores foram escurecidas automaticamente para o texto continuar legível.", .size = 12, .color = CUI_RGB(0xffc46b));
        cui_row(ui, .gap = 8, .width = CUI_FILL, .wrap = true) {
            if (cui_button(ui, "Aplicar", .variant = CUI_BUTTON_PRIMARY)) { grad_t g = ed; snprintf(g.id, sizeof g.id, "%s", ed_id[0] ? ed_id : "tmp"); apply(ui, &g); }
            if (cui_button(ui, ed_id[0] ? "Salvar alterações" : "Salvar em Meus gradientes", .variant = CUI_BUTTON_SECONDARY)) {
                if (!ed.name[0]) say(ui, "Dê um nome ao gradiente.");
                else if (ed_id[0]) { for (int i = 0; i < mine_n; i++) if (!strcmp(mine[i].id, ed_id)) { snprintf(ed.id, sizeof ed.id, "%s", ed_id); mine[i] = ed; } save_store(); say(ui, "Alterações salvas."); }
                else if (mine_n >= MAXMY) say(ui, "Limite de 16 gradientes salvos.");
                else { new_id(ed.id); mine[mine_n++] = ed; snprintf(ed_id, sizeof ed_id, "%s", ed.id); save_store(); say(ui, "Salvo em Meus gradientes."); }
            }
            if (ed_id[0] && cui_button(ui, "Novo", .variant = CUI_BUTTON_GHOST)) { ed_id[0] = 0; snprintf(ed.name, sizeof ed.name, "Novo gradiente"); }
        }
        if (fresh) cui_text(ui, note, .size = 12, .color = B_ONLINE);
    }

    TCARD(ui, "MEUS GRADIENTES") {
        if (!mine_n) cui_text(ui, "Você ainda não salvou nenhum gradiente. Monte o seu acima e salve.", .size = 12, .color = B_MUTED);
        for (int i = 0; i < mine_n; i++) {
            cui_push_id_int(ui, 6100 + i);
            cui_row(ui, .gap = 8, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
                preview(ui, &mine[i], 72, 38, !strcmp(active, mine[i].id), "mv");
                cui_text(ui, mine[i].name, .nowrap = true, .width = CUI_FILL);
                if (cui_button(ui, "Aplicar", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM)) apply(ui, &mine[i]);
                if (cui_button(ui, "Editar", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) { ed = mine[i]; snprintf(ed_id, sizeof ed_id, "%s", mine[i].id); ed_sel = 0; }
                if (cui_button(ui, "Duplicar", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM) && mine_n < MAXMY) {
                    grad_t c = mine[i]; new_id(c.id); snprintf(c.name, sizeof c.name, "%.30s (cópia)", mine[i].name); mine[mine_n++] = c; save_store();
                }
                if (cui_button(ui, "Excluir", .variant = CUI_BUTTON_DANGER, .size = CUI_SIZE_SM)) {
                    if (!strcmp(active, mine[i].id)) snprintf(active, sizeof active, "%s", "violeta");   /* o tema aplicado continua; só deixa de estar na lista */
                    if (!strcmp(ed_id, mine[i].id)) ed_id[0] = 0;
                    memmove(&mine[i], &mine[i + 1], (size_t)(mine_n - i - 1) * sizeof mine[0]); mine_n--; save_store();
                }
            }
            cui_pop_id(ui);
        }
        cui_row(ui, .gap = 8, .width = CUI_FILL, .wrap = true) {
            if (cui_button(ui, "Copiar lista", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) { static char b[16384]; write_json(b, sizeof b); clip_set(b); say(ui, "Lista copiada para a área de transferência."); }
            if (cui_button(ui, "Colar lista", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) {
                static char b[16384]; int n = clip_get(b, sizeof b) ? parse_items(b, false) : -1;
                if (n > 0) { for (int i = mine_n - n; i < mine_n; i++) new_id(mine[i].id); save_store(); say(ui, "Gradientes importados."); }
                else say(ui, n == 0 ? "Nada para importar na área de transferência." : "O texto copiado não é uma lista de gradientes.");
            }
        }
        if (fresh) cui_text(ui, note, .size = 12, .color = B_ONLINE);
    }

    TCARD(ui, "AVANÇADO") {
        cui_text(ui, "O editor completo (vidro, bolhas, raios, reflexos, refração) parte do gradiente escolhido.", .size = 12, .color = B_MUTED);
        cui_row(ui, .gap = 8, .width = CUI_FILL, .wrap = true) {
            if (cui_button(ui, "Personalização avançada", .variant = CUI_BUTTON_PRIMARY)) briks_theme_editor_open();
            if (cui_button(ui, "Restaurar padrão", .variant = CUI_BUTTON_SECONDARY)) { briks_theme_reset(); snprintf(active, sizeof active, "%s", "violeta"); save_store(); briks_apply_theme(ui); say(ui, "Tema padrão restaurado."); }
        }
        if (fresh) cui_text(ui, note, .size = 12, .color = B_ONLINE);
        if (g_perf != PERF_NORMAL) cui_text(ui, "Modo de desempenho reduzido: o fundo não custa mais por causa do gradiente; só as prévias ficam mais grosseiras.", .size = 11, .color = B_MUTED);
    }
}
