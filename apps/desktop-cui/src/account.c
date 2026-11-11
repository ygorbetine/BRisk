/* Contas online: ponte entre a UI e o módulo `accounts` do briks-core (comandos account.*, friends.*, import.*).
 * A UI nunca vê tokens: só o estado (fora, em andamento, e-mail enviado, dentro) e os dados públicos do perfil.
 * Depois de entrar, a conta é ligada a um usuário do banco local deste computador (mensagens e grupos ainda ficam
 * no backend local): o core entrega uma credencial local aleatória, cifrada com a DPAPI, que não é a senha da conta. */
#include "briks_ui.h"
#include "backend.h"
#include "core.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <commdlg.h>

static void copy(char *dst, size_t cap, const char *src) { snprintf(dst, cap, "%s", src ? src : ""); }
static void wipe_phrase(app_state_t *s);

/* cmd + pares chave/valor de texto (escapados aqui). NULL termina a lista. */
static bool send_kv(const char *cmd, const char *k1, const char *v1, const char *k2, const char *v2, const char *k3, const char *v3) {
    char line[1400], esc[400]; size_t n = (size_t)snprintf(line, sizeof line, "{\"cmd\":\"%s\"", cmd);
    const char *ks[3] = { k1, k2, k3 }, *vs[3] = { v1, v2, v3 };
    for (int i = 0; i < 3 && ks[i]; i++) {
        json_escape(vs[i] ? vs[i] : "", esc, sizeof esc);
        n += (size_t)snprintf(line + n, sizeof line - n, ",\"%s\":\"%s\"", ks[i], esc);
        if (n >= sizeof line - 2) return false;
    }
    snprintf(line + n, sizeof line - n, "}");
    bool ok = core_send(line);
    memset(line, 0, sizeof line); memset(esc, 0, sizeof esc);   /* podia ter senha */
    return ok;
}

/* foto da conta: o core baixa e guarda em cache (PNG/JPEG); aqui só vira textura. path vazio = volta às iniciais */
static void set_photo(app_state_t *s, const char *path) {
    cui_ctx *ui = cui_app_ui(s->app);
    if (s->user_photo) { cui_image_destroy(ui, s->user_photo); s->user_photo = 0; }
    if (path && path[0]) { int w = 0, h = 0; s->user_photo = cui_image_load(ui, path, &w, &h); }
}

static bool need_core(app_state_t *s) {
    if (core_state() == CORE_READY) return true;
    copy(s->acct_error, sizeof s->acct_error, core_state() == CORE_FAILED ? core_error() : "O Briks ainda está iniciando. Tente de novo em instantes.");
    return false;
}

static void begin(app_state_t *s, const char *what) {
    s->acct_state = ACCT_BUSY; s->acct_error[0] = 0; s->acct_info[0] = 0;
    copy(s->acct_what, sizeof s->acct_what, what);
}

void account_oauth(app_state_t *s, const char *provider) {
    if (!need_core(s)) return;
    begin(s, "oauth"); copy(s->acct_provider, sizeof s->acct_provider, provider);
    send_kv("account.oauth", "provider", provider, NULL, NULL, NULL, NULL);
}

