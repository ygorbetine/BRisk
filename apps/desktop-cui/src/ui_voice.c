/* Painel "Sala de voz" completo: participantes com indicador de fala, mudo/ensurdecido, status, silenciar e volume por pessoa,
 * expulsar (só o dono) e os controles de voz de quem está na sala. Tudo em vidro, cantos arredondados. */
#include "call.h"
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
        level = s->mic_on ? s->me_level : 0; mic_off = !s->mic_on; deaf = s->deaf;
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
            else if (mic_off) briks_icon(ui, BRIKS_ICON_MIC_OFF, 15, B_DANGER);
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
    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 14, .pad_y = 8, .gap = 8, .align = CUI_ALIGN_CENTER) {
        if (!strncmp(st, "Conectando", 10) || !strncmp(st, "Reconectando", 12)) cui_spinner(ui, 14);   /* BAS-30: entrando na sala / subindo o serviço de chamadas */
        cui_text(ui, st, .size = 12, .color = B_MUTED, .nowrap = true);
    }
    cui_scroll(ui, .id = "voice-scroll", .width = CUI_FILL, .height = CUI_FILL, .pad = 10) {
        cui_column(ui, .gap = 8, .width = CUI_FILL) {
            person_card(ui, s, -1, true);
            for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (s->rtc_peers[i].id[0]) person_card(ui, s, i, false);
        }
    }
    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad = 10, .gap = 8, .justify = CUI_JUSTIFY_CENTER, .align = CUI_ALIGN_CENTER) {
        bool mic_eff = s->mic_on;
        {   /* botão do microfone: ligado = primário; desligado = vermelho com o ícone riscado (caixa própria: o botão do CUI só aceita glifo) */
            cui_box(ui, .id = "vmic", .width = CUI_PX(36), .height = CUI_PX(36), .radius = 12, .bg = mic_eff ? B_C3 : CUI_RGB(0xe95555), .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                briks_icon(ui, mic_eff ? CUI_ICON_MIC : BRIKS_ICON_MIC_OFF, 16, CUI_RGB(0xffffff));
            }
            if (briks_hover(ui, "vmic")) briks_tip(mic_eff ? "Silenciar microfone" : "Ligar microfone");
            if (briks_clicked_r(ui, "vmic", 12.0f)) { s->mic_on = !s->mic_on; rtc_set_mic(s, s->mic_on); }
        }
        if (cui_icon_button(ui, CUI_ICON_VOLUME_X, .variant = s->deaf ? CUI_BUTTON_PRIMARY : CUI_BUTTON_SECONDARY, .tooltip = s->deaf ? "Voltar a ouvir" : "Ensurdecer")) { s->deaf = !s->deaf; rtc_set_deaf(s, s->deaf); }
        if (cui_button(ui, "Abrir chamada", .variant = CUI_BUTTON_SECONDARY)) s->screen = SCREEN_CALL;
        if (cui_button(ui, "Sair", .variant = CUI_BUTTON_DANGER)) rtc_leave_room(s);
    }
}

/* Mini-painel "Voz conectada" (rodapé do menu lateral): aparece só dentro de uma sala. Clicar no texto abre a chamada;
   os três ícones são microfone, fone (ensurdecer) e desligar. Painel estreito: ficam só os ícones. */
