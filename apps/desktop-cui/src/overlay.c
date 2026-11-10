/* Overlay do Briks (segundo executável): janela sempre no topo, sem foco, com cliques passando.
 *   briks_overlay.exe [--selftest] [nome[:f] ...]     ("nome:f" = falando)
 * Estilo: WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW.
 * Desenho por UpdateLayeredWindow com bitmap 32 bits pré-multiplicado (cantos arredondados reais e alfa por pixel).
 * Substitui o spike que foi sobrescrito; o conteúdo real virá do briks-core (quem está falando). */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define MAXP 8
typedef struct { wchar_t name[48]; int speaking; } person_t;
static person_t people[MAXP];
static int npeople;
static HWND hwnd;

static const COLORREF PANEL = RGB(0x1a, 0x0a, 0x26), ACCENT = RGB(0x8b, 0x5c, 0xf6), TEXT = RGB(0xed, 0xf1, 0xf7);

static float rr_dist(float px, float py, float hw, float hh, float r) {
    float qx = fabsf(px) - hw + r, qy = fabsf(py) - hh + r;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float inner = (qx > qy ? qx : qy); if (inner > 0) inner = 0;
    return inner + sqrtf(ox * ox + oy * oy) - r;
}

static void paint(void) {
    const int rowh = 34, pad = 12, w = 240;
    int h = pad * 2 + (npeople ? npeople : 1) * rowh;
    HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB } };
    unsigned *px = NULL;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    /* fundo: GDI pinta o conteúdo (alfa fica 0); depois o alfa é gerado pela máscara do retângulo arredondado */
    HBRUSH bg = CreateSolidBrush(PANEL);
    RECT all = { 0, 0, w, h }; FillRect(mem, &all, bg); DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);
    HFONT font = CreateFontW(-15, 0, 0, 0, FW_MEDIUM, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    SelectObject(mem, font);
    for (int i = 0; i < npeople; i++) {
        int y = pad + i * rowh;
        HBRUSH dot = CreateSolidBrush(people[i].speaking ? ACCENT : RGB(0x4a, 0x3a, 0x5c));
        SelectObject(mem, dot);
        SelectObject(mem, GetStockObject(NULL_PEN));
        Ellipse(mem, pad + 2, y + 9, pad + 18, y + 25);
        DeleteObject(dot);
        SetTextColor(mem, people[i].speaking ? TEXT : RGB(0xa5, 0x92, 0xb8));
        RECT tr = { pad + 30, y, w - pad, y + rowh };
        DrawTextW(mem, people[i].name, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    if (!npeople) { SetTextColor(mem, RGB(0xa5, 0x92, 0xb8)); RECT tr = { pad, pad, w - pad, pad + rowh }; DrawTextW(mem, L"Briks — ninguém na chamada", -1, &tr, DT_SINGLELINE | DT_VCENTER); }
    GdiFlush();
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float d = rr_dist(x + 0.5f - w * 0.5f, y + 0.5f - h * 0.5f, w * 0.5f, h * 0.5f, 16.0f);
            float cov = 0.5f - d; cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
            float a = cov * 0.88f;
            unsigned c = px[y * w + x];
            unsigned r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
            px[y * w + x] = ((unsigned)(a * 255) << 24) | ((unsigned)(r * a) << 16) | ((unsigned)(g * a) << 8) | (unsigned)(b * a);
        }
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    POINT pos = { wa.right - w - 20, wa.top + 20 }, src = { 0, 0 };
    SIZE sz = { w, h };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hwnd, screen, &pos, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old); DeleteObject(font); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL, screen);
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCHITTEST: return HTTRANSPARENT;          /* o clique atravessa */
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;      /* nunca rouba o foco */
    case WM_DISPLAYCHANGE: paint(); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static int check(int ok, const char *what) { printf("%s: %s\n", ok ? "ok" : "FALHOU", what); return ok ? 0 : 1; }

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev; (void)cmd; (void)show;
    int argc; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int selftest = 0;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--selftest")) { selftest = 1; continue; }
        if (npeople >= MAXP) break;
        person_t *p = &people[npeople++];
        wcsncpy(p->name, argv[i], 47);
        wchar_t *c = wcsrchr(p->name, L':');
        if (c && !wcscmp(c, L":f")) { *c = 0; p->speaking = 1; }
    }
    if (!npeople && selftest) { wcscpy(people[0].name, L"Marina Costa"); people[0].speaking = 1; wcscpy(people[1].name, L"Rafael Lima"); npeople = 2; }
    WNDCLASSW wc = { 0 }; wc.lpfnWndProc = proc; wc.hInstance = inst; wc.lpszClassName = L"BriksOverlay"; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                           wc.lpszClassName, L"Briks overlay", WS_POPUP, 0, 0, 10, 10, NULL, NULL, inst, NULL);
    if (!hwnd) return 1;
    HWND fg_before = GetForegroundWindow();
    paint();
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (selftest) {
        int fails = 0;
        LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
        fails += check((ex & WS_EX_TOPMOST) != 0, "sempre no topo (WS_EX_TOPMOST)");
        fails += check((ex & WS_EX_TRANSPARENT) && (ex & WS_EX_LAYERED), "clique atravessa (WS_EX_TRANSPARENT + LAYERED)");
        fails += check((ex & WS_EX_NOACTIVATE) != 0, "não ativa (WS_EX_NOACTIVATE)");
        fails += check(!(ex & WS_EX_APPWINDOW) && (ex & WS_EX_TOOLWINDOW), "fora da barra de tarefas (TOOLWINDOW)");
        fails += check(GetForegroundWindow() == fg_before, "o foco do usuário não mudou");
        fails += check(IsWindowVisible(hwnd) != 0, "visível");
        RECT r; GetWindowRect(hwnd, &r);
        POINT pt = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
        fails += check(SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(pt.x, pt.y)) == HTTRANSPARENT, "WM_NCHITTEST devolve HTTRANSPARENT");
        printf(fails ? "\n%d falha(s)\n" : "\ntudo certo\n", fails);
        DestroyWindow(hwnd);
        return fails ? 1 : 0;
    }
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
