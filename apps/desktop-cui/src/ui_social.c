#include "briks_ui.h"
#include <math.h>
#include "layout.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

static void initials(const char *name, char *out) {
    out[0] = name[0] ? name[0] : '?';
    const char *sp = strchr(name, ' ');
    out[1] = sp && sp[1] ? sp[1] : 0;
    out[2] = 0;
}

static void space_button(cui_ctx *ui, const char *label, bool active, bool dashed, int *clicked_id, int id) {
    cui_push_id_int(ui, id);
    cui_box(ui, .id = "b", .width = CUI_PX(50), .height = CUI_PX(50), .radius = active ? 13 : 17,
            .bg = active ? B_C3 : dashed ? CUI_RGBA(0x8b5cf60b) : CUI_RGB(0x171c24),
            .border = dashed ? 1 : 0, .border_color = CUI_RGBA(0x8b5cf666),
            .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
        cui_text(ui, label, .size = 13, .color = active ? CUI_RGB(0xffffff) : dashed ? B_C3 : CUI_RGB(0xc9d1dc));
    }
    if (briks_clicked(ui, "b")) *clicked_id = id;
    cui_pop_id(ui);
}

static bool nav_item(cui_ctx *ui, const char *icon, const char *label, bool active) {
    cui_push_id(ui, label);
    cui_box(ui, .id = "b", .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 10, .pad_y = 9, .gap = 10, .radius = 12,
            .bg = active ? CUI_RGBA(0x6d5cf638) : CUI_RGBA(0x00000000),
            .border = active ? 1 : 0, .border_color = CUI_RGBA(0xffffff1a), .align = CUI_ALIGN_CENTER) {
        cui_icon(ui, icon, 16, active ? B_TEXT : CUI_RGB(0x8d97a6));
        cui_text(ui, label, .color = active ? B_TEXT : CUI_RGB(0xa592b8), .nowrap = true);
    }
    bool hit = briks_clicked(ui, "b");
    cui_pop_id(ui);
    return hit;
}

static void section_title(cui_ctx *ui, const char *t) {
    cui_box(ui, .pad_left = 9, .pad_top = 22, .pad_bottom = 7) {
        cui_text(ui, t, .size = 10, .color = CUI_RGB(0x8472a0));
    }
}

/* Balões de pensamento (Liquid Glass): as telas registram onde fica o avatar e o texto; o desenho vem por cima de tudo no fim do quadro. */
typedef struct { cui_rect av; char text[64]; } bub_note_t;
static bub_note_t g_bub[4]; static int g_bub_n;
static void bubble_note(cui_rect av, const char *text) {
    if (g_bub_n < 4 && av.w > 0 && text[0]) { g_bub[g_bub_n].av = av; snprintf(g_bub[g_bub_n].text, sizeof g_bub[g_bub_n].text, "%s", text); g_bub_n++; }
}
static void abs_glass(cui_ctx *ui, float x, float y, float w, float h, float r, bool shadow) {
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_START, .pad_left = x, .pad_top = y) {
        cui_box(ui, .width = CUI_PX(w), .height = CUI_PX(h), .radius = r, .bg = B_GLASS_BUBBLE, .border = 1, .border_color = CUI_RGBA(0xffffff30),
                .elevation = shadow ? CUI_ELEVATION_RESTING : CUI_ELEVATION_NONE) {}
    }
}
static void bubbles_draw(cui_ctx *ui) {
    for (int i = 0; i < g_bub_n; i++) {
        cui_push_id_int(ui, 9100 + i);
        cui_rect a = g_bub[i].av; const char *t = g_bub[i].text;
        cui_text_metrics m = cui_measure_text(ui, CUI_FONT_BODY, 13, t, (int)strlen(t));
        float w = fminf(m.width, 190.0f) + 30, h = 32;
        float bx = a.x + 8, by = a.y - 22 - h;
        if (by < 6) by = 6;
        abs_glass(ui, a.x + 26, a.y - 4, 9, 9, 4.5f, false);           /* bolinhas de pensamento, da menor (junto do avatar) à maior */
        abs_glass(ui, a.x + 31, a.y - 18, 14, 14, 7, false);
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_START, .pad_left = bx, .pad_top = by) {
            cui_box(ui, .width = CUI_PX(w), .height = CUI_PX(h), .radius = 16, .bg = B_GLASS_BUBBLE, .border = 1, .border_color = CUI_RGBA(0xffffff30),
                    .elevation = CUI_ELEVATION_RESTING, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                cui_text(ui, t, .size = 13, .color = CUI_RGB(0xffffff), .nowrap = true);
            }
        }
        cui_pop_id(ui);
    }
    g_bub_n = 0;
}

