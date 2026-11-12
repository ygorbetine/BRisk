/* Configurações > Armazenamento (CFG-18..22): uso por tipo, limpar anexos, apagar histórico e exportar contatos e ajustes.
 * Cada botão faz o que diz; o que o Briks não consegue fazer aparece escrito na própria tela. */
#include "briks_ui.h"
#include "storage.h"
#include "backend.h"
#include "json.h"
#include "update.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>
#include <commdlg.h>

void chat_dm_channel(const app_state_t *s, const char *peer, char *out, size_t cap);   /* chat.c */
void settings_export_json(FILE *f);                                                   /* settings.c: ajustes (sem segredos) */

#define DCARD(ui, title) CUI__SCOPE((cui_box_begin((ui), CUI__OPTS(cui_box, .width = CUI_FILL, .pad = 14, .gap = 10, .radius = 16, .bg = B_GLASS_CARD, \
    .border = 1, .border_color = B_GLASS_BORDER)), cui_text((ui), (title), .font = CUI_FONT_MEDIUM, .size = 13, .color = B_C3)), cui_box_end(ui))

static store_sizes_t g_sz;
static char data_msg[300]; static bool data_msg_bad;
static int data_confirm;                                 /* 0 nada, 1 limpar anexos, 2 apagar histórico */
static bk_call *purge_calls[96]; static int purge_n, purge_ok, purge_fail; static bool purge_running;

void data_refresh(void) { storage_measure(&g_sz); data_confirm = 0; }

static void say(bool bad, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(data_msg, sizeof data_msg, fmt, ap); va_end(ap); data_msg_bad = bad;
}

static void purge_history(app_state_t *s) {
    int files = storage_clear_history_files(NULL);
    purge_n = purge_ok = purge_fail = 0;
    if (s->token[0] && bk_status() == BK_READY) {
        char ch[96], path[160];
        for (int i = 0; i < s->friend_count && purge_n < 96; i++) { chat_dm_channel(s, s->friends[i].id, ch, sizeof ch); snprintf(path, sizeof path, "/p2p/channels/%s", ch); purge_calls[purge_n++] = bk_request("POST", path, "{}", s->token); }
        for (int g = 0; g < s->group_count; g++) {
            const group_t *gr = &s->groups[g];
            if (!gr->nch && purge_n < 96) { snprintf(path, sizeof path, "/p2p/channels/%s", gr->id); purge_calls[purge_n++] = bk_request("POST", path, "{}", s->token); }
            for (int k = 0; k < gr->nch && purge_n < 96; k++) { snprintf(path, sizeof path, "/p2p/channels/%s", gr->ch[k].id); purge_calls[purge_n++] = bk_request("POST", path, "{}", s->token); }
        }
        purge_running = purge_n > 0;
        if (purge_running) say(false, "Apagando %d conversa(s)…", purge_n);
        else say(false, "Nenhuma conversa para apagar. Histórico sincronizado: %d arquivo(s) removido(s).", files);
    } else say(true, "Só o histórico sincronizado foi apagado (%d arquivo(s)). As mensagens do banco local ficam: entre na conta e tente de novo.", files);
    storage_measure(&g_sz);
}

/* a cada quadro (hotkeys.c): recolhe as respostas dos pedidos de apagar e recarrega a conversa aberta */
void settings_data_pump(app_state_t *s) {
    if (!purge_running) return;
    int pending = 0;
    for (int i = 0; i < purge_n; i++) if (purge_calls[i]) {
        if (!bk_done(purge_calls[i])) { pending++; continue; }
        if (bk_http_status(purge_calls[i]) == 200) purge_ok++; else purge_fail++;
        bk_free(purge_calls[i]); purge_calls[i] = NULL;
    }
    if (pending) return;
    purge_running = false; s->msg_count = 0; s->reload_msgs = true;
    if (purge_fail) say(true, "Apaguei %d conversa(s); %d não deu certo. Tente de novo.", purge_ok, purge_fail);
    else say(false, "Histórico apagado: %d conversa(s) neste computador. O que seus amigos guardam não é afetado.", purge_ok);
    storage_measure(&g_sz);
}

