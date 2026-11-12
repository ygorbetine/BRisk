/* Convite de grupo enviado pela DM (cartão "Convite para o grupo X, de Fulano" com Entrar e Recusar).
 *
 * O convite viaja como uma mensagem especial na DM: `briks-invite:1|<grupo>|<nome>|<quem convida>|<código briks-grp1.…>`. Ela segue o MESMO caminho
 * da fila local das mensagens (caixa de saída neste PC, entrega quando o amigo ficar online, id único = nunca duplica). O código assinado NÃO vai
 * para o banco de mensagens nem para o servidor: na conversa fica só o texto curto `briks-invite:1|<grupo>|<nome>|<quem>` e o código fica neste
 * arquivo local (invites.dat). Entrar usa a MESMA conferência de hoje (group.verify: assinatura + destinatário); o app nunca entra sozinho. */
#include "briks_ui.h"
#include "ginvite.h"
#include "json.h"
#include "core.h"
#include "storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

#define PFX "briks-invite:1|"
#define NINV 64
#define EXPIRE_S (7 * 24 * 3600)   /* um convite recebido vale 7 dias */
typedef struct { bool used; char id[48], gid[48], name[64], inviter[64], from[48]; int st; long long created; char *code; } inv_t;
static inv_t g_inv[NINV]; static bool g_loaded; static int g_wait = -1;   /* g_wait: cartão cujo Entrar espera a conferência do core */
static char g_expect_user[48], g_expect_gid[48];                          /* convite a enviar pela DM quando o core terminar de assinar o grupo */

static void inv_path(char *out, size_t cap) { char d[MAX_PATH], h[MAX_PATH], r[MAX_PATH]; storage_dirs(d, h, r, sizeof d); snprintf(out, cap, "%s\\invites.dat", d); }
static void clean(char *o, size_t cap, const char *in) {   /* sem | nem controles: cabe no texto do convite e no arquivo */
    size_t n = 0; for (; *in && n + 1 < cap; in++) o[n++] = ((unsigned char)*in < 0x20 || *in == '|' || *in == '\t') ? ' ' : *in; o[n] = 0;
}
static void save(void) {
    char p[MAX_PATH + 16]; inv_path(p, sizeof p); FILE *f = fopen(p, "w"); if (!f) return;
    for (int i = 0; i < NINV; i++) if (g_inv[i].used) {
        const inv_t *e = &g_inv[i];
        fprintf(f, "%s\t%d\t%s\t%s\t%s\t%s\t%lld\t%s\n", e->id, e->st, e->gid, e->name, e->inviter, e->from, e->created, e->code ? e->code : "");
    }
    fclose(f);
}
static void load(void) {
    g_loaded = true; char p[MAX_PATH + 16]; inv_path(p, sizeof p); FILE *f = fopen(p, "r"); if (!f) return;
    char *line = malloc(8192); if (!line) { fclose(f); return; }
    while (fgets(line, 8192, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *fld[8]; int n = 0; char *c = line;
        while (n < 8) { fld[n++] = c; char *t = strchr(c, '\t'); if (!t) break; *t = 0; c = t + 1; }
        if (n < 8) continue;
        for (int i = 0; i < NINV; i++) if (!g_inv[i].used) {
            inv_t *e = &g_inv[i]; memset(e, 0, sizeof *e); e->used = true;
            snprintf(e->id, sizeof e->id, "%s", fld[0]); e->st = atoi(fld[1]); snprintf(e->gid, sizeof e->gid, "%s", fld[2]); snprintf(e->name, sizeof e->name, "%s", fld[3]);
            snprintf(e->inviter, sizeof e->inviter, "%s", fld[4]); snprintf(e->from, sizeof e->from, "%s", fld[5]); e->created = atoll(fld[6]); e->code = fld[7][0] ? _strdup(fld[7]) : NULL;
            break;
        }
    }
    free(line); fclose(f);
}
static inv_t *find(const char *id) { if (!g_loaded) load(); for (int i = 0; i < NINV; i++) if (g_inv[i].used && !strcmp(g_inv[i].id, id)) return &g_inv[i]; return NULL; }
static inv_t *slot(void) {
    for (int i = 0; i < NINV; i++) if (!g_inv[i].used) return &g_inv[i];
    int old = 0; for (int i = 1; i < NINV; i++) if (g_inv[i].created < g_inv[old].created) old = i;   /* cheio: sai o mais antigo */
    free(g_inv[old].code); memset(&g_inv[old], 0, sizeof g_inv[old]); return &g_inv[old];
}
static bool expired(const inv_t *e) { return e->st == GINV_PENDING && (long long)time(NULL) - e->created > EXPIRE_S; }

bool ginvite_is(const char *content) { return content && !strncmp(content, PFX, sizeof PFX - 1); }

/* separa "gid|nome|quem[|código]" */
static bool parse(const char *content, char *gid, char *name, char *inviter, const char **code) {
    const char *p = content + sizeof PFX - 1; char *o[3] = { gid, name, inviter }; const size_t cap[3] = { 48, 64, 64 };
    for (int k = 0; k < 3; k++) {
        const char *e = strchr(p, '|'); size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n >= cap[k]) n = cap[k] - 1; memcpy(o[k], p, n); o[k][n] = 0;
        if (!e) { if (k < 2) return false; p = p + strlen(p); *code = NULL; return gid[0] != 0; }
        p = e + 1;
    }
    *code = p[0] ? p : NULL; return gid[0] != 0;
}

