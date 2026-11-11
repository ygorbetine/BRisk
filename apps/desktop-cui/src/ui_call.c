/* Tela de chamada: cabeçalho com abas em blocos, grade de tiles e barra de controles flutuante.
 * Medidas de apps/web/src/components/CallWorkspace.tsx + call-workspace.css / glass-layout.css. */
#include "briks_ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct { const char *name; bool me, speaking, muted; float level; int slot; bool by_me; bool can_kick; const char *status; } tile_t;
enum { TILE_NONE = 0, TILE_MUTE = 1, TILE_KICK = 2 };
static float g_lvl[10];   /* nível de fala por tile, lido pelo canvas do avatar na hora de pintar */


static void initials2(const char *name, char *out) {
    const char *sp = strchr(name, ' ');
    out[0] = name[0] ? name[0] : '?';
    out[1] = sp && sp[1] ? sp[1] : (name[0] && name[1] ? name[1] : 0);
    out[2] = 0;
    for (int i = 0; i < 2 && out[i]; i++) if (out[i] >= 'a' && out[i] <= 'z') out[i] -= 32;
}

/* degradê roxo (--briks-c1 -> --briks-c2) num retângulo arredondado */
static void paint_grad(cui_ctx *ui, cui_rect r, void *user) {
    float rad = *(const float *)user;
    cui_draw_rect_gradient(ui, r, CUI_CORNERS(rad), B_C1, B_C2);
}

/* avatar do tile: halo suave + disco em degradê + anel */
static void paint_avatar(cui_ctx *ui, cui_rect r, void *user) {
    float lvl = user ? *(const float *)user : 0;
    cui_rect box = r;                                   /* o canvas é maior que o disco para o halo não ser cortado */
    float d = 128;
    r = (cui_rect){ box.x + (box.w - d) * 0.5f, box.y + (box.h - d) * 0.5f, d, d };
    /* halo radial: discos concêntricos com queda suave, todos dentro do canvas (nunca um retângulo de blur cortado) */
    {
        cui_vec2 c = { box.x + box.w * 0.5f, box.y + box.h * 0.5f };
        float rmax = fminf(box.w, box.h) * 0.5f - 1, rmin = d * 0.5f;
        const int N = 28;
        for (int k = 0; k < N; k++) {
            float t = (float)k / (N - 1);                       /* 0 fora .. 1 dentro */
            float rad = rmax - (rmax - rmin) * t;
            cui_color h = B_C3; h.a = (unsigned char)(2 + 5 * t * t);
            cui_draw_circle(ui, c, rad, h);
        }
        if (lvl > 0) {   /* falando: anel fino e nítido (2,5 px) com brilho suave que cresce com o nível */
            float gap = 7 + 3 * lvl;
            cui_color glow = CUI_RGB(0x57d28b); glow.a = (unsigned char)(30 + 50 * lvl);
            cui_color ring = CUI_RGB(0x57d28b); ring.a = (unsigned char)(150 + 100 * lvl);
            float rg = rmin + gap;
            cui_draw_rect_border(ui, (cui_rect){ c.x - rg - 3, c.y - rg - 3, (rg + 3) * 2, (rg + 3) * 2 }, CUI_CORNERS(rg + 3), CUI_RGBA(0x00000000), glow, 6);
            cui_draw_rect_border(ui, (cui_rect){ c.x - rg, c.y - rg, rg * 2, rg * 2 }, CUI_CORNERS(rg), CUI_RGBA(0x00000000), ring, 2.5f);
        }
    }
    cui_color sh = CUI_RGBA(0x00000066);
    cui_draw_shadow(ui, r, CUI_CORNERS(r.w * 0.5f), 18, 0, (cui_vec2){ 0, 6 }, sh);
    cui_draw_rect(ui, r, CUI_CORNERS(r.w * 0.5f), B_C3);
    cui_draw_rect_gradient(ui, (cui_rect){ r.x + 3, r.y + 3, r.w - 6, r.h - 6 }, CUI_CORNERS((r.w - 6) * 0.5f), B_C3, B_C2);
}

