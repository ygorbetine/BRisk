/* Hub de Amigos (paridade com a tela Amigos do Electron): abas Todos, Online, Pendentes, Bloqueados e Adicionar, busca, e ações por linha
 * (mensagem, desfazer amizade, bloquear). Amizades e pedidos vêm do Supabase pelo briks-core (account.c); o bloqueio é uma lista LOCAL gravada em
 * %APPDATA%\Briks-CUI\blocked.json (o servidor ainda não tem bloqueio: supabase/migrations/0006_friend_blocks.sql prepara isso, sem estar aplicada). */
#include "briks_ui.h"
#include "json.h"
#include "core.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <windows.h>
#include <shlobj.h>

/* ---- bloqueados: lista local (cache + fallback) sincronizada com o servidor pelo core quando a 0006 existe ---- */
static bool blocked_path(char *out, size_t cap) {
    const char *over = getenv("BRIKS_BLOCKED_FILE");
    if (over && over[0]) { snprintf(out, cap, "%s", over); return true; }
    char appdata[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) != S_OK) return false;
    char dir[MAX_PATH]; snprintf(dir, sizeof dir, "%s\\Briks-CUI", appdata);
    CreateDirectoryA(dir, NULL);
    snprintf(out, cap, "%s\\blocked.json", dir);
    return true;
}

static void blocked_save(const app_state_t *s) {
    char p[MAX_PATH]; if (!blocked_path(p, sizeof p)) return;
    FILE *f = fopen(p, "wb"); if (!f) return;
    fputs("[", f);
    for (int i = 0; i < s->blocked_count; i++) {
        char id[100], nm[200]; json_escape(s->blocked[i].id, id, sizeof id); json_escape(s->blocked[i].name, nm, sizeof nm);
        fprintf(f, "%s{\"id\":\"%s\",\"name\":\"%s\"}", i ? "," : "", id, nm);
    }
    fputs("]", f); fclose(f);
}

static void blocked_load(app_state_t *s) {
    if (s->blocked_loaded) return;
    s->blocked_loaded = true; s->blocked_count = 0;
    char p[MAX_PATH]; if (!blocked_path(p, sizeof p)) return;
    FILE *f = fopen(p, "rb"); if (!f) return;
    char *buf = malloc(16384); size_t n = buf ? fread(buf, 1, 16383, f) : 0; fclose(f);
    if (!buf) return;
    buf[n] = 0;
    jval *j = json_parse(buf); free(buf);
    for (int i = 0; i < json_len(j) && s->blocked_count < 64; i++) {
        const jval *e = json_at(j, i);
        snprintf(s->blocked[s->blocked_count].id, sizeof s->blocked[0].id, "%s", json_str(e, "id", ""));
        snprintf(s->blocked[s->blocked_count].name, sizeof s->blocked[0].name, "%s", json_str(e, "name", "?"));
        if (s->blocked[s->blocked_count].id[0]) s->blocked_count++;
    }
    json_free(j);
}

bool friends_blocked(app_state_t *s, const char *id) {
    blocked_load(s);
    if (!id || !id[0]) return false;
    for (int i = 0; i < s->blocked_count; i++) if (!strcmp(s->blocked[i].id, id)) return true;
    return false;
}

void friends_block(app_state_t *s, const char *id, const char *name) {
    blocked_load(s);
    if (friends_blocked(s, id) || s->blocked_count >= 64) return;
    snprintf(s->blocked[s->blocked_count].id, sizeof s->blocked[0].id, "%s", id);
    snprintf(s->blocked[s->blocked_count].name, sizeof s->blocked[0].name, "%s", name);
    s->blocked_count++; blocked_save(s);
    if (s->acct_user_id[0]) account_block_remote(s, id, true);
    for (int i = 0; i < s->friend_count; i++) if (!strcmp(s->friends[i].id, id)) {   /* some das listas na hora */
        if (s->active_friend == i) s->active_friend = -1; else if (s->active_friend > i) s->active_friend--;
        memmove(&s->friends[i], &s->friends[i + 1], sizeof(person_t) * (size_t)(s->friend_count - i - 1)); s->friend_count--; break;
    }
    for (int i = 0; i < s->pending_count; i++) if (!strcmp(s->pending[i].id, id)) { memmove(&s->pending[i], &s->pending[i + 1], sizeof(person_t) * (size_t)(s->pending_count - i - 1)); s->pending_count--; break; }
}

