/* Remake do Briks em CUI: login, social e chamada (dados de exemplo; sem rede/WebRTC ainda). */
#include "briks_ui.h"
#include "update.h"
#include "msgui.h"
#include "backend.h"
#include "core.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <crtdbg.h>
#include <stdlib.h>
#endif

static void *g_prev_fg;   /* janela que tinha o foco antes de uma janela de teste abrir */
static app_state_t state = { .screen = SCREEN_AUTH, .active_friend = -1, .active_group = -1, .mic_on = true };

/* Gancho só para teste automatizado: BRIKS_DEV_AUTH="modo|nome|email|senha" (modo 0 login, 1 cadastro). */
static void dev_autologin(void) {
    static bool done;
    const char *spec = getenv("BRIKS_DEV_AUTH");
    if (done || !spec || bk_status() != BK_READY || state.busy) return;
    done = true;
    char buf[512]; snprintf(buf, sizeof buf, "%s", spec);
    char *f[4] = { 0 }; int n = 0;
    for (char *p = strtok(buf, "|"); p && n < 4; p = strtok(NULL, "|")) f[n++] = p;
    if (n < 4) return;
    state.auth_mode = atoi(f[0]);
    snprintf(state.name, sizeof state.name, "%s", f[1]);
    snprintf(state.email, sizeof state.email, "%s", f[2]);
    snprintf(state.password, sizeof state.password, "%s", f[3]);
    app_submit_auth(&state);
}

int script_probe(const char *n) {
    if (!strcmp(n, "profile")) return state.profile_open;
    if (!strcmp(n, "settings")) return state.settings_open;
    if (!strcmp(n, "add_friend")) return state.add_friend_open;
    if (!strcmp(n, "create_group")) return state.create_group_open;
    if (!strcmp(n, "friends_tab")) return state.friends_tab;
    if (!strcmp(n, "screen")) return (int)state.screen;
    if (!strcmp(n, "modal")) return g_modal_prev;
    if (!strcmp(n, "win_w") || !strcmp(n, "win_h")) { RECT r; if (state.app && GetClientRect((HWND)cui_app_native_window(state.app), &r)) return n[4] == 'w' ? (int)(r.right - r.left) : (int)(r.bottom - r.top); return -1; }
    return dock_probe(n);
}

static void frame(cui_ctx *ui) {
    g_modal_prev = g_modal_now; g_modal_now = false; g_in_modal = 0;
    memcpy(g_cov_prev, g_cov_now, sizeof g_cov_now); g_cov_n_prev = g_cov_n_now; g_cov_n_now = 0;
    { static int applied = -1; if (applied != g_text_size) { applied = g_text_size; briks_text_scale_apply(ui); } }
    if (briks_theme_poll()) briks_apply_theme(ui);   /* hot reload do theme.json */
    { static bool demo_done; const char *d = getenv("BRIKS_DEMO"); if (d && !demo_done) { demo_done = true; app_demo(&state, d); } }
    dev_autologin();
    { const char *r = getenv("BRIKS_DEV_ROOM"); if (r && r[0] && !state.in_room && !state.rtc_session_sent) rtc_join_room(&state, r); }   /* teste: entra na sala sem clicar */
    account_tick(&state);
    migrate_pump(&state);
    app_pump(&state);
    rtc_pump(&state);
    msgui_pump(&state);
    profile_tick(&state);
    settings_tick();
    hotkeys_tick(ui, &state);
    themes_tick(ui);
    { const char *o = getenv("BRIKS_DEV_OPEN"); if (o && state.screen == SCREEN_SOCIAL) { if (!strncmp(o, "settings", 8)) { state.settings_open = true; if (o[8] == ':') state.settings_tab = atoi(o + 9); } if (!strcmp(o, "add")) state.add_friend_open = true; if (!strncmp(o, "profile", 7)) { state.profile_open = true; if (o[7] == ':') state.profile_tab = atoi(o + 8); } } }
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 0) {
        briks_gradient_bar(ui);
        cui_box(ui, .dir = CUI_STACK, .width = CUI_FILL, .height = CUI_FILL) {
            switch (state.screen) {
            case SCREEN_AUTH: ui_auth(ui, &state); break;
            case SCREEN_SOCIAL: ui_social(ui, &state); break;
            case SCREEN_CALL: ui_call(ui, &state); break;
            case SCREEN_HISTORY: ui_history(ui, &state); break;
            }
            briks_tip_draw(ui);
            update_banner(ui, &state);   /* dentro da pilha: sobrepõe a tela sem ocupar espaço do layout */
            if (getenv("BRIKS_SCRIPT")) script_overlay(ui);
        }
    }
    migrate_dialog(ui, &state);
    briks_theme_editor(ui);   /* Ctrl+Shift+T com BRIKS_DEV=1 */
}

