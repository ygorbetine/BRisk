/* Overlay do Briks (segundo executável): janela sempre no topo, sem foco, com cliques passando.
 *   briks_overlay.exe [--selftest] [--stdin] [nome[:f] ...]     ("nome:f" = falando)
 * Estilo: WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW.
 * Desenho por UpdateLayeredWindow com bitmap 32 bits pré-multiplicado (cantos arredondados reais e alfa por pixel).
 * Com --stdin o app manda o conteúdo por linhas (UTF-8) e o overlay fecha sozinho se o app sumir (stdin fecha):
 *   cfg <opacidade 0-100> <canto 0-3>      canto: 0 superior direito, 1 superior esquerdo, 2 inferior direito, 3 inferior esquerdo
 *   place <monitor> <x> <y> <largura>        posição salva (x,y relativos à área útil do monitor); monitor ausente = canto do monitor principal
 *   edit <0|1>                             modo de edição: o painel aceita o mouse (arrastar, canto inferior direito ou roda do mouse redimensionam) e aparece a barra de edição (2ª janela, embaixo da tela)
 *   flags <0-15>                           itens ligados (1 transmissão, 2 quem está na sala, 4 sala conectada, 8 aviso de mudo): estado dos botões da barra
 *   lives <n>                              quantas transmissões ao vivo há na sala (n > 1 mostra "Trocar transmissão")
 *   begin                                  começa um quadro novo (apaga as linhas)
 *   p <0|1> <nome>                         pessoa na sala (1 = falando)
 *   i <texto>                              linha de informação (ex.: Sala conectada)
 *   w <texto>                              linha de aviso (ex.: Microfone mudo)
 *   end                                    mostra o quadro (sem linhas = esconde a janela)
 *   quit                                   fecha
 * O overlay responde pela saída padrão (uma linha por evento):  layout <monitor> <x> <y> <largura>  (soltou depois de mover/redimensionar)  |  edit 0  (Concluir ou Esc)
 *   widget <live|voice|status|mute|all> <0|1>  (botão da barra)  |  opacity <10-100>  (opacidade da barra)  |  selectlive  (Trocar transmissão)
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
typedef struct { row_t r[MAXR]; int n; int opacity, corner; int edit, flags, lives; char pdev[40]; int pvalid, px, py, pw; } frame_t;
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
    if (!strncmp(ln, "flags ", 6)) { int v = atoi(ln + 6); build->flags = v < 0 ? 0 : v > 15 ? 15 : v; return 0; }
    if (!strncmp(ln, "lives ", 6)) { int v = atoi(ln + 6); build->lives = v < 0 ? 0 : v > 99 ? 99 : v; return 0; }
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
static RECT g_grip;                        /* canto de redimensionar (px do painel) */
static void paint_bar(const frame_t *f);   /* barra de edição (2ª janela) */
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

/* cópia dos últimos quadros desenhados (0 painel, 1 barra) para a captura --shot */
static struct { unsigned *px; int w, h, x, y; } g_snap[2]; static int g_snap_on;
static void snap_keep(int i, const unsigned *px, int w, int h, int x, int y) {
    if (!g_snap_on) return;
    free(g_snap[i].px); g_snap[i].px = (unsigned *)malloc((size_t)w * h * 4); if (!g_snap[i].px) return;
    memcpy(g_snap[i].px, px, (size_t)w * h * 4); g_snap[i].w = w; g_snap[i].h = h; g_snap[i].x = x; g_snap[i].y = y;
}

static void paint(const frame_t *f) {
    frame_t tmp;
    if (f->edit && !f->n) { tmp = *f; add_row(&tmp, R_INFO, L"Pré-visualização da sobreposição"); f = &tmp; }
    if (!f->n) { if (!g_noshow) ShowWindow(hwnd, SW_HIDE); g_edit = f->edit; paint_bar(f); return; }
    const int w = g_dragging && g_pw ? g_pw : (f->pvalid && f->pw ? f->pw : W_DEF);
    const float sc = (float)w / (float)W_DEF;
    const int rowh = (int)(30 * sc + 0.5f), pad = (int)(12 * sc + 0.5f), ebar = 0;
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
    memset(&g_grip, 0, sizeof g_grip);
    if (f->edit) {   /* canto inferior direito = redimensionar (a barra de ferramentas é outra janela, embaixo da tela) */
        g_grip.left = w - (int)(22 * sc + 0.5f); g_grip.top = h - (int)(22 * sc + 0.5f); g_grip.right = w; g_grip.bottom = h;
        HPEN gp = CreatePen(PS_SOLID, 2, MUTED); HGDIOBJ opn = SelectObject(mem, gp);
        for (int k = 1; k <= 3; k++) { MoveToEx(mem, w - 6 - k * 4 - 2, h - 6, NULL); LineTo(mem, w - 6, h - 6 - k * 4 - 2); }   /* três tracinhos diagonais */
        SelectObject(mem, opn); DeleteObject(gp);
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
    snap_keep(0, px, w, h, pos.x, pos.y);
    SelectObject(mem, old); DeleteObject(font); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL, screen);
    paint_bar(f);
}

