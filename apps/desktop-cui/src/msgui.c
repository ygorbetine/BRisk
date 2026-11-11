#include "msgui.h"
#include "json.h"
#include "attach.h"
#include "richtext.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <shlobj.h>

#define MAXST 512
#define NREACT 6
typedef struct { char e[16]; int n; bool mine; unsigned who[8]; int nwho; } react_t;   /* who: hash das impressões digitais de quem reagiu (eventos remotos) */
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

/* ---------- eventos assinados (core) ---------- */
static void st_save(void);
static void st_load(void);
static app_state_t *g_app;
#define EVQ 24
static struct { char ch[64]; char body[1200]; } g_evq[EVQ];
static int g_evq_n;
static char g_seen[128][40]; static int g_seen_n;

static unsigned fphash(const char *t) { unsigned h = 2166136261u; for (; *t; t++) { h ^= (unsigned char)*t; h *= 16777619u; } return h; }

/* assina e envia ao grupo da sala (peers autenticados). Fora de uma sala o efeito fica só local. */
static void emit_ev(const char *target, const char *action, const char *content, const char *reference, const char *emoji) {
    if (g_demo || !g_app || !g_app->in_room || !g_app->chat_channel[0]) return;
    char ch[200], tg[120], ac[40], ct[1200] = "", rf[120] = "", em[40] = "", line[2000];
    json_escape(g_app->chat_channel, ch, sizeof ch); json_escape(target, tg, sizeof tg); json_escape(action, ac, sizeof ac);
    int n = snprintf(line, sizeof line, "\"channel\":\"%s\",\"target\":\"%s\",\"action\":\"%s\"", ch, tg, ac);
    if (content) { json_escape(content, ct, sizeof ct); n += snprintf(line + n, sizeof line - (size_t)n, ",\"content\":\"%s\"", ct); }
    if (reference) { json_escape(reference, rf, sizeof rf); n += snprintf(line + n, sizeof line - (size_t)n, ",\"reference\":\"%s\"", rf); }
    if (emoji) { json_escape(emoji, em, sizeof em); snprintf(line + n, sizeof line - (size_t)n, ",\"emoji\":\"%s\"", em); }
    rtc_chat_event(line);
}

/* fila de gravação no backend local (POST /p2p/chat-events/<canal>), um por vez */
void msgui_pump(app_state_t *s) {
    g_app = s;
    if (g_evq_n && s->token[0] && !s->busy) {
        char path[160]; snprintf(path, sizeof path, "/p2p/chat-events/%s", g_evq[0].ch);
        app_begin(s, OP_EVENT_SAVE, "POST", path, g_evq[0].body);
        memmove(&g_evq[0], &g_evq[1], sizeof g_evq[0] * (size_t)(g_evq_n - 1)); g_evq_n--;
    }
}

static const char *peer_name(const app_state_t *s, const char *fp) {
    for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (s->rtc_peers[i].id[0] && s->rtc_peers[i].fp[0] && !strcmp(s->rtc_peers[i].fp, fp)) return s->rtc_peers[i].name;
    return NULL;
}

/* Evento verificado pelo core (assinatura, autor = peer provado). `local` = criado por mim (só persistir).
 * Ações de autor (edit/delete/reply) só valem se a mensagem está na conversa aberta e o autor dela tem o nome do peer provado;
 * fixar só em conversa direta (em grupo o core de cada lado não sabe quem é admin). Reações valem de qualquer peer presente. */