/* devolve a ação pedida: clicar no rótulo ou "Silenciar" no menu de contexto (botão direito) alterna o silêncio; "Expulsar" (só o dono) pede confirmação */
static int tile(cui_ctx *ui, const tile_t *t, bool mic_on) {
    int act = TILE_NONE;
    bool muted = t->muted || (t->me && !mic_on);
    bool speaking = t->level > 0;
    int gi = t->slot < 0 ? 0 : t->slot > 9 ? 9 : t->slot;   /* 0..7 peers, 8 prévia, 9 eu */
    g_lvl[gi] = t->level;
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .radius = 20, .bg = B_A(B_PANEL, 0x80), .clip = true,
            .border = speaking ? 2 : 1, .border_color = speaking ? CUI_RGBA(0x57d28bcc) : B_A(B_C1, 0x55),
            .dir = CUI_STACK) {
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
            cui_box(ui, .dir = CUI_STACK, .width = CUI_PX(300), .height = CUI_PX(300), .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                cui_push_id_int(ui, (int)(t->level * 8));   /* o canvas é reaproveitado por id: muda o id quando o nível muda */
                cui_canvas(ui, "av", CUI_FILL, CUI_FILL, paint_avatar, &g_lvl[gi], 0);
                cui_pop_id(ui);
                char in[4]; initials2(t->name, in);
                cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                    cui_text(ui, in, .font = CUI_FONT_DISPLAY, .size = 40, .color = CUI_RGB(0xffffff));
                }
            }
        }
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_END, .pad = 14) {
            briks_status_pill(ui, t->status);   /* chip de status acima do rótulo: só com status */
            cui_card_begin(ui, &(cui_card_opts){ .id = "lab", .hoverable = true, .dir = CUI_ROW, .gap = 7, .pad_x = 12, .pad_y = 8, .align = CUI_ALIGN_CENTER });
            {
                if (muted) cui_icon(ui, CUI_ICON_VOLUME_X, 14, B_DANGER);
                if (t->by_me) cui_icon(ui, CUI_ICON_VOLUME_X, 14, B_MUTED);
                char lab[160];
                if (t->me) snprintf(lab, sizeof lab, "Você · %s", t->name); else snprintf(lab, sizeof lab, "%s%s", t->name, t->by_me ? " (silenciado por você)" : "");
                cui_text(ui, lab, .size = 14);
            }
            cui_card_end(ui);
            if (!t->me) {
                if (briks_clicked(ui, "lab")) act = TILE_MUTE;
                {   /* botão direito no rótulo: popover de vidro acima do rótulo (não o cobre) */
                    cui_rect lr = cui_get_rect(ui, cui_make_id(ui, "lab")); cui_vec2 mp = cui_mouse_pos(ui);
                    if (cui_mouse_clicked(ui, CUI_MOUSE_RIGHT) && lr.w > 0 && mp.x >= lr.x && mp.y >= lr.y && mp.x < lr.x + lr.w && mp.y < lr.y + lr.h) cui_popover_open(ui, "pmenu");
                }
                cui_popover(ui, "pmenu", .placement = CUI_PLACE_TOP_START, .pad = 6, .gap = 8) {
                    cui_menu_label(ui, t->name);
                    if (cui_menu_item(ui, t->by_me ? "Reativar o áudio dele" : "Silenciar para mim", .icon = CUI_ICON_VOLUME_X)) act = TILE_MUTE;
                    if (t->can_kick) {
                        cui_menu_separator(ui);
                        if (cui_menu_item(ui, "Expulsar da chamada…", .icon = CUI_ICON_PHONE, .danger = true)) act = TILE_KICK;
                    }
                }
                if (act != TILE_NONE) cui_popover_close(ui, "pmenu");
            }
        }
    }
    return act;
}

/* aba de um segmented control: ativa = pílula em degradê com texto branco; inativa = texto muted */
static bool tab(cui_ctx *ui, int id, const char *icon, const char *label, bool active) {
    static float rad = 9;
    cui_push_id_int(ui, id);
    cui_box(ui, .id = "b", .dir = CUI_STACK, .height = CUI_PX(34), .radius = 9, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
        if (active) cui_canvas(ui, "tabbg", CUI_FILL, CUI_FILL, paint_grad, &rad, 0);
        cui_box(ui, .dir = CUI_ROW, .gap = 7, .pad_x = 16, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
            cui_color c = active ? CUI_RGB(0xffffff) : B_A(B_MUTED, 0x66);
            if (icon) cui_icon(ui, icon, 15, c);
            cui_text(ui, label, .font = CUI_FONT_DISPLAY, .size = 15, .color = c);
        }
    }
    bool hit = briks_clicked(ui, "b");
    cui_pop_id(ui);
    return hit;
}

static void seg_begin_style(void) {}

static bool pill(cui_ctx *ui, int id, const char *icon, const char *label, int kind /*0 normal 1 ativo-verde 2 perigo*/) {
    cui_push_id_int(ui, id);
    cui_color bg = kind == 2 ? CUI_RGB(0xe95555) : kind == 1 ? CUI_RGBA(0x57d28b26) : CUI_RGBA(0xffffff12);
    cui_color bd = kind == 2 ? CUI_RGB(0xe95555) : kind == 1 ? CUI_RGBA(0x57d28b88) : B_GLASS_BORDER;
    cui_color fg = kind == 2 ? CUI_RGB(0xffffff) : kind == 1 ? CUI_RGB(0x7be8a8) : B_TEXT;
    cui_box(ui, .id = "b", .dir = CUI_ROW, .gap = 8, .pad_x = 18, .height = CUI_PX(50), .radius = 25, .bg = bg, .border = 1, .border_color = bd,
            .align = CUI_ALIGN_CENTER) {
        cui_icon(ui, icon, 18, fg);
        cui_text(ui, label, .font = CUI_FONT_DISPLAY, .size = 15, .color = fg);
    }
    bool hit = briks_clicked(ui, "b");
    cui_pop_id(ui);
    return hit;
}

