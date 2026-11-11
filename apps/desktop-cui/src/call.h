/* Chamada: estado da tela (chat da sala, grade/foco, tela cheia, pedido de transmissão), histórico e mini-painel de voz.
 * Fica fora de briks_ui.h de propósito: nada aqui mexe em app_state_t. */
#ifndef BRIKS_CALL_H
#define BRIKS_CALL_H
#include "briks_ui.h"
#include "callhist.h"

#define CALL_MAX_MSGS 200
typedef struct {
    msg_t msgs[CALL_MAX_MSGS]; int msg_count;   /* chat da sala: vale só durante a chamada */
    int unread;                                 /* chegaram com a aba Chat fechada */
    char draft[512];
    int layout;                                 /* 0 grade, 1 foco */
    int focus;                                  /* -1 automático (quem fala); 9 = eu; 0..7 = slot do participante */
    bool fullscreen;
    double last_move;                           /* última vez que o mouse mexeu (a barra some após 3 s em tela cheia) */
    float mx, my;
    struct { bool open; char peer_id[48], name[64]; double at; } req;   /* alguém pediu para eu transmitir */
    char toast[160]; double toast_until;        /* retorno curto: "Pedido enviado", "Fulano não pode transmitir agora" */
} call_ui_t;
extern call_ui_t g_call;

/* ui_call.c */
void call_chat_add(const char *id, const char *author, const char *content, long long ts_ms, bool mine);
void call_chat_send(app_state_t *s);                    /* envia g_call.draft a quem está na sala e mostra o balão */
void call_reset(app_state_t *s);                        /* saiu da sala: limpa o chat, o pedido e a tela cheia */
void call_set_fullscreen(app_state_t *s, bool on);
void call_toast(const char *text);
void call_request_in(const char *peer_id, const char *name);   /* evento stream.request */
void call_banner(cui_ctx *ui, app_state_t *s);          /* "Fulano pediu para você transmitir": na chamada e no menu */
int call_probe(const char *name);                       /* roteiro: estado para 'expect' (-2 = desconhecido) */
/* rtc.c */
void rtc_room_chat(const char *id, const char *text);
void rtc_stream_request(app_state_t *s, int slot);
void rtc_stream_reply(const char *peer_id, bool accepted);
/* ui_history.c */
enum { HIST_CENTER, HIST_FULL, HIST_DOCK };
void history_tick(app_state_t *s);                      /* todo quadro: registra a chamada em andamento e grava ao sair */
void history_open(app_state_t *s);                      /* item "Histórico de chamadas": abre no painel central (ou tela inteira sem ele) */
void history_close(void);
bool history_is_open(void);
void ui_history_panel(cui_ctx *ui, app_state_t *s, int mode);
int history_probe(const char *name);
void call_room_title(const app_state_t *s, char *out, size_t cap);   /* nome exibido da sala atual */
/* ui_voice.c */
void ui_voice_mini(cui_ctx *ui, app_state_t *s);        /* cartão "Voz conectada" no rodapé do menu lateral */
/* ui_social.c */
void briks_bubble(cui_ctx *ui, app_state_t *s, const msg_t *m, bool interactive, bool grouped);   /* o balão do painel de conversa */
/* settings.c */
void settings_audio_set(float in_vol, float out_vol, bool gate_on, float gate_db);   /* 0..100, porta em dB: grava como a aba Voz */
#endif