static bool person_row(cui_ctx *ui, const person_t *f, bool active) {
    cui_push_id(ui, f->name);
    if (active && f->status[0]) cui_gap(ui, 52);   /* reserva o lugar do balão de status: ele não cobre o título nem outros itens */
    cui_box(ui, .id = "b", .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 8, .pad_y = 6, .gap = 10, .radius = 12,
            .bg = active ? CUI_RGBA(0x6d5cf638) : CUI_RGBA(0x00000000), .align = CUI_ALIGN_CENTER) {
        cui_box(ui, .id = "av") { cui_avatar(ui, f->name, .size = 32); }
        const char *line = f->presence;   /* a linha sob o nome é só o rich presence; o status vai no balão */
        if (line[0]) {   /* rich presence: linha pequena sob o nome */
            cui_column(ui, .gap = 0, .width = CUI_FILL) {
                cui_text(ui, f->name, .nowrap = true);
                cui_text(ui, line, .size = 11, .color = B_MUTED, .nowrap = true);
            }
        } else cui_text(ui, f->name, .nowrap = true, .width = CUI_FILL);
        if (active && f->status[0]) bubble_note(cui_get_rect(ui, cui_make_id(ui, "av")), f->status);
    }
    bool hit = briks_clicked(ui, "b");
    cui_pop_id(ui);
    return hit;
}

static void navigation(cui_ctx *ui, app_state_t *s) {
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = B_GLASS, .radius = (int)g_th.panel_radius, .clip = true,
            .border = 1, .border_color = B_GLASS_BORDER, .elevation = briks_elev(CUI_ELEVATION_RAISED)) {
        cui_box(ui, .dir = CUI_ROW, .height = CUI_PX(62), .width = CUI_FILL, .pad_x = 20, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, s->active_friend >= 0 ? s->friends[s->active_friend].name : "Briks",
                     .font = CUI_FONT_DISPLAY, .size = 16, .nowrap = true);
        }
        cui_separator(ui);
        cui_scroll(ui, .id = "nav-scroll", .width = CUI_FILL, .height = CUI_FILL, .pad = 9) {
            if (nav_item(ui, CUI_ICON_USERS, "Amigos", s->active_friend < 0)) s->active_friend = -1;
            if (nav_item(ui, CUI_ICON_USER_PLUS, "Adicionar amigo", false)) { s->add_friend_open = true; s->notice[0] = 0; }
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
            for (int i = 0; i < s->group_count; i++) {
                cui_push_id_int(ui, 2000 + i);
                person_t g; memset(&g, 0, sizeof g); snprintf(g.name, sizeof g.name, "%s", s->groups[i].name);
                if (person_row(ui, &g, s->active_group == i)) app_open_group(s, i);
                cui_pop_id(ui);
            }
            char t[48]; snprintf(t, sizeof t, "AMIGOS — %d", s->friend_count);
            section_title(ui, t);
            if (!s->friend_count) cui_text(ui, "Nenhum amigo ainda. Use “Adicionar amigo”.", .size = 12, .color = B_MUTED);
            for (int i = 0; i < s->friend_count; i++) {
                cui_push_id_int(ui, i);
                if (person_row(ui, &s->friends[i], s->active_friend == i)) app_open_friend(s, i);
                cui_pop_id(ui);
            }
        }
        cui_box(ui, .width = CUI_FILL, .pad = 10) {
            cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad = 8, .gap = 10, .radius = 16, .bg = B_GLASS_CARD,
                    .border = 1, .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER) {
                cui_box(ui, .id = "avatar") { cui_avatar(ui, s->user_name, .size = 36); }
                if (briks_clicked(ui, "avatar")) s->profile_open = true;
                const char *mine = s->rp_enabled ? s->user_presence : "";   /* linha sob o nome: só a presença */
                bubble_note(cui_get_rect(ui, cui_make_id(ui, "avatar")), s->user_status);
                cui_column(ui, .gap = 0, .width = CUI_FILL) {
                    cui_text(ui, s->user_name, .nowrap = true);
                    cui_text(ui, mine[0] ? mine : s->user_email, .size = 11, .color = B_MUTED, .nowrap = true);
                }
                if (cui_icon_button(ui, CUI_ICON_SETTINGS, .tooltip = "Configurações")) s->settings_open = true;
            }
        }
    }
}

