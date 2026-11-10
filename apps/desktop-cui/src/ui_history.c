#include "briks_ui.h"

/* Histórico de chamadas. O backend local ainda não registra chamadas (elas nascem no marco 3):
   a tela existe com o visual final e mostra o estado vazio. */
void ui_history(cui_ctx *ui, app_state_t *s) {
    briks_ambient(ui, 0.85f);
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 10) {
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 0, .bg = B_GLASS, .radius = (int)g_th.panel_radius, .clip = true,
            .border = 1, .border_color = B_GLASS_BORDER, .elevation = briks_elev(CUI_ELEVATION_RAISED)) {
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(62), .pad_x = 20, .gap = 10,
                .radius_tl = (int)g_th.panel_radius - 1, .radius_tr = (int)g_th.panel_radius - 1, .bg = CUI_RGBA(0xffffff0a), .align = CUI_ALIGN_CENTER) {
            if (cui_icon_button(ui, CUI_ICON_ARROW_LEFT, .tooltip = "Voltar")) s->screen = SCREEN_SOCIAL;
            cui_text(ui, "Histórico de chamadas", .font = CUI_FONT_MEDIUM);
        }
        cui_separator(ui);
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .gap = 10) {
            cui_box(ui, .width = CUI_PX(420), .pad = 34, .gap = 10, .radius = 24, .bg = B_GLASS_STRONG,
                    .border = 1, .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER) {
                cui_icon(ui, CUI_ICON_HISTORY, 38, B_C3);
                cui_text(ui, "Nenhuma chamada ainda", .font = CUI_FONT_DISPLAY, .size = 22);
                cui_text(ui, "As chamadas que você fizer ou receber aparecem aqui, com duração e participantes.", .size = 13, .color = B_MUTED);
            }
        }
    }
    }
}
