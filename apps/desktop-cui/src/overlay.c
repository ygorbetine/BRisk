/* Overlay do Briks (segundo executável): janela sempre no topo, sem foco, com cliques passando.
 *   briks_overlay.exe [--selftest] [--stdin] [nome[:f] ...]     ("nome:f" = falando)
 * Estilo: WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW.
 * Desenho por UpdateLayeredWindow com bitmap 32 bits pré-multiplicado (cantos arredondados reais e alfa por pixel).
 * Com --stdin o app manda o conteúdo por linhas (UTF-8) e o overlay fecha sozinho se o app sumir (stdin fecha):
 *   cfg <opacidade 0-100> <canto 0-3>      canto: 0 superior direito, 1 superior esquerdo, 2 inferior direito, 3 inferior esquerdo
 *   begin                                  começa um quadro novo (apaga as linhas)
 *   p <0|1> <nome>                         pessoa na sala (1 = falando)
 *   i <texto>                              linha de informação (ex.: Sala conectada)
 *   w <texto>                              linha de aviso (ex.: Microfone mudo)
 *   end                                    mostra o quadro (sem linhas = esconde a janela)
 *   quit                                   fecha
 * Só desenha por cima de janelas e jogos em modo janela ou janela sem bordas; tela cheia exclusiva do jogo cobre qualquer sobreposição. */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdarg.h>
#include <stdarg.h>

#define MAXR 12
enum { R_IDLE, R_SPEAK, R_INFO, R_WARN };
typedef struct { wchar_t text[64]; int kind; } row_t;
typedef struct { row_t r[MAXR]; int n; int opacity, corner; } frame_t;
static frame_t cur;            /* o que está na tela */
static frame_t pend;           /* quadro em montagem pela thread de leitura */
static frame_t ready;          /* quadro completo, entregue à janela */
static CRITICAL_SECTION cs;
static HWND hwnd;
#define WM_FRAME (WM_APP + 1)

static const COLORREF PANEL = RGB(0x1a, 0x0a, 0x26), ACCENT = RGB(0x8b, 0x5c, 0xf6), TEXT = RGB(0xed, 0xf1, 0xf7), MUTED = RGB(0xa5, 0x92, 0xb8), WARN = RGB(0xe9, 0x55, 0x55);

static float rr_dist(float px, float py, float hw, float hh, float r) {
    float qx = fabsf(px) - hw + r, qy = fabsf(py) - hh + r;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float inner = (qx > qy ? qx : qy); if (inner > 0) inner = 0;
    return inner + sqrtf(ox * ox + oy * oy) - r;
}

static void add_row(frame_t *f, int kind, const wchar_t *t) { if (f->n < MAXR) { f->r[f->n].kind = kind; wcsncpy(f->r[f->n].text, t, 63); f->r[f->n].text[63] = 0; f->n++; } }

/* Uma linha de comando do protocolo. Devolve 1 quando o quadro foi concluído (end), 2 em quit. Puro: serve ao --selftest. */
static int apply_line(frame_t *build, frame_t *out, const char *ln) {
    wchar_t w[128];
    if (!strncmp(ln, "cfg ", 4)) { int o = 92, c = 0; sscanf(ln + 4, "%d %d", &o, &c); build->opacity = o < 0 ? 0 : o > 100 ? 100 : o; build->corner = c < 0 || c > 3 ? 0 : c; return 0; }
    if (!strcmp(ln, "begin")) { build->n = 0; return 0; }
    if ((ln[0] == 'p' && ln[1] == ' ' && (ln[2] == '0' || ln[2] == '1') && ln[3] == ' ') || (ln[0] == 'i' && ln[1] == ' ') || (ln[0] == 'w' && ln[1] == ' ')) {
        const char *t = ln[0] == 'p' ? ln + 4 : ln + 2;
        MultiByteToWideChar(CP_UTF8, 0, t, -1, w, 128);
        add_row(build, ln[0] == 'p' ? (ln[2] == '1' ? R_SPEAK : R_IDLE) : ln[0] == 'i' ? R_INFO : R_WARN, w);
        return 0;
    }
    if (!strcmp(ln, "end")) { *out = *build; return 1; }
    if (!strcmp(ln, "quit")) return 2;
    return 0;
}

