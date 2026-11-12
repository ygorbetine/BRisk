/* Tela de chamada: cabeçalho com abas em blocos, palco (grade ou foco de participantes, transmissão, chat da sala) e barra de
 * controles flutuante. Tela cheia (F11/Esc), indicador de rede, ajustes de áudio e pedido de transmissão ficam aqui.
 * Medidas de apps/web/src/components/CallWorkspace.tsx + call-workspace.css / glass-layout.css; lógica pura em callhist.c. */
#include "call.h"
#include "netdiag.h"
#include "call_idle.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

call_ui_t g_call = { .focus = -1 };
static int g_sheen_seen;   /* roteiros: quadros em que o brilho foi desenhado no botão Transmissão */

static app_state_t *g_tile_s;   /* estado para o menu do bloco (slider de volume por pessoa) */
typedef struct { const char *name; bool me, muted; float level; int slot; bool by_me, can_kick, live; const char *status; const char *badge; cui_texture vtex; int vw, vh; bool deaf; } tile_t;
enum { TILE_NONE = 0, TILE_MUTE, TILE_KICK, TILE_FOCUS, TILE_ASK, TILE_WATCH, TILE_ROOM_MUTE };
#define SLOT_ME (BRIKS_MAX_PEERS + 1)
static bool g_can_room_mute;   /* sou o dono e a chamada está no servidor de chamadas: posso silenciar alguém para todos */
static struct { float lvl, d; } g_av[BRIKS_MAX_PEERS + 2];   /* nível de fala e diâmetro do avatar por tile (0..7 peers, 8 prévia, 9 eu), lidos pelo canvas ao pintar */
static bool g_bar_visible = true, g_aud_open;

/* ---------- chat da sala ---------- */
static void iso_from_ms(long long ms, char *out, size_t cap) {
    SYSTEMTIME t;
    if (ms > 0) { ULARGE_INTEGER u; u.QuadPart = (unsigned long long)ms * 10000ULL + 116444736000000000ULL; FILETIME ft = { u.LowPart, u.HighPart }; if (!FileTimeToSystemTime(&ft, &t)) GetSystemTime(&t); }
    else GetSystemTime(&t);
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
}

void call_chat_add(const char *id, const char *author, const char *content, long long ts_ms, bool mine) {
    if (!content || !content[0]) return;
    for (int i = 0; i < g_call.msg_count; i++) if (id && id[0] && !strcmp(g_call.msgs[i].id, id)) return;   /* repetida */
    if (g_call.msg_count >= CALL_MAX_MSGS) { memmove(g_call.msgs, g_call.msgs + 1, sizeof(msg_t) * (CALL_MAX_MSGS - 1)); g_call.msg_count = CALL_MAX_MSGS - 1; }
    msg_t *m = &g_call.msgs[g_call.msg_count++];
    memset(m, 0, sizeof *m);
    snprintf(m->id, sizeof m->id, "%s", id ? id : ""); snprintf(m->author, sizeof m->author, "%s", author && author[0] ? author : "Participante");
    snprintf(m->content, sizeof m->content, "%s", content); iso_from_ms(ts_ms, m->created, sizeof m->created);
    m->mine = mine;
}

void call_chat_send(app_state_t *s) {
    char *t = g_call.draft; while (*t == ' ') t++;
    size_t n = strlen(t); while (n && t[n - 1] == ' ') t[--n] = 0;
    if (!n) { g_call.draft[0] = 0; return; }
    static unsigned seq; char id[48];
    snprintf(id, sizeof id, "rc-%08lx%08lx-%u", (unsigned long)GetTickCount(), (unsigned long)GetCurrentProcessId(), ++seq);
    call_chat_add(id, s->user_name[0] ? s->user_name : "Você", t, 0, true);
    rtc_room_chat(id, t);
    g_call.draft[0] = 0;
}

void call_toast(const char *text) { snprintf(g_call.toast, sizeof g_call.toast, "%s", text); g_call.toast_until = -1; }

void call_request_in(const char *peer_id, const char *name) {
    g_call.req.open = true; g_call.req.at = -1;
    snprintf(g_call.req.peer_id, sizeof g_call.req.peer_id, "%s", peer_id);
    snprintf(g_call.req.name, sizeof g_call.req.name, "%s", name && name[0] ? name : "Alguém");
}

