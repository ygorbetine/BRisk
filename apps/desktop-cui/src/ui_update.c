/* Aviso de atualização (vidro, canto inferior direito): aparece quando o instalador novo já foi baixado e verificado.
 * Também: Configurações > Atualizações com "Verificar atualização agora" (ATU-04) e "Novidades da versão em uso" (CFG-29). */
#include "briks_ui.h"
#include "update.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void update_banner(cui_ctx *ui, app_state_t *s) {
    if (update_phase() != UPD_READY || update_dismissed()) return;
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_END, .justify = CUI_JUSTIFY_END, .pad = 14) {
        cui_box(ui, .dir = CUI_ROW, .gap = 10, .pad_x = 14, .pad_y = 8, .radius = 14, .bg = CUI_RGBA(0x180726f2), .border = 1,
                .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER) {
            char t[96]; snprintf(t, sizeof t, "Briks %s pronto para instalar.", update_version());
            cui_text(ui, t, .size = 12);
            if (cui_button(ui, "Reiniciar e instalar", .variant = CUI_BUTTON_PRIMARY)) { if (update_install_now()) cui_app_quit(s->app); }
            if (cui_button(ui, "Depois", .variant = CUI_BUTTON_SECONDARY)) update_dismiss();
        }
    }
}

static void paint_indef(cui_ctx *ui, cui_rect r, void *user);
static bool g_dlg;
void update_dialog_open(void) { g_dlg = true; }
void update_dialog(cui_ctx *ui, app_state_t *s) {
    { static int dev = -1; if (dev < 0) dev = getenv("BRIKS_DEV_UPDDLG") != NULL; if (dev == 1) { dev = 2; g_dlg = true; } }   /* captura: abre o diálogo sozinho */
    if (!g_dlg) return;
    upd_phase_t ph = update_phase(); const upd_check_t ck = update_check_state();
    static int fake = -1; if (fake < 0) fake = getenv("BRIKS_DEV_UPDFAKE") != NULL;   /* captura/vídeo: simula um download subindo de 0 a 100% */
    int fake_pct = 0; const bool fake_ready = fake && getenv("BRIKS_DEV_UPDFAKE")[0] == 'r';   /* BRIKS_DEV_UPDFAKE=ready simula "pronto para instalar" */
    if (fake_ready) ph = UPD_READY; else if (fake) { ph = UPD_DOWNLOADING; { static unsigned fr; fake_pct = (int)((fr++ * 12u / 10u) % 100u); } }   /* por quadro (não por relógio): o vídeo sai suave */
    cui_vec2 vp = cui_viewport_size(ui);
    cui_dialog(ui, "Atualização do Briks", &g_dlg, .width = fminf(420.0f, fmaxf(vp.x - 48.0f, 260.0f))) {
        char t[160];
        if (ph == UPD_READY) {
            snprintf(t, sizeof t, "O Briks %s já foi baixado e verificado. Reinicie para instalar (a chamada em andamento será encerrada).", fake_ready ? "1.5.7" : update_version());
            cui_text(ui, t, .size = 13, .color = B_TEXT, .width = CUI_FILL);
            cui_text(ui, "Se preferir, ele é instalado sozinho quando você fechar o Briks.", .size = 12, .color = B_MUTED, .width = CUI_FILL);
            cui_dialog_footer(ui) {
                if (cui_button(ui, "Depois", .variant = CUI_BUTTON_SECONDARY)) g_dlg = false;
                if (cui_button(ui, "Reiniciar e instalar", .variant = CUI_BUTTON_PRIMARY)) { if (update_install_now()) cui_app_quit(s->app); }
            }
        } else if (ph == UPD_DOWNLOADING || (ph == UPD_CHECKING && ck == UCHK_NEWER)) {
            const int pc = fake ? fake_pct : update_percent(); const bool pct = ph == UPD_DOWNLOADING && pc > 0;
            if (pct) snprintf(t, sizeof t, "Baixando o Briks %s: %d%%", fake ? "1.5.7" : update_version(), pc);
            else snprintf(t, sizeof t, "Nova versão %s: preparando o download…", ph == UPD_DOWNLOADING ? update_version() : update_check_version());
            cui_text(ui, t, .size = 13, .color = B_TEXT, .width = CUI_FILL);
            if (brick_on()) brick_progress(ui, 0, pct ? (float)pc / 100.0f : 0.0f, !pct);
            else if (pct) cui_progress(ui, (float)pc / 100.0f);
            else cui_canvas(ui, "upd-dlg-indef", CUI_FILL, CUI_PX(6), paint_indef, NULL, 0);
            cui_text(ui, "Quando terminar, o botão pede para reiniciar e instalar. Você pode continuar usando o Briks.", .size = 12, .color = B_MUTED, .width = CUI_FILL);
            cui_request_wakeup(ui, pct ? 0.3f : 0.03f);
            cui_dialog_footer(ui) { if (cui_button(ui, "Fechar", .variant = CUI_BUTTON_SECONDARY)) g_dlg = false; }
        } else {
            if (ck == UCHK_NEWER) snprintf(t, sizeof t, "Nova versão %s disponível. %s", update_check_version(), update_auto_on() ? "O download começa em segundo plano." : "A atualização automática está desligada nesta cópia: instale pela página do Briks.");
            else if (ck == UCHK_RUNNING) snprintf(t, sizeof t, "Verificando…");
            else if (ck == UCHK_LATEST) snprintf(t, sizeof t, "Você está na última versão (%s).", update_current_version());
            else snprintf(t, sizeof t, "%s", ph == UPD_ERROR ? "Não foi possível baixar agora; o Briks tenta de novo mais tarde." : "Nenhuma atualização pendente.");
            cui_text(ui, t, .size = 13, .color = B_TEXT, .width = CUI_FILL);
            if (ck == UCHK_RUNNING) cui_request_wakeup(ui, 0.2f);
            cui_dialog_footer(ui) {
                if (cui_button(ui, "Fechar", .variant = CUI_BUTTON_SECONDARY)) g_dlg = false;
                if (cui_button(ui, ck == UCHK_RUNNING ? "Verificando…" : "Verificar agora", .variant = CUI_BUTTON_PRIMARY, .loading = ck == UCHK_RUNNING)) update_check_now();
            }
        }
    }
}

