/* Remake da interface do Briks em CUI. Paleta e medidas vêm de apps/web/src/styles.css e glass-layout.css. */
#ifndef BRIKS_UI_H
#define BRIKS_UI_H
#include <cui/cui.h>
#include <cui/cui_app.h>
#include <stdbool.h>
#include <stddef.h>
#include "theme_cfg.h"
#include "perf.h"

/* Participantes remotos que a UI acompanha numa chamada (a grade se ajusta; o servidor de chamadas aceita mais que a malha P2P). */
#define BRIKS_MAX_PEERS 32

/* Paleta (--briks-*, --glass-*) */
#define B_C1        (g_th.c1)   /* acento: vem do gradiente do tema (themes.c) */
#define B_C2        (g_th.c2)
#define B_C3        (g_th.c3)
#define B_BG        CUI_RGB(0x12041a)
#define B_BG_DEEP   CUI_RGB(0x0f0316)
#define B_PANEL     (g_th.field)   /* campos de texto: versao escura do matiz do tema */
#define B_LINE      (g_th.line)
#define B_TEXT      CUI_RGB(0xedf1f7)
#define B_MUTED     (g_th.muted)
#define B_DANGER    CUI_RGB(0xe95555)
#define B_ONLINE    CUI_RGB(0x57d28b)
#define B_GLASS_BORDER CUI_RGBA(0xffffff1c)  /* .11 */
#define B_GLASS_BORDER_SOFT CUI_RGBA(0xffffff12) /* .07 */

typedef enum { SCREEN_AUTH, SCREEN_SOCIAL, SCREEN_CALL, SCREEN_HISTORY } screen_t;

#define MAX_FRIENDS 128
#define MAX_COMMUNITIES 32
typedef struct { char id[48]; char name[96]; char request_id[48]; char status[64]; char presence[64]; char nick[32]; char fp[72]; char jwk[260]; } person_t;   /* status = texto do balão; presence = o que a pessoa está usando */
typedef struct { char id[48]; char name[96]; int members; } community_t;

#define MAX_GROUPS 32
#define MAX_MSGS 200
#define GROUP_MAX_CH 16
#define GROUP_MAX_MEMBERS 48
typedef struct { char id[48]; char name[48]; char room[48]; bool voice; } channel_t;       /* canal de texto ou de voz (room = sala da chamada) */
typedef struct { char id[48]; char name[96]; bool admin; char fp[72]; char jwk[260]; } gmember_t;   /* fp/jwk: identidade P2P (grupo assinado) */
typedef struct { char id[48]; char name[96]; char channel[64]; bool can_moderate;
                 char owner[48]; bool is_owner; channel_t ch[GROUP_MAX_CH]; int nch, active_ch; gmember_t m[GROUP_MAX_MEMBERS]; int nm; char *raw; } group_t;   /* can_moderate: sou dono ou administrador (apagar mensagens alheias, fixar) */
typedef struct { char id[48]; char author[96]; char content[512]; char created[40]; bool mine; } msg_t;

typedef enum { OP_NONE, OP_AUTH, OP_ME, OP_FRIENDS, OP_COMMUNITIES, OP_GROUPS, OP_ADD_FRIEND, OP_ACCEPT,
               OP_MSG_LOAD, OP_MSG_RECV, OP_MSG_SEND, OP_GROUP_SAVE, OP_EVENT_SAVE, OP_BRIDGE_LOGIN, OP_LEGACY_FRIENDS } op_t;


/* conta online (account.c): estado vindo do briks-core (eventos account.*) */
enum { ACCT_UNKNOWN, ACCT_OUT, ACCT_BUSY, ACCT_EMAIL_SENT, ACCT_IN };
enum { AV_MAIN, AV_LOGIN, AV_SIGNUP, AV_RECOVER, AV_NEWPASS };   /* telas da entrada */
enum { BRIDGE_NONE, BRIDGE_WAIT, BRIDGE_RUNNING, BRIDGE_FAILED };   /* ligação da conta com o banco local deste PC */
#define MAX_LEGACY 64
enum { BKP_UNKNOWN, BKP_NONE, BKP_OK, BKP_OTHER };   /* não consultado; sem cópia; cópia desta identidade; cópia de outra identidade */

