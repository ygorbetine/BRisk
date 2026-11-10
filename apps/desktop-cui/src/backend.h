/* Sidecar do backend local (briks-desktop-backend) e chamadas HTTP assíncronas. Só Windows por enquanto. */
#ifndef BRIKS_BACKEND_H
#define BRIKS_BACKEND_H
#include <stdbool.h>

typedef enum { BK_STOPPED, BK_STARTING, BK_READY, BK_FAILED } bk_status_t;

void bk_start(void);                   /* inicia o sidecar em segundo plano */
void bk_stop(void);                    /* encerra o sidecar */
bk_status_t bk_status(void);
const char *bk_error(void);            /* motivo quando BK_FAILED */

typedef struct bk_call bk_call;
/* Envia a requisição em uma thread. body pode ser NULL; bearer pode ser NULL. */
bk_call *bk_request(const char *method, const char *path, const char *body, const char *bearer);
bool bk_done(const bk_call *c);
int bk_http_status(const bk_call *c);  /* 0 = falha de rede; só vale após bk_done */
const char *bk_body(const bk_call *c); /* JSON da resposta ("" se vazio) */
void bk_free(bk_call *c);              /* aguarda a thread terminar, se preciso */
#endif
