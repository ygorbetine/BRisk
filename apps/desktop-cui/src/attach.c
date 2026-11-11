#include "attach.h"
#include "core.h"
#include "richtext.h"
#include "msgui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

static double json_num(const jval *o, const char *k, double fb) { const jval *v = json_get(o, k); return v && v->type == J_NUM ? v->num : fb; }

/* ---------- base64 (RGBA das miniaturas) ---------- */
static unsigned char *b64_decode(const char *in, size_t *outn) {
    static signed char T[256]; static bool init;
    if (!init) { memset(T, -1, sizeof T); const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; for (int i = 0; i < 64; i++) T[(unsigned char)a[i]] = (signed char)i; init = true; }
    size_t n = strlen(in); unsigned char *o = malloc(n / 4 * 3 + 4); if (!o) return NULL;
    size_t k = 0; unsigned acc = 0; int bits = 0;
    for (size_t i = 0; i < n; i++) { int v = T[(unsigned char)in[i]]; if (v < 0) continue; acc = (acc << 6) | (unsigned)v; bits += 6; if (bits >= 8) { bits -= 8; o[k++] = (unsigned char)(acc >> bits); acc &= (1u << bits) - 1; } }
    *outn = k; return o;
}

typedef struct { unsigned char *rgba; int w, h; cui_texture tex; } thumb_t;
static void thumb_from_json(thumb_t *t, const jval *th) {
    if (!th || th->type != J_OBJ) return;
    int w = (int)json_num(th, "w", 0), h = (int)json_num(th, "h", 0);
    size_t n = 0; unsigned char *px = b64_decode(json_str(th, "rgba", ""), &n);
    if (px && w > 0 && h > 0 && n >= (size_t)w * (size_t)h * 4) { t->rgba = px; t->w = w; t->h = h; } else free(px);
}
static cui_texture thumb_tex(cui_ctx *ui, thumb_t *t) {
    if (!t->tex && t->rgba) { t->tex = cui_image_create(ui, t->w, t->h, t->rgba); free(t->rgba); t->rgba = NULL; }
    return t->tex;
}

/* ---------- transferências ---------- */
#define NXF 8
typedef enum { XF_OFFER, XF_RUN, XF_DONE, XF_CANCELLED, XF_FAILED, XF_WAIT, XF_PAUSED } xf_state;   /* OFFER: oferta recebida (Baixar/Recusar); WAIT: aguardando o aceite do outro lado */
typedef struct { bool used; char id[48]; char name[104]; double size, done; xf_state st; bool mine; char path[MAX_PATH]; char err[120]; thumb_t th; } xf_t;
static xf_t g_xf[NXF];
static char g_open_path[MAX_PATH]; static bool g_open_dlg, g_gal_dlg, g_demo_tl;
static void tl_add(const app_state_t *s, const char *xid, const xf_t *x);

static xf_t *xf_find(const char *id, bool create) {
    for (int i = 0; i < NXF; i++) if (g_xf[i].used && !strcmp(g_xf[i].id, id)) return &g_xf[i];
    if (!create) return NULL;
    int pick = -1;
    for (int i = 0; i < NXF; i++) if (!g_xf[i].used) { pick = i; break; }
    if (pick < 0) for (int i = 0; i < NXF; i++) if (g_xf[i].st != XF_RUN && g_xf[i].st != XF_OFFER && g_xf[i].st != XF_WAIT && g_xf[i].st != XF_PAUSED) { pick = i; break; }
    if (pick < 0) return NULL;
    free(g_xf[pick].th.rgba); memset(&g_xf[pick], 0, sizeof g_xf[pick]); g_xf[pick].used = true; snprintf(g_xf[pick].id, sizeof g_xf[pick].id, "%s", id);
    return &g_xf[pick];
}

/* ---------- pré-visualização de links ---------- */
#define NLK 16
typedef struct { bool used; char url[220]; char rid[24]; int st; char title[150], desc[310], site[70]; thumb_t th; } lk_t;   /* st: 0 buscando, 1 ok, 2 falhou */
static lk_t g_lk[NLK]; static int g_lk_next;

static lk_t *lk_find(const char *url) { for (int i = 0; i < NLK; i++) if (g_lk[i].used && !strcmp(g_lk[i].url, url)) return &g_lk[i]; return NULL; }
static lk_t *lk_new(const char *url) {
    lk_t *l = &g_lk[g_lk_next++ % NLK]; free(l->th.rgba); memset(l, 0, sizeof *l);
    l->used = true; snprintf(l->url, sizeof l->url, "%s", url); snprintf(l->rid, sizeof l->rid, "lk%d", g_lk_next); return l;
}
static bool first_url(const char *t, char *out, size_t cap) {
    const char *p = strstr(t, "https://"); if (!p) return false;
    size_t n = 0; while (p[n] && !isspace((unsigned char)p[n]) && p[n] != '<' && p[n] != '>' && p[n] != '"') n++;
    while (n > 8 && strchr(".,;)]!?'", p[n - 1])) n--;
    if (n <= 8 || n >= cap) return false;
    memcpy(out, p, n); out[n] = 0; return true;
}

