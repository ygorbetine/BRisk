/* Atalhos globais de verdade: RegisterHotKey numa thread própria com fila de mensagens (a janela do app não precisa saber).
 * O sistema entrega WM_HOTKEY só para a combinação exata e a tecla NÃO chega ao programa que estiver em foco.
 * Se outro programa já usa o combo, RegisterHotKey falha e o status vira HKS_BUSY (aviso na tela, nada é registrado).
 * Botões do mouse (meio/laterais) não existem em RegisterHotKey: ficam HKS_POLLED e quem chama lê com GetAsyncKeyState. */
#include "hotkeys.h"
#include <windows.h>
#include <string.h>
#include <stdio.h>

#define WM_HK_APPLY (WM_APP + 1)
#define WM_HK_QUIT  (WM_APP + 2)
#define HK_ID_BASE  0x4B10

static HANDLE g_thread, g_ready, g_ack;
static DWORD g_tid;
static CRITICAL_SECTION g_cs; static LONG g_cs_init;
static unsigned g_mods[HK_N]; static int g_vk[HK_N];   /* pedido (sob g_cs) */
static int g_status[HK_N];                              /* resultado (sob g_cs) */
static volatile LONG g_fired[HK_N];

static void cs_once(void) { if (!InterlockedCompareExchange(&g_cs_init, 1, 0)) InitializeCriticalSection(&g_cs); }

static DWORD WINAPI hk_thread(LPVOID arg) {
    (void)arg;
    MSG m; PeekMessageW(&m, NULL, WM_USER, WM_USER, PM_NOREMOVE);   /* cria a fila de mensagens da thread */
    SetEvent(g_ready);
    bool reg[HK_N] = { false };
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_HOTKEY) {
            int i = (int)m.wParam - HK_ID_BASE;
            if (i >= 0 && i < HK_N) InterlockedIncrement(&g_fired[i]);
        } else if (m.message == WM_HK_APPLY) {
            for (int i = 0; i < HK_N; i++) if (reg[i]) { UnregisterHotKey(NULL, HK_ID_BASE + i); reg[i] = false; }
            EnterCriticalSection(&g_cs);
            for (int i = 0; i < HK_N; i++) {
                if (!g_vk[i]) { g_status[i] = HKS_NONE; continue; }
                if (hk_is_mouse(g_vk[i])) { g_status[i] = HKS_POLLED; continue; }
                UINT mo = MOD_NOREPEAT | ((g_mods[i] & HK_CTRL) ? MOD_CONTROL : 0) | ((g_mods[i] & HK_SHIFT) ? MOD_SHIFT : 0) | ((g_mods[i] & HK_ALT) ? MOD_ALT : 0);
                if (RegisterHotKey(NULL, HK_ID_BASE + i, mo, (UINT)g_vk[i])) { reg[i] = true; g_status[i] = HKS_OK; }
                else g_status[i] = HKS_BUSY;
            }
            LeaveCriticalSection(&g_cs);
            SetEvent(g_ack);
        } else if (m.message == WM_HK_QUIT) break;
    }
    for (int i = 0; i < HK_N; i++) if (reg[i]) UnregisterHotKey(NULL, HK_ID_BASE + i);
    return 0;
}

static bool ensure_thread(void) {
    cs_once();
    if (g_thread) return true;
    g_ready = CreateEventW(NULL, FALSE, FALSE, NULL); g_ack = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_thread = CreateThread(NULL, 0, hk_thread, NULL, 0, &g_tid);
    if (!g_thread) return false;
    if (WaitForSingleObject(g_ready, 2000) != WAIT_OBJECT_0) return false;
    return true;
}