/* ---- arrastar e redimensionar (modo de edição). O cursor vem de cursor_pos(): em teste é injetado. ---- */
static int g_inject; static POINT g_inj;
static POINT cursor_pos(void) { if (g_inject) return g_inj; POINT p; GetCursorPos(&p); return p; }
/* ---- barra de edição: 2ª janela (embaixo da tela, no monitor do painel), vidro escuro de cantos arredondados ---- */
enum { B_NONE, B_ALL, B_W0, B_W1, B_W2, B_W3, B_OPM, B_OPP, B_SLIDER, B_LIVE, B_DONE, B_N };
#define BAR_MAXW 680
#define BAR_MINW 560
#define HINT_H 26
#define BAR_PAD 10
#define BTN_H 32
#define BAR_GAP 8
#define BAR_H (BAR_PAD * 2 + BTN_H * 2 + BAR_GAP)
#define BAR_TOTAL (HINT_H + 8 + BAR_H)
#define OP_MIN 10
static HWND hbar;
static RECT g_bh[B_N];                         /* acertos, px da janela da barra (vazio = não existe) */
static RECT g_barshape, g_hintshape, g_oplab;   /* formas arredondadas e rótulo da opacidade */
static int g_bww, g_bar_on, g_bhover, g_bar_drag, g_esc_reg;
static DWORD g_hold;                            /* até lá, o que o usuário acabou de mudar vale mais que um quadro velho vindo do app */
static const struct { const wchar_t *label; const char *id; int bit; } WG[4] = { { L"Transmissão", "live", 1 }, { L"Quem está na sala", "voice", 2 }, { L"Sala conectada", "status", 4 }, { L"Aviso de mudo", "mute", 8 } };
static const wchar_t *HINT = L"Arraste para mover · roda do mouse ou o canto mudam o tamanho · Esc ou Ctrl+Shift+O conclui";

static int rc_in(const RECT *r, int x, int y) { return r->right > r->left && x >= r->left && x < r->right && y >= r->top && y < r->bottom; }

/* Geometria pura da barra para a largura ww (e "Trocar transmissão" só com mais de uma live). Linha 1: tudo/itens; linha 2: opacidade, trocar, concluir. */
static void bar_layout(int ww, int lives) {
    memset(g_bh, 0, sizeof g_bh);
    g_hintshape = (RECT){ 0, 0, 0, HINT_H };
    g_barshape = (RECT){ 0, HINT_H + 8, ww, HINT_H + 8 + BAR_H };
    const int x0 = BAR_PAD, x1 = ww - BAR_PAD, iw = x1 - x0, y1 = g_barshape.top + BAR_PAD, y2 = y1 + BTN_H + BAR_GAP;
    static const int nat[5] = { 112, 100, 132, 120, 108 };   /* "Desativar tudo" e os 4 itens: larguras naturais, esticadas/encolhidas para encher a linha */
    int sum = 0; for (int i = 0; i < 5; i++) sum += nat[i];
    const int avail = iw - 4 * BAR_GAP; int x = x0, used = 0;
    for (int i = 0; i < 5; i++) {
        int wdt = i == 4 ? avail - used : avail * nat[i] / sum; used += wdt;
        g_bh[i == 0 ? B_ALL : B_W0 + i - 1] = (RECT){ x, y1, x + wdt, y1 + BTN_H }; x += wdt + BAR_GAP;
    }
    const int donew = 100, livew = 150;
    g_bh[B_DONE] = (RECT){ x1 - donew, y2, x1, y2 + BTN_H };
    int right = g_bh[B_DONE].left - 16;
    if (lives > 1) { g_bh[B_LIVE] = (RECT){ right - livew, y2, right, y2 + BTN_H }; right -= livew + 16; }
    g_oplab = (RECT){ x0, y2, x0 + 112, y2 + BTN_H };
    g_bh[B_OPM] = (RECT){ g_oplab.right, y2, g_oplab.right + BTN_H, y2 + BTN_H };
    g_bh[B_OPP] = (RECT){ right - BTN_H, y2, right, y2 + BTN_H };
    g_bh[B_SLIDER] = (RECT){ g_bh[B_OPM].right + BAR_GAP, y2, g_bh[B_OPP].left - BAR_GAP, y2 + BTN_H };
    if (g_bh[B_SLIDER].right - g_bh[B_SLIDER].left < 48) g_bh[B_SLIDER].right = g_bh[B_SLIDER].left + 48;
}
static int bar_hit(int x, int y) { for (int i = B_ALL; i < B_N; i++) if (rc_in(&g_bh[i], x, y)) return i; return B_NONE; }
static int slider_value(int x) {
    const RECT *r = &g_bh[B_SLIDER]; int span = r->right - r->left - 18; if (span < 1) span = 1;
    int v = OP_MIN + (x - (r->left + 9)) * (100 - OP_MIN) / span; return v < OP_MIN ? OP_MIN : v > 100 ? 100 : v;
}

