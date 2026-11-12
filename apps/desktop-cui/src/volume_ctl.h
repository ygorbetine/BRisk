/* 1.4.4: controle de volume único do Briks, com o comportamento do Windows:
 *  - clicar no ÍCONE do alto-falante muda/desmuda (ao desmutar volta ao volume de antes; o slider mostra 0 e lembra o valor);
 *  - RODA do mouse sobre a linha inteira ajusta em passos (5% por clique, 1% com Ctrl, 10% com Shift) e não rola o painel por trás;
 *  - arrastar e as setas do teclado (slider focado) seguem como no slider normal.
 * Todo slider de volume/ganho do app passa por aqui (menu do bloco, painel do participante, transmissão assistida, Configurações, chamada, sistema). */
#ifndef BRIKS_VOLUME_CTL_H
#define BRIKS_VOLUME_CTL_H
#include "briks_ui.h"

enum { VOLCTL_VOL = 1, VOLCTL_MUTE = 2 };   /* o que mudou neste quadro */
/* id: único na tela; vol: valor em [lo,hi]; pct_unit: quanto vale 1% no valor (1.0 para escalas 0..100/0..200, 0.01 para 0..2);
 * muted: NULL = o mudo é o volume 0 (a memória do valor anterior fica aqui); senão, o flag de mudo do chamador (o volume é preservado);
 * show_pct: escreve o percentual à direita. Devolve a máscara de VOLCTL_*. */
int briks_volume(cui_ctx *ui, const char *id, float *vol, float lo, float hi, float pct_unit, bool *muted, bool show_pct);
/* teste: aplica a roda como o widget (delta em "cliques", mods = CUI_MOD_*), e devolve o novo valor */
float briks_volume_wheel_apply(float cur, float lo, float hi, float pct_unit, float clicks, int mods);
#endif