/* Transmissão de tela (ui_share.c): fontes do seletor, áudio por app, qualidade e estado local/remoto */
#define SH_MAX_SRC 24
#define SH_MAX_APPS 16
typedef struct { char id[24]; char name[96]; char sub[64]; char thumb[280]; int w, h; cui_texture tex; bool tex_tried; } share_src_t;
typedef struct { unsigned pid; char name[64]; bool playing, on; } share_app_t;
typedef struct { int res, fps, br_mode, content; float br_custom; } qual_t;   /* qualidade de UMA transmissão (1.4.1: cada tela tem a sua) */
typedef struct {
    bool dialog_open, loading, quality_open;
    qual_t qs[2]; bool qs_ok[2]; int qsel;       /* 1.4.1: qualidade guardada por transmissão (0 = 1ª, 1 = 2ª); res/fps/br_mode/br_custom/content abaixo são a da transmissão em edição (qsel) */
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
    bool audio_muted;   /* 1.4.1: "Sem áudio" ao vivo pelo botão da barra (a faixa continua aberta no core); volta a falso a cada início */
    int w, h, cur_fps, kbps, cpu, audio_sources;
    float stat_fps, enc_ms, audio_peak; int stat_kbps;
    /* laço de adaptação ao vivo (share.stats) */
    int live_h, target_fps, target_kbps, est_kbps, rtt_ms, rtx; float loss_pct;
    bool black;                             /* a fonte está saindo preta (tela cheia exclusiva / conteúdo protegido) */
    char adapting[8], reason[12];           /* "" | "rede" | "cpu"; "perda" | "atraso" */
    char label[64], encoder[64], err[160], last_kind[12], last_id[24];
    /* transmissões dos outros (índice = slot em rtc_peers) */
    struct { bool on, audio, watching, has_frame, poor, stalled; int w, h, fps, kbps, low_secs; float volume; bool vol_set; char label[64], path[300]; } remote[BRIKS_MAX_PEERS];
    /* 2ª tela simultânea (segundo monitor), minha e dos outros: sem áudio próprio (o áudio da transmissão é o da 1ª) */
    bool pick_second;                          /* o seletor aberto vai iniciar a 2ª tela */
    struct { bool active, starting; int w, h, fps, kbps; char label[64], kind[12], id[24]; } second;
    int viewers[2]; bool vknown[2]; char vnames[2][200];   /* quem assiste à MINHA tela (slot 0/1): só P2P e só depois do 1º aviso do core (sem dado = oculto) */
    struct { bool on, watching, has_frame, stalled; int w, h, fps, kbps; char label[64], path[300]; } remote2[BRIKS_MAX_PEERS];
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
    cui_texture user_photo;                    /* foto de perfil (0 = só iniciais) */
    char user_nick[32];                        /* @nick da conta (sem o @): o escolhido, ou um provisório derivado do nome; nunca do e-mail */
    char user_status[64], user_presence[64];   /* status (balão) e rich presence (programa em uso); status vazio = mostra a presença */
    bool rp_enabled; int rp_audience;           /* rich presence ligado; 0 = todos os amigos, 1 = só quem está na mesma chamada */
    person_t friends[MAX_FRIENDS]; int friend_count;
    person_t pending[MAX_FRIENDS]; int pending_count;
    person_t outgoing[MAX_FRIENDS]; int outgoing_count;              /* pedidos que eu enviei e ainda não foram respondidos */
    struct { char id[48]; char name[96]; } blocked[64]; int blocked_count;   /* bloqueados: lista local (ui_friends.c); 0006 prepara o servidor */
    int friends_tab; char friends_q[64];                            /* aba do hub de Amigos (0 todos, 1 online, 2 pendentes, 3 bloqueados, 4 adicionar) e a busca */
    bool chan_open, members_open, gren_open, gleave_open; char chan_name[48], grp_name_edit[96], gchan_id[48]; int chan_kind;   /* grupos (ui_groups.c) */
    bool unfriend_open; char unfriend_id[48], unfriend_name[96]; bool blocked_loaded;
    struct { char id[48]; bool online; } pres[MAX_FRIENDS]; int pres_n; bool pres_remote;   /* presença por batimento (0007): quem está online; pres_remote=false = servidor sem a 0007 */
    char my_jwk[260];                                                                         /* minha chave pública (session.ready), compacta */
    bool join_open, ginv_open, ginv_wait; char join_err[160]; char *ginv_code; char join_code[32768]; char ginv_short[24], ginv_short_msg[200]; int ginv_short_st;   /* 1.4.1: código curto BRK-XXXX-XXXX (0=ainda não, 1=pedindo, 2=pronto, 3=indisponível) */   /* convite de grupo: colar e entrar / mostrar o código */
    char gnick[32], gnick_msg[140]; person_t gnick_hit; bool gnick_found;                       /* convite por @nick (friends.lookup): busca e resultado */
    bool notify_off; char ginv_q[48];                                                         /* avisos de amigo online (desligável) · busca na lista de convite do grupo */
    bool invisible;                                                                          /* "Aparecer offline": o batimento segue, mas os amigos não me veem (profile.json) */
    community_t communities[MAX_COMMUNITIES]; int community_count;
    group_t groups[MAX_GROUPS]; int group_count;
    int active_group;              /* -1 = nenhum */
    msg_t msgs[MAX_MSGS]; int msg_count;
    char chat_channel[160];
    bool reload_msgs;
    bool create_group_open, profile_open, logout_confirm_open;
    long long status_expires;                  /* epoch (s) em que o status some sozinho; 0 = não expira (profile.c) */
    char user_emoji[16]; int status_clear, profile_tab, settings_tab;      /* perfil: emoji do status e "limpar depois de" (0 não, 1 30 min, 2 1 h, 3 hoje) */
    char rp_ignore[6][32]; int rp_ignore_n;    /* programas ignorados pela presença */
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
    bool net_cfg_known, net_cfg_sfu; char net_cfg_err[120];   /* net.config do core: SFU configurado? erro de configuração */
    unsigned long long rtc_conn_since;   /* GetTickCount64 de quando o estado virou "conectando" (0 = não está) */
    char rtc_state[16];            /* "" | conectando | conectado | reconectando | erro | desconectado */
    char rtc_msg[160], rtc_fp[72], room_name[64];
    bool rtc_want_room, rtc_session_sent, rtc_session_ready, in_room;
    int rtc_peer_count, rtc_authed_count;
    int rtc_overflow;              /* participantes além de BRIKS_MAX_PEERS: estão na chamada (o áudio toca), mas sem quadro: a grade mostra "+N" */
    struct { char id[48]; char state[16]; float level; bool mic_off, deafened, muted_by_me; float volume; bool vol_set; char fp[72], name[64], status[64], presence[64]; } rtc_peers[BRIKS_MAX_PEERS];
    float me_level;                /* nível de fala do microfone local (0..1), evento "speaking" do core */
    bool rtc_owner_me;             /* sou o dono da sala (evento room.owner): só o dono vê "Expulsar" */
    int kick_slot;                 /* peer pendente de confirmação de expulsão (-1 = nenhum) */
    bool kick_open, kicked_open;   /* diálogo de confirmação; aviso "Você foi removido da chamada" */
    bool import_open, import_done;   /* diálogo "Importar seus dados do Briks antigo?" (migrate.c) */
    char import_info[200], import_result[200];
    /* conta online */
    int acct_state, acct_view, acct_bridge;
    bool acct_configured, acct_mock, acct_asked, acct_demo;
    char acct_what[16], acct_provider[16], acct_kind[12];      /* o que está em andamento; signup|recovery no e-mail enviado */
    char acct_error[240], acct_info[240];
    char acct_user_id[48], acct_nick[32], acct_code[12];
    char bridge_email[96], bridge_pw[64];                      /* credencial do banco LOCAL (não é a senha da conta) */
    bool nick_open; char nick_edit[32], nick_msg[160];
    char name_edit[64], profile_msg[200];
    /* cópia cifrada da identidade, recuperável por frase (o servidor nunca vê a frase nem a chave) */
    int backup_known;                                          /* BK_UNKNOWN.. (abaixo) */
    bool backup_phrase_open, backup_restore_open, backup_delete_open, backup_ack, backup_busy;
    char backup_words[12][24];                                 /* frase recém-gerada: só enquanto o diálogo está aberto */
    char backup_input[240], backup_msg[240], backup_date[16];                      /* editor do Perfil: nome em edição e o retorno da última troca */
    int import_state;                                          /* 0 não consultado, 1 há dados, 2 importando, 3 feito, 4 nada a importar */
    char import_msg[240];
    person_t legacy[MAX_LEGACY]; int legacy_count;             /* contatos do Briks antigo (só nome; sem conta) */
    bool reload_groups;
    share_state_t sh;
    char ice_note[120];   /* "Somente STUN" / "STUN + TURN" (evento ice.info do core) */
    char net_note[220];   /* modo de rede da chamada: "SFU · servidor" ou "P2P · motivo" (evento net.mode do core) */
    bool net_failed;      /* a entrada pelo servidor de chamadas falhou e nenhuma chamada abriu: a tela oferece "Entrar em modo P2P" */
    bool force_p2p;       /* o usuário escolheu entrar em P2P nesta chamada (vale até sair dela) */
    bool net_sfu;         /* a chamada atual está no servidor de chamadas (SFU): o dono pode silenciar alguém para todos */
    /* tela Rede: TURN próprio (ice.json). A senha só existe aqui enquanto se digita; o core a cifra e nunca a devolve */
    bool ice_turn, ice_has_cred, ice_asked, ice_busy, ice_testing, ice_edit_init;
    char ice_urls[300], ice_user[100], ice_pass[130], ice_msg[240], ice_test_msg[240];
    int ice_test_level;                                        /* 0 sem teste, 1 nada respondeu, 2 só STUN, 3 TURN respondeu */
    char others_msg[200]; bool others_busy, others_confirm;
    char ice_source[8];                                        /* default | file | env */
    char birth[16];                                            /* cadastro: data de nascimento digitada (só valida, nunca é guardada) */
    /* Voz e atalhos: teste do microfone fora da chamada e resultado do registro dos atalhos globais */
    bool mic_test_on; float mic_test_level; char mic_test_msg[160];
    /* aviso do microfone/áudio (core: audio.mic, audio.device, mic.test com code): mic_state "" = sem notícia, "ok" ou no_device | denied | busy | error */
    char mic_state[12]; char mic_text[260]; char dev_notice[220];
    int hk_status[3];                                          /* HKS_* (hotkeys.h): mutar, ensurdecer, transmissão */
    char *amg_code; char amg_in[1024], amg_msg[200]; bool amg_wait; int amg_pend;   /* amigo P2P por código assinado (friends_code_card) */
    /* PRF-12: computadores ligados ao @nick (core: account.devices / account.device_revoke; migração 0009) */
    struct { char peer[48]; char name[72]; char created[16]; char used[16]; } devs[12];
    int dev_n; bool dev_remote, dev_have; char dev_current[48], dev_msg[160], dev_confirm[48]; unsigned long long dev_asked_at;
    /* AMI-16: "Desconectar P2P" (fecha só o chat privado com um amigo; a chamada e a amizade ficam) */
    char dmoff[8][160]; int dmoff_n; bool dmdisc_open; char dmdisc_fp[160], dmdisc_name[96];
    /* CHM-16: inatividade na chamada (call_idle.h) */
    unsigned long long idle_last; int idle_phase_now; long long idle_left_ms; char idle_notice[120];
    /* AMI-15: amigos com o sino desligado (sem aviso de "ficou online" / "entrou na chamada"); lista guardada em friend-bells.txt */
    char fbell[24][48]; int fbell_n; bool fbell_loaded;
    bool friends_loaded;   /* BAS-30: a primeira lista de amigos já chegou do serviço (antes dela a tela mostra o esqueleto) */
    int ice_test_ms;                                           /* ice.test: tempo até o relay responder (ms) */
} app_state_t;

