#include "update.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <process.h>
#include <shlobj.h>

#ifndef BRIKS_APP_VERSION
#define BRIKS_APP_VERSION "1.3.0"
#endif
#define DEFAULT_FEED "https://github.com/ygorbetine/Briks/releases/latest/download/latest.yml"
#define CHECK_INTERVAL_MS (4UL * 60UL * 60UL * 1000UL)
#define MAX_FEED_BYTES (64 * 1024)
#define MAX_INSTALLER_BYTES (600LL * 1024 * 1024)

static volatile LONG g_phase = UPD_IDLE, g_percent;
static char g_version[48], g_error[256], g_ready_path[MAX_PATH];
static volatile LONG g_dismissed, g_started;

upd_phase_t update_phase(void) { return (upd_phase_t)g_phase; }
int update_percent(void) { return (int)g_percent; }
const char *update_version(void) { return g_version; }
const char *update_error(void) { return g_error; }
void update_dismiss(void) { InterlockedExchange(&g_dismissed, 1); }
bool update_dismissed(void) { return g_dismissed != 0; }
const char *update_current_version(void) {
    const char *o = getenv("BRIKS_APP_VERSION");   /* testes: finge ser outra versão */
    return o && o[0] ? o : BRIKS_APP_VERSION;
}

/* ---- utilitários puros ---- */
static void split_version(const char *v, long n[3], bool *pre) {
    n[0] = n[1] = n[2] = 0; *pre = false;
    if (*v == 'v' || *v == 'V') v++;
    for (int i = 0; i < 3; i++) {
        n[i] = strtol(v, (char **)&v, 10);
        if (*v == '.') v++; else break;
    }
    if (*v == '-') *pre = true;
}
int update_version_compare(const char *a, const char *b) {
    long x[3], y[3]; bool px, py;
    split_version(a, x, &px); split_version(b, y, &py);
    for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    if (px != py) return px ? -1 : 1;
    return 0;
}

static void yaml_value(const char *line, size_t len, char *out, size_t cap) {
    while (len && (isspace((unsigned char)*line))) { line++; len--; }
    while (len && isspace((unsigned char)line[len - 1])) len--;
    if (len >= 2 && (line[0] == '\'' || line[0] == '"') && line[len - 1] == line[0]) { line++; len -= 2; }
    if (len >= cap) len = cap - 1;
    memcpy(out, line, len); out[len] = 0;
}
bool update_parse_feed(const char *yaml, char *version, size_t vcap, char *path, size_t pcap, char *sha512, size_t scap) {
    version[0] = path[0] = sha512[0] = 0;
    for (const char *p = yaml; *p;) {
        const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
        /* só chaves de topo (sem recuo): version, path, sha512 */
        if (n > 8 && !strncmp(p, "version:", 8)) yaml_value(p + 8, n - 8, version, vcap);
        else if (n > 5 && !strncmp(p, "path:", 5)) yaml_value(p + 5, n - 5, path, pcap);
        else if (n > 7 && !strncmp(p, "sha512:", 7)) yaml_value(p + 7, n - 7, sha512, scap);
        if (!e) break;
        p = e + 1;
    }
    if (!version[0] || !path[0] || !sha512[0]) return false;
    if (strchr(path, '\\') || strstr(path, "..") || strchr(path, ':') && strncmp(path, "http", 4)) return false;
    size_t pl = strlen(path);
    return pl > 4 && !_stricmp(path + pl - 4, ".exe");
}
bool update_url_allowed(const char *url) {
    if (!_strnicmp(url, "https://", 8)) return true;
    if (!_strnicmp(url, "http://127.0.0.1", 16) || !_strnicmp(url, "http://localhost", 16) || !_strnicmp(url, "http://[::1]", 12)) return true;
    return false;
}

/* ---- rede ---- */
static void set_err(const char *m) { snprintf(g_error, sizeof g_error, "%s", m); InterlockedExchange(&g_phase, UPD_ERROR); }

