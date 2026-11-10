#include "msgui.h"
#include "json.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <shlobj.h>

#define MAXST 512
#define NREACT 6
typedef struct { char e[16]; int n; bool mine; } react_t;
typedef struct {
    char id[48], reply[48], edited[512], remind[40];
    bool used, has_edit, deleted, pinned, fav, unread;
    react_t r[NREACT]; int nr;
} mstate_t;
static mstate_t g_st[MAXST];
#define NQUICK 6
static char g_quick[NQUICK][16] = { "👍", "❤️", "😂", "😮", "😢", "🙏" };   /* barra rápida: o usuário troca arrastando da lista grande */
static bool g_loaded, g_demo;

void msgui_demo(bool on) { g_demo = on; g_loaded = true; }

/* ---------- persistência local (JSON) ---------- */
static bool state_path(char *out, size_t cap) {
    char appdata[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) != S_OK) return false;
    char dir[MAX_PATH]; snprintf(dir, sizeof dir, "%s\\Briks-CUI", appdata);
    CreateDirectoryA(dir, NULL);
    snprintf(out, cap, "%s\\msg_state.json", dir);
    return true;
}

static mstate_t *st_find(const char *id, bool create) {
    for (int i = 0; i < MAXST; i++) if (g_st[i].used && !strcmp(g_st[i].id, id)) return &g_st[i];
    if (!create) return NULL;
    for (int i = 0; i < MAXST; i++) if (!g_st[i].used) { memset(&g_st[i], 0, sizeof g_st[i]); g_st[i].used = true; snprintf(g_st[i].id, sizeof g_st[i].id, "%s", id); return &g_st[i]; }
    return NULL;
}

void msgui_demo_seed(void) {
    mstate_t *a = st_find("m1", true), *b = st_find("m2", true), *c = st_find("m3", true);
    snprintf(a->r[0].e, sizeof a->r[0].e, "👍"); a->r[0].n = 2; a->r[0].mine = true;
    snprintf(a->r[1].e, sizeof a->r[1].e, "❤️"); a->r[1].n = 1; a->nr = 2;
    snprintf(b->reply, sizeof b->reply, "m1"); snprintf(b->edited, sizeof b->edited, "Entro sim, só terminar o jantar. Já já eu chego!"); b->has_edit = true;
    c->pinned = true; c->fav = true;
}

static void st_save(void) {
    char p[MAX_PATH];
    if (g_demo || !state_path(p, sizeof p)) return;
    FILE *f = fopen(p, "wb"); if (!f) return;
    fputs("{\"version\":1,\"quick\":[", f);
    for (int i = 0; i < NQUICK; i++) { char e[40]; json_escape(g_quick[i], e, sizeof e); fprintf(f, "%s\"%s\"", i ? "," : "", e); }
    fputs("],\"messages\":[", f);
    bool first = true;
    for (int i = 0; i < MAXST; i++) {
        const mstate_t *m = &g_st[i];
        if (!m->used) continue;
        char id[100], rp[100], ed[1100], rm[90], rx[400] = "";
        json_escape(m->id, id, sizeof id); json_escape(m->reply, rp, sizeof rp); json_escape(m->edited, ed, sizeof ed); json_escape(m->remind, rm, sizeof rm);
        size_t n = 0;
        for (int k = 0; k < m->nr; k++) { char e[40]; json_escape(m->r[k].e, e, sizeof e); n += (size_t)snprintf(rx + n, sizeof rx - n, "%s[\"%s\",%d,%d]", k ? "," : "", e, m->r[k].n, m->r[k].mine); }
        fprintf(f, "%s{\"id\":\"%s\",\"reply\":\"%s\",\"edit\":%s%s%s,\"del\":%d,\"pin\":%d,\"fav\":%d,\"unread\":%d,\"remind\":\"%s\",\"rx\":[%s]}",
                first ? "" : ",", id, rp, m->has_edit ? "\"" : "null", m->has_edit ? ed : "", m->has_edit ? "\"" : "", m->deleted, m->pinned, m->fav, m->unread, rm, rx);
        first = false;
    }
    fputs("]}\n", f); fclose(f);
}

static bool jbool(const jval *o, const char *k) { const jval *v = json_get(o, k); return v && ((v->type == J_BOOL && v->b) || (v->type == J_NUM && v->num != 0)); }

