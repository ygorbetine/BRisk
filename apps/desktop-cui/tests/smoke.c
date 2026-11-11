/* Teste de fumaça: sidecar real + cliente HTTP + JSON. Usa %APPDATA%\Briks-CUI (não toca no Briks de produção). */
#include "../src/backend.h"
#include "../src/json.h"
#include <math.h>
#include "../src/layout.h"
#include "../src/core.h"
#include "../src/update.h"
#include "../src/drag_stack.h"
#include "../src/hotkeys.h"
#include "../src/netdiag.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FALHOU: " __VA_ARGS__); printf("\n"); } else { printf("ok: " __VA_ARGS__); printf("\n"); } } while (0)

static bk_call *wait_call(bk_call *c) { while (!bk_done(c)) Sleep(10); return c; }


/* ---- árvore de layout (src/layout.c): operações puras, sem backend ---- */
static float area_sum(const layout_t *l, lrect area) {   /* área total das folhas + vãos = área da raiz */
    lrect r[LAYOUT_MAX_NODES]; layout_compute(l, area, r);
    float s = 0; for (int i = 0; i < LAYOUT_MAX_NODES; i++) if (l->n[i].used && l->n[i].leaf) s += r[i].w * r[i].h;
    return s;
}
static void hotkey_tests(void) {
    unsigned m; int vk;
    CHECK(hk_parse("Ctrl+Shift+M", &m, &vk) && m == (HK_CTRL | HK_SHIFT) && vk == 'M', "atalho Ctrl+Shift+M");
    CHECK(hk_parse("alt + f5", &m, &vk) && m == HK_ALT && vk == 0x74, "atalho Alt+F5 (espaços e caixa)");
    CHECK(hk_parse("Mouse4", &m, &vk) && m == 0 && vk == 0x05 && hk_parse("Mouse5", &m, &vk) && vk == 0x06, "botões laterais do mouse");
    CHECK(!hk_parse("", &m, &vk) && !hk_parse("Ctrl+Shift", &m, &vk) && !hk_parse("Ctrl+A+B", &m, &vk) && !hk_parse("Ctrl+Banana", &m, &vk), "atalhos inválidos recusados");
    hk_ptt_t p; memset(&p, 0, sizeof p);
    CHECK(hk_ptt_step(&p, true, 1000, 200), "PTT abre ao apertar");
    CHECK(hk_ptt_step(&p, false, 1100, 200), "PTT segue aberto durante o atraso ao soltar");
    CHECK(!hk_ptt_step(&p, false, 1300, 200), "PTT fecha depois do atraso");
    CHECK(hk_ptt_step(&p, true, 1400, 200) && hk_ptt_step(&p, false, 1401, 0) == false, "PTT sem atraso fecha na hora");
}

static void netdiag_tests(void) {
    uint8_t tx[12] = { 1,2,3,4,5,6,7,8,9,10,11,12 }, rq[20], rs[20];
    stun_build_request(rq, tx);
    CHECK(rq[0] == 0 && rq[1] == 1 && rq[4] == 0x21 && !memcmp(rq + 8, tx, 12), "pacote STUN Binding Request");
    memcpy(rs, rq, 20); rs[0] = 0x01; rs[1] = 0x01;
    CHECK(stun_is_response(rs, 20, tx) && !stun_is_response(rq, 20, tx), "resposta STUN reconhecida; o próprio pedido não");
    uint8_t other[12] = { 9 }; CHECK(!stun_is_response(rs, 20, other) && !stun_is_response(rs, 10, tx), "txid diferente e pacote curto recusados");
    double r[5] = { 20, 30, -1, 20, 30 }; net_stats_t s = net_stats(r, 5);
    CHECK(s.got == 4 && fabs(s.avg_ms - 25) < 1e-9 && fabs(s.loss_pct - 20) < 1e-9 && fabs(s.jitter_ms - 10) < 1e-9, "estatística: média 25 ms, perda 20%%, variação 10 ms (veio %.1f / %.0f%% / %.1f)", s.avg_ms, s.loss_pct, s.jitter_ms);
    double z[3] = { -1, -1, -1 }; s = net_stats(z, 3); CHECK(s.got == 0 && s.loss_pct == 100, "sem resposta: perda 100%%");
}

