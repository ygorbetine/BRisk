/* 1.4.3: pré-visualização inline de vídeo e áudio nos anexos do chat (Media Foundation: decodifica no próprio PC, nada vai à rede).
 * Um único player por vez (tocar outro para o anterior). Vídeo: miniatura com ▶, toca no próprio balão com barra de progresso;
 * áudio: botão ▶/⏸, barra e tempo. Anexo RECEBIDO só é decodificado depois do clique em ▶ (um arquivo de fora não é aberto sozinho);
 * anexo ENVIADO por mim já mostra a miniatura. Formatos: o que o Windows decodifica (mp4/m4v/mov/wmv/avi; mp3/wav/m4a/aac/wma/flac).
 * Separado de attach.c de propósito: a pré-visualização de imagem (1.4.2) mexe na mesma tela. */
#ifndef BRIKS_MEDIA_PREVIEW_H
#define BRIKS_MEDIA_PREVIEW_H
#include "briks_ui.h"

enum { MEDIA_NONE = 0, MEDIA_VIDEO = 1, MEDIA_AUDIO = 2 };
int  media_kind_of(const char *name);                                  /* pela extensão do nome */
/* desenha a prévia do anexo (largura até max_w); `mine` = enviado por mim (miniatura automática). true se desenhou algo */
bool media_inline(cui_ctx *ui, app_state_t *s, const char *id, const char *path, const char *name, bool mine, float max_w);
void media_frame(cui_ctx *ui);                                         /* todo quadro: para o player se a prévia dele saiu da tela (trocou de conversa) */
void media_shutdown(void);                                             /* ao sair (ou ao sair da conversa): para o som e libera tudo */
int  media_probe(const char *name);                                    /* roteiro de teste: media_playing, media_pos_ms, media_state, media_dur_ms */
void media_script(const char *cmd);                                    /* teste: "play <caminho>", "pause", "seek <ms>" sem cliques */
#endif
