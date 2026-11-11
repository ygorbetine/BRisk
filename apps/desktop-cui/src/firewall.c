/* Firewall do Windows: regra de ENTRADA só para os programas do Briks (briks_cui/Briks.exe e briks-core.exe), criada na primeira chamada
 * com a confirmação da pessoa. Sem porta geral. A criação pede UAC (ShellExecuteEx "runas"); se a pessoa recusar, vale "Agora não".
 * A escolha (liberado / agora não / não perguntar) fica em settings.cfg (fw_choice). A chamada segue em qualquer escolha: o diálogo
 * nunca bloqueia nada. Testes: BRIKS_NETSH aponta para um netsh falso (.cmd/.exe que só registra os argumentos); nesse modo não há UAC
 * e nada toca o firewall real. Em demonstração (BRIKS_DEMO) sem BRIKS_NETSH, tudo aqui é inerte. */
#include "briks_ui.h"
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

enum { FW_NEW = 0, FW_ON = 1, FW_LATER = 2, FW_NEVER = 3 };

static volatile LONG g_ask, g_busy, g_exists = -1, g_result;   /* g_exists: -1 não sabe, 0 não, 1 sim; g_result: 0 nada, 1 ok, 2 recusou/falhou */
static bool g_open, g_asked_session; static char g_msg[160];
static volatile LONG g_checking;

static bool inert(void) { return getenv("BRIKS_DEMO") && !getenv("BRIKS_NETSH"); }
static const char *netsh(void) { const char *n = getenv("BRIKS_NETSH"); return n && n[0] ? n : "netsh"; }

static void utf8(const wchar_t *w, char *out, size_t cap) { if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) <= 0) out[0] = 0; }
static void exe_paths(char *cui, size_t cc, char *core, size_t kc) {
    wchar_t w[MAX_PATH]; GetModuleFileNameW(NULL, w, MAX_PATH); utf8(w, cui, cc);
    const char *be = getenv("BRIKS_BACKEND_EXE");
    if (be && be[0]) { snprintf(core, kc, "%s", be); return; }
    snprintf(core, kc, "%s", cui); char *sl = strrchr(core, '\\'); if (sl) *sl = 0;
    strncat(core, "\\briks-core.exe", kc - strlen(core) - 1);
}

/* linha de comando do cmd.exe que executa os netsh em sequência (call: o netsh de teste pode ser .cmd) */
static void chain(bool add, char *out, size_t cap) {
    char cui[MAX_PATH * 3], core[MAX_PATH * 3]; exe_paths(cui, sizeof cui, core, sizeof core);
    const char *n = netsh();
    int k = snprintf(out, cap, "/c \"call \"%s\" advfirewall firewall delete rule name=\"Briks\" & call \"%s\" advfirewall firewall delete rule name=\"Briks core\"", n, n);
    if (add) {
        k += snprintf(out + k, cap - (size_t)k, " & call \"%s\" advfirewall firewall add rule name=\"Briks\" dir=in action=allow program=\"%s\" enable=yes profile=private,public", n, cui);
        k += snprintf(out + k, cap - (size_t)k, " & call \"%s\" advfirewall firewall add rule name=\"Briks core\" dir=in action=allow program=\"%s\" enable=yes profile=private,public", n, core);
    }
    snprintf(out + k, cap - (size_t)k, "\"");
}

/* roda o cmd.exe com os parâmetros; elevate = UAC (só fora de teste). Devolve o código de saída, ou -2 se a pessoa recusou o UAC. */
static int run_cmd(const char *params, bool elevate) {
    wchar_t wp[4096]; MultiByteToWideChar(CP_UTF8, 0, params, -1, wp, 4096);
    if (elevate) {
        SHELLEXECUTEINFOW x = { sizeof x }; x.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC; x.lpVerb = L"runas"; x.lpFile = L"cmd.exe"; x.lpParameters = wp; x.nShow = SW_HIDE;
        if (!ShellExecuteExW(&x) || !x.hProcess) return -2;
        WaitForSingleObject(x.hProcess, 60000); DWORD c = 1; GetExitCodeProcess(x.hProcess, &c); CloseHandle(x.hProcess); return (int)c;
    }
    wchar_t cl[4200]; swprintf(cl, 4200, L"cmd.exe %ls", wp);
    STARTUPINFOW si = { sizeof si }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cl, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, 20000); DWORD c = 1; GetExitCodeProcess(pi.hProcess, &c); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return (int)c;
}

static bool rule_exists(void) {
    char p[600]; snprintf(p, sizeof p, "/c \"call \"%s\" advfirewall firewall show rule name=\"Briks\"\"", netsh());
    return run_cmd(p, false) == 0;
}

