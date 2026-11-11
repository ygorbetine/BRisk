/* Configurações > Armazenamento: medir, limpar anexos, apagar o histórico guardado e exportar contatos e ajustes.
 * Pastas (iguais às do core e do backend; as variáveis BRIKS_* servem aos testes):
 *   dados   %APPDATA%\Briks-CUI (BRIKS_CUI_DATA)        contas, perfil, ajustes, risk.sqlite3 (mensagens e grupos)
 *   hist    %LOCALAPPDATA%\Briks-CUI\historico (BRIKS_HIST_DIR)   histórico sincronizado entre pessoas (core)
 *   recv    %LOCALAPPDATA%\Briks-CUI\recebidos (BRIKS_RECV_DIR)   arquivos recebidos por transferência (core) */
#ifndef BRIKS_STORAGE_H
#define BRIKS_STORAGE_H
#include <stdbool.h>
#include <stddef.h>

typedef struct { unsigned long long msgs, hist, files, other, total; } store_sizes_t;   /* msgs = banco local (mensagens, grupos, amigos) */

void storage_dirs(char *data, char *hist, char *recv, size_t cap);
void storage_measure(store_sizes_t *o);
void storage_fmt(unsigned long long bytes, char *out, size_t cap);
/* Apaga os arquivos recebidos (só dentro da pasta de recebidos) e tira os cartões deles das conversas. Devolve quantos arquivos saíram. */
int storage_clear_attachments(unsigned long long *freed);
/* Apaga o histórico sincronizado guardado pelo core (pasta historico). Devolve quantos arquivos saíram. */
int storage_clear_history_files(unsigned long long *freed);
/* true se `path` fica dentro de `dir` (sem sair por ..). Usado para nunca apagar fora das pastas do Briks. */
bool storage_inside(const char *dir, const char *path);
#endif
