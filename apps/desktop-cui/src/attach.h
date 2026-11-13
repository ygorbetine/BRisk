/* Anexos P2P, pré-visualização de links, estados de envio, "digitando" e busca simples no chat (CUI).
 * O trabalho de rede/segurança está no briks-core (filetransfer.rs, linkpreview.rs); aqui ficam só a tabela de estado e a UI.
 * Ver docs/plans/interacao-mensagens.md. */
#ifndef BRIKS_ATTACH_H
#define BRIKS_ATTACH_H
#include "briks_ui.h"
#include "json.h"

/* eventos do core: "file", "link.preview", "chat.typing", "chat.ack"; true se mudou algo visível */
bool attach_event(app_state_t *s, const char *ev, const jval *j);
int  attach_queued_fp(int n, char *out, size_t cap);                    /* n-ésimo amigo (fp) com anexo na fila local; 0 se não há */
bool attach_error_open(void);                                       /* o diálogo "Não foi possível enviar" está aberto (roteiro) */
void attach_pick(app_state_t *s);                                  /* abre o seletor de arquivo e oferece aos peers da sala */
void attach_strip(cui_ctx *ui, app_state_t *s);                    /* faixa de transferências acima do campo de texto */
void attach_overlay(cui_ctx *ui, app_state_t *s);                  /* fim do quadro: confirmação de "abrir arquivo recebido" */
void attach_link_card(cui_ctx *ui, app_state_t *s, const char *text);   /* cartão de pré-visualização do primeiro link do texto */
void attach_typing_ping(app_state_t *s);                           /* o rascunho mudou: avisa os peers (no máximo a cada 3 s) */
const char *attach_typing_label(const app_state_t *s);             /* "Nome está digitando…" ou NULL */
void attach_search_button(cui_ctx *ui);                            /* cabeçalho do chat: botão e campo de busca */
bool attach_search_is_on(void);                                         /* roteiro: a busca da conversa está aberta? */
bool attach_search_match(const char *text);                        /* true se não há busca ou o texto contém o termo */
enum { ATT_SENDING = 1, ATT_SENT, ATT_FAILED, ATT_DELIVERED, ATT_WIRE };   /* relógio, check (só neste PC), alerta, check duplo (entregue), check claro (enviada: saiu pelo canal direto) */
void attach_mark(const char *msg_id, int state);                   /* estado de envio de uma mensagem minha */
int  attach_state(const char *msg_id);
void attach_state_tag(cui_ctx *ui, app_state_t *s, const msg_t *m);   /* ícone de 12 px: relógio (enviando), check (enviada), check duplo (entregue), alerta (falhou; clique reenvia) */
void attach_script_event(const char *json_line);                  /* teste: entrega um evento do core à UI (chat.message, chat.sent, chat.ack, dm.wake…) */
void attach_script_state(app_state_t *s_or_null, int n, const char *state);   /* teste: força o estado da n-ésima mensagem minha, de trás para a frente ("ack" simula o chat.ack do core) */
void attach_rich(cui_ctx *ui, const char *id, const char *text);   /* texto da mensagem: **negrito**, `código`, links clicáveis, menções */
void attach_mentions(cui_ctx *ui, app_state_t *s);                 /* sugestões de @menção acima do campo de texto */
bool attach_grouped(const app_state_t *s, int idx);                /* a mensagem idx continua a anterior (mesmo autor, < 5 min, sem resposta) */
void attach_timeline(cui_ctx *ui, app_state_t *s, int *cur, const char *until);   /* anexos concluídos desta conversa, na ordem do horário (until NULL = o resto) */
void attach_demo(app_state_t *s);                                  /* BRIKS_DEMO=msgs: dados de exemplo para capturas, sem rede */
void attach_set_inflight(const char *msg_id);                      /* chat.c: mensagem cuja gravação acabou de começar */
void attach_inflight_done(app_state_t *s, bool ok);                /* app.c: resultado do OP_MSG_SEND */

/* BAS-04: texto da mensagem selecionável com o mouse (só mensagens sem formatação; as com link/negrito/código continuam em markdown) */
cui_id attach_text_id(cui_ctx *ui, const char *msg_id);              /* id do texto selecionável da mensagem (mesmo escopo de id do attach_rich) */
bool attach_selected_text(cui_ctx *ui, const char *msg_id, const char *text, char *out, size_t cap);   /* trecho marcado e com foco, copiado de `text` */
/* BAS-23/24: anexos preparados (arquivo solto ou imagem colada), com cartão acima do campo de texto */
bool attach_paste_image(cui_ctx *ui);                              /* há imagem na área de transferência (e nenhum texto)? vira anexo preparado; true se pegou */
void attach_paste_key(cui_ctx *ui, cui_id field);                  /* Ctrl+V com o campo em foco: imagem vira anexo (o texto o próprio campo cola) */
void attach_dropzone(cui_ctx *ui, cui_rect panel);                 /* "Solte para enviar" sobre o painel enquanto arrastam arquivos pela janela */
const char *attach_last_open(void);                                /* último link aberto por clique (teste) */
int  attach_probe(const char *name);                               /* roteiro de teste: pending, staged_img, search_on... */
void attach_fake_image(const char *path);                          /* testes: imagem "na área de transferência" (só com BRIKS_FAKE_CLIPBOARD) */
/* BAS-08: Ctrl+F busca na conversa; Ctrl+K paleta (amigos, grupos e mensagens) */
void attach_search_open(void);
void attach_palette_keys(cui_ctx *ui);                             /* atalhos Ctrl+F / Ctrl+K (todo quadro) */
void attach_palette(cui_ctx *ui, app_state_t *s);                  /* diálogo da paleta */
#endif
