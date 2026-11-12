/* Lógica pura da chamada (sem CUI, sem janela): registro do histórico de chamadas, duração por extenso, grade de participantes
 * e indicador de rede. Testada em tests/call_test.c. A interface fica em ui_call.c, ui_history.c e ui_voice.c. */
#ifndef BRIKS_CALLHIST_H
#define BRIKS_CALLHIST_H
#include <stdbool.h>
#include <stddef.h>

#define CALLHIST_MAX 100
typedef struct {
    long long started;        /* epoch (s) em que entrei na sala */
    int duration;             /* segundos na chamada */
    char room[64];            /* sala técnica (para "Ligar de novo") */
    char title[96];           /* nome exibido (grupo/sala) */
    char people[240];         /* quem esteve comigo, separados por ", " */
    int npeople;
    bool shared;              /* transmiti a tela nessa chamada */
} call_rec_t;
typedef struct { bool keep; call_rec_t r[CALLHIST_MAX]; int n; } callhist_t;   /* r[0] = a mais recente */

void callhist_init(callhist_t *h);                           /* vazio, guardando */
bool callhist_add(callhist_t *h, const call_rec_t *rec);     /* false (e nada muda) se "Guardar histórico" está desligado ou a chamada durou 0 s */
void callhist_clear(callhist_t *h);                          /* apaga os registros; mantém a opção */
size_t callhist_to_json(const callhist_t *h, char *out, size_t cap);
bool callhist_from_json(callhist_t *h, const char *text);    /* false = texto inválido (h fica vazio, guardando) */
/* junta um nome à lista "A, B, C" sem repetir; devolve true se entrou */
bool callhist_people_add(char *list, size_t cap, int *n, const char *name);

void call_fmt_duration(int secs, char *out, size_t cap);     /* "45 s", "12 min", "1 h 05 min" */
/* "hoje 21:04", "ontem 21:04", "09/11 21:04", "09/11/2025 21:04"; tudo em horário local já decomposto */
void call_fmt_when(int y, int mo, int d, int hh, int mi, int ny, int nmo, int nd, bool yesterday, char *out, size_t cap);

/* Grade de n blocos numa área w x h: escolhe colunas/linhas que deixam o bloco maior e mais perto de 16:10. */
void call_grid(int n, float w, float h, float gap, int *cols, int *rows);

/* Indicador de rede da chamada, só com o que o app realmente sabe. */
enum { NETQ_OFF, NETQ_CONNECTING, NETQ_GOOD, NETQ_UNSTABLE, NETQ_DOWN };
typedef struct { const char *state; int peers, authed, failed; bool stun_done, stun_ok; double rtt_ms, loss_pct; } netq_in_t;
int call_net_quality(const netq_in_t *in);
const char *call_net_label(int q);                           /* "Rede boa", "Rede instável"... */

/* Transmissão. Aviso "X começou a transmitir · Assistir": só na passagem de desligada para ligada e só para quem NÃO está no modo
 * Transmissão (lá a imagem já aparece). */
bool share_notice_wanted(bool was_on, bool now_on, bool viewing);
/* Zoom de quem assiste. `zoom` = pontos de tela por pixel da imagem (0 = encaixada na moldura); `fit` = o zoom que encaixa.
 * share_zoom_pct: 100 = encaixada. share_zoom_clamp: abaixo do encaixe volta a encaixada (zoom/cx/cy = 0), acima de max_mult x
 * o encaixe limita; devolve true se a imagem ficou ampliada. */
int share_zoom_pct(float zoom, float fit);
bool share_zoom_clamp(float *zoom, float *cx, float *cy, float fit, float max_mult);
int share_ds_steps(int w, int want);   /* quantas metades (0..3) do quadro de largura w ainda ficam >= 1,25x da largura em que aparece (want; 0 = sem redução) */
void share_box_down(const unsigned char *src, int w, int k, int ow, int oh, unsigned char *dst);   /* média de blocos (2^k)^2 de pixels de 4 bytes */
#endif
