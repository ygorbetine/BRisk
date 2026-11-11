/* Lógica da aplicação: cada ação vira uma chamada HTTP ao backend local; app_pump consome a resposta. */
#include "briks_ui.h"
#include "backend.h"
#include "json.h"
#include "attach.h"
#include <stdio.h>
#include <string.h>

#define begin app_begin
static void copy(char *dst, size_t cap, const char *src) { snprintf(dst, cap, "%s", src ? src : ""); }

static void set_error(app_state_t *s, const char *body, const char *fallback) {
    jval *j = json_parse(body);
    copy(s->error, sizeof s->error, json_str(j, "message", fallback));
    json_free(j);
}

void app_begin(app_state_t *s, op_t op, const char *method, const char *path, const char *body) {
    s->op = op;
    s->busy = true;
    s->call = bk_request(method, path, body, s->token[0] ? s->token : NULL);
}

void app_submit_auth(app_state_t *s) {
    if (s->busy) return;
    if (bk_status() != BK_READY) { copy(s->error, sizeof s->error, "O backend local ainda está iniciando."); return; }
    s->error[0] = 0;
    char e[256], p[160], n[192], b[768];
    json_escape(s->email, e, sizeof e);
    json_escape(s->password, p, sizeof p);
    json_escape(s->name, n, sizeof n);
    if (strlen(s->password) < 8) { copy(s->error, sizeof s->error, "A senha precisa ter ao menos 8 caracteres."); return; }
    if (s->auth_mode == 1) {
        snprintf(b, sizeof b, "{\"displayName\":\"%s\",\"email\":\"%s\",\"password\":\"%s\"}", n, e, p);
        begin(s, OP_AUTH, "POST", "/auth/register", b);
    } else if (s->auth_mode == 2) {
        if (strcmp(s->password, s->confirm)) { copy(s->error, sizeof s->error, "As senhas não conferem."); return; }
        snprintf(b, sizeof b, "{\"email\":\"%s\",\"newPassword\":\"%s\"}", e, p);
        begin(s, OP_AUTH, "POST", "/auth/reset-password", b);
    } else {
        snprintf(b, sizeof b, "{\"email\":\"%s\",\"password\":\"%s\"}", e, p);
        begin(s, OP_AUTH, "POST", "/auth/login", b);
    }
}

void app_add_friend(app_state_t *s) {
    if (s->acct_user_id[0]) { account_add_friend(s); return; }   /* conta online: pedido pelo @nick, via briks-core */
    if (s->busy || !s->add_email[0]) return;
    char e[256], b[320];
    json_escape(s->add_email, e, sizeof e);
    snprintf(b, sizeof b, "{\"email\":\"%s\"}", e);
    s->notice[0] = 0;
    begin(s, OP_ADD_FRIEND, "POST", "/friends/requests", b);
}

void app_accept_friend(app_state_t *s, const char *request_id) {
    if (s->acct_user_id[0]) { account_respond_friend(s, request_id, true); return; }
    if (s->busy) return;
    char path[128];
    snprintf(path, sizeof path, "/friends/requests/%s/accept", request_id);
    begin(s, OP_ACCEPT, "POST", path, "{}");
}

void app_logout(app_state_t *s) {
    account_logout(s);      /* invalida a sessão online e apaga o refresh token cifrado */
    app_local_logout(s);
}

void app_local_logout(app_state_t *s) {
    if (s->token[0]) {
        bk_call *c = bk_request("POST", "/auth/logout", "{}", s->token);
        bk_free(c);
    }
    memset(s->token, 0, sizeof s->token);
    s->friend_count = s->pending_count = s->community_count = s->group_count = s->msg_count = 0;
    s->active_group = -1;
    s->password[0] = 0;
    s->screen = SCREEN_AUTH;
    s->active_friend = -1;
    s->settings_open = s->profile_open = s->add_friend_open = s->create_group_open = s->nick_open = false;
}

