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

/* Fotos dos amigos (PRF-03): o core baixa para o cache e manda friends.photos {photos:[{user_id,path}]}; aqui viram textura, uma por pessoa. */
static struct { char id[48]; char path[300]; cui_texture tex; } g_fph[MAX_FRIENDS];
static void fph_clear(cui_ctx *ui) { for (int i = 0; i < MAX_FRIENDS; i++) { if (g_fph[i].tex) cui_image_destroy(ui, g_fph[i].tex); memset(&g_fph[i], 0, sizeof g_fph[i]); } }
cui_texture friend_photo(const char *user_id) {
    if (!user_id || !user_id[0]) return 0;
    for (int i = 0; i < MAX_FRIENDS; i++) if (g_fph[i].tex && !strcmp(g_fph[i].id, user_id)) return g_fph[i].tex;
    return 0;
}
cui_texture friend_photo_by_name(const app_state_t *s, const char *name) {   /* mensagens e membros só trazem o nome: casa com a lista de amigos */
    if (!name || !name[0]) return 0;
    for (int i = 0; i < s->friend_count; i++) if (!strcmp(s->friends[i].name, name)) return friend_photo(s->friends[i].id);
    return 0;
}
static void on_friend_photos(app_state_t *s, const jval *j) {
    cui_ctx *ui = cui_app_ui(s->app);
    const jval *l = json_get(j, "photos");
    for (int k = 0; k < json_len(l); k++) {
        const jval *e = json_at(l, k); const char *id = json_str(e, "user_id", ""), *path = json_str(e, "path", "");
        if (!id[0] || !path[0]) continue;
        int slot = -1, freeslot = -1;
        for (int i = 0; i < MAX_FRIENDS; i++) { if (!strcmp(g_fph[i].id, id) && g_fph[i].id[0]) { slot = i; break; } if (freeslot < 0 && !g_fph[i].id[0]) freeslot = i; }
        if (slot < 0) slot = freeslot;
        if (slot < 0) continue;
        if (g_fph[slot].tex && !strcmp(g_fph[slot].path, path)) continue;   /* mesma foto: nada a recarregar */
        if (g_fph[slot].tex) { cui_image_destroy(ui, g_fph[slot].tex); g_fph[slot].tex = 0; }
        int w = 0, h = 0; cui_texture t = cui_image_load(ui, path, &w, &h);
        copy(g_fph[slot].id, sizeof g_fph[slot].id, id); copy(g_fph[slot].path, sizeof g_fph[slot].path, path); g_fph[slot].tex = t;
    }
    cui_request_redraw(ui);
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
    if (s->acct_view == AV_LOGIN && (s->email[0] == '@' || (s->email[0] && !strchr(s->email, '@')))) {   /* AUT-11: @nick -> e-mail guardado neste computador */
        char em[96]; if (login_hint_lookup(s->email, em, sizeof em)) copy(s->email, sizeof s->email, em);
        else { copy(s->acct_error, sizeof s->acct_error, "Não conheço esse @nick neste computador. Entre com o e-mail desta vez; da próxima você pode usar o @nick."); return; }
    }
    if (!strchr(s->email, '@')) { copy(s->acct_error, sizeof s->acct_error, "Digite um e-mail válido, como voce@exemplo.com."); return; }
    if (s->acct_view == AV_SIGNUP) {
        if (!s->name[0]) { copy(s->acct_error, sizeof s->acct_error, "Diga como devemos chamar você."); return; }
        if (strlen(s->password) < 8) { copy(s->acct_error, sizeof s->acct_error, "A senha precisa ter pelo menos 8 caracteres."); return; }
        if (strcmp(s->password, s->confirm)) { copy(s->acct_error, sizeof s->acct_error, "As senhas não conferem."); return; }
        { SYSTEMTIME st; GetLocalTime(&st); if (birth_check(s->birth, st.wYear, st.wMonth, st.wDay, s->acct_error, sizeof s->acct_error)) return; }   /* AUT-03: só maiores de 18; a data não é enviada nem guardada */
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
    if (core_state() == CORE_READY) { core_send("{\"cmd\":\"presence.clear\"}"); core_send("{\"cmd\":\"account.logout\"}"); s->pres_n = 0; }
    s->acct_state = ACCT_OUT; s->acct_view = AV_MAIN; s->acct_bridge = BRIDGE_NONE;
    s->acct_user_id[0] = s->acct_nick[0] = s->acct_error[0] = s->acct_info[0] = 0;
    s->user_nick[0] = 0; set_photo(s, NULL);
    memset(s->bridge_pw, 0, sizeof s->bridge_pw);
    s->legacy_count = 0; s->import_state = 0; s->import_msg[0] = 0;
    wipe_phrase(s); memset(s->backup_input, 0, sizeof s->backup_input);
    s->backup_known = BKP_UNKNOWN; s->backup_msg[0] = 0; s->backup_busy = false;
    s->backup_phrase_open = s->backup_restore_open = s->backup_delete_open = false;
}

void account_friends(app_state_t *s) {
    (void)s; if (core_state() != CORE_READY) return;
    core_send("{\"cmd\":\"friends.list\"}");
    core_send("{\"cmd\":\"friends.blocks\"}");   /* lista de bloqueados do servidor (0006); sem a RPC o core responde remote=false e vale a lista local */
}

/* bloquear/desbloquear no servidor (a lista local é atualizada por quem chama; sem a 0006 o core só confirma remote=false) */
void account_block_remote(app_state_t *s, const char *user_id, bool block) {
    (void)s; if (core_state() != CORE_READY || !user_id[0]) return;
    char id[96], line[200]; json_escape(user_id, id, sizeof id);
    snprintf(line, sizeof line, "{\"cmd\":\"friends.%s\",\"user_id\":\"%s\"}", block ? "block" : "unblock", id);
    core_send(line);
}

void account_add_friend(app_state_t *s) {
    if (!s->add_email[0]) return;
    if (core_state() != CORE_READY) { copy(s->notice, sizeof s->notice, "Sem conexão com o Briks. Tente de novo."); return; }
    copy(s->notice, sizeof s->notice, "Enviando…");
    send_kv("friends.request", "nick", s->add_email, NULL, NULL, NULL, NULL);
}

void account_remove_friend(app_state_t *s, const char *user_id) {
    if (core_state() != CORE_READY) { copy(s->notice, sizeof s->notice, "Sem conexão com o Briks. Tente de novo."); return; }
    send_kv("friends.remove", "user_id", user_id, NULL, NULL, NULL, NULL);
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

void account_logout_others(app_state_t *s) {
    if (core_state() != CORE_READY || s->others_busy) return;
    s->others_busy = true; copy(s->others_msg, sizeof s->others_msg, "Desconectando os outros computadores…");
    core_send("{\"cmd\":\"account.logout_others\"}");
}

void account_devices_get(app_state_t *s) {
    s->dev_asked_at = GetTickCount64();
    if (core_state() == CORE_READY) core_send("{\"cmd\":\"account.devices\"}");
}

void account_device_revoke(app_state_t *s, const char *peer_id) {
    if (core_state() != CORE_READY || !peer_id || !peer_id[0]) return;
    copy(s->dev_msg, sizeof s->dev_msg, "Desconectando…");
    send_kv("account.device_revoke", "peer_id", peer_id, NULL, NULL, NULL, NULL);
}

static void iso_date(char *out, size_t cap, const char *iso) {   /* "2026-11-11T10:20:30Z" -> "11/11/2026" */
    if (iso && strlen(iso) >= 10 && iso[4] == '-' && iso[7] == '-') snprintf(out, cap, "%.2s/%.2s/%.4s", iso + 8, iso + 5, iso);
    else out[0] = 0;
}

static bool jbool(const jval *j, const char *k);
static void on_devices(app_state_t *s, const jval *j) {
    s->dev_have = true; s->dev_remote = jbool(j, "remote"); s->dev_n = 0;
    copy(s->dev_current, sizeof s->dev_current, json_str(j, "current", ""));
    const jval *d = json_get(j, "devices");
    for (int i = 0; i < json_len(d) && s->dev_n < 12; i++) {
        const jval *e = json_at(d, i); int k = s->dev_n++;
        copy(s->devs[k].peer, sizeof s->devs[k].peer, json_str(e, "peer_id", ""));
        copy(s->devs[k].name, sizeof s->devs[k].name, json_str(e, "device_name", ""));
        iso_date(s->devs[k].created, sizeof s->devs[k].created, json_str(e, "created_at", ""));
        iso_date(s->devs[k].used, sizeof s->devs[k].used, json_str(e, "last_used_at", ""));
    }
    s->dev_confirm[0] = 0;
}

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
        s->acct_user_id[0] = s->acct_nick[0] = s->user_nick[0] = 0; set_photo(s, NULL); fph_clear(cui_app_ui(s->app));
        copy(s->acct_error, sizeof s->acct_error, strcmp(json_str(j, "error_code", ""), "cancelled") ? msg : "");
        copy(s->acct_info, sizeof s->acct_info, strcmp(json_str(j, "error_code", ""), "cancelled") || !was_busy ? "" : "Login cancelado.");
        if (s->acct_view == AV_NEWPASS) s->acct_view = AV_MAIN;
        memset(s->password, 0, sizeof s->password); memset(s->confirm, 0, sizeof s->confirm);
        if (s->screen != SCREEN_AUTH) { rtc_leave_room(s); app_local_logout(s); }   /* sessão expirou com o app aberto */
    }
}

