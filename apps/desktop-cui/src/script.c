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
 *   key enter|tab|backspace|esc   aperta e solta a tecla (idem); também com modificadores: key ctrl+a, ctrl+shift+z, ctrl+v, home, shift+end...
 *   rclick X Y | rclick <ponto>   clique com o botão direito de verdade (abre menus de contexto)
 *   drop <arquivo>         solta o arquivo na janela, na posição do cursor (cui_input_file_drop)
 *   hover on|off           simula o arraste de arquivos passando pela janela (cui_input_file_hover)
 *   clip <texto>           põe o texto na área de transferência FALSA (só com BRIKS_FAKE_CLIPBOARD; nunca toca na do usuário); sem texto, esvazia
 *   clipimg <png>          imagem "copiada" na área de transferência falsa (idem); sem argumento, remove
 *   hkey <combinação>      finge a tecla apertada na captura da tela de Atalhos (ex.: hkey Ctrl+Shift+K, hkey Esc)
 *   signal <nome>          cria o arquivo <nome>.flag na pasta de saída (um roteiro externo espera por ele para agir) */
#include "briks_ui.h"
#include "drag_stack_ui.h"
#include "layout.h"
#include "sysint.h"
#include "attach.h"
#include "hotkeys.h"
static int demo_voice_n = -1, demo_servers_n = -1;
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>

