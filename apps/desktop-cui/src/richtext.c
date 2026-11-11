#include "richtext.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct { char *o; size_t cap, n; } buf_t;
static void put(buf_t *b, char c) { if (b->n + 1 < b->cap) b->o[b->n++] = c; }
static void puts_(buf_t *b, const char *s) { while (*s) put(b, *s++); }
static bool name_ch(unsigned char c) { return isalnum(c) || c == '_' || c == '-' || c >= 0x80; }

bool rich_safe_url(const char *u) {
    size_t n = strlen(u);
    if (n < 9 || n > 2048) return false;
    if (_strnicmp(u, "https://", 8) && _strnicmp(u, "http://", 7)) return false;
    for (size_t i = 0; i < n; i++) { unsigned char c = (unsigned char)u[i]; if (c <= 0x20 || c == 0x7f || c == '<' || c == '>' || c == '"') return false; }
    return n > (size_t)(u[4] == 's' || u[4] == 'S' ? 8 : 7);
}

static size_t url_len(const char *p) {   /* http(s)://... até espaço ou < ; sem pontuação final */
    size_t n = 0; while (p[n] && !isspace((unsigned char)p[n]) && p[n] != '<') n++;
    while (n > 8 && strchr(".,;:!?)]}'\"", p[n - 1])) n--;
    return n;
}
static bool at_url(const char *p) { return !_strnicmp(p, "https://", 8) || !_strnicmp(p, "http://", 7); }

static void esc(buf_t *b, char c, bool line_start) {
    if (strchr("\\*_~#>|[]<`", c)) { put(b, '\\'); put(b, c); return; }
    if (line_start && (c == '-' || c == '+' || c == '=')) { put(b, '\\'); put(b, c); return; }
    put(b, c);
}

size_t rich_prepare(const char *in, char *out, size_t cap) {
    buf_t b = { out, cap, 0 };
    if (!cap) return 0;
    bool ls = true;
    for (const char *p = in; *p;) {
        if (*p == '\r') { p++; continue; }
        if (*p == '\n') { puts_(&b, "  \n"); p++; ls = true; continue; }
        /* [texto](url) */
        if (*p == '[') {
            const char *e = strchr(p, ']');
            if (e && e[1] == '(' && (size_t)(e - p) <= 161 && !memchr(p, '\n', (size_t)(e - p))) {
                const char *u = e + 2, *c = strchr(u, ')');
                if (c) {
                    char url[2100]; size_t un = (size_t)(c - u);
                    if (un < sizeof url) {
                        memcpy(url, u, un); url[un] = 0;
                        if (rich_safe_url(url) && e - p > 1) {
                            put(&b, '['); for (const char *q = p + 1; q < e; q++) esc(&b, *q, false); puts_(&b, "]("); puts_(&b, url); put(&b, ')');
                            p = c + 1; ls = false; continue;
                        }
                    }
                }
            }
        }
        /* link solto */
        if ((*p == 'h' || *p == 'H') && at_url(p)) {
            size_t n = url_len(p); char url[2100];
            if (n < sizeof url) { memcpy(url, p, n); url[n] = 0; if (rich_safe_url(url)) { put(&b, '<'); puts_(&b, url); put(&b, '>'); p += n; ls = false; continue; } }
        }
        /* **negrito** */
        if (p[0] == '*' && p[1] == '*') {
            const char *e = strstr(p + 2, "**");
            if (e && e > p + 2 && !memchr(p, '\n', (size_t)(e - p)) && !memchr(p + 2, '*', (size_t)(e - p - 2))) {
                puts_(&b, "**"); for (const char *q = p + 2; q < e; q++) esc(&b, *q, false); puts_(&b, "**"); p = e + 2; ls = false; continue;
            }
        }
        /* `código` */
        if (*p == '`') {
            const char *e = strchr(p + 1, '`');
            if (e && e > p + 1 && !memchr(p, '\n', (size_t)(e - p))) {
                put(&b, '`'); for (const char *q = p + 1; q < e; q++) put(&b, *q); put(&b, '`'); p = e + 1; ls = false; continue;
            }
        }
        /* menções */
        if (*p == '@') {
            if (p[1] == '[') {
                const char *e = strchr(p + 2, ']');
                if (e && e - (p + 2) >= 2 && e - (p + 2) <= 80 && !memchr(p, '\n', (size_t)(e - p))) {
                    puts_(&b, "**@"); for (const char *q = p + 2; q < e; q++) esc(&b, *q, false); puts_(&b, "**"); p = e + 1; ls = false; continue;
                }
            } else {
                size_t n = 0; while (name_ch((unsigned char)p[1 + n]) && n < 80) n++;
                if (n >= 2 && (p == in || isspace((unsigned char)p[-1]))) {
                    puts_(&b, "**@"); for (size_t k = 0; k < n; k++) esc(&b, p[1 + k], false); puts_(&b, "**"); p += 1 + n; ls = false; continue;
                }
            }
        }
        if (isdigit((unsigned char)*p) && ls) {   /* "1. item" não vira lista */
            const char *q = p; while (isdigit((unsigned char)*q)) q++;
            if ((*q == '.' || *q == ')') && q[1] == ' ') { while (p < q) put(&b, *p++); put(&b, '\\'); put(&b, *p++); ls = false; continue; }
        }
        esc(&b, *p, ls);
        if (*p != ' ') ls = false;
        p++;
    }
    b.o[b.n] = 0;
    return b.n;
}

