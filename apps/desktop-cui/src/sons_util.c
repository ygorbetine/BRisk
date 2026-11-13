/* Painel de sons: funções puras (ver sons_util.h). Nada aqui toca janela, arquivo ou rede. */
#include "sons_util.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* tamanho em bytes do caractere UTF-8 que começa em p (1 se a sequência for inválida) */
static int u8len(const unsigned char *p) {
    if (p[0] < 0x80) return 1;
    int n = (p[0] & 0xE0) == 0xC0 ? 2 : (p[0] & 0xF0) == 0xE0 ? 3 : (p[0] & 0xF8) == 0xF0 ? 4 : 1;
    for (int i = 1; i < n; i++) if ((p[i] & 0xC0) != 0x80) return 1;
    return n;
}

bool sons_clean_name(const char *in, char *out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = 0;
    if (!in) return false;
    char tmp[256]; size_t n = 0, chars = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && chars < 32 && n + 5 < sizeof tmp;) {
        int l = u8len(p);
        if (l == 1 && (*p < 0x20 || *p == 0x7f)) { p++; continue; }   /* controles somem (quebra de linha, tab) */
        if (l == 2 && p[0] == 0xC2 && p[1] >= 0x80 && p[1] < 0xA0) { p += 2; continue; }   /* controles C1 */
        memcpy(tmp + n, p, (size_t)l); n += (size_t)l; p += l; chars++;
    }
    tmp[n] = 0;
    size_t a = 0, b = n;
    while (a < b && tmp[a] == ' ') a++;
    while (b > a && tmp[b - 1] == ' ') b--;
    if (b == a || b - a + 1 > cap) { if (b == a) return false; b = a + cap - 1; while (b > a && ((unsigned char)tmp[b] & 0xC0) == 0x80) b--; }   /* destino pequeno: corta sem partir um caractere */
    memcpy(out, tmp + a, b - a); out[b - a] = 0;
    return out[0] != 0;
}

void sons_label(const char *emoji, const char *name, int max_chars, char *out, size_t cap) {
    if (!out || cap == 0) return;
    char full[320]; snprintf(full, sizeof full, "%s%s%s", emoji && emoji[0] ? emoji : "", emoji && emoji[0] ? " " : "", name ? name : "");
    size_t n = 0; int chars = 0;
    const unsigned char *p = (const unsigned char *)full;
    bool cut = false;
    while (*p) {
        int l = u8len(p);
        if (chars >= max_chars) { cut = true; break; }
        if (n + (size_t)l + 4 >= cap) { cut = true; break; }
        memcpy(out + n, p, (size_t)l); n += (size_t)l; p += l; chars++;
    }
    if (cut) { while (n > 0 && out[n - 1] == ' ') n--; if (n + 4 < cap) { memcpy(out + n, "\xE2\x80\xA6", 3); n += 3; } }
    out[n] = 0;
}

float sons_peak_gain(const short *pcm, size_t nsamples, float volume01) {
    if (volume01 < 0) volume01 = 0;
    if (volume01 > 1) volume01 = 1;
    int peak = 0;
    for (size_t i = 0; i < nsamples; i++) { int v = pcm[i] < 0 ? -(int)pcm[i] : (int)pcm[i]; if (v > peak) peak = v; }
    float g = 1.0f;
    const float ceil_ = 0.708f * 32768.0f;   /* -3 dBFS */
    if (peak > 0 && (float)peak > ceil_) g = ceil_ / (float)peak;
    return g * volume01;
}

/* minúscula sem acento (só o que aparece em nomes em português); devolve quantos bytes consumiu de p */
static int fold(const unsigned char *p, char *c) {
    if (p[0] < 0x80) { *c = (char)((p[0] >= 'A' && p[0] <= 'Z') ? p[0] + 32 : p[0]); return 1; }
    if (p[0] == 0xC3 && p[1]) {
        const unsigned char x = p[1];
        const char *map = NULL;
        if (x >= 0x80 && x <= 0x85) map = "a"; else if (x == 0x87) map = "c"; else if (x >= 0x88 && x <= 0x8B) map = "e"; else if (x >= 0x8C && x <= 0x8F) map = "i";
        else if (x >= 0x92 && x <= 0x96) map = "o"; else if (x >= 0x99 && x <= 0x9C) map = "u";
        else if (x >= 0xA0 && x <= 0xA5) map = "a"; else if (x == 0xA7) map = "c"; else if (x >= 0xA8 && x <= 0xAB) map = "e"; else if (x >= 0xAC && x <= 0xAF) map = "i";
        else if (x >= 0xB2 && x <= 0xB6) map = "o"; else if (x >= 0xB9 && x <= 0xBC) map = "u";
        if (map) { *c = map[0]; return 2; }
    }
    *c = 0; return u8len(p);   /* outros caracteres (emoji etc.) não participam da busca */
}

static size_t fold_str(const char *s, char *out, size_t cap) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && n + 1 < cap;) { char c; p += fold(p, &c); if (c) out[n++] = c; }
    out[n] = 0;
    return n;
}

bool sons_match(const char *name, const char *query) {
    if (!query || !query[0]) return true;
    char q[160], h[320];
    if (!fold_str(query, q, sizeof q)) return true;   /* a busca só tinha espaços/símbolos: não filtra */
    fold_str(name ? name : "", h, sizeof h);
    return strstr(h, q) != NULL;
}

const char *sons_fmt_dur(unsigned ms, char *out, size_t cap) {
    if (!out || cap == 0) return "";
    unsigned t = (ms + 50) / 100;   /* décimos, arredondado */
    if (t % 10) snprintf(out, cap, "%u,%u s", t / 10, t % 10); else snprintf(out, cap, "%u s", t / 10);
    return out;
}