void friends_unblock(app_state_t *s, const char *id) {
    blocked_load(s);
    for (int i = 0; i < s->blocked_count; i++) if (!strcmp(s->blocked[i].id, id)) {
        memmove(&s->blocked[i], &s->blocked[i + 1], sizeof s->blocked[0] * (size_t)(s->blocked_count - i - 1)); s->blocked_count--; blocked_save(s); break;
    }
    if (s->acct_user_id[0]) account_block_remote(s, id, false);   /* o core devolve a lista de amigos atualizada */
    else account_friends(s);
}

/* ---- apresentação ---- */
static bool in_call(const app_state_t *s, const person_t *p) {   /* está na sua chamada agora */
    for (int i = 0; i < 8; i++) if (s->rtc_peers[i].id[0] && (!strcmp(s->rtc_peers[i].state, "autenticado") || !strcmp(s->rtc_peers[i].state, "conectado")) && !strcmp(s->rtc_peers[i].name, p->name)) return true;
    return false;
}

/* online = batimento recente no servidor (0007) ou está na sua chamada agora (vale mesmo se o servidor ainda não tem a 0007) */
bool friends_online(const app_state_t *s, const person_t *p) {
    if (in_call(s, p)) return true;
    if (s->pres_remote) for (int i = 0; i < s->pres_n; i++) if (s->pres[i].online && !strcmp(s->pres[i].id, p->id)) return true;
    return false;
}

/* ---- avisos de amigo (FriendNotifyPanel): quem ficou online ou entrou na chamada; a faixa aparece no topo da tela e some sozinha ---- */
static char fn_text[100]; static ULONGLONG fn_until;
void friends_notify_tick(app_state_t *s) {
    static char prev_id[MAX_FRIENDS][48]; static bool prev_on[MAX_FRIENDS], prev_call[MAX_FRIENDS]; static int prev_n = -1;
    if (!s->acct_user_id[0]) { prev_n = -1; return; }
    if (prev_n < 0) {   /* primeira leitura: só memoriza, não avisa de quem já estava online */
        if (!s->pres_remote && !s->friend_count) return;
        if (s->pres_remote && !s->pres_n && !s->friend_count) return;
    }
    const ULONGLONG now = GetTickCount64();
    int n = s->friend_count < MAX_FRIENDS ? s->friend_count : MAX_FRIENDS;
    bool nid[MAX_FRIENDS], ncall[MAX_FRIENDS];
    for (int i = 0; i < n; i++) {
        nid[i] = friends_online(s, &s->friends[i]); ncall[i] = in_call(s, &s->friends[i]);
        if (prev_n < 0 || s->notify_off) continue;
        int k = -1; for (int q = 0; q < prev_n; q++) if (!strcmp(prev_id[q], s->friends[i].id)) k = q;
        if (k < 0) continue;
        if (ncall[i] && !prev_call[k]) { snprintf(fn_text, sizeof fn_text, "%s entrou na sua chamada", s->friends[i].name); fn_until = now + 7000; cui_request_redraw(cui_app_ui(s->app)); }
        else if (nid[i] && !prev_on[k]) { snprintf(fn_text, sizeof fn_text, "%s ficou online", s->friends[i].name); fn_until = now + 7000; cui_request_redraw(cui_app_ui(s->app)); }
    }
    for (int i = 0; i < n; i++) { snprintf(prev_id[i], sizeof prev_id[i], "%s", s->friends[i].id); prev_on[i] = nid[i]; prev_call[i] = ncall[i]; }
    prev_n = n;
    if (fn_until && now > fn_until) { fn_until = 0; fn_text[0] = 0; cui_request_redraw(cui_app_ui(s->app)); }
}
void friends_notify_banner(cui_ctx *ui, app_state_t *s) {
    (void)s; if (!fn_until || !fn_text[0]) return;
    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 18, .pad_y = 6) {
        cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 14, .pad_y = 7, .gap = 8, .radius = 14, .bg = B_A(B_ONLINE, 0x24), .border = 1, .border_color = B_A(B_ONLINE, 0x66), .align = CUI_ALIGN_CENTER) {
            cui_box(ui, .width = CUI_PX(8), .height = CUI_PX(8), .radius = 4, .bg = B_ONLINE);
            cui_text(ui, fn_text, .size = 13, .nowrap = true);
        }
    }
}

