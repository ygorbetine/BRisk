#include "briks_ui.h"
#include <math.h>
#include "layout.h"
#include "msgui.h"
#include "attach.h"
#include "richtext.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

static void nick_text(const app_state_t *s, char *out, size_t cap);

static void initials(const char *name, char *out) {
    out[0] = name[0] ? name[0] : '?';
    const char *sp = strchr(name, ' ');
    out[1] = sp && sp[1] ? sp[1] : 0;
    out[2] = 0;
}

/* Barra de grupos responsiva: só ícones (36..56 px conforme a largura) ou, com folga, ícone + nome + "N membros".
 * g_panel_w é a largura atual do painel (dock.c). Texto sempre medido com a fonte real e cortado com reticências antes da borda. */
float g_panel_w = 76, g_tools_w;
char g_tip[160];
static void hot_paint(cui_ctx *ui, cui_rect r, void *user) {
    (void)r; const float *q = (const float *)user;
    if (q[2] <= 0) return;
    cui_rect z = { q[0], q[1], q[2], q[3] };
    cui_draw_rect(ui, z, CUI_CORNERS(fminf(12.0f, q[3] * 0.5f)), q[4] > 0.5f ? CUI_RGBA(0xffffff24) : CUI_RGBA(0xffffff12));
}
/* Dentro de diálogo/menu a camada de baixo fica coberta: aqui só o cursor de mão; o realce é do próprio item (menus de Configurações e Perfil). */
void briks_hot_modal_draw(cui_ctx *ui) { if (g_hot_on) { cui_set_cursor(ui, CUI_CURSOR_HAND); g_hot_on = false; } }
void briks_tip_draw(cui_ctx *ui) {
    if (g_hot_on) {   /* realce do controle sob o mouse (hover) e apertado (clique) */
        g_hot_on = false;
        cui_set_cursor(ui, CUI_CURSOR_HAND);
        if (g_hot[2] * g_hot[3] < 70000.0f) {   /* cartões/áreas grandes: só o cursor */
            float q[5] = { g_hot[0], g_hot[1], g_hot[2], g_hot[3], cui_mouse_down(ui, CUI_MOUSE_LEFT) ? 1.0f : 0.0f };
            cui_canvas(ui, "briks-hot", CUI_FILL, CUI_FILL, hot_paint, q, sizeof q);
        }
    }
    if (!g_tip[0]) return;
    cui_vec2 vp = cui_viewport_size(ui), m = cui_mouse_pos(ui);
    cui_text_metrics tm = cui_measure_text(ui, CUI_FONT_BODY, 12.0f, g_tip, (int)strlen(g_tip));
    float w = fminf(tm.width, 320.0f) + 24, h = 28, x = m.x + 14, y = m.y + 20;
    if (x + w + 6 > vp.x) x = m.x - w - 8;
    if (y + h + 6 > vp.y) y = m.y - h - 10;
    x = fmaxf(x, 6); y = fmaxf(y, 6);
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_START, .pad_left = x, .pad_top = y) {
        cui_box(ui, .width = CUI_PX(w), .height = CUI_PX(h), .radius = 14, .bg = B_GLASS_POPUP, .border = 1, .border_color = CUI_RGBA(0xffffff30), .clip = true,
                .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
            cui_text(ui, g_tip, .size = 12, .nowrap = true, .color = CUI_RGB(0xf6f1ff));
        }
    }
    g_tip[0] = 0;
}

static void fit_text(cui_ctx *ui, cui_font font, float size, const char *src, float maxw, char *out, size_t cap, bool *cut) {
    snprintf(out, cap, "%s", src); *cut = false;
    if (cui_measure_text(ui, font, size, out, (int)strlen(out)).width <= maxw) return;
    *cut = true;
    size_t n = strlen(src);
    while (n > 0) {
        n--; while (n > 0 && (src[n] & 0xC0) == 0x80) n--;   /* não corta no meio de um caractere UTF-8 */
        char t[160]; snprintf(t, sizeof t, "%.*s\xE2\x80\xA6", (int)n, src);
        if (cui_measure_text(ui, font, size, t, (int)strlen(t)).width <= maxw) { snprintf(out, cap, "%s", t); return; }
    }
    snprintf(out, cap, "\xE2\x80\xA6");
}

static void space_button(cui_ctx *ui, const char *label, const char *name, const char *sub, bool active, bool dashed, int *clicked_id, int id) {
    float W = g_panel_w, icon = fminf(fmaxf(W - 20.0f, 36.0f), 56.0f), rad = icon * 0.3f;
    bool wide = W >= 110.0f;
    cui_push_id_int(ui, id);
    cui_color bg = active ? B_C3 : dashed ? CUI_RGBA(0x8b5cf60b) : CUI_RGBA(0xffffff16);
    cui_color bd = active ? CUI_RGBA(0xffffff40) : dashed ? CUI_RGBA(0x8b5cf666) : CUI_RGBA(0xffffff26);
    float itemw = wide ? W - 20.0f : icon;
    cui_box(ui, .id = "b", .dir = CUI_ROW, .width = CUI_PX(itemw), .height = CUI_PX(icon), .gap = 10, .radius = rad,
            .bg = wide ? CUI_RGBA(0x00000000) : bg, .border = wide ? 0 : 1, .border_color = bd, .clip = true,
            .align = CUI_ALIGN_CENTER, .justify = wide ? CUI_JUSTIFY_START : CUI_JUSTIFY_CENTER) {
        if (wide) {
            cui_box(ui, .width = CUI_PX(icon), .height = CUI_PX(icon), .radius = rad, .bg = bg, .border = 1, .border_color = bd,
                    .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                cui_text(ui, label, .size = icon * 0.26f, .color = active ? CUI_RGB(0xffffff) : dashed ? B_C3 : CUI_RGB(0xe6dcff));
            }
            float tw = itemw - icon - 10.0f - 8.0f;   /* folga interna: o texto nunca encosta na borda */
            char nm[160], sb[64]; bool cn, cs;
            fit_text(ui, CUI_FONT_MEDIUM, 13.0f, name, tw, nm, sizeof nm, &cn);
            fit_text(ui, CUI_FONT_BODY, 11.0f, sub, tw, sb, sizeof sb, &cs);
            cui_box(ui, .width = CUI_PX(tw), .gap = 2, .clip = true) {
                cui_text(ui, nm, .font = CUI_FONT_MEDIUM, .size = 13, .nowrap = true, .color = active ? B_TEXT : CUI_RGB(0xe6dcff));
                if (sb[0]) cui_text(ui, sb, .size = 11, .nowrap = true, .color = B_MUTED);
            }
        } else {
            cui_text(ui, label, .size = icon * 0.3f, .color = active ? CUI_RGB(0xffffff) : dashed ? B_C3 : CUI_RGB(0xe6dcff));
        }
    }
    { cui_rect tr = cui_get_rect(ui, cui_make_id(ui, "b")); cui_vec2 tm = cui_mouse_pos(ui);   /* tooltip só com o mouse realmente sobre o item (cui_last_hovered não serve p/ caixas simples) */
      if (tr.w > 0 && tm.x >= tr.x && tm.y >= tr.y && tm.x < tr.x + tr.w && tm.y < tr.y + tr.h && (!wide || strlen(name) > 14)) briks_tip(name); }
    if (briks_clicked(ui, "b")) *clicked_id = id;
    cui_pop_id(ui);
}

/* ícone compacto (26 px) do rodapé do usuário: cabe três na largura do menu sem espremer o nome/status */
static bool foot_icon(cui_ctx *ui, const char *id, const char *icon, const char *tip) {
    cui_push_id(ui, id);
    cui_box(ui, .id = "b", .width = CUI_PX(26), .height = CUI_PX(26), .radius = 9, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .bg = CUI_RGBA(0xffffff0c)) {
        cui_icon(ui, icon, 15, B_MUTED);
    }
    if (briks_hover(ui, "b")) briks_tip(tip);
    bool hit = briks_clicked(ui, "b");
    cui_pop_id(ui);
    return hit;
}

