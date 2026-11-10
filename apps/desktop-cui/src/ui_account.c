/* Editor da conta para o diálogo de Perfil: foto, nome e @nick. Bloco independente (só depende de app_state_t e de
 * account.c) para poder ser encaixado em qualquer layout do Perfil. */
#include "briks_ui.h"
#include <stdio.h>
#include <string.h>

void ui_account_editor(cui_ctx *ui, app_state_t *s) {
    /* ao abrir, o campo começa com o nome atual */
    static double last = -10.0;   /* o bloco só roda com o diálogo aberto: uma pausa nos quadros = acabou de abrir */
    const double now = (double)cui_time(ui);
    if (now - last > 0.4) { snprintf(s->name_edit, sizeof s->name_edit, "%s", s->user_name); s->profile_msg[0] = 0; }
    last = now;

    cui_box(ui, .width = CUI_FILL, .pad = 12, .gap = 12, .radius = 16, .bg = B_GLASS_CARD, .border = 1, .border_color = B_GLASS_BORDER) {
        /* foto */
        cui_text(ui, "Foto", .size = 12, .color = B_MUTED);
        cui_row(ui, .gap = 12, .align = CUI_ALIGN_CENTER) {
            cui_avatar_ex(ui, s->user_name, &(cui_avatar_opts){ .size = 56, .image = s->user_photo });
            cui_column(ui, .gap = 6) {
                cui_row(ui, .gap = 8) {
                    if (cui_button(ui, "Trocar foto", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) account_pick_photo(s);
                    if (cui_button(ui, "Usar a foto da conta", .variant = CUI_BUTTON_LINK, .size = CUI_SIZE_SM)) account_reset_photo(s);
                }
                cui_text(ui, "PNG ou JPEG. A imagem é recortada em quadrado.", .size = 11, .color = B_MUTED);
            }
        }
        /* nome */
        cui_text(ui, "Nome", .size = 12, .color = B_MUTED);
        cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_box(ui, .width = CUI_FILL) {
                int r = cui_input(ui, "name-edit", s->name_edit, sizeof s->name_edit, .placeholder = "Como seus amigos veem você");
                if ((r & CUI_SUBMITTED) && strcmp(s->name_edit, s->user_name)) account_set_name(s);
            }
            if (cui_button(ui, "Salvar", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM, .disabled = !s->name_edit[0] || !strcmp(s->name_edit, s->user_name))) account_set_name(s);
        }
        /* @nick */
        cui_text(ui, "@nick", .size = 12, .color = B_MUTED);
        cui_row(ui, .gap = 10, .align = CUI_ALIGN_CENTER) {
            cui_box(ui, .width = CUI_FILL) { cui_textf(ui, "@%s", s->user_nick[0] ? s->user_nick : "…"); }
            if (cui_button(ui, "Trocar @nick", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) {
                s->profile_open = false; s->nick_open = true; s->nick_msg[0] = 0;
                snprintf(s->nick_edit, sizeof s->nick_edit, "%s", s->acct_nick);
            }
        }
        if (s->profile_msg[0]) cui_text(ui, s->profile_msg, .size = 12, .color = CUI_RGB(0xd9cdea));
    }
    cui_text(ui, "Seus amigos acham você pelo @nick e veem este nome e esta foto. Mensagens e grupos ficam guardados neste computador.", .size = 12, .color = B_MUTED);
}