/* ---------- tela cheia ---------- */
void titlebar_set_fullscreen(bool on);   /* titlebar.c */
static struct { bool applied; LONG_PTR style; RECT rc; } g_fsw;   /* estilo e retângulo de antes (janela maximizada volta maximizada: o estilo guarda isso) */
void call_set_fullscreen(app_state_t *s, bool on) {
    if (g_call.fullscreen == on) return;
    g_call.fullscreen = on; g_call.last_move = -1;
    titlebar_set_fullscreen(on);   /* titlebar.c: em tela cheia a janela toda é HTCLIENT e a barra própria some */
    HWND h = s->app ? (HWND)cui_app_native_window(s->app) : NULL;
    if (!h) return;
    /* janela oculta (teste automatizado): só o layout imersivo, sem mexer na janela; BRIKS_TEST_FULLSCREEN exercita o caminho real, ainda oculto */
    const bool vis = IsWindowVisible(h) != 0;
    if (on && (vis || getenv("BRIKS_TEST_FULLSCREEN"))) {
        MONITORINFO mi = { sizeof mi };
        if (!GetWindowRect(h, &g_fsw.rc) || !GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return;
        g_fsw.style = GetWindowLongPtrA(h, GWL_STYLE);
        SetWindowLongPtrA(h, GWL_STYLE, g_fsw.style & ~(LONG_PTR)WS_OVERLAPPEDWINDOW);
        SetWindowPos(h, NULL, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
        g_fsw.applied = true;
    } else if (!on && g_fsw.applied) {
        g_fsw.applied = false;
        SetWindowLongPtrA(h, GWL_STYLE, g_fsw.style);
        SetWindowPos(h, NULL, g_fsw.rc.left, g_fsw.rc.top, g_fsw.rc.right - g_fsw.rc.left, g_fsw.rc.bottom - g_fsw.rc.top, SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    }
}

void call_reset(app_state_t *s) {
    call_set_fullscreen(s, false);
    g_call.msg_count = 0; g_call.unread = 0; g_call.draft[0] = 0; g_call.focus = -1;
    g_call.req.open = false; g_call.toast[0] = 0;
}

static bool g_tile_me_deaf;
int call_probe(const char *n) {
    if (!strcmp(n, "call_layout")) return g_call.layout;
    if (!strcmp(n, "call_focus")) return g_call.focus;
    if (!strcmp(n, "sheen_frames")) return g_sheen_seen > 0;   /* 1 = o brilho já correu pelo botão */
    if (!strcmp(n, "call_full")) return g_call.fullscreen;
    if (!strcmp(n, "call_msgs")) return g_call.msg_count;
    if (!strcmp(n, "call_unread")) return g_call.unread;
    if (!strcmp(n, "call_req")) return g_call.req.open;
    if (!strcmp(n, "call_bar")) return g_bar_visible;
    if (!strcmp(n, "call_audio")) return g_aud_open;
    if (!strcmp(n, "call_deaf")) return g_tile_me_deaf;
    if (!strcmp(n, "call_fs_window")) return g_fsw.applied;
    return history_probe(n);
}

/* ---------- desenho ---------- */
static void initials2(const char *name, char *out) {
    const char *sp = strchr(name, ' ');
    out[0] = name[0] ? name[0] : '?';
    out[1] = sp && sp[1] ? sp[1] : (name[0] && name[1] ? name[1] : 0);
    out[2] = 0;
    for (int i = 0; i < 2 && out[i]; i++) if (out[i] >= 'a' && out[i] <= 'z') out[i] -= 32;
}

/* degradê do tema (--briks-c1 -> --briks-c2) num retângulo arredondado */
static void paint_grad(cui_ctx *ui, cui_rect r, void *user) {
    float rad = *(const float *)user;
    cui_draw_rect_gradient(ui, r, CUI_CORNERS(rad), B_C1, B_C2);
}

/* avatar do tile: halo suave + disco em degradê + anel; tudo proporcional ao diâmetro d */
static void paint_avatar(cui_ctx *ui, cui_rect r, void *user) {
    const float lvl = user ? ((const float *)user)[0] : 0, d = user ? ((const float *)user)[1] : 128, k = d / 128.0f;
    cui_rect box = r;                                   /* o canvas é maior que o disco para o halo não ser cortado */
    r = (cui_rect){ box.x + (box.w - d) * 0.5f, box.y + (box.h - d) * 0.5f, d, d };
    /* halo radial: discos concêntricos com queda suave, todos dentro do canvas (nunca um retângulo de blur cortado) */
    {
        cui_vec2 c = { box.x + box.w * 0.5f, box.y + box.h * 0.5f };
        float rmax = fminf(box.w, box.h) * 0.5f - 1, rmin = d * 0.5f;
        const int N = 28;
        for (int i = 0; i < N; i++) {
            float t = (float)i / (N - 1);                       /* 0 fora .. 1 dentro */
            float rad = rmax - (rmax - rmin) * t;
            cui_color h = B_C3; h.a = (unsigned char)(2 + 5 * t * t);
            cui_draw_circle(ui, c, rad, h);
        }
        if (lvl > 0) {   /* falando: anel fino e nítido com brilho suave que cresce com o nível */
            float gap = (7 + 3 * lvl) * fmaxf(k, 0.55f);
            cui_color glow = CUI_RGB(0x57d28b); glow.a = (unsigned char)(30 + 50 * lvl);
            cui_color ring = CUI_RGB(0x57d28b); ring.a = (unsigned char)(150 + 100 * lvl);
            float rg = fminf(rmin + gap, rmax - 4);
            cui_draw_rect_border(ui, (cui_rect){ c.x - rg - 3, c.y - rg - 3, (rg + 3) * 2, (rg + 3) * 2 }, CUI_CORNERS(rg + 3), CUI_RGBA(0x00000000), glow, 6);
            cui_draw_rect_border(ui, (cui_rect){ c.x - rg, c.y - rg, rg * 2, rg * 2 }, CUI_CORNERS(rg), CUI_RGBA(0x00000000), ring, 2.5f);
        }
    }
    cui_color sh = CUI_RGBA(0x00000066);
    cui_draw_shadow(ui, r, CUI_CORNERS(r.w * 0.5f), 18 * k, 0, (cui_vec2){ 0, 6 * k }, sh);
    cui_draw_rect(ui, r, CUI_CORNERS(r.w * 0.5f), B_C3);
    const float in = fmaxf(2.0f, 3.0f * k);
    cui_draw_rect_gradient(ui, (cui_rect){ r.x + in, r.y + in, r.w - in * 2, r.h - in * 2 }, CUI_CORNERS((r.w - in * 2) * 0.5f), B_C3, B_C2);
}

static bool g_tile_me_deaf;   /* o bloco "Você" mostra o ícone de ensurdecido (atualizado a cada quadro antes de desenhar os blocos) */
static bool in_rect(cui_rect r, cui_vec2 p) { return r.w > 0 && p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h; }

/* Um participante num bloco de w x h. Clique no bloco destaca (modo Foco); clique no rótulo silencia para mim; botão direito abre o menu. */
static int tile(cui_ctx *ui, const tile_t *t, float w, float h, bool mic_on) {
    int act = TILE_NONE;
    const bool muted = t->muted || (t->me && !mic_on), speaking = t->level > 0;
    const int gi = t->slot < 0 ? 0 : t->slot > SLOT_ME ? SLOT_ME : t->slot;
    const bool sm = w < 260.0f || h < 200.0f;
    /* bloco pequeno: o avatar fica na área acima do rótulo (que cresce com o tamanho do texto), nunca atrás dele */
    const float lab_h = sm ? 12.0f * cui_theme_get(ui)->font_scale + 10.0f + 8.0f + 4.0f : 0.0f;
    const float side = fminf(w, h - lab_h), d = fminf(fmaxf(side * (sm ? 0.62f : 0.40f), 22.0f), 128.0f), cv = fmaxf(fminf(d * 2.34f, side - 4.0f), d);
    g_av[gi].lvl = t->level; g_av[gi].d = d;
    cui_box(ui, .id = "tile", .width = CUI_PX(w), .height = CUI_PX(h), .radius = sm ? 16 : 20, .bg = B_A(B_PANEL, 0x80), .clip = true,
            .border = speaking ? 2 : 1, .border_color = speaking ? CUI_RGBA(0x57d28bcc) : B_A(B_C1, 0x55), .dir = CUI_STACK) {
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .pad_bottom = lab_h) {
            if (t->vtex && t->vw > 0 && t->vh > 0) {   /* câmera ligada: o vídeo ocupa o bloco (inteiro, sem cortar), com folga para os cantos arredondados */
                const float aw = fmaxf(w - 8.0f, 16.0f), ah = fmaxf(h - 8.0f - lab_h, 16.0f), sc = fminf(aw / (float)t->vw, ah / (float)t->vh);
                cui_image(ui, t->vtex, .width = CUI_PX(floorf((float)t->vw * sc)), .height = CUI_PX(floorf((float)t->vh * sc)), .radius = sm ? 10 : 14);
            } else
            cui_box(ui, .dir = CUI_STACK, .width = CUI_PX(cv), .height = CUI_PX(cv), .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                cui_push_id_int(ui, (int)(t->level * 8) + ((int)d << 4));   /* o canvas é reaproveitado por id: muda o id quando o nível ou o tamanho muda */
                cui_canvas(ui, "av", CUI_FILL, CUI_FILL, paint_avatar, &g_av[gi], 0);
                cui_pop_id(ui);
                cui_texture th = 0; int tw = 0, thh = 0;
                const bool has_th = t->live && !t->me && g_tile_s && share_thumb(ui, g_tile_s, t->slot, &th, &tw, &thh);
                if (t->live && !t->me) {   /* quem transmite: thumb desfocada da tela como fundo do avatar, ícone de transmissão por cima e anel pulsante */
                    const float pulse = 0.5f + 0.5f * sinf((float)cui_time(ui) * 3.2f), ringd = fminf(d + 10.0f, cv);
                    cui_request_wakeup(ui, 0.06f);
                    if (has_th) cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                        const float di = d - 2.0f;   /* a imagem tem o PRÓPRIO raio (o recorte do pai é retangular): disco perfeito; a tela esticada para o quadrado some no desfoque */
                        cui_box(ui, .dir = CUI_STACK, .width = CUI_PX(di), .height = CUI_PX(di), .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                            cui_image(ui, th, .width = CUI_PX(di), .height = CUI_PX(di), .radius = (int)(di * 0.5f));
                            cui_box(ui, .width = CUI_PX(di), .height = CUI_PX(di), .radius = (int)(di * 0.5f), .bg = CUI_RGBA(0x1a0a2678)) {}
                        }
                    }
                    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                        cui_box(ui, .width = CUI_PX(ringd), .height = CUI_PX(ringd), .radius = (int)(ringd * 0.5f), .border = 2, .border_color = B_A(B_C3, (unsigned char)(110 + 120 * pulse))) {}
                    }
                    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) { cui_icon(ui, CUI_ICON_MONITOR, fmaxf(12.0f, d * 0.34f), CUI_RGB(0xffffff)); }
                    const float dp = fmaxf(7.0f, d * 0.12f), off = (cv - d) * 0.5f + d * 0.06f;
                    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_END, .justify = CUI_JUSTIFY_START, .pad_right = off, .pad_top = off) {
                        cui_box(ui, .width = CUI_PX(dp), .height = CUI_PX(dp), .radius = (int)(dp * 0.5f), .bg = B_A(CUI_RGB(0xff4d6d), (unsigned char)(150 + 105 * pulse)), .border = 1, .border_color = CUI_RGBA(0x1a0a26cc)) {}
                    }
                }
                char in[4]; initials2(t->name, in);
                if (!(t->live && !t->me)) cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                    cui_text(ui, in, .font = CUI_FONT_DISPLAY, .size = fmaxf(10.0f, d * 0.31f / cui_theme_get(ui)->font_scale), .color = CUI_RGB(0xffffff));   /* as iniciais acompanham o disco, não a escala do texto */
                }
            }
        }
        if (t->badge && !sm) cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_START, .pad = 14) {
            cui_box(ui, .dir = CUI_ROW, .gap = 6, .pad_x = 11, .pad_y = 6, .radius = 13, .bg = B_GLASS_STRONG, .border = 1, .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER) {
                cui_icon(ui, CUI_ICON_MAXIMIZE_2, 12, B_C3);
                cui_text(ui, t->badge, .size = 12, .color = B_TEXT, .nowrap = true);
            }
        }
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_END, .pad = sm ? 8 : 14, .gap = 6) {
            if (!sm && h >= 300.0f) briks_status_pill(ui, t->status);   /* chip de status acima do rótulo: só com status e com espaço */
            cui_card_begin(ui, &(cui_card_opts){ .id = "lab", .hoverable = true, .dir = CUI_ROW, .gap = sm ? 5 : 7, .pad_x = sm ? 9 : 12, .pad_y = sm ? 5 : 8, .align = CUI_ALIGN_CENTER });
            {
                const float ts = sm ? 12.0f : 14.0f;
                if (muted) briks_icon(ui, BRIKS_ICON_MIC_OFF, ts, B_DANGER);
                if (t->deaf || (t->me && g_tile_me_deaf)) cui_icon(ui, CUI_ICON_VOLUME_X, ts, B_DANGER);   /* ensurdecido: ícone próprio, separado do mic mudo */
                if (t->by_me) cui_icon(ui, CUI_ICON_VOLUME_X, ts, B_MUTED);
                if (t->live) cui_icon(ui, CUI_ICON_MONITOR, ts, B_ONLINE);
                char lab[160];
                if (t->me) { if (sm) snprintf(lab, sizeof lab, "Você"); else snprintf(lab, sizeof lab, "Você · %s", t->name); }
                else if (sm) snprintf(lab, sizeof lab, "%.22s", t->name);
                else snprintf(lab, sizeof lab, "%s%s", t->name, t->by_me ? " (silenciado por você)" : "");
                cui_text(ui, lab, .size = ts, .nowrap = true);
            }
            cui_card_end(ui);
            const cui_rect lr = cui_get_rect(ui, cui_make_id(ui, "lab")), tr = cui_get_rect(ui, cui_make_id(ui, "tile"));
            const cui_vec2 mp = cui_mouse_pos(ui);
            if (!t->me) {
                if (briks_clicked_r(ui, "lab", 20.0f)) act = TILE_MUTE;
                if (!briks_blocked() && cui_mouse_clicked(ui, CUI_MOUSE_RIGHT) && in_rect(tr, mp)) cui_popover_open(ui, "pmenu");   /* botão direito em qualquer ponto do bloco */
                cui_popover(ui, "pmenu", .placement = CUI_PLACE_TOP_START, .pad = 6, .gap = 8) {
                    cui_menu_label(ui, t->name);
                    if (cui_menu_item(ui, t->by_me ? "Reativar o áudio dele" : "Silenciar para mim", .icon = CUI_ICON_VOLUME_X)) act = TILE_MUTE;
                    if (g_tile_s && t->slot >= 0 && t->slot < BRIKS_MAX_PEERS) {   /* CHM-36: volume por pessoa no próprio menu do bloco (lembrado entre chamadas) */
                        float *vp = &g_tile_s->rtc_peers[t->slot].volume; if (!g_tile_s->rtc_peers[t->slot].vol_set) { g_tile_s->rtc_peers[t->slot].vol_set = true; *vp = 100.0f; }
                        cui_box(ui, .dir = CUI_ROW, .width = CUI_PX(220), .gap = 8, .pad_x = 10, .pad_y = 4, .align = CUI_ALIGN_CENTER) {
                            const float before = *vp; cui_icon(ui, CUI_ICON_VOLUME_2, 14, B_MUTED);
                            cui_slider(ui, "##tvol", vp, 0, 200, .step = 5, .width = CUI_FILL);
                            if (fabsf(before - *vp) > 0.01f) rtc_set_peer_volume(g_tile_s, t->slot, *vp / 100.0f);
                            char pct[12]; snprintf(pct, sizeof pct, "%d%%", (int)(*vp + 0.5f)); cui_text(ui, pct, .size = 11, .color = B_MUTED, .nowrap = true);
                        }
                    }
                    /* só o dono, só no servidor de chamadas: fecha (ou pede para reabrir) o microfone da pessoa para todos */
                    if (g_can_room_mute && t->can_kick && cui_menu_item(ui, t->muted ? "Reabrir o microfone dele" : "Silenciar para todos", .icon = CUI_ICON_VOLUME_X)) act = TILE_ROOM_MUTE;
                    if (cui_menu_item(ui, "Destacar (modo Foco)", .icon = CUI_ICON_MAXIMIZE_2)) act = TILE_FOCUS;
                    if (t->live) { if (cui_menu_item(ui, "Ver a transmissão", .icon = CUI_ICON_MONITOR)) act = TILE_WATCH; }
                    else if (cui_menu_item(ui, "Pedir transmissão", .icon = CUI_ICON_MONITOR)) act = TILE_ASK;
                    if (t->can_kick) {
                        cui_menu_separator(ui);
                        if (cui_menu_item(ui, "Expulsar da chamada…", .icon = CUI_ICON_PHONE, .danger = true)) act = TILE_KICK;
                    }
                }
                if (act != TILE_NONE) cui_popover_close(ui, "pmenu");
            }
            /* clique no corpo do bloco (fora do rótulo): destaca no modo Foco */
            if (act == TILE_NONE && !briks_blocked() && !briks_covered(ui) && cui_mouse_clicked(ui, CUI_MOUSE_LEFT) && in_rect(tr, mp) && !in_rect(lr, mp)) act = TILE_FOCUS;
        }
    }
    return act;
}

