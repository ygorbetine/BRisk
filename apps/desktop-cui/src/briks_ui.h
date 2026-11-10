/* Remake da interface do Briks em CUI. Paleta e medidas vêm de apps/web/src/styles.css e glass-layout.css. */
#ifndef BRIKS_UI_H
#define BRIKS_UI_H
#include <cui/cui.h>
#include <cui/cui_app.h>
#include <stdbool.h>
#include <stddef.h>
#include "theme_cfg.h"
#include "perf.h"

/* Paleta (--briks-*, --glass-*) */
#define B_C1        CUI_RGB(0xb01fcf)
#define B_C2        CUI_RGB(0x6d5cf6)
#define B_C3        CUI_RGB(0x8b5cf6)
#define B_BG        CUI_RGB(0x12041a)
#define B_BG_DEEP   CUI_RGB(0x0f0316)
#define B_PANEL     CUI_RGB(0x1a0a26)
#define B_LINE      CUI_RGB(0x2f1745)
#define B_TEXT      CUI_RGB(0xedf1f7)
#define B_MUTED     CUI_RGB(0xa592b8)
#define B_DANGER    CUI_RGB(0xe95555)
#define B_ONLINE    CUI_RGB(0x57d28b)
#define B_GLASS_BORDER CUI_RGBA(0xffffff1c)  /* .11 */
#define B_GLASS_BORDER_SOFT CUI_RGBA(0xffffff12) /* .07 */

typedef enum { SCREEN_AUTH, SCREEN_SOCIAL, SCREEN_CALL, SCREEN_HISTORY } screen_t;

#define MAX_FRIENDS 128
#define MAX_COMMUNITIES 32
typedef struct { char id[48]; char name[96]; char request_id[48]; char status[64]; char presence[64]; } person_t;   /* status = texto do balão; presence = o que a pessoa está usando */
typedef struct { char id[48]; char name[96]; } community_t;

#define MAX_GROUPS 32
#define MAX_MSGS 200
typedef struct { char id[48]; char name[96]; char channel[64]; } group_t;
typedef struct { char id[48]; char author[96]; char content[512]; char created[40]; bool mine; } msg_t;

typedef enum { OP_NONE, OP_AUTH, OP_ME, OP_FRIENDS, OP_COMMUNITIES, OP_GROUPS, OP_ADD_FRIEND, OP_ACCEPT,
               OP_MSG_LOAD, OP_MSG_LOAD2, OP_MSG_SEND, OP_GROUP_SAVE } op_t;

/* Transmissão de tela (ui_share.c): fontes do seletor, áudio por app, qualidade e estado local/remoto */
#define SH_MAX_SRC 24
#define SH_MAX_APPS 16
typedef struct { char id[24]; char name[96]; char sub[64]; char thumb[280]; int w, h; cui_texture tex; bool tex_tried; } share_src_t;
typedef struct { unsigned pid; char name[64]; bool playing, on; } share_app_t;
typedef struct {
    bool dialog_open, loading, quality_open;
    int tab;                                   /* 0 telas, 1 janelas */
    int sel[2];                                /* fonte escolhida em cada aba */
    share_src_t src[2][SH_MAX_SRC]; int nsrc[2];
    int audio_mode;                            /* 0 da fonte, 1 escolher apps, 2 sem áudio */
    share_app_t apps[SH_MAX_APPS]; int napps;
    int res, fps, br_mode;                     /* índices: 720p/1080p/1440p/4K; 15/30/60; automático/alto/máximo/personalizado */
    float br_custom;                           /* Mbps (1..50) quando personalizado */
    int content;                               /* prioridade no aperto: 0 nitidez (cede FPS), 1 fluidez (cede resolução) */
    /* transmissão local */
    bool active, starting, audio_on, hardware;
    int w, h, cur_fps, kbps, cpu, audio_sources;
    float stat_fps, enc_ms, audio_peak; int stat_kbps;
    /* laço de adaptação ao vivo (share.stats) */
    int live_h, target_fps, target_kbps, est_kbps, rtt_ms, rtx; float loss_pct;
    char adapting[8], reason[12];           /* "" | "rede" | "cpu"; "perda" | "atraso" */
    char label[64], encoder[64], err[160], last_kind[12], last_id[24];
    /* transmissões dos outros (índice = slot em rtc_peers) */
    struct { bool on, audio, watching, has_frame; int w, h, fps, kbps; char label[64], path[300]; } remote[8];
} share_state_t;