static void st_load(void) {
    if (g_loaded) return;
    g_loaded = true;
    char p[MAX_PATH];
    if (!state_path(p, sizeof p)) return;
    FILE *f = fopen(p, "rb"); if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 4 * 1024 * 1024) { fclose(f); return; }
    char *buf = malloc((size_t)n + 1); size_t got = fread(buf, 1, (size_t)n, f); buf[got] = 0; fclose(f);
    jval *j = json_parse(buf); free(buf); if (!j) return;
    const jval *qk = json_get(j, "quick");
    for (int i = 0; qk && i < json_len(qk) && i < NQUICK; i++) { const jval *e = json_at(qk, i); if (e && e->type == J_STR && e->str[0]) snprintf(g_quick[i], sizeof g_quick[i], "%s", e->str); }
    const jval *arr = json_get(j, "messages");
    for (int i = 0; arr && i < json_len(arr); i++) {
        const jval *o = json_at(arr, i);
        mstate_t *m = st_find(json_str(o, "id", ""), true); if (!m) break;
        snprintf(m->reply, sizeof m->reply, "%s", json_str(o, "reply", ""));
        const jval *ed = json_get(o, "edit");
        if (ed && ed->type == J_STR) { m->has_edit = true; snprintf(m->edited, sizeof m->edited, "%s", ed->str); }
        m->deleted = jbool(o, "del"); m->pinned = jbool(o, "pin"); m->fav = jbool(o, "fav"); m->unread = jbool(o, "unread");
        snprintf(m->remind, sizeof m->remind, "%s", json_str(o, "remind", ""));
        const jval *rx = json_get(o, "rx");
        for (int k = 0; rx && k < json_len(rx) && m->nr < NREACT; k++) {
            const jval *t = json_at(rx, k);
            if (t && t->type == J_ARR && t->count >= 3) { react_t *r = &m->r[m->nr++]; snprintf(r->e, sizeof r->e, "%s", t->items[0].str ? t->items[0].str : ""); r->n = (int)t->items[1].num; r->mine = t->items[2].num != 0 || t->items[2].b; }
        }
    }
    json_free(j);
}

/* ---------- consultas ---------- */
bool msgui_deleted(const char *id) { st_load(); const mstate_t *m = st_find(id, false); return m && m->deleted; }
const char *msgui_text(const msg_t *m) { st_load(); const mstate_t *s = st_find(m->id, false); return s && s->has_edit ? s->edited : m->content; }

static void toggle_react(const char *id, const char *emoji) {
    mstate_t *m = st_find(id, true); if (!m) return;
    for (int i = 0; i < m->nr; i++) if (!strcmp(m->r[i].e, emoji)) {
        if (m->r[i].mine) { m->r[i].mine = false; if (m->r[i].n > 0) m->r[i].n--; } else { m->r[i].mine = true; m->r[i].n++; }
        if (m->r[i].n <= 0) { memmove(&m->r[i], &m->r[i + 1], sizeof(react_t) * (size_t)(m->nr - i - 1)); m->nr--; }
        st_save(); return;
    }
    if (m->nr >= NREACT) return;
    react_t *r = &m->r[m->nr++]; snprintf(r->e, sizeof r->e, "%s", emoji); r->n = 1; r->mine = true;
    st_save();
}

static void set_clipboard(const char *utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0); if (n <= 0) return;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (size_t)n * sizeof(wchar_t)); if (!h) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, (wchar_t *)GlobalLock(h), n); GlobalUnlock(h);
    if (OpenClipboard(NULL)) { EmptyClipboard(); if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h); CloseClipboard(); } else GlobalFree(h);
}

/* ---------- estado da interação ---------- */
static char g_reply_id[48], g_reply_author[96], g_reply_text[120];
static char g_edit_id[48];
static cui_rect g_view;
void msgui_set_view(cui_rect r) { g_view = r; }

static bool g_menu_want;             /* pedido de abrir o menu (consumido no overlay) */
static float g_menu_x, g_menu_y;
static msg_t g_menu_m;               /* mensagem alvo do menu/diálogos */
static bool g_menu_live;
static bool g_menu_open, g_emo_open, g_drag_on;
static bool g_dlg_delete, g_dlg_forward, g_dlg_remind, g_dlg_pins, g_dlg_topic;
static float g_emo_x, g_emo_y;
static char g_recent[160];
static int g_tone;


