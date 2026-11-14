/* Sobreposição em jogos (lado do app): abre o briks_overlay.exe numa chamada, manda por stdin quem está na sala e o que mostrar,
 * e o fecha ao sair da chamada ou desligar a opção. Protocolo em src/overlay.c. Testes ocultos nunca abrem o processo (só montam o quadro). */
#include "briks_ui.h"
#include "sysint.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void settings_overlay(bool *on, float *opacity, int *flags, int *corner);
bool settings_ovl_place(char *dev, size_t cap, int *x, int *y, int *w);        /* settings.c: posição do monitor ativo */
void settings_ovl_set(const char *dev, int x, int y, int w);
bool settings_ovl_selfshare(void); bool settings_ovl_through(void); void settings_ovl_set_through(bool on);
bool settings_ovl_widget(const char *id, bool on); void settings_ovl_opacity(int pct);   /* settings.c: botões da barra de edição do overlay */
bool settings_tile_place(const char *key, char *dev, size_t cap, int *x, int *y, int *pct); void settings_tile_set(const char *key, const char *dev, int x, int y, int pct);
bool settings_tile_enabled(const char *key); void settings_tile_enable(const char *key, bool on);   /* settings.c: miniaturas de transmissão */
void ovlkey_set(bool on); int ovlkey_take(void); int ovlkey_status(void);       /* hotkeys_ovl.c */

#define NT 4              /* miniaturas de transmissão ao mesmo tempo */
#define MAXLIVE 16        /* transmissões consideradas (mais de NT: "Trocar transmissão" gira a janela de NT) */
#define LV_W 640
#define LV_HMAX 360
#define LV_SLOT (64 + LV_W * LV_HMAX * 4)   /* um quadro por miniatura na memória compartilhada: cabeçalho 'BOVL', seq, w, h, n (64 bytes) + BGRA */
#define LV_PERIOD 0.030                     /* ~30 quadros/s por miniatura (1.5.20; antes 0,25 s = 4/s) */

static HANDLE g_proc, g_pipe, g_rd;   /* g_rd: saída do overlay (layout/edit 0) */
static bool g_edit; static char g_rbuf[512]; static size_t g_rn; static int g_layouts, g_hk_state;

/* ---- miniaturas ao vivo: lê os MESMOS arquivos de quadros que a tela de transmissão usa (sh.remote[slot].path / remote2), reduz para ~320 px e
 * escreve numa memória compartilhada nomeada que o briks_overlay.exe lê (sem pipe: nada bloqueia o app nem o jogo). Só as miniaturas LIGADAS
 * gastam algo: desligada (ou sem a opção Transmissão, ou sem overlay) não é lida, não é decodificada e a memória dela é zerada. ---- */
typedef struct { char path[300]; HANDLE file, map; const unsigned char *p; } lsrc_t;
typedef struct { char key[12], name[64], path[300]; int slot, second; } live_t;   /* uma transmissão ao vivo (de outra pessoa) */
static live_t g_lives[MAXLIVE]; static int g_nlives;          /* todas as transmissões */
static int g_shown[NT], g_nshown; static bool g_tile_on[NT];   /* as exibidas (janela de NT a partir de g_live_sel) e se cada uma está ligada */
static bool g_want_r[BRIKS_MAX_PEERS], g_want_r2[BRIKS_MAX_PEERS];   /* slots que o receptor deve decodificar para o overlay */
static lsrc_t g_ls[NT]; static unsigned long long g_img_n[NT]; static int g_img_w[NT], g_img_h[NT], g_img_sent; static double g_live_at;
static HANDLE g_shm_h; static unsigned char *g_shm; static char g_shm_name[48];