/* migrate.c: importação dos dados do Electron */
void migrate_probe(app_state_t *s);
void migrate_pump(app_state_t *s);
void migrate_dialog(cui_ctx *ui, app_state_t *s);
/* ui_update.c: aviso "pronto para instalar" (update.c baixa e verifica em segundo plano) */
void update_banner(cui_ctx *ui, app_state_t *s);

/* app.c: máquina de estados das chamadas ao backend */
void app_submit_auth(app_state_t *s);
void app_add_friend(app_state_t *s);
void app_accept_friend(app_state_t *s, const char *request_id);
void app_logout(app_state_t *s);          /* sai da conta online (se houver) e do banco local */
void app_local_logout(app_state_t *s);    /* só limpa o estado local e volta para a entrada */
/* account.c: contas online pelo briks-core */
void account_tick(app_state_t *s);                          /* a cada quadro: liga o core, pede o estado da conta, conclui a ponte local */
bool account_handle_event(app_state_t *s, const char *line); /* eventos account.*, friends.*, local.*, import.* */
void account_oauth(app_state_t *s, const char *provider);   /* "google" | "discord": abre o navegador */
void account_submit(app_state_t *s);                        /* envia o formulário da tela atual (entrar, criar, recuperar, nova senha) */
void account_submit_code(app_state_t *s);                   /* código de 6 dígitos do e-mail */
void account_cancel(app_state_t *s);
void account_logout(app_state_t *s);
void account_retry(app_state_t *s);                         /* pede o estado de novo (erro de rede ao abrir) */
/* ui_friends.c: hub de Amigos e bloqueio local */
bool friends_blocked(app_state_t *s, const char *id);
void friends_block(app_state_t *s, const char *id, const char *name);
void friends_unblock(app_state_t *s, const char *id);
void friends_hub(cui_ctx *ui, app_state_t *s);
void friends_dialogs(cui_ctx *ui, app_state_t *s);
/* ui_groups.c: canais, cargos, membros e convites dos grupos */
void groups_load(app_state_t *s, const char *body);
cui_texture groups_image(app_state_t *s, const char *gid);                     /* imagem do grupo (0 = só iniciais); pede ao core na 1ª vez */
void groups_core_event(app_state_t *s, const struct jval *j, const char *ev);   /* group.result / group.snapshot do core */
void groups_resend(app_state_t *s);                                            /* reenvia snapshots aos membros presentes (peer novo na sala) */
void groups_tick(app_state_t *s);                                              /* grava o que os eventos deixaram pendente */
void groups_sidebar(cui_ctx *ui, app_state_t *s, int gi);
bool groups_collapsed(const char *gid);
bool settings_group_collapsed(const char *gid); void settings_group_collapse(const char *gid, bool on);   /* settings.c */
void groups_collapse_set(const char *gid, bool on);   /* 1.4.1: grupo recolhido na barra lateral */
void groups_header(cui_ctx *ui, app_state_t *s);
void groups_dialogs(cui_ctx *ui, app_state_t *s);
void groups_create_json(const app_state_t *s, char *out, size_t cap);
void groups_overlay(cui_ctx *ui, app_state_t *s);                              /* menu de contexto do grupo (camada de cima, junto do menu da mensagem) */
void friends_code_card(cui_ctx *ui, app_state_t *s);   /* cartão "Por código" (gerar/copiar/aceitar), usado no diálogo e no hub */
void friends_p2p_event(app_state_t *s, const struct jval *j);   /* friend.result do core */
bool groups_want_roster(const app_state_t *s);                                  /* há grupo com sala de voz: vale abrir a sessão do core cedo (presença por sala) */
void groups_roster_tick(app_state_t *s); void groups_roster_reset(void);
void groups_roster_event(app_state_t *s, const struct jval *j);                         /* evento group.roster do core */
void groups_demo_roster(void);                                                  /* dados demo da presença por sala (BRIKS_DEMO_GROSTER) */
int groups_probe(const app_state_t *s, const char *name);                      /* roteiro oculto: expect grp_nch / grp_nm / grp_nadmin ... */
bool groups_members_panel(cui_ctx *ui, app_state_t *s);                        /* painel Membros: membros do grupo aberto (false = nenhum grupo aberto) */
void groups_row_menu(cui_ctx *ui, app_state_t *s, int gi, cui_rect r);         /* botão direito na linha do grupo: menu de contexto */
void account_friends(app_state_t *s);
void account_status_sync(app_state_t *s);   /* 1.4.3: sobe o status personalizado (texto, emoji, validade) para a conta quando muda */
void account_friends_poll(app_state_t *s);   /* a cada 30 s e ao voltar o foco: convites novos aparecem sem reiniciar */
void account_block_remote(app_state_t *s, const char *user_id, bool block);
void account_remove_friend(app_state_t *s, const char *user_id);   /* desfaz a amizade ou cancela o pedido enviado */
void account_add_friend(app_state_t *s);
void account_respond_friend(app_state_t *s, const char *request_id, bool accept);
void account_set_nick(app_state_t *s);
void account_set_name(app_state_t *s);                      /* troca o nome de exibição por s->name_edit */
void account_pick_photo(app_state_t *s);                    /* abre o seletor de arquivos do Windows e envia a foto escolhida */
void account_reset_photo(app_state_t *s);                   /* volta para a foto da conta (Google/Discord) */
void ui_account_editor(cui_ctx *ui, app_state_t *s);
void account_logout_others(app_state_t *s);                 /* "Sair dos outros computadores" */
void account_ice_get(app_state_t *s);
void account_ice_save(app_state_t *s);                      /* grava ice_urls/ice_user/ice_pass no core */
void account_ice_clear(app_state_t *s);
void account_ice_test(app_state_t *s);
void account_backup_status(app_state_t *s);
void account_backup_create(app_state_t *s);                 /* gera a frase (o core mostra uma vez) */
void account_backup_confirm(app_state_t *s);                /* "anotei": cifra e envia a cópia */
void account_backup_cancel(app_state_t *s);                 /* descarta a frase gerada */
void account_backup_restore(app_state_t *s);                /* usa s->backup_input */
void account_backup_delete(app_state_t *s);
bool friends_bell_off(app_state_t *s, const char *id);               /* o sino desse amigo está desligado (AMI-15) */
void friends_bell_set(app_state_t *s, const char *id, bool off);    /* liga/desliga o sino e grava */
bool friends_dm_off(const app_state_t *s, const char *fp);          /* o chat privado com essa pessoa foi desconectado */
void friends_dm_link(app_state_t *s, const char *fp, bool on);      /* evento chat.link do core: desconectou (false) ou reconectou (true) */
void account_devices_get(app_state_t *s);                          /* pede ao core a lista de computadores da conta */
void account_device_revoke(app_state_t *s, const char *peer_id);   /* revoga um computador (nunca o atual) */
void ui_others_section(cui_ctx *ui, app_state_t *s);        /* "Sair dos outros computadores" (aba Conta) */
void ui_backup_section(cui_ctx *ui, app_state_t *s);        /* bloco para Configurações */
void ui_backup_dialogs(cui_ctx *ui, app_state_t *s);        /* diálogos: frase nova, recuperar, apagar */        /* bloco "foto, nome e @" para o diálogo de Perfil */
void account_import(app_state_t *s, bool apply);
void account_bridge_done(app_state_t *s, bool ok);          /* app.c avisa o fim da ligação com o banco local */
void app_begin(app_state_t *s, op_t op, const char *method, const char *path, const char *body);
void app_open_friend(app_state_t *s, int idx);   /* abre a DM e carrega o histórico persistido */
void app_open_group(app_state_t *s, int idx);
void app_send_message(app_state_t *s);
void app_forward_message(app_state_t *s, bool to_group, int idx, const char *text);   /* encaminha o texto para outra conversa */
void app_create_group(app_state_t *s);
void app_load_messages(app_state_t *s, const char *body);
/* DM fora da sala de voz (1.4.1): o core abre um canal direto com o amigo (dmlink.rs); a UI cuida da entrega, do histórico local e do estado de cada mensagem */
bool chat_in_message(app_state_t *s, const struct jval *j, bool history);   /* chat.message / chat.history do core: grava (idempotente), mostra e avisa */
void chat_in_pump(app_state_t *s);                                          /* app_pump: grava a próxima mensagem recebida no backend local */
void chat_in_done(app_state_t *s);                                          /* a gravação terminou: recarrega a conversa aberta */
void chat_dm_wake(app_state_t *s, const char *fp);                          /* dm.wake: um amigo me tocou */
bool chat_dm_wants_session(const app_state_t *s);                           /* a sessão do core (identidade) é precisa para receber DMs */
void chat_dm_tick(app_state_t *s);                                          /* abre a minha caixa de chegada ao entrar na conta */
void chat_dm_reset(app_state_t *s);                                         /* saiu da conta: fecha caixa e canais */
void app_load_groups(app_state_t *s, const char *body);
void app_demo(app_state_t *s, const char *mode);  /* BRIKS_DEMO: dados fixos para capturas */
void app_pump(app_state_t *s);
/* rtc.c: ponte com o briks-core */
void rtc_join_room(app_state_t *s, const char *room);   /* inicia o core se preciso, abre a sessão e entra na sala */
void rtc_leave_room(app_state_t *s);
void themes_tab(cui_ctx *ui);                              /* themes.c: aba Temas (gradientes) */
void themes_tick(cui_ctx *ui);                             /* themes.c: carrega Meus gradientes; BRIKS_THEME_PRESET para capturas */
void settings_dialog(cui_ctx *ui, app_state_t *s);   /* settings.c: Configurações com lateral de seções */
int settings_fw_choice(void); void settings_fw_set(int c);   /* escolha do Firewall: 0 nova, 1 liberado, 2 agora não, 3 não perguntar (firewall.c) */
void fw_maybe_prompt(void); void fw_tick(void); void fw_dialog(cui_ctx *ui, app_state_t *s); void fw_request(bool add); void fw_refresh(void);
bool fw_busy(void); bool fw_open(void); int fw_exists(void); const char *fw_msg(void); const char *fw_state_text(void);
int settings_net_pref(void);                               /* 0 Automático, 1 Internet direta, 2 VPN privada */
int settings_idle_min(void);                              /* CHM-35: minutos sem falar até sair da chamada (0 = nunca) */
int settings_call_mode(void);                              /* 0 Automático, 1 P2P, 2 SFU (Configurações > Rede) */
void settings_audio(float *in_gain, float *out_gain, bool *gate_on, float *gate_db);   /* ganhos 0..1 (1 = normal) e porta de ruído, para o core */
void settings_audio_ex(char *mic, size_t mic_cap, char *out, size_t out_cap, int *noise_off);   /* dispositivos escolhidos (nome; vazio = padrão) e supressão de ruído (1 = desligada) */
void rtc_mic_test(app_state_t *s, bool on);               /* rtc.c: teste do microfone fora da chamada (eventos mic.test) */
bool rtc_mic_problem(const app_state_t *s);               /* rtc.c: o microfone tem um problema a avisar (sem dispositivo, negado, em uso) */
void rtc_open_mic_privacy(void);                          /* rtc.c: abre Configurações do Windows > Privacidade > Microfone */
extern bool g_hk_recording;                                /* hotkeys_rt.c: capturando tecla na tela de Atalhos */
void settings_keys(const char **mute, const char **deafen, const char **stream, const char **ptt, bool *ptt_on, float *delay);
void hotkeys_tick(cui_ctx *ui, app_state_t *s);            /* hotkeys.c: atalhos globais e apertar para falar */
void settings_tick(void);                                  /* carrega/grava settings.cfg */
int settings_probe(const char *n, app_state_t *s);          /* settings.c: sondas dos roteiros de teste */
void settings_data_pump(app_state_t *s);                  /* settings_data.c: recolhe os pedidos de apagar histórico */
void overlay_tick(cui_ctx *ui, app_state_t *s);             /* overlay_host.c: abre/fecha e alimenta o briks_overlay.exe */
void settings_notice(const char *text);                    /* registra um aviso na aba Avisos */
void briks_theme_editor_open(void);                        /* abre o editor de tema (aba Temas) */
void briks_theme_reset(void);                              /* restaura o tema Briks e grava */
long long profile_expiry_for(int clear, long long now);   /* epoch de expiração do status; 0 = nunca */
bool profile_ignored(const app_state_t *s, const char *exe, const char *text);
void profile_tick(app_state_t *s);
void friends_notify_tick(app_state_t *s);                       /* detecta amigo que ficou online / entrou na chamada */
void friends_notify_banner(cui_ctx *ui, app_state_t *s);        /* faixa de aviso (some sozinha em ~7 s) */
bool friends_online(const app_state_t *s, const person_t *p);   /* online de verdade (batimento) ou na sua chamada agora */
void account_presence_tick(app_state_t *s);                      /* batimento de ~60 s (account.c) */                              /* perfil local: presença do programa em uso, persistência (profile.c) */
void rtc_set_mic(app_state_t *s, bool on);
void rtc_set_deaf(app_state_t *s, bool on);
float rtc_saved_volume(const char *fp);                          /* volume lembrado para essa identidade (0..2; 1 = normal), CHM-14 */
void rtc_set_peer_volume(app_state_t *s, int slot, float gain);   /* volume só para quem escuta, 0..2 (1 = normal) */
void ui_voice_panel(cui_ctx *ui, app_state_t *s);          /* ui_voice.c: conteúdo do painel Sala de voz */
void rtc_kick_peer(app_state_t *s, int slot);               /* expulsa (só o dono): mensagem assinada pelo core */
bool rtc_saved_mute(const char *fp);                       /* 'Silenciar para mim' lembrado para essa identidade (CHM-36) */
void rtc_mute_peer(app_state_t *s, int slot, bool muted);   /* ganho zero só para essa pessoa */
void rtc_join_p2p(app_state_t *s);                          /* reentra na mesma sala pedindo P2P de propósito (depois de o SFU falhar) */
void rtc_room_mute(app_state_t *s, int slot, bool muted);   /* só o dono, só no SFU: fecha/reabre o microfone da pessoa para todos */
void rtc_pump(app_state_t *s);
void rtc_chat_event(const char *fields);   /* assina e envia um chat.event (campos JSON sem chaves) a quem está na sala */                           /* a cada quadro: consome eventos do core */
const char *rtc_status_text(const app_state_t *s, char *buf, size_t cap);   /* "Conectado · 2 pessoas" etc. */
/* ui_share.c: transmissão de tela */
struct jval;
bool share_handle_event(app_state_t *s, const struct jval *j, const char *ev);   /* eventos share.* do core */
bool share_thumb(cui_ctx *ui, app_state_t *s, int slot, cui_texture *tex, int *w, int *h);   /* thumb minúscula (desfocada ao esticar) da transmissão de quem está no slot; false = sem quadro */
bool share_any_live(const app_state_t *s);   /* alguém (outro participante) transmite a tela */
bool share_mini(cui_ctx *ui, app_state_t *s, float w);        /* miniatura ao vivo da transmissão em foco, na barra "Em chamada" (ui_voice_mini) */
void share_tick(cui_ctx *ui, app_state_t *s);                 /* a cada quadro: assistir só no modo Transmissão, ganchos de teste */
void share_open_dialog(app_state_t *s);                       /* pede as fontes e os apps com áudio e abre o seletor */
void share_stop(app_state_t *s);
void share_stop_second(app_state_t *s);                       /* para só a 2ª tela */
void share_reset(app_state_t *s);                             /* saiu da sala: esquece tudo */
void ui_share_dialog(cui_ctx *ui, app_state_t *s);            /* "Compartilhar tela — Escolha a fonte e confirme" */
void ui_share_bar(cui_ctx *ui, app_state_t *s);               /* botão Compartilhar + chips de áudio e qualidade */
int share_probe(const char *name, const app_state_t *s);      /* estados para os roteiros de teste; -9999 = desconhecido */
bool share_notice_active(cui_ctx *ui, app_state_t *s);       /* há aviso "X começou a transmitir" para mostrar */
void ui_share_notice_row(cui_ctx *ui, app_state_t *s);        /* a linha do aviso, com o botão Assistir */
bool ui_share_stage(cui_ctx *ui, app_state_t *s, float W, float H, int layout);             /* palco do modo Transmissão; false = ninguém transmitindo */
bool share_peer_live(const app_state_t *s, int slot);
void share_peer_gone(app_state_t *s, int slot);   /* a pessoa saiu da chamada: some o que ela transmitia (tela nos dois slots) e a última imagem */
/* ui_cam.c: câmera na chamada */
bool cam_handle_event(app_state_t *s, const struct jval *j, const char *ev);   /* eventos cam.* do core */
void cam_tick(cui_ctx *ui, app_state_t *s);                   /* a cada quadro: lista câmeras, assiste só quem aparece, solta a câmera ao sair da sala */
void cam_toggle(app_state_t *s);                              /* botão da barra: liga ou desliga */
void cam_reset(void);                                         /* saiu da sala */
bool cam_active(void); bool cam_starting(void); bool cam_remote_on(int slot);
bool cam_frame(cui_ctx *ui, app_state_t *s, int slot, cui_texture *tex, int *w, int *h);   /* vídeo do bloco; false = mostra o avatar */
int cam_dev_count(void); const char *cam_dev_name(int i);
void cam_preview_set(bool on); bool cam_preview_wanted(void); bool cam_preview_on(void); const char *cam_preview_error(void);
void cam_settings_view(cui_ctx *ui, app_state_t *s, float w, float h);
int cam_probe(const char *name); bool cam_devs_loaded(void);
const char *settings_cam_name(void); bool settings_cam_tab_visible(const app_state_t *s);         /* esse participante está transmitindo? */
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
/* Logo do Briks (cubo de tijolos, cantos já transparentes) em px x px; false se assets\logoriks-logo-128.png faltar (o chamador desenha o "B" antigo). */
bool briks_logo(cui_ctx *ui, float px);
/* Textura da logo (0 se faltar o arquivo); para quem desenha direto no canvas (cui_draw_image). */
cui_texture briks_logo_tex(cui_ctx *ui);
void briks_gradient_bar(cui_ctx *ui);                     /* faixa de 3px do topo */
void ui_auth(cui_ctx *ui, app_state_t *s);
void ui_social(cui_ctx *ui, app_state_t *s);
void ui_call(cui_ctx *ui, app_state_t *s);
void ui_history(cui_ctx *ui, app_state_t *s);
extern float g_tools_w;   /* largura reservada à direita do cabeçalho para os botões + e × do painel (0 = sem botões) */
extern int g_text_size;   /* 0 normal, 1 grande (padrão), 2 maior, 3 máximo: Configurações > Temas > Aparência */
void briks_text_scale_apply(cui_ctx *ui);
void settings_text_step(cui_ctx *ui, int delta);   /* settings.c: Ctrl+mais/menos/0 mudam o tamanho do texto (delta 0 = padrão) */
extern float g_panel_w;   /* largura atual do painel que está sendo desenhado (barra de grupos responsiva) */
void ui_panel(cui_ctx *ui, app_state_t *s, int kind);   /* ui_social.c: conteúdo de um painel do dock */
void dock_render(cui_ctx *ui, app_state_t *s);
/* Microfone riscado: a fonte de ícones do CUI é um subconjunto sem o glifo mic-off, então briks_icon desenha o microfone e um traço diagonal por cima.
   BRIKS_ICON_MIC_OFF é só um marcador para briks_icon (não é um glifo). */