void account_submit(app_state_t *s) {
    if (s->acct_state == ACCT_BUSY) return;
    s->acct_error[0] = 0;
    if (s->acct_view == AV_NEWPASS) {
        if (strlen(s->password) < 8) { copy(s->acct_error, sizeof s->acct_error, "A senha precisa ter pelo menos 8 caracteres."); return; }
        if (strcmp(s->password, s->confirm)) { copy(s->acct_error, sizeof s->acct_error, "As senhas não conferem."); return; }
        if (!need_core(s)) return;
        copy(s->acct_what, sizeof s->acct_what, "password");
        send_kv("account.password", "password", s->password, NULL, NULL, NULL, NULL);
        return;
    }
    if (!strchr(s->email, '@')) { copy(s->acct_error, sizeof s->acct_error, "Digite um e-mail válido, como voce@exemplo.com."); return; }
    if (s->acct_view == AV_SIGNUP) {
        if (!s->name[0]) { copy(s->acct_error, sizeof s->acct_error, "Diga como devemos chamar você."); return; }
        if (strlen(s->password) < 8) { copy(s->acct_error, sizeof s->acct_error, "A senha precisa ter pelo menos 8 caracteres."); return; }
        if (strcmp(s->password, s->confirm)) { copy(s->acct_error, sizeof s->acct_error, "As senhas não conferem."); return; }
    }
    if (s->acct_view == AV_LOGIN && !s->password[0]) { copy(s->acct_error, sizeof s->acct_error, "Digite sua senha."); return; }
    if (!need_core(s)) return;
    if (s->acct_view == AV_SIGNUP) { begin(s, "signup"); send_kv("account.signup", "email", s->email, "password", s->password, "name", s->name); }
    else if (s->acct_view == AV_RECOVER) { begin(s, "recover"); send_kv("account.recover", "email", s->email, NULL, NULL, NULL, NULL); }
    else { begin(s, "login"); send_kv("account.login", "email", s->email, "password", s->password, NULL, NULL); }
}

void account_submit_code(app_state_t *s) {
    if (!s->acct_code[0] || !need_core(s)) return;
    s->acct_error[0] = 0;
    send_kv("account.code", "email", s->email, "code", s->acct_code, "kind", s->acct_kind);
}

void account_cancel(app_state_t *s) {
    if (core_state() == CORE_READY) core_send("{\"cmd\":\"account.cancel\"}");
    else s->acct_state = ACCT_OUT;
}

void account_retry(app_state_t *s) { s->acct_asked = false; s->acct_state = ACCT_UNKNOWN; s->acct_error[0] = 0; }

void account_logout(app_state_t *s) {
    if (core_state() == CORE_READY) core_send("{\"cmd\":\"account.logout\"}");
    s->acct_state = ACCT_OUT; s->acct_view = AV_MAIN; s->acct_bridge = BRIDGE_NONE;
    s->acct_user_id[0] = s->acct_nick[0] = s->acct_error[0] = s->acct_info[0] = 0;
    s->user_nick[0] = 0; set_photo(s, NULL);
    memset(s->bridge_pw, 0, sizeof s->bridge_pw);
    s->legacy_count = 0; s->import_state = 0; s->import_msg[0] = 0;
    wipe_phrase(s); memset(s->backup_input, 0, sizeof s->backup_input);
    s->backup_known = BKP_UNKNOWN; s->backup_msg[0] = 0; s->backup_busy = false;
    s->backup_phrase_open = s->backup_restore_open = s->backup_delete_open = false;
}

void account_friends(app_state_t *s) { (void)s; if (core_state() == CORE_READY) core_send("{\"cmd\":\"friends.list\"}"); }

void account_add_friend(app_state_t *s) {
    if (!s->add_email[0]) return;
    if (core_state() != CORE_READY) { copy(s->notice, sizeof s->notice, "Sem conexão com o Briks. Tente de novo."); return; }
    copy(s->notice, sizeof s->notice, "Enviando…");
    send_kv("friends.request", "nick", s->add_email, NULL, NULL, NULL, NULL);
}

void account_respond_friend(app_state_t *s, const char *request_id, bool accept) {
    (void)s;
    char line[160], id[96]; json_escape(request_id, id, sizeof id);
    snprintf(line, sizeof line, "{\"cmd\":\"friends.respond\",\"request_id\":\"%s\",\"accept\":%s}", id, accept ? "true" : "false");
    core_send(line);
}

void account_set_nick(app_state_t *s) {
    if (!s->nick_edit[0] || core_state() != CORE_READY) return;
    copy(s->nick_msg, sizeof s->nick_msg, "Salvando…");
    send_kv("account.nick", "nick", s->nick_edit, NULL, NULL, NULL, NULL);
}

void account_set_name(app_state_t *s) {
    if (core_state() != CORE_READY) return;
    if (!s->name_edit[0]) { copy(s->profile_msg, sizeof s->profile_msg, "O nome não pode ficar vazio."); return; }
    copy(s->profile_msg, sizeof s->profile_msg, "Salvando…");
    send_kv("account.name", "name", s->name_edit, NULL, NULL, NULL, NULL);
}