static void ls_close(int j) { lsrc_t *l = &g_ls[j]; if (l->p) UnmapViewOfFile(l->p); if (l->map) CloseHandle(l->map); if (l->file && l->file != INVALID_HANDLE_VALUE) CloseHandle(l->file); memset(l, 0, sizeof *l); }
static void shm_ensure(void) {
    if (g_shm) return;
    snprintf(g_shm_name, sizeof g_shm_name, "Local\\BriksOvlLive-%lu", (unsigned long)GetCurrentProcessId());
    g_shm_h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, NT * LV_SLOT, g_shm_name);
    if (g_shm_h) g_shm = (unsigned char *)MapViewOfFile(g_shm_h, FILE_MAP_WRITE, 0, 0, 0);
    if (!g_shm) { if (g_shm_h) CloseHandle(g_shm_h); g_shm_h = NULL; g_shm_name[0] = 0; return; }
    memset(g_shm, 0, NT * LV_SLOT > 4096 ? 4096 : NT * LV_SLOT);
    for (int j = 0; j < NT; j++) memset(g_shm + (size_t)j * LV_SLOT, 0, 64);
}
static void shm_free(void) { if (g_shm) UnmapViewOfFile(g_shm); if (g_shm_h) CloseHandle(g_shm_h); g_shm = NULL; g_shm_h = NULL; g_shm_name[0] = 0; }
static void slot_hdr(int j, unsigned w, unsigned h, unsigned long long n) {   /* seqlock: seq ímpar enquanto escreve */
    unsigned char *m = g_shm + (size_t)j * LV_SLOT; volatile unsigned *seq = (volatile unsigned *)(m + 4); *seq += 1; MemoryBarrier();
    memcpy(m, "BOVL", 4); memcpy(m + 8, &w, 4); memcpy(m + 12, &h, 4); memcpy(m + 16, &n, 8);
    MemoryBarrier(); *seq += 1;
}
static void tile_idle(int j) {   /* miniatura sem nada para mostrar: solta o arquivo e zera a memória dela */
    ls_close(j);
    if (g_img_n[j] || g_img_w[j]) { if (g_shm) slot_hdr(j, 0, 0, 0); g_img_n[j] = 0; g_img_w[j] = g_img_h[j] = 0; }
}
static void tiles_idle(void) { for (int j = 0; j < NT; j++) tile_idle(j); memset(g_want_r, 0, sizeof g_want_r); memset(g_want_r2, 0, sizeof g_want_r2); }
/* Um quadro por miniatura, no máximo a cada LV_PERIOD: RGBA do core -> BGRA reduzido (média de 2x2 em torno do ponto amostrado). */
static void tile_pump(int j, const char *path) {
    lsrc_t *l = &g_ls[j];
    if (strcmp(l->path, path)) { ls_close(j); snprintf(l->path, sizeof l->path, "%s", path); g_img_n[j] = 0; }
    if (!l->p) {
        l->file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        if (l->file == INVALID_HANDLE_VALUE) { l->file = NULL; return; }
        l->map = CreateFileMappingA(l->file, NULL, PAGE_READONLY, 0, 0, NULL);
        if (l->map) l->p = (const unsigned char *)MapViewOfFile(l->map, FILE_MAP_READ, 0, 0, 0);
        if (!l->p) { if (l->map) CloseHandle(l->map); CloseHandle(l->file); l->map = l->file = NULL; return; }
    }
    const volatile unsigned *seq = (const volatile unsigned *)(l->p + 4); unsigned a = *seq;
    if (memcmp(l->p, "BKFR", 4) || (a & 1)) return;
    int w = *(const int *)(l->p + 8), h = *(const int *)(l->p + 12); unsigned long long n = *(const unsigned long long *)(l->p + 16);
    if (w < 2 || h < 2 || w > 3840 || h > 2160 || n == 0 || n == g_img_n[j]) return;
    int dw = LV_W, dh = h * dw / w; if (dh > LV_HMAX) { dh = LV_HMAX; dw = w * dh / h; } if (dw < 2) dw = 2; if (dh < 2) dh = 2;
    unsigned char *m = g_shm + (size_t)j * LV_SLOT, *dst = m + 64; const unsigned char *src = l->p + 64;
    volatile unsigned *ws = (volatile unsigned *)(m + 4); *ws += 1; MemoryBarrier();   /* seq ímpar: o overlay não lê no meio */
    for (int y = 0; y < dh; y++) {
        const int ya = (2 * y + 1) * h / (2 * dh), sy = (4 * y + 1) * h / (4 * dh), sy2 = (4 * y + 3) * h / (4 * dh); (void)ya;
        for (int x = 0; x < dw; x++) {
            const int sx = (4 * x + 1) * w / (4 * dw), sx2 = (4 * x + 3) * w / (4 * dw);   /* 1.5.20: 4 amostras nos 1/4 e 3/4 da caixa de origem (antes: 2x2 colado no ponto) */
            const unsigned char *p00 = src + ((size_t)sy * w + sx) * 4, *p01 = src + ((size_t)sy * w + sx2) * 4, *p10 = src + ((size_t)sy2 * w + sx) * 4, *p11 = src + ((size_t)sy2 * w + sx2) * 4;
            unsigned char *o = dst + ((size_t)y * dw + x) * 4;
            o[0] = (unsigned char)((p00[2] + p01[2] + p10[2] + p11[2]) >> 2); o[1] = (unsigned char)((p00[1] + p01[1] + p10[1] + p11[1]) >> 2); o[2] = (unsigned char)((p00[0] + p01[0] + p10[0] + p11[0]) >> 2); o[3] = 255;
        }
    }
    unsigned long long nn = n; unsigned uw = (unsigned)dw, uh = (unsigned)dh;
    memcpy(m, "BOVL", 4); memcpy(m + 8, &uw, 4); memcpy(m + 12, &uh, 4); memcpy(m + 16, &nn, 8);
    MemoryBarrier(); *ws += 1;
    if (*seq != a) return;   /* o core escreveu no meio da leitura: refaz no próximo ciclo (n não avança) */
    g_img_n[j] = n; g_img_w[j] = dw; g_img_h[j] = dh; g_img_sent++;
}
/* ui_share.c pergunta: este slot precisa ser decodificado para uma miniatura do overlay, mesmo fora da aba Transmissão? (second = 2ª tela) */
bool overlay_live_wanted(int slot, bool second) { return slot >= 0 && slot < BRIKS_MAX_PEERS && (second ? g_want_r2[slot] : g_want_r[slot]); }

