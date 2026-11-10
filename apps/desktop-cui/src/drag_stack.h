/* Dinâmica da "pilha levantada" (prévia de arraste): C puro, sem CUI, testável em tests/smoke.c.
 * Três cartões seguem o cursor com atraso diferente (molas), o giro responde à velocidade horizontal,
 * a pilha "levanta" ao pegar (escala + sombra) e desce/encolhe até o slot ao soltar (ou volta à origem no Esc). */
#ifndef BRIKS_DRAG_STACK_H
#define BRIKS_DRAG_STACK_H
#include <stdbool.h>

typedef struct { float x, v; } spring1;
extern int pile_simple;   /* 0 = molas; 1/2 = prévia simples sem mola (modos Simplificado/Mínimo) */
/* Mola amortecida (semi-implícita, em subpassos): stiff = k (1/s²), zeta = razão de amortecimento (1 = crítico, <1 = elástica). */
void spring_step(spring1 *s, float target, float dt, float stiff, float zeta);
/* Inclinação (graus) para a velocidade horizontal vx (px/s): proporcional, limitada a ±max_deg. */
float pile_tilt_deg(float vx);

#define PILE_LAYERS 3
typedef enum { PILE_IDLE, PILE_HELD, PILE_DROP, PILE_CANCEL } pile_phase;

typedef struct {
    pile_phase phase;
    spring1 x[PILE_LAYERS], y[PILE_LAYERS], rot[PILE_LAYERS];   /* centro de cada cartão e giro (graus) */
    spring1 lift;                                               /* 0 = repouso, 1 = levantada (escala 1.04, sombra maior) */
    float vx, last_mx;                                          /* velocidade horizontal suavizada do cursor */
    float t;                                                    /* progresso da descida/volta (0..1) */
    float end_x, end_y;                                         /* destino da descida/volta */
    float from_scale;
    bool have_last;
} pile_t;

/* Deslocamento de cada cartão em relação ao da frente (atrás, meio, frente) e giro de repouso (graus). */
extern const float PILE_OFF_X[PILE_LAYERS], PILE_OFF_Y[PILE_LAYERS], PILE_REST_DEG[PILE_LAYERS], PILE_ALPHA[PILE_LAYERS];

void pile_grab(pile_t *p, float fx, float fy);              /* (fx, fy) = centro do cartão da frente ao pegar */
void pile_follow(pile_t *p, float fx, float fy, float dt);  /* durante o arraste: alvo = centro do cartão da frente */
void pile_release(pile_t *p, float sx, float sy);           /* soltou: desce e encolhe até o centro do slot */
void pile_cancel(pile_t *p, float ox, float oy);            /* Esc: volta ao lugar de origem */
void pile_step(pile_t *p, float dt);                        /* anima descida/volta e as molas (chamar todo quadro) */
bool pile_active(const pile_t *p);                          /* precisa desenhar/redesenhar */
float pile_scale(const pile_t *p);                          /* escala da pilha: 1 + 0.04*lift, encolhendo na descida */
float pile_alpha(const pile_t *p);                          /* 1 em arraste; 1→0 na descida/volta */

/* ---- lógica pura da arte do cartão da frente (testada no smoke) ---- */
/* Tamanho do cartão: proporção do painel (pw x ph) com limites; servidores e voz ficam mais largos para caberem as grades. */
void stack_card_size(int panel_kind, float pw, float ph, float *w, float *h);
/* Grade de blocos: cells = quadrados desenhados; plus_n > 0 => o último vira "+N". */
typedef struct { int cols, rows, cells, plus_n; } stack_grid;
stack_grid stack_voice_grid(int people);          /* 1 a 3 em uma linha, 4 = 2x2, 5 e 6 = 3x2, mais = 3x2 com "+N" no último */
stack_grid stack_servers_grid(int communities);   /* B + comunidades + "+": grade 6x2 (12 ícones); acima de 12, o 12º vira "+N" */
#endif
