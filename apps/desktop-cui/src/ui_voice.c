/* Painel "Sala de voz" completo: participantes com indicador de fala, mudo/ensurdecido, status, silenciar e volume por pessoa,
 * expulsar (só o dono) e os controles de voz de quem está na sala. Tudo em vidro, cantos arredondados. */
#include "briks_ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static void ini2(const char *name, char *out) {
    out[0] = name[0] ? name[0] : '?';
    out[1] = 0;
    if (out[0] >= 'a' && out[0] <= 'z') out[0] -= 32;
}

/* medidor de fala: disco com anel verde que cresce com o nível */
static void voice_avatar(cui_ctx *ui, const char *name, float level, bool muted) {
    char in[2]; ini2(name, in);
    cui_color ring = level > 0 ? CUI_RGBA(0x57d28bdd) : B_GLASS_BORDER;
    cui_box(ui, .width = CUI_PX(36), .height = CUI_PX(36), .radius = 18, .bg = muted ? B_A(B_C1, 0x40) : B_A(B_C3, 0xdd),
            .border = level > 0 ? 2 : 1, .border_color = ring, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
        cui_text(ui, in, .font = CUI_FONT_MEDIUM, .size = 14, .color = CUI_RGB(0xffffff));
    }
}

static void person_card(cui_ctx *ui, app_state_t *s, int slot, bool me) {
    /* slot < 0 e me = eu; senão peer do slot */
    const char *name; const char *sub; float level; bool mic_off, deaf, by_me = false, can_kick = false;
    char nmbuf[96];
    if (me) {
        name = s->user_name[0] ? s->user_name : "Você"; snprintf(nmbuf, sizeof nmbuf, "%s (você)", name); name = nmbuf;
        sub = s->sh.active ? "Transmitindo a tela" : s->user_status;
        level = (s->mic_on && !s->deaf) ? s->me_level : 0; mic_off = !s->mic_on || s->deaf; deaf = s->deaf;
    } else {
        #define P (s->rtc_peers[slot])
        if (P.name[0]) snprintf(nmbuf, sizeof nmbuf, "%s", P.name); else snprintf(nmbuf, sizeof nmbuf, "Participante %d", slot + 1);
        name = nmbuf; sub = share_peer_live(s, slot) ? "Transmitindo a tela" : P.status[0] ? P.status : P.presence;
        by_me = P.muted_by_me; level = by_me ? 0 : P.level; mic_off = P.mic_off; deaf = P.deafened; can_kick = s->rtc_owner_me && P.fp[0];
    }
    cui_push_id_int(ui, me ? 6000 : 6100 + slot);
    cui_box(ui, .width = CUI_FILL, .radius = 16, .bg = level > 0 ? CUI_RGBA(0x57d28b14) : CUI_RGBA(0xffffff0e), .border = 1,
            .border_color = level > 0 ? CUI_RGBA(0x57d28b66) : B_GLASS_BORDER_SOFT, .pad = 10, .gap = 8) {
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .gap = 10, .align = CUI_ALIGN_CENTER) {
            voice_avatar(ui, me ? s->user_name : name, level, mic_off);
            cui_box(ui, .width = CUI_FILL, .gap = 1, .clip = true) {
                cui_text(ui, name, .font = CUI_FONT_MEDIUM, .size = 13, .nowrap = true);
                if (sub && sub[0]) cui_text(ui, sub, .size = 11, .color = B_MUTED, .nowrap = true);
                else if (level > 0) cui_text(ui, "Falando", .size = 11, .color = B_ONLINE, .nowrap = true);
            }
            if (deaf) cui_icon(ui, CUI_ICON_VOLUME_X, 15, B_DANGER);
            else if (mic_off) cui_icon(ui, CUI_ICON_MIC, 15, B_DANGER);
        }
        if (!me) {
            if (!P.vol_set) { P.vol_set = true; P.volume = 100.0f; }
            cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                float before = P.volume;
                cui_icon(ui, CUI_ICON_VOLUME_2, 14, B_MUTED);
                cui_slider(ui, "##vol", &P.volume, 0, 200, .step = 5, .width = CUI_FILL);
                if (fabsf(before - P.volume) > 0.01f) rtc_set_peer_volume(s, slot, P.volume / 100.0f);
                char pct[12]; snprintf(pct, sizeof pct, "%d%%", (int)(P.volume + 0.5f));
                cui_text(ui, pct, .size = 11, .color = B_MUTED, .nowrap = true);
            }
            cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                if (cui_button(ui, by_me ? "Reativar" : "Silenciar", .variant = by_me ? CUI_BUTTON_PRIMARY : CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) rtc_mute_peer(s, slot, !by_me);
                if (can_kick && cui_button(ui, "Expulsar", .variant = CUI_BUTTON_DANGER, .size = CUI_SIZE_SM)) { s->kick_slot = slot; s->kick_open = true; }
            }
        }
    }
    cui_pop_id(ui);
#undef P
}

void ui_voice_panel(cui_ctx *ui, app_state_t *s) {
    char st[160]; rtc_status_text(s, st, sizeof st);
    if (!s->in_room) {
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .gap = 10, .pad = 16) {
            cui_text(ui, "Sala geral", .font = CUI_FONT_DISPLAY, .size = 18);
            cui_text(ui, st, .size = 12, .color = B_MUTED);
            if (cui_button(ui, "Entrar na sala", .variant = CUI_BUTTON_PRIMARY)) { s->screen = SCREEN_CALL; s->mic_on = true; rtc_join_room(s, "voz-geral"); }
        }
        return;
    }
    cui_box(ui, .width = CUI_FILL, .pad_x = 14, .pad_y = 8) { cui_text(ui, st, .size = 12, .color = B_MUTED, .nowrap = true); }
    cui_scroll(ui, .id = "voice-scroll", .width = CUI_FILL, .height = CUI_FILL, .pad = 10) {
        cui_column(ui, .gap = 8, .width = CUI_FILL) {
            person_card(ui, s, -1, true);
            for (int i = 0; i < 8; i++) if (s->rtc_peers[i].id[0]) person_card(ui, s, i, false);
        }
    }
    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad = 10, .gap = 8, .justify = CUI_JUSTIFY_CENTER, .align = CUI_ALIGN_CENTER) {
        bool mic_eff = s->mic_on && !s->deaf;
        if (cui_icon_button(ui, CUI_ICON_MIC, .variant = mic_eff ? CUI_BUTTON_PRIMARY : CUI_BUTTON_SECONDARY, .tooltip = mic_eff ? "Silenciar microfone" : "Ligar microfone")) {
            if (s->deaf) { s->deaf = false; rtc_set_deaf(s, false); } else { s->mic_on = !s->mic_on; rtc_set_mic(s, s->mic_on); }
        }
        if (cui_icon_button(ui, CUI_ICON_VOLUME_X, .variant = s->deaf ? CUI_BUTTON_PRIMARY : CUI_BUTTON_SECONDARY, .tooltip = s->deaf ? "Voltar a ouvir" : "Ensurdecer")) { s->deaf = !s->deaf; rtc_set_deaf(s, s->deaf); }
        if (cui_button(ui, "Abrir chamada", .variant = CUI_BUTTON_SECONDARY)) s->screen = SCREEN_CALL;
        if (cui_button(ui, "Sair", .variant = CUI_BUTTON_DANGER)) rtc_leave_room(s);
    }
}
