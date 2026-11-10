/* Desenho da pilha levantada (CUI): ver drag_stack_ui.c. A dinâmica fica em drag_stack.c/.h (C puro). */
#ifndef BRIKS_DRAG_STACK_UI_H
#define BRIKS_DRAG_STACK_UI_H
#include "briks_ui.h"
#include "drag_stack.h"

typedef struct {
    float cx[PILE_LAYERS], cy[PILE_LAYERS], ang[PILE_LAYERS], scale, alpha, lift, w, h;
    int kind;
    const app_state_t *s;   /* dados reais para a arte do cartão (nomes, quantidades) */
} drag_stack_draw;

void drag_stack_paint(cui_ctx *ui, cui_rect r, void *user);                   /* callback de cui_canvas */
void drag_stack_art(cui_ctx *ui, const app_state_t *s, int kind, cui_rect c);  /* arte do cartão da frente (uma por tipo de painel) */
void drag_stack_set_demo(int voice_people, int servers);                      /* roteiro/demo: pessoas na call e comunidades (-1 = estado real) */
void drag_stack_set_art(int kind);                                              /* força a arte de um tipo (galeria de testes); -1 = o painel real */
#endif
