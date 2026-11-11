/* Testes do texto das mensagens (src/richtext.c): sem janela, sem rede. */
#include "../src/richtext.h"
#include <stdio.h>
#include <string.h>
static int fails;
#define EQ(got, want, name) do { if (strcmp((got), (want))) { fails++; printf("FALHOU: %s\n  obtido:   [%s]\n  esperado: [%s]\n", name, got, want); } else printf("ok: %s\n", name); } while (0)
#define OK(c, name) do { if (!(c)) { fails++; printf("FALHOU: %s\n", name); } else printf("ok: %s\n", name); } while (0)
static const char *prep(const char *in) { static char o[8192]; rich_prepare(in, o, sizeof o); return o; }
int main(void) {
    EQ(prep("oi **forte** e `x*y`"), "oi **forte** e `x*y`", "negrito e código preservados");
    EQ(prep("# titulo\n- item\n1. um\n> cita"), "\\# titulo  \n\\- item  \n1\\. um  \n\\> cita", "títulos, listas e citações viram texto");
    EQ(prep("veja https://a.com/x?y=1, ok."), "veja <https://a.com/x?y=1>, ok.", "link solto vira autolink sem a pontuação");
    EQ(prep("[site](https://a.com/p)"), "[site](https://a.com/p)", "link com texto preservado");
    OK(strstr(prep("[x](javascript:alert(1))"), "\\](javascript") != NULL, "link com esquema perigoso não vira link");
    OK(strstr(prep("[x](file:///c:/a)"), "\\](file") != NULL, "file:// bloqueado");
    OK(strstr(prep("<script>alert(1)</script>"), "\\<script") != NULL, "HTML escapado");
    EQ(prep("fala @ana e @[Maria Souza]!"), "fala **@ana** e **@Maria Souza**!", "menções destacadas");
    EQ(prep("email a@b.com"), "email a@b.com", "e-mail não é menção");
    EQ(prep("*solto* _x_ ~y~"), "\\*solto\\* \\_x\\_ \\~y\\~", "itálico e tachado não existem no Electron");
    EQ(prep("linha1\nlinha2"), "linha1  \nlinha2", "quebra de linha mantida");
    EQ(prep("**aberto sem fim"), "\\*\\*aberto sem fim", "negrito sem fechamento fica texto");
    EQ(prep("café ☕ ação"), "café ☕ ação", "UTF-8 intacto");
    char small[8]; size_t n = rich_prepare("abcdefghijklmnop", small, sizeof small);
    OK(n == 7 && strlen(small) == 7, "saída limitada ao buffer");
    OK(rich_safe_url("https://a.com") && rich_safe_url("http://a.com/x") && !rich_safe_url("ftp://a.com") && !rich_safe_url("https://") && !rich_safe_url("https://a b"), "rich_safe_url");

    char u[4][220]; int k = rich_urls("a https://x.com/1. b http://y.com/2 c https://x.com/1 d [t](https://z.com)", u, 4);
    OK(k == 3 && !strcmp(u[0], "https://x.com/1") && !strcmp(u[1], "http://y.com/2") && !strcmp(u[2], "https://z.com"), "rich_urls sem repetir");

    char q[96]; int at;
    OK(rich_mention_query("oi @an", q, sizeof q, &at) && !strcmp(q, "an") && at == 3, "consulta de menção");
    OK(rich_mention_query("@", q, sizeof q, &at) && q[0] == 0 && at == 0, "@ sozinho abre a lista");
    OK(!rich_mention_query("email a@b", q, sizeof q, &at) && !rich_mention_query("oi @ana ", q, sizeof q, &at), "sem consulta fora do fim/depois de espaço");
    char d[64] = "oi @an"; OK(rich_apply_mention(d, sizeof d, "Ana Lima") && !strcmp(d, "oi @[Ana Lima] "), "aplicar menção");
    char d2[10] = "oi @an"; OK(!rich_apply_mention(d2, sizeof d2, "Ana Lima Souza"), "menção que não cabe");

    long long t0 = rich_iso_ms("2026-11-09T23:41:00.000Z"), t1 = rich_iso_ms("2026-11-09T23:44:30.000Z"), t2 = rich_iso_ms("2026-11-09T23:47:00.000Z");
    OK(t1 - t0 == 210000 && rich_iso_ms("lixo") == -1 && rich_iso_ms("1970-01-01T00:00:01.500Z") == 1500, "rich_iso_ms");
    OK(rich_group_with_prev("A", t0, "A", t1, false) && !rich_group_with_prev("A", t0, "A", t2, false) && !rich_group_with_prev("A", t0, "B", t1, false) && !rich_group_with_prev("A", t0, "A", t1, true), "agrupamento (autor, 5 min, sem resposta)");
    OK(rich_mentions_me("oi @Ana tudo bem", "Ana", NULL) && rich_mentions_me("@[Ana Lima] veja", "Ana Lima", "ana") && rich_mentions_me("e @ANA", "x", "ana"), "menciona a mim (nome, nome com espaço, @nick)");
    OK(!rich_mentions_me("mail a@ana.com", "ana", "ana") && !rich_mentions_me("oi @Anabela", "Ana", "ana") && !rich_mentions_me("oi", "Ana", "ana") && !rich_mentions_me(NULL, "Ana", "ana"), "não confunde e-mail, prefixo nem texto vazio");
    printf(fails ? "\n%d FALHA(S)\n" : "\ntudo certo\n", fails);
    return fails ? 1 : 0;
}