static bool matches(const app_state_t *s, const person_t *p) {
    if (!s->friends_q[0]) return true;
    char hay[160], q[64];
    snprintf(hay, sizeof hay, "%s %s", p->name, p->nick); snprintf(q, sizeof q, "%s", s->friends_q);
    for (char *c = hay; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
    for (char *c = q; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
    return strstr(hay, q[0] == '@' ? q + 1 : q) != NULL;
}

/* linha de pessoa: avatar, nome + @nick/status, e os botões do chamador; devolve o cartão para os botões irem dentro */
#define ROW_BEGIN(ui, p) \
    cui_box((ui), .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 12, .pad_y = 8, .gap = 12, .radius = 14, .bg = B_GLASS_CARD, .border = 1, .border_color = B_GLASS_BORDER_SOFT, .align = CUI_ALIGN_CENTER)
static void row_ident(cui_ctx *ui, const person_t *p, const char *sub_override, bool online) {
    cui_avatar(ui, p->name, .size = 36);
    cui_column(ui, .gap = 1, .width = CUI_FILL) {
        cui_row(ui, .gap = 6, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, p->name, .nowrap = true);
            if (online) cui_box(ui, .width = CUI_PX(8), .height = CUI_PX(8), .radius = 4, .bg = B_ONLINE);   /* ponto verde: online agora */
        }
        char sub[130] = "";
        if (sub_override) snprintf(sub, sizeof sub, "%s", sub_override);
        else if (p->status[0]) snprintf(sub, sizeof sub, "%s", p->status);
        else if (p->nick[0]) snprintf(sub, sizeof sub, "@%s", p->nick);
        if (sub[0]) cui_text(ui, sub, .size = 11.5f, .color = B_MUTED, .nowrap = true);
    }
}

static float g_hub_h;   /* altura da área rolável do hub (quadro anterior): centraliza o estado vazio na vertical */
static void empty_state(cui_ctx *ui, const char *icon, const char *title, const char *sub) {
    float top = g_hub_h > 0 ? fmaxf(24.0f, (g_hub_h - 230.0f) * 0.5f - 18.0f) : 56.0f;
    cui_box(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .pad_top = top, .gap = 8) {
        cui_icon(ui, icon, 34, B_C3);
        cui_text(ui, title, .font = CUI_FONT_MEDIUM);
        cui_text(ui, sub, .size = 12, .color = B_MUTED);
    }
}

static void friend_row(cui_ctx *ui, app_state_t *s, int i) {
    person_t *p = &s->friends[i];
    cui_push_id_int(ui, 9000 + i);
    ROW_BEGIN(ui, p) {
        row_ident(ui, p, NULL, friends_online(s, p));
        if (cui_icon_button(ui, CUI_ICON_MESSAGE_SQUARE, .tooltip = "Mensagem")) app_open_friend(s, i);
        cui_menu(ui, "Mais", .icon = CUI_ICON_ELLIPSIS, .icon_only = true, .placement = CUI_PLACE_BOTTOM_END) {
            cui_menu_label(ui, p->name);
            if (cui_menu_item(ui, "Enviar mensagem", .icon = CUI_ICON_MESSAGE_SQUARE)) app_open_friend(s, i);
            cui_menu_separator(ui);
            if (cui_menu_item(ui, "Desfazer amizade…", .icon = CUI_ICON_X, .danger = true)) {
                snprintf(s->unfriend_id, sizeof s->unfriend_id, "%s", p->id); snprintf(s->unfriend_name, sizeof s->unfriend_name, "%s", p->name); s->unfriend_open = true;
            }
            if (cui_menu_item(ui, "Bloquear", .icon = CUI_ICON_SHIELD, .danger = true)) { char id[48], nm[96]; snprintf(id, sizeof id, "%s", p->id); snprintf(nm, sizeof nm, "%s", p->name); friends_block(s, id, nm); }
        }
    }
    cui_pop_id(ui);
}

static void list_tab(cui_ctx *ui, app_state_t *s, bool only_online) {
    int shown = 0;
    for (int i = 0; i < s->friend_count; i++) {
        if (only_online && !friends_online(s, &s->friends[i])) continue;
        if (!matches(s, &s->friends[i])) continue;
        friend_row(ui, s, i); shown++;
    }
    if (shown) return;
    if (s->friends_q[0]) empty_state(ui, CUI_ICON_SEARCH, "Ninguém encontrado", "Tente outro nome ou @nick.");
    else if (only_online) empty_state(ui, CUI_ICON_USERS, "Ninguém online agora", s->pres_remote ? "Quem estiver com o Briks aberto aparece aqui (e quem está invisível não aparece)." : "Por enquanto “Online” mostra quem está na sua chamada; a presença em tempo real depende de uma atualização do servidor.");
    else {
        empty_state(ui, CUI_ICON_USER_PLUS, "Nenhum amigo ainda", "Convide alguém pelo @nick para conversar e entrar em chamadas.");
        cui_box(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .pad_top = 6) {
            if (cui_button(ui, "Adicionar amigo", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_LG, .icon = CUI_ICON_USER_PLUS)) s->friends_tab = 4;
        }
    }
}

/* Atividade: quem está online agora e o que está fazendo. O texto (status / programa em uso) só chega de quem está na sua chamada: é a única via em que o amigo o envia. */
static void activity_tab(cui_ctx *ui, app_state_t *s) {
    int shown = 0;
    for (int i = 0; i < s->friend_count; i++) {
        person_t *p = &s->friends[i];
        if (!friends_online(s, p) || !matches(s, p)) continue;
        const char *txt = "Online agora";
        for (int q = 0; q < 8; q++) if (s->rtc_peers[q].id[0] && !strcmp(s->rtc_peers[q].name, p->name) && in_call(s, p)) {
            txt = s->rtc_peers[q].status[0] ? s->rtc_peers[q].status : s->rtc_peers[q].presence[0] ? s->rtc_peers[q].presence : "Na sua chamada";
        }
        cui_push_id_int(ui, 9500 + i);
        ROW_BEGIN(ui, p) {
            row_ident(ui, p, txt, true);
            if (cui_icon_button(ui, CUI_ICON_MESSAGE_SQUARE, .tooltip = "Mensagem")) app_open_friend(s, i);
        }
        cui_pop_id(ui); shown++;
    }
    if (!shown) empty_state(ui, CUI_ICON_USERS, "Nada acontecendo por enquanto", "Quando um amigo ficar online ou entrar na sua chamada, ele aparece aqui com o que está fazendo.");
}

static void pending_tab(cui_ctx *ui, app_state_t *s) {
    if (s->pending_count) {
        cui_text(ui, "RECEBIDOS", .size = 11, .color = B_MUTED);
        for (int i = 0; i < s->pending_count; i++) {
            if (!matches(s, &s->pending[i])) continue;
            person_t *p = &s->pending[i];
            cui_push_id_int(ui, 9300 + i);
            ROW_BEGIN(ui, p) {
                row_ident(ui, p, p->nick[0] ? NULL : "quer ser seu amigo", false);
                if (cui_icon_button(ui, CUI_ICON_CHECK, .variant = CUI_BUTTON_PRIMARY, .tooltip = "Aceitar")) app_accept_friend(s, p->request_id);
                if (cui_icon_button(ui, CUI_ICON_X, .tooltip = "Recusar")) account_respond_friend(s, p->request_id, false);
                if (cui_icon_button(ui, CUI_ICON_SHIELD, .tooltip = "Bloquear")) { char id[48], nm[96]; snprintf(id, sizeof id, "%s", p->id); snprintf(nm, sizeof nm, "%s", p->name); friends_block(s, id, nm); account_respond_friend(s, p->request_id, false); }
            }
            cui_pop_id(ui);
        }
    }
    if (s->outgoing_count) {
        cui_text(ui, "ENVIADOS", .size = 11, .color = B_MUTED);
        for (int i = 0; i < s->outgoing_count; i++) {
            if (!matches(s, &s->outgoing[i])) continue;
            person_t *p = &s->outgoing[i];
            cui_push_id_int(ui, 9400 + i);
            ROW_BEGIN(ui, p) {
                row_ident(ui, p, p->nick[0] ? NULL : "aguardando resposta", false);
                cui_text(ui, "Aguardando", .size = 11.5f, .color = B_MUTED);
                if (cui_icon_button(ui, CUI_ICON_UNDO_2, .tooltip = "Cancelar pedido")) account_remove_friend(s, p->id);
            }
            cui_pop_id(ui);
        }
    }
    if (!s->pending_count && !s->outgoing_count) empty_state(ui, CUI_ICON_INBOX, "Nenhum pedido pendente", "Pedidos que você enviar ou receber aparecem aqui.");
}

static void blocked_tab(cui_ctx *ui, app_state_t *s) {
    int unb = -1;
    cui_text(ui, "Bloqueados não aparecem nas suas listas nem podem te mandar pedido por aqui. A lista fica neste computador.", .size = 12, .color = B_MUTED);
    for (int i = 0; i < s->blocked_count; i++) {
        person_t p; memset(&p, 0, sizeof p); snprintf(p.name, sizeof p.name, "%s", s->blocked[i].name);
        if (!matches(s, &p)) continue;
        cui_push_id_int(ui, 9500 + i);
        ROW_BEGIN(ui, &p) {
            row_ident(ui, &p, "bloqueado", false);
            if (cui_button(ui, "Desbloquear", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) unb = i;
        }
        cui_pop_id(ui);
    }
    if (unb >= 0) { char id[48]; snprintf(id, sizeof id, "%s", s->blocked[unb].id); friends_unblock(s, id); }
    if (!s->blocked_count) empty_state(ui, CUI_ICON_SHIELD, "Ninguém bloqueado", "Use “Bloquear” no menu de um amigo ou de um pedido.");
}

static void amg_send(app_state_t *s) {   /* manda o pedido pendente quando o core estiver pronto (ele sobe sob demanda) */
    if (!s->amg_pend) return;
    core_status_t cs = core_state();
    if (cs == CORE_STOPPED) { core_start(); return; }
    if (cs == CORE_FAILED) { snprintf(s->amg_msg, sizeof s->amg_msg, "O serviço do Briks não iniciou. Tente de novo."); s->amg_pend = 0; s->amg_wait = false; return; }
    if (cs != CORE_READY) { cui_request_redraw(cui_app_ui(s->app)); return; }
    char line[1200];
    if (s->amg_pend == 1) { char nm[200]; json_escape(s->user_name, nm, sizeof nm); snprintf(line, sizeof line, "{\"cmd\":\"friend.invite\",\"name\":\"%s\"}", nm); }
    else { char c[1100]; json_escape(s->amg_in, c, sizeof c); snprintf(line, sizeof line, "{\"cmd\":\"friend.accept\",\"code\":\"%s\"}", c); }
    s->amg_pend = 0; core_send(line);
}
static void amg_make(app_state_t *s) { s->amg_pend = 1; s->amg_wait = true; s->amg_msg[0] = 0; amg_send(s); }
static void amg_accept(app_state_t *s) {
    if (!s->amg_in[0]) { snprintf(s->amg_msg, sizeof s->amg_msg, "Cole o código que a pessoa mandou."); return; }
    s->amg_pend = 2; s->amg_wait = true; s->amg_msg[0] = 0; amg_send(s);
}

/* ---- amigo P2P por código assinado (core: friend.invite / friend.accept; briks-core/src/friend_invite.rs) ---- */
static void amg_clip(const char *utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0); if (n <= 0) return;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (size_t)n * sizeof(wchar_t)); if (!h) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, (wchar_t *)GlobalLock(h), n); GlobalUnlock(h);
    if (OpenClipboard(NULL)) { EmptyClipboard(); if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h); CloseClipboard(); } else GlobalFree(h);
}