#define BRIKS_ICON_MIC_OFF "briks:mic-off"
void briks_icon(cui_ctx *ui, const char *icon, float size, cui_color color);   /* cui_icon + ícones compostos do Briks */
float titlebar_height(void);   /* titlebar.c: 36 px, ou 0 em tela cheia */
bool dock_has_panel(int kind);   /* o painel está no layout atual? */
int dock_probe(const char *name);   /* roteiro: estado do dock para 'expect' */
int script_probe(const char *name);
extern int g_script_fail, g_script_rw, g_script_rh;
bool dock_script_point(const char *spec, float *x, float *y);   /* roteiro: "grip:chat", "zone:nav:left", "divider:nav"... */
void dock_script_pick(const char *kind);
void dock_script_menu(float x, float y);
void dock_script_restore(void);
void dock_restore_default(void);   /* configurações: reaplica o layout padrão (animado) e grava layout.json */
/* script.c: BRIKS_SCRIPT injeta mouse e salva um PNG por quadro */
bool script_load(void);
void script_tick(cui_ctx *ui);
void script_overlay(cui_ctx *ui);
bool script_capture(cui_app *app);

/* Clique de verdade num contêiner: cui_last_hovered não serve para caixas simples (o "último componente" é o canvas do fundo,
   que cobre a janela e deixava qualquer clique acionar todos os itens). `id` é o .id da caixa, no escopo de id atual. */
