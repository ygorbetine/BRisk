/* Editor da conta para o diálogo de Perfil: foto, nome e @nick. Bloco independente (só depende de app_state_t e de
 * account.c) para poder ser encaixado em qualquer layout do Perfil. */
#include "briks_ui.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <windows.h>

void ui_account_editor(cui_ctx *ui, app_state_t *s) {
    /* ao abrir, o campo começa com o nome atual */
    static double last = -10.0;   /* o bloco só roda com o diálogo aberto: uma pausa nos quadros = acabou de abrir */
    const double now = (double)cui_time(ui);
    if (now - last > 0.4) { snprintf(s->name_edit, sizeof s->name_edit, "%s", s->user_name); s->profile_msg[0] = 0; }
    last = now;

    cui_box(ui, .width = CUI_FILL, .pad = 12, .gap = 12, .radius = 16, .bg = B_GLASS_CARD, .border = 1, .border_color = B_GLASS_BORDER) {
        /* foto */
        cui_text(ui, "Foto", .size = 12, .color = B_MUTED);
        cui_row(ui, .gap = 12, .align = CUI_ALIGN_CENTER) {
            cui_avatar_ex(ui, s->user_name, &(cui_avatar_opts){ .size = 56, .image = s->user_photo });
            cui_column(ui, .gap = 6) {
                cui_row(ui, .gap = 8) {
                    if (cui_button(ui, "Trocar foto", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) account_pick_photo(s);
                    if (cui_button(ui, "Usar a foto da conta", .variant = CUI_BUTTON_LINK, .size = CUI_SIZE_SM)) account_reset_photo(s);
                }
                cui_text(ui, "PNG ou JPEG. A imagem é recortada em quadrado.", .size = 11, .color = B_MUTED);
            }
        }
        /* nome */
        cui_text(ui, "Nome", .size = 12, .color = B_MUTED);
        cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_box(ui, .width = CUI_FILL) {
                int r = cui_input(ui, "name-edit", s->name_edit, sizeof s->name_edit, .placeholder = "Como seus amigos veem você");
                if ((r & CUI_SUBMITTED) && strcmp(s->name_edit, s->user_name)) account_set_name(s);
            }
            if (cui_button(ui, "Salvar", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM, .disabled = !s->name_edit[0] || !strcmp(s->name_edit, s->user_name))) account_set_name(s);
        }
        /* @nick */
        cui_text(ui, "@nick", .size = 12, .color = B_MUTED);
        cui_row(ui, .gap = 10, .align = CUI_ALIGN_CENTER) {
            cui_box(ui, .width = CUI_FILL) { cui_textf(ui, "@%s", s->user_nick[0] ? s->user_nick : "…"); }
            if (cui_button(ui, "Trocar @nick", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) {
                s->profile_open = false; s->nick_open = true; s->nick_msg[0] = 0;
                snprintf(s->nick_edit, sizeof s->nick_edit, "%s", s->acct_nick);
            }
        }
        if (s->profile_msg[0]) cui_text(ui, s->profile_msg, .size = 12, .color = CUI_RGB(0xd9cdea));
    }
    cui_text(ui, "Seus amigos acham você pelo @nick e veem este nome e esta foto. Mensagens e grupos ficam guardados neste computador.", .size = 12, .color = B_MUTED);
}

/* ---------- segurança: sair dos outros computadores ---------- */

/* Computadores ligados ao @nick (PRF-12): lista as instalações de profile_keys e revoga uma (o livekit-token passa a recusar o peerId).
 * Só aparece com a migração 0009 no servidor (dev_remote); sem ela o cartão some e nada quebra. */
static void ui_devices_card(cui_ctx *ui, app_state_t *s) {
    const unsigned long long now = GetTickCount64();
    if (!s->dev_asked_at || now - s->dev_asked_at > 60000) account_devices_get(s);   /* atualiza ao abrir e a cada minuto */
    if (!s->dev_have || !s->dev_remote) return;
    cui_box(ui, .width = CUI_FILL, .pad = 12, .gap = 8, .radius = 16, .bg = B_GLASS_CARD, .border = 1, .border_color = B_GLASS_BORDER) {
        cui_text(ui, "Computadores ligados ao seu @nick", .size = 12, .color = B_MUTED);
        if (!s->dev_n) cui_text(ui, "Nenhum computador registrado ainda.", .size = 12, .color = B_MUTED);
        for (int i = 0; i < s->dev_n; i++) {
            const bool cur = !strcmp(s->devs[i].peer, s->dev_current), asking = !cur && !strcmp(s->devs[i].peer, s->dev_confirm);
            cui_push_id_int(ui, 7300 + i);
            cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                cui_icon(ui, CUI_ICON_MONITOR, 16, cur ? B_ONLINE : B_MUTED);
                cui_column(ui, .gap = 1, .width = CUI_FILL) {
                    char t[120]; snprintf(t, sizeof t, "%s%s", s->devs[i].name[0] ? s->devs[i].name : "Computador sem nome", cur ? " (este)" : "");
                    cui_text(ui, t, .size = 13, .nowrap = true);
                    char d[80]; snprintf(d, sizeof d, s->devs[i].used[0] ? "Último uso em %s" : "Sem uso registrado", s->devs[i].used);
                    cui_text(ui, d, .size = 12, .color = B_MUTED, .nowrap = true);
                }
                if (!cur && !asking) { if (cui_button(ui, "Revogar", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) { snprintf(s->dev_confirm, sizeof s->dev_confirm, "%s", s->devs[i].peer); s->dev_msg[0] = 0; } }
                if (!cur && asking) {
                    if (cui_button(ui, "Confirmar", .variant = CUI_BUTTON_DANGER, .size = CUI_SIZE_SM)) { char id[48]; snprintf(id, sizeof id, "%s", s->devs[i].peer); s->dev_confirm[0] = 0; account_device_revoke(s, id); }
                    if (cui_button(ui, "Cancelar", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM)) s->dev_confirm[0] = 0;
                }
            }
            cui_pop_id(ui);
        }
        cui_text(ui, "Revogar tira o computador da conta: ele perde o acesso às salas de chamada. Para derrubar também o login, use Sair dos outros computadores.", .size = 12, .color = B_MUTED, .width = CUI_FILL);
        if (s->dev_msg[0]) cui_text(ui, s->dev_msg, .size = 12, .color = CUI_RGB(0xd9cdea), .width = CUI_FILL);
    }
}

void ui_others_section(cui_ctx *ui, app_state_t *s) {
    ui_devices_card(ui, s);
    /* segurança: derruba a conta nos outros computadores (perdeu um aparelho, ou esqueceu logado) */
    cui_box(ui, .width = CUI_FILL, .pad = 12, .gap = 8, .radius = 16, .bg = B_GLASS_CARD, .border = 1, .border_color = B_GLASS_BORDER) {
        cui_text(ui, "Outros computadores", .size = 12, .color = B_MUTED);
        cui_text(ui, "Se você esqueceu a conta aberta em outro computador, ou perdeu um aparelho, desconecte-a de todos os outros. Este continua conectado.", .size = 12, .width = CUI_FILL);
        if (!s->others_confirm) {
            if (cui_button(ui, "Sair dos outros computadores", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM, .loading = s->others_busy)) { s->others_confirm = true; s->others_msg[0] = 0; }
        } else {
            cui_row(ui, .gap = 8) {
                if (cui_button(ui, "Confirmar", .variant = CUI_BUTTON_DANGER, .size = CUI_SIZE_SM, .loading = s->others_busy)) account_logout_others(s);
                if (cui_button(ui, "Cancelar", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM)) s->others_confirm = false;
            }
        }
        if (s->others_msg[0]) cui_text(ui, s->others_msg, .size = 12, .color = CUI_RGB(0xd9cdea), .width = CUI_FILL);
    }
}

/* ---------- cópia cifrada da identidade, recuperável por frase ---------- */

void ui_backup_section(cui_ctx *ui, app_state_t *s) {
    cui_text(ui, "Cópia de segurança da identidade", .font = CUI_FONT_MEDIUM);
    cui_box(ui, .width = CUI_FILL, .pad = 12, .gap = 10, .radius = 16, .bg = B_GLASS_CARD, .border = 1, .border_color = B_GLASS_BORDER) {
        if (s->backup_known == BKP_UNKNOWN) cui_text(ui, "Consultando…", .size = 13, .color = B_MUTED);
        else if (s->backup_known == BKP_NONE) {
            cui_text(ui, "Sem cópia. Se este computador for formatado ou trocado, sua identidade se perde e seus amigos passam a ver uma chave nova.", .size = 13, .color = B_MUTED, .width = CUI_FILL);
            if (cui_button(ui, "Criar cópia de segurança", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM)) account_backup_create(s);
        } else if (s->backup_known == BKP_OK) {
            char t[120]; snprintf(t, sizeof t, s->backup_date[0] ? "Cópia guardada em %s. Só a sua frase consegue abri-la." : "Cópia guardada. Só a sua frase consegue abri-la.", s->backup_date);
            cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER) {
                cui_box(ui, .width = CUI_PX(8), .height = CUI_PX(8), .radius = 4, .bg = B_ONLINE) {}
                cui_text(ui, t, .size = 13);
            }
            cui_row(ui, .gap = 8) {
                if (cui_button(ui, "Trocar frase", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) account_backup_create(s);
                if (cui_button(ui, "Apagar cópia", .variant = CUI_BUTTON_LINK, .size = CUI_SIZE_SM)) s->backup_delete_open = true;
            }
        } else {
            cui_text(ui, "Esta conta tem uma cópia de outra identidade (de outro computador). Recupere-a aqui para seus amigos reconhecerem você.", .size = 13, .color = B_MUTED, .width = CUI_FILL);
            cui_row(ui, .gap = 8) {
                if (cui_button(ui, "Recuperar neste computador", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM)) { s->backup_restore_open = true; s->backup_msg[0] = 0; }
                if (cui_button(ui, "Apagar cópia", .variant = CUI_BUTTON_LINK, .size = CUI_SIZE_SM)) s->backup_delete_open = true;
            }
        }
        if (s->backup_msg[0] && !s->backup_restore_open) cui_text(ui, s->backup_msg, .size = 12, .color = CUI_RGB(0xd9cdea), .width = CUI_FILL);
    }
    cui_text(ui, "A chave é cifrada neste computador com uma frase de 12 palavras. O servidor guarda só o pacote cifrado: sem a frase, ninguém abre, nem nós.", .size = 12, .color = B_MUTED);
}

void ui_backup_dialogs(cui_ctx *ui, app_state_t *s) {
    cui_vec2 vp = cui_viewport_size(ui);
    /* fechar o diálogo da frase sem confirmar descarta a frase (nada foi enviado) */
    static bool phrase_was_open;
    if (phrase_was_open && !s->backup_phrase_open && s->backup_words[0][0]) account_backup_cancel(s);
    phrase_was_open = s->backup_phrase_open;

    cui_dialog(ui, "Sua frase de recuperação", &s->backup_phrase_open, .width = fminf(520.0f, fmaxf(vp.x - 48.0f, 280.0f))) {
        cui_text(ui, "Anote estas 12 palavras, na ordem, em papel ou num gerenciador de senhas. Ela aparece só agora.", .color = B_MUTED, .width = CUI_FILL);
        for (int row = 0; row < 4; row++) {
            cui_row(ui, .gap = 8, .width = CUI_FILL) {
                for (int col = 0; col < 3; col++) {
                    const int i = row * 3 + col;
                    cui_push_id_int(ui, 4100 + i);
                    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 12, .pad_y = 9, .gap = 8, .radius = 12, .bg = B_GLASS_CARD, .border = 1, .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER) {
                        char n[8]; snprintf(n, sizeof n, "%d", i + 1);
                        cui_text(ui, n, .size = 11, .color = B_MUTED);
                        cui_text(ui, s->backup_words[i], .font = CUI_FONT_MEDIUM, .nowrap = true);
                    }
                    cui_pop_id(ui);
                }
            }
        }
        cui_box(ui, .width = CUI_FILL, .pad_x = 12, .pad_y = 9, .radius = 12, .bg = CUI_RGBA(0xe8c46a1c), .border = 1, .border_color = CUI_RGBA(0xe8c46a55)) {
            cui_text(ui, "Quem tiver a frase e a sua conta consegue se passar por você. Se você perder a frase, a cópia não pode ser aberta por ninguém.", .size = 12, .color = CUI_RGB(0xf0d79a), .width = CUI_FILL);
        }
        cui_switch(ui, "Anotei a frase em um lugar seguro", &s->backup_ack);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Concluir e guardar a cópia", .variant = CUI_BUTTON_PRIMARY, .disabled = !s->backup_ack, .loading = s->backup_busy)) account_backup_confirm(s);
        }
    }

    cui_dialog(ui, "Recuperar identidade", &s->backup_restore_open, .width = fminf(480.0f, fmaxf(vp.x - 48.0f, 280.0f))) {
        cui_text(ui, "Digite as 12 palavras da sua frase de recuperação, separadas por espaço.", .color = B_MUTED, .width = CUI_FILL);
        int r = cui_input(ui, "backup-phrase", s->backup_input, sizeof s->backup_input, .placeholder = "palavra1 palavra2 … palavra12", .autofocus = true);
        cui_text(ui, "A identidade atual deste computador é substituída pela recuperada (a atual fica guardada como reserva neste computador).", .size = 12, .color = B_MUTED, .width = CUI_FILL);
        if (s->backup_msg[0]) cui_text(ui, s->backup_msg, .size = 13, .color = CUI_RGB(0xff9a9a), .width = CUI_FILL);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Recuperar", .variant = CUI_BUTTON_PRIMARY, .disabled = !s->backup_input[0], .loading = s->backup_busy) || ((r & CUI_SUBMITTED) && s->backup_input[0])) account_backup_restore(s);
        }
    }
    if (!s->backup_restore_open && s->backup_input[0]) memset(s->backup_input, 0, sizeof s->backup_input);   /* fechou: a frase digitada não fica na memória */

    cui_dialog(ui, "Apagar cópia de segurança", &s->backup_delete_open, .width = fminf(420.0f, fmaxf(vp.x - 48.0f, 280.0f))) {
        cui_text(ui, "A cópia cifrada é apagada do servidor. A identidade deste computador continua funcionando, mas não poderá mais ser recuperada em outro.", .color = B_MUTED, .width = CUI_FILL);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Apagar cópia", .variant = CUI_BUTTON_DANGER, .loading = s->backup_busy)) account_backup_delete(s);
        }
    }
}