/* pílulas e anéis suavizados direto no quadro opaco (antes da máscara de alfa): fill (ring = 0) ou contorno de espessura ring */
static void px_rr(unsigned *px, int w, int h, RECT r, float rad, COLORREF c, float a, float ring) {
    const float hw = (r.right - r.left) * 0.5f, hh = (r.bottom - r.top) * 0.5f, cx = r.left + hw, cy = r.top + hh;
    for (int y = r.top - 1; y <= r.bottom; y++) for (int x = r.left - 1; x <= r.right; x++) {
        if (x < 0 || y < 0 || x >= w || y >= h) continue;
        float d = rr_dist(x + 0.5f - cx, y + 0.5f - cy, hw, hh, rad);
        float cov = 0.5f - d; cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
        if (ring > 0) { float ci = 0.5f - (d + ring); ci = ci < 0 ? 0 : ci > 1 ? 1 : ci; cov -= ci; }
        float t = cov * a; if (t <= 0) continue;
        unsigned o = px[y * w + x]; int R = (o >> 16) & 255, G = (o >> 8) & 255, B = o & 255;
        R += (int)((GetRValue(c) - R) * t + 0.5f); G += (int)((GetGValue(c) - G) * t + 0.5f); B += (int)((GetBValue(c) - B) * t + 0.5f);
        px[y * w + x] = ((unsigned)R << 16) | ((unsigned)G << 8) | (unsigned)B;
    }
}
static void bar_text(HDC dc, const wchar_t *t, RECT r, COLORREF c, UINT fl) { SetTextColor(dc, c); DrawTextW(dc, t, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | fl); }
static HFONT bar_font(int px, int weight) { return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI"); }

static RECT bar_workarea(void) {
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    HMONITOR m = MonitorFromPoint((POINT){ g_px_screen + g_pw / 2, g_py_screen + g_ph / 2 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi; mi.cbSize = sizeof mi; if (GetMonitorInfoW(m, &mi)) wa = mi.rcWork;
    return wa;
}
static void esc_register(int on) {
    if (g_noshow || !hbar || on == g_esc_reg) return;
    if (on) g_esc_reg = RegisterHotKey(hbar, 1, MOD_NOREPEAT, VK_ESCAPE) ? 1 : 0; else { UnregisterHotKey(hbar, 1); g_esc_reg = 0; }
}

static void paint_bar(const frame_t *f) {
    if (!hbar) return;
    if (!f->edit) { g_bar_on = 0; g_bar_drag = 0; memset(g_bh, 0, sizeof g_bh); esc_register(0); if (!g_noshow) ShowWindow(hbar, SW_HIDE); return; }
    const RECT wa = bar_workarea();
    int ww = wa.right - wa.left - 24; if (ww > BAR_MAXW) ww = BAR_MAXW; if (ww < BAR_MINW) ww = BAR_MINW;
    const int hh = BAR_TOTAL; g_bww = ww; g_bar_on = 1;
    bar_layout(ww, f->lives);
    HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), ww, -hh, 1, 32, BI_RGB } };
    unsigned *px = NULL; HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(RGB(0x16, 0x0a, 0x22)); RECT all = { 0, 0, ww, hh }; FillRect(mem, &all, bg); DeleteObject(bg);
    GdiFlush();
    const COLORREF WHITE = RGB(255, 255, 255), PURP = RGB(0x7c, 0x3a, 0xed);
    {   /* a pílula da dica se ajusta ao texto */
        HFONT hf = bar_font(12, FW_NORMAL); HGDIOBJ oh = SelectObject(mem, hf); RECT tr = { 0, 0, 0, 0 }; DrawTextW(mem, HINT, -1, &tr, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        int tw = tr.right + 28; if (tw > ww) tw = ww; g_hintshape.left = (ww - tw) / 2; g_hintshape.right = g_hintshape.left + tw;
        SelectObject(mem, oh); DeleteObject(hf);
    }
    px_rr(px, ww, hh, g_barshape, 14, RGB(0x0c, 0x04, 0x14), 0.45f, 0); px_rr(px, ww, hh, g_barshape, 14, WHITE, 0.16f, 1);
    px_rr(px, ww, hh, g_hintshape, 13, WHITE, 0.14f, 1);
    const int any = (f->flags & 15) != 0;
    for (int i = B_ALL; i < B_N; i++) {
        const RECT r = g_bh[i]; if (r.right <= r.left) continue;
        const int hov = g_bhover == i;
        if (i == B_ALL) { px_rr(px, ww, hh, r, BTN_H / 2, any ? WHITE : PURP, any ? (hov ? 0.18f : 0.08f) : (hov ? 1.0f : 0.92f), 0); if (any) px_rr(px, ww, hh, r, BTN_H / 2, WHITE, 0.35f, 1); }
        else if (i >= B_W0 && i <= B_W3) {
            const int on = (f->flags & WG[i - B_W0].bit) != 0;
            if (on) px_rr(px, ww, hh, r, BTN_H / 2, PURP, hov ? 1.0f : 0.92f, 0); else { px_rr(px, ww, hh, r, BTN_H / 2, WHITE, hov ? 0.16f : 0.05f, 0); px_rr(px, ww, hh, r, BTN_H / 2, WHITE, 0.28f, 1); }
            RECT dot = { r.left + 11, (r.top + r.bottom) / 2 - 4, r.left + 19, (r.top + r.bottom) / 2 + 4 };
            if (on) px_rr(px, ww, hh, dot, 4, WHITE, 1.0f, 0); else px_rr(px, ww, hh, dot, 4, WHITE, 0.6f, 1.5f);
        }
        else if (i == B_OPM || i == B_OPP) { px_rr(px, ww, hh, r, BTN_H / 2, WHITE, hov ? 0.18f : 0.08f, 0); px_rr(px, ww, hh, r, BTN_H / 2, WHITE, 0.28f, 1); }
        else if (i == B_SLIDER) {
            const int v = f->opacity < OP_MIN ? OP_MIN : f->opacity > 100 ? 100 : f->opacity, cy = (r.top + r.bottom) / 2;
            RECT tr = { r.left + 9, cy - 3, r.right - 9, cy + 3 }; px_rr(px, ww, hh, tr, 3, WHITE, 0.25f, 0);
            const int tx = tr.left + (tr.right - tr.left) * (v - OP_MIN) / (100 - OP_MIN);
            RECT fr = { tr.left, tr.top, tx, tr.bottom }; if (fr.right > fr.left) px_rr(px, ww, hh, fr, 3, RGB(0xa7, 0x8b, 0xfa), 1.0f, 0);
            RECT th = { tx - 9, cy - 9, tx + 9, cy + 9 }; px_rr(px, ww, hh, th, 9, RGB(0, 0, 0), 0.35f, 0); RECT th2 = { tx - 8, cy - 8, tx + 8, cy + 8 }; px_rr(px, ww, hh, th2, 8, WHITE, 1.0f, 0);
        }
        else if (i == B_LIVE) { px_rr(px, ww, hh, r, BTN_H / 2, WHITE, hov ? 0.18f : 0.08f, 0); px_rr(px, ww, hh, r, BTN_H / 2, WHITE, 0.35f, 1); }
        else if (i == B_DONE) { px_rr(px, ww, hh, r, BTN_H / 2, ACCENT, hov ? 1.0f : 0.94f, 0); px_rr(px, ww, hh, r, BTN_H / 2, WHITE, 0.25f, 1); }
    }
    SetBkMode(mem, TRANSPARENT);
    HFONT fm = bar_font(13, FW_MEDIUM), fn = bar_font(12, FW_NORMAL);
    HGDIOBJ oldf = SelectObject(mem, fn); bar_text(mem, HINT, g_hintshape, RGB(0xd8, 0xd0, 0xe4), DT_CENTER | DT_END_ELLIPSIS);
    SelectObject(mem, fm);
    for (int i = B_ALL; i < B_N; i++) {
        RECT r = g_bh[i]; if (r.right <= r.left) continue;
        if (i == B_ALL) bar_text(mem, any ? L"Desativar tudo" : L"Ativar tudo", r, TEXT, DT_CENTER | DT_END_ELLIPSIS);
        else if (i >= B_W0 && i <= B_W3) { r.left += 26; r.right -= 8; bar_text(mem, WG[i - B_W0].label, r, (f->flags & WG[i - B_W0].bit) ? RGB(255, 255, 255) : RGB(0xc9, 0xbd, 0xd8), DT_LEFT | DT_END_ELLIPSIS); }
        else if (i == B_OPM) bar_text(mem, L"\x2212", r, TEXT, DT_CENTER);
        else if (i == B_OPP) bar_text(mem, L"+", r, TEXT, DT_CENTER);
        else if (i == B_LIVE) { r.left += 8; r.right -= 8; bar_text(mem, L"Trocar transmissão", r, TEXT, DT_CENTER | DT_END_ELLIPSIS); }
        else if (i == B_DONE) bar_text(mem, L"Concluir", r, RGB(255, 255, 255), DT_CENTER);
    }
    { wchar_t ot[32]; swprintf(ot, 32, L"Opacidade %d%%", f->opacity < 0 ? 0 : f->opacity > 100 ? 100 : f->opacity); bar_text(mem, ot, g_oplab, MUTED, DT_LEFT | DT_END_ELLIPSIS); }
    SelectObject(mem, oldf); DeleteObject(fm); DeleteObject(fn);
    GdiFlush();
    /* máscara de alfa: união da pílula da dica e da barra, com a mesma suavização dos cantos */
    const RECT *sh[2] = { &g_barshape, &g_hintshape };
    for (int y = 0; y < hh; y++) for (int x = 0; x < ww; x++) {
        float cov = 0;
        for (int k = 0; k < 2; k++) {
            float hw = (sh[k]->right - sh[k]->left) * 0.5f, hh2 = (sh[k]->bottom - sh[k]->top) * 0.5f;
            float d = rr_dist(x + 0.5f - (sh[k]->left + hw), y + 0.5f - (sh[k]->top + hh2), hw, hh2, k == 0 ? 14.0f : 13.0f);
            float c = 0.5f - d; c = c < 0 ? 0 : c > 1 ? 1 : c; if (c > cov) cov = c;
        }
        float a = cov * 0.97f; unsigned c = px[y * ww + x];
        unsigned r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
        px[y * ww + x] = ((unsigned)(a * 255) << 24) | ((unsigned)(r * a) << 16) | ((unsigned)(g * a) << 8) | (unsigned)(b * a);
    }
    POINT pos = { wa.left + (wa.right - wa.left - ww) / 2, wa.bottom - hh - 24 }, src = { 0, 0 }; SIZE sz = { ww, hh };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hbar, screen, &pos, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    snap_keep(1, px, ww, hh, pos.x, pos.y);
    if (!g_noshow) { if (!IsWindowVisible(hbar)) ShowWindow(hbar, SW_SHOWNOACTIVATE); SetWindowPos(hbar, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); esc_register(1); }
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL, screen);
}

