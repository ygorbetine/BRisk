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
#include "core.h"

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
            snprintf(gm->fp, sizeof gm->fp, "%s", json_str(m, "fp", "")); snprintf(gm->jwk, sizeof gm->jwk, "%s", json_str(m, "jwk", ""));
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

static void group_save_ex(app_state_t *s, int gi, const char *snap_text) {
    if (s->busy || gi < 0 || gi >= s->group_count) return;
    group_t *g = &s->groups[gi];
    jval *raw = json_parse(g->raw ? g->raw : "{}");
    sbuf b = { 0 }; sb_put(&b, "{");
    int n = 0;
    for (int i = 0; raw && i < raw->count; i++) {
        const char *k = raw->keys[i];
        if (!strcmp(k, "name") || !strcmp(k, "channels") || !strcmp(k, "members") || !strcmp(k, "administratorPeerIds") || !strcmp(k, "membershipVersion") || !strcmp(k, "manifestVersion") || (snap_text && !strcmp(k, "briksSnap"))) continue;
        if (n++) sb_put(&b, ","); sb_str(&b, k); sb_put(&b, ":"); dump(&b, &raw->items[i]);
    }
    const jval *oc = raw ? json_get(raw, "channels") : NULL, *om = raw ? json_get(raw, "members") : NULL;
    const jval *ov = raw ? json_get(raw, "membershipVersion") : NULL, *mv = raw ? json_get(raw, "manifestVersion") : NULL;
    char num[40];
    if (snap_text) { if (n++) sb_put(&b, ","); sb_put(&b, "\"briksSnap\":"); sb_put(&b, snap_text); }
    if (n++) sb_put(&b, ","); sb_put(&b, "\"name\":"); sb_str(&b, g->name);
    sb_put(&b, ",\"channels\":[");
    for (int i = 0; i < g->nch; i++) { if (i) sb_put(&b, ","); put_channel(&b, &g->ch[i], find_by(oc, "id", g->ch[i].id)); }
    sb_put(&b, "],\"members\":[");
    for (int i = 0; i < g->nm; i++) {
        if (i) sb_put(&b, ",");
        const jval *old = find_by(om, "peerId", g->m[i].id);
        sb_put(&b, "{");
        int mn = 0;
        if (old) for (int q = 0; q < old->count; q++) {   /* o que já havia no membro (chaves do protocolo...) fica como veio */
            const char *k = old->keys[q]; if (!strcmp(k, "peerId") || !strcmp(k, "displayName") || !strcmp(k, "fp") || !strcmp(k, "jwk")) continue;
            if (mn++) sb_put(&b, ","); sb_str(&b, k); sb_put(&b, ":"); dump(&b, &old->items[q]);
        }
        if (mn) sb_put(&b, ",");
        sb_put(&b, "\"peerId\":"); sb_str(&b, g->m[i].id); sb_put(&b, ",\"displayName\":"); sb_str(&b, g->m[i].name);
        if (g->m[i].fp[0]) { sb_put(&b, ",\"fp\":"); sb_str(&b, g->m[i].fp); }
        if (g->m[i].jwk[0]) { sb_put(&b, ",\"jwk\":"); sb_str(&b, g->m[i].jwk); }
        sb_put(&b, "}");
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


/* ================= grupo P2P assinado (formato novo; core: group.sign / group.verify / group.send / group.code) =================
 * O dono/administrador monta o snapshot (membros por impressão digital, canais) e o core assina; o snapshot guardado no documento do grupo
 * ("briksSnap") é o estado confiado. Convite = o snapshot em texto (código `briks-grp1.…`) para quem está na lista. Detalhes: docs/plans/grupos-p2p-core.md. */
static void send_obj(const char *cmd, const char *snap, const char *trusted, const char *tag) {
    sbuf b = { 0 }; sb_put(&b, "{\"cmd\":\""); sb_put(&b, cmd); sb_put(&b, "\",\"snapshot\":"); sb_put(&b, snap);
    if (trusted) { sb_put(&b, ",\"trusted\":"); sb_put(&b, trusted); }
    if (tag) { sb_put(&b, ",\"tag\":\""); sb_put(&b, tag); sb_put(&b, "\""); }
    sb_put(&b, "}"); core_send(b.p); free(b.p);
}

static char *stored_snap(const group_t *g, long long *version) {   /* snapshot confiado deste grupo (texto JSON) ou NULL */
    if (version) *version = 0;
    jval *raw = json_parse(g->raw ? g->raw : "{}"); if (!raw) return NULL;
    const jval *sn = json_get(raw, "briksSnap"); char *out = NULL;
    if (sn && sn->type == J_OBJ) { sbuf b = { 0 }; dump(&b, sn); out = b.p; const jval *v = json_get(sn, "version"); if (version && v && v->type == J_NUM) *version = (long long)v->num; }
    json_free(raw); return out;
}

/* monta o snapshot sem assinatura; NULL + motivo se alguém não tem identidade publicada */
static char *snap_build(const app_state_t *s, const group_t *g, long long version, char *why, size_t whycap) {
    char owner_fp[72] = "";
    for (int i = 0; i < g->nm; i++) {
        const gmember_t *m = &g->m[i]; const bool me = !strcmp(m->id, s->user_id);
        const char *fp = me ? s->rtc_fp : m->fp, *jwk = me ? s->my_jwk : m->jwk;
        if (!fp[0]) { snprintf(why, whycap, me ? "sua identidade ainda não carregou" : "%s ainda não publicou a identidade", m->name); return NULL; }
        if ((m->admin || !strcmp(m->id, g->owner)) && jwk[0] != '{') { snprintf(why, whycap, "falta a chave de %s", m->name); return NULL; }
        if (!strcmp(m->id, g->owner)) snprintf(owner_fp, sizeof owner_fp, "%s", fp);
    }
    if (!owner_fp[0]) { snprintf(why, whycap, "dono não encontrado"); return NULL; }
    sbuf b = { 0 }; char num[48];
    sb_put(&b, "{\"v\":1,\"group_id\":"); sb_str(&b, g->id); sb_put(&b, ",\"name\":"); sb_str(&b, g->name);
    sb_put(&b, ",\"owner_fp\":"); sb_str(&b, owner_fp);
    snprintf(num, sizeof num, ",\"version\":%lld,\"ts\":0,\"members\":[", version); sb_put(&b, num);
    for (int i = 0; i < g->nm; i++) {
        const gmember_t *m = &g->m[i]; const bool me = !strcmp(m->id, s->user_id);
        const char *fp = me ? s->rtc_fp : m->fp, *jwk = me ? s->my_jwk : m->jwk;
        if (i) sb_put(&b, ",");
        sb_put(&b, "{\"fp\":"); sb_str(&b, fp); sb_put(&b, ",\"name\":"); sb_str(&b, m->name); sb_put(&b, m->admin || !strcmp(m->id, g->owner) ? ",\"admin\":true" : ",\"admin\":false");
        if ((m->admin || !strcmp(m->id, g->owner)) && jwk[0] == '{') { sb_put(&b, ",\"jwk\":"); sb_put(&b, jwk); }
        sb_put(&b, "}");
    }
    sb_put(&b, "],\"channels\":[");
    for (int i = 0; i < g->nch; i++) {   /* canal de voz usa a sala como id: assim todos entram na mesma sala */
        if (i) sb_put(&b, ",");
        sb_put(&b, "{\"id\":"); sb_str(&b, g->ch[i].voice ? g->ch[i].room : g->ch[i].id); sb_put(&b, ",\"name\":"); sb_str(&b, g->ch[i].name); sb_put(&b, g->ch[i].voice ? ",\"kind\":\"voice\"}" : ",\"kind\":\"text\"}");
    }
    sb_put(&b, "],\"signer_fp\":\"\",\"signature\":\"\"}");
    return b.p;
}

static void group_publish(app_state_t *s, int gi, bool want_code) {   /* pede ao core para assinar o estado atual do grupo (só dono/administrador) */
    group_t *g = &s->groups[gi]; if (!g->can_moderate) return;
    long long ver = 0; free(stored_snap(g, &ver));
    char why[120]; char *snap = snap_build(s, g, ver + 1, why, sizeof why);
    if (!snap) { snprintf(s->notice, sizeof s->notice, "Grupo só neste computador por enquanto: %s.", why); return; }
    if (want_code) s->ginv_wait = true;
    send_obj("group.sign", snap, NULL, NULL); free(snap);
}

static void group_save(app_state_t *s, int gi) { if (s->busy) return; group_save_ex(s, gi, NULL); group_publish(s, gi, false); }

/* fila de gravações que os eventos do core deixam para o quadro seguinte (só se grava quando não há outra operação em andamento) */
typedef struct { int kind; char gid[48]; char *text; } gpend_t;   /* 1 gravar o que assinei · 2 aplicar atualização recebida · 3 grupo novo por convite · 4 fui removido */
static gpend_t g_pend[6];
static void pend_add(int kind, const char *gid, const char *text) {
    for (int i = 0; i < 6; i++) if (!g_pend[i].kind) { g_pend[i].kind = kind; snprintf(g_pend[i].gid, sizeof g_pend[i].gid, "%s", gid); g_pend[i].text = text ? _strdup(text) : NULL; return; }
}
static int find_group(const app_state_t *s, const char *gid) { for (int i = 0; i < s->group_count; i++) if (!strcmp(s->groups[i].id, gid)) return i; return -1; }

static void jwk_of(const jval *m, char *out, size_t cap) {
    const jval *k = json_get(m, "jwk"); out[0] = 0;
    if (k && json_str(k, "x", "")[0] && json_str(k, "y", "")[0]) snprintf(out, cap, "{\"kty\":\"EC\",\"crv\":\"P-256\",\"x\":\"%s\",\"y\":\"%s\"}", json_str(k, "x", ""), json_str(k, "y", ""));
}

/* o snapshot recebido vira o estado do grupo em memória; ids dos membros já conhecidos são mantidos, eu continuo sendo user_id */
static void snap_apply(app_state_t *s, group_t *g, const jval *sn) {
    snprintf(g->name, sizeof g->name, "%s", json_str(sn, "name", "?"));
    gmember_t oldm[GROUP_MAX_MEMBERS]; int nold = g->nm; memcpy(oldm, g->m, sizeof oldm);
    const char *owner_fp = json_str(sn, "owner_fp", "");
    const jval *ms = json_get(sn, "members"); g->nm = 0; bool me_admin = false;
    for (int i = 0; i < json_len(ms) && g->nm < GROUP_MAX_MEMBERS; i++) {
        const jval *m = json_at(ms, i); const char *fp = json_str(m, "fp", "");
        gmember_t *d = &g->m[g->nm++]; memset(d, 0, sizeof *d);
        const gmember_t *o = NULL; for (int q = 0; q < nold; q++) if (!strcmp(oldm[q].fp, fp)) o = &oldm[q];
        const bool me = s->rtc_fp[0] && !strcmp(fp, s->rtc_fp);
        snprintf(d->id, sizeof d->id, "%s", me ? s->user_id : o ? o->id : fp);
        snprintf(d->name, sizeof d->name, "%s", json_str(m, "name", "?")); snprintf(d->fp, sizeof d->fp, "%s", fp);
        const jval *ad = json_get(m, "admin"); d->admin = ad && ad->type == J_BOOL && ad->b;
        jwk_of(m, d->jwk, sizeof d->jwk); if (!d->jwk[0] && o) snprintf(d->jwk, sizeof d->jwk, "%s", o->jwk);
        if (!strcmp(fp, owner_fp)) snprintf(g->owner, sizeof g->owner, "%s", d->id);
        if (me && d->admin) me_admin = true;
    }
    g->is_owner = !strcmp(g->owner, s->user_id); g->can_moderate = g->is_owner || me_admin;
    channel_t oldc[GROUP_MAX_CH]; int ncold = g->nch; memcpy(oldc, g->ch, sizeof oldc); (void)ncold;
    const jval *cs = json_get(sn, "channels"); g->nch = 0;
    for (int i = 0; i < json_len(cs) && g->nch < GROUP_MAX_CH; i++) {
        const jval *c = json_at(cs, i); channel_t *d = &g->ch[g->nch++]; memset(d, 0, sizeof *d);
        snprintf(d->id, sizeof d->id, "%s", json_str(c, "id", "")); snprintf(d->name, sizeof d->name, "%s", json_str(c, "name", "canal"));
        d->voice = !strcmp(json_str(c, "kind", "text"), "voice"); snprintf(d->room, sizeof d->room, "%s", d->id);
    }
    g->active_ch = 0; for (int i = 0; i < g->nch; i++) if (!g->ch[i].voice) { g->active_ch = i; break; }
    for (int i = 0; i < g->nch; i++) if (!g->ch[i].voice && s->gchan_id[0] && !strcmp(g->ch[i].id, s->gchan_id)) g->active_ch = i;
    snprintf(g->channel, sizeof g->channel, "%s", g->nch ? g->ch[g->active_ch].id : g->id);
}

/* documento de um grupo novo (convite aceito): mesmo formato do groups_create_json, com o snapshot confiado dentro */
static char *doc_from_snap(const app_state_t *s, const jval *sn, const char *snap_text) {
    char sec[40]; new_id(sec, sizeof sec);
    const char *owner_fp = json_str(sn, "owner_fp", ""); char owner_id[72] = "";
    sbuf b = { 0 }; char num[48];
    sb_put(&b, "{\"groupId\":"); sb_str(&b, json_str(sn, "group_id", "")); sb_put(&b, ",\"name\":"); sb_str(&b, json_str(sn, "name", "?")); sb_put(&b, ",\"avatar\":null,\"channels\":[");
    const jval *cs = json_get(sn, "channels");
    for (int i = 0; i < json_len(cs); i++) {
        const jval *c = json_at(cs, i); const bool v = !strcmp(json_str(c, "kind", "text"), "voice");
        if (i) sb_put(&b, ",");
        sb_put(&b, "{\"id\":"); sb_str(&b, json_str(c, "id", "")); sb_put(&b, ",\"name\":"); sb_str(&b, json_str(c, "name", "canal")); sb_put(&b, v ? ",\"kind\":\"voice\",\"voiceRoomId\":" : ",\"kind\":\"text\"");
        if (v) sb_str(&b, json_str(c, "id", ""));
        sb_put(&b, "}");
    }
    sb_put(&b, "],\"members\":[");
    const jval *ms = json_get(sn, "members"); sbuf adm = { 0 };
    for (int i = 0; i < json_len(ms); i++) {
        const jval *m = json_at(ms, i); const char *fp = json_str(m, "fp", ""); const bool me = s->rtc_fp[0] && !strcmp(fp, s->rtc_fp);
        const char *id = me ? s->user_id : fp; char jwk[260]; jwk_of(m, jwk, sizeof jwk);
        const jval *ad = json_get(m, "admin");
        if (i) sb_put(&b, ",");
        sb_put(&b, "{\"peerId\":"); sb_str(&b, id); sb_put(&b, ",\"displayName\":"); sb_str(&b, json_str(m, "name", "?")); sb_put(&b, ",\"fp\":"); sb_str(&b, fp);
        if (jwk[0]) { sb_put(&b, ",\"jwk\":"); sb_str(&b, jwk); }
        sb_put(&b, "}");
        if (ad && ad->type == J_BOOL && ad->b) { if (adm.n) sb_put(&adm, ","); sb_str(&adm, id); }
        if (!strcmp(fp, owner_fp)) snprintf(owner_id, sizeof owner_id, "%s", id);
    }
    snprintf(num, sizeof num, "],\"joinedAt\":%lld,", (long long)time(NULL) * 1000); sb_put(&b, num);
    sb_put(&b, "\"ownerPeerId\":"); sb_str(&b, owner_id); sb_put(&b, ",\"membershipVersion\":1,\"manifestVersion\":1,\"administratorPeerIds\":[");
    if (adm.p) sb_put(&b, adm.p);
    sb_put(&b, "],\"removedPeerIds\":[],\"removedMembers\":[],\"rendezvousSecret\":"); sb_str(&b, sec);
    sb_put(&b, ",\"briksSnap\":"); sb_put(&b, snap_text); sb_put(&b, "}");
    free(adm.p); return b.p;
}

static void set_clip(const char *utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0); if (n <= 0) return;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (size_t)n * sizeof(wchar_t)); if (!h) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, (wchar_t *)GlobalLock(h), n); GlobalUnlock(h);
    if (OpenClipboard(NULL)) { EmptyClipboard(); if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h); CloseClipboard(); } else GlobalFree(h);
}

