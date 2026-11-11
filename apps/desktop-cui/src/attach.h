/* Anexos P2P, pré-visualização de links, estados de envio, "digitando" e busca simples no chat (CUI).
 * O trabalho de rede/segurança está no briks-core (filetransfer.rs, linkpreview.rs); aqui ficam só a tabela de estado e a UI.
 * Ver docs/plans/interacao-mensagens.md. */
#ifndef BRIKS_ATTACH_H
#define BRIKS_ATTACH_H
#include "briks_ui.h"
#include "json.h"

/* eventos do core: "file", "link.preview", "chat.typing", "chat.ack"; true se mudou algo visível */
bool attach_event(app_state_t *s, const char *ev, const jval *j);
void attach_pick(app_state_t *s);                                  /* abre o seletor de arquivo e oferece aos peers da sala */
void attach_strip(cui_ctx *ui, app_state_t *s);                    /* faixa de transferências acima do campo de texto */
void attach_overlay(cui_ctx *ui, app_state_t *s);                  /* fim do quadro: confirmação de "abrir arquivo recebido" */
void attach_link_card(cui_ctx *ui, app_state_t *s, const char *text);   /* cartão de pré-visualização do primeiro link do texto */
void attach_typing_ping(app_state_t *s);                           /* o rascunho mudou: avisa os peers (no máximo a cada 3 s) */
const char *attach_typing_label(const app_state_t *s);             /* "Nome está digitando…" ou NULL */
void attach_search_button(cui_ctx *ui);                            /* cabeçalho do chat: botão e campo de busca */
bool attach_search_match(const char *text);                        /* true se não há busca ou o texto contém o termo */
enum { ATT_SENDING = 1, ATT_SENT, ATT_FAILED };
void attach_mark(const char *msg_id, int state);                   /* estado de envio de uma mensagem minha */
int  attach_state(const char *msg_id);
void attach_state_tag(cui_ctx *ui, app_state_t *s, const msg_t *m);   /* "enviando…" / "não enviada · reenviar" */
void attach_rich(cui_ctx *ui, const char *id, const char *text);   /* texto da mensagem: **negrito**, `código`, links clicáveis, menções */
void attach_mentions(cui_ctx *ui, app_state_t *s);                 /* sugestões de @menção acima do campo de texto */
bool attach_grouped(const app_state_t *s, int idx);                /* a mensagem idx continua a anterior (mesmo autor, < 5 min, sem resposta) */
void attach_timeline(cui_ctx *ui, app_state_t *s, int *cur, const char *until);   /* anexos concluídos desta conversa, na ordem do horário (until NULL = o resto) */
void attach_demo(app_state_t *s);                                  /* BRIKS_DEMO=msgs: dados de exemplo para capturas, sem rede */
void attach_set_inflight(const char *msg_id);                      /* chat.c: mensagem cuja gravação acabou de começar */
void attach_inflight_done(app_state_t *s, bool ok);                /* app.c: resultado do OP_MSG_SEND */
#endif
