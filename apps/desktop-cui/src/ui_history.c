/* Histórico de chamadas: registro real (quando, duração, sala, quem estava, se transmiti), opção de guardar e apagar.
 * Fica só neste computador, em call_history.json na pasta de dados do app. A mesma lista serve o painel central (no lugar da
 * conversa, com trilho e menu à vista), o painel "Histórico" do dock e a tela inteira (janela sem painel de conversa). */
#include "call.h"
#include "layout.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include <shlobj.h>

static callhist_t H;
static bool h_loaded, h_open, h_confirm, h_mem_only;
static int h_frame_owner = -1;          /* qual instância do painel desenha o diálogo neste quadro */
static struct { bool on, connected; long long start; call_rec_t rec; double last_save; } cur;   /* chamada em andamento */

static bool hist_path(char *out, size_t cap) {
    const char *o = getenv("BRIKS_CUI_DATA");
    if (o && o[0]) { CreateDirectoryA(o, NULL); snprintf(out, cap, "%s\\call_history.json", o); return true; }
    if (getenv("BRIKS_DEMO")) return false;   /* demonstração sem pasta isolada: nada é lido nem gravado */
    char ad[MAX_PATH]; if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, ad) != S_OK) return false;
    char dir[MAX_PATH]; snprintf(dir, sizeof dir, "%s\\Briks-CUI", ad); CreateDirectoryA(dir, NULL);
    snprintf(out, cap, "%s\\call_history.json", dir); return true;
}

static void hist_save_as(const callhist_t *h) {
    char p[MAX_PATH]; if (h_mem_only || !hist_path(p, sizeof p)) return;
    static char buf[CALLHIST_MAX * 760 + 256];
    size_t n = callhist_to_json(h, buf, sizeof buf);
    char tmp[MAX_PATH + 8]; snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "wb"); if (!f) return;
    bool ok = fwrite(buf, 1, n, f) == n; fclose(f);
    if (ok) MoveFileExA(tmp, p, MOVEFILE_REPLACE_EXISTING); else DeleteFileA(tmp);   /* troca atômica: queda no meio não estraga o arquivo */
}
static void hist_save(void) { hist_save_as(&H); }

static void hist_load(void) {
    if (h_loaded) return;
    h_loaded = true; callhist_init(&H);
    const char *demo = getenv("BRIKS_DEMO_HIST");
    if (demo && getenv("BRIKS_DEMO")) {   /* capturas: registros de exemplo só na memória */
        h_mem_only = true;
        if (demo[0] == '1') {
            long long now = (long long)time(NULL);
            const call_rec_t ex[4] = {
                { now - 2 * 3600, 47 * 60 + 12, "voz-geral", "Sala geral", "Marina Costa, Rafael Lima", 2, true },
                { now - 26 * 3600, 3600 + 5 * 60, "g-demo", "Noite de jogos · Valorant", "Marina Costa, Rafael Lima, Bia Nogueira", 3, false },
                { now - 4 * 86400, 38, "voz-geral", "Sala geral", "", 0, false },
                { now - 400LL * 86400, 12 * 60, "voz-geral", "Sala geral", "Bia Nogueira", 1, false } };
            for (int i = 3; i >= 0; i--) callhist_add(&H, &ex[i]);
        }
        return;
    }
    char p[MAX_PATH]; if (!hist_path(p, sizeof p)) return;
    FILE *f = fopen(p, "rb"); if (!f) return;
    static char buf[CALLHIST_MAX * 760 + 256];
    size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    callhist_from_json(&H, buf);
}

/* nome exibido da sala: canal de voz do grupo, "Sala geral" ou o nome técnico */
void call_room_title(const app_state_t *s, char *out, size_t cap) {
    for (int g = 0; g < s->group_count; g++) for (int c = 0; c < s->groups[g].nch; c++) if (s->groups[g].ch[c].voice) {
        char rn[96]; snprintf(rn, sizeof rn, "g-%s", s->groups[g].ch[c].room);
        if (!strcmp(rn, s->room_name)) { snprintf(out, cap, "%s · %s", s->groups[g].name, s->groups[g].ch[c].name); return; }
    }
    snprintf(out, cap, "%s", !s->room_name[0] || !strcmp(s->room_name, "voz-geral") ? "Sala geral" : s->room_name);
}

