/* Tela de entrada: conta online (account.c). Google e Discord em destaque; e-mail e senha como alternativa discreta.
 * Tudo em vidro com cantos arredondados; nenhum retângulo reto. */
#include "briks_ui.h"
#include <stdio.h>
#include <string.h>

#include <windows.h>

#define ERR_COLOR CUI_RGB(0xff9a9a)

/* Logos dos provedores: assets\login\<nome>.png ao lado do executável. Se o arquivo faltar, o botão usa a letra. */
static cui_texture logo(cui_ctx *ui, int which) {
    static cui_texture tex[2]; static bool tried[2];
    if (!tried[which]) {
        tried[which] = true;
        char dir[MAX_PATH], path[MAX_PATH + 40]; int w = 0, h = 0;
        GetModuleFileNameA(NULL, dir, sizeof dir);
        char *slash = strrchr(dir, '\\'); if (slash) *slash = 0;
        snprintf(path, sizeof path, "%s\\assets\\login\\%s.png", dir, which ? "discord" : "google");
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) tex[which] = cui_image_load(ui, path, &w, &h);
    }
    return tex[which];
}

/* Botão grande de provedor: vidro arredondado com o selo da marca. Devolve true no clique. */
static bool provider_button(cui_ctx *ui, const char *id, const char *label, const char *letter, cui_texture mark, cui_color badge, cui_color badge_fg, cui_color tint, bool enabled) {
    cui_push_id(ui, id);
    cui_rect r = cui_get_rect(ui, cui_make_id(ui, "b")); cui_vec2 m = cui_mouse_pos(ui);
    bool hover = enabled && r.w > 0 && m.x >= r.x && m.y >= r.y && m.x < r.x + r.w && m.y < r.y + r.h;
    cui_color bg = tint; bg.a = (uint8_t)(hover ? 0x5c : 0x3a);
    if (!enabled) bg.a = 0x18;
    cui_box(ui, .id = "b", .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(54), .pad_x = 16, .gap = 12, .radius = 17,
            .bg = bg, .border = 1, .border_color = hover ? CUI_RGBA(0xffffff55) : CUI_RGBA(0xffffff26),
            .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
        cui_box(ui, .width = CUI_PX(30), .height = CUI_PX(30), .radius = 15, .bg = badge, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
            if (mark) cui_image_ex(ui, mark, &(cui_image_opts){ .width = CUI_PX(18), .height = CUI_PX(18) });
            else cui_text(ui, letter, .font = CUI_FONT_DISPLAY, .size = 16, .color = badge_fg);
        }
        cui_text(ui, label, .font = CUI_FONT_MEDIUM, .size = 16, .color = enabled ? CUI_RGB(0xffffff) : B_MUTED, .nowrap = true);
    }
    bool hit = enabled && briks_clicked(ui, "b");
    cui_pop_id(ui);
    return hit;
}

static void messages(cui_ctx *ui, app_state_t *s) {
    if (s->acct_error[0]) {
        cui_box(ui, .width = CUI_FILL, .pad_x = 12, .pad_y = 9, .radius = 12, .bg = CUI_RGBA(0xe9555524), .border = 1, .border_color = CUI_RGBA(0xe9555555)) {
            cui_text(ui, s->acct_error, .color = ERR_COLOR, .size = 13, .width = CUI_FILL);
        }
    } else if (s->acct_info[0]) cui_text(ui, s->acct_info, .color = B_MUTED, .size = 13);
}

static void go(app_state_t *s, int view) {
    s->acct_view = view; s->acct_error[0] = 0; s->acct_info[0] = 0;
    memset(s->password, 0, sizeof s->password); memset(s->confirm, 0, sizeof s->confirm);
}

static void heading(cui_ctx *ui, const char *title, const char *sub) {
    cui_text(ui, title, .font = CUI_FONT_DISPLAY, .size = 30, .line_height = 1.1f);
    if (sub) cui_text(ui, sub, .color = B_MUTED);
    cui_gap(ui, 6);
}

