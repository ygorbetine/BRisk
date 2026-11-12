#include "core.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <process.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define Q_CAP 256
#define LINE_MAX_BYTES 65536

static volatile LONG g_state = CORE_STOPPED;
static char g_error[256];
static SOCKET g_sock = INVALID_SOCKET;
static HANDLE g_job, g_proc;
static bool g_in_job;   /* o core entrou no Job Object (falha se a UI já roda dentro de um job sem aninhamento) */
static CRITICAL_SECTION g_lock;
static bool g_lock_init;
static char *g_q[Q_CAP];
static int g_qh, g_qt;

static volatile LONG g_proto;
int core_proto(void) { return (int)g_proto; }
void core_set_proto(int p) { InterlockedExchange(&g_proto, p); }
core_status_t core_state(void) { return (core_status_t)g_state; }
const char *core_error(void) { return g_error; }

static void fail(const char *msg) {
    snprintf(g_error, sizeof g_error, "%s", msg);
    InterlockedExchange(&g_state, CORE_FAILED);
}

static void push_line(const char *line, size_t n) {
    char *c = malloc(n + 1);
    if (!c) return;
    memcpy(c, line, n); c[n] = 0;
    EnterCriticalSection(&g_lock);
    int next = (g_qt + 1) % Q_CAP;
    if (next == g_qh) { free(g_q[g_qh]); g_q[g_qh] = NULL; g_qh = (g_qh + 1) % Q_CAP; }   /* fila cheia: descarta o mais antigo */
    g_q[g_qt] = c; g_qt = next;
    LeaveCriticalSection(&g_lock);
}

bool core_next_event(char *out, size_t cap) {
    if (!g_lock_init) return false;
    bool ok = false;
    EnterCriticalSection(&g_lock);
    if (g_qh != g_qt) {
        char *c = g_q[g_qh]; g_q[g_qh] = NULL; g_qh = (g_qh + 1) % Q_CAP;
        snprintf(out, cap, "%s", c); free(c); ok = true;
    }
    LeaveCriticalSection(&g_lock);
    return ok;
}

bool core_send(const char *json_line) {
    if (g_state != CORE_READY || g_sock == INVALID_SOCKET) return false;
    size_t n = strlen(json_line);
    char *buf = malloc(n + 2);
    if (!buf) return false;
    memcpy(buf, json_line, n); buf[n] = '\n';
    EnterCriticalSection(&g_lock);
    bool ok = send(g_sock, buf, (int)n + 1, 0) == (int)n + 1;
    LeaveCriticalSection(&g_lock);
    free(buf);
    return ok;
}

/* Core órfão (de uma versão antiga que não vigiava o pai): um briks-core.exe DESTA pasta cujo processo pai já não existe. Encerra só esses;
 * os que têm pai vivo (outra janela do Briks) e os de outras pastas ficam como estão. */
static void kill_orphan_cores(const char *exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32 pe = { sizeof pe };
    for (BOOL ok = Process32First(snap, &pe); ok; ok = Process32Next(snap, &pe)) {
        if (_stricmp(pe.szExeFile, "briks-core.exe") != 0 || pe.th32ProcessID == GetCurrentProcessId()) continue;
        HANDLE par = OpenProcess(SYNCHRONIZE, FALSE, pe.th32ParentProcessID);
        const bool parent_alive = par && WaitForSingleObject(par, 0) == WAIT_TIMEOUT;
        if (par) CloseHandle(par);
        if (parent_alive) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pe.th32ProcessID); if (!h) continue;
        char path[MAX_PATH]; DWORD n = sizeof path;
        if (QueryFullProcessImageNameA(h, 0, path, &n) && _stricmp(path, exe) == 0) TerminateProcess(h, 1);
        CloseHandle(h);
    }
    CloseHandle(snap);
}

