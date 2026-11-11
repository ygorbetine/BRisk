/* Atalhos globais e apertar-para-falar. Parte pura (sem Win32 de entrada): testável em tests/smoke.c. */
#ifndef BRIKS_HOTKEYS_H
#define BRIKS_HOTKEYS_H
#include <stdbool.h>
enum { HK_CTRL = 1, HK_SHIFT = 2, HK_ALT = 4 };
/* "Ctrl+Shift+M", "Alt+F5", "Mouse4", "Espaço"... -> modificadores + código de tecla virtual do Windows. false = inválido/vazio. */
bool hk_parse(const char *acc, unsigned *mods, int *vk);
/* Apertar para falar: aberto enquanto a tecla está apertada e por `delay_ms` depois de soltar. */
typedef struct { bool held; double released_at; bool open; } hk_ptt_t;
bool hk_ptt_step(hk_ptt_t *p, bool down_now, double now_ms, double delay_ms);   /* devolve se o microfone deve estar aberto */
#endif