bool ginvite_receive(app_state_t *s, const char *id, const char *from_user, const char *content, bool mine, char *shown, size_t cap) {
    (void)s; char gid[48], name[64], inv[64]; const char *code = NULL;
    if (!ginvite_is(content) || !parse(content, gid, name, inv, &code)) return false;
    snprintf(shown, cap, PFX "%s|%s|%s", gid, name, inv);   /* o que fica no banco de mensagens: sem o código */
    if (!find(id)) {
        inv_t *e = slot(); memset(e, 0, sizeof *e); e->used = true;
        snprintf(e->id, sizeof e->id, "%s", id); snprintf(e->gid, sizeof e->gid, "%s", gid); snprintf(e->name, sizeof e->name, "%s", name);
        snprintf(e->inviter, sizeof e->inviter, "%s", inv); snprintf(e->from, sizeof e->from, "%s", from_user);
        e->st = mine ? GINV_SENT : GINV_PENDING; e->created = (long long)time(NULL); e->code = (!mine && code) ? _strdup(code) : NULL;
        save();
    }
    return true;
}
const char *ginvite_summary(const char *content, char *out, size_t cap) {
    char gid[48], name[64], inv[64]; const char *code;
    if (ginvite_is(content) && parse(content, gid, name, inv, &code)) snprintf(out, cap, "Convite para o grupo %s", name); else snprintf(out, cap, "%s", content);
    return out;
}
int ginvite_state(const char *id) { inv_t *e = find(id); return !e ? -1 : expired(e) ? GINV_EXPIRED : e->st; }
int ginvite_pending_from(const char *user_id) { if (!g_loaded) load(); int n = 0; for (int i = 0; i < NINV; i++) if (g_inv[i].used && g_inv[i].st == GINV_PENDING && !expired(&g_inv[i]) && !strcmp(g_inv[i].from, user_id)) n++; return n; }
int ginvite_pending_total(void) { if (!g_loaded) load(); int n = 0; for (int i = 0; i < NINV; i++) if (g_inv[i].used && g_inv[i].st == GINV_PENDING && !expired(&g_inv[i])) n++; return n; }

/* ---- enviar ---- */
void ginvite_expect(const char *friend_user, const char *gid) { snprintf(g_expect_user, sizeof g_expect_user, "%s", friend_user); snprintf(g_expect_gid, sizeof g_expect_gid, "%s", gid); }
void ginvite_signed(app_state_t *s, const char *gid, const char *gname, const char *code) {   /* o core assinou o grupo: se era um convite a enviar, vai pela DM */
    if (!g_expect_user[0] || strcmp(g_expect_gid, gid) || !code || !code[0]) return;
    int fi = -1; for (int i = 0; i < s->friend_count; i++) if (!strcmp(s->friends[i].id, g_expect_user)) fi = i;
    g_expect_user[0] = 0; g_expect_gid[0] = 0;
    if (fi < 0) return;
    char n[64], iv[64]; clean(n, sizeof n, gname); clean(iv, sizeof iv, s->user_name[0] ? s->user_name : "Alguém");
    const size_t cap = strlen(code) + 300; char *wire = malloc(cap); if (!wire) return;
    snprintf(wire, cap, PFX "%s|%s|%s|%s", gid, n, iv, code);
    if (strlen(wire) <= 3900 && chat_dm_post(s, fi, wire)) snprintf(s->notice, sizeof s->notice, "Convite enviado a %s pela conversa. Chega quando a pessoa estiver online.", s->friends[fi].name);
    else snprintf(s->notice, sizeof s->notice, "O convite ficou grande demais para a conversa: use o botão de copiar o código.");
    free(wire);
}