/* Ação de um botão da barra: vale na hora na tela e vai para o app (que grava em Configurações > Sobreposição). */
static void bar_touch(void) { g_hold = GetTickCount() + 500; paint(&cur); }
static void bar_set_opacity(int v) { v = v < OP_MIN ? OP_MIN : v > 100 ? 100 : v; if (v == cur.opacity) return; cur.opacity = v; emit("opacity %d\n", v); bar_touch(); }
static void bar_act(int id, int x) {
    if (id == B_ALL) { int on = (cur.flags & 15) == 0; cur.flags = on ? 15 : 0; emit("widget all %d\n", on); bar_touch(); }
    else if (id >= B_W0 && id <= B_W3) { int bit = WG[id - B_W0].bit; cur.flags ^= bit; emit("widget %s %d\n", WG[id - B_W0].id, (cur.flags & bit) != 0); bar_touch(); }
    else if (id == B_OPM) bar_set_opacity(cur.opacity - 5);
    else if (id == B_OPP) bar_set_opacity(cur.opacity + 5);
    else if (id == B_SLIDER) { g_bar_drag = 1; if (!g_inject) SetCapture(hbar); bar_set_opacity(slider_value(x)); }
    else if (id == B_LIVE) emit("selectlive\n");
    else if (id == B_DONE) emit("edit 0\n");
}
static void bar_down(int x, int y) { if (g_bar_on) bar_act(bar_hit(x, y), x); }
static void bar_move(int x, int y) {
    if (g_bar_drag) { bar_set_opacity(slider_value(x)); return; }
    int h = bar_hit(x, y); if (h != g_bhover) { g_bhover = h; paint_bar(&cur); }
}
static void bar_up(void) { if (!g_bar_drag) return; g_bar_drag = 0; if (!g_inject) ReleaseCapture(); }
/* quadro vindo do app: dentro da janela de espera, o que o usuário acabou de mudar vale mais */
static void adopt_frame(const frame_t *nw) {
    const int fl = cur.flags, op = cur.opacity; cur = *nw;
    if ((int)(g_hold - GetTickCount()) > 0 || g_bar_drag) { cur.flags = fl; cur.opacity = op; }
}

static int g_mode;                          /* 0 nada, 1 mover, 2 redimensionar */
static POINT g_c0; static int g_x0, g_y0, g_w0;

static int hit_kind(int x, int y) {         /* 0 nada, 1 mover, 2 redimensionar (x, y em px do painel) */
    if (x < 0 || y < 0 || x >= g_pw || y >= g_ph) return 0;
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
    paint_bar(&cur);   /* a barra acompanha o monitor onde o painel foi solto */
}
/* roda do mouse sobre o painel (modo de edição): muda o tamanho de 16 em 16 px */
static void wheel_step(int dir) {
    if (!g_edit || g_dragging || !dir) return;
    int w = g_pw + (dir > 0 ? 16 : -16); w = w < W_MIN ? W_MIN : w > W_MAX ? W_MAX : w;
    if (w == g_pw) return;
    g_dragging = 1; g_pw = w; paint(&cur); g_dragging = 0;   /* g_dragging: largura e posição atuais valem, não as do quadro */
    send_layout();
}