void hkw_set(const char *const acc[HK_N], int status[HK_N]) {
    cs_once();
    bool any = false;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < HK_N; i++) {
        unsigned m = 0; int v = 0;
        if (acc && acc[i] && hk_parse(acc[i], &m, &v)) { g_mods[i] = m; g_vk[i] = v; any = true; } else { g_mods[i] = 0; g_vk[i] = 0; }
    }
    LeaveCriticalSection(&g_cs);
    if (!any && !g_thread) { if (status) for (int i = 0; i < HK_N; i++) status[i] = HKS_NONE; return; }
    if (ensure_thread()) {
        PostThreadMessageW(g_tid, WM_HK_APPLY, 0, 0);
        WaitForSingleObject(g_ack, 1000);
    }
    if (status) { EnterCriticalSection(&g_cs); for (int i = 0; i < HK_N; i++) status[i] = g_status[i]; LeaveCriticalSection(&g_cs); }
}

unsigned hkw_take(int i) {
    if (i < 0 || i >= HK_N) return 0;
    LONG n = InterlockedExchange(&g_fired[i], 0);
    return n > 0 ? (unsigned)n : 0;
}

void hkw_shutdown(void) {
    if (!g_thread) return;
    PostThreadMessageW(g_tid, WM_HK_QUIT, 0, 0);
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread); g_thread = NULL;
    memset(g_status, 0, sizeof g_status);
}

bool hkw_probe(unsigned mods, int vk) {
    if (hk_is_mouse(vk)) return true;
    UINT mo = MOD_NOREPEAT | ((mods & HK_CTRL) ? MOD_CONTROL : 0) | ((mods & HK_SHIFT) ? MOD_SHIFT : 0) | ((mods & HK_ALT) ? MOD_ALT : 0);
    if (!RegisterHotKey(NULL, 0x4BFF, mo, (UINT)vk)) return false;
    UnregisterHotKey(NULL, 0x4BFF);
    return true;
}

/* ---- captura de tecla ---- */
static bool cap_mouse, cap_armed; static char cap_inject[32];
static bool is_modifier(int vk) { return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || (vk >= VK_LSHIFT && vk <= VK_RMENU) || vk == VK_LWIN || vk == VK_RWIN; }
static bool down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

void hkw_capture_begin(bool allow_mouse) { cap_mouse = allow_mouse; cap_armed = false; cap_inject[0] = 0; }
void hkw_capture_inject(const char *acc) { snprintf(cap_inject, sizeof cap_inject, "%s", acc ? acc : ""); }

int hkw_capture_poll(char *out, size_t cap) {
    if (cap_inject[0]) {
        unsigned m; int v; char t[48]; bool esc = !strcmp(cap_inject, "Esc");
        bool ok = !esc && hk_parse(cap_inject, &m, &v) && hk_format(m, v, t, sizeof t) && (cap_mouse || !hk_is_mouse(v));
        cap_inject[0] = 0;
        if (esc) return 2;
        if (ok) { snprintf(out, cap, "%s", t); return 1; }
        return 0;
    }
    /* só começa a ouvir depois que tudo foi solto (o Enter/clique que abriu a captura não conta) */
    if (!cap_armed) {
        bool any = false;
        for (int v = 0x08; v < 0xFF; v++) if (!is_modifier(v) && down(v) && (v > 2 || v == 0x08)) { if (!hk_is_mouse(v) || cap_mouse) any = true; }
        if (!any) cap_armed = true;
        return 0;
    }
    int key = 0;
    for (int v = 0x08; v < 0xFF && !key; v++) {
        if (is_modifier(v) || !down(v)) continue;
        if (hk_is_mouse(v)) { if (cap_mouse) key = v; continue; }
        if (v == VK_LBUTTON || v == VK_RBUTTON) continue;
        key = v;
    }
    if (!key) return 0;
    if (key == VK_ESCAPE) { return 2; }
    unsigned m = hk_is_mouse(key) ? 0 : (down(VK_CONTROL) ? HK_CTRL : 0) | (down(VK_SHIFT) ? HK_SHIFT : 0) | (down(VK_MENU) ? HK_ALT : 0);
    char t[48];
    if (!hk_format(m, key, t, sizeof t)) return 0;   /* tecla sem nome conhecido: ignora e continua ouvindo */
    snprintf(out, cap, "%s", t);
    return 1;
}