void history_tick(app_state_t *s) {
    hist_load();
    h_frame_owner = -1;
    const long long now = (long long)time(NULL);
    const bool connected = s->in_room && (!strcmp(s->rtc_state, "conectado") || getenv("BRIKS_DEMO_PEERS"));
    if (s->in_room && !cur.on) {
        memset(&cur, 0, sizeof cur); cur.on = true; cur.start = now;
        snprintf(cur.rec.room, sizeof cur.rec.room, "%s", s->room_name[0] ? s->room_name : "voz-geral");
    }
    if (cur.on && s->in_room) {
        if (connected && !cur.connected) { cur.connected = true; cur.start = now; }   /* a duração conta a partir da conexão */
        call_room_title(s, cur.rec.title, sizeof cur.rec.title);
        for (int i = 0; i < 8; i++) if (s->rtc_peers[i].id[0] && s->rtc_peers[i].name[0]) callhist_people_add(cur.rec.people, sizeof cur.rec.people, &cur.rec.npeople, s->rtc_peers[i].name);
        if (s->sh.active) cur.rec.shared = true;
        cur.rec.started = cur.start; cur.rec.duration = (int)(now - cur.start);
        /* a cada 30 s grava como se a chamada terminasse agora: fechar o app (ou cair) no meio não perde o registro */
        if (cur.connected && H.keep && cur.rec.duration >= 30 && (double)now - cur.last_save >= 30.0) {
            static callhist_t snap; snap = H; callhist_add(&snap, &cur.rec); hist_save_as(&snap); cur.last_save = (double)now;
        }
    }
    if (cur.on && !s->in_room) {   /* saiu (ou foi removido): fecha o registro */
        cur.on = false;
        if (cur.connected) { cur.rec.started = cur.start; cur.rec.duration = (int)(now - cur.start); if (cur.rec.duration < 1) cur.rec.duration = 1; if (callhist_add(&H, &cur.rec)) hist_save(); else if (cur.last_save > 0) hist_save(); }
    }
}

void history_close(void) { h_open = false; }
bool history_is_open(void) { return h_open; }
void history_open(app_state_t *s) {
    if (dock_has_panel(P_CHAT)) { app_open_friend(s, -1); h_open = true; }   /* painel central: trilho e menu continuam à vista */
    else s->screen = SCREEN_HISTORY;                                           /* layout sem painel de conversa: tela inteira com seta de voltar */
}

int history_probe(const char *name) {
    hist_load();
    if (!strcmp(name, "hist_open")) return h_open;
    if (!strcmp(name, "hist_n")) return H.n;
    if (!strcmp(name, "hist_keep")) return H.keep;
    if (!strcmp(name, "hist_people")) return H.n ? H.r[0].npeople : -1;
    if (!strcmp(name, "hist_confirm")) return h_confirm;
    return -2;
}

static void when_text(long long t, char *out, size_t cap) {
    time_t tt = (time_t)t, nn = time(NULL), yy = nn - 86400;
    struct tm a, n, y; localtime_s(&a, &tt); localtime_s(&n, &nn); localtime_s(&y, &yy);
    call_fmt_when(a.tm_year + 1900, a.tm_mon + 1, a.tm_mday, a.tm_hour, a.tm_min, n.tm_year + 1900, n.tm_mon + 1, n.tm_mday,
                  a.tm_year == y.tm_year && a.tm_mon == y.tm_mon && a.tm_mday == y.tm_mday, out, cap);
}

