#include "briks_ui.h"
#include <stdio.h>
#include <string.h>
#include "backend.h"

void ui_auth(cui_ctx *ui, app_state_t *s) {
    const bool reg = s->auth_mode == 1, reset = s->auth_mode == 2;
    briks_ambient(ui, 1.0f);   /* primeiro filho do STACK: fundo */
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
        cui_box(ui, .width = CUI_FILL, .max_width = 430, .pad = 38, .gap = 12, .radius = 24,
                .bg = B_GLASS_STRONG, .border = 1, .border_color = CUI_RGBA(0x8b5cf633), .elevation = briks_elev(CUI_ELEVATION_FLOATING)) {
            briks_brand(ui, 26);
            cui_gap(ui, 14);
            cui_text(ui, reset ? "Redefinir senha" : reg ? "Crie seu espaço" : "Bom ter você de volta",
                     .font = CUI_FONT_DISPLAY, .size = 32, .line_height = 1.1f);
            cui_text(ui, reset ? "As contas ficam só neste computador. Informe o e-mail ou @nick e escolha uma senha nova."
                               : "Conversas que parecem estar na mesma sala.", .color = B_MUTED);
            cui_gap(ui, 8);
            if (reg) cui_input(ui, "name", s->name, sizeof s->name, .placeholder = "Como devemos chamar você?");
            cui_input(ui, "email", s->email, sizeof s->email, .placeholder = reg ? "seu@email.com" : "E-mail ou @nick");
            cui_input(ui, "password", s->password, sizeof s->password, .password = true,
                      .placeholder = reset ? "Nova senha (mínimo 8 caracteres)" : "Senha");
            if (reset) cui_input(ui, "confirm", s->confirm, sizeof s->confirm, .password = true, .placeholder = "Repita a nova senha");
            if (s->error[0]) cui_text(ui, s->error, .color = CUI_RGB(0xff8f8f), .size = 13);
            if (bk_status() == BK_FAILED) cui_text(ui, bk_error(), .color = CUI_RGB(0xff8f8f), .size = 13);
            if (cui_button(ui, reset ? "Redefinir e entrar" : "Continuar", .variant = CUI_BUTTON_PRIMARY,
                           .size = CUI_SIZE_LG, .full_width = true, .loading = s->busy || bk_status() == BK_STARTING,
                           .disabled = bk_status() == BK_FAILED)) {
                app_submit_auth(s);
            }
            if (!reg && !reset && cui_button(ui, "Esqueci minha senha", .variant = CUI_BUTTON_LINK, .full_width = true)) {
                s->auth_mode = 2; s->error[0] = 0;
            }
            if (cui_button(ui, reset ? "Voltar ao login" : reg ? "Já tenho uma conta" : "Criar uma conta",
                           .variant = CUI_BUTTON_LINK, .full_width = true)) {
                s->auth_mode = (reset || reg) ? 0 : 1; s->error[0] = 0;
            }
        }
    }
}
