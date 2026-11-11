/* Grupos: canais de texto e de voz, cargos (dono, administrador, membro), membros e convites, sobre os grupos P2P do backend local (/p2p/groups).
 * O grupo é um documento JSON que também carrega campos do protocolo P2P (concessões de admin assinadas, revogações, segredo de encontro...).
 * Para não estragar nada, o save relê o JSON guardado (raw), troca SÓ nome, canais, membros, administradores e versões, e devolve o resto como veio.
 * Limite honesto: aqui as mudanças valem neste computador; a entrega assinada aos outros membros é do módulo P2P de grupos (briks-core). */
#include "briks_ui.h"
#include "backend.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <bcrypt.h>

static void new_id(char *out, size_t cap) {
    unsigned char r[16]; BCryptGenRandom(NULL, r, sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    size_t n = 0; for (int i = 0; i < 16 && n + 3 <= cap; i++) n += (size_t)snprintf(out + n, cap - n, "%02x", r[i]);
}

/* ---- buffer de texto e serializador JSON (o json.h só lê) ---- */
typedef struct { char *p; size_t n, cap; } sbuf;
static void sb_put(sbuf *b, const char *s) {
    size_t l = strlen(s);
    if (b->n + l + 1 > b->cap) { b->cap = (b->n + l + 1) * 2 + 256; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, l + 1); b->n += l;
}
static void sb_str(sbuf *b, const char *s) {
    size_t cap = strlen(s) * 6 + 8; char *t = malloc(cap); json_escape(s, t, cap);
    sb_put(b, "\""); sb_put(b, t); sb_put(b, "\""); free(t);
}
static void dump(sbuf *b, const jval *v) {
    char tmp[48];
    if (!v) { sb_put(b, "null"); return; }
    switch (v->type) {
    case J_NULL: sb_put(b, "null"); break;
    case J_BOOL: sb_put(b, v->b ? "true" : "false"); break;
    case J_NUM: if (v->num == (double)(long long)v->num) snprintf(tmp, sizeof tmp, "%lld", (long long)v->num); else snprintf(tmp, sizeof tmp, "%.17g", v->num); sb_put(b, tmp); break;
    case J_STR: sb_str(b, v->str ? v->str : ""); break;
    case J_ARR: sb_put(b, "["); for (int i = 0; i < v->count; i++) { if (i) sb_put(b, ","); dump(b, &v->items[i]); } sb_put(b, "]"); break;
    case J_OBJ: sb_put(b, "{"); for (int i = 0; i < v->count; i++) { if (i) sb_put(b, ","); sb_str(b, v->keys[i]); sb_put(b, ":"); dump(b, &v->items[i]); } sb_put(b, "}"); break;
    }
}

/* ---- leitura ---- */
static const jval *find_by(const jval *arr, const char *key, const char *id) {
    for (int i = 0; i < json_len(arr); i++) { const jval *e = json_at(arr, i); if (!strcmp(json_str(e, key, ""), id)) return e; }
    return NULL;
}

void groups_load(app_state_t *s, const char *body) {
    char keep[48]; snprintf(keep, sizeof keep, "%s", s->active_group >= 0 && s->active_group < s->group_count ? s->groups[s->active_group].id : "");
    for (int i = 0; i < s->group_count; i++) { free(s->groups[i].raw); s->groups[i].raw = NULL; }
    jval *j = json_parse(body);
    s->group_count = 0;
    for (int i = 0; i < json_len(j) && s->group_count < MAX_GROUPS; i++) {
        const jval *g = json_at(j, i);
        group_t *d = &s->groups[s->group_count++]; memset(d, 0, sizeof *d);
        snprintf(d->id, sizeof d->id, "%s", json_str(g, "groupId", ""));
        snprintf(d->name, sizeof d->name, "%s", json_str(g, "name", "?"));
        snprintf(d->owner, sizeof d->owner, "%s", json_str(g, "ownerPeerId", ""));
        d->is_owner = !strcmp(d->owner, s->user_id);
        const jval *adm = json_get(g, "administratorPeerIds");
        bool me_admin = false;
        for (int k = 0; adm && k < json_len(adm); k++) { const jval *a = json_at(adm, k); if (a && a->type == J_STR && !strcmp(a->str, s->user_id)) me_admin = true; }
        d->can_moderate = d->is_owner || me_admin;
        const jval *chs = json_get(g, "channels");
        for (int k = 0; k < json_len(chs) && d->nch < GROUP_MAX_CH; k++) {
            const jval *c = json_at(chs, k); channel_t *ch = &d->ch[d->nch++];
            snprintf(ch->id, sizeof ch->id, "%s", json_str(c, "id", "")); snprintf(ch->name, sizeof ch->name, "%s", json_str(c, "name", "canal"));
            const char *kind = json_str(c, "kind", json_str(c, "type", "text"));   /* o Electron grava "kind"; o CUI antigo gravava "type" */
            ch->voice = !strcmp(kind, "voice");
            snprintf(ch->room, sizeof ch->room, "%s", json_str(c, "voiceRoomId", ch->id));
        }
        const jval *ms = json_get(g, "members");
        for (int k = 0; k < json_len(ms) && d->nm < GROUP_MAX_MEMBERS; k++) {
            const jval *m = json_at(ms, k); gmember_t *gm = &d->m[d->nm++];
            snprintf(gm->id, sizeof gm->id, "%s", json_str(m, "peerId", "")); snprintf(gm->name, sizeof gm->name, "%s", json_str(m, "displayName", "?"));
            gm->admin = false; for (int q = 0; adm && q < json_len(adm); q++) { const jval *a = json_at(adm, q); if (a && a->type == J_STR && !strcmp(a->str, gm->id)) gm->admin = true; }
        }
        d->active_ch = 0;
        for (int k = 0; k < d->nch; k++) if (!d->ch[k].voice) { d->active_ch = k; break; }   /* abre no primeiro canal de texto */
        if (!strcmp(d->id, keep) && s->gchan_id[0]) for (int k = 0; k < d->nch; k++) if (!strcmp(d->ch[k].id, s->gchan_id)) d->active_ch = k;
        snprintf(d->channel, sizeof d->channel, "%s", d->nch ? d->ch[d->active_ch].id : d->id);
        sbuf b = { 0 }; dump(&b, g); d->raw = b.p;
    }
    json_free(j);
    s->active_group = -1;
    for (int i = 0; i < s->group_count; i++) if (keep[0] && !strcmp(s->groups[i].id, keep)) s->active_group = i;
}

/* ---- escrita: relê o raw, troca só o que a interface edita ---- */
static void put_channel(sbuf *b, const channel_t *c, const jval *old) {
    sb_put(b, "{");
    int n = 0;
    if (old) for (int i = 0; i < old->count; i++) {
        const char *k = old->keys[i]; if (!strcmp(k, "name") || !strcmp(k, "kind") || !strcmp(k, "type") || !strcmp(k, "voiceRoomId")) continue;
        if (n++) sb_put(b, ","); sb_str(b, k); sb_put(b, ":"); dump(b, &old->items[i]);
    }
    if (n++) sb_put(b, ","); sb_put(b, "\"name\":"); sb_str(b, c->name);
    sb_put(b, ",\"kind\":"); sb_str(b, c->voice ? "voice" : "text");
    if (c->voice) { sb_put(b, ",\"voiceRoomId\":"); sb_str(b, c->room); }
    sb_put(b, "}");
}

static void group_save(app_state_t *s, int gi) {
    if (s->busy || gi < 0 || gi >= s->group_count) return;
    group_t *g = &s->groups[gi];
    jval *raw = json_parse(g->raw ? g->raw : "{}");
    sbuf b = { 0 }; sb_put(&b, "{");
    int n = 0;
    for (int i = 0; raw && i < raw->count; i++) {
        const char *k = raw->keys[i];
        if (!strcmp(k, "name") || !strcmp(k, "channels") || !strcmp(k, "members") || !strcmp(k, "administratorPeerIds") || !strcmp(k, "membershipVersion") || !strcmp(k, "manifestVersion")) continue;
        if (n++) sb_put(&b, ","); sb_str(&b, k); sb_put(&b, ":"); dump(&b, &raw->items[i]);
    }
    const jval *oc = raw ? json_get(raw, "channels") : NULL, *om = raw ? json_get(raw, "members") : NULL;
    const jval *ov = raw ? json_get(raw, "membershipVersion") : NULL, *mv = raw ? json_get(raw, "manifestVersion") : NULL;
    char num[40];
    if (n++) sb_put(&b, ","); sb_put(&b, "\"name\":"); sb_str(&b, g->name);
    sb_put(&b, ",\"channels\":[");
    for (int i = 0; i < g->nch; i++) { if (i) sb_put(&b, ","); put_channel(&b, &g->ch[i], find_by(oc, "id", g->ch[i].id)); }
    sb_put(&b, "],\"members\":[");
    for (int i = 0; i < g->nm; i++) {
        if (i) sb_put(&b, ",");
        const jval *old = find_by(om, "peerId", g->m[i].id);
        if (old) dump(&b, old);
        else { sb_put(&b, "{\"peerId\":"); sb_str(&b, g->m[i].id); sb_put(&b, ",\"displayName\":"); sb_str(&b, g->m[i].name); sb_put(&b, "}"); }
    }
    sb_put(&b, "],\"administratorPeerIds\":[");
    int na = 0; for (int i = 0; i < g->nm; i++) if (g->m[i].admin) { if (na++) sb_put(&b, ","); sb_str(&b, g->m[i].id); }
    snprintf(num, sizeof num, "],\"membershipVersion\":%lld,", (ov ? (long long)ov->num : 1) + 1); sb_put(&b, num);
    snprintf(num, sizeof num, "\"manifestVersion\":%lld}", (mv ? (long long)mv->num : 1) + 1); sb_put(&b, num);
    json_free(raw);
    snprintf(s->gchan_id, sizeof s->gchan_id, "%s", g->nch ? g->ch[g->active_ch].id : "");
    app_begin(s, OP_GROUP_SAVE, "POST", "/p2p/groups", b.p);
    free(b.p);
}

/* ---- ações ---- */
static void select_channel(app_state_t *s, int gi, int ci) {
    group_t *g = &s->groups[gi]; if (ci < 0 || ci >= g->nch) return;
    if (g->ch[ci].voice) {   /* canal de voz: entra na sala dele (mesmo caminho da "Sala geral") */
        char room[96]; snprintf(room, sizeof room, "g-%s", g->ch[ci].room);
        s->screen = SCREEN_CALL; s->mic_on = true; if (!s->in_room) rtc_join_room(s, room);
        return;
    }
    g->active_ch = ci; snprintf(g->channel, sizeof g->channel, "%s", g->ch[ci].id); snprintf(s->gchan_id, sizeof s->gchan_id, "%s", g->ch[ci].id);
    s->msg_count = 0; snprintf(s->chat_channel, sizeof s->chat_channel, "%s", g->channel); s->reload_msgs = true;
}

static void add_channel(app_state_t *s, int gi) {
    group_t *g = &s->groups[gi];
    if (!s->chan_name[0] || g->nch >= GROUP_MAX_CH || !g->can_moderate) return;
    channel_t *c = &g->ch[g->nch++]; memset(c, 0, sizeof *c);
    new_id(c->id, sizeof c->id); snprintf(c->name, sizeof c->name, "%s", s->chan_name); c->voice = s->chan_kind == 1;
    if (c->voice) new_id(c->room, sizeof c->room);
    if (!c->voice) g->active_ch = g->nch - 1;
    s->chan_name[0] = 0; s->chan_open = false;
    group_save(s, gi);
}

static void remove_channel(app_state_t *s, int gi, int ci) {
    group_t *g = &s->groups[gi];
    int texts = 0; for (int i = 0; i < g->nch; i++) if (!g->ch[i].voice) texts++;
    if (!g->can_moderate || ci < 0 || ci >= g->nch || (!g->ch[ci].voice && texts <= 1)) return;   /* sempre sobra um canal de texto */
    memmove(&g->ch[ci], &g->ch[ci + 1], sizeof(channel_t) * (size_t)(g->nch - ci - 1)); g->nch--;
    g->active_ch = 0; for (int i = 0; i < g->nch; i++) if (!g->ch[i].voice) { g->active_ch = i; break; }
    snprintf(g->channel, sizeof g->channel, "%s", g->ch[g->active_ch].id); snprintf(s->chat_channel, sizeof s->chat_channel, "%s", g->channel); s->reload_msgs = true;
    group_save(s, gi);
}

static bool is_member(const group_t *g, const char *id) { for (int i = 0; i < g->nm; i++) if (!strcmp(g->m[i].id, id)) return true; return false; }

static void add_member(app_state_t *s, int gi, const person_t *p) {
    group_t *g = &s->groups[gi];
    if (!g->can_moderate || g->nm >= GROUP_MAX_MEMBERS || is_member(g, p->id)) return;
    gmember_t *m = &g->m[g->nm++]; memset(m, 0, sizeof *m); snprintf(m->id, sizeof m->id, "%s", p->id); snprintf(m->name, sizeof m->name, "%s", p->name);
    group_save(s, gi);
}

static void set_admin(app_state_t *s, int gi, int mi, bool on) {
    group_t *g = &s->groups[gi];
    if (!g->is_owner || mi < 0 || mi >= g->nm || !strcmp(g->m[mi].id, g->owner)) return;   /* só o dono promove ou rebaixa */
    g->m[mi].admin = on; group_save(s, gi);
}

static void remove_member(app_state_t *s, int gi, int mi) {
    group_t *g = &s->groups[gi];
    if (!g->can_moderate || mi < 0 || mi >= g->nm || !strcmp(g->m[mi].id, g->owner)) return;
    if (g->m[mi].admin && !g->is_owner) return;   /* administrador não remove outro administrador */
    memmove(&g->m[mi], &g->m[mi + 1], sizeof(gmember_t) * (size_t)(g->nm - mi - 1)); g->nm--; group_save(s, gi);
}

static void leave_or_delete(app_state_t *s, int gi) {
    if (s->busy || gi < 0 || gi >= s->group_count) return;
    char path[120]; snprintf(path, sizeof path, "/p2p/groups/%s/delete", s->groups[gi].id);
    s->active_group = -1; s->active_friend = -1; s->msg_count = 0; s->gleave_open = false;
    app_begin(s, OP_GROUP_SAVE, "POST", path, "{}");
}

/* ---- interface ---- */
static const char *role_of(const group_t *g, const gmember_t *m) { return !strcmp(m->id, g->owner) ? "Dono" : m->admin ? "Administrador" : "Membro"; }

void groups_sidebar(cui_ctx *ui, app_state_t *s, int gi) {   /* canais do grupo aberto, sob a linha dele na barra lateral */
    group_t *g = &s->groups[gi];
    cui_box(ui, .width = CUI_FILL, .pad_left = 14, .gap = 2) {
        for (int i = 0; i < g->nch; i++) {
            cui_push_id_int(ui, 2500 + i);
            cui_box(ui, .id = "ch", .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 10, .pad_y = 5, .gap = 8, .radius = 10, .align = CUI_ALIGN_CENTER,
                    .bg = (!g->ch[i].voice && g->active_ch == i) ? B_A(B_C1, 0x30) : CUI_RGBA(0x00000000)) {
                cui_icon(ui, g->ch[i].voice ? CUI_ICON_VOLUME_2 : CUI_ICON_HASH, 14, B_MUTED);
                cui_text(ui, g->ch[i].name, .nowrap = true, .width = CUI_FILL, .size = 13.5f);
            }
            if (briks_clicked(ui, "ch")) select_channel(s, gi, i);
            cui_pop_id(ui);
        }
        if (g->can_moderate && g->nch < GROUP_MAX_CH) {
            cui_push_id(ui, "newch");
            cui_box(ui, .id = "nc", .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 10, .pad_y = 5, .gap = 8, .radius = 10, .align = CUI_ALIGN_CENTER) {
                cui_icon(ui, CUI_ICON_PLUS, 14, B_MUTED); cui_text(ui, "Novo canal", .size = 13, .color = B_MUTED);
            }
            if (briks_clicked(ui, "nc")) { s->chan_open = true; s->chan_name[0] = 0; s->chan_kind = 0; }
            cui_pop_id(ui);
        }
    }
}

void groups_header(cui_ctx *ui, app_state_t *s) {   /* botões do cabeçalho do grupo: membros e menu */
    int gi = s->active_group; if (gi < 0 || gi >= s->group_count) return;
    group_t *g = &s->groups[gi];
    cui_box(ui, .width = CUI_FILL) {}   /* empurra os botões para a direita */
    char t[32]; snprintf(t, sizeof t, "Membros · %d", g->nm);
    if (cui_button(ui, t, .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM, .icon = CUI_ICON_USERS)) s->members_open = true;
    cui_menu(ui, "Grupo", .icon = CUI_ICON_ELLIPSIS, .icon_only = true, .placement = CUI_PLACE_BOTTOM_END) {
        cui_menu_label(ui, g->name);
        if (cui_menu_item(ui, "Membros e convites", .icon = CUI_ICON_USER_PLUS)) s->members_open = true;
        if (g->can_moderate) {
            if (cui_menu_item(ui, "Novo canal", .icon = CUI_ICON_HASH)) { s->chan_open = true; s->chan_name[0] = 0; s->chan_kind = 0; }
            if (cui_menu_item(ui, "Personalizar grupo", .icon = CUI_ICON_PENCIL)) { snprintf(s->grp_name_edit, sizeof s->grp_name_edit, "%s", g->name); s->gren_open = true; }
        }
        cui_menu_separator(ui);
        if (cui_menu_item(ui, g->is_owner ? "Apagar grupo…" : "Sair do grupo…", .icon = CUI_ICON_X, .danger = true)) s->gleave_open = true;
    }
}

void groups_dialogs(cui_ctx *ui, app_state_t *s) {
    int gi = s->active_group; group_t *g = (gi >= 0 && gi < s->group_count) ? &s->groups[gi] : NULL;
    if (!g) { s->chan_open = s->members_open = s->gren_open = s->gleave_open = false; return; }

    cui_dialog(ui, "Novo canal", &s->chan_open) {
        cui_text(ui, "Canais de texto guardam a conversa; canais de voz abrem uma sala de chamada do grupo.", .size = 12.5f, .color = B_MUTED);
        static const char *const kinds[] = { "Texto", "Voz" };
        cui_tabs(ui, "chan-kind", kinds, 2, &s->chan_kind, .variant = CUI_TABS_PILL);
        int r = cui_input(ui, "chan-name", s->chan_name, sizeof s->chan_name, .placeholder = s->chan_kind ? "Sala de voz" : "novidades", .autofocus = true);
        cui_dialog_footer(ui) { if (cui_button(ui, "Criar canal", .variant = CUI_BUTTON_PRIMARY, .loading = s->busy) || (r & CUI_SUBMITTED)) add_channel(s, gi); }
    }

    int rm_ch = -1, rm_mem = -1, inv = -1, adm_i = -1; bool adm_on = false;
    cui_dialog(ui, "Personalizar grupo", &s->gren_open) {
        cui_text(ui, "Nome do grupo", .size = 12.5f, .color = B_MUTED);
        int r = cui_input(ui, "grp-name", s->grp_name_edit, sizeof s->grp_name_edit, .autofocus = true);
        if (g->nch) cui_text(ui, "Canais", .size = 12.5f, .color = B_MUTED);
        for (int i = 0; i < g->nch; i++) {
            cui_push_id_int(ui, 2700 + i);
            cui_row(ui, .gap = 8, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
                cui_icon(ui, g->ch[i].voice ? CUI_ICON_VOLUME_2 : CUI_ICON_HASH, 14, B_MUTED);
                cui_text(ui, g->ch[i].name, .width = CUI_FILL, .nowrap = true);
                if (cui_icon_button(ui, CUI_ICON_X, .tooltip = "Remover canal", .size = CUI_SIZE_SM)) rm_ch = i;
            }
            cui_pop_id(ui);
        }
        cui_dialog_footer(ui) {
            if ((cui_button(ui, "Salvar", .variant = CUI_BUTTON_PRIMARY, .loading = s->busy) || (r & CUI_SUBMITTED)) && s->grp_name_edit[0]) {
                snprintf(g->name, sizeof g->name, "%s", s->grp_name_edit); s->gren_open = false; group_save(s, gi);
            }
        }
    }

    cui_dialog(ui, "Sair do grupo", &s->gleave_open) {
        cui_textf(ui, g->is_owner ? "Apagar “%s” deste computador?" : "Sair de “%s”?", g->name);
        cui_text(ui, g->is_owner ? "Você é o dono. O grupo deixa de existir aqui; as mensagens gravadas continuam no seu histórico." : "O grupo some da sua lista. Para voltar, um administrador precisa te convidar de novo.", .size = 12.5f, .color = B_MUTED);
        cui_dialog_footer(ui) { if (cui_button(ui, g->is_owner ? "Apagar grupo" : "Sair", .variant = CUI_BUTTON_DANGER, .loading = s->busy)) leave_or_delete(s, gi); }
    }

    cui_dialog(ui, "Membros", &s->members_open, .width = 460.0f) {
        cui_text(ui, "O dono e os administradores gerenciam canais e membros. Só o dono nomeia administradores.", .size = 12.5f, .color = B_MUTED);
        cui_scroll(ui, .id = "gm-scroll", .width = CUI_FILL, .height = CUI_PX(260), .pad = 2) {
            cui_column(ui, .gap = 6, .width = CUI_FILL) {
                for (int i = 0; i < g->nm; i++) {
                    gmember_t *m = &g->m[i]; const bool owner = !strcmp(m->id, g->owner), me = !strcmp(m->id, s->user_id);
                    cui_push_id_int(ui, 2800 + i);
                    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 10, .pad_y = 6, .gap = 10, .radius = 12, .bg = B_GLASS_CARD, .border = 1, .border_color = B_GLASS_BORDER_SOFT, .align = CUI_ALIGN_CENTER) {
                        cui_avatar(ui, m->name, .size = 30);
                        cui_column(ui, .gap = 0, .width = CUI_FILL) {
                            cui_text(ui, m->name, .nowrap = true);
                            cui_text(ui, role_of(g, m), .size = 11, .color = owner ? B_C3 : B_MUTED);
                        }
                        const bool can_manage = !owner && !me && g->can_moderate && (g->is_owner || !m->admin);
                        if (can_manage) {
                            int act = 0;
                            cui_menu(ui, "Gerenciar", .icon = CUI_ICON_ELLIPSIS, .icon_only = true, .placement = CUI_PLACE_BOTTOM_END) {
                                cui_menu_label(ui, m->name);
                                if (g->is_owner && cui_menu_item(ui, m->admin ? "Remover administrador" : "Tornar administrador", .icon = CUI_ICON_SHIELD)) act = 1;
                                if (cui_menu_item(ui, "Remover do grupo", .icon = CUI_ICON_X, .danger = true)) act = 2;
                            }
                            if (act == 1) { adm_i = i; adm_on = !m->admin; }
                            if (act == 2) rm_mem = i;
                        }
                    }
                    cui_pop_id(ui);
                }
            }
        }
        if (g->can_moderate) {
            cui_text(ui, "Convidar amigos", .font = CUI_FONT_MEDIUM, .size = 13);
            int shown = 0;
            for (int i = 0; i < s->friend_count; i++) {
                if (is_member(g, s->friends[i].id)) continue;
                cui_push_id_int(ui, 2900 + i);
                cui_row(ui, .gap = 8, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
                    cui_avatar(ui, s->friends[i].name, .size = 24);
                    cui_text(ui, s->friends[i].name, .width = CUI_FILL, .nowrap = true);
                    if (cui_button(ui, "Convidar", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM, .loading = s->busy)) inv = i;
                }
                cui_pop_id(ui); shown++;
            }
            if (!shown) cui_text(ui, s->friend_count ? "Todos os seus amigos já estão no grupo." : "Adicione amigos na tela Amigos para convidá-los.", .size = 12.5f, .color = B_MUTED);
            cui_text(ui, "O convite entra na lista deste computador; a entrega aos outros membros vem com o módulo P2P de grupos.", .size = 11.5f, .color = B_MUTED);
        }
    }
    /* ações adiadas: mexem nos vetores, então só depois de fechar os blocos de interface */
    if (rm_ch >= 0) remove_channel(s, gi, rm_ch);
    else if (adm_i >= 0) set_admin(s, gi, adm_i, adm_on);
    else if (rm_mem >= 0) remove_member(s, gi, rm_mem);
    else if (inv >= 0) { person_t p = s->friends[inv]; add_member(s, gi, &p); }
}