static void jesc(FILE *f, const char *t) { char b[400]; json_escape(t ? t : "", b, sizeof b); fputs(b, f); }
/* Contatos, grupos e ajustes. Mensagens e anexos NÃO entram (e a senha, o token e as chaves nunca). */
static bool export_data(const app_state_t *s, const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) return false;
    SYSTEMTIME t; GetSystemTime(&t);
    fprintf(f, "{\n  \"app\": \"Briks\", \"version\": \"%s\", \"exported_at\": \"%04d-%02d-%02dT%02d:%02d:%02dZ\",\n", update_current_version(), t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    fprintf(f, "  \"messages_included\": false, \"attachments_included\": false,\n  \"user\": \""); jesc(f, s->user_name); fprintf(f, "\",\n  \"friends\": [");
    for (int i = 0; i < s->friend_count; i++) { fprintf(f, "%s\n    {\"id\": \"", i ? "," : ""); jesc(f, s->friends[i].id); fprintf(f, "\", \"name\": \""); jesc(f, s->friends[i].name); fprintf(f, "\"}"); }
    fprintf(f, "\n  ],\n  \"groups\": [");
    for (int g = 0; g < s->group_count; g++) {
        const group_t *gr = &s->groups[g];
        fprintf(f, "%s\n    {\"id\": \"", g ? "," : ""); jesc(f, gr->id); fprintf(f, "\", \"name\": \""); jesc(f, gr->name); fprintf(f, "\", \"channels\": [");
        for (int k = 0; k < gr->nch; k++) { fprintf(f, "%s{\"id\": \"", k ? ", " : ""); jesc(f, gr->ch[k].id); fprintf(f, "\", \"name\": \""); jesc(f, gr->ch[k].name); fprintf(f, "\", \"voice\": %s}", gr->ch[k].voice ? "true" : "false"); }
        fprintf(f, "]}");
    }
    fprintf(f, "\n  ],\n  \"settings\": {");
    settings_export_json(f);
    fprintf(f, "},\n  \"storage_bytes\": {\"messages_db\": %llu, \"history\": %llu, \"received_files\": %llu, \"other\": %llu}\n}\n", g_sz.msgs, g_sz.hist, g_sz.files, g_sz.other);
    fclose(f); return true;
}

static void do_export(const app_state_t *s) {
    char path[MAX_PATH] = "briks-meus-dados.json";
    const char *e = getenv("BRIKS_EXPORT_FILE");   /* testes: sem a janela de salvar */
    if (e && e[0]) snprintf(path, sizeof path, "%s", e);
    else {
        OPENFILENAMEA o; memset(&o, 0, sizeof o); o.lStructSize = sizeof o;
        o.lpstrFilter = "JSON (*.json)\0*.json\0"; o.lpstrFile = path; o.nMaxFile = sizeof path; o.lpstrDefExt = "json"; o.lpstrTitle = "Exportar meus dados";
        o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameA(&o)) { say(false, "Exportação cancelada."); return; }
    }
    if (export_data(s, path)) say(false, "Dados exportados: contatos, grupos e ajustes (sem mensagens, anexos, senha nem chaves). Arquivo: %s", path);
    else say(true, "Não consegui gravar em %s.", path);
}

static void legend(cui_ctx *ui, cui_color c, const char *name, unsigned long long b, const char *what) {
    char sz[32]; storage_fmt(b, sz, sizeof sz);
    cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
        cui_box(ui, .width = CUI_PX(10), .height = CUI_PX(10), .radius = 5, .bg = c) {}
        cui_text(ui, name, .size = 13, .width = CUI_FILL);
        cui_text(ui, sz, .size = 13, .color = B_MUTED);
    }
    cui_text(ui, what, .size = 11, .color = g_th.ph, .width = CUI_FILL);
}