/* contatos do Briks antigo (importados para /p2p/friends): só o nome; não têm conta online */
static void load_legacy(app_state_t *s, const char *body) {
    jval *j = json_parse(body);
    s->legacy_count = 0;
    for (int i = 0; i < json_len(j) && s->legacy_count < MAX_LEGACY; i++) {
        person_t *d = &s->legacy[s->legacy_count++];
        memset(d, 0, sizeof *d);
        copy(d->id, sizeof d->id, json_str(json_at(j, i), "peerId", ""));
        copy(d->name, sizeof d->name, json_str(json_at(j, i), "displayName", "?"));
    }
    json_free(j);
}

static void load_friends(app_state_t *s, const char *body) {
    jval *j = json_parse(body);
    s->friend_count = s->pending_count = 0;
    const jval *f = json_get(j, "friends"), *p = json_get(j, "pending");
    for (int i = 0; i < json_len(f) && s->friend_count < MAX_FRIENDS; i++) {
        person_t *d = &s->friends[s->friend_count++];
        copy(d->id, sizeof d->id, json_str(json_at(f, i), "id", ""));
        copy(d->name, sizeof d->name, json_str(json_at(f, i), "displayName", "?"));
    }
    for (int i = 0; i < json_len(p) && s->pending_count < MAX_FRIENDS; i++) {
        person_t *d = &s->pending[s->pending_count++];
        copy(d->id, sizeof d->id, json_str(json_at(p, i), "id", ""));
        copy(d->name, sizeof d->name, json_str(json_at(p, i), "displayName", "?"));
        copy(d->request_id, sizeof d->request_id, json_str(json_at(p, i), "requestId", ""));
    }
    json_free(j);
    if (s->active_friend >= s->friend_count) s->active_friend = -1;
}

static void load_communities(app_state_t *s, const char *body) {
    jval *j = json_parse(body);
    s->community_count = 0;
    for (int i = 0; i < json_len(j) && s->community_count < MAX_COMMUNITIES; i++) {
        community_t *d = &s->communities[s->community_count++];
        copy(d->id, sizeof d->id, json_str(json_at(j, i), "id", ""));
        copy(d->name, sizeof d->name, json_str(json_at(j, i), "name", "?"));
        d->members = atoi(json_str(json_at(j, i), "member_count", "0"));
    }
    json_free(j);
}

