#include "layout.h"
#include "json.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const struct { const char *name, *title; float mw, mh; } PANELS[P_COUNT] = {
    { "servers", "Servidores",    56, 200 },
    { "nav",     "Menu lateral", 200, 240 },
    { "chat",    "Conversa",     320, 260 },
    { "friends", "Amigos",       200, 200 },
    { "members", "Membros",      180, 200 },
    { "voice",   "Sala de voz",  220, 180 },
    { "history", "Histórico",    240, 180 },
};

const char *panel_name(int k) { return k >= 0 && k < P_COUNT ? PANELS[k].name : "?"; }
const char *panel_title(int k) { return k >= 0 && k < P_COUNT ? PANELS[k].title : "?"; }
float panel_min_w(int k) { return k >= 0 && k < P_COUNT ? PANELS[k].mw : 100; }
float panel_min_h(int k) { return k >= 0 && k < P_COUNT ? PANELS[k].mh : 100; }
int panel_from_name(const char *n) {
    for (int i = 0; i < P_COUNT; i++) if (!strcmp(PANELS[i].name, n)) return i;
    return -1;
}

static int alloc(layout_t *l) {
    for (int i = 0; i < LAYOUT_MAX_NODES; i++) if (!l->n[i].used) { memset(&l->n[i], 0, sizeof l->n[i]); l->n[i].used = true; l->n[i].parent = -1; return i; }
    return -1;
}
static int new_leaf(layout_t *l, int kind) { int i = alloc(l); if (i >= 0) { l->n[i].leaf = true; l->n[i].panel = kind; } return i; }

void layout_default(layout_t *l, float width) {
    memset(l, 0, sizeof *l);
    float t = width - 20.0f - LAYOUT_GAP;
    if (t < 400) t = 400;
    int s = new_leaf(l, P_SERVERS), n = new_leaf(l, P_NAV), c = new_leaf(l, P_CHAT);
    int inner = alloc(l), root = alloc(l);
    float t2 = t - 76.0f - LAYOUT_GAP;
    l->n[inner].vertical = false; l->n[inner].ratio = 270.0f / t2; l->n[inner].a = n; l->n[inner].b = c;
    l->n[n].parent = inner; l->n[c].parent = inner;
    l->n[root].vertical = false; l->n[root].ratio = 76.0f / t; l->n[root].a = s; l->n[root].b = inner;
    l->n[s].parent = root; l->n[inner].parent = root;
    l->root = root;
}

/* Padrão do app (primeiro uso e "Restaurar layout padrão"): grupos | menu | conversa (larga) | Amigos à direita.
 * Em janela estreita (< 1240 px) não cabe o quarto painel: fica o trio de layout_default e a tela Amigos aparece no painel da conversa. */
void layout_app_default(layout_t *l, float width) {
    layout_default(l, width);
    if (width < 1240.0f || !layout_split(l, P_CHAT, P_FRIENDS, Z_RIGHT)) return;
    int f = layout_find(l, P_FRIENDS), p = f >= 0 ? l->n[f].parent : -1;
    if (p < 0) return;
    float rest = width - 20.0f - 76.0f - 270.0f - 3.0f * LAYOUT_GAP;          /* conversa + Amigos */
    float fw = width * 0.26f; if (fw < 300.0f) fw = 300.0f; if (fw > 440.0f) fw = 440.0f;
    if (rest > fw + 340.0f) l->n[p].ratio = 1.0f - fw / rest;
}

int layout_find(const layout_t *l, int kind) {
    for (int i = 0; i < LAYOUT_MAX_NODES; i++) if (l->n[i].used && l->n[i].leaf && l->n[i].panel == kind) return i;
    return -1;
}
int layout_count(const layout_t *l) {
    int c = 0; for (int i = 0; i < LAYOUT_MAX_NODES; i++) if (l->n[i].used && l->n[i].leaf) c++;
    return c;
}

