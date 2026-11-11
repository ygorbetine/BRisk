#include "backend.h"
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <process.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile LONG g_status = BK_STOPPED;
static char g_error[256];
static char g_host[64];
static INTERNET_PORT g_port;
static char g_token[65];
static HANDLE g_job, g_proc;

bk_status_t bk_status(void) { return (bk_status_t)g_status; }
const char *bk_error(void) { return g_error; }

static void fail(const char *msg) {
    snprintf(g_error, sizeof g_error, "%s", msg);
    InterlockedExchange(&g_status, BK_FAILED);
}

static void exe_dir(char *out, size_t cap) {
    GetModuleFileNameA(NULL, out, (DWORD)cap);
    char *slash = strrchr(out, '\\');
    if (slash) *slash = 0;
}

static unsigned __stdcall start_thread(void *unused) {
    (void)unused;
    unsigned char rnd[32];
    if (BCryptGenRandom(NULL, rnd, sizeof rnd, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) { fail("Sem fonte de aleatoriedade."); return 0; }
    for (int i = 0; i < 32; i++) snprintf(g_token + i * 2, 3, "%02x", rnd[i]);

    char dir[MAX_PATH], exe[MAX_PATH], data[MAX_PATH], appdata[MAX_PATH];
    exe_dir(dir, sizeof dir);
    snprintf(exe, sizeof exe, "%s\\backend\\briks-desktop-backend.exe", dir);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) { fail("backend\\briks-desktop-backend.exe não encontrado ao lado do executável."); return 0; }
    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) != S_OK) { fail("Pasta AppData indisponível."); return 0; }
    /* Pasta própria do experimento: nunca toca nos dados do Briks de produção (@briks\desktop). */
    snprintf(data, sizeof data, "%s\\Briks-CUI", appdata);
    { const char *o = getenv("BRIKS_CUI_DATA"); if (o && o[0]) snprintf(data, sizeof data, "%s", o); }   /* testes: pasta isolada (use junto com BRIKS_CORE_DATA) */
    CreateDirectoryA(data, NULL);

    SetEnvironmentVariableA("BRIKS_LOCAL_TOKEN", g_token);
    SetEnvironmentVariableA("BRIKS_DATA_DIR", data);
    SetEnvironmentVariableA("BRIKS_WEB_ORIGIN", "http://localhost:5173");
    SetEnvironmentVariableA("BRIKS_BACKEND_BIND", "127.0.0.1:0");

    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { fail("Falha ao iniciar o serviço local do Briks (canal de saída)."); return 0; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    /* O backend sai quando o stdin fecha (vigia do processo pai): entrega um pipe que fica aberto enquanto vivermos. */
    HANDLE in_rd = NULL, in_wr = NULL;
    if (!CreatePipe(&in_rd, &in_wr, &sa, 0)) { CloseHandle(rd); CloseHandle(wr); fail("Falha ao iniciar o serviço local do Briks (canal de entrada)."); return 0; }
    SetHandleInformation(in_wr, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = in_rd;
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(exe, NULL, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, dir, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr); fail("Não foi possível iniciar o serviço local do Briks."); return 0;
    }
    CloseHandle(wr); CloseHandle(in_rd);   /* in_wr fica aberto até o fim do processo */
    /* O backend morre junto com a interface, mesmo se ela travar. */
    g_job = CreateJobObjectA(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = { 0 };
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &lim, sizeof lim);
    AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_proc = pi.hProcess;

    char buf[4096]; size_t len = 0;
    DWORD deadline = GetTickCount() + 20000;
    while (GetTickCount() < deadline) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL)) break;      /* processo terminou */
        if (!avail) { Sleep(30); continue; }
        DWORD got = 0;
        if (!ReadFile(rd, buf + len, (DWORD)(sizeof buf - 1 - len), &got, NULL) || !got) break;
        len += got; buf[len] = 0;
        char *ready = strstr(buf, "BRIKS_BACKEND_READY");
        if (ready) {
            char *url = strstr(ready, "http://");
            char *end = url ? strpbrk(url, "\"\r\n") : NULL;
            if (url && end) {
                *end = 0;
                char *colon = strrchr(url + 7, ':');
                if (colon) {
                    g_port = (INTERNET_PORT)atoi(colon + 1);
                    size_t hl = (size_t)(colon - (url + 7));
                    if (hl >= sizeof g_host) hl = sizeof g_host - 1;
                    memcpy(g_host, url + 7, hl); g_host[hl] = 0;
                    CloseHandle(rd);
                    InterlockedExchange(&g_status, BK_READY);
                    return 0;
                }
            }
        }
        if (len >= sizeof buf - 1) break;
    }
    CloseHandle(rd);
    fail("O serviço local do Briks não respondeu a tempo.");
    return 0;
}