/* "2026-11-09T23:41:00.000Z" (UTC) -> "20:41" no fuso local */
static void hhmm_local(const char *iso, char *out, size_t cap) {
    SYSTEMTIME u = { 0 }, l;
    if (sscanf(iso, "%hd-%hd-%hdT%hd:%hd:%hd", &u.wYear, &u.wMonth, &u.wDay, &u.wHour, &u.wMinute, &u.wSecond) == 6 &&
        SystemTimeToTzSpecificLocalTime(NULL, &u, &l)) snprintf(out, cap, "%02d:%02d", l.wHour, l.wMinute);
    else snprintf(out, cap, "--:--");
}

static void bubble(cui_ctx *ui, const msg_t *m) {
    char hm[8]; hhmm_local(m->created, hm, sizeof hm);
    /* bubble_align: top = topo do balão no topo do avatar; middle = a pontinha (quina reta no topo) na altura do centro do avatar */
    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .justify = m->mine ? CUI_JUSTIFY_END : CUI_JUSTIFY_START, .gap = 4,
            .align = m->mine ? CUI_ALIGN_DEFAULT : CUI_ALIGN_START) {
        if (!m->mine) { cui_box(ui) { cui_avatar(ui, m->author, .size = 32); } }   /* avatar sem relevo */
        if (m->mine) {
            /* nosso: roxo padrão; só a quina superior direita mais reta */
            cui_box(ui, .max_width = 520, .pad_x = 14, .pad_y = 9, .gap = 3,
                    .radius_tl = g_th.bubble_big, .radius_tr = g_th.bubble_small, .radius_br = g_th.bubble_big, .radius_bl = g_th.bubble_big,
                    .bg = g_th.bubble_own, .border = 1, .border_color = CUI_RGBA(0xffffff26)) {
                cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER) {
                    cui_text(ui, "Você", .size = 12, .color = CUI_RGB(0xe6dcff));
                    cui_text(ui, hm, .size = 10, .color = CUI_RGBA(0xffffffb0));
                }
                cui_text(ui, m->content);
            }
        } else {
            /* recebido: vidro; quina superior do lado do avatar com raio 4 */
            cui_box(ui, .pad_top = g_th.bubble_align ? 16 : 0) {   /* middle: o topo (a pontinha) do balão sai da altura do centro do avatar (32/2) */
            cui_box(ui, .max_width = 520, .pad_x = 14, .pad_y = 9, .gap = 3,
                    .radius_tl = g_th.bubble_small, .radius_tr = g_th.bubble_big, .radius_br = g_th.bubble_big, .radius_bl = g_th.bubble_big, .bg = B_GLASS_CARD,
                    .border = 1, .border_color = B_GLASS_BORDER) {
                cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER) {
                    cui_text(ui, m->author, .size = 12, .color = B_C3);
                    cui_text(ui, hm, .size = 10, .color = CUI_RGB(0x8472a0));
                }
                cui_text(ui, m->content);
            }
            }
        }
    }
}

