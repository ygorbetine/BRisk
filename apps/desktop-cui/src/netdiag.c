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

void netdiag_vpn_scan(void) {
    ULONG len = 16384, fl = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    IP_ADAPTER_ADDRESSES *a = (IP_ADAPTER_ADDRESSES *)malloc(len);
    if (!a) return;
    if (GetAdaptersAddresses(AF_UNSPEC, fl, NULL, a, &len) == ERROR_BUFFER_OVERFLOW) { free(a); a = (IP_ADAPTER_ADDRESSES *)malloc(len); if (!a) return; GetAdaptersAddresses(AF_UNSPEC, fl, NULL, a, &len); }
    int n = 0; char names[4][48]; memset(names, 0, sizeof names);
    for (IP_ADAPTER_ADDRESSES *p = a; p && n < 4; p = p->Next) {
        if (p->OperStatus != IfOperStatusUp) continue;
        char d[160]; WideCharToMultiByte(CP_UTF8, 0, p->Description, -1, d, sizeof d, NULL, NULL);
        const char *kind = strstr(d, "ZeroTier") ? "ZeroTier" : strstr(d, "Tailscale") ? "Tailscale" : (strstr(d, "WireGuard") || strstr(d, "Wintun")) ? "WireGuard" : NULL;
        if (kind) snprintf(names[n++], 48, "%s", kind);
    }
    free(a);
    lock(); G.vpn_n = n; memcpy(G.vpn_names, names, sizeof names); unlock();
}

static DWORD WINAPI run(LPVOID u) {
    (void)u;
    netdiag_vpn_scan();
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    double rtt[5]; int sent = 0; bool reach = false;
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
            if (rtt[sent] >= 0) reach = true;
            sent++;
            Sleep(120);
        }
        closesocket(s); freeaddrinfo(res);
    } else for (int i = 0; i < 5; i++) rtt[sent++] = -1;
    net_stats_t st = net_stats(rtt, sent);
    lock(); G.st = st; G.reachable = reach; G.running = false; G.done = true; unlock();
    return 0;
}
void netdiag_start(void) {
    lock(); if (G.running) { unlock(); return; } G.running = true; G.done = false; unlock();
    HANDLE h = CreateThread(NULL, 0, run, NULL, 0, NULL); if (h) CloseHandle(h);
}