void data_ui(cui_ctx *ui, app_state_t *s) {
    DCARD(ui, "USO NESTE COMPUTADOR") {
        char b[32]; storage_fmt(g_sz.total, b, sizeof b);
        cui_row(ui, .width = CUI_FILL) { cui_text(ui, "Dados do Briks", .color = B_MUTED, .width = CUI_FILL); cui_text(ui, b); }
        const unsigned long long v[4] = { g_sz.msgs, g_sz.hist, g_sz.files, g_sz.other };
        const cui_color col[4] = { B_C3, B_C1, B_ONLINE, B_MUTED };
        cui_row(ui, .width = CUI_FILL, .height = CUI_PX(8), .gap = 2) {
            for (int i = 0; i < 4; i++) {
                if (!v[i] || !g_sz.total) continue;
                float p = 100.0f * (float)v[i] / (float)g_sz.total; if (p < 1.5f) p = 1.5f;
                cui_push_id_int(ui, 7100 + i);
                cui_box(ui, .width = CUI_PCT(p), .height = CUI_PX(8), .radius = 4, .bg = col[i]) {}
                cui_pop_id(ui);
            }
        }
        legend(ui, col[0], "Mensagens e grupos", g_sz.msgs, "Banco local: conversas, amigos e grupos.");
        legend(ui, col[1], "Histórico sincronizado", g_sz.hist, "Cópia das conversas trocada com os amigos.");
        legend(ui, col[2], "Anexos recebidos", g_sz.files, "Arquivos que amigos enviaram por transferência.");
        legend(ui, col[3], "Contas, perfil e ajustes", g_sz.other, "Sessão, foto, tema e configurações.");
        cui_text(ui, "Tudo fica neste dispositivo: nada disso está num servidor do Briks.", .size = 12, .color = B_MUTED, .width = CUI_FILL);
        if (cui_button(ui, "Atualizar medida", .variant = CUI_BUTTON_LINK, .size = CUI_SIZE_SM)) storage_measure(&g_sz);
    }
    DCARD(ui, "LIMPEZA") {
        cui_row(ui, .gap = 8, .width = CUI_FILL, .wrap = true) {
            if (cui_button(ui, "Limpar anexos", .variant = CUI_BUTTON_SECONDARY)) { data_confirm = data_confirm == 1 ? 0 : 1; data_msg[0] = 0; }
            if (cui_button(ui, "Apagar histórico de mensagens", .variant = CUI_BUTTON_SECONDARY, .disabled = purge_running)) { data_confirm = data_confirm == 2 ? 0 : 2; data_msg[0] = 0; }
            if (cui_button(ui, "Exportar meus dados", .variant = CUI_BUTTON_SECONDARY)) { data_confirm = 0; do_export(s); }
        }
        if (data_confirm == 1) {
            cui_text(ui, "Apagar os arquivos recebidos deste computador? Os cartões deles saem das conversas. Arquivos que você salvou em outra pasta não são tocados. Não dá para desfazer.", .size = 12, .color = B_DANGER, .width = CUI_FILL);
            cui_row(ui, .gap = 8) {
                if (cui_button(ui, "Apagar anexos", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM)) {
                    unsigned long long fr = 0; int n = storage_clear_attachments(&fr); char sz[32]; storage_fmt(fr, sz, sizeof sz);
                    say(false, "%d arquivo(s) apagado(s), %s liberados.", n, sz); data_confirm = 0; storage_measure(&g_sz);
                }
                if (cui_button(ui, "Cancelar", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM)) data_confirm = 0;
            }
        }
        if (data_confirm == 2) {
            cui_text(ui, "Apagar todas as conversas guardadas neste computador (amigos e grupos) e o histórico sincronizado? Quem conversou com você mantém a própria cópia. Não dá para desfazer.", .size = 12, .color = B_DANGER, .width = CUI_FILL);
            cui_row(ui, .gap = 8) {
                if (cui_button(ui, "Apagar histórico", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM)) { data_confirm = 0; purge_history(s); }
                if (cui_button(ui, "Cancelar", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM)) data_confirm = 0;
            }
        }
        if (purge_running) cui_request_wakeup(ui, 0.1f);
        if (data_msg[0]) cui_text(ui, data_msg, .size = 12, .color = data_msg_bad ? B_DANGER : B_ONLINE, .width = CUI_FILL);
        cui_text(ui, "A exportação leva contatos, grupos e ajustes. Mensagens e anexos não entram nela ainda, e a senha, o token e as chaves nunca saem do computador.", .size = 11, .color = g_th.ph, .width = CUI_FILL);
    }
}

int data_probe(const char *n) {
    if (!strcmp(n, "data_confirm")) return data_confirm;
    if (!strcmp(n, "data_msg")) return data_msg[0] != 0;
    if (!strcmp(n, "data_msg_bad")) return data_msg_bad;
    if (!strcmp(n, "data_total_kb")) return (int)(g_sz.total / 1024);
    if (!strcmp(n, "data_files_kb")) return (int)(g_sz.files / 1024);
    if (!strcmp(n, "data_hist_kb")) return (int)(g_sz.hist / 1024);
    return -9999;
}
