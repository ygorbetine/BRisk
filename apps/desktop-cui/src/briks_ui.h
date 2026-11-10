/* Remake da interface do Briks em CUI. Paleta e medidas vêm de apps/web/src/styles.css e glass-layout.css. */
#ifndef BRIKS_UI_H
#define BRIKS_UI_H
#include <cui/cui.h>
#include <cui/cui_app.h>
#include <stdbool.h>

/* Paleta (--briks-*, --glass-*) */
#define B_C1        CUI_RGB(0xb01fcf)
#define B_C2        CUI_RGB(0x6d5cf6)
#define B_C3        CUI_RGB(0x8b5cf6)
#define B_BG        CUI_RGB(0x12041a)
#define B_BG_DEEP   CUI_RGB(0x0f0316)
#define B_PANEL     CUI_RGB(0x1a0a26)
#define B_LINE      CUI_RGB(0x2f1745)
#define B_TEXT      CUI_RGB(0xedf1f7)
#define B_MUTED     CUI_RGB(0xa592b8)
#define B_DANGER    CUI_RGB(0xe95555)
#define B_ONLINE    CUI_RGB(0x57d28b)
#define B_GLASS       CUI_RGBA(0x220c3480)   /* rgba(34,12,52,.50) */
#define B_GLASS_SOFT  CUI_RGBA(0x220c3457)   /* .34 */
#define B_GLASS_STRONG CUI_RGBA(0x180726bd)  /* rgba(24,7,38,.74) */
#define B_GLASS_POPUP CUI_RGBA(0x180726d9)   /* popups/diálogos: camada de desfoque própria */
#define B_GLASS_BORDER CUI_RGBA(0xffffff1c)  /* .11 */
#define B_GLASS_BORDER_SOFT CUI_RGBA(0xffffff12) /* .07 */

typedef enum { SCREEN_AUTH, SCREEN_SOCIAL, SCREEN_CALL } screen_t;

#define MAX_FRIENDS 128
#define MAX_COMMUNITIES 32
typedef struct { char id[48]; char name[96]; char request_id[48]; } person_t;
typedef struct { char id[48]; char name[96]; } community_t;

typedef enum { OP_NONE, OP_AUTH, OP_ME, OP_FRIENDS, OP_COMMUNITIES, OP_ADD_FRIEND, OP_ACCEPT } op_t;

typedef struct {
    screen_t screen;
    cui_app *app;
    /* conexão com o backend local */
    struct bk_call *call;
    op_t op;
    bool busy;
    char token[512];
    /* auth */
    int auth_mode;                 /* 0 login, 1 criar, 2 redefinir */
    char email[96], password[64], name[64], confirm[64], error[160];
    /* sessão */
    char user_id[48], user_name[96], user_email[96];
    person_t friends[MAX_FRIENDS]; int friend_count;
    person_t pending[MAX_FRIENDS]; int pending_count;
    community_t communities[MAX_COMMUNITIES]; int community_count;
    /* social */
    int active_friend;             /* -1 = lista de amigos */
    int active_space;              /* 0 = início */
    char draft[256];
    bool settings_open, add_friend_open;
    char add_email[96], notice[160];
    /* call */
    bool mic_on, cam_on, sharing;
} app_state_t;

/* app.c: máquina de estados das chamadas ao backend */
void app_submit_auth(app_state_t *s);
void app_add_friend(app_state_t *s);
void app_accept_friend(app_state_t *s, const char *request_id);
void app_logout(app_state_t *s);
void app_pump(app_state_t *s);                 /* chamar uma vez por quadro */

bool briks_glass_init(cui_app *app);        /* glass.c: desfoque real por trás dos painéis B_GLASS* */
void briks_glass_shutdown(void);
void briks_apply_theme(cui_ctx *ui);
void briks_ambient(cui_ctx *ui, float strength);          /* fundo ambiente das manchas de cor */
void briks_brand(cui_ctx *ui, float size);
void briks_gradient_bar(cui_ctx *ui);                     /* faixa de 3px do topo */
void ui_auth(cui_ctx *ui, app_state_t *s);
void ui_social(cui_ctx *ui, app_state_t *s);
void ui_call(cui_ctx *ui, app_state_t *s);
#endif