static LRESULT CALLBACK barproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCHITTEST: return g_bar_on ? HTCLIENT : HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR: if (g_bar_on) { POINT p; GetCursorPos(&p); ScreenToClient(h, &p); SetCursor(LoadCursor(NULL, bar_hit(p.x, p.y) ? IDC_HAND : IDC_ARROW)); return TRUE; } break;
    case WM_LBUTTONDOWN: bar_down((short)LOWORD(l), (short)HIWORD(l)); return 0;
    case WM_MOUSEMOVE: { TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 }; TrackMouseEvent(&t); bar_move((short)LOWORD(l), (short)HIWORD(l)); return 0; }
    case WM_MOUSELEAVE: if (g_bhover) { g_bhover = 0; paint_bar(&cur); } return 0;
    case WM_LBUTTONUP: case WM_CAPTURECHANGED: bar_up(); return 0;
    case WM_HOTKEY: if (g_bar_on) emit("edit 0\n"); return 0;   /* Esc registrado só durante a edição */
    }
    return DefWindowProcW(h, m, w, l);
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
    case WM_MOUSEWHEEL: wheel_step((short)HIWORD(w) > 0 ? 1 : -1); return 0;
    case WM_FRAME: { frame_t nw; EnterCriticalSection(&cs); nw = ready; LeaveCriticalSection(&cs); adopt_frame(&nw); if (!g_dragging) paint(&cur); return 0; }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* thread de leitura: stdin do app. Fim do pipe = o app fechou. */
static DWORD WINAPI reader(LPVOID arg) {
    (void)arg;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    char buf[2048], ln[1024]; size_t n = 0; DWORD got;
    frame_t build; memset(&build, 0, sizeof build); build.opacity = 92; build.flags = 15;
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
    apply_line(&b, &o, "flags 11"); apply_line(&b, &o, "lives 3"); apply_line(&b, &o, "end");
    f += check(o.flags == 11 && o.lives == 3, "flags (itens ligados) e lives entram no quadro");
    apply_line(&b, &o, "flags 99"); apply_line(&b, &o, "lives -4"); f += check(b.flags == 15 && b.lives == 0, "flags e lives fora da faixa são contidos");
    f += check(apply_line(&b, &o, "quit") == 2 && apply_line(&b, &o, "lixo qualquer") == 0, "quit fecha; linha desconhecida é ignorada");
    for (int i = 0; i < 20; i++) apply_line(&b, &o, "p 0 x");
    f += check(b.n <= MAXR, "no máximo 12 linhas");
    return f;
}

#define CLR() do { g_out_n = 0; g_out[0] = 0; } while (0)
#define CTR(i) ((g_bh[i].left + g_bh[i].right) / 2), ((g_bh[i].top + g_bh[i].bottom) / 2)
static int px_alpha(int k, int x, int y) { return g_snap[k].px && x >= 0 && y >= 0 && x < g_snap[k].w && y < g_snap[k].h ? (int)(g_snap[k].px[y * g_snap[k].w + x] >> 24) : -1; }

