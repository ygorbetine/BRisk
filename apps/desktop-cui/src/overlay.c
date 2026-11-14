/* Overlay do Briks (segundo executável): janela sempre no topo, sem foco, com cliques passando.
 *   briks_overlay.exe [--selftest] [--stdin] [nome[:f] ...]     ("nome:f" = falando)
 * Estilo: WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW.
 * Desenho por UpdateLayeredWindow com bitmap 32 bits pré-multiplicado (cantos arredondados reais e alfa por pixel).
 * Com --stdin o app manda o conteúdo por linhas (UTF-8) e o overlay fecha sozinho se o app sumir (stdin fecha):
 *   cfg <opacidade 0-100> <canto 0-3>      canto: 0 superior direito, 1 superior esquerdo, 2 inferior direito, 3 inferior esquerdo
 *   place <monitor> <x> <y> <largura>        posição salva (x,y relativos à área útil do monitor); monitor ausente = canto do monitor principal
 *   edit <0|1>                             modo de edição: o painel aceita o mouse (arrastar, canto inferior direito ou roda do mouse redimensionam) e aparece a barra de edição (2ª janela, embaixo da tela)
 *   flags <0-15>                           itens ligados (1 transmissão, 2 quem está na sala, 4 sala conectada, 8 aviso de mudo): estado dos botões da barra
 *   livemap <nome>                        memória compartilhada com as miniaturas (4 regiões; cabeçalho 'BOVL', seq, w, h, n; BGRA reduzido a ~320 px)
 *   tile <i> <chave> <0|1> <nome>          miniatura i (0-3) de uma transmissão: chave estável da pessoa, ligada (1) ou oculta (0) e o nome
 *   tplace <i> <monitor> <x> <y> <%>       posição salva (relativa à área útil) e escala 50-200 da miniatura i
 *   lives <n>                              quantas transmissões ao vivo há na sala (n > 1 mostra "Trocar transmissão")
 *   begin                                  começa um quadro novo (apaga as linhas)
 *   p <0|1> <nome>                         pessoa na sala (1 = falando)
 *   i <texto>                              linha de informação (ex.: Sala conectada)
 *   w <texto>                              linha de aviso (ex.: Microfone mudo)
 *   end                                    mostra o quadro (sem linhas = esconde a janela)
 *   quit                                   fecha
 * O overlay responde pela saída padrão (uma linha por evento):  layout <monitor> <x> <y> <largura>  (soltou depois de mover/redimensionar)  |  edit 0  (Concluir ou Esc)
 *   widget <live|voice|status|mute|all> <0|1>  (botão da barra)  |  opacity <10-100>  (opacidade da barra)  |  selectlive  (Trocar transmissão, só com mais de 4 lives)
 *   tile <chave> <monitor> <x> <y> <%>  (miniatura movida/redimensionada)  |  tileon <chave> <0|1>  (ligou/ocultou a miniatura)
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
enum { R_IDLE, R_SPEAK, R_INFO, R_WARN, R_LIVE };
#define NT 4   /* miniaturas de transmissão */
typedef struct { char key[16]; int on, pvalid, px, py, pct; char pdev[40]; wchar_t name[64]; } tspec_t;
typedef struct { wchar_t text[64]; int kind; } row_t;
typedef struct { row_t r[MAXR]; int n; int opacity, corner; int edit, drag, flags, lives; char lmap[48]; tspec_t t[NT]; int ntile; char pdev[40]; int pvalid, px, py, pw; } frame_t;
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
    if (!strncmp(ln, "livemap ", 8)) { snprintf(build->lmap, sizeof build->lmap, "%s", ln + 8); return 0; }
    if (!strncmp(ln, "tile ", 5)) {
        int i, on, c = 0; char key[16];
        if (sscanf(ln + 5, "%d %15s %d %n", &i, key, &on, &c) >= 3 && c > 0 && i >= 0 && i < NT) {
            tspec_t *t = &build->t[i]; memset(t, 0, sizeof *t); snprintf(t->key, sizeof t->key, "%s", key); t->on = on != 0;
            MultiByteToWideChar(CP_UTF8, 0, ln + 5 + c, -1, t->name, 64); if (i >= build->ntile) build->ntile = i + 1;
        }
        return 0;
    }
    if (!strncmp(ln, "tplace ", 7)) {
        int i, x, y, p; char dev[40];
        if (sscanf(ln + 7, "%d %39s %d %d %d", &i, dev, &x, &y, &p) == 5 && i >= 0 && i < build->ntile) { tspec_t *t = &build->t[i]; snprintf(t->pdev, sizeof t->pdev, "%s", dev); t->px = x; t->py = y; t->pct = p < 50 ? 50 : p > 200 ? 200 : p; t->pvalid = 1; }
        return 0;
    }
    if (!strncmp(ln, "drag ", 5)) { build->drag = atoi(ln + 5) != 0; return 0; }
    if (!strncmp(ln, "flags ", 6)) { int v = atoi(ln + 6); build->flags = v < 0 ? 0 : v > 15 ? 15 : v; return 0; }
    if (!strncmp(ln, "lives ", 6)) { int v = atoi(ln + 6); build->lives = v < 0 ? 0 : v > 99 ? 99 : v; return 0; }
    if (!strncmp(ln, "place ", 6)) {
        char dev[40]; int x, y, wd;
        if (sscanf(ln + 6, "%39s %d %d %d", dev, &x, &y, &wd) == 4) { snprintf(build->pdev, sizeof build->pdev, "%s", dev); build->px = x; build->py = y; build->pw = wd < W_MIN ? W_MIN : wd > W_MAX ? W_MAX : wd; build->pvalid = 1; }
        else build->pvalid = 0;
        return 0;
    }
    if (!strcmp(ln, "begin")) { build->n = 0; build->ntile = 0; return 0; }
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

/* cópia dos últimos quadros desenhados (0 painel, 1 barra, 2+k miniatura k) para a captura --shot */
static struct { unsigned *px; int w, h, x, y; } g_snap[2 + NT]; static int g_snap_on; static int g_tdrag = -1;   /* g_tdrag: miniatura sendo arrastada */
static void snap_keep(int i, const unsigned *px, int w, int h, int x, int y) {
    if (!g_snap_on) return;
    free(g_snap[i].px); g_snap[i].px = (unsigned *)malloc((size_t)w * h * 4); if (!g_snap[i].px) return;
    memcpy(g_snap[i].px, px, (size_t)w * h * 4); g_snap[i].w = w; g_snap[i].h = h; g_snap[i].x = x; g_snap[i].y = y;
}

static void px_rr(unsigned *px, int w, int h, RECT r, float rad, COLORREF c, float a, float ring);
static HFONT bar_font(int px, int weight);

static void paint_tiles(const frame_t *f);   /* miniaturas de transmissão (janelas próprias) */

static void paint(const frame_t *f) {
    frame_t tmp;
    if (f->edit && !f->n && !f->ntile) { tmp = *f; add_row(&tmp, R_INFO, L"Pré-visualização da sobreposição"); f = &tmp; }
    if (!f->n) { if (!g_noshow) ShowWindow(hwnd, SW_HIDE); g_edit = f->edit; paint_bar(f); paint_tiles(f); return; }
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
    paint_bar(f); paint_tiles(f);
}