static void content(cui_ctx *ui, app_state_t *s) {
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = B_GLASS, .radius = (int)g_th.panel_radius, .clip = true,
            .border = 1, .border_color = B_GLASS_BORDER, .elevation = briks_elev(CUI_ELEVATION_RAISED)) {
        if (s->active_friend < 0 && s->active_group < 0) {
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER,
                    .justify = CUI_JUSTIFY_CENTER, .gap = 8) {
                cui_icon(ui, CUI_ICON_PHONE, 40, B_C3);
                cui_text(ui, "Escolha uma conversa", .font = CUI_FONT_DISPLAY, .size = 28);
                cui_text(ui, "Selecione um amigo ou grupo na barra lateral.", .color = B_MUTED);
            }
            return;
        }
        const bool group = s->active_group >= 0;
        const char *title = group ? s->groups[s->active_group].name : s->friends[s->active_friend].name;
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(62), .pad_x = 20, .gap = 10,
                .radius_tl = (int)g_th.panel_radius - 1, .radius_tr = (int)g_th.panel_radius - 1, .bg = CUI_RGBA(0xffffff0a), .align = CUI_ALIGN_CENTER) {
            cui_icon(ui, group ? CUI_ICON_USERS : CUI_ICON_MESSAGE_SQUARE, 18, B_MUTED);
            cui_text(ui, title, .font = CUI_FONT_MEDIUM);
            cui_text(ui, group ? "Grupo P2P · canal geral" : "Mensagem direta P2P", .size = 12, .color = B_MUTED);
            if (!group) briks_status_pill(ui, s->friends[s->active_friend].status);   /* só se o amigo tiver status */
        }
        cui_separator(ui);
        cui_scroll(ui, .id = "chat-scroll", .width = CUI_FILL, .height = CUI_FILL, .pad = g_th.chat_pad) {
            if (!s->msg_count) {
                cui_box(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .pad_top = 60, .gap = 8) {
                    cui_icon(ui, CUI_ICON_MESSAGE_SQUARE, 34, B_C3);
                    cui_textf(ui, "Conversa com %s", title);
                    cui_text(ui, "As mensagens ficam gravadas só neste computador até a entrega P2P (marco 3).", .size = 12, .color = B_MUTED);
                }
            }
            cui_column(ui, .gap = 10, .width = CUI_FILL) {
                for (int i = 0; i < s->msg_count; i++) { cui_push_id_int(ui, 5000 + i); bubble(ui, &s->msgs[i]); cui_pop_id(ui); }
            }
        }
        cui_box(ui, .width = CUI_FILL, .pad = 12, .pad_top = 0) {
            cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad = 7, .gap = 6, .radius = 18, .bg = B_GLASS_CARD,
                    .border = 1, .border_color = B_GLASS_BORDER_SOFT, .align = CUI_ALIGN_CENTER) {
                cui_push_disabled(ui, true);
                cui_icon_button(ui, CUI_ICON_PAPERCLIP, .tooltip = "Anexar (marco 3)");
                cui_pop_disabled(ui);
                int r = cui_input(ui, "draft", s->draft, sizeof s->draft, .placeholder = "Escreva uma mensagem", .width = CUI_FILL);
                bool send = cui_icon_button(ui, CUI_ICON_SEND, .variant = CUI_BUTTON_PRIMARY, .tooltip = "Enviar");
                if ((send || (r & CUI_SUBMITTED)) && s->draft[0]) app_send_message(s);
            }
        }
    }
}

static void panel_servers(cui_ctx *ui, app_state_t *s) {
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .pad_x = 8, .pad_y = 12, .gap = 10, .bg = B_GLASS, .radius = (int)g_th.panel_radius, .clip = true,
                .border = 1, .border_color = B_GLASS_BORDER, .elevation = briks_elev(CUI_ELEVATION_RAISED), .align = CUI_ALIGN_CENTER) {
            int click = -1;
            space_button(ui, "B", s->active_space == 0, false, &click, 0);
            if (s->community_count) cui_box(ui, .width = CUI_PX(32), .height = CUI_PX(1), .bg = CUI_RGBA(0xffffff14)) {}
            for (int i = 0; i < s->community_count; i++) {
                char ini[3]; initials(s->communities[i].name, ini);
                space_button(ui, ini, s->active_space == i + 1, false, &click, i + 1);
            }
            space_button(ui, "+", false, true, &click, 999);
            if (click >= 0 && click != 999) s->active_space = click;
        }
}


static void panel_shell(cui_ctx *ui, const char *title, const char *icon) {
    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(56), .pad_x = 18, .gap = 10, .align = CUI_ALIGN_CENTER) {
        cui_icon(ui, icon, 16, B_MUTED);
        cui_text(ui, title, .font = CUI_FONT_DISPLAY, .size = 15, .nowrap = true);
    }
    cui_separator(ui);
}
#define GLASS_PANEL .width = CUI_FILL, .height = CUI_FILL, .bg = B_GLASS, .radius = (int)g_th.panel_radius, .clip = true, \
        .border = 1, .border_color = B_GLASS_BORDER, .elevation = briks_elev(CUI_ELEVATION_RAISED)