/* Barra de edição: geometria, cantos, textos, cada botão (com o que manda ao app), Esc, estado "valendo na hora". */
static int bar_selftest(frame_t *b, frame_t *o) {
    int f = 0; RECT br, wa = bar_workarea(), hs = g_hintshape; GetWindowRect(hbar, &br);
    f += check(g_bar_on && IsWindow(hbar), "em edição existe a barra (2ª janela)");
    f += check(br.right - br.left <= BAR_MAXW && br.left >= wa.left && br.right <= wa.right && br.bottom == wa.bottom - 24 && abs((br.left + br.right) / 2 - (wa.left + wa.right) / 2) <= 1, "barra embaixo, centralizada na área útil do monitor e com largura contida (<= 680)");
    LONG ex = GetWindowLongW(hbar, GWL_EXSTYLE);
    f += check((ex & WS_EX_NOACTIVATE) && (ex & WS_EX_TOPMOST) && !(ex & WS_EX_TRANSPARENT) && SendMessageW(hbar, WM_NCHITTEST, 0, 0) == HTCLIENT, "barra: no topo, sem ativar, recebe o mouse");
    /* geometria em várias larguras: tudo dentro da barra, sem sobreposição, alvos de pelo menos 28 px */
    int geo = 1, tall = 1, over = 0;
    for (int ww = BAR_MINW; ww <= BAR_MAXW; ww += 40) for (int lv = 0; lv <= 2; lv += 2) {
        bar_layout(ww, lv);
        for (int i = B_ALL; i < B_N; i++) {
            const RECT *r = &g_bh[i]; if (i == B_LIVE && lv < 2) { if (r->right) geo = 0; continue; }
            if (r->left < BAR_PAD || r->right > ww - BAR_PAD || r->top < g_barshape.top || r->bottom > g_barshape.bottom) geo = 0;
            if (r->bottom - r->top < 28) tall = 0;
            for (int j = i + 1; j < B_N; j++) { const RECT *q = &g_bh[j]; if (q->right && r->right && r->left < q->right && q->left < r->right && r->top < q->bottom && q->top < r->bottom) over++; }
        }
        if (g_bh[B_SLIDER].right - g_bh[B_SLIDER].left < 48) geo = 0;
        if (g_oplab.right > g_bh[B_OPM].left) geo = 0;
    }
    f += check(geo && !over, "botões sempre dentro da barra (560 a 680 px), sem sobrepor e com o controle de opacidade >= 48 px");
    f += check(tall, "alvos de clique com 28 px ou mais de altura");
    bar_layout(g_bww, 2);
    {   /* textos: com a largura cheia nenhum rótulo precisa de reticências; nas menores a elipse protege (DT_END_ELLIPSIS) */
        HDC dc = CreateCompatibleDC(NULL); HFONT fm = bar_font(13, FW_MEDIUM); HGDIOBJ oo = SelectObject(dc, fm); int fit = 1; SIZE sz;
        bar_layout(BAR_MAXW, 2);
        for (int i = 0; i < 4; i++) { GetTextExtentPoint32W(dc, WG[i].label, (int)wcslen(WG[i].label), &sz); if (sz.cx > g_bh[B_W0 + i].right - g_bh[B_W0 + i].left - 34) fit = 0; }
        GetTextExtentPoint32W(dc, L"Trocar transmissão", 18, &sz); if (sz.cx > g_bh[B_LIVE].right - g_bh[B_LIVE].left - 16) fit = 0;
        GetTextExtentPoint32W(dc, L"Desativar tudo", 14, &sz); if (sz.cx > g_bh[B_ALL].right - g_bh[B_ALL].left - 8) fit = 0;
        GetTextExtentPoint32W(dc, L"Opacidade 100%", 14, &sz); if (sz.cx > g_oplab.right - g_oplab.left) fit = 0;
        SelectObject(dc, oo); DeleteObject(fm); DeleteDC(dc);
        f += check(fit, "na largura cheia todos os textos cabem inteiros (sem reticências)");
        bar_layout(g_bww, 2);
    }
    /* cantos: nada de canto reto (alfa ~0 na quina, opaco logo dentro do arco) na barra, na dica e no painel */
    {
        const int by = g_barshape.top, bx1 = g_bww - 1, byb = g_barshape.bottom - 1;
        int ok = px_alpha(1, 0, by) < 8 && px_alpha(1, bx1, by) < 8 && px_alpha(1, 0, byb) < 8 && px_alpha(1, bx1, byb) < 8
              && px_alpha(1, 5, by + 5) > 200 && px_alpha(1, bx1 - 5, by + 5) > 200 && px_alpha(1, 5, byb - 5) > 200 && px_alpha(1, bx1 - 5, byb - 5) > 200;
        f += check(ok, "barra: quinas transparentes e arco de raio 14 (nenhum canto reto)");
        const int hl = hs.left, hr = hs.right - 1, ht = 0, hb = HINT_H - 1;
        ok = px_alpha(1, hl, ht) < 8 && px_alpha(1, hr, hb) < 8 && px_alpha(1, hl, hb) < 8 && px_alpha(1, hr, ht) < 8 && px_alpha(1, hl + 14, ht + 13) > 200;
        f += check(ok, "pílula da dica: cantos totalmente arredondados");
        ok = px_alpha(0, 0, 0) < 8 && px_alpha(0, g_pw - 1, 0) < 8 && px_alpha(0, 0, g_ph - 1) < 8 && px_alpha(0, g_pw - 1, g_ph - 1) < 8 && px_alpha(0, g_pw / 2, g_ph / 2) > 150;
        f += check(ok, "painel em edição: quinas transparentes (canto arredondado também na edição)");
        f += check(px_alpha(1, 3, HINT_H + 3) < 8, "sombra/fundo não vaza fora do arco (fora da barra é transparente)");
    }
    /* botões */
    CLR(); bar_down(CTR(B_W1));
    f += check(strstr(g_out, "widget voice 0\n") && cur.flags == 13, "clicar 'Quem está na sala' desliga na hora e avisa o app (widget voice 0)");
    CLR(); bar_down(CTR(B_ALL));
    f += check(strstr(g_out, "widget all 0\n") && cur.flags == 0, "'Desativar tudo' apaga os quatro (widget all 0)");
    CLR(); bar_down(CTR(B_ALL));
    f += check(strstr(g_out, "widget all 1\n") && cur.flags == 15, "com tudo apagado vira 'Ativar tudo' (widget all 1)");
    CLR(); bar_down(CTR(B_W0)); bar_down(CTR(B_W2)); bar_down(CTR(B_W3));
    f += check(strstr(g_out, "widget live 0\n") && strstr(g_out, "widget status 0\n") && strstr(g_out, "widget mute 0\n") && cur.flags == 2, "cada botão manda o seu id (live, status, mute)");
    CLR(); bar_down(CTR(B_W0)); f += check(strstr(g_out, "widget live 1\n") && (cur.flags & 1), "clicar de novo liga");
    CLR(); bar_down(CTR(B_OPM)); f += check(strstr(g_out, "opacity 85\n") && cur.opacity == 85, "'−' baixa a opacidade de 5 em 5 (90 -> 85)");
    CLR(); bar_down(CTR(B_OPP)); bar_down(CTR(B_OPP)); f += check(strstr(g_out, "opacity 95\n") && cur.opacity == 95, "'+' sobe (85 -> 95)");
    CLR(); bar_down(g_bh[B_SLIDER].left + 9, (g_bh[B_SLIDER].top + g_bh[B_SLIDER].bottom) / 2); f += check(g_bar_drag && cur.opacity == OP_MIN && strstr(g_out, "opacity 10\n"), "clicar no começo do controle vai ao mínimo (10%)");
    bar_move(g_bh[B_SLIDER].right + 40, (g_bh[B_SLIDER].top + g_bh[B_SLIDER].bottom) / 2); f += check(cur.opacity == 100 && g_bar_drag, "arrastar o controle até o fim vai a 100% (limitado)");
    bar_up(); f += check(!g_bar_drag, "soltar encerra o arraste do controle");
    for (int i = 0; i < 30; i++) bar_down(CTR(B_OPP)); f += check(cur.opacity == 100, "opacidade nunca passa de 100");
    for (int i = 0; i < 30; i++) bar_down(CTR(B_OPM)); f += check(cur.opacity == OP_MIN, "opacidade nunca abaixo de 10");
    /* o que o usuário acabou de mudar vale mais que um quadro velho do app; depois da espera, o app manda */
    { frame_t st = *o; st.flags = 15; st.opacity = 40; bar_down(CTR(B_W1)); const int fl = cur.flags; adopt_frame(&st);
      f += check(cur.flags == fl && cur.opacity == OP_MIN, "quadro velho do app logo após o clique não desfaz a mudança");
      g_hold = 0; adopt_frame(&st); f += check(cur.flags == 15 && cur.opacity == 40, "passada a espera, o valor do app (Configurações) prevalece: sincronia nos dois sentidos"); }
    /* Trocar transmissão: só com mais de uma live */
    CLR(); bar_down(CTR(B_LIVE)); f += check(strstr(g_out, "selectlive\n") != NULL, "'Trocar transmissão' manda selectlive");
    { frame_t one = cur; one.lives = 1; paint_bar(&one); f += check(!g_bh[B_LIVE].right && g_bar_on, "com uma live só não há 'Trocar transmissão'"); paint_bar(&cur); }
    /* hover */
    bar_move(CTR(B_W2)); f += check(g_bhover == B_W2, "passar o mouse destaca o botão");
    bar_move(2, 2); f += check(g_bhover == B_NONE, "tirar o mouse remove o destaque");
    /* Concluir e Esc */
    CLR(); bar_down(CTR(B_DONE)); f += check(strstr(g_out, "edit 0\n") != NULL, "Concluir manda 'edit 0'");
    CLR(); SendMessageW(hbar, WM_HOTKEY, 1, 0); f += check(strstr(g_out, "edit 0\n") != NULL, "Esc (WM_HOTKEY da barra) também manda 'edit 0'");
    /* roda do mouse no painel */
    { char ln[120]; CLR(); const int w0 = g_pw; wheel_step(1); f += check(g_pw == w0 + 16 && strstr(g_out, "layout ") != NULL, "roda do mouse aumenta o painel (+16 px) e avisa o app"); wheel_step(-1); wheel_step(-1); f += check(g_pw == w0 - 16, "roda para o outro lado diminui");
      for (int i = 0; i < 60; i++) wheel_step(-1); f += check(g_pw == W_MIN, "a roda respeita a largura mínima"); for (int i = 0; i < 80; i++) wheel_step(1); f += check(g_pw == W_MAX, "e a máxima"); (void)ln; }
    (void)b; return f;
}