/* ---- arrastar e redimensionar (modo de edição). O cursor vem de cursor_pos(): em teste é injetado. ---- */
static int g_inject; static POINT g_inj;
static POINT cursor_pos(void) { if (g_inject) return g_inj; POINT p; GetCursorPos(&p); return p; }
/* ---- barra de edição: 2ª janela (embaixo da tela, no monitor do painel), vidro escuro de cantos arredondados ---- */
enum { B_NONE, B_ALL, B_W0, B_W1, B_W2, B_W3, B_OPM, B_OPP, B_SLIDER, B_LIVE, B_DONE, B_T0, B_T1, B_T2, B_T3, B_N };
#define BAR_MAXW 680
#define BAR_MINW 560
#define HINT_H 26
#define BAR_PAD 10
#define BTN_H 32
#define BAR_GAP 8
#define BAR_H g_barh   /* 2 linhas; 3 com miniaturas (uma chave por transmissão) */
#define BAR_TOTAL (HINT_H + 8 + g_barh)
#define OP_MIN 10
static HWND hbar; static int g_barh = BAR_PAD * 2 + BTN_H * 2 + BAR_GAP;
static RECT g_bh[B_N];                         /* acertos, px da janela da barra (vazio = não existe) */
static RECT g_barshape, g_hintshape, g_oplab;   /* formas arredondadas e rótulo da opacidade */
static int g_bww, g_bar_on, g_bhover, g_bar_drag, g_esc_reg;
static DWORD g_hold;                            /* até lá, o que o usuário acabou de mudar vale mais que um quadro velho vindo do app */
static const struct { const wchar_t *label; const char *id; int bit; } WG[4] = { { L"Transmissão", "live", 1 }, { L"Quem está na sala", "voice", 2 }, { L"Sala conectada", "status", 4 }, { L"Aviso de mudo", "mute", 8 } };
static const wchar_t *HINT = L"Arraste para mover · roda do mouse ou o canto mudam o tamanho · Esc ou Ctrl+Shift+O conclui";

static int rc_in(const RECT *r, int x, int y) { return r->right > r->left && x >= r->left && x < r->right && y >= r->top && y < r->bottom; }