typedef struct { const char *icon; int n; const char *e[48]; } cat_t;
#define NCAT 8
static const cat_t CATS[NCAT] = {
 { "😀", 48, { "😀","😃","😄","😁","😆","😅","🤣","😂","🙂","😉","😊","😇","🥰","😍","🤩","😘","😋","😛","😜","🤪","🤗","🤔","🤨","😐","😑","🙄","😏","😮","😯","😲","😳","🥺","😢","😭","😤","😡","🥳","😎","🤯","😴","🤮","🥵","🥶","😱","😬","🤐","😈","💀" } },
 { "🐶", 40, { "🐶","🐱","🐭","🐹","🐰","🦊","🐻","🐼","🐨","🐯","🦁","🐮","🐷","🐸","🐵","🐔","🐧","🐦","🦆","🦉","🦇","🐺","🐴","🦄","🐝","🦋","🐌","🐞","🐢","🐍","🐙","🦀","🐠","🐬","🐳","🦈","🌵","🌲","🌻","🌹" } },
 { "🍕", 40, { "🍏","🍎","🍐","🍊","🍋","🍌","🍉","🍇","🍓","🍒","🍑","🥭","🍍","🥥","🥝","🍅","🥑","🌽","🥕","🍞","🧀","🍳","🥓","🍔","🍟","🍕","🌭","🌮","🍣","🍜","🍦","🍩","🍪","🎂","🍫","🍿","☕","🍺","🍷","🥤" } },
 { "⚽", 32, { "⚽","🏀","🏈","⚾","🎾","🏐","🎱","🏓","🥊","🎮","🕹️","🎲","🎯","🎳","🎸","🎹","🥁","🎧","🎤","🎬","🎨","🏆","🥇","🥈","🥉","🏅","🎪","🎭","🎟️","🎰","🧩","🎻" } },
 { "🚗", 32, { "🚗","🚕","🚌","🚑","🚒","🚓","🚲","🏍️","✈️","🚀","🛸","🚁","⛵","🚢","🏠","🏢","🏰","🗼","🗽","⛺","🌋","🏖️","🌅","🌃","🌉","🎡","🎢","⛽","🚦","🗺️","🧭","🌍" } },
 { "💡", 32, { "⌚","📱","💻","🖥️","⌨️","🖱️","💾","📷","🎥","📺","📻","🔋","🔌","💡","🔦","📚","📖","✏️","📌","📎","✂️","🔒","🔑","🔨","🔧","💊","🧪","🔭","💰","💎","📦","🎁" } },
 { "❤️", 40, { "❤️","🧡","💛","💚","💙","💜","🖤","🤍","💔","💕","💞","💓","💗","💖","💯","💢","💥","💫","💦","💤","✨","⭐","🌟","🔥","🎉","🎊","👍","👎","👏","🙌","🙏","💪","👌","✌️","🤞","🫶","👀","🧠","👋","🤝" } },
 { "🏳️", 24, { "🏳️","🏴","🏁","🚩","🇧🇷","🇵🇹","🇺🇸","🇬🇧","🇫🇷","🇩🇪","🇮🇹","🇪🇸","🇯🇵","🇰🇷","🇨🇳","🇦🇷","🇲🇽","🇨🇦","🇦🇺","🇮🇳","✅","❌","❓","❗" } },
};
static bool inside(cui_rect r, cui_vec2 p) { return r.w > 0 && p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h; }

static bool is_mine(const msg_t *m) { return m->mine; }
static bool group_open(const app_state_t *s) { return s->active_group >= 0; }
static bool can_moderate(const app_state_t *s) { return group_open(s) && s->groups[s->active_group].can_moderate; }
static bool can_delete(const app_state_t *s, const msg_t *m) { return is_mine(m) || can_moderate(s); }
static bool can_pin(const app_state_t *s) { return !group_open(s) || can_moderate(s); }

static char g_hot_now[48], g_hot_prev[48];   /* mensagem sob o mouse (balão ou seta) neste quadro / no quadro anterior */

static void open_menu(const msg_t *m, float x, float y) { g_menu_m = *m; g_menu_x = x; g_menu_y = y; g_menu_want = true; }

/* depois do balão e da seta: o hover vale só para o balão (e a seta); botão direito no balão abre o menu no mouse */
void msgui_row(cui_ctx *ui, app_state_t *s, const msg_t *m) {
    (void)s;
    cui_rect b = cui_get_rect(ui, cui_make_id(ui, "bub")), a = cui_get_rect(ui, cui_make_id(ui, "arw"));
    cui_vec2 mp = cui_mouse_pos(ui);
    if (b.w <= 0 || !inside(g_view, mp) || g_drag_on) return;
    if (g_menu_open || g_emo_open) return;
    bool over = inside(b, mp) || inside(a, mp);
    if (over) snprintf(g_hot_now, sizeof g_hot_now, "%s", m->id);
    if (inside(b, mp) && cui_mouse_clicked(ui, CUI_MOUSE_RIGHT)) open_menu(m, mp.x, mp.y);
}

/* setinha ao lado do balão: o espaço é sempre reservado (o layout não pula); só aparece no hover da mensagem */
void msgui_arrow(cui_ctx *ui, const msg_t *m) {
    bool show = !strcmp(g_hot_prev, m->id) || (g_menu_open && !strcmp(g_menu_m.id, m->id));
    cui_box(ui, .id = "arw", .width = CUI_PX(26), .height = CUI_PX(26), .radius = 13, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER,
            .bg = show ? B_GLASS_BUBBLE : CUI_RGBA(0x00000000), .border = show ? 1 : 0, .border_color = CUI_RGBA(0xffffff30)) {
        if (show) cui_icon(ui, CUI_ICON_CHEVRON_DOWN, 15, B_TEXT);
    }
    if (show && briks_clicked(ui, "arw")) {
        cui_rect r = cui_get_rect(ui, cui_make_id(ui, "arw"));
        open_menu(m, r.x < 640 ? r.x : r.x - 200, r.y + r.h + 4);
    }
}