static int walk(const layout_t *l, int i, int parent, int depth, unsigned *seen_kinds, int *visited) {
    if (i < 0 || i >= LAYOUT_MAX_NODES || depth > LAYOUT_MAX_NODES) return 0;
    const lnode *n = &l->n[i];
    if (!n->used || n->parent != parent) return 0;
    (*visited)++;
    if (n->leaf) {
        if (n->panel < 0 || n->panel >= P_COUNT || (*seen_kinds & (1u << n->panel))) return 0;
        *seen_kinds |= 1u << n->panel;
        return 1;
    }
    if (!(n->ratio > 0.0f && n->ratio < 1.0f)) return 0;
    return walk(l, n->a, i, depth + 1, seen_kinds, visited) && walk(l, n->b, i, depth + 1, seen_kinds, visited);
}
bool layout_valid(const layout_t *l) {
    unsigned seen = 0; int visited = 0, used = 0;
    if (!walk(l, l->root, -1, 0, &seen, &visited)) return false;
    for (int i = 0; i < LAYOUT_MAX_NODES; i++) used += l->n[i].used;
    return used == visited && seen != 0;
}

/* menor tamanho que a subárvore aceita no eixo (0 = largura, 1 = altura) */
static float min_extent(const layout_t *l, int i, int axis) {
    const lnode *n = &l->n[i];
    if (n->leaf) return axis ? panel_min_h(n->panel) : panel_min_w(n->panel);
    float a = min_extent(l, n->a, axis), b = min_extent(l, n->b, axis);
    bool along = (axis == 1) == n->vertical;
    return along ? a + b + LAYOUT_GAP : (a > b ? a : b);
}

/* tamanho do primeiro filho dentro do total T (sem o vão) */
static float first_size(const layout_t *l, int i, float total) {
    const lnode *n = &l->n[i];
    int axis = n->vertical ? 1 : 0;
    float ma = min_extent(l, n->a, axis), mb = min_extent(l, n->b, axis);
    float sa = total * n->ratio;
    if (total >= ma + mb) { if (sa < ma) sa = ma; if (sa > total - mb) sa = total - mb; }
    else sa = total * ma / (ma + mb);
    return sa;
}

static void compute(const layout_t *l, int i, lrect r, lrect *out) {
    out[i] = r;
    const lnode *n = &l->n[i];
    if (n->leaf) return;
    if (!n->vertical) {
        float t = r.w - LAYOUT_GAP; if (t < 1) t = 1;
        float sa = first_size(l, i, t);
        compute(l, n->a, (lrect){ r.x, r.y, sa, r.h }, out);
        compute(l, n->b, (lrect){ r.x + sa + LAYOUT_GAP, r.y, t - sa, r.h }, out);
    } else {
        float t = r.h - LAYOUT_GAP; if (t < 1) t = 1;
        float sa = first_size(l, i, t);
        compute(l, n->a, (lrect){ r.x, r.y, r.w, sa }, out);
        compute(l, n->b, (lrect){ r.x, r.y + sa + LAYOUT_GAP, r.w, t - sa }, out);
    }
}
void layout_compute(const layout_t *l, lrect area, lrect out[LAYOUT_MAX_NODES]) {
    memset(out, 0, sizeof(lrect) * LAYOUT_MAX_NODES);
    compute(l, l->root, area, out);
}

static void replace_child(layout_t *l, int parent, int old, int neu) {
    if (parent < 0) { l->root = neu; return; }
    if (l->n[parent].a == old) l->n[parent].a = neu; else l->n[parent].b = neu;
}

bool layout_split(layout_t *l, int target_kind, int new_kind, zone_t side) {
    if (side == Z_CENTER || side == Z_NONE || new_kind < 0 || new_kind >= P_COUNT) return false;
    int t = layout_find(l, target_kind);
    if (t < 0 || layout_find(l, new_kind) >= 0) return false;
    int nl = new_leaf(l, new_kind), s = alloc(l);
    if (nl < 0 || s < 0) { if (nl >= 0) l->n[nl].used = false; if (s >= 0) l->n[s].used = false; return false; }
    int parent = l->n[t].parent;
    replace_child(l, parent, t, s);
    l->n[s].parent = parent; l->n[s].ratio = 0.5f;
    l->n[s].vertical = side == Z_TOP || side == Z_BOTTOM;
    bool first = side == Z_LEFT || side == Z_TOP;
    l->n[s].a = first ? nl : t; l->n[s].b = first ? t : nl;
    l->n[t].parent = s; l->n[nl].parent = s;
    return true;
}