/* ---------- estados de envio, digitando, busca ---------- */
#define NST 64
static struct { char id[48]; int st; } g_stt[NST]; static int g_stt_n;
static char g_inflight[48];
#define NTY 4
static struct { char name[96]; DWORD until; } g_ty[NTY];
static DWORD g_typing_sent;
static char g_search[64]; static bool g_search_on;
static bool g_demo_attach;

void attach_mark(const char *id, int st) {
    for (int i = 0; i < g_stt_n; i++) if (!strcmp(g_stt[i].id, id)) { g_stt[i].st = st; return; }
    int k = g_stt_n < NST ? g_stt_n++ : 0;
    if (k == 0 && g_stt_n >= NST) memmove(&g_stt[0], &g_stt[1], sizeof g_stt[0] * (NST - 1)), k = NST - 1;
    snprintf(g_stt[k].id, sizeof g_stt[k].id, "%s", id); g_stt[k].st = st;
}
int attach_state(const char *id) { for (int i = 0; i < g_stt_n; i++) if (!strcmp(g_stt[i].id, id)) return g_stt[i].st; return 0; }
void attach_set_inflight(const char *id) { snprintf(g_inflight, sizeof g_inflight, "%s", id); attach_mark(id, ATT_SENDING); }
void attach_inflight_done(app_state_t *s, bool ok) {
    (void)s;
    if (!g_inflight[0]) return;
    attach_mark(g_inflight, ok ? ATT_SENT : ATT_FAILED); g_inflight[0] = 0;
}

/* ---------- eventos do core ---------- */
bool attach_event(app_state_t *s, const char *ev, const jval *j) {
    if (!strcmp(ev, "file")) {
        const char *kind = json_str(j, "kind", ""), *id = json_str(j, "id", "");
        if (!id[0]) { snprintf(s->notice, sizeof s->notice, "Anexo: %s", json_str(j, "error", "falhou")); return true; }
        xf_t *x = xf_find(id, !strcmp(kind, "offer") || !strcmp(kind, "failed")); if (!x) return false;
        const jval *mn = json_get(j, "mine"); if (mn && mn->type == J_BOOL) x->mine = mn->b;
        if (json_str(j, "name", "")[0]) snprintf(x->name, sizeof x->name, "%s", json_str(j, "name", ""));
        if (json_get(j, "size")) x->size = json_num(j, "size", x->size);
        if (!strcmp(kind, "offer")) { x->st = x->mine ? XF_WAIT : XF_OFFER; if (json_str(j, "path", "")[0]) snprintf(x->path, sizeof x->path, "%s", json_str(j, "path", "")); }
        else if (!strcmp(kind, "accepted") || !strcmp(kind, "resumed")) x->st = XF_RUN;
        else if (!strcmp(kind, "paused")) x->st = XF_PAUSED;
        else if (!strcmp(kind, "progress")) { x->done = json_num(j, "done", x->done); if (x->st != XF_PAUSED) x->st = XF_RUN; }
        else if (!strcmp(kind, "done")) {
            x->st = XF_DONE; x->done = x->size;
            if (json_str(j, "path", "")[0]) snprintf(x->path, sizeof x->path, "%s", json_str(j, "path", ""));
            thumb_from_json(&x->th, json_get(j, "thumb"));
            tl_add(s, id, x);
        } else if (!strcmp(kind, "cancelled")) x->st = XF_CANCELLED;
        else if (!strcmp(kind, "failed")) { x->st = XF_FAILED; snprintf(x->err, sizeof x->err, "%s", json_str(j, "error", "falhou")); }
        return true;
    }
    if (!strcmp(ev, "link.preview")) {
        for (int i = 0; i < NLK; i++) if (g_lk[i].used && !strcmp(g_lk[i].rid, json_str(j, "id", ""))) {
            lk_t *l = &g_lk[i]; const jval *ok = json_get(j, "ok");
            if (ok && ok->type == J_BOOL && ok->b) {
                l->st = 1; snprintf(l->title, sizeof l->title, "%s", json_str(j, "title", "")); snprintf(l->desc, sizeof l->desc, "%s", json_str(j, "description", ""));
                snprintf(l->site, sizeof l->site, "%s", json_str(j, "site", "")); thumb_from_json(&l->th, json_get(j, "image"));
            } else l->st = 2;
            return true;
        }
        return false;
    }
    if (!strcmp(ev, "chat.typing")) {
        const char *fp = json_str(j, "from_fp", "");
        char nm[96] = "";
        for (int i = 0; i < 8; i++) if (s->rtc_peers[i].id[0] && !strcmp(s->rtc_peers[i].fp, fp)) snprintf(nm, sizeof nm, "%s", s->rtc_peers[i].name);
        if (!nm[0]) snprintf(nm, sizeof nm, "Alguém");
        int slot = -1; for (int i = 0; i < NTY; i++) if (!strcmp(g_ty[i].name, nm)) slot = i;
        for (int i = 0; slot < 0 && i < NTY; i++) if ((int)(g_ty[i].until - GetTickCount()) <= 0) slot = i;
        if (slot < 0) slot = 0;
        snprintf(g_ty[slot].name, sizeof g_ty[slot].name, "%s", nm); g_ty[slot].until = GetTickCount() + 5000; return true;
    }
    if (!strcmp(ev, "chat.ack")) return false;   /* confirmação de entrega do canal de DM do core: a UI ainda não envia mensagens por ele */
    return false;
}