static void panel_friends(cui_ctx *ui, app_state_t *s) {
    cui_box(ui, GLASS_PANEL) {
        panel_shell(ui, "Amigos", CUI_ICON_USERS);
        cui_scroll(ui, .id = "friends-scroll", .width = CUI_FILL, .height = CUI_FILL, .pad = 9) {
            if (!s->friend_count) cui_text(ui, "Nenhum amigo ainda.", .size = 12, .color = B_MUTED);
            for (int i = 0; i < s->friend_count; i++) {
                cui_push_id_int(ui, 7000 + i);
                if (person_row(ui, &s->friends[i], s->active_friend == i)) app_open_friend(s, i);
                cui_pop_id(ui);
            }
        }
    }
}

static void panel_members(cui_ctx *ui, app_state_t *s) {
    cui_box(ui, GLASS_PANEL) {
        panel_shell(ui, "Membros", CUI_ICON_USERS);
        cui_scroll(ui, .id = "members-scroll", .width = CUI_FILL, .height = CUI_FILL, .pad = 9) {
            person_t me; snprintf(me.name, sizeof me.name, "%s", s->user_name[0] ? s->user_name : "Você");
            person_row(ui, &me, false);
            for (int i = 0; i < s->friend_count; i++) { cui_push_id_int(ui, 7500 + i); person_row(ui, &s->friends[i], false); cui_pop_id(ui); }
        }
    }
}

static void panel_voice(cui_ctx *ui, app_state_t *s) {
    cui_box(ui, GLASS_PANEL) {
        panel_shell(ui, "Sala de voz", CUI_ICON_VOLUME_2);
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .gap = 10, .pad = 16) {
            cui_text(ui, "Sala geral", .font = CUI_FONT_DISPLAY, .size = 18);
            { char st[160]; cui_text(ui, rtc_status_text(s, st, sizeof st), .size = 12, .color = B_MUTED); }
            if (cui_button(ui, s->in_room ? "Voltar à sala" : "Entrar na sala", .variant = CUI_BUTTON_PRIMARY)) { s->screen = SCREEN_CALL; s->mic_on = true; if (!s->in_room) rtc_join_room(s, "voz-geral"); }
        }
    }
}

static void panel_history(cui_ctx *ui, app_state_t *s) {
    (void)s;
    cui_box(ui, GLASS_PANEL) {
        panel_shell(ui, "Histórico de chamadas", CUI_ICON_HISTORY);
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .gap = 8, .pad = 16) {
            cui_icon(ui, CUI_ICON_HISTORY, 34, B_C3);
            cui_text(ui, "Nenhuma chamada ainda", .font = CUI_FONT_DISPLAY, .size = 18);
        }
    }
}