void app_pump(app_state_t *s) {
    if (!s->call && !s->busy && s->reload_msgs && s->token[0]) {
        s->reload_msgs = false;
        char path[256]; snprintf(path, sizeof path, "/p2p/messages/%s?limit=200", s->chat_channel);
        begin(s, OP_MSG_LOAD, "GET", path, NULL);
    }
    if (!s->call && !s->busy && s->reload_msgs2 && s->token[0]) {
        s->reload_msgs2 = false;
        char path[256]; snprintf(path, sizeof path, "/p2p/messages/%s?limit=200", s->chat2_channel);
        begin(s, OP_MSG_LOAD2, "GET", path, NULL);
    }
    if (!s->call || !bk_done(s->call)) { if (s->call) cui_request_redraw(cui_app_ui(s->app)); return; }
    int code = bk_http_status(s->call);
    char *body = _strdup(bk_body(s->call));
    bk_free(s->call);
    s->call = NULL;
    op_t op = s->op;
    s->busy = false;
    s->op = OP_NONE;
    bool ok = code >= 200 && code < 300;
    if (!body) return;

    switch (op) {
    case OP_BRIDGE_LOGIN: {   /* conta online -> usuário do banco local; se ainda não existe, cadastra com a credencial local */
        if (ok) {
            jval *j = json_parse(body);
            copy(s->token, sizeof s->token, json_str(j, "accessToken", ""));
            json_free(j);
            if (s->token[0]) { begin(s, OP_ME, "GET", "/me", NULL); break; }
        }
        if (!code) { copy(s->error, sizeof s->error, "O backend local não respondeu."); account_bridge_done(s, false); break; }
        char e[200], p[160], n[200], b[640];
        json_escape(s->bridge_email, e, sizeof e); json_escape(s->bridge_pw, p, sizeof p); json_escape(s->user_name[0] ? s->user_name : "Você", n, sizeof n);
        snprintf(b, sizeof b, "{\"displayName\":\"%s\",\"email\":\"%s\",\"password\":\"%s\"}", n, e, p);
        begin(s, OP_AUTH, "POST", "/auth/register", b);
        break;
    }
    case OP_AUTH: {
        if (!ok) { set_error(s, body, code ? "E-mail ou senha incorretos." : "O backend local não respondeu."); if (s->acct_user_id[0]) account_bridge_done(s, false); break; }
        jval *j = json_parse(body);
        copy(s->token, sizeof s->token, json_str(j, "accessToken", ""));
        json_free(j);
        if (!s->token[0]) { copy(s->error, sizeof s->error, "Resposta inesperada do backend."); break; }
        begin(s, OP_ME, "GET", "/me", NULL);
        break;
    }
    case OP_ME: {
        if (!ok) { set_error(s, body, "Não foi possível carregar a conta."); if (s->acct_user_id[0]) account_bridge_done(s, false); else app_logout(s); break; }
        jval *j = json_parse(body);
        copy(s->user_id, sizeof s->user_id, json_str(j, "id", ""));
        if (!s->acct_user_id[0]) {   /* com conta online, nome e e-mail são os da conta (o e-mail local é só interno) */
            copy(s->user_name, sizeof s->user_name, json_str(j, "displayName", ""));
            copy(s->user_email, sizeof s->user_email, json_str(j, "email", ""));
        }
        json_free(j);
        begin(s, OP_FRIENDS, "GET", "/friends", NULL);
        break;
    }
    case OP_FRIENDS:
        if (ok && !s->acct_user_id[0]) load_friends(s, body);   /* com conta online os amigos vêm do servidor (account.c) */
        if (s->screen == SCREEN_AUTH) begin(s, OP_COMMUNITIES, "GET", "/communities", NULL);
        break;
    case OP_COMMUNITIES:
        if (ok) load_communities(s, body);
        begin(s, OP_GROUPS, "GET", "/p2p/groups", NULL);
        break;
    case OP_GROUPS:
        if (ok) app_load_groups(s, body);
        if (s->acct_user_id[0]) { begin(s, OP_LEGACY_FRIENDS, "GET", "/p2p/friends", NULL); break; }
        if (s->screen == SCREEN_AUTH) { s->screen = SCREEN_SOCIAL; s->password[0] = 0; s->confirm[0] = 0; s->active_group = -1; }
        break;
    case OP_LEGACY_FRIENDS:
        if (ok) load_legacy(s, body);
        if (s->screen == SCREEN_AUTH) { s->screen = SCREEN_SOCIAL; s->active_group = -1; account_bridge_done(s, true); }
        break;
    case OP_MSG_LOAD:
        if (ok) app_load_messages(s, body);
        break;
    case OP_MSG_LOAD2:
        if (ok) app_load_messages2(s, body);
        break;
    case OP_MSG_SEND:
        attach_inflight_done(s, ok);   /* falhou: a mensagem fica com "não enviada · reenviar" em vez de sumir */
        if (!ok) snprintf(s->notice, sizeof s->notice, "Não foi possível gravar a mensagem.");
        break;
    case OP_EVENT_SAVE:   /* evento assinado gravado na API local (se falhar, fica só no estado local) */
        break;
    case OP_GROUP_SAVE:
        begin(s, OP_GROUPS, "GET", "/p2p/groups", NULL);
        break;
    case OP_ADD_FRIEND: {
        if (ok) { copy(s->notice, sizeof s->notice, "Pedido enviado."); s->add_email[0] = 0; }
        else { jval *j = json_parse(body); copy(s->notice, sizeof s->notice, json_str(j, "message", "Não foi possível enviar o pedido.")); json_free(j); }
        begin(s, OP_FRIENDS, "GET", "/friends", NULL);
        break;
    }
    case OP_ACCEPT:
        begin(s, OP_FRIENDS, "GET", "/friends", NULL);
        break;
    default: break;
    }
    free(body);
}