/* brilho que atravessa o botão (modo Call com alguém transmitindo): faixa clara de bordas suaves, de um lado ao outro; o pai recorta nos cantos arredondados */
static float g_sheen_phase = -1;   /* 0..1 durante a passada; < 0 = parado */
static bool g_tab_sheen;           /* a próxima aba desenhada recebe o brilho */
static void paint_sheen(cui_ctx *ui, cui_rect r, void *user) {
    const float p = *(const float *)user; if (p < 0 || p > 1) return;
    const float bw = r.w * 0.42f, cx = r.x - bw * 0.5f + (r.w + bw) * p;
    for (int k = 0; k < 3; k++) {   /* três cápsulas concêntricas, cada vez mais estreitas e mais claras = borda suave, sem listras; recortadas aos limites do botão */
        const float w = bw * (1.0f - 0.3f * (float)k), x0 = fmaxf(cx - w * 0.5f, r.x), x1 = fminf(cx + w * 0.5f, r.x + r.w);
        if (x1 - x0 < 2.0f) continue;
        cui_color c = CUI_RGB(0xffffff); c.a = 16;
        cui_draw_rect(ui, (cui_rect){ x0, r.y, x1 - x0, r.h }, CUI_CORNERS(fminf(9.0f, (x1 - x0) * 0.5f)), c);
    }
}

/* aba de um segmented control: ativa = pílula em degradê com texto branco; inativa = texto muted. compact = só o ícone (com dica) */
static bool tab(cui_ctx *ui, int id, const char *icon, const char *label, bool active, bool compact, int badge) {
    static float rad = 9;
    cui_push_id_int(ui, id);
    cui_box(ui, .id = "b", .dir = CUI_STACK, .height = CUI_PX(34), .radius = 9, .clip = true, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
        if (active) cui_canvas(ui, "tabbg", CUI_FILL, CUI_FILL, paint_grad, &rad, 0);
        const bool sheen = g_tab_sheen && g_sheen_phase >= 0; g_tab_sheen = false;
        cui_box(ui, .dir = CUI_ROW, .gap = 7, .pad_x = compact ? 11 : 16, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
            cui_color c = active ? CUI_RGB(0xffffff) : B_MUTED;
            if (icon) cui_icon(ui, icon, 15, c);
            if (!compact || !icon) cui_text(ui, label, .font = CUI_FONT_DISPLAY, .size = 15, .color = c, .nowrap = true);
            if (badge > 0) cui_box(ui, .min_width = 18, .height = CUI_PX(18), .pad_x = 5, .radius = 9, .bg = B_DANGER, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                char b[8]; snprintf(b, sizeof b, badge > 9 ? "9+" : "%d", badge);
                cui_text(ui, b, .size = 11, .color = CUI_RGB(0xffffff), .nowrap = true);
            }
        }
        if (sheen) { cui_push_id_int(ui, 100 + (int)(g_sheen_phase * 24.0f)); cui_canvas(ui, "sheen", CUI_FILL, CUI_FILL, paint_sheen, &g_sheen_phase, 0); cui_pop_id(ui); }   /* o canvas é guardado por id: muda a cada passo da passada */
    }
    if (compact && briks_hover(ui, "b")) briks_tip(label);
    bool hit = briks_clicked_r(ui, "b", 9.0f);
    cui_pop_id(ui);
    return hit;
}

/* pílula da barra: kind 0 normal, 1 ativo (verde), 2 perigo; label NULL = só o ícone */
static bool pill(cui_ctx *ui, int id, const char *icon, const char *label, int kind, const char *tip) {
    cui_push_id_int(ui, id);
    cui_color bg = kind == 2 ? CUI_RGB(0xe95555) : kind == 1 ? CUI_RGBA(0x57d28b26) : CUI_RGBA(0xffffff12);
    cui_color bd = kind == 2 ? CUI_RGB(0xe95555) : kind == 1 ? CUI_RGBA(0x57d28b88) : B_GLASS_BORDER;
    cui_color fg = kind == 2 ? CUI_RGB(0xffffff) : kind == 1 ? CUI_RGB(0x7be8a8) : B_TEXT;
    cui_box(ui, .id = "b", .dir = CUI_ROW, .gap = 8, .pad_x = label ? 18 : 0, .width = label ? CUI_FIT : CUI_PX(50), .height = CUI_PX(50), .radius = 25, .bg = bg, .border = 1, .border_color = bd,
            .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
        briks_icon(ui, icon, 18, fg);
        if (label) cui_text(ui, label, .font = CUI_FONT_DISPLAY, .size = 15, .color = fg, .nowrap = true);
    }
    if (tip && briks_hover(ui, "b")) briks_tip(tip);
    bool hit = briks_clicked_r(ui, "b", 25.0f);   /* o realce segue a pílula/círculo de 50 px (raio 25), sem cantos sobrando */
    cui_pop_id(ui);
    return hit;
}