static void layout_tests(void) {
    const lrect area = { 10, 10, 1260, 777 };
    layout_t l; layout_default(&l, 1280);
    CHECK(layout_valid(&l) && layout_count(&l) == 3, "layout padrão válido com 3 painéis");
    lrect r[LAYOUT_MAX_NODES]; layout_compute(&l, area, r);
    int s = layout_find(&l, P_SERVERS), n = layout_find(&l, P_NAV), c = layout_find(&l, P_CHAT);
    CHECK((int)(r[s].w + .5f) == 76 && (int)(r[n].w + .5f) == 270, "padrão: servidores 76px e menu 270px (veio %.1f e %.1f)", r[s].w, r[n].w);
    CHECK((int)(r[n].x - (r[s].x + r[s].w) + .5f) == 10 && (int)(r[c].x - (r[n].x + r[n].w) + .5f) == 10, "vão de 10px entre os painéis");
    CHECK((int)(r[c].x + r[c].w + .5f) == 1270 && (int)(r[c].h + .5f) == 777, "a árvore preenche a área inteira");

    /* redimensionar: a divisória move, o vizinho cresce */
    int inner = l.n[n].parent; lrect ia = r[inner];
    float before = r[c].w;
    CHECK(layout_resize(&l, inner, ia, ia.x + 400), "arrastar a divisória do menu para 400px");
    layout_compute(&l, area, r);
    CHECK(r[n].w > 380 && r[c].w < before - 100, "menu cresceu (%.0f) e a conversa encolheu (%.0f)", r[n].w, r[c].w);
    CHECK(layout_resize(&l, inner, ia, ia.x + 5) , "arrastar além do mínimo é limitado, não recusado");
    layout_compute(&l, area, r);
    CHECK(r[n].w >= panel_min_w(P_NAV) - .01f, "mínimo do menu respeitado (%.1f >= %.0f)", r[n].w, panel_min_w(P_NAV));
    CHECK(layout_resize(&l, inner, ia, ia.x + 5000), "arrastar para o outro extremo também é limitado");
    layout_compute(&l, area, r);
    CHECK(r[c].w >= panel_min_w(P_CHAT) - .01f, "mínimo da conversa respeitado (%.1f)", r[c].w);

    /* split: encaixar na lateral e por cima */
    layout_default(&l, 1280);
    CHECK(layout_split(&l, P_CHAT, P_MEMBERS, Z_RIGHT) && layout_valid(&l) && layout_count(&l) == 4, "membros encaixa à direita da conversa");
    CHECK(layout_split(&l, P_CHAT, P_VOICE, Z_TOP) && layout_valid(&l) && layout_count(&l) == 5, "sala de voz encaixa por cima da conversa");
    CHECK(!layout_split(&l, P_CHAT, P_VOICE, Z_LEFT), "painel repetido é recusado");
    CHECK(!layout_split(&l, P_HISTORY, P_FRIENDS, Z_LEFT), "alvo inexistente é recusado");
    layout_compute(&l, area, r);
    CHECK(area_sum(&l, area) > 0.9f * area.w * area.h && area_sum(&l, area) <= area.w * area.h, "sem espaço vazio depois de dividir");
    int v = layout_find(&l, P_VOICE); c = layout_find(&l, P_CHAT);
    CHECK(r[v].y < r[c].y && (int)(r[v].x + .5f) == (int)(r[c].x + .5f), "voz acima da conversa, mesma coluna");

    /* remover: o vizinho absorve */
    float cw = r[layout_find(&l, P_CHAT)].h, vh = r[layout_find(&l, P_VOICE)].h;
    CHECK(layout_remove(&l, P_VOICE) && layout_valid(&l) && layout_count(&l) == 4, "fechar a sala de voz");
    layout_compute(&l, area, r);
    CHECK(r[layout_find(&l, P_CHAT)].h > cw && (int)(r[layout_find(&l, P_CHAT)].h + .5f) == 777, "conversa absorveu o espaço (%.0f + %.0f -> %.0f)", cw, vh, r[layout_find(&l, P_CHAT)].h);
    layout_t one; layout_default(&one, 1280);
    layout_remove(&one, P_SERVERS); layout_remove(&one, P_NAV);
    CHECK(!layout_remove(&one, P_CHAT) && layout_count(&one) == 1, "o último painel não fecha");

    /* mover */
    layout_default(&l, 1280);
    CHECK(layout_move(&l, P_CHAT, P_NAV, Z_LEFT) && layout_valid(&l) && layout_count(&l) == 3, "mover a conversa para a esquerda do menu");
    layout_compute(&l, area, r);
    CHECK(r[layout_find(&l, P_CHAT)].x < r[layout_find(&l, P_NAV)].x, "ordem mudou: conversa antes do menu");
    CHECK(layout_move(&l, P_SERVERS, P_CHAT, Z_CENTER) && layout_valid(&l), "centro troca dois painéis de lugar");
    layout_compute(&l, area, r);
    CHECK(r[layout_find(&l, P_SERVERS)].w > 300, "servidores ocupa o lugar que era da conversa (%.0f)", r[layout_find(&l, P_SERVERS)].w);
    CHECK(!layout_move(&l, P_NAV, P_NAV, Z_LEFT), "mover para si mesmo é recusado");
    CHECK(area_sum(&l, area) > 0.9f * area.w * area.h, "sem espaço vazio depois de mover");

    /* zonas */
    lrect pr = { 100, 100, 400, 200 };
    CHECK(layout_zone_at(pr, 110, 200) == Z_LEFT && layout_zone_at(pr, 490, 200) == Z_RIGHT, "zonas esquerda e direita");
    CHECK(layout_zone_at(pr, 300, 110) == Z_TOP && layout_zone_at(pr, 300, 290) == Z_BOTTOM, "zonas topo e base");
    CHECK(layout_zone_at(pr, 300, 200) == Z_CENTER && layout_zone_at(pr, 50, 50) == Z_NONE, "centro e fora");

    /* JSON: ida e volta, e entradas inválidas */
    layout_default(&l, 1280); layout_split(&l, P_CHAT, P_MEMBERS, Z_RIGHT); layout_split(&l, P_NAV, P_VOICE, Z_BOTTOM);
    char js[2048]; size_t len = layout_to_json(&l, js, sizeof js);
    layout_t back; CHECK(len > 0 && layout_from_json(&back, js) && layout_valid(&back), "JSON do layout lê de volta");
    char js2[2048]; layout_to_json(&back, js2, sizeof js2);
    CHECK(!strcmp(js, js2), "ida e volta idêntica");
    CHECK(!layout_from_json(&back, "{\"p\":\"nada\"}") && !layout_from_json(&back, "lixo") && !layout_from_json(&back, "{\"v\":0,\"r\":2,\"a\":{\"p\":\"nav\"},\"b\":{\"p\":\"chat\"}}"), "JSON inválido é recusado");
    CHECK(!layout_from_json(&back, "{\"v\":0,\"r\":0.5,\"a\":{\"p\":\"nav\"},\"b\":{\"p\":\"nav\"}}"), "painel duplicado no JSON é recusado");
    layout_default(&back, 1280);
    layout_from_json(&back, "{\"v\":0,\"r\":2,\"a\":{\"p\":\"nav\"},\"b\":{\"p\":\"chat\"}}");
    CHECK(layout_valid(&back) && layout_count(&back) == 3, "layout anterior fica intacto quando o JSON é inválido");
}

