/* Configurações > Rede > TESTE DE CONEXÃO (RDE-04): um botão dispara o STUN (atraso, variação, perda e se o NAT muda a porta por destino: netdiag.c)
 * e o teste do relay TURN no core (ice.test); o resultado vira linhas com selos de cor e um veredito "precisa de relay". */
#include "briks_ui.h"
#include "netdiag.h"
#include "core.h"
#include <stdio.h>
#include <string.h>

typedef enum { NV_NONE, NV_GOOD, NV_RELAY, NV_BAD } nt_verdict_t;
static bool g_asked;

/* Veredito único a partir do STUN e do relay. Puro e sondável. */
static nt_verdict_t verdict(const netdiag_t *nd, const app_state_t *s) {
    if (!nd->done) return NV_NONE;
    if (!nd->reachable) return s->ice_test_level == 3 ? NV_RELAY : NV_BAD;   /* UDP bloqueado: só o relay (se respondeu) salva */
    if (nd->nat_symmetric) return NV_RELAY;
    if (nd->st.loss_pct >= 20 || nd->st.avg_ms >= 200) return NV_BAD;
    return NV_GOOD;
}

static void chip(cui_ctx *ui, const char *t, cui_color c) {
    cui_box(ui, .pad_x = 10, .pad_y = 3, .radius = 11, .bg = B_A(c, 0x2a), .border = 1, .border_color = B_A(c, 0x77)) {
        cui_text(ui, t, .size = 12, .color = c, .nowrap = true);
    }
}

void nettest_card(cui_ctx *ui, app_state_t *s) {
    netdiag_t nd = netdiag_get();
    const nt_verdict_t v = verdict(&nd, s);
    const bool busy = nd.running || s->ice_testing;
    cui_text(ui, "Mede o atraso até o STUN, a variação e a perda (5 pacotes), vê se a sua rede muda a porta por destino e testa o relay (TURN) se houver um configurado. Uns 10 segundos.", .size = 12, .color = B_MUTED, .width = CUI_FILL);
    if (cui_button(ui, busy ? "Testando…" : nd.done ? "Testar de novo" : "Testar conexão", .variant = CUI_BUTTON_PRIMARY, .loading = busy, .disabled = busy)) {
        g_asked = true; netdiag_start(); if (core_state() == CORE_READY) account_ice_test(s);
    }
    if (busy) cui_request_wakeup(ui, 0.2f);
    if (!nd.done || busy) return;
    cui_row(ui, .gap = 8, .wrap = true, .width = CUI_FILL) {
        if (v == NV_GOOD) chip(ui, "Conexão boa", B_ONLINE);
        else if (v == NV_RELAY) chip(ui, "Precisa de relay", CUI_RGB(0xe8c46a));
        else chip(ui, "Conexão ruim", B_DANGER);
        chip(ui, nd.reachable ? "STUN responde" : "STUN sem resposta", nd.reachable ? B_ONLINE : B_DANGER);
        if (s->ice_test_level == 3) chip(ui, "Relay responde", B_ONLINE);
        else if (s->ice_test_level == 2 && s->ice_turn) chip(ui, "Relay sem resposta", B_DANGER);
        else if (!s->ice_turn) chip(ui, "Sem relay configurado", B_MUTED);
    }
    if (nd.reachable) {
        cui_textf(ui, "Até o STUN: %.0f ms", nd.st.avg_ms);
        cui_textf(ui, "Variação: %.0f ms", nd.st.jitter_ms);
        cui_textf(ui, "Perda: %.0f%%", nd.st.loss_pct);
        if (s->ice_test_level == 3 && s->ice_test_ms > 0) cui_textf(ui, "Relay (TURN): respondeu em %d ms", s->ice_test_ms);
    }
    const char *why = "";
    if (v == NV_GOOD) why = nd.nat_checked ? "Sua rede mantém a mesma porta para destinos diferentes: a conexão direta (P2P) costuma funcionar sem relay." : "O STUN respondeu bem; não deu para conferir o tipo de NAT, mas redes comuns conectam direto.";
    else if (v == NV_RELAY) why = nd.reachable ? "Sua rede troca a porta pública a cada destino (NAT simétrico, comum em CGNAT e 4G): a conexão direta tende a falhar e a chamada precisa passar por um relay (TURN)." : "O STUN não respondeu (UDP pode estar bloqueado), mas o relay sim: as chamadas só conectam pelo relay.";
    else if (v == NV_BAD) why = nd.reachable ? "A conexão está instável (perda ou atraso altos): a qualidade da chamada pode cair." : "Sem resposta do STUN: a rede pode bloquear UDP. Chamadas diretas podem falhar; tente outra rede ou configure um TURN.";
    if (v == NV_RELAY && !s->ice_turn) why = "Sua rede parece precisar de relay (NAT simétrico ou UDP bloqueado) e não há TURN configurado: sem ele as chamadas podem não conectar. Veja o cartão Servidor TURN logo abaixo.";
    cui_text(ui, why, .size = 12, .color = B_MUTED, .width = CUI_FILL);
    if (s->ice_test_msg[0] && g_asked && s->ice_turn) cui_text(ui, s->ice_test_msg, .size = 12, .color = B_MUTED, .width = CUI_FILL);
}

int nettest_probe(const char *n, app_state_t *s) {
    netdiag_t nd = netdiag_get();
    if (!strcmp(n, "net_done")) return nd.done;
    if (!strcmp(n, "net_running")) return nd.running;
    if (!strcmp(n, "net_verdict")) return (int)verdict(&nd, s);   /* 0 nada, 1 boa, 2 precisa de relay, 3 ruim */
    if (!strcmp(n, "net_reach")) return nd.reachable;
    if (!strcmp(n, "net_nat_symmetric")) return nd.nat_symmetric;
    if (!strcmp(n, "net_nat_checked")) return nd.nat_checked;
    if (!strcmp(n, "net_loss")) return (int)(nd.st.loss_pct + 0.5);
    if (!strcmp(n, "net_vpn_n")) return nd.vpn_n;
    if (!strcmp(n, "net_turn")) return s->ice_turn;
    return -9999;
}
