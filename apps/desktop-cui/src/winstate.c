/* Janela e sistema (TRY-02/04/05/06, BAS-13..16): instância única, lembrar posição/tamanho/monitor/maximizado, fechar para a bandeja,
 * iniciar com o Windows, links externos e a configuração do pacote (sistema.cfg). O CUI trata o WM_CLOSE sozinho (encerra o laço); aqui a janela
 * é "subclassificada" (SetWindowLongPtr) para interceptar o fechar e as mensagens do ícone da bandeja sem mexer no CUI.
 * Teste automatizado: nunca grava no %APPDATA% real (só com BRIKS_SYS_FILE), nunca mostra janela/aviso, nunca registra autostart real. */
#include "sysint.h"
#include "volume_ctl.h"
#include "update.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

sys_cfg_t g_sys = { .close_to_tray = true, .nt_msg = true, .sounds = true, .sound_vol = 70 };

#define WM_TRAY (WM_APP + 0x41)
#define SYS_TIMER 7701
static bool g_automation, g_headless, g_start_min, g_quitting, g_in_tray, g_tray_ok, g_dirty, g_have_mutex;
static HWND g_hwnd; static WNDPROC g_orig; static cui_ctx *g_ui; static app_state_t *g_s;
static HANDLE g_mutex; static UINT g_msg_show, g_msg_taskbar;
static ULONGLONG g_dirty_at; static int g_show_count, g_restart_dry, g_restored = -1, g_saves;
#define LINK_MAX 34000   /* convite de grupo assinado cabe em ~32 KB (limite da linha de comando do Windows) */
static char g_link[LINK_MAX]; static wchar_t g_prop[100]; static int g_links;   /* briks://mensagem/<canal>/<id>, briks://friend/<codigo> ou briks://group/<codigo> pendente */
static char g_cfg_file[MAX_PATH]; static int g_url_ok, g_url_bad;

/* ---------------- configuração ---------------- */
static bool cfg_path(char *out, size_t cap) {
    const char *o = getenv("BRIKS_SYS_FILE");
    if (o && o[0]) { snprintf(out, cap, "%s", o); return true; }
    if (g_automation) return false;   /* teste nunca grava no AppData real */
    char ad[MAX_PATH]; if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, ad) != S_OK) return false;
    char dir[MAX_PATH]; snprintf(dir, sizeof dir, "%s\\Briks-CUI", ad); CreateDirectoryA(dir, NULL);
    snprintf(out, cap, "%s\\sistema.cfg", dir); return true;
}
static const struct { const char *k; char t; void *p; } CF[] = {
    { "close_to_tray", 'b', &g_sys.close_to_tray }, { "autostart", 'b', &g_sys.autostart }, { "nt_msg", 'b', &g_sys.nt_msg }, { "sounds", 'b', &g_sys.sounds },
    { "sound_vol", 'i', &g_sys.sound_vol }, { "tray_hint", 'b', &g_sys.tray_hint_shown }, { "win_x", 'i', &g_sys.win_x }, { "win_y", 'i', &g_sys.win_y },
    { "win_w", 'i', &g_sys.win_w }, { "win_h", 'i', &g_sys.win_h }, { "win_max", 'b', &g_sys.win_max }, { "win_valid", 'b', &g_sys.win_valid },
};
static void cfg_load(void) {
    if (!cfg_path(g_cfg_file, sizeof g_cfg_file)) return;
    FILE *f = fopen(g_cfg_file, "rb"); if (!f) return;
    char ln[160];
    while (fgets(ln, sizeof ln, f)) {
        char *eq = strchr(ln, '='); if (!eq) continue; *eq = 0; char *v = eq + 1; v[strcspn(v, "\r\n")] = 0;
        for (size_t i = 0; i < sizeof CF / sizeof *CF; i++) if (!strcmp(CF[i].k, ln)) { if (CF[i].t == 'i') *(int *)CF[i].p = atoi(v); else *(bool *)CF[i].p = atoi(v) != 0; }
    }
    fclose(f);
    if (g_sys.sound_vol < 0) g_sys.sound_vol = 0; if (g_sys.sound_vol > 100) g_sys.sound_vol = 100;
}
void sys_cfg_save(void) {
    if (!g_cfg_file[0]) return;
    FILE *f = fopen(g_cfg_file, "wb"); if (!f) return;
    for (size_t i = 0; i < sizeof CF / sizeof *CF; i++) { if (CF[i].t == 'i') fprintf(f, "%s=%d\n", CF[i].k, *(int *)CF[i].p); else fprintf(f, "%s=%d\n", CF[i].k, *(bool *)CF[i].p ? 1 : 0); }
    fclose(f); g_saves++;
}