static bool nav_item(cui_ctx *ui, const char *icon, const char *label, bool active) {
    cui_push_id(ui, label);
    cui_box(ui, .id = "b", .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 10, .pad_y = 9, .gap = 10, .radius = 12,
            .bg = active ? B_A(B_C1, 0x38) : CUI_RGBA(0x00000000),
            .border = active ? 1 : 0, .border_color = CUI_RGBA(0xffffff1a), .align = CUI_ALIGN_CENTER) {
        cui_icon(ui, icon, 16, active ? B_TEXT : B_MUTED);
        cui_text(ui, label, .color = active ? B_TEXT : B_MUTED, .nowrap = true);
    }
    bool hit = briks_clicked(ui, "b");
    cui_pop_id(ui);
    return hit;
}

static void section_title(cui_ctx *ui, const char *t) {
    cui_box(ui, .pad_left = 9, .pad_top = 22, .pad_bottom = 7) {
        cui_text(ui, t, .size = 10, .color = g_th.ph);
    }
}

/* Balões de pensamento (Liquid Glass): as telas registram onde fica o avatar e o texto; o desenho vem por cima de tudo no fim do quadro. */
typedef struct { cui_rect av; char text[64]; } bub_note_t;
static bub_note_t g_bub[4]; static int g_bub_n;
static cui_rect g_bclip;   /* área visível da lista que contém o avatar (rect do cui_scroll no quadro anterior); w = 0: sem corte */
static void bubble_note(cui_rect av, const char *text) {
    if (g_bub_n < 4 && av.w > 0 && text[0]) { g_bub[g_bub_n].av = av; snprintf(g_bub[g_bub_n].text, sizeof g_bub[g_bub_n].text, "%s", text); g_bub_n++; }
}
static void abs_glass(cui_ctx *ui, float x, float y, float w, float h, float r, bool shadow) {
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_START, .pad_left = x, .pad_top = y) {
        cui_box(ui, .width = CUI_PX(w), .height = CUI_PX(h), .radius = r, .bg = briks_bubble_bg(), .border = 1, .border_color = briks_bubble_border(),
                .elevation = shadow ? CUI_ELEVATION_RESTING : CUI_ELEVATION_NONE) {}
    }
}
static void bubbles_draw(cui_ctx *ui) {
    for (int i = 0; i < g_bub_n; i++) {
        cui_push_id_int(ui, 9100 + i);
        cui_rect a = g_bub[i].av; const char *t = g_bub[i].text;
        cui_text_metrics m = cui_measure_text(ui, CUI_FONT_BODY, 13, t, (int)strlen(t));
        float w = fminf(m.width, 190.0f) + 30, h = 32;
        const float up = 8.0f;   /* balão e bolinhas sobem juntos: respiro entre a cauda e o avatar */
        float bx = a.x + 8, by = a.y - 17 - up - h;
        if (by < 6) by = 6;
        /* bolinhas de pensamento (theme.json: bubble_tail_gap/r1/r2): a 1ª fica a "gap" px da borda de baixo do balão, a 2ª a "gap" px da 1ª, rumo ao avatar; glass.c as une com o pescoço bubble_neck_k */
        float r1 = g_th.bubble_tail_r1, r2 = g_th.bubble_tail_r2, gp = g_th.bubble_tail_gap;
        float c1x = a.x + 38, c1y = a.y - 17 - up + gp + r1;
        float ux = (a.x + 31) - c1x, uy = a.y - c1y, ul = sqrtf(ux * ux + uy * uy); if (ul < 1) ul = 1;
        float c2x = c1x + ux / ul * (r1 + r2 + gp), c2y = c1y + uy / ul * (r1 + r2 + gp);
        abs_glass(ui, c2x - r2, c2y - r2, r2 * 2, r2 * 2, r2, false);
        abs_glass(ui, c1x - r1, c1y - r1, r1 * 2, r1 * 2, r1, false);
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_START, .pad_left = bx, .pad_top = by) {
            cui_box(ui, .width = CUI_PX(w), .height = CUI_PX(h), .radius = briks_bubble_radius(h), .bg = briks_bubble_bg(), .border = 1, .border_color = briks_bubble_border(),
                    .elevation = CUI_ELEVATION_RESTING, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                cui_text(ui, t, .size = 13, .color = CUI_RGB(0xffffff), .nowrap = true);
            }
        }
        cui_pop_id(ui);
    }
    if (g_bub_n && g_perf == PERF_NORMAL && !getenv("BRIKS_NO_BREATH")) cui_request_redraw(ui);   /* respiração lenta do balão (só no Normal) */
    g_bub_n = 0;
}

static bool person_row_ex(cui_ctx *ui, const person_t *f, bool active, cui_texture img) {
    cui_push_id(ui, f->name);
    if (active && f->status[0]) cui_gap(ui, 52);   /* reserva o lugar do balão de status: ele não cobre o título nem outros itens */
    cui_box(ui, .id = "b", .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 8, .pad_y = 6, .gap = 10, .radius = 12,
            .bg = active ? B_A(B_C1, 0x38) : CUI_RGBA(0x00000000), .align = CUI_ALIGN_CENTER) {
        cui_box(ui, .id = "av") { cui_avatar_ex(ui, f->name, &(cui_avatar_opts){ .size = 32, .image = img }); }
        const char *line = f->presence;   /* a linha sob o nome é só o rich presence; o status vai no balão */
        if (line[0]) {   /* rich presence: linha pequena sob o nome */
            cui_column(ui, .gap = 0, .width = CUI_FILL) {
                cui_text(ui, f->name, .nowrap = true);
                cui_text(ui, line, .size = 11, .color = B_MUTED, .nowrap = true);
            }
        } else cui_text(ui, f->name, .nowrap = true, .width = CUI_FILL);
        if (active && f->status[0]) {   /* avatar rolado para fora da lista (painel baixo, layout empilhado): o balão não aparece sobre outro painel */
            cui_rect av = cui_get_rect(ui, cui_make_id(ui, "av")), c = g_bclip;
            if (!(c.w > 0 && (av.y < c.y || av.y + av.h > c.y + c.h || av.x < c.x || av.x + av.w > c.x + c.w))) bubble_note(av, f->status);
        }
    }
    bool hit = briks_clicked(ui, "b");
    cui_pop_id(ui);
    return hit;
}
#define person_row(ui, f, active) person_row_ex((ui), (f), (active), 0)

