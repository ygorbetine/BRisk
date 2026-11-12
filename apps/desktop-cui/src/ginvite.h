/* Convite de grupo pela DM (ginvite.c): cartão com Entrar/Recusar, estados e a fila local das mensagens. */
#ifndef BRIKS_GINVITE_H
#define BRIKS_GINVITE_H
#include "briks_ui.h"

enum { GINV_PENDING = 0, GINV_ACCEPTED, GINV_DECLINED, GINV_EXPIRED, GINV_SENT };

bool ginvite_is(const char *content);                                           /* a mensagem é um convite de grupo? */
/* mensagem de convite recebida/enviada/sincronizada: registra (idempotente por id), guarda o código só neste PC e devolve em `shown` o texto
 * curto (sem o código) que vai para o banco de mensagens. */
bool ginvite_receive(app_state_t *s, const char *id, const char *from_user, const char *content, bool mine, char *shown, size_t cap);
const char *ginvite_summary(const char *content, char *out, size_t cap);        /* "Convite para o grupo X" (avisos do sistema) */
int  ginvite_state(const char *id);                                             /* -1 se não conhece */
int  ginvite_pending_from(const char *user_id);                                 /* convites pendentes (não expirados) deste amigo */
int  ginvite_pending_total(void);
void ginvite_expect(const char *friend_user, const char *gid);                  /* "Convidar": envia pela DM assim que o core assinar o grupo */
void ginvite_signed(app_state_t *s, const char *gid, const char *gname, const char *code);
void ginvite_verified(app_state_t *s, const char *gid, bool ok, const char *msg);   /* resultado do group.verify disparado por Entrar */
bool ginvite_card(cui_ctx *ui, app_state_t *s, const msg_t *m);          /* desenha o cartão no balão; false se não é convite */
#endif
