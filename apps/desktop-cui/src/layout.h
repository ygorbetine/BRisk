/* Árvore de divisões do layout de painéis (estilo FancyZones). C puro, sem CUI: testável em tests/smoke.c.
 * Nós: divisão (a | b, horizontal = lado a lado, vertical = um sobre o outro) ou folha (um painel).
 * A árvore sempre preenche a área inteira: não existe espaço vazio nem painel solto. */
#ifndef BRIKS_LAYOUT_H
#define BRIKS_LAYOUT_H
#include <stdbool.h>
#include <stddef.h>

typedef enum { P_SERVERS, P_NAV, P_CHAT, P_FRIENDS, P_MEMBERS, P_VOICE, P_HISTORY, P_COUNT } panel_kind;   /* "chat2" (segunda conversa) saiu na 1.4.1: layouts salvos com ele são migrados em layout_from_json */
typedef enum { Z_LEFT, Z_RIGHT, Z_TOP, Z_BOTTOM, Z_CENTER, Z_NONE } zone_t;

#define LAYOUT_MAX_NODES 32
#define LAYOUT_GAP 10.0f

typedef struct { float x, y, w, h; } lrect;

typedef struct {
    bool used;
    bool leaf;
    int panel;          /* folha: panel_kind */
    bool vertical;      /* divisão: true = a em cima e b embaixo; false = a à esquerda e b à direita */
    float ratio;        /* divisão: parte de a, 0..1 (do espaço sem o vão) */
    bool manual;        /* divisão: o usuário arrastou a divisória; vale o ratio salvo e a largura automática (barra de grupos) não mexe mais */
    int a, b;           /* divisão: filhos */
    int parent;         /* -1 na raiz */
} lnode;

typedef struct { lnode n[LAYOUT_MAX_NODES]; int root; } layout_t;

const char *panel_name(int kind);        /* "chat", "nav"... (também o id no JSON) */
const char *panel_title(int kind);       /* "Conversa", "Menu lateral"... */
int panel_from_name(const char *name);   /* -1 se desconhecido */
float panel_min_w(int kind);
float panel_min_h(int kind);

void layout_app_default(layout_t *l, float width);   /* padrão do app: grupos | menu | conversa | Amigos (>= 1240 px); senão o trio abaixo */
void layout_default(layout_t *l, float width);   /* o layout de hoje: servidores | menu | conversa, para a largura dada */
int layout_find(const layout_t *l, int kind);    /* nó da folha ou -1 */
int layout_count(const layout_t *l);             /* folhas */
bool layout_valid(const layout_t *l);            /* estrutura consistente (usada nos testes e ao carregar) */

/* Calcula o retângulo de todos os nós dentro de area (mínimos respeitados quando couberem). */
void layout_compute(const layout_t *l, lrect area, lrect out[LAYOUT_MAX_NODES]);

bool layout_split(layout_t *l, int target_kind, int new_kind, zone_t side);   /* encaixa new_kind ao lado de target_kind */
bool layout_remove(layout_t *l, int kind);                                    /* o vizinho absorve o espaço; recusa o último painel */
bool layout_move(layout_t *l, int kind, int target_kind, zone_t zone);         /* centro = trocar de lugar */
/* Move a divisória do nó de divisão `split` para pos (coordenada absoluta do eixo), respeitando os mínimos. */
bool layout_resize(layout_t *l, int split, lrect split_area, float pos);

/* Zona de encaixe sob o ponto (x, y) dentro da folha de rect r: bordas = 25%, resto = centro. */
zone_t layout_zone_at(lrect r, float x, float y);
lrect layout_zone_rect(lrect r, zone_t z);       /* pré-visualização da zona */


/* ---- reajuste ao vivo (arraste) ---- */
/* Prévia: copia l e aplica o encaixe de `kind` na zona z de `target_kind` (from_menu = painel novo, centro vira direita).
 * Devolve false (e out = l) se a operação não se aplica. Mesma lógica do soltar, então prévia == resultado. */
bool layout_preview(const layout_t *l, int kind, bool from_menu, int target_kind, zone_t z, layout_t *out);

/* Histerese: só troca a zona prevista depois de o cursor ficar nela por hold_s segundos. */
typedef struct { int cur_target; zone_t cur_zone; int cand_target; zone_t cand_zone; float held; } zone_hold;
void zone_hold_reset(zone_hold *h);
/* Entra com a zona sob o cursor (target -1 = nenhuma: mantém a atual). Devolve true se a zona prevista mudou. */
bool zone_hold_update(zone_hold *h, int target_kind, zone_t z, float dt, float hold_s);

/* Interpolação exponencial de um retângulo até o alvo (tau em segundos). Devolve true enquanto ainda se move. */
bool lrect_step(lrect *cur, lrect target, float dt, float tau);

size_t layout_to_json(const layout_t *l, char *out, size_t cap);
bool layout_from_json(layout_t *l, const char *text);
#endif