/* criação do grupo: canal "geral" de texto + canal de voz "Sala de voz", com "kind" (formato do Electron) */
void groups_create_json(const app_state_t *s, char *out, size_t cap) {
    char gid[40], sec[40], chid[40], vid[40], vroom[40], n[200], dn[200];
    new_id(gid, sizeof gid); new_id(sec, sizeof sec); new_id(chid, sizeof chid); new_id(vid, sizeof vid); new_id(vroom, sizeof vroom);
    json_escape(s->new_group, n, sizeof n); json_escape(s->user_name, dn, sizeof dn);
    long long now = (long long)time(NULL) * 1000;
    snprintf(out, cap,
        "{\"groupId\":\"%s\",\"name\":\"%s\",\"avatar\":null,\"channels\":[{\"id\":\"%s\",\"name\":\"geral\",\"kind\":\"text\"},{\"id\":\"%s\",\"name\":\"Sala de voz\",\"kind\":\"voice\",\"voiceRoomId\":\"%s\"}],"
        "\"members\":[{\"peerId\":\"%s\",\"displayName\":\"%s\"}],\"joinedAt\":%lld,\"ownerPeerId\":\"%s\",\"membershipVersion\":1,\"manifestVersion\":1,"
        "\"administratorPeerIds\":[\"%s\"],\"removedPeerIds\":[],\"removedMembers\":[],\"rendezvousSecret\":\"%s\"}",
        gid, n, chid, vid, vroom, s->user_id, dn, now, s->user_id, s->user_id, sec);
}