static void paint(const frame_t *f) {
    const int rowh = 30, pad = 12, w = 260;
    if (!f->n) { ShowWindow(hwnd, SW_HIDE); return; }
    int h = pad * 2 + f->n * rowh;
    HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB } };
    unsigned *px = NULL;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(PANEL);
    RECT all = { 0, 0, w, h }; FillRect(mem, &all, bg); DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);
    HFONT font = CreateFontW(-15, 0, 0, 0, FW_MEDIUM, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    SelectObject(mem, font);
    for (int i = 0; i < f->n; i++) {
        int y = pad + i * rowh, k = f->r[i].kind;
        if (k == R_IDLE || k == R_SPEAK) {
            HBRUSH dot = CreateSolidBrush(k == R_SPEAK ? ACCENT : RGB(0x4a, 0x3a, 0x5c));
            SelectObject(mem, dot); SelectObject(mem, GetStockObject(NULL_PEN));
            Ellipse(mem, pad + 2, y + 7, pad + 18, y + 23);
            DeleteObject(dot);
        }
        SetTextColor(mem, k == R_SPEAK ? TEXT : k == R_WARN ? WARN : MUTED);
        RECT tr = { pad + (k <= R_SPEAK ? 30 : 4), y, w - pad, y + rowh };
        DrawTextW(mem, f->r[i].text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    GdiFlush();
    const float base = 0.88f * (float)f->opacity / 100.0f;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float d = rr_dist(x + 0.5f - w * 0.5f, y + 0.5f - h * 0.5f, w * 0.5f, h * 0.5f, 16.0f);
            float cov = 0.5f - d; cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
            float a = cov * base;
            unsigned c = px[y * w + x];
            unsigned r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
            px[y * w + x] = ((unsigned)(a * 255) << 24) | ((unsigned)(r * a) << 16) | ((unsigned)(g * a) << 8) | (unsigned)(b * a);
        }
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    POINT pos = { (f->corner == 1 || f->corner == 3) ? wa.left + 20 : wa.right - w - 20, (f->corner >= 2) ? wa.bottom - h - 20 : wa.top + 20 }, src = { 0, 0 };
    SIZE sz = { w, h };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hwnd, screen, &pos, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SelectObject(mem, old); DeleteObject(font); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL, screen);
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCHITTEST: return HTTRANSPARENT;          /* o clique atravessa */
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;      /* nunca rouba o foco */
    case WM_DISPLAYCHANGE: paint(&cur); return 0;
    case WM_FRAME: EnterCriticalSection(&cs); cur = ready; LeaveCriticalSection(&cs); paint(&cur); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* thread de leitura: stdin do app. Fim do pipe = o app fechou. */
static DWORD WINAPI reader(LPVOID arg) {
    (void)arg;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    char buf[2048], ln[1024]; size_t n = 0; DWORD got;
    frame_t build; memset(&build, 0, sizeof build); build.opacity = 92;
    for (;;) {
        if (!ReadFile(in, buf, sizeof buf, &got, NULL) || !got) break;
        for (DWORD i = 0; i < got; i++) {
            if (buf[i] == '\n' || n >= sizeof ln - 1) {
                ln[n] = 0; if (n && ln[n - 1] == '\r') ln[n - 1] = 0; n = 0;
                frame_t out; int r = apply_line(&build, &out, ln);
                if (r == 1) { EnterCriticalSection(&cs); ready = out; LeaveCriticalSection(&cs); PostMessageW(hwnd, WM_FRAME, 0, 0); }
                else if (r == 2) { PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
            } else ln[n++] = buf[i];
        }
    }
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return 0;
}

/* saída padrão por WriteFile: o executável é GUI, então printf não chega a um pipe redirecionado */
static void say(const char *fmt, ...) {
    char b[400]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE); DWORD w; if (o && o != INVALID_HANDLE_VALUE && n > 0) WriteFile(o, b, (DWORD)n, &w, NULL);
}
static int check(int ok, const char *what) { say("%s: %s\n", ok ? "ok" : "FALHOU", what); return ok ? 0 : 1; }

static int protocol_selftest(void) {
    frame_t b, o; memset(&b, 0, sizeof b); memset(&o, 0, sizeof o); int f = 0;
    apply_line(&b, &o, "cfg 40 3"); f += check(b.opacity == 40 && b.corner == 3, "cfg: opacidade 40 e canto 3");
    apply_line(&b, &o, "cfg 500 9"); f += check(b.opacity == 100 && b.corner == 0, "cfg fora da faixa é contido (100 e canto 0)");
    apply_line(&b, &o, "begin");
    apply_line(&b, &o, "i Sala conectada"); apply_line(&b, &o, "p 1 Marina Costa"); apply_line(&b, &o, "p 0 Rafael"); apply_line(&b, &o, "w Microfone mudo");
    f += check(apply_line(&b, &o, "end") == 1 && o.n == 4, "quadro de 4 linhas entregue no end");
    f += check(o.r[1].kind == R_SPEAK && o.r[2].kind == R_IDLE && o.r[3].kind == R_WARN && o.r[0].kind == R_INFO, "tipos: informação, falando, parado, aviso");
    f += check(!wcscmp(o.r[1].text, L"Marina Costa"), "nome com espaço inteiro");
    apply_line(&b, &o, "begin"); apply_line(&b, &o, "p 1 José Ângelo");
    apply_line(&b, &o, "end"); f += check(o.n == 1 && !wcscmp(o.r[0].text, L"José Ângelo"), "begin apaga o quadro anterior; acentos em UTF-8");
    apply_line(&b, &o, "begin"); f += check(apply_line(&b, &o, "end") == 1 && o.n == 0, "quadro vazio (esconde a janela)");
    f += check(apply_line(&b, &o, "quit") == 2 && apply_line(&b, &o, "lixo qualquer") == 0, "quit fecha; linha desconhecida é ignorada");
    for (int i = 0; i < 20; i++) apply_line(&b, &o, "p 0 x");
    f += check(b.n <= MAXR, "no máximo 12 linhas");
    return f;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev; (void)cmd; (void)show;
    InitializeCriticalSection(&cs);
    int argc; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int selftest = 0, use_stdin = 0, protocol = 0;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--selftest")) { selftest = 1; continue; }
        if (!wcscmp(argv[i], L"--protocol-test")) { protocol = 1; continue; }
        if (!wcscmp(argv[i], L"--stdin")) { use_stdin = 1; continue; }
        if (cur.n >= MAXR) break;
        wchar_t *c = wcsrchr(argv[i], L':'); int sp = 0;
        if (c && !wcscmp(c, L":f")) { *c = 0; sp = 1; }
        add_row(&cur, sp ? R_SPEAK : R_IDLE, argv[i]);
    }
    if (protocol) {   /* só o analisador de linhas, sem criar janela */
        int f = protocol_selftest(); say(f ? "\n%d falha(s)\n" : "\ntudo certo\n", f); return f ? 1 : 0;
    }
    cur.opacity = 92;
    if (!cur.n && selftest) { add_row(&cur, R_SPEAK, L"Marina Costa"); add_row(&cur, R_IDLE, L"Rafael Lima"); }
    WNDCLASSW wc = { 0 }; wc.lpfnWndProc = proc; wc.hInstance = inst; wc.lpszClassName = L"BriksOverlay"; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                           wc.lpszClassName, L"Briks overlay", WS_POPUP, 0, 0, 10, 10, NULL, NULL, inst, NULL);
    if (!hwnd) return 1;
    HWND fg_before = GetForegroundWindow();
    if (use_stdin) CreateThread(NULL, 0, reader, NULL, 0, NULL);
    else { paint(&cur); ShowWindow(hwnd, SW_SHOWNOACTIVATE); SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); }
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
        fails += protocol_selftest();
        say(fails ? "\n%d falha(s)\n" : "\ntudo certo\n", fails);
        DestroyWindow(hwnd);
        return fails ? 1 : 0;
    }
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