/* ---- pilha levantada: mola, inclinação por velocidade e fases ---- */
static void pile_tests(void) {
    spring1 s = { 0, 0 };
    for (int i = 0; i < 90; i++) spring_step(&s, 100, 1.0f / 60, 400, 1.0f);
    CHECK(fabsf(s.x - 100) < 1.0f, "mola crítica chega ao alvo em 1,5 s (%.2f)", s.x);
    spring1 e = { 0, 0 }; float peak = 0;
    for (int i = 0; i < 120; i++) { spring_step(&e, 1, 1.0f / 60, 380, 0.5f); if (e.x > peak) peak = e.x; }
    CHECK(peak > 1.05f && peak < 1.4f && fabsf(e.x - 1) < 0.01f, "mola elástica passa do alvo (pico %.3f) e assenta", peak);
    spring1 a = { 0, 0 }, b = { 0, 0 };
    for (int i = 0; i < 30; i++) spring_step(&a, 50, 1.0f / 30, 300, 0.8f);
    for (int i = 0; i < 120; i++) spring_step(&b, 50, 1.0f / 120, 300, 0.8f);
    CHECK(fabsf(a.x - b.x) < 1.5f, "resultado independe do passo de tempo (%.2f vs %.2f)", a.x, b.x);
    spring1 big = { 0, 0 }; spring_step(&big, 10, 0.5f, 5000, 0.7f);
    CHECK(isfinite(big.x) && fabsf(big.x) < 100, "passo grande não explode");

    CHECK(pile_tilt_deg(0) == 0 && pile_tilt_deg(1000) > 5 && pile_tilt_deg(-1000) < -5, "inclina para o lado em que se move");
    CHECK(pile_tilt_deg(100000) <= 14.01f && pile_tilt_deg(-100000) >= -14.01f, "inclinação limitada a 14 graus");

    pile_t p; pile_grab(&p, 300, 300);
    CHECK(p.phase == PILE_HELD && fabsf(p.rot[2].x - 2) < 0.01f && fabsf(p.rot[0].x + 5) < 0.01f, "repouso: giros -5, -2 e +2");
    for (int i = 0; i < 40; i++) pile_step(&p, 1.0f / 60);
    CHECK(p.lift.x > 0.9f && pile_scale(&p) > 1.035f && pile_scale(&p) < 1.06f, "pegar levanta a pilha (escala %.3f)", pile_scale(&p));
    float mx = 300; float max_tilt = 0;
    for (int i = 0; i < 30; i++) { mx += 20; pile_follow(&p, mx, 300, 1.0f / 60); pile_step(&p, 1.0f / 60); if (p.rot[2].x > max_tilt) max_tilt = p.rot[2].x; }
    CHECK(max_tilt > 6, "mover para a direita inclina a frente no sentido do movimento (%.1f)", max_tilt);
    CHECK(p.x[0].x < p.x[2].x - 8, "a camada de trás fica atrasada em relação à da frente (%.0f < %.0f)", p.x[0].x, p.x[2].x);
    for (int i = 0; i < 90; i++) { pile_follow(&p, mx, 300, 1.0f / 60); pile_step(&p, 1.0f / 60); }
    CHECK(fabsf(p.rot[2].x - 2) < 0.6f && fabsf(p.rot[0].x + 5) < 0.6f, "parado, o giro volta ao repouso (%.2f, %.2f)", p.rot[2].x, p.rot[0].x);
    CHECK(fabsf(p.x[2].x - mx) < 1.0f && fabsf(p.x[0].x - (mx - 10)) < 1.0f, "parado, as camadas assentam no empilhamento");
    pile_follow(&p, mx - 40, 300, 1.0f / 60); pile_step(&p, 1.0f / 60);
    CHECK(p.vx < 0, "velocidade negativa ao ir para a esquerda");
    pile_release(&p, 700, 400);
    CHECK(p.phase == PILE_DROP, "soltar inicia a descida");
    float prev = 2, mono = 1; float s0 = pile_scale(&p);
    for (int i = 0; i < 12; i++) { pile_step(&p, 1.0f / 60); float al = pile_alpha(&p); if (al > prev + 1e-4f) mono = 0; prev = al; }
    CHECK(mono && pile_scale(&p) < s0, "descida: opacidade só diminui e a pilha encolhe (%.3f -> %.3f)", s0, pile_scale(&p));
    for (int i = 0; i < 40; i++) pile_step(&p, 1.0f / 60);
    CHECK(!pile_active(&p) && pile_alpha(&p) == 0, "termina em ~260 ms e some (sem redesenho ocioso)");
    pile_grab(&p, 100, 100); for (int i = 0; i < 20; i++) { pile_follow(&p, 400, 300, 1.0f / 60); pile_step(&p, 1.0f / 60); }
    pile_cancel(&p, 100, 100);
    CHECK(p.phase == PILE_CANCEL, "Esc inicia a volta à origem");
    for (int i = 0; i < 8; i++) pile_step(&p, 1.0f / 60);
    CHECK(p.x[2].x < 380, "na volta o cartão da frente se aproxima da origem (%.0f)", p.x[2].x);
    for (int i = 0; i < 40; i++) pile_step(&p, 1.0f / 60);
    CHECK(!pile_active(&p), "a volta termina");
    pile_t idle = { 0 }; pile_step(&idle, 0.016f);
    CHECK(!pile_active(&idle) && idle.phase == PILE_IDLE, "ocioso: nada anima");
    /* arte do cartão: tamanho e grades */
    float cw, ch;
    stack_card_size(P_CHAT, 790, 777, &cw, &ch);
    CHECK(cw >= 150 && cw <= 200 && ch >= 100 && ch <= 128, "cartão da conversa dentro dos limites (%.0fx%.0f)", cw, ch);
    stack_card_size(P_NAV, 270, 777, &cw, &ch);
    CHECK(cw >= 150 && cw < 170 && ch >= 100 && ch <= 128, "painel alto vira cartão estreito com mínimo (%.0fx%.0f)", cw, ch);
    stack_card_size(P_SERVERS, 76, 777, &cw, &ch); float sw = cw;
    stack_card_size(P_VOICE, 400, 300, &cw, &ch);
    CHECK(sw >= 176 && cw >= 168, "servidores e voz ganham largura mínima para a grade (%.0f, %.0f)", sw, cw);
    stack_card_size(P_CHAT, 1000, 100, &cw, &ch);
    CHECK(cw <= 200 && ch >= 100, "painel muito largo respeita o teto (%.0fx%.0f)", cw, ch);
    stack_grid g;
    g = stack_voice_grid(1); CHECK(g.cols == 1 && g.rows == 1 && g.cells == 1 && !g.plus_n, "voz: 1 pessoa = 1 bloco");
    g = stack_voice_grid(3); CHECK(g.cols == 3 && g.rows == 1 && g.cells == 3, "voz: 3 pessoas em uma linha");
    g = stack_voice_grid(4); CHECK(g.cols == 2 && g.rows == 2 && g.cells == 4, "voz: 4 pessoas = 2x2");
    g = stack_voice_grid(5); CHECK(g.cols == 3 && g.rows == 2 && g.cells == 5 && !g.plus_n, "voz: 5 pessoas = 3x2 com 5 blocos");
    g = stack_voice_grid(6); CHECK(g.cols == 3 && g.rows == 2 && g.cells == 6 && !g.plus_n, "voz: 6 pessoas = 3x2 cheio");
    g = stack_voice_grid(9); CHECK(g.cols == 3 && g.rows == 2 && g.cells == 6 && g.plus_n == 4, "voz: 9 pessoas = 3x2 com +4 no último");
    g = stack_voice_grid(0); CHECK(g.cells == 1, "voz: 0 vira 1 bloco");
    g = stack_servers_grid(0); CHECK(g.cols == 6 && g.rows == 1 && g.cells == 2 && !g.plus_n, "servidores: só B e + quando não há comunidades");
    g = stack_servers_grid(3); CHECK(g.cells == 5 && g.rows == 1 && !g.plus_n, "servidores: 3 comunidades = 5 quadrados em uma linha");
    g = stack_servers_grid(5); CHECK(g.cells == 7 && g.rows == 2, "servidores: 7 quadrados quebram para a 2a linha");
    g = stack_servers_grid(10); CHECK(g.cells == 12 && g.rows == 2 && !g.plus_n, "servidores: exatamente 12 cabem sem +N");
    g = stack_servers_grid(11); CHECK(g.cells == 12 && g.plus_n == 2, "servidores: 13 itens, o 12o vira +2 (%d)", g.plus_n);
    g = stack_servers_grid(40); CHECK(g.cells == 12 && g.plus_n == 31, "servidores: 42 itens, o 12o vira +31 (%d)", g.plus_n);
}

