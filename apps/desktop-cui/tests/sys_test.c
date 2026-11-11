/* Testes do pacote "Janela e sistema" (src/sysmodel.h, o mesmo código do app): sem janela, sem rede, sem registro. */
#include "../src/sysmodel.h"
#include <stdio.h>
#include <stdlib.h>
static int fails;
#define OK(c, name) do { if (!(c)) { fails++; printf("FALHOU: %s\n", name); } else printf("ok: %s\n", name); } while (0)

static bool inside(wf_rect o, wf_rect w) { return o.l >= w.l && o.t >= w.t && o.r <= w.r && o.b <= w.b; }

int main(void) {
    /* --- geometria da janela salva (BAS-16) --- */
    wf_rect one[1] = { { 0, 0, 1920, 1040 } }, two[2] = { { 0, 0, 1920, 1040 }, { 1920, 0, 3840, 1040 } }, left[2] = { { 0, 0, 1920, 1040 }, { -1280, 0, 0, 984 } };
    wf_rect o; bool mv;
    OK(wf_fit(one, 1, (wf_rect){ 100, 80, 1380, 880 }, 560, 480, &o, &mv) == 0 && o.l == 100 && o.t == 80 && wf_w(o) == 1280 && wf_h(o) == 800 && !mv, "janela inteira dentro do monitor: igual");
    OK(wf_fit(one, 1, (wf_rect){ 1500, 600, 2780, 1400 }, 560, 480, &o, &mv) == 0 && inside(o, one[0]) && wf_w(o) == 1280 && wf_h(o) == 800 && mv, "meio fora: empurrada para dentro, mesmo tamanho");
    OK(wf_fit(one, 1, (wf_rect){ 2100, 100, 3380, 900 }, 560, 480, &o, &mv) == 0 && inside(o, one[0]) && o.l == (1920 - 1280) / 2 && mv, "monitor que sumiu: centraliza no principal");
    OK(wf_fit(two, 2, (wf_rect){ 2100, 100, 3380, 900 }, 560, 480, &o, NULL) == 1 && o.l == 2100 && o.t == 100, "segundo monitor ligado: continua nele");
    OK(wf_fit(two, 2, (wf_rect){ 1800, 100, 3080, 900 }, 560, 480, &o, NULL) == 1 && inside(o, two[1]), "cruzando dois monitores: fica no que tem mais área");
    OK(wf_fit(left, 2, (wf_rect){ -1200, 50, -100, 700 }, 560, 480, &o, NULL) == 1 && o.l == -1200 && inside(o, left[1]), "monitor à esquerda (coordenadas negativas)");
    OK(wf_fit(one, 1, (wf_rect){ 0, 0, 5000, 3000 }, 560, 480, &o, &mv) == 0 && wf_w(o) == 1920 && wf_h(o) == 1040 && inside(o, one[0]), "maior que o monitor: encolhe");
    OK(wf_fit(one, 1, (wf_rect){ -32000, -32000, -31000, -31200 }, 560, 480, &o, NULL) == 0 && inside(o, one[0]), "posição de janela minimizada (-32000): volta para dentro");
    OK(wf_fit(one, 1, (wf_rect){ 0, 0, 0, 0 }, 560, 480, &o, NULL) == -1 && wf_fit(one, 1, (wf_rect){ 10, 10, 50, 50 }, 560, 480, &o, NULL) == -1, "retângulo inválido/minúsculo: usa o padrão");
    OK(wf_fit(one, 0, (wf_rect){ 0, 0, 800, 600 }, 560, 480, &o, NULL) == -1, "sem monitores: padrão");
    {   wf_rect small[1] = { { 0, 0, 800, 560 } };
        OK(wf_fit(small, 1, (wf_rect){ 0, 0, 1280, 800 }, 560, 480, &o, NULL) == 0 && wf_w(o) == 800 && wf_h(o) == 560, "monitor pequeno: encolhe para a área útil"); }

    /* --- quando avisar (NOT-01..05) --- */
    sm_ctx c = { .nt_msg = true, .nt_online = true, .nt_stream = true };
    OK(sm_toast_allowed(NT_MESSAGE, &c) && sm_toast_allowed(NT_FRIEND_ONLINE, &c) && sm_toast_allowed(NT_STREAM, &c), "em segundo plano: avisa tudo");
    sm_ctx f = c; f.focused = true;
    OK(sm_toast_allowed(NT_MESSAGE, &f) && !sm_toast_allowed(NT_FRIEND_ONLINE, &f) && !sm_toast_allowed(NT_STREAM, &f), "em foco: mensagem de outra conversa avisa; amigo/transmissão não");
    f.same_chat = true; OK(!sm_toast_allowed(NT_MESSAGE, &f), "em foco na própria conversa: não avisa");
    sm_ctx k = c; k.in_call = true; OK(!sm_toast_allowed(NT_MESSAGE, &k) && !sm_toast_allowed(NT_FRIEND_CALL, &k) && !sm_toast_allowed(NT_STREAM, &k), "durante a chamada: nada (NOT-05)");
    sm_ctx g = c; g.grace = true; OK(!sm_toast_allowed(NT_MESSAGE, &g), "logo ao abrir: nada (NOT-05)");
    sm_ctx off = c; off.nt_msg = off.nt_online = off.nt_stream = false; OK(!sm_toast_allowed(NT_MESSAGE, &off) && !sm_toast_allowed(NT_FRIEND_ONLINE, &off) && !sm_toast_allowed(NT_STREAM, &off), "interruptores desligados: nada");
    sm_ctx fa = c; fa.friend_alerts_off = true; OK(!sm_toast_allowed(NT_FRIEND_ONLINE, &fa) && sm_toast_allowed(NT_MESSAGE, &fa), "avisos de amigo desligados no perfil: só amigos calam");

    /* --- não lidas (BAS-22) --- */
    sm_unread u; memset(&u, 0, sizeof u);
    sm_unread_add(&u, "dm-a-b"); sm_unread_add(&u, "dm-a-b"); sm_unread_add(&u, "grp-1"); sm_unread_add(&u, ""); sm_unread_add(&u, NULL);
    OK(sm_unread_total(&u) == 3 && sm_unread_get(&u, "dm-a-b") == 2 && sm_unread_get(&u, "grp-1") == 1 && sm_unread_get(&u, "x") == 0, "contagem por conversa e total");
    OK(sm_unread_clear(&u, "dm-a-b") == 2 && sm_unread_total(&u) == 1 && sm_unread_clear(&u, "dm-a-b") == 0, "abrir a conversa zera só ela");
    for (int i = 0; i < 100; i++) { char kk[16]; snprintf(kk, sizeof kk, "c%d", i); sm_unread_add(&u, kk); }
    OK(sm_unread_total(&u) == 101, "lista cheia: o total continua certo");

    /* --- texto do balão --- */
    char t[64]; sm_trunc(t, sizeof t, "curto", 10); OK(!strcmp(t, "curto"), "texto curto intacto");
    sm_trunc(t, sizeof t, "ação é útil demais", 6); OK(!strcmp(t, "ação é\xe2\x80\xa6"), "corta por caractere UTF-8 e põe reticências");
    sm_trunc(t, 8, "abcdefghijkl", 100); OK(strlen(t) < 8, "respeita o buffer");

    /* --- links e iniciar com o Windows --- */
    OK(sm_url_ok("https://briks.app/x?y=1") && sm_url_ok("http://a.com") && !sm_url_ok("file:///c:/a") && !sm_url_ok("javascript:alert(1)") && !sm_url_ok("ftp://a.com/x") && !sm_url_ok("https://") && !sm_url_ok("https:///x") && !sm_url_ok("https://a b.com") && !sm_url_ok("https://a.com/\"x") && !sm_url_ok(NULL), "só http(s) abre no navegador");
    char cmd[300]; sm_autostart_cmd("C:\\Program Files\\Briks\\briks_cui.exe", cmd, sizeof cmd);
    OK(!strcmp(cmd, "\"C:\\Program Files\\Briks\\briks_cui.exe\" --minimizado"), "comando do autostart com aspas e --minimizado");

    /* --- sons (CHM-20, BAS-21) --- */
    for (int kd = 0; kd < SND_COUNT; kd++) {
        static unsigned char b[SM_WAV_CAP]; size_t n = sm_wav((sound_kind_t)kd, 70, b, sizeof b);
        uint32_t riff, data; memcpy(&riff, b + 4, 4); memcpy(&data, b + 40, 4); short *pcm = (short *)(b + 44); int peak = 0;
        for (size_t i = 0; i < data / 2; i++) { int v = abs(pcm[i]); if (v > peak) peak = v; }
        char nm[64]; snprintf(nm, sizeof nm, "WAV %d: cabeçalho, tamanho e sinal audível", kd);
        OK(n > 1000 && n <= SM_WAV_CAP && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVEfmt ", 8) && !memcmp(b + 36, "data", 4) && riff == n - 8 && data == n - 44 && peak > 2000 && peak < 32000, nm);
    }
    { static unsigned char a[SM_WAV_CAP], z[SM_WAV_CAP]; size_t na = sm_wav(SND_NOTIFY, 100, a, sizeof a), nz = sm_wav(SND_NOTIFY, 0, z, sizeof z); short *pz = (short *)(z + 44); int pk = 0;
      for (size_t i = 0; i < (nz - 44) / 2; i++) if (abs(pz[i]) > pk) pk = abs(pz[i]);
      OK(na == nz && pk == 0, "volume 0 gera silêncio do mesmo tamanho"); }
    { static unsigned char j[SM_WAV_CAP], l[SM_WAV_CAP]; sm_wav(SND_JOIN, 70, j, sizeof j); sm_wav(SND_LEAVE, 70, l, sizeof l); OK(memcmp(j + 44, l + 44, 2000) != 0, "entrar e sair soam diferentes"); }

    /* --- ícone do contador (pixels) --- */
    { uint32_t a[32 * 32], b2[32 * 32], c2[32 * 32]; sm_badge_bgra(3, 0x7c3aed, 32, a); sm_badge_bgra(12, 0x7c3aed, 32, b2); sm_badge_bgra(9, 0x7c3aed, 32, c2);
      int white = 0, opaque = 0; for (int i = 0; i < 1024; i++) { if ((a[i] >> 24) == 255) { opaque++; if ((a[i] & 0xffffff) == 0xffffff) white++; } }
      OK((a[0] >> 24) == 0 && (a[31] >> 24) == 0 && (a[16 * 32 + 16] >> 24) == 255, "círculo: cantos transparentes, centro opaco");
      OK(white > 20 && opaque > 500 && (a[16 * 32 + 2] & 0xffffff) == 0x7c3aed, "número branco sobre a cor do tema");
      OK(memcmp(a, b2, sizeof a) != 0 && memcmp(a, c2, sizeof a) != 0, "3, 9 e 12 (9+) desenham diferente");
      uint32_t d[20 * 20]; sm_badge_bgra(5, 0xe95555, 20, d); int w2 = 0; for (int i = 0; i < 400; i++) if ((d[i] & 0xffffff) == 0xffffff && (d[i] >> 24) == 255) w2++;
      OK(w2 > 8, "versão pequena (bandeja) também tem o número"); }

    printf(fails ? "\n%d falha(s)\n" : "\ntudo certo\n", fails);
    return fails ? 1 : 0;
}