void account_pick_photo(app_state_t *s) {
    if (core_state() != CORE_READY) return;
    wchar_t wpath[1024] = L""; char path[3072];
    OPENFILENAMEW of = { sizeof of };
    of.hwndOwner = GetActiveWindow();
    of.lpstrFilter = L"Imagens (PNG, JPEG)\0*.png;*.jpg;*.jpeg\0";
    of.lpstrFile = wpath; of.nMaxFile = (DWORD)(sizeof wpath / sizeof wpath[0]);
    of.lpstrTitle = L"Escolher foto de perfil";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&of)) return;   /* cancelou */
    if (!WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, (int)sizeof path, NULL, NULL)) return;
    copy(s->profile_msg, sizeof s->profile_msg, "Enviando a foto…");
    char line[6400], esc[6200]; json_escape(path, esc, sizeof esc);
    snprintf(line, sizeof line, "{\"cmd\":\"account.photo\",\"path\":\"%s\"}", esc);
    core_send(line);
}

void account_reset_photo(app_state_t *s) {
    if (core_state() != CORE_READY) return;
    copy(s->profile_msg, sizeof s->profile_msg, "Só um instante…");
    core_send("{\"cmd\":\"account.photo_reset\"}");
}

static void wipe_phrase(app_state_t *s) { memset(s->backup_words, 0, sizeof s->backup_words); s->backup_ack = false; }

void account_ice_get(app_state_t *s) { s->ice_asked = true; if (core_state() == CORE_READY) core_send("{\"cmd\":\"ice.get\"}"); }

void account_ice_save(app_state_t *s) {
    if (core_state() != CORE_READY || s->ice_busy) return;
    s->ice_busy = true; copy(s->ice_msg, sizeof s->ice_msg, "Salvando…");
    send_kv("ice.set", "urls", s->ice_urls, "username", s->ice_user, "credential", s->ice_pass);
    memset(s->ice_pass, 0, sizeof s->ice_pass);   /* a senha não fica na memória da UI */
}

void account_ice_clear(app_state_t *s) {
    if (core_state() != CORE_READY || s->ice_busy) return;
    s->ice_busy = true; s->ice_urls[0] = s->ice_user[0] = 0; memset(s->ice_pass, 0, sizeof s->ice_pass);
    core_send("{\"cmd\":\"ice.clear\"}");
}

void account_ice_test(app_state_t *s) {
    if (core_state() != CORE_READY || s->ice_testing) return;
    s->ice_testing = true; s->ice_test_level = 0; copy(s->ice_test_msg, sizeof s->ice_test_msg, "Testando… (até 8 segundos)");
    core_send("{\"cmd\":\"ice.test\"}");
}

void account_backup_status(app_state_t *s) { (void)s; if (core_state() == CORE_READY) core_send("{\"cmd\":\"backup.status\"}"); }

void account_backup_create(app_state_t *s) {
    if (core_state() != CORE_READY) return;
    wipe_phrase(s); s->backup_msg[0] = 0;
    core_send("{\"cmd\":\"backup.create\"}");
}

void account_backup_confirm(app_state_t *s) {
    if (core_state() != CORE_READY || !s->backup_ack || s->backup_busy) return;
    s->backup_busy = true;
    core_send("{\"cmd\":\"backup.confirm\"}");
}

void account_backup_cancel(app_state_t *s) {
    wipe_phrase(s); s->backup_phrase_open = false;
    if (core_state() == CORE_READY) core_send("{\"cmd\":\"backup.cancel\"}");
}

void account_backup_restore(app_state_t *s) {
    if (core_state() != CORE_READY || s->backup_busy || !s->backup_input[0]) return;
    s->backup_busy = true; s->backup_msg[0] = 0;
    send_kv("backup.restore", "phrase", s->backup_input, NULL, NULL, NULL, NULL);
}