/* friends.blocks {remote, blocks:[...]}: com a 0006 o servidor manda; bloqueios que só existiam aqui sobem para ele */
static void on_blocks(app_state_t *s, const jval *j) {
    if (!jbool(j, "remote")) return;   /* servidor sem a 0006: fica a lista local */
    const jval *b = json_get(j, "blocks");
    char ids[64][48]; int n = 0;
    for (int i = 0; i < json_len(b) && n < 64; i++) copy(ids[n++], sizeof ids[0], json_str(json_at(b, i), "user_id", ""));
    for (int i = 0; i < s->blocked_count; i++) {   /* local que o servidor não conhece: sobe */
        bool known = false; for (int k = 0; k < n; k++) if (!strcmp(ids[k], s->blocked[i].id)) known = true;
        if (!known) account_block_remote(s, s->blocked[i].id, true);
    }
    for (int i = 0; i < json_len(b) && i < 64; i++) {   /* servidor que a lista local não tem (outro computador): entra */
        const jval *e = json_at(b, i); const char *id = json_str(e, "user_id", "");
        if (id[0] && !friends_blocked(s, id)) { const char *nm = json_str(e, "display_name", json_str(e, "nick", "?")); friends_block(s, id, nm); }
    }
}

static void on_presence(app_state_t *s, const jval *j) {
    const jval *f = json_get(j, "friends");
    s->pres_remote = jbool(j, "remote"); s->pres_n = 0;
    for (int i = 0; i < json_len(f) && s->pres_n < MAX_FRIENDS; i++) {
        const jval *p = json_at(f, i);
        copy(s->pres[s->pres_n].id, sizeof s->pres[0].id, json_str(p, "user_id", ""));
        s->pres[s->pres_n].online = jbool(p, "online"); s->pres_n++;
    }
    cui_request_redraw(cui_app_ui(s->app));
}