bool msgui_on_event(app_state_t *s, const char *from_fp, bool local, const char *wire) {
    g_app = s; st_load();
    jval *j = json_parse(wire); if (!j) return false;
    const char *id = json_str(j, "id", ""), *ch = json_str(j, "channelId", ""), *tg = json_str(j, "targetMessageId", ""), *ac = json_str(j, "action", "");
    bool changed = false;
    for (int i = 0; i < g_seen_n; i++) if (!strcmp(g_seen[i], id)) { json_free(j); return false; }
    snprintf(g_seen[g_seen_n % 128], sizeof g_seen[0], "%s", id); g_seen_n++; if (g_seen_n > 100000) g_seen_n = 128;
    if (g_evq_n < EVQ && !g_demo) { snprintf(g_evq[g_evq_n].ch, sizeof g_evq[0].ch, "%s", ch); snprintf(g_evq[g_evq_n].body, sizeof g_evq[0].body, "%s", wire); g_evq_n++; }
    if (!local) {
        const char *who = peer_name(s, from_fp);
        const msg_t *msg = NULL;
        if (!strcmp(ch, s->chat_channel)) for (int i = 0; i < s->msg_count; i++) if (!strcmp(s->msgs[i].id, tg)) { msg = &s->msgs[i]; break; }
        bool author = msg && who && !msg->mine && !strcmp(msg->author, who);
        mstate_t *t = NULL;
        if (!strcmp(ac, "reaction.add") || !strcmp(ac, "reaction.remove")) {
            const char *em = json_str(j, "emoji", ""); unsigned h = fphash(from_fp); bool add = !strcmp(ac, "reaction.add");
            t = st_find(tg, true);
            if (t && em[0]) {
                int k = 0; while (k < t->nr && strcmp(t->r[k].e, em)) k++;
                if (k == t->nr && add && t->nr < NREACT) { snprintf(t->r[k].e, sizeof t->r[k].e, "%s", em); t->r[k].n = 0; t->r[k].nwho = 0; t->nr++; }
                if (k < t->nr) {
                    react_t *r = &t->r[k]; int w = 0; while (w < r->nwho && r->who[w] != h) w++;
                    if (add && w == r->nwho && r->nwho < 8) { r->who[r->nwho++] = h; r->n++; changed = true; }
                    else if (!add && w < r->nwho) { r->who[w] = r->who[--r->nwho]; if (r->n > 0) r->n--; changed = true; }
                    if (r->n <= 0 && !r->mine) { memmove(r, r + 1, sizeof *r * (size_t)(t->nr - k - 1)); t->nr--; }
                }
            }
        } else if (!strcmp(ac, "edit") && author) { t = st_find(tg, true); if (t) { snprintf(t->edited, sizeof t->edited, "%s", json_str(j, "content", "")); t->has_edit = true; changed = true; } }
        else if (!strcmp(ac, "delete") && author) { t = st_find(tg, true); if (t) { t->deleted = true; changed = true; } }
        else if (!strcmp(ac, "reply") && author) { t = st_find(tg, true); if (t) { snprintf(t->reply, sizeof t->reply, "%s", json_str(j, "referenceMessageId", "")); changed = true; } }
        else if ((!strcmp(ac, "pin") || !strcmp(ac, "unpin")) && msg && s->active_group < 0) { t = st_find(tg, true); if (t) { t->pinned = !strcmp(ac, "pin"); changed = true; } }
        if (changed) st_save();
    }
    json_free(j);
    return changed;
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
        char id[100], rp[100], ed[1100], rm[90], rx[900] = "";
        json_escape(m->id, id, sizeof id); json_escape(m->reply, rp, sizeof rp); json_escape(m->edited, ed, sizeof ed); json_escape(m->remind, rm, sizeof rm);
        size_t n = 0;
        for (int k = 0; k < m->nr; k++) {
            char e[40], w[120] = ""; json_escape(m->r[k].e, e, sizeof e);
            size_t wn = 0; for (int q = 0; q < m->r[k].nwho; q++) wn += (size_t)snprintf(w + wn, sizeof w - wn, "%s%u", q ? "," : "", m->r[k].who[q]);
            n += (size_t)snprintf(rx + n, sizeof rx - n, "%s[\"%s\",%d,%d,[%s]]", k ? "," : "", e, m->r[k].n, m->r[k].mine, w);
        }
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
            if (t && t->type == J_ARR && t->count >= 3) { react_t *r = &m->r[m->nr++]; snprintf(r->e, sizeof r->e, "%s", t->items[0].str ? t->items[0].str : ""); r->n = (int)t->items[1].num; r->mine = t->items[2].num != 0 || t->items[2].b;
                if (t->count >= 4 && t->items[3].type == J_ARR) for (int q = 0; q < t->items[3].count && r->nwho < 8; q++) r->who[r->nwho++] = (unsigned)t->items[3].items[q].num; }
        }
    }
    json_free(j);
}

/* ---------- consultas ---------- */
bool msgui_has_reply(const msg_t *m) { st_load(); const mstate_t *t = st_find(m->id, false); return t && t->reply[0]; }
bool msgui_deleted(const char *id) { st_load(); const mstate_t *m = st_find(id, false); return m && m->deleted; }
const char *msgui_text(const msg_t *m) { st_load(); const mstate_t *s = st_find(m->id, false); return s && s->has_edit ? s->edited : m->content; }

