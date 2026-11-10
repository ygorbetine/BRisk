#include "perf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <shlobj.h>

int g_perf = PERF_NORMAL;
bool g_perf_hint;
static bool g_explicit;      /* nível veio de BRIKS_PERF ou do settings.json: não sugerir nem sobrescrever */

static bool g_dismissed;     /* "Agora não": não reaparece nesta sessão */
static int g_hint_level = PERF_SIMPLE;
static char g_hint_text[200];
#define MAX_PHRASES 32
static const char *DEFAULT_PHRASES[] = {
    "Seu computador é à manivela?", "Seu computador parece lento.", "Sua carroça tem baixa performance",
    "Seu PC é uma batata?", "Seu PC da Xuxa está sofrendo",
};

static bool dir_file(const char *name, char *out, size_t cap) {
    char appdata[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) != S_OK) return false;
    char dir[MAX_PATH]; snprintf(dir, sizeof dir, "%s\\Briks-CUI", appdata);
    CreateDirectoryA(dir, NULL);
    snprintf(out, cap, "%s\\%s", dir, name);
    return true;
}

/* sorteia uma frase por exibição, sem repetir a anterior em sequência (a última fica gravada em perf-frase.last). A lista é um arquivo de texto
 * editável, uma frase por linha (BRIKS_PERF_PHRASES_FILE sobrepõe o caminho); se não existir é criado com as frases padrão. */
static void pick_phrase(void) {
    static char lines[MAX_PHRASES][200]; int n = 0;
    char path[MAX_PATH]; const char *o = getenv("BRIKS_PERF_PHRASES_FILE");
    bool have = o && o[0] ? (snprintf(path, sizeof path, "%s", o), true) : dir_file("perf-frases.txt", path, sizeof path);
    if (have) {
        FILE *f = fopen(path, "rb");
        if (!f) { f = fopen(path, "wb"); if (f) { for (size_t i = 0; i < sizeof DEFAULT_PHRASES / sizeof *DEFAULT_PHRASES; i++) fprintf(f, "%s\r\n", DEFAULT_PHRASES[i]); fclose(f); } f = fopen(path, "rb"); }
        if (f) {
            char buf[256];
            while (n < MAX_PHRASES && fgets(buf, sizeof buf, f)) {
                size_t l = strlen(buf); while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r' || buf[l - 1] == ' ')) buf[--l] = 0;
                if (l && buf[0] != '#') snprintf(lines[n++], sizeof lines[0], "%s", buf);
            }
            fclose(f);
        }
    }
    if (!n) for (size_t i = 0; i < sizeof DEFAULT_PHRASES / sizeof *DEFAULT_PHRASES && n < MAX_PHRASES; i++) snprintf(lines[n++], sizeof lines[0], "%s", DEFAULT_PHRASES[i]);
    static int last = -1; static bool seeded;
    if (!seeded) {
        seeded = true; srand((unsigned)(GetTickCount64() ^ GetCurrentProcessId()));
        char lp[MAX_PATH]; if (dir_file("perf-frase.last", lp, sizeof lp)) { FILE *f = fopen(lp, "rb"); if (f) { if (fscanf(f, "%d", &last) != 1) last = -1; fclose(f); } }
    }
    int i = rand() % n;
    if (n > 1 && i == last) i = (i + 1 + rand() % (n - 1)) % n;   /* nunca a mesma da vez anterior */
    last = i;
    { char lp[MAX_PATH]; if (dir_file("perf-frase.last", lp, sizeof lp)) { FILE *f = fopen(lp, "wb"); if (f) { fprintf(f, "%d", last); fclose(f); } } }
    snprintf(g_hint_text, sizeof g_hint_text, "%s", lines[i]);
}

static void hint_raise(int level) {
    if (g_dismissed || g_perf_hint) return;
    g_hint_level = level; pick_phrase(); g_perf_hint = true;
}
int briks_perf_hint_level(void) { return g_hint_level; }
const char *briks_perf_hint_text(void) { return g_hint_text; }
void briks_perf_hint_dismiss(void) { g_perf_hint = false; g_dismissed = true; }

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
    { const char *pp = getenv("BRIKS_PERF_PROMPT"); if (pp && pp[0] && pp[0] != '0') hint_raise(pp[0] == '2' ? PERF_MIN : PERF_SIMPLE); }   /* teste: força o aviso (1 = Simplificado, 2 = Mínimo) */
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
        hint_raise(PERF_MIN);   /* renderizador por software: o Mínimo é o que ajuda */
}

/* média dos 8 primeiros quadros desenhados: acima de 45 ms sugere o modo Simplificado */
void briks_perf_note_frame(double ms, int index) {
    static double sum; if (index == 1) sum = 0;
    if (index >= 2 && index <= 9) sum += ms;     /* o 1º quadro paga compilação de shaders */
    if (getenv("CUI_SCREENSHOT") || getenv("BRIKS_SCRIPT")) return;   /* captura/roteiro paga leitura de pixels: não é máquina lenta */
    if (index == 9 && !g_explicit && g_perf == PERF_NORMAL && sum / 8.0 > 45.0) hint_raise(sum / 8.0 > 120.0 ? PERF_MIN : PERF_SIMPLE);   /* muito lento: sugere o Mínimo */
}
