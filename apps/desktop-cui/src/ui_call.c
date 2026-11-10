#include "briks_ui.h"

typedef struct { const char *name; bool me, online, speaking, muted; } tile_t;


static void tile(cui_ctx *ui, const tile_t *t, bool mic_on, float h) {
    bool speaking = t->me ? mic_on : t->speaking;
    cui_box(ui, .width = CUI_FILL, .height = CUI_PX(h), .radius = 20, .bg = CUI_RGBA(0x1a0a268c), .clip = true,
            .border = 1, .border_color = speaking ? CUI_RGBA(0x8b5cf688) : t->me ? CUI_RGBA(0x8b5cf666) : B_GLASS_BORDER_SOFT,
            .dir = CUI_STACK, .elevation = speaking ? CUI_ELEVATION_RAISED : CUI_ELEVATION_RESTING) {
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
            cui_avatar(ui, t->name, .size = 88);
        }
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_END, .pad = 14) {
            cui_box(ui, .dir = CUI_ROW, .gap = 8, .pad_x = 11, .pad_y = 8, .radius = 9, .bg = CUI_RGBA(0x14052088),
                    .border = 1, .border_color = B_GLASS_BORDER_SOFT, .align = CUI_ALIGN_CENTER) {
                cui_icon(ui, (t->muted || (t->me && !mic_on)) ? CUI_ICON_VOLUME_X : CUI_ICON_MIC, 14,
                         (t->muted || (t->me && !mic_on)) ? B_DANGER : B_C3);
                cui_text(ui, t->name, .size = 13);
                if (t->me) cui_text(ui, "VOCÊ", .size = 9, .color = B_C3);
            }
        }
    }
}

static bool pill(cui_ctx *ui, const char *icon, const char *label, bool active, bool danger) {
    cui_button_variant v = danger ? CUI_BUTTON_DANGER : active ? CUI_BUTTON_PRIMARY : CUI_BUTTON_SECONDARY;
    return cui_button(ui, label, .icon = icon, .variant = v, .size = CUI_SIZE_LG);
}

void ui_call(cui_ctx *ui, app_state_t *s) {
    const tile_t me = { s->user_name, true, true, true, false };
    briks_ambient(ui, 0.85f);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 0) {
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(76), .pad_x = 28, .bg = B_GLASS_SOFT,
                .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
            briks_brand(ui, 22);
            cui_column(ui, .gap = 0, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, "Sala geral", .font = CUI_FONT_DISPLAY, .size = 16);
                cui_text(ui, "Só você (prévia sem rede)", .size = 12, .color = B_MUTED);
            }
            cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER) {
                cui_box(ui, .width = CUI_PX(8), .height = CUI_PX(8), .radius = 4, .bg = B_C3) {}
                cui_text(ui, "Prévia local", .size = 14, .color = B_MUTED);
            }
        }
        cui_separator(ui);
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 18, .scroll_y = true, .id = "stage") {
            cui_box(ui, .dir = CUI_GRID, .min_column_width = 280, .gap = 14, .width = CUI_FILL) {
                tile(ui, &me, s->mic_on, 240);
            }
        }
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(96), .gap = 12, .bg = B_GLASS_STRONG,
                .justify = CUI_JUSTIFY_CENTER, .align = CUI_ALIGN_CENTER) {
            if (pill(ui, CUI_ICON_MIC, "Microfone", s->mic_on, false)) s->mic_on = !s->mic_on;
            if (pill(ui, CUI_ICON_VIDEO, "Câmera", s->cam_on, false)) s->cam_on = !s->cam_on;
            if (pill(ui, CUI_ICON_MONITOR, "Transmitir", s->sharing, false)) s->sharing = !s->sharing;
            if (pill(ui, CUI_ICON_PHONE, "Sair", false, true)) s->screen = SCREEN_SOCIAL;
        }
    }
}