/* ---- responder ---- */
static void mark(inv_t *e, int st) { e->st = st; if (st != GINV_PENDING) { free(e->code); e->code = NULL; } save(); }
void ginvite_verified(app_state_t *s, const char *gid, bool ok, const char *msg) {   /* resultado do group.verify disparado por Entrar */
    if (g_wait < 0 || !g_inv[g_wait].used) { g_wait = -1; return; }
    inv_t *e = &g_inv[g_wait]; g_wait = -1;
    if (ok && (!gid[0] || !strcmp(gid, e->gid))) mark(e, GINV_ACCEPTED);
    else if (!ok) { mark(e, GINV_EXPIRED); snprintf(s->notice, sizeof s->notice, "Convite não aceito: %s", msg && msg[0] ? msg : "inválido ou expirado."); }
}
static void inv_accept(app_state_t *s, inv_t *e) {
    for (int i = 0; i < s->group_count; i++) if (!strcmp(s->groups[i].id, e->gid)) { mark(e, GINV_ACCEPTED); return; }   /* já estou nele */
    if (!e->code) { mark(e, GINV_EXPIRED); return; }
    char *b = malloc(strlen(e->code) * 2 + 80); if (!b) return;
    char *esc = malloc(strlen(e->code) * 2 + 8); if (!esc) { free(b); return; }
    json_escape(e->code, esc, strlen(e->code) * 2 + 8);
    snprintf(b, strlen(e->code) * 2 + 80, "{\"cmd\":\"group.verify\",\"tag\":\"invite\",\"code\":\"%s\"}", esc);
    if (core_send(b)) g_wait = (int)(e - g_inv); else snprintf(s->notice, sizeof s->notice, "O motor do Briks ainda não está pronto. Tente de novo em instantes.");
    free(esc); free(b);
}

/* ---- cartão no balão ---- */
bool ginvite_card(cui_ctx *ui, app_state_t *s, const msg_t *m) {
    char gid[48], name[64], inv[64]; const char *code;
    if (!ginvite_is(m->content) || !parse(m->content, gid, name, inv, &code)) return false;
    inv_t *e = find(m->id); const int st = !e ? GINV_PENDING : expired(e) ? GINV_EXPIRED : e->st;
    cui_push_id(ui, "ginv");
    cui_column(ui, .gap = 6, .min_width = 220) {
        cui_row(ui, .gap = 6, .align = CUI_ALIGN_CENTER) { cui_icon(ui, CUI_ICON_USERS, 14, B_C3); cui_text(ui, "Convite para o grupo", .size = 11, .color = B_MUTED); }
        cui_text(ui, name, .font = CUI_FONT_MEDIUM, .size = 15, .nowrap = true);
        if (!m->mine) { char de[96]; snprintf(de, sizeof de, "de %s", inv); cui_text(ui, de, .size = 12, .color = B_MUTED, .nowrap = true); }
        if (m->mine || st == GINV_SENT) cui_text(ui, "Convite enviado. A pessoa decide se entra.", .size = 12, .color = B_MUTED);
        else if (st == GINV_PENDING && e) {
            cui_row(ui, .gap = 8) {
                if (cui_button(ui, "Entrar", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM, .loading = g_wait == (int)(e - g_inv))) inv_accept(s, e);
                if (cui_button(ui, "Recusar", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) mark(e, GINV_DECLINED);
            }
        }
        else if (st == GINV_ACCEPTED) cui_text(ui, "Aceito: você entrou no grupo", .size = 12, .color = CUI_RGB(0x7dd3fc));
        else if (st == GINV_DECLINED) cui_text(ui, "Recusado", .size = 12, .color = B_MUTED);
        else cui_text(ui, "Expirado: peça um convite novo", .size = 12, .color = CUI_RGB(0xf5d9a0));
    }
    cui_pop_id(ui);
    return true;
}
