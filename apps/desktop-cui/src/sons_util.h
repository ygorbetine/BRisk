/* Painel de sons: funções puras (sem janela, sem Windows), testadas em tests/sons_test.c. */
#ifndef BRIKS_SONS_UTIL_H
#define BRIKS_SONS_UTIL_H
#include <stdbool.h>
#include <stddef.h>

bool sons_clean_name(const char *in, char *out, size_t cap);                     /* nome do som: sem controles, aparado, até 32 caracteres; false se vazio */
void sons_label(const char *emoji, const char *name, int max_chars, char *out, size_t cap);   /* "📯 Buzina…" cortado em max_chars caracteres (UTF-8 seguro) */
float sons_peak_gain(const short *pcm, size_t nsamples, float volume01);        /* ganho que limita o pico a -3 dBFS e aplica o volume do painel (nunca amplifica) */
bool sons_match(const char *name, const char *query);                            /* busca sem diferenciar maiúsculas/acentos simples; query vazia casa tudo */
const char *sons_fmt_dur(unsigned ms, char *out, size_t cap);                    /* "1,5 s" */

#endif