/* Teste do modo de edição, sem mostrar janela e com o cursor injetado: arrastar, redimensionar, Concluir, por monitor. */
static int edit_selftest(void) {
    int f = 0; g_noshow = 1; g_capture_out = 1; g_inject = 1; g_snap_on = 1;
    HMONITOR pm = MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY); MONITORINFOEXW mi; mi.cbSize = sizeof mi; GetMonitorInfoW(pm, (MONITORINFO *)&mi);
    char dev[40]; WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, dev, sizeof dev, NULL, NULL);
    frame_t b, o; memset(&b, 0, sizeof b); memset(&o, 0, sizeof o); char ln[160];
    apply_line(&b, &o, "cfg 90 0"); apply_line(&b, &o, "edit 1"); apply_line(&b, &o, "flags 15"); apply_line(&b, &o, "lives 2"); apply_line(&b, &o, "begin"); apply_line(&b, &o, "p 1 Marina"); apply_line(&b, &o, "p 0 Rafael");
    snprintf(ln, sizeof ln, "place %s 100 80 300", dev); apply_line(&b, &o, ln);
    apply_line(&b, &o, "end"); cur = o; paint(&cur);
    RECT r; GetWindowRect(hwnd, &r);
    f += check(g_edit == 1 && g_pw == 300, "modo de edição ligado e largura salva (300) aplicada");
    f += check(r.left == mi.rcWork.left + 100 && r.top == mi.rcWork.top + 80, "posição salva relativa à área útil do monitor");
    f += check(!(GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) && SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(r.left + 20, r.top + 20)) == HTCLIENT, "em edição o mouse é recebido (sem WS_EX_TRANSPARENT)");
    f += check((GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0, "continua sem ativar (não rouba o foco do jogo)");
    f += check(hit_kind(10, 10) == 1 && hit_kind(g_pw - 4, g_ph - 4) == 2 && hit_kind(-3, 5) == 0, "acerto: corpo move, canto redimensiona, fora nada");
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
    f += bar_selftest(&b, &o);
    apply_line(&b, &o, "edit 0"); apply_line(&b, &o, "end"); cur = o; paint(&cur);
    f += check(!g_bar_on && !bar_hit(g_bh[B_DONE].left + 2, g_bh[B_DONE].top + 2), "fora da edição a barra some e não tem botões");
    g_out_n = 0; g_out[0] = 0; bar_down(30, 50); f += check(g_out_n == 0, "fora da edição clicar na barra não manda nada");
    f += check(g_edit == 0 && (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) && SendMessageW(hwnd, WM_NCHITTEST, 0, 0) == HTTRANSPARENT, "fora da edição o clique volta a atravessar");
    drag_begin(10, 10); f += check(!g_dragging, "fora da edição o arraste é ignorado");
    apply_line(&b, &o, "place \\\\.\\DISPLAY99 50 50 260"); apply_line(&b, &o, "end"); cur = o; paint(&cur);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0); GetWindowRect(hwnd, &r);
    f += check(r.right == wa.right - 20 && r.top == wa.top + 20, "monitor salvo que sumiu: volta ao canto escolhido do principal");
    snprintf(ln, sizeof ln, "place %s 99999 99999 260", dev); apply_line(&b, &o, ln); apply_line(&b, &o, "end"); cur = o; paint(&cur); GetWindowRect(hwnd, &r);
    f += check(r.right <= mi.rcWork.right && r.bottom <= mi.rcWork.bottom, "posição salva fora da tela é trazida para dentro");
    return f;
}

/* Captura SEM mostrar janela: desenha o painel e a barra em modo de edição e compõe sobre um "jogo" de mentira, salvando um BMP 24 bits.
 *   briks_overlay.exe --shot arquivo.bmp      (o script scripts/overlay-barra-captura.ps1 converte em PNG) */