static void toggle_react(const char *id, const char *emoji) {
    mstate_t *m = st_find(id, true); if (!m) return;
    char em[16], mid[48]; snprintf(em, sizeof em, "%s", emoji); snprintf(mid, sizeof mid, "%s", id); emoji = em; id = mid;   /* copia: os ponteiros podem apontar para dentro do que vamos mexer */
    for (int i = 0; i < m->nr; i++) if (!strcmp(m->r[i].e, emoji)) {
        bool add = !m->r[i].mine;
        if (!add) { m->r[i].mine = false; if (m->r[i].n > 0) m->r[i].n--; } else { m->r[i].mine = true; m->r[i].n++; }
        if (m->r[i].n <= 0) { memmove(&m->r[i], &m->r[i + 1], sizeof(react_t) * (size_t)(m->nr - i - 1)); m->nr--; }
        st_save(); emit_ev(id, add ? "reaction.add" : "reaction.remove", NULL, NULL, emoji); return;
    }
    if (m->nr >= NREACT) return;
    react_t *r = &m->r[m->nr++]; memset(r, 0, sizeof *r); snprintf(r->e, sizeof r->e, "%s", emoji); r->n = 1; r->mine = true;
    st_save(); emit_ev(id, "reaction.add", NULL, NULL, emoji);
}

/* ---------- área de transferência (real; ou falsa nos testes) ---------- */
static bool g_clip_fake; static char g_clip_buf[8192];
static void fake_set(void *u, const char *t) { (void)u; snprintf(g_clip_buf, sizeof g_clip_buf, "%s", t ? t : ""); }
static const char *fake_get(void *u) { (void)u; return g_clip_buf; }
/* BRIKS_FAKE_CLIPBOARD=1 (ou o texto inicial): copiar e colar vão para um buffer em memória; a área de transferência do usuário nunca é lida nem escrita */
void briks_clip_install(cui_ctx *ui) {
    const char *f = getenv("BRIKS_FAKE_CLIPBOARD");
    if (!f || !f[0]) return;
    cui_platform_iface p; cui_get_platform(ui, &p);
    p.set_clipboard = fake_set; p.get_clipboard = fake_get; cui_set_platform(ui, &p);
    g_clip_fake = true; if (strcmp(f, "1")) fake_set(NULL, f);
}
bool briks_clip_fake(void) { return g_clip_fake; }
const char *briks_clip_probe(void) { return g_clip_fake ? g_clip_buf : ""; }

void briks_clip_set(const char *utf8) {
    if (g_clip_fake) { fake_set(NULL, utf8); return; }
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0); if (n <= 0) return;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (size_t)n * sizeof(wchar_t)); if (!h) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, (wchar_t *)GlobalLock(h), n); GlobalUnlock(h);
    if (OpenClipboard(NULL)) { EmptyClipboard(); if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h); CloseClipboard(); } else GlobalFree(h);
}

/* ---------- menu de clique direito dos campos de texto (BAS-02) ----------
 * Todo cui_input do app passa por aqui (macro em briks_ui.h). O menu é o popup do CUI (acima de diálogos, fecha ao clicar fora) e conta como
 * modal para a regra central; as ações viram comandos de edição do campo (cui_edit_command), que rodam com o foco devolvido a ele. */
int briks_input_ex(cui_ctx *ui, const char *id, char *buf, size_t cap, const cui_input_opts *o) {
    int r = cui_input_ex(ui, id, buf, cap, o);
    cui_id fid = cui_make_id(ui, id);
    bool pw = o && o->password, ro = o && (o->read_only || o->disabled);
    cui_push_id(ui, id);
    cui_context_menu(ui, "ctxmenu") {
        int s0 = 0, s1 = 0; bool can_u = false, can_r = false;
        cui_edit_selection(ui, fid, &s0, &s1, &can_u, &can_r);
        bool sel = s1 > s0, any = buf[0] != 0;
        if (cui_menu_item(ui, "Desfazer", .icon = CUI_ICON_UNDO_2, .shortcut = "Ctrl+Z", .disabled = ro || !can_u)) cui_edit_command(ui, fid, CUI_EDIT_UNDO);
        if (cui_menu_item(ui, "Refazer", .icon = CUI_ICON_REDO_2, .shortcut = "Ctrl+Y", .disabled = ro || !can_r)) cui_edit_command(ui, fid, CUI_EDIT_REDO);
        cui_menu_separator(ui);
        if (cui_menu_item(ui, "Recortar", .icon = CUI_ICON_SCISSORS, .shortcut = "Ctrl+X", .disabled = ro || pw || !sel)) cui_edit_command(ui, fid, CUI_EDIT_CUT);
        if (cui_menu_item(ui, "Copiar", .icon = CUI_ICON_COPY, .shortcut = "Ctrl+C", .disabled = pw || !sel)) cui_edit_command(ui, fid, CUI_EDIT_COPY);
        if (cui_menu_item(ui, "Colar", .icon = CUI_ICON_CLIPBOARD, .shortcut = "Ctrl+V", .disabled = ro)) {
            if (!strcmp(id, "draft") && attach_paste_image(ui)) {}   /* imagem na área de transferência: vira anexo (BAS-24) */
            else cui_edit_command(ui, fid, CUI_EDIT_PASTE);
        }
        cui_menu_separator(ui);
        if (cui_menu_item(ui, "Selecionar tudo", .icon = CUI_ICON_SQUARE_CHECK, .shortcut = "Ctrl+A", .disabled = !any)) cui_edit_command(ui, fid, CUI_EDIT_SELECT_ALL);
    }
    cui_pop_id(ui);
    return r;
}