typedef struct {
    screen_t screen;
    cui_app *app;
    /* conexão com o backend local */
    struct bk_call *call;
    op_t op;
    bool busy;
    char token[512];
    /* auth */
    int auth_mode;                 /* 0 login, 1 criar, 2 redefinir */
    char email[96], password[64], name[64], confirm[64], error[160];
    /* sessão */
    char user_id[48], user_name[96], user_email[96];
    char user_status[64], user_presence[64];   /* status (balão) e rich presence (programa em uso); status vazio = mostra a presença */
    bool rp_enabled; int rp_audience;           /* rich presence ligado; 0 = todos os amigos, 1 = só quem está na mesma chamada */
    person_t friends[MAX_FRIENDS]; int friend_count;
    person_t pending[MAX_FRIENDS]; int pending_count;
    community_t communities[MAX_COMMUNITIES]; int community_count;
    group_t groups[MAX_GROUPS]; int group_count;
    int active_group;              /* -1 = nenhum */
    msg_t msgs[MAX_MSGS]; int msg_count;
    char chat_channel[160];
    bool reload_msgs;
    msg_t msgs2[MAX_MSGS]; int msg2_count;   /* segunda conversa (painel chat2): só leitura */
    char chat2_channel[160]; bool reload_msgs2; int chat2_friend;
    bool create_group_open, profile_open;
    char new_group[96];
    /* social */
    int active_friend;             /* -1 = lista de amigos */
    int active_space;              /* 0 = início */
    char draft[256];
    bool settings_open, add_friend_open;
    char add_email[96], notice[160];
    /* call */
    bool mic_on, cam_on, sharing, deaf;
    int call_mode, call_view;      /* 0 Call/Transmissão; 0 Chamada/Chat */
    /* WebRTC nativo (briks-core): estado de conexão exibido na tela */
    char rtc_state[16];            /* "" | conectando | conectado | reconectando | erro | desconectado */
    char rtc_msg[160], rtc_fp[72], room_name[64];
    bool rtc_want_room, rtc_session_sent, rtc_session_ready, in_room;
    int rtc_peer_count, rtc_authed_count;
    struct { char id[48]; char state[16]; float level; bool mic_off, deafened, muted_by_me; char fp[72], name[64], status[64], presence[64]; } rtc_peers[8];
    float me_level;                /* nível de fala do microfone local (0..1), evento "speaking" do core */
    bool rtc_owner_me;             /* sou o dono da sala (evento room.owner): só o dono vê "Expulsar" */
    int kick_slot;                 /* peer pendente de confirmação de expulsão (-1 = nenhum) */
    bool kick_open, kicked_open;   /* diálogo de confirmação; aviso "Você foi removido da chamada" */
    share_state_t sh;
} app_state_t;

/* app.c: máquina de estados das chamadas ao backend */
void app_submit_auth(app_state_t *s);
void app_add_friend(app_state_t *s);
void app_accept_friend(app_state_t *s, const char *request_id);
void app_logout(app_state_t *s);
void app_begin(app_state_t *s, op_t op, const char *method, const char *path, const char *body);
void app_open_friend(app_state_t *s, int idx);   /* abre a DM e carrega o histórico persistido */
void app_open_group(app_state_t *s, int idx);
void app_open_chat2(app_state_t *s, int friend_idx);
void app_load_messages2(app_state_t *s, const char *body);
void app_send_message(app_state_t *s);
void app_create_group(app_state_t *s);
void app_load_messages(app_state_t *s, const char *body);
void app_load_groups(app_state_t *s, const char *body);
void app_demo(app_state_t *s, const char *mode);  /* BRIKS_DEMO: dados fixos para capturas */
void app_pump(app_state_t *s);
/* rtc.c: ponte com o briks-core */
void rtc_join_room(app_state_t *s, const char *room);   /* inicia o core se preciso, abre a sessão e entra na sala */
void rtc_leave_room(app_state_t *s);
void profile_tick(app_state_t *s);                              /* perfil local: presença do programa em uso, persistência (profile.c) */
void rtc_set_mic(app_state_t *s, bool on);
void rtc_set_deaf(app_state_t *s, bool on);
void rtc_kick_peer(app_state_t *s, int slot);               /* expulsa (só o dono): mensagem assinada pelo core */
void rtc_mute_peer(app_state_t *s, int slot, bool muted);   /* ganho zero só para essa pessoa */
void rtc_pump(app_state_t *s);                           /* a cada quadro: consome eventos do core */
const char *rtc_status_text(const app_state_t *s, char *buf, size_t cap);   /* "Conectado · 2 pessoas" etc. */
/* ui_share.c: transmissão de tela */
struct jval;
bool share_handle_event(app_state_t *s, const struct jval *j, const char *ev);   /* eventos share.* do core */
void share_tick(cui_ctx *ui, app_state_t *s);                 /* a cada quadro: assistir só no modo Transmissão, ganchos de teste */
void share_open_dialog(app_state_t *s);                       /* pede as fontes e os apps com áudio e abre o seletor */
void share_stop(app_state_t *s);
void share_reset(app_state_t *s);                             /* saiu da sala: esquece tudo */
void ui_share_dialog(cui_ctx *ui, app_state_t *s);            /* "Compartilhar tela — Escolha a fonte e confirme" */
void ui_share_bar(cui_ctx *ui, app_state_t *s);               /* botão Compartilhar + chips de áudio e qualidade */
bool ui_share_stage(cui_ctx *ui, app_state_t *s);             /* palco do modo Transmissão; false = ninguém transmitindo */
bool share_peer_live(const app_state_t *s, int slot);         /* esse participante está transmitindo? */
bool rtc_handle_event(app_state_t *s, const char *line);                    /* exposto para o smoke */                 /* chamar uma vez por quadro */