static void amg_paste(app_state_t *s) {   /* cola o texto da área de transferência no campo do código */
    if (getenv("BRIKS_DEV_NOCLIP") && s->amg_code) { snprintf(s->amg_in, sizeof s->amg_in, "%s", s->amg_code); return; }   /* teste oculto: sem mexer na área de transferência do usuário */
    if (!OpenClipboard(NULL)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) { const wchar_t *w = (const wchar_t *)GlobalLock(h); if (w) { WideCharToMultiByte(CP_UTF8, 0, w, -1, s->amg_in, (int)sizeof s->amg_in, NULL, NULL); s->amg_in[sizeof s->amg_in - 1] = 0; GlobalUnlock(h); } }
    CloseClipboard();
}

/* resposta do core (friend.result): convite gerado, amigo aceito ou erro em português */
void friends_p2p_event(app_state_t *s, const jval *j) {
    const char *op = json_str(j, "op", ""); const jval *okv = json_get(j, "ok"); const bool ok = okv && okv->type == J_BOOL && okv->b;
    s->amg_wait = false;
    if (!ok) { snprintf(s->amg_msg, sizeof s->amg_msg, "%s", json_str(j, "message", "Não deu certo.")); return; }
    if (!strcmp(op, "invite")) {
        free(s->amg_code); s->amg_code = _strdup(json_str(j, "code", ""));
        snprintf(s->amg_msg, sizeof s->amg_msg, "Código pronto (vale por 7 dias). Mande só para quem você quer como amigo.");
    } else if (!strcmp(op, "accept")) {
        const char *fp = json_str(j, "fp", ""), *nm = json_str(j, "name", "?");
        bool have = false; for (int i = 0; i < s->friend_count; i++) if (fp[0] && !strcmp(s->friends[i].fp, fp)) have = true;
        if (!have && s->friend_count < MAX_FRIENDS) {   /* só nesta sessão: a lista de verdade vem da conta; o amigo P2P fica gravado no core (friends-p2p.json) */
            person_t *p = &s->friends[s->friend_count++]; memset(p, 0, sizeof *p);
            snprintf(p->id, sizeof p->id, "p2p:%.40s", fp); snprintf(p->name, sizeof p->name, "%s", nm); snprintf(p->fp, sizeof p->fp, "%s", fp);
        }
        s->amg_in[0] = 0;
        snprintf(s->amg_msg, sizeof s->amg_msg, json_get(j, "existed") && json_get(j, "existed")->b ? "%s já era seu amigo (dados atualizados)." : "%s foi adicionado como amigo.", nm);
    }
}