static void view_main(cui_ctx *ui, app_state_t *s) {
    heading(ui, "Entre no Briks", "Conversas que parecem estar na mesma sala.");
    bool on = s->acct_configured;
    if (provider_button(ui, "google", "Continuar com Google", "G", logo(ui, 0), CUI_RGB(0xffffff), CUI_RGB(0x4285f4), CUI_RGB(0xffffff), on)) account_oauth(s, "google");
    if (provider_button(ui, "discord", "Continuar com Discord", "D", logo(ui, 1), CUI_RGB(0x5865f2), CUI_RGB(0xffffff), CUI_RGB(0x5865f2), on)) account_oauth(s, "discord");
    cui_text(ui, "Mais rápido e mais seguro: você entra com uma conta que já tem, sem criar outra senha.", .color = B_MUTED, .size = 13);
    messages(ui, s);
    cui_gap(ui, 4);
    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .gap = 10, .align = CUI_ALIGN_CENTER) {
        cui_box(ui, .width = CUI_FILL, .height = CUI_PX(1), .radius = 1, .bg = CUI_RGBA(0xffffff1c)) {}
        cui_text(ui, "ou", .size = 12, .color = B_MUTED);
        cui_box(ui, .width = CUI_FILL, .height = CUI_PX(1), .radius = 1, .bg = CUI_RGBA(0xffffff1c)) {}
    }
    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .gap = 4, .justify = CUI_JUSTIFY_CENTER) {
        if (cui_button(ui, "Entrar com e-mail", .variant = CUI_BUTTON_LINK, .size = CUI_SIZE_SM, .disabled = !on)) go(s, AV_LOGIN);
        if (cui_button(ui, "Criar conta com e-mail", .variant = CUI_BUTTON_LINK, .size = CUI_SIZE_SM, .disabled = !on)) go(s, AV_SIGNUP);
    }
    if (!on && s->acct_state != ACCT_UNKNOWN && cui_button(ui, "Tentar de novo", .variant = CUI_BUTTON_SECONDARY, .full_width = true)) account_retry(s);
}

static void view_form(cui_ctx *ui, app_state_t *s) {
    const int v = s->acct_view;
    heading(ui, v == AV_SIGNUP ? "Criar conta com e-mail" : v == AV_RECOVER ? "Recuperar senha" : "Entrar com e-mail",
            v == AV_SIGNUP ? "Vamos enviar um e-mail para confirmar que o endereço é seu."
            : v == AV_RECOVER ? "Enviamos um link para você escolher uma senha nova." : NULL);
    int r = 0;
    if (v == AV_SIGNUP) cui_input(ui, "name", s->name, sizeof s->name, .placeholder = "Como devemos chamar você?");
    r |= cui_input(ui, "email", s->email, sizeof s->email, .placeholder = "seu@email.com", .autofocus = v != AV_SIGNUP);
    if (v != AV_RECOVER) r |= cui_input(ui, "password", s->password, sizeof s->password, .password = true, .placeholder = v == AV_SIGNUP ? "Senha (mínimo 8 caracteres)" : "Senha");
    if (v == AV_SIGNUP) r |= cui_input(ui, "confirm", s->confirm, sizeof s->confirm, .password = true, .placeholder = "Repita a senha");
    messages(ui, s);
    if (cui_button(ui, v == AV_SIGNUP ? "Criar conta" : v == AV_RECOVER ? "Enviar e-mail de recuperação" : "Entrar",
                   .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_LG, .full_width = true) || (r & CUI_SUBMITTED)) account_submit(s);
    if (v == AV_LOGIN && cui_button(ui, "Esqueci minha senha", .variant = CUI_BUTTON_LINK, .full_width = true)) go(s, AV_RECOVER);
    if (cui_button(ui, v == AV_RECOVER ? "Voltar para entrar com e-mail" : "Voltar para Google e Discord", .variant = CUI_BUTTON_LINK, .full_width = true)) go(s, v == AV_RECOVER ? AV_LOGIN : AV_MAIN);
}

static void view_busy(cui_ctx *ui, app_state_t *s) {
    if (!strcmp(s->acct_what, "oauth")) {
        const char *p = !strcmp(s->acct_provider, "discord") ? "Discord" : "Google";
        char sub[200]; snprintf(sub, sizeof sub, "Abrimos o login do %s no seu navegador. Conclua por lá e volte: esta tela continua sozinha.", p);
        heading(ui, "Continue no navegador", sub);
        cui_button(ui, "Aguardando o navegador…", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_LG, .full_width = true, .loading = true, .disabled = true);
        if (cui_button(ui, "Cancelar", .variant = CUI_BUTTON_LINK, .full_width = true)) account_cancel(s);
        return;
    }
    heading(ui, !strcmp(s->acct_what, "signup") ? "Criando sua conta…" : !strcmp(s->acct_what, "recover") ? "Enviando o e-mail…" : "Entrando…", NULL);
    cui_button(ui, "Só um instante", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_LG, .full_width = true, .loading = true, .disabled = true);
    if (strcmp(s->acct_what, "restore") && cui_button(ui, "Cancelar", .variant = CUI_BUTTON_LINK, .full_width = true)) account_cancel(s);
}