/* Pilha levantada: grupos 0..2 = cartões (atrás, meio, frente), 3 = âncora de captura do vidro (alfa 0). O dock desenha cada grupo sem giro entre
   marcadores PILE_MARK(g)...PILE_MARK_END e chama briks_pile_set antes do quadro; glass.c gira/escala/esmaece os vértices do grupo. */
#define PILE_GROUPS 4
#define PILE_MARK(g)  CUI_RGBA(0x5A17C100 | ((g) + 1))
#define PILE_MARK_END CUI_RGBA(0x5A17C1FF)
void briks_pile_set(int group, float pivot_x, float pivot_y, float angle_deg, float scale, float alpha);
void briks_pile_clear(void);
bool briks_glass_init(cui_app *app);        /* glass.c: desfoque real por trás dos painéis B_GLASS* */
void briks_glass_shutdown(void);
void briks_apply_theme(cui_ctx *ui);
void briks_ambient(cui_ctx *ui, float strength);          /* fundo ambiente das manchas de cor */
void briks_brand(cui_ctx *ui, float size);
void briks_gradient_bar(cui_ctx *ui);                     /* faixa de 3px do topo */
void ui_auth(cui_ctx *ui, app_state_t *s);
void ui_social(cui_ctx *ui, app_state_t *s);
void ui_call(cui_ctx *ui, app_state_t *s);
void ui_history(cui_ctx *ui, app_state_t *s);
void ui_panel(cui_ctx *ui, app_state_t *s, int kind);   /* ui_social.c: conteúdo de um painel do dock */
void dock_render(cui_ctx *ui, app_state_t *s);
bool dock_script_point(const char *spec, float *x, float *y);   /* roteiro: "grip:chat", "zone:nav:left", "divider:nav"... */
void dock_script_pick(const char *kind);
void dock_script_restore(void);
void dock_restore_default(void);   /* configurações: reaplica o layout padrão (animado) e grava layout.json */
/* script.c: BRIKS_SCRIPT injeta mouse e salva um PNG por quadro */
bool script_load(void);
void script_tick(cui_ctx *ui);
void script_overlay(cui_ctx *ui);
bool script_capture(cui_app *app);

/* Clique de verdade num contêiner: cui_last_hovered não serve para caixas simples (o "último componente" é o canvas do fundo,
   que cobre a janela e deixava qualquer clique acionar todos os itens). `id` é o .id da caixa, no escopo de id atual. */
/* pílula de status em linha (cabeçalho do chat, chip do tile): só existe se houver texto; vidro de bolha (Liquid Glass) */
static inline void briks_status_pill(cui_ctx *ui, const char *text) {
    if (!text || !text[0]) return;
    cui_box(ui, .dir = CUI_ROW, .pad_x = 12, .pad_y = 5, .radius = 14, .bg = B_GLASS_BUBBLE, .border = 1, .border_color = CUI_RGBA(0xffffff30), .align = CUI_ALIGN_CENTER) {
        cui_text(ui, text, .size = 12, .color = CUI_RGB(0xffffff), .nowrap = true);
    }
}
static inline bool briks_clicked(cui_ctx *ui, const char *id) {
    cui_rect r = cui_get_rect(ui, cui_make_id(ui, id)); cui_vec2 m = cui_mouse_pos(ui);
    return cui_mouse_clicked(ui, CUI_MOUSE_LEFT) && r.w > 0 && m.x >= r.x && m.y >= r.y && m.x < r.x + r.w && m.y < r.y + r.h;
}
#endif
