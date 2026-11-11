/* Texto das mensagens como no chat do Electron (SafeMessageContent.tsx): **negrito**, `código`, [texto](https://…), links soltos,
 * menções @nome / @[Nome com espaço] e quebras de linha; todo o resto é texto puro (nada de títulos, listas ou HTML).
 * Funções puras (sem CUI): o resultado vai para o cui_markdown. */
#ifndef BRIKS_RICHTEXT_H
#define BRIKS_RICHTEXT_H
#include <stdbool.h>
#include <stddef.h>

/* Converte o texto da mensagem em markdown seguro para o cui_markdown. Devolve os bytes escritos (sem o NUL). */
size_t rich_prepare(const char *in, char *out, size_t cap);
/* http(s) até 2048 bytes, sem espaços, controles nem < > ". */
bool rich_safe_url(const char *url);
/* Links http(s) do texto (soltos ou em [texto](url)), sem repetir; devolve quantos. */
int rich_urls(const char *text, char urls[][220], int max);
/* O rascunho termina em "@consulta" (início ou depois de espaço)? Copia a consulta; `*start` = posição do '@'. */
bool rich_mention_query(const char *draft, char *query, size_t cap, int *start);
/* Troca o "@consulta" final por "@[nome] ". Devolve false se não coube. */
bool rich_apply_mention(char *draft, size_t cap, const char *name);
/* Agrupa com a mensagem anterior (mesmo autor, sem resposta, menos de 5 minutos). */
bool rich_group_with_prev(const char *prev_author, long long prev_ms, const char *author, long long ms, bool has_reply);
/* "2026-11-09T23:41:00.000Z" -> milissegundos desde 1970 (UTC); -1 se inválido. */
long long rich_iso_ms(const char *iso);
/* O texto menciona esta pessoa? "@nick", "@nome" ou "@[Nome com espaço]" (sem diferenciar maiúsculas); null/vazio é ignorado. */
bool rich_mentions_me(const char *text, const char *name, const char *nick);
#endif
