/* Barra de título própria (TRY-09): o CUI cria a janela com borda do sistema; aqui a área cliente passa a ocupar a janela toda (WM_NCCALCSIZE)
 * e o Briks desenha a barra de vidro: nome, versão e minimizar / maximizar-restaurar / fechar.
 *   - arrastar pela barra e duplo clique maximizar são do próprio Windows (WM_NCHITTEST devolve HTCAPTION fora dos botões);
 *   - os botões ficam na área cliente (HTCLIENT) para o CUI tratar o mouse; fechar manda WM_CLOSE (bandeja quando configurado: winstate.c);
 *   - bordas finas de 6 px (HTLEFT, HTTOPLEFT...) redimensionam, já que a borda do sistema deixou de existir;
 *   - o Windows 11 continua arredondando os cantos da janela; os botões são pílulas internas com raio próprio (sem cantos retos vazando).
 * Em automação (janela oculta) os comandos só são contados: nunca mostram, minimizam nem maximizam uma janela de verdade. */
#include "briks_ui.h"
#include "sysint.h"
#include "update.h"
#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TB_H 36            /* altura lógica */
#define TB_BW 40           /* largura de cada botão */
#define TB_BH 26
#define TB_GAP 4
#define TB_MARGIN 8
#define TB_EDGE 6
enum { TBA_MIN = 0, TBA_MAX = 1, TBA_CLOSE = 2 };

static float g_scale = 1.0f;
static int g_count[3], g_last = -1, g_drag_hint;
static HWND g_hwnd;

static float scale(void) { return g_scale < 0.5f ? 1.0f : g_scale; }

void titlebar_action(int a) {
    if (a < 0 || a > 2) return;
    g_count[a]++; g_last = a;
    HWND h = g_hwnd ? g_hwnd : (HWND)sys_hwnd();
    if (!h) return;
    if (a == TBA_CLOSE) { PostMessageW(h, WM_CLOSE, 0, 0); return; }   /* winstate.c decide: bandeja ou sair */
    if (sys_is_headless()) return;                                      /* janela oculta de teste: só conta */
    if (a == TBA_MIN) ShowWindow(h, SW_MINIMIZE);
    else ShowWindow(h, IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE);
}

/* Resultado do WM_NCHITTEST para um ponto em pixels de tela relativos ao canto da janela (cx, cy) numa janela w x h. Puro: serve ao roteiro de teste. */
int titlebar_hit(int cx, int cy, int w, int h, bool maximized, float sc) {
    const int edge = maximized ? 0 : (int)(TB_EDGE * sc + 0.5f), bar = (int)(TB_H * sc + 0.5f);
    const bool l = cx < edge, r = cx >= w - edge, t = cy < edge, b = cy >= h - edge;
    if (t && l) return HTTOPLEFT; if (t && r) return HTTOPRIGHT; if (b && l) return HTBOTTOMLEFT; if (b && r) return HTBOTTOMRIGHT;
    if (l) return HTLEFT; if (r) return HTRIGHT; if (t) return HTTOP; if (b) return HTBOTTOM;
    if (cy < bar) {
        const int btns = (int)((3 * TB_BW + 2 * TB_GAP + TB_MARGIN) * sc + 0.5f);
        return cx >= w - btns ? HTCLIENT : HTCAPTION;
    }
    return HTCLIENT;
}

/* Chamado primeiro por sys_proc (winstate.c). Devolve true quando tratou a mensagem (resultado em *res). */
bool titlebar_proc(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res) {
    g_hwnd = h;
    switch (m) {
    case WM_NCCALCSIZE:
        if (w) {   /* sem moldura nem título do sistema: o cliente é a janela toda (maximizada, recua a borda invisível para não passar da tela) */
            NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS *)l;
            if (IsZoomed(h)) {
                const UINT dpi = GetDpiForWindow(h);
                const int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi), fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                p->rgrc[0].left += fx; p->rgrc[0].right -= fx; p->rgrc[0].top += fy; p->rgrc[0].bottom -= fy;
            }
            *res = 0; return true;
        }
        break;
    case WM_NCHITTEST: {
        RECT r; GetWindowRect(h, &r);
        const int hit = titlebar_hit(GET_X_LPARAM(l) - r.left, GET_Y_LPARAM(l) - r.top, r.right - r.left, r.bottom - r.top, IsZoomed(h) != 0, scale());
        if (hit == HTCAPTION) g_drag_hint++;
        *res = hit; return true;
    }
    default: break;
    }
    return false;
}

void titlebar_attach(void *hwnd) {
    g_hwnd = (HWND)hwnd;
    if (g_hwnd) SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);   /* recalcula o cliente sem a moldura */
}

