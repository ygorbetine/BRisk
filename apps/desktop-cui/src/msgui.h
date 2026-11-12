/* Interação com mensagens (estilo Discord): barra de ações ao passar o mouse, menu de contexto, resposta, edição, exclusão,
 * reações, fixadas, favoritas, lembrete, encaminhar. Estado local persistido em %APPDATA%\Briks-CUI\msg_state.json; o protocolo
 * assinado dos eventos (reply/edit/delete/reaction/pin) está em briks-core/src/chat_events.rs. Ver docs/plans/interacao-mensagens.md. */
#ifndef BRIKS_MSGUI_H
#define BRIKS_MSGUI_H
#include "briks_ui.h"

void msgui_demo_seed(void);                                /* BRIKS_DEMO=msgs: m1 com reação, m2 resposta+editada, m3 fixada */
void msgui_demo(bool on);                                   /* BRIKS_DEMO: não lê nem grava o arquivo */
bool msgui_deleted(const char *id);
const char *msgui_text(const msg_t *m);                     /* texto exibido (a versão editada, se houver) */
void msgui_row(cui_ctx *ui, app_state_t *s, const msg_t *m);/* depois do balão (que tem .id = "bub") e da seta: hover e botão direito */
void msgui_arrow(cui_ctx *ui, const msg_t *m);            /* setinha ao lado do balão (só aparece no hover da mensagem) */
void msgui_tags(cui_ctx *ui, const msg_t *m);               /* "(editada)", estrela, fixada, lembrete na linha do autor */
void msgui_quote(cui_ctx *ui, const app_state_t *s, const msg_t *m);   /* citação da resposta, acima do texto */
void msgui_reactions(cui_ctx *ui, const msg_t *m);          /* chips de reação sob o texto */
void msgui_set_view(cui_rect r);                            /* área rolável do chat (a barra não sai dela) */
void msgui_composer(cui_ctx *ui, app_state_t *s);           /* faixa "Respondendo a…" / "Editando mensagem" sobre o campo */
void msgui_submit(app_state_t *s);                          /* Enter/Enviar: confirma a edição ou envia */
void msgui_after_send(const char *new_id);                  /* chat.c: liga a mensagem nova à resposta pendente */
void msgui_emoji_pick(float x, float y, void (*cb)(const char *emoji, void *user), void *user);   /* abre o MESMO seletor completo do chat; cb recebe o emoji escolhido */
bool msgui_emoji_picking(void);
int msgui_emoji_hits(void);
void msgui_overlay(cui_ctx *ui, app_state_t *s);            /* fim do quadro: barra, menu e diálogos */
bool msgui_has_reply(const msg_t *m);                       /* a mensagem responde a outra (não agrupa) */
void msgui_pump(app_state_t *s);                           /* todo quadro: grava na API local os eventos assinados pendentes */
bool msgui_on_event(app_state_t *s, const char *from_fp, bool local, const char *wire);   /* evento verificado pelo core; true se mudou algo visível */
void msgui_pinned_button(cui_ctx *ui, app_state_t *s);      /* cabeçalho do chat: lista de fixadas do canal */
#endif
