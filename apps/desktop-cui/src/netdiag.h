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
/* Porta pública que o STUN viu (XOR-MAPPED-ADDRESS ou MAPPED-ADDRESS); 0 se a resposta não traz. */
int stun_mapped_port(const uint8_t *buf, size_t len);
/* Dois servidores STUN viram portas diferentes para o MESMO socket = NAT que muda a porta por destino (simétrico): a conexão direta falha, precisa de relay. */
bool net_needs_relay(int port_a, int port_b);
typedef struct { bool running, done, reachable; net_stats_t st; int vpn_n; char vpn_names[4][48]; int nat_checked, nat_symmetric; } netdiag_t;   /* nat_checked: 1 = dois STUN responderam */
void netdiag_start(void);                 /* dispara o teste em segundo plano (ignora se já roda) */
netdiag_t netdiag_get(void);              /* cópia do estado atual */
const char *netdiag_vpn_kind(const char *desc);   /* ZeroTier | Tailscale | WireGuard | NULL */
void netdiag_vpn_scan(void);              /* só a detecção de redes privadas */
#endif
