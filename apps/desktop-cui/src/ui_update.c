/* Aviso de atualização (vidro, canto inferior direito): aparece quando o instalador novo já foi baixado e verificado. */
#include "briks_ui.h"
#include "update.h"
#include <stdio.h>

void update_banner(cui_ctx *ui, app_state_t *s) {
    if (update_phase() != UPD_READY || update_dismissed()) return;
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_END, .justify = CUI_JUSTIFY_END, .pad = 14) {
        cui_box(ui, .dir = CUI_ROW, .gap = 10, .pad_x = 14, .pad_y = 8, .radius = 14, .bg = CUI_RGBA(0x180726f2), .border = 1,
                .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER) {
            char t[96]; snprintf(t, sizeof t, "Briks %s pronto para instalar.", update_version());
            cui_text(ui, t, .size = 12);
            if (cui_button(ui, "Reiniciar e instalar", .variant = CUI_BUTTON_PRIMARY)) { if (update_install_now()) cui_app_quit(s->app); }
            if (cui_button(ui, "Depois", .variant = CUI_BUTTON_SECONDARY)) update_dismiss();
        }
    }
}
