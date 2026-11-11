/* Sobreposição em jogos (lado do app): abre o briks_overlay.exe numa chamada, manda por stdin quem está na sala e o que mostrar,
 * e o fecha ao sair da chamada ou desligar a opção. Protocolo em src/overlay.c. Testes ocultos nunca abrem o processo (só montam o quadro). */
#include "briks_ui.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void settings_overlay(bool *on, float *opacity, int *flags, int *corner);

static HANDLE g_proc, g_pipe;
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
    if (!strcmp(n, "overlay_opacity")) { const char *c = strstr(g_last, "cfg "); return c ? atoi(c + 4) : -1; }
    if (!strcmp(n, "overlay_corner")) { const char *c = strstr(g_last, "cfg "); if (!c) return -1; c += 4; while (*c && *c != ' ') c++; return atoi(c); }
    return -9999;
}

static bool dry_run(void) { return getenv("BRIKS_HIDDEN") || getenv("BRIKS_SCRIPT") || getenv("BRIKS_DEMO"); }

static void stop(void) {
    if (g_pipe) { DWORD w; WriteFile(g_pipe, "quit\n", 5, &w, NULL); CloseHandle(g_pipe); g_pipe = NULL; }
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
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { g_state = 3; return false; }
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = rd;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(rd);
    if (!ok) { CloseHandle(wr); g_state = 3; return false; }
    CloseHandle(pi.hThread); g_proc = pi.hProcess; g_pipe = wr; g_state = 1;
    return true;
}

/* Monta o quadro do que aparece por cima do jogo. Visível para teste (sonda). */
static void compose(const app_state_t *s, float opacity, int flags, int corner, char *out, size_t cap) {
    size_t n = 0;
    n += (size_t)snprintf(out + n, cap - n, "cfg %d %d\nbegin\n", (int)(opacity + 0.5f), corner);
    int lines = 0;
    if ((flags & 4) && s->in_room) { n += (size_t)snprintf(out + n, cap - n, "i Sala conectada\n"); lines++; }
    if ((flags & 1) && s->sh.active) { n += (size_t)snprintf(out + n, cap - n, "i Você está transmitindo a tela\n"); lines++; }
    if ((flags & 8) && (s->deaf || !s->mic_on)) { n += (size_t)snprintf(out + n, cap - n, "w %s\n", s->deaf ? "Ensurdecido" : "Microfone mudo"); lines++; }
    if (flags & 2) {
        n += (size_t)snprintf(out + n, cap - n, "p %d %s\n", (s->mic_on && !s->deaf && s->me_level > 0.05f) ? 1 : 0, s->user_name[0] ? s->user_name : "Você"); lines++;
        for (int i = 0; i < BRIKS_MAX_PEERS && n + 100 < cap; i++) if (s->rtc_peers[i].id[0]) {
            char nm[64]; snprintf(nm, sizeof nm, "%s", s->rtc_peers[i].name[0] ? s->rtc_peers[i].name : "Participante");
            for (char *c = nm; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
            n += (size_t)snprintf(out + n, cap - n, "p %d %s\n", s->rtc_peers[i].level > 0.05f ? 1 : 0, nm); lines++;
        }
    }
    snprintf(out + n, cap - n, "end\n");
    g_last_lines = lines;
}

void overlay_tick(cui_ctx *ui, app_state_t *s) {
    bool on; float op; int flags, corner; settings_overlay(&on, &op, &flags, &corner);
    if (getenv("BRIKS_OVERLAY_FORCE")) on = true;
    const bool want = on && s->in_room;
    if (!want) { if (g_proc || g_pipe) stop(); if (g_state != 2 || !on) g_state = 0; g_last[0] = 0; return; }
    const double now = cui_time(ui);
    if (!dry_run()) {
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