/* cor do tema com alfa 0..255 (acentos, bordas, tintas): nada de roxo fixo fora do tema */
static inline cui_color briks_acc(cui_color c, int a) { c.a = (uint8_t)a; return c; }
#define B_A(c, a) briks_acc((c), (a))

/* Balão de status: Cristal violeta (glass.c). Normal = translúcido, refração nas bordas e balão + bolinhas da cauda unidos (metaballs); Simplificado = sem refração, menos desfoque,
   mesma forma unida; Mínimo = sem shader: violeta chapado, sem borda, e pescoços desenhados em CUI para manter a silhueta unida. */
static inline cui_color briks_bubble_bg(void) { return g_perf < PERF_MIN ? B_GLASS_BUBBLE : CUI_RGBA(0x6a48d0ff); }
static inline cui_color briks_bubble_border(void) { return g_perf >= PERF_MIN ? CUI_RGBA(0xdcc8ffb0) : CUI_RGBA(0x00000000); }
static inline float briks_bubble_radius(float h) { return h * 0.5f; }

/* pílula de status em linha (cabeçalho do chat, chip do tile): só existe se houver texto; vidro de bolha (Liquid Glass) */
static inline void briks_status_pill(cui_ctx *ui, const char *text) {
    if (!text || !text[0]) return;
    cui_box(ui, .dir = CUI_ROW, .pad_x = 12, .pad_y = 5, .radius = briks_bubble_radius(26), .bg = briks_bubble_bg(), .border = 1, .border_color = briks_bubble_border(), .align = CUI_ALIGN_CENTER) {
        cui_text(ui, text, .size = 12, .color = CUI_RGB(0xffffff), .nowrap = true);
    }
}
/* passar o mouse de verdade sobre a caixa `id` (cui_last_hovered não serve para caixas simples; em modo roteiro deixava tooltips presos no topo) */
/* tooltip próprio: registra o texto neste quadro; briks_tip_draw (fim do quadro, dentro da pilha raiz) o desenha ao lado do mouse, dentro da janela.
   Não entra no layout (o cui_tooltip ancorava no "último componente" e ficava preso no topo / empurrava irmãos). */