/* Os controles da janela valem também com um diálogo aberto (minimizar/fechar nunca ficam presos atrás de um modal): por isso não passam por briks_blocked. */
static bool tb_inside(cui_ctx *ui, const char *id) {
    cui_rect r = cui_get_rect(ui, cui_make_id(ui, id)); cui_vec2 m = cui_mouse_pos(ui);
    return r.w > 0 && m.x >= r.x && m.y >= r.y && m.x < r.x + r.w && m.y < r.y + r.h;
}
static bool tb_btn(cui_ctx *ui, const char *id, const char *icon, bool danger) {
    const bool hov = tb_inside(ui, id);
    cui_box(ui, .id = id, .width = CUI_PX(TB_BW), .height = CUI_PX(TB_BH), .radius = 8, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER,
            .bg = hov ? (danger ? CUI_RGBA(0xe95555dd) : CUI_RGBA(0xffffff1f)) : CUI_RGBA(0x00000000)) {
        cui_icon(ui, icon, 14, hov && danger ? CUI_RGB(0xffffff) : B_TEXT);
    }
    return hov && cui_mouse_clicked(ui, CUI_MOUSE_LEFT);
}

/* Desenha a barra (primeiro item do quadro, antes do filete de gradiente). */
void titlebar_draw(cui_ctx *ui, app_state_t *s) {
    (void)s;
    g_scale = cui_dpi_scale(ui);
    const bool max = g_hwnd && IsZoomed(g_hwnd);
    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(TB_H), .pad_x = TB_MARGIN + 4, .gap = 10, .align = CUI_ALIGN_CENTER, .bg = CUI_RGBA(0x0c021466)) {
        if (!briks_logo(ui, 22))
        cui_box(ui, .width = CUI_PX(20), .height = CUI_PX(20), .radius = 6, .bg = B_C2, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
            cui_text(ui, "B", .font = CUI_FONT_DISPLAY, .size = 13, .color = CUI_RGB(0xffffff));
        }
        cui_text(ui, "Briks", .font = CUI_FONT_MEDIUM, .size = 13, .color = B_TEXT, .nowrap = true);
        char v[40]; snprintf(v, sizeof v, "v%s", update_current_version());
        cui_text(ui, v, .size = 12, .color = B_MUTED, .nowrap = true);
        cui_box(ui, .width = CUI_FILL, .height = CUI_PX(1)) {}   /* empurra os botões para a direita; a faixa é arrastável (HTCAPTION) */
        cui_row(ui, .gap = TB_GAP, .align = CUI_ALIGN_CENTER) {
            if (tb_btn(ui, "tb-min", CUI_ICON_MINUS, false)) titlebar_action(TBA_MIN);
            if (tb_btn(ui, "tb-max", max ? CUI_ICON_COPY : CUI_ICON_SQUARE, false)) titlebar_action(TBA_MAX);
            if (tb_btn(ui, "tb-close", CUI_ICON_X, true)) titlebar_action(TBA_CLOSE);
        }
    }
}

int titlebar_probe(const char *n) {
    if (!strcmp(n, "tb_min")) return g_count[TBA_MIN];
    if (!strcmp(n, "tb_max")) return g_count[TBA_MAX];
    if (!strcmp(n, "tb_close")) return g_count[TBA_CLOSE];
    if (!strcmp(n, "tb_last")) return g_last;
    if (!strcmp(n, "tb_h")) return (int)(TB_H * scale() + 0.5f);
    if (!strcmp(n, "tb_nc_client")) {   /* o cliente cobre a janela toda? (sem moldura do sistema) */
        RECT w, c; if (!g_hwnd || !GetWindowRect(g_hwnd, &w) || !GetClientRect(g_hwnd, &c)) return -1;
        return (c.right - c.left) == (w.right - w.left) && (c.bottom - c.top) == (w.bottom - w.top);
    }
    return -9999;
}

/* sys tbhit X Y (pixels do cliente) -> guarda o resultado do teste de acerto; sys tbact min|max|close. Lido por expect tb_hit <n>. */
static int g_hit_result = -1;
bool titlebar_script(const char *cmd, int a, int b) {
    if (!strcmp(cmd, "tbhit")) {
        RECT r; if (!g_hwnd || !GetWindowRect(g_hwnd, &r)) return false;
        const int w = r.right - r.left, h = r.bottom - r.top;
        if (a < 0) a += w;   /* negativo = distância da borda direita/inferior (a janela real pode ter outro tamanho por causa do DPI) */
        if (b < 0) b += h;
        g_hit_result = titlebar_hit(a, b, r.right - r.left, r.bottom - r.top, IsZoomed(g_hwnd) != 0, scale());
        return true;
    }
    if (!strcmp(cmd, "tbact")) { titlebar_action(a); return true; }
    return false;
}
int titlebar_hit_result(void) { return g_hit_result; }