void account_backup_delete(app_state_t *s) {
    if (core_state() != CORE_READY || s->backup_busy) return;
    s->backup_busy = true;
    core_send("{\"cmd\":\"backup.delete\"}");
}

void account_import(app_state_t *s, bool apply) {
    if (core_state() != CORE_READY) return;
    if (apply) { s->import_state = 2; copy(s->import_msg, sizeof s->import_msg, "Importando…"); }
    core_send(apply ? "{\"cmd\":\"import.apply\"}" : "{\"cmd\":\"import.probe\"}");
}

void account_bridge_done(app_state_t *s, bool ok) {
    memset(s->bridge_pw, 0, sizeof s->bridge_pw);
    if (ok) { s->acct_bridge = BRIDGE_NONE; account_friends(s); account_import(s, false); account_backup_status(s); return; }
    s->acct_bridge = BRIDGE_FAILED;
    copy(s->acct_error, sizeof s->acct_error, s->error[0] ? s->error : "Não foi possível abrir os dados locais deste computador.");
    s->error[0] = 0;
}

static bool jbool(const jval *j, const char *k) { const jval *v = json_get(j, k); return v && v->type == J_BOOL && v->b; }
static int jint(const jval *j, const char *k) { const jval *v = json_get(j, k); return v && v->type == J_NUM ? (int)v->num : 0; }

static void on_state(app_state_t *s, const jval *j) {
    const char *st = json_str(j, "state", "");
    s->acct_configured = jbool(j, "configured"); s->acct_mock = jbool(j, "mock");
    const char *msg = json_str(j, "message", "");
    if (!strcmp(st, "busy")) {
        s->acct_state = ACCT_BUSY;
        copy(s->acct_what, sizeof s->acct_what, json_str(j, "what", ""));
        copy(s->acct_provider, sizeof s->acct_provider, json_str(j, "provider", ""));
    } else if (!strcmp(st, "email_sent")) {
        s->acct_state = ACCT_EMAIL_SENT;
        copy(s->acct_kind, sizeof s->acct_kind, json_str(j, "kind", "signup"));
        copy(s->acct_error, sizeof s->acct_error, msg);
        memset(s->password, 0, sizeof s->password); memset(s->confirm, 0, sizeof s->confirm);
    } else if (!strcmp(st, "signed_in")) {
        const jval *u = json_get(j, "user");
        bool same = s->acct_state == ACCT_IN && !strcmp(s->acct_user_id, json_str(u, "id", ""));
        s->acct_state = ACCT_IN; s->acct_error[0] = 0; s->acct_code[0] = 0;
        copy(s->acct_user_id, sizeof s->acct_user_id, json_str(u, "id", ""));
        copy(s->user_name, sizeof s->user_name, json_str(u, "display_name", "Você"));
        copy(s->user_email, sizeof s->user_email, json_str(u, "email", ""));
        if (!s->acct_nick[0]) copy(s->user_nick, sizeof s->user_nick, json_str(j, "nick_hint", ""));   /* provisório até o servidor devolver o @nick */
        if (!same) { const char *dp = getenv("BRIKS_DEV_PHOTO"); set_photo(s, dp ? dp : json_str(j, "photo", "")); }   /* BRIKS_DEV_PHOTO: captura com uma foto local */                                             /* foto já em cache de um login anterior */
        memset(s->password, 0, sizeof s->password); memset(s->confirm, 0, sizeof s->confirm);
        if (jbool(j, "recovery")) { s->acct_view = AV_NEWPASS; return; }   /* veio do e-mail de recuperação: escolhe a senha nova antes de entrar */
        if (same && s->screen != SCREEN_AUTH) return;                      /* cancelar um fluxo com a sessão já aberta */
        s->acct_bridge = BRIDGE_WAIT;
        core_send("{\"cmd\":\"local.credentials\"}");
    } else {   /* signed_out */
        const bool was_busy = s->acct_state == ACCT_BUSY;
        s->acct_state = ACCT_OUT; s->acct_bridge = BRIDGE_NONE;
        s->acct_user_id[0] = s->acct_nick[0] = s->user_nick[0] = 0; set_photo(s, NULL);
        copy(s->acct_error, sizeof s->acct_error, strcmp(json_str(j, "error_code", ""), "cancelled") ? msg : "");
        copy(s->acct_info, sizeof s->acct_info, strcmp(json_str(j, "error_code", ""), "cancelled") || !was_busy ? "" : "Login cancelado.");
        if (s->acct_view == AV_NEWPASS) s->acct_view = AV_MAIN;
        memset(s->password, 0, sizeof s->password); memset(s->confirm, 0, sizeof s->confirm);
        if (s->screen != SCREEN_AUTH) { rtc_leave_room(s); app_local_logout(s); }   /* sessão expirou com o app aberto */
    }
}

