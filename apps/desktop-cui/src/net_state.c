/* Estado da conexão com a internet (BAS-32): uma sonda em segundo plano e a faixa persistente no topo da janela.
 *
 * A sonda só abre uma conexão TCP (porta 443) até um de três endereços públicos e fecha na hora: não envia nem lê nenhum dado, não usa
 * conta nem servidor do Briks. Ela dá "sem conexão" só depois de duas rodadas seguidas sem resposta (evita piscar por um pacote perdido).
 * Testes e capturas ocultas NÃO sondam a rede: BRIKS_NET=online|offline|reconnecting|restored força o estado (sem a variável, em modo
 * automatizado o estado é "online"). */
#include "briks_ui.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static volatile LONG g_net = NET_UNKNOWN;
static volatile LONG g_kick;
static HANDLE g_ev, g_thread;
static ULONGLONG g_restored_at;          /* quando voltou do offline (faixa verde por alguns segundos) */

static bool try_connect(const char *ip, int port, int ms) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    bool ok = false;
    int r = connect(s, (struct sockaddr *)&a, sizeof a);
    if (r == 0) ok = true;
    else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set w, e; FD_ZERO(&w); FD_ZERO(&e); FD_SET(s, &w); FD_SET(s, &e);
        struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
        if (select(0, NULL, &w, &e, &tv) > 0 && FD_ISSET(s, &w)) ok = true;
    }
    closesocket(s);
    return ok;
}

static bool probe_once(void) {
    static const char *hosts[] = { "1.1.1.1", "8.8.8.8", "9.9.9.9" };
    for (int i = 0; i < 3; i++) if (try_connect(hosts[i], 443, 2500)) return true;
    return false;
}

static DWORD WINAPI probe_thread(LPVOID p) {
    (void)p;
    WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return 0;
    int fails = 0;
    for (;;) {
        bool ok = probe_once();
        if (ok) { fails = 0; InterlockedExchange(&g_net, NET_ONLINE); }
        else if (++fails >= 2) InterlockedExchange(&g_net, NET_OFFLINE);
        else if (g_net == NET_UNKNOWN) { /* primeira falha: segue sem afirmar nada */ }
        WaitForSingleObject(g_ev, fails ? 3000 : 20000);
        if (InterlockedExchange(&g_kick, 0)) fails = fails > 0 ? 1 : 0;   /* "Tentar agora": uma falha basta para manter offline */
    }
}

static int forced(void) {
    static int f = -2;
    if (f == -2) {
        const char *e = getenv("BRIKS_NET"); f = -1;
        if (e && !strcmp(e, "online")) f = NET_ONLINE;
        else if (e && (!strcmp(e, "offline") || !strcmp(e, "reconnecting") || !strcmp(e, "restored"))) f = NET_OFFLINE;
        else if (getenv("BRIKS_SCRIPT") || getenv("CUI_SCREENSHOT") || getenv("BRIKS_HIDDEN") || getenv("BRIKS_DISABLE_NETPROBE") || (getenv("BRIKS_NET_LOOPBACK_ONLY") && getenv("BRIKS_NET_LOOPBACK_ONLY")[0] == '1')) f = NET_ONLINE;
        if (e && !strcmp(e, "reconnecting")) f = NET_ONLINE;
        if (e && !strcmp(e, "restored")) f = NET_ONLINE;
    }
    return f;
}

int net_status(void) {
    int f = forced();
    if (f >= 0) return f;
    if (!g_thread) {
        g_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_thread = CreateThread(NULL, 0, probe_thread, NULL, 0, NULL);
    }
    return (int)g_net;
}

void net_retry_now(void) { InterlockedExchange(&g_kick, 1); if (g_ev) SetEvent(g_ev); }

