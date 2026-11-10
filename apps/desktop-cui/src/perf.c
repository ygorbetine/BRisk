#include "perf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <shlobj.h>

int g_perf = PERF_NORMAL;
bool g_perf_hint;
static bool g_explicit;      /* nível veio de BRIKS_PERF ou do settings.json: não sugerir nem sobrescrever */

static const char *KEY[] = { "normal", "simples", "minimo" };
const char *briks_perf_label(int l) { static const char *n[] = { "Normal", "Simplificado", "Mínimo" }; return n[l < 0 ? 0 : l > 2 ? 2 : l]; }
const char *briks_perf_help(int l) {
    static const char *h[] = {
        "Todas as animações, vidro com desfoque e sombras (como hoje).",
        "Sem mola nem animação longa; sombras de uma camada; ideal para PCs mais fracos.",
        "Sem animação, sem desfoque de vidro (fundo translúcido) e sem sombras; o mais leve.",
    };
    return h[l < 0 ? 0 : l > 2 ? 2 : l];
}

static bool settings_path(char *out, size_t cap) {
    const char *o = getenv("BRIKS_SETTINGS_FILE");
    if (o && o[0]) { snprintf(out, cap, "%s", o); return true; }
    char appdata[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) != S_OK) return false;
    char dir[MAX_PATH]; snprintf(dir, sizeof dir, "%s\\Briks-CUI", appdata);
    CreateDirectoryA(dir, NULL);
    snprintf(out, cap, "%s\\settings.json", dir);
    return true;
}

static int parse_level(const char *s) { for (int i = 0; i < 3; i++) if (!_stricmp(s, KEY[i])) return i; return -1; }

void briks_perf_init(void) {
    const char *e = getenv("BRIKS_PERF");
    int l = e ? parse_level(e) : -1;
    if (l >= 0) { g_perf = l; g_explicit = true; return; }
    char p[MAX_PATH];
    if (settings_path(p, sizeof p)) {
        FILE *f = fopen(p, "rb");
        if (f) {
            char js[512]; size_t n = fread(js, 1, sizeof js - 1, f); js[n] = 0; fclose(f);
            const char *k = strstr(js, "\"perf\"");
            if (k && (k = strchr(k + 6, '"'))) {
                char v[16] = { 0 }; k++; for (int i = 0; i < 15 && *k && *k != '"'; i++) v[i] = *k++;
                l = parse_level(v);
                if (l >= 0) { g_perf = l; g_explicit = true; return; }
            }
        }
    }
    /* padrão inicial: respeita "reduzir animações" do Windows */
    BOOL anim = TRUE;
    if (SystemParametersInfoA(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0) && !anim) g_perf = PERF_SIMPLE;
}

void briks_perf_set(cui_ctx *ui, int level, bool save) {
    if (level < 0) level = 0; if (level > 2) level = 2;
    g_perf = level; g_explicit = true; g_perf_hint = false;
    cui_set_reduced_motion(ui, level >= PERF_SIMPLE);
    if (!save) return;
    char p[MAX_PATH];
    if (settings_path(p, sizeof p)) { FILE *f = fopen(p, "wb"); if (f) { fprintf(f, "{ \"perf\": \"%s\" }\n", KEY[level]); fclose(f); } }
}

void briks_perf_note_renderer(const char *name) {
    if (!name || g_explicit || g_perf != PERF_NORMAL) return;
    if (strstr(name, "llvmpipe") || strstr(name, "Software") || strstr(name, "GDI Generic") || strstr(name, "Basic Render") || strstr(name, "SwiftShader"))
        g_perf_hint = true;
}

/* média dos 8 primeiros quadros desenhados: acima de 45 ms sugere o modo Simplificado */
void briks_perf_note_frame(double ms, int index) {
    static double sum; if (index == 1) sum = 0;
    if (index >= 2 && index <= 9) sum += ms;     /* o 1º quadro paga compilação de shaders */
    if (getenv("CUI_SCREENSHOT") || getenv("BRIKS_SCRIPT")) return;   /* captura/roteiro paga leitura de pixels: não é máquina lenta */
    if (index == 9 && !g_explicit && g_perf == PERF_NORMAL && sum / 8.0 > 45.0) g_perf_hint = true;
}