void bk_start(void) {
    if (g_status != BK_STOPPED) return;
    InterlockedExchange(&g_status, BK_STARTING);
    HANDLE t = (HANDLE)_beginthreadex(NULL, 0, start_thread, NULL, 0, NULL);
    if (t) CloseHandle(t); else fail("Falha ao iniciar o serviço local do Briks.");
}

void bk_stop(void) {
    if (g_job) { CloseHandle(g_job); g_job = NULL; }   /* KILL_ON_JOB_CLOSE */
    if (g_proc) { CloseHandle(g_proc); g_proc = NULL; }
    InterlockedExchange(&g_status, BK_STOPPED);
}

struct bk_call {
    volatile LONG done;
    int status;
    char *body;
    char method[8], path[192], bearer[512];
    char *req;
    HANDLE thread;
};

static wchar_t *widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static unsigned __stdcall call_thread(void *arg) {
    bk_call *c = arg;
    HINTERNET ses = WinHttpOpen(L"Briks-CUI/0", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    HINTERNET con = NULL, req = NULL;
    wchar_t *whost = widen(g_host), *wpath = widen(c->path), *wmethod = widen(c->method);
    if (ses && whost && wpath && wmethod) {
        WinHttpSetTimeouts(ses, 3000, 3000, 15000, 30000);
        con = WinHttpConnect(ses, whost, g_port, 0);
        if (con) req = WinHttpOpenRequest(con, wmethod, wpath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    }
    if (req) {
        char hdr[1024];
        int n = snprintf(hdr, sizeof hdr, "X-Briks-Desktop-Token: %s\r\nContent-Type: application/json\r\n", g_token);
        if (c->bearer[0]) n += snprintf(hdr + n, sizeof hdr - (size_t)n, "Authorization: Bearer %s\r\n", c->bearer);
        wchar_t *wh = widen(hdr);
        DWORD blen = c->req ? (DWORD)strlen(c->req) : 0;
        if (wh && WinHttpSendRequest(req, wh, (DWORD)-1, c->req ? c->req : WINHTTP_NO_REQUEST_DATA, blen, blen, 0)
            && WinHttpReceiveResponse(req, NULL)) {
            DWORD code = 0, sz = sizeof code;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &sz, NULL);
            c->status = (int)code;
            size_t cap = 4096, len = 0;
            char *out = malloc(cap);
            DWORD avail;
            while (out && WinHttpQueryDataAvailable(req, &avail) && avail) {
                if (len + avail + 1 > cap) { cap = (len + avail + 1) * 2; char *no = realloc(out, cap); if (!no) { free(out); out = NULL; break; } out = no; }
                DWORD got = 0;
                if (!WinHttpReadData(req, out + len, avail, &got) || !got) break;
                len += got;
                if (len > 8u * 1024 * 1024) break;
            }
            if (out) { out[len] = 0; c->body = out; }
        }
        free(wh);
    }
    free(whost); free(wpath); free(wmethod);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    if (!c->body) c->body = _strdup("");
    InterlockedExchange(&c->done, 1);
    return 0;
}

bk_call *bk_request(const char *method, const char *path, const char *body, const char *bearer) {
    bk_call *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    snprintf(c->method, sizeof c->method, "%s", method);
    snprintf(c->path, sizeof c->path, "%s", path);
    if (bearer) snprintf(c->bearer, sizeof c->bearer, "%s", bearer);
    if (body) c->req = _strdup(body);
    c->thread = (HANDLE)_beginthreadex(NULL, 0, call_thread, c, 0, NULL);
    if (!c->thread) { c->body = _strdup(""); c->done = 1; }
    return c;
}

bool bk_done(const bk_call *c) { return c && c->done; }
int bk_http_status(const bk_call *c) { return c ? c->status : 0; }
const char *bk_body(const bk_call *c) { return c && c->body ? c->body : ""; }

void bk_free(bk_call *c) {
    if (!c) return;
    if (c->thread) { WaitForSingleObject(c->thread, 35000); CloseHandle(c->thread); }
    free(c->body); free(c->req); free(c);
}