static int g_fake_lives = -1;   /* roteiro de teste: finge N transmissões ao vivo (-1 = as reais) */
static int g_live_sel, g_widget_evs, g_opacity_evs, g_select_evs, g_tile_evs, g_tileon_evs;   /* barra de edição: início da janela de NT lives, eventos recebidos */
static int g_state;            /* 0 desligada, 1 ativa, 2 executável não encontrado, 3 fechou sozinha */
static char g_last[4096];      /* último quadro enviado */
static int g_last_lines; static double g_last_send, g_fail_at;

int overlay_state(void) { return g_state; }
/* Configurações > Sobreposição lista as miniaturas atuais (uma chave por transmissão) */
int overlay_tile_count(void) { return g_nshown; }
const char *overlay_tile_name(int j) { return j >= 0 && j < g_nshown ? g_lives[g_shown[j]].name : ""; }
bool overlay_tile_enabled(int j) { return j >= 0 && j < g_nshown && settings_tile_enabled(g_lives[g_shown[j]].key); }
void overlay_tile_enable(int j, bool on) { if (j >= 0 && j < g_nshown) settings_tile_enable(g_lives[g_shown[j]].key, on); }

static bool g_suppressed;   /* a sobreposição está escondida porque o Briks está em primeiro plano */
int overlay_probe(const char *n) {
    if (!strcmp(n, "overlay_suppressed")) return g_suppressed;
    if (!strcmp(n, "overlay_state")) return g_state;
    if (!strcmp(n, "overlay_lines")) return g_last_lines;
    if (!strcmp(n, "overlay_has_mute")) return strstr(g_last, "\nw Microfone mudo\n") != NULL;
    if (!strcmp(n, "overlay_has_live")) return strstr(g_last, "\ni Você está transmitindo") != NULL;
    if (!strcmp(n, "overlay_has_room")) return strstr(g_last, "\ni Sala conectada") != NULL;
    if (!strcmp(n, "overlay_people")) { int c = 0; for (const char *p = g_last; (p = strstr(p, "\np ")) != NULL; p += 3) c++; return c; }
    if (!strcmp(n, "overlay_speaking")) { int c = 0; for (const char *p = g_last; (p = strstr(p, "\np 1 ")) != NULL; p += 3) c++; return c; }
    if (!strcmp(n, "overlay_edit")) return g_edit;
    if (!strcmp(n, "overlay_layouts")) return g_layouts;                 /* quantas posições o overlay mandou */
    if (!strcmp(n, "overlay_hotkey")) return g_hk_state;                 /* 0 sem atalho, 1 Ctrl+Shift+O, 2 Ctrl+Alt+O, 3 ocupado */
    if (!strcmp(n, "overlay_flags")) { const char *c = strstr(g_last, "\nflags "); return c ? atoi(c + 7) : -1; }
    if (!strcmp(n, "overlay_lives")) { const char *c = strstr(g_last, "\nlives "); return c ? atoi(c + 7) : -1; }
    if (!strcmp(n, "overlay_live_sel")) return g_live_sel;
    if (!strcmp(n, "overlay_tiles")) { int c = 0; for (const char *p = g_last; (p = strstr(p, "\ntile ")) != NULL; p += 6) c++; return c; }   /* miniaturas no quadro */
    if (!strcmp(n, "overlay_tiles_on")) { int c = 0; for (int j = 0; j < g_nshown; j++) if (g_tile_on[j]) c++; return c; }
    if (!strcmp(n, "overlay_tplaces")) { int c = 0; for (const char *p = g_last; (p = strstr(p, "\ntplace ")) != NULL; p += 8) c++; return c; }
    if (!strcmp(n, "overlay_live_img")) { int c = 0; for (int j = 0; j < NT; j++) if (g_img_n[j]) c++; return c; }   /* miniaturas com imagem na memória compartilhada */
    if (!strcmp(n, "overlay_live_w")) return g_img_w[0];
    if (!strcmp(n, "overlay_live_h")) return g_img_h[0];
    if (!strcmp(n, "overlay_img_sent")) return g_img_sent;
    if (!strcmp(n, "overlay_want_n")) { int c = 0; for (int i = 0; i < BRIKS_MAX_PEERS; i++) c += g_want_r[i] + g_want_r2[i]; return c; }
    if (!strcmp(n, "overlay_has_map")) return strstr(g_last, "\nlivemap ") != NULL;
    if (!strcmp(n, "overlay_has_oplive")) return strstr(g_last, "\ntile 0 ") != NULL;
    if (!strcmp(n, "overlay_widget_evs")) return g_widget_evs;
    if (!strcmp(n, "overlay_opacity_evs")) return g_opacity_evs;
    if (!strcmp(n, "overlay_select_evs")) return g_select_evs;
    if (!strcmp(n, "overlay_tile_evs")) return g_tile_evs;
    if (!strcmp(n, "overlay_tileon_evs")) return g_tileon_evs;
    if (!strcmp(n, "overlay_has_place")) return strstr(g_last, "\nplace \\\\") != NULL;
    if (!strcmp(n, "overlay_has_edit")) return strstr(g_last, "\nedit 1\n") != NULL;
    if (!strcmp(n, "overlay_place_w") || !strcmp(n, "overlay_place_x")) { const char *p = strstr(g_last, "\nplace "); if (!p) return -1; int x, y, w; char d[48]; return sscanf(p + 7, "%47s %d %d %d", d, &x, &y, &w) == 4 ? (n[14] == 'w' ? w : x) : -1; }
    if (!strcmp(n, "overlay_opacity")) { const char *c = strstr(g_last, "cfg "); return c ? atoi(c + 4) : -1; }
    if (!strcmp(n, "overlay_corner")) { const char *c = strstr(g_last, "cfg "); if (!c) return -1; c += 4; while (*c && *c != ' ') c++; return atoi(c); }
    return -9999;
}