static void navigation(cui_ctx *ui, app_state_t *s) {
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = B_GLASS, .radius = (int)g_th.panel_radius, .clip = true,
            .border = 1, .border_color = B_GLASS_BORDER, .elevation = briks_elev(CUI_ELEVATION_RAISED)) {
        cui_box(ui, .dir = CUI_ROW, .height = CUI_PX(62), .width = CUI_FILL, .pad_x = 20, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, s->active_friend >= 0 ? s->friends[s->active_friend].name : "Briks",
                     .font = CUI_FONT_DISPLAY, .size = 16, .nowrap = true);
        }
        cui_separator(ui);
        g_bclip = cui_get_rect(ui, cui_make_id(ui, "nav-scroll"));
        cui_scroll(ui, .id = "nav-scroll", .width = CUI_FILL, .height = CUI_FILL, .pad = 9) {
            if (nav_item(ui, CUI_ICON_USERS, "Amigos", s->active_friend < 0 && s->active_group < 0)) app_open_friend(s, -1);
            if (nav_item(ui, CUI_ICON_USER_PLUS, "Adicionar amigo", false)) { app_open_friend(s, -1); s->friends_tab = 4; s->notice[0] = 0; }
            if (nav_item(ui, CUI_ICON_HISTORY, "Histórico de chamadas", false)) s->screen = SCREEN_HISTORY;
            section_title(ui, "SALAS DE VOZ");
            /* Sala de teste: a chamada real chega com o briks-core (marco 3). */
            if (nav_item(ui, CUI_ICON_VOLUME_2, "Sala geral", false)) { s->screen = SCREEN_CALL; s->mic_on = true; if (!s->in_room) rtc_join_room(s, "voz-geral"); }
            if (s->pending_count) {
                char t[48]; snprintf(t, sizeof t, "PEDIDOS — %d", s->pending_count);
                section_title(ui, t);
                for (int i = 0; i < s->pending_count; i++) {
                    cui_push_id_int(ui, 1000 + i);
                    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 8, .pad_y = 6, .gap = 8, .align = CUI_ALIGN_CENTER) {
                        cui_avatar(ui, s->pending[i].name, .size = 28);
                        cui_text(ui, s->pending[i].name, .nowrap = true, .width = CUI_FILL);
                        if (cui_icon_button(ui, CUI_ICON_CHECK, .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM, .tooltip = "Aceitar"))
                            app_accept_friend(s, s->pending[i].request_id);
                    }
                    cui_pop_id(ui);
                }
            }
            if (s->legacy_count) {   /* contatos importados do Briks antigo: ainda sem conta; servem de lembrete para pedir o @nick */
                char t[64]; snprintf(t, sizeof t, "DO BRIKS ANTIGO — %d", s->legacy_count);
                section_title(ui, t);
                for (int i = 0; i < s->legacy_count; i++) {
                    cui_push_id_int(ui, 3000 + i);
                    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 8, .pad_y = 5, .gap = 8, .radius = 12, .align = CUI_ALIGN_CENTER) {
                        cui_avatar(ui, s->legacy[i].name, .size = 24);
                        cui_text(ui, s->legacy[i].name, .nowrap = true, .width = CUI_FILL, .color = B_MUTED);
                    }
                    cui_pop_id(ui);
                }
                cui_text(ui, "Peça o @nick deles para adicionar pela conta.", .size = 11, .color = B_MUTED);
            }
            section_title(ui, "GRUPOS");
            if (nav_item(ui, CUI_ICON_PLUS, "Criar grupo", false)) { s->create_group_open = true; s->new_group[0] = 0; }
            if (nav_item(ui, CUI_ICON_LINK, "Entrar com convite", false)) { s->join_open = true; s->join_code[0] = 0; s->join_err[0] = 0; }
            for (int i = 0; i < s->group_count; i++) {
                cui_push_id_int(ui, 2000 + i);
                person_t g; memset(&g, 0, sizeof g); snprintf(g.name, sizeof g.name, "%s", s->groups[i].name);
                if (person_row_ex(ui, &g, s->active_group == i, groups_image(s, s->groups[i].id))) app_open_group(s, i);
                if (s->active_group == i) groups_sidebar(ui, s, i);
                cui_pop_id(ui);
            }
            char t[48]; snprintf(t, sizeof t, "AMIGOS — %d", s->friend_count);
            section_title(ui, t);
            if (!s->friend_count) {   /* estado vazio no espaço livre até o rodapé: centralizado, sem botão (a ação é o item "Adicionar amigo" lá em cima) */
                cui_box(ui, .id = "nf-anchor", .width = CUI_FILL, .height = CUI_PX(1)) {}
                cui_rect an = cui_get_rect(ui, cui_make_id(ui, "nf-anchor"));
                float avail = (an.w > 0 && g_bclip.h > 0) ? g_bclip.y + g_bclip.h - an.y - 9.0f : 0.0f;
                if (avail >= 150.0f) {
                    cui_box(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .pad_top = fmaxf(8.0f, (avail - 96.0f) * 0.5f - 6.0f), .gap = 6) {
                        cui_box(ui, .width = CUI_PX(44), .height = CUI_PX(44), .radius = 22, .bg = CUI_RGBA(0xffffff10), .border = 1, .border_color = CUI_RGBA(0xffffff1c),
                                .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) { cui_icon(ui, CUI_ICON_USERS, 20, B_C3); }
                        cui_text(ui, "Nenhum amigo ainda", .font = CUI_FONT_MEDIUM, .size = 13, .nowrap = true);
                        cui_text(ui, "Quem você adicionar aparece aqui.", .size = 12, .color = B_MUTED, .nowrap = true);
                    }
                } else cui_text(ui, "Você ainda não tem amigos.", .size = 12, .color = B_MUTED);
            }
            for (int i = 0; i < s->friend_count; i++) {
                cui_push_id_int(ui, i);
                if (person_row_ex(ui, &s->friends[i], s->active_friend == i, friend_photo(s->friends[i].id))) app_open_friend(s, i);
                cui_pop_id(ui);
            }
        }
        /* rodapé do usuário (paridade com o .account-bar do Electron): foto, nome + status/@nick, e três ícones: @ (Perfil e conta), configurações, sair */
        cui_box(ui, .width = CUI_FILL, .pad = 10, .pad_top = s->user_status[0] ? 58 : 10) {   /* com status: o rodapé reserva a altura do balão, que nunca cobre a lista */
            const bool f_compact = g_panel_w < 220.0f;   /* painel estreito: sai o texto (nome/status), ficam a foto e os três ícones, sem nada cortado */
            cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad = 6, .pad_right = 8, .gap = g_panel_w < 300.0f ? 4 : 8, .radius = 16, .bg = B_GLASS_CARD, .clip = true,
                    .border = 1, .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER) {
                cui_box(ui, .id = "avatar") { cui_avatar_ex(ui, s->user_name, &(cui_avatar_opts){ .size = 32, .image = s->user_photo }); }
                if (briks_clicked(ui, "avatar")) { s->profile_open = true; s->profile_tab = 0; }
                bubble_note(cui_get_rect(ui, cui_make_id(ui, "avatar")), s->user_status);
                if (f_compact) cui_box(ui, .width = CUI_FILL) {}
                else cui_box(ui, .id = "ident", .dir = CUI_COLUMN, .gap = 1, .width = CUI_FILL, .pad_x = 4) {
                    cui_text(ui, s->user_name, .font = CUI_FONT_DISPLAY, .size = 14, .nowrap = true);
                    /* linha de status: presença em uso > status digitado > "Disponível" (verde) */
                    const char *mine = s->rp_enabled ? s->user_presence : "";
                    if (s->user_status[0]) cui_text(ui, s->user_status, .size = 11, .color = B_MUTED, .nowrap = true);
                    else if (mine[0]) cui_text(ui, mine, .size = 11, .color = B_MUTED, .nowrap = true);
                    else if (s->acct_user_id[0] && s->user_nick[0]) { char nk[48]; nick_text(s, nk, sizeof nk); cui_text(ui, nk, .size = 11, .color = B_MUTED, .nowrap = true); }   /* como no Electron: conta online mostra o @nick */
                    else cui_row(ui, .gap = 5, .align = CUI_ALIGN_CENTER) {
                        cui_box(ui, .width = CUI_PX(7), .height = CUI_PX(7), .radius = 4, .bg = B_ONLINE) {}
                        cui_text(ui, "Disponível", .size = 10.5f, .color = B_ONLINE, .nowrap = true);
                    }
                }
                if (!f_compact && briks_clicked(ui, "ident")) { s->profile_open = true; s->profile_tab = 1; }   /* clicar no status: abre Perfil e conta na aba Status */
                if (foot_icon(ui, "ft-at", CUI_ICON_AT_SIGN, "Perfil e conta")) { s->profile_open = true; s->profile_tab = 3; }
                if (foot_icon(ui, "ft-cfg", CUI_ICON_SETTINGS, "Configurações")) s->settings_open = true;
                if (foot_icon(ui, "ft-out", CUI_ICON_LOG_OUT, "Sair")) app_logout(s);
            }
        }
    }
}