void groups_core_event(app_state_t *s, const jval *j, const char *ev) {
    if (!strcmp(ev, "group.snapshot")) {   /* chegou pelo DataChannel (já com assinatura conferida): o core decide se vale contra o que eu confio */
        const jval *sn = json_get(j, "snapshot"); if (!sn) return;
        int gi = find_group(s, json_str(sn, "group_id", "")); if (gi < 0) return;   /* grupo que não conheço: só entra por convite */
        char *trusted = stored_snap(&s->groups[gi], NULL); if (!trusted) return;
        sbuf b = { 0 }; dump(&b, sn); send_obj("group.verify", b.p, trusted, "dc"); free(b.p); free(trusted);
        return;
    }
    if (strcmp(ev, "group.result")) return;
    const char *op = json_str(j, "op", ""); const jval *okv = json_get(j, "ok"); const bool ok = okv && okv->type == J_BOOL && okv->b;
    if (!strcmp(op, "sign")) {
        if (!ok) { snprintf(s->notice, sizeof s->notice, "Não foi possível assinar o grupo: %s", json_str(j, "message", "erro")); s->ginv_wait = false; return; }
        sbuf b = { 0 }; dump(&b, json_get(j, "snapshot"));
        pend_add(1, json_str(j, "group_id", ""), b.p);
        send_obj("group.send", b.p, NULL, NULL);   /* quem está na sala e é membro recebe agora */
        free(b.p);
        if (s->ginv_wait) { s->ginv_wait = false; free(s->ginv_code); s->ginv_code = _strdup(json_str(j, "code", "")); s->ginv_open = s->ginv_code != NULL; }
    } else if (!strcmp(op, "code")) {
        if (ok) { free(s->ginv_code); s->ginv_code = _strdup(json_str(j, "code", "")); s->ginv_open = s->ginv_code != NULL; }
    } else if (!strcmp(op, "verify")) {
        const bool invite = !strcmp(json_str(j, "tag", ""), "invite");
        if (!ok) { if (invite) snprintf(s->join_err, sizeof s->join_err, "%s", json_str(j, "message", "Convite inválido.")); return; }   /* de um peer, "versão antiga" é normal: silêncio */
        const char *dec = json_str(j, "decision", ""); const jval *sn = json_get(j, "snapshot"); const char *gid = json_str(sn, "group_id", "");
        sbuf b = { 0 }; dump(&b, sn);
        if (invite) {
            if (find_group(s, gid) >= 0) snprintf(s->join_err, sizeof s->join_err, "Você já está nesse grupo.");
            else { pend_add(3, gid, b.p); s->join_open = false; s->join_code[0] = 0; s->join_err[0] = 0; }
        } else if (!strcmp(dec, "update")) pend_add(2, gid, b.p);
        else if (!strcmp(dec, "removed")) pend_add(4, gid, NULL);
        free(b.p);
    }
}