/* botão quadrado de ícone do cabeçalho */
static bool head_icon(cui_ctx *ui, int id, const char *icon, const char *tip) {
    cui_push_id_int(ui, id);
    cui_box(ui, .id = "b", .width = CUI_PX(38), .height = CUI_PX(38), .radius = 12, .bg = B_GLASS, .border = 1,
            .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
        cui_icon(ui, icon, 16, B_TEXT);
    }
    if (briks_hover(ui, "b")) briks_tip(tip);
    bool hit = briks_clicked(ui, "b");
    cui_pop_id(ui);
    return hit;
}

/* 1.4.1: "Menu" com seta: volta ao menu SEM sair da chamada. Rotulado (antes era só um ícone ambíguo) e com alvo de 38 px de altura; em janela estreita fica só a seta. */
static bool back_btn(cui_ctx *ui, bool compact) {
    cui_push_id_int(ui, 902);
    cui_box(ui, .id = "b", .dir = CUI_ROW, .gap = 7, .height = CUI_PX(38), .min_width = 38, .pad_x = compact ? 11 : 14, .radius = 13, .bg = B_GLASS, .border = 1,
            .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
        cui_icon(ui, CUI_ICON_ARROW_LEFT, 16, B_TEXT);
        if (!compact) cui_text(ui, "Menu", .font = CUI_FONT_DISPLAY, .size = 15, .nowrap = true);
    }
    if (briks_hover(ui, "b")) briks_tip("Voltar ao menu — a chamada continua (use \"Abrir chamada\" ou a barra \"Em chamada\" para voltar)");
    const bool hit = briks_clicked_r(ui, "b", 13.0f);
    cui_pop_id(ui);
    return hit;
}

static void empty_state(cui_ctx *ui, const char *icon, const char *title, const char *sub) {
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .gap = 10, .pad = 20) {
        cui_box(ui, .width = CUI_PX(60), .height = CUI_PX(60), .radius = 30, .bg = CUI_RGBA(0xffffff10), .border = 1, .border_color = CUI_RGBA(0xffffff1c),
                .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) { cui_icon(ui, icon, 26, B_C3); }
        cui_text(ui, title, .font = CUI_FONT_DISPLAY, .size = 20, .nowrap = true);
        cui_box(ui, .max_width = 420, .align = CUI_ALIGN_CENTER) { cui_text(ui, sub, .size = 14, .color = B_MUTED); }
    }
}

/* ---------- indicador de rede (cabeçalho) ---------- */
static void paint_dot(cui_ctx *ui, cui_rect r, void *user) { cui_draw_rect(ui, r, CUI_CORNERS(r.w * 0.5f), *(const cui_color *)user); }
static cui_color netq_color(int q) { return q == NETQ_GOOD ? B_ONLINE : q == NETQ_UNSTABLE ? CUI_RGB(0xf5b942) : q == NETQ_DOWN ? B_DANGER : B_MUTED; }
static void info_row(cui_ctx *ui, const char *k, const char *v) {
    cui_row(ui, .gap = 10, .width = CUI_FILL) { cui_text(ui, k, .size = 13, .color = B_MUTED, .width = CUI_FILL, .nowrap = true); cui_text(ui, v, .size = 13, .nowrap = true); }
}
/* modo em uso na chamada (vem do net.mode do core): "SFU", "P2P" ou "" quando ainda não se sabe */
static const char *net_mode_short(const app_state_t *s) { return !s->in_room || !s->net_note[0] ? "" : s->net_sfu ? "SFU" : s->net_failed ? "" : "P2P"; }
static void net_chip(cui_ctx *ui, app_state_t *s, bool compact) {
    netdiag_t nd = netdiag_get();
    int failed = 0; for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (s->rtc_peers[i].id[0] && !strcmp(s->rtc_peers[i].state, "falhou")) failed++;
    const netq_in_t in = { s->in_room ? (s->rtc_state[0] ? s->rtc_state : (getenv("BRIKS_DEMO_PEERS") ? "conectado" : "conectando")) : "", s->rtc_peer_count, s->rtc_authed_count, failed,
                           nd.done, nd.reachable, nd.st.avg_ms, nd.st.loss_pct };
    const int q = call_net_quality(&in);
    cui_box(ui, .id = "netchip", .dir = CUI_ROW, .gap = 8, .pad_x = compact ? 0 : 12, .width = compact ? CUI_PX(38) : CUI_FIT, .height = CUI_PX(38), .radius = 12, .bg = B_GLASS, .border = 1, .border_color = B_GLASS_BORDER,
            .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
        { cui_color dc = netq_color(q); cui_canvas(ui, "netdot", CUI_PX(8), CUI_PX(8), paint_dot, &dc, sizeof dc); }   /* canvas: é um componente, então serve de âncora ao popover (caixa simples não serve) */
        if (!compact) { char lb[64]; const char *m = net_mode_short(s); snprintf(lb, sizeof lb, m[0] ? "%s · %s" : "%s", call_net_label(q), m); cui_text(ui, lb, .size = 13, .color = B_TEXT, .nowrap = true); }
    }
    if (briks_hover(ui, "netchip")) briks_tip(compact ? call_net_label(q) : "Ver o diagnóstico da conexão");
    if (briks_clicked(ui, "netchip")) cui_popover_open(ui, "netpop");
    cui_popover(ui, "netpop", .placement = CUI_PLACE_BOTTOM_START, .width = 340, .pad = 16, .gap = 9) {
        cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_box(ui, .width = CUI_PX(9), .height = CUI_PX(9), .radius = 5, .bg = netq_color(q)) {}
            cui_text(ui, call_net_label(q), .font = CUI_FONT_DISPLAY, .size = 16);
        }
        cui_text(ui, q == NETQ_GOOD ? "Você está conectado a todos da sala." : q == NETQ_UNSTABLE ? "A conexão com alguém da sala ou com a internet está falhando." :
                     q == NETQ_DOWN ? "Sem ligação com a sala. O Briks tenta de novo sozinho." : q == NETQ_CONNECTING ? "Ligando você à sala e às pessoas." : "Entre numa sala para ver a conexão.", .size = 13, .color = B_MUTED);
        cui_separator(ui);
        char b[96];
        info_row(ui, "Sinalização", s->in_room ? (s->rtc_state[0] ? s->rtc_state : "conectando") : "fora da sala");
        snprintf(b, sizeof b, "%d (autenticadas: %d)", s->rtc_peer_count, s->rtc_authed_count); info_row(ui, "Pessoas na sala", b);
        if (failed) { snprintf(b, sizeof b, "%d sem conexão direta", failed); info_row(ui, "Falhas", b); }
        { static const char *const CMN[] = { "Automático", "Sempre P2P", "Sempre SFU" }; const int cm = settings_call_mode();
          info_row(ui, "Modo escolhido", CMN[cm < 0 || cm > 2 ? 0 : cm]);
          info_row(ui, "Modo em uso", s->net_failed ? "nenhum (erro)" : net_mode_short(s)[0] ? (s->net_sfu ? "SFU (servidor)" : "P2P (direto)") : "—"); }
        info_row(ui, "Transporte", s->ice_note[0] ? (strstr(s->ice_note, "TURN") && !strstr(s->ice_note, "Somente") ? "STUN + TURN" : "Somente STUN") : "STUN público");
        if (s->rtc_msg[0]) cui_text(ui, s->rtc_msg, .size = 12, .color = B_MUTED);
        cui_separator(ui);
        if (nd.done) {
            if (!nd.reachable) cui_text(ui, "O teste não teve resposta: a rede pode bloquear UDP.", .size = 12.5f, .color = B_DANGER);
            else { snprintf(b, sizeof b, "%.0f ms", nd.st.avg_ms); info_row(ui, "Atraso até o STUN", b); snprintf(b, sizeof b, "%.0f ms", nd.st.jitter_ms); info_row(ui, "Variação", b); snprintf(b, sizeof b, "%.0f%%", nd.st.loss_pct); info_row(ui, "Perda", b); }
        } else cui_text(ui, "O teste mede atraso, variação e perda até o servidor STUN (uns 3 segundos).", .size = 12.5f, .color = B_MUTED);
        if (cui_button(ui, nd.running ? "Testando…" : nd.done ? "Testar de novo" : "Testar conexão", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM, .loading = nd.running)) netdiag_start();
        if (nd.running) cui_request_wakeup(ui, 0.2f);
    }
}

