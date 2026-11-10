#include "briks_ui.h"
#include <stdio.h>
#include <string.h>

static bool clicked_last(cui_ctx *ui) {
    return cui_last_hovered(ui) && cui_mouse_clicked(ui, CUI_MOUSE_LEFT);
}

static void initials(const char *name, char *out) {
    out[0] = name[0] ? name[0] : '?';
    const char *sp = strchr(name, ' ');
    out[1] = sp && sp[1] ? sp[1] : 0;
    out[2] = 0;
}

static void space_button(cui_ctx *ui, const char *label, bool active, bool dashed, int *clicked_id, int id) {
    cui_push_id_int(ui, id);
    cui_box(ui, .width = CUI_PX(50), .height = CUI_PX(50), .radius = active ? 13 : 17,
            .bg = active ? B_C3 : dashed ? CUI_RGBA(0x8b5cf60b) : CUI_RGB(0x171c24),
            .border = dashed ? 1 : 0, .border_color = CUI_RGBA(0x8b5cf666),
            .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
        cui_text(ui, label, .size = 13, .color = active ? CUI_RGB(0xffffff) : dashed ? B_C3 : CUI_RGB(0xc9d1dc));
    }
    if (clicked_last(ui)) *clicked_id = id;
    cui_pop_id(ui);
}

static bool nav_item(cui_ctx *ui, const char *icon, const char *label, bool active) {
    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 10, .pad_y = 9, .gap = 10, .radius = 12,
            .bg = active ? CUI_RGBA(0x6d5cf638) : CUI_RGBA(0x00000000),
            .border = active ? 1 : 0, .border_color = CUI_RGBA(0xffffff1a), .align = CUI_ALIGN_CENTER) {
        cui_icon(ui, icon, 16, active ? B_TEXT : CUI_RGB(0x8d97a6));
        cui_text(ui, label, .color = active ? B_TEXT : CUI_RGB(0xa592b8), .nowrap = true);
    }
    return clicked_last(ui);
}

static void section_title(cui_ctx *ui, const char *t) {
    cui_box(ui, .pad_left = 9, .pad_top = 22, .pad_bottom = 7) {
        cui_text(ui, t, .size = 10, .color = CUI_RGB(0x8472a0));
    }
}

static bool person_row(cui_ctx *ui, const person_t *f, bool active) {
    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 8, .pad_y = 6, .gap = 10, .radius = 12,
            .bg = active ? CUI_RGBA(0x6d5cf638) : CUI_RGBA(0x00000000), .align = CUI_ALIGN_CENTER) {
        cui_avatar(ui, f->name, .size = 32);
        cui_text(ui, f->name, .nowrap = true, .width = CUI_FILL);
    }
    return clicked_last(ui);
}

static void navigation(cui_ctx *ui, app_state_t *s) {
    cui_box(ui, .width = CUI_PX(280), .height = CUI_FILL, .bg = B_GLASS) {
        cui_box(ui, .dir = CUI_ROW, .height = CUI_PX(62), .width = CUI_FILL, .pad_x = 20, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, s->active_friend >= 0 ? s->friends[s->active_friend].name : "Briks",
                     .font = CUI_FONT_DISPLAY, .size = 16, .nowrap = true);
        }
        cui_separator(ui);
        cui_scroll(ui, .id = "nav-scroll", .width = CUI_FILL, .height = CUI_FILL, .pad = 9) {
            if (nav_item(ui, CUI_ICON_USERS, "Amigos", s->active_friend < 0)) s->active_friend = -1;
            if (nav_item(ui, CUI_ICON_USER_PLUS, "Adicionar amigo", false)) { s->add_friend_open = true; s->notice[0] = 0; }
            nav_item(ui, CUI_ICON_HISTORY, "Histórico de chamadas", false);
            section_title(ui, "SALAS DE VOZ");
            /* Sala de teste: a chamada real chega com o briks-core (marco 3). */
            if (nav_item(ui, CUI_ICON_VOLUME_2, "Sala geral (prévia)", false)) { s->screen = SCREEN_CALL; s->mic_on = true; }
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
            char t[48]; snprintf(t, sizeof t, "AMIGOS — %d", s->friend_count);
            section_title(ui, t);
            if (!s->friend_count) cui_text(ui, "Nenhum amigo ainda. Use “Adicionar amigo”.", .size = 12, .color = B_MUTED);
            for (int i = 0; i < s->friend_count; i++) {
                cui_push_id_int(ui, i);
                if (person_row(ui, &s->friends[i], s->active_friend == i)) s->active_friend = i;
                cui_pop_id(ui);
            }
        }
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad = 8, .gap = 10, .radius = 16,
                .bg = B_GLASS_STRONG, .border = 1, .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER) {
            cui_avatar(ui, s->user_name, .size = 36);
            cui_column(ui, .gap = 0, .width = CUI_FILL) {
                cui_text(ui, s->user_name, .nowrap = true);
                cui_text(ui, s->user_email, .size = 11, .color = B_MUTED, .nowrap = true);
            }
            if (cui_icon_button(ui, CUI_ICON_SETTINGS, .tooltip = "Configurações")) s->settings_open = true;
        }
    }
}

