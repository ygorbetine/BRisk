/* Perfil local: status (balão), rich presence (programa em primeiro plano, só o nome do executável) e preferências.
 * Persistência em %APPDATA%\Briks-CUI\profile.json. A presença só é lida se o usuário ligou "Mostrar o programa que estou usando":
 * nunca lemos o título da janela, só o nome do .exe do programa em primeiro plano. */
#include "briks_ui.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <windows.h>
#include <shlobj.h>

static bool profile_path(char *out, size_t cap) {
    char appdata[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) != S_OK) return false;
    char dir[MAX_PATH]; snprintf(dir, sizeof dir, "%s\\Briks-CUI", appdata);
    CreateDirectoryA(dir, NULL);
    snprintf(out, cap, "%s\\profile.json", dir);
    return true;
}

static void profile_load(app_state_t *s) {
    char p[MAX_PATH];
    if (getenv("BRIKS_DEMO_STATUS") || !profile_path(p, sizeof p)) return;
    FILE *f = fopen(p, "rb"); if (!f) return;
    static char buf[2048]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    jval *j = json_parse(buf); if (!j) return;
    snprintf(s->user_status, sizeof s->user_status, "%s", json_str(j, "status", ""));
    const jval *rp = json_get(j, "rich_presence"), *au = json_get(j, "audience");
    s->rp_enabled = rp && rp->type == J_BOOL && rp->b;
    s->rp_audience = au && au->type == J_NUM && au->num >= 1 ? 1 : 0;
    json_free(j);
}

static void profile_save(const app_state_t *s) {
    char p[MAX_PATH]; if (!profile_path(p, sizeof p)) return;
    char st[200]; json_escape(s->user_status, st, sizeof st);
    FILE *f = fopen(p, "wb"); if (!f) return;
    fprintf(f, "{\"status\":\"%s\",\"rich_presence\":%s,\"audience\":%d}\n", st, s->rp_enabled ? "true" : "false", s->rp_audience);
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
static bool foreground_text(char *out, size_t cap) {
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
    for (size_t i = 0; i < sizeof KNOWN / sizeof *KNOWN; i++) if (!strcmp(name, KNOWN[i].exe)) { snprintf(out, cap, "%s", KNOWN[i].text); return true; }
    if (!name[0]) return false;
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
        if (foreground_text(t, sizeof t) && strcmp(t, s->user_presence)) { snprintf(s->user_presence, sizeof s->user_presence, "%s", t); cui_request_redraw(cui_app_ui(s->app)); }
    }
    if (!s->rp_enabled && s->user_presence[0] && !demo) s->user_presence[0] = 0;
    static char saved_status[64]; static bool saved_rp; static int saved_aud = -1;
    if (saved_aud < 0) { snprintf(saved_status, sizeof saved_status, "%s", s->user_status); saved_rp = s->rp_enabled; saved_aud = s->rp_audience; return; }
    if (strcmp(saved_status, s->user_status) || saved_rp != s->rp_enabled || saved_aud != s->rp_audience) {
        snprintf(saved_status, sizeof saved_status, "%s", s->user_status); saved_rp = s->rp_enabled; saved_aud = s->rp_audience;
        if (!demo) profile_save(s);
    }
}