/* baixa `url` para memória (buf, até cap) ou arquivo (fp); devolve bytes ou -1 */
static long long http_get(const char *url, char *buf, size_t cap, FILE *fp, long long max_bytes, bool progress) {
    if (!update_url_allowed(url)) return -1;
    { const char *lo = getenv("BRIKS_NET_LOOPBACK_ONLY");   /* testes: so loopback, sem aviso do Firewall (docs/plans/testes-rede-local.md) */
      if (lo && lo[0] == '1' && _strnicmp(url, "http://127.0.0.1", 16) && _strnicmp(url, "http://localhost", 16) && _strnicmp(url, "http://[::1]", 12)) return -1; }
    wchar_t wurl[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024) <= 0) return -1;
    URL_COMPONENTS uc = { sizeof uc };
    wchar_t host[256], path[1024];
    uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return -1;
    HINTERNET ses = WinHttpOpen(L"Briks-CUI-Updater/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0), con = NULL, req = NULL;
    long long total = -1;
    if (ses) {
        WinHttpSetTimeouts(ses, 8000, 8000, 20000, 60000);
        con = WinHttpConnect(ses, host, uc.nPort, 0);
        if (con) req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    }
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, NULL)) {
        DWORD status = 0, sz = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            wchar_t cl[32]; DWORD cls = sizeof cl; long long expect = 0;
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, NULL, cl, &cls, WINHTTP_NO_HEADER_INDEX)) expect = _wtoi64(cl);
            if (expect > max_bytes) { total = -1; goto done; }
            total = 0;
            static char chunk[64 * 1024];
            for (;;) {
                DWORD got = 0;
                if (!WinHttpReadData(req, chunk, sizeof chunk, &got)) { total = -1; break; }
                if (!got) break;
                if ((long long)got + total > max_bytes) { total = -1; break; }
                if (fp) { if (fwrite(chunk, 1, got, fp) != got) { total = -1; break; } }
                else { if (total + got >= cap) { total = -1; break; } memcpy(buf + total, chunk, got); }
                total += got;
                if (progress && expect > 0) InterlockedExchange(&g_percent, (LONG)(total * 100 / expect));
            }
            if (buf && total >= 0) buf[total] = 0;
        }
    }
done:
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return total;
}

/* SHA-512 do arquivo em base64 (o formato do latest.yml) */
static bool file_sha512_b64(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "rb"); if (!f) return false;
    BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_HASH_HANDLE h = NULL; bool ok = false;
    UCHAR dig[64];
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA512_ALGORITHM, NULL, 0) == 0 && BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        static UCHAR b[64 * 1024]; size_t n; ok = true;
        while ((n = fread(b, 1, sizeof b, f)) > 0) if (BCryptHashData(h, b, (ULONG)n, 0) != 0) { ok = false; break; }
        if (ok && BCryptFinishHash(h, dig, sizeof dig, 0) != 0) ok = false;
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    fclose(f);
    if (!ok) return false;
    DWORD len = (DWORD)cap;
    return CryptBinaryToStringA(dig, sizeof dig, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out, &len) != 0;
}

static bool updates_dir(char *out, size_t cap) {
    const char *o = getenv("BRIKS_UPDATE_DIR");   /* testes: pasta isolada */
    if (o && o[0]) { snprintf(out, cap, "%s", o); CreateDirectoryA(out, NULL); return true; }
    char appdata[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) != S_OK) return false;
    snprintf(out, cap, "%s\\Briks-CUI", appdata); CreateDirectoryA(out, NULL);
    snprintf(out, cap, "%s\\Briks-CUI\\updates", appdata); CreateDirectoryA(out, NULL);
    return true;
}

/* uma checagem completa: feed -> comparação -> download -> verificação. Devolve true se ficou pronto. */
static void check_once(void) {
    const char *feed = getenv("BRIKS_UPDATE_FEED"); if (!feed || !feed[0]) feed = DEFAULT_FEED;
    if (g_phase == UPD_READY) return;
    InterlockedExchange(&g_phase, UPD_CHECKING);
    static char yaml[MAX_FEED_BYTES + 1];
    if (http_get(feed, yaml, sizeof yaml, NULL, MAX_FEED_BYTES, false) < 0) { set_err("Não foi possível consultar as atualizações."); return; }
    char ver[48], rel[256], sha[200];
    if (!update_parse_feed(yaml, ver, sizeof ver, rel, sizeof rel, sha, sizeof sha)) { set_err("Feed de atualização inválido."); return; }
    if (update_version_compare(ver, update_current_version()) <= 0) { InterlockedExchange(&g_phase, UPD_IDLE); return; }
    char url[1024];
    if (!_strnicmp(rel, "http", 4)) snprintf(url, sizeof url, "%s", rel);
    else {   /* relativo à pasta do feed */
        snprintf(url, sizeof url, "%s", feed);
        char *q = strchr(url, '?'); if (q) *q = 0;
        char *sl = strrchr(url, '/'); if (!sl) { set_err("URL do feed inválida."); return; }
        sl[1] = 0;
        size_t n = strlen(url); snprintf(url + n, sizeof url - n, "%s", rel);
    }
    if (!update_url_allowed(url)) { set_err("O instalador precisa ser baixado por HTTPS."); return; }
    snprintf(g_version, sizeof g_version, "%s", ver);
    char dir[MAX_PATH], part[MAX_PATH], fin[MAX_PATH];
    if (!updates_dir(dir, sizeof dir)) { set_err("Pasta de atualizações indisponível."); return; }
    const char *base = strrchr(rel, '/'); base = base ? base + 1 : rel;
    snprintf(fin, sizeof fin, "%s\\%s", dir, base); snprintf(part, sizeof part, "%s.part", fin);
    InterlockedExchange(&g_percent, 0); InterlockedExchange(&g_phase, UPD_DOWNLOADING);
    FILE *fp = fopen(part, "wb"); if (!fp) { set_err("Não foi possível gravar o instalador."); return; }
    long long n = http_get(url, NULL, 0, fp, MAX_INSTALLER_BYTES, true);
    fclose(fp);
    if (n <= 0) { DeleteFileA(part); set_err("Falha ao baixar a atualização."); return; }
    char got[200] = { 0 };
    if (!file_sha512_b64(part, got, sizeof got) || strcmp(got, sha)) { DeleteFileA(part); set_err("O instalador baixado não confere com o feed (SHA-512): descartado."); return; }
    DeleteFileA(fin);
    if (!MoveFileA(part, fin)) { DeleteFileA(part); set_err("Não foi possível guardar o instalador."); return; }
    snprintf(g_ready_path, sizeof g_ready_path, "%s", fin);
    InterlockedExchange(&g_percent, 100);
    InterlockedExchange(&g_phase, UPD_READY);
}