static void call_again(app_state_t *s, const call_rec_t *r) {
    if (s->in_room && strcmp(s->room_name, r->room)) rtc_leave_room(s);
    s->screen = SCREEN_CALL; s->mic_on = true; h_open = false;
    if (!s->in_room) rtc_join_room(s, r->room[0] ? r->room : "voz-geral");
}

static void rec_card(cui_ctx *ui, app_state_t *s, const call_rec_t *r, float width) {
    char when[40], dur[24], meta[96], with[300];
    when_text(r->started, when, sizeof when); call_fmt_duration(r->duration, dur, sizeof dur);
    snprintf(meta, sizeof meta, "%s  ·  %s", when, dur);
    if (r->npeople > 0) snprintf(with, sizeof with, "Com %s", r->people); else snprintf(with, sizeof with, "Só você na sala");
    const bool narrow = width < 430.0f;
    cui_box(ui, .width = CUI_FILL, .dir = narrow ? CUI_COLUMN : CUI_ROW, .pad = 14, .gap = 12, .radius = 18, .bg = B_GLASS_CARD, .border = 1, .border_color = B_GLASS_BORDER,
            .align = narrow ? CUI_ALIGN_DEFAULT : CUI_ALIGN_CENTER, .clip = true) {
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_CENTER) {
            cui_box(ui, .width = CUI_PX(42), .height = CUI_PX(42), .radius = 21, .bg = B_A(B_C3, 0x30), .border = 1, .border_color = B_A(B_C3, 0x66),
                    .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) { cui_icon(ui, r->shared ? CUI_ICON_MONITOR : CUI_ICON_PHONE, 18, B_TEXT); }
            cui_box(ui, .width = CUI_FILL, .gap = 3, .clip = true) {
                cui_text(ui, r->title[0] ? r->title : "Sala", .font = CUI_FONT_MEDIUM, .size = 15, .nowrap = true);
                cui_text(ui, meta, .size = 12.5f, .color = B_MUTED, .nowrap = true);
                cui_text(ui, with, .size = 12.5f, .color = B_MUTED, .nowrap = true);
            }
        }
        cui_box(ui, .dir = CUI_ROW, .gap = 8, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_END) {
            if (r->shared) cui_box(ui, .dir = CUI_ROW, .pad_x = 10, .pad_y = 5, .gap = 6, .radius = 13, .bg = B_A(B_C1, 0x38), .border = 1, .border_color = B_A(B_C3, 0x55), .align = CUI_ALIGN_CENTER) {
                cui_text(ui, "Você transmitiu", .size = 12, .color = B_TEXT, .nowrap = true);
            }
            if (cui_button(ui, "Ligar de novo", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM, .icon = CUI_ICON_PHONE)) call_again(s, r);
        }
    }
}