/* Geometria pura da barra para a largura ww (e "Trocar transmissão" só com mais de uma live). Com miniaturas, a 1ª fileira são os nomes das transmissões; depois: tudo/itens; e opacidade, trocar, concluir. */
static void bar_layout(int ww, int lives, int ntile) {
    memset(g_bh, 0, sizeof g_bh);
    g_barh = BAR_PAD * 2 + BTN_H * 2 + BAR_GAP + (ntile > 0 ? BTN_H + BAR_GAP : 0);
    g_hintshape = (RECT){ 0, 0, 0, HINT_H };
    g_barshape = (RECT){ 0, HINT_H + 8, ww, HINT_H + 8 + BAR_H };
    /* 1.4.1: com miniaturas, a fileira dos nomes das transmissões fica EM CIMA da fileira dos botões comuns */
    const int x0 = BAR_PAD, x1 = ww - BAR_PAD, iw = x1 - x0, yt = g_barshape.top + BAR_PAD, y1 = yt + (ntile > 0 ? BTN_H + BAR_GAP : 0), y2 = y1 + BTN_H + BAR_GAP;
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
    if (lives > NT) { g_bh[B_LIVE] = (RECT){ right - livew, y2, right, y2 + BTN_H }; right -= livew + 16; }
    g_oplab = (RECT){ x0, y2, x0 + 112, y2 + BTN_H };
    g_bh[B_OPM] = (RECT){ g_oplab.right, y2, g_oplab.right + BTN_H, y2 + BTN_H };
    g_bh[B_OPP] = (RECT){ right - BTN_H, y2, right, y2 + BTN_H };
    g_bh[B_SLIDER] = (RECT){ g_bh[B_OPM].right + BAR_GAP, y2, g_bh[B_OPP].left - BAR_GAP, y2 + BTN_H };
    if (g_bh[B_SLIDER].right - g_bh[B_SLIDER].left < 48) g_bh[B_SLIDER].right = g_bh[B_SLIDER].left + 48;
    if (ntile > 0) {   /* linha 3: uma chave por miniatura (liga/desliga só aquela transmissão) */
        const int y3 = yt, cw = (iw - 3 * BAR_GAP) / NT;
        for (int t = 0; t < ntile && t < NT; t++) g_bh[B_T0 + t] = (RECT){ x0 + t * (cw + BAR_GAP), y3, x0 + t * (cw + BAR_GAP) + cw, y3 + BTN_H };
    }
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
    g_bww = ww; g_bar_on = 1;
    bar_layout(ww, f->lives, f->ntile); const int hh = BAR_TOTAL;
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
        else if (i >= B_T0 && i <= B_T3) {
            const int on = f->t[i - B_T0].on;
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
        else if (i >= B_T0 && i <= B_T3) { r.left += 26; r.right -= 8; bar_text(mem, f->t[i - B_T0].name, r, f->t[i - B_T0].on ? RGB(255, 255, 255) : RGB(0xc9, 0xbd, 0xd8), DT_LEFT | DT_END_ELLIPSIS); }
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
    else if (id >= B_T0 && id <= B_T3) { int k = id - B_T0; if (k < cur.ntile) { cur.t[k].on = !cur.t[k].on; emit("tileon %s %d\n", cur.t[k].key, cur.t[k].on); bar_touch(); } }
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
    const int fl = cur.flags, op = cur.opacity, on = cur.ntile; tspec_t old[NT]; memcpy(old, cur.t, sizeof old); cur = *nw;
    if ((int)(g_hold - GetTickCount()) > 0 || g_bar_drag || g_tdrag >= 0) {
        cur.flags = fl; cur.opacity = op;
        for (int k = 0; k < cur.ntile && k < on; k++) if (!strcmp(cur.t[k].key, old[k].key)) {
            cur.t[k].on = old[k].on; cur.t[k].pct = old[k].pct;
            if (old[k].pvalid) { cur.t[k].pvalid = 1; snprintf(cur.t[k].pdev, sizeof cur.t[k].pdev, "%s", old[k].pdev); cur.t[k].px = old[k].px; cur.t[k].py = old[k].py; }
        }
    }
}

/* ---- miniaturas ao vivo: até NT janelas, uma por transmissão, cada uma com posição e escala próprias.
 * As imagens vêm do app por memória compartilhada (livemap): NT regiões de LV_SLOT bytes (cabeçalho 'BOVL', seq, w, h, n + BGRA reduzido a ~320 px). ---- */
#define LV_MAXW 640
#define LV_MAXH 360
#define LV_SLOT (64 + LV_MAXW * LV_MAXH * 4)
#define TILE_BASE 320            /* largura a 100% */
#define TH 28                    /* altura da alça (só em edição) */
#define PCT_MIN 50
#define PCT_MAX 200
static HANDLE g_lvh; static const unsigned char *g_lvp; static char g_lvname[48];
static unsigned g_lvpx[NT][LV_MAXW * LV_MAXH]; static int g_lvw[NT], g_lvhh[NT], g_lvvalid[NT]; static unsigned long long g_lvn[NT];
static void lv_close(void) { if (g_lvp) UnmapViewOfFile(g_lvp); if (g_lvh) CloseHandle(g_lvh); g_lvp = NULL; g_lvh = NULL; g_lvname[0] = 0; for (int k = 0; k < NT; k++) { g_lvvalid[k] = 0; g_lvn[k] = 0; } }
static void lv_open(const char *name) {
    if (!name[0]) { if (g_lvp) lv_close(); return; }
    if (g_lvp && !strcmp(name, g_lvname)) return;
    lv_close();
    wchar_t wn[64]; MultiByteToWideChar(CP_UTF8, 0, name, -1, wn, 64);
    g_lvh = OpenFileMappingW(FILE_MAP_READ, FALSE, wn); if (!g_lvh) return;
    g_lvp = (const unsigned char *)MapViewOfFile(g_lvh, FILE_MAP_READ, 0, 0, 0);
    if (!g_lvp) { CloseHandle(g_lvh); g_lvh = NULL; return; }
    snprintf(g_lvname, sizeof g_lvname, "%s", name);
}
/* 1 se a miniatura k mudou (quadro novo copiado, ou imagem apagada); trata o escritor no meio (seq ímpar) */
static int lv_poll(int k) {
    if (!g_lvp) return 0;
    const unsigned char *m = g_lvp + (size_t)k * LV_SLOT; const volatile unsigned *seq = (const volatile unsigned *)(m + 4); unsigned a = *seq;
    if (memcmp(m, "BOVL", 4) || (a & 1)) return 0;
    int w = *(const int *)(m + 8), h = *(const int *)(m + 12); unsigned long long n = *(const unsigned long long *)(m + 16);
    if (n == 0 || w <= 0 || h <= 0 || w > LV_MAXW || h > LV_MAXH) { int ch = g_lvvalid[k]; g_lvvalid[k] = 0; g_lvn[k] = 0; return ch; }
    if (n == g_lvn[k]) return 0;
    memcpy(g_lvpx[k], m + 64, (size_t)w * h * 4);
    if (*seq != a) return 0;   /* o escritor mexeu no meio da cópia: tenta no próximo ciclo */
    g_lvw[k] = w; g_lvhh[k] = h; g_lvn[k] = n; g_lvvalid[k] = 1; return 1;
}

typedef struct { HWND wnd; int shown, edit, drag, x, y, w, h, ty; RECT bminus, bplus, bclose, bpct; } tile_t;   /* x,y: canto da MINIATURA na tela; ty: altura da alça acima dela */
static tile_t T[NT]; static POINT g_tc0; static int g_tx0, g_ty0;

static int tile_pct(const tspec_t *s) { int p = s->pct ? s->pct : 100; return p < PCT_MIN ? PCT_MIN : p > PCT_MAX ? PCT_MAX : p; }
/* posição da miniatura k: a salva (no monitor dela, se existe) ou, sem posição, uma coluna no lado oposto ao do painel */
static POINT tile_pos(int k, const frame_t *f, int w, int h) {
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    POINT pos = { (f->corner == 1 || f->corner == 3) ? wa.right - w - 20 : wa.left + 20, wa.top + 20 + k * (h + 14) };
    if (pos.y > wa.bottom - h - 20) pos.y = wa.bottom - h - 20;
    const tspec_t *s = &f->t[k];
    if (s->pvalid) {
        monq_t q = { s->pdev, { 0 }, 0 }; EnumDisplayMonitors(NULL, NULL, mon_cb, (LPARAM)&q);
        if (q.found) {
            pos.x = q.work.left + s->px; pos.y = q.work.top + s->py;
            if (pos.x > q.work.right - w) pos.x = q.work.right - w;
            if (pos.y > q.work.bottom - h) pos.y = q.work.bottom - h;
            if (pos.x < q.work.left) pos.x = q.work.left;
            if (pos.y < q.work.top) pos.y = q.work.top;
        }
    }
    return pos;
}

static void paint_tile(int k, const frame_t *f) {
    tile_t *t = &T[k]; if (!t->wnd) return;
    if (k >= f->ntile || (!f->t[k].on && !f->edit)) { t->shown = 0; t->edit = 0; t->drag = 0; if (!g_noshow) ShowWindow(t->wnd, SW_HIDE); return; }
    const tspec_t *sp = &f->t[k]; const int edit = f->edit, on = sp->on;
    const int w = TILE_BASE * tile_pct(sp) / 100, h = w * 9 / 16, ty = edit ? TH + 4 : 0, wh = h + ty;
    const float sc = (float)w / TILE_BASE, rad = 10.0f * sc;
    t->shown = 1; t->edit = edit; t->drag = edit || f->drag; t->w = w; t->h = h; t->ty = ty;
    POINT pos = g_tdrag == k ? (POINT){ t->x, t->y } : tile_pos(k, f, w, h);
    if (pos.y - ty < 0 && ty) pos.y = ty;
    t->x = pos.x; t->y = pos.y;
    HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -wh, 1, 32, BI_RGB } };
    unsigned *px = NULL; HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(RGB(0x0c, 0x04, 0x14)); RECT all = { 0, 0, w, wh }; FillRect(mem, &all, bg); DeleteObject(bg);
    GdiFlush();
    const RECT TR = { 0, ty, w, wh };
    const int have = on && g_lvvalid[k] && g_lvw[k] > 0;
    int iw = w, ih = h, ix0 = 0, iy0 = ty;   /* imagem encaixada (sem cortar), centralizada */
    if (have) { if (g_lvw[k] * h > g_lvhh[k] * w) { ih = g_lvhh[k] * w / g_lvw[k]; iy0 += (h - ih) / 2; } else { iw = g_lvw[k] * h / g_lvhh[k]; ix0 += (w - iw) / 2; } }
    if (have) {   /* 1.5.20: ampliação/redução bilinear em ponto fixo (antes: vizinho mais próximo, serrilhado) */
        static int cx0[1024], cx1[1024], cwx[1024]; const int lw = g_lvw[k], lh = g_lvhh[k]; const unsigned *src = g_lvpx[k]; const int cn = iw < 1024 ? iw : 1024;
        for (int x = 0; x < cn; x++) { int fx = (int)(((long long)(2 * x + 1) * lw * 256) / (2 * iw)) - 128; if (fx < 0) fx = 0; int x0 = fx >> 8; if (x0 > lw - 1) x0 = lw - 1; cx0[x] = x0; cx1[x] = x0 + 1 < lw ? x0 + 1 : x0; cwx[x] = fx & 255; }
        for (int y = 0; y < ih; y++) {
            int fy = (int)(((long long)(2 * y + 1) * lh * 256) / (2 * ih)) - 128; if (fy < 0) fy = 0; int y0 = fy >> 8; if (y0 > lh - 1) y0 = lh - 1; const int y1 = y0 + 1 < lh ? y0 + 1 : y0, wy = fy & 255;
            const unsigned *r0 = src + (size_t)y0 * lw, *r1 = src + (size_t)y1 * lw; unsigned *dst = px + (size_t)(iy0 + y) * w + ix0;
            for (int x = 0; x < cn; x++) {
                const unsigned a = r0[cx0[x]], b = r0[cx1[x]], c = r1[cx0[x]], d = r1[cx1[x]]; const int wx = cwx[x];
                unsigned out = 0;
                for (int sh = 0; sh < 24; sh += 8) {
                    int t0 = (int)((a >> sh) & 255) * (256 - wx) + (int)((b >> sh) & 255) * wx, t1 = (int)((c >> sh) & 255) * (256 - wx) + (int)((d >> sh) & 255) * wx;
                    out |= (unsigned)((t0 * (256 - wy) + t1 * wy) >> 16) << sh;
                }
                dst[x] = out;
            }
        }
    }
    if (edit && !on) px_rr(px, w, wh, TR, rad, RGB(0, 0, 0), 0.62f, 0);   /* oculta: véu escuro */
    px_rr(px, w, wh, TR, rad, RGB(255, 255, 255), 0.16f, 1);
    if (edit) px_rr(px, w, wh, TR, rad, ACCENT, 0.97f, 2);
    memset(&t->bminus, 0, sizeof t->bminus); memset(&t->bplus, 0, sizeof t->bplus); memset(&t->bclose, 0, sizeof t->bclose); memset(&t->bpct, 0, sizeof t->bpct);
    const RECT HR = { 0, 0, w, TH };
    if (edit) {   /* alça: nome + − 100% + × (oculta: "Ligar") */
        px_rr(px, w, wh, HR, TH / 2.0f, RGB(0x1c, 0x0e, 0x2c), 0.97f, 0); px_rr(px, w, wh, HR, TH / 2.0f, RGB(255, 255, 255), 0.18f, 1);
        const int bs = TH - 8, by = 4, wide = w >= 220;
        if (on) { t->bclose = (RECT){ w - 6 - bs, by, w - 6, by + bs }; } else { t->bclose = (RECT){ w - 6 - 52, by, w - 6, by + bs }; }
        const int rl = t->bclose.left - 4;
        if (on) { t->bplus = (RECT){ rl - bs, by, rl, by + bs }; t->bpct = (RECT){ t->bplus.left - 38, by, t->bplus.left, by + bs }; t->bminus = (RECT){ (wide ? t->bpct.left : t->bplus.left) - bs - (wide ? 0 : 4), by, (wide ? t->bpct.left : t->bplus.left) - (wide ? 0 : 4), by + bs }; if (!wide) memset(&t->bpct, 0, sizeof t->bpct); }
        if (on) { px_rr(px, w, wh, t->bminus, bs / 2.0f, RGB(255, 255, 255), 0.12f, 0); px_rr(px, w, wh, t->bplus, bs / 2.0f, RGB(255, 255, 255), 0.12f, 0); px_rr(px, w, wh, t->bclose, bs / 2.0f, RGB(0xdc, 0x26, 0x26), 0.85f, 0); }
        else px_rr(px, w, wh, t->bclose, bs / 2.0f, RGB(0x7c, 0x3a, 0xed), 0.95f, 0);
    } else {
        /* selo "Ao vivo" e nome (pílulas): só fora da edição; na edição o nome está na alça */
    }
    /* selo "Ao vivo", nome e fontes têm tamanho fixo: o zoom da miniatura só muda a imagem, nunca o texto nem o ícone */
    const int bm = 6, bh = 20;
    static HFONT sf, nf; if (!sf) sf = bar_font(12, FW_SEMIBOLD); if (!nf) nf = bar_font(12, FW_MEDIUM);   /* em cache: criar duas fontes por quadro pesava a 30 quadros/s */
    SetBkMode(mem, TRANSPARENT); HGDIOBJ osf = SelectObject(mem, sf);
    RECT mw = { 0, 0, 0, 0 }; DrawTextW(mem, L"Ao vivo", -1, &mw, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    const int bw = bh / 2 + 6 + mw.right + 12;   /* ponto + texto medido + folga: o selo acompanha o texto, sem sobra nem corte */
    RECT badge = { bm, ty + bm, bm + bw, ty + bm + bh };
    if (on) { px_rr(px, w, wh, badge, bh / 2.0f, RGB(0xdc, 0x26, 0x26), 0.95f, 0); RECT bdot = { badge.left + bh / 2 - 3, badge.top + bh / 2 - 3, badge.left + bh / 2 + 3, badge.top + bh / 2 + 3 }; px_rr(px, w, wh, bdot, 3, RGB(255, 255, 255), 1.0f, 0); }
    SelectObject(mem, nf);
    RECT np = { 0, 0, 0, 0 };
    if (!edit) {
        RECT mr = { 0, 0, 0, 0 }; DrawTextW(mem, sp->name, -1, &mr, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        int nw = mr.right + 16, nmax = w - 2 * bm; if (nw > nmax) nw = nmax;
        np = (RECT){ bm, wh - bm - bh, bm + nw, wh - bm }; px_rr(px, w, wh, np, bh / 2.0f, RGB(0, 0, 0), 0.62f, 0);
    }
    GdiFlush();
    if (!edit) { SetTextColor(mem, RGB(255, 255, 255)); RECT nt = { np.left + 8, np.top, np.right - 8, np.bottom }; DrawTextW(mem, sp->name, -1, &nt, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS); }
    if (on) { SelectObject(mem, sf); SetTextColor(mem, RGB(255, 255, 255)); RECT bt = { badge.left + bh / 2 + 6, badge.top, badge.right, badge.bottom }; DrawTextW(mem, L"Ao vivo", -1, &bt, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX); SelectObject(mem, nf); }
    if (edit) {
        SetTextColor(mem, RGB(255, 255, 255));
        const int right = on ? (t->bpct.right ? t->bpct.left : t->bminus.left) : t->bclose.left;
        RECT nt = { 12, 0, right - 6, TH }; DrawTextW(mem, sp->name, -1, &nt, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (on) {
            DrawTextW(mem, L"\x2212", -1, &t->bminus, DT_SINGLELINE | DT_VCENTER | DT_CENTER); DrawTextW(mem, L"+", -1, &t->bplus, DT_SINGLELINE | DT_VCENTER | DT_CENTER); DrawTextW(mem, L"\xd7", -1, &t->bclose, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            if (t->bpct.right) { wchar_t pt[16]; swprintf(pt, 16, L"%d%%", tile_pct(sp)); SetTextColor(mem, MUTED); DrawTextW(mem, pt, -1, &t->bpct, DT_SINGLELINE | DT_VCENTER | DT_CENTER); }
        } else {
            DrawTextW(mem, L"Ligar", -1, &t->bclose, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            SetTextColor(mem, MUTED); RECT hm = TR; DrawTextW(mem, L"Oculta: sem quadros, sem CPU", -1, &hm, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }
    if (on && !have) { SelectObject(mem, nf); SetTextColor(mem, MUTED); RECT lt = TR; DrawTextW(mem, L"Carregando transmissão…", -1, &lt, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX); }
    SelectObject(mem, osf);
    GdiFlush();
    /* alfa: união da miniatura e da alça, cantos arredondados; a opacidade do overlay vale para a imagem (a alça fica quase opaca) */
    const float base = (f->opacity < 30 ? 30 : f->opacity > 100 ? 100 : f->opacity) / 100.0f;
    const int rz = (int)rad + 2;   /* só os cantos têm cobertura parcial: o resto da miniatura (fora da edição) tem alfa constante, sem sqrt por pixel */
    const unsigned ai_fast = (unsigned)(base * (on ? 1.0f : 0.9f) * 255.0f), kmul = ai_fast * 257u;
    for (int y = 0; y < wh; y++) for (int x = 0; x < w; x++) {
        if (!edit && y >= ty && !((x < rz || x >= w - rz) && (y - ty < rz || y - ty >= h - rz))) {
            unsigned c = px[y * w + x]; unsigned r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
            px[y * w + x] = (ai_fast << 24) | (((r * kmul) >> 16) << 16) | (((g * kmul) >> 16) << 8) | ((b * kmul) >> 16);
            continue;
        }
        float d = rr_dist(x + 0.5f - w * 0.5f, y + 0.5f - (ty + h * 0.5f), w * 0.5f, h * 0.5f, rad), cov = 0.5f - d; cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
        float a = cov * (on ? base : base * 0.9f);
        if (edit) { float d2 = rr_dist(x + 0.5f - w * 0.5f, y + 0.5f - TH * 0.5f, w * 0.5f, TH * 0.5f, TH * 0.5f), c2 = 0.5f - d2; c2 = c2 < 0 ? 0 : c2 > 1 ? 1 : c2; c2 *= 0.97f; if (y < TH + 2 && c2 > a) a = c2; }
        unsigned c = px[y * w + x]; unsigned r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
        px[y * w + x] = ((unsigned)(a * 255) << 24) | ((unsigned)(r * a) << 16) | ((unsigned)(g * a) << 8) | (unsigned)(b * a);
    }
    POINT wp = { t->x, t->y - ty }, src = { 0, 0 }; SIZE sz = { w, wh }; BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    LONG ex = GetWindowLongW(t->wnd, GWL_EXSTYLE); SetWindowLongW(t->wnd, GWL_EXSTYLE, t->drag ? (ex & ~WS_EX_TRANSPARENT) : (ex | WS_EX_TRANSPARENT));
    UpdateLayeredWindow(t->wnd, screen, &wp, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    snap_keep(2 + k, px, w, wh, wp.x, wp.y);
    if (!g_noshow) { if (!IsWindowVisible(t->wnd)) ShowWindow(t->wnd, SW_SHOWNOACTIVATE); SetWindowPos(t->wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); }
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL, screen);
}
/* o timer de 33 ms (~30 quadros/s) só existe com alguma miniatura LIGADA na tela (nada roda para as ocultas) */
static void paint_tiles(const frame_t *f) {
    lv_open(f->lmap);
    int any = 0;
    for (int k = 0; k < NT; k++) { paint_tile(k, f); if (T[k].shown && k < f->ntile && f->t[k].on && g_lvp) any = 1; }
    if (any && hwnd) SetTimer(hwnd, 1, 16, NULL); else if (hwnd) KillTimer(hwnd, 1);
}
static void tiles_poll(void) { for (int k = 0; k < NT; k++) if (T[k].shown && k < cur.ntile && cur.t[k].on && lv_poll(k)) paint_tile(k, &cur); }

/* ---- interação com as miniaturas (modo de edição): arrastar, − / + / roda (escala), × (oculta) e Ligar ---- */
enum { TH_NONE, TH_MOVE, TH_MINUS, TH_PLUS, TH_CLOSE };
static int tile_hit(int k, int x, int y) {
    const tile_t *t = &T[k]; if (!t->drag || !t->shown || x < 0 || y < 0 || x >= t->w || y >= t->h + t->ty) return TH_NONE;
    if (rc_in(&t->bminus, x, y)) return TH_MINUS; if (rc_in(&t->bplus, x, y)) return TH_PLUS; if (rc_in(&t->bclose, x, y)) return TH_CLOSE;
    return TH_MOVE;
}
static void tile_send(int k) {   /* soltou ou mudou a escala: "tile <chave> <monitor> x y %" (e já vale na cópia local) */
    const tile_t *t = &T[k];
    HMONITOR m = MonitorFromPoint((POINT){ t->x + t->w / 2, t->y + t->h / 2 }, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi; mi.cbSize = sizeof mi; if (!GetMonitorInfoW(m, (MONITORINFO *)&mi)) return;
    char d[40]; WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, d, sizeof d, NULL, NULL);
    tspec_t *s = &cur.t[k]; s->pvalid = 1; snprintf(s->pdev, sizeof s->pdev, "%s", d); s->px = t->x - (int)mi.rcWork.left; s->py = t->y - (int)mi.rcWork.top;
    emit("tile %s %s %d %d %d\n", s->key, d, s->px, s->py, tile_pct(s));
}
static void tile_set_pct(int k, int pct) {
    if (k >= cur.ntile) return; pct = pct < PCT_MIN ? PCT_MIN : pct > PCT_MAX ? PCT_MAX : pct;
    if (pct == tile_pct(&cur.t[k])) return;
    cur.t[k].pct = pct; g_hold = GetTickCount() + 500; paint_tile(k, &cur); tile_send(k);
}
static void tile_scale(int k, int dir) { if (cur.edit && k < cur.ntile && cur.t[k].on && dir) tile_set_pct(k, tile_pct(&cur.t[k]) + (dir > 0 ? 10 : -10)); }
static void tile_set_on(int k, int on) { if (k >= cur.ntile || !!cur.t[k].on == !!on) return; cur.t[k].on = on; emit("tileon %s %d\n", cur.t[k].key, on); g_hold = GetTickCount() + 500; paint_tile(k, &cur); paint_bar(&cur); }
static void tile_down(int k, int x, int y) {
    int h = tile_hit(k, x, y);
    if (h == TH_MINUS) tile_scale(k, -1); else if (h == TH_PLUS) tile_scale(k, 1); else if (h == TH_CLOSE) tile_set_on(k, !cur.t[k].on);
    else if (h == TH_MOVE) { g_tdrag = k; g_tc0 = cursor_pos(); g_tx0 = T[k].x; g_ty0 = T[k].y; if (!g_inject) SetCapture(T[k].wnd); }
}
static void tile_move(int k) {
    if (g_tdrag != k) return; POINT c = cursor_pos();
    T[k].x = g_tx0 + (c.x - g_tc0.x); T[k].y = g_ty0 + (c.y - g_tc0.y);
    POINT p = { T[k].x, T[k].y - T[k].ty }; UpdateLayeredWindow(T[k].wnd, NULL, &p, NULL, NULL, NULL, 0, NULL, 0);   /* só move */
}
static void tile_up(int k) { if (g_tdrag != k) return; g_tdrag = -1; if (!g_inject) ReleaseCapture(); tile_send(k); }

static LRESULT CALLBACK tproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    int k = (int)(GetWindowLongPtrW(h, GWLP_USERDATA) - 1); if (k < 0 || k >= NT) return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_NCHITTEST: return T[k].drag ? HTCLIENT : HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR: if (T[k].drag) { POINT p; GetCursorPos(&p); ScreenToClient(h, &p); int t = tile_hit(k, p.x, p.y); SetCursor(LoadCursor(NULL, g_tdrag == k ? IDC_SIZEALL : t >= TH_MINUS ? IDC_HAND : IDC_SIZEALL)); return TRUE; } break;
    case WM_LBUTTONDOWN: tile_down(k, (short)LOWORD(l), (short)HIWORD(l)); return 0;
    case WM_MOUSEMOVE: tile_move(k); return 0;
    case WM_LBUTTONUP: case WM_CAPTURECHANGED: tile_up(k); return 0;
    case WM_MOUSEWHEEL: tile_scale(k, (short)HIWORD(w) > 0 ? 1 : -1); return 0;
    }
    return DefWindowProcW(h, m, w, l);
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
    case WM_TIMER: if (w == 1 && g_tdrag < 0) tiles_poll(); return 0;   /* ~30 quadros/s por miniatura; o timer só existe com miniatura ligada na tela */
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
    for (int ww = BAR_MINW; ww <= BAR_MAXW; ww += 40) for (int lv = 0; lv <= 6; lv += 6) {
        bar_layout(ww, lv, lv ? NT : 0);
        for (int i = B_ALL; i < B_N; i++) {
            const RECT *r = &g_bh[i]; if (i == B_LIVE && lv <= NT) { if (r->right) geo = 0; continue; }
            if (i >= B_T0 && !r->right) { if (lv) geo = 0; continue; }
            if (r->left < BAR_PAD || r->right > ww - BAR_PAD || r->top < g_barshape.top || r->bottom > g_barshape.bottom) geo = 0;
            if (r->bottom - r->top < 28) tall = 0;
            for (int j = i + 1; j < B_N; j++) { const RECT *q = &g_bh[j]; if (q->right && r->right && r->left < q->right && q->left < r->right && r->top < q->bottom && q->top < r->bottom) over++; }
        }
        if (g_bh[B_SLIDER].right - g_bh[B_SLIDER].left < 48) geo = 0;
        if (g_oplab.right > g_bh[B_OPM].left) geo = 0;
    }
    f += check(geo && !over, "botões sempre dentro da barra (560 a 680 px), sem sobrepor e com o controle de opacidade >= 48 px");
    f += check(tall, "alvos de clique com 28 px ou mais de altura");
    bar_layout(g_bww, 6, 0);
    {   /* textos: com a largura cheia nenhum rótulo precisa de reticências; nas menores a elipse protege (DT_END_ELLIPSIS) */
        HDC dc = CreateCompatibleDC(NULL); HFONT fm = bar_font(13, FW_MEDIUM); HGDIOBJ oo = SelectObject(dc, fm); int fit = 1; SIZE sz;
        bar_layout(BAR_MAXW, 6, 0);
        for (int i = 0; i < 4; i++) { GetTextExtentPoint32W(dc, WG[i].label, (int)wcslen(WG[i].label), &sz); if (sz.cx > g_bh[B_W0 + i].right - g_bh[B_W0 + i].left - 34) fit = 0; }
        GetTextExtentPoint32W(dc, L"Trocar transmissão", 18, &sz); if (sz.cx > g_bh[B_LIVE].right - g_bh[B_LIVE].left - 16) fit = 0;
        GetTextExtentPoint32W(dc, L"Desativar tudo", 14, &sz); if (sz.cx > g_bh[B_ALL].right - g_bh[B_ALL].left - 8) fit = 0;
        GetTextExtentPoint32W(dc, L"Opacidade 100%", 14, &sz); if (sz.cx > g_oplab.right - g_oplab.left) fit = 0;
        SelectObject(dc, oo); DeleteObject(fm); DeleteDC(dc);
        f += check(fit, "na largura cheia todos os textos cabem inteiros (sem reticências)");
        bar_layout(g_bww, 6, 0);
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

/* escreve um quadro de cor lisa (BGRA) na região k da memória compartilhada de teste, como o app faz */
static void lv_fake_write(unsigned char *m, int k, int w, int h, unsigned long long n, unsigned rgb) {
    m += (size_t)k * LV_SLOT; volatile unsigned *seq = (volatile unsigned *)(m + 4); *seq += 1;
    memcpy(m, "BOVL", 4); memcpy(m + 8, &w, 4); memcpy(m + 12, &h, 4); memcpy(m + 16, &n, 8);
    for (int i = 0; i < w * h && i < LV_MAXW * LV_MAXH; i++) ((unsigned *)(m + 64))[i] = 0xff000000u | rgb;
    *seq += 1;
}
static unsigned snap_px(int k, int x, int y) { return g_snap[k].px && x >= 0 && y >= 0 && x < g_snap[k].w && y < g_snap[k].h ? g_snap[k].px[y * g_snap[k].w + x] : 0; }
static unsigned tile_center(int k) { return snap_px(2 + k, T[k].w / 2, T[k].ty + T[k].h / 2); }
#define CH_R(c) (int)(((c) >> 16) & 255)
#define CH_G(c) (int)(((c) >> 8) & 255)
#define CH_B(c) (int)((c) & 255)

/* Miniaturas ao vivo (até 4, cada uma uma janela): leitura da memória compartilhada, escala/posição próprias, liga/desliga, edição, quinas. */
static int live_selftest(frame_t *b, frame_t *o, const char *dev) {
    int f = 0; char nm[64], ln[240]; snprintf(nm, sizeof nm, "Local\\BriksOvlTest-%lu", (unsigned long)GetCurrentProcessId());
    HANDLE mh = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, NT * LV_SLOT, nm);
    unsigned char *m = mh ? (unsigned char *)MapViewOfFile(mh, FILE_MAP_WRITE, 0, 0, 0) : NULL;
    f += check(m != NULL, "memória compartilhada de teste criada (4 regiões)");
    if (!m) return f;
    memset(m, 0, NT * LV_SLOT > 4096 ? 4096 : NT * LV_SLOT);
    lv_fake_write(m, 0, 160, 90, 1, 0xff0000); lv_fake_write(m, 1, 320, 180, 1, 0x00ff00); lv_fake_write(m, 2, 320, 180, 1, 0x0000ff); lv_fake_write(m, 3, 320, 180, 1, 0xffff00);
    RECT wk; { HMONITOR pm = MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY); MONITORINFO mi; mi.cbSize = sizeof mi; GetMonitorInfoW(pm, &mi); wk = mi.rcWork; }
    apply_line(b, o, "cfg 90 0"); apply_line(b, o, "edit 0"); apply_line(b, o, "flags 15"); apply_line(b, o, "lives 4"); snprintf(ln, sizeof ln, "livemap %s", nm); apply_line(b, o, ln);
    apply_line(b, o, "begin"); apply_line(b, o, "i Sala conectada");
    apply_line(b, o, "tile 0 aaaa1 1 Rafael Lima Souza de Albuquerque Montenegro Filho"); apply_line(b, o, "tile 1 bbbb1 1 Marina"); apply_line(b, o, "tile 2 cccc1 0 Oculta"); apply_line(b, o, "tile 3 dddd1 1 Quarta");
    snprintf(ln, sizeof ln, "tplace 1 %s 300 200 150", dev); apply_line(b, o, ln);
    apply_line(b, o, "end");
    f += check(o->ntile == 4 && !strcmp(o->t[0].key, "aaaa1") && o->t[0].on && !o->t[2].on && !wcscmp(o->t[1].name, L"Marina") && o->t[1].pct == 150 && o->t[1].pvalid, "linhas 'tile' e 'tplace' entram no quadro (até 4, com chave, ligada, nome, posição e escala)");
    apply_line(b, o, "begin"); apply_line(b, o, "tile 9 x 1 y"); apply_line(b, o, "tplace 5 d 1 1 1"); apply_line(b, o, "tile 0 k 1"); f += check(b->ntile == 1 && b->t[0].on, "tile fora de 0-3 é ignorado; tplace de miniatura inexistente também");
    apply_line(b, o, "begin"); apply_line(b, o, "i Sala conectada");
    apply_line(b, o, "tile 0 aaaa1 1 Rafael Lima Souza de Albuquerque Montenegro Filho"); apply_line(b, o, "tile 1 bbbb1 1 Marina"); apply_line(b, o, "tile 2 cccc1 0 Oculta"); apply_line(b, o, "tile 3 dddd1 1 Quarta");
    snprintf(ln, sizeof ln, "tplace 1 %s 300 200 150", dev); apply_line(b, o, ln); apply_line(b, o, "end");
    cur = *o; paint(&cur); for (int k = 0; k < NT; k++) lv_poll(k); for (int k = 0; k < NT; k++) paint_tile(k, &cur);
    f += check(T[0].shown && T[1].shown && !T[2].shown && T[3].shown, "fora da edição só aparecem as miniaturas ligadas (a oculta não tem janela na tela)");
    f += check(T[0].w == 320 && T[0].h == 180 && T[1].w == 480 && T[1].h == 270, "escala própria: 100% = 320x180, 150% = 480x270 (16:9)");
    { RECT r; GetWindowRect(T[1].wnd, &r); f += check(r.left == wk.left + 300 && r.top == wk.top + 200 && r.right - r.left == 480, "posição salva da miniatura 2, relativa à área útil do monitor"); }
    { RECT a, c; GetWindowRect(T[0].wnd, &a); GetWindowRect(T[3].wnd, &c); f += check(!(a.left < c.right && c.left < a.right && a.top < c.bottom && c.top < a.bottom), "sem posição salva, as miniaturas ficam empilhadas sem sobrepor"); }
    f += check(CH_R(tile_center(0)) > 150 && CH_G(tile_center(0)) < 40 && CH_G(tile_center(1)) > 150 && CH_R(tile_center(1)) < 40 && CH_R(tile_center(3)) > 150 && CH_G(tile_center(3)) > 150, "cada miniatura mostra o quadro da própria região (vermelho, verde, amarelo)");
    { int a = (int)(snap_px(2, T[0].w / 2, T[0].h / 2) >> 24); f += check(a > 215 && a < 245, "a opacidade do overlay (90%) vale para a imagem"); }
    for (int k = 0; k < NT; k++) if (T[k].shown) {
        const int w = T[k].w, h = T[k].h;
        int ok = (snap_px(2 + k, 0, 0) >> 24) < 8 && (snap_px(2 + k, w - 1, 0) >> 24) < 8 && (snap_px(2 + k, 0, h - 1) >> 24) < 8 && (snap_px(2 + k, w - 1, h - 1) >> 24) < 8 && (snap_px(2 + k, 6, 6) >> 24) > 150;
        snprintf(ln, sizeof ln, "miniatura %d: quinas transparentes e raio 10 (nenhum canto reto)", k + 1); f += check(ok, ln);
    }
    f += check(CH_R(snap_px(2, 14, 20)) > 150 && CH_G(snap_px(2, 14, 20)) < 90, "selo 'Ao vivo' vermelho no canto superior esquerdo");
    { LONG ex = GetWindowLongW(T[0].wnd, GWL_EXSTYLE); f += check((ex & WS_EX_TRANSPARENT) && (ex & WS_EX_NOACTIVATE) && (ex & WS_EX_TOPMOST) && SendMessageW(T[0].wnd, WM_NCHITTEST, 0, 0) == HTTRANSPARENT, "miniatura fora da edição: clique atravessa, não ativa, no topo"); }
    f += check(lv_poll(0) == 0 && lv_poll(1) == 0, "sem quadro novo não há repintura (baixo consumo)");
    lv_fake_write(m, 1, 320, 180, 2, 0xff00ff); f += check(lv_poll(1) == 1 && g_lvn[1] == 2 && lv_poll(0) == 0, "quadro novo numa região só mexe naquela miniatura");
    paint_tile(1, &cur); f += check(CH_R(tile_center(1)) > 150 && CH_B(tile_center(1)) > 150 && CH_G(tile_center(1)) < 40, "o quadro novo (magenta) aparece só na miniatura 2");
    lv_fake_write(m, 3, 320, 180, 0, 0); f += check(lv_poll(3) == 1 && !g_lvvalid[3], "n = 0 (app parou de enviar) apaga a imagem daquela miniatura");
    paint_tile(3, &cur); f += check(CH_R(tile_center(3)) < 70 && CH_G(tile_center(3)) < 70, "sem imagem fica o fundo escuro com 'Carregando transmissão…'");
    { int bw = 4000; unsigned long long n9 = 9; memcpy(m + 2 * LV_SLOT + 8, &bw, 4); memcpy(m + 2 * LV_SLOT + 12, &bw, 4); memcpy(m + 2 * LV_SLOT + 16, &n9, 8); f += check(lv_poll(2) <= 1 && !g_lvvalid[2], "cabeçalho com dimensão absurda é ignorado"); }
    lv_fake_write(m, 3, 320, 180, 3, 0xffff00); lv_poll(3);

    /* ---- modo de edição ---- */
    apply_line(b, o, "edit 1"); apply_line(b, o, "end"); cur = *o; g_inject = 1;
    paint(&cur); for (int k = 0; k < NT; k++) paint_tile(k, &cur);
    f += check(T[0].shown && T[1].shown && T[2].shown && T[3].shown && T[2].edit, "em edição a miniatura oculta também aparece (esmaecida, com 'Ligar')");
    { RECT r; GetWindowRect(T[1].wnd, &r); f += check(r.left == wk.left + 300 && r.top == wk.top + 200 - (TH + 4) && r.bottom - r.top == 270 + TH + 4, "a alça fica ACIMA da miniatura: a imagem não muda de lugar ao entrar na edição"); }
    f += check(!(GetWindowLongW(T[0].wnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) && SendMessageW(T[0].wnd, WM_NCHITTEST, 0, 0) == HTCLIENT, "em edição a miniatura recebe o mouse");
    { int geo = 1; for (int k = 0; k < NT; k++) { const tile_t *t = &T[k]; const RECT *rr[4] = { &t->bminus, &t->bplus, &t->bclose, &t->bpct }; for (int q = 0; q < 4; q++) if (rr[q]->right && (rr[q]->left < 0 || rr[q]->right > t->w || rr[q]->top < 0 || rr[q]->bottom > TH)) geo = 0; if (t->bminus.right && t->bplus.right && !(t->bminus.right <= t->bplus.left)) geo = 0; if (t->bclose.right && t->bplus.right && !(t->bplus.right <= t->bclose.left)) geo = 0; }
      f += check(geo, "botões da alça (− / + / ×) sempre dentro da alça e sem sobrepor, em qualquer escala"); }
    f += check(snap_px(2, 0, 0) >> 24 < 8 && snap_px(2, T[0].w - 1, 0) >> 24 < 8 && snap_px(2, 0, T[0].ty + T[0].h - 1) >> 24 < 8 && snap_px(2, 2, TH / 2) > 0, "edição: quinas da alça e da miniatura arredondadas (transparentes)");
    CLR(); { const RECT *p = &T[0].bplus; tile_down(0, (p->left + p->right) / 2, (p->top + p->bottom) / 2); }
    snprintf(ln, sizeof ln, "tile aaaa1 %s ", dev); f += check(strstr(g_out, ln) && strstr(g_out, " 110\n") && tile_pct(&cur.t[0]) == 110 && T[0].w == 352, "'+' sobe a escala 10% e manda 'tile <chave> <monitor> x y %'");
    CLR(); { const RECT *p = &T[0].bminus; tile_down(0, (p->left + p->right) / 2, (p->top + p->bottom) / 2); tile_down(0, (T[0].bminus.left + T[0].bminus.right) / 2, (T[0].bminus.top + T[0].bminus.bottom) / 2); }
    f += check(tile_pct(&cur.t[0]) == 90 && strstr(g_out, " 90\n"), "'−' desce 10% de cada vez");
    CLR(); tile_scale(0, 1); tile_scale(0, 1); f += check(tile_pct(&cur.t[0]) == 110, "a roda do mouse sobre a miniatura também muda a escala (±10%)");
    for (int i = 0; i < 30; i++) tile_scale(0, 1); f += check(tile_pct(&cur.t[0]) == PCT_MAX, "escala limitada a 200%"); for (int i = 0; i < 40; i++) tile_scale(0, -1); f += check(tile_pct(&cur.t[0]) == PCT_MIN, "e a 50%");
    tile_set_pct(0, 100);
    { RECT r0; GetWindowRect(T[1].wnd, &r0); const int x0 = T[1].x, y0 = T[1].y; CLR();
      g_inj = (POINT){ 600, 600 }; tile_down(1, 30, T[1].ty + 40); f += check(g_tdrag == 1, "clicar no corpo da miniatura começa o arraste");
      g_inj = (POINT){ 640, 625 }; tile_move(1); { RECT r1; GetWindowRect(T[1].wnd, &r1); f += check(r1.left == r0.left + 40 && r1.top == r0.top + 25, "arrastar move só aquela miniatura junto com o cursor"); }
      tile_up(1); snprintf(ln, sizeof ln, "tile bbbb1 %s %d %d 150\n", dev, x0 + 40 - (int)wk.left, y0 + 25 - (int)wk.top); f += check(strstr(g_out, ln) != NULL && g_tdrag < 0, "ao soltar manda a posição (relativa ao monitor) e a escala");
      f += check(cur.t[1].pvalid && cur.t[1].px == x0 + 40 - wk.left, "a posição nova vale na hora (sem esperar o app)"); }
    f += check(T[0].x != T[1].x || T[0].y != T[1].y, "as outras miniaturas ficam onde estavam");
    CLR(); { const RECT *p = &T[0].bclose; tile_down(0, (p->left + p->right) / 2, (p->top + p->bottom) / 2); }
    f += check(strstr(g_out, "tileon aaaa1 0\n") && !cur.t[0].on && T[0].shown, "× oculta só aquela transmissão ('tileon <chave> 0') e a alça vira 'Ligar'");
    lv_poll(0); f += check(!cur.t[0].on, "oculta fica oculta (a leitura de quadros do overlay só roda nas ligadas)");
    CLR(); { const RECT *p = &T[0].bclose; tile_down(0, (p->left + p->right) / 2, (p->top + p->bottom) / 2); } f += check(strstr(g_out, "tileon aaaa1 1\n") && cur.t[0].on, "'Ligar' volta a mostrar ('tileon <chave> 1')");
    CLR(); { const RECT *p = &T[2].bclose; tile_down(2, (p->left + p->right) / 2, (p->top + p->bottom) / 2); } f += check(strstr(g_out, "tileon cccc1 1\n") && cur.t[2].on, "'Ligar' numa miniatura que estava oculta");
    { frame_t st = *o; st.t[2].on = 0; st.t[2].pct = 100; g_hold = GetTickCount() + 400; adopt_frame(&st); f += check(cur.t[2].on, "quadro velho do app logo após o clique não desfaz o liga/desliga");
      g_hold = 0; adopt_frame(&st); f += check(!cur.t[2].on, "passada a espera, o app (Configurações) prevalece: sincronia nos dois sentidos"); }
    /* barra: linha 3 com uma chave por miniatura */
    paint_bar(&cur); f += check(g_bh[B_T0].right && g_bh[B_T1].right && g_bh[B_T2].right && g_bh[B_T3].right && g_bh[B_T0].bottom < g_bh[B_ALL].top, "a barra ganha a 3ª linha com uma chave por miniatura (até 4)");
    CLR(); bar_down(CTR(B_T1)); f += check(strstr(g_out, "tileon bbbb1 0\n") && !cur.t[1].on, "chave da barra oculta só a transmissão 2 ('tileon bbbb1 0')");
    CLR(); bar_down(CTR(B_T1)); f += check(strstr(g_out, "tileon bbbb1 1\n") && cur.t[1].on, "e liga de novo");
    { frame_t few = cur; few.ntile = 2; paint_bar(&few); f += check(g_bh[B_T1].right && !g_bh[B_T2].right, "com 2 transmissões a barra só tem 2 chaves"); frame_t none = cur; none.ntile = 0; paint_bar(&none); f += check(!g_bh[B_T0].right, "sem transmissões não há a 3ª linha"); paint_bar(&cur); }
    { frame_t many = cur; many.lives = 7; paint_bar(&many); f += check(g_bh[B_LIVE].right != 0, "'Trocar transmissão' só aparece com mais de 4 transmissões"); frame_t four = cur; four.lives = 4; paint_bar(&four); f += check(!g_bh[B_LIVE].right, "com 4 ou menos não aparece"); paint_bar(&cur); }
    /* encerra: sem tiles nem livemap solta tudo */
    apply_line(b, o, "livemap "); apply_line(b, o, "edit 0"); apply_line(b, o, "begin"); apply_line(b, o, "i x"); apply_line(b, o, "end"); cur = *o; paint(&cur);
    f += check(g_lvp == NULL && !T[0].shown && !T[1].shown && !T[2].shown && !T[3].shown, "sem miniaturas e sem livemap: solta a memória e esconde todas as janelas");
    g_inject = 1; UnmapViewOfFile(m); CloseHandle(mh); return f;
}

/* Teste do modo de edição, sem mostrar janela e com o cursor injetado: arrastar, redimensionar, Concluir, por monitor. */
static int edit_selftest(void) {
    int f = 0; g_noshow = 1; g_capture_out = 1; g_inject = 1; g_snap_on = 1;
    HMONITOR pm = MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY); MONITORINFOEXW mi; mi.cbSize = sizeof mi; GetMonitorInfoW(pm, (MONITORINFO *)&mi);
    char dev[40]; WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, dev, sizeof dev, NULL, NULL);
    frame_t b, o; memset(&b, 0, sizeof b); memset(&o, 0, sizeof o); char ln[160];
    apply_line(&b, &o, "cfg 90 0"); apply_line(&b, &o, "edit 1"); apply_line(&b, &o, "flags 15"); apply_line(&b, &o, "lives 6"); apply_line(&b, &o, "begin"); apply_line(&b, &o, "p 1 Marina"); apply_line(&b, &o, "p 0 Rafael");
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
    f += live_selftest(&b, &o, dev);
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
    char mn[64]; snprintf(mn, sizeof mn, "Local\\BriksOvlShot-%lu", (unsigned long)GetCurrentProcessId());
    HANDLE mh = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, NT * LV_SLOT, mn);
    unsigned char *mm = mh ? (unsigned char *)MapViewOfFile(mh, FILE_MAP_WRITE, 0, 0, 0) : NULL;
    if (mm) {   /* 4 "jogos" de mentira (16:9) com cores diferentes: céu, chão e um personagem */
        for (int k = 0; k < NT; k++) {
            lv_fake_write(mm, k, 320, 180, 1, 0);
            unsigned *dst = (unsigned *)(mm + (size_t)k * LV_SLOT + 64);
            for (int y = 0; y < 180; y++) for (int x = 0; x < 320; x++) {
                unsigned r = y < 120 ? 60 + y / 2 : 40, g = y < 120 ? 120 + y / 3 : 150 - (y - 120), bl = y < 120 ? 220 - y / 2 : 60, t;
                if (k == 1) { t = r; r = bl / 2 + 40; bl = t; } else if (k == 2) { t = g; g = r; r = t; } else if (k == 3) { r = r / 2 + 90; g = g / 2; }
                if (x > 140 + k * 8 && x < 180 + k * 8 && y > 70 && y < 140) { r = 240; g = 190; bl = 60; }
                dst[y * 320 + x] = 0xff000000u | (r << 16) | (g << 8) | bl;
            }
        }
        char lm[100]; snprintf(lm, sizeof lm, "livemap %s", mn); apply_line(&b, &o, lm);
    }
    apply_line(&b, &o, "cfg 86 0"); apply_line(&b, &o, "edit 1"); apply_line(&b, &o, "flags 11"); apply_line(&b, &o, "lives 4");
    apply_line(&b, &o, "begin"); apply_line(&b, &o, "i Sala conectada"); apply_line(&b, &o, "p 1 Marina Costa"); apply_line(&b, &o, "p 0 Rafael Lima"); apply_line(&b, &o, "p 0 Você");
    apply_line(&b, &o, "tile 0 k1 1 Rafael Lima"); apply_line(&b, &o, "tile 1 k2 1 Marina Costa"); apply_line(&b, &o, "tile 2 k3 0 Júlia Andrade"); apply_line(&b, &o, "tile 3 k4 1 Pedro");
    {   /* posições salvas: uma coluna no lado direito, tamanhos diferentes */
        MONITORINFOEXW mi; mi.cbSize = sizeof mi; GetMonitorInfoW(MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), (MONITORINFO *)&mi);
        char dev[40], ln[120]; WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, dev, sizeof dev, NULL, NULL);
        const int W = (int)(mi.rcWork.right - mi.rcWork.left);
        snprintf(ln, sizeof ln, "tplace 0 %s %d 60 100", dev, W - 720); apply_line(&b, &o, ln);
        snprintf(ln, sizeof ln, "tplace 1 %s %d 330 70", dev, W - 720); apply_line(&b, &o, ln);
        snprintf(ln, sizeof ln, "tplace 2 %s %d 520 100", dev, W - 720); apply_line(&b, &o, ln);
        snprintf(ln, sizeof ln, "tplace 3 %s 40 360 100", dev); apply_line(&b, &o, ln);
    }
    apply_line(&b, &o, "end");
    cur = o; g_bhover = B_W1; paint(&cur); for (int k = 0; k < NT; k++) lv_poll(k); for (int k = 0; k < NT; k++) paint_tile(k, &cur);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0); const int cw = wa.right - wa.left, ch = wa.bottom - wa.top;
    unsigned char *rgb = (unsigned char *)malloc((size_t)cw * ch * 3); if (!rgb) return 1;
    for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) {   /* "jogo": céu em degradê com blocos */
        unsigned char *d = rgb + ((size_t)y * cw + x) * 3; int t = y * 255 / ch;
        d[0] = (unsigned char)(70 + t / 3 + ((x / 90 + y / 90) % 2) * 14); d[1] = (unsigned char)(110 - t / 4 + ((x / 90 + y / 90) % 2) * 10); d[2] = (unsigned char)(150 - t / 3);
    }
    for (int k = 0; k < 2 + NT; k++) blend_snap(rgb, cw, ch, k, wa.left, wa.top);
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
    { WNDCLASSW tc = { 0 }; tc.lpfnWndProc = tproc; tc.hInstance = inst; tc.lpszClassName = L"BriksOverlayTile"; tc.hCursor = LoadCursor(NULL, IDC_ARROW); RegisterClassW(&tc);
      for (int k = 0; k < NT; k++) { T[k].wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, tc.lpszClassName, L"Briks overlay - transmissao", WS_POPUP, 0, 0, 10, 10, NULL, NULL, inst, NULL); if (T[k].wnd) SetWindowLongPtrW(T[k].wnd, GWLP_USERDATA, k + 1); } }
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
