/* Perfil local: status (balão), rich presence (programa em primeiro plano, só o nome do executável) e preferências.
 * Persistência em %APPDATA%\Briks-CUI\profile.json. A presença só é lida se o usuário ligou "Mostrar o programa que estou usando":
 * nunca lemos o título da janela, só o nome do .exe do programa em primeiro plano. */
#include "briks_ui.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <windows.h>
#include <shlobj.h>

static bool profile_path(char *out, size_t cap) {
    const char *over = getenv("BRIKS_PROFILE_FILE");   /* testes: não mexe no perfil real */
    if (over && over[0]) { snprintf(out, cap, "%s", over); return true; }
    char appdata[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) != S_OK) return false;
    char dir[MAX_PATH]; snprintf(dir, sizeof dir, "%s\\Briks-CUI", appdata);
    CreateDirectoryA(dir, NULL);
    snprintf(out, cap, "%s\\profile.json", dir);
    return true;
}

/* "limpar depois de": 0 não limpar, 1 hora, 2 4 horas, 3 hoje (até a meia-noite local); devolve o epoch de expiração (0 = nunca) */
long long profile_expiry_for(int clear, long long now) {
    if (clear == 1) return now + 3600;
    if (clear == 2) return now + 4 * 3600;
    if (clear == 3) {
        time_t t = (time_t)now; struct tm lt; localtime_s(&lt, &t);
        lt.tm_hour = 23; lt.tm_min = 59; lt.tm_sec = 59; lt.tm_isdst = -1;
        return (long long)mktime(&lt) + 1;
    }
    return 0;
}