static void blend_snap(unsigned char *rgb, int cw, int ch, int k, int ox, int oy) {
    if (!g_snap[k].px) return;
    for (int y = 0; y < g_snap[k].h; y++) for (int x = 0; x < g_snap[k].w; x++) {
        int X = g_snap[k].x - ox + x, Y = g_snap[k].y - oy + y; if (X < 0 || Y < 0 || X >= cw || Y >= ch) continue;
        unsigned c = g_snap[k].px[y * g_snap[k].w + x]; unsigned a = c >> 24; unsigned char *d = rgb + ((size_t)Y * cw + X) * 3;
        d[0] = (unsigned char)(((c & 255) + d[0] * (255 - a) / 255)); d[1] = (unsigned char)((((c >> 8) & 255) + d[1] * (255 - a) / 255)); d[2] = (unsigned char)((((c >> 16) & 255) + d[2] * (255 - a) / 255));
    }
}
static int shot_main(const wchar_t *path) {
    g_noshow = 1; g_capture_out = 1; g_inject = 1; g_snap_on = 1;
    frame_t b, o; memset(&b, 0, sizeof b); memset(&o, 0, sizeof o);
    apply_line(&b, &o, "cfg 86 0"); apply_line(&b, &o, "edit 1"); apply_line(&b, &o, "flags 11"); apply_line(&b, &o, "lives 2");
    apply_line(&b, &o, "begin"); apply_line(&b, &o, "i Sala conectada"); apply_line(&b, &o, "i Ao vivo: Rafael Lima"); apply_line(&b, &o, "p 1 Marina Costa"); apply_line(&b, &o, "p 0 Rafael Lima"); apply_line(&b, &o, "p 0 Você");
    apply_line(&b, &o, "end");
    cur = o; g_bhover = B_W1; paint(&cur);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0); const int cw = wa.right - wa.left, ch = wa.bottom - wa.top;
    unsigned char *rgb = (unsigned char *)malloc((size_t)cw * ch * 3); if (!rgb) return 1;
    for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) {   /* "jogo": céu em degradê com blocos */
        unsigned char *d = rgb + ((size_t)y * cw + x) * 3; int t = y * 255 / ch;
        d[0] = (unsigned char)(70 + t / 3 + ((x / 90 + y / 90) % 2) * 14); d[1] = (unsigned char)(110 - t / 4 + ((x / 90 + y / 90) % 2) * 10); d[2] = (unsigned char)(150 - t / 3);
    }
    blend_snap(rgb, cw, ch, 0, wa.left, wa.top); blend_snap(rgb, cw, ch, 1, wa.left, wa.top);
    HANDLE fh = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); if (fh == INVALID_HANDLE_VALUE) { free(rgb); return 1; }
    const int stride = (cw * 3 + 3) & ~3; BITMAPFILEHEADER bf = { 0x4d42, (DWORD)(54 + (size_t)stride * ch), 0, 0, 54 }; BITMAPINFOHEADER bh = { sizeof bh, cw, ch, 1, 24, BI_RGB, (DWORD)((size_t)stride * ch), 0, 0, 0, 0 };
    DWORD w; WriteFile(fh, &bf, sizeof bf, &w, NULL); WriteFile(fh, &bh, sizeof bh, &w, NULL);
    unsigned char *row = (unsigned char *)calloc(1, (size_t)stride);
    for (int y = ch - 1; y >= 0; y--) { memcpy(row, rgb + (size_t)y * cw * 3, (size_t)cw * 3); WriteFile(fh, row, (DWORD)stride, &w, NULL); }
    CloseHandle(fh); free(row); free(rgb); return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev; (void)cmd; (void)show;
    InitializeCriticalSection(&cs);
    int argc; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int selftest = 0, use_stdin = 0, protocol = 0, edittest = 0; const wchar_t *shot = NULL;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--selftest")) { selftest = 1; continue; }
        if (!wcscmp(argv[i], L"--protocol-test")) { protocol = 1; continue; }
        if (!wcscmp(argv[i], L"--stdin")) { use_stdin = 1; continue; }
        if (!wcscmp(argv[i], L"--edit-test")) { edittest = 1; continue; }
        if (!wcscmp(argv[i], L"--shot") && i + 1 < argc) { shot = argv[++i]; continue; }
        if (cur.n >= MAXR) break;
        wchar_t *c = wcsrchr(argv[i], L':'); int sp = 0;
        if (c && !wcscmp(c, L":f")) { *c = 0; sp = 1; }
        add_row(&cur, sp ? R_SPEAK : R_IDLE, argv[i]);
    }
    if (protocol) {   /* só o analisador de linhas, sem criar janela */
        int f = protocol_selftest(); say(f ? "\n%d falha(s)\n" : "\ntudo certo\n", f); return f ? 1 : 0;
    }
    cur.opacity = 92; cur.flags = 15;
    if (!cur.n && selftest) { add_row(&cur, R_SPEAK, L"Marina Costa"); add_row(&cur, R_IDLE, L"Rafael Lima"); }
    WNDCLASSW wc = { 0 }; wc.lpfnWndProc = proc; wc.hInstance = inst; wc.lpszClassName = L"BriksOverlay"; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                           wc.lpszClassName, L"Briks overlay", WS_POPUP, 0, 0, 10, 10, NULL, NULL, inst, NULL);
    if (!hwnd) return 1;
    { WNDCLASSW bc = { 0 }; bc.lpfnWndProc = barproc; bc.hInstance = inst; bc.lpszClassName = L"BriksOverlayBar"; bc.hCursor = LoadCursor(NULL, IDC_ARROW); RegisterClassW(&bc);
      hbar = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, bc.lpszClassName, L"Briks overlay - barra de edicao", WS_POPUP, 0, 0, 10, 10, NULL, NULL, inst, NULL); }
    if (!hbar) return 1;
    if (shot) { int r = shot_main(shot); DestroyWindow(hbar); DestroyWindow(hwnd); return r; }
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
