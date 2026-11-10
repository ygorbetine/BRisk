/* Importação dos dados do Briks antigo (Electron). Quem faz o trabalho é `briks-core.exe import-electron` (somente leitura da origem,
 * idempotente, com backup e reversão); aqui só perguntamos ao usuário e mostramos o resultado. O backend local só sobe depois da resposta,
 * porque ele abre (e trava) o banco do app nativo. */
#include "briks_ui.h"
#include "backend.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* roda `briks-core.exe import-electron <args>` e devolve a saída (uma linha JSON); false se não rodou */
static bool run_import(const char *action, char *out, size_t cap) {
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, sizeof exe);
    char *sl = strrchr(exe, '\\'); if (sl) *sl = 0;
    char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\briks-core.exe", exe);
    const char *o = getenv("BRIKS_CORE_EXE"); if (o && o[0]) snprintf(path, sizeof path, "%s", o);
    char cmd[2 * MAX_PATH + 160]; size_t n = (size_t)snprintf(cmd, sizeof cmd, "\"%s\" import-electron %s", path, action);
    const char *src = getenv("BRIKS_IMPORT_SOURCE"), *dst = getenv("BRIKS_IMPORT_DEST");
    if (src && src[0] && n < sizeof cmd) n += (size_t)snprintf(cmd + n, sizeof cmd - n, " --source \"%s\"", src);
    if (dst && dst[0] && n < sizeof cmd) snprintf(cmd + n, sizeof cmd - n, " --dest \"%s\"", dst);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = { sizeof si }; si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) { CloseHandle(rd); CloseHandle(wr); return false; }
    CloseHandle(wr);
    size_t len = 0; DWORD got;
    while (len + 1 < cap && ReadFile(rd, out + len, (DWORD)(cap - 1 - len), &got, NULL) && got) len += got;
    out[len] = 0;
    WaitForSingleObject(pi.hProcess, 20000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd);
    return len > 0;
}

static long table_rows(const jval *j, const char *name) {
    const jval *t = json_get(j, "tables");
    if (!t || t->type != J_ARR) return 0;
    for (int i = 0; i < json_len(t); i++) {
        const jval *e = json_at(t, i);
        if (!strcmp(json_str(e, "table", ""), name)) { const jval *r = json_get(e, "rows"); return r && r->type == J_NUM ? (long)r->num : 0; }
    }
    return 0;
}

void migrate_probe(app_state_t *s) {
    if (getenv("BRIKS_NO_IMPORT")) return;
    if ((getenv("CUI_SCREENSHOT") || getenv("BRIKS_SCRIPT")) && !getenv("BRIKS_IMPORT_DEMO")) return;   /* capturas não perguntam */
    static char buf[4096];
    if (!run_import("probe", buf, sizeof buf)) return;
    jval *j = json_parse(buf); if (!j) return;
    const jval *f = json_get(j, "found");
    if (f && f->type == J_BOOL && f->b) {
        snprintf(s->import_info, sizeof s->import_info, "Conta \"%s\": %ld amigos, %ld grupos e %ld mensagens.",
                 json_str(j, "user", "?"), table_rows(j, "p2p_friends"), table_rows(j, "p2p_groups"), table_rows(j, "p2p_messages"));
        s->import_open = true;
    }
    json_free(j);
}

/* chamado a cada quadro: sobe o backend local assim que não há pergunta pendente */
void migrate_pump(app_state_t *s) {
    static bool started;
    if (!started && !s->import_open) { started = true; bk_start(); }
}

static void do_apply(app_state_t *s) {
    static char buf[4096];
    if (!run_import("apply", buf, sizeof buf)) { snprintf(s->import_result, sizeof s->import_result, "Não foi possível executar a importação."); s->import_done = true; return; }
    jval *j = json_parse(buf);
    const char *st = j ? json_str(j, "state", "") : "";
    if (j && (!strcmp(st, "already") || json_get(j, "mode"))) snprintf(s->import_result, sizeof s->import_result, "Pronto: seus dados foram importados. Os dados do Briks antigo não foram alterados.");
    else snprintf(s->import_result, sizeof s->import_result, "A importação falhou: %s", j ? json_str(j, "error", "erro desconhecido") : "resposta inválida");
    if (j) json_free(j);
    s->import_done = true;
}

void migrate_dialog(cui_ctx *ui, app_state_t *s) {
    cui_dialog(ui, "Importar seus dados do Briks antigo?", &s->import_open, .width = 460) {
        if (!s->import_done) {
            cui_text(ui, "Encontramos o Briks antigo neste computador.", .color = B_TEXT);
            cui_text(ui, s->import_info, .color = B_MUTED, .size = 13);
            cui_text(ui, "Copiamos só para este app. O Briks antigo não é alterado nem apagado, e você pode desfazer a importação depois.", .color = B_MUTED, .size = 13);
            cui_text(ui, "Sua identidade de chamadas será nova (a chave antiga não pode ser exportada): seus amigos podem ver você como uma pessoa nova até adicionarem você de novo. Você também precisa entrar de novo na conta.", .color = B_MUTED, .size = 13);
            cui_dialog_footer(ui) {
                if (cui_button(ui, "Importar", .variant = CUI_BUTTON_PRIMARY)) do_apply(s);
                if (cui_button(ui, "Agora não")) s->import_open = false;
                if (cui_button(ui, "Não perguntar mais")) { char b[256]; run_import("decline", b, sizeof b); s->import_open = false; }
            }
        } else {
            cui_text(ui, s->import_result, .color = B_TEXT);
            cui_dialog_footer(ui) { if (cui_button(ui, "Concluir", .variant = CUI_BUTTON_PRIMARY)) s->import_open = false; }
        }
    }
}