/* ---------- estado da interação ---------- */
static char g_reply_id[48], g_reply_author[96], g_reply_text[120];
static char g_edit_id[48];
static cui_rect g_view;
void msgui_set_view(cui_rect r) { g_view = r; }

static bool g_menu_want;             /* pedido de abrir o menu (consumido no overlay) */
static float g_menu_x, g_menu_y;
static msg_t g_menu_m;               /* mensagem alvo do menu/diálogos */
static char g_menu_sel[1100];         /* trecho de texto marcado com o mouse na mensagem alvo (BAS-04); vazio = sem seleção */
static bool g_menu_live;
static bool g_menu_open, g_emo_open, g_drag_on;
static bool g_dlg_delete, g_dlg_forward, g_dlg_remind, g_dlg_pins, g_dlg_topic, g_dlg_select;
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

static void open_menu(cui_ctx *ui, const msg_t *m, float x, float y) {
    g_menu_m = *m; g_menu_x = x; g_menu_y = y; g_menu_want = true;
    g_menu_sel[0] = 0; attach_selected_text(ui, m->id, msgui_text(m), g_menu_sel, sizeof g_menu_sel);   /* lê a seleção agora: o clique no menu tira o foco do texto */
}

/* depois do balão e da seta: o hover vale só para o balão (e a seta); botão direito no balão abre o menu no mouse */
void msgui_row(cui_ctx *ui, app_state_t *s, const msg_t *m) {
    (void)s;
    cui_rect b = cui_get_rect(ui, cui_make_id(ui, "bub")), a = cui_get_rect(ui, cui_make_id(ui, "arw"));
    cui_vec2 mp = cui_mouse_pos(ui);
    if (b.w <= 0 || !inside(g_view, mp) || g_drag_on) return;
    if (g_menu_open || g_emo_open) return;
    bool over = inside(b, mp) || inside(a, mp);
    if (over) snprintf(g_hot_now, sizeof g_hot_now, "%s", m->id);
    if (!briks_blocked() && inside(b, mp) && cui_mouse_clicked(ui, CUI_MOUSE_RIGHT)) open_menu(ui, m, mp.x, mp.y);   /* modal aberto: botão direito não atravessa */
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
        open_menu(ui, m, r.x < 640 ? r.x : r.x - 200, r.y + r.h + 4);
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
        if (t) { snprintf(t->edited, sizeof t->edited, "%s", s->draft); t->has_edit = true; st_save(); emit_ev(g_edit_id, "edit", s->draft, NULL, NULL); }
        s->draft[0] = 0; g_edit_id[0] = 0;
        return;
    }
    app_send_message(s);
}

