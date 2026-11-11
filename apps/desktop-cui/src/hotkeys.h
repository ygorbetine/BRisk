/* Atalhos globais e apertar-para-falar.
 * hotkeys.c: parte pura (texto <-> tecla, PTT), testável sem Win32 de entrada.
 * hotkeys_win.c: registro real (RegisterHotKey numa thread própria), captura de tecla e sonda de "atalho ocupado". */
#ifndef BRIKS_HOTKEYS_H
#define BRIKS_HOTKEYS_H
#include <stdbool.h>
#include <stddef.h>
enum { HK_CTRL = 1, HK_SHIFT = 2, HK_ALT = 4 };
/* "Ctrl+Shift+M", "Alt+F5", "Mouse4", "Espaço"... -> modificadores + código de tecla virtual do Windows. false = inválido/vazio. */
bool hk_parse(const char *acc, unsigned *mods, int *vk);
/* Inverso: modificadores + tecla -> texto que hk_parse aceita de volta ("Ctrl+Shift+M"). false = tecla sem nome conhecido. */
bool hk_format(unsigned mods, int vk, char *out, size_t cap);
/* Dois textos de atalho valem o mesmo combo (ignora caixa, espaços e ordem)? Vazios/inválidos nunca são iguais. */
bool hk_same(const char *a, const char *b);
/* Teclas do mouse (botão do meio e laterais) não existem em RegisterHotKey: são lidas por varredura. */
bool hk_is_mouse(int vk);
/* Apertar para falar: aberto enquanto a tecla está apertada e por `delay_ms` depois de soltar. */
typedef struct { bool held; double released_at; bool open; } hk_ptt_t;
bool hk_ptt_step(hk_ptt_t *p, bool down_now, double now_ms, double delay_ms);   /* devolve se o microfone deve estar aberto */

/* ---- registro real (hotkeys_win.c) ---- */
#define HK_N 3
enum { HKS_NONE = 0, HKS_OK = 1, HKS_BUSY = 2, HKS_POLLED = 3 };   /* sem atalho / registrado / outro programa usa / mouse: lido por varredura */
/* Troca o conjunto de atalhos globais registrados (acc[i] vazio = nenhum). status[i] (opcional) devolve o resultado de cada um. */
void hkw_set(const char *const acc[HK_N], int status[HK_N]);
/* Quantas vezes o atalho i foi apertado desde a última chamada. */
unsigned hkw_take(int i);
/* Desregistra tudo e encerra a thread. */
void hkw_shutdown(void);
/* Registra e desregistra na hora, só para saber se o combo está livre. Mouse conta como livre. */
bool hkw_probe(unsigned mods, int vk);
/* Captura de tecla para os campos de atalho: begin() arma; poll() devolve 1 e escreve o texto quando alguém aperta um combo válido, 2 se cancelou (Esc), 0 se ainda espera. */
void hkw_capture_begin(bool allow_mouse);
int hkw_capture_poll(char *out, size_t cap);
void hkw_capture_inject(const char *acc);   /* roteiros de teste: finge que essa combinação foi apertada */
#endif
