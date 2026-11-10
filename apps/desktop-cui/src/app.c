/* Lógica da aplicação: cada ação vira uma chamada HTTP ao backend local; app_pump consome a resposta. */
#include "briks_ui.h"
#include "backend.h"
#include "json.h"
#include <stdio.h>
#include <string.h>

static void copy(char *dst, size_t cap, const char *src) { snprintf(dst, cap, "%s", src ? src : ""); }

static void set_error(app_state_t *s, const char *body, const char *fallback) {
    jval *j = json_parse(body);
    copy(s->error, sizeof s->error, json_str(j, "message", fallback));
    json_free(j);
}

static void begin(app_state_t *s, op_t op, const char *method, const char *path, const char *body) {
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
    if (s->busy || !s->add_email[0]) return;
    char e[256], b[320];
    json_escape(s->add_email, e, sizeof e);
    snprintf(b, sizeof b, "{\"email\":\"%s\"}", e);
    s->notice[0] = 0;
    begin(s, OP_ADD_FRIEND, "POST", "/friends/requests", b);
}

void app_accept_friend(app_state_t *s, const char *request_id) {
    if (s->busy) return;
    char path[128];
    snprintf(path, sizeof path, "/friends/requests/%s/accept", request_id);
    begin(s, OP_ACCEPT, "POST", path, "{}");
}

void app_logout(app_state_t *s) {
    if (s->token[0]) {
        bk_call *c = bk_request("POST", "/auth/logout", "{}", s->token);
        bk_free(c);
    }
    memset(s->token, 0, sizeof s->token);
    s->friend_count = s->pending_count = s->community_count = 0;
    s->password[0] = 0;
    s->screen = SCREEN_AUTH;
    s->active_friend = -1;
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
    }
    json_free(j);
}

void app_pump(app_state_t *s) {
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
    case OP_AUTH: {
        if (!ok) { set_error(s, body, code ? "E-mail ou senha incorretos." : "O backend local não respondeu."); break; }
        jval *j = json_parse(body);
        copy(s->token, sizeof s->token, json_str(j, "accessToken", ""));
        json_free(j);
        if (!s->token[0]) { copy(s->error, sizeof s->error, "Resposta inesperada do backend."); break; }
        begin(s, OP_ME, "GET", "/me", NULL);
        break;
    }
    case OP_ME: {
        if (!ok) { set_error(s, body, "Não foi possível carregar a conta."); app_logout(s); break; }
        jval *j = json_parse(body);
        copy(s->user_id, sizeof s->user_id, json_str(j, "id", ""));
        copy(s->user_name, sizeof s->user_name, json_str(j, "displayName", ""));
        copy(s->user_email, sizeof s->user_email, json_str(j, "email", ""));
        json_free(j);
        begin(s, OP_FRIENDS, "GET", "/friends", NULL);
        break;
    }
    case OP_FRIENDS:
        if (ok) load_friends(s, body);
        if (s->screen == SCREEN_AUTH) begin(s, OP_COMMUNITIES, "GET", "/communities", NULL);
        break;
    case OP_COMMUNITIES:
        if (ok) load_communities(s, body);
        s->screen = SCREEN_SOCIAL;
        s->password[0] = 0; s->confirm[0] = 0;
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