/* cartão "Por código": gerar o meu convite, copiar, e colar o de outra pessoa para aceitar */
void friends_code_card(cui_ctx *ui, app_state_t *s) {
    amg_send(s);
    cui_box(ui, .width = CUI_FILL, .pad = 16, .gap = 10, .radius = 16, .bg = B_GLASS_CARD, .border = 1, .border_color = B_GLASS_BORDER) {
        cui_text(ui, "Adicionar por código", .font = CUI_FONT_MEDIUM);
        cui_text(ui, "Sem servidor: gere o seu código e mande à pessoa, ou cole aqui o código que ela mandou. O código é assinado com a sua chave e vale por 7 dias.", .size = 12.5f, .color = B_MUTED);
        cui_row(ui, .gap = 8, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
            if (cui_button(ui, s->amg_code ? "Gerar outro" : "Gerar meu código", .variant = CUI_BUTTON_SECONDARY, .loading = s->amg_wait)) amg_make(s);
            if (s->amg_code && cui_button(ui, "Copiar código", .variant = CUI_BUTTON_PRIMARY, .icon = CUI_ICON_COPY)) { if (!getenv("BRIKS_DEV_NOCLIP")) amg_clip(s->amg_code); snprintf(s->amg_msg, sizeof s->amg_msg, "Código copiado."); }
        }
        if (s->amg_code) { char head[72]; snprintf(head, sizeof head, "%.44s…", s->amg_code); cui_text(ui, head, .size = 13, .color = B_MUTED, .nowrap = true); }
        cui_row(ui, .gap = 8, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
            int r = cui_input(ui, "amg-in", s->amg_in, sizeof s->amg_in, .placeholder = "Cole o código briks-amg1…", .width = CUI_FILL);
            if (cui_button(ui, "Colar", .variant = CUI_BUTTON_SECONDARY)) amg_paste(s);
            if (cui_button(ui, "Adicionar", .variant = CUI_BUTTON_PRIMARY, .loading = s->amg_wait) || (r & CUI_SUBMITTED)) amg_accept(s);
        }
        if (s->amg_msg[0]) cui_text(ui, s->amg_msg, .size = 12.5f, .color = B_MUTED);
    }
}

