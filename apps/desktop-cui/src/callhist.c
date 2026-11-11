/* Lógica pura da chamada: ver callhist.h. */
#include "callhist.h"
#include "json.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void callhist_init(callhist_t *h) { memset(h, 0, sizeof *h); h->keep = true; }

bool callhist_add(callhist_t *h, const call_rec_t *rec) {
    if (!h->keep || rec->duration <= 0) return false;
    int keepn = h->n < CALLHIST_MAX ? h->n : CALLHIST_MAX - 1;   /* cheio: a mais antiga sai */
    memmove(&h->r[1], &h->r[0], sizeof(call_rec_t) * (size_t)keepn);
    h->r[0] = *rec;
    h->n = keepn + 1;
    return true;
}

void callhist_clear(callhist_t *h) { h->n = 0; memset(h->r, 0, sizeof h->r); }

bool callhist_people_add(char *list, size_t cap, int *n, const char *name) {
    if (!name || !name[0]) return false;
    size_t nl = strlen(name);
    for (const char *p = list; *p;) {   /* já está? compara item a item */
        const char *e = strstr(p, ", "); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == nl && !strncmp(p, name, nl)) return false;
        if (!e) break;
        p = e + 2;
    }
    size_t cur = strlen(list);
    if (cur + nl + 3 > cap) return false;
    snprintf(list + cur, cap - cur, "%s%s", cur ? ", " : "", name);
    if (n) (*n)++;
    return true;
}

size_t callhist_to_json(const callhist_t *h, char *out, size_t cap) {
    size_t n = 0;
    if (!cap) return 0;
    n += (size_t)snprintf(out + n, cap - n, "{\"version\":1,\"keep\":%s,\"calls\":[", h->keep ? "true" : "false");
    for (int i = 0; i < h->n && n + 700 < cap; i++) {
        const call_rec_t *r = &h->r[i];
        char room[140], title[200], people[500];
        json_escape(r->room, room, sizeof room); json_escape(r->title, title, sizeof title); json_escape(r->people, people, sizeof people);
        n += (size_t)snprintf(out + n, cap - n, "%s{\"started\":%lld,\"duration\":%d,\"room\":\"%s\",\"title\":\"%s\",\"people\":\"%s\",\"npeople\":%d,\"shared\":%s}",
                              i ? "," : "", r->started, r->duration, room, title, people, r->npeople, r->shared ? "true" : "false");
    }
    n += (size_t)snprintf(out + n, cap - n, "]}");
    return n < cap ? n : cap - 1;
}

static double num(const jval *o, const char *k) { const jval *v = json_get(o, k); return v && v->type == J_NUM ? v->num : 0; }
static bool boolean(const jval *o, const char *k, bool dflt) { const jval *v = json_get(o, k); return v && v->type == J_BOOL ? v->b : dflt; }

bool callhist_from_json(callhist_t *h, const char *text) {
    callhist_init(h);
    jval *j = text ? json_parse(text) : NULL;
    if (!j || j->type != J_OBJ) { if (j) json_free(j); return false; }
    h->keep = boolean(j, "keep", true);
    const jval *calls = json_get(j, "calls");
    int cnt = calls && calls->type == J_ARR ? json_len(calls) : 0;
    for (int i = 0; i < cnt && h->n < CALLHIST_MAX; i++) {
        const jval *c = json_at(calls, i);
        if (!c || c->type != J_OBJ) continue;
        call_rec_t *r = &h->r[h->n];
        memset(r, 0, sizeof *r);
        r->started = (long long)num(c, "started"); r->duration = (int)num(c, "duration");
        if (r->started <= 0 || r->duration <= 0 || r->duration > 7 * 24 * 3600) continue;   /* registro estragado: pula */
        snprintf(r->room, sizeof r->room, "%s", json_str(c, "room", ""));
        snprintf(r->title, sizeof r->title, "%s", json_str(c, "title", ""));
        snprintf(r->people, sizeof r->people, "%s", json_str(c, "people", ""));
        r->npeople = (int)num(c, "npeople"); if (r->npeople < 0) r->npeople = 0;
        r->shared = boolean(c, "shared", false);
        h->n++;
    }
    json_free(j);
    return true;
}

