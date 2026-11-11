/* Overlay do Briks (segundo executável): janela sempre no topo, sem foco, com cliques passando.
 *   briks_overlay.exe [--selftest] [--stdin] [nome[:f] ...]     ("nome:f" = falando)
 * Estilo: WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW.
 * Desenho por UpdateLayeredWindow com bitmap 32 bits pré-multiplicado (cantos arredondados reais e alfa por pixel).
 * Com --stdin o app manda o conteúdo por linhas (UTF-8) e o overlay fecha sozinho se o app sumir (stdin fecha):
 *   cfg <opacidade 0-100> <canto 0-3>      canto: 0 superior direito, 1 superior esquerdo, 2 inferior direito, 3 inferior esquerdo
 *   place <monitor> <x> <y> <largura>        posição salva (x,y relativos à área útil do monitor); monitor ausente = canto do monitor principal
 *   edit <0|1>                             modo de edição: a janela aceita o mouse (arrastar o painel, canto inferior direito redimensiona, Concluir sai)
 *   begin                                  começa um quadro novo (apaga as linhas)
 *   p <0|1> <nome>                         pessoa na sala (1 = falando)
 *   i <texto>                              linha de informação (ex.: Sala conectada)
 *   w <texto>                              linha de aviso (ex.: Microfone mudo)
 *   end                                    mostra o quadro (sem linhas = esconde a janela)
 *   quit                                   fecha
 * O overlay responde pela saída padrão (uma linha por evento):  layout <monitor> <x> <y> <largura>  (soltou depois de mover/redimensionar)  |  edit 0  (clicou em Concluir)
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
typedef struct { row_t r[MAXR]; int n; int opacity, corner; int edit; char pdev[40]; int pvalid, px, py, pw; } frame_t;
#define W_DEF 260
#define W_MIN 216
#define W_MAX 640
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
    if (!strncmp(ln, "edit ", 5)) { build->edit = atoi(ln + 5) != 0; return 0; }
    if (!strncmp(ln, "place ", 6)) {
        char dev[40]; int x, y, wd;
        if (sscanf(ln + 6, "%39s %d %d %d", dev, &x, &y, &wd) == 4) { snprintf(build->pdev, sizeof build->pdev, "%s", dev); build->px = x; build->py = y; build->pw = wd < W_MIN ? W_MIN : wd > W_MAX ? W_MAX : wd; build->pvalid = 1; }
        else build->pvalid = 0;
        return 0;
    }
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

/* ---- estado do modo de edição ---- */
static int g_noshow;                       /* teste: nunca mostra a janela */
static int g_edit, g_dragging;             /* 1 = a janela aceita o mouse */
static int g_pw = W_DEF, g_ph, g_px_screen, g_py_screen;   /* painel atual: largura/altura e canto na tela */
static RECT g_btn, g_grip;                 /* botão Concluir e canto de redimensionar (px do painel) */
static char g_out[1024]; static int g_out_n; static int g_capture_out;   /* saída: stdout; em teste vai para g_out */

static void emit(const char *fmt, ...) {
    char b[300]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (n <= 0) return;
    if (g_capture_out) { if (g_out_n + n < (int)sizeof g_out) { memcpy(g_out + g_out_n, b, (size_t)n); g_out_n += n; g_out[g_out_n] = 0; } return; }
    HANDLE o = GetStdHandle(STD_OUTPUT_HANDLE); DWORD w; if (o && o != INVALID_HANDLE_VALUE) WriteFile(o, b, (DWORD)n, &w, NULL);
}

typedef struct { const char *dev; RECT work; int found; } monq_t;
static BOOL CALLBACK mon_cb(HMONITOR m, HDC dc, LPRECT r, LPARAM lp) {
    (void)dc; (void)r; monq_t *q = (monq_t *)lp; MONITORINFOEXW mi; mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(m, (MONITORINFO *)&mi)) return TRUE;
    char d[40]; WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, d, sizeof d, NULL, NULL);
    if (!strcmp(d, q->dev)) { q->work = mi.rcWork; q->found = 1; return FALSE; }
    return TRUE;
}
/* Posição do painel (canto superior esquerdo): a salva do monitor (se ele existe), senão o canto escolhido do monitor principal. */
static POINT place_pos(const frame_t *f, int w, int h) {
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    POINT pos = { (f->corner == 1 || f->corner == 3) ? wa.left + 20 : wa.right - w - 20, (f->corner >= 2) ? wa.bottom - h - 20 : wa.top + 20 };
    if (f->pvalid) {
        monq_t q = { f->pdev, { 0 }, 0 }; EnumDisplayMonitors(NULL, NULL, mon_cb, (LPARAM)&q);
        if (q.found) {
            pos.x = q.work.left + f->px; pos.y = q.work.top + f->py;
            if (pos.x > q.work.right - w) pos.x = q.work.right - w;
            if (pos.y > q.work.bottom - h) pos.y = q.work.bottom - h;
            if (pos.x < q.work.left) pos.x = q.work.left;
            if (pos.y < q.work.top) pos.y = q.work.top;
        }
    }
    return pos;
}