typedef struct { const char *version; const char *items[14]; } notes_t;
static const notes_t NOTES[] = {
    { "1.5.21", {
        "Overlay: a barra de edição ganhou a caixa Atravessar cliques, a mesma opção das Configurações (fora da edição; no modo de edição o overlay sempre recebe o mouse).",
        NULL } },
    { "1.5.20", {
        "Transmissão: a taxa de quadros escolhida não cai mais; quando falta rede ou PC, cai só a resolução e o bitrate. Envio em cadência fixa e adaptação sem vaivém.",
        "Aviso \"começou a transmitir\": o botão Assistir agora responde ao clique.",
        "Transmissões: nome, qualidade e volume só aparecem quando o mouse está em cima.",
        "Ao entrar numa chamada que já tem gente, o Briks escolhe sozinho entre servidor (SFU) e P2P.",
        "Chamada em servidor: reconexão mais calma e reaproveitamento do acesso, sem estourar o limite.",
        "Overlay: arraste as transmissões direto, sem modo de edição; nova opção Atravessar cliques (ligada); imagem mais nítida e fluida.",
        "Overlay: a opção Transmissão só aparece quando alguém está transmitindo.",
        "Volume: silenciar a voz não cala a transmissão, e a transmissão só toca depois de você assistir.",
        NULL } },
    { "1.5.19", {
        "Chamada em servidor (SFU): corrigida a reconexão infinita; o servidor passou a exigir outro papel no handshake de segurança e o Briks agora responde do jeito certo.",
        "A confirmação de idade agora é gravada: o diálogo aparece uma vez por conta, não a cada entrada.",
        NULL } },
    { "1.5.18", {
        "Data de nascimento: a barra (dd/mm/aaaa) entra sozinha enquanto você digita.",
        "Registro da rede da chamada em servidor com mais detalhes (estado das conexões) para achar a instabilidade.",
        NULL } },
    { "1.5.17", {
        "Entrada só com Google e Discord: a criação de conta por e-mail saiu da tela (quem já tem conta continua entrando por e-mail).",
        "Idade: com a leitura pelo Google ligada, contas sem data de nascimento com ano precisam preenchê-la no Google para entrar.",
        NULL } },
    { "1.5.16", {
        "Qualquer pessoa na chamada pode expulsar e permitir de volta (P2P e servidor).",
        "Confirmação de idade: o Briks é só para maiores de 18 anos; contas que entram por Google ou Discord confirmam uma vez.",
        "AO VIVO agora é um vidro vermelho translúcido e reflexivo; o menu de clique direito abre onde você clicou.",
        "Registro da rede mais detalhado (rede.log) para achar a causa do \"Reconectando à chamada\".",
        NULL } },
    { "1.5.15", {
        "Transmissão: o fundo desfocado atualiza a 8 quadros/s (4 no modo Simplificado, estático no Mínimo); as ondas rodam a 15 quadros/s e só no modo Normal.",
        "Corrigido um anel preto que aparecia em volta das ondas.",
        "Modos de desempenho: o Lego da transmissão usa menos desenhos por peça.",
        NULL } },
    { "1.5.14", {
        "Transmissão: quem transmite ganha borda vermelha, selo AO VIVO e ondas em volta da foto (também no seu bloco).",
        "O fundo desfocado da transmissão agora é ao vivo, a 10 quadros por segundo (5 nos modos de desempenho).",
        NULL } },
    { "1.5.13", {
        "Transmissão desfocada: a imagem não fica mais esticada, mantém a proporção da tela.",
        "Tijolos viraram peças de Lego de vários tamanhos, com as cores acompanhando a imagem da transmissão (também na sua tela).",
        NULL } },
    { "1.5.12", {
        "Chamada: novo botão de configurações no cabeçalho da chamada.",
        "Tijolos da transmissão menores, e agora também aparecem na sua própria tela.",
        "Rede: as quedas de conexão com o servidor de chamadas passam a ser registradas em rede.log, para descobrirmos a causa.",
        "Desinstalador: não trava mais a atualização com um aviso.",
        NULL } },
    { "1.5.11", {
        "Atualização: o instalador agora é baixado numa pasta local, com nome neutro e informações de versão completas, para o antivírus não bloquear.",
        NULL } },
    { "1.5.10", {
        "Sons: os sons do grupo voltam a tocar quando você entra na chamada logo ao abrir o app (o dono da sala era descoberto antes dos grupos carregarem).",
        NULL } },
    { "1.5.9", {
        "Transmissão: a miniatura desfocada de tijolos também aparece no seu bloco \"Você\" enquanto você transmite.",
        "Transmissão: o olhinho de espectadores agora aparece para todos na chamada, no ponto a ponto e no servidor (SFU).",
        "Sons: o seletor de emojis do editor \"Adicionar som\" agora abre na frente do diálogo.",
        "Sons: corrigido o armazenamento que podia nascer vazio ou temporário; sons ausentes mostram \"esse som não está neste aparelho\".",
        NULL } },
    { "1.5.8", {
        "Visual de tijolos: a barra de download da atualização é feita de tijolos que se enchem conforme o progresso.",
        "Transmissão: a miniatura desfocada de quem transmite ganha um mosaico de parede de tijolos que guia o desfoque.",
        "Amigos: o estado vazio da lista de amigos ganhou uma ilustração de tijolos.",
        NULL } },
    { "1.5.7", {
        "Transmissão: o seletor de codec (H.264, VP9 ou VP8) agora vale também no servidor de chamadas (SFU); antes só funcionava no ponto a ponto. O codec de cada tela é definido na primeira transmissão da chamada.",
        "Atualização: o botão de download ao lado do minimizar abre uma janela que mostra o progresso do download e, quando termina, oferece Reiniciar e instalar.",
        "Grupos: a seta ao lado do nome do grupo agora também abre o grupo na primeira vez (antes só o clique no nome abria).",
        NULL } },
    { "1.5.6", {
        "Transmissão: a miniatura de quem transmite, no modo Call, agora fica realmente desfocada (antes aparecia nítida).",
        "Transmissão: o som de uma transmissão só sai quando você está na aba Transmissão; na miniatura, na thumb e no overlay ela fica muda.",
        "Amigos online: quem fecha o Briks some da lista na hora, e a lista de amigos online é atualizada a cada 15 segundos.",
        NULL } },
    { "1.5.5", {
        "Transmissão: corrigido o caso em que a transmissão de um amigo só aparecia depois de ele parar e retomar. Quando o aviso de que ele transmite chega antes de o Briks terminar de registrá-lo na chamada, o aviso agora fica guardado e vale assim que ele aparece.",
        NULL } },
    { "1.5.4", {
        "Chamada: quem transmite aparece com uma miniatura estática e desfocada da tela preenchendo o bloco, com a foto de perfil no centro e o ícone de transmissão na frente.",
        "Volume e mudo separados: um controle para a voz da pessoa e outro para o som da transmissão, no menu do botão direito do bloco da chamada.",
        "Transmissão: o botão direito em cima da imagem abre o mesmo menu, com os dois controles de volume.",
        NULL } },
    { "1.5.3", {
        "Configurações lembradas: o microfone e o ensurdecer entram na próxima chamada (e na próxima abertura do Briks) como você os deixou.",
        "Transmissão no servidor de chamadas: a barra de volume da transmissão do amigo volta a aparecer mesmo quando o áudio da tela chega depois do vídeo.",
        "Painel de sons: se o serviço de chamadas não responder ao clique em até 1,5 s, o Briks avisa e toca o som neste aparelho em vez de ficar mudo.",
        NULL } },
    { "1.5.2", {
        "Atualização: ao clicar em Verificar, o download começa na hora (antes só começava no ciclo de 4 horas) e uma barra animada aparece até a barra de progresso começar a encher.",
        "Painel de sons: o editor de novo som ganhou botão de play junto da onda, toca só o trecho entre as barras (que você arrasta), para ao clicar de novo e muda o volume em tempo real.",
        NULL } },
    { "1.5.1", {
        "Transmissão: o olhinho mostra quem está assistindo, com nomes, no servidor e no ponto a ponto; quadros e kbps reais para quem assiste; o PC não suspende enquanto você assiste.",
        "Sobreposição: 30 fps e selo Ao vivo com tamanho fixo e alinhado.",
        NULL } },
    { "1.4.4", {
        "Chamadas: o servidor de chamadas (SFU) do Briks vem configurado no instalador; no modo Automático o app usa o servidor quando ele existe e cai para ponto a ponto quando não.",
        "Transmissão: se o codificador de vídeo falhar, a transmissão se recupera sozinha (com aviso discreto) em vez de cair; o aviso de PC sem conta só aparece com sinal real de máquina lenta.",
        "Transmissão: escolha de codec (H.264, VP9 ou VP8) nas chamadas ponto a ponto, com queda automática para H.264 se alguém não decodificar; no servidor de chamadas segue H.264.",
        "Transmissão: clique no bloco abre em Foco, painel de qualidade que se adapta à janela, seletor de emoji com busca e participantes com avatar de quem transmite.",
        "Mensagens: convite de grupo pela conversa direta vira um cartão Entrar/Recusar; mensagens esperam na fila deste aparelho até o amigo voltar.",
        NULL } },
    { "1.4.3", {
        "Chamada: volume e silenciar para cada pessoa (menu do bloco), lembrados entre chamadas; desconexão automática por inatividade com aviso antes, configurável em Configurações (Nunca, 15, 30 min, 1 h ou 2 h).",
        "Transmissão: escolha de qual app capturar o som (por exemplo, o Spotify), trocando ao vivo sem parar a imagem; as duas telas ao mesmo tempo seguem funcionando.",
        "Perfil: status personalizado (texto, emoji e validade) visível para seus amigos.",
        "Chat: vídeos e áudios anexados tocam dentro da conversa; arquivos recebidos só são abertos depois do clique em tocar.",
        NULL } },
    { "1.4.2", {
        "Amigos: pedidos e amizades aparecem na hora para a outra pessoa (aviso sem dados pela caixa de chegada), com a consulta a cada 30 s como reserva.",
        "Chat: imagens anexadas (PNG, JPG, BMP) aparecem com pré-visualização dentro da conversa.",
        "Transmissão: quem assiste vê o texto mais nítido quando a tela aparece pequena na grade (redução do quadro por média, sem borrar).",
        "Grupos: a lateral lembra quais grupos você recolheu depois de fechar o Briks; tentativas erradas do código curto agora contam no limite por hora.",
        "Atualização do instalador e do fluxo de assinatura de código (SignPath) integrados ao projeto.",
        NULL } },
    { "1.4.1", {
        "Mensagens: conversa direta sem precisar estar na mesma sala de voz, com o estado de cada mensagem (só neste PC, enviada, entregue); caixa de texto sempre à mão e troca de conversa confiável.",
        "Amigos e grupos: indicador de pedidos pendentes, convites visíveis com o app aberto, atualização ao voltar para a janela, painel Amigos que se adapta a larguras estreitas.",
        "Grupos: código curto de convite (BRK-XXXX-XXXX) além do convite longo, e lateral reorganizada com TEXTO e VOZ, canal ativo em destaque e não lidas por canal.",
        "Transmissão: qualidade separada para cada tela, botão Com áudio/Sem áudio ao vivo, clique no espaço vazio alterna grade e foco, e a imagem some quando quem transmite sai.",
        "Chamada: botão Menu para voltar sem sair da chamada, Ensurdecer verde/vermelho e opção de ocultar o aviso de que você está transmitindo.",
        "Ao fechar o Briks, o serviço de chamadas e os auxiliares são encerrados por completo, e o instalador encerra os do próprio diretório.",
        NULL } },
    { "1.4.0", {
        "Conta e amigos: entrar com Google, Discord ou e-mail (com @nick e data de nascimento); foto, nome e @nick; abas Todos, Online, Pendentes, Bloqueados, Adicionar e Atividade.",
        "Grupos: canais de texto e salas de voz, cargos, membros, convite por código, personalizar, sair e apagar.",
        "Mensagens: reações, resposta, edição, exclusão, fixadas, anexos, prévia de link, texto formatado, busca (Ctrl+F) e troca rápida (Ctrl+K).",
        "Chamada: grade e foco, tela cheia (F11), chat da chamada, pedir transmissão, volume por pessoa e histórico de chamadas.",
        "Voz: escolha de microfone e saída, teste de microfone, porta de ruído, cancelamento de eco, atalhos globais e apertar para falar.",
        "Sobreposição em jogos que se arrasta e se redimensiona (Ctrl+Shift+O edita a posição em qualquer tela).",
        "Barra de título própria com minimizar, maximizar e fechar, e botão Verificar agora em Atualizações.",
        "Teste de rede com STUN e relay, TURN da sua conta ou o seu próprio, e detecção de ZeroTier, Tailscale e WireGuard.",
        "Câmera na chamada (beta), com prévia em Configurações, e avisos quando o microfone está negado, ausente ou em uso.",
        "Transmissão de tela (beta), com segunda tela e zoom, e servidor de chamadas SFU (beta).",
        NULL } },
    { "1.3.0", { "Primeira versão nativa do Briks: login, amigos, chat e chamadas ponto a ponto.", NULL } },
};
static const notes_t *notes_for(const char *v) {
    for (size_t i = 0; i < sizeof NOTES / sizeof *NOTES; i++) if (!strcmp(NOTES[i].version, v)) return &NOTES[i];
    return &NOTES[0];   /* versão sem texto próprio (compilação de desenvolvimento): mostra a mais nova */
}
static int notes_count(const char *v) { int n = 0; const notes_t *t = notes_for(v); while (t->items[n]) n++; return n; }