void msgui_tags(cui_ctx *ui, const msg_t *m) {
    st_load();
    const mstate_t *t = st_find(m->id, false);
    if (!t) return;
    if (t->has_edit) cui_text(ui, "(editada)", .size = 10, .color = CUI_RGBA(0xffffff90));
    if (t->pinned) cui_icon(ui, CUI_ICON_MAP_PIN, 11, CUI_RGBA(0xffffffb0));
    if (t->fav) cui_icon(ui, CUI_ICON_STAR, 11, CUI_RGB(0xf5c542));
    if (t->remind[0]) cui_icon(ui, CUI_ICON_BELL, 11, CUI_RGBA(0xffffffb0));
    if (t->unread) cui_text(ui, "não lida", .size = 10, .color = CUI_RGB(0xf5c542));
}

void msgui_quote(cui_ctx *ui, const app_state_t *s, const msg_t *m) {
    st_load();
    const mstate_t *t = st_find(m->id, false);
    if (!t || !t->reply[0]) return;
    const msg_t *ref = NULL;
    for (int i = 0; i < s->msg_count; i++) if (!strcmp(s->msgs[i].id, t->reply)) { ref = &s->msgs[i]; break; }
    cui_box(ui, .dir = CUI_ROW, .gap = 6, .pad_x = 8, .pad_y = 4, .radius = 8, .bg = CUI_RGBA(0x00000030), .align = CUI_ALIGN_CENTER, .max_width = 460) {
        cui_icon(ui, CUI_ICON_MESSAGE_SQUARE, 11, B_MUTED);
        if (ref && !msgui_deleted(ref->id)) {
            char ex[90]; snprintf(ex, sizeof ex, "%.80s", msgui_text(ref));
            cui_text(ui, ref->author, .size = 11, .color = CUI_RGB(0xc4b5fd), .nowrap = true);
            cui_text(ui, ex, .size = 11, .color = CUI_RGBA(0xffffffb8), .nowrap = true);
        } else cui_text(ui, "Mensagem original indisponível", .size = 11, .color = B_MUTED);
    }
}

void msgui_reactions(cui_ctx *ui, const msg_t *m) {
    st_load();
    mstate_t *t = st_find(m->id, false);
    if (!t || !t->nr) return;
    cui_row(ui, .gap = 4) {
        for (int i = 0; i < t->nr; i++) {
            cui_push_id_int(ui, 300 + i);
            cui_box(ui, .id = "rx", .dir = CUI_ROW, .gap = 4, .pad_x = 7, .pad_y = 2, .radius = 10, .align = CUI_ALIGN_CENTER,
                    .bg = t->r[i].mine ? CUI_RGBA(0x8b5cf65a) : CUI_RGBA(0x00000033), .border = 1, .border_color = t->r[i].mine ? CUI_RGBA(0xc4b5fdaa) : CUI_RGBA(0xffffff22)) {
                cui_emoji(ui, t->r[i].e, 14);
                cui_textf(ui, "%d", t->r[i].n);
            }
            if (briks_clicked(ui, "rx")) toggle_react(m->id, t->r[i].e);
            cui_pop_id(ui);
        }
    }
}

/* ---------- composer: resposta e edição ---------- */
static void start_reply(const msg_t *m) {
    g_edit_id[0] = 0;
    snprintf(g_reply_id, sizeof g_reply_id, "%s", m->id); snprintf(g_reply_author, sizeof g_reply_author, "%s", m->author);
    snprintf(g_reply_text, sizeof g_reply_text, "%.110s", msgui_text(m));
}
static void start_edit(app_state_t *s, const msg_t *m) {
    g_reply_id[0] = 0;
    snprintf(g_edit_id, sizeof g_edit_id, "%s", m->id);
    snprintf(s->draft, sizeof s->draft, "%.250s", msgui_text(m));
}

void msgui_composer(cui_ctx *ui, app_state_t *s) {
    if (!g_reply_id[0] && !g_edit_id[0]) return;
    bool edit = g_edit_id[0] != 0;
    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .gap = 8, .pad_x = 12, .pad_y = 7, .radius = 14, .bg = CUI_RGBA(0x8b5cf626),
            .border = 1, .border_color = CUI_RGBA(0xc4b5fd44), .align = CUI_ALIGN_CENTER) {
        cui_icon(ui, edit ? CUI_ICON_PENCIL : CUI_ICON_MESSAGE_SQUARE, 14, CUI_RGB(0xc4b5fd));
        if (edit) cui_text(ui, "Editando mensagem", .size = 12, .color = CUI_RGB(0xc4b5fd));
        else { cui_textf(ui, "Respondendo a %s", g_reply_author); cui_text(ui, g_reply_text, .size = 12, .color = B_MUTED, .nowrap = true, .width = CUI_FILL); }
        if (edit) cui_box(ui, .width = CUI_FILL) {}
        if (cui_icon_button(ui, CUI_ICON_X, .size = CUI_SIZE_SM, .tooltip = "Cancelar")) { g_reply_id[0] = g_edit_id[0] = 0; if (edit) s->draft[0] = 0; }
    }
}