/* "2026-11-09T23:41:00.000Z" (UTC) -> "20:41" no fuso local; de outro dia: "09/11 20:41"; full: "09/11/2026 20:41:00" (dica ao passar o mouse) */
static void hhmm_local_ex(const char *iso, char *out, size_t cap, bool full) {
    SYSTEMTIME u = { 0 }, l, now;
    if (sscanf(iso, "%hd-%hd-%hdT%hd:%hd:%hd", &u.wYear, &u.wMonth, &u.wDay, &u.wHour, &u.wMinute, &u.wSecond) == 6 &&
        SystemTimeToTzSpecificLocalTime(NULL, &u, &l)) {
        GetLocalTime(&now);
        if (full) snprintf(out, cap, "%02d/%02d/%04d %02d:%02d:%02d", l.wDay, l.wMonth, l.wYear, l.wHour, l.wMinute, l.wSecond);
        else if (l.wDay != now.wDay || l.wMonth != now.wMonth || l.wYear != now.wYear) snprintf(out, cap, "%02d/%02d %02d:%02d", l.wDay, l.wMonth, l.wHour, l.wMinute);
        else snprintf(out, cap, "%02d:%02d", l.wHour, l.wMinute);
    } else snprintf(out, cap, "--:--");
}
static void hhmm_local(const char *iso, char *out, size_t cap) { hhmm_local_ex(iso, out, cap, false); }
/* hora da mensagem; passar o mouse mostra data e hora completas */
static void time_label(cui_ctx *ui, const char *iso, const char *short_s, cui_color col) {
    cui_text(ui, short_s, .size = 10, .color = col);
    { char f[32]; hhmm_local_ex(iso, f, sizeof f, true); cui_tooltip(ui, f); }
}

static bool g_grouped;   /* mensagem consecutiva do mesmo autor: sem avatar nem nome (como o .grouped do Electron) */
static void bubble(cui_ctx *ui, app_state_t *s, const msg_t *m, bool interactive) {
    char hm[16]; hhmm_local(m->created, hm, sizeof hm);
    const char *text = interactive ? msgui_text(m) : m->content;
    /* bubble_align: top = topo do balão no topo do avatar; middle = a pontinha (quina reta no topo) na altura do centro do avatar */
    cui_box(ui, .id = "row", .dir = CUI_ROW, .width = CUI_FILL, .justify = m->mine ? CUI_JUSTIFY_END : CUI_JUSTIFY_START, .gap = 4,
            .align = m->mine ? CUI_ALIGN_DEFAULT : CUI_ALIGN_START) {
        if (!m->mine) { if (g_grouped) cui_box(ui, .width = CUI_PX(32)) {} else cui_box(ui) { cui_avatar_ex(ui, m->author, &(cui_avatar_opts){ .size = 32, .image = friend_photo_by_name(s, m->author) }); } }   /* avatar sem relevo; agrupada: só o espaço */
        if (m->mine) {
            if (interactive) msgui_arrow(ui, m);   /* seta do lado de dentro, como no WhatsApp: à esquerda do nosso balão */
            /* nosso: roxo padrão; só a quina superior direita mais reta */
            cui_box(ui, .id = "bub", .max_width = 520, .pad_x = 14, .pad_y = 9, .gap = 3,
                    .radius_tl = g_th.bubble_big, .radius_tr = g_th.bubble_small, .radius_br = g_th.bubble_big, .radius_bl = g_th.bubble_big,
                    .bg = g_th.bubble_own, .border = 1, .border_color = CUI_RGBA(0xffffff26)) {
                cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER) {
                    if (!g_grouped) cui_text(ui, "Você", .size = 12, .color = B_TEXT);
                    time_label(ui, m->created, hm, CUI_RGBA(0xffffffb0));
                    if (interactive) msgui_tags(ui, m);
                    if (interactive) attach_state_tag(ui, s, m);
                }
                if (interactive) msgui_quote(ui, s, m);
                if (interactive) attach_rich(ui, m->id, text); else cui_text(ui, text);
                if (interactive) attach_link_card(ui, s, text);
                if (interactive) msgui_reactions(ui, m);
            }
        } else {
            /* recebido: vidro; quina superior do lado do avatar com raio 4 */
            cui_box(ui, .pad_top = g_th.bubble_align ? 16 : 0) {   /* middle: o topo (a pontinha) do balão sai da altura do centro do avatar (32/2) */
            cui_box(ui, .id = "bub", .max_width = 520, .pad_x = 14, .pad_y = 9, .gap = 3,
                    .radius_tl = g_th.bubble_small, .radius_tr = g_th.bubble_big, .radius_br = g_th.bubble_big, .radius_bl = g_th.bubble_big, .bg = B_GLASS_CARD,
                    .border = 1, .border_color = text && rich_mentions_me(text, s->user_name, s->user_nick) ? B_C3 : B_GLASS_BORDER) {   /* borda de destaque: mencionaram você */
                cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER) {
                    if (!g_grouped) cui_text(ui, m->author, .size = 12, .color = B_C3);
                    time_label(ui, m->created, hm, g_th.ph);
                    if (interactive) msgui_tags(ui, m);
                }
                if (interactive) msgui_quote(ui, s, m);
                if (interactive) attach_rich(ui, m->id, text); else cui_text(ui, text);
                if (interactive) attach_link_card(ui, s, text);
                if (interactive) msgui_reactions(ui, m);
            }
            }
            if (interactive) msgui_arrow(ui, m);   /* à direita do balão alheio */
        }
    }
    if (interactive) msgui_row(ui, s, m);
}