static DWORD WINAPI check_thread(LPVOID a) {   /* a != NULL: se não há regra, abre o diálogo; NULL: só atualiza o estado */
    const bool ex = rule_exists();
    InterlockedExchange(&g_exists, ex ? 1 : 0);
    if (!ex && a) InterlockedExchange(&g_ask, 1);
    InterlockedExchange(&g_checking, 0);
    return 0;
}
static void check_async(bool ask) { if (InterlockedCompareExchange(&g_checking, 1, 0) == 0) { HANDLE h = CreateThread(NULL, 0, check_thread, ask ? (LPVOID)1 : NULL, 0, NULL); if (h) CloseHandle(h); else g_checking = 0; } }

/* chamada: pergunta (uma vez por sessão) se não existe regra e a pessoa não pediu para parar de perguntar */
void fw_maybe_prompt(void) {
    if (inert() || g_asked_session || settings_fw_choice() == FW_NEVER) return;
    g_asked_session = true;
    check_async(true);
}

static DWORD WINAPI apply_thread(LPVOID a) {
    const bool add = a != NULL;
    char p[3000]; chain(add, p, sizeof p);
    const int rc = run_cmd(p, !getenv("BRIKS_NETSH"));
    if (rc == 0) { InterlockedExchange(&g_exists, add ? 1 : 0); InterlockedExchange(&g_result, 1); }
    else InterlockedExchange(&g_result, 2);   /* UAC recusado ou netsh falhou: tratado como "Agora não" */
    InterlockedExchange(&g_busy, 0);
    return 0;
}
static bool g_last_add;
void fw_request(bool add) {
    if (inert() || InterlockedCompareExchange(&g_busy, 1, 0) != 0) return;
    g_last_add = add; g_msg[0] = 0;
    HANDLE h = CreateThread(NULL, 0, apply_thread, add ? (LPVOID)1 : NULL, 0, NULL);
    if (h) CloseHandle(h); else g_busy = 0;
}
bool fw_busy(void) { return g_busy != 0; }
bool fw_open(void) { return g_open; }
int fw_exists(void) { return (int)g_exists; }
const char *fw_msg(void) { return g_msg; }

void fw_refresh(void) {   /* Configurações > Rede: lê o estado real de tempos em tempos */
    static ULONGLONG last; const ULONGLONG now = GetTickCount64();
    if (inert() || (last && now - last < 10000)) return; last = now;
    check_async(false);   /* só consulta: não abre o diálogo */
}

void fw_tick(void) {
    if (g_ask) { g_ask = 0; if (!g_open && settings_fw_choice() != FW_NEVER) g_open = true; }
    if (g_result) {
        const LONG r = g_result; g_result = 0;
        if (r == 1) { if (g_last_add) { settings_fw_set(FW_ON); snprintf(g_msg, sizeof g_msg, "Pronto: o Briks está liberado no Firewall."); } else { settings_fw_set(FW_LATER); snprintf(g_msg, sizeof g_msg, "Regra removida."); } }
        else { if (g_last_add && settings_fw_choice() != FW_NEVER) settings_fw_set(FW_LATER); snprintf(g_msg, sizeof g_msg, "Não foi possível alterar o Firewall (o pedido de permissão foi recusado)."); }
        g_open = false;
    }
}

const char *fw_state_text(void) {
    const int c = settings_fw_choice();
    if (g_exists == 1) return "Liberado: o Windows já permite o Briks nas chamadas.";
    if (c == FW_ON) return "Você liberou, mas a regra não foi encontrada agora.";
    if (c == FW_NEVER) return "Não liberado (o Briks não vai mais perguntar).";
    if (c == FW_LATER) return "Ainda não liberado. O Briks pergunta de novo na próxima chamada.";
    return "Ainda não liberado.";
}

void fw_dialog(cui_ctx *ui, app_state_t *s) {
    (void)s;
    cui_vec2 vp = cui_viewport_size(ui);
    cui_dialog(ui, "Liberar o Briks no Firewall", &g_open, .width = fminf(420.0f, fmaxf(vp.x - 48.0f, 260.0f))) {
        cui_text(ui, "O Windows vai pedir permissão para o Briks usar a rede nas chamadas. Quer que o Briks libere isso agora?", .size = 13, .color = B_TEXT, .width = CUI_FILL);
        cui_text(ui, "Só o Briks é liberado, nada mais. Você pode mudar isso depois em Configurações > Rede. A chamada continua do mesmo jeito, qualquer que seja a sua escolha.", .size = 12, .color = B_MUTED, .width = CUI_FILL);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Não perguntar de novo", .variant = CUI_BUTTON_LINK, .size = CUI_SIZE_SM)) { settings_fw_set(FW_NEVER); g_open = false; }
            if (cui_button(ui, "Agora não", .variant = CUI_BUTTON_SECONDARY)) { settings_fw_set(FW_LATER); g_open = false; }
            if (cui_button(ui, "Liberar", .variant = CUI_BUTTON_PRIMARY, .loading = g_busy != 0)) fw_request(true);
        }
    }
}
