/* Modo roteiro (BRIKS_SCRIPT=<arquivo>, BRIKS_SCRIPT_OUT=<pasta>): injeta mouse de forma determinística e salva um PNG por quadro.
 * Existe porque a captura de tela do ambiente sai preta: os quadros do vídeo saem do próprio app.
 * Comandos (um por linha, '#' comenta):
 *   caption <texto>        legenda até a próxima (vazio apaga)
 *   at X Y                 põe o cursor em X,Y
 *   to X Y N | to <ponto> N   move o cursor em N quadros (com suavização); <ponto> é grip:chat, zone:nav:left, divider:nav...
 *   down | up              aperta/solta o botão esquerdo
 *   wait N                 espera N quadros
 *   pick <painel>          escolhe o painel no menu "Adicionar painel" (entra no modo "clique numa zona")
 *   esc                    aperta Esc (cancela o arraste)
 *   restore                restaura o layout padrão
 *   voice N | servers N    (demo da pilha) pessoas na call / comunidades na arte do cartão; art <painel> força a arte
 *   demo <modo>            não usado no vídeo; reservado */
#include "briks_ui.h"
#include "drag_stack_ui.h"
#include "layout.h"
static int demo_voice_n = -1, demo_servers_n = -1;
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>

static char *lines[512]; static int nlines, pc;
static int timed_left, timed_total;       /* operação com duração em andamento */
static bool timed_move; static float mx0, my0, mx1, my1;
static float cx, cy; static bool mdown;
static char caption[200];
static int frame_no, tail;
static bool esc_up;
static char outdir[512];
static unsigned char *px;

bool script_load(void) {
    const char *path = getenv("BRIKS_SCRIPT");
    if (!path || !path[0]) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char buf[1024];
    while (fgets(buf, sizeof buf, f) && nlines < 512) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
        if (!n || buf[0] == '#') continue;
        lines[nlines++] = _strdup(buf);
    }
    fclose(f);
    const char *o = getenv("BRIKS_SCRIPT_OUT");
    snprintf(outdir, sizeof outdir, "%s", o && o[0] ? o : "script-frames");
    CreateDirectoryA(outdir, NULL);
    cx = 640; cy = 400;
    return nlines > 0;
}

static float ease(float t) { return t * t * (3 - 2 * t); }

static bool resolve(const char *tok, const char *tok2, float *x, float *y, const char **rest) {
    if (tok[0] >= '0' && tok[0] <= '9') { *x = (float)atof(tok); *y = (float)atof(tok2); return true; }
    (void)rest;
    return dock_script_point(tok, x, y);
}