/* mensagem de erro de rede em linguagem de gente: usada quando o core/servidor devolve algo técnico */
void net_friendly_error(const char *raw, char *out, size_t cap) {
    char low[400]; size_t n = 0;
    for (; raw && raw[n] && n < sizeof low - 1; n++) low[n] = (char)((raw[n] >= 'A' && raw[n] <= 'Z') ? raw[n] + 32 : raw[n]);
    low[n] = 0;
    const char *m;
    if (!raw || !raw[0]) { if (cap) out[0] = 0; return; }   /* sem texto: fica vazio (quem chama mostra o seu padrão) */
    else if (strstr(low, "timed out") || strstr(low, "timeout") || strstr(low, "10060")) m = "O servidor demorou demais para responder. Confira a internet e tente de novo.";
    else if (strstr(low, "dns") || strstr(low, "resolve") || strstr(low, "no such host") || strstr(low, "11001")) m = "Não foi possível encontrar o servidor. Confira a internet e tente de novo.";
    else if (strstr(low, "connection refused") || strstr(low, "10061") || strstr(low, "connect error") || strstr(low, "network is unreachable") || strstr(low, "error sending request")) m = "Sem conexão com o servidor. Confira a internet e tente de novo.";
    else if (strstr(low, "certificate") || strstr(low, "ssl") || strstr(low, "tls")) m = "Não foi possível abrir uma conexão segura. Confira a data e a hora do computador.";
    else if (strstr(low, "relógio deste pc difere")) m = "O relógio deste PC está fora de hora e o servidor de chamadas recusou o acesso. Ajuste a data e a hora do Windows.";
    else if (strstr(low, "servidor de chamadas recusou a conexão") && (strstr(low, "401") || strstr(low, "403") || strstr(low, "unauthorized") || strstr(low, "forbidden"))) m = "O servidor de chamadas recusou o token da sala (não é a sua senha). Confira a data e a hora do computador; se continuar, use P2P.";
    else if (strstr(low, "serviço de tokens recusou (401") || strstr(low, "serviço de tokens recusou (403")) m = "O servidor de chamadas recusou o acesso desta conta. Saia e entre de novo no Briks; se continuar, use P2P.";   /* não é senha: é a sessão/instalação que o servidor de chamadas não aceitou */
    else if (strstr(low, "401") || strstr(low, "unauthorized") || strstr(low, "invalid login")) m = "E-mail ou senha incorretos.";
    else if (strstr(low, "429") || strstr(low, "too many")) m = "Muitas tentativas seguidas. Espere um pouco e tente de novo.";
    else if (strstr(low, "500") || strstr(low, "502") || strstr(low, "503") || strstr(low, "504")) m = "O servidor está com problemas agora. Tente de novo em alguns minutos.";
    else { snprintf(out, cap, "%s", raw); return; }
    snprintf(out, cap, "%s", m);
}

/* Pílula persistente no topo, por cima da pilha (não mexe no layout do dock, que mede a janela inteira). Fica acima do texto dos cabeçalhos e
   cobre a área para o que está por baixo não receber hover/clique. Some sozinha quando a conexão volta. */
void net_banner(cui_ctx *ui, app_state_t *s) {
    static bool was_off;
    const char *e = getenv("BRIKS_NET");
    int st = net_status();
    if (st == NET_OFFLINE) was_off = true;
    else if (st == NET_ONLINE && was_off) { was_off = false; g_restored_at = GetTickCount64(); }
    if (e && !strcmp(e, "restored") && !g_restored_at) g_restored_at = GetTickCount64();
    const bool restored = g_restored_at && GetTickCount64() - g_restored_at < 4000;
    const bool offline = st == NET_OFFLINE;
    const bool reconn = !offline && !restored && ((s->rtc_state[0] && !strcmp(s->rtc_state, "reconectando")) || (e && !strcmp(e, "reconnecting")));
    if (!offline && !reconn && !restored) return;
    if (restored) cui_request_redraw(ui);   /* some sozinha depois de 4 s */
    cui_color bg = offline ? CUI_RGBA(0x5a1620f4) : reconn ? CUI_RGBA(0x4a3a10f4) : CUI_RGBA(0x0f3d26f4);
    cui_color fg = offline ? CUI_RGB(0xffd5d5) : reconn ? CUI_RGB(0xffe6a8) : CUI_RGB(0xc8f5da);
    cui_rect pr = cui_get_rect(ui, cui_make_id(ui, "net-pill"));
    if (pr.w > 0) briks_cover(pr.x, pr.y, pr.w, pr.h);
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_START, .pad_top = 5, .pad_x = 8) {
        cui_box(ui, .id = "net-pill", .dir = CUI_ROW, .pad_x = 14, .pad_y = 4, .gap = 9, .radius = 18, .bg = bg, .border = 1, .border_color = B_GLASS_BORDER,
                .align = CUI_ALIGN_CENTER, .elevation = briks_elev(CUI_ELEVATION_RAISED)) {
            if (offline || reconn) cui_spinner(ui, 14);
            else cui_icon(ui, CUI_ICON_CIRCLE_CHECK, 16, fg);
            cui_text(ui, offline ? "Sem conexão. Tentando reconectar…" : reconn ? "Reconectando à chamada…" : "Conexão restabelecida.", .size = 13, .color = fg, .nowrap = true);
            if (offline) {
                cui_box(ui, .id = "net-retry", .pad_x = 9, .pad_y = 1, .radius = 10, .bg = CUI_RGBA(0xffffff22), .align = CUI_ALIGN_CENTER) {
                    cui_text(ui, "Tentar agora", .size = 12, .color = fg, .nowrap = true);
                }
                if (briks_hover_top(ui, "net-retry")) briks_tip("Verificar a conexão agora");
                if (briks_clicked_top(ui, "net-retry")) net_retry_now();
            }
        }
    }
}