static void add_tab(cui_ctx *ui, app_state_t *s) {
    const bool online = s->acct_user_id[0] != 0;
    cui_box(ui, .width = CUI_FILL, .pad = 16, .gap = 10, .radius = 16, .bg = B_GLASS_CARD, .border = 1, .border_color = B_GLASS_BORDER) {
        cui_text(ui, "Adicionar por @nick", .font = CUI_FONT_MEDIUM);
        cui_text(ui, online ? "Digite o @nick de quem você quer adicionar. A pessoa recebe o pedido e, ao aceitar, vocês passam a se achar pela conta."
                            : "Entre na sua conta para adicionar amigos pelo @nick.", .size = 12.5f, .color = B_MUTED);
        cui_row(ui, .gap = 8, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
            int r = cui_input(ui, "add-nick", s->add_email, sizeof s->add_email, .placeholder = "@nick", .width = CUI_FILL);
            if ((cui_button(ui, "Enviar pedido", .variant = CUI_BUTTON_PRIMARY, .loading = s->busy) || (r & CUI_SUBMITTED)) && online) app_add_friend(s);
        }
        if (online && s->acct_nick[0]) cui_textf(ui, "O seu é @%s", s->acct_nick);
        if (s->notice[0]) cui_text(ui, s->notice, .size = 12.5f, .color = B_MUTED);
    }
    friends_code_card(ui, s);
}