static bool dry_run(void) { return getenv("BRIKS_HIDDEN") || getenv("BRIKS_SCRIPT") || getenv("BRIKS_DEMO"); }

static void stop(void) {
    if (g_pipe) { DWORD w; WriteFile(g_pipe, "quit\n", 5, &w, NULL); CloseHandle(g_pipe); g_pipe = NULL; }
    if (g_rd) { CloseHandle(g_rd); g_rd = NULL; } g_rn = 0;
    if (g_proc) { if (WaitForSingleObject(g_proc, 600) != WAIT_OBJECT_0) TerminateProcess(g_proc, 0); CloseHandle(g_proc); g_proc = NULL; }
    g_last[0] = 0; g_last_lines = 0;
    tiles_idle(); shm_free(); g_nshown = 0;
}

static bool spawn(void) {
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, sizeof exe);
    char *sl = strrchr(exe, '\\'); if (sl) *sl = 0;
    char cmd[MAX_PATH + 40]; snprintf(cmd, sizeof cmd, "\"%s\\briks_overlay.exe\" --stdin", exe);
    char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\briks_overlay.exe", exe);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) { g_state = 2; return false; }
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL, orf = NULL, owf = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { g_state = 3; return false; }
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&orf, &owf, &sa, 0)) { CloseHandle(rd); CloseHandle(wr); g_state = 3; return false; }
    SetHandleInformation(orf, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = rd; si.hStdOutput = owf; si.hStdError = owf;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(rd); CloseHandle(owf);
    if (!ok) { CloseHandle(wr); CloseHandle(orf); g_state = 3; return false; }
    CloseHandle(pi.hThread); g_proc = pi.hProcess; g_pipe = wr; g_rd = orf; g_state = 1;
    return true;
}

