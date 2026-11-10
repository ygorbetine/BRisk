/* Cliente do briks-core (processo filho): IPC por TCP loopback com token, linhas JSON nos dois sentidos.
   Uma thread inicia o processo, conecta e lê; a UI só enfileira comandos e retira eventos (core_next_event) a cada quadro. */
#ifndef BRIKS_CORE_H
#define BRIKS_CORE_H
#include <stdbool.h>
#include <stddef.h>

typedef enum { CORE_STOPPED, CORE_STARTING, CORE_READY, CORE_FAILED } core_status_t;

void core_start(void);                         /* inicia o briks-core.exe ao lado do executável (BRIKS_CORE_EXE troca o caminho) */
void core_stop(void);
core_status_t core_state(void);
const char *core_error(void);
bool core_send(const char *json_line);         /* uma linha JSON (sem \n); false se não conectado */
bool core_next_event(char *out, size_t cap);   /* retira um evento da fila; false se vazia */
#endif