static void on_friends(app_state_t *s, const jval *j) {
    const jval *f = json_get(j, "friends");
    char keep[48] = ""; if (s->active_friend >= 0 && s->active_friend < s->friend_count) copy(keep, sizeof keep, s->friends[s->active_friend].id);
    s->friend_count = s->pending_count = 0;
    for (int i = 0; i < json_len(f); i++) {
        const jval *p = json_at(f, i); const char *state = json_str(p, "state", "");
        person_t *d = NULL;
        if (!strcmp(state, "friend") && s->friend_count < MAX_FRIENDS) d = &s->friends[s->friend_count++];
        else if (!strcmp(state, "incoming") && s->pending_count < MAX_FRIENDS) d = &s->pending[s->pending_count++];
        if (!d) continue;   /* pedidos enviados ("outgoing") não entram nas listas */
        memset(d, 0, sizeof *d);
        copy(d->id, sizeof d->id, json_str(p, "user_id", ""));
        copy(d->name, sizeof d->name, json_str(p, "display_name", "?"));
        copy(d->request_id, sizeof d->request_id, json_str(p, "request_id", ""));
        copy(d->status, sizeof d->status, json_str(p, "status", ""));
    }
    int idx = -1;
    for (int i = 0; i < s->friend_count; i++) if (keep[0] && !strcmp(s->friends[i].id, keep)) idx = i;
    if (s->active_friend >= 0 && idx < 0) { s->active_friend = -1; } else if (idx >= 0) s->active_friend = idx;
}

