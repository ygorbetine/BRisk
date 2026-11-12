/* Testes da lógica pura da chamada (src/callhist.c): histórico, duração, grade de participantes e indicador de rede. Sem janela. */
#include "../src/callhist.h"
#include <stdio.h>
#include <string.h>
static int fails;
#define OK(c, name) do { if (!(c)) { fails++; printf("FALHOU: %s\n", name); } else printf("ok: %s\n", name); } while (0)
#define EQ(got, want, name) do { if (strcmp((got), (want))) { fails++; printf("FALHOU: %s\n  obtido:   [%s]\n  esperado: [%s]\n", name, got, want); } else printf("ok: %s\n", name); } while (0)

static call_rec_t rec(long long t, int dur, const char *title, const char *people, int n, bool shared) {
    call_rec_t r; memset(&r, 0, sizeof r);
    r.started = t; r.duration = dur; snprintf(r.room, sizeof r.room, "voz-geral"); snprintf(r.title, sizeof r.title, "%s", title);
    snprintf(r.people, sizeof r.people, "%s", people); r.npeople = n; r.shared = shared;
    return r;
}

int main(void) {
    static callhist_t h, g; static char buf[CALLHIST_MAX * 760 + 256]; char b[64];

    /* histórico: ordem, opção de guardar, apagar */
    callhist_init(&h);
    OK(h.keep && h.n == 0, "começa vazio e guardando");
    call_rec_t a = rec(1000, 60, "Sala geral", "Ana", 1, false), c = rec(2000, 3700, "Grupo \"X\" · voz\\1", "Ana, Beto", 2, true);
    OK(callhist_add(&h, &a) && callhist_add(&h, &c) && h.n == 2, "registra duas chamadas");
    OK(h.r[0].started == 2000 && h.r[1].started == 1000, "a mais recente fica primeiro");
    call_rec_t zero = rec(3000, 0, "Sala", "", 0, false);
    OK(!callhist_add(&h, &zero) && h.n == 2, "chamada de 0 s não entra");
    h.keep = false;
    OK(!callhist_add(&h, &a) && h.n == 2, "com 'Guardar histórico' desligado nada é registrado");
    h.keep = true;

    /* ida e volta pelo JSON, com aspas e barras no título */
    size_t n = callhist_to_json(&h, buf, sizeof buf);
    OK(n > 0 && n == strlen(buf), "JSON gerado");
    OK(callhist_from_json(&g, buf) && g.n == 2 && g.keep, "JSON lido de volta");
    OK(!strcmp(g.r[0].title, "Grupo \"X\" · voz\\1") && g.r[0].duration == 3700 && g.r[0].shared && g.r[0].npeople == 2 && !strcmp(g.r[0].people, "Ana, Beto"), "campos preservados");
    h.keep = false; callhist_to_json(&h, buf, sizeof buf);
    OK(callhist_from_json(&g, buf) && !g.keep && g.n == 2, "opção desligada é preservada");
    OK(!callhist_from_json(&g, "isto não é json") && g.n == 0 && g.keep, "arquivo estragado vira histórico vazio");
    OK(!callhist_from_json(&g, "[1,2]") && g.n == 0, "JSON de outro formato é recusado");
    OK(callhist_from_json(&g, "{\"calls\":[{\"started\":5,\"duration\":-3},{\"started\":0,\"duration\":9},{\"started\":7,\"duration\":9,\"title\":\"ok\"},42]}") && g.n == 1 && !strcmp(g.r[0].title, "ok"), "registros inválidos são pulados");

    /* limite: o mais antigo sai */
    callhist_init(&h);
    for (int i = 1; i <= CALLHIST_MAX + 5; i++) { call_rec_t r = rec(i, 10, "S", "", 0, false); callhist_add(&h, &r); }
    OK(h.n == CALLHIST_MAX && h.r[0].started == CALLHIST_MAX + 5 && h.r[CALLHIST_MAX - 1].started == 6, "cheio: mantém os 100 mais recentes");
    n = callhist_to_json(&h, buf, sizeof buf);
    OK(callhist_from_json(&g, buf) && g.n == CALLHIST_MAX, "histórico cheio cabe no arquivo e volta inteiro");
    callhist_clear(&h);
    OK(h.n == 0 && h.keep, "apagar zera os registros e mantém a opção");

    /* lista de pessoas sem repetir */
    char people[40] = ""; int np = 0;
    OK(callhist_people_add(people, sizeof people, &np, "Ana") && callhist_people_add(people, sizeof people, &np, "Beto") && np == 2, "junta nomes");
    OK(!callhist_people_add(people, sizeof people, &np, "Ana") && !callhist_people_add(people, sizeof people, &np, "") && np == 2, "não repete nem aceita vazio");
    OK(callhist_people_add(people, sizeof people, &np, "An") && np == 3, "nome que é prefixo de outro conta como pessoa diferente");
    EQ(people, "Ana, Beto, An", "lista montada");
    OK(!callhist_people_add(people, sizeof people, &np, "Um Nome Comprido Demais Para Caber") && np == 3 && strlen(people) < sizeof people, "sem espaço: não estoura");

    /* duração e data */
    call_fmt_duration(45, b, sizeof b); EQ(b, "45 s", "segundos");
    call_fmt_duration(60, b, sizeof b); EQ(b, "1 min", "um minuto");
    call_fmt_duration(3599, b, sizeof b); EQ(b, "59 min", "quase uma hora");
    call_fmt_duration(3900, b, sizeof b); EQ(b, "1 h 05 min", "hora e minutos");
    call_fmt_duration(-5, b, sizeof b); EQ(b, "0 s", "negativo vira zero");
    call_fmt_when(2026, 11, 11, 21, 4, 2026, 11, 11, false, b, sizeof b); EQ(b, "hoje 21:04", "hoje");
    call_fmt_when(2026, 11, 10, 9, 30, 2026, 11, 11, true, b, sizeof b); EQ(b, "ontem 09:30", "ontem");
    call_fmt_when(2026, 3, 2, 9, 30, 2026, 11, 11, false, b, sizeof b); EQ(b, "02/03 09:30", "mesmo ano");
    call_fmt_when(2025, 3, 2, 9, 30, 2026, 11, 11, false, b, sizeof b); EQ(b, "02/03/2025 09:30", "outro ano");

    /* grade */
    int cols, rows;
    call_grid(1, 1200, 600, 14, &cols, &rows); OK(cols == 1 && rows == 1, "1 pessoa: bloco único");
    call_grid(2, 1200, 600, 14, &cols, &rows); OK(cols == 2 && rows == 1, "2 pessoas em janela larga: lado a lado");
    call_grid(2, 500, 900, 14, &cols, &rows); OK(cols == 1 && rows == 2, "2 pessoas em janela alta: uma sobre a outra");
    call_grid(4, 1200, 600, 14, &cols, &rows); OK(cols * rows >= 4 && cols >= 2 && cols <= 4, "4 pessoas cabem");
    call_grid(9, 1200, 600, 14, &cols, &rows); OK(cols * rows >= 9 && rows <= 3, "9 pessoas cabem em até 3 linhas");
    for (int k = 1; k <= 10; k++) { call_grid(k, 1244, 590, 14, &cols, &rows); if (cols * rows < k || cols < 1 || (rows - 1) * cols >= k) { fails++; printf("FALHOU: grade de %d (%dx%d)\n", k, cols, rows); } }
    printf("ok: grades de 1 a 10 cobrem todos sem linha vazia\n");
    call_grid(0, 100, 100, 14, &cols, &rows); OK(cols == 1 && rows == 1, "zero pessoas não quebra");
    call_grid(5, 10, 10, 14, &cols, &rows); OK(cols >= 1 && rows >= 1 && cols * rows >= 5, "área minúscula não quebra");

    /* indicador de rede */
    netq_in_t q = { "", 0, 0, 0, false, false, 0, 0 };
    OK(call_net_quality(&q) == NETQ_OFF, "fora da sala");
    q.state = "conectando"; OK(call_net_quality(&q) == NETQ_CONNECTING, "conectando");
    q.state = "conectado"; OK(call_net_quality(&q) == NETQ_GOOD, "conectado e sozinho: boa");
    q.peers = 2; q.authed = 1; OK(call_net_quality(&q) == NETQ_CONNECTING, "alguém ainda negociando");
    q.authed = 2; OK(call_net_quality(&q) == NETQ_GOOD, "todos autenticados: boa");
    q.failed = 1; OK(call_net_quality(&q) == NETQ_UNSTABLE, "um peer falhou: instável");
    q.failed = 0; q.stun_done = true; q.stun_ok = false; OK(call_net_quality(&q) == NETQ_UNSTABLE, "teste sem resposta: instável");
    q.stun_ok = true; q.rtt_ms = 40; q.loss_pct = 0; OK(call_net_quality(&q) == NETQ_GOOD, "teste bom");
    q.loss_pct = 20; OK(call_net_quality(&q) == NETQ_UNSTABLE, "perda alta: instável");
    q.loss_pct = 0; q.state = "reconectando"; OK(call_net_quality(&q) == NETQ_UNSTABLE, "reconectando");
    q.state = "erro"; OK(call_net_quality(&q) == NETQ_DOWN, "erro: sem conexão");
    EQ(call_net_label(NETQ_GOOD), "Rede boa", "rótulo");

    printf(fails ? "\n%d FALHA(S)\n" : "\ntudo ok\n", fails);
    /* transmissão: aviso "começou a transmitir" e zoom de quem assiste */
    OK(share_notice_wanted(false, true, false), "aviso quando alguém começa a transmitir e estou no modo Call");
    OK(!share_notice_wanted(false, true, true), "sem aviso para quem já está no modo Transmissão");
    OK(!share_notice_wanted(true, true, false), "sem aviso repetido a cada atualização da mesma transmissão");
    OK(!share_notice_wanted(true, false, false), "sem aviso quando a transmissão termina");
    OK(share_ds_steps(1920, 0) == 0 && share_ds_steps(1920, 1800) == 0 && share_ds_steps(1920, 700) == 1 && share_ds_steps(1920, 400) == 1 && share_ds_steps(1920, 300) == 2 && share_ds_steps(1920, 100) == 3, "redução do quadro: só quando aparece bem menor (e nunca abaixo de 1,25x)");
    { unsigned char src[4 * 4 * 4], dst[2 * 2 * 4]; for (int i = 0; i < 16; i++) { src[i * 4] = (unsigned char)(i % 2 ? 200 : 100); src[i * 4 + 1] = 50; src[i * 4 + 2] = 0; src[i * 4 + 3] = 255; }
      share_box_down(src, 4, 1, 2, 2, dst); OK(dst[0] == 150 && dst[1] == 50 && dst[2] == 0 && dst[3] == 255 && dst[12] == 150, "média dos 4 pixels de cada bloco 2x2"); }
    OK(share_zoom_pct(0, 0.5f) == 100 && share_zoom_pct(0.5f, 0.5f) == 100, "encaixada = 100%");
    OK(share_zoom_pct(1.0f, 0.5f) == 200 && share_zoom_pct(0.75f, 0.5f) == 150, "percentual relativo ao encaixe");
    OK(share_zoom_pct(0.2f, 0.5f) == 100 && share_zoom_pct(1.0f, 0) == 100, "nunca abaixo de 100%");
    { float z = 1.0f, x = 300, y = 200;
      OK(share_zoom_clamp(&z, &x, &y, 0.5f, 8) && z == 1.0f && x == 300, "ampliada: mantém zoom e posição");
      z = 9.0f; OK(share_zoom_clamp(&z, &x, &y, 0.5f, 8) && z == 4.0f, "limite de 8x o encaixe");
      z = 0.4f; OK(!share_zoom_clamp(&z, &x, &y, 0.5f, 8) && z == 0 && x == 0 && y == 0, "abaixo do encaixe volta a 100% e esquece a posição");
      z = 0; x = 5; OK(!share_zoom_clamp(&z, &x, &y, 0.5f, 8) && x == 5, "encaixada: nada muda"); }
    return fails ? 1 : 0;
}
