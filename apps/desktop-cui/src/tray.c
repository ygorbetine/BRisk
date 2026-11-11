/* Bandeja do sistema (TRY-01..04, BAS-12/13): ícone com o logo, menu nativo (Abrir / Silenciar / Reiniciar / Sair), clique abre a janela,
 * dica com o estado (em chamada, não lidas), balão nativo do Windows para os avisos (notify.c) e ícone de sobreposição na barra de tarefas.
 * O ícone usa a janela principal como destino das mensagens (WM_APP + 0x41, protocolo NOTIFYICON_VERSION_4); winstate.c repassa a mensagem. */
#define COBJMACROS
#include "sysint.h"
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>

int g_tray_balloons, g_tray_badge_ok;
static NOTIFYICONDATAW g_nid;
static bool g_on;
static bool g_menu_call, g_menu_mic;
static HICON g_base_icon, g_badged;
static int g_last_unread = -1;
static wchar_t g_tray_tip[128];
static ITaskbarList3 *g_tb;
static int g_overlay_n = -1;
static HICON g_overlay;

static void wide(const char *u8, wchar_t *w, int cap) { if (MultiByteToWideChar(CP_UTF8, 0, u8 ? u8 : "", -1, w, cap) <= 0) w[0] = 0; w[cap - 1] = 0; }

static HICON load_base(void) {
    int sz = GetSystemMetrics(SM_CXICON);
    HICON h = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, sz, sz, LR_DEFAULTCOLOR);
    return h ? h : LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
}

/* ícone da bandeja com a bolinha de contador no canto: lê os pixels do ícone base (32 bits com alfa) e compõe o contador por cima */
static HICON badged_icon(HICON base, int n, uint32_t rgb) {
    ICONINFO ii; if (!GetIconInfo(base, &ii)) return NULL;
    HICON out = NULL; BITMAP bm;
    if (ii.hbmColor && GetObject(ii.hbmColor, sizeof bm, &bm) && bm.bmWidth >= 16 && bm.bmWidth <= 256 && bm.bmWidth == bm.bmHeight) {
        int sz = bm.bmWidth; BITMAPINFO bi; memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = sz; bi.bmiHeader.biHeight = -sz; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        uint32_t *px = (uint32_t *)malloc((size_t)sz * (size_t)sz * 4);
        HDC dc = GetDC(NULL);
        if (px && GetDIBits(dc, ii.hbmColor, 0, (UINT)sz, px, &bi, DIB_RGB_COLORS) == sz) {
            bool has_alpha = false; for (int i = 0; i < sz * sz; i++) if (px[i] >> 24) { has_alpha = true; break; }
            if (has_alpha) {
                int bs = sz >= 28 ? sz * 5 / 8 : sz * 3 / 4; uint32_t *bp = (uint32_t *)malloc((size_t)bs * (size_t)bs * 4);
                if (bp) {
                    sm_badge_bgra(n, rgb, bs, bp);
                    for (int y = 0; y < bs; y++) for (int x = 0; x < bs; x++) {   /* alfa sobre o ícone, canto inferior direito */
                        uint32_t s = bp[y * bs + x], *d = &px[(sz - bs + y) * sz + (sz - bs + x)]; unsigned sa = s >> 24; if (!sa) continue;
                        unsigned da = *d >> 24, oa = sa + da * (255 - sa) / 255;
                        unsigned r = (((s >> 16) & 255) * sa + ((*d >> 16) & 255) * da * (255 - sa) / 255) / (oa ? oa : 1);
                        unsigned g = (((s >> 8) & 255) * sa + ((*d >> 8) & 255) * da * (255 - sa) / 255) / (oa ? oa : 1);
                        unsigned b = ((s & 255) * sa + (*d & 255) * da * (255 - sa) / 255) / (oa ? oa : 1);
                        *d = (oa << 24) | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
                    }
                    free(bp);
                    void *bits = NULL; BITMAPINFO bi2 = bi; bi2.bmiHeader.biHeight = -sz;
                    HBITMAP col = CreateDIBSection(dc, &bi2, DIB_RGB_COLORS, &bits, NULL, 0), mask = CreateBitmap(sz, sz, 1, 1, NULL);
                    if (col && bits && mask) {
                        memcpy(bits, px, (size_t)sz * (size_t)sz * 4);
                        ICONINFO ni = { TRUE, 0, 0, mask, col }; out = CreateIconIndirect(&ni);
                    }
                    if (col) DeleteObject(col); if (mask) DeleteObject(mask);
                }
            }
        }
        ReleaseDC(NULL, dc); free(px);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor); if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return out;
}

bool tray_present(void) { return g_on; }

bool tray_create(void *hwnd, unsigned callback_msg) {
    if (g_on) return true;
    memset(&g_nid, 0, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid; g_nid.hWnd = (HWND)hwnd; g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP; g_nid.uCallbackMessage = callback_msg;
    g_base_icon = load_base(); g_nid.hIcon = g_base_icon;
    wcscpy_s(g_tray_tip, 128, L"Briks"); wcscpy_s(g_nid.szTip, 128, g_tray_tip);
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) return false;
    g_nid.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    g_on = true; g_last_unread = 0;
    return true;
}

