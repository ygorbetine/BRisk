/* Testes da lógica pura do painel de sons (src/sons_util.c): nome, rótulo, ganho de pico, busca, duração. Sem janela. */
#include "../src/sons_util.h"
#include <stdio.h>
#include <string.h>
static int fails;
#define OK(c, name) do { if (!(c)) { fails++; printf("FALHOU: %s\n", name); } else printf("ok: %s\n", name); } while (0)
#define EQ(got, want, name) do { if (strcmp((got), (want))) { fails++; printf("FALHOU: %s\n  obtido:   [%s]\n  esperado: [%s]\n", name, got, want); } else printf("ok: %s\n", name); } while (0)

int main(void) {
    char b[200];
    /* nome */
    OK(sons_clean_name("  Buzina\n\tgrande  ", b, sizeof b) && !strcmp(b, "Buzinagrande"), "nome: controles somem e espaços das pontas saem");
    OK(!sons_clean_name("   \n ", b, sizeof b) && !b[0], "nome: só espaço é vazio");
    OK(!sons_clean_name(NULL, b, sizeof b), "nome: NULL é vazio");
    sons_clean_name("12345678901234567890123456789012345678", b, sizeof b); OK(strlen(b) == 32, "nome: corta em 32 caracteres");
    sons_clean_name("ááááááááááááááááááááááááááááááááááááá", b, sizeof b); OK(strlen(b) == 64, "nome: 32 caracteres acentuados = 64 bytes, sem partir UTF-8");
    char pequeno[6]; sons_clean_name("ááá", pequeno, sizeof pequeno); OK(strlen(pequeno) == 4, "nome: destino pequeno corta num limite de caractere");
    /* rótulo */
    sons_label("📯", "Buzina", 18, b, sizeof b); EQ(b, "📯 Buzina", "rótulo: emoji + nome");
    sons_label("", "Buzina", 18, b, sizeof b); EQ(b, "Buzina", "rótulo: sem emoji");
    sons_label("📯", "Uma buzina muito muito grande mesmo", 12, b, sizeof b); EQ(b, "📯 Uma buzina…", "rótulo: corta com reticências");
    sons_label("🎺", "áéíóúçãõ muito longo", 5, b, sizeof b); OK(strstr(b, "…") != NULL && (unsigned char)b[strlen(b) - 3] == 0xE2, "rótulo: corte em UTF-8 válido");
    /* ganho de pico */
    short quiet[4] = { 1000, -2000, 500, 0 };
    OK(sons_peak_gain(quiet, 4, 1.0f) == 1.0f, "pico: som baixo não muda");
    OK(sons_peak_gain(quiet, 4, 0.5f) == 0.5f, "pico: volume do painel se aplica");
    short loud[3] = { 32767, -32768, 100 };
    float g = sons_peak_gain(loud, 3, 1.0f);
    OK(g > 0.70f && g < 0.72f, "pico: som no talo é limitado a -3 dBFS");
    OK(32768.0f * g <= 0.709f * 32768.0f, "pico: depois do ganho não passa do teto");
    OK(sons_peak_gain(loud, 3, 2.0f) <= 0.72f && sons_peak_gain(loud, 3, -1.0f) == 0.0f, "pico: volume fora de 0..1 é preso");
    OK(sons_peak_gain(NULL, 0, 0.8f) == 0.8f, "pico: sem amostras só aplica o volume");
    /* busca */
    OK(sons_match("Buzina do Ônibus", ""), "busca: vazia casa tudo");
    OK(sons_match("Buzina do Ônibus", "onibus"), "busca: sem acento");
    OK(sons_match("BUZINA", "buz"), "busca: sem diferenciar maiúsculas");
    OK(sons_match("Coração", "coracao"), "busca: ç e ã");
    OK(!sons_match("Buzina", "gato"), "busca: não casa");
    OK(sons_match("📯 Buzina", "buzina"), "busca: emoji no nome não atrapalha");
    OK(sons_match("Buzina", "🎺"), "busca: só emoji não filtra");
    /* duração */
    EQ(sons_fmt_dur(1500, b, sizeof b), "1,5 s", "duração: 1,5 s");
    EQ(sons_fmt_dur(8000, b, sizeof b), "8 s", "duração: 8 s");
    EQ(sons_fmt_dur(240, b, sizeof b), "0,2 s", "duração: arredonda");
    printf(fails ? "\n%d FALHA(S)\n" : "\ntodos passaram\n", fails);
    return fails ? 1 : 0;
}
