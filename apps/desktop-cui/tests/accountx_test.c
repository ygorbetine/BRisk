/* Teste sem janela: data de nascimento (maiores de 18), dica de login por @nick, piso de 12 px do texto e frase de erro de rede. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include "../src/accountx.c"
#include "../src/net_state.c"
briks_theme_t g_th; int g_perf; char g_tip[160];
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FALHOU: %s (linha %d)\n", #c, __LINE__); fails++; } } while (0)
int main(void) {
    char err[200]; int d, m, y;
    CHECK(birth_parse("25/03/1998", &d, &m, &y) && d == 25 && m == 3 && y == 1998);
    CHECK(birth_parse("1-2-2000", &d, &m, &y) && d == 1 && m == 2);
    CHECK(!birth_parse("31/04/2000", &d, &m, &y));            /* abril tem 30 */
    CHECK(birth_parse("29/02/2000", &d, &m, &y) && !birth_parse("29/02/1900", &d, &m, &y) && !birth_parse("29/02/2001", &d, &m, &y));
    CHECK(!birth_parse("abc", &d, &m, &y) && !birth_parse("", &d, &m, &y) && !birth_parse("12/13/2000", &d, &m, &y) && !birth_parse("12/12/2000x", &d, &m, &y));
    CHECK(birth_check("25/03/1998", 2026, 11, 11, err, sizeof err) == 0);
    CHECK(birth_check("11/11/2008", 2026, 11, 11, err, sizeof err) == 0);      /* faz 18 hoje */
    CHECK(birth_check("12/11/2008", 2026, 11, 11, err, sizeof err) == 3 && strstr(err, "18"));   /* falta 1 dia */
    CHECK(birth_check("01/01/2015", 2026, 11, 11, err, sizeof err) == 3);
    CHECK(birth_check("01/01/2030", 2026, 11, 11, err, sizeof err) == 2);
    CHECK(birth_check("ontem", 2026, 11, 11, err, sizeof err) == 1);
    /* dica de login */
    char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\briks_hints_test.txt", getenv("TEMP") ? getenv("TEMP") : "."); remove(path);
    _putenv_s("BRIKS_LOGIN_HINTS_FILE", path);
    char em[100] = "";
    CHECK(!login_hint_lookup("@ygor", em, sizeof em));
    login_hint_save("Ygor", "ygor@exemplo.com"); login_hint_save("@marina", "marina@exemplo.com");
    CHECK(login_hint_lookup("@YGOR", em, sizeof em) && !strcmp(em, "ygor@exemplo.com"));
    CHECK(login_hint_lookup("marina", em, sizeof em) && !strcmp(em, "marina@exemplo.com"));
    login_hint_save("ygor", "novo@exemplo.com");                                   /* troca, não duplica */
    CHECK(login_hint_lookup("ygor", em, sizeof em) && !strcmp(em, "novo@exemplo.com") && login_hint_lookup("marina", em, sizeof em));
    login_hint_save("semarroba", "invalido");                                       /* sem @ no e-mail: ignora */
    CHECK(!login_hint_lookup("semarroba", em, sizeof em));
    remove(path);
    /* piso de texto */
    CHECK(briks_text_floor(10) == 12.0f && briks_text_floor(11.5f) == 12.0f && briks_text_floor(12) == 12.0f && briks_text_floor(15) == 15.0f && briks_text_floor(0) == 0.0f);
    /* erros de rede em linguagem de gente */
    char o[200];
    net_friendly_error("error sending request for url (https://x)", o, sizeof o); CHECK(strstr(o, "Sem conexão") && !strstr(o, "https"));
    net_friendly_error("operation timed out", o, sizeof o); CHECK(strstr(o, "demorou"));
    net_friendly_error("dns error: no such host", o, sizeof o); CHECK(strstr(o, "encontrar o servidor"));
    net_friendly_error("HTTP 429", o, sizeof o); CHECK(strstr(o, "Muitas tentativas"));
    net_friendly_error("", o, sizeof o); CHECK(strstr(o, "Algo deu errado"));
    net_friendly_error("Senha curta demais.", o, sizeof o); CHECK(!strcmp(o, "Senha curta demais."));   /* mensagem já amigável passa intacta */
    if (fails) { printf("%d falha(s)\n", fails); return 1; }
    printf("accountx_test: ok\n"); return 0;
}