extern char g_tip[160];
static inline void briks_tip(const char *t) { snprintf(g_tip, sizeof g_tip, "%s", t); }
void briks_tip_draw(cui_ctx *ui);
/* Modais: enquanto houver diálogo/menu aberto, hover e clique por rect nas camadas de BAIXO são ignorados (só vale o que é desenhado dentro do modal). */
__declspec(selectany) bool g_modal_now, g_modal_prev; __declspec(selectany) int g_in_modal;
/* Controles sobrepostos (ex.: botões + e × do painel) registram o retângulo para o próximo quadro: o que está por baixo não recebe hover/clique ali. */
__declspec(selectany) float g_cov_now[8][4], g_cov_prev[8][4]; __declspec(selectany) int g_cov_n_now, g_cov_n_prev;
static inline void briks_cover(float x, float y, float w, float h) { if (g_cov_n_now < 8) { g_cov_now[g_cov_n_now][0] = x; g_cov_now[g_cov_n_now][1] = y; g_cov_now[g_cov_n_now][2] = w; g_cov_now[g_cov_n_now][3] = h; g_cov_n_now++; } }
/* g_cov_own > 0: quem está desenhando É o controle sobreposto (o dono da cobertura); a cobertura vale só para o que está por baixo dele. */
__declspec(selectany) int g_cov_own;
static inline bool briks_covered(cui_ctx *ui) {
    if (g_cov_own > 0) return false;
    cui_vec2 m = cui_mouse_pos(ui);
    for (int i = 0; i < g_cov_n_prev; i++) if (m.x >= g_cov_prev[i][0] && m.y >= g_cov_prev[i][1] && m.x < g_cov_prev[i][0] + g_cov_prev[i][2] && m.y < g_cov_prev[i][1] + g_cov_prev[i][3]) return true;
    return false;
}
#include <stdlib.h>
/* Regra central: com modal aberto (diálogo ou menu), só a camada modal recebe input. BRIKS_TEST_NO_MODAL=1 desliga a regra (só o teste de roteiro usa, para provar que ele falha sem a correção). */
static inline bool briks_modal_open(void) { static int off = -1; if (off < 0) off = getenv("BRIKS_TEST_NO_MODAL") != NULL; return !off && g_modal_prev; }
static inline bool briks_blocked(void) { return briks_modal_open() && g_in_modal == 0; }
static inline bool briks_dlg_begin(cui_ctx *ui, const char *t, bool *open, const cui_dialog_opts *o) {
    bool b = cui_dialog_begin(ui, t, open, o); if (b) { g_modal_now = true; g_in_modal++; } return b;
}
__declspec(selectany) void (*g_hot_modal_draw)(cui_ctx *);   /* definido pelo app (main.c); os testes ficam sem */
void briks_hot_modal_draw(cui_ctx *ui);
static inline void briks_dlg_end(cui_ctx *ui) { if (g_hot_modal_draw) g_hot_modal_draw(ui); g_in_modal--; cui_dialog_end(ui); }
static inline bool briks_menu_begin(cui_ctx *ui, const char *l, const cui_menu_opts *o) { bool b = cui_menu_begin(ui, l, o); if (b) { g_modal_now = true; g_in_modal++; } return b; }
static inline void briks_menu_end(cui_ctx *ui) { g_in_modal--; cui_menu_end(ui); }
static inline bool briks_pop_begin(cui_ctx *ui, const char *id, const cui_popover_opts *o) { bool b = cui_popover_begin(ui, id, o); if (b) { g_modal_now = true; g_in_modal++; } return b; }
static inline void briks_pop_end(cui_ctx *ui) { g_in_modal--; cui_popover_end(ui); }
#undef cui_menu
#define cui_menu(ui, label, ...) CUI__SCOPE_IF(briks_menu_begin((ui), (label), CUI__OPTS(cui_menu_opts, __VA_ARGS__)), briks_menu_end(ui))
#undef cui_popover
#define cui_popover(ui, id, ...) CUI__SCOPE_IF(briks_pop_begin((ui), (id), CUI__OPTS(cui_popover_opts, __VA_ARGS__)), briks_pop_end(ui))
/* Menu de clique direito dos campos de texto (BAS-02): todo cui_input do app passa por briks_input_ex (msgui.c), que oferece
   Desfazer, Recortar, Copiar, Colar e Selecionar tudo. O menu também é modal: nada por baixo recebe clique enquanto está aberto. */