static void set_click_through(int through) {
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    SetWindowLongW(hwnd, GWL_EXSTYLE, through ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT));
}

static void paint(const frame_t *f) {
    frame_t tmp;
    if (f->edit && !f->n) { tmp = *f; add_row(&tmp, R_INFO, L"Pré-visualização da sobreposição"); f = &tmp; }
    if (!f->n) { if (!g_noshow) ShowWindow(hwnd, SW_HIDE); g_edit = f->edit; return; }
    const int w = g_dragging && g_pw ? g_pw : (f->pvalid && f->pw ? f->pw : W_DEF);
    const float sc = (float)w / (float)W_DEF;
    const int rowh = (int)(30 * sc + 0.5f), pad = (int)(12 * sc + 0.5f), ebar = f->edit ? (int)(34 * sc + 0.5f) + 8 : 0;
    const int h = pad * 2 + f->n * rowh + ebar;
    const int fh = (int)(15 * sc + 0.5f) < 12 ? 12 : (int)(15 * sc + 0.5f);
    g_pw = w; g_ph = h; g_edit = f->edit;
    HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB } };
    unsigned *px = NULL;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(PANEL);
    RECT all = { 0, 0, w, h }; FillRect(mem, &all, bg); DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);
    HFONT font = CreateFontW(-fh, 0, 0, 0, FW_MEDIUM, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    HGDIOBJ oldf = SelectObject(mem, font);
    const int dot = (int)(16 * sc + 0.5f);
    for (int i = 0; i < f->n; i++) {
        int y = pad + i * rowh, k = f->r[i].kind;
        if (k == R_IDLE || k == R_SPEAK) {
            HBRUSH db = CreateSolidBrush(k == R_SPEAK ? ACCENT : RGB(0x4a, 0x3a, 0x5c));
            HGDIOBJ ob = SelectObject(mem, db), op = SelectObject(mem, GetStockObject(NULL_PEN));
            Ellipse(mem, pad + 2, y + (rowh - dot) / 2, pad + 2 + dot, y + (rowh - dot) / 2 + dot);
            SelectObject(mem, ob); SelectObject(mem, op); DeleteObject(db);
        }
        SetTextColor(mem, k == R_SPEAK ? TEXT : k == R_WARN ? WARN : MUTED);
        RECT tr = { pad + (k <= R_SPEAK ? (int)(30 * sc + 0.5f) : 4), y, w - pad, y + rowh };
        DrawTextW(mem, f->r[i].text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    memset(&g_btn, 0, sizeof g_btn); memset(&g_grip, 0, sizeof g_grip);
    if (f->edit) {   /* barra de edição: dica + Concluir; canto inferior direito = redimensionar */
        const int bh = (int)(26 * sc + 0.5f) < 26 ? 26 : (int)(26 * sc + 0.5f), bw = (int)(84 * sc + 0.5f), by = h - pad / 2 - bh - 2;
        g_btn.left = w - pad - bw - (int)(18 * sc); g_btn.right = g_btn.left + bw; g_btn.top = by; g_btn.bottom = by + bh;
        g_grip.left = w - (int)(22 * sc + 0.5f); g_grip.top = h - (int)(22 * sc + 0.5f); g_grip.right = w; g_grip.bottom = h;
        HFONT sm = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
        SelectObject(mem, sm);
        SetTextColor(mem, MUTED); RECT hr = { pad, by, g_btn.left - 6, by + bh };
        DrawTextW(mem, L"Arraste para mover", -1, &hr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        HBRUSH pb = CreateSolidBrush(ACCENT); HGDIOBJ ob = SelectObject(mem, pb), op = SelectObject(mem, GetStockObject(NULL_PEN));
        RoundRect(mem, g_btn.left, g_btn.top, g_btn.right, g_btn.bottom, bh, bh);
        SelectObject(mem, ob); SelectObject(mem, op); DeleteObject(pb);
        SetTextColor(mem, RGB(0xff, 0xff, 0xff)); RECT br = g_btn;
        DrawTextW(mem, L"Concluir", -1, &br, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        HPEN gp = CreatePen(PS_SOLID, 2, MUTED); HGDIOBJ opn = SelectObject(mem, gp);
        for (int k = 1; k <= 3; k++) { MoveToEx(mem, w - 6 - k * 4 - 2, h - 6, NULL); LineTo(mem, w - 6, h - 6 - k * 4 - 2); }   /* três tracinhos diagonais */
        SelectObject(mem, opn); DeleteObject(gp); DeleteObject(sm);
    }
    SelectObject(mem, oldf);
    GdiFlush();
    const float base = f->edit ? fmaxf(0.88f * (float)f->opacity / 100.0f, 0.78f) : 0.88f * (float)f->opacity / 100.0f;
    const float rad = 16.0f * sc;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float d = rr_dist(x + 0.5f - w * 0.5f, y + 0.5f - h * 0.5f, w * 0.5f, h * 0.5f, rad);
            float cov = 0.5f - d; cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
            float a = cov * base;
            unsigned c = px[y * w + x];
            unsigned r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
            if (f->edit && d > -2.4f) {   /* contorno do modo de edição, seguindo o mesmo raio do painel */
                float t = (d < -1.4f ? (d + 2.4f) : 1.0f) * cov;
                a = a + (0.97f - a) * t; r = (unsigned)(r + (0x8b - (int)r) * t); g = (unsigned)(g + (0x5c - (int)g) * t); b = (unsigned)(b + (0xf6 - (int)b) * t);
            }
            px[y * w + x] = ((unsigned)(a * 255) << 24) | ((unsigned)(r * a) << 16) | ((unsigned)(g * a) << 8) | (unsigned)(b * a);
        }
    POINT pos = g_dragging ? (POINT){ g_px_screen, g_py_screen } : place_pos(f, w, h), src = { 0, 0 };
    g_px_screen = pos.x; g_py_screen = pos.y;
    SIZE sz = { w, h };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    set_click_through(!f->edit);
    UpdateLayeredWindow(hwnd, screen, &pos, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    if (!g_noshow) {
        if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    SelectObject(mem, old); DeleteObject(font); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL, screen);
}

/* ---- arrastar e redimensionar (modo de edição). O cursor vem de cursor_pos(): em teste é injetado. ---- */
static int g_inject; static POINT g_inj;
static POINT cursor_pos(void) { if (g_inject) return g_inj; POINT p; GetCursorPos(&p); return p; }
static int g_mode;                          /* 0 nada, 1 mover, 2 redimensionar */
static POINT g_c0; static int g_x0, g_y0, g_w0;

static int hit_kind(int x, int y) {         /* 0 nada, 1 mover, 2 redimensionar, 3 Concluir (x, y em px do painel) */
    if (x < 0 || y < 0 || x >= g_pw || y >= g_ph) return 0;
    if (g_btn.right && x >= g_btn.left && x < g_btn.right && y >= g_btn.top && y < g_btn.bottom) return 3;
    if (g_grip.right && x >= g_grip.left && y >= g_grip.top) return 2;
    return 1;
}
static void send_layout(void) {
    HMONITOR m = MonitorFromPoint((POINT){ g_px_screen + g_pw / 2, g_py_screen + g_ph / 2 }, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi; mi.cbSize = sizeof mi; if (!GetMonitorInfoW(m, (MONITORINFO *)&mi)) return;
    char d[40]; WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, d, sizeof d, NULL, NULL);
    emit("layout %s %d %d %d\n", d, g_px_screen - (int)mi.rcWork.left, g_py_screen - (int)mi.rcWork.top, g_pw);
}
static void drag_begin(int x, int y) {
    if (!g_edit) return;
    int k = hit_kind(x, y);
    if (k == 3) { emit("edit 0\n"); return; }
    if (k == 0) return;
    g_mode = k; g_dragging = 1; g_c0 = cursor_pos(); g_x0 = g_px_screen; g_y0 = g_py_screen; g_w0 = g_pw;
    if (!g_inject) SetCapture(hwnd);
}
static void drag_move(void) {
    if (!g_dragging) return;
    POINT c = cursor_pos();
    if (g_mode == 1) {
        g_px_screen = g_x0 + (c.x - g_c0.x); g_py_screen = g_y0 + (c.y - g_c0.y);
        POINT p = { g_px_screen, g_py_screen };
        UpdateLayeredWindow(hwnd, NULL, &p, NULL, NULL, NULL, 0, NULL, 0);   /* só move: sem redesenhar */
    } else {
        int w = g_w0 + (c.x - g_c0.x); w = w < W_MIN ? W_MIN : w > W_MAX ? W_MAX : w;
        if (w != g_pw) { g_pw = w; paint(&cur); }
    }
}
static void drag_end(void) {
    if (!g_dragging) return;
    g_dragging = 0; g_mode = 0;
    if (!g_inject) ReleaseCapture();
    send_layout();
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCHITTEST: return g_edit ? HTCLIENT : HTTRANSPARENT;   /* fora da edição o clique atravessa */
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;      /* nunca rouba o foco */
    case WM_SETCURSOR:
        if (g_edit) { POINT p; GetCursorPos(&p); ScreenToClient(h, &p); int k = hit_kind(p.x, p.y); SetCursor(LoadCursor(NULL, g_dragging ? (g_mode == 2 ? IDC_SIZENWSE : IDC_SIZEALL) : k == 2 ? IDC_SIZENWSE : k == 3 ? IDC_HAND : IDC_SIZEALL)); return TRUE; }
        break;
    case WM_LBUTTONDOWN: drag_begin((short)LOWORD(l), (short)HIWORD(l)); return 0;
    case WM_MOUSEMOVE: drag_move(); return 0;
    case WM_LBUTTONUP: case WM_CAPTURECHANGED: drag_end(); return 0;
    case WM_DISPLAYCHANGE: paint(&cur); return 0;
    case WM_FRAME: EnterCriticalSection(&cs); cur = ready; LeaveCriticalSection(&cs); if (!g_dragging) paint(&cur); return 0;
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
    apply_line(&b, &o, "edit 1"); apply_line(&b, &o, "place \\\\.\\DISPLAY1 120 80 9999"); apply_line(&b, &o, "end");
    f += check(o.edit == 1 && o.pvalid && o.px == 120 && o.py == 80 && o.pw == W_MAX && !strcmp(o.pdev, "\\\\.\\DISPLAY1"), "edit e place entram no quadro (largura contida em 640)");
    apply_line(&b, &o, "place x"); f += check(!b.pvalid, "place inválido apaga a posição");
    f += check(apply_line(&b, &o, "quit") == 2 && apply_line(&b, &o, "lixo qualquer") == 0, "quit fecha; linha desconhecida é ignorada");
    for (int i = 0; i < 20; i++) apply_line(&b, &o, "p 0 x");
    f += check(b.n <= MAXR, "no máximo 12 linhas");
    return f;
}

/* Teste do modo de edição, sem mostrar janela e com o cursor injetado: arrastar, redimensionar, Concluir, por monitor. */
static int edit_selftest(void) {
    int f = 0; g_noshow = 1; g_capture_out = 1; g_inject = 1;
    HMONITOR pm = MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY); MONITORINFOEXW mi; mi.cbSize = sizeof mi; GetMonitorInfoW(pm, (MONITORINFO *)&mi);
    char dev[40]; WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, dev, sizeof dev, NULL, NULL);
    frame_t b, o; memset(&b, 0, sizeof b); memset(&o, 0, sizeof o); char ln[160];
    apply_line(&b, &o, "cfg 90 0"); apply_line(&b, &o, "edit 1"); apply_line(&b, &o, "begin"); apply_line(&b, &o, "p 1 Marina"); apply_line(&b, &o, "p 0 Rafael");
    snprintf(ln, sizeof ln, "place %s 100 80 300", dev); apply_line(&b, &o, ln);
    apply_line(&b, &o, "end"); cur = o; paint(&cur);
    RECT r; GetWindowRect(hwnd, &r);
    f += check(g_edit == 1 && g_pw == 300, "modo de edição ligado e largura salva (300) aplicada");
    f += check(r.left == mi.rcWork.left + 100 && r.top == mi.rcWork.top + 80, "posição salva relativa à área útil do monitor");
    f += check(!(GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) && SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(r.left + 20, r.top + 20)) == HTCLIENT, "em edição o mouse é recebido (sem WS_EX_TRANSPARENT)");
    f += check((GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0, "continua sem ativar (não rouba o foco do jogo)");
    f += check(hit_kind(10, 10) == 1 && hit_kind(g_pw - 4, g_ph - 4) == 2 && hit_kind((int)((g_btn.left + g_btn.right) / 2), (int)((g_btn.top + g_btn.bottom) / 2)) == 3 && hit_kind(-3, 5) == 0, "acerto: corpo move, canto redimensiona, Concluir sai, fora nada");
    g_inj = (POINT){ 500, 500 }; drag_begin(10, 10); g_inj = (POINT){ 560, 530 }; drag_move(); GetWindowRect(hwnd, &r);
    f += check(r.left == mi.rcWork.left + 160 && r.top == mi.rcWork.top + 110, "arrastar move a janela junto com o cursor");
    drag_end();
    snprintf(ln, sizeof ln, "layout %s 160 110 300\n", dev);
    f += check(strstr(g_out, ln) != NULL, "ao soltar manda 'layout <monitor> x y largura'");
    g_out_n = 0; g_out[0] = 0;
    g_inj = (POINT){ 700, 700 }; drag_begin(g_pw - 6, g_ph - 6); g_inj = (POINT){ 780, 700 }; drag_move(); drag_end();
    snprintf(ln, sizeof ln, "layout %s 160 110 380\n", dev);
    f += check(g_pw == 380 && strstr(g_out, ln) != NULL, "o canto inferior direito redimensiona (300 -> 380)");
    g_out_n = 0; g_out[0] = 0;
    g_inj = (POINT){ 900, 900 }; drag_begin(g_pw - 6, g_ph - 6); g_inj = (POINT){ -2000, 900 }; drag_move(); drag_end();
    f += check(g_pw == W_MIN, "largura mínima 216 (texto nunca abaixo de 12 px)");
    snprintf(ln, sizeof ln, "layout %s 160 110 %d\n", dev, W_MIN); f += check(strstr(g_out, ln) != NULL, "a largura mínima também é informada");
    g_out_n = 0; g_out[0] = 0;
    drag_begin((g_btn.left + g_btn.right) / 2, (g_btn.top + g_btn.bottom) / 2);
    f += check(strstr(g_out, "edit 0\n") != NULL && !g_dragging, "Concluir manda 'edit 0'");
    apply_line(&b, &o, "edit 0"); apply_line(&b, &o, "end"); cur = o; paint(&cur);
    f += check(g_edit == 0 && (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) && SendMessageW(hwnd, WM_NCHITTEST, 0, 0) == HTTRANSPARENT, "fora da edição o clique volta a atravessar");
    drag_begin(10, 10); f += check(!g_dragging, "fora da edição o arraste é ignorado");
    apply_line(&b, &o, "place \\\\.\\DISPLAY99 50 50 260"); apply_line(&b, &o, "end"); cur = o; paint(&cur);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0); GetWindowRect(hwnd, &r);
    f += check(r.right == wa.right - 20 && r.top == wa.top + 20, "monitor salvo que sumiu: volta ao canto escolhido do principal");
    snprintf(ln, sizeof ln, "place %s 99999 99999 260", dev); apply_line(&b, &o, ln); apply_line(&b, &o, "end"); cur = o; paint(&cur); GetWindowRect(hwnd, &r);
    f += check(r.right <= mi.rcWork.right && r.bottom <= mi.rcWork.bottom, "posição salva fora da tela é trazida para dentro");
    return f;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev; (void)cmd; (void)show;
    InitializeCriticalSection(&cs);
    int argc; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int selftest = 0, use_stdin = 0, protocol = 0, edittest = 0;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--selftest")) { selftest = 1; continue; }
        if (!wcscmp(argv[i], L"--protocol-test")) { protocol = 1; continue; }
        if (!wcscmp(argv[i], L"--stdin")) { use_stdin = 1; continue; }
        if (!wcscmp(argv[i], L"--edit-test")) { edittest = 1; continue; }
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
    if (edittest) { int fl = edit_selftest(); say(fl ? "\n%d falha(s)\n" : "\ntudo certo\n", fl); DestroyWindow(hwnd); return fl ? 1 : 0; }
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
