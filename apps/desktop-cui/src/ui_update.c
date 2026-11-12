/* Aviso de atualização (vidro, canto inferior direito): aparece quando o instalador novo já foi baixado e verificado.
 * Também: Configurações > Atualizações com "Verificar atualização agora" (ATU-04) e "Novidades da versão em uso" (CFG-29). */
#include "briks_ui.h"
#include "update.h"
#include <stdio.h>
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

typedef struct { const char *version; const char *items[14]; } notes_t;
static const notes_t NOTES[] = {
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

void update_settings_cards(cui_ctx *ui, app_state_t *s) {
    card_begin(ui, "ATUALIZAÇÕES");
    cui_textf(ui, "Versão %s", update_current_version());
    upd_phase_t ph = update_phase(); upd_check_t ck = update_check_state();
    if (ph == UPD_READY) {
        char t[120]; snprintf(t, sizeof t, "Briks %s pronto para instalar. Ele é instalado ao fechar o Briks.", update_version());
        cui_text(ui, t, .size = 12, .color = B_ONLINE, .width = CUI_FILL);
        if (cui_button(ui, "Reiniciar e instalar", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM)) { if (update_install_now()) cui_app_quit(s->app); }
    } else if (ph == UPD_DOWNLOADING) {
        char t[120]; snprintf(t, sizeof t, "Baixando a versão %s: %d%%", update_version(), update_percent());
        cui_text(ui, t, .size = 12, .color = B_MUTED, .width = CUI_FILL);
        cui_progress(ui, (float)update_percent() / 100.0f);
        cui_request_wakeup(ui, 0.3f);
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