void msgui_submit(app_state_t *s) {
    if (!s->draft[0]) return;
    if (g_edit_id[0]) {
        mstate_t *t = st_find(g_edit_id, true);
        if (t) { snprintf(t->edited, sizeof t->edited, "%s", s->draft); t->has_edit = true; st_save(); }
        s->draft[0] = 0; g_edit_id[0] = 0;
        return;
    }
    app_send_message(s);
}

void msgui_after_send(const char *new_id) {
    if (!g_reply_id[0]) return;
    mstate_t *t = st_find(new_id, true);
    if (t) { snprintf(t->reply, sizeof t->reply, "%s", g_reply_id); st_save(); }
    g_reply_id[0] = 0;
}

/* ---------- barra de ações, menu e diálogos ---------- */

static bool chip(cui_ctx *ui, const char *id, const char *emoji, const char *icon, int sz) {
    cui_push_id(ui, id);
    cui_box(ui, .id = "c", .width = CUI_PX(28), .height = CUI_PX(28), .radius = 9, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER,
            .bg = CUI_RGBA(0xffffff10)) {
        if (emoji) cui_emoji(ui, emoji, (float)sz); else cui_icon(ui, icon, 15, B_TEXT);
    }
    bool hit = briks_clicked(ui, "c");
    cui_pop_id(ui);
    return hit;
}

static void act_close_menu(cui_ctx *ui) { (void)ui; g_menu_open = false; }

/* linha do menu: 30px, ícone + texto + dica; o realce acompanha o mouse (retângulo do quadro anterior) */
static bool mrow(cui_ctx *ui, int id, const char *icon, const char *label, const char *hint, bool danger) {
    cui_push_id_int(ui, 6000 + id);
    cui_rect pr = cui_get_rect(ui, cui_make_id(ui, "r"));
    bool hot = inside(pr, cui_mouse_pos(ui));
    cui_color c = danger ? B_DANGER : B_TEXT;
    cui_box(ui, .id = "r", .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(30), .gap = 10, .pad_x = 9, .radius = 9, .align = CUI_ALIGN_CENTER,
            .bg = hot ? (danger ? CUI_RGBA(0xe9555530) : CUI_RGBA(0xffffff1c)) : CUI_RGBA(0x00000000)) {
        cui_icon(ui, icon, 15, danger ? B_DANGER : B_MUTED);
        cui_text(ui, label, .size = 13, .color = c, .nowrap = true, .width = CUI_FILL);
        if (hint) cui_text(ui, hint, .size = 10, .color = CUI_RGB(0xf5c542), .nowrap = true);
    }
    bool hit = briks_clicked(ui, "r");
    cui_pop_id(ui);
    return hit;
}
static void msep(cui_ctx *ui) { cui_box(ui, .width = CUI_FILL, .height = CUI_PX(1), .bg = CUI_RGBA(0xffffff1a)) {} }

static void menu_body(cui_ctx *ui, app_state_t *s) {
    const msg_t *m = &g_menu_m;
    const mstate_t *t = st_find(m->id, false);
    bool pinned = t && t->pinned, fav = t && t->fav;
    cui_box(ui, .dir = CUI_ROW, .gap = 1, .pad = 3, .width = CUI_FILL, .radius = 12, .bg = CUI_RGBA(0xffffff12), .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
        for (int i = 0; i < NQUICK; i++) { char id[8]; snprintf(id, sizeof id, "q%d", i); if (chip(ui, id, g_quick[i], NULL, 18)) { toggle_react(m->id, g_quick[i]); act_close_menu(ui); } }
        if (chip(ui, "qplus", NULL, CUI_ICON_PLUS, 0)) { g_emo_open = true; g_emo_x = g_menu_x; g_emo_y = g_menu_y; act_close_menu(ui); }
    }
    msep(ui);
    if (mrow(ui, 3, CUI_ICON_MESSAGE_SQUARE, "Responder", NULL, false)) { start_reply(m); act_close_menu(ui); }
    if (mrow(ui, 4, CUI_ICON_SHARE_2, "Encaminhar", NULL, false)) { g_dlg_forward = true; act_close_menu(ui); }
    if (mrow(ui, 6, CUI_ICON_COPY, "Copiar texto", NULL, false)) { set_clipboard(msgui_text(m)); act_close_menu(ui); }
    msep(ui);
    if (is_mine(m) && mrow(ui, 2, CUI_ICON_PENCIL, "Editar mensagem", NULL, false)) { start_edit(s, m); act_close_menu(ui); }
    if (can_pin(s) && mrow(ui, 7, CUI_ICON_MAP_PIN, pinned ? "Desafixar mensagem" : "Fixar mensagem", NULL, false)) { mstate_t *w = st_find(m->id, true); if (w) { w->pinned = !pinned; st_save(); } act_close_menu(ui); }
    if (mrow(ui, 8, CUI_ICON_STAR, fav ? "Remover dos favoritos" : "Favoritar mensagem", NULL, false)) { mstate_t *w = st_find(m->id, true); if (w) { w->fav = !fav; st_save(); } act_close_menu(ui); }
    if (mrow(ui, 9, CUI_ICON_BELL, "Criar lembrete", "sem efeito", false)) { g_dlg_remind = true; act_close_menu(ui); }
    if (mrow(ui, 10, CUI_ICON_EYE_OFF, t && t->unread ? "Marcar como lida" : "Marcar como não lida", NULL, false)) { mstate_t *w = st_find(m->id, true); if (w) { w->unread = !(t && t->unread); st_save(); } act_close_menu(ui); }
    msep(ui);
    if (mrow(ui, 11, CUI_ICON_LINK, "Copiar link da mensagem", NULL, false)) { char l[220]; snprintf(l, sizeof l, "briks://mensagem/%s/%s", s->chat_channel, m->id); set_clipboard(l); act_close_menu(ui); }
    if (mrow(ui, 13, CUI_ICON_HASH, "Copiar ID da mensagem", NULL, false)) { set_clipboard(m->id); act_close_menu(ui); }
    if (can_delete(s, m)) { msep(ui); if (mrow(ui, 12, CUI_ICON_TRASH_2, "Excluir mensagem", NULL, true)) { g_dlg_delete = true; act_close_menu(ui); } }
}