static void content(cui_ctx *ui, app_state_t *s) {
    if (s->active_friend < 0 && s->active_group < 0) {   /* sem conversa aberta. SEM return dentro do cui_box (deixava a caixa aberta e quebrava os painéis seguintes) */
        const bool hub_here = !dock_has_panel(P_FRIENDS);   /* a tela Amigos (abas, busca, pedidos) fica aqui só quando não há o painel Amigos no layout */
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = B_GLASS, .radius = (int)g_th.panel_radius, .clip = true,
                .border = 1, .border_color = B_GLASS_BORDER, .elevation = briks_elev(CUI_ELEVATION_RAISED)) {
            if (hub_here) friends_hub(ui, s);
            else {   /* painel de conversa vazio: cabeçalho, estado vazio no centro e a caixa de escrever ancorada embaixo (inativa até escolher alguém) */
                cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(62), .pad_left = 20, .pad_right = 20 + g_tools_w, .gap = 10,
                        .radius_tl = (int)g_th.panel_radius - 1, .radius_tr = (int)g_th.panel_radius - 1, .bg = CUI_RGBA(0xffffff0a), .align = CUI_ALIGN_CENTER) {
                    cui_icon(ui, CUI_ICON_MESSAGE_SQUARE, 18, B_MUTED);
                    cui_text(ui, "Conversa", .font = CUI_FONT_MEDIUM, .nowrap = true);
                }
                cui_separator(ui);
                cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .gap = 8, .pad = 20) {
                    cui_box(ui, .width = CUI_PX(56), .height = CUI_PX(56), .radius = 28, .bg = CUI_RGBA(0xffffff10), .border = 1, .border_color = CUI_RGBA(0xffffff1c),
                            .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) { cui_icon(ui, CUI_ICON_MESSAGE_SQUARE, 24, B_C3); }
                    cui_text(ui, "Escolha uma conversa", .font = CUI_FONT_DISPLAY, .size = 17, .nowrap = true);
                    cui_text(ui, s->friend_count || s->group_count ? "Clique em um amigo ou grupo para abrir aqui." : "Adicione um amigo para começar a conversar.", .size = 12.5f, .color = B_MUTED);
                }
                cui_box(ui, .width = CUI_FILL, .pad = 12, .pad_top = 0) {
                    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(50), .pad_x = 16, .gap = 12, .radius = 18, .bg = B_GLASS_CARD,
                            .border = 1, .border_color = CUI_RGBA(0xffffff1c), .align = CUI_ALIGN_CENTER) {
                        cui_icon(ui, CUI_ICON_PAPERCLIP, 16, g_th.ph);
                        cui_text(ui, "Escolha uma conversa para escrever", .color = g_th.ph, .nowrap = true, .width = CUI_FILL);
                        cui_icon(ui, CUI_ICON_SEND, 16, g_th.ph);
                    }
                }
            }
        }
        return;
    }
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = B_GLASS, .radius = (int)g_th.panel_radius, .clip = true,
            .border = 1, .border_color = B_GLASS_BORDER, .elevation = briks_elev(CUI_ELEVATION_RAISED)) {
        const bool group = s->active_group >= 0;
        const char *title = group ? s->groups[s->active_group].name : s->friends[s->active_friend].name;
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(62), .pad_left = 20, .pad_right = 20 + g_tools_w, .gap = 10,
                .radius_tl = (int)g_th.panel_radius - 1, .radius_tr = (int)g_th.panel_radius - 1, .bg = CUI_RGBA(0xffffff0a), .align = CUI_ALIGN_CENTER) {
            cui_icon(ui, group ? CUI_ICON_USERS : CUI_ICON_MESSAGE_SQUARE, 18, B_MUTED);
            cui_text(ui, title, .font = CUI_FONT_MEDIUM, .nowrap = true);
            { char sub[96]; if (group && s->groups[s->active_group].nch) snprintf(sub, sizeof sub, "Grupo P2P · #%s", s->groups[s->active_group].ch[s->groups[s->active_group].active_ch].name); else snprintf(sub, sizeof sub, "%s", group ? "Grupo P2P" : "Mensagem direta P2P");
              if (g_panel_w >= 620.0f * cui_theme_get(ui)->font_scale) cui_text(ui, sub, .size = 12, .color = B_MUTED, .nowrap = true); }   /* painel estreito ou texto grande: a legenda sai, o título fica inteiro */
            if (group) groups_header(ui, s);
            if (!group) briks_status_pill(ui, s->friends[s->active_friend].status);   /* só se o amigo tiver status */
            { const char *ty = attach_typing_label(s); if (ty) cui_text(ui, ty, .size = 12, .color = B_C3, .nowrap = true); }
            msgui_pinned_button(ui, s);
            attach_search_button(ui);
        }
        cui_separator(ui);
        friends_notify_banner(ui, s);
        msgui_set_view(cui_get_rect(ui, cui_make_id(ui, "chat-scroll")));
        cui_scroll(ui, .id = "chat-scroll", .width = CUI_FILL, .height = CUI_FILL, .pad = g_th.chat_pad) {
            if (!s->msg_count) {
                cui_box(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .pad_top = 60, .gap = 8) {
                    cui_icon(ui, CUI_ICON_MESSAGE_SQUARE, 34, B_C3);
                    cui_textf(ui, "Conversa com %s", title);
                    cui_text(ui, "As mensagens ficam gravadas só neste computador.", .size = 12, .color = B_MUTED);
                }
            }
            cui_column(ui, .gap = 10, .width = CUI_FILL) {
                int tcur = 0;   /* anexos concluídos entram na linha do tempo, na posição do horário */
                for (int i = 0; i < s->msg_count; i++) { attach_timeline(ui, s, &tcur, s->msgs[i].created); if (msgui_deleted(s->msgs[i].id) || !attach_search_match(msgui_text(&s->msgs[i]))) continue; cui_push_id_int(ui, 5000 + i); g_grouped = attach_grouped(s, i); bubble(ui, s, &s->msgs[i], true); g_grouped = false; cui_pop_id(ui); }
                attach_timeline(ui, s, &tcur, NULL);
            }
        }
        cui_box(ui, .width = CUI_FILL, .pad = 12, .pad_top = 0, .gap = 6) {
            attach_strip(ui, s);
            attach_mentions(ui, s);
            msgui_composer(ui, s);
            cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad = 7, .gap = 6, .radius = 18, .bg = B_GLASS_CARD,
                    .border = 1, .border_color = B_A(B_C3, 0x55), .align = CUI_ALIGN_CENTER) {
                if (cui_icon_button(ui, CUI_ICON_PAPERCLIP, .tooltip = "Anexar arquivo (envia aos participantes da sala)")) attach_pick(s);
                int r = cui_input(ui, "draft", s->draft, sizeof s->draft, .placeholder = "Escreva uma mensagem", .width = CUI_FILL);
                { static char prev[1100]; if (strcmp(prev, s->draft)) { if (s->draft[0]) attach_typing_ping(s); snprintf(prev, sizeof prev, "%s", s->draft); } }
                bool send = cui_icon_button(ui, CUI_ICON_SEND, .variant = CUI_BUTTON_PRIMARY, .tooltip = "Enviar");
                if ((send || (r & CUI_SUBMITTED)) && s->draft[0]) msgui_submit(s);
            }
        }
    }
}

static void panel_servers(cui_ctx *ui, app_state_t *s) {
    bool wide = g_panel_w >= 110.0f;
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .pad_x = 10, .pad_y = 12, .gap = 10, .bg = B_GLASS, .radius = (int)g_th.panel_radius, .clip = true,
                .border = 1, .border_color = B_GLASS_BORDER, .elevation = briks_elev(CUI_ELEVATION_RAISED), .align = wide ? CUI_ALIGN_START : CUI_ALIGN_CENTER) {
            int click = -1;
            space_button(ui, "B", "Briks", "Início", s->active_space == 0, false, &click, 0);
            if (s->community_count) cui_box(ui, .width = CUI_PX(wide ? g_panel_w - 20.0f : 32), .height = CUI_PX(1), .bg = CUI_RGBA(0xffffff14)) {}
            for (int i = 0; i < s->community_count; i++) {
                char ini[3]; initials(s->communities[i].name, ini);
                char sub[32]; int mm = s->communities[i].members > 0 ? s->communities[i].members : 1;
                snprintf(sub, sizeof sub, mm == 1 ? "1 membro" : "%d membros", mm);
                space_button(ui, ini, s->communities[i].name, sub, s->active_space == i + 1, false, &click, i + 1);
            }
            space_button(ui, "+", "Criar grupo", "", false, true, &click, 999);
            if (click == 999) { s->create_group_open = true; click = -1; }
            if (click >= 0) s->active_space = click;
        }
}


static void panel_shell(cui_ctx *ui, const char *title, const char *icon) {
    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(56), .pad_left = 18, .pad_right = 18 + g_tools_w, .gap = 10, .align = CUI_ALIGN_CENTER) {
        cui_icon(ui, icon, 16, B_MUTED);
        cui_text(ui, title, .font = CUI_FONT_DISPLAY, .size = 15, .nowrap = true);
    }
    cui_separator(ui);
}
#define GLASS_PANEL .width = CUI_FILL, .height = CUI_FILL, .bg = B_GLASS, .radius = (int)g_th.panel_radius, .clip = true, \
        .border = 1, .border_color = B_GLASS_BORDER, .elevation = briks_elev(CUI_ELEVATION_RAISED)

static void panel_friends(cui_ctx *ui, app_state_t *s) {   /* painel Amigos: a tela completa (abas, busca, pedidos, bloqueados, adicionar) */
    cui_box(ui, GLASS_PANEL) { friends_hub(ui, s); }
}

