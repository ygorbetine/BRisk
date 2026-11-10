/* Teste de fumaça: sidecar real + cliente HTTP + JSON. Usa %APPDATA%\Briks-CUI (não toca no Briks de produção). */
#include "../src/backend.h"
#include "../src/json.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FALHOU: " __VA_ARGS__); printf("\n"); } else { printf("ok: " __VA_ARGS__); printf("\n"); } } while (0)

static bk_call *wait_call(bk_call *c) { while (!bk_done(c)) Sleep(10); return c; }

int main(void) {
    bk_start();
    for (int i = 0; i < 400 && bk_status() == BK_STARTING; i++) Sleep(50);
    CHECK(bk_status() == BK_READY, "sidecar iniciou (%s)", bk_error());
    if (bk_status() != BK_READY) return 1;

    char email[64]; snprintf(email, sizeof email, "smoke%lu@briks.local", GetTickCount());
    char body[256];
    snprintf(body, sizeof body, "{\"displayName\":\"Teste CUI\",\"email\":\"%s\",\"password\":\"senha-segura-1\"}", email);
    bk_call *c = wait_call(bk_request("POST", "/auth/register", body, NULL));
    CHECK(bk_http_status(c) == 200, "cadastro 200 (veio %d)", bk_http_status(c));
    jval *j = json_parse(bk_body(c));
    char token[512]; snprintf(token, sizeof token, "%s", json_str(j, "accessToken", ""));
    json_free(j); bk_free(c);
    CHECK(token[0], "token recebido");

    c = wait_call(bk_request("GET", "/me", NULL, token));
    j = json_parse(bk_body(c));
    CHECK(bk_http_status(c) == 200 && !strcmp(json_str(j, "displayName", ""), "Teste CUI"), "/me devolve o nome");
    json_free(j); bk_free(c);

    c = wait_call(bk_request("GET", "/friends", NULL, token));
    j = json_parse(bk_body(c));
    CHECK(bk_http_status(c) == 200 && json_get(j, "friends") && json_get(j, "pending"), "/friends tem friends e pending");
    json_free(j); bk_free(c);

    snprintf(body, sizeof body, "{\"email\":\"%s\",\"password\":\"errada-errada\"}", email);
    c = wait_call(bk_request("POST", "/auth/login", body, NULL));
    CHECK(bk_http_status(c) == 401, "senha errada dá 401 (veio %d)", bk_http_status(c));
    bk_free(c);

    c = wait_call(bk_request("GET", "/me", NULL, NULL));
    CHECK(bk_http_status(c) == 401, "sem token dá 401 (veio %d)", bk_http_status(c));
    bk_free(c);

    bk_stop();
    printf(fails ? "\n%d falha(s)\n" : "\ntudo certo\n", fails);
    return fails ? 1 : 0;
}