static inline bool briks_ctx_begin(cui_ctx *ui, const char *id) { bool b = cui_context_menu_begin(ui, id); if (b) { g_modal_now = true; g_in_modal++; } return b; }
static inline void briks_ctx_end(cui_ctx *ui) { g_in_modal--; cui_context_menu_end(ui); }
#undef cui_context_menu
#define cui_context_menu(ui, id) CUI__SCOPE_IF(briks_ctx_begin((ui), (id)), briks_ctx_end(ui))
int briks_input_ex(cui_ctx *ui, const char *id, char *buf, size_t cap, const cui_input_opts *o);
#undef cui_input
#define cui_input(ui, id, buf, cap, ...) briks_input_ex((ui), (id), (buf), (cap), CUI__OPTS(cui_input_opts, __VA_ARGS__))
/* Área de transferência do app (msgui.c): com BRIKS_FAKE_CLIPBOARD=1 é uma cópia em memória, nunca a do usuário (testes). */
void briks_clip_install(cui_ctx *ui);                      /* main.c, depois de criar a janela */
void briks_clip_set(const char *utf8);
const char *briks_clip_probe(void);                        /* texto da área falsa ("" se desligada; script.c: expect/clip) */
bool briks_clip_fake(void);
#undef cui_dialog
#define cui_dialog(ui, title, open, ...) CUI__SCOPE_IF(briks_dlg_begin((ui), (title), (open), CUI__OPTS(cui_dialog_opts, __VA_ARGS__)), briks_dlg_end(ui))
/* Texto sempre legível (LAY-11): nenhuma etiqueta abaixo de BRIKS_MIN_TEXT px (antes havia 10 a 11 px espalhados). O piso fica aqui, no cui_text, para valer
   em todos os arquivos; o tamanho do texto ajustável (Configurações > Temas) multiplica por cima (font_scale). size = 0 segue o tema. */
