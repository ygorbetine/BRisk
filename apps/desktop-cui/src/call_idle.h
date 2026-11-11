/* CHM-16: desconexão da chamada depois de 1 h sem falar, com aviso nos 5 minutos finais e botão Continuar (paridade com services/call/idle.ts).
 * Só funções puras (testáveis sem janela); o relógio e o banner ficam em rtc.c / ui_call.c. BRIKS_IDLE_LIMIT_S / BRIKS_IDLE_WARN_S encurtam os
 * prazos nos testes ocultos. */
#ifndef BRIKS_CALL_IDLE_H
#define BRIKS_CALL_IDLE_H
#include <stdlib.h>
enum { IDLE_ACTIVE = 0, IDLE_WARNING = 1, IDLE_EXPIRED = 2 };
static inline long long idle_limit_ms(void) { const char *e = getenv("BRIKS_IDLE_LIMIT_S"); return e && atoi(e) > 0 ? (long long)atoi(e) * 1000 : 3600000LL; }
static inline long long idle_warn_ms(void) { const char *e = getenv("BRIKS_IDLE_WARN_S"); return e && atoi(e) > 0 ? (long long)atoi(e) * 1000 : 300000LL; }
/* fase para `idle_ms` sem falar; *remaining_ms recebe o que falta para desconectar */
static inline int idle_phase(long long idle_ms, long long *remaining_ms) {
    long long rem = idle_limit_ms() - (idle_ms > 0 ? idle_ms : 0); if (rem < 0) rem = 0;
    if (remaining_ms) *remaining_ms = rem;
    if (rem == 0) return IDLE_EXPIRED;
    return rem <= idle_warn_ms() ? IDLE_WARNING : IDLE_ACTIVE;
}
#endif
