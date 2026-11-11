/* Diagnóstico de rede (Configurações > Rede): teste do STUN (atraso, variação, perda) e detecção de redes privadas (ZeroTier/Tailscale/WireGuard).
 * Parte pura (pacote STUN, estatística) testável em tests/smoke.c; o resto roda numa thread. */
#ifndef BRIKS_NETDIAG_H
#define BRIKS_NETDIAG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Binding Request de 20 bytes (cookie 0x2112A442) com o txid dado. */
void stun_build_request(uint8_t out[20], const uint8_t txid[12]);
/* É uma resposta de sucesso a esse txid (Binding Success 0x0101, cookie e txid iguais)? */
bool stun_is_response(const uint8_t *buf, size_t len, const uint8_t txid[12]);
typedef struct { int sent, got; double avg_ms, jitter_ms, loss_pct; } net_stats_t;
net_stats_t net_stats(const double *rtt_ms, int sent);   /* rtt < 0 = sem resposta; jitter = média das diferenças consecutivas */
typedef struct { bool running, done, reachable; net_stats_t st; int vpn_n; char vpn_names[4][48]; } netdiag_t;
void netdiag_start(void);                 /* dispara o teste em segundo plano (ignora se já roda) */
netdiag_t netdiag_get(void);              /* cópia do estado atual */
void netdiag_vpn_scan(void);              /* só a detecção de redes privadas */
#endif