static float menu_height(const app_state_t *s, const msg_t *m) { return 8.0f + (3.0f + (can_delete(s, m) ? 1 : 0)) * 9.0f + 30.0f * (float)(8 + (is_mine(m) ? 1 : 0) + (can_pin(s) ? 1 : 0) + (can_delete(s, m) ? 1 : 0)) + 6.0f; }

void msgui_pinned_button(cui_ctx *ui, app_state_t *s) {
    int n = 0;
    for (int i = 0; i < s->msg_count; i++) { const mstate_t *t = st_find(s->msgs[i].id, false); if (t && t->pinned && !t->deleted) n++; }
    cui_push_id(ui, "pinsbtn");
    cui_box(ui, .id = "b", .dir = CUI_ROW, .gap = 5, .pad_x = 9, .pad_y = 5, .radius = 12, .align = CUI_ALIGN_CENTER, .bg = CUI_RGBA(0xffffff12)) {
        cui_icon(ui, CUI_ICON_MAP_PIN, 13, n ? CUI_RGB(0xc4b5fd) : B_MUTED);
        cui_textf(ui, "Fixadas %d", n);
    }
    if (briks_clicked(ui, "b")) g_dlg_pins = true;
    cui_pop_id(ui);
}

void msgui_overlay(cui_ctx *ui, app_state_t *s) {
    st_load();
    cui_vec2 vp = cui_viewport_size(ui), mp = cui_mouse_pos(ui);
    static bool emoji_ok; if (!emoji_ok) { emoji_ok = true; cui_emoji_font_load_system(ui); }

    g_menu_live = g_menu_open || g_emo_open;
    memcpy(g_hot_prev, g_hot_now, sizeof g_hot_prev); g_hot_now[0] = 0;   /* a seta de cada mensagem lê o hover do quadro anterior */

    /* menu de contexto ancorado no mouse (vidro próprio, absoluto): sempre dentro da janela, rola se a janela for baixa */
    if (g_menu_want) { g_menu_open = true; g_menu_want = false; g_emo_open = false; }
    if (g_menu_open) {
        float mw = 252.0f, mh = menu_height(s, &g_menu_m), maxh = fmaxf(vp.y - 16.0f, 120.0f);
        float h = fminf(mh, maxh);
        float x = fminf(fmaxf(g_menu_x, 8), fmaxf(vp.x - mw - 8, 8)), y = fminf(fmaxf(g_menu_y, 8), fmaxf(vp.y - h - 8, 8));
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_START, .pad_left = x, .pad_top = y) {
            cui_box(ui, .id = "msgmenu", .width = CUI_PX(mw), .height = CUI_PX(h), .pad = 6, .radius = 16, .bg = B_GLASS_POPUP, .border = 1, .border_color = CUI_RGBA(0xc4b5fd40),
                    .elevation = CUI_ELEVATION_FLOATING, .scroll_y = mh > maxh, .clip = true) {
                menu_body(ui, s);
            }
        }
        cui_rect mr = cui_get_rect(ui, cui_make_id(ui, "msgmenu"));
        bool outside = mr.w > 0 && !inside(mr, mp);
        if (cui_key_pressed(ui, CUI_KEY_ESCAPE) || (outside && (cui_mouse_clicked(ui, CUI_MOUSE_LEFT) || cui_mouse_clicked(ui, CUI_MOUSE_RIGHT)))) g_menu_open = false;
    }

    /* seletor de emojis: barra rápida em cima (espaços que recebem emojis arrastados) e a lista grande por categorias */
    if (g_emo_open) {
        float pw = 372.0f, ph = 400.0f;
        float x = fminf(fmaxf(g_emo_x, 8), fmaxf(vp.x - pw - 8, 8)), y = fminf(fmaxf(g_emo_y, 8), fmaxf(vp.y - ph - 8, 8));
        static int cat; static char cand[16]; static bool prev_down, dragging; static cui_vec2 p0;
        bool down = cui_mouse_down(ui, CUI_MOUSE_LEFT);
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_START, .pad_left = x, .pad_top = y) {
            cui_box(ui, .id = "msgemoji", .width = CUI_PX(pw), .pad = 10, .gap = 8, .radius = 16, .bg = B_GLASS_POPUP, .border = 1, .border_color = CUI_RGBA(0xc4b5fd40),
                    .elevation = CUI_ELEVATION_FLOATING, .clip = true) {
                cui_text(ui, "Barra rápida — arraste um emoji da lista até um espaço", .size = 11, .color = B_MUTED);
                cui_row(ui, .gap = 6) {
                    for (int i = 0; i < NQUICK; i++) {
                        cui_push_id_int(ui, 7100 + i);
                        cui_rect sr = cui_get_rect(ui, cui_make_id(ui, "slot"));
                        bool tgt = dragging && inside(sr, mp);
                        cui_box(ui, .id = "slot", .width = CUI_PX(40), .height = CUI_PX(40), .radius = 11, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER,
                                .bg = tgt ? CUI_RGBA(0x8b5cf655) : CUI_RGBA(0xffffff0c), .border = 1, .border_color = tgt ? CUI_RGB(0xc4b5fd) : CUI_RGBA(0xc4b5fd60)) {
                            cui_emoji(ui, g_quick[i], 22);
                        }
                        if (dragging && prev_down && !down && tgt && cand[0]) { snprintf(g_quick[i], sizeof g_quick[i], "%s", cand); st_save(); }
                        cui_pop_id(ui);
                    }
                }
                cui_row(ui, .gap = 2) {
                    for (int c = 0; c < NCAT; c++) {
                        cui_push_id_int(ui, 7200 + c);
                        cui_box(ui, .id = "t", .width = CUI_PX(38), .height = CUI_PX(30), .radius = 9, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER,
                                .bg = c == cat ? CUI_RGBA(0x8b5cf65a) : CUI_RGBA(0x00000000)) { cui_emoji(ui, CATS[c].icon, 17); }
                        if (briks_clicked(ui, "t")) cat = c;
                        cui_pop_id(ui);
                    }
                }
                cui_box(ui, .id = "egrid", .width = CUI_FILL, .height = CUI_PX(228), .scroll_y = true, .gap = 2) {
                    int cols = 8, n = CATS[cat].n;
                    for (int r0 = 0; r0 < n; r0 += cols) {
                        cui_push_id_int(ui, 7300 + r0);
                        cui_row(ui, .gap = 2) {
                            for (int k = r0; k < r0 + cols && k < n; k++) {
                                cui_push_id_int(ui, 7400 + k);
                                const char *e = CATS[cat].e[k];
                                cui_rect cr = cui_get_rect(ui, cui_make_id(ui, "c"));
                                bool hot = inside(cr, mp);
                                cui_box(ui, .id = "c", .width = CUI_PX(40), .height = CUI_PX(38), .radius = 9, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER,
                                        .bg = hot ? CUI_RGBA(0xffffff1c) : CUI_RGBA(0x00000000)) { cui_emoji(ui, e, 24); }
                                if (down && !prev_down && hot) { snprintf(cand, sizeof cand, "%s", e); p0 = mp; dragging = false; }
                                cui_pop_id(ui);
                            }
                        }
                        cui_pop_id(ui);
                    }
                }
                cui_text(ui, "Clique para reagir. As escolhas da barra rápida ficam salvas neste computador.", .size = 11, .color = B_MUTED);
            }
        }
        if (down && cand[0] && !dragging && (fabsf(mp.x - p0.x) + fabsf(mp.y - p0.y)) > 6) dragging = true;
        g_drag_on = dragging;
        if (dragging) {   /* o emoji acompanha o mouse */
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_START, .pad_left = mp.x - 18, .pad_top = mp.y - 18) {
                cui_box(ui, .width = CUI_PX(36), .height = CUI_PX(36), .radius = 12, .bg = B_GLASS_BUBBLE, .border = 1, .border_color = CUI_RGBA(0xffffff40),
                        .elevation = CUI_ELEVATION_RESTING, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) { cui_emoji(ui, cand, 22); }
            }
        }
        cui_rect er = cui_get_rect(ui, cui_make_id(ui, "msgemoji"));
        if (prev_down && !down) {
            if (!dragging && cand[0]) { toggle_react(g_menu_m.id, cand); g_emo_open = false; }   /* clique simples = reagir */
            cand[0] = 0; dragging = false; g_drag_on = false;
        } else if (er.w > 0 && !inside(er, mp) && cui_mouse_clicked(ui, CUI_MOUSE_LEFT) && !dragging) g_emo_open = false;
        if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) { g_emo_open = false; cand[0] = 0; dragging = false; g_drag_on = false; }
        prev_down = down;
    }

    float dw = fminf(400.0f, fmaxf(vp.x - 48.0f, 260.0f));
    cui_dialog(ui, "Excluir mensagem", &g_dlg_delete, .width = dw) {
        cui_text(ui, "Tem certeza de que quer excluir esta mensagem? Não dá para desfazer.", .color = B_MUTED, .size = 13);
        cui_box(ui, .width = CUI_FILL, .pad = 10, .radius = 12, .bg = CUI_RGBA(0x00000030)) { cui_text(ui, g_menu_m.author, .size = 12, .color = B_C3); cui_text(ui, msgui_text(&g_menu_m)); }
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Cancelar", .variant = CUI_BUTTON_SECONDARY)) g_dlg_delete = false;
            if (cui_button(ui, "Excluir", .variant = CUI_BUTTON_DANGER)) { mstate_t *w = st_find(g_menu_m.id, true); if (w) { w->deleted = true; st_save(); } g_dlg_delete = false; }
        }
    }
    cui_dialog(ui, "Encaminhar para…", &g_dlg_forward, .width = dw) {
        cui_box(ui, .width = CUI_FILL, .max_height = fmaxf(vp.y - 260.0f, 120.0f), .scroll_y = true, .gap = 4) {
            for (int i = 0; i < s->group_count; i++) {
                cui_push_id_int(ui, 4000 + i);
                if (cui_button(ui, s->groups[i].name, .width = CUI_FILL, .variant = CUI_BUTTON_SECONDARY)) { app_forward_message(s, true, i, msgui_text(&g_menu_m)); g_dlg_forward = false; }
                cui_pop_id(ui);
            }
            for (int i = 0; i < s->friend_count; i++) {
                cui_push_id_int(ui, 4100 + i);
                if (cui_button(ui, s->friends[i].name, .width = CUI_FILL, .variant = CUI_BUTTON_SECONDARY)) { app_forward_message(s, false, i, msgui_text(&g_menu_m)); g_dlg_forward = false; }
                cui_pop_id(ui);
            }
            if (!s->group_count && !s->friend_count) cui_text(ui, "Nenhuma conversa disponível.", .color = B_MUTED, .size = 13);
        }
    }
    cui_dialog(ui, "Criar lembrete", &g_dlg_remind, .width = dw) {
        cui_text(ui, "Quando você quer ser lembrado desta mensagem?", .color = B_MUTED, .size = 13);
        const char *opt[] = { "Em 20 minutos", "Em 1 hora", "Em 3 horas", "Amanhã" };
        for (int i = 0; i < 4; i++) {
            cui_push_id_int(ui, 4200 + i);
            if (cui_button(ui, opt[i], .width = CUI_FILL, .variant = CUI_BUTTON_SECONDARY)) { mstate_t *w = st_find(g_menu_m.id, true); if (w) { snprintf(w->remind, sizeof w->remind, "%s", opt[i]); st_save(); } g_dlg_remind = false; }
            cui_pop_id(ui);
        }
        cui_text(ui, "Sem efeito real ainda: o lembrete fica gravado aqui, mas ainda não existe notificação.", .size = 12, .color = CUI_RGB(0xf5c542));
    }
    cui_dialog(ui, "Criar tópico", &g_dlg_topic, .width = dw) {
        cui_text(ui, "Tópicos precisam de um canal próprio no grupo, que o P2P ainda não tem.", .color = B_MUTED, .size = 13);
        cui_text(ui, "Sem efeito real ainda.", .size = 12, .color = CUI_RGB(0xf5c542));
        if (cui_button(ui, "Entendi", .variant = CUI_BUTTON_PRIMARY)) g_dlg_topic = false;
    }
    cui_dialog(ui, "Mensagens fixadas", &g_dlg_pins, .width = fminf(460.0f, fmaxf(vp.x - 48.0f, 260.0f))) {
        int n = 0;
        cui_box(ui, .width = CUI_FILL, .max_height = fmaxf(vp.y - 240.0f, 120.0f), .scroll_y = true, .gap = 6) {
            for (int i = 0; i < s->msg_count; i++) {
                const mstate_t *t = st_find(s->msgs[i].id, false);
                if (!t || !t->pinned || t->deleted) continue;
                n++;
                cui_push_id_int(ui, 4300 + i);
                cui_box(ui, .width = CUI_FILL, .pad = 10, .radius = 12, .bg = CUI_RGBA(0xffffff0e), .border = 1, .border_color = B_GLASS_BORDER_SOFT, .gap = 2) {
                    cui_text(ui, s->msgs[i].author, .size = 12, .color = B_C3);
                    cui_text(ui, msgui_text(&s->msgs[i]));
                }
                cui_pop_id(ui);
            }
            if (!n) cui_text(ui, "Nenhuma mensagem fixada neste canal.", .color = B_MUTED, .size = 13);
        }
    }
}
