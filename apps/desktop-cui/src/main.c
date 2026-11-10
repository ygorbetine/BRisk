/* Remake do Briks em CUI: login, social e chamada (dados de exemplo; sem rede/WebRTC ainda). */
#include "briks_ui.h"
#include "backend.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

static app_state_t state = { .screen = SCREEN_AUTH, .active_friend = -1, .mic_on = true };

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

static void frame(cui_ctx *ui) {
    dev_autologin();
    app_pump(&state);
    { const char *o = getenv("BRIKS_DEV_OPEN"); if (o && state.screen == SCREEN_SOCIAL) { if (!strcmp(o, "settings")) state.settings_open = true; if (!strcmp(o, "add")) state.add_friend_open = true; } }
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 0) {
        briks_gradient_bar(ui);
        cui_box(ui, .dir = CUI_STACK, .width = CUI_FILL, .height = CUI_FILL) {
            switch (state.screen) {
            case SCREEN_AUTH: ui_auth(ui, &state); break;
            case SCREEN_SOCIAL: ui_social(ui, &state); break;
            case SCREEN_CALL: ui_call(ui, &state); break;
            }
        }
    }
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE a, HINSTANCE b, LPSTR c, int d) { (void)a; (void)b; (void)c; (void)d;
#else
int main(void) {
#endif
    cui_app *app = cui_app_create(&(cui_app_desc){
        .title = "Briks", .width = 1280, .height = 800, .min_width = 900, .min_height = 560,
        .theme = CUI_THEME_DARK, .accent = CUI_ACCENT_VIOLET });
    if (!app) return 1;
    state.app = app;
    bk_start();
    briks_apply_theme(cui_app_ui(app));
    briks_glass_init(app);
    while (cui_app_frame(app)) frame(cui_app_ui(app));
    briks_glass_shutdown();
    cui_app_destroy(app);
    bk_stop();
    return 0;
}
