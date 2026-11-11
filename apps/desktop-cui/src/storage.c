#include "storage.h"
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int attach_forget_under(const char *dir);   /* attach.c */

void storage_dirs(char *data, char *hist, char *recv, size_t cap) {
    char ad[MAX_PATH] = "", la[MAX_PATH] = "";
    SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, ad); SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, la);
    const char *e;
    if ((e = getenv("BRIKS_CUI_DATA")) && e[0]) snprintf(data, cap, "%s", e); else snprintf(data, cap, "%s\\Briks-CUI", ad);
    if ((e = getenv("BRIKS_HIST_DIR")) && e[0]) snprintf(hist, cap, "%s", e); else snprintf(hist, cap, "%s\\Briks-CUI\\historico", la);
    if ((e = getenv("BRIKS_RECV_DIR")) && e[0]) snprintf(recv, cap, "%s", e); else snprintf(recv, cap, "%s\\Briks-CUI\\recebidos", la);
}

static unsigned long long dir_size(const char *dir, int depth, const char *only_prefix) {
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd); if (h == INVALID_HANDLE_VALUE) return 0;
    unsigned long long t = 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   /* nunca segue atalhos/junções */
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 6 && !only_prefix) { char sub[MAX_PATH]; snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName); t += dir_size(sub, depth + 1, NULL); } }
        else if (!only_prefix || !strncmp(fd.cFileName, only_prefix, strlen(only_prefix))) t += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileA(h, &fd));
    FindClose(h); return t;
}

void storage_measure(store_sizes_t *o) {
    char d[MAX_PATH], h[MAX_PATH], r[MAX_PATH]; storage_dirs(d, h, r, MAX_PATH);
    memset(o, 0, sizeof *o);
    o->msgs = dir_size(d, 0, "risk.sqlite3");
    const unsigned long long all = dir_size(d, 0, NULL);
    o->other = all > o->msgs ? all - o->msgs : 0;
    o->hist = dir_size(h, 0, NULL); o->files = dir_size(r, 0, NULL);
    o->total = o->msgs + o->other + o->hist + o->files;
}

void storage_fmt(unsigned long long b, char *o, size_t cap) {
    if (b < 1024) snprintf(o, cap, "%llu B", b); else if (b < 1048576) snprintf(o, cap, "%.1f KB", b / 1024.0);
    else if (b < 1073741824ULL) snprintf(o, cap, "%.1f MB", b / 1048576.0); else snprintf(o, cap, "%.2f GB", b / 1073741824.0);
}

bool storage_inside(const char *dir, const char *path) {
    char a[MAX_PATH], b[MAX_PATH];
    if (!GetFullPathNameA(dir, MAX_PATH, a, NULL) || !GetFullPathNameA(path, MAX_PATH, b, NULL)) return false;
    size_t n = strlen(a); while (n > 3 && (a[n - 1] == '\\' || a[n - 1] == '/')) a[--n] = 0;
    if (n < 4) return false;   /* nunca uma raiz de disco */
    if (_strnicmp(a, b, n) != 0 || b[n] != '\\') return false;
    return !strstr(b + n, "..");
}

/* apaga os arquivos e subpastas de `dir` (mantém a própria pasta) */
static int wipe(const char *dir, int depth, unsigned long long *freed) {
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd); if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 6) { n += wipe(p, depth + 1, freed); RemoveDirectoryA(p); } }
        else { unsigned long long sz = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; SetFileAttributesA(p, FILE_ATTRIBUTE_NORMAL); if (DeleteFileA(p)) { n++; if (freed) *freed += sz; } }
    } while (FindNextFileA(h, &fd));
    FindClose(h); return n;
}

int storage_clear_attachments(unsigned long long *freed) {
    char d[MAX_PATH], h[MAX_PATH], r[MAX_PATH]; storage_dirs(d, h, r, MAX_PATH);
    if (freed) *freed = 0;
    int n = 0;
    if (strlen(r) > 8) n = wipe(r, 0, freed);
    attach_forget_under(r);
    return n;
}

int storage_clear_history_files(unsigned long long *freed) {
    char d[MAX_PATH], h[MAX_PATH], r[MAX_PATH]; storage_dirs(d, h, r, MAX_PATH);
    if (freed) *freed = 0;
    return strlen(h) > 8 ? wipe(h, 0, freed) : 0;
}