/* ---------- ajustes de áudio na chamada (popover da pílula Áudio) ---------- */
static void audio_popover(cui_ctx *ui, app_state_t *s) {
    g_aud_open = false;
    cui_popover(ui, "audpop", .placement = CUI_PLACE_TOP, .width = 340, .pad = 16, .gap = 9) {
        g_aud_open = true;
        float in, out, gdb; bool gate; settings_audio(&in, &out, &gate, &gdb);
        float in_v = in * 100.0f, out_v = out * 100.0f; bool g2 = gate; float gdb2 = gdb;
        cui_text(ui, "Áudio da chamada", .font = CUI_FONT_DISPLAY, .size = 16);
        cui_text(ui, "Microfone", .size = 12.5f, .color = B_MUTED);
        cui_progress(ui, s->mic_on ? s->me_level : 0.0f);
        cui_text(ui, s->mic_on ? "A barra mostra o que a sala está ouvindo de você." : "Seu microfone está desligado.", .size = 12, .color = B_MUTED);
        cui_row(ui, .gap = 10, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) { cui_text(ui, "Volume de entrada", .size = 13, .width = CUI_FILL); char p[12]; snprintf(p, sizeof p, "%d%%", (int)(in_v + 0.5f)); cui_text(ui, p, .size = 12, .color = B_MUTED); }
        cui_slider(ui, "##call-in", &in_v, 0, 100, .width = CUI_FILL);
        cui_row(ui, .gap = 10, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) { cui_text(ui, "Volume de saída", .size = 13, .width = CUI_FILL); char p[12]; snprintf(p, sizeof p, "%d%%", (int)(out_v + 0.5f)); cui_text(ui, p, .size = 12, .color = B_MUTED); }
        cui_slider(ui, "##call-out", &out_v, 0, 100, .width = CUI_FILL);
        cui_separator(ui);
        cui_switch(ui, "Porta de ruído", &g2);
        if (g2) { cui_textf(ui, "Corta o microfone abaixo de %d dB", (int)gdb2); cui_slider(ui, "##call-gate", &gdb2, -70, -20, .width = CUI_FILL); }
        else cui_text(ui, "Silencia o microfone quando só há ruído baixo (teclado, ventilador).", .size = 12, .color = B_MUTED);
        cui_text(ui, "Vale na hora e fica salvo. Supressão de ruído e cancelamento de eco ainda não têm efeito nesta versão.", .size = 12, .color = g_th.ph);
        if (fabsf(in_v - in * 100.0f) > 0.01f || fabsf(out_v - out * 100.0f) > 0.01f || g2 != gate || fabsf(gdb2 - gdb) > 0.01f) settings_audio_set(in_v, out_v, g2, gdb2);
    }
}

/* ---------- aviso "pediu para você transmitir" e retorno curto ---------- */
void call_banner(cui_ctx *ui, app_state_t *s) {
    const double now = cui_time(ui);
    if (g_call.req.open) {
        if (g_call.req.at < 0) g_call.req.at = now;
        if (!s->in_room && !getenv("BRIKS_DEMO_PEERS")) g_call.req.open = false;
        else if (s->sh.active || s->sh.starting) {   /* já estou transmitindo: responde sozinho */
            rtc_stream_reply(g_call.req.peer_id, true); g_call.req.open = false;
        } else if (now - g_call.req.at > 60.0) { rtc_stream_reply(g_call.req.peer_id, false); g_call.req.open = false; }   /* sem resposta em 1 min: conta como "agora não" */
        else cui_request_wakeup(ui, 1.0f);
    }
    if (g_call.toast[0]) {
        if (g_call.toast_until < 0) g_call.toast_until = now + 4.0;
        if (now > g_call.toast_until) g_call.toast[0] = 0; else cui_request_wakeup(ui, 0.5f);
    }
    const bool share_note = share_notice_active(ui, s);
    const bool mic_note = s->in_room && rtc_mic_problem(s);   /* aviso fixo enquanto o microfone não funciona (o toast some em 4 s) */
    if (!g_call.req.open && !g_call.toast[0] && !share_note && !mic_note) return;
    cui_push_id_int(ui, 9400);
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_START, .pad = 14, .pad_top = s->screen == SCREEN_CALL && !g_call.fullscreen ? 84 : 14, .gap = 8) {
        if (g_call.req.open) {
            cui_box(ui, .id = "reqbox", .dir = CUI_ROW, .gap = 12, .pad_x = 16, .pad_y = 10, .radius = 18, .bg = B_GLASS_POPUP, .border = 1, .border_color = B_A(B_C3, 0x77),
                    .elevation = briks_elev(CUI_ELEVATION_RAISED), .align = CUI_ALIGN_CENTER, .max_width = 720, .wrap = true, .justify = CUI_JUSTIFY_CENTER) {
                cui_box(ui, .width = CUI_PX(34), .height = CUI_PX(34), .radius = 17, .bg = B_A(B_C3, 0x38), .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) { cui_icon(ui, CUI_ICON_MONITOR, 16, B_TEXT); }
                char t[140]; snprintf(t, sizeof t, "%s pediu para você transmitir a tela", g_call.req.name);
                cui_text(ui, t, .font = CUI_FONT_MEDIUM, .size = 14, .nowrap = true);
                cui_row(ui, .gap = 8) {
                    if (cui_button(ui, "Transmitir", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM)) { rtc_stream_reply(g_call.req.peer_id, true); g_call.req.open = false; s->screen = SCREEN_CALL; share_open_dialog(s); }
                    if (cui_button(ui, "Agora não", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) { rtc_stream_reply(g_call.req.peer_id, false); g_call.req.open = false; }
                }
            }
            { cui_rect r = cui_get_rect(ui, cui_make_id(ui, "reqbox")); if (r.w > 0) briks_cover(r.x, r.y, r.w, r.h); }   /* o clique no aviso não vale para o bloco que está por baixo */
        }
        if (share_note) ui_share_notice_row(ui, s);
        if (mic_note) {
            cui_box(ui, .dir = CUI_ROW, .gap = 10, .pad_x = 16, .pad_y = 9, .radius = 16, .bg = B_GLASS_POPUP, .border = 1, .border_color = B_A(B_DANGER, 0x99), .align = CUI_ALIGN_CENTER, .max_width = 640, .wrap = true, .justify = CUI_JUSTIFY_CENTER) {
                cui_icon(ui, CUI_ICON_MIC, 14, B_DANGER);
                cui_text(ui, !strcmp(s->mic_state, "denied") ? "O Windows bloqueou o microfone: ninguém ouve você." : !strcmp(s->mic_state, "no_device") ? "Nenhum microfone encontrado: ninguém ouve você." : !strcmp(s->mic_state, "busy") ? "Outro aplicativo está usando o microfone: ninguém ouve você." : "O microfone não abriu: ninguém ouve você.", .size = 13, .nowrap = true);
                if (!strcmp(s->mic_state, "denied") && cui_button(ui, "Abrir Configurações", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) rtc_open_mic_privacy();
            }
        }
        if (g_call.toast[0]) {
            cui_box(ui, .dir = CUI_ROW, .gap = 8, .pad_x = 16, .pad_y = 9, .radius = 16, .bg = B_GLASS_POPUP, .border = 1, .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER, .max_width = 560) {
                cui_icon(ui, CUI_ICON_INFO, 14, B_C3);
                cui_text(ui, g_call.toast, .size = 13, .nowrap = true);
            }
        }
    }
    cui_pop_id(ui);
}

/* ---------- palco: chat da sala ---------- */
static void stage_chat(cui_ctx *ui, app_state_t *s) {
    static int seen = -1;
    const bool can_send = s->in_room || getenv("BRIKS_DEMO") != NULL;
    g_call.unread = 0;
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .radius = 20, .bg = B_GLASS, .border = 1, .border_color = B_GLASS_BORDER_SOFT, .clip = true, .gap = 0) {
        if (!g_call.msg_count) empty_state(ui, CUI_ICON_MESSAGE_SQUARE, "Chat da sala", "As mensagens daqui vão para quem está na chamada agora e somem quando você sai.");
        else cui_scroll(ui, .id = "callchat-scroll", .width = CUI_FILL, .height = CUI_FILL, .pad = g_th.chat_pad) {
            cui_column(ui, .gap = 10, .width = CUI_FILL) {
                for (int i = 0; i < g_call.msg_count; i++) {
                    const msg_t *m = &g_call.msgs[i];
                    const bool grouped = i > 0 && g_call.msgs[i - 1].mine == m->mine && !strcmp(g_call.msgs[i - 1].author, m->author);
                    cui_push_id_int(ui, 5200 + i);
                    briks_bubble(ui, s, m, false, grouped);
                    cui_pop_id(ui);
                }
            }
        }
        if (seen != g_call.msg_count) { seen = g_call.msg_count; if (g_call.msg_count) cui_scroll_to(ui, "callchat-scroll", 1.0e7f); cui_request_redraw(ui); }   /* mensagem nova: desce até ela */
        cui_box(ui, .width = CUI_FILL, .pad = 12, .pad_top = 0) {
            cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad = 7, .pad_left = 14, .gap = 6, .radius = 18, .bg = B_GLASS_CARD,
                    .border = 1, .border_color = can_send ? B_A(B_C3, 0x55) : B_GLASS_BORDER, .align = CUI_ALIGN_CENTER) {
                int r = cui_input(ui, "call-draft", g_call.draft, sizeof g_call.draft, .placeholder = can_send ? "Escreva para a sala" : "Entre numa sala para escrever", .width = CUI_FILL, .disabled = !can_send);
                bool send = cui_icon_button(ui, CUI_ICON_SEND, .variant = CUI_BUTTON_PRIMARY, .tooltip = "Enviar", .disabled = !can_send);
                if (can_send && (send || (r & CUI_SUBMITTED)) && g_call.draft[0]) call_chat_send(s);
            }
        }
    }
}