static void panel_members(cui_ctx *ui, app_state_t *s) {
    cui_box(ui, GLASS_PANEL) {
        panel_shell(ui, "Membros", CUI_ICON_USERS);
        g_bclip = cui_get_rect(ui, cui_make_id(ui, "members-scroll"));
        cui_scroll(ui, .id = "members-scroll", .width = CUI_FILL, .height = CUI_FILL, .pad = 9) {
            person_t me; memset(&me, 0, sizeof me); snprintf(me.name, sizeof me.name, "%s", s->user_name[0] ? s->user_name : "Você");
            person_row_ex(ui, &me, false, s->user_photo);
            for (int i = 0; i < s->friend_count; i++) { cui_push_id_int(ui, 7500 + i); person_row_ex(ui, &s->friends[i], false, friend_photo(s->friends[i].id)); cui_pop_id(ui); }
        }
    }
}

static void panel_voice(cui_ctx *ui, app_state_t *s) {
    cui_box(ui, GLASS_PANEL) {
        panel_shell(ui, "Sala de voz", CUI_ICON_VOLUME_2);
        ui_voice_panel(ui, s);
    }
}

static void panel_history(cui_ctx *ui, app_state_t *s) {
    (void)s;
    cui_box(ui, GLASS_PANEL) {
        panel_shell(ui, "Histórico de chamadas", CUI_ICON_HISTORY);
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .gap = 8, .pad = 16) {
            cui_icon(ui, CUI_ICON_HISTORY, 34, B_C3);
            cui_text(ui, "Nenhuma chamada ainda", .font = CUI_FONT_DISPLAY, .size = g_panel_w < 300.0f ? 14 : 18, .nowrap = true);
        }
    }
}


static void panel_chat2(cui_ctx *ui, app_state_t *s) {
    if (!s->chat2_channel[0] && s->friend_count) app_open_chat2(s, s->chat2_friend < s->friend_count ? s->chat2_friend : 0);
    cui_box(ui, GLASS_PANEL) {
        const bool has = s->chat2_friend >= 0 && s->chat2_friend < s->friend_count;
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(56), .pad_left = 18, .pad_right = 18 + g_tools_w, .gap = 10, .align = CUI_ALIGN_CENTER,
                .radius_tl = (int)g_th.panel_radius - 1, .radius_tr = (int)g_th.panel_radius - 1, .bg = CUI_RGBA(0xffffff0a)) {
            cui_icon(ui, CUI_ICON_MESSAGE_SQUARE, 16, B_MUTED);
            cui_text(ui, has ? s->friends[s->chat2_friend].name : "Escolha um amigo", .font = CUI_FONT_MEDIUM, .nowrap = true);   /* só o nome de quem está aberto; nome de painel fica nos menus de layout */
        }
        cui_separator(ui);
        if (!has || !s->msg2_count) {
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .gap = 8, .pad = 20) {
                cui_box(ui, .width = CUI_PX(48), .height = CUI_PX(48), .radius = 24, .bg = CUI_RGBA(0xffffff10), .border = 1, .border_color = CUI_RGBA(0xffffff1c),
                        .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) { cui_icon(ui, CUI_ICON_MESSAGE_SQUARE, 20, B_C3); }
                cui_text(ui, has ? "Sem mensagens ainda" : "Nenhuma conversa aqui", .font = CUI_FONT_MEDIUM, .nowrap = true);
                cui_text(ui, has ? "O que vocês escreverem aparece aqui." : s->friend_count ? "Use Escolher amigo, aqui embaixo, para abrir a conversa." : "Adicione um amigo para abrir uma conversa neste painel.", .size = 12, .color = B_MUTED);
            }
        } else cui_scroll(ui, .id = "chat2-scroll", .width = CUI_FILL, .height = CUI_FILL, .pad = 16) {
            cui_column(ui, .gap = 10, .width = CUI_FILL) {
                for (int i = 0; i < s->msg2_count; i++) { cui_push_id_int(ui, 8000 + i); bubble(ui, s, &s->msgs2[i], false); cui_pop_id(ui); }
            }
        }
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad = 12, .gap = 8, .align = CUI_ALIGN_CENTER) {
            if (s->friend_count > 0) cui_menu(ui, s->chat2_friend >= 0 && s->chat2_friend < s->friend_count ? "Trocar amigo" : "Escolher amigo", .placement = CUI_PLACE_TOP_START) {
                for (int i = 0; i < s->friend_count; i++) { cui_push_id_int(ui, 8600 + i); if (cui_menu_item(ui, s->friends[i].name)) app_open_chat2(s, i); cui_pop_id(ui); }
            }
        }
    }
}

/* chamado pelo dock: desenha o conteúdo do painel dentro do retângulo que ele reservou */
void ui_panel(cui_ctx *ui, app_state_t *s, int kind) {
    switch (kind) {
    case P_SERVERS: panel_servers(ui, s); break;
    case P_NAV: navigation(ui, s); break;
    case P_CHAT: content(ui, s); break;
    case P_FRIENDS: panel_friends(ui, s); break;
    case P_MEMBERS: panel_members(ui, s); break;
    case P_VOICE: panel_voice(ui, s); break;
    case P_HISTORY: panel_history(ui, s); break;
    case P_CHAT2: panel_chat2(ui, s); break;
    }
}

/* ---- Perfil e conta: diálogo vertical e compacto; cada seção é um cartão de vidro arredondado ---- */
#define PCARD(ui, title) CUI__SCOPE((cui_box_begin((ui), CUI__OPTS(cui_box, .width = CUI_FILL, .pad = 14, .gap = 10, .radius = 16, .bg = B_GLASS_CARD, \
    .border = 1, .border_color = B_GLASS_BORDER)), cui_text((ui), (title), .font = CUI_FONT_MEDIUM, .size = 13, .color = B_C3)), cui_box_end(ui))

static bool profile_nav(cui_ctx *ui, const char *icon, const char *label, bool active) {
    bool hit = false;
    cui_box(ui, .id = label, .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 12, .pad_y = 9, .gap = 10, .radius = 12, .align = CUI_ALIGN_CENTER,
            .bg = active ? B_A(B_C3, 0x33) : briks_hover(ui, label) ? (cui_mouse_down(ui, CUI_MOUSE_LEFT) ? CUI_RGBA(0xffffff24) : CUI_RGBA(0xffffff12)) : CUI_RGBA(0x00000000), .border = active ? 1 : 0, .border_color = B_A(B_C3, 0x66)) {   /* hover e clique visíveis também dentro do diálogo */
        cui_icon(ui, icon, 16, active ? B_TEXT : B_MUTED);
        cui_text(ui, label, .color = active ? B_TEXT : B_MUTED, .nowrap = true);
    }
    hit = briks_clicked(ui, label);
    return hit;
}

/* @nick exibido: o da conta (account.c) quando há sessão online; senão deriva do e-mail local */
static void nick_text(const app_state_t *s, char *out, size_t cap) {
    if (s->acct_user_id[0] && s->user_nick[0]) snprintf(out, cap, "@%s", s->user_nick);
    else snprintf(out, cap, "@%.*s", (int)strcspn(s->user_email, "@"), s->user_email);
}