/* Politica de ligar a atualizacao (pura, testada em tests/smoke.c). Ordem: BRIKS_DISABLE_UPDATE=1 desliga sempre; feed de teste liga; BRIKS_ENABLE_UPDATE=1 ou o arquivo
 * "update-enabled" ligam; e, no build Release do instalador (BRIKS_UPDATE_DEFAULT_ON), liga por padrao SO no app instalado (marcador briks.installed ao lado do exe). */
bool update_policy(bool disable, bool has_feed, bool enable_env, bool enable_file, bool default_on, bool installed) {
    if (disable) return false;
    if (has_feed || enable_env || enable_file) return true;
    return default_on && installed;
}

static bool update_enabled(void) {
    const char *d = getenv("BRIKS_DISABLE_UPDATE"), *e = getenv("BRIKS_ENABLE_UPDATE");
    char appdata[MAX_PATH], f[MAX_PATH]; bool file = false, installed = false;
    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) == S_OK) {
        snprintf(f, sizeof f, "%s\\Briks-CUI\\update-enabled", appdata);
        file = GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES;
    }
    char exe[MAX_PATH]; if (GetModuleFileNameA(NULL, exe, sizeof exe)) {
        char *sl = strrchr(exe, '\\'); if (sl) { *sl = 0; snprintf(f, sizeof f, "%s\\briks.installed", exe); installed = GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES; }
    }
#ifdef BRIKS_UPDATE_DEFAULT_ON
    const bool def = true;
#else
    const bool def = false;
#endif
    return update_policy(d && d[0] == '1', getenv("BRIKS_UPDATE_FEED") != NULL, e && e[0] == '1', file, def, installed);
}

static unsigned __stdcall update_thread(void *unused) {
    (void)unused;
    const char *w = getenv("BRIKS_UPDATE_DELAY_MS");
    Sleep(w ? (DWORD)atoi(w) : 15000);
    for (;;) {
        check_once();
        if (g_phase == UPD_READY) return 0;
        Sleep(CHECK_INTERVAL_MS);
    }
}

void update_start(void) {
    { const char *d = getenv("BRIKS_UPDATE_DEMO"); if (d && d[0]) { snprintf(g_version, sizeof g_version, "%s", d); InterlockedExchange(&g_phase, UPD_READY); return; } }   /* so para capturas: mostra o aviso sem baixar nada */
    if (InterlockedExchange(&g_started, 1) || !update_enabled()) return;
    HANDLE t = (HANDLE)_beginthreadex(NULL, 0, update_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

static bool run_installer(const char *args) {
    if (g_phase != UPD_READY || !g_ready_path[0]) return false;
    char cmd[2 * MAX_PATH + 64]; snprintf(cmd, sizeof cmd, "\"%s\" %s", g_ready_path, args);
    STARTUPINFOA si = { sizeof si }; PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi)) return false;
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return true;
}
bool update_install_now(void) { return run_installer("--updated /S --force-run"); }
void update_install_on_exit(void) { static bool done; if (!done) { done = true; run_installer("--updated /S"); } }