/* ---------- palco: participantes (grade ou foco) ---------- */
static void do_tile(cui_ctx *ui, app_state_t *s, const tile_t *t, float w, float h, bool mic_eff) {
    cui_push_id_int(ui, 700 + t->slot); g_tile_s = s;
    const int act = tile(ui, t, w, h, t->me ? mic_eff : true);
    const int i = t->slot;
    if (act == TILE_FOCUS) {
        if (g_call.layout == 1 && g_call.focus == i) g_call.focus = -1;   /* clicar de novo em quem está fixado: volta a seguir quem fala */
        else { g_call.focus = i; g_call.layout = 1; }
    } else if (i >= 0 && i < BRIKS_MAX_PEERS) {
        if (act == TILE_MUTE) rtc_mute_peer(s, i, !s->rtc_peers[i].muted_by_me);
        else if (act == TILE_ROOM_MUTE) rtc_room_mute(s, i, !s->rtc_peers[i].mic_off);
        else if (act == TILE_KICK) { s->kick_slot = i; s->kick_open = true; }
        else if (act == TILE_WATCH) s->call_mode = 1;
        else if (act == TILE_ASK) {
            rtc_stream_request(s, i);
            char m[160]; snprintf(m, sizeof m, "Pedido de transmissão enviado a %s.", s->rtc_peers[i].name[0] ? s->rtc_peers[i].name : "essa pessoa"); call_toast(m);
        }
    }
    cui_pop_id(ui);
}

static void stage_people(cui_ctx *ui, app_state_t *s, float W, float H) {
    cui_texture vt = 0; int vw = 0, vh = 0;
    const bool mic_eff = s->mic_on;   /* ensurdecer e mutar são independentes (como no Electron) */
    g_tile_me_deaf = s->deaf;
    tile_t T[BRIKS_MAX_PEERS + 2]; char names[BRIKS_MAX_PEERS][64]; int n = 0;
    g_can_room_mute = s->rtc_owner_me && s->net_sfu;
    T[n++] = (tile_t){ s->user_name[0] ? s->user_name : "Você", true, false, mic_eff ? s->me_level : 0, SLOT_ME, false, false, s->sh.active, s->sh.active ? "Transmitindo a tela" : s->user_status, NULL, 0, 0, 0 };
    if (cam_frame(ui, s, SLOT_ME, &vt, &vw, &vh)) { T[0].vtex = vt; T[0].vw = vw; T[0].vh = vh; }
    if (!s->in_room && getenv("BRIKS_DEMO") && !getenv("BRIKS_DEMO_PEERS")) T[n++] = (tile_t){ "Marina Costa", false, false, 0, BRIKS_MAX_PEERS, false, false, false, "", NULL };   /* participante de exemplo: só nas capturas de demonstração */
    for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (s->rtc_peers[i].id[0]) {
        if (s->rtc_peers[i].name[0]) snprintf(names[i], sizeof names[i], "%s", s->rtc_peers[i].name); else snprintf(names[i], sizeof names[i], "Participante %d", i + 1);
        const bool live = share_peer_live(s, i);
        T[n++] = (tile_t){ names[i], false, s->rtc_peers[i].mic_off, s->rtc_peers[i].muted_by_me ? 0 : s->rtc_peers[i].level, i, s->rtc_peers[i].muted_by_me,
                           s->rtc_owner_me && s->rtc_peers[i].fp[0] != 0, live, live ? "Transmitindo a tela" : s->rtc_peers[i].status, NULL, 0, 0, 0, s->rtc_peers[i].deafened };
        if (cam_frame(ui, s, i, &vt, &vw, &vh)) { T[n - 1].vtex = vt; T[n - 1].vw = vw; T[n - 1].vh = vh; }
    }
    const float gap = 14.0f;
    if (g_call.layout == 1 && n > 1 && (W >= 720.0f ? H >= 150.0f : H >= 260.0f)) {   /* palco baixo demais para um bloco grande + faixa: fica a grade */
        /* Foco: um bloco grande (fixado por clique ou quem está falando) e os demais numa faixa */
        static int auto_slot = -1;
        int mi = -1; float best = 0.02f;
        for (int i = 0; i < n; i++) if (T[i].slot == g_call.focus) mi = i;
        const bool pinned = mi >= 0;
        if (!pinned) {
            if (g_call.focus != -1) g_call.focus = -1;   /* quem estava fixado saiu */
            for (int i = 0; i < n; i++) if (!T[i].me && T[i].level > best) { best = T[i].level; auto_slot = T[i].slot; }
            for (int i = 0; i < n; i++) if (T[i].slot == auto_slot) mi = i;
            if (mi < 0) { mi = n > 1 ? 1 : 0; auto_slot = T[mi].slot; }
        }
        T[mi].badge = pinned ? "Fixado · clique para soltar" : "Em foco · quem está falando";
        const int k = n - 1;
        const bool side = W >= 720.0f;
        cui_box(ui, .dir = side ? CUI_ROW : CUI_COLUMN, .width = CUI_FILL, .height = CUI_FILL, .gap = gap) {
            if (side) {
                const float sw = fminf(fmaxf(W * 0.22f, 150.0f), 250.0f), th = fminf(fmaxf((H - gap * (float)(k - 1)) / (float)k, 84.0f), sw * 0.72f);
                do_tile(ui, s, &T[mi], W - gap - sw, H, mic_eff);
                const bool over = th * (float)k + gap * (float)(k - 1) > H + 0.5f;
                if (over) { cui_scroll(ui, .id = "focus-strip", .width = CUI_PX(sw), .height = CUI_PX(H)) { cui_column(ui, .gap = gap) { for (int i = 0; i < n; i++) if (i != mi) do_tile(ui, s, &T[i], sw - 10.0f, th, mic_eff); } } }
                else cui_column(ui, .gap = gap, .width = CUI_PX(sw), .height = CUI_PX(H), .justify = CUI_JUSTIFY_CENTER) { for (int i = 0; i < n; i++) if (i != mi) do_tile(ui, s, &T[i], sw, th, mic_eff); }
            } else {
                const float sh = 104.0f, tw = fminf(fmaxf((W - gap * (float)(k - 1)) / (float)k, 96.0f), 170.0f);
                do_tile(ui, s, &T[mi], W, H - gap - sh, mic_eff);
                const bool over = tw * (float)k + gap * (float)(k - 1) > W + 0.5f;
                if (over) { cui_box(ui, .id = "focus-strip-h", .dir = CUI_ROW, .width = CUI_PX(W), .height = CUI_PX(sh), .gap = gap, .scroll_x = true) { for (int i = 0; i < n; i++) if (i != mi) do_tile(ui, s, &T[i], tw, sh - 8.0f, mic_eff); } }
                else cui_row(ui, .gap = gap, .width = CUI_PX(W), .height = CUI_PX(sh), .justify = CUI_JUSTIFY_CENTER) { for (int i = 0; i < n; i++) if (i != mi) do_tile(ui, s, &T[i], tw, sh, mic_eff); }
            }
        }
        return;
    }
    /* Grade: blocos iguais; a última linha fica centralizada */
    int cols, rows; call_grid(n, W, H, gap, &cols, &rows);
    const float tw = floorf((W - gap * (float)(cols - 1)) / (float)cols), th = floorf((H - gap * (float)(rows - 1)) / (float)rows);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = gap, .justify = CUI_JUSTIFY_CENTER) {
        for (int r = 0; r < rows; r++) {
            cui_push_id_int(ui, 760 + r);
            cui_row(ui, .width = CUI_FILL, .gap = gap, .justify = CUI_JUSTIFY_CENTER) {
                for (int c = 0; c < cols && r * cols + c < n; c++) do_tile(ui, s, &T[r * cols + c], tw, th, mic_eff);
            }
            cui_pop_id(ui);
        }
    }
}