static void clean_name(char *nm) { for (char *c = nm; *c; c++) if (*c == '\n' || *c == '\r' || *c == '|') *c = ' '; }
static void live_key(char *out, size_t cap, const char *id, int second) {   /* chave estável (hash da pessoa + qual das duas telas): posição/escala/liga-desliga ficam salvas nela */
    unsigned h = 2166136261u; for (const char *p = id; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    snprintf(out, cap, "%08x%d", h, second ? 2 : 1);
}
static void live_add(const char *id, const char *name, const char *path, int slot, int second) {
    if (g_nlives >= MAXLIVE) return; live_t *l = &g_lives[g_nlives++]; memset(l, 0, sizeof *l);
    live_key(l->key, sizeof l->key, id, second);
    snprintf(l->name, sizeof l->name, "%s%s", name[0] ? name : "Participante", second ? " (tela 2)" : ""); clean_name(l->name);
    snprintf(l->path, sizeof l->path, "%s", path); l->slot = slot; l->second = second;
}
/* Monta a lista de transmissões (de outras pessoas, 1ª e 2ª tela) e a janela de NT exibidas. Em edição fora de chamada: 2 exemplos; no teste: N fingidas. */
static void collect_lives(const app_state_t *s) {
    g_nlives = 0;
    if (g_fake_lives >= 0) { for (int k = 0; k < g_fake_lives && k < MAXLIVE; k++) { char id[24], nm[24]; snprintf(id, sizeof id, "fake-%d", k); snprintf(nm, sizeof nm, "Participante %d", k + 1); live_add(id, nm, "", -1, 0); } }
    else if (g_edit && !s->in_room) { live_add("demo-a", "Amigo 1", "", -1, 0); live_add("demo-b", "Amigo 2", "", -1, 0); }
    else for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (s->rtc_peers[i].id[0]) {
        if (s->sh.remote[i].on) live_add(s->rtc_peers[i].id, s->rtc_peers[i].name, s->sh.remote[i].path, i, 0);
        if (s->sh.remote2[i].on) live_add(s->rtc_peers[i].id, s->rtc_peers[i].name, s->sh.remote2[i].path, i, 1);
    }
    if (g_live_sel >= g_nlives) g_live_sel = 0;
    g_nshown = g_nlives < NT ? g_nlives : NT;
    for (int j = 0; j < g_nshown; j++) { g_shown[j] = (g_live_sel + j) % g_nlives; g_tile_on[j] = settings_tile_enabled(g_lives[g_shown[j]].key); }
}

/* Monta o quadro do que aparece por cima do jogo. Visível para teste (sonda). Em edição fora de chamada mostra exemplos que respeitam os itens ligados. */
static void compose(const app_state_t *s, float opacity, int flags, int corner, char *out, size_t cap) {
    size_t n = 0;
    n += (size_t)snprintf(out + n, cap - n, "cfg %d %d\nedit %d\nflags %d\nlives %d\n", (int)(opacity + 0.5f), corner, g_edit ? 1 : 0, flags & 15, g_nlives);
    if (g_shm_name[0]) n += (size_t)snprintf(out + n, cap - n, "livemap %s\n", g_shm_name);
    { char dev[48]; int x, y, w; if (settings_ovl_place(dev, sizeof dev, &x, &y, &w)) n += (size_t)snprintf(out + n, cap - n, "place %s %d %d %d\n", dev, x, y, w); else n += (size_t)snprintf(out + n, cap - n, "place -\n"); }
    n += (size_t)snprintf(out + n, cap - n, "begin\n");
    int lines = 0;
    if ((flags & 1) && (s->in_room || g_edit)) {   /* uma miniatura por transmissão: posição/escala/ligada vêm das configurações (por chave) */
        for (int j = 0; j < g_nshown && n + 260 < cap; j++) {
            const live_t *l = &g_lives[g_shown[j]]; char dev[48]; int x, y, pct;
            n += (size_t)snprintf(out + n, cap - n, "tile %d %s %d %s\n", j, l->key, g_tile_on[j] ? 1 : 0, l->name);
            if (settings_tile_place(l->key, dev, sizeof dev, &x, &y, &pct)) n += (size_t)snprintf(out + n, cap - n, "tplace %d %s %d %d %d\n", j, dev, x, y, pct);
        }
    }
    if (g_edit && !s->in_room) {   /* editando fora de uma chamada: exemplos para ver o tamanho real; cada item aparece só se estiver ligado */
        const char *me = s->user_name[0] ? s->user_name : "Você";
        if (flags & 4) { n += (size_t)snprintf(out + n, cap - n, "i Sala conectada\n"); lines++; }
        if (flags & 8) { n += (size_t)snprintf(out + n, cap - n, "w Microfone mudo\n"); lines++; }
        if (flags & 2) { n += (size_t)snprintf(out + n, cap - n, "p 1 %s\np 0 Amigo\n", me); lines += 2; }
        snprintf(out + n, cap - n, "end\n"); g_last_lines = lines; return;
    }
    if ((flags & 4) && s->in_room) { n += (size_t)snprintf(out + n, cap - n, "i Sala conectada\n"); lines++; }
    if ((flags & 1) && s->sh.active && settings_ovl_selfshare()) { n += (size_t)snprintf(out + n, cap - n, "i Você está transmitindo a tela\n"); lines++; }
    if ((flags & 8) && s->deaf) { n += (size_t)snprintf(out + n, cap - n, "w Ensurdecido\n"); lines++; }   /* independentes: com os dois, aparecem os dois avisos */
    if ((flags & 8) && !s->mic_on) { n += (size_t)snprintf(out + n, cap - n, "w Microfone mudo\n"); lines++; }
    if (flags & 2) {
        n += (size_t)snprintf(out + n, cap - n, "p %d %s\n", (s->mic_on && s->me_level > 0.05f) ? 1 : 0, s->user_name[0] ? s->user_name : "Você"); lines++;
        for (int i = 0; i < BRIKS_MAX_PEERS && n + 100 < cap; i++) if (s->rtc_peers[i].id[0]) {
            char nm[64]; snprintf(nm, sizeof nm, "%s", s->rtc_peers[i].name[0] ? s->rtc_peers[i].name : "Participante"); clean_name(nm);
            n += (size_t)snprintf(out + n, cap - n, "p %d %s\n", s->rtc_peers[i].level > 0.05f ? 1 : 0, nm); lines++;
        }
    }
    snprintf(out + n, cap - n, "end\n");
    g_last_lines = lines;
}

/* chave vinda do overlay; "#<j>" (roteiros de teste) = a miniatura j exibida */
static const char *tile_key(const char *k, char *buf, size_t cap) {
    if (k[0] == '#') { int j = atoi(k + 1); if (j >= 0 && j < g_nshown) { snprintf(buf, cap, "%s", g_lives[g_shown[j]].key); return buf; } return NULL; }
    return k;
}
/* Linha vinda do overlay: "layout <monitor> x y largura" (soltou), "edit 0" (Concluir/Esc), "widget <id> <0|1>", "opacity <n>", "selectlive",
 * "tile <chave> <monitor> x y %" (miniatura movida/redimensionada), "tileon <chave> <0|1>" (liga/desliga a miniatura). Também usada pelo roteiro de teste. */
static void handle_line(const char *ln) {
    char dev[48]; int x, y, w;
    if (!strncmp(ln, "layout ", 7) && sscanf(ln + 7, "%47s %d %d %d", dev, &x, &y, &w) == 4) { settings_ovl_set(dev, x, y, w); g_layouts++; }
    else if (!strcmp(ln, "edit 0")) g_edit = false;
    else if (!strncmp(ln, "widget ", 7)) {   /* botão da barra de edição: "widget <live|voice|status|mute|all> <0|1>" */
        char id[16]; int v;
        if (sscanf(ln + 7, "%15s %d", id, &v) == 2 && settings_ovl_widget(id, v != 0)) g_widget_evs++;
    }
    else if (!strncmp(ln, "through ", 8)) { settings_ovl_set_through(atoi(ln + 8) != 0); g_widget_evs++; }
    else if (!strncmp(ln, "opacity ", 8)) { settings_ovl_opacity(atoi(ln + 8)); g_opacity_evs++; }
    else if (!strcmp(ln, "selectlive")) { g_live_sel = g_nlives > NT ? (g_live_sel + 1) % g_nlives : 0; g_select_evs++; }
    else if (!strncmp(ln, "tileon ", 7)) { char k[16], kb[16]; int v; const char *key; if (sscanf(ln + 7, "%15s %d", k, &v) == 2 && (key = tile_key(k, kb, sizeof kb)) != NULL) { settings_tile_enable(key, v != 0); g_tileon_evs++; } }
    else if (!strncmp(ln, "tile ", 5)) { char k[16], kb[16]; int pct; const char *key; if (sscanf(ln + 5, "%15s %47s %d %d %d", k, dev, &x, &y, &pct) == 5 && (key = tile_key(k, kb, sizeof kb)) != NULL) { settings_tile_set(key, dev, x, y, pct); g_tile_evs++; } }
}
static void pump_out(void) {
    DWORD avail = 0, got = 0; char b[256];
    while (g_rd && PeekNamedPipe(g_rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
        if (!ReadFile(g_rd, b, (DWORD)(avail < sizeof b ? avail : sizeof b), &got, NULL) || !got) break;
        for (DWORD i = 0; i < got; i++) {
            if (b[i] == '\n') { g_rbuf[g_rn] = 0; if (g_rn && g_rbuf[g_rn - 1] == '\r') g_rbuf[g_rn - 1] = 0; handle_line(g_rbuf); g_rn = 0; }
            else if (g_rn < sizeof g_rbuf - 1) g_rbuf[g_rn++] = b[i];
        }
    }
}
bool overlay_edit_active(void) { return g_edit; }
void overlay_edit_toggle(void) { g_edit = !g_edit; }
void overlay_edit_set(bool on) { g_edit = on; }
/* roteiro: "sys ovl key" finge o atalho; "sys ovl line <texto>" finge uma linha vinda do overlay; "sys ovl lives <n>" finge n transmissões ao vivo */
bool overlay_script(const char *args) {
    if (!strcmp(args, "key")) { overlay_edit_toggle(); return true; }
    if (!strncmp(args, "line ", 5)) { handle_line(args + 5); return true; }
    if (!strncmp(args, "lives ", 6)) { g_fake_lives = atoi(args + 6); return true; }
    return false;
}

void overlay_tick(cui_ctx *ui, app_state_t *s) {
    bool on; float op; int flags, corner; settings_overlay(&on, &op, &flags, &corner);
    if (getenv("BRIKS_OVERLAY_FORCE")) on = true;
    if (!on) { g_edit = false; tiles_idle(); }
    {   /* Ctrl+Shift+O: ligado enquanto a sobreposição está ligada, em qualquer tela */
        static bool hk_on;
        if (on != hk_on) { hk_on = on; if (!dry_run()) ovlkey_set(on); }
        g_hk_state = on ? (dry_run() ? 1 : ovlkey_status()) : 0;
        if (!dry_run() && ovlkey_take() > 0 && on) overlay_edit_toggle();
    }
    /* Briks em primeiro plano (maximizado ou não, visível, não minimizado): a sobreposição só faz sentido por cima de OUTROS apps/jogos, então
       manda um quadro vazio (a janela do overlay se esconde e nenhuma miniatura decodifica). Minimizar, restaurar para trás de outro app ou
       trocar de foco traz de volta; a edição (g_edit) continua visível. Roteiros: "sys focus 1|0|auto". */
    g_suppressed = !g_edit && sys_window_focused();
    if (g_suppressed) flags = 0;
    const bool want = on && (s->in_room || g_edit);
    if (!want) { tiles_idle(); g_nshown = 0; if (g_proc || g_pipe) stop(); if (g_state != 2 || !on) g_state = 0; g_last[0] = 0; return; }
    const double now = cui_time(ui);
    if (!dry_run()) {
        pump_out();
        if (g_proc && WaitForSingleObject(g_proc, 0) == WAIT_OBJECT_0) { stop(); g_state = 3; g_fail_at = now; }
        if (!g_proc) { if (g_state == 2 || (g_state == 3 && now - g_fail_at < 5.0)) return; if (!spawn()) { g_fail_at = now; return; } g_last[0] = 0; }
    } else g_state = 1;
    collect_lives(s);
    const bool tiles_on = (flags & 1) && s->in_room;       /* só chamada de verdade recebe/decodifica; a edição fora da chamada mostra exemplos sem imagem */
    if (tiles_on) shm_ensure();
    char frame[4096]; compose(s, op, flags, corner, frame, sizeof frame);
    /* só miniatura LIGADA pede decodificação e gasta leitura/redução; as outras ficam zeradas */
    memset(g_want_r, 0, sizeof g_want_r); memset(g_want_r2, 0, sizeof g_want_r2);
    const bool pump_now = now - g_live_at >= LV_PERIOD; if (pump_now) g_live_at = now;
    for (int j = 0; j < NT; j++) {
        const live_t *l = j < g_nshown ? &g_lives[g_shown[j]] : NULL;
        if (tiles_on && l && g_tile_on[j] && l->slot >= 0) {
            if (l->second) g_want_r2[l->slot] = true; else g_want_r[l->slot] = true;
            if (l->path[0] && g_shm && (g_pipe || dry_run())) { if (pump_now) tile_pump(j, l->path); continue; }
        }
        tile_idle(j);
    }
    if (strcmp(frame, g_last) && now - g_last_send > 0.12) {
        snprintf(g_last, sizeof g_last, "%s", frame); g_last_send = now;
        if (g_pipe) { DWORD w; if (!WriteFile(g_pipe, frame, (DWORD)strlen(frame), &w, NULL)) { stop(); g_state = 3; g_fail_at = now; } }
    }
    { bool live = false; for (int j = 0; j < NT; j++) if (g_img_w[j] || (j < g_nshown && g_tile_on[j])) live = true; cui_request_wakeup(ui, live && tiles_on ? 0.016f : 0.15f); }   /* miniatura ligada: acorda a ~60 Hz para entregar 30 quadros/s */
}