void friends_hub(cui_ctx *ui, app_state_t *s) {
    blocked_load(s);
    static const char *const tabs[] = { "Todos", "Online", "Pendentes", "Bloqueados", "Adicionar", "Atividade" };
    char pend[24]; snprintf(pend, sizeof pend, "Pendentes %d", s->pending_count);
    static const char *const shorts[] = { "Todos", "Online", "Pend.", "Bloq.", "Novo", "Ativ." };
    const bool mid = g_panel_w >= 600, tiny = g_panel_w < 400;   /* largo: nomes inteiros; estreito: nomes curtos */
    const char *items[6] = { tabs[0], tabs[1], s->pending_count ? pend : tabs[2], tabs[3], tabs[4], tabs[5] };
    if (!mid) { items[0] = shorts[0]; items[1] = shorts[1]; items[2] = s->pending_count ? pend : shorts[2]; items[3] = shorts[3]; items[4] = shorts[4]; items[5] = shorts[5]; }
    g_hub_h = cui_get_rect(ui, cui_make_id(ui, "friends-hub")).h;
    cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .height = CUI_PX(56), .pad_left = 18, .pad_right = 18 + g_tools_w, .gap = 10, .align = CUI_ALIGN_CENTER, .bg = CUI_RGBA(0xffffff0a),
            .radius_tl = (int)g_th.panel_radius - 1, .radius_tr = (int)g_th.panel_radius - 1) {
        cui_icon(ui, CUI_ICON_USERS, 18, B_MUTED);
        cui_text(ui, "Amigos", .font = CUI_FONT_MEDIUM, .nowrap = true);
    }
    cui_box(ui, .width = CUI_FILL, .pad_x = 12, .pad_y = 8, .bg = CUI_RGBA(0xffffff0a)) {   /* abas em linha própria: ocupam a largura toda, sem corte nem degradê */
        static const char *const tab_icons[6] = { CUI_ICON_USERS, CUI_ICON_WIFI, CUI_ICON_CLOCK, CUI_ICON_SHIELD, CUI_ICON_USER_PLUS, CUI_ICON_HISTORY };
        if (tiny) {   /* muito estreito: só ícones, cada um um botão próprio (hover, clique e dica com o nome) */
            cui_box(ui, .dir = CUI_ROW, .width = CUI_FILL, .gap = 4) {
                for (int i = 0; i < 6; i++) {
                    cui_push_id_int(ui, 9700 + i);
                    bool on = s->friends_tab == i;
                    cui_box(ui, .id = "t", .width = CUI_FILL, .height = CUI_PX(32), .radius = 12, .bg = on ? CUI_RGBA(0xffffff1c) : CUI_RGBA(0x00000000),
                            .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) { cui_icon(ui, tab_icons[i], 16, on ? B_TEXT : B_MUTED); }
                    if (briks_hover(ui, "t")) briks_tip(i == 2 && s->pending_count ? pend : tabs[i]);
                    if (briks_clicked(ui, "t")) s->friends_tab = i;
                    cui_pop_id(ui);
                }
            }
        } else cui_tabs(ui, "friends-tabs", items, 6, &s->friends_tab, .variant = CUI_TABS_PILL, .full_width = true);
    }
    cui_separator(ui);
    if (s->friends_tab != 4 && s->friend_count > 0) cui_box(ui, .width = CUI_FILL, .pad_x = 14, .pad_top = 10) {   /* botão sempre visível no topo da seção, em linha própria (não espreme as abas) */
        if (cui_button(ui, "Adicionar amigo", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM, .icon = CUI_ICON_USER_PLUS, .full_width = true)) s->friends_tab = 4;
    }
    friends_notify_banner(ui, s);
    if ((s->friends_tab <= 3 && s->friends_tab != 2) || s->friends_tab == 5) {
        cui_box(ui, .width = CUI_FILL, .pad_x = 18, .pad_top = 12) {
            cui_input(ui, "friends-q", s->friends_q, sizeof s->friends_q, .placeholder = "Buscar amigo", .width = CUI_FILL);
        }
    }
    cui_scroll(ui, .id = "friends-hub", .width = CUI_FILL, .height = CUI_FILL, .pad = 18) {
        cui_column(ui, .gap = 8, .width = CUI_FILL) {
            switch (s->friends_tab) {
            case 0: list_tab(ui, s, false); break;
            case 1: list_tab(ui, s, true); break;
            case 2: pending_tab(ui, s); break;
            case 3: blocked_tab(ui, s); break;
            case 5: activity_tab(ui, s); break;
            default: add_tab(ui, s); break;
            }
        }
    }
}

void friends_dialogs(cui_ctx *ui, app_state_t *s) {
    friends_notify_tick(s);
    cui_dialog(ui, "Desfazer amizade", &s->unfriend_open) {
        cui_textf(ui, "Remover %s dos seus amigos?", s->unfriend_name);
        cui_text(ui, "As conversas continuam gravadas neste computador. Para voltar a ser amigos, é preciso um novo pedido.", .size = 12.5f, .color = B_MUTED);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Desfazer amizade", .variant = CUI_BUTTON_DANGER)) { account_remove_friend(s, s->unfriend_id); s->unfriend_open = false; }
        }
    }
}