static void content(cui_ctx *ui, app_state_t *s) {
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_RGBA(0x12041c4d)) {
        if (s->active_friend < 0) {
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER,
                    .justify = CUI_JUSTIFY_CENTER, .gap = 8) {
                cui_icon(ui, CUI_ICON_PHONE, 40, B_C3);
                cui_text(ui, "Escolha uma conversa", .font = CUI_FONT_DISPLAY, .size = 28);
                cui_text(ui, "Selecione um amigo na barra lateral.", .color = B_MUTED);
            }
            return;
        }
        const person_t *f = &s->friends[s->active_friend];
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(62), .pad_x = 20, .gap = 10,
                .bg = B_GLASS_SOFT, .align = CUI_ALIGN_CENTER) {
            cui_icon(ui, CUI_ICON_MESSAGE_SQUARE, 18, B_MUTED);
            cui_text(ui, f->name, .font = CUI_FONT_MEDIUM);
            cui_text(ui, "Mensagem direta P2P", .size = 12, .color = B_MUTED);
        }
        cui_separator(ui);
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .gap = 8) {
            cui_icon(ui, CUI_ICON_MESSAGE_SQUARE, 34, B_C3);
            cui_textf(ui, "Conversa com %s", f->name);
            cui_text(ui, "As mensagens P2P entram no próximo marco (precisam do canal WebRTC).", .size = 12, .color = B_MUTED);
        }
        cui_box(ui, .width = CUI_FILL, .pad = 20, .pad_top = 0) {
            cui_push_disabled(ui, true);
            cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad = 6, .gap = 6, .radius = 16, .bg = B_GLASS,
                    .border = 1, .border_color = B_GLASS_BORDER_SOFT, .align = CUI_ALIGN_CENTER) {
                cui_icon_button(ui, CUI_ICON_PAPERCLIP, .tooltip = "Anexar");
                cui_input(ui, "draft", s->draft, sizeof s->draft, .placeholder = "Escreva uma mensagem", .width = CUI_FILL);
                cui_icon_button(ui, CUI_ICON_SEND, .variant = CUI_BUTTON_PRIMARY, .tooltip = "Enviar");
            }
            cui_pop_disabled(ui);
        }
    }
}

void ui_social(cui_ctx *ui, app_state_t *s) {
    briks_ambient(ui, 0.85f);
    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_FILL, .gap = 0) {
        cui_box(ui, .width = CUI_PX(76), .height = CUI_FILL, .pad = 14, .gap = 10, .bg = B_GLASS_SOFT,
                .align = CUI_ALIGN_CENTER) {
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
        navigation(ui, s);
        content(ui, s);
    }
    cui_dialog(ui, "Adicionar amigo", &s->add_friend_open) {
        cui_text(ui, "Informe o e-mail da conta que você quer adicionar.", .color = B_MUTED);
        cui_input(ui, "add-email", s->add_email, sizeof s->add_email, .placeholder = "amigo@email.com");
        if (s->notice[0]) cui_text(ui, s->notice, .color = B_MUTED, .size = 13);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Enviar pedido", .variant = CUI_BUTTON_PRIMARY, .loading = s->busy)) app_add_friend(s);
        }
    }
    cui_dialog(ui, "Configurações", &s->settings_open) {
        cui_textf(ui, "Conectado como %s", s->user_name);
        cui_text(ui, "Voz e vídeo, rede, atalhos, atualizações e sobre chegam nos próximos marcos.", .color = B_MUTED);
        if (cui_button(ui, "Desconectar", .variant = CUI_BUTTON_DANGER)) { s->settings_open = false; app_logout(s); }
    }
}