static unsigned __stdcall start_thread(void *unused) {
    (void)unused;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fail("Winsock indisponível."); return 0; }
    unsigned char rnd[32]; char token[65];
    if (BCryptGenRandom(NULL, rnd, sizeof rnd, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) { fail("Sem fonte de aleatoriedade."); return 0; }
    for (int i = 0; i < 32; i++) snprintf(token + i * 2, 3, "%02x", rnd[i]);

    char dir[MAX_PATH], exe[MAX_PATH];
    GetModuleFileNameA(NULL, dir, sizeof dir);
    char *slash = strrchr(dir, '\\'); if (slash) *slash = 0;
    const char *over = getenv("BRIKS_CORE_EXE");
    if (over && over[0]) snprintf(exe, sizeof exe, "%s", over);
    else snprintf(exe, sizeof exe, "%s\\briks-core.exe", dir);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) { fail("briks-core.exe não encontrado ao lado do executável."); return 0; }

    kill_orphan_cores(exe);
    SetEnvironmentVariableA("BRIKS_CORE_TOKEN", token);
    /* segunda trava contra core órfão (além do Job Object, que falha se a UI já roda dentro de um job sem aninhamento):
       o core vigia este processo e sai quando ele morre, por qualquer motivo */
    { char pid[24]; snprintf(pid, sizeof pid, "%lu", (unsigned long)GetCurrentProcessId()); SetEnvironmentVariableA("BRIKS_PARENT_PID", pid); }
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL, in_rd = NULL, in_wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0) || !CreatePipe(&in_rd, &in_wr, &sa, 0)) { fail("Falha ao criar os pipes do core."); return 0; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(in_wr, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = in_rd;
    PROCESS_INFORMATION pi;
    char cmd[MAX_PATH + 32]; snprintf(cmd, sizeof cmd, "\"%s\" serve-ipc", exe);
    BOOL created = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, dir, &si, &pi);
    SetEnvironmentVariableA("BRIKS_CORE_TOKEN", NULL);   /* o token não fica no ambiente da UI */
    if (!created) { CloseHandle(rd); CloseHandle(wr); fail("Não foi possível iniciar o serviço de chamadas."); return 0; }
    CloseHandle(wr); CloseHandle(in_rd);
    g_job = CreateJobObjectA(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = { 0 };
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &lim, sizeof lim);
    g_in_job = AssignProcessToJobObject(g_job, pi.hProcess) != 0;
    ResumeThread(pi.hThread); CloseHandle(pi.hThread);
    g_proc = pi.hProcess;

    char buf[2048]; size_t len = 0; int port = 0;
    DWORD deadline = GetTickCount() + 20000;
    while (GetTickCount() < deadline && !port) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL)) break;
        if (!avail) { Sleep(30); continue; }
        DWORD got = 0;
        if (!ReadFile(rd, buf + len, (DWORD)(sizeof buf - 1 - len), &got, NULL) || !got) break;
        len += got; buf[len] = 0;
        char *r = strstr(buf, "BRIKS_CORE_READY");
        char *p = r ? strstr(r, "\"port\":") : NULL;
        if (p && strchr(p, '}')) port = atoi(p + 7);
        if (len >= sizeof buf - 1) break;
    }
    CloseHandle(rd);
    if (port <= 0) { fail("O serviço de chamadas não respondeu a tempo."); return 0; }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET; addr.sin_port = htons((u_short)port); inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (s == INVALID_SOCKET || connect(s, (struct sockaddr *)&addr, sizeof addr) != 0) { fail("Não foi possível conectar ao serviço de chamadas."); return 0; }
    int nodelay = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof nodelay);
    char hello[160]; snprintf(hello, sizeof hello, "{\"cmd\":\"hello\",\"token\":\"%s\"}\n", token);
    if (send(s, hello, (int)strlen(hello), 0) <= 0) { fail("Falha ao conectar ao serviço de chamadas."); closesocket(s); return 0; }
    g_sock = s;
    InterlockedExchange(&g_state, CORE_READY);

    /* leitura: acumula e entrega linha a linha */
    char *acc = malloc(LINE_MAX_BYTES); size_t alen = 0;
    char tmp[4096];
    while (acc) {
        int n = recv(s, tmp, sizeof tmp, 0);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            if (tmp[i] == '\n') { if (alen) push_line(acc, alen); alen = 0; }
            else if (alen < LINE_MAX_BYTES - 1) acc[alen++] = tmp[i];
        }
    }
    free(acc);
    if (g_state == CORE_READY) { push_line("{\"ev\":\"core.closed\"}", 20); fail("O serviço de chamadas encerrou."); }
    return 0;
}

void core_start(void) {
    if (g_state != CORE_STOPPED) return;
    if (!g_lock_init) { InitializeCriticalSection(&g_lock); g_lock_init = true; }
    InterlockedExchange(&g_state, CORE_STARTING);
    HANDLE t = (HANDLE)_beginthreadex(NULL, 0, start_thread, NULL, 0, NULL);
    if (t) CloseHandle(t); else fail("Falha ao criar a thread do core.");
}

/* 1.4.1 (item 20): encerramento do core sem sobra. Fecha o socket (o core sai sozinho), espera ~1 s e então MATA à força o core e qualquer filho dele
 * (Job Object; e, se o Job não pegou, taskkill /F /T só deste PID). Rede de segurança além disto: KILL_ON_JOB_CLOSE (app morto no Gerenciador de
 * Tarefas ou por crash) e BRIKS_PARENT_PID (o próprio core vigia o pai). Nunca mexe em processo de outra instalação: só no PID/Job que o app iniciou. */
void core_stop(void) {
    if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; }
    if (g_proc) {
        if (WaitForSingleObject(g_proc, 1000) != WAIT_OBJECT_0) {
            if (g_job && g_in_job) TerminateJobObject(g_job, 1);
            else { char c[64]; snprintf(c, sizeof c, "taskkill /F /T /PID %lu", (unsigned long)GetProcessId(g_proc)); STARTUPINFOA si = { sizeof si }; PROCESS_INFORMATION pi;
                   if (CreateProcessA(NULL, c, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) { WaitForSingleObject(pi.hProcess, 3000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); } }
            TerminateProcess(g_proc, 1); WaitForSingleObject(g_proc, 1500);
        }
    }
    if (g_job) { CloseHandle(g_job); g_job = NULL; }   /* KILL_ON_JOB_CLOSE: leva junto qualquer neto que sobrou */
    if (g_proc) { CloseHandle(g_proc); g_proc = NULL; }
    InterlockedExchange(&g_state, CORE_STOPPED);
}