void tray_destroy(void) {
    if (g_on) { Shell_NotifyIconW(NIM_DELETE, &g_nid); g_on = false; }
    if (g_badged) { DestroyIcon(g_badged); g_badged = NULL; }
    if (g_overlay) { DestroyIcon(g_overlay); g_overlay = NULL; }
    if (g_tb) { ITaskbarList3_Release(g_tb); g_tb = NULL; }
}

/* explorer reiniciou (TaskbarCreated): o ícone some e precisa ser readicionado */
bool tray_readd(void) {
    if (!g_on) return false;
    g_on = false; return tray_create(g_nid.hWnd, g_nid.uCallbackMessage);
}

void tray_update(const char *tip, int unread) {
    if (!g_on) return;
    wchar_t t[128]; wide(tip, t, 128);
    bool tip_changed = wcscmp(t, g_tray_tip) != 0, badge_changed = unread != g_last_unread;
    if (!tip_changed && !badge_changed) return;
    wcscpy_s(g_tray_tip, 128, t); wcscpy_s(g_nid.szTip, 128, t);
    if (badge_changed) {
        HICON old = g_badged; g_badged = NULL;
        if (unread > 0) g_badged = badged_icon(g_base_icon, unread, (uint32_t)(((unsigned)g_th.c1.r << 16) | ((unsigned)g_th.c1.g << 8) | g_th.c1.b));
        g_nid.hIcon = g_badged ? g_badged : g_base_icon;
        if (old) DestroyIcon(old);   /* só depois de trocar no shell */
        g_last_unread = unread;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        if (old) { /* já substituído */ }
    } else Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

bool tray_balloon(const char *title, const char *text) {
    g_tray_balloons++;
    if (!g_on || sys_is_headless()) return false;   /* teste/automação: conta, mas nunca mostra aviso de verdade */
    NOTIFYICONDATAW n = g_nid; n.uFlags = NIF_INFO | NIF_SHOWTIP;
    wide(title, n.szInfoTitle, 64); wide(text, n.szInfo, 256);
    n.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | NIIF_RESPECT_QUIET_TIME; n.hBalloonIcon = g_base_icon;
    return Shell_NotifyIconW(NIM_MODIFY, &n) != 0;
}

void tray_menu_state(bool in_call, bool mic_on) { g_menu_call = in_call; g_menu_mic = mic_on; }

/* devolve o comando escolhido no menu nativo; o chamador (winstate.c) executa */
static int run_menu(HWND hwnd) {
    HMENU m = CreatePopupMenu(); if (!m) return 0;
    AppendMenuW(m, MF_STRING, TRAY_CMD_OPEN, L"Abrir o Briks");
    AppendMenuW(m, MF_STRING | (g_menu_call ? 0 : MF_GRAYED), TRAY_CMD_MUTE, g_menu_mic ? L"Silenciar microfone" : L"Ativar microfone");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, TRAY_CMD_RESTART, L"Reiniciar o Briks");
    AppendMenuW(m, MF_STRING, TRAY_CMD_QUIT, L"Sair");
    SetMenuDefaultItem(m, TRAY_CMD_OPEN, FALSE);
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(hwnd);   /* sem isso o menu não fecha ao clicar fora */
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    return cmd;
}

void tray_handle(unsigned ev, void *hwnd, int *cmd_out) {
    *cmd_out = 0;
    switch (ev) {
    case NIN_SELECT: case NIN_KEYSELECT: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: *cmd_out = TRAY_CMD_OPEN; break;
    case WM_CONTEXTMENU: case WM_RBUTTONUP: *cmd_out = run_menu((HWND)hwnd); break;
    case NIN_BALLOONUSERCLICK: *cmd_out = TRAY_CMD_OPEN; break;
    default: break;
    }
}

/* ícone de sobreposição na barra de tarefas (ITaskbarList3::SetOverlayIcon); só aparece com a janela visível na barra */
void tray_taskbar_badge(void *hwnd, int n, uint32_t rgb) {
    if (n == g_overlay_n) return;
    g_overlay_n = n;
    HICON ic = NULL;
    if (n > 0) {
        enum { S = 32 }; uint32_t px[S * S]; sm_badge_bgra(n, rgb, S, px);
        BITMAPINFO bi; memset(&bi, 0, sizeof bi); bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = S; bi.bmiHeader.biHeight = -S; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        void *bits = NULL; HDC dc = GetDC(NULL);
        HBITMAP col = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0), mask = CreateBitmap(S, S, 1, 1, NULL);
        ReleaseDC(NULL, dc);
        if (col && bits && mask) { memcpy(bits, px, sizeof px); ICONINFO ni = { TRUE, 0, 0, mask, col }; ic = CreateIconIndirect(&ni); }
        if (col) DeleteObject(col); if (mask) DeleteObject(mask);
        if (ic) g_tray_badge_ok++;
    }
    if (!sys_is_headless() && hwnd) {   /* em automação a janela não está na barra: só o ícone é montado */
        if (!g_tb) {
            CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
            if (SUCCEEDED(CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskbarList3, (void **)&g_tb)) && g_tb) ITaskbarList3_HrInit(g_tb);
            else g_tb = NULL;
        }
        if (g_tb) { wchar_t d[48]; swprintf(d, 48, n > 0 ? L"%d não lida%ls" : L"", n, n == 1 ? L"" : L"s"); ITaskbarList3_SetOverlayIcon(g_tb, (HWND)hwnd, ic, d); }
    }
    if (g_overlay) DestroyIcon(g_overlay);
    g_overlay = ic;
}