/* ---------- barra de controles ---------- */
static void control_bar(cui_ctx *ui, app_state_t *s, bool compact) {
    if (g_call.fullscreen) { cui_rect r = cui_get_rect(ui, cui_make_id(ui, "ctlbar")); if (r.w > 0) briks_cover(r.x, r.y, r.w, r.h); }   /* flutuando sobre o palco: o clique na barra não destaca o bloco de baixo */
    /* janela estreita ou texto grande: os controles quebram em duas linhas em vez de passar da largura */
    const float vw = cui_viewport_size(ui).x;
    const bool own = g_call.fullscreen; if (own) g_cov_own++;   /* a barra cobre o palco, mas os botões DELA continuam valendo (antes a cobertura matava os próprios botões em tela cheia) */
    cui_box(ui, .id = "ctlbar", .dir = CUI_ROW, .gap = compact ? 8 : 10, .pad = compact ? 10 : 14, .radius = compact ? 30 : 39, .bg = B_GLASS_STRONG, .border = 1, .border_color = B_GLASS_BORDER,
            .elevation = briks_elev(CUI_ELEVATION_RAISED), .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .wrap = true, .max_width = vw - 16.0f) {
        const bool mic_eff = s->mic_on;
        if (pill(ui, 1, mic_eff ? CUI_ICON_MIC : BRIKS_ICON_MIC_OFF, compact ? NULL : "Microfone", mic_eff ? 1 : 2, mic_eff ? "Desligar o microfone" : "Ligar o microfone")) {   /* desligado: vermelho e riscado */
            s->mic_on = !s->mic_on; rtc_set_mic(s, s->mic_on);   /* o microfone não mexe no ensurdecer */
        }
        /* como o microfone, nunca neutro: verde ouvindo, vermelho ensurdecido (ícone com risco) */
        if (pill(ui, 2, s->deaf ? CUI_ICON_VOLUME_X : CUI_ICON_VOLUME_2, compact ? NULL : "Ensurdecer", s->deaf ? 2 : 1, s->deaf ? "Voltar a ouvir" : "Parar de ouvir a sala")) { s->deaf = !s->deaf; rtc_set_deaf(s, s->deaf); }
        /* Câmera: liga/desliga de verdade (o estado vem do core); só abre o dispositivo enquanto ligada */
        {
            const bool cam_on = cam_active(), cam_wait = cam_starting();
            if (pill(ui, 3, CUI_ICON_VIDEO, compact ? NULL : cam_wait ? "Câmera…" : "Câmera", cam_on ? 1 : 0, cam_on ? "Desligar a câmera" : "Ligar a câmera: o vídeo vai para quem está na chamada")) cam_toggle(s);
        }
        ui_share_bar(ui, s);
        cui_box(ui, .dir = CUI_STACK, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_START) {
            const bool open_aud = pill(ui, 6, CUI_ICON_SLIDERS_HORIZONTAL, compact ? NULL : "Áudio", g_aud_open ? 1 : 0, "Volume e porta de ruído, sem sair da chamada");
            cui_canvas(ui, "audanchor", CUI_FILL, CUI_PX(1), NULL, NULL, 0);   /* âncora do popover (um componente de verdade): topo da pílula, com a largura dela */
            if (open_aud) cui_popover_open(ui, "audpop");
            audio_popover(ui, s);
        }
        if (g_call.fullscreen && pill(ui, 7, CUI_ICON_MINIMIZE_2, NULL, 0, "Sair da tela cheia (Esc)")) call_set_fullscreen(s, false);
        if (pill(ui, 5, CUI_ICON_PHONE, compact ? NULL : "Sair", 2, "Sair da chamada")) { rtc_leave_room(s); s->screen = SCREEN_SOCIAL; }
    }
    if (own) g_cov_own--;
}

