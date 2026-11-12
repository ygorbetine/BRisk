#include "volume_ctl.h"
#include <math.h>
#include <string.h>

/* memória do volume de antes do mudo (para o modo sem flag de mudo): por controle, só nesta execução */
#define MEM_N 32
static struct { cui_id id; float saved; } g_mem[MEM_N]; static int g_mem_n;
static float *mem_slot(cui_id id) {
    for (int i = 0; i < g_mem_n; i++) if (g_mem[i].id == id) return &g_mem[i].saved;
    int k = g_mem_n < MEM_N ? g_mem_n++ : 0;
    g_mem[k].id = id; g_mem[k].saved = 0; return &g_mem[k].saved;
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

float briks_volume_wheel_apply(float cur, float lo, float hi, float pct_unit, float clicks, int mods) {
    const float pct = (mods & CUI_MOD_CTRL) ? 1.0f : (mods & CUI_MOD_SHIFT) ? 10.0f : 5.0f;
    float nv = cur + clicks * pct * pct_unit;
    nv = roundf(nv / pct_unit * 1000.0f) / 1000.0f * pct_unit;   /* evita resíduo de ponto flutuante */
    return clampf(nv, lo, hi);
}

int briks_volume(cui_ctx *ui, const char *id, float *vol, float lo, float hi, float pct_unit, bool *muted, bool show_pct) {
    int ch = 0;
    cui_push_id(ui, id);
    const cui_id row_id = cui_make_id(ui, "row");
    float *mem = mem_slot(row_id);
    const float def = fminf(hi, 100.0f * pct_unit);
    const bool is_muted = (muted && *muted) || *vol <= lo + 1e-4f;
    float cur = (muted && *muted) ? lo : *vol;

    cui_box(ui, .id = "row", .dir = CUI_ROW, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
        cui_box(ui, .id = "ic", .width = CUI_PX(24), .height = CUI_PX(24), .radius = 12, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
            cui_icon(ui, is_muted ? CUI_ICON_VOLUME_X : CUI_ICON_VOLUME_2, 15, is_muted ? B_DANGER : B_MUTED);
        }
        cui_tooltip(ui, is_muted ? "Reativar o som" : "Silenciar");
        float tmp = cur;
        cui_slider(ui, "##v", &tmp, lo, hi, .step = pct_unit, .width = CUI_FILL);
        if (fabsf(tmp - cur) > 1e-4f) {   /* arrastou ou usou as setas: sair do mudo */
            if (muted && *muted) { *muted = false; ch |= VOLCTL_MUTE; }
            if (tmp > lo) *mem = tmp;
            *vol = tmp; ch |= VOLCTL_VOL; cur = tmp;
        }
        if (show_pct) {
            char p[16]; snprintf(p, sizeof p, "%d%%", (int)floorf((is_muted ? 0.0f : cur) / pct_unit + 0.5f));
            cui_text(ui, p, .size = 11, .color = B_MUTED, .nowrap = true);
        }
    }
    /* clique no ícone */
    if (briks_clicked_r(ui, "ic", 12.0f)) {
        const float restore = *mem > lo ? *mem : def;
        if (muted && *muted) { *muted = false; ch |= VOLCTL_MUTE; if (*vol <= lo) { *vol = restore; ch |= VOLCTL_VOL; } }
        else if (muted && *vol > lo) { *muted = true; ch |= VOLCTL_MUTE; *mem = *vol; }
        else if (*vol > lo) { *mem = *vol; *vol = lo; ch |= VOLCTL_VOL; }
        else { *vol = restore; ch |= VOLCTL_VOL; }
    }
    /* roda do mouse sobre a linha inteira: ajusta o volume e consome a roda (o painel por trás não rola) */
    {
        const cui_rect r = cui_get_rect(ui, row_id); const cui_vec2 mp = cui_mouse_pos(ui);
        if (r.w > 0) cui_wheel_claim(ui, r);   /* o painel por trás não rola quando a roda está sobre este controle (valendo a partir do próximo quadro) */
        if (r.w > 0 && mp.x >= r.x && mp.x < r.x + r.w && mp.y >= r.y && mp.y < r.y + r.h && !briks_blocked() && !briks_covered(ui)) {
            const float dy = cui_wheel_take(ui);
            if (dy != 0.0f) {
                const float base = *vol;   /* mudo + roda: desmuta e sobe a partir do volume lembrado */
                const float nv = briks_volume_wheel_apply(base, lo, hi, pct_unit, dy, cui_key_mods(ui));
                if (muted && *muted && nv > lo) { *muted = false; ch |= VOLCTL_MUTE; }
                if (fabsf(nv - *vol) > 1e-6f) { *vol = nv; ch |= VOLCTL_VOL; if (nv > lo) *mem = nv; }
            }
        }
    }
    cui_pop_id(ui);
    return ch;
}