static void card_begin(cui_ctx *ui, const char *title) {
    cui_box_begin(ui, CUI__OPTS(cui_box, .width = CUI_FILL, .pad = 14, .gap = 10, .radius = 16, .bg = B_GLASS_CARD, .border = 1, .border_color = B_GLASS_BORDER));
    cui_text(ui, title, .font = CUI_FONT_MEDIUM, .size = 13, .color = B_C3);
}

static void paint_indef(cui_ctx *ui, cui_rect r, void *user) {
    (void)user;
    cui_draw_rect(ui, r, CUI_CORNERS(r.h * 0.5f), CUI_RGBA(0xffffff22));
    float seg = r.w * 0.3f, t = (float)(GetTickCount64() % 1400) / 1400.0f;
    float x = r.x - seg + t * (r.w + seg), x0 = x < r.x ? r.x : x, x1 = x + seg > r.x + r.w ? r.x + r.w : x + seg;
    if (x1 > x0) cui_draw_rect(ui, (cui_rect){ x0, r.y, x1 - x0, r.h }, CUI_CORNERS(r.h * 0.5f), CUI_RGB(0xa78bfa));
}

void update_settings_cards(cui_ctx *ui, app_state_t *s) {
    card_begin(ui, "ATUALIZAÇÕES");
    cui_textf(ui, "Versão %s", update_current_version());
    upd_phase_t ph = update_phase(); upd_check_t ck = update_check_state();
    if (ph == UPD_READY) {
        char t[120]; snprintf(t, sizeof t, "Briks %s pronto para instalar. Ele é instalado ao fechar o Briks.", update_version());
        cui_text(ui, t, .size = 12, .color = B_ONLINE, .width = CUI_FILL);
        if (cui_button(ui, "Reiniciar e instalar", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM)) { if (update_install_now()) cui_app_quit(s->app); }
    } else if (ph == UPD_DOWNLOADING || (ph == UPD_CHECKING && ck == UCHK_NEWER)) {
        char t[120];
        if (ph == UPD_DOWNLOADING && update_percent() > 0) snprintf(t, sizeof t, "Baixando a versão %s: %d%%", update_version(), update_percent());
        else snprintf(t, sizeof t, "Nova versão %s: preparando o download…", ph == UPD_DOWNLOADING ? update_version() : update_check_version());
        cui_text(ui, t, .size = 12, .color = B_MUTED, .width = CUI_FILL);
        if (brick_on()) brick_progress(ui, 1, ph == UPD_DOWNLOADING && update_percent() > 0 ? (float)update_percent() / 100.0f : 0.0f, !(ph == UPD_DOWNLOADING && update_percent() > 0));
        else if (ph == UPD_DOWNLOADING && update_percent() > 0) cui_progress(ui, (float)update_percent() / 100.0f);
        else cui_canvas(ui, "upd-indef", CUI_FILL, CUI_PX(6), paint_indef, NULL, 0);   /* barra infinita até chegar o 1º byte */
        cui_request_wakeup(ui, ph == UPD_DOWNLOADING && update_percent() > 0 ? 0.3f : 0.03f);
    } else {
        cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER, .width = CUI_FILL, .wrap = true) {
            if (cui_button(ui, ck == UCHK_RUNNING ? "Verificando…" : "Verificar atualização agora", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM, .loading = ck == UCHK_RUNNING)) update_check_now();
        }
        if (ck == UCHK_RUNNING) cui_request_wakeup(ui, 0.2f);
        else if (ck == UCHK_LATEST) cui_text(ui, "Você está na última versão.", .size = 12, .color = B_ONLINE, .width = CUI_FILL);
        else if (ck == UCHK_NEWER) {
            char t[240]; snprintf(t, sizeof t, "Nova versão %s disponível. %s", update_check_version(), update_auto_on() ? "O Briks baixa em segundo plano e avisa quando estiver pronta." : "A atualização automática está desligada nesta cópia: instale a nova versão pelo site do Briks.");
            cui_text(ui, t, .size = 12, .color = CUI_RGB(0xe8c46a), .width = CUI_FILL);
        } else if (ck == UCHK_ERROR) cui_text(ui, update_check_error(), .size = 12, .color = B_DANGER, .width = CUI_FILL);
        else cui_text(ui, ph == UPD_ERROR ? "Não foi possível verificar agora; o Briks tenta de novo mais tarde." : "O Briks verifica novas versões sozinho a cada 4 horas quando instalado. Use o botão para conferir agora.", .size = 12, .color = B_MUTED, .width = CUI_FILL);
    }
    cui_box_end(ui);

    const notes_t *n = notes_for(update_current_version());
    char title[64]; snprintf(title, sizeof title, "NOVIDADES DA %s", n->version);
    card_begin(ui, title);
    for (int i = 0; n->items[i]; i++) {
        cui_row(ui, .gap = 8, .width = CUI_FILL) {
            cui_text(ui, "•", .size = 13, .color = B_C3);
            cui_text(ui, n->items[i], .size = 13, .color = B_TEXT, .width = CUI_FILL);
        }
    }
    cui_box_end(ui);
}

int update_probe(const char *n) {
    if (!strcmp(n, "upd_chk")) return (int)update_check_state();   /* 0 nada, 1 verificando, 2 última, 3 nova, 4 erro */
    if (!strcmp(n, "upd_notes")) return notes_count(update_current_version());
    if (!strcmp(n, "upd_phase")) return (int)update_phase();
    return -9999;
}