/* ---- reajuste ao vivo: prévia, histerese e interpolação ---- */
static void live_tests(void) {
    const lrect area = { 10, 10, 1260, 777 };
    layout_t base; layout_default(&base, 1280);
    layout_t p;
    CHECK(layout_preview(&base, P_CHAT, false, P_NAV, Z_LEFT, &p) && layout_valid(&p) && layout_count(&p) == 3, "prévia de mover a conversa para a esquerda do menu é válida");
    lrect r[LAYOUT_MAX_NODES]; layout_compute(&p, area, r);
    CHECK(r[layout_find(&p, P_CHAT)].x < r[layout_find(&p, P_NAV)].x, "na prévia a conversa já está à esquerda do menu");
    CHECK(layout_find(&base, P_CHAT) >= 0 && base.n[layout_find(&base, P_CHAT)].parent == base.n[layout_find(&base, P_NAV)].parent, "a prévia não altera o layout original");
    layout_t real = base; layout_move(&real, P_CHAT, P_NAV, Z_LEFT);
    char a[4096], b[4096]; layout_to_json(&p, a, sizeof a); layout_to_json(&real, b, sizeof b);
    CHECK(!strcmp(a, b), "prévia == resultado de soltar (mesmo JSON)");
    CHECK(layout_preview(&base, P_VOICE, true, P_CHAT, Z_CENTER, &p) && layout_count(&p) == 4 && layout_valid(&p), "prévia de painel novo no centro vira encaixe à direita");
    layout_compute(&p, area, r);
    float tot = 0; for (int i = 0; i < LAYOUT_MAX_NODES; i++) if (p.n[i].used && p.n[i].leaf) tot += r[i].w * r[i].h;
    CHECK(tot > 0.9f * area.w * area.h, "a prévia preenche a tela sem buracos");
    CHECK(!layout_preview(&base, P_CHAT, false, P_CHAT, Z_CENTER, &p) && layout_count(&p) == 3, "soltar sobre si mesmo não muda nada");
    CHECK(!layout_preview(&base, P_VOICE, true, P_HISTORY, Z_LEFT, &p), "alvo ausente: sem prévia");

    /* histerese */
    zone_hold h; zone_hold_reset(&h);
    CHECK(zone_hold_update(&h, P_NAV, Z_LEFT, 0.016f, 0.10f) && h.cur_target == P_NAV && h.cur_zone == Z_LEFT, "primeira zona vale na hora");
    CHECK(!zone_hold_update(&h, P_CHAT, Z_RIGHT, 0.016f, 0.10f) && h.cur_target == P_NAV, "nova zona não troca no primeiro quadro");
    CHECK(!zone_hold_update(&h, P_NAV, Z_LEFT, 0.016f, 0.10f) && h.cur_target == P_NAV, "voltar à zona atual cancela o candidato");
    zone_hold_update(&h, P_CHAT, Z_RIGHT, 0.04f, 0.10f); zone_hold_update(&h, P_CHAT, Z_RIGHT, 0.04f, 0.10f);
    CHECK(h.cur_target == P_NAV, "ainda não passou o tempo de espera");
    CHECK(zone_hold_update(&h, P_CHAT, Z_RIGHT, 0.04f, 0.10f) && h.cur_target == P_CHAT && h.cur_zone == Z_RIGHT, "depois do tempo de espera a prévia troca");
    zone_hold_update(&h, -1, Z_NONE, 0.5f, 0.10f);
    CHECK(h.cur_target == P_CHAT, "cursor fora de qualquer painel mantém a zona atual");

    /* interpolação */
    lrect cur = { 0, 0, 100, 100 }, tg = { 200, 100, 300, 50 };
    CHECK(lrect_step(&cur, tg, 0.016f, 0.055f) && cur.x > 0 && cur.x < 200 && cur.w > 100 && cur.w < 300, "um passo anda só parte do caminho (sem salto)");
    float x1 = cur.x; lrect_step(&cur, tg, 0.016f, 0.055f);
    CHECK(cur.x > x1 && cur.x < 200, "o passo seguinte continua andando na mesma direção");
    int frames = 0; while (lrect_step(&cur, tg, 1.0f / 60.0f, 0.055f) && frames < 600) frames++;
    CHECK(frames < 24 && cur.x == 200 && cur.y == 100 && cur.w == 300 && cur.h == 50, "converge exatamente ao alvo em ~%d quadros (~%d ms a 60 fps)", frames, frames * 1000 / 60);
    CHECK(!lrect_step(&cur, tg, 1.0f / 60.0f, 0.055f), "parado no alvo: nada mais se move (CPU ocioso)");
    lrect snap = { 0, 0, 10, 10 };
    CHECK(lrect_step(&snap, tg, 0.016f, 0) && snap.x == 200 && snap.w == 300, "tau 0 vai direto ao alvo");
}

