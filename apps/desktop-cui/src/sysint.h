/* Pacote "Janela e sistema": bandeja, instância única, estado da janela, avisos do sistema, sons, não lidas, iniciar com o Windows.
 * winstate.c = janela/instância/configuração; tray.c = ícone da bandeja, menu, balão e ícone da barra de tarefas; notify.c = quando avisar. */
#ifndef BRIKS_SYSINT_H
#define BRIKS_SYSINT_H
#include "briks_ui.h"
#include "sysmodel.h"

/* ---- configuração do sistema (sistema.cfg; BRIKS_SYS_FILE em testes) ---- */
typedef struct {
    bool close_to_tray;    /* fechar a janela deixa o Briks na bandeja (a chamada continua) */
    bool autostart;        /* iniciar com o Windows (chave Run do usuário) */
    bool nt_msg;           /* aviso de nova mensagem */
    bool sounds;           /* sons de aviso e da chamada */
    int sound_vol;         /* 0..100 */
    bool tray_hint_shown;  /* já avisou "continua na bandeja" */
    int win_x, win_y, win_w, win_h; bool win_max; bool win_valid;   /* última janela (retângulo externo normal) */
} sys_cfg_t;
extern sys_cfg_t g_sys;

/* ---- winstate.c ---- */
bool sys_startup(const char *cmdline, bool automation);   /* antes de criar a janela: --aguardar=PID (reinício) e instância única; false = já havia outra (foi trazida à frente): sair */
bool sys_start_minimized(void);                          /* aberto pelo Windows (--minimizado): começa só na bandeja */
void sys_attach(app_state_t *s, bool automation);        /* depois de criar a janela (oculta): restaura posição, liga bandeja e mostra */
void sys_tick(app_state_t *s);                           /* a cada quadro */
void sys_before_destroy(app_state_t *s);                 /* salva a janela e remove o ícone da bandeja */
int  sys_probe(const char *name);                        /* roteiro: expect <nome> <valor> */
bool sys_script(const char *args);                       /* roteiro: sys <comando> ... */
void sys_show_window(void);                              /* traz a janela à frente (bandeja, segunda instância, clique no aviso) */
void sys_request_quit(void);                             /* sair de verdade (menu da bandeja) */
void sys_restart(void);                                  /* reiniciar o Briks */
bool sys_open_url(const char *url);                      /* só http(s) */
bool sys_protocol_register(void);
bool sys_autostart_get(void);
bool sys_autostart_set(bool on);                         /* grava/remove na chave Run (nunca em teste sem BRIKS_AUTOSTART_KEY) */
void sys_cfg_save(void);
bool sys_in_tray(void);
bool sys_is_headless(void);                              /* automação: nunca mostra janela nem aviso de verdade */
bool sys_window_focused(void);
void *sys_hwnd(void);
void sys_settings_notifications(cui_ctx *ui);            /* cartões "Avisos do sistema" e "Sons" (aba Notificações) */
void sys_settings_system(cui_ctx *ui);                   /* cartão "Sistema" (aba Sobre) */

/* ---- tray.c ---- */
bool tray_create(void *hwnd, unsigned callback_msg);     /* adiciona o ícone */
void tray_destroy(void);
bool tray_present(void);
bool tray_visible(void);
bool tray_readd(void);
void tray_taskbar_invalidate(void);                                   /* explorer reiniciou (TaskbarCreated) */
void tray_update(const char *tip, int unread);           /* dica e contador (ícone com bolinha) */
bool tray_balloon(const char *title, const char *text);  /* aviso nativo (balão do Windows) */
void tray_handle(unsigned lparam_event, void *hwnd, int *cmd_out);   /* evento do ícone (mensagem de retorno); devolve o comando do menu escolhido, 0 = nenhum */
void tray_menu_state(bool in_call, bool mic_on);
void tray_taskbar_badge(void *hwnd, int n, uint32_t rgb);   /* ícone de sobreposição na barra de tarefas (0 = tira) */
enum { TRAY_CMD_NONE = 0, TRAY_CMD_OPEN = 1001, TRAY_CMD_MUTE = 1002, TRAY_CMD_RESTART = 1003, TRAY_CMD_QUIT = 1004 };
extern int g_tray_balloons, g_tray_badge_ok, g_tray_icon_badged;             /* contadores de teste */

/* ---- notify.c ---- */
void notify_tick(app_state_t *s);                        /* sons da chamada, transmissão iniciada, contador, grace */
void notify_post(app_state_t *s, notify_kind_t kind, const char *chan, const char *title, const char *text);   /* regras + balão + som + não lida */
void notify_chat_message(app_state_t *s, const char *channel, const char *author, const char *content);       /* chat.message do core */
void notify_friend(app_state_t *s, const char *text, bool in_call);                                          /* amigo online / entrou na chamada (ui_friends.c) */
void notify_open_channel(app_state_t *s, const char *channel);                                               /* abre a conversa (clique no aviso) */
void notify_chat_opened(app_state_t *s);                 /* zera as não lidas da conversa aberta */
int  notify_unread_total(void);
int  notify_unread_for(const char *channel);
void sound_play(sound_kind_t k);
extern int g_notify_shown, g_notify_blocked, g_sound_played;   /* contadores de teste */
bool notify_take_click(char *chan, size_t cap);          /* conversa do último aviso clicado (NIN_BALLOONUSERCLICK) */
void notify_set_force_focus(int v);                      /* teste: -1 real, 0/1 força "janela em foco" */
int  notify_force_focus(void);
void notify_set_grace_ms(unsigned ms);
/* settings.c */
bool settings_nt_online(void);
bool settings_nt_stream(void);
#endif
