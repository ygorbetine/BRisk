/* Atalhos: texto <-> tecla (puro) e registro real por RegisterHotKey (curto: um combo raro, desfeito no fim). */
#include "../src/hotkeys.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FALHOU: " __VA_ARGS__); printf("\n"); } else { printf("ok: " __VA_ARGS__); printf("\n"); } } while (0)

static bool inject_ok = true;
static void key(WORD vk, bool up) { INPUT in = { 0 }; in.type = INPUT_KEYBOARD; in.ki.wVk = vk; in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0; UINT r = SendInput(1, &in, sizeof in); if (!r) inject_ok = false; Sleep(15); }

int main(void) {
    unsigned m; int vk; char t[48];
    /* formatar e ler de volta */
    const char *amostras[] = { "Ctrl+Shift+M", "Alt+F5", "Ctrl+Shift+Alt+F23", "Mouse4", "Mouse5", "Mouse3", "Espaço", "Ctrl+Left", "Shift+PageUp", "Delete", "F1", "9", "Tab", "Alt+Insert" };
    for (size_t i = 0; i < sizeof amostras / sizeof *amostras; i++) {
        bool ok = hk_parse(amostras[i], &m, &vk) && hk_format(m, vk, t, sizeof t) && !strcmp(t, amostras[i]);
        CHECK(ok, "ida e volta: %s (veio %s)", amostras[i], t);
    }
    CHECK(hk_same("ctrl + shift + m", "Shift+Ctrl+M") && !hk_same("Ctrl+M", "Ctrl+Shift+M") && !hk_same("", "") && !hk_same("xx", "xx"), "hk_same ignora caixa, espaço e ordem; vazio e inválido nunca igualam");
    CHECK(!hk_parse("Ctrl+Mouse4", &m, &vk) && hk_parse("Mouse4", &m, &vk) && hk_is_mouse(vk), "mouse não combina com modificador");
    CHECK(!hk_format(0, 0xE8, t, sizeof t), "tecla sem nome conhecido não vira texto");

    /* captura por injeção (roteiros) */
    hkw_capture_begin(false);
    hkw_capture_inject("Ctrl+Shift+K");
    CHECK(hkw_capture_poll(t, sizeof t) == 1 && !strcmp(t, "Ctrl+Shift+K"), "captura devolve a combinação");
    hkw_capture_inject("Mouse4");
    CHECK(hkw_capture_poll(t, sizeof t) == 0, "mouse recusado quando não permitido");
    hkw_capture_begin(true); hkw_capture_inject("Mouse4");
    CHECK(hkw_capture_poll(t, sizeof t) == 1 && !strcmp(t, "Mouse4"), "mouse aceito quando permitido");
    hkw_capture_inject("Esc");
    CHECK(hkw_capture_poll(t, sizeof t) == 2, "Esc cancela");

    /* registro real: Ctrl+Alt+Shift+F23 (ninguém usa), apertado de verdade e desfeito */
    const char *combo = "Ctrl+Alt+Shift+F23";
    CHECK(hk_parse(combo, &m, &vk) && hkw_probe(m, vk), "combo livre antes do registro");
    const char *set[HK_N] = { combo, "Mouse4", "" }; int st[HK_N] = { 9, 9, 9 };
    hkw_set(set, st);
    CHECK(st[0] == HKS_OK && st[1] == HKS_POLLED && st[2] == HKS_NONE, "status: registrado / lido do mouse / vazio (veio %d %d %d)", st[0], st[1], st[2]);
    CHECK(!hkw_probe(m, vk), "com o combo registrado, a sonda o vê ocupado (é assim que outro programa o veria)");
    const char *dup[HK_N] = { combo, combo, "" }; int st2[HK_N];
    hkw_set(dup, st2);
    CHECK(st2[0] == HKS_OK && st2[1] == HKS_BUSY, "o mesmo combo duas vezes: o segundo vira 'ocupado' (veio %d %d)", st2[0], st2[1]);
    hkw_set(set, st);
    hkw_take(0);
    key(VK_CONTROL, false); key(VK_MENU, false); key(VK_SHIFT, false); key(VK_F23, false); key(VK_F23, true); key(VK_SHIFT, true); key(VK_MENU, true); key(VK_CONTROL, true);
    unsigned n = 0; for (int i = 0; i < 60 && !n; i++) { Sleep(25); n = hkw_take(0); }
    if (!inject_ok) printf("pulado: o Windows negou SendInput (sessão bloqueada ou sem desktop de entrada); o registro em si foi conferido acima");
    else CHECK(n == 1, "apertar a combinação aciona o atalho uma vez (veio %u)", n);
    const char *none[HK_N] = { "", "", "" };
    hkw_set(none, st);
    CHECK(st[0] == HKS_NONE && hkw_probe(m, vk), "depois de desregistrar, o combo volta a ficar livre");
    hkw_shutdown();
    CHECK(hkw_probe(m, vk), "depois de encerrar a thread, nada ficou registrado");
    printf(fails ? "\n%d falha(s)\n" : "\ntudo certo\n", fails);
    return fails ? 1 : 0;
}
