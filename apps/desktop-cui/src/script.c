/* Modo roteiro (BRIKS_SCRIPT=<arquivo>, BRIKS_SCRIPT_OUT=<pasta>): injeta mouse de forma determinística e salva um PNG por quadro.
 * Existe porque a captura de tela do ambiente sai preta: os quadros do vídeo saem do próprio app.
 * Comandos (um por linha, '#' comenta):
 *   caption <texto>        legenda até a próxima (vazio apaga)
 *   at X Y                 põe o cursor em X,Y
 *   to X Y N | to <ponto> N   move o cursor em N quadros (com suavização); <ponto> é grip:chat, zone:nav:left, divider:nav...
 *   down | up              aperta/solta o botão esquerdo
 *   wait N                 espera N quadros
 *   menu X Y               abre o menu 'Adicionar painel' ancorado em X,Y (como o clique no +)
 *   pick <painel>          escolhe o painel no menu "Adicionar painel" (entra no modo "clique numa zona")
 *   esc                    aperta Esc (cancela o arraste)
 *   restore                restaura o layout padrão
 *   voice N | servers N    (demo da pilha) pessoas na call / comunidades na arte do cartão; art <painel> força a arte
 *   demo <modo>            não usado no vídeo; reservado
 *   click X Y              move, aperta e solta o botão esquerdo em X,Y (um clique de verdade, pelo mesmo caminho do mouse)
 *   waitms N               espera N milissegundos de relógio (independe da taxa de quadros)
 *   shot <nome>            salva <nome>.png (com BRIKS_SCRIPT_SHOTS=1 só estes são salvos, não um PNG por quadro)
 *   type <texto>           digita o texto no campo com foco (como o teclado; usado pelo roteiro de paridade)
 *   key enter|tab|backspace|esc   aperta e solta a tecla (idem)
 *   signal <nome>          cria o arquivo <nome>.flag na pasta de saída (um roteiro externo espera por ele para agir) */
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
static bool esc_up; static int key_up = -1;
static double wait_until; static int click_phase; static char shot_name[96]; static bool shots_only;
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
    shots_only = getenv("BRIKS_SCRIPT_SHOTS") != NULL;
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
    if (key_up >= 0) { cui_input_key(ui, (cui_key)key_up, false, 0); key_up = -1; }
    /* o mouse real do usuário não pode interferir na gravação: o roteiro reafirma posição e botão a cada quadro */
    cui_input_mouse_move(ui, cx, cy);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, mdown);
    if (click_phase) {   /* clique em andamento: 3 quadros parado em cima (para o controle ver o cursor), 3 apertado, 3 solto */
        click_phase++;
        if (click_phase == 4) { cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); mdown = true; }
        if (click_phase == 7) { cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); mdown = false; }
        if (click_phase >= 10) click_phase = 0;
        cui_request_redraw(ui);
        return;
    }
    if (wait_until > 0) { if (cui_time(ui) < wait_until) { cui_request_redraw(ui); return; } wait_until = 0; }
    /* inicia operações até achar uma com duração */
    while (!timed_left && !click_phase && wait_until == 0 && !shot_name[0] && pc < nlines) {
        char line[1024]; snprintf(line, sizeof line, "%s", lines[pc++]);
        char *cmd = strtok(line, " ");
        if (!cmd) continue;
        if (!strcmp(cmd, "caption")) { char *r = strtok(NULL, ""); snprintf(caption, sizeof caption, "%s", r ? r : ""); }
        else if (!strcmp(cmd, "at")) { char *a = strtok(NULL, " "), *b = strtok(NULL, " "); if (a && b) { cx = (float)atof(a); cy = (float)atof(b); cui_input_mouse_move(ui, cx, cy); } }
        else if (!strcmp(cmd, "click")) { char *a = strtok(NULL, " "), *b = strtok(NULL, " "); if (a && b) { cx = (float)atof(a); cy = (float)atof(b); cui_input_mouse_move(ui, cx, cy); click_phase = 1; } }
        else if (!strcmp(cmd, "waitms")) { char *n = strtok(NULL, " "); wait_until = cui_time(ui) + (n ? atof(n) : 100) / 1000.0; }
        else if (!strcmp(cmd, "shot")) { char *n = strtok(NULL, " "); snprintf(shot_name, sizeof shot_name, "%s", n ? n : "shot"); }
        else if (!strcmp(cmd, "signal")) { char *n = strtok(NULL, " "); char f[640]; snprintf(f, sizeof f, "%s\\%s.flag", outdir, n ? n : "sinal"); FILE *o = fopen(f, "wb"); if (o) fclose(o); }
        else if (!strcmp(cmd, "down")) { cui_input_mouse_move(ui, cx, cy); cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); mdown = true; }
        else if (!strcmp(cmd, "rdown")) { cui_input_mouse_move(ui, cx, cy); cui_input_mouse_button(ui, CUI_MOUSE_RIGHT, true); }
        else if (!strcmp(cmd, "rup")) { cui_input_mouse_button(ui, CUI_MOUSE_RIGHT, false); }
        else if (!strcmp(cmd, "up")) { cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); mdown = false; }
        else if (!strcmp(cmd, "menu")) { char *a = strtok(NULL, " "), *b = strtok(NULL, " "); if (a && b) dock_script_menu((float)atof(a), (float)atof(b)); }
        else if (!strcmp(cmd, "pick")) { char *k = strtok(NULL, " "); if (k) dock_script_pick(k); }
        else if (!strcmp(cmd, "esc")) { cui_input_key(ui, CUI_KEY_ESCAPE, true, 0); esc_up = true; }
        else if (!strcmp(cmd, "type")) { char *r = strtok(NULL, ""); if (r) cui_input_text(ui, r); }
        else if (!strcmp(cmd, "key")) {
            char *k = strtok(NULL, " "); int kc = !k ? -1 : !strcmp(k, "enter") ? CUI_KEY_ENTER : !strcmp(k, "tab") ? CUI_KEY_TAB : !strcmp(k, "backspace") ? CUI_KEY_BACKSPACE : !strcmp(k, "esc") ? CUI_KEY_ESCAPE : -1;
            if (kc >= 0) { cui_input_key(ui, (cui_key)kc, true, 0); key_up = kc; }
        }
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
    if (px && (shot_name[0] || !shots_only) && cui_app_screenshot(app, px, w, h)) {
        char name[640];
        if (shot_name[0]) snprintf(name, sizeof name, "%s\\%s.png", outdir, shot_name); else snprintf(name, sizeof name, "%s\\frame_%05d.png", outdir, frame_no++);
        cui_write_png(name, px, w, h, w * 4);
    }
    shot_name[0] = 0;
    if (tail > 20) { cui_app_quit(app); return true; }
    return false;
}