/* um peer acabou de autenticar na sala: reenvia os snapshots que eu confio (o core só entrega a quem é membro de cada grupo) */
void groups_resend(app_state_t *s) {
    for (int i = 0; i < s->group_count; i++) { char *sn = stored_snap(&s->groups[i], NULL); if (sn) { send_obj("group.send", sn, NULL, NULL); free(sn); } }
}

static void leave_or_delete(app_state_t *s, int gi);
void groups_tick(app_state_t *s) {
    if (s->busy) return;
    for (int i = 0; i < 6; i++) {
        gpend_t *p = &g_pend[i]; if (!p->kind) continue;
        const int kind = p->kind; char gid[48]; snprintf(gid, sizeof gid, "%s", p->gid); char *text = p->text; p->kind = 0; p->text = NULL;
        int gi = find_group(s, gid);
        if (kind == 1 && gi >= 0 && text) group_save_ex(s, gi, text);
        else if (kind == 2 && gi >= 0 && text) { jval *sn = json_parse(text); if (sn) { snap_apply(s, &s->groups[gi], sn); json_free(sn); group_save_ex(s, gi, text); } }
        else if (kind == 3 && text) { jval *sn = json_parse(text); if (sn) { char *doc = doc_from_snap(s, sn, text); json_free(sn); app_begin(s, OP_GROUP_SAVE, "POST", "/p2p/groups", doc); free(doc); } }
        else if (kind == 4 && gi >= 0) leave_or_delete(s, gi);
        free(text);
        if (s->busy) break;   /* um POST por quadro */
    }
}