bool sys_is_headless(void) { return g_headless; }
bool sys_in_tray(void) { return g_in_tray && !g_headless; }   /* janela escondida: o laço principal desacelera (main.c) */
void *sys_hwnd(void) { return g_hwnd; }
bool sys_start_minimized(void) { return g_start_min; }
bool sys_window_focused(void) {
    int f = notify_force_focus(); if (f >= 0) return f != 0;
    return g_hwnd && IsWindowVisible(g_hwnd) && !IsIconic(g_hwnd) && GetForegroundWindow() == g_hwnd;
}

/* ---------------- links externos (TRY-08) ---------------- */
bool sys_open_url(const char *url) {
    if (!sm_url_ok(url)) return false;
    if (g_headless && !getenv("BRIKS_SYS_OPEN_REAL")) return true;   /* teste: valida, mas não abre o navegador de ninguém */
    wchar_t w[2100]; if (MultiByteToWideChar(CP_UTF8, 0, url, -1, w, 2100) <= 0) return false;
    return (INT_PTR)ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL) > 32;
}

/* ---------------- iniciar com o Windows (TRY-06, BAS-14) ---------------- */
static const wchar_t *run_key(void) { static wchar_t k[200]; const char *o = getenv("BRIKS_AUTOSTART_KEY"); if (o && o[0]) { MultiByteToWideChar(CP_UTF8, 0, o, -1, k, 200); return k; } return L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"; }
static bool autostart_allowed(void) { return !g_automation || getenv("BRIKS_AUTOSTART_KEY"); }   /* nunca no registro real em teste */
bool sys_autostart_get(void) {
    if (!autostart_allowed()) return g_sys.autostart;   /* teste: nem lê o registro real */
    HKEY k; if (RegOpenKeyExW(HKEY_CURRENT_USER, run_key(), 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    DWORD ty = 0, n = 0; bool ok = RegQueryValueExW(k, L"Briks", NULL, &ty, NULL, &n) == ERROR_SUCCESS && ty == REG_SZ && n > 2;
    RegCloseKey(k); return ok;
}
bool sys_autostart_set(bool on) {
    if (!autostart_allowed()) return false;
    HKEY k; if (RegCreateKeyExW(HKEY_CURRENT_USER, run_key(), 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return false;
    bool ok;
    if (on) {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
        char u8[MAX_PATH * 3], cmd[MAX_PATH * 3 + 20]; WideCharToMultiByte(CP_UTF8, 0, exe, -1, u8, sizeof u8, NULL, NULL); sm_autostart_cmd(u8, cmd, sizeof cmd);
        wchar_t wc[MAX_PATH * 3 + 20]; MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wc, MAX_PATH * 3 + 20);
        ok = RegSetValueExW(k, L"Briks", 0, REG_SZ, (const BYTE *)wc, (DWORD)((wcslen(wc) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else { LONG r = RegDeleteValueW(k, L"Briks"); ok = r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND; }
    RegCloseKey(k);
    if (ok) { g_sys.autostart = on; sys_cfg_save(); }
    return ok;
}

/* ---------------- instância única (TRY-05, BAS-15) ---------------- */
static BOOL CALLBACK find_inst(HWND h, LPARAM lp) { struct { const wchar_t *p; HWND h; } *q = (void *)lp; if (GetPropW(h, q->p)) { q->h = h; return FALSE; } return TRUE; }
static void handle_link(const char *u) { if (u && !strncmp(u, "briks://", 8)) { snprintf(g_link, sizeof g_link, "%s", u); g_links++; } }
bool sys_startup(const char *cmd, bool automation) {
    g_automation = automation; g_headless = automation && !getenv("BRIKS_SHOW");
    cfg_load();
    const char *a = cmd ? strstr(cmd, "--aguardar=") : NULL;
    if (a) {   /* reinício: o processo antigo ainda segura o mutex; espera ele sair (até 15 s) */
        DWORD pid = (DWORD)strtoul(a + 11, NULL, 10); HANDLE h = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : NULL;
        if (h) { WaitForSingleObject(h, 15000); CloseHandle(h); }
    }
    g_start_min = cmd && strstr(cmd, "--minimizado") != NULL;
    { const char *b = cmd ? strstr(cmd, "briks://") : NULL; if (b) { snprintf(g_link, sizeof g_link, "%s", b); g_link[strcspn(g_link, "\" ")] = 0; } }
    const char *id = getenv("BRIKS_INSTANCE_ID");
    if (automation && !id) return true;   /* testes em paralelo não disputam o mutex (só com BRIKS_INSTANCE_ID, usado no teste da instância única) */
    wchar_t mn[120], sn[120], tn[40]; wchar_t wid[64] = L""; if (id) MultiByteToWideChar(CP_UTF8, 0, id, -1, wid, 64);
    swprintf(mn, 120, L"Local\\BriksCUI.Instancia.%ls", wid); swprintf(sn, 120, L"BriksCUI.Mostrar.%ls", wid); (void)tn;
    g_msg_show = RegisterWindowMessageW(sn);
    swprintf(g_prop, 100, L"BriksInst.%ls", wid);
    g_mutex = CreateMutexW(NULL, FALSE, mn);
    if (g_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        AllowSetForegroundWindow(ASFW_ANY);
        if (g_link[0]) { HWND t = NULL; struct { const wchar_t *p; HWND h; } q = { g_prop, NULL }; EnumWindows(find_inst, (LPARAM)&q); t = q.h;
            if (t) { COPYDATASTRUCT cd = { 0xB21C, (DWORD)strlen(g_link) + 1, g_link }; DWORD_PTR r; SendMessageTimeoutW(t, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 3000, &r); } }
        PostMessageW(HWND_BROADCAST, g_msg_show, 0, 0);   /* a primeira instância mostra a janela */
        CloseHandle(g_mutex); g_mutex = NULL;
        return false;
    }
    g_have_mutex = g_mutex != NULL;
    return true;
}

/* ---------------- briks:// (TRY-07): registro em HKCU\Software\Classes\briks, so no app real (ou BRIKS_PROTO_KEY em teste) ---------------- */
bool sys_protocol_register(void) {
    const char *o = getenv("BRIKS_PROTO_KEY"); if (g_automation && !o) return false;
    wchar_t base[200] = L"Software\\Classes\\briks"; if (o && o[0]) MultiByteToWideChar(CP_UTF8, 0, o, -1, base, 200);
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 20], k[260]; GetModuleFileNameW(NULL, exe, MAX_PATH); swprintf(cmd, MAX_PATH + 20, L"\"%ls\" \"%%1\"", exe);
    HKEY h; bool ok = true;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, base, 0, NULL, 0, KEY_SET_VALUE, NULL, &h, NULL) != ERROR_SUCCESS) return false;
    ok &= RegSetValueExW(h, NULL, 0, REG_SZ, (const BYTE *)L"URL:Briks", 20) == ERROR_SUCCESS; ok &= RegSetValueExW(h, L"URL Protocol", 0, REG_SZ, (const BYTE *)L"", 2) == ERROR_SUCCESS; RegCloseKey(h);
    swprintf(k, 260, L"%ls\\shell\\open\\command", base);
    if (RegCreateKeyExW(HKEY_CURRENT_USER, k, 0, NULL, 0, KEY_SET_VALUE, NULL, &h, NULL) != ERROR_SUCCESS) return false;
    ok &= RegSetValueExW(h, NULL, 0, REG_SZ, (const BYTE *)cmd, (DWORD)((wcslen(cmd) + 1) * 2)) == ERROR_SUCCESS; RegCloseKey(h);
    return ok;
}

/* ---------------- estado da janela (BAS-16) ---------------- */
typedef struct { wf_rect w[16]; int n; } mons_t;
static BOOL CALLBACK mon_cb(HMONITOR m, HDC dc, LPRECT r, LPARAM lp) {
    (void)dc; (void)r; mons_t *o = (mons_t *)lp; MONITORINFO mi = { sizeof mi }; if (o->n >= 16 || !GetMonitorInfoW(m, &mi)) return TRUE;
    wf_rect w = { mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom };
    if (mi.dwFlags & MONITORINFOF_PRIMARY) { memmove(&o->w[1], &o->w[0], sizeof(wf_rect) * (size_t)o->n); o->w[0] = w; } else o->w[o->n] = w;   /* principal na frente */
    o->n++; return TRUE;
}
static void monitors(mons_t *m) { m->n = 0; EnumDisplayMonitors(NULL, NULL, mon_cb, (LPARAM)m); if (m->n == 0) { m->w[0] = (wf_rect){ 0, 0, 1920, 1040 }; m->n = 1; } }

static void capture_window(void) {
    if (!g_hwnd) return;
    WINDOWPLACEMENT wp = { sizeof wp }; if (!GetWindowPlacement(g_hwnd, &wp)) return;
    RECT r;
    if (IsZoomed(g_hwnd) || IsIconic(g_hwnd)) {   /* retângulo "normal" vem em coordenadas da área de trabalho: converte para a tela */
        r = wp.rcNormalPosition; MONITORINFO mi = { sizeof mi }; HMONITOR pm = MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        if (GetMonitorInfoW(pm, &mi)) { int dx = mi.rcWork.left - mi.rcMonitor.left, dy = mi.rcWork.top - mi.rcMonitor.top; r.left += dx; r.right += dx; r.top += dy; r.bottom += dy; }
        g_sys.win_max = IsZoomed(g_hwnd) || (wp.flags & WPF_RESTORETOMAXIMIZED);
    } else { if (!GetWindowRect(g_hwnd, &r)) return; g_sys.win_max = false; }
    g_sys.win_x = r.left; g_sys.win_y = r.top; g_sys.win_w = r.right - r.left; g_sys.win_h = r.bottom - r.top; g_sys.win_valid = true;
    g_dirty = false; sys_cfg_save();
}

static void restore_window(void) {
    if (!g_sys.win_valid) return;
    mons_t m; monitors(&m); wf_rect out, saved = { g_sys.win_x, g_sys.win_y, g_sys.win_x + g_sys.win_w, g_sys.win_y + g_sys.win_h };
    g_restored = wf_fit(m.w, m.n, saved, 560, 480, &out, NULL);
    if (g_restored < 0) return;
    for (int i = 0; i < 2; i++)   /* 2x: ao trocar de monitor com outra escala o Windows ajusta o retângulo uma vez (WM_DPICHANGED) */
        SetWindowPos(g_hwnd, NULL, out.l, out.t, wf_w(out), wf_h(out), SWP_NOZORDER | SWP_NOACTIVATE);
}

/* ---------------- janela: mostrar, fechar, sair, reiniciar ---------------- */
void sys_show_window(void) {
    g_in_tray = false; g_show_count++; tray_taskbar_invalidate();
    if (g_hwnd && !g_headless) {
        if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
        else if (!IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd, g_sys.win_max ? SW_SHOWMAXIMIZED : SW_SHOW);
        SetForegroundWindow(g_hwnd);
    }
    if (g_ui) cui_request_redraw(g_ui);
}
void sys_request_quit(void) { g_quitting = true; if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0); }
void sys_restart(void) {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t cl[MAX_PATH + 60]; swprintf(cl, MAX_PATH + 60, L"\"%ls\" --aguardar=%lu", exe, (unsigned long)GetCurrentProcessId());
    const char *dry = getenv("BRIKS_SYS_RESTART_DRY");
    if (dry && dry[0]) { FILE *f = fopen(dry, "wb"); if (f) { char u[MAX_PATH * 3 + 80]; WideCharToMultiByte(CP_UTF8, 0, cl, -1, u, sizeof u, NULL, NULL); fputs(u, f); fclose(f); } g_restart_dry++; sys_request_quit(); return; }
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    if (CreateProcessW(NULL, cl, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); sys_request_quit(); }
}

static void run_cmd(int cmd) {
    switch (cmd) {
    case TRAY_CMD_OPEN: sys_show_window(); break;
    case TRAY_CMD_MUTE: if (g_s && g_s->in_room) { g_s->mic_on = !g_s->mic_on; rtc_set_mic(g_s, g_s->mic_on); if (g_ui) cui_request_redraw(g_ui); } break;
    case TRAY_CMD_RESTART: sys_restart(); break;
    case TRAY_CMD_QUIT: sys_request_quit(); break;
    default: break;
    }
}

bool overlay_script(const char *args);   /* overlay_host.c */
bool titlebar_proc(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res); void titlebar_attach(void *hwnd); int titlebar_probe(const char *n); bool titlebar_script(const char *cmd, int a, int b); int titlebar_hit_result(void);   /* titlebar.c */
static LRESULT CALLBACK sys_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    { LRESULT tr; if (titlebar_proc(h, m, w, l, &tr)) return tr; }   /* barra de título própria: sem moldura do sistema, acerto do mouse */
    if (g_msg_show && m == g_msg_show) { sys_show_window(); return 0; }
    if (g_msg_taskbar && m == g_msg_taskbar) { tray_readd(); return 0; }
    switch (m) {
    case WM_TRAY: {
        int cmd; tray_handle(LOWORD(l), h, &cmd); run_cmd(cmd);
        if (LOWORD(l) == NIN_BALLOONUSERCLICK && g_s) { char ch[64]; if (notify_take_click(ch, sizeof ch)) notify_open_channel(g_s, ch); }   /* clicou no aviso: abre a conversa */
        return 0;
    }
    case WM_CLOSE:
        capture_window();
        if (!g_quitting && g_sys.close_to_tray && tray_present() && update_phase() != UPD_READY) {   /* atualização baixada: fechar encerra de verdade para instalar */   /* fechar = bandeja: a chamada continua */
            if (!g_headless) ShowWindow(h, SW_HIDE);
            g_in_tray = true;
            if (!g_sys.tray_hint_shown) { g_sys.tray_hint_shown = true; sys_cfg_save(); tray_balloon("O Briks continua aberto", "Ele ficou na bandeja do sistema, e a chamada não cai. Para sair de vez, use Sair no menu do ícone."); }
            return 0;
        }
        break;
    case WM_COPYDATA: { COPYDATASTRUCT *cd = (COPYDATASTRUCT *)l; if (cd && cd->dwData == 0xB21C && cd->lpData && cd->cbData > 8 && cd->cbData < LINK_MAX) { static char b[LINK_MAX]; memcpy(b, cd->lpData, cd->cbData); b[cd->cbData - 1] = 0; handle_link(b); } return TRUE; }
    case WM_TIMER:
        if (w == SYS_TIMER) { if (g_ui && (!IsWindowVisible(h) || IsIconic(h))) cui_request_redraw(g_ui); return 0; }   /* escondida: o app segue recebendo eventos do core */
        break;
    case WM_EXITSIZEMOVE: case WM_SIZE: case WM_MOVE: g_dirty = true; g_dirty_at = GetTickCount64(); break;
    default: break;
    }
    return CallWindowProcW(g_orig, h, m, w, l);
}

void sys_attach(app_state_t *s, bool automation) {
    g_s = s; g_automation = automation; g_headless = automation && !getenv("BRIKS_SHOW");
    g_hwnd = (HWND)cui_app_native_window(s->app); g_ui = cui_app_ui(s->app);
    if (!g_hwnd) return;
    g_orig = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)sys_proc);
    titlebar_attach(g_hwnd);
    g_msg_taskbar = RegisterWindowMessageW(L"TaskbarCreated");
    if (!g_prop[0]) swprintf(g_prop, 100, L"BriksInst."); SetPropW(g_hwnd, g_prop, (HANDLE)1);
    if (!getenv("BRIKS_WINDOW") && !getenv("BRIKS_TEST_WINDOW") && (!automation || getenv("BRIKS_SYS_FILE"))) restore_window();
    if (!automation || getenv("BRIKS_TRAY_TEST")) g_tray_ok = tray_create(g_hwnd, WM_TRAY);
    if (autostart_allowed()) { bool reg = sys_autostart_get(); if (reg != g_sys.autostart) { g_sys.autostart = reg; } if (reg) sys_autostart_set(true); }   /* o registro manda; reescreve o caminho se o exe mudou */
    SetTimer(g_hwnd, SYS_TIMER, 400, NULL);
    sys_protocol_register();
    if (g_headless) { if (g_start_min && g_tray_ok) g_in_tray = true; return; }
    if (g_start_min && g_tray_ok) { g_in_tray = true; return; }   /* aberto pelo Windows: só na bandeja */
    ShowWindow(g_hwnd, g_sys.win_max && g_restored >= 0 ? SW_SHOWMAXIMIZED : SW_SHOWDEFAULT); UpdateWindow(g_hwnd);
}

void sys_tick(app_state_t *s) {
    g_s = s;
    if (g_dirty && GetTickCount64() - g_dirty_at > 1500 && g_hwnd && !IsIconic(g_hwnd)) capture_window();   /* posição/tamanho gravados logo depois de mexer */
    notify_tick(s);
    if (g_link[0] && s->screen != SCREEN_AUTH) {   /* briks://mensagem/<canal>/<id>: abre a conversa */
        static char l[LINK_MAX]; snprintf(l, sizeof l, "%s", g_link); g_link[0] = 0;
        if (!strncmp(l, "briks://mensagem/", 17)) { char *ch = l + 17, *sl = strchr(ch, '/'); if (sl) *sl = 0; if (ch[0]) { notify_open_channel(s, ch); sys_show_window(); } }
        else if (!strncmp(l, "briks://friend/", 15) || !strncmp(l, "briks://group/", 14)) {
            /* convites: o link só PREENCHE o código e abre o diálogo; entrar/adicionar exige o clique da pessoa (nunca é automático) */
            const bool friend = l[8] == 'f'; char *src = l + (friend ? 15 : 14);
            src[strcspn(src, "?#\r\n \t\"")] = 0;
            size_t n = strlen(src); while (n && src[n - 1] == '/') src[--n] = 0;
            char *code = friend ? s->amg_in : s->join_code; const size_t cap = friend ? sizeof s->amg_in : sizeof s->join_code; size_t o = 0;
            const char *c = src;
            for (; *c && o + 1 < cap; c++) {   /* o navegador pode ter escapado + / = (%XX) */
                if (*c == '%' && isxdigit((unsigned char)c[1]) && isxdigit((unsigned char)c[2])) { char hx[3] = { c[1], c[2], 0 }; code[o++] = (char)strtol(hx, NULL, 16); c += 2; }
                else code[o++] = *c;
            }
            code[o] = 0;
            const bool whole = *c == 0;   /* cabe inteiro? */
            if (!o || !whole) { snprintf(s->notice, sizeof s->notice, "O link de convite está incompleto ou é grande demais."); code[0] = 0; }
            else {
                if (friend) { s->amg_msg[0] = 0; snprintf(s->amg_msg, sizeof s->amg_msg, "Convite recebido por link. Confira de quem é e toque em Adicionar para confirmar."); s->add_friend_open = true; }
                else { s->join_err[0] = 0; s->join_open = true; }
                s->screen = SCREEN_SOCIAL; sys_show_window();
            }
        }
    }
}

void sys_before_destroy(app_state_t *s) {
    (void)s;
    if (g_hwnd && IsWindow(g_hwnd)) capture_window();
    tray_destroy();
    if (g_hwnd && IsWindow(g_hwnd)) KillTimer(g_hwnd, SYS_TIMER);
    if (g_mutex) { CloseHandle(g_mutex); g_mutex = NULL; }
}

/* ---------------- roteiro de teste ---------------- */
int sys_probe(const char *n) {
    RECT r = { 0 }; if (g_hwnd) GetWindowRect(g_hwnd, &r);
    if (!strcmp(n, "tray")) return tray_present();
    if (!strcmp(n, "to_tray")) return g_in_tray;
    if (!strcmp(n, "quitting")) return g_quitting;
    if (!strcmp(n, "shown")) return g_show_count;
    if (!strcmp(n, "unread")) return notify_unread_total();
    if (!strcmp(n, "notif")) return g_notify_shown;
    if (!strcmp(n, "blocked")) return g_notify_blocked;
    if (!strcmp(n, "sounds")) return g_sound_played;
    if (!strcmp(n, "balloons")) return g_tray_balloons;
    if (!strcmp(n, "badge_ok")) return g_tray_badge_ok;
    if (!strcmp(n, "close_tray")) return g_sys.close_to_tray;
    if (!strcmp(n, "autostart")) return sys_autostart_get();
    if (!strcmp(n, "wnd_x")) return r.left; if (!strcmp(n, "wnd_y")) return r.top;
    if (!strcmp(n, "wnd_w")) return r.right - r.left; if (!strcmp(n, "wnd_h")) return r.bottom - r.top;
    if (!strcmp(n, "wnd_max")) return g_hwnd && IsZoomed(g_hwnd);
    if (!strcmp(n, "wnd_inside")) { mons_t m; monitors(&m); wf_rect w = { r.left, r.top, r.right, r.bottom }; for (int i = 0; i < m.n; i++) if (w.l >= m.w[i].l && w.t >= m.w[i].t && w.r <= m.w[i].r && w.b <= m.w[i].b) return 1; return 0; }
    if (!strcmp(n, "restored")) return g_restored;
    if (!strcmp(n, "saved")) return g_sys.win_valid;
    if (!strcmp(n, "saves")) return g_saves;
    if (!strcmp(n, "mutex")) return g_have_mutex;
    if (!strcmp(n, "restart_dry")) return g_restart_dry;
    if (!strcmp(n, "links")) return g_links;
    if (!strcmp(n, "url_ok")) return g_url_ok; if (!strcmp(n, "url_bad")) return g_url_bad;
    if (!strcmp(n, "mic")) return g_s ? g_s->mic_on : -1;
    if (!strcmp(n, "active_group")) return g_s ? g_s->active_group : -1;
    if (!strcmp(n, "active_friend")) return g_s ? g_s->active_friend : -1;
    if (!strcmp(n, "tray_visible")) return tray_visible();
    if (!strcmp(n, "tray_icon_badged")) return g_tray_icon_badged;
    if (!strcmp(n, "tb_hit")) return titlebar_hit_result();
    return titlebar_probe(n);
}

static int tokens(char *s, char **t, int max) { int n = 0; for (char *p = strtok(s, " "); p && n < max; p = strtok(NULL, " ")) t[n++] = p; return n; }
bool sys_script(const char *args) {
    char buf[600]; snprintf(buf, sizeof buf, "%s", args ? args : ""); char *t[8]; int n = tokens(buf, t, 8); if (!n) return false;
    const char *c = t[0]; char rest[400] = "", rest1[400] = "";   /* rest1 = depois do 1º termo; rest = depois do 2º */
    { const char *p = args; for (int i = 0; i < 2 && p; i++) { p = strchr(p, ' '); if (p) p++; if (i == 0 && p) snprintf(rest1, sizeof rest1, "%s", p); } if (p) snprintf(rest, sizeof rest, "%s", p); }
    if (!strcmp(c, "close")) { if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0); }
    else if (!strcmp(c, "tray") && n > 1) { UINT ev = !strcmp(t[1], "balloon") ? NIN_BALLOONUSERCLICK : !strcmp(t[1], "menu") ? WM_CONTEXTMENU : NIN_SELECT; if (ev != WM_CONTEXTMENU && g_hwnd) PostMessageW(g_hwnd, WM_TRAY, 1, ev); }
    else if (!strcmp(c, "cmd") && n > 1) run_cmd(!strcmp(t[1], "open") ? TRAY_CMD_OPEN : !strcmp(t[1], "mute") ? TRAY_CMD_MUTE : !strcmp(t[1], "restart") ? TRAY_CMD_RESTART : !strcmp(t[1], "quit") ? TRAY_CMD_QUIT : 0);
    else if (!strcmp(c, "focus") && n > 1) notify_set_force_focus(!strcmp(t[1], "auto") ? -1 : atoi(t[1]));
    else if (!strcmp(c, "grace") && n > 1) notify_set_grace_ms((unsigned)atoi(t[1]));
    else if (!strcmp(c, "second") && g_hwnd && g_msg_show) PostMessageW(g_hwnd, g_msg_show, 0, 0);   /* o que a segunda instância faz */
    else if (!strcmp(c, "save")) capture_window();
    else if (!strcmp(c, "move") && n > 4 && g_hwnd) SetWindowPos(g_hwnd, NULL, atoi(t[1]), atoi(t[2]), atoi(t[3]), atoi(t[4]), SWP_NOZORDER | SWP_NOACTIVATE);
    else if (!strcmp(c, "sound") && n > 1) sound_play((sound_kind_t)atoi(t[1]));
    else if (!strcmp(c, "cfg") && n > 2) { bool v = atoi(t[2]) != 0; if (!strcmp(t[1], "close_to_tray")) g_sys.close_to_tray = v; else if (!strcmp(t[1], "nt_msg")) g_sys.nt_msg = v; else if (!strcmp(t[1], "sounds")) g_sys.sounds = v; }
    else if (!strcmp(c, "ovl") && n > 1) { return overlay_script(rest1); }   /* ovl key | ovl line <texto do overlay> */
    else if (!strcmp(c, "tbhit") && n > 2) titlebar_script("tbhit", atoi(t[1]), atoi(t[2]));   /* tbhit X Y (pixels do cliente): guarda o WM_NCHITTEST desse ponto; expect tb_hit <n> */
    else if (!strcmp(c, "tbact") && n > 1) titlebar_script("tbact", !strcmp(t[1], "min") ? 0 : !strcmp(t[1], "max") ? 1 : 2, 0);
    else if (!strcmp(c, "link") && n > 1) handle_link(t[1]);
    else if (!strcmp(c, "fwask")) fw_maybe_prompt();   /* o que a primeira chamada faz */
    else if (!strcmp(c, "autostart") && n > 1) sys_autostart_set(atoi(t[1]) != 0);
    else if (!strcmp(c, "msg") && n > 3 && g_s) { char x[300]; snprintf(x, sizeof x, "%s", strstr(rest, t[2]) ? strstr(rest, t[2]) + strlen(t[2]) + 1 : ""); notify_chat_message(g_s, t[1], t[2], x); }   /* msg <canal> <autor> <texto> */
    else if (!strcmp(c, "friend") && g_s) notify_friend(g_s, rest1, false);
    else if (!strcmp(c, "friendcall") && g_s) notify_friend(g_s, rest1, true);
    else if (!strcmp(c, "room") && n > 1 && g_s) g_s->in_room = atoi(t[1]) != 0;   /* simula entrar/sair da sala (sons, regra de "em chamada") */
    else if (!strcmp(c, "url") && n > 1) { if (sys_open_url(t[1])) g_url_ok++; else g_url_bad++; }
    else return false;
    return true;
}