void call_fmt_duration(int secs, char *out, size_t cap) {
    if (secs < 0) secs = 0;
    if (secs < 60) snprintf(out, cap, "%d s", secs);
    else if (secs < 3600) snprintf(out, cap, "%d min", secs / 60);
    else snprintf(out, cap, "%d h %02d min", secs / 3600, (secs % 3600) / 60);
}

void call_fmt_when(int y, int mo, int d, int hh, int mi, int ny, int nmo, int nd, bool yesterday, char *out, size_t cap) {
    if (y == ny && mo == nmo && d == nd) snprintf(out, cap, "hoje %02d:%02d", hh, mi);
    else if (yesterday) snprintf(out, cap, "ontem %02d:%02d", hh, mi);
    else if (y == ny) snprintf(out, cap, "%02d/%02d %02d:%02d", d, mo, hh, mi);
    else snprintf(out, cap, "%02d/%02d/%04d %02d:%02d", d, mo, y, hh, mi);
}

void call_grid(int n, float w, float h, float gap, int *cols, int *rows) {
    if (n < 1) n = 1;
    int best = 1; float best_score = -1;
    for (int c = 1; c <= n; c++) {
        int r = (n + c - 1) / c;
        float tw = (w - gap * (float)(c - 1)) / (float)c, th = (h - gap * (float)(r - 1)) / (float)r;
        if (tw <= 0 || th <= 0) continue;
        float score = fminf(tw, th * 1.6f);   /* lado útil de um bloco 16:10 que cabe ali */
        if (score > best_score + 0.5f) { best_score = score; best = c; }
    }
    *cols = best; *rows = (n + best - 1) / best;
}

int call_net_quality(const netq_in_t *in) {
    const char *st = in->state ? in->state : "";
    if (!st[0]) return NETQ_OFF;
    if (!strcmp(st, "erro") || !strcmp(st, "desconectado")) return NETQ_DOWN;
    if (!strcmp(st, "reconectando")) return NETQ_UNSTABLE;
    if (strcmp(st, "conectado")) return NETQ_CONNECTING;
    if (in->stun_done && !in->stun_ok) return NETQ_UNSTABLE;                              /* o teste não achou saída UDP */
    if (in->failed > 0) return NETQ_UNSTABLE;                                             /* alguém da sala não conectou comigo */
    if (in->stun_done && (in->rtt_ms >= 150 || in->loss_pct >= 5)) return NETQ_UNSTABLE;
    if (in->peers > in->authed) return NETQ_CONNECTING;                                   /* ainda negociando com alguém */
    return NETQ_GOOD;
}

const char *call_net_label(int q) {
    switch (q) {
    case NETQ_GOOD: return "Rede boa";
    case NETQ_UNSTABLE: return "Rede instável";
    case NETQ_DOWN: return "Sem conexão";
    case NETQ_CONNECTING: return "Conectando";
    default: return "Fora da sala";
    }
}

bool share_notice_wanted(bool was_on, bool now_on, bool viewing) { return now_on && !was_on && !viewing; }

int share_zoom_pct(float zoom, float fit) {
    if (zoom <= 0 || fit <= 0) return 100;
    int p = (int)(zoom / fit * 100.0f + 0.5f);
    return p < 100 ? 100 : p;
}

bool share_zoom_clamp(float *zoom, float *cx, float *cy, float fit, float max_mult) {
    if (*zoom <= 0 || fit <= 0) return false;
    if (*zoom <= fit * 1.02f) { *zoom = 0; *cx = 0; *cy = 0; return false; }   /* de volta ao encaixe: esquece a posição */
    if (max_mult > 1 && *zoom > fit * max_mult) *zoom = fit * max_mult;
    return true;
}