static void empty_state(cui_ctx *ui, const char *icon, const char *title, const char *sub) {
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .gap = 10) {
        cui_icon(ui, icon, 34, B_C3);
        cui_text(ui, title, .font = CUI_FONT_DISPLAY, .size = 20);
        cui_text(ui, sub, .size = 14, .color = B_MUTED);
    }
}

void ui_call(cui_ctx *ui, app_state_t *s) {
    (void)seg_begin_style;
    share_tick(ui, s);
    const char *group = s->active_group >= 0 ? s->groups[s->active_group].name : (s->group_count ? s->groups[0].name : "Sala geral");
    char sub[200], st[160]; snprintf(sub, sizeof sub, "%s · %s", group, rtc_status_text(s, st, sizeof st));
    briks_ambient(ui, 0.85f);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 0) {
        /* cabeçalho */
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(70), .pad_x = 20, .bg = B_GLASS_SOFT,
                .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER) {
            cui_row(ui, .gap = 18, .align = CUI_ALIGN_CENTER) {
                briks_brand(ui, 20);
                cui_column(ui, .gap = 1) {
                    cui_text(ui, "Geral", .font = CUI_FONT_DISPLAY, .size = 15);
                    cui_text(ui, sub, .size = 12, .color = B_MUTED);
                }
            }
            cui_row(ui, .gap = 12, .align = CUI_ALIGN_CENTER) {
                cui_push_id_int(ui, 900);
                cui_box(ui, .id = "b", .width = CUI_PX(38), .height = CUI_PX(38), .radius = 12, .bg = B_GLASS, .border = 1,
                        .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                    cui_icon(ui, CUI_ICON_MINIMIZE_2, 16, B_TEXT);
                }
                if (briks_clicked(ui, "b")) s->screen = SCREEN_SOCIAL;
                cui_pop_id(ui);
                cui_box(ui, .dir = CUI_ROW, .gap = 4, .pad = 4, .radius = 12, .bg = B_GLASS, .border = 1, .border_color = B_GLASS_BORDER) {
                    /* ids 11..14: as pílulas da barra usam 1..5 com o mesmo nome de caixa; ids iguais faziam um clique valer nos dois lugares */
                    if (tab(ui, 11, CUI_ICON_PHONE, "Call", s->call_mode == 0)) s->call_mode = 0;
                    if (tab(ui, 12, CUI_ICON_MONITOR, "Transmissão", s->call_mode == 1)) s->call_mode = 1;
                }
                cui_box(ui, .dir = CUI_ROW, .gap = 4, .pad = 4, .radius = 12, .bg = B_GLASS, .border = 1, .border_color = B_GLASS_BORDER) {
                    if (tab(ui, 13, CUI_ICON_VIDEO, "Chamada", s->call_view == 0)) s->call_view = 0;
                    if (tab(ui, 14, CUI_ICON_MESSAGE_SQUARE, "Chat", s->call_view == 1)) s->call_view = 1;
                }
            }
        }
        cui_separator(ui);
        /* palco */
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 18, .id = "stage") {
            if (s->call_view == 1) {
                cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .radius = 20, .bg = B_GLASS, .border = 1, .border_color = B_GLASS_BORDER_SOFT, .pad = 18, .gap = 10, .scroll_y = true) {
                    if (!s->msg_count) cui_text(ui, "Nenhuma mensagem nesta sala ainda.", .size = 14, .color = B_MUTED);
                    for (int i = 0; i < s->msg_count; i++) {
                        cui_push_id_int(ui, i);
                        cui_column(ui, .gap = 2) {
                            cui_text(ui, s->msgs[i].mine ? "Você" : s->msgs[i].author, .size = 12, .color = B_C3);
                            cui_text(ui, s->msgs[i].content, .size = 15);
                        }
                        cui_pop_id(ui);
                    }
                }
            } else if (s->call_mode == 1) {
                if (!ui_share_stage(ui, s)) {
                    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .radius = 20, .bg = B_A(B_PANEL, 0x80), .border = 1, .border_color = B_A(B_C1, 0x55)) {
                        empty_state(ui, CUI_ICON_MONITOR, "Ninguém transmitindo", "Quando alguém compartilhar a tela, ela aparece aqui.");
                    }
                }
            } else {
                bool mic_eff = s->mic_on && !s->deaf;   /* ensurdecer implica microfone mudo */
                const tile_t me = { s->user_name[0] ? s->user_name : "Você", true, mic_eff, false, mic_eff ? s->me_level : 0, 9, false, false, s->sh.active ? "Transmitindo a tela" : s->user_status };
                const tile_t other = { "Marina Costa", false, false, false, 0, 8, false, false, "" };
                cui_row(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 14) {
                    tile(ui, &me, mic_eff);
                    if (!s->in_room) tile(ui, &other, s->mic_on);   /* prévia local sem sala */
                    for (int i = 0; i < 8; i++) if (s->rtc_peers[i].id[0]) {
                        char nm[64]; if (s->rtc_peers[i].name[0]) snprintf(nm, sizeof nm, "%s", s->rtc_peers[i].name); else snprintf(nm, sizeof nm, "Participante %d", i + 1);
                        const tile_t t = { nm, false, false, s->rtc_peers[i].mic_off || s->rtc_peers[i].deafened, s->rtc_peers[i].muted_by_me ? 0 : s->rtc_peers[i].level, i, s->rtc_peers[i].muted_by_me,
                                           s->rtc_owner_me && s->rtc_peers[i].fp[0] != 0, share_peer_live(s, i) ? "Transmitindo a tela" : s->rtc_peers[i].status };
                        cui_push_id_int(ui, 700 + i);
                        int act = tile(ui, &t, true);
                        if (act == TILE_MUTE) rtc_mute_peer(s, i, !s->rtc_peers[i].muted_by_me);
                        else if (act == TILE_KICK) { s->kick_slot = i; s->kick_open = true; }
                        cui_pop_id(ui);
                    }
                }
            }
        }
        /* barra de controles flutuante */
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(104), .justify = CUI_JUSTIFY_CENTER, .align = CUI_ALIGN_CENTER) {
            cui_box(ui, .dir = CUI_ROW, .gap = 10, .pad = 14, .radius = 28, .bg = B_GLASS_STRONG, .border = 1, .border_color = B_GLASS_BORDER,
                    .elevation = briks_elev(CUI_ELEVATION_RAISED), .align = CUI_ALIGN_CENTER) {
                if (pill(ui, 1, CUI_ICON_MIC, "Microfone", (s->mic_on && !s->deaf) ? 1 : 0)) {
                    if (s->deaf) { s->deaf = false; rtc_set_deaf(s, false); }   /* ligar o mic sai do modo ensurdecido */
                    else { s->mic_on = !s->mic_on; rtc_set_mic(s, s->mic_on); }
                }
                if (pill(ui, 2, CUI_ICON_VOLUME_2, "Ensurdecer", s->deaf ? 1 : 0)) { s->deaf = !s->deaf; rtc_set_deaf(s, s->deaf); }
                cui_push_disabled(ui, true); pill(ui, 3, CUI_ICON_VIDEO, "Câmera · em breve", 0); cui_pop_disabled(ui);
                cui_push_id_int(ui, 3); if (briks_hover(ui, "b")) cui_tooltip(ui, "O envio de vídeo da câmera chega numa próxima versão. Compartilhar tela já funciona."); cui_pop_id(ui);   /* sem backend de vídeo da câmera nesta versão */
                ui_share_bar(ui, s);
                if (pill(ui, 5, CUI_ICON_PHONE, "Sair", 2)) { rtc_leave_room(s); s->screen = SCREEN_SOCIAL; }
            }
        }
    }
    ui_share_dialog(ui, s);
    /* confirmação rápida antes de expulsar */
    if (s->kick_open) {
        cui_vec2 vp = cui_viewport_size(ui);
        const char *who = (s->kick_slot >= 0 && s->kick_slot < 8 && s->rtc_peers[s->kick_slot].name[0]) ? s->rtc_peers[s->kick_slot].name : "esta pessoa";
        cui_dialog(ui, "Expulsar da chamada", &s->kick_open, .width = fminf(420.0f, fmaxf(vp.x - 48.0f, 260.0f))) {
            cui_textf(ui, "Expulsar %s da chamada?", who);
            cui_text(ui, "Ela será desconectada e não poderá voltar a esta sala nesta sessão até você permitir.", .size = 13, .color = B_MUTED);
            cui_row(ui, .gap = 10) {
                if (cui_button(ui, "Cancelar")) s->kick_open = false;
                if (cui_button(ui, "Expulsar", .variant = CUI_BUTTON_DANGER)) { rtc_kick_peer(s, s->kick_slot); s->kick_open = false; }
            }
        }
    }
}