bool layout_remove(layout_t *l, int kind) {
    int n = layout_find(l, kind);
    if (n < 0 || layout_count(l) < 2) return false;
    int p = l->n[n].parent;
    int sib = l->n[p].a == n ? l->n[p].b : l->n[p].a;
    int gp = l->n[p].parent;
    replace_child(l, gp, p, sib);
    l->n[sib].parent = gp;
    l->n[n].used = false; l->n[p].used = false;
    return true;
}

bool layout_move(layout_t *l, int kind, int target_kind, zone_t zone) {
    int a = layout_find(l, kind), b = layout_find(l, target_kind);
    if (a < 0 || b < 0 || a == b || zone == Z_NONE) return false;
    if (zone == Z_CENTER) { int t = l->n[a].panel; l->n[a].panel = l->n[b].panel; l->n[b].panel = t; return true; }
    layout_t backup = *l;
    if (!layout_remove(l, kind) || !layout_split(l, target_kind, kind, zone)) { *l = backup; return false; }
    return true;
}

bool layout_resize(layout_t *l, int split, lrect area, float pos) {
    if (split < 0 || split >= LAYOUT_MAX_NODES || !l->n[split].used || l->n[split].leaf) return false;
    lnode *n = &l->n[split];
    int axis = n->vertical ? 1 : 0;
    float total = (axis ? area.h : area.w) - LAYOUT_GAP;
    float ma = min_extent(l, n->a, axis), mb = min_extent(l, n->b, axis);
    if (total < ma + mb || total <= 0) return false;
    float sa = pos - (axis ? area.y : area.x) - LAYOUT_GAP * 0.5f;
    if (sa < ma) sa = ma;
    if (sa > total - mb) sa = total - mb;
    n->ratio = sa / total;
    n->manual = true;
    return true;
}

zone_t layout_zone_at(lrect r, float x, float y) {
    if (x < r.x || y < r.y || x > r.x + r.w || y > r.y + r.h) return Z_NONE;
    float fx = (x - r.x) / r.w, fy = (y - r.y) / r.h;
    /* a borda mais próxima vence quando está dentro dos 25% */
    float dl = fx, dr = 1 - fx, dt = fy, db = 1 - fy;
    float m = dl; zone_t z = Z_LEFT;
    if (dr < m) { m = dr; z = Z_RIGHT; }
    if (dt < m) { m = dt; z = Z_TOP; }
    if (db < m) { m = db; z = Z_BOTTOM; }
    return m < 0.25f ? z : Z_CENTER;
}
lrect layout_zone_rect(lrect r, zone_t z) {
    switch (z) {
    case Z_LEFT: return (lrect){ r.x, r.y, r.w * 0.5f, r.h };
    case Z_RIGHT: return (lrect){ r.x + r.w * 0.5f, r.y, r.w * 0.5f, r.h };
    case Z_TOP: return (lrect){ r.x, r.y, r.w, r.h * 0.5f };
    case Z_BOTTOM: return (lrect){ r.x, r.y + r.h * 0.5f, r.w, r.h * 0.5f };
    default: return r;
    }
}

/* ---- JSON: {"p":"chat"} ou {"v":0,"r":0.23,"a":{...},"b":{...}} ---- */
static size_t emit(const layout_t *l, int i, char *out, size_t cap, size_t pos) {
    if (pos >= cap) return pos;
    const lnode *n = &l->n[i];
    int w;
    if (n->leaf) w = snprintf(out + pos, cap - pos, "{\"p\":\"%s\"}", panel_name(n->panel));
    else {
        w = snprintf(out + pos, cap - pos, n->manual ? "{\"v\":%d,\"r\":%.4f,\"m\":1,\"a\":" : "{\"v\":%d,\"r\":%.4f,\"a\":", n->vertical ? 1 : 0, n->ratio);
        if (w < 0) return cap;
        pos = emit(l, n->a, out, cap, pos + (size_t)w);
        if (pos >= cap) return pos;
        w = snprintf(out + pos, cap - pos, ",\"b\":");
        if (w < 0) return cap;
        pos = emit(l, n->b, out, cap, pos + (size_t)w);
        if (pos >= cap) return pos;
        w = snprintf(out + pos, cap - pos, "}");
    }
    return w < 0 ? cap : pos + (size_t)w;
}
size_t layout_to_json(const layout_t *l, char *out, size_t cap) {
    size_t n = emit(l, l->root, out, cap, 0);
    if (n >= cap) { if (cap) out[0] = 0; return 0; }
    return n;
}