void ui_history_panel(cui_ctx *ui, app_state_t *s, int mode) {
    hist_load();
    const bool full = mode == HIST_FULL;
    const float width = full ? cui_viewport_size(ui).x - 20.0f : g_panel_w;
    const float tools = full ? 0.0f : g_tools_w;
    const bool tight = width < 420.0f;   /* painel estreito: textos mais curtos */
    cui_push_id_int(ui, 9300 + mode);
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 0, .bg = B_GLASS, .radius = (int)g_th.panel_radius, .clip = true,
            .border = 1, .border_color = B_GLASS_BORDER, .elevation = briks_elev(CUI_ELEVATION_RAISED)) {
        bool keep = H.keep, wipe = false;
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(62), .pad_left = 20, .pad_right = 20 + tools, .gap = 10,
                .radius_tl = (int)g_th.panel_radius - 1, .radius_tr = (int)g_th.panel_radius - 1, .bg = CUI_RGBA(0xffffff0a), .align = CUI_ALIGN_CENTER) {
            if (full && cui_icon_button(ui, CUI_ICON_ARROW_LEFT, .tooltip = "Voltar")) s->screen = SCREEN_SOCIAL;
            cui_icon(ui, CUI_ICON_HISTORY, 18, B_MUTED);
            cui_text(ui, "Histórico de chamadas", .font = CUI_FONT_MEDIUM, .nowrap = true);
            if (H.n && !tight) { char c[40]; snprintf(c, sizeof c, H.n == 1 ? "1 chamada" : "%d chamadas", H.n); cui_text(ui, c, .size = 12, .color = B_MUTED, .nowrap = true); }
        }
        cui_separator(ui);
        /* opções numa faixa própria, abaixo do cabeçalho: o cabeçalho é a alça de arrasto do painel (dock.c) e não recebe cliques de botões */
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 18, .pad_y = 10, .gap = 10, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_BETWEEN, .wrap = true) {
            cui_switch(ui, "Guardar histórico", &keep);
            if (cui_button(ui, tight ? "Apagar" : "Apagar histórico", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM, .icon = CUI_ICON_TRASH_2, .disabled = H.n == 0)) wipe = true;
        }
        if (keep != H.keep) { H.keep = keep; hist_save(); }
        if (wipe) h_confirm = true;
        if (!H.n) {
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .gap = 8, .pad = 20) {
                cui_box(ui, .width = CUI_PX(56), .height = CUI_PX(56), .radius = 28, .bg = CUI_RGBA(0xffffff10), .border = 1, .border_color = CUI_RGBA(0xffffff1c),
                        .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) { cui_icon(ui, CUI_ICON_HISTORY, 24, B_C3); }
                cui_text(ui, "Nenhuma chamada ainda", .font = CUI_FONT_DISPLAY, .size = 18, .nowrap = true);
                if (width >= 440.0f) {   /* linhas curtas, centralizadas */
                    cui_text(ui, H.keep ? "Quando você sair de uma chamada, ela aparece aqui" : "O histórico está desligado:", .size = 13, .color = B_MUTED, .nowrap = true);
                    cui_text(ui, H.keep ? "com a duração e quem estava. Fica só neste computador." : "as próximas chamadas não serão guardadas.", .size = 13, .color = B_MUTED, .nowrap = true);
                } else cui_text(ui, H.keep ? "As chamadas aparecem aqui ao sair. Ficam só neste computador." : "O histórico está desligado.", .size = 12.5f, .color = B_MUTED);
            }
        } else {
            cui_scroll(ui, .id = "hist-scroll", .width = CUI_FILL, .height = CUI_FILL, .pad = 14) {
                cui_column(ui, .gap = 10, .width = CUI_FILL) {
                    if (!H.keep) cui_text(ui, "O histórico está desligado: as próximas chamadas não serão guardadas.", .size = 12.5f, .color = B_MUTED);
                    for (int i = 0; i < H.n; i++) { cui_push_id_int(ui, i); rec_card(ui, s, &H.r[i], width - 28.0f); cui_pop_id(ui); }
                }
            }
        }
    }
    if (h_frame_owner < 0 || h_frame_owner == mode) {   /* um diálogo só, mesmo com o painel aberto em dois lugares */
        h_frame_owner = mode;
        cui_vec2 vp = cui_viewport_size(ui);
        cui_dialog(ui, "Apagar o histórico?", &h_confirm, .width = fminf(400.0f, fmaxf(vp.x - 48.0f, 260.0f))) {
            cui_text(ui, "Todas as chamadas guardadas neste computador serão apagadas. Não dá para desfazer.", .color = B_MUTED);
            cui_dialog_footer(ui) {
                if (cui_button(ui, "Cancelar", .variant = CUI_BUTTON_SECONDARY)) h_confirm = false;
                if (cui_button(ui, "Apagar", .variant = CUI_BUTTON_DANGER)) { callhist_clear(&H); hist_save(); h_confirm = false; }
            }
        }
    }
    cui_pop_id(ui);
}

/* Tela inteira: só quando o layout não tem o painel de conversa (janela estreita ou painel fechado). */
void ui_history(cui_ctx *ui, app_state_t *s) {
    briks_ambient(ui, 0.85f);
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 10) { ui_history_panel(ui, s, HIST_FULL); }
}