#define BRIKS_MIN_TEXT 12.0f
static inline float briks_text_floor(float size) { return (size > 0 && size < BRIKS_MIN_TEXT) ? BRIKS_MIN_TEXT : size; }
static inline void briks_text_ex(cui_ctx *ui, const char *text, const cui_text_opts *o) { cui_text_opts c = *o; c.size = briks_text_floor(c.size); cui_text_ex(ui, text, &c); }
#undef cui_text
#define cui_text(ui, text, ...) briks_text_ex((ui), (text), CUI__OPTS(cui_text_opts, __VA_ARGS__))
/* account.c: fotos dos amigos no cache local (PRF-03); 0 = sem foto (só iniciais) */
cui_texture friend_photo(const char *user_id);
cui_texture friend_photo_by_name(const app_state_t *s, const char *name);
/* foto de perfil de qualquer pessoa: eu (user_photo), amigo (por id, depois por nome); 0 = só iniciais */
static inline cui_texture person_photo(const app_state_t *s, const char *id, const char *name) {
    if (id && id[0] && !strcmp(id, s->user_id)) return s->user_photo;
    cui_texture t = id && id[0] ? friend_photo(id) : 0;
    return t ? t : (name && name[0] && !strcmp(name, s->user_name)) ? s->user_photo : friend_photo_by_name(s, name);
}
/* accountx.c: data de nascimento do cadastro e dica de login por @nick */
bool birth_parse(const char *t, int *d, int *m, int *y);
int birth_check(const char *txt, int ty, int tm, int td, char *err, size_t cap);   /* 0 = maior de 18; 1 formato, 2 futuro, 3 menor */
void login_hint_save(const char *nick, const char *email);
bool login_hint_lookup(const char *nick, char *email, size_t cap);
/* net_state.c: estado da internet e faixa persistente (BAS-32) */
enum { NET_UNKNOWN = 0, NET_ONLINE, NET_OFFLINE };
int net_status(void);                                                    /* sonda em segundo plano; BRIKS_NET força o estado nos testes */
void net_retry_now(void);                                                /* "Tentar agora": sonda na hora */
void idle_banner(cui_ctx *ui, app_state_t *s);                          /* aviso "sem falar há muito tempo" com Continuar (ui_call.c) */
void net_banner(cui_ctx *ui, app_state_t *s);                            /* pílula no topo: sem conexão, reconectando, restabelecida */
void net_friendly_error(const char *raw, char *out, size_t cap);         /* erro técnico de rede -> frase compreensível */
/* clique do próprio controle sobreposto (ignora a cobertura, respeita modal) */
static inline bool briks_clicked_top(cui_ctx *ui, const char *id) {
    if (briks_blocked()) return false;
    cui_rect r = cui_get_rect(ui, cui_make_id(ui, id)); cui_vec2 m = cui_mouse_pos(ui);
    return cui_mouse_clicked(ui, CUI_MOUSE_LEFT) && r.w > 0 && m.x >= r.x && m.y >= r.y && m.x < r.x + r.w && m.y < r.y + r.h;
}
static inline bool briks_hover_top(cui_ctx *ui, const char *id) {
    if (briks_blocked()) return false;
    cui_rect r = cui_get_rect(ui, cui_make_id(ui, id)); cui_vec2 m = cui_mouse_pos(ui);
    return r.w > 0 && m.x >= r.x && m.y >= r.y && m.x < r.x + r.w && m.y < r.y + r.h;
}
static inline bool briks_hover(cui_ctx *ui, const char *id) {
    if (briks_blocked() || briks_covered(ui)) return false;
    cui_rect r = cui_get_rect(ui, cui_make_id(ui, id)); cui_vec2 m = cui_mouse_pos(ui);
    return r.w > 0 && m.x >= r.x && m.y >= r.y && m.x < r.x + r.w && m.y < r.y + r.h;
}
/* Realce de hover/clique para TODO controle feito de caixa simples: quem chama briks_clicked registra o retângulo sob o mouse (o menor vence);
   briks_tip_draw pinta um véu claro por cima no fim do quadro (mais forte com o botão apertado) e põe o cursor de mão. Não mexe no layout. */
__declspec(selectany) float g_hot[5]; __declspec(selectany) bool g_hot_on;   /* g_hot[4] = raio dos cantos do realce */
/* O véu de realce precisa ter o MESMO raio do controle, senão os cantos dele aparecem para fora de pílulas e círculos.
   Quem desenha um controle com raio diferente de 12 chama briks_clicked_r com o raio real (use h/2 para pílula/círculo). */
static inline bool briks_clicked_r(cui_ctx *ui, const char *id, float radius) {
    if (briks_blocked() || briks_covered(ui)) return false;
    cui_rect r = cui_get_rect(ui, cui_make_id(ui, id)); cui_vec2 m = cui_mouse_pos(ui);
    bool in = r.w > 0 && m.x >= r.x && m.y >= r.y && m.x < r.x + r.w && m.y < r.y + r.h;
    if (in && (!g_hot_on || r.w * r.h < g_hot[2] * g_hot[3])) { g_hot[0] = r.x; g_hot[1] = r.y; g_hot[2] = r.w; g_hot[3] = r.h; g_hot[4] = radius; g_hot_on = true; }
    return in && cui_mouse_clicked(ui, CUI_MOUSE_LEFT);
}
static inline bool briks_clicked(cui_ctx *ui, const char *id) { return briks_clicked_r(ui, id, 12.0f); }
#endif