int g_script_fail, g_script_rw, g_script_rh;   /* resize pendente (aplicado pelo laço principal) */
static char *lines[512]; static int nlines, pc;
static int timed_left, timed_total;       /* operação com duração em andamento */
static bool timed_move; static float mx0, my0, mx1, my1;
static float cx, cy; static bool mdown;
static char caption[200];
static int frame_no, tail;
static bool esc_up, mods_reset; static int key_up = -1;
static int click_btn = CUI_MOUSE_LEFT;   /* botão do clique em andamento (click_phase) */
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
    if (mods_reset) { cui_input_key(ui, CUI_KEY_NONE, false, 0); mods_reset = false; }   /* solta os modificadores do atalho (o CUI só os relê quando uma tecla cai) */
    /* o mouse real do usuário não pode interferir na gravação: o roteiro reafirma posição e botão a cada quadro */
    cui_input_mouse_move(ui, cx, cy);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, mdown);
    if (click_phase) {   /* clique em andamento: 3 quadros parado em cima (para o controle ver o cursor), 3 apertado, 3 solto */
        click_phase++;
        if (click_phase == 4) { cui_input_mouse_button(ui, (cui_mouse_button)click_btn, true); if (click_btn == CUI_MOUSE_LEFT) mdown = true; }
        if (click_phase == 7) { cui_input_mouse_button(ui, (cui_mouse_button)click_btn, false); if (click_btn == CUI_MOUSE_LEFT) mdown = false; }
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
        else if (!strcmp(cmd, "at")) { char *a = strtok(NULL, " "), *b = a && a[0] >= '0' && a[0] <= '9' ? strtok(NULL, " ") : NULL; float tx, ty; if (a && b) { cx = (float)atof(a); cy = (float)atof(b); cui_input_mouse_move(ui, cx, cy); } else if (a && dock_script_point(a, &tx, &ty)) { cx = tx; cy = ty; cui_input_mouse_move(ui, cx, cy); } }   /* at X Y | at <ponto simbólico> */
        else if (!strcmp(cmd, "click")) {   /* click X Y | click <ponto> (ex.: plus:chat, close:chat, header:nav) */
            char *a = strtok(NULL, " "), *b = a && a[0] >= '0' && a[0] <= '9' ? strtok(NULL, " ") : NULL;
            float tx, ty; bool ok = a && (b ? (tx = (float)atof(a), ty = (float)atof(b), true) : dock_script_point(a, &tx, &ty));
            if (ok) { cx = tx; cy = ty; cui_input_mouse_move(ui, cx, cy); click_phase = 1; click_btn = CUI_MOUSE_LEFT; }
        }
        else if (!strcmp(cmd, "rclick")) {   /* rclick X Y | rclick <ponto>: como o click, com o botão direito */
            char *a = strtok(NULL, " "), *b = a && a[0] >= '0' && a[0] <= '9' ? strtok(NULL, " ") : NULL;
            float tx, ty; bool ok = a && (b ? (tx = (float)atof(a), ty = (float)atof(b), true) : dock_script_point(a, &tx, &ty));
            if (ok) { cx = tx; cy = ty; cui_input_mouse_move(ui, cx, cy); click_phase = 1; click_btn = CUI_MOUSE_RIGHT; }
        }
        else if (!strcmp(cmd, "drop")) { char *r = strtok(NULL, ""); if (r) { const char *ps[1] = { r }; cui_input_file_drop(ui, ps, 1, cx, cy); } }
        else if (!strcmp(cmd, "hover")) { char *r = strtok(NULL, " "); cui_input_file_hover(ui, r && !strcmp(r, "on"), cx, cy); }
        else if (!strcmp(cmd, "clip")) { char *r = strtok(NULL, ""); if (briks_clip_fake()) briks_clip_set(r ? r : ""); else fputs("roteiro: clip ignorado (sem BRIKS_FAKE_CLIPBOARD)\n", stderr); }
        else if (!strcmp(cmd, "wheel")) { char *a = strtok(NULL, " "); cui_input_mouse_move(ui, cx, cy); cui_input_scroll(ui, 0, a ? (float)atof(a) : -3); }   /* wheel <linhas> sobre o ponto atual (negativo desce) */
        else if (!strcmp(cmd, "msgstate")) { char *a = strtok(NULL, " "), *b = strtok(NULL, " "); if (a && b) attach_script_state(NULL, atoi(a), b); }   /* msgstate <n> sending|sent|failed|delivered|ack */
        else if (!strcmp(cmd, "media")) { char *r = strtok(NULL, ""); if (r) media_script(r); }   /* media play <arquivo> | pause | resume | seek <ms> | stop (1.4.3) */
        else if (!strcmp(cmd, "coreev")) { char *r = strtok(NULL, ""); if (r) attach_script_event(r); }   /* coreev <json>: injeta um evento do core (DM recebida, chat.sent…) */
        else if (!strcmp(cmd, "clipimg")) { char *r = strtok(NULL, ""); if (briks_clip_fake()) attach_fake_image(r); else fputs("roteiro: clipimg ignorado (sem BRIKS_FAKE_CLIPBOARD)\n", stderr); }
        else if (!strcmp(cmd, "expect")) {   /* expect <estado> <valor>: confere e conta falha (o processo sai com código 1) */
            char *n = strtok(NULL, " "), *v = strtok(NULL, " ");
            int got = n ? script_probe(n) : -1, want = v ? atoi(v) : 0;
            if (got != want) { g_script_fail++; fprintf(stderr, "roteiro: FALHOU linha %d: %s = %d, esperado %d\n", pc, n ? n : "?", got, want); }
            else fprintf(stderr, "roteiro: ok linha %d: %s = %d\n", pc, n, got);
        }
        else if (!strcmp(cmd, "resize")) { char *a = strtok(NULL, " "), *b = strtok(NULL, " "); if (a && b) { g_script_rw = atoi(a); g_script_rh = atoi(b); } }   /* resize L A: redimensiona a janela (testa o layout ao vivo) */
        else if (!strcmp(cmd, "scroll")) { char *n = strtok(NULL, " "); cui_input_mouse_move(ui, cx, cy); cui_input_scroll(ui, 0, n ? (float)atof(n) : -3.0f); }   /* scroll DY: roda a roda do mouse (negativo desce) */
        else if (!strcmp(cmd, "sys")) { char *r = strtok(NULL, ""); if (!sys_script(r ? r : "")) fprintf(stderr, "roteiro: sys inválido na linha %d\n", pc); }   /* sys <comando>: bandeja, avisos, janela (winstate.c) */
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
            char *k = strtok(NULL, " "); int mods = 0, kc = -1;
            while (k) {   /* ctrl+shift+z: modificadores antes do + final */
                char *pl = strchr(k, '+');
                if (pl && pl[1]) { *pl = 0; if (!_stricmp(k, "ctrl")) mods |= CUI_MOD_CTRL; else if (!_stricmp(k, "shift")) mods |= CUI_MOD_SHIFT; else if (!_stricmp(k, "alt")) mods |= CUI_MOD_ALT; k = pl + 1; continue; }
                kc = !strcmp(k, "enter") ? CUI_KEY_ENTER : !strcmp(k, "tab") ? CUI_KEY_TAB : !strcmp(k, "backspace") ? CUI_KEY_BACKSPACE : !strcmp(k, "esc") ? CUI_KEY_ESCAPE
                   : !strcmp(k, "home") ? CUI_KEY_HOME : !strcmp(k, "end") ? CUI_KEY_END : !strcmp(k, "left") ? CUI_KEY_LEFT : !strcmp(k, "right") ? CUI_KEY_RIGHT : !strcmp(k, "delete") ? CUI_KEY_DELETE : !strcmp(k, "f11") ? CUI_KEY_F11
                   : (strlen(k) == 1 && k[0] >= 'a' && k[0] <= 'z') ? (int)CUI_KEY_A + (k[0] - 'a') : -1;
                break;
            }
            if (kc >= 0) { cui_input_key(ui, (cui_key)kc, true, mods); key_up = kc; if (mods) mods_reset = true; }
        }
        else if (!strcmp(cmd, "hkey")) { char *r = strtok(NULL, ""); if (r) hkw_capture_inject(r); }   /* hkey <combinação>: finge que a tecla foi apertada na captura de atalhos */
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
    static int hide_cursor = -1;   /* capturas pontuais (BRIKS_SCRIPT_SHOTS) saem sem o círculo do ponteiro (LAY-12); BRIKS_SCRIPT_CURSOR=1 o mantém */
    if (hide_cursor < 0) hide_cursor = shots_only && !getenv("BRIKS_SCRIPT_CURSOR");
    if (!hide_cursor) cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_START,
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
    static int pw, ph; if (!px || pw != w || ph != h) { free(px); px = malloc((size_t)w * (size_t)h * 4); pw = w; ph = h; }   /* a janela pode mudar de tamanho (comando resize) */
    if (px && (shot_name[0] || !shots_only) && cui_app_screenshot(app, px, w, h)) {
        char name[640];
        if (shot_name[0]) snprintf(name, sizeof name, "%s\\%s.png", outdir, shot_name); else snprintf(name, sizeof name, "%s\\frame_%05d.png", outdir, frame_no++);
        cui_write_png(name, px, w, h, w * 4);
    }
    shot_name[0] = 0;
    if (tail > 20) { cui_app_quit(app); return true; }
    return false;
}
