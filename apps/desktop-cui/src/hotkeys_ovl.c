/* Atalho global do modo de edição da sobreposição (ATL-06): Ctrl+Shift+O em qualquer tela do Briks e com ele em segundo plano.
 * Como no Briks antigo, se outro programa já usa a combinação tenta Ctrl+Alt+O. RegisterHotKey numa thread própria (a tecla não chega ao programa em foco). */
#include <windows.h>
#include <stdbool.h>
#include <string.h>

#define WM_OV_SET  (WM_APP + 11)
#define WM_OV_QUIT (WM_APP + 12)
#define OV_ID 0x4B30

static HANDLE g_thread, g_ready; static DWORD g_tid;
static volatile LONG g_fired, g_status;   /* 0 sem atalho, 1 Ctrl+Shift+O, 2 Ctrl+Alt+O, 3 ocupado */

static DWORD WINAPI ov_thread(LPVOID arg) {
    (void)arg;
    MSG m; PeekMessageW(&m, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    SetEvent(g_ready);
    bool reg = false;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_HOTKEY) InterlockedIncrement(&g_fired);
        else if (m.message == WM_OV_SET) {
            if (reg) { UnregisterHotKey(NULL, OV_ID); reg = false; }
            InterlockedExchange(&g_status, 0);
            if (m.wParam) {
                if (RegisterHotKey(NULL, OV_ID, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'O')) { reg = true; InterlockedExchange(&g_status, 1); }
                else if (RegisterHotKey(NULL, OV_ID, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'O')) { reg = true; InterlockedExchange(&g_status, 2); }
                else InterlockedExchange(&g_status, 3);
            }
        } else if (m.message == WM_OV_QUIT) break;
    }
    if (reg) UnregisterHotKey(NULL, OV_ID);
    return 0;
}

void ovlkey_set(bool on) {
    if (!g_thread) {
        if (!on) return;
        g_ready = CreateEventW(NULL, FALSE, FALSE, NULL);
        g_thread = CreateThread(NULL, 0, ov_thread, NULL, 0, &g_tid);
        if (!g_thread || WaitForSingleObject(g_ready, 2000) != WAIT_OBJECT_0) return;
    }
    PostThreadMessageW(g_tid, WM_OV_SET, on ? 1 : 0, 0);
}
/* Quantas vezes o atalho foi apertado desde a última chamada. */
int ovlkey_take(void) { LONG n = InterlockedExchange(&g_fired, 0); return n > 0 ? (int)n : 0; }
int ovlkey_status(void) { return (int)g_status; }
void ovlkey_shutdown(void) { if (!g_thread) return; PostThreadMessageW(g_tid, WM_OV_QUIT, 0, 0); WaitForSingleObject(g_thread, 2000); CloseHandle(g_thread); g_thread = NULL; }
