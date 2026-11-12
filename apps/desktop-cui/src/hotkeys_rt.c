#include "hotkeys.h"
#include <string.h>
#include <stdlib.h>
#include "briks_ui.h"
#include "core.h"
#include <windows.h>

static bool key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
static bool combo_down(unsigned mods, int vk) {
    if (!key_down(vk)) return false;
    if (hk_is_mouse(vk)) return true;
    return key_down(VK_CONTROL) == ((mods & HK_CTRL) != 0) && key_down(VK_SHIFT) == ((mods & HK_SHIFT) != 0) && key_down(VK_MENU) == ((mods & HK_ALT) != 0);
}

bool g_hk_recording;   /* a tela de Atalhos está capturando uma tecla: os atalhos reais não agem */

static void act(app_state_t *s, int i) {
    if (i == 0) { s->mic_on = !s->mic_on; rtc_set_mic(s, s->mic_on); }   /* mutar não mexe no ensurdecer */
    else if (i == 1) { s->deaf = !s->deaf; rtc_set_deaf(s, s->deaf); }
    else { if (s->sh.active) share_stop(s); else share_open_dialog(s); }
}

/* Teste do microfone (Configurações > Voz): pedido ao core quando ele estiver pronto; para ao fechar a tela, trocar de aba ou entrar numa sala. */
static void mic_test_tick(app_state_t *s) {
    static bool sent;
    const bool want = s->mic_test_on && s->settings_open && s->settings_tab == 0 && !s->in_room;
    if (s->mic_test_on && !want) { s->mic_test_on = false; s->mic_test_level = 0; }
    if (want && !sent) {
        if (core_state() == CORE_STOPPED) core_start();
        if (core_state() == CORE_READY) { rtc_mic_test(s, true); sent = true; }
        else if (core_state() == CORE_FAILED) { s->mic_test_on = false; snprintf(s->mic_test_msg, sizeof s->mic_test_msg, "O Briks ainda não está pronto para ouvir o microfone. Tente de novo em instantes."); }
    } else if (!want && sent) { rtc_mic_test(s, false); sent = false; s->mic_test_level = 0; }
    if (want && sent) {   /* trocou de microfone, ganho, porta ou supressão com o teste ligado: o core reinicia o teste com os ajustes novos */
        static char sig[200]; char now[200], mic[64], outd[64]; int noff; float gi, go, gd; bool ga;
        settings_audio_ex(mic, sizeof mic, outd, sizeof outd, &noff); settings_audio(&gi, &go, &ga, &gd);
        snprintf(now, sizeof now, "%s|%d|%.2f|%d|%.0f", mic, noff, gi, ga, ga ? gd : 0.0f);
        if (strcmp(sig, now)) { if (sig[0]) rtc_mic_test(s, true); snprintf(sig, sizeof sig, "%s", now); }
    }
}

/* A cada quadro. Atalhos globais (mutar, ensurdecer, transmissão) só valem numa chamada: RegisterHotKey numa thread própria (o sistema avisa
 * mesmo com o Briks em segundo plano e a tecla não chega ao outro programa). Botões do mouse não existem em RegisterHotKey: varredura.
 * Apertar para falar é varredura da tecla (precisa seguir o soltar, com atraso). Pede acordar a cada 30 ms só quando há varredura. */
void hotkeys_tick(cui_ctx *ui, app_state_t *s) {
    mic_test_tick(s);
    settings_data_pump(s);
    overlay_tick(ui, s);
    static char applied[HK_N][24]; static bool applied_on, prev[HK_N]; static hk_ptt_t ptt;
    static int real = -1;
    if (real < 0) real = (getenv("BRIKS_HIDDEN") || getenv("BRIKS_SCRIPT") || getenv("BRIKS_DEMO")) ? 0 : 1;   /* testes ocultos: nunca registra atalho de verdade */
    const char *k[HK_N], *kp; bool ptt_on; float delay;
    settings_keys(&k[0], &k[1], &k[2], &kp, &ptt_on, &delay);
    unsigned m; int vk;

    const bool want = s->in_room;
    bool changed = want != applied_on;
    for (int i = 0; i < HK_N; i++) if (strcmp(applied[i], k[i])) changed = true;
    if (changed) {
        applied_on = want;
        for (int i = 0; i < HK_N; i++) snprintf(applied[i], sizeof applied[i], "%s", k[i]);
        const char *use[HK_N];
        for (int i = 0; i < HK_N; i++) use[i] = want ? k[i] : "";
        if (real) hkw_set(use, s->hk_status);
        else for (int i = 0; i < HK_N; i++) s->hk_status[i] = hk_parse(use[i], &m, &vk) ? (hk_is_mouse(vk) ? HKS_POLLED : HKS_OK) : HKS_NONE;
        memset(prev, 0, sizeof prev);
    }
    if (!s->in_room) { memset(&ptt, 0, sizeof ptt); return; }

    bool scan = false;
    for (int i = 0; i < HK_N; i++) {
        if (real && hkw_take(i) && !g_hk_recording && s->hk_status[i] == HKS_OK) act(s, i);   /* o sistema avisou: borda da tecla */
        if (s->hk_status[i] == HKS_POLLED && hk_parse(k[i], &m, &vk)) {
            scan = true;
            bool d = real && !g_hk_recording && combo_down(m, vk);
            if (d && !prev[i]) act(s, i);
            prev[i] = d;
        }
    }
    if (ptt_on && hk_parse(kp, &m, &vk)) {
        scan = true;
        bool open = hk_ptt_step(&ptt, real && !g_hk_recording && key_down(vk), (double)GetTickCount64(), delay);
        if (open != s->mic_on) { s->mic_on = open; rtc_set_mic(s, open); }   /* o apertar para falar não mexe no ensurdecer */
    }
    if (scan) cui_request_wakeup(ui, 0.03f);
    else cui_request_wakeup(ui, 0.1f);   /* hkw_take: lê o contador dos atalhos registrados */
}
