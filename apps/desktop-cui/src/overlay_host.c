/* Sobreposição em jogos (lado do app): abre o briks_overlay.exe numa chamada, manda por stdin quem está na sala e o que mostrar,
 * e o fecha ao sair da chamada ou desligar a opção. Protocolo em src/overlay.c. Testes ocultos nunca abrem o processo (só montam o quadro). */
#include "briks_ui.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void settings_overlay(bool *on, float *opacity, int *flags, int *corner);
bool settings_ovl_place(char *dev, size_t cap, int *x, int *y, int *w);        /* settings.c: posição do monitor ativo */
void settings_ovl_set(const char *dev, int x, int y, int w);
void ovlkey_set(bool on); int ovlkey_take(void); int ovlkey_status(void);       /* hotkeys_ovl.c */

static HANDLE g_proc, g_pipe, g_rd;   /* g_rd: saída do overlay (layout/edit 0) */
static bool g_edit; static char g_rbuf[512]; static size_t g_rn; static int g_layouts, g_hk_state;
static int g_state;            /* 0 desligada, 1 ativa, 2 executável não encontrado, 3 fechou sozinha */
static char g_last[2048];      /* último quadro enviado */
static int g_last_lines; static double g_last_send, g_fail_at;

int overlay_state(void) { return g_state; }
int overlay_probe(const char *n) {
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

/* Monta o quadro do que aparece por cima do jogo. Visível para teste (sonda). */
static void compose(const app_state_t *s, float opacity, int flags, int corner, char *out, size_t cap) {
    size_t n = 0;
    n += (size_t)snprintf(out + n, cap - n, "cfg %d %d\nedit %d\n", (int)(opacity + 0.5f), corner, g_edit ? 1 : 0);
    { char dev[48]; int x, y, w; if (settings_ovl_place(dev, sizeof dev, &x, &y, &w)) n += (size_t)snprintf(out + n, cap - n, "place %s %d %d %d\n", dev, x, y, w); else n += (size_t)snprintf(out + n, cap - n, "place -\n"); }
    n += (size_t)snprintf(out + n, cap - n, "begin\n");
    int lines = 0;
    if (g_edit && !s->in_room) {   /* editando fora de uma chamada: pessoas de exemplo para ver o tamanho real */
        n += (size_t)snprintf(out + n, cap - n, "i Modo de edição\np 1 %s\np 0 Amigo\n", s->user_name[0] ? s->user_name : "Você"); lines = 3;
        snprintf(out + n, cap - n, "end\n"); g_last_lines = lines; return;
    }
    if ((flags & 4) && s->in_room) { n += (size_t)snprintf(out + n, cap - n, "i Sala conectada\n"); lines++; }
    if ((flags & 1) && s->sh.active) { n += (size_t)snprintf(out + n, cap - n, "i Você está transmitindo a tela\n"); lines++; }
    if ((flags & 8) && s->deaf) { n += (size_t)snprintf(out + n, cap - n, "w Ensurdecido\n"); lines++; }   /* independentes: com os dois, aparecem os dois avisos */
    if ((flags & 8) && !s->mic_on) { n += (size_t)snprintf(out + n, cap - n, "w Microfone mudo\n"); lines++; }
    if (flags & 2) {
        n += (size_t)snprintf(out + n, cap - n, "p %d %s\n", (s->mic_on && s->me_level > 0.05f) ? 1 : 0, s->user_name[0] ? s->user_name : "Você"); lines++;
        for (int i = 0; i < BRIKS_MAX_PEERS && n + 100 < cap; i++) if (s->rtc_peers[i].id[0]) {
            char nm[64]; snprintf(nm, sizeof nm, "%s", s->rtc_peers[i].name[0] ? s->rtc_peers[i].name : "Participante");
            for (char *c = nm; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
            n += (size_t)snprintf(out + n, cap - n, "p %d %s\n", s->rtc_peers[i].level > 0.05f ? 1 : 0, nm); lines++;
        }
    }
    snprintf(out + n, cap - n, "end\n");
    g_last_lines = lines;
}

/* Linha vinda do overlay: "layout <monitor> x y largura" (soltou) ou "edit 0" (Concluir). Também usada pelo roteiro de teste. */
static void handle_line(const char *ln) {
    char dev[48]; int x, y, w;
    if (!strncmp(ln, "layout ", 7) && sscanf(ln + 7, "%47s %d %d %d", dev, &x, &y, &w) == 4) { settings_ovl_set(dev, x, y, w); g_layouts++; }
    else if (!strcmp(ln, "edit 0")) g_edit = false;
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
/* roteiro: "sys ovl key" finge o atalho; "sys ovl line <texto>" finge uma linha vinda do overlay */
bool overlay_script(const char *args) {
    if (!strcmp(args, "key")) { overlay_edit_toggle(); return true; }
    if (!strncmp(args, "line ", 5)) { handle_line(args + 5); return true; }
    return false;
}

void overlay_tick(cui_ctx *ui, app_state_t *s) {
    bool on; float op; int flags, corner; settings_overlay(&on, &op, &flags, &corner);
    if (getenv("BRIKS_OVERLAY_FORCE")) on = true;
    if (!on) g_edit = false;
    {   /* Ctrl+Shift+O: ligado enquanto a sobreposição está ligada, em qualquer tela */
        static bool hk_on;
        if (on != hk_on) { hk_on = on; if (!dry_run()) ovlkey_set(on); }
        g_hk_state = on ? (dry_run() ? 1 : ovlkey_status()) : 0;
        if (!dry_run() && ovlkey_take() > 0 && on) overlay_edit_toggle();
    }
    const bool want = on && (s->in_room || g_edit);
    if (!want) { if (g_proc || g_pipe) stop(); if (g_state != 2 || !on) g_state = 0; g_last[0] = 0; return; }
    const double now = cui_time(ui);
    if (!dry_run()) {
        pump_out();
        if (g_proc && WaitForSingleObject(g_proc, 0) == WAIT_OBJECT_0) { stop(); g_state = 3; g_fail_at = now; }
        if (!g_proc) { if (g_state == 2 || (g_state == 3 && now - g_fail_at < 5.0)) return; if (!spawn()) { g_fail_at = now; return; } g_last[0] = 0; }
    } else g_state = 1;
    char frame[2048]; compose(s, op, flags, corner, frame, sizeof frame);
    if (strcmp(frame, g_last) && now - g_last_send > 0.12) {
        snprintf(g_last, sizeof g_last, "%s", frame); g_last_send = now;
        if (g_pipe) { DWORD w; if (!WriteFile(g_pipe, frame, (DWORD)strlen(frame), &w, NULL)) { stop(); g_state = 3; g_fail_at = now; } }
    }
    cui_request_wakeup(ui, 0.15f);
}