bool account_handle_event(app_state_t *s, const char *line) {
    jval *j = json_parse(line);
    if (!j) return false;
    const char *ev = json_str(j, "ev", "");
    bool known = true;
    if (!strcmp(ev, "account.state")) on_state(s, j);
    else if (!strcmp(ev, "account.profile")) {
        copy(s->acct_nick, sizeof s->acct_nick, json_str(j, "nick", ""));
        if (s->acct_nick[0]) copy(s->user_nick, sizeof s->user_nick, s->acct_nick);
        if (json_str(j, "display_name", "")[0]) copy(s->user_name, sizeof s->user_name, json_str(j, "display_name", ""));   /* vale o nome do perfil (pode ter sido trocado) */
        if (json_str(j, "message", "")[0]) copy(s->profile_msg, sizeof s->profile_msg, json_str(j, "message", ""));
        if (jbool(j, "changed")) { copy(s->nick_msg, sizeof s->nick_msg, "@nick alterado."); s->nick_edit[0] = 0; }
    } else if (!strcmp(ev, "account.avatar")) {
        if (s->acct_state == ACCT_IN) set_photo(s, json_str(j, "path", ""));
    } else if (!strcmp(ev, "backup.state")) {
        s->backup_known = !jbool(j, "exists") ? BKP_NONE : jbool(j, "matches") ? BKP_OK : BKP_OTHER;
        const char *d = json_str(j, "updated_at", "");   /* 2026-11-10T... -> 10/11/2026 */
        if (strlen(d) >= 10) snprintf(s->backup_date, sizeof s->backup_date, "%.2s/%.2s/%.4s", d + 8, d + 5, d); else s->backup_date[0] = 0;
        if (json_str(j, "message", "")[0]) copy(s->backup_msg, sizeof s->backup_msg, json_str(j, "message", ""));
    } else if (!strcmp(ev, "backup.phrase")) {
        const jval *w = json_get(j, "words");
        wipe_phrase(s);
        for (int i = 0; i < 12 && i < json_len(w); i++) { const jval *x = json_at(w, i); copy(s->backup_words[i], sizeof s->backup_words[i], x && x->type == J_STR ? x->str : ""); }
        s->backup_phrase_open = true; s->backup_busy = false;
    } else if (!strcmp(ev, "backup.result")) {
        const char *op = json_str(j, "op", ""); const bool ok = jbool(j, "ok");
        s->backup_busy = false;
        copy(s->backup_msg, sizeof s->backup_msg, json_str(j, "message", ""));
        if (!strcmp(op, "save")) { wipe_phrase(s); s->backup_phrase_open = false; }
        if (!strcmp(op, "restore") && ok) { memset(s->backup_input, 0, sizeof s->backup_input); s->backup_restore_open = false; }
        if (!strcmp(op, "delete")) s->backup_delete_open = false;
    } else if (!strcmp(ev, "identity.changed")) {
        /* a identidade deste aparelho foi trocada pela recuperada: a sessão P2P é reaberta com a chave nova */
        if (s->in_room) rtc_leave_room(s);
        s->rtc_session_sent = s->rtc_session_ready = false;
        copy(s->rtc_fp, sizeof s->rtc_fp, json_str(j, "fingerprint", ""));
    } else if (!strcmp(ev, "account.notice")) {
        if (s->nick_open) copy(s->nick_msg, sizeof s->nick_msg, json_str(j, "message", ""));
        else if (s->profile_open) copy(s->profile_msg, sizeof s->profile_msg, json_str(j, "message", ""));
        else if (s->acct_bridge == BRIDGE_WAIT && !strcmp(json_str(j, "error_code", ""), "storage")) { s->acct_bridge = BRIDGE_FAILED; copy(s->acct_error, sizeof s->acct_error, json_str(j, "message", "")); }
        else copy(s->acct_info, sizeof s->acct_info, json_str(j, "message", ""));
    } else if (!strcmp(ev, "account.password")) {
        if (jbool(j, "ok")) {   /* senha nova gravada: segue para dentro do app */
            memset(s->password, 0, sizeof s->password); memset(s->confirm, 0, sizeof s->confirm);
            s->acct_view = AV_MAIN; s->acct_bridge = BRIDGE_WAIT;
            core_send("{\"cmd\":\"local.credentials\"}");
        } else copy(s->acct_error, sizeof s->acct_error, json_str(j, "message", ""));
    } else if (!strcmp(ev, "local.credentials")) {
        copy(s->bridge_email, sizeof s->bridge_email, json_str(j, "email", ""));
        copy(s->bridge_pw, sizeof s->bridge_pw, json_str(j, "password", ""));
        /* account_tick conclui quando o backend local estiver pronto */
    } else if (!strcmp(ev, "ice.state")) {
        s->ice_busy = false; s->ice_turn = jbool(j, "turn"); s->ice_has_cred = jbool(j, "has_credential");
        copy(s->ice_source, sizeof s->ice_source, json_str(j, "source", "default"));
        if (json_str(j, "message", "")[0]) copy(s->ice_msg, sizeof s->ice_msg, json_str(j, "message", ""));
        else if (json_str(j, "warn", "")[0]) snprintf(s->ice_msg, sizeof s->ice_msg, "Configuração ignorada: %s", json_str(j, "warn", ""));
        if (!s->ice_edit_init || jbool(j, "ok")) { copy(s->ice_urls, sizeof s->ice_urls, json_str(j, "urls", "")); copy(s->ice_user, sizeof s->ice_user, json_str(j, "username", "")); s->ice_edit_init = true; }
        s->ice_test_level = 0; s->ice_test_msg[0] = 0;   /* a configuração mudou: o resultado antigo não vale mais */
    } else if (!strcmp(ev, "ice.test")) {
        s->ice_testing = false;
        const char *lv = json_str(j, "level", "none");
        s->ice_test_level = !strcmp(lv, "relay") ? 3 : !strcmp(lv, "stun") ? 2 : 1;
        copy(s->ice_test_msg, sizeof s->ice_test_msg, json_str(j, "message", ""));
    } else if (!strcmp(ev, "friends.list")) on_friends(s, j);
    else if (!strcmp(ev, "friends.result")) {
        copy(s->notice, sizeof s->notice, json_str(j, "message", ""));
        if (jbool(j, "ok")) s->add_email[0] = 0;
    } else if (!strcmp(ev, "import.result")) {
        const char *st = json_str(j, "state", "");
        const jval *r = json_get(j, "report");
        if (!strcmp(st, "ready")) {
            int f = jint(j, "friends"), g = jint(j, "groups"), m = jint(j, "messages");
            s->import_state = (f || g || m) ? 1 : 4;
            snprintf(s->import_msg, sizeof s->import_msg, "Encontramos %d amigo%s, %d grupo%s e %d mensage%s do Briks antigo neste computador.", f, f == 1 ? "" : "s", g, g == 1 ? "" : "s", m, m == 1 ? "m" : "ns");
        } else if (!strcmp(st, "applied")) {
            s->import_state = 3; s->reload_groups = true;
            snprintf(s->import_msg, sizeof s->import_msg, "Importado: %d amigo%s, %d grupo%s e %d mensage%s.", jint(r, "friends"), jint(r, "friends") == 1 ? "" : "s", jint(r, "groups"), jint(r, "groups") == 1 ? "" : "s", jint(r, "messages"), jint(r, "messages") == 1 ? "m" : "ns");
        } else if (!strcmp(st, "imported") || !strcmp(st, "already")) {
            s->import_state = 3; copy(s->import_msg, sizeof s->import_msg, "Os dados do Briks antigo já foram importados.");
            if (!strcmp(st, "already")) s->reload_groups = true;
        } else if (!strcmp(st, "no-source")) { s->import_state = 4; s->import_msg[0] = 0; }
        else { s->import_state = jbool(j, "applied") ? 1 : 4; copy(s->import_msg, sizeof s->import_msg, json_str(j, "message", json_str(j, "error", "Não foi possível ler o Briks antigo."))); }
    } else known = false;
    json_free(j);
    return known;
}