void ui_call(cui_ctx *ui, app_state_t *s) {
    share_tick(ui, s);
    cam_tick(ui, s);
    const cui_vec2 vp = cui_viewport_size(ui);
    const double now = cui_time(ui);
    const bool fs = g_call.fullscreen;
    const bool modal = s->sh.dialog_open || s->kick_open || briks_modal_open();
    /* teclado: F11 alterna a tela cheia; Esc sai dela (se não houver diálogo aberto, que usa o Esc para fechar) */
    if (cui_key_pressed(ui, CUI_KEY_F11)) call_set_fullscreen(s, !fs);
    else if (fs && !modal && cui_key_pressed(ui, CUI_KEY_ESCAPE)) call_set_fullscreen(s, false);
    /* em tela cheia a barra some depois de 3 s sem mexer o mouse */
    {
        const cui_vec2 m = cui_mouse_pos(ui);
        if (g_call.last_move < 0 || fabsf(m.x - g_call.mx) > 1.0f || fabsf(m.y - g_call.my) > 1.0f || cui_mouse_down(ui, CUI_MOUSE_LEFT)) { g_call.last_move = now; g_call.mx = m.x; g_call.my = m.y; }
        g_bar_visible = !g_call.fullscreen || now - g_call.last_move < 3.0 || g_aud_open || modal;
        if (g_call.fullscreen && g_bar_visible) cui_request_wakeup(ui, 0.5f);
    }
    char title[120], st[160]; call_room_title(s, title, sizeof title); rtc_status_text(s, st, sizeof st);
    /* os limites usam a largura "em texto": com o texto no Máximo (x1,4) os rótulos saem mais cedo */
    const float fscale = fmaxf(cui_theme_get(ui)->font_scale, 1.0f), ew = vp.x / fscale;
    const bool compact = ew < 1120.0f, tiny = ew < 740.0f, bar_compact = ew < 900.0f;   /* 1280 px com o texto padrão (x1,12) ainda mostra os rótulos */
    const cui_rect barr = cui_get_rect(ui, cui_make_id(ui, "ctlbar"));
    const float bar_h = fmaxf(104.0f, barr.h > 0 ? barr.h + 26.0f : 104.0f);   /* a barra pode ter duas linhas; ela não depende do palco, então medir é seguro */
    briks_ambient(ui, 0.85f);
    cui_box(ui, .id = "callroot", .dir = CUI_STACK, .width = CUI_FILL, .height = CUI_FILL) {
        cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 0) {
            if (!g_call.fullscreen) {
                /* cabeçalho */
                cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(70), .pad_x = tiny ? 12 : 20, .gap = 12, .bg = B_GLASS_SOFT,
                        .justify = CUI_JUSTIFY_BETWEEN, .align = CUI_ALIGN_CENTER, .clip = true) {
                    cui_row(ui, .gap = 14, .align = CUI_ALIGN_CENTER, .width = CUI_FILL, .min_width = 60, .clip = true) {
                        if (back_btn(ui, compact)) s->screen = SCREEN_SOCIAL;
                        if (ew >= 1360.0f) briks_brand(ui, 20);   /* o botão Menu já ocupa o canto: a marca só cabe em janela larga, senão corta o estado */
                        cui_column(ui, .gap = 1, .width = CUI_FILL, .clip = true) {
                            cui_text(ui, title, .font = CUI_FONT_DISPLAY, .size = 16, .nowrap = true);
                            cui_text(ui, st, .size = 13, .color = B_MUTED, .nowrap = true);
                        }
                    }
                    cui_row(ui, .gap = tiny ? 5 : 10, .align = CUI_ALIGN_CENTER) {
                        net_chip(ui, s, compact);
                        cui_box(ui, .dir = CUI_ROW, .gap = 4, .pad = 4, .radius = 13, .bg = B_GLASS, .border = 1, .border_color = B_GLASS_BORDER) {
                            /* ids 11..16: as pílulas da barra usam 1..7 com o mesmo nome de caixa; ids iguais faziam um clique valer nos dois lugares */
                            if (tab(ui, 11, CUI_ICON_PHONE, "Call", s->call_mode == 0, compact, 0)) s->call_mode = 0;
                            {   /* alguém transmite e eu estou no modo Call: um brilho passa pelo botão "Transmissão" a cada poucos segundos; para no modo Transmissão ou sem transmissão */
                                const bool nudge = s->call_mode == 0 && share_any_live(s); static int fast = -1; if (fast < 0) { const char *dv = getenv("BRIKS_DEV_SHARE"); fast = dv && !strcmp(dv, "demo-sheen"); }   /* captura de teste: passa quase sempre */
                                const double tn = cui_time(ui), per = fast ? 1.0 : 4.5, ph = fmod(tn, per);
                                g_sheen_phase = nudge && ph < 0.9 ? (float)(ph / 0.9) : -1.0f; g_sheen_seen += g_sheen_phase >= 0; g_tab_sheen = nudge;
                                if (nudge) cui_request_wakeup(ui, ph < 0.9 ? 0.03f : fminf((float)(per - ph), 0.5f));   /* redesenha só enquanto o brilho corre */
                            }
                            if (tab(ui, 12, CUI_ICON_MONITOR, "Transmissão", s->call_mode == 1, compact, 0)) s->call_mode = 1;
                        }
                        {   /* Grade/Foco vale nos dois modos (Call: participantes; Transmissão: as transmissões); só esmaece no Chat e clicar volta para a Chamada */
                            const bool here = s->call_view == 0;
                            cui_box(ui, .dir = CUI_ROW, .gap = 4, .pad = 4, .radius = 13, .bg = B_GLASS, .border = 1, .border_color = B_GLASS_BORDER, .opacity = here ? 0.0f : 0.5f) {
                                if (tab(ui, 15, CUI_ICON_GRID_3X3, "Grade", here && g_call.layout == 0, compact, 0)) { g_call.layout = 0; s->call_view = 0; }
                                if (tab(ui, 16, CUI_ICON_LAYOUT_DASHBOARD, "Foco", here && g_call.layout == 1, compact, 0)) { g_call.layout = 1; s->call_view = 0; }
                            }
                        }
                        cui_box(ui, .dir = CUI_ROW, .gap = 4, .pad = 4, .radius = 13, .bg = B_GLASS, .border = 1, .border_color = B_GLASS_BORDER) {
                            if (tab(ui, 13, CUI_ICON_VIDEO, "Chamada", s->call_view == 0, compact, 0)) s->call_view = 0;
                            if (tab(ui, 14, CUI_ICON_MESSAGE_SQUARE, "Chat", s->call_view == 1, compact, s->call_view == 1 ? 0 : g_call.unread)) s->call_view = 1;
                        }
                        if (head_icon(ui, 901, CUI_ICON_MAXIMIZE_2, "Tela cheia (F11)")) call_set_fullscreen(s, true);
                    }
                }
                cui_separator(ui);
            }
            /* palco */
            const float pad = g_call.fullscreen ? 12.0f : 18.0f;
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = pad, .gap = 10, .id = "stage") {
                if (s->net_failed) {
                    /* o servidor de chamadas não deixou entrar e nenhuma chamada abriu: diz por quê e oferece o P2P de propósito */
                    cui_box(ui, .dir = CUI_ROW, .gap = 12, .pad_x = 14, .pad_y = 10, .radius = 14, .bg = CUI_RGBA(0xf5a52422), .border = 1, .border_color = CUI_RGBA(0xf5a52466), .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
                        cui_icon(ui, CUI_ICON_PHONE, 18, CUI_RGB(0xf5a524));
                        cui_column(ui, .gap = 2, .width = CUI_FILL) {
                            cui_text(ui, s->rtc_msg[0] ? s->rtc_msg : "Não foi possível entrar pelo servidor de chamadas.", .size = 14);
                            cui_text(ui, "Você ainda não está na chamada. Em P2P você só fala com quem também entrar em P2P.", .size = 12, .color = B_MUTED);
                        }
                        if (cui_button(ui, "Tentar de novo")) { char room[sizeof s->room_name]; snprintf(room, sizeof room, "%s", s->room_name); s->net_failed = false; s->rtc_session_sent = s->rtc_session_ready; rtc_join_room(s, room); }
                        if (cui_button(ui, "Entrar em modo P2P", .variant = CUI_BUTTON_PRIMARY)) rtc_join_p2p(s);
                    }
                }
                if (s->in_room && s->rtc_overflow > 0 && s->call_view != 1) {
                    /* mais gente na chamada do que quadros na grade: estão ouvindo e falando; sobem quando abrir vaga */
                    cui_box(ui, .dir = CUI_ROW, .gap = 8, .pad_x = 12, .pad_y = 6, .radius = 12, .bg = B_GLASS, .border = 1, .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER) {
                        char pn[64]; snprintf(pn, sizeof pn, "+%d %s na chamada, fora da grade", s->rtc_overflow, s->rtc_overflow == 1 ? "pessoa" : "pessoas");
                        cui_text(ui, pn, .size = 13, .color = B_MUTED);
                    }
                }
                /* tamanho do palco a partir da janela (menos a faixa de 3 px do topo), nunca de uma caixa medida: os blocos têm tamanho fixo em px,
                   então medir o palco ou a raiz realimentava o tamanho antigo (ao sair da tela cheia a barra era empurrada para fora) */
                const float rw = vp.x, rh = vp.y - 4.0f - titlebar_height();   /* desconta a barra de título própria: sem isto o palco passava da base da janela e a barra de controles saía cortada */
                float W = rw - pad * 2, H = rh - (g_call.fullscreen ? 0.0f : 70.0f + 1.0f + bar_h) - pad * 2;
                if (s->net_failed) H -= 78.0f;                               /* faixa "não foi possível entrar pelo servidor" */
                if (s->in_room && s->rtc_overflow > 0) H -= 46.0f;           /* faixa "+N pessoas fora da grade" */
                if (W < 120) W = 120; if (H < 90) H = 90;
                if (s->call_view == 1) stage_chat(ui, s);
                else if (s->call_mode == 1) {
                    if (!ui_share_stage(ui, s, W, H, g_call.layout)) {
                        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .radius = 20, .bg = B_A(B_PANEL, 0x80), .border = 1, .border_color = B_A(B_C1, 0x55), .clip = true) {
                            empty_state(ui, CUI_ICON_MONITOR, "Ninguém transmitindo", "Quando alguém compartilhar a tela, ela aparece aqui. Para pedir, clique com o botão direito em alguém na aba Call.");
                        }
                    }
                } else {
                    stage_people(ui, s, W, H);
                }
            }
            /* barra de controles (fora da tela cheia ocupa a sua faixa; em tela cheia flutua sobre o palco) */
            if (!g_call.fullscreen) cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(bar_h), .justify = CUI_JUSTIFY_CENTER, .align = CUI_ALIGN_CENTER) { control_bar(ui, s, bar_compact); }
        }
        if (g_call.fullscreen && g_bar_visible) {
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .justify = CUI_JUSTIFY_END, .align = CUI_ALIGN_CENTER, .pad_bottom = 16) { control_bar(ui, s, bar_compact); }
        }
        call_banner(ui, s);
    }
    ui_share_dialog(ui, s);
    /* confirmação rápida antes de expulsar */
    if (s->kick_open) {
        const char *who = (s->kick_slot >= 0 && s->kick_slot < BRIKS_MAX_PEERS && s->rtc_peers[s->kick_slot].name[0]) ? s->rtc_peers[s->kick_slot].name : "esta pessoa";
        cui_dialog(ui, "Expulsar da chamada", &s->kick_open, .width = fminf(420.0f, fmaxf(vp.x - 48.0f, 260.0f))) {
            cui_textf(ui, "Expulsar %s da chamada?", who);
            cui_text(ui, "Ela será desconectada e não poderá voltar a esta sala nesta sessão até você permitir.", .size = 13, .color = B_MUTED);
            cui_row(ui, .gap = 10) {
                if (cui_button(ui, "Cancelar")) s->kick_open = false;
                if (cui_button(ui, "Expulsar", .variant = CUI_BUTTON_DANGER)) { rtc_kick_peer(s, s->kick_slot); s->kick_open = false; }
            }
        }
    }
    if (s->screen != SCREEN_CALL && g_call.fullscreen) call_set_fullscreen(s, false);   /* saiu da tela da chamada: a janela volta ao normal */
}

/* CHM-16: aviso nos 5 minutos finais antes de sair por inatividade (e o resultado depois que saiu) */
void idle_banner(cui_ctx *ui, app_state_t *s) {
    if (s->idle_phase_now != IDLE_WARNING) {
        if (s->idle_notice[0] && s->screen != SCREEN_CALL) cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_START, .pad_top = 8, .pad_x = 8) {
            cui_box(ui, .id = "idle-done", .dir = CUI_ROW, .pad_x = 14, .pad_y = 7, .gap = 10, .radius = 18, .bg = B_GLASS_CARD, .border = 1, .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, s->idle_notice, .size = 13, .nowrap = true);
                if (cui_button(ui, "Ok", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) s->idle_notice[0] = 0;
            }
        }
        return;
    }
    const long long sec = (s->idle_left_ms + 999) / 1000;
    char t[160]; snprintf(t, sizeof t, "Você está há muito tempo sem falar. A chamada será encerrada em %lld:%02lld.", sec / 60, sec % 60);
    cui_rect pr = cui_get_rect(ui, cui_make_id(ui, "idle-pill"));
    if (pr.w > 0) briks_cover(pr.x, pr.y, pr.w, pr.h);
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_START, .pad_top = 82, .pad_x = 8) {
        cui_box(ui, .id = "idle-pill", .dir = CUI_ROW, .pad_x = 14, .pad_y = 7, .gap = 10, .radius = 18, .bg = CUI_RGBA(0x4a3a10f4), .border = 1, .border_color = B_GLASS_BORDER,
                .align = CUI_ALIGN_CENTER, .elevation = briks_elev(CUI_ELEVATION_RAISED)) {
            cui_icon(ui, CUI_ICON_CLOCK, 16, CUI_RGB(0xffe6a8));
            cui_text(ui, t, .size = 13, .color = CUI_RGB(0xffe6a8), .nowrap = true);
            if (cui_button(ui, "Continuar", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM)) { s->idle_last = GetTickCount64(); s->idle_phase_now = IDLE_ACTIVE; }
        }
    }
}