static int parse_node(layout_t *l, const jval *j, int parent, int depth) {
    if (!j || j->type != J_OBJ || depth > 12) return -1;
    const char *p = json_str(j, "p", "");
    if (p[0]) {
        if (!strcmp(p, "chat2")) return -2;   /* painel removido na 1.4.1: quem chama some com ele e o vizinho ocupa o lugar */
        int k = panel_from_name(p);
        if (k < 0) return -1;
        int i = new_leaf(l, k);
        if (i >= 0) l->n[i].parent = parent;
        return i;
    }
    const jval *r = json_get(j, "r"), *v = json_get(j, "v");
    if (!r || r->type != J_NUM) return -1;
    int i = alloc(l);
    if (i < 0) return -1;
    l->n[i].parent = parent;
    l->n[i].ratio = (float)r->num;
    l->n[i].vertical = v && v->num != 0;
    { const jval *m = json_get(j, "m"); l->n[i].manual = m && m->type == J_NUM && m->num != 0; }
    int a = parse_node(l, json_get(j, "a"), i, depth + 1);
    int b = a == -1 ? -1 : parse_node(l, json_get(j, "b"), i, depth + 1);
    if (a == -1 || b == -1) return -1;
    if (a == -2 || b == -2) {   /* um lado era o painel removido: esta divisão some e o outro lado sobe para o lugar dela */
        l->n[i].used = false;
        int keep = a == -2 ? b : a;
        if (keep >= 0) l->n[keep].parent = parent;
        return keep;
    }
    l->n[i].a = a; l->n[i].b = b;
    return i;
}
bool layout_from_json(layout_t *l, const char *text) {
    jval *j = json_parse(text);
    layout_t tmp; memset(&tmp, 0, sizeof tmp);
    int root = j ? parse_node(&tmp, j, -1, 0) : -1;
    json_free(j);
    if (root < 0) return false;
    tmp.root = root;
    if (!layout_valid(&tmp)) return false;
    *l = tmp;
    return true;
}

/* ---- reajuste ao vivo ---- */
bool layout_preview(const layout_t *l, int kind, bool from_menu, int target_kind, zone_t z, layout_t *out) {
    *out = *l;
    if (z == Z_NONE || target_kind < 0 || target_kind == kind || layout_find(l, target_kind) < 0) return false;
    bool ok = from_menu ? layout_split(out, target_kind, kind, z == Z_CENTER ? Z_RIGHT : z) : layout_move(out, kind, target_kind, z);
    if (!ok) *out = *l;
    return ok;
}

void zone_hold_reset(zone_hold *h) { h->cur_target = -1; h->cur_zone = Z_NONE; h->cand_target = -1; h->cand_zone = Z_NONE; h->held = 0; }

bool zone_hold_update(zone_hold *h, int target_kind, zone_t z, float dt, float hold_s) {
    if (target_kind < 0 || z == Z_NONE) { h->cand_target = -1; h->cand_zone = Z_NONE; h->held = 0; return false; }
    if (target_kind == h->cur_target && z == h->cur_zone) { h->cand_target = -1; h->cand_zone = Z_NONE; h->held = 0; return false; }
    if (target_kind != h->cand_target || z != h->cand_zone) { h->cand_target = target_kind; h->cand_zone = z; h->held = 0; }
    h->held += dt;
    if (h->cur_target < 0 || h->held >= hold_s) {   /* primeira zona: imediato */
        h->cur_target = target_kind; h->cur_zone = z; h->cand_target = -1; h->cand_zone = Z_NONE; h->held = 0;
        return true;
    }
    return false;
}

static bool approach(float *v, float t, float k) {
    float d = t - *v;
    if (d > -0.75f && d < 0.75f) { bool moved = *v != t; *v = t; return moved; }
    *v += d * k; return true;
}
bool lrect_step(lrect *cur, lrect t, float dt, float tau) {
    float k = tau <= 0 || dt <= 0 ? 1.0f : 1.0f - (float)exp(-(double)dt / (double)tau);
    if (tau <= 0) k = 1.0f;
    bool m = false;
    m |= approach(&cur->x, t.x, k); m |= approach(&cur->y, t.y, k);
    m |= approach(&cur->w, t.w, k); m |= approach(&cur->h, t.h, k);
    return m;
}