/* a presença é ignorada se o nome do programa (ou o texto) contém algum item da lista "Ignorar programa", sem diferenciar maiúsculas */
bool profile_ignored(const app_state_t *s, const char *exe, const char *text) {
    for (int i = 0; i < s->rp_ignore_n; i++) {
        char k[32]; snprintf(k, sizeof k, "%s", s->rp_ignore[i]);
        for (char *c = k; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (!k[0]) continue;
        char a[96], b[96]; snprintf(a, sizeof a, "%s", exe ? exe : ""); snprintf(b, sizeof b, "%s", text ? text : "");
        for (char *c = a; *c; c++) *c = (char)tolower((unsigned char)*c);
        for (char *c = b; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (strstr(a, k) || strstr(b, k)) return true;
    }
    return false;
}

static void profile_load(app_state_t *s) {
    char p[MAX_PATH];
    if (getenv("BRIKS_DEMO_STATUS") || !profile_path(p, sizeof p)) return;
    FILE *f = fopen(p, "rb"); if (!f) return;
    static char buf[4096]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    jval *j = json_parse(buf); if (!j) return;
    snprintf(s->user_status, sizeof s->user_status, "%s", json_str(j, "status", ""));
    const jval *rp = json_get(j, "rich_presence"), *au = json_get(j, "audience");
    s->rp_enabled = rp && rp->type == J_BOOL && rp->b;
    s->rp_audience = au && au->type == J_NUM && au->num >= 1 ? 1 : 0;
    snprintf(s->user_emoji, sizeof s->user_emoji, "%s", json_str(j, "emoji", ""));
    { const jval *iv = json_get(j, "invisible"); s->invisible = iv && iv->type == J_BOOL && iv->b; const jval *nv = json_get(j, "notify_off"); s->notify_off = nv && nv->type == J_BOOL && nv->b; }
    const jval *cl = json_get(j, "clear"), *ex = json_get(j, "expires"), *ig = json_get(j, "ignore");
    s->status_clear = cl && cl->type == J_NUM ? (int)cl->num : 0; if (s->status_clear < 0 || s->status_clear > 3) s->status_clear = 0;
    s->status_expires = ex && ex->type == J_NUM ? (long long)ex->num : 0;
    s->rp_ignore_n = 0;
    for (int i = 0; i < json_len(ig) && s->rp_ignore_n < 6; i++) { const jval *e = json_at(ig, i); if (e && e->type == J_STR && e->str[0]) snprintf(s->rp_ignore[s->rp_ignore_n++], sizeof s->rp_ignore[0], "%s", e->str); }
    json_free(j);
}

static void profile_save(const app_state_t *s) {
    char p[MAX_PATH]; if (!profile_path(p, sizeof p)) return;
    char st[200]; json_escape(s->user_status, st, sizeof st);
    FILE *f = fopen(p, "wb"); if (!f) return;
    char em[40]; json_escape(s->user_emoji, em, sizeof em);
    fprintf(f, "{\"status\":\"%s\",\"emoji\":\"%s\",\"clear\":%d,\"expires\":%lld,\"rich_presence\":%s,\"audience\":%d,\"invisible\":%s,\"notify_off\":%s,\"ignore\":[", st, em, s->status_clear, s->status_expires, s->rp_enabled ? "true" : "false", s->rp_audience, s->invisible ? "true" : "false", s->notify_off ? "true" : "false");
    for (int i = 0; i < s->rp_ignore_n; i++) { char ig[80]; json_escape(s->rp_ignore[i], ig, sizeof ig); fprintf(f, "%s\"%s\"", i ? "," : "", ig); }
    fprintf(f, "]}\n");
    fclose(f);
}

typedef struct { const char *exe, *text; } known_t;
static const known_t KNOWN[] = {
    { "chrome", "Usando Google Chrome" }, { "msedge", "Usando Microsoft Edge" }, { "firefox", "Usando Firefox" },
    { "code", "Usando VS Code" }, { "devenv", "Usando Visual Studio" }, { "spotify", "Ouvindo no Spotify" },
    { "discord", "No Discord" }, { "steam", "Na Steam" }, { "obs64", "Transmitindo no OBS" },
    { "valorant-win64-shipping", "Jogando Valorant" }, { "leagueclient", "Jogando League of Legends" },
    { "league of legends", "Jogando League of Legends" }, { "fortniteclient-win64-shipping", "Jogando Fortnite" },
    { "cs2", "Jogando Counter-Strike 2" }, { "dota2", "Jogando Dota 2" }, { "gta5", "Jogando GTA V" },
    { "winword", "Usando Word" }, { "excel", "Usando Excel" }, { "powerpnt", "Usando PowerPoint" },
};
/* programas de sistema que não dizem nada sobre o que a pessoa está fazendo */
static const char *SKIP_EXE[] = { "explorer", "searchhost", "startmenuexperiencehost", "lockapp", "applicationframehost", "shellexperiencehost",
    "textinputhost", "dwm", "taskmgr", "systemsettings", "briks_cui", "briks", "snippingtool", "cmd", "powershell", "conhost", "windowsterminal" };

/* escreve o texto de presença do programa em primeiro plano; devolve false se não há nada a mostrar (mantém o anterior) */
static bool foreground_text(const app_state_t *s, char *out, size_t cap) {
    HWND w = GetForegroundWindow(); if (!w) return false;
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    if (!pid || pid == GetCurrentProcessId()) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); if (!h) return false;
    wchar_t wp[MAX_PATH]; DWORD n = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(h, 0, wp, &n) != 0;
    CloseHandle(h);
    if (!ok) return false;
    wchar_t *b = wcsrchr(wp, L'\\'); b = b ? b + 1 : wp;
    char name[96] = { 0 };
    if (WideCharToMultiByte(CP_UTF8, 0, b, -1, name, sizeof name, NULL, NULL) <= 0) return false;
    char *dot = strrchr(name, '.'); if (dot) *dot = 0;
    for (char *c = name; *c; c++) *c = (char)tolower((unsigned char)*c);
    for (size_t i = 0; i < sizeof SKIP_EXE / sizeof *SKIP_EXE; i++) if (!strcmp(name, SKIP_EXE[i])) return false;
    if (!name[0] || profile_ignored(s, name, NULL)) return false;   /* programa na lista "Ignorar": nada é mostrado */
    for (size_t i = 0; i < sizeof KNOWN / sizeof *KNOWN; i++) if (!strcmp(name, KNOWN[i].exe)) { snprintf(out, cap, "%s", KNOWN[i].text); return true; }
    name[0] = (char)toupper((unsigned char)name[0]);
    snprintf(out, cap, "Usando %s", name);
    return true;
}

/* chamado a cada quadro: sonda o programa em uso a cada 4 s (só com o recurso ligado) e grava o perfil quando algo muda */
void profile_tick(app_state_t *s) {
    static bool loaded; if (!loaded) { loaded = true; profile_load(s); }
    const bool demo = getenv("BRIKS_DEMO_STATUS") != NULL;
    static ULONGLONG last;
    ULONGLONG now = GetTickCount64();
    if (s->rp_enabled && !demo && now - last > 4000) {
        last = now;
        char t[64];
        if (foreground_text(s, t, sizeof t) && strcmp(t, s->user_presence)) { snprintf(s->user_presence, sizeof s->user_presence, "%s", t); cui_request_redraw(cui_app_ui(s->app)); }
    }
    if (!s->rp_enabled && s->user_presence[0] && !demo) s->user_presence[0] = 0;
    static char saved_status[64], saved_emoji[16], saved_ign[6 * 32 + 8]; static bool saved_rp, saved_inv, saved_notify; static int saved_aud = -1, saved_clear;
    char ign[6 * 32 + 8] = ""; for (int i = 0; i < s->rp_ignore_n; i++) { strncat(ign, s->rp_ignore[i], 31); strcat(ign, "|"); }
    if (saved_aud < 0) {
        snprintf(saved_status, sizeof saved_status, "%s", s->user_status); snprintf(saved_emoji, sizeof saved_emoji, "%s", s->user_emoji); snprintf(saved_ign, sizeof saved_ign, "%s", ign);
        saved_rp = s->rp_enabled; saved_aud = s->rp_audience; saved_clear = s->status_clear; saved_inv = s->invisible; saved_notify = s->notify_off; return;
    }
    const long long nowt = (long long)time(NULL);
    if (s->status_expires && nowt >= s->status_expires && !demo) {   /* o status venceu: some, como no Electron */
        s->user_status[0] = 0; s->user_emoji[0] = 0; s->status_expires = 0; if (s->app) cui_request_redraw(cui_app_ui(s->app));
    }
    if (strcmp(saved_status, s->user_status) || strcmp(saved_emoji, s->user_emoji) || saved_rp != s->rp_enabled || saved_aud != s->rp_audience || strcmp(saved_ign, ign) || saved_clear != s->status_clear || saved_inv != s->invisible || saved_notify != s->notify_off) {
        const bool status_changed = strcmp(saved_status, s->user_status) != 0 || saved_clear != s->status_clear;
        if (status_changed) s->status_expires = s->user_status[0] ? profile_expiry_for(s->status_clear, nowt) : 0;   /* novo status: conta o prazo escolhido */
        snprintf(saved_status, sizeof saved_status, "%s", s->user_status); snprintf(saved_emoji, sizeof saved_emoji, "%s", s->user_emoji); snprintf(saved_ign, sizeof saved_ign, "%s", ign);
        saved_rp = s->rp_enabled; saved_aud = s->rp_audience; saved_clear = s->status_clear; saved_inv = s->invisible; saved_notify = s->notify_off;
        if (!demo) profile_save(s);
    }
}