void msgui_after_send(const char *new_id) {
    if (!g_reply_id[0]) return;
    mstate_t *t = st_find(new_id, true);
    if (t) { snprintf(t->reply, sizeof t->reply, "%s", g_reply_id); st_save(); emit_ev(new_id, "reply", NULL, g_reply_id, NULL); }
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
    if (g_menu_sel[0] && mrow(ui, 14, CUI_ICON_TYPE, "Copiar seleção", NULL, false)) { briks_clip_set(g_menu_sel); act_close_menu(ui); }
    if (!rich_is_plain(msgui_text(m)) && mrow(ui, 15, CUI_ICON_TYPE, "Selecionar texto…", NULL, false)) { g_dlg_select = true; act_close_menu(ui); }   /* mensagem com formatação: texto cru selecionável num quadro */
    if (mrow(ui, 6, CUI_ICON_COPY, "Copiar texto", NULL, false)) { briks_clip_set(msgui_text(m)); act_close_menu(ui); }
    msep(ui);
    if (is_mine(m) && mrow(ui, 2, CUI_ICON_PENCIL, "Editar mensagem", NULL, false)) { start_edit(s, m); act_close_menu(ui); }
    if (can_pin(s) && mrow(ui, 7, CUI_ICON_MAP_PIN, pinned ? "Desafixar mensagem" : "Fixar mensagem", NULL, false)) { mstate_t *w = st_find(m->id, true); if (w) { w->pinned = !pinned; st_save(); emit_ev(m->id, w->pinned ? "pin" : "unpin", NULL, NULL, NULL); } act_close_menu(ui); }
    if (mrow(ui, 8, CUI_ICON_STAR, fav ? "Remover dos favoritos" : "Favoritar mensagem", NULL, false)) { mstate_t *w = st_find(m->id, true); if (w) { w->fav = !fav; st_save(); } act_close_menu(ui); }
    if (mrow(ui, 9, CUI_ICON_BELL, "Criar lembrete", "sem efeito", false)) { g_dlg_remind = true; act_close_menu(ui); }
    if (mrow(ui, 10, CUI_ICON_EYE_OFF, t && t->unread ? "Marcar como lida" : "Marcar como não lida", NULL, false)) { mstate_t *w = st_find(m->id, true); if (w) { w->unread = !(t && t->unread); st_save(); } act_close_menu(ui); }
    msep(ui);
    if (mrow(ui, 11, CUI_ICON_LINK, "Copiar link da mensagem", NULL, false)) { char l[220]; snprintf(l, sizeof l, "briks://mensagem/%s/%s", s->chat_channel, m->id); briks_clip_set(l); act_close_menu(ui); }
    if (mrow(ui, 13, CUI_ICON_HASH, "Copiar ID da mensagem", NULL, false)) { briks_clip_set(m->id); act_close_menu(ui); }
    if (can_delete(s, m)) { msep(ui); if (mrow(ui, 12, CUI_ICON_TRASH_2, "Excluir mensagem", NULL, true)) { g_dlg_delete = true; act_close_menu(ui); } }
}

static float menu_height(const app_state_t *s, const msg_t *m) { return 8.0f + (3.0f + (can_delete(s, m) ? 1 : 0)) * 9.0f + 30.0f * (float)(8 + (g_menu_sel[0] ? 1 : 0) + (rich_is_plain(msgui_text(m)) ? 0 : 1) + (is_mine(m) ? 1 : 0) + (can_pin(s) ? 1 : 0) + (can_delete(s, m) ? 1 : 0)) + 6.0f; }

void msgui_pinned_button(cui_ctx *ui, app_state_t *s) {
    int n = 0;
    for (int i = 0; i < s->msg_count; i++) { const mstate_t *t = st_find(s->msgs[i].id, false); if (t && t->pinned && !t->deleted) n++; }
    cui_push_id(ui, "pinsbtn");
    cui_box(ui, .id = "b", .dir = CUI_ROW, .gap = 5, .pad_x = 9, .pad_y = 5, .radius = 12, .align = CUI_ALIGN_CENTER, .bg = CUI_RGBA(0xffffff12)) {
        cui_icon(ui, CUI_ICON_MAP_PIN, 13, n ? CUI_RGB(0xc4b5fd) : B_MUTED);
        { char t[24]; if (g_view.w > 0 && g_view.w < 560) snprintf(t, sizeof t, "%d", n); else snprintf(t, sizeof t, "Fixadas %d", n); cui_text(ui, t, .nowrap = true); }   /* painel estreito (janela mínima, texto grande): só o número, sem quebrar em duas linhas */
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
            if (cui_button(ui, "Excluir", .variant = CUI_BUTTON_DANGER)) { mstate_t *w = st_find(g_menu_m.id, true); if (w) { w->deleted = true; st_save(); emit_ev(g_menu_m.id, "delete", NULL, NULL, NULL); } g_dlg_delete = false; }
        }
    }
    cui_dialog(ui, "Selecionar texto", &g_dlg_select, .width = dw) {
        cui_text(ui, "Arraste para marcar um trecho e use Ctrl+C.", .color = B_MUTED, .size = 12);
        cui_box(ui, .width = CUI_FILL, .pad = 10, .radius = 12, .bg = CUI_RGBA(0x00000030), .max_height = fmaxf(vp.y - 260.0f, 120.0f), .scroll_y = true) {
            cui_push_id(ui, "seltxt"); cui_selectable_text(ui, "sel", msgui_text(&g_menu_m)); cui_pop_id(ui);
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Copiar tudo", .variant = CUI_BUTTON_SECONDARY)) briks_clip_set(msgui_text(&g_menu_m));
            if (cui_button(ui, "Fechar", .variant = CUI_BUTTON_PRIMARY)) g_dlg_select = false;
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