/* ---------------- Configurações: cartões do pacote ---------------- */
#define SYS_CARD(ui, title) CUI__SCOPE((cui_box_begin((ui), CUI__OPTS(cui_box, .width = CUI_FILL, .pad = 14, .gap = 10, .radius = 16, .bg = B_GLASS_CARD, \
    .border = 1, .border_color = B_GLASS_BORDER)), cui_text((ui), (title), .font = CUI_FONT_MEDIUM, .size = 13, .color = B_C3)), cui_box_end(ui))
static void hint(cui_ctx *ui, const char *t) { cui_text(ui, t, .size = 12, .color = B_MUTED); }

void sys_settings_notifications(cui_ctx *ui) {
    SYS_CARD(ui, "AVISOS DO SISTEMA") {
        if (cui_switch(ui, "Nova mensagem", &g_sys.nt_msg)) sys_cfg_save();
        hint(ui, "Aparece pelo ícone da bandeja quando o Briks não está em foco. Nada é avisado durante uma chamada nem nos primeiros segundos depois de abrir.");
        if (!tray_present()) hint(ui, "O ícone da bandeja não está ativo neste momento, então os avisos do sistema não aparecem.");
    }
    SYS_CARD(ui, "SONS") {
        if (cui_switch(ui, "Sons de aviso e da chamada", &g_sys.sounds)) sys_cfg_save();
        hint(ui, "Toca ao receber um aviso e quando você ou alguém entra ou sai da chamada.");
        float v = (float)g_sys.sound_vol; cui_textf(ui, "Volume dos sons: %d%%", g_sys.sound_vol);
        if (briks_volume(ui, "sys-vol", &v, 0.0f, 100.0f, 1.0f, NULL, false)) { g_sys.sound_vol = (int)(v + 0.5f); sys_cfg_save(); }
        if (cui_button(ui, "Testar som", .variant = CUI_BUTTON_SECONDARY)) sound_play(SND_NOTIFY);
    }
}

void sys_settings_system(cui_ctx *ui) {
    SYS_CARD(ui, "SISTEMA") {
        if (cui_switch(ui, "Fechar para a bandeja", &g_sys.close_to_tray)) sys_cfg_save();
        hint(ui, g_sys.close_to_tray ? "Ao fechar a janela o Briks continua na bandeja e a chamada não cai. Para sair de vez, use Sair no menu do ícone."
                                     : "Ao fechar a janela o Briks encerra, e a chamada em andamento acaba.");
        bool a = g_sys.autostart;
        if (cui_switch(ui, "Iniciar com o Windows", &a) && a != g_sys.autostart) {
            if (!sys_autostart_set(a)) settings_notice("Não foi possível alterar o início com o Windows.");
        }
        hint(ui, "Abre o Briks só na bandeja quando você entra no Windows.");
    }
}