/* convida: entra na lista (precisa ter publicado a identidade) e o core assina; o código volta no evento */
static void send_group_code(app_state_t *s, int gi) {
    char *snap = stored_snap(&s->groups[gi], NULL); if (!snap) { snprintf(s->notice, sizeof s->notice, "Este grupo ainda não tem convite assinado: faça uma alteração (ex.: convidar um amigo) para gerar."); return; }
    send_obj("group.code", snap, NULL, NULL); free(snap);
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

static void add_member(app_state_t *s, int gi, const person_t *p, bool invite) {
    group_t *g = &s->groups[gi];
    if (!g->can_moderate || g->nm >= GROUP_MAX_MEMBERS || is_member(g, p->id)) return;
    gmember_t *m = &g->m[g->nm++]; memset(m, 0, sizeof *m); snprintf(m->id, sizeof m->id, "%s", p->id); snprintf(m->name, sizeof m->name, "%s", p->name);
    snprintf(m->fp, sizeof m->fp, "%s", p->fp); snprintf(m->jwk, sizeof m->jwk, "%s", p->jwk);
    if (s->busy) return;
    group_save_ex(s, gi, NULL); group_publish(s, gi, invite);
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
            if (g->ch[i].voice) {   /* quem está na sala de voz (só da sala em que eu estou: sem presença por sala, o resto é desconhecido) */
                char rn[96]; snprintf(rn, sizeof rn, "g-%s", g->ch[i].room);
                if (s->in_room && !strcmp(s->room_name, rn)) cui_box(ui, .width = CUI_FILL, .pad_left = 26, .gap = 1) {
                    cui_text(ui, s->user_name, .size = 12, .color = B_ONLINE, .nowrap = true);
                    for (int q = 0; q < 8; q++) if (s->rtc_peers[q].id[0] && s->rtc_peers[q].name[0] && !strcmp(s->rtc_peers[q].state, "autenticado")) cui_text(ui, s->rtc_peers[q].name, .size = 12, .color = B_MUTED, .nowrap = true);
                }
            }
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
    groups_tick(s);
    cui_dialog(ui, "Entrar com convite", &s->join_open, .width = 460.0f) {
        cui_text(ui, "Cole o código que um administrador do grupo mandou para você. Ele só vale para a sua conta.", .size = 12.5f, .color = B_MUTED);
        int jr = cui_input(ui, "join-code", s->join_code, sizeof s->join_code, .placeholder = "briks-grp1.…", .autofocus = true);
        if (s->join_err[0]) cui_text(ui, s->join_err, .size = 12.5f, .color = B_DANGER);
        cui_dialog_footer(ui) {
            if ((cui_button(ui, "Entrar no grupo", .variant = CUI_BUTTON_PRIMARY) || (jr & CUI_SUBMITTED)) && s->join_code[0]) {
                s->join_err[0] = 0;
                sbuf jb = { 0 }; sb_put(&jb, "{\"cmd\":\"group.verify\",\"tag\":\"invite\",\"code\":"); sb_str(&jb, s->join_code); sb_put(&jb, "}");
                if (!core_send(jb.p)) snprintf(s->join_err, sizeof s->join_err, "O motor do Briks ainda não está pronto. Tente de novo em instantes.");
                free(jb.p);
            }
        }
    }
    cui_dialog(ui, "Convite do grupo", &s->ginv_open, .width = 460.0f) {
        cui_text(ui, "Mande este código para a pessoa convidada. Ele é assinado por você e só funciona para quem está na lista de membros.", .size = 12.5f, .color = B_MUTED);
        char head[64] = ""; if (s->ginv_code) snprintf(head, sizeof head, "%.40s…", s->ginv_code);
        cui_text(ui, head, .size = 12, .color = B_MUTED, .nowrap = true);
        cui_dialog_footer(ui) { if (cui_button(ui, "Copiar código", .variant = CUI_BUTTON_PRIMARY, .icon = CUI_ICON_COPY) && s->ginv_code) set_clip(s->ginv_code); }
    }
    int gi = s->active_group; group_t *g = (gi >= 0 && gi < s->group_count) ? &s->groups[gi] : NULL;
    if (!g) { s->chan_open = s->members_open = s->gren_open = s->gleave_open = false; return; }

    cui_dialog(ui, "Novo canal", &s->chan_open) {
        cui_text(ui, "Canais de texto guardam a conversa; canais de voz abrem uma sala de chamada do grupo.", .size = 12.5f, .color = B_MUTED);
        static const char *const kinds[] = { "Texto", "Voz" };
        cui_tabs(ui, "chan-kind", kinds, 2, &s->chan_kind, .variant = CUI_TABS_PILL);
        int r = cui_input(ui, "chan-name", s->chan_name, sizeof s->chan_name, .placeholder = s->chan_kind ? "Sala de voz" : "novidades", .autofocus = true);
        cui_dialog_footer(ui) { if (cui_button(ui, "Criar canal", .variant = CUI_BUTTON_PRIMARY, .loading = s->busy) || (r & CUI_SUBMITTED)) add_channel(s, gi); }
    }

    int rm_ch = -1, rm_mem = -1, inv = -1, adm_i = -1; bool adm_on = false, want_code = false;
    cui_dialog(ui, "Personalizar grupo", &s->gren_open) {
        cui_text(ui, "Nome do grupo", .size = 12.5f, .color = B_MUTED);
        int r = cui_input(ui, "grp-name", s->grp_name_edit, sizeof s->grp_name_edit, .autofocus = true);
        if (g->nch) cui_text(ui, "Canais", .size = 12.5f, .color = B_MUTED);
        for (int i = 0; i < g->nch; i++) {
            cui_push_id_int(ui, 2700 + i);
            cui_row(ui, .gap = 8, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
                cui_icon(ui, g->ch[i].voice ? CUI_ICON_VOLUME_2 : CUI_ICON_HASH, 14, B_MUTED);
                cui_input(ui, "chn", g->ch[i].name, sizeof g->ch[i].name, .width = CUI_FILL);   /* renomear: vale ao salvar */
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
            cui_input(ui, "ginv-q", s->ginv_q, sizeof s->ginv_q, .placeholder = "Buscar por @nick ou nome", .width = CUI_FILL);
            int shown = 0;
            for (int i = 0; i < s->friend_count; i++) {
                if (is_member(g, s->friends[i].id)) continue;
                if (s->ginv_q[0]) { char hay[160], q[48]; snprintf(hay, sizeof hay, "%s %s", s->friends[i].name, s->friends[i].nick); snprintf(q, sizeof q, "%s", s->ginv_q[0] == '@' ? s->ginv_q + 1 : s->ginv_q);
                    for (char *c = hay; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
                    for (char *c = q; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
                    if (!strstr(hay, q)) continue; }
                cui_push_id_int(ui, 2900 + i);
                cui_row(ui, .gap = 8, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
                    cui_avatar(ui, s->friends[i].name, .size = 24);
                    cui_text(ui, s->friends[i].name, .width = CUI_FILL, .nowrap = true);
                    if (cui_button(ui, "Convidar", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM, .loading = s->busy)) inv = i;
                }
                cui_pop_id(ui); shown++;
            }
            if (!shown) cui_text(ui, s->friend_count ? "Todos os seus amigos já estão no grupo." : "Adicione amigos na tela Amigos para convidá-los.", .size = 12.5f, .color = B_MUTED);
            cui_text(ui, "Convidar gera um código assinado: mande para a pessoa (mensagem, onde quiser). Só ela consegue usar.", .size = 11.5f, .color = B_MUTED);
            if (cui_button(ui, "Gerar código do grupo", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM, .icon = CUI_ICON_LINK)) want_code = true;
        }
    }
    /* ações adiadas: mexem nos vetores, então só depois de fechar os blocos de interface */
    if (rm_ch >= 0) remove_channel(s, gi, rm_ch);
    else if (adm_i >= 0) set_admin(s, gi, adm_i, adm_on);
    else if (rm_mem >= 0) remove_member(s, gi, rm_mem);
    else if (inv >= 0) { person_t p = s->friends[inv]; add_member(s, gi, &p, true); }
    else if (want_code) send_group_code(s, gi);
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
