/* Painel de sons (soundboard) do Briks nativo. Lado da UI: índice espelhado do core (sound.list), painel da chamada, diálogos de
 * adicionar/renomear/apagar, catálogo do grupo (publicado no manifesto assinado, ver ui_groups.c) e reprodução local.
 * O core guarda os arquivos por hash, transfere pela rede e valida o disparo; aqui só se toca (Media Foundation + waveOut) e se desenha.
 * Plano: docs/plans/painel-de-sons.md. */
#ifndef BRIKS_SONS_H
#define BRIKS_SONS_H
#include <stdbool.h>
#include <stddef.h>
#include "json.h"
#include "sons_util.h"
#include "briks_ui.h"

/* eventos do core (sound.*) e ligação com o resto do app */
void sons_core_event(app_state_t *s, const jval *j, const char *ev);
void sons_on_room_joined(app_state_t *s);                 /* sala aberta: envia a lista de membros do grupo e espelha os catálogos */
void sons_catalog_sync(const char *snapshot_json);        /* um snapshot de grupo confiado foi gravado/aplicado: o core espelha o catálogo */
char *sons_group_catalog_json(const char *gid, const char *stored_snapshot_json);   /* "[...]" para o snapshot a assinar (malloc; NULL = sem sons) */
void sons_note_snapshot(const char *gid, const jval *snapshot);                      /* guarda o catálogo do snapshot confiado (base das edições do administrador) */
void sons_ask_missing(void);                              /* um par autenticou: pede os arquivos que faltam */

/* interface (chamadas pela barra e pela tela da chamada) */
void sons_popover(cui_ctx *ui, app_state_t *s);           /* popover "Sons" (dentro do escopo da pílula) */
void sons_dialogs(cui_ctx *ui, app_state_t *s);           /* diálogos (fora do popover) */
bool sons_panel_open(void);
void sons_stop_all(void);                                 /* sair da chamada / fechar: para o que estiver tocando */
void sons_shutdown(void);

/* roteiros de teste (BRIKS_SCRIPT): abre o painel direto, injeta um item local, toca sem rede */
void sons_script(app_state_t *s, const char *cmd, const char *arg);
int sons_probe(const char *name);                         /* estados para `expect` dos roteiros (-1 = desconhecido) */

/* implementados em ui_groups.c */
void groups_republish(app_state_t *s, const char *gid);   /* reassina o snapshot do grupo (dono/administrador), levando o catálogo de sons atual */
char *groups_room_roster(app_state_t *s, const char *room, char *gid, size_t gcap);   /* sala de voz de um grupo: JSON ["fp",...] dos membros (malloc) e o id do grupo; NULL se a sala não é de grupo */
#endif