void account_tick(app_state_t *s) {
    static bool init;
    if (!init) {
        init = true;
        if (getenv("BRIKS_DEMO") || getenv("BRIKS_DEV_AUTH")) {   /* capturas e teste antigo do backend local: sem conta online */
            s->acct_demo = true; s->acct_state = ACCT_OUT; s->acct_configured = true;
            const char *v = getenv("BRIKS_DEV_ACCT");   /* captura das outras telas: login|signup|recover|browser|email|newpass|error|setup */
            if (v) {
                if (!strcmp(v, "login")) s->acct_view = AV_LOGIN; else if (!strcmp(v, "signup")) s->acct_view = AV_SIGNUP;
                else if (!strcmp(v, "recover")) s->acct_view = AV_RECOVER; else if (!strcmp(v, "newpass")) { s->acct_view = AV_NEWPASS; s->acct_state = ACCT_IN; }
                else if (!strcmp(v, "browser")) { s->acct_state = ACCT_BUSY; copy(s->acct_what, sizeof s->acct_what, "oauth"); copy(s->acct_provider, sizeof s->acct_provider, "google"); }
                else if (!strcmp(v, "email")) { s->acct_state = ACCT_EMAIL_SENT; copy(s->acct_kind, sizeof s->acct_kind, "signup"); copy(s->email, sizeof s->email, "voce@exemplo.com"); }
                else if (!strcmp(v, "error")) { s->acct_view = AV_LOGIN; copy(s->email, sizeof s->email, "voce@exemplo.com"); copy(s->acct_error, sizeof s->acct_error, "E-mail ou senha incorretos."); }
                else if (!strcmp(v, "setup")) { s->acct_configured = false; copy(s->acct_error, sizeof s->acct_error, "Servidor de contas não configurado. Crie o arquivo supabase.json na pasta de dados do Briks (veja docs/plans/contas-online.md)."); }
            }
        }
    }
    if (s->acct_demo) return;
    core_status_t cs = core_state();
    if (cs == CORE_STOPPED) { core_start(); return; }
    if (cs == CORE_FAILED) {
        if (s->acct_state == ACCT_UNKNOWN || s->acct_state == ACCT_BUSY) { s->acct_state = ACCT_OUT; copy(s->acct_error, sizeof s->acct_error, core_error()); }
        return;
    }
    if (cs != CORE_READY) { cui_request_redraw(cui_app_ui(s->app)); return; }
    if (!s->acct_asked) { s->acct_asked = true; core_send("{\"cmd\":\"account.status\"}"); }
    {   /* ganchos de teste automatizado: BRIKS_DEV_ACCT_AUTO=google|discord entra sozinho; BRIKS_DEV_IMPORT=1 importa o Briks antigo */
        static bool auto_done, import_done; const char *a = getenv("BRIKS_DEV_ACCT_AUTO");
        if (a || getenv("BRIKS_DEV_REDRAW")) cui_request_redraw(cui_app_ui(s->app));   /* capturas: mantém os quadros andando */
        if (a && !auto_done && s->acct_state == ACCT_OUT && s->acct_configured) { auto_done = true; account_oauth(s, a); }
        static bool prof_done; const char *pr = getenv("BRIKS_DEV_PROFILE");   /* "Nome|caminho da foto": troca nome e foto depois de entrar */
        if (pr && !prof_done && s->screen == SCREEN_SOCIAL && s->acct_nick[0]) {
            prof_done = true;
            char buf[1200]; snprintf(buf, sizeof buf, "%s", pr); char *bar = strchr(buf, '|'); if (bar) *bar++ = 0;
            snprintf(s->name_edit, sizeof s->name_edit, "%s", buf); account_set_name(s);
            if (bar && bar[0]) { char line[2600], esc[2400]; json_escape(bar, esc, sizeof esc); snprintf(line, sizeof line, "{\"cmd\":\"account.photo\",\"path\":\"%s\"}", esc); core_send(line); }
        }
        static int bk_step; const char *bk = getenv("BRIKS_DEV_BACKUP");   /* "phrase": abre o diálogo da frase; "saved": cria e confirma */
        if (bk && s->screen == SCREEN_SOCIAL && s->backup_known != BKP_UNKNOWN) {
            if (bk_step == 0) { bk_step = 1; account_backup_create(s); }
            else if (bk_step == 1 && s->backup_words[0][0] && !strcmp(bk, "saved")) { bk_step = 2; s->backup_ack = true; account_backup_confirm(s); }
        }
        if (getenv("BRIKS_DEV_IMPORT") && !import_done && s->import_state == 1) { import_done = true; account_import(s, true); }
    }
    if (s->acct_state == ACCT_UNKNOWN || s->acct_state == ACCT_BUSY || s->acct_bridge == BRIDGE_WAIT || s->acct_bridge == BRIDGE_RUNNING) cui_request_redraw(cui_app_ui(s->app));
    /* liga a conta ao banco local: tenta entrar; se o usuário local ainda não existe, app.c cadastra */
    if (s->acct_bridge == BRIDGE_WAIT && s->bridge_pw[0] && !s->busy && !s->call) {
        if (bk_status() == BK_FAILED) { s->acct_bridge = BRIDGE_FAILED; copy(s->acct_error, sizeof s->acct_error, bk_error()); return; }
        if (bk_status() != BK_READY) return;
        char e[200], p[160], b[420];
        json_escape(s->bridge_email, e, sizeof e); json_escape(s->bridge_pw, p, sizeof p);
        snprintf(b, sizeof b, "{\"email\":\"%s\",\"password\":\"%s\"}", e, p);
        s->acct_bridge = BRIDGE_RUNNING; s->token[0] = 0;
        app_begin(s, OP_BRIDGE_LOGIN, "POST", "/auth/login", b);
    }
    if (s->reload_groups && s->token[0] && !s->busy && !s->call) { s->reload_groups = false; app_begin(s, OP_GROUPS, "GET", "/p2p/groups", NULL); }
}
