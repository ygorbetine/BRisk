/* CHM-16: desconexão da chamada depois de 1 h sem falar, com aviso nos 5 minutos finais e botão Continuar (paridade com services/call/idle.ts).
 * Só funções puras (testáveis sem janela); o relógio e o banner ficam em rtc.c / ui_call.c. BRIKS_IDLE_LIMIT_S / BRIKS_IDLE_WARN_S encurtam os
 * prazos nos testes ocultos. */
#ifndef BRIKS_CALL_IDLE_H
#define BRIKS_CALL_IDLE_H
#include <stdlib.h>
enum { IDLE_ACTIVE = 0, IDLE_WARNING = 1, IDLE_EXPIRED = 2 };
static inline long long idle_limit_ms(void) { const char *e = getenv("BRIKS_IDLE_LIMIT_S"); return e && atoi(e) > 0 ? (long long)atoi(e) * 1000 : 3600000LL; }
static inline long long idle_warn_ms(void) { const char *e = getenv("BRIKS_IDLE_WARN_S"); return e && atoi(e) > 0 ? (long long)atoi(e) * 1000 : 300000LL; }
/* CHM-35 (1.4.3): o prazo vem de Configurações > Voz e vídeo (minutos; 0 = nunca desconectar). BRIKS_IDLE_LIMIT_S continua mandando nos testes.
 * Devolve 0 para "nunca". O aviso é de 5 min, mas nunca passa de 1/3 do prazo (prazo de 15 min avisa aos 5; de 5 min avisaria aos ~1:40). */
static inline long long idle_limit_for(int cfg_minutes) {
    const char *e = getenv("BRIKS_IDLE_LIMIT_S"); if (e && atoi(e) > 0) return (long long)atoi(e) * 1000;
    if (cfg_minutes <= 0) return 0;
    return (long long)cfg_minutes * 60000LL;
}
static inline long long idle_warn_for(long long limit_ms) { long long w = idle_warn_ms(); return w > limit_ms / 3 && !getenv("BRIKS_IDLE_WARN_S") ? limit_ms / 3 : w; }
/* fase para `idle_ms` sem falar com prazo `limit_ms` (0 = nunca); *remaining_ms recebe o que falta para desconectar */
static inline int idle_phase_lim(long long idle_ms, long long limit_ms, long long *remaining_ms) {
    if (limit_ms <= 0) { if (remaining_ms) *remaining_ms = 0; return IDLE_ACTIVE; }
    long long rem = limit_ms - (idle_ms > 0 ? idle_ms : 0); if (rem < 0) rem = 0;
    if (remaining_ms) *remaining_ms = rem;
    if (rem == 0) return IDLE_EXPIRED;
    return rem <= idle_warn_for(limit_ms) ? IDLE_WARNING : IDLE_ACTIVE;
}
static inline int idle_phase(long long idle_ms, long long *remaining_ms) { return idle_phase_lim(idle_ms, idle_limit_ms(), remaining_ms); }
#endif