static void profile_dialog(cui_ctx *ui, app_state_t *s) {
    static char st_draft[64], name_draft[64], ig_add[32]; static bool was_open;
    static const char *CHIPS[] = { "Jogando", "Estudando", "Trabalhando", "Ouvindo música", "Ausente" };
    static const char *CLEAR[] = { "Não limpar", "1 hora", "4 horas", "Hoje" };   /* mesmas opções do Electron (StatusEditor) */
    /* a fonte do app não traz emoji colorido: o seletor usa os ícones do CUI (UI pronta; emoji de verdade depende de fonte) */
    static const char *const EMOJI[] = { CUI_ICON_SMILE, CUI_ICON_MUSIC, CUI_ICON_BRIEFCASE, CUI_ICON_MOON, CUI_ICON_ROCKET, CUI_ICON_HEART, CUI_ICON_STAR, CUI_ICON_BOOKMARK };
    static bool emoji_open;
    if (s->profile_open && !was_open) {
        snprintf(st_draft, sizeof st_draft, "%s", s->user_status); snprintf(name_draft, sizeof name_draft, "%s", s->user_name); ig_add[0] = 0; emoji_open = false;
    }
    was_open = s->profile_open;
    cui_vec2 vp = cui_viewport_size(ui);
    const float dw = fminf(880.0f, fmaxf(vp.x - 48.0f, 320.0f)), dh = fminf(480.0f, fmaxf(vp.y - 160.0f, 240.0f));
    cui_dialog(ui, "Perfil e conta", &s->profile_open, .width = dw) {
        cui_row(ui, .width = CUI_FILL, .height = CUI_PX(dh), .gap = 16) {
            /* lateral: foto + nome + @nick, seções e o botão discreto de desconectar no rodapé */
            cui_box(ui, .width = CUI_PX(236), .height = CUI_FILL, .pad = 12, .gap = 4, .radius = 18, .bg = B_GLASS_CARD, .border = 1, .border_color = B_GLASS_BORDER) {
                cui_row(ui, .gap = 12, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .pad_bottom = 10) {
                    cui_avatar_ex(ui, s->user_name, &(cui_avatar_opts){ .size = 56, .image = s->user_photo });
                    cui_column(ui, .gap = 1, .width = CUI_FILL) {
                        cui_text(ui, s->user_name, .font = CUI_FONT_DISPLAY, .size = 16, .nowrap = true);
                        char nick[100]; nick_text(s, nick, sizeof nick);
                        cui_text(ui, nick, .color = B_MUTED, .size = 12, .nowrap = true);
                    }
                }
                cui_separator(ui);
                static const char *NAMES[] = { "Perfil", "Status", "Programa em uso", "Conta" };
                static const char *const ICONS[] = { CUI_ICON_USER, CUI_ICON_MESSAGE_SQUARE, CUI_ICON_SETTINGS, CUI_ICON_USERS };
                for (int i = 0; i < 4; i++) { cui_push_id_int(ui, 4300 + i); if (profile_nav(ui, ICONS[i], NAMES[i], s->profile_tab == i)) s->profile_tab = i; cui_pop_id(ui); }
                cui_box(ui, .width = CUI_FILL, .height = CUI_FILL) {}
                if (cui_button(ui, "Desconectar", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM, .full_width = true)) s->logout_confirm_open = true;
            }
            /* conteúdo: uma seção por vez */
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 12, .scroll_y = true) {
                switch (s->profile_tab) {
                case 0:
                    if (s->acct_user_id[0]) { ui_account_editor(ui, s); break; }   /* conta online: foto, nome e @nick vêm do servidor */
                    PCARD(ui, "PERFIL") {
                        cui_row(ui, .gap = 18, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
                            cui_avatar_ex(ui, s->user_name, &(cui_avatar_opts){ .size = 96, .image = s->user_photo });
                            cui_column(ui, .gap = 8, .width = CUI_FILL) {
                                cui_text(ui, "Nome exibido", .size = 12, .color = B_MUTED);
                                cui_input(ui, "pf-name", name_draft, sizeof name_draft, .placeholder = "Seu nome");
                                cui_button(ui, "Escolher foto", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM);   /* UI pronta, sem backend de upload */
                            }
                        }
                        cui_row(ui, .gap = 8, .width = CUI_FILL, .justify = CUI_JUSTIFY_END) {
                            if (cui_button(ui, "Salvar perfil", .variant = CUI_BUTTON_PRIMARY) && name_draft[0])
                                snprintf(s->user_name, sizeof s->user_name, "%s", name_draft);
                        }
                    }
                    break;
                case 1:
                    PCARD(ui, "STATUS") {
                        cui_text(ui, "Aparece num balão perto da sua foto.", .size = 12, .color = B_MUTED);
                        cui_row(ui, .gap = 8, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
                            if (cui_icon_button(ui, s->user_emoji[0] ? s->user_emoji : CUI_ICON_SMILE, .tooltip = "Escolher emoji")) emoji_open = !emoji_open;
                            cui_input(ui, "pf-status", st_draft, sizeof st_draft, .placeholder = "O que está pensando?", .width = CUI_FILL);
                        }
                        if (emoji_open) {
                            cui_row(ui, .gap = 4, .width = CUI_FILL, .wrap = true, .pad = 6, .radius = 12, .bg = CUI_RGBA(0xffffff0a)) {
                                for (int i = 0; i < 8; i++) {
                                    cui_push_id_int(ui, 4400 + i);
                                    if (cui_icon_button(ui, EMOJI[i])) { snprintf(s->user_emoji, sizeof s->user_emoji, "%s", EMOJI[i]); emoji_open = false; }
                                    cui_pop_id(ui);
                                }
                            }
                        }
                        cui_row(ui, .gap = 6, .width = CUI_FILL, .wrap = true) {
                            for (int i = 0; i < 5; i++) {
                                cui_push_id_int(ui, 4100 + i);
                                bool sel = !strcmp(st_draft, CHIPS[i]);
                                if (cui_chip(ui, CHIPS[i], &sel)) snprintf(st_draft, sizeof st_draft, "%s", sel ? CHIPS[i] : "");
                                cui_pop_id(ui);
                            }
                        }
                        cui_text(ui, "Limpar depois de", .size = 12, .color = B_MUTED);
                        cui_segmented(ui, "pf-clear", CLEAR, 4, &s->status_clear, .full_width = true, .size = CUI_SIZE_SM);
                        cui_row(ui, .gap = 8, .width = CUI_FILL, .justify = CUI_JUSTIFY_END) {
                            if (cui_button(ui, "Limpar", .variant = CUI_BUTTON_SECONDARY)) { st_draft[0] = 0; s->user_emoji[0] = 0; s->user_status[0] = 0; }
                            if (cui_button(ui, "Salvar status", .variant = CUI_BUTTON_PRIMARY)) snprintf(s->user_status, sizeof s->user_status, "%s", st_draft);
                        }
                    }
                    break;
                case 2:
                    PCARD(ui, "PRESENÇA") {
                        cui_switch(ui, "Aparecer offline para os amigos", &s->invisible);
                        { bool on = !s->notify_off; cui_switch(ui, "Avisar quando um amigo ficar online", &on); s->notify_off = !on; }
                        cui_text(ui, s->invisible ? "Seus amigos não veem que você está online. Você continua usando tudo normalmente." : "Seus amigos veem um ponto verde quando você está com o Briks aberto.", .size = 12, .color = B_MUTED);
                    }
                    PCARD(ui, "PROGRAMA EM USO") {
                        cui_switch(ui, "Mostrar o programa que estou usando", &s->rp_enabled);
                        if (s->rp_enabled) {
                            static const char *AUD[] = { "Todos os amigos", "Só na chamada" };
                            cui_text(ui, "Quem pode ver", .size = 12, .color = B_MUTED);
                            cui_segmented(ui, "pf-aud", AUD, 2, &s->rp_audience, .full_width = true, .size = CUI_SIZE_SM);
                            cui_text(ui, "Ignorar programa", .size = 12, .color = B_MUTED);
                            cui_row(ui, .gap = 8, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
                                cui_input(ui, "pf-ignore", ig_add, sizeof ig_add, .placeholder = "ex.: spotify", .width = CUI_FILL);
                                if (cui_button(ui, "Ignorar", .variant = CUI_BUTTON_SECONDARY) && ig_add[0] && s->rp_ignore_n < 6) {
                                    snprintf(s->rp_ignore[s->rp_ignore_n++], sizeof s->rp_ignore[0], "%s", ig_add); ig_add[0] = 0;
                                }
                            }
                            cui_row(ui, .gap = 6, .width = CUI_FILL, .wrap = true) {
                                for (int i = 0; i < s->rp_ignore_n; i++) {
                                    cui_push_id_int(ui, 4200 + i);
                                    cui_row(ui, .gap = 6, .pad_x = 10, .pad_y = 4, .radius = 14, .bg = CUI_RGBA(0xffffff10), .align = CUI_ALIGN_CENTER) {
                                        cui_text(ui, s->rp_ignore[i], .size = 13, .nowrap = true);
                                        if (cui_icon_button(ui, CUI_ICON_X, .size = CUI_SIZE_SM, .tooltip = "Remover")) {
                                            memmove(s->rp_ignore[i], s->rp_ignore[i + 1], (size_t)(s->rp_ignore_n - i - 1) * sizeof s->rp_ignore[0]);
                                            s->rp_ignore_n--;
                                        }
                                    }
                                    cui_pop_id(ui);
                                }
                            }
                        }
                    }
                    break;
                default:
                    PCARD(ui, "CONTA") {
                        cui_row(ui, .gap = 8, .width = CUI_FILL) { cui_text(ui, "E-mail", .color = B_MUTED, .width = CUI_FILL); cui_text(ui, s->user_email, .nowrap = true); }
                        char nick[100]; nick_text(s, nick, sizeof nick);
                        cui_row(ui, .gap = 8, .width = CUI_FILL) { cui_text(ui, "Nick", .color = B_MUTED, .width = CUI_FILL); cui_text(ui, nick, .nowrap = true); }
                        if (!s->acct_user_id[0]) cui_text(ui, "Conta local: os dados ficam só neste computador.", .size = 12, .color = B_MUTED);
                        cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_END) {
                            if (s->acct_user_id[0] && cui_button(ui, "Trocar @nick", .variant = CUI_BUTTON_SECONDARY)) {
                                s->profile_open = false; s->nick_open = true; s->nick_msg[0] = 0;
                                snprintf(s->nick_edit, sizeof s->nick_edit, "%s", s->acct_nick);
                            }
                            if (cui_button(ui, "Sair", .variant = CUI_BUTTON_SECONDARY)) { s->profile_open = false; app_logout(s); }
                        }
                    }
                    if (s->acct_user_id[0]) {
                        ui_others_section(ui, s);
                        ui_backup_section(ui, s);
                        if (s->import_state == 1 || s->import_state == 2 || s->import_state == 3) {
                            PCARD(ui, "BRIKS ANTIGO") {
                                if (s->import_msg[0]) cui_text(ui, s->import_msg, .size = 12, .color = B_MUTED);
                                if (s->import_state != 3 && cui_button(ui, "Importar amigos do Briks antigo", .variant = CUI_BUTTON_SECONDARY, .loading = s->import_state == 2)) account_import(s, true);
                                if (s->import_state != 3) cui_text(ui, "Traz contatos, grupos e mensagens. A identidade antiga não é importada e nada é alterado no Briks antigo.", .size = 12, .color = B_MUTED);
                            }
                        }
                    }
                    break;
                }
            }
        }
    }
    cui_dialog(ui, "Desconectar?", &s->logout_confirm_open, .width = fminf(380.0f, fmaxf(vp.x - 48.0f, 260.0f))) {
        cui_text(ui, "Você precisará entrar de novo para usar o Briks.", .color = B_MUTED);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Cancelar", .variant = CUI_BUTTON_SECONDARY)) s->logout_confirm_open = false;
            if (cui_button(ui, "Desconectar", .variant = CUI_BUTTON_DANGER)) { s->logout_confirm_open = false; s->profile_open = false; app_logout(s); }
        }
    }
}

