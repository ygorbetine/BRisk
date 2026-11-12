/* Grupos: canais de texto e de voz, cargos (dono, administrador, membro), membros e convites, sobre os grupos P2P do backend local (/p2p/groups).
 * O grupo é um documento JSON que também carrega campos do protocolo P2P (concessões de admin assinadas, revogações, segredo de encontro...).
 * Para não estragar nada, o save relê o JSON guardado (raw), troca SÓ nome, canais, membros, administradores e versões, e devolve o resto como veio.
 * Limite honesto: aqui as mudanças valem neste computador; a entrega assinada aos outros membros é do módulo P2P de grupos (briks-core). */
#include "briks_ui.h"
#include "backend.h"
#include "json.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <bcrypt.h>
#include <commdlg.h>
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

/* ---- imagem do grupo: só neste computador (o core recorta em quadrado 256 px e guarda em avatar-cache/grp-<id>) ---- */
static struct { char id[48]; cui_texture tex; bool asked; } g_gimg[MAX_GROUPS];
static int gimg_slot(const char *gid) {
    for (int i = 0; i < MAX_GROUPS; i++) if (!strcmp(g_gimg[i].id, gid)) return i;
    for (int i = 0; i < MAX_GROUPS; i++) if (!g_gimg[i].id[0]) { snprintf(g_gimg[i].id, sizeof g_gimg[i].id, "%s", gid); return i; }
    return -1;
}
cui_texture groups_image(app_state_t *s, const char *gid) {
    (void)s; int i = gimg_slot(gid); if (i < 0) return 0;
    if (!g_gimg[i].asked && core_state() == CORE_READY) {
        g_gimg[i].asked = true;
        char line[160]; snprintf(line, sizeof line, "{\"cmd\":\"group.image_get\",\"group_id\":\"%.60s\"}", gid); core_send(line);
    }
    return g_gimg[i].tex;
}
static void pick_group_image(const char *gid) {
    wchar_t wpath[1024] = L""; char path[3072];
    OPENFILENAMEW of = { sizeof of };
    of.hwndOwner = GetActiveWindow();
    of.lpstrFilter = L"Imagens (PNG, JPEG)\0*.png;*.jpg;*.jpeg\0";
    of.lpstrFile = wpath; of.nMaxFile = (DWORD)(sizeof wpath / sizeof wpath[0]);
    of.lpstrTitle = L"Escolher imagem do grupo";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&of)) return;
    if (!WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, (int)sizeof path, NULL, NULL)) return;
    char line[6400], esc[6200], eg[80]; json_escape(path, esc, sizeof esc); json_escape(gid, eg, sizeof eg);
    snprintf(line, sizeof line, "{\"cmd\":\"group.image\",\"group_id\":\"%s\",\"path\":\"%s\"}", eg, esc);
    core_send(line);
}
static void reset_group_image(const char *gid) {
    char line[160], eg[80]; json_escape(gid, eg, sizeof eg); snprintf(line, sizeof line, "{\"cmd\":\"group.image_reset\",\"group_id\":\"%s\"}", eg); core_send(line);
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
    if (!strcmp(ev, "group.image")) {
        int i = gimg_slot(json_str(j, "group_id", "")); if (i < 0) return;
        g_gimg[i].asked = true;
        const jval *ok = json_get(j, "ok");
        if (!ok || ok->type != J_BOOL || !ok->b) { snprintf(s->notice, sizeof s->notice, "%s", json_str(j, "message", "Não foi possível usar essa imagem.")); return; }
        cui_ctx *ui = cui_app_ui(s->app);
        if (g_gimg[i].tex) { cui_image_destroy(ui, g_gimg[i].tex); g_gimg[i].tex = 0; }
        const char *p = json_str(j, "path", ""); if (p[0]) { int w = 0, h = 0; g_gimg[i].tex = cui_image_load(ui, p, &w, &h); }
        cui_request_redraw(ui);
        return;
    }
    if (!strcmp(ev, "group.invite.short")) {   /* resposta do servidor ao pedido de código curto (migração 0010) */
        const jval *okv = json_get(j, "ok");
        if (okv && okv->type == J_BOOL && okv->b) { snprintf(s->ginv_short, sizeof s->ginv_short, "%s", json_str(j, "code", "")); s->ginv_short_st = s->ginv_short[0] ? 2 : 3; s->ginv_short_msg[0] = 0; }
        else { s->ginv_short_st = 3; s->ginv_short[0] = 0;
               snprintf(s->ginv_short_msg, sizeof s->ginv_short_msg, "%s", !strcmp(json_str(j, "error_code", ""), "server_not_ready") ? "O código curto ainda não está disponível no servidor. Use o convite longo (abaixo)." : json_str(j, "message", "Não foi possível gerar o código curto. Use o convite longo (abaixo).")); }
        return;
    }
    if (!strcmp(ev, "group.invite.resolve")) {   /* código curto resolvido para o convite longo assinado: o app confere assinatura e destinatário como sempre (group.verify) */
        const jval *okv = json_get(j, "ok");
        if (okv && okv->type == J_BOOL && okv->b) {
            sbuf jb = { 0 }; sb_put(&jb, "{\"cmd\":\"group.verify\",\"tag\":\"invite\",\"code\":"); sb_str(&jb, json_str(j, "package", "")); sb_put(&jb, "}");
            if (!core_send(jb.p)) snprintf(s->join_err, sizeof s->join_err, "O motor do Briks ainda não está pronto. Tente de novo em instantes.");
            free(jb.p);
        } else snprintf(s->join_err, sizeof s->join_err, "%s", !strcmp(json_str(j, "error_code", ""), "server_not_ready") ? "O código curto ainda não está disponível no servidor. Use o convite longo (briks-grp1.…)." : json_str(j, "message", "Código inválido."));
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
        if (s->ginv_wait) { s->ginv_short[0] = 0; s->ginv_short_st = 0; s->ginv_short_msg[0] = 0; s->ginv_wait = false; free(s->ginv_code); s->ginv_code = _strdup(json_str(j, "code", "")); s->ginv_open = s->ginv_code != NULL; }
    } else if (!strcmp(op, "code")) {
        if (ok) { s->ginv_short[0] = 0; s->ginv_short_st = 0; s->ginv_short_msg[0] = 0; free(s->ginv_code); s->ginv_code = _strdup(json_str(j, "code", "")); s->ginv_open = s->ginv_code != NULL; }
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
    char *snap = stored_snap(&s->groups[gi], NULL);
    if (!snap) { group_publish(s, gi, true); return; }   /* ainda sem convite assinado: o core assina o estado atual e o código volta no evento (ou o aviso diz o que falta) */
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

/* ---- quem está em cada sala de voz do grupo (GRP-17) ----
 * Fontes: (1) a sala em que EU estou (eu + participantes autenticados, com "Ao vivo" de share_peer_live); (2) a tabela g_ros, alimentada pelo evento
 * group.roster do core ({"group","room","total","more","members":[{"fp","name","live"}]}) e pelos dados demo. O core mantém um canal de presença por
 * grupo (group.roster.watch/here/unwatch): só entram membros do grupo, com os nomes da lista de membros daqui; no máximo 8 por sala e 4 grupos. */
typedef struct { char name[48]; bool live; } rmem_t;
typedef struct { char room[96]; int n, more; rmem_t m[8]; } rroster_t;
static rroster_t g_ros[16];
static rroster_t *ros_slot(const char *room, bool create) {
    for (int i = 0; i < 16; i++) if (!strcmp(g_ros[i].room, room)) return &g_ros[i];
    if (create) for (int i = 0; i < 16; i++) if (!g_ros[i].room[0]) { snprintf(g_ros[i].room, sizeof g_ros[i].room, "%s", room); return &g_ros[i]; }
    return NULL;
}
static void ros_add(const char *room, const char *name, bool live) {
    rroster_t *r = ros_slot(room, true); if (!r || r->n >= 8) return;
    snprintf(r->m[r->n].name, sizeof r->m[r->n].name, "%s", name); r->m[r->n].live = live; r->n++;
}
void groups_demo_roster(void) {   /* BRIKS_DEMO: o demo tem canais c0..c3 (c2 e c3 são de voz) */
    ros_add("c2", "Marina Costa", true); ros_add("c2", "Rafael Lima", false); ros_add("c3", "Bia Nogueira", false);
}
static int roster_get(const app_state_t *s, const channel_t *c, rmem_t out[8], bool *mine) {
    const char *key = c->room[0] ? c->room : c->id;
    char rn[96]; snprintf(rn, sizeof rn, "g-%s", key);
    int n = 0; *mine = false;
    if (s->in_room && !strcmp(s->room_name, rn)) {
        *mine = true;
        snprintf(out[n].name, sizeof out[n].name, "%s", s->user_name); out[n].live = s->sh.active; n++;
        for (int q = 0; q < BRIKS_MAX_PEERS && n < 8; q++) if (s->rtc_peers[q].id[0] && s->rtc_peers[q].name[0] && !strcmp(s->rtc_peers[q].state, "autenticado")) {
            snprintf(out[n].name, sizeof out[n].name, "%s", s->rtc_peers[q].name); out[n].live = s->sh.remote[q].on; n++;
        }
        return n;
    }
    const rroster_t *r = ros_slot(key, false);
    for (int i = 0; r && i < r->n; i++) out[n++] = r->m[i];
    return n;
}

/* ---- presença por sala: o que o app pede ao core (GRP-17) ---- */
#define ROS_GROUPS 4
static struct { char gid[48]; unsigned long long sig; char room[96]; bool live, sent; } g_rw[ROS_GROUPS];
static const char *chan_key(const channel_t *c) { return c->room[0] ? c->room : c->id; }
static bool group_has_voice(const group_t *g) { for (int i = 0; i < g->nch; i++) if (g->ch[i].voice) return true; return false; }
bool groups_want_roster(const app_state_t *s) {
    if (getenv("BRIKS_DEMO")) return false;
    for (int i = 0; i < s->group_count && i < ROS_GROUPS; i++) if (group_has_voice(&s->groups[i]) && s->groups[i].nm > 0) return true;
    return false;
}
void groups_roster_reset(void) { memset(g_rw, 0, sizeof g_rw); }   /* o core reiniciou: nada do que foi pedido vale mais */
void groups_roster_tick(app_state_t *s) {
    if (!s->rtc_session_ready) return;
    int slot = 0;
    for (int i = 0; i < s->group_count && slot < ROS_GROUPS; i++) {
        const group_t *g = &s->groups[i];
        if (!group_has_voice(g) || g->nm <= 0) continue;
        unsigned long long sig = 1469598103934665603ULL;   /* FNV-1a do id e da lista (impressão digital + nome) */
        for (const char *c = g->id; *c; c++) sig = (sig ^ (unsigned char)*c) * 1099511628211ULL;
        for (int m = 0; m < g->nm; m++) { for (const char *c = g->m[m].fp; *c; c++) sig = (sig ^ (unsigned char)*c) * 1099511628211ULL; for (const char *c = g->m[m].name; *c; c++) sig = (sig ^ (unsigned char)*c) * 1099511628211ULL; }
        if (strcmp(g_rw[slot].gid, g->id) || g_rw[slot].sig != sig) {
            sbuf b = { 0 }; sb_put(&b, "{\"cmd\":\"group.roster.watch\",\"group\":"); sb_str(&b, g->id); sb_put(&b, ",\"members\":[");
            int k = 0;
            for (int m = 0; m < g->nm; m++) if (g->m[m].fp[0]) { if (k++) sb_put(&b, ","); sb_put(&b, "{\"fp\":"); sb_str(&b, g->m[m].fp); sb_put(&b, ",\"name\":"); sb_str(&b, g->m[m].name); sb_put(&b, "}"); }
            sb_put(&b, "]}"); core_send(b.p); free(b.p);
            snprintf(g_rw[slot].gid, sizeof g_rw[slot].gid, "%s", g->id); g_rw[slot].sig = sig; g_rw[slot].sent = false;
        }
        /* em qual sala de voz deste grupo eu estou (vazio = em nenhuma) */
        char room[96] = "";
        if (s->in_room) for (int c = 0; c < g->nch; c++) if (g->ch[c].voice) {
            char rn[96]; snprintf(rn, sizeof rn, "g-%s", chan_key(&g->ch[c]));
            if (!strcmp(s->room_name, rn)) { snprintf(room, sizeof room, "%s", chan_key(&g->ch[c])); break; }
        }
        const bool live = room[0] && s->sh.active;
        if (!g_rw[slot].sent || strcmp(g_rw[slot].room, room) || g_rw[slot].live != live) {
            char gi[96], ri[200]; json_escape(g->id, gi, sizeof gi); json_escape(room, ri, sizeof ri);
            char line[420]; snprintf(line, sizeof line, "{\"cmd\":\"group.roster.here\",\"group\":\"%s\",\"room\":\"%s\",\"live\":%s}", gi, ri, live ? "true" : "false");
            core_send(line);
            snprintf(g_rw[slot].room, sizeof g_rw[slot].room, "%s", room); g_rw[slot].live = live; g_rw[slot].sent = true;
        }
        slot++;
    }
    for (; slot < ROS_GROUPS; slot++) if (g_rw[slot].gid[0]) {   /* o grupo saiu da lista: para de observar */
        char line[160], gi[96]; json_escape(g_rw[slot].gid, gi, sizeof gi);
        snprintf(line, sizeof line, "{\"cmd\":\"group.roster.unwatch\",\"group\":\"%s\"}", gi); core_send(line);
        memset(&g_rw[slot], 0, sizeof g_rw[slot]);
    }
}
/* evento group.roster: troca a lista da sala; sala esvaziada some */
void groups_roster_event(app_state_t *s, const jval *j) {
    const char *gid = json_str(j, "group", ""), *room = json_str(j, "room", "");
    if (!room[0] || strlen(room) >= sizeof g_ros[0].room || find_group(s, gid) < 0) return;   /* grupo que não conheço: ignora */
    const jval *ms = json_get(j, "members"); const int n = ms ? json_len(ms) : 0;
    rroster_t *r = ros_slot(room, n > 0);
    if (!r) return;
    r->n = 0; r->more = 0;
    for (int i = 0; i < n && r->n < 8; i++) {
        const jval *m = json_at(ms, i); const char *nm = json_str(m, "name", ""); if (!nm[0]) continue;
        const jval *lv = json_get(m, "live");
        snprintf(r->m[r->n].name, sizeof r->m[r->n].name, "%s", nm); r->m[r->n].live = lv && lv->type == J_BOOL && lv->b; r->n++;
    }
    { const jval *mo = json_get(j, "more"); r->more = mo && mo->type == J_NUM ? (int)mo->num : 0; }
    if (!r->n) memset(r, 0, sizeof *r);
}

/* estado só de interface (diálogos do canal e do membro); os dados continuam no group_t */
static bool g_ren_open, g_del_open, g_memrm_open;
static int g_ren_ci = -1, g_del_ci = -1, g_mem_sel = -1;
static char g_ren_name[48];

static void rename_channel(app_state_t *s, int gi, int ci, const char *name) {
    group_t *g = &s->groups[gi];
    if (!g->can_moderate || ci < 0 || ci >= g->nch || !name[0]) return;
    snprintf(g->ch[ci].name, sizeof g->ch[ci].name, "%s", name);
    group_save(s, gi);
}

/* botão de ícone pequeno (24 px) que só aparece no hover da linha */
static bool mini_icon(cui_ctx *ui, const char *id, const char *icon, const char *tip, bool danger) {
    cui_box(ui, .id = id, .width = CUI_PX(24), .height = CUI_PX(24), .radius = 8, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER,
            .bg = briks_hover(ui, id) ? CUI_RGBA(0xffffff20) : CUI_RGBA(0x00000000)) { cui_icon(ui, icon, 14, danger ? B_DANGER : B_MUTED); }
    if (briks_hover(ui, id)) briks_tip(tip);
    return briks_clicked_r(ui, id, 8.0f);
}

static bool last_text_channel(const group_t *g, int ci) {
    int texts = 0; for (int i = 0; i < g->nch; i++) if (!g->ch[i].voice) texts++;
    return !g->ch[ci].voice && texts <= 1;
}

/* 1.4.1 (item 19): grupo = cabeçalho de seção. O grupo não tem mais fundo roxo cheio (ui_social.c desenha avatar + nome + seta); aqui ficam as duas
 * subseções, TEXTO e VOZ, com o "+" de novo canal ao lado do título (só para quem pode criar), e só o canal de texto ATIVO recebe o roxo cheio, com
 * barra de acento à esquerda. Sob cada sala de voz: ponto verde + quem está nela. Não lidas por canal de texto (notify_unread_for). */
bool groups_collapsed(const char *gid) { return settings_group_collapsed(gid); }   /* 1.4.2: guardado em settings.cfg (por hash do id), sobrevive ao reinício */
void groups_collapse_set(const char *gid, bool on) { settings_group_collapse(gid, on); }

static void sub_title(cui_ctx *ui, app_state_t *s, group_t *g, const char *id, const char *title, int kind) {
    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 10, .pad_top = 6, .gap = 6, .align = CUI_ALIGN_CENTER) {
        cui_text(ui, title, .size = 10.5f, .color = CUI_RGBA(0xa592b8cc), .nowrap = true, .width = CUI_FILL);
        if (g->can_moderate && g->nch < GROUP_MAX_CH && mini_icon(ui, id, CUI_ICON_PLUS, kind ? "Novo canal de voz" : "Novo canal de texto", false)) {
            s->chan_open = true; s->chan_name[0] = 0; s->chan_kind = kind;
        }
    }
}

void groups_sidebar(cui_ctx *ui, app_state_t *s, int gi) {   /* canais do grupo aberto, sob o cabeçalho dele na barra lateral */
    group_t *g = &s->groups[gi];
    if (groups_collapsed(g->id)) return;
    cui_box(ui, .width = CUI_FILL, .pad_left = 12, .gap = 2) {
        for (int kind = 0; kind < 2; kind++) {   /* 0 = TEXTO, 1 = VOZ */
            int count = 0; for (int i = 0; i < g->nch; i++) if ((int)g->ch[i].voice == kind) count++;
            if (!count && !(g->can_moderate && g->nch < GROUP_MAX_CH)) continue;   /* subseção vazia só aparece para quem pode criar canal nela */
            cui_push_id_int(ui, 2490 + kind);
            sub_title(ui, s, g, "snew", kind ? "VOZ" : "TEXTO", kind);
            cui_pop_id(ui);
            for (int i = 0; i < g->nch; i++) {
                if ((int)g->ch[i].voice != kind) continue;
                cui_push_id_int(ui, 2500 + i);
                const bool hov = g->can_moderate && briks_hover(ui, "ch");
                const bool act = !g->ch[i].voice && g->active_ch == i;
                const int unread = (!g->ch[i].voice && !act) ? notify_unread_for(g->ch[i].id) : 0;
                bool ren = false, del = false;
                cui_box(ui, .id = "ch", .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 8, .pad_y = 6, .gap = 8, .radius = 12, .align = CUI_ALIGN_CENTER,
                        .bg = act ? B_A(B_C1, 0x5a) : (briks_hover(ui, "ch") ? CUI_RGBA(0xffffff0c) : CUI_RGBA(0x00000000))) {
                    cui_box(ui, .width = CUI_PX(3), .height = CUI_PX(16), .radius = 2, .bg = act ? B_C3 : CUI_RGBA(0x00000000)) {}   /* barra de acento (só o canal ativo) */
                    cui_icon(ui, g->ch[i].voice ? CUI_ICON_VOLUME_2 : CUI_ICON_HASH, 15, act ? B_TEXT : B_MUTED);
                    cui_text(ui, g->ch[i].name, .nowrap = true, .width = CUI_FILL, .size = 14, .color = act ? B_TEXT : unread ? B_TEXT : B_MUTED, .font = (act || unread) ? CUI_FONT_MEDIUM : CUI_FONT_BODY);
                    if (unread > 0 && !hov) {   /* não lidas por canal de texto */
                        char nb[8]; snprintf(nb, sizeof nb, "%d", unread > 99 ? 99 : unread);
                        cui_box(ui, .pad_x = 6, .pad_y = 1, .radius = 9, .bg = B_C3) { cui_text(ui, nb, .size = 10.5f, .color = CUI_RGB(0x1b1226), .nowrap = true); }
                    }
                    if (hov) {
                        ren = mini_icon(ui, "chr", CUI_ICON_PENCIL, "Renomear", false);
                        del = mini_icon(ui, "chd", CUI_ICON_TRASH_2, "Apagar", true);
                    }
                }
                if (ren) { g_ren_ci = i; g_ren_open = true; snprintf(g_ren_name, sizeof g_ren_name, "%s", g->ch[i].name); }
                else if (del) { g_del_ci = i; g_del_open = true; }
                else if (briks_clicked(ui, "ch")) select_channel(s, gi, i);
                if (g->ch[i].voice) {   /* quem está na sala de voz, com ponto verde, "Ao vivo" e Assistir (ver roster_get para a origem dos dados) */
                    rmem_t who[8]; bool mine; const int nw = roster_get(s, &g->ch[i], who, &mine);
                    bool watch = false;
                    if (nw) cui_box(ui, .width = CUI_FILL, .pad_left = 20, .gap = 3) {
                        for (int w = 0; w < nw; w++) {
                            cui_push_id_int(ui, 100 + w);
                            cui_box(ui, .width = CUI_FILL, .gap = 3) {
                                cui_box(ui, .dir = CUI_ROW, .gap = 6, .align = CUI_ALIGN_CENTER) {
                                    cui_box(ui, .width = CUI_PX(7), .height = CUI_PX(7), .radius = 4, .bg = B_ONLINE) {}   /* ponto verde: há gente na sala */
                                    briks_avatar(ui, who[w].name, 16, person_photo(s, NULL, who[w].name));
                                    cui_text(ui, who[w].name, .size = 13, .color = (mine && w == 0) ? B_ONLINE : B_MUTED, .nowrap = true);
                                }
                                if (who[w].live) cui_box(ui, .dir = CUI_ROW, .gap = 8) {
                                    cui_text(ui, "AO VIVO", .size = 10, .color = B_C3, .nowrap = true);
                                    cui_box(ui, .id = "wt", .pad_x = 8, .pad_y = 2, .radius = 9, .bg = B_A(B_C1, 0x55)) { cui_text(ui, "Assistir", .size = 11, .nowrap = true); }
                                    if (briks_clicked_r(ui, "wt", 9.0f)) watch = true;
                                }
                            }
                            cui_pop_id(ui);
                        }
                        { const rroster_t *rr = mine ? NULL : ros_slot(g->ch[i].room[0] ? g->ch[i].room : g->ch[i].id, false);
                          if (rr && rr->more > 0) cui_textf(ui, "+%d na sala", rr->more); }
                    }
                    if (watch) { s->screen = SCREEN_CALL; s->call_mode = 1; s->call_view = 0; }
                }
                cui_pop_id(ui);
            }
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
        if (g->can_moderate && cui_menu_item(ui, "Convidar (gerar código)", .icon = CUI_ICON_LINK)) send_group_code(s, gi);
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
        cui_text(ui, "Cole o código que um administrador do grupo mandou para você (ou abra o link do convite). Confira o código e toque em Entrar no grupo: o Briks nunca entra sozinho. Ele só vale para a sua conta.", .size = 12.5f, .color = B_MUTED);
        int jr = cui_input(ui, "join-code", s->join_code, sizeof s->join_code, .placeholder = "BRK-XXXX-XXXX ou briks-grp1.…", .autofocus = true);
        if (s->join_err[0]) cui_text(ui, s->join_err, .size = 12.5f, .color = B_DANGER);
        cui_dialog_footer(ui) {
            if ((cui_button(ui, "Entrar no grupo", .variant = CUI_BUTTON_PRIMARY) || (jr & CUI_SUBMITTED)) && s->join_code[0]) {
                s->join_err[0] = 0;
                const char *jc = s->join_code; while (*jc == ' ') jc++;
                const bool is_long = !strncmp(jc, "briks-grp1.", 11) || strlen(jc) > 40;   /* convite longo (assinado) ou o que não parece código curto */
                sbuf jb = { 0 };
                if (is_long) { sb_put(&jb, "{\"cmd\":\"group.verify\",\"tag\":\"invite\",\"code\":"); sb_str(&jb, s->join_code); sb_put(&jb, "}"); }
                else { sb_put(&jb, "{\"cmd\":\"group.invite.resolve\",\"tag\":\"invite\",\"code\":"); sb_str(&jb, jc); sb_put(&jb, "}"); }   /* BRK-XXXX-XXXX: o servidor devolve o convite assinado e ele passa pela MESMA conferência */
                if (!core_send(jb.p)) snprintf(s->join_err, sizeof s->join_err, "O motor do Briks ainda não está pronto. Tente de novo em instantes.");
                free(jb.p);
            }
        }
    }
    cui_dialog(ui, "Convite do grupo", &s->ginv_open, .width = 460.0f) {
        cui_text(ui, "Mande este código para a pessoa convidada. Ele é assinado por você e só funciona para quem está na lista de membros.", .size = 12.5f, .color = B_MUTED);
        char head[64] = ""; if (s->ginv_code) snprintf(head, sizeof head, "%.40s…", s->ginv_code);
        if (s->ginv_short_st == 2) {   /* código curto pronto: grande, fácil de ditar */
            cui_text(ui, s->ginv_short, .font = CUI_FONT_DISPLAY, .size = 26, .color = B_TEXT, .nowrap = true);
            cui_text(ui, "Vale por 24 horas e até 10 usos. Quem entrar ainda precisa estar na lista de membros: o app confere a sua assinatura.", .size = 12, .color = B_MUTED);
            if (cui_button(ui, "Copiar código curto", .variant = CUI_BUTTON_PRIMARY, .icon = CUI_ICON_COPY)) set_clip(s->ginv_short);
        } else {
            if (s->ginv_short_st == 3) cui_text(ui, s->ginv_short_msg, .size = 12, .color = CUI_RGB(0xf5d9a0));
            if (cui_button(ui, s->ginv_short_st == 1 ? "Gerando…" : "Gerar código curto (BRK-XXXX-XXXX)", .variant = CUI_BUTTON_PRIMARY, .loading = s->ginv_short_st == 1) && s->ginv_short_st != 1 && s->ginv_code) {
                s->ginv_short_st = 1; s->ginv_short_msg[0] = 0;
                sbuf jb = { 0 }; sb_put(&jb, "{\"cmd\":\"group.invite.short\",\"package\":"); sb_str(&jb, s->ginv_code); sb_put(&jb, "}");
                if (!core_send(jb.p)) { s->ginv_short_st = 3; snprintf(s->ginv_short_msg, sizeof s->ginv_short_msg, "O motor do Briks ainda não está pronto. Tente de novo em instantes."); }
                free(jb.p);
            }
        }
        cui_text(ui, "Convite longo (funciona até sem internet, só para quem está na lista):", .size = 12, .color = B_MUTED);
        cui_text(ui, head, .size = 12, .color = B_MUTED, .nowrap = true);
        cui_dialog_footer(ui) { if (cui_button(ui, "Copiar convite longo", .variant = CUI_BUTTON_SECONDARY, .icon = CUI_ICON_COPY) && s->ginv_code) set_clip(s->ginv_code); }
    }
    int gi = s->active_group; group_t *g = (gi >= 0 && gi < s->group_count) ? &s->groups[gi] : NULL;
    if (!g) { s->chan_open = s->members_open = s->gren_open = s->gleave_open = g_ren_open = g_del_open = g_memrm_open = false; return; }

    cui_dialog(ui, "Novo canal", &s->chan_open) {
        cui_text(ui, "Canais de texto guardam a conversa; canais de voz abrem uma sala de chamada do grupo.", .size = 12.5f, .color = B_MUTED);
        static const char *const kinds[] = { "Texto", "Voz" };
        cui_tabs(ui, "chan-kind", kinds, 2, &s->chan_kind, .variant = CUI_TABS_PILL);
        int r = cui_input(ui, "chan-name", s->chan_name, sizeof s->chan_name, .placeholder = s->chan_kind ? "Sala de voz" : "novidades", .autofocus = true);
        cui_dialog_footer(ui) { if (cui_button(ui, "Criar canal", .variant = CUI_BUTTON_PRIMARY, .loading = s->busy) || (r & CUI_SUBMITTED)) add_channel(s, gi); }
    }

    int rm_ch = -1, rm_mem = -1, inv = -1, adm_i = -1; bool adm_on = false, want_code = false, pick_img = false, rst_img = false;
    cui_dialog(ui, "Personalizar grupo", &s->gren_open) {
        cui_row(ui, .gap = 12, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
            cui_avatar_ex(ui, g->name, &(cui_avatar_opts){ .size = 64, .image = groups_image(s, g->id) });
            cui_column(ui, .gap = 6) {
                cui_text(ui, "Imagem do grupo", .size = 12.5f, .color = B_MUTED);
                cui_row(ui, .gap = 8) {
                    if (cui_button(ui, "Trocar imagem", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM, .icon = CUI_ICON_IMAGE)) pick_img = true;
                    if (groups_image(s, g->id) && cui_button(ui, "Remover", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) rst_img = true;
                }
            }
        }
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

    if (g_ren_ci >= g->nch) g_ren_open = false;
    cui_dialog(ui, "Renomear canal", &g_ren_open, .width = 420.0f) {
        cui_text(ui, g_ren_ci >= 0 && g_ren_ci < g->nch && g->ch[g_ren_ci].voice ? "Novo nome da sala de voz." : "Novo nome do canal de texto.", .size = 13, .color = B_MUTED);
        int r = cui_input(ui, "ren-name", g_ren_name, sizeof g_ren_name, .autofocus = true);
        cui_dialog_footer(ui) {
            if ((cui_button(ui, "Salvar", .variant = CUI_BUTTON_PRIMARY, .loading = s->busy) || (r & CUI_SUBMITTED)) && g_ren_name[0]) { rename_channel(s, gi, g_ren_ci, g_ren_name); g_ren_open = false; }
        }
    }
    if (g_del_ci >= g->nch) g_del_open = false;
    cui_dialog(ui, "Apagar canal", &g_del_open, .width = 420.0f) {
        const bool ok_del = g_del_ci >= 0 && g_del_ci < g->nch;
        cui_textf(ui, "Apagar “%s”?", ok_del ? g->ch[g_del_ci].name : "");
        const bool last = ok_del && last_text_channel(g, g_del_ci);
        cui_text(ui, last ? "Este é o único canal de texto: o grupo precisa de pelo menos um. Crie outro antes de apagar este." : "O canal some para todos os membros. As mensagens já gravadas continuam nos computadores de quem as recebeu.", .size = 13, .color = last ? B_DANGER : B_MUTED);
        cui_dialog_footer(ui) {
            if (!last && cui_button(ui, "Apagar canal", .variant = CUI_BUTTON_DANGER, .loading = s->busy)) { remove_channel(s, gi, g_del_ci); g_del_open = false; }
        }
    }
    if (g_mem_sel >= g->nm) g_memrm_open = false;
    cui_dialog(ui, "Remover membro", &g_memrm_open, .width = 420.0f) {
        cui_textf(ui, "Remover %s do grupo?", g_mem_sel >= 0 && g_mem_sel < g->nm ? g->m[g_mem_sel].name : "");
        cui_text(ui, "A pessoa deixa de receber o grupo e some da lista de membros. Para voltar, precisa de um novo convite.", .size = 13, .color = B_MUTED);
        cui_dialog_footer(ui) { if (cui_button(ui, "Remover membro", .variant = CUI_BUTTON_DANGER, .loading = s->busy)) { remove_member(s, gi, g_mem_sel); g_memrm_open = false; } }
    }

    cui_dialog(ui, "Membros", &s->members_open, .width = 460.0f) {
        cui_text(ui, "O dono e os administradores gerenciam canais e membros. Só o dono nomeia administradores.", .size = 12.5f, .color = B_MUTED);
        cui_scroll(ui, .id = "gm-scroll", .width = CUI_FILL, .height = CUI_PX(150), .pad = 2) {
            cui_column(ui, .gap = 6, .width = CUI_FILL) {
                for (int i = 0; i < g->nm; i++) {
                    gmember_t *m = &g->m[i]; const bool owner = !strcmp(m->id, g->owner), me = !strcmp(m->id, s->user_id);
                    cui_push_id_int(ui, 2800 + i);
                    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 10, .pad_y = 6, .gap = 10, .radius = 12, .bg = B_GLASS_CARD, .border = 1, .border_color = B_GLASS_BORDER_SOFT, .align = CUI_ALIGN_CENTER) {
                        briks_avatar(ui, m->name, 30, person_photo(s, m->id, m->name));
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
            cui_text(ui, "Ou convide alguém que ainda não é seu amigo pelo @nick exato:", .size = 12, .color = B_MUTED);
            cui_row(ui, .gap = 8, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
                int nr = cui_input(ui, "gnick", s->gnick, sizeof s->gnick, .placeholder = "@nick", .width = CUI_FILL);
                if ((cui_button(ui, "Buscar", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM) || (nr & CUI_SUBMITTED)) && s->gnick[0]) {
                    s->gnick_found = false; snprintf(s->gnick_msg, sizeof s->gnick_msg, "Buscando…");
                    char esc[80], line[160]; json_escape(s->gnick, esc, sizeof esc); snprintf(line, sizeof line, "{\"cmd\":\"friends.lookup\",\"nick\":\"%s\"}", esc); core_send(line);
                }
            }
            if (s->gnick_msg[0]) cui_text(ui, s->gnick_msg, .size = 12, .color = B_MUTED);
            if (s->gnick_found && !is_member(g, s->gnick_hit.id)) {
                cui_row(ui, .gap = 8, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
                    briks_avatar(ui, s->gnick_hit.name, 24, person_photo(s, s->gnick_hit.id, s->gnick_hit.name));
                    cui_textf(ui, "%s  @%s", s->gnick_hit.name, s->gnick_hit.nick);
                    cui_box(ui, .width = CUI_FILL) {}
                    if (cui_button(ui, "Convidar", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM, .loading = s->busy)) inv = -2;
                }
            }
            int shown = 0;
            for (int i = 0; i < s->friend_count; i++) {
                if (is_member(g, s->friends[i].id)) continue;
                if (s->ginv_q[0]) { char hay[160], q[48]; snprintf(hay, sizeof hay, "%s %s", s->friends[i].name, s->friends[i].nick); snprintf(q, sizeof q, "%s", s->ginv_q[0] == '@' ? s->ginv_q + 1 : s->ginv_q);
                    for (char *c = hay; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
                    for (char *c = q; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
                    if (!strstr(hay, q)) continue; }
                cui_push_id_int(ui, 2900 + i);
                cui_row(ui, .gap = 8, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
                    briks_avatar(ui, s->friends[i].name, 24, friend_photo(s->friends[i].id));
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
    if (pick_img) pick_group_image(g->id);
    else if (rst_img) reset_group_image(g->id);
    else if (rm_ch >= 0) remove_channel(s, gi, rm_ch);
    else if (adm_i >= 0) set_admin(s, gi, adm_i, adm_on);
    else if (rm_mem >= 0) { g_mem_sel = rm_mem; g_memrm_open = true; }   /* pede confirmação (como o Expulsar da chamada) */
    else if (inv == -2) { person_t p = s->gnick_hit; s->gnick_found = false; s->gnick[0] = 0; s->gnick_msg[0] = 0; add_member(s, gi, &p, true); }
    else if (inv >= 0) { person_t p = s->friends[inv]; add_member(s, gi, &p, true); }
    else if (want_code) send_group_code(s, gi);
}

/* ---- painel Membros do dock: membros do GRUPO aberto (não só amigos) ---- */
/* o mesmo membro duas vezes (mesmo id ou mesma impressão digital): vem de convites repetidos / reimportação */
static bool is_dup_at(const group_t *g, int i) {
    for (int k = 0; k < i; k++) if (!strcmp(g->m[k].id, g->m[i].id) || (g->m[i].fp[0] && !strcmp(g->m[k].fp, g->m[i].fp))) return true;
    return false;
}
static int dup_count(const group_t *g) { int n = 0; for (int i = 1; i < g->nm; i++) if (is_dup_at(g, i)) n++; return n; }
static void remove_duplicates(app_state_t *s, int gi) {
    group_t *g = &s->groups[gi]; if (!g->can_moderate || s->busy) return;
    bool any = false;
    for (int i = g->nm - 1; i >= 1; i--) if (is_dup_at(g, i) && strcmp(g->m[i].id, g->owner)) {   /* fica o primeiro; o dono nunca sai */
        memmove(&g->m[i], &g->m[i + 1], sizeof(gmember_t) * (size_t)(g->nm - i - 1)); g->nm--; any = true;
    }
    if (any) group_save(s, gi);
}

static void role_pill(cui_ctx *ui, const char *t, cui_color c) {
    cui_box(ui, .dir = CUI_ROW, .pad_x = 8, .pad_y = 2, .radius = 9, .bg = B_A(c, 0x38), .border = 1, .border_color = B_A(c, 0x70), .align = CUI_ALIGN_CENTER) {
        cui_text(ui, t, .size = 10.5f, .color = CUI_RGB(0xffffff), .nowrap = true);
    }
}

bool groups_members_panel(cui_ctx *ui, app_state_t *s) {
    int gi = s->active_group; if (gi < 0 || gi >= s->group_count) return false;
    group_t *g = &s->groups[gi];
    int act = 0, act_i = -1; bool reload = false, add = false, dedup = false;
    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 18, .pad_y = 8, .gap = 8, .align = CUI_ALIGN_CENTER) {
        cui_column(ui, .width = CUI_FILL, .gap = 0) {
            cui_text(ui, g->name, .font = CUI_FONT_MEDIUM, .size = 14, .nowrap = true);
            cui_textf(ui, g->nm == 1 ? "%d membro" : "%d membros", g->nm);
        }
        if (cui_icon_button(ui, CUI_ICON_REFRESH_CW, .tooltip = "Recarregar lista", .size = CUI_SIZE_SM)) reload = true;
        if (g->can_moderate && cui_icon_button(ui, CUI_ICON_USER_PLUS, .tooltip = "Adicionar membro", .size = CUI_SIZE_SM)) add = true;
    }
    cui_scroll(ui, .id = "gmembers-scroll", .width = CUI_FILL, .height = CUI_FILL, .pad = 9) {
        for (int i = 0; i < g->nm; i++) {
            gmember_t *m = &g->m[i]; const bool owner = !strcmp(m->id, g->owner), me = !strcmp(m->id, s->user_id);
            const bool manage = !owner && !me && g->can_moderate && (g->is_owner || !m->admin);
            bool online = me;
            for (int f = 0; !online && f < s->friend_count; f++) if (!strcmp(s->friends[f].id, m->id)) online = friends_online(s, &s->friends[f]);
            cui_push_id_int(ui, 7600 + i);
            cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 8, .pad_y = 6, .gap = 10, .radius = 14, .align = CUI_ALIGN_CENTER) {
                briks_avatar(ui, m->name, 34, person_photo(s, m->id, m->name));
                cui_column(ui, .gap = 2, .width = CUI_FILL) {
                    cui_text(ui, m->name, .nowrap = true, .size = 14);
                    cui_row(ui, .gap = 6, .align = CUI_ALIGN_CENTER) {
                        if (owner) role_pill(ui, "DONO", B_C3);
                        else if (m->admin) role_pill(ui, "ADMIN", B_C1);
                        if (me) role_pill(ui, "VOCÊ", B_ONLINE);
                        if (online && !me) cui_text(ui, "online", .size = 11.5f, .color = B_ONLINE);
                    }
                }
                if (manage) {
                    int a = 0;
                    cui_menu(ui, "Gerenciar", .icon = CUI_ICON_ELLIPSIS, .icon_only = true, .placement = CUI_PLACE_BOTTOM_END) {
                        cui_menu_label(ui, m->name);
                        if (g->is_owner && cui_menu_item(ui, m->admin ? "Remover administrador" : "Tornar administrador", .icon = CUI_ICON_SHIELD)) a = 1;
                        if (cui_menu_item(ui, "Remover do grupo", .icon = CUI_ICON_X, .danger = true)) a = 2;
                    }
                    if (a) { act = a; act_i = i; }
                }
            }
            cui_pop_id(ui);
        }
        const int nd = dup_count(g);
        if (nd && g->can_moderate && cui_button(ui, nd == 1 ? "Limpar 1 duplicado" : "Limpar duplicados", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM, .icon = CUI_ICON_X)) dedup = true;
    }
    if (reload) s->reload_groups = true;
    if (add) s->members_open = true;
    if (dedup) remove_duplicates(s, gi);
    if (act == 1) set_admin(s, gi, act_i, !g->m[act_i].admin);
    else if (act == 2) { g_mem_sel = act_i; g_memrm_open = true; }
    return true;
}

/* menu de contexto (botão direito) da linha de um grupo na barra lateral; r = retângulo da linha. Chamar dentro do id do grupo. */
static bool g_ctx_open; static int g_ctx_gi; static float g_ctx_x, g_ctx_y;
void groups_row_menu(cui_ctx *ui, app_state_t *s, int gi, cui_rect r) {
    (void)s;
    cui_vec2 mp = cui_mouse_pos(ui);
    if (!briks_blocked() && cui_mouse_clicked(ui, CUI_MOUSE_RIGHT) && r.w > 0 && mp.x >= r.x && mp.y >= r.y && mp.x < r.x + r.w && mp.y < r.y + r.h) {
        g_ctx_open = true; g_ctx_gi = gi; g_ctx_x = mp.x; g_ctx_y = mp.y;
    }
}

static bool ctx_item(cui_ctx *ui, const char *id, const char *icon, const char *label, bool danger) {
    cui_box(ui, .id = id, .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 12, .pad_y = 9, .gap = 10, .radius = 11, .align = CUI_ALIGN_CENTER,
            .bg = briks_hover_top(ui, id) ? CUI_RGBA(0xffffff1c) : CUI_RGBA(0x00000000)) {
        cui_icon(ui, icon, 16, danger ? B_DANGER : B_MUTED);
        cui_text(ui, label, .size = 14, .color = danger ? B_DANGER : B_TEXT, .nowrap = true);
    }
    return briks_clicked_top(ui, id);
}

/* o menu é um painel de vidro absoluto ancorado no mouse (como o menu da mensagem) e conta como modal: nada por baixo recebe clique */
void groups_overlay(cui_ctx *ui, app_state_t *s) {
    if (!g_ctx_open) return;
    if (g_ctx_gi < 0 || g_ctx_gi >= s->group_count) { g_ctx_open = false; return; }
    const int gi = g_ctx_gi; group_t *g = &s->groups[gi]; int act = 0;
    cui_vec2 vp = cui_viewport_size(ui), mp = cui_mouse_pos(ui);
    const float mw = 270.0f, mh = 20.0f + 28.0f + 40.0f * (1.0f + (g->can_moderate ? 2.0f : 0.0f) + 1.0f) + 10.0f;
    const float x = fminf(fmaxf(g_ctx_x, 8), fmaxf(vp.x - mw - 8, 8)), y = fminf(fmaxf(g_ctx_y, 8), fmaxf(vp.y - mh - 8, 8));
    g_modal_now = true; g_in_modal++;
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_START, .pad_left = x, .pad_top = y) {
        cui_box(ui, .id = "gctx", .width = CUI_PX(mw), .height = CUI_FIT, .pad = 6, .gap = 2, .radius = 16, .bg = B_GLASS_POPUP, .border = 1, .border_color = CUI_RGBA(0xc4b5fd40),
                .elevation = CUI_ELEVATION_FLOATING, .clip = true) {
            cui_box(ui, .width = CUI_FILL, .pad_x = 12, .pad_y = 6) { cui_text(ui, g->name, .size = 12.5f, .color = B_MUTED, .nowrap = true); }
            if (ctx_item(ui, "c-mem", CUI_ICON_USERS, "Membros", false)) act = 1;
            if (g->can_moderate && ctx_item(ui, "c-inv", CUI_ICON_LINK, "Convidar (gerar código)", false)) act = 2;
            if (g->can_moderate && ctx_item(ui, "c-cus", CUI_ICON_PENCIL, "Personalizar grupo", false)) act = 3;
            cui_box(ui, .width = CUI_FILL, .height = CUI_PX(1), .bg = CUI_RGBA(0xffffff14)) {}
            if (ctx_item(ui, "c-out", CUI_ICON_X, g->is_owner ? "Apagar grupo…" : "Sair do grupo…", true)) act = 4;
        }
    }
    g_in_modal--;
    cui_rect mr = cui_get_rect(ui, cui_make_id(ui, "gctx"));    const bool outside = mr.w > 0 && !(mp.x >= mr.x && mp.y >= mr.y && mp.x < mr.x + mr.w && mp.y < mr.y + mr.h);
    if (cui_key_pressed(ui, CUI_KEY_ESCAPE) || (outside && (cui_mouse_clicked(ui, CUI_MOUSE_LEFT) || cui_mouse_clicked(ui, CUI_MOUSE_RIGHT)))) g_ctx_open = false;
    if (!act) return;
    g_ctx_open = false;
    if (s->active_group != gi) app_open_group(s, gi);
    if (act == 1) s->members_open = true;
    else if (act == 2) send_group_code(s, gi);
    else if (act == 3) { snprintf(s->grp_name_edit, sizeof s->grp_name_edit, "%s", g->name); s->gren_open = true; }
    else s->gleave_open = true;
}

/* roteiro oculto: `expect grp_<estado> <valor>` confere o grupo aberto (BRIKS_SCRIPT). -1 = estado desconhecido. */
int groups_probe(const app_state_t *s, const char *n) {
    if (!strcmp(n, "grp_ctx_open")) return g_ctx_open;
    if (!strcmp(n, "grp_members_open")) return s->members_open;
    if (!strcmp(n, "grp_leave_open")) return s->gleave_open;
    if (!strcmp(n, "grp_gren_open")) return s->gren_open;
    if (!strcmp(n, "grp_join_open")) return s->join_open;
    if (!strcmp(n, "grp_chan_open")) return s->chan_open;
    if (!strcmp(n, "grp_ren_open")) return g_ren_open;
    if (!strcmp(n, "grp_del_open")) return g_del_open;
    if (!strcmp(n, "grp_memrm_open")) return g_memrm_open;
    if (!strcmp(n, "grp_count")) return s->group_count;
    if (s->active_group < 0 || s->active_group >= s->group_count) return -1;
    const group_t *g = &s->groups[s->active_group];
    if (!strcmp(n, "grp_nch")) return g->nch;
    if (!strcmp(n, "grp_nm")) return g->nm;
    if (!strcmp(n, "grp_ntext")) { int t = 0; for (int i = 0; i < g->nch; i++) if (!g->ch[i].voice) t++; return t; }
    if (!strcmp(n, "grp_nadmin")) { int a = 0; for (int i = 0; i < g->nm; i++) if (g->m[i].admin) a++; return a; }
    if (!strcmp(n, "grp_dups")) return dup_count(g);
    if (!strcmp(n, "grp_ch1len")) return g->nch > 1 ? (int)strlen(g->ch[1].name) : -1;   /* comprimento do nome do 2º canal (confere renomear) */
    if (!strcmp(n, "grp_namelen")) return (int)strlen(g->name);
    if (!strcmp(n, "grp_moderate")) return g->can_moderate;
    if (!strcmp(n, "grp_voice_who") || !strcmp(n, "grp_voice_live")) {   /* presença por sala: total de pessoas / de transmissões nas salas de voz */
        int t = 0; for (int i = 0; i < g->nch; i++) if (g->ch[i].voice) {
            rmem_t w[8]; bool m; int k = roster_get(s, &g->ch[i], w, &m);
            for (int q = 0; q < k; q++) t += !strcmp(n, "grp_voice_who") ? 1 : w[q].live;
        } return t;
    }
    if (!strcmp(n, "grp_call_mode")) return s->screen == SCREEN_CALL ? 10 + s->call_mode : -1;   /* 11 = abriu a chamada no modo Transmissão */
    return -1;
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