/* batimento: ao entrar na conta, a cada 60 s, e na hora quando "Aparecer offline" muda. Sem a 0007 o core responde remote=false e nada mais é tentado com frequência */
void account_presence_tick(app_state_t *s) {
    static ULONGLONG last; static bool inv_sent; static char who[48];
    if (core_state() != CORE_READY || !s->acct_user_id[0] || s->acct_demo) { last = 0; who[0] = 0; return; }
    const ULONGLONG now = GetTickCount64();
    const bool changed = strcmp(who, s->acct_user_id) != 0 || inv_sent != s->invisible;
    if (!changed && last && now - last < 60000) return;
    if (!changed && !s->pres_remote && last && now - last < 300000) return;   /* servidor sem a 0007: só tenta de novo a cada 5 min */
    last = now; inv_sent = s->invisible; copy(who, sizeof who, s->acct_user_id);
    char line[96]; snprintf(line, sizeof line, "{\"cmd\":\"presence.beat\",\"invisible\":%s}", s->invisible ? "true" : "false");
    core_send(line);
}

static void on_friends(app_state_t *s, const jval *j) {
    s->friends_loaded = true;
    const jval *f = json_get(j, "friends");
    char keep[48] = ""; if (s->active_friend >= 0 && s->active_friend < s->friend_count) copy(keep, sizeof keep, s->friends[s->active_friend].id);
    s->friend_count = s->pending_count = s->outgoing_count = 0;
    for (int i = 0; i < json_len(f); i++) {
        const jval *p = json_at(f, i); const char *state = json_str(p, "state", "");
        person_t *d = NULL;
        if (friends_blocked(s, json_str(p, "user_id", ""))) continue;   /* bloqueados saem de todas as listas (ficam só na aba Bloqueados) */
        if (!strcmp(state, "friend") && s->friend_count < MAX_FRIENDS) d = &s->friends[s->friend_count++];
        else if (!strcmp(state, "incoming") && s->pending_count < MAX_FRIENDS) d = &s->pending[s->pending_count++];
        else if (!strcmp(state, "outgoing") && s->outgoing_count < MAX_FRIENDS) d = &s->outgoing[s->outgoing_count++];
        if (!d) continue;
        memset(d, 0, sizeof *d);
        copy(d->id, sizeof d->id, json_str(p, "user_id", ""));
        copy(d->name, sizeof d->name, json_str(p, "display_name", "?"));
        copy(d->request_id, sizeof d->request_id, json_str(p, "request_id", ""));
        copy(d->nick, sizeof d->nick, json_str(p, "nick", ""));
        copy(d->status, sizeof d->status, json_str(p, "status", ""));
        copy(d->fp, sizeof d->fp, json_str(p, "fingerprint", ""));
        const jval *pk = json_get(p, "public_key");
        if (pk && json_str(pk, "x", "")[0] && json_str(pk, "y", "")[0]) snprintf(d->jwk, sizeof d->jwk, "{\"kty\":\"EC\",\"crv\":\"P-256\",\"x\":\"%s\",\"y\":\"%s\"}", json_str(pk, "x", ""), json_str(pk, "y", ""));
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
        if (s->acct_nick[0]) { copy(s->user_nick, sizeof s->user_nick, s->acct_nick); if (!s->acct_demo) login_hint_save(s->acct_nick, s->user_email); }   /* dica de login por @nick (AUT-11) */
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
    } else if (!strcmp(ev, "account.others")) {
        s->others_busy = false; s->others_confirm = false;
        copy(s->others_msg, sizeof s->others_msg, json_str(j, "message", ""));
    } else if (!strcmp(ev, "account.devices")) on_devices(s, j);
    else if (!strcmp(ev, "account.device_result")) {
        copy(s->dev_msg, sizeof s->dev_msg, json_str(j, "message", ""));
    } else if (!strcmp(ev, "ice.state")) {
        s->ice_busy = false; s->ice_turn = jbool(j, "turn"); s->ice_has_cred = jbool(j, "has_credential");
        copy(s->ice_source, sizeof s->ice_source, json_str(j, "source", "default"));
        if (json_str(j, "message", "")[0]) copy(s->ice_msg, sizeof s->ice_msg, json_str(j, "message", ""));
        else if (json_str(j, "warn", "")[0]) snprintf(s->ice_msg, sizeof s->ice_msg, "Configuração ignorada: %s", json_str(j, "warn", ""));
        if (!s->ice_edit_init || jbool(j, "ok")) { copy(s->ice_urls, sizeof s->ice_urls, json_str(j, "urls", "")); copy(s->ice_user, sizeof s->ice_user, json_str(j, "username", "")); s->ice_edit_init = true; }
        if (json_get(j, "ok")) { s->ice_test_level = 0; s->ice_test_msg[0] = 0; }   /* salvou/removeu: o resultado antigo não vale mais */
    } else if (!strcmp(ev, "ice.test")) {
        s->ice_testing = false;
        const char *lv = json_str(j, "level", "none");
        s->ice_test_level = !strcmp(lv, "relay") ? 3 : !strcmp(lv, "stun") ? 2 : 1;
        copy(s->ice_test_msg, sizeof s->ice_test_msg, json_str(j, "message", ""));
    } else if (!strcmp(ev, "friends.list")) on_friends(s, j);
    else if (!strcmp(ev, "friends.photos")) on_friend_photos(s, j);
    else if (!strcmp(ev, "friends.blocks")) on_blocks(s, j);
    else if (!strcmp(ev, "presence.list")) on_presence(s, j);
    else if (!strcmp(ev, "friends.lookup")) {
        s->gnick_found = false; s->gnick_msg[0] = 0;
        if (jbool(j, "ok")) {
            const jval *h = json_get(j, "hit"); person_t *p = &s->gnick_hit; memset(p, 0, sizeof *p);
            copy(p->id, sizeof p->id, json_str(h, "user_id", "")); copy(p->name, sizeof p->name, json_str(h, "display_name", "?"));
            copy(p->nick, sizeof p->nick, json_str(h, "nick", "")); copy(p->fp, sizeof p->fp, json_str(h, "fingerprint", ""));
            s->gnick_found = p->id[0] && p->fp[0];
        } else copy(s->gnick_msg, sizeof s->gnick_msg, !strcmp(json_str(j, "error_code", ""), "server_not_ready") ? "A busca por @nick ainda não está ativa no servidor." : json_str(j, "message", "Não encontrado."));
    }
    else if (!strncmp(ev, "group.", 6)) groups_core_event(s, j, ev);
    else if (!strcmp(ev, "friend.result")) friends_p2p_event(s, j);
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
    if (s->acct_demo) {   /* captura: BRIKS_DEV_FPHOTO=caminho põe essa imagem como foto do primeiro amigo (nada baixado) */
        static bool fp_done; static ULONGLONG t0; const char *fp = getenv("BRIKS_DEV_FPHOTO"), *dl = getenv("BRIKS_DEV_FPHOTO_DELAY");   /* _DELAY: ms até a foto chegar (teste antes/depois) */
        if (!t0) t0 = GetTickCount64();
        if (fp && !fp_done && s->friend_count > 0 && (!dl || GetTickCount64() - t0 >= (ULONGLONG)atoi(dl))) {
            cui_request_redraw(cui_app_ui(s->app));
            fp_done = true; char esc[700], js[900]; json_escape(fp, esc, sizeof esc);
            snprintf(js, sizeof js, "{\"photos\":[{\"user_id\":\"%s\",\"path\":\"%s\"}]}", s->friends[0].id, esc);
            jval *j = json_parse(js); if (j) { on_friend_photos(s, j); json_free(j); }
        }
        return;
    }
    core_status_t cs = core_state();
    if (cs == CORE_STOPPED) { core_start(); return; }
    if (cs == CORE_FAILED) {
        if (s->acct_state == ACCT_UNKNOWN || s->acct_state == ACCT_BUSY) { s->acct_state = ACCT_OUT; copy(s->acct_error, sizeof s->acct_error, core_error()); }
        return;
    }
    if (cs != CORE_READY) { cui_request_redraw(cui_app_ui(s->app)); return; }
    if (!s->acct_asked) { s->acct_asked = true; core_send("{\"cmd\":\"account.status\"}"); }
    account_presence_tick(s);
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