static void view_email_sent(cui_ctx *ui, app_state_t *s) {
    const bool rec = !strcmp(s->acct_kind, "recovery");
    char sub[320];
    snprintf(sub, sizeof sub, rec ? "Se existir uma conta com %s, enviamos um link para escolher uma senha nova. Abra o link neste computador, com o Briks aberto."
                                  : "Enviamos um link de confirmação para %s. Abra o link neste computador, com o Briks aberto.", s->email);
    heading(ui, "Confira seu e-mail", sub);
    cui_text(ui, "O e-mail trouxe um código? Digite aqui:", .size = 13, .color = B_MUTED);
    int r = cui_input(ui, "code", s->acct_code, sizeof s->acct_code, .placeholder = "Código de 6 dígitos");
    messages(ui, s);
    if (cui_button(ui, "Confirmar código", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_LG, .full_width = true, .disabled = !s->acct_code[0]) || ((r & CUI_SUBMITTED) && s->acct_code[0])) account_submit_code(s);
    cui_text(ui, "Não chegou? Veja a caixa de spam ou volte e peça de novo em um minuto.", .size = 12, .color = B_MUTED);
    if (cui_button(ui, rec ? "Voltar" : "Já confirmei: entrar", .variant = CUI_BUTTON_LINK, .full_width = true)) { account_cancel(s); go(s, AV_LOGIN); }
}

static void view_newpass(cui_ctx *ui, app_state_t *s) {
    heading(ui, "Escolha uma senha nova", "Você entrou pelo link de recuperação.");
    cui_input(ui, "password", s->password, sizeof s->password, .password = true, .placeholder = "Nova senha (mínimo 8 caracteres)", .autofocus = true);
    int r = cui_input(ui, "confirm", s->confirm, sizeof s->confirm, .password = true, .placeholder = "Repita a nova senha");
    messages(ui, s);
    if (cui_button(ui, "Salvar e entrar", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_LG, .full_width = true) || (r & CUI_SUBMITTED)) account_submit(s);
    if (cui_button(ui, "Sair", .variant = CUI_BUTTON_LINK, .full_width = true)) app_logout(s);
}

static void view_entering(cui_ctx *ui, app_state_t *s) {
    if (s->acct_bridge == BRIDGE_FAILED) {
        heading(ui, "Quase lá", "Sua conta entrou, mas não conseguimos abrir os dados guardados neste computador.");
        messages(ui, s);
        if (cui_button(ui, "Tentar de novo", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_LG, .full_width = true)) { s->acct_error[0] = 0; account_retry(s); }
        if (cui_button(ui, "Sair", .variant = CUI_BUTTON_LINK, .full_width = true)) app_logout(s);
        return;
    }
    char t[140]; snprintf(t, sizeof t, "Olá, %s", s->user_name[0] ? s->user_name : "você");
    heading(ui, t, "Preparando seu espaço…");
    cui_button(ui, "Só um instante", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_LG, .full_width = true, .loading = true, .disabled = true);
}

void ui_auth(cui_ctx *ui, app_state_t *s) {
    briks_ambient(ui, 1.0f);   /* primeiro filho do STACK: fundo */
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .pad = 16) {
        cui_box(ui, .width = CUI_FILL, .max_width = 440, .pad = 36, .gap = 12, .radius = 26,
                .bg = B_GLASS_STRONG, .border = 1, .border_color = CUI_RGBA(0x8b5cf633), .elevation = briks_elev(CUI_ELEVATION_FLOATING)) {
            briks_brand(ui, 26);
            cui_gap(ui, 12);
            if (s->acct_state == ACCT_UNKNOWN) {
                heading(ui, "Briks", "Carregando…");
                cui_button(ui, "Só um instante", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_LG, .full_width = true, .loading = true, .disabled = true);
            } else if (s->acct_state == ACCT_BUSY) view_busy(ui, s);
            else if (s->acct_state == ACCT_EMAIL_SENT) view_email_sent(ui, s);
            else if (s->acct_state == ACCT_IN) { if (s->acct_view == AV_NEWPASS) view_newpass(ui, s); else view_entering(ui, s); }
            else if (s->acct_view == AV_MAIN || s->acct_view == AV_NEWPASS) view_main(ui, s);
            else view_form(ui, s);
            if (s->acct_mock) {
                cui_gap(ui, 2);
                cui_text(ui, "Modo de desenvolvimento: contas de mentira, sem internet.", .size = 11, .color = CUI_RGB(0xe8c46a));
            }
        }
    }
}