static bool mini_icon(cui_ctx *ui, const char *id, const char *icon, const char *tip, int kind /*0 normal 1 ligado 2 desligado/perigo*/) {
    cui_color bg = kind == 2 ? CUI_RGB(0xe95555) : kind == 1 ? CUI_RGBA(0x57d28b26) : CUI_RGBA(0xffffff12);   /* 2 = vermelho de perigo cheio (microfone desligado, ensurdecido, sair) */
    cui_color fg = kind == 2 ? CUI_RGB(0xffffff) : kind == 1 ? CUI_RGB(0x7be8a8) : B_TEXT;
    cui_box(ui, .id = id, .width = CUI_PX(30), .height = CUI_PX(30), .radius = 15, .bg = bg, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) { briks_icon(ui, icon, 15, fg); }
    if (briks_hover(ui, id)) briks_tip(tip);
    return briks_clicked_r(ui, id, 15.0f);   /* círculo de 30 px */
}
void ui_voice_mini(cui_ctx *ui, app_state_t *s) {
    if (!s->in_room) return;
    const bool narrow = g_panel_w < 212.0f, mic_eff = s->mic_on;
    const bool ok = !strcmp(s->rtc_state, "conectado") || getenv("BRIKS_DEMO_PEERS");
    /* estado de verdade (antes tudo que não era "conectado" aparecia como "Conectando…", inclusive erro e reconexão) */
    const bool bad = !ok && !strcmp(s->rtc_state, "erro");
    const bool reconn = !ok && !strcmp(s->rtc_state, "reconectando");
    const char *state_txt = ok ? (g_panel_w >= 262.0f ? "Em chamada" : "Conectada") : bad ? "Sem conexão" : reconn ? "Reconectando…" : "Conectando…";
    const cui_color state_col = ok ? B_ONLINE : bad ? CUI_RGB(0xff9a9a) : CUI_RGB(0xf5d9a0);
    char title[120]; call_room_title(s, title, sizeof title);
    cui_push_id_int(ui, 6400);
    g_cov_own++;   /* a barra cobre linhas da lista rolada por baixo dela (clicar na seta/mic abria a conversa escondida): cobertura só para o que está sob ela */
    cui_box(ui, .id = "vmbar", .width = CUI_FILL, .pad_x = 10, .pad_top = 8, .gap = 6) {
        share_mini(ui, s, g_panel_w - 20.0f);   /* transmissão no ar e fora da tela da chamada: previa da transmissao acima da barra (ui_share.c) */
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(56), .pad_x = 9, .gap = 4, .radius = 16, .bg = B_GLASS_CARD, .clip = true,
                .border = 1, .border_color = ok ? CUI_RGBA(0x57d28b66) : bad ? CUI_RGBA(0xe9555566) : B_GLASS_BORDER, .align = CUI_ALIGN_CENTER, .justify = narrow ? CUI_JUSTIFY_CENTER : CUI_JUSTIFY_START) {
            if (!narrow) {
                cui_box(ui, .id = "open", .dir = CUI_COLUMN, .gap = 1, .width = CUI_FILL, .pad_x = 2, .clip = true) {
                    cui_text(ui, title, .font = CUI_FONT_MEDIUM, .size = 13, .nowrap = true);   /* nome da sala: o que pode ser cortado é ele, não o estado */
                    cui_row(ui, .gap = 5, .align = CUI_ALIGN_CENTER) {
                        if (!ok && !bad) cui_spinner(ui, 9);   /* BAS-30: conectando/reconectando */
                        else cui_box(ui, .width = CUI_PX(7), .height = CUI_PX(7), .radius = 4, .bg = ok ? B_ONLINE : CUI_RGB(0xe95555)) {}
                        cui_text(ui, state_txt, .size = 11.5f, .color = state_col, .nowrap = true);
                    }
                }
                if (briks_hover(ui, "open")) briks_tip((bad || reconn) && s->rtc_msg[0] ? s->rtc_msg : "Abrir a chamada");
                if (briks_clicked_r(ui, "open", 10.0f)) s->screen = SCREEN_CALL;
            }
            if (mini_icon(ui, "mic", mic_eff ? CUI_ICON_MIC : BRIKS_ICON_MIC_OFF, mic_eff ? "Desligar o microfone" : "Ligar o microfone", mic_eff ? 1 : 2)) {
                s->mic_on = !s->mic_on; rtc_set_mic(s, s->mic_on);
            }
            if (mini_icon(ui, "deaf", s->deaf ? CUI_ICON_VOLUME_X : CUI_ICON_VOLUME_2, s->deaf ? "Voltar a ouvir" : "Parar de ouvir a sala", s->deaf ? 2 : 0)) { s->deaf = !s->deaf; rtc_set_deaf(s, s->deaf); }
            if (mini_icon(ui, "leave", CUI_ICON_PHONE, "Sair da chamada", 2)) rtc_leave_room(s);
        }
    }
    g_cov_own--;
    { cui_rect r = cui_get_rect(ui, cui_make_id(ui, "vmbar")); if (r.w > 0) briks_cover(r.x, r.y, r.w, r.h); }
    cui_pop_id(ui);
}