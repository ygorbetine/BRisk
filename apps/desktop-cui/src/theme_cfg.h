/* Tema editável do Briks (theme.json, hot reload e editor Ctrl+Shift+T). Ver docs/plans/editor-tema.md.
   Todos os valores passam por briks_theme_validate(): faixas que nunca geram canto reto nem texto ilegível. */
#ifndef BRIKS_THEME_CFG_H
#define BRIKS_THEME_CFG_H
#include <cui/cui.h>
#include <stdbool.h>

typedef struct {
    /* fundo */
    cui_color bg;                    /* base atrás das manchas */
    cui_color blob[3];               /* manchas: topo-esquerdo, topo-direito, base */
    float blob_a[3];                 /* opacidade de cada mancha, 0..1 */
    /* vidro */
    cui_color glass_rgb;             /* cor do tint dos painéis (escura) */
    float a_glass, a_soft, a_strong, a_popup;   /* opacidade do tint por camada, 0.2..0.95 */
    float blur_offset;               /* raio do desfoque (passo do filtro Kawase), 0.5..4 */
    float saturation;                /* saturação do que passa pelo vidro, 0.5..2.5 */
    float highlight;                 /* luz de borda do vidro, 0..0.2 */
    /* painéis e controles */
    float panel_radius;              /* raio dos painéis, 8..32 */
    float control_radius;            /* raio de botões e campos, 4..20 */
    float chat_pad;                  /* espaço interno da lista de mensagens, 8..40 */
    /* balões */
    cui_color bubble_own;            /* balão enviado (RGB; alpha fixo 0xbd) */
    float bubble_big, bubble_small;  /* raio normal e da quina reta, 2..28 / 2..12 */
    /* derivados (briks_theme_apply) */
    cui_color glass, glass_soft, glass_strong, glass_popup;
} briks_theme_t;

extern briks_theme_t g_th;

void briks_theme_defaults(briks_theme_t *t);
void briks_theme_validate(briks_theme_t *t);              /* trava nas faixas e recalcula os derivados */
bool briks_theme_load(const char *path, briks_theme_t *t);  /* parte dos padrões e sobrepõe o que o arquivo definir */
bool briks_theme_save(const char *path, const briks_theme_t *t);
const char *briks_theme_path(void);                        /* BRIKS_THEME_FILE ou %APPDATA%\Briks-CUI\theme.json */
bool briks_theme_poll(void);                               /* true quando o arquivo mudou e o tema foi recarregado (1 stat a cada 0,4 s) */
void briks_theme_editor(cui_ctx *ui);                      /* Ctrl+Shift+T com BRIKS_DEV=1: painel de ajuste */

/* cores derivadas (usadas por B_GLASS* em briks_ui.h) */
#define B_GLASS        (g_th.glass)
#define B_GLASS_SOFT   (g_th.glass_soft)
#define B_GLASS_STRONG (g_th.glass_strong)
#define B_GLASS_POPUP  (g_th.glass_popup)
#endif