/* ---------- enviar anexo ---------- */
#define MAX_SEND_BATCH 10   /* arquivos por seleção/soltura; o resto é ignorado com aviso */
static void send_path(app_state_t *s, const char *path) {
    char esc[2 * MAX_PATH + 8], line[2 * MAX_PATH + 48]; (void)s;
    json_escape(path, esc, sizeof esc);
    snprintf(line, sizeof line, "{\"cmd\":\"file.send\",\"path\":\"%s\"}", esc);
    core_send(line);
}
static bool can_send(app_state_t *s) {
    if (core_state() != CORE_READY || !s->in_room) { snprintf(s->notice, sizeof s->notice, "Entre numa sala de voz para enviar anexos aos participantes."); return false; }
    return true;
}
/* seletor nativo com seleção múltipla (o Electron aceitava vários arquivos de uma vez) */
void attach_pick(app_state_t *s) {
    if (!can_send(s)) return;
    static char buf[32768]; buf[0] = 0;
    OPENFILENAMEA o; memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o; o.hwndOwner = GetActiveWindow(); o.lpstrFile = buf; o.nMaxFile = sizeof buf;
    o.lpstrTitle = "Enviar arquivos"; o.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&o)) return;
    const char *dir = buf, *p = buf + strlen(buf) + 1; int n = 0;
    if (!*p) { send_path(s, dir); return; }                        /* um arquivo só: buf já é o caminho completo */
    for (; *p; p += strlen(p) + 1) {
        if (n++ >= MAX_SEND_BATCH) { snprintf(s->notice, sizeof s->notice, "Enviei os primeiros %d arquivos; envie o resto em outra seleção.", MAX_SEND_BATCH); break; }
        char full[MAX_PATH]; snprintf(full, sizeof full, "%s/%s", dir, p); send_path(s, full);
    }
}
/* arquivos soltos na janela (WM_DROPFILES → cui_dropped_file); pastas são ignoradas */
static void take_drops(cui_ctx *ui, app_state_t *s) {
    int n = cui_dropped_file_count(ui); if (n <= 0 || !can_send(s)) return;
    int sent = 0;
    for (int i = 0; i < n; i++) {
        const char *p = cui_dropped_file(ui, i); if (!p || !*p) continue;
        DWORD at = GetFileAttributesA(p); if (at == INVALID_FILE_ATTRIBUTES || (at & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (sent >= MAX_SEND_BATCH) { snprintf(s->notice, sizeof s->notice, "Enviei os primeiros %d arquivos; solte o resto de novo.", MAX_SEND_BATCH); break; }
        send_path(s, p); sent++;
    }
}

static void xf_cancel(const xf_t *x) { char line[160]; snprintf(line, sizeof line, "{\"cmd\":\"file.cancel\",\"id\":\"%s\"}", x->id); core_send(line); }

static const char *fmt_size(double b, char *o, size_t n) {
    if (b >= 1048576) snprintf(o, n, "%.1f MB", b / 1048576); else if (b >= 1024) snprintf(o, n, "%.0f KB", b / 1024); else snprintf(o, n, "%.0f B", b);
    return o;
}

static bool risky_ext(const char *p) {
    const char *e = strrchr(p, '.'); if (!e) return false;
    static const char *R[] = { ".exe", ".bat", ".cmd", ".com", ".ps1", ".msi", ".scr", ".js", ".jse", ".vbs", ".vbe", ".wsf", ".lnk", ".hta", ".jar", ".reg", ".dll", NULL };
    for (int i = 0; R[i]; i++) if (!_stricmp(e, R[i])) return true;
    return false;
}

/* ---------- anexos na conversa: itens da linha do tempo, guardados neste computador (anexos.tsv) ----------
 * Cada transferência concluída vira um cartão na conversa aberta, na posição do horário, e continua lá depois de reiniciar. */
#define NTL 200
typedef struct { bool used; char xid[48], conv[48], when[40], name[104], path[MAX_PATH]; double size; bool mine; } tl_t;
static tl_t g_tl[NTL]; static bool g_tl_loaded;
static bool tl_path(char *out, size_t cap) {
    char ad[MAX_PATH]; if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, ad) != S_OK) return false;
    char dir[MAX_PATH]; snprintf(dir, sizeof dir, "%s\\Briks-CUI", ad); CreateDirectoryA(dir, NULL);
    snprintf(out, cap, "%s\\anexos.tsv", dir); return true;
}
static void tl_clean(char *s) { for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' '; }
static void tl_save(void) {
    char p[MAX_PATH]; if (g_demo_tl || !tl_path(p, sizeof p)) return;
    FILE *f = fopen(p, "wb"); if (!f) return;
    for (int i = 0; i < NTL; i++) if (g_tl[i].used)
        fprintf(f, "%s\t%s\t%s\t%s\t%d\t%.0f\t%s\n", g_tl[i].xid, g_tl[i].conv, g_tl[i].when, g_tl[i].name, g_tl[i].mine ? 1 : 0, g_tl[i].size, g_tl[i].path);
    fclose(f);
}
static void tl_load(void) {
    if (g_tl_loaded) return; g_tl_loaded = true;
    char p[MAX_PATH]; if (g_demo_tl || !tl_path(p, sizeof p)) return;
    FILE *f = fopen(p, "rb"); if (!f) return;
    char ln[1400]; int n = 0;
    while (n < NTL && fgets(ln, sizeof ln, f)) {
        char *fld[7]; int k = 0; char *q = ln; ln[strcspn(ln, "\r\n")] = 0;
        while (k < 7) { fld[k++] = q; char *t = strchr(q, '\t'); if (!t) break; *t = 0; q = t + 1; }
        if (k < 7) continue;
        tl_t *t = &g_tl[n++]; memset(t, 0, sizeof *t); t->used = true;
        snprintf(t->xid, sizeof t->xid, "%s", fld[0]); snprintf(t->conv, sizeof t->conv, "%s", fld[1]); snprintf(t->when, sizeof t->when, "%s", fld[2]);
        snprintf(t->name, sizeof t->name, "%s", fld[3]); t->mine = fld[4][0] == '1'; t->size = atof(fld[5]); snprintf(t->path, sizeof t->path, "%s", fld[6]);
    }
    fclose(f);
}
static const char *tl_conv(const app_state_t *s) {
    if (s->active_group >= 0 && s->active_group < s->group_count) return s->groups[s->active_group].id;
    if (s->active_friend >= 0 && s->active_friend < s->friend_count) return s->friends[s->active_friend].id;
    return "";
}
static void tl_add(const app_state_t *s, const char *xid, const xf_t *x) {
    tl_load();
    for (int i = 0; i < NTL; i++) if (g_tl[i].used && !strcmp(g_tl[i].xid, xid)) return;   /* já registrado */
    int slot = -1; for (int i = 0; i < NTL; i++) if (!g_tl[i].used) { slot = i; break; }
    if (slot < 0) { memmove(&g_tl[0], &g_tl[1], sizeof g_tl[0] * (NTL - 1)); slot = NTL - 1; }   /* cheio: descarta o mais antigo */
    tl_t *t = &g_tl[slot]; memset(t, 0, sizeof *t); t->used = true;
    snprintf(t->xid, sizeof t->xid, "%s", xid); snprintf(t->conv, sizeof t->conv, "%s", tl_conv(s));
    SYSTEMTIME u; GetSystemTime(&u);
    snprintf(t->when, sizeof t->when, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", u.wYear, u.wMonth, u.wDay, u.wHour, u.wMinute, u.wSecond, u.wMilliseconds);
    snprintf(t->name, sizeof t->name, "%s", x->name[0] ? x->name : "arquivo"); tl_clean(t->name);
    snprintf(t->path, sizeof t->path, "%s", x->path); tl_clean(t->path);
    t->size = x->size; t->mine = x->mine; tl_save();
}
/* ordena por horário (ISO UTC compara como texto): desenha os itens desta conversa com horário < until (NULL = todos) a partir de *cur */
void attach_timeline(cui_ctx *ui, app_state_t *s, int *cur, const char *until) {
    tl_load();
    const char *cv = tl_conv(s);
    for (; *cur < NTL; (*cur)++) {
        tl_t *t = &g_tl[*cur]; if (!t->used || (strcmp(t->conv, cv) && t->conv[0] != '*')) continue;   /* "*" = demonstração: aparece em qualquer conversa */
        if (until && strcmp(t->when, until) >= 0) break;
        if (!attach_search_match(t->name)) continue;
        char a[24]; cui_push_id_int(ui, 9000 + *cur);
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .justify = t->mine ? CUI_JUSTIFY_END : CUI_JUSTIFY_START) {
            cui_box(ui, .dir = CUI_ROW, .max_width = 420, .pad = 10, .gap = 10, .radius = 16, .bg = t->mine ? g_th.bubble_own : B_GLASS_CARD, .border = 1,
                    .border_color = t->mine ? CUI_RGBA(0xffffff26) : B_GLASS_BORDER, .align = CUI_ALIGN_CENTER) {
                cui_box(ui, .width = CUI_PX(40), .height = CUI_PX(40), .radius = 10, .bg = CUI_RGBA(0x8b5cf633), .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) { cui_icon(ui, CUI_ICON_PAPERCLIP, 18, B_C3); }
                cui_box(ui, .gap = 2, .max_width = 250) {
                    cui_text(ui, t->name, .font = CUI_FONT_MEDIUM, .size = 13, .nowrap = true);
                    char l[80]; snprintf(l, sizeof l, "%s · %s", t->mine ? "Enviado" : "Recebido", fmt_size(t->size, a, sizeof a));
                    cui_text(ui, l, .size = 11, .color = t->mine ? CUI_RGBA(0xffffffb0) : B_MUTED);
                }
                if (t->path[0]) {
                    if (!t->mine && cui_button(ui, "Abrir…", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) { snprintf(g_open_path, sizeof g_open_path, "%s", t->path); g_open_dlg = true; }
                    if (cui_icon_button(ui, CUI_ICON_FOLDER, .size = CUI_SIZE_SM, .tooltip = "Mostrar na pasta")) {
                        char arg[MAX_PATH + 16]; snprintf(arg, sizeof arg, "/select,\"%s\"", t->path); ShellExecuteA(NULL, "open", "explorer.exe", arg, NULL, SW_SHOWNORMAL);
                    }
                }
            }
        }
        cui_pop_id(ui);
    }
}

void attach_strip(cui_ctx *ui, app_state_t *s) {
    take_drops(ui, s);
    bool any = false; for (int i = 0; i < NXF; i++) if (g_xf[i].used) any = true;
    if (!any) return;
    int imgs = 0; for (int i = 0; i < NXF; i++) if (g_xf[i].used && g_xf[i].st == XF_DONE && (g_xf[i].th.tex || g_xf[i].th.rgba)) imgs++;
    if (imgs) { cui_push_id(ui, "gal"); if (cui_button(ui, "Galeria de imagens", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) g_gal_dlg = true; cui_pop_id(ui); }
    cui_box(ui, .width = CUI_FILL, .max_height = 150, .scroll_y = true, .gap = 6) {   /* no máximo ~2 linhas visíveis; o resto rola */
        for (int i = 0; i < NXF; i++) {
            xf_t *x = &g_xf[i]; if (!x->used) continue;
            cui_push_id_int(ui, 8000 + i);
            char a[24], b[24], line[200];
            cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad = 8, .gap = 10, .radius = 14, .bg = B_GLASS_CARD, .border = 1, .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER, .clip = true) {
                cui_texture tx = x->th.tex || x->th.rgba ? thumb_tex(ui, &x->th) : 0;
                if (tx) cui_image(ui, tx, .width = CUI_PX(44), .height = CUI_PX(44), .radius = 10);
                else cui_box(ui, .width = CUI_PX(44), .height = CUI_PX(44), .radius = 10, .bg = CUI_RGBA(0x8b5cf633), .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) { cui_icon(ui, CUI_ICON_PAPERCLIP, 20, B_C3); }
                cui_box(ui, .width = CUI_FILL, .gap = 3) {
                    cui_text(ui, x->name[0] ? x->name : "arquivo", .font = CUI_FONT_MEDIUM, .size = 13);
                    if (x->st == XF_OFFER) cui_textf(ui, "%s quer enviar · %s · nada foi baixado", "Alguém da sala", fmt_size(x->size, a, sizeof a));
                    else if (x->st == XF_WAIT) cui_textf(ui, "Aguardando o aceite · %s", fmt_size(x->size, a, sizeof a));
                    else if (x->st == XF_RUN || x->st == XF_PAUSED) {
                        float f = x->size > 0 ? (float)(x->done / x->size) : 0;
                        snprintf(line, sizeof line, "%s %s de %s%s", x->mine ? "Enviando" : "Recebendo", fmt_size(x->done, a, sizeof a), fmt_size(x->size, b, sizeof b), x->st == XF_PAUSED ? " · pausado" : "");
                        cui_text(ui, line, .size = 11, .color = B_MUTED);
                        cui_progress(ui, f, .height = 5);
                    } else if (x->st == XF_DONE) {
                        snprintf(line, sizeof line, x->mine ? "Enviado · %s" : "Recebido · %s · não foi aberto", fmt_size(x->size, a, sizeof a));
                        cui_text(ui, line, .size = 11, .color = B_MUTED);
                    } else if (x->st == XF_CANCELLED) cui_text(ui, "Cancelado", .size = 11, .color = B_MUTED);
                    else cui_textf(ui, "Falhou: %s", x->err);
                }
                if (x->st == XF_OFFER) {
                    if (cui_button(ui, "Baixar", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM)) { char l[160]; snprintf(l, sizeof l, "{\"cmd\":\"file.accept\",\"id\":\"%s\"}", x->id); core_send(l); x->st = XF_RUN; }
                    if (cui_button(ui, "Recusar", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) { xf_cancel(x); x->st = XF_CANCELLED; }
                } else if (x->st == XF_RUN || x->st == XF_PAUSED || x->st == XF_WAIT) {
                    if (x->st != XF_WAIT && cui_button(ui, x->st == XF_PAUSED ? "Retomar" : "Pausar", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) {
                        char l[160]; snprintf(l, sizeof l, "{\"cmd\":\"file.pause\",\"id\":\"%s\",\"paused\":%s}", x->id, x->st == XF_PAUSED ? "false" : "true"); core_send(l);
                        x->st = x->st == XF_PAUSED ? XF_RUN : XF_PAUSED;
                    }
                    if (cui_button(ui, "Cancelar", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) { xf_cancel(x); x->st = XF_CANCELLED; }
                } else {
                    if (x->st == XF_FAILED && x->mine && x->path[0] && cui_button(ui, "Tentar de novo", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) {
                        char esc[2 * MAX_PATH + 8], l[2 * MAX_PATH + 48]; json_escape(x->path, esc, sizeof esc); snprintf(l, sizeof l, "{\"cmd\":\"file.send\",\"path\":\"%s\"}", esc); core_send(l);
                        free(x->th.rgba); if (x->th.tex) cui_image_destroy(ui, x->th.tex); memset(x, 0, sizeof *x);
                    }
                    if (x->st == XF_DONE && !x->mine) {
                        if (cui_button(ui, "Abrir…", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) { snprintf(g_open_path, sizeof g_open_path, "%s", x->path); g_open_dlg = true; }
                        if (cui_icon_button(ui, CUI_ICON_FOLDER, .size = CUI_SIZE_SM, .tooltip = "Mostrar na pasta")) {
                            char arg[MAX_PATH + 16]; snprintf(arg, sizeof arg, "/select,\"%s\"", x->path); ShellExecuteA(NULL, "open", "explorer.exe", arg, NULL, SW_SHOWNORMAL);
                        }
                    }
                    if (cui_icon_button(ui, CUI_ICON_X, .size = CUI_SIZE_SM, .tooltip = "Dispensar")) { free(x->th.rgba); if (x->th.tex) cui_image_destroy(ui, x->th.tex); memset(x, 0, sizeof *x); }
                }
            }
            cui_pop_id(ui);
        }
    }
}

void attach_overlay(cui_ctx *ui, app_state_t *s) {
    (void)s;
    cui_vec2 vp = cui_viewport_size(ui);
    float dw = fminf(420.0f, fmaxf(vp.x - 48.0f, 260.0f));
    cui_dialog(ui, "Galeria de imagens", &g_gal_dlg, .width = fminf(520.0f, fmaxf(vp.x - 48.0f, 260.0f))) {
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .gap = 8, .wrap = true, .max_height = fmaxf(vp.y - 240.0f, 120.0f), .scroll_y = true) {
            for (int i = 0; i < NXF; i++) {
                xf_t *x = &g_xf[i]; if (!x->used || x->st != XF_DONE || !(x->th.tex || x->th.rgba)) continue;
                cui_push_id_int(ui, 8700 + i);
                cui_texture tx = thumb_tex(ui, &x->th);
                cui_box(ui, .pad = 6, .gap = 4, .radius = 12, .bg = B_GLASS_CARD, .border = 1, .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER) {
                    cui_image(ui, tx, .width = CUI_PX((float)x->th.w * 1.4f), .height = CUI_PX((float)x->th.h * 1.4f), .radius = 8);
                    cui_text(ui, x->name, .size = 11, .color = B_MUTED, .nowrap = true, .width = CUI_PX(130));
                    if (x->path[0] && cui_button(ui, "Mostrar na pasta", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) {
                        char arg[MAX_PATH + 16]; snprintf(arg, sizeof arg, "/select,\"%s\"", x->path); ShellExecuteA(NULL, "open", "explorer.exe", arg, NULL, SW_SHOWNORMAL);
                    }
                }
                cui_pop_id(ui);
            }
        }
    }
    cui_dialog(ui, "Abrir arquivo recebido?", &g_open_dlg, .width = dw) {
        bool risky = risky_ext(g_open_path);
        const char *nm = strrchr(g_open_path, '\\'); nm = nm ? nm + 1 : g_open_path;
        cui_box(ui, .width = CUI_FILL, .pad = 10, .radius = 12, .bg = CUI_RGBA(0x00000030)) { cui_text(ui, nm); }
        cui_text(ui, risky ? "Este tipo de arquivo pode executar programas. Só abra se você confia em quem enviou e esperava por ele."
                           : "O arquivo veio de outra pessoa e ainda não foi aberto. Abra só se você confia em quem enviou.", .color = risky ? CUI_RGB(0xfca5a5) : B_MUTED, .size = 13);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Cancelar", .variant = CUI_BUTTON_SECONDARY)) g_open_dlg = false;
            if (cui_button(ui, "Abrir", .variant = risky ? CUI_BUTTON_DANGER : CUI_BUTTON_PRIMARY)) { ShellExecuteA(NULL, "open", g_open_path, NULL, NULL, SW_SHOWNORMAL); g_open_dlg = false; }
        }
    }
}

/* ---------- cartão de link ---------- */
void attach_link_card(cui_ctx *ui, app_state_t *s, const char *text) {
    (void)s;
    char url[220]; if (!first_url(text, url, sizeof url)) return;
    lk_t *l = lk_find(url);
    if (!l) {
        l = lk_new(url);
        if (g_demo_attach || core_state() != CORE_READY) { l->st = 2; if (g_demo_attach) l->st = 0; }
        else { char e[500], line[640]; json_escape(url, e, sizeof e); snprintf(line, sizeof line, "{\"cmd\":\"link.preview\",\"id\":\"%s\",\"url\":\"%s\"}", l->rid, e); core_send(line); }
    }
    if (l->st != 1) return;   /* buscando ou sem prévia: o link continua só como texto */
    cui_push_id(ui, "lk");
    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .max_width = 420, .pad = 8, .gap = 10, .radius = 12, .bg = CUI_RGBA(0x00000040), .border = 1, .border_color = CUI_RGBA(0xffffff22), .clip = true) {
        cui_box(ui, .width = CUI_PX(3), .height = CUI_FILL, .radius = 2, .bg = B_C3) {}
        cui_texture tx = l->th.tex || l->th.rgba ? thumb_tex(ui, &l->th) : 0;
        cui_box(ui, .width = CUI_FILL, .gap = 2) {
            if (l->site[0]) cui_text(ui, l->site, .size = 11, .color = B_MUTED);
            if (l->title[0]) cui_text(ui, l->title, .font = CUI_FONT_MEDIUM, .size = 13, .nowrap = true, .width = CUI_FILL);
            if (l->desc[0]) { char d[170]; snprintf(d, sizeof d, "%.160s%s", l->desc, strlen(l->desc) > 160 ? "…" : ""); cui_text(ui, d, .size = 12, .color = B_MUTED); }
        }
        if (tx) cui_image(ui, tx, .width = CUI_PX(l->th.w ? (float)l->th.w : 64), .height = CUI_PX(l->th.h ? (float)l->th.h : 64), .radius = 10);
    }
    cui_pop_id(ui);
}

/* ---------- digitando ---------- */
void attach_typing_ping(app_state_t *s) {
    DWORD now = GetTickCount();
    if (!s->in_room || core_state() != CORE_READY || now - g_typing_sent < 3000) return;
    g_typing_sent = now; core_send("{\"cmd\":\"typing.send\"}");
}
const char *attach_typing_label(const app_state_t *s) {
    (void)s; static char buf[220];
    const char *n[NTY]; int k = 0;
    for (int i = 0; i < NTY; i++) if (g_ty[i].name[0] && (int)(g_ty[i].until - GetTickCount()) > 0) n[k++] = g_ty[i].name;
    if (k == 0) return NULL;
    if (k == 1) snprintf(buf, sizeof buf, "%s está digitando…", n[0]);
    else if (k == 2) snprintf(buf, sizeof buf, "%s e %s estão digitando…", n[0], n[1]);
    else snprintf(buf, sizeof buf, "%s e mais %d pessoas estão digitando…", n[0], k - 1);
    return buf;
}

/* ---------- busca ---------- */
void attach_search_button(cui_ctx *ui) {
    if (g_search_on) {
        cui_input(ui, "msgsearch", g_search, sizeof g_search, .placeholder = "Buscar nesta conversa", .width = CUI_PX(190));
        if (cui_icon_button(ui, CUI_ICON_X, .size = CUI_SIZE_SM, .tooltip = "Fechar busca")) { g_search_on = false; g_search[0] = 0; }
    } else if (cui_icon_button(ui, CUI_ICON_SEARCH, .size = CUI_SIZE_SM, .tooltip = "Buscar mensagens")) g_search_on = true;
}
bool attach_search_match(const char *text) {
    if (!g_search_on || !g_search[0]) return true;
    char a[1200], b[64]; size_t i = 0; for (; text[i] && i < sizeof a - 1; i++) a[i] = (char)tolower((unsigned char)text[i]); a[i] = 0;
    for (i = 0; g_search[i] && i < sizeof b - 1; i++) b[i] = (char)tolower((unsigned char)g_search[i]); b[i] = 0;
    return strstr(a, b) != NULL;
}

/* ---------- etiqueta de estado ---------- */
void attach_state_tag(cui_ctx *ui, app_state_t *s, const msg_t *m) {
    (void)s;
    int st = attach_state(m->id);
    if (st == ATT_SENDING) cui_text(ui, "enviando…", .size = 10, .color = CUI_RGBA(0xffffffb0));
    else if (st == ATT_FAILED) {
        cui_push_id(ui, "fail");
        if (cui_button(ui, "não enviada · reenviar", .variant = CUI_BUTTON_DANGER, .size = CUI_SIZE_SM)) {
            for (int i = 0; i < s->msg_count; i++) if (!strcmp(s->msgs[i].id, m->id)) {
                snprintf(s->draft, sizeof s->draft, "%s", m->content);
                memmove(&s->msgs[i], &s->msgs[i + 1], sizeof(msg_t) * (size_t)(s->msg_count - i - 1)); s->msg_count--;
                app_send_message(s); break;
            }
        }
        cui_pop_id(ui);
    }
}

/* ---------- demonstração (capturas) ---------- */
void attach_demo(app_state_t *s) {
    (void)s; g_demo_attach = true;
    lk_t *l = lk_new("https://exemplo.com/artigo-p2p"); l->st = 1;
    snprintf(l->site, sizeof l->site, "exemplo.com"); snprintf(l->title, sizeof l->title, "Como funciona o P2P sem servidor central");
    snprintf(l->desc, sizeof l->desc, "Um passeio rápido por WebRTC, DataChannels e assinaturas, com exemplos que você pode testar.");
    l->th.w = 64; l->th.h = 64; l->th.rgba = malloc(64 * 64 * 4);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) { unsigned char *p = l->th.rgba + (y * 64 + x) * 4; p[0] = (unsigned char)(80 + x * 2); p[1] = (unsigned char)(60 + y); p[2] = 220; p[3] = 255; }
    xf_t *a = xf_find("demo-a", true); snprintf(a->name, sizeof a->name, "relatorio-final.pdf"); a->size = 8.4 * 1048576; a->done = 3.5 * 1048576; a->st = XF_RUN; a->mine = false;
    xf_t *b = xf_find("demo-b", true); snprintf(b->name, sizeof b->name, "planilha.xlsx"); b->size = 220 * 1024; b->done = b->size; b->st = XF_DONE; b->mine = false; snprintf(b->path, sizeof b->path, "C:\\Users\\demo\\planilha.xlsx");
    xf_t *d = xf_find("demo-d", true); snprintf(d->name, sizeof d->name, "video-ferias.mp4"); d->size = 310.0 * 1048576; d->st = XF_OFFER; d->mine = false;
    xf_t *c = xf_find("demo-c", true); snprintf(c->name, sizeof c->name, "foto.png"); c->size = 540 * 1024; c->st = XF_FAILED; snprintf(c->err, sizeof c->err, "conexão encerrada");
    g_demo_tl = true; g_tl_loaded = true;
    tl_t *t0 = &g_tl[0]; memset(t0, 0, sizeof *t0); t0->used = true; snprintf(t0->xid, sizeof t0->xid, "d1"); snprintf(t0->conv, sizeof t0->conv, "*"); snprintf(t0->when, sizeof t0->when, "2000-01-01T00:00:00.000Z");
    snprintf(t0->name, sizeof t0->name, "contrato-assinado.pdf"); t0->size = 1.2 * 1048576; snprintf(t0->path, sizeof t0->path, "C:/Users/demo/contrato-assinado.pdf");
    tl_t *t1 = &g_tl[1]; *t1 = *t0; snprintf(t1->xid, sizeof t1->xid, "d2"); snprintf(t1->name, sizeof t1->name, "capturas-da-tela.zip"); t1->size = 14.5 * 1048576; t1->mine = true;
    snprintf(g_ty[0].name, sizeof g_ty[0].name, "Marina Costa"); g_ty[0].until = GetTickCount() + 600000;
}

/* ---------- texto rico, menções, agrupamento (paridade com SafeMessageContent/MessageComposer do Electron) ---------- */
void attach_rich(cui_ctx *ui, const char *id, const char *text) {
    /* o cui_markdown lê o texto só ao desenhar (fim do quadro): cada chamada precisa do seu buffer, que vale até o próximo ciclo do anel */
    static char ring[256][1700]; static unsigned ri;
    char *md = ring[ri++ % 256]; rich_prepare(text, md, sizeof ring[0]);
    cui_markdown_result r = cui_markdown(ui, id, md, .fit_content = true, .compact = true);
    if (r.link && rich_safe_url(r.link)) ShellExecuteA(NULL, "open", r.link, NULL, NULL, SW_SHOWNORMAL);   /* só http(s), como o openExternal do Electron */
}

static bool starts_ci(const char *s, const char *q) { while (*q) { if (tolower((unsigned char)*s) != tolower((unsigned char)*q)) return false; s++; q++; } return true; }

void attach_mentions(cui_ctx *ui, app_state_t *s) {
    char q[96]; int at;
    if (!rich_mention_query(s->draft, q, sizeof q, &at)) return;
    const char *cand[24]; int n = 0;
    for (int i = 0; i < 8 && n < 24; i++) if (s->rtc_peers[i].id[0] && s->rtc_peers[i].name[0]) cand[n++] = s->rtc_peers[i].name;
    for (int i = 0; i < s->friend_count && n < 24; i++) cand[n++] = s->friends[i].name;
    const char *pick[6]; int k = 0;
    for (int i = 0; i < n && k < 6; i++) {
        if (!starts_ci(cand[i], q)) continue;
        bool dup = false; for (int j = 0; j < k; j++) if (!strcmp(pick[j], cand[i])) dup = true;
        if (!dup && strcmp(cand[i], s->user_name)) pick[k++] = cand[i];
    }
    if (!k) return;
    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad = 6, .gap = 6, .radius = 14, .bg = B_GLASS_POPUP, .border = 1, .border_color = B_GLASS_BORDER, .wrap = true) {
        for (int i = 0; i < k; i++) {
            cui_push_id_int(ui, 8500 + i);
            char lab[120]; snprintf(lab, sizeof lab, "@%s", pick[i]);
            if (cui_button(ui, lab, .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) rich_apply_mention(s->draft, sizeof s->draft, pick[i]);
            cui_pop_id(ui);
        }
    }
}

bool attach_grouped(const app_state_t *s, int idx) {
    if (idx <= 0 || idx >= s->msg_count) return false;
    const msg_t *p = &s->msgs[idx - 1], *m = &s->msgs[idx];
    return rich_group_with_prev(p->author, rich_iso_ms(p->created), m->author, rich_iso_ms(m->created), msgui_has_reply(m));
}