/* ---- briks-core: processo filho, IPC com token e sinalização contra um Realtime SIMULADO em loopback (sem Supabase) ---- */
static bool wait_ev(const char *needle, int ms, char *out, size_t cap) {
    for (int t = 0; t < ms; t += 20) {
        char line[8192];
        while (core_next_event(line, sizeof line)) if (strstr(line, needle)) { if (out) snprintf(out, cap, "%s", line); return true; }
        Sleep(20);
    }
    return false;
}

static void core_tests(void) {
    const char *exe = getenv("BRIKS_CORE_EXE");
    if (!exe || !exe[0]) { printf("pulado: BRIKS_CORE_EXE nao definido (testes do briks-core)\n"); return; }
    char cmd[MAX_PATH + 40]; snprintf(cmd, sizeof cmd, "\"%s\" mock-realtime 47831", exe);
    STARTUPINFOA si = { sizeof si }; PROCESS_INFORMATION pi = { 0 };
    BOOL mock = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CHECK(mock, "simulador de Realtime iniciou");
    Sleep(700);
    char tmp[MAX_PATH]; GetTempPathA(sizeof tmp, tmp);
    char data[MAX_PATH]; snprintf(data, sizeof data, "%sbriks-core-smoke-%lu", tmp, GetTickCount());
    SetEnvironmentVariableA("BRIKS_CORE_DATA", data);
    SetEnvironmentVariableA("BRIKS_REALTIME_URL", "ws://127.0.0.1:47831");
    SetEnvironmentVariableA("BRIKS_REALTIME_KEY", "chave-de-teste");
    core_start();
    for (int i = 0; i < 400 && core_state() == CORE_STARTING; i++) Sleep(50);
    CHECK(core_state() == CORE_READY, "briks-core iniciou e aceitou o token (%s)", core_error());
    if (core_state() == CORE_READY) {
        CHECK(wait_ev("hello.ok", 3000, NULL, 0), "hello com token valido");
        core_send("{\"cmd\":\"session.start\",\"display_name\":\"Smoke\"}");
        char ev[8192];
        CHECK(wait_ev("session.ready", 5000, ev, sizeof ev) && strstr(ev, "fingerprint"), "sessao criada com identidade ECDSA");
        core_send("{\"cmd\":\"room.join\",\"namespace\":\"room\",\"room\":\"smoke\"}");
        CHECK(wait_ev("\"state\":\"conectado\"", 10000, NULL, 0), "sinalizacao conectou (estado conectado)");
        core_send("{\"cmd\":\"room.leave\"}");
        CHECK(wait_ev("desconectado", 5000, NULL, 0), "saiu da sala (estado desconectado)");
    }
    core_stop();
    if (mock) { TerminateProcess(pi.hProcess, 0); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
}

static void update_tests(void) {
    CHECK(update_version_compare("1.2.2", "1.3.0") < 0 && update_version_compare("1.3.0", "1.2.9") > 0, "versao: ordem numerica");
    CHECK(update_version_compare("1.3.0", "1.3.0") == 0 && update_version_compare("1.3.0-cui.1", "1.3.0") < 0, "versao: igual e pre-lancamento antes da final");
    CHECK(update_version_compare("1.10.0", "1.9.0") > 0, "versao: 1.10 > 1.9");
    char v[48], p[128], h[200];
    const char *ok = "version: 1.3.1\nfiles:\n  - url: Briks-Setup-1.3.1.exe\n    sha512: ZZZ\n    size: 10\npath: Briks-Setup-1.3.1.exe\nsha512: AbC+/=\nreleaseDate: '2026-01-01T00:00:00.000Z'\n";
    CHECK(update_parse_feed(ok, v, sizeof v, p, sizeof p, h, sizeof h) && !strcmp(v, "1.3.1") && !strcmp(p, "Briks-Setup-1.3.1.exe") && !strcmp(h, "AbC+/="), "feed latest.yml do electron-updater lido");
    CHECK(!update_parse_feed("version: 1.3.1\npath: ..\\evil.exe\nsha512: x\n", v, sizeof v, p, sizeof p, h, sizeof h), "feed: caminho com .. ou barra invertida recusado");
    CHECK(!update_parse_feed("version: 1.3.1\npath: setup.zip\nsha512: x\n", v, sizeof v, p, sizeof p, h, sizeof h), "feed: so aceita .exe");
    CHECK(!update_parse_feed("version: 1.3.1\npath: a.exe\n", v, sizeof v, p, sizeof p, h, sizeof h), "feed: sem sha512 recusado");
    CHECK(!update_policy(false, false, false, false, false, true), "politica: sem Release/opt-in fica desligado");
    CHECK(update_policy(false, false, false, false, true, true), "politica: Release instalado liga por padrao");
    CHECK(!update_policy(false, false, false, false, true, false), "politica: Release fora do instalador (pasta de build) nao liga");
    CHECK(!update_policy(true, true, true, true, true, true), "politica: BRIKS_DISABLE_UPDATE=1 vence tudo");
    CHECK(update_policy(false, true, false, false, false, false), "politica: feed de teste liga");
    CHECK(update_url_allowed("https://github.com/x/y/latest.yml") && update_url_allowed("http://127.0.0.1:8099/latest.yml") && !update_url_allowed("http://example.com/latest.yml") && !update_url_allowed("ftp://x"), "url: https ou http so em loopback");
}

int main(void) {
    layout_tests();
    hotkey_tests();
    netdiag_tests();
    update_tests();
    pile_tests();
    live_tests();
    core_tests();
    bk_start();
    for (int i = 0; i < 400 && bk_status() == BK_STARTING; i++) Sleep(50);
    CHECK(bk_status() == BK_READY, "sidecar iniciou (%s)", bk_error());
    if (bk_status() != BK_READY) return 1;

    char email[64]; snprintf(email, sizeof email, "smoke%lu@briks.local", GetTickCount());
    char body[1024];
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

    /* marco 2: mensagem e grupo persistidos pelas rotas /p2p */
    snprintf(body, sizeof body, "{\"id\":\"msg-smoke-0001\",\"channelId\":\"dm-smoke-channel\",\"author\":\"Teste CUI\",\"content\":\"ola mundo\",\"createdAt\":\"2026-11-09T12:00:00.000Z\",\"reactions\":{}}");
    c = wait_call(bk_request("POST", "/p2p/messages/dm-smoke-channel", body, token));
    CHECK(bk_http_status(c) == 200, "grava mensagem P2P (veio %d: %s)", bk_http_status(c), bk_body(c));
    bk_free(c);
    c = wait_call(bk_request("GET", "/p2p/messages/dm-smoke-channel?limit=50", NULL, token));
    j = json_parse(bk_body(c));
    CHECK(bk_http_status(c) == 200 && json_len(j) == 1 && !strcmp(json_str(json_at(j, 0), "id", ""), "msg-smoke-0001"), "mensagem volta do histórico");
    json_free(j); bk_free(c);
    snprintf(body, sizeof body, "{\"groupId\":\"grp-smoke-0001\",\"name\":\"Grupo teste\",\"avatar\":null,\"channels\":[{\"id\":\"chan-smoke-0001\",\"name\":\"geral\",\"type\":\"text\"}],\"members\":[],\"joinedAt\":1760000000000,\"ownerPeerId\":\"peer-smoke-0001\",\"membershipVersion\":1,\"manifestVersion\":1,\"administratorPeerIds\":[],\"removedPeerIds\":[],\"removedMembers\":[]}");
    c = wait_call(bk_request("POST", "/p2p/groups", body, token));
    CHECK(bk_http_status(c) == 200, "grava grupo P2P (veio %d: %s)", bk_http_status(c), bk_body(c));
    bk_free(c);
    c = wait_call(bk_request("GET", "/p2p/groups", NULL, token));
    j = json_parse(bk_body(c));
    CHECK(bk_http_status(c) == 200 && json_len(j) == 1 && !strcmp(json_str(json_at(j, 0), "name", ""), "Grupo teste"), "grupo volta da lista");
    json_free(j); bk_free(c);

    bk_stop();
    printf(fails ? "\n%d falha(s)\n" : "\ntudo certo\n", fails);
    return fails ? 1 : 0;
}