void script_tick(cui_ctx *ui) {
    if (esc_up) { cui_input_key(ui, CUI_KEY_ESCAPE, false, 0); esc_up = false; }
    /* o mouse real do usuário não pode interferir na gravação: o roteiro reafirma posição e botão a cada quadro */
    cui_input_mouse_move(ui, cx, cy);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, mdown);
    /* inicia operações até achar uma com duração */
    while (!timed_left && pc < nlines) {
        char line[1024]; snprintf(line, sizeof line, "%s", lines[pc++]);
        char *cmd = strtok(line, " ");
        if (!cmd) continue;
        if (!strcmp(cmd, "caption")) { char *r = strtok(NULL, ""); snprintf(caption, sizeof caption, "%s", r ? r : ""); }
        else if (!strcmp(cmd, "at")) { char *a = strtok(NULL, " "), *b = strtok(NULL, " "); if (a && b) { cx = (float)atof(a); cy = (float)atof(b); cui_input_mouse_move(ui, cx, cy); } }
        else if (!strcmp(cmd, "down")) { cui_input_mouse_move(ui, cx, cy); cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); mdown = true; }
        else if (!strcmp(cmd, "up")) { cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); mdown = false; }
        else if (!strcmp(cmd, "pick")) { char *k = strtok(NULL, " "); if (k) dock_script_pick(k); }
        else if (!strcmp(cmd, "esc")) { cui_input_key(ui, CUI_KEY_ESCAPE, true, 0); esc_up = true; }
        else if (!strcmp(cmd, "restore")) dock_script_restore();
        else if (!strcmp(cmd, "art")) { char *n = strtok(NULL, " "); drag_stack_set_art(n ? panel_from_name(n) : -1); }
        else if (!strcmp(cmd, "voice")) { char *n = strtok(NULL, " "); demo_voice_n = n ? atoi(n) : -1; drag_stack_set_demo(demo_voice_n, demo_servers_n); }
        else if (!strcmp(cmd, "servers")) { char *n = strtok(NULL, " "); demo_servers_n = n ? atoi(n) : -1; drag_stack_set_demo(demo_voice_n, demo_servers_n); }
        else if (!strcmp(cmd, "wait")) { char *n = strtok(NULL, " "); timed_total = timed_left = n ? atoi(n) : 1; timed_move = false; }
        else if (!strcmp(cmd, "to")) {
            char *a = strtok(NULL, " "), *b = strtok(NULL, " "), *c = NULL;
            float tx, ty; bool ok;
            if (a && a[0] >= '0' && a[0] <= '9') { c = strtok(NULL, " "); ok = resolve(a, b ? b : "0", &tx, &ty, NULL); }
            else { c = b; ok = a && resolve(a, "0", &tx, &ty, NULL); }
            if (ok) { mx0 = cx; my0 = cy; mx1 = tx; my1 = ty; timed_total = timed_left = c ? atoi(c) : 20; if (timed_total < 1) timed_total = timed_left = 1; timed_move = true; }
            else fprintf(stderr, "roteiro: ponto inválido na linha %d\n", pc);
        }
    }
    if (timed_left) {
        if (timed_move) {
            float t = ease((float)(timed_total - timed_left + 1) / (float)timed_total);
            cx = mx0 + (mx1 - mx0) * t; cy = my0 + (my1 - my0) * t;
            cui_input_mouse_move(ui, cx, cy);
        }
        timed_left--;
    } else if (pc >= nlines) tail++;
}

/* cursor (anel com ponto) e legenda, desenhados como caixas comuns (não interceptam o mouse) */
void script_overlay(cui_ctx *ui) {
    float r = mdown ? 15.0f : 11.0f;
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_START,
            .pad_left = cx - r, .pad_top = cy - r) {
        cui_box(ui, .width = CUI_PX(r * 2), .height = CUI_PX(r * 2), .radius = r, .bg = mdown ? CUI_RGBA(0xa78bfacc) : CUI_RGBA(0xffffffcc),
                .border = 2, .border_color = mdown ? CUI_RGB(0xffffff) : CUI_RGB(0x6d28d9)) {}
    }
    if (caption[0]) {
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_START, .pad = 14) {
            cui_box(ui, .pad_x = 22, .pad_y = 12, .radius = 16, .bg = CUI_RGBA(0x0c0214ee), .border = 1, .border_color = CUI_RGBA(0xa78bfa88)) {
                cui_text(ui, caption, .font = CUI_FONT_MEDIUM, .size = 18, .color = CUI_RGB(0xffffff));
            }
        }
    }
}

bool script_capture(cui_app *app) {
    cui_ctx *ui = cui_app_ui(app);
    cui_vec2 vp = cui_viewport_size(ui);
    float sc = cui_dpi_scale(ui);
    int w = (int)(vp.x * sc + 0.5f), h = (int)(vp.y * sc + 0.5f);
    if (!px) px = malloc((size_t)w * (size_t)h * 4);
    if (px && cui_app_screenshot(app, px, w, h)) {
        char name[640]; snprintf(name, sizeof name, "%s\\frame_%05d.png", outdir, frame_no++);
        cui_write_png(name, px, w, h, w * 4);
    }
    if (tail > 20) { cui_app_quit(app); return true; }
    return false;
}
