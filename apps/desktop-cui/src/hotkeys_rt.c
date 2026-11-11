#include "hotkeys.h"
#include <string.h>
#include "briks_ui.h"
#include <windows.h>

static bool key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
static bool combo_down(unsigned mods, int vk) {
    if (!key_down(vk)) return false;
    return key_down(VK_CONTROL) == ((mods & HK_CTRL) != 0) && key_down(VK_SHIFT) == ((mods & HK_SHIFT) != 0) && key_down(VK_MENU) == ((mods & HK_ALT) != 0);
}

/* A cada quadro: só numa chamada. Atalhos globais (mutar, ensurdecer, transmissão) agem na borda da tecla; PTT segue a tecla.
 * Pede um acordar a cada 30 ms enquanto algum atalho está configurado e a pessoa está na sala. */
void hotkeys_tick(cui_ctx *ui, app_state_t *s) {
    static bool prev[3]; static hk_ptt_t ptt;
    const char *k[3]; const char *kp; bool ptt_on; float delay;
    settings_keys(&k[0], &k[1], &k[2], &kp, &ptt_on, &delay);
    unsigned m; int vk;
    if (!s->in_room) { memset(prev, 0, sizeof prev); memset(&ptt, 0, sizeof ptt); return; }
    bool any = ptt_on && hk_parse(kp, &m, &vk);
    for (int i = 0; i < 3; i++) {
        bool ok = hk_parse(k[i], &m, &vk); any |= ok;
        bool d = ok && combo_down(m, vk);
        if (d && !prev[i]) {
            if (i == 0) { if (s->deaf) { s->deaf = false; rtc_set_deaf(s, false); } else { s->mic_on = !s->mic_on; rtc_set_mic(s, s->mic_on); } }
            else if (i == 1) { s->deaf = !s->deaf; rtc_set_deaf(s, s->deaf); }
            else { if (s->sh.active) share_stop(s); else share_open_dialog(s); }
        }
        prev[i] = d;
    }
    if (ptt_on && hk_parse(kp, &m, &vk)) {
        bool open = hk_ptt_step(&ptt, key_down(vk), (double)GetTickCount64(), delay);
        if (open != (s->mic_on && !s->deaf)) { s->deaf = false; s->mic_on = open; rtc_set_mic(s, open); }
    }
    if (any) cui_request_wakeup(ui, 0.03f);
}
