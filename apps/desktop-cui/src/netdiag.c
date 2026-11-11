#include "netdiag.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

void stun_build_request(uint8_t out[20], const uint8_t txid[12]) {
    out[0] = 0x00; out[1] = 0x01; out[2] = 0; out[3] = 0;                 /* Binding Request, sem atributos */
    out[4] = 0x21; out[5] = 0x12; out[6] = 0xA4; out[7] = 0x42;           /* magic cookie */
    memcpy(out + 8, txid, 12);
}
bool stun_is_response(const uint8_t *b, size_t n, const uint8_t txid[12]) {
    return n >= 20 && b[0] == 0x01 && b[1] == 0x01 && b[4] == 0x21 && b[5] == 0x12 && b[6] == 0xA4 && b[7] == 0x42 && !memcmp(b + 8, txid, 12);
}
int stun_mapped_port(const uint8_t *b, size_t n) {
    if (n < 20) return 0;
    size_t len = ((size_t)b[2] << 8) | b[3]; if (20 + len > n) len = n - 20;
    for (size_t i = 20; i + 4 <= 20 + len;) {
        unsigned t = ((unsigned)b[i] << 8) | b[i + 1], l = ((unsigned)b[i + 2] << 8) | b[i + 3];
        if (i + 4 + l > n) break;
        if ((t == 0x0020 || t == 0x0001) && l >= 8) { unsigned p = ((unsigned)b[i + 6] << 8) | b[i + 7]; return (int)(t == 0x0020 ? (p ^ 0x2112u) : p); }
        i += 4 + ((l + 3u) & ~3u);
    }
    return 0;
}
bool net_needs_relay(int a, int b) { return a > 0 && b > 0 && a != b; }
net_stats_t net_stats(const double *r, int sent) {
    net_stats_t s = { sent, 0, 0, 0, 100 }; double prev = -1, sum = 0, dj = 0; int nj = 0;
    for (int i = 0; i < sent; i++) if (r[i] >= 0) { s.got++; sum += r[i]; if (prev >= 0) { dj += fabs(r[i] - prev); nj++; } prev = r[i]; }
    if (s.got) s.avg_ms = sum / s.got;
    if (nj) s.jitter_ms = dj / nj;
    s.loss_pct = sent ? 100.0 * (sent - s.got) / sent : 0;
    return s;
}

static netdiag_t G; static CRITICAL_SECTION cs; static volatile LONG cs_init;
static void lock(void) { if (!InterlockedCompareExchange(&cs_init, 1, 0)) InitializeCriticalSection(&cs); EnterCriticalSection(&cs); }
static void unlock(void) { LeaveCriticalSection(&cs); }
netdiag_t netdiag_get(void) { lock(); netdiag_t c = G; unlock(); return c; }

/* Qual rede privada é esta placa? (pela descrição do adaptador) NULL = nenhuma. */
const char *netdiag_vpn_kind(const char *desc) {
    return strstr(desc, "ZeroTier") ? "ZeroTier" : strstr(desc, "Tailscale") ? "Tailscale" : (strstr(desc, "WireGuard") || strstr(desc, "Wintun")) ? "WireGuard" : NULL;
}
void netdiag_vpn_scan(void) {
    { const char *fk = getenv("BRIKS_FAKE_VPN");   /* testes: finge placas "ZeroTier,Tailscale" sem olhar o sistema */
      if (fk) { int n = 0; char names[4][48]; memset(names, 0, sizeof names); char b[120]; snprintf(b, sizeof b, "%s", fk);
                for (char *t = strtok(b, ","); t && n < 4; t = strtok(NULL, ",")) { const char *k = netdiag_vpn_kind(t); if (k) snprintf(names[n++], 48, "%s", k); }
                lock(); G.vpn_n = n; memcpy(G.vpn_names, names, sizeof names); unlock(); return; } }
    ULONG len = 16384, fl = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    IP_ADAPTER_ADDRESSES *a = (IP_ADAPTER_ADDRESSES *)malloc(len);
    if (!a) return;
    if (GetAdaptersAddresses(AF_UNSPEC, fl, NULL, a, &len) == ERROR_BUFFER_OVERFLOW) { free(a); a = (IP_ADAPTER_ADDRESSES *)malloc(len); if (!a) return; GetAdaptersAddresses(AF_UNSPEC, fl, NULL, a, &len); }
    int n = 0; char names[4][48]; memset(names, 0, sizeof names);
    for (IP_ADAPTER_ADDRESSES *p = a; p && n < 4; p = p->Next) {
        if (p->OperStatus != IfOperStatusUp) continue;
        char d[160]; WideCharToMultiByte(CP_UTF8, 0, p->Description, -1, d, sizeof d, NULL, NULL);
        const char *kind = netdiag_vpn_kind(d);
        if (kind) snprintf(names[n++], 48, "%s", kind);
    }
    free(a);
    lock(); G.vpn_n = n; memcpy(G.vpn_names, names, sizeof names); unlock();
}