static void panel_chat2(cui_ctx *ui, app_state_t *s) {
    if (!s->chat2_channel[0] && s->friend_count) app_open_chat2(s, s->chat2_friend < s->friend_count ? s->chat2_friend : 0);
    cui_box(ui, GLASS_PANEL) {
        const char *title = (s->chat2_friend >= 0 && s->chat2_friend < s->friend_count) ? s->friends[s->chat2_friend].name : "Escolha um amigo";
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(56), .pad_x = 18, .gap = 10, .align = CUI_ALIGN_CENTER,
                .radius_tl = (int)g_th.panel_radius - 1, .radius_tr = (int)g_th.panel_radius - 1, .bg = CUI_RGBA(0xffffff0a)) {
            cui_icon(ui, CUI_ICON_MESSAGE_SQUARE, 16, B_MUTED);
            cui_text(ui, title, .font = CUI_FONT_MEDIUM, .nowrap = true);
            cui_text(ui, "Segunda conversa", .size = 12, .color = B_MUTED, .nowrap = true);
        }
        cui_separator(ui);
        cui_scroll(ui, .id = "chat2-scroll", .width = CUI_FILL, .height = CUI_FILL, .pad = 16) {
            if (!s->msg2_count) cui_text(ui, "Sem mensagens ainda.", .size = 12, .color = B_MUTED);
            cui_column(ui, .gap = 10, .width = CUI_FILL) {
                for (int i = 0; i < s->msg2_count; i++) { cui_push_id_int(ui, 8000 + i); bubble(ui, &s->msgs2[i]); cui_pop_id(ui); }
            }
        }
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad = 12, .gap = 8, .align = CUI_ALIGN_CENTER) {
            if (s->friend_count > 1 && cui_button(ui, "Trocar amigo", .variant = CUI_BUTTON_SECONDARY)) {
                int n = s->chat2_friend + 1; if (n >= s->friend_count) n = 0;
                app_open_chat2(s, n);
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
        cui_text(ui, "O grupo fica gravado neste computador; os convites por P2P chegam com o briks-core.", .color = B_MUTED);
        int r = cui_input(ui, "new-group", s->new_group, sizeof s->new_group, .placeholder = "Nome do grupo", .autofocus = true);
        cui_dialog_footer(ui) {
            if ((cui_button(ui, "Criar", .variant = CUI_BUTTON_PRIMARY, .loading = s->busy) || (r & CUI_SUBMITTED))) app_create_group(s);
        }
    }
    cui_dialog(ui, "Perfil", &s->profile_open) {
        cui_row(ui, .gap = 14, .align = CUI_ALIGN_CENTER) {
            cui_avatar(ui, s->user_name, .size = 64);
            cui_column(ui, .gap = 2) {
                cui_text(ui, s->user_name, .font = CUI_FONT_DISPLAY, .size = 20);
                cui_text(ui, s->user_email, .color = B_MUTED);
            }
        }
        cui_text(ui, "Status (aparece num balão perto da sua foto)", .size = 12, .color = B_MUTED);
        cui_input(ui, "status-edit", s->user_status, sizeof s->user_status, .placeholder = "Vazio = sem balão");
        if (s->acct_user_id[0]) {
            cui_row(ui, .gap = 10, .align = CUI_ALIGN_CENTER) {
                cui_textf(ui, "@%s", s->acct_nick[0] ? s->acct_nick : "…");
                if (cui_button(ui, "Trocar @nick", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) { s->profile_open = false; s->nick_open = true; s->nick_msg[0] = 0; snprintf(s->nick_edit, sizeof s->nick_edit, "%s", s->acct_nick); }
            }
            cui_text(ui, "Seus amigos acham você pelo @nick. Mensagens e grupos ficam guardados neste computador.", .size = 12, .color = B_MUTED);
        } else cui_text(ui, "Conta local: os dados ficam só neste computador.", .size = 12, .color = B_MUTED);
    }
    cui_dialog(ui, "Trocar @nick", &s->nick_open) {
        cui_text(ui, "De 3 a 20 caracteres: letras minúsculas, números e _. Depois de escolher, só dá para trocar de novo em 30 dias.", .color = B_MUTED);
        int nr = cui_input(ui, "nick-edit", s->nick_edit, sizeof s->nick_edit, .placeholder = "seu_nick", .autofocus = true);
        if (s->nick_msg[0]) cui_text(ui, s->nick_msg, .color = B_MUTED, .size = 13);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Salvar", .variant = CUI_BUTTON_PRIMARY) || (nr & CUI_SUBMITTED)) account_set_nick(s);
        }
    }
    bubbles_draw(ui);
    if (g_perf_hint) {   /* hardware fraco detectado: aviso discreto no canto, sem bloquear nada */
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_END, .justify = CUI_JUSTIFY_START, .pad = 14) {   /* canto superior direito: nunca sobre a caixa de escrever */
            cui_box(ui, .dir = CUI_ROW, .gap = 10, .pad_x = 14, .pad_y = 8, .radius = 14, .bg = CUI_RGBA(0x180726f2), .border = 1,
                    .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, briks_perf_hint_text(), .size = 12);   /* frase sorteada a cada exibição (perf.c) */
                char lab[48]; snprintf(lab, sizeof lab, "Ativar %s", briks_perf_label(briks_perf_hint_level()));
                if (cui_button(ui, lab, .variant = CUI_BUTTON_PRIMARY)) briks_perf_set(ui, briks_perf_hint_level(), true);
                if (cui_button(ui, "Agora não", .variant = CUI_BUTTON_SECONDARY)) briks_perf_hint_dismiss();
            }
        }
    }
    static float restored_at = -10;
    cui_vec2 vp = cui_viewport_size(ui);
    /* largura e altura acompanham o conteúdo, limitadas à janela; se não couber, rola na vertical */
    cui_dialog(ui, "Configurações", &s->settings_open, .width = fminf(480.0f, fmaxf(vp.x - 48.0f, 260.0f))) {
        cui_box(ui, .width = CUI_FILL, .max_height = fmaxf(vp.y - 200.0f, 120.0f), .scroll_y = true, .gap = 12) {
            if (s->acct_user_id[0]) {
                cui_text(ui, "Conta", .font = CUI_FONT_MEDIUM);
                cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad = 10, .gap = 12, .radius = 16, .bg = B_GLASS_CARD, .border = 1, .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER) {
                    cui_avatar_ex(ui, s->user_name, &(cui_avatar_opts){ .size = 40, .image = s->user_photo });
                    cui_column(ui, .gap = 1, .width = CUI_FILL) {
                        cui_text(ui, s->user_name, .font = CUI_FONT_MEDIUM, .nowrap = true);
                        cui_textf(ui, "@%s · %s", s->user_nick[0] ? s->user_nick : "…", s->user_email);
                    }
                    if (cui_button(ui, "Sair", .variant = CUI_BUTTON_DANGER, .size = CUI_SIZE_SM)) { s->settings_open = false; rtc_leave_room(s); app_logout(s); }
                }
                cui_text(ui, "Sair apaga a sessão guardada neste computador. Suas mensagens locais continuam aqui para a próxima vez.", .size = 12, .color = B_MUTED);
                if (s->import_state == 1 || s->import_state == 2 || s->import_state == 3) {
                    cui_text(ui, "Briks antigo", .font = CUI_FONT_MEDIUM);
                    if (s->import_msg[0]) cui_text(ui, s->import_msg, .size = 12, .color = B_MUTED);
                    if (s->import_state != 3 && cui_button(ui, "Importar amigos do Briks antigo", .variant = CUI_BUTTON_SECONDARY, .loading = s->import_state == 2)) account_import(s, true);
                    if (s->import_state != 3) cui_text(ui, "Traz contatos, grupos e mensagens. A identidade antiga não é importada e nada é alterado no Briks antigo.", .size = 12, .color = B_MUTED);
                }
            } else cui_textf(ui, "Conectado como %s", s->user_name);
            cui_text(ui, "Voz e vídeo, rede, atalhos, atualizações e sobre chegam nos próximos marcos.", .color = B_MUTED);
            cui_text(ui, "Desempenho", .font = CUI_FONT_MEDIUM);
            cui_row(ui, .gap = 8) {
                for (int l = 0; l < 3; l++) {
                    cui_push_id_int(ui, 700 + l);
                    if (cui_button(ui, briks_perf_label(l), .variant = g_perf == l ? CUI_BUTTON_PRIMARY : CUI_BUTTON_SECONDARY)) briks_perf_set(ui, l, true);
                    cui_pop_id(ui);
                }
            }
            cui_text(ui, briks_perf_help(g_perf), .size = 12, .color = B_MUTED);
            cui_text(ui, "Presença", .font = CUI_FONT_MEDIUM);
            cui_switch(ui, "Mostrar o programa que estou usando", &s->rp_enabled);
            if (s->rp_enabled) {
                cui_row(ui, .gap = 8) {
                    for (int l = 0; l < 2; l++) {
                        cui_push_id_int(ui, 760 + l);
                        if (cui_button(ui, l ? "Só na chamada" : "Todos os amigos", .variant = s->rp_audience == l ? CUI_BUTTON_PRIMARY : CUI_BUTTON_SECONDARY)) s->rp_audience = l;
                        cui_pop_id(ui);
                    }
                }
            }
            cui_text(ui, "Painéis", .font = CUI_FONT_MEDIUM);
            cui_text(ui, "Volta a disposição original: Servidores | Menu | Conversa.", .size = 12, .color = B_MUTED);
            if (cui_button(ui, "Restaurar layout padrão", .variant = CUI_BUTTON_SECONDARY)) {
                dock_restore_default();
                restored_at = (float)cui_time(ui);
            }
            if ((float)cui_time(ui) - restored_at < 3.0f)
                cui_text(ui, "Layout restaurado.", .size = 12, .color = B_ONLINE);
            if (!s->acct_user_id[0] && cui_button(ui, "Desconectar", .variant = CUI_BUTTON_DANGER)) { s->settings_open = false; app_logout(s); }
        }
    }
    /* o dono da sala expulsou você */
    cui_dialog(ui, "Chamada", &s->kicked_open, .width = fminf(380.0f, fmaxf(vp.x - 48.0f, 260.0f))) {
        cui_text(ui, "Você foi removido da chamada.", .font = CUI_FONT_MEDIUM);
        cui_text(ui, "Você não pode voltar a esta sala nesta sessão até o dono permitir.", .size = 13, .color = B_MUTED);
        if (cui_button(ui, "Entendi", .variant = CUI_BUTTON_PRIMARY)) s->kicked_open = false;
    }
}