void ui_social(cui_ctx *ui, app_state_t *s) {
    briks_ambient(ui, 0.85f);
    dock_render(ui, s);
    cui_dialog(ui, "Adicionar amigo", &s->add_friend_open) {
        const bool online = s->acct_user_id[0] != 0;
        cui_text(ui, online ? "Digite o @nick do seu amigo. Ele recebe o pedido e, ao aceitar, vocês passam a se achar pela conta." : "Informe o e-mail da conta que você quer adicionar.", .color = B_MUTED);
        cui_input(ui, "add-email", s->add_email, sizeof s->add_email, .placeholder = online ? "@nick" : "amigo@email.com", .autofocus = true);
        if (online && s->acct_nick[0]) cui_textf(ui, "O seu é @%s", s->acct_nick);
        if (s->notice[0]) cui_text(ui, s->notice, .color = B_MUTED, .size = 13);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Enviar pedido", .variant = CUI_BUTTON_PRIMARY, .loading = s->busy)) app_add_friend(s);
        }
    }
    cui_dialog(ui, "Criar grupo", &s->create_group_open) {
        cui_text(ui, "O grupo fica gravado neste computador. Para chamar alguém, abra “Membros” e convide um amigo: você recebe um código para mandar a ele.", .color = B_MUTED);
        int r = cui_input(ui, "new-group", s->new_group, sizeof s->new_group, .placeholder = "Nome do grupo", .autofocus = true);
        cui_dialog_footer(ui) {
            if ((cui_button(ui, "Criar", .variant = CUI_BUTTON_PRIMARY, .loading = s->busy) || (r & CUI_SUBMITTED))) app_create_group(s);
        }
    }
    profile_dialog(ui, s);
    friends_dialogs(ui, s);
    groups_dialogs(ui, s);
    cui_dialog(ui, "Trocar @nick", &s->nick_open) {
        cui_text(ui, "De 3 a 20 caracteres: letras minúsculas, números e _. Depois de escolher, só dá para trocar de novo em 30 dias.", .color = B_MUTED);
        int nr = cui_input(ui, "nick-edit", s->nick_edit, sizeof s->nick_edit, .placeholder = "seu_nick", .autofocus = true);
        if (s->nick_msg[0]) cui_text(ui, s->nick_msg, .color = B_MUTED, .size = 13);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Salvar", .variant = CUI_BUTTON_PRIMARY) || (nr & CUI_SUBMITTED)) account_set_nick(s);
        }
    }
    bubbles_draw(ui);
    msgui_overlay(ui, s);
    attach_overlay(ui, s);
    if (g_perf_hint) {   /* hardware fraco detectado: aviso discreto no canto, sem bloquear nada */
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_END, .justify = CUI_JUSTIFY_START, .pad = 14) {   /* canto superior direito: nunca sobre a caixa de escrever */
            cui_box(ui, .dir = CUI_ROW, .gap = 10, .pad_x = 14, .pad_y = 8, .radius = 14, .bg = B_A(B_PANEL, 0xf2), .border = 1,
                    .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, briks_perf_hint_text(), .size = 12);   /* frase sorteada a cada exibição (perf.c) */
                char lab[48]; snprintf(lab, sizeof lab, "Ativar %s", briks_perf_label(briks_perf_hint_level()));
                if (cui_button(ui, lab, .variant = CUI_BUTTON_PRIMARY)) briks_perf_set(ui, briks_perf_hint_level(), true);
                if (cui_button(ui, "Agora não", .variant = CUI_BUTTON_SECONDARY)) briks_perf_hint_dismiss();
            }
        }
    }
    cui_vec2 vp = cui_viewport_size(ui);
    settings_dialog(ui, s);
    ui_backup_dialogs(ui, s);
    /* o dono da sala expulsou você */
    cui_dialog(ui, "Chamada", &s->kicked_open, .width = fminf(380.0f, fmaxf(vp.x - 48.0f, 260.0f))) {
        cui_text(ui, "Você foi removido da chamada.", .font = CUI_FONT_MEDIUM);
        cui_text(ui, "Você não pode voltar a esta sala nesta sessão até o dono permitir.", .size = 13, .color = B_MUTED);
        if (cui_button(ui, "Entendi", .variant = CUI_BUTTON_PRIMARY)) s->kicked_open = false;
    }
}