int rich_urls(const char *text, char urls[][220], int max) {
    int n = 0;
    for (const char *p = text; *p && n < max; p++) {
        if (!((*p == 'h' || *p == 'H') && at_url(p))) continue;
        size_t l = url_len(p); char u[2100];
        if (l >= sizeof u) { p += l - 1; continue; }
        memcpy(u, p, l); u[l] = 0; p += l - 1;
        if (!rich_safe_url(u) || strlen(u) >= 220) continue;
        bool dup = false; for (int i = 0; i < n; i++) if (!strcmp(urls[i], u)) dup = true;
        if (!dup) snprintf(urls[n++], 220, "%s", u);
    }
    return n;
}

bool rich_mention_query(const char *d, char *q, size_t cap, int *start) {
    size_t n = strlen(d), i = n;
    while (i > 0 && name_ch((unsigned char)d[i - 1])) i--;
    if (i == 0 || d[i - 1] != '@') return false;
    size_t at = i - 1;
    if (at > 0 && !isspace((unsigned char)d[at - 1])) return false;
    if (n - i >= cap) return false;
    memcpy(q, d + i, n - i); q[n - i] = 0; if (start) *start = (int)at;
    return true;
}

bool rich_apply_mention(char *d, size_t cap, const char *name) {
    char q[96]; int at;
    if (!rich_mention_query(d, q, sizeof q, &at)) return false;
    if ((size_t)at + strlen(name) + 4 >= cap) return false;
    snprintf(d + at, cap - (size_t)at, "@[%s] ", name);
    return true;
}

bool rich_group_with_prev(const char *pa, long long pms, const char *a, long long ms, bool reply) {
    if (reply || !pa || !a || strcmp(pa, a) || pms < 0 || ms < 0) return false;
    long long d = ms - pms; if (d < 0) d = -d;
    return d < 5 * 60 * 1000;
}

long long rich_iso_ms(const char *s) {
    int Y, M, D, h, m, sec, ms = 0;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6) return -1;
    const char *dot = strchr(s, '.');
    if (dot) { int dgt = 0, v = 0; for (const char *q = dot + 1; isdigit((unsigned char)*q) && dgt < 3; q++, dgt++) v = v * 10 + (*q - '0'); while (dgt++ < 3) v *= 10; ms = v; }
    long long y = Y - (M <= 2), era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    long long doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe - 719468;
    return ((days * 24 + h) * 60 + m) * 60000 + sec * 1000LL + ms;
}

static bool ieq_n(const char *a, const char *b, size_t n) { for (size_t i = 0; i < n; i++) if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false; return true; }
bool rich_mentions_me(const char *text, const char *name, const char *nick) {
    if (!text) return false;
    for (const char *p = text; *p; p++) {
        if (*p != '@' || (p != text && !isspace((unsigned char)p[-1]))) continue;
        const char *w = p + 1, *e; size_t n;
        if (*w == '[') { w++; e = strchr(w, ']'); if (!e) continue; n = (size_t)(e - w); }
        else { n = 0; while (name_ch((unsigned char)w[n]) && n < 80) n++; }
        if (n < 2) continue;
        const char *c[2] = { nick, name };
        for (int k = 0; k < 2; k++) if (c[k] && c[k][0] && strlen(c[k]) == n && ieq_n(w, c[k], n)) return true;
    }
    return false;
}