/* Teste automatizado (captura, roteiro, BRIKS_HIDDEN): nunca abre diálogo na tela de ninguém. Erros do CRT/Run-Time Check vão para stderr e o processo sai com código 70. */
#ifdef _WIN32
static int crt_hook(int type, char *msg, int *ret) { (void)type; if (msg) fputs(msg, stderr); fputs(" briks_cui: falha de runtime, encerrando (70) ", stderr); fflush(stderr); ExitProcess(70); *ret = 1; return TRUE; }
static void quiet_automation(void) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_error_mode(_OUT_TO_STDERR);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportHook(crt_hook);
}
#endif

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE a, HINSTANCE b, LPSTR c, int d) { (void)a; (void)b; (void)c; (void)d;
#else
int main(void) {
#endif
    const bool automation = getenv("CUI_SCREENSHOT") || getenv("BRIKS_SCRIPT") || getenv("BRIKS_HIDDEN");   /* janela oculta por padrão em testes */
#ifdef _WIN32
    if (automation) quiet_automation();
#endif
    int win_w = 1280, win_h = 800;   /* BRIKS_WINDOW=LxA: tamanho inicial (capturas) */
    { const char *w = getenv("BRIKS_WINDOW"); int a, h; if (w && sscanf(w, "%dx%d", &a, &h) == 2 && a >= 560 && h >= 480) { win_w = a; win_h = h; } }
#ifdef _WIN32
    if (!getenv("BRIKS_WINDOW") || getenv("BRIKS_FAKE_WORKAREA")) {   /* nunca abre maior que a área útil da tela (tela pequena / escala de 125%+): senão o app passa das bordas */
        RECT wa; int fw, fh; const char *fk = getenv("BRIKS_FAKE_WORKAREA");   /* teste: simula monitor pequeno (LxA) */
        bool have = fk && sscanf(fk, "%dx%d", &fw, &fh) == 2 ? (wa.left = 0, wa.top = 0, wa.right = fw, wa.bottom = fh, true) : SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
        if (have) {
            int mw = (int)(wa.right - wa.left) - 24, mh = (int)(wa.bottom - wa.top) - 24;
            if (win_w > mw) win_w = mw < 560 ? 560 : mw;
            if (win_h > mh) win_h = mh < 480 ? 480 : mh;
        }
    }
    g_prev_fg = getenv("BRIKS_TEST_WINDOW") ? (void *)GetForegroundWindow() : NULL;
#endif
    cui_app *app = cui_app_create(&(cui_app_desc){
        .title = "Briks", .width = win_w, .height = win_h, .min_width = 560, .min_height = 480,
        .theme = CUI_THEME_DARK, .accent = CUI_ACCENT_VIOLET, .continuous = getenv("BRIKS_SCRIPT") != NULL, .hidden = automation && !getenv("BRIKS_SHOW") });
    if (!app) return 1;
    state.app = app;
    migrate_probe(&state);   /* pergunta antes de subir o backend (que abre o banco) */
    update_start();   /* checa o feed de releases em segundo plano (só no app instalado) */
#ifdef _WIN32
    {   /* BRIKS_TEST_WINDOW="x,y": janela de teste noutra tela (ou fora da área visível), sem ficar com o foco de quem está usando o PC */
        const char *tw = getenv("BRIKS_TEST_WINDOW"); int x, y;
        if (tw && sscanf(tw, "%d,%d", &x, &y) == 2) {
            HWND prev = (HWND)g_prev_fg, me = (HWND)cui_app_native_window(app);
            SetWindowPos(me, HWND_BOTTOM, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            if (prev && prev != me) SetForegroundWindow(prev);
        }
    }
#endif
    briks_perf_init();
    briks_theme_poll();   /* lê theme.json (ou os padrões) antes de montar o tema */
    briks_apply_theme(cui_app_ui(app));
    cui_set_reduced_motion(cui_app_ui(app), g_perf >= PERF_SIMPLE);
    briks_glass_init(app);
    bool scripted = script_load();
    while (cui_app_frame(app)) {
        if (scripted) { script_tick(cui_app_ui(app)); if (g_script_rw) { cui_app_set_size(app, g_script_rw, g_script_rh); g_script_rw = g_script_rh = 0; } }
        frame(cui_app_ui(app));
        if (scripted && script_capture(app)) break;
    }
    briks_glass_shutdown();
    cui_app_destroy(app);
    core_stop();
    bk_stop();
    update_install_on_exit();   /* atualização já baixada e verificada: instala em silêncio ao fechar */
    return g_script_fail ? 1 : 0;
}