static DWORD WINAPI run(LPVOID u) {
    (void)u;
    netdiag_vpn_scan();
    { const char *fk = getenv("BRIKS_FAKE_NET");   /* testes: ok | symmetric | blocked | lossy — sem tocar na rede */
      if (fk && fk[0]) {
          double rtt[5] = { 22, 25, 23, 24, 22 }; bool reach = true; int nat = 1, sym = 0;
          if (!strcmp(fk, "symmetric")) sym = 1;
          else if (!strcmp(fk, "blocked")) { reach = false; nat = 0; for (int i = 0; i < 5; i++) rtt[i] = -1; }
          else if (!strcmp(fk, "lossy")) { rtt[1] = -1; rtt[3] = -1; rtt[4] = -1; }
          Sleep(300);
          net_stats_t st = net_stats(rtt, 5);
          lock(); G.st = st; G.reachable = reach; G.nat_checked = nat; G.nat_symmetric = sym; G.running = false; G.done = true; unlock();
          return 0; } }
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    double rtt[5]; int sent = 0; bool reach = false; int port_a = 0, port_b = 0;
    struct addrinfo hints; memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo("stun.l.google.com", "19302", &hints, &res) == 0 && res) {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        DWORD to = 1000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
        for (int i = 0; i < 5; i++) {
            uint8_t tx[12], req[20], buf[256];
            ULONGLONG seed = GetTickCount64() * 2654435761ULL + (ULONGLONG)i * 40503ULL;
            for (int k = 0; k < 12; k++) tx[k] = (uint8_t)((seed >> ((k % 8) * 8)) ^ (unsigned)(i * 31 + k * 17));
            stun_build_request(req, tx);
            LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
            sendto(s, (const char *)req, 20, 0, res->ai_addr, (int)res->ai_addrlen);
            int n = recv(s, (char *)buf, sizeof buf, 0);
            QueryPerformanceCounter(&t1);
            rtt[sent] = (n > 0 && stun_is_response(buf, (size_t)n, tx)) ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart : -1.0;
            if (rtt[sent] >= 0) { reach = true; if (!port_a) port_a = stun_mapped_port(buf, (size_t)n); }
            sent++;
            Sleep(120);
        }
        { /* mesmo socket, outro servidor STUN: a porta pública muda por destino? */
            struct addrinfo *r2 = NULL;
            if (port_a && getaddrinfo("stun1.l.google.com", "19302", &hints, &r2) == 0 && r2) {
                uint8_t tx[12] = { 7, 7, 7, 1, 2, 3, 4, 5, 6, 7, 8, 9 }, req[20], buf[256];
                for (int k = 0; k < 12; k++) tx[k] ^= (uint8_t)(GetTickCount64() >> (k % 5));
                stun_build_request(req, tx);
                sendto(s, (const char *)req, 20, 0, r2->ai_addr, (int)r2->ai_addrlen);
                int n = recv(s, (char *)buf, sizeof buf, 0);
                if (n > 0 && stun_is_response(buf, (size_t)n, tx)) port_b = stun_mapped_port(buf, (size_t)n);
                freeaddrinfo(r2);
            }
        }
        closesocket(s); freeaddrinfo(res);
    } else for (int i = 0; i < 5; i++) rtt[sent++] = -1;
    net_stats_t st = net_stats(rtt, sent);
    lock(); G.st = st; G.reachable = reach; G.nat_checked = (port_a && port_b) ? 1 : 0; G.nat_symmetric = net_needs_relay(port_a, port_b); G.running = false; G.done = true; unlock();
    return 0;
}
void netdiag_start(void) {
    lock(); if (G.running) { unlock(); return; } G.running = true; G.done = false; unlock();
    HANDLE h = CreateThread(NULL, 0, run, NULL, 0, NULL); if (h) CloseHandle(h);
}
