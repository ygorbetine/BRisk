/* Marco 2: mensagens e grupos P2P locais, gravados pelas rotas /p2p/* do backend local.
 * As mensagens ficam só neste computador até o briks-core (marco 3) entregá-las pelo DataChannel. */
#include "briks_ui.h"
#include "backend.h"
#include "json.h"
#include "msgui.h"
#include "attach.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <windows.h>
#include <bcrypt.h>

static void new_id(char *out, size_t cap) {
    unsigned char r[16];
    BCryptGenRandom(NULL, r, sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    size_t n = 0;
    for (int i = 0; i < 16 && n + 3 <= cap; i++) n += (size_t)snprintf(out + n, cap - n, "%02x", r[i]);
}

static void now_iso(char *out, size_t cap) {
    SYSTEMTIME t; GetSystemTime(&t);
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
}

/* Canal da DM: igual nos dois lados porque os ids entram em ordem. */
static void dm_channel(const app_state_t *s, const char *peer, char *out, size_t cap) {
    const char *a = s->acct_user_id[0] ? s->acct_user_id : s->user_id, *b = peer;   /* conta online: os dois lados usam os ids das contas */
    if (strcmp(a, b) > 0) { const char *t = a; a = b; b = t; }
    snprintf(out, cap, "dm-%s-%s", a, b);
}

void app_open_friend(app_state_t *s, int idx) {
    s->active_friend = idx; s->active_group = -1; s->msg_count = 0;
    if (idx < 0) return;
    dm_channel(s, s->friends[idx].id, s->chat_channel, sizeof s->chat_channel);
    s->reload_msgs = true;
}

void app_open_group(app_state_t *s, int idx) {
    s->active_group = idx; s->active_friend = -1; s->msg_count = 0;
    if (idx < 0) return;
    snprintf(s->chat_channel, sizeof s->chat_channel, "%s", s->groups[idx].channel);
    s->reload_msgs = true;
}

static void add_msg(app_state_t *s, const char *id, const char *author, const char *content, const char *created, bool mine) {
    if (s->msg_count >= MAX_MSGS) { memmove(s->msgs, s->msgs + 1, sizeof(msg_t) * (MAX_MSGS - 1)); s->msg_count = MAX_MSGS - 1; }
    msg_t *m = &s->msgs[s->msg_count++];
    snprintf(m->id, sizeof m->id, "%s", id); snprintf(m->author, sizeof m->author, "%s", author);
    snprintf(m->content, sizeof m->content, "%s", content); snprintf(m->created, sizeof m->created, "%s", created);
    m->mine = mine;
}

void app_send_message(app_state_t *s) {
    if (s->busy || !s->draft[0] || !s->chat_channel[0]) return;
    char id[40], iso[40], a[200], c[1100], b[1700], path[256];
    new_id(id, sizeof id); now_iso(iso, sizeof iso);
    json_escape(s->user_name, a, sizeof a); json_escape(s->draft, c, sizeof c);
    snprintf(b, sizeof b, "{\"id\":\"%s\",\"channelId\":\"%s\",\"author\":\"%s\",\"content\":\"%s\",\"createdAt\":\"%s\",\"reactions\":{}}",
             id, s->chat_channel, a, c, iso);
    snprintf(path, sizeof path, "/p2p/messages/%s", s->chat_channel);
    add_msg(s, id, s->user_name, s->draft, iso, true);   /* otimista: some se a gravação falhar */
    msgui_after_send(id);                                 /* liga a resposta pendente (citação) ao id novo */
    s->draft[0] = 0;
    attach_set_inflight(id);
    app_begin(s, OP_MSG_SEND, "POST", path, b);
}

void app_forward_message(app_state_t *s, bool to_group, int idx, const char *text) {
    if (!text || !text[0]) return;
    char ch[160];
    if (to_group) { if (idx < 0 || idx >= s->group_count) return; snprintf(ch, sizeof ch, "%s", s->groups[idx].channel); }
    else { if (idx < 0 || idx >= s->friend_count) return; dm_channel(s, s->friends[idx].id, ch, sizeof ch); }
    char id[40], iso[40], a[200], c[1100], b[1800], path[256], body[600];
    snprintf(body, sizeof body, "↪ Encaminhada: %.480s", text);
    new_id(id, sizeof id); now_iso(iso, sizeof iso);
    json_escape(s->user_name, a, sizeof a); json_escape(body, c, sizeof c);
    snprintf(b, sizeof b, "{\"id\":\"%s\",\"channelId\":\"%s\",\"author\":\"%s\",\"content\":\"%s\",\"createdAt\":\"%s\",\"reactions\":{}}", id, ch, a, c, iso);
    snprintf(path, sizeof path, "/p2p/messages/%s", ch);
    if (!strcmp(ch, s->chat_channel)) add_msg(s, id, s->user_name, body, iso, true);
    if (s->token[0] && !s->busy) app_begin(s, OP_MSG_SEND, "POST", path, b);
}

void app_load_messages(app_state_t *s, const char *body) {
    jval *j = json_parse(body);
    s->msg_count = 0;
    int cnt = json_len(j); if (cnt > MAX_MSGS) cnt = MAX_MSGS;
    int order[MAX_MSGS];
    for (int i = 0; i < cnt; i++) order[i] = i;
    /* a API devolve do mais novo ao mais antigo: ordena por createdAt crescente */
    for (int i = 1; i < cnt; i++) {
        int k = order[i], t = i - 1;
        while (t >= 0 && strcmp(json_str(json_at(j, order[t]), "createdAt", ""), json_str(json_at(j, k), "createdAt", "")) > 0) { order[t + 1] = order[t]; t--; }
        order[t + 1] = k;
    }
    for (int i = 0; i < cnt; i++) {
        const jval *m = json_at(j, order[i]);
        const char *author = json_str(m, "author", "?");
        if (!*json_str(m, "deletedAt", ""))
            add_msg(s, json_str(m, "id", ""), author, json_str(m, "content", ""), json_str(m, "createdAt", ""), !strcmp(author, s->user_name));
    }
    json_free(j);
}

void app_open_chat2(app_state_t *s, int idx) {
    s->chat2_friend = idx; s->msg2_count = 0;
    if (idx < 0 || idx >= s->friend_count) return;
    dm_channel(s, s->friends[idx].id, s->chat2_channel, sizeof s->chat2_channel);
    s->reload_msgs2 = true;
}

static void add_msg2(app_state_t *s, const char *id, const char *author, const char *content, const char *created, bool mine) {
    if (s->msg2_count >= MAX_MSGS) return;
    msg_t *m = &s->msgs2[s->msg2_count++];
    snprintf(m->id, sizeof m->id, "%s", id); snprintf(m->author, sizeof m->author, "%s", author);
    snprintf(m->content, sizeof m->content, "%s", content); snprintf(m->created, sizeof m->created, "%s", created);
    m->mine = mine;
}

void app_load_messages2(app_state_t *s, const char *body) {
    jval *j = json_parse(body);
    s->msg2_count = 0;
    int cnt = json_len(j); if (cnt > MAX_MSGS) cnt = MAX_MSGS;
    int order[MAX_MSGS];
    for (int i = 0; i < cnt; i++) order[i] = i;
    for (int i = 1; i < cnt; i++) {
        int k = order[i], t = i - 1;
        while (t >= 0 && strcmp(json_str(json_at(j, order[t]), "createdAt", ""), json_str(json_at(j, k), "createdAt", "")) > 0) { order[t + 1] = order[t]; t--; }
        order[t + 1] = k;
    }
    for (int i = 0; i < cnt; i++) {
        const jval *m = json_at(j, order[i]);
        const char *author = json_str(m, "author", "?");
        if (!*json_str(m, "deletedAt", ""))
            add_msg2(s, json_str(m, "id", ""), author, json_str(m, "content", ""), json_str(m, "createdAt", ""), !strcmp(author, s->user_name));
    }
    json_free(j);
}

void app_create_group(app_state_t *s) {
    if (s->busy || !s->new_group[0]) return;
    char b[2200];
    groups_create_json(s, b, sizeof b);   /* ui_groups.c: canal "geral" + sala de voz */
    s->new_group[0] = 0; s->create_group_open = false;
    app_begin(s, OP_GROUP_SAVE, "POST", "/p2p/groups", b);
}

void app_load_groups(app_state_t *s, const char *body) { groups_load(s, body); }

/* BRIKS_DEMO=login|social|call|history|group: dados fixos, sem backend, para comparar o visual entre as versões. */
void app_demo(app_state_t *s, const char *mode) {
    msgui_demo(true);
    snprintf(s->user_name, sizeof s->user_name, "Ygor"); snprintf(s->user_email, sizeof s->user_email, "ygor@briks.app");
    const char *fn[] = { "Marina Costa", "Rafael Lima", "Bia Nogueira" };
    for (int i = 0; i < 3; i++) { snprintf(s->friends[i].name, sizeof s->friends[i].name, "%s", fn[i]); snprintf(s->friends[i].id, sizeof s->friends[i].id, "friend-demo-%d", i); }
    s->friend_count = 3;
    snprintf(s->communities[0].name, sizeof s->communities[0].name, "Time Briks"); s->community_count = 1; s->communities[0].members = 12;
    if (getenv("BRIKS_DEMO_COMM")) {   /* nomes curtos e longos para testar a barra de grupos responsiva */
        const char *cn[] = { "Noite de Domingo", "Devs", "Comunidade dos Jogadores Noturnos de Domingo" }; int cm[] = { 8, 1, 128 };
        for (int i = 0; i < 3; i++) { snprintf(s->communities[1 + i].name, sizeof s->communities[1 + i].name, "%s", cn[i]); s->communities[1 + i].members = cm[i]; }
        s->community_count = 4;
    }
    snprintf(s->groups[0].name, sizeof s->groups[0].name, "Noite de jogos"); s->group_count = 1;
    { group_t *g = &s->groups[0]; snprintf(g->owner, sizeof g->owner, "me"); snprintf(s->user_id, sizeof s->user_id, "me"); g->is_owner = g->can_moderate = true;   /* demo: canais, cargos e membros */
      const char *cn[] = { "geral", "memes", "Sala de voz", "Valorant" }; for (int i = 0; i < 4; i++) { snprintf(g->ch[i].id, sizeof g->ch[i].id, "c%d", i); snprintf(g->ch[i].name, sizeof g->ch[i].name, "%s", cn[i]); g->ch[i].voice = i >= 2; } g->nch = 4;
      const char *mn[] = { "Ygor", "Marina Costa", "Rafael Lima", "Bia Nogueira" }; for (int i = 0; i < 4; i++) { snprintf(g->m[i].id, sizeof g->m[i].id, "m%d", i == 0 ? 99 : i); snprintf(g->m[i].name, sizeof g->m[i].name, "%s", mn[i]); } g->m[1].admin = true; g->nm = 4; snprintf(g->m[0].id, sizeof g->m[0].id, "me");
      if (getenv("BRIKS_DEMO_GDUP")) { g->m[g->nm] = g->m[1]; g->nm++; }                       /* captura: membro duplicado (botão Limpar duplicados) */
      if (getenv("BRIKS_DEMO_GMEMBER")) { snprintf(g->owner, sizeof g->owner, "m1"); g->is_owner = g->can_moderate = false; g->m[0].admin = false; }   /* captura: sou membro comum (sem gestão) */
      const char *gd = getenv("BRIKS_DEMO_GDLG"); if (gd) { int v = atoi(gd); s->members_open = v == 1; s->chan_open = v == 2; s->gren_open = v == 3; s->gleave_open = v == 4; } }
    { const char *ft = getenv("BRIKS_DEMO_FTAB"); if (ft) {   /* captura do hub de Amigos: aba + pedidos/bloqueados de exemplo (nada é gravado) */
        s->friends_tab = atoi(ft); s->blocked_loaded = true;
        const char *pn[] = { "Lucas Prado", "Helena Dias" }, *pk[] = { "lucasp", "helenad" };
        for (int i = 0; i < 2; i++) { snprintf(s->pending[i].name, sizeof s->pending[i].name, "%s", pn[i]); snprintf(s->pending[i].nick, sizeof s->pending[i].nick, "%s", pk[i]); }
        s->pending_count = 2;
        s->pres_remote = true; s->pres_n = 2; for (int i = 0; i < 2; i++) { snprintf(s->pres[i].id, sizeof s->pres[i].id, "%s", s->friends[i].id); s->pres[i].online = true; }   /* dois amigos "online" na captura */
        snprintf(s->outgoing[0].name, sizeof s->outgoing[0].name, "Caio Ramos"); snprintf(s->outgoing[0].nick, sizeof s->outgoing[0].nick, "caior"); s->outgoing_count = 1;
        snprintf(s->blocked[0].name, sizeof s->blocked[0].name, "Fulano Spam"); snprintf(s->blocked[0].id, sizeof s->blocked[0].id, "x1"); s->blocked_count = 1;
        for (int i = 0; i < 3; i++) snprintf(s->friends[i].nick, sizeof s->friends[i].nick, "%s", i == 0 ? "marina" : i == 1 ? "rafa_lima" : "bianog");
    } }
    s->active_group = -1; s->active_friend = -1; s->chat2_friend = 1; snprintf(s->chat2_channel, sizeof s->chat2_channel, "demo");
    s->screen = SCREEN_SOCIAL;
    if (getenv("BRIKS_DEMO_EMPTY")) { s->friend_count = 0; s->group_count = 0; s->pending_count = 0; s->community_count = 0; }   /* conta nova: sem amigos nem grupos (capturas dos estados vazios) */
    if (getenv("BRIKS_DEMO_STATUS")) {   /* status em balão e rich presence de exemplo (captura) */
        snprintf(s->user_status, sizeof s->user_status, "Focado, volto já"); snprintf(s->user_presence, sizeof s->user_presence, "Usando VS Code"); s->rp_enabled = true;
        snprintf(s->friends[0].status, sizeof s->friends[0].status, "Chego em 10 minutos");
        snprintf(s->friends[1].presence, sizeof s->friends[1].presence, "Jogando Valorant"); snprintf(s->friends[2].presence, sizeof s->friends[2].presence, "Ouvindo no Spotify");
    }
    if (getenv("BRIKS_DEMO_STATUS")) {   /* status em balão e rich presence de exemplo (captura) */
        snprintf(s->user_status, sizeof s->user_status, "Focado, volto já"); snprintf(s->user_presence, sizeof s->user_presence, "Usando VS Code"); s->rp_enabled = true;
        snprintf(s->friends[0].status, sizeof s->friends[0].status, "Chego em 10 minutos");
        snprintf(s->friends[1].presence, sizeof s->friends[1].presence, "Jogando Valorant"); snprintf(s->friends[2].presence, sizeof s->friends[2].presence, "Ouvindo no Spotify");
    }
    { const char *ds = getenv("BRIKS_DEMO_STATUS"); if (ds && ds[0] == '2') {   /* variante: textos de tamanhos diferentes (captura) */
        snprintf(s->user_status, sizeof s->user_status, "Ok"); snprintf(s->friends[0].status, sizeof s->friends[0].status, "Chego em 10 minutos, pode começar sem mim"); } }
    snprintf(s->rp_ignore[0], sizeof s->rp_ignore[0], "spotify"); snprintf(s->rp_ignore[1], sizeof s->rp_ignore[1], "discord"); s->rp_ignore_n = 2;
    if (!strcmp(mode, "login")) { s->screen = SCREEN_AUTH; return; }
    if (!strcmp(mode, "call") || !strcmp(mode, "call-chat") || !strcmp(mode, "call-broadcast")) {
        s->screen = SCREEN_CALL; s->mic_on = true;
        add_msg(s, "m1", "Marina Costa", "Oi! Você entra na sala às 21h?", "2026-11-09T23:41:00.000Z", false);
        add_msg(s, "m2", "Ygor", "Entro sim, só terminar o jantar.", "2026-11-09T23:42:00.000Z", true);
        if (!strcmp(mode, "call-chat")) s->call_view = 1;
        if (!strcmp(mode, "call-broadcast")) s->call_mode = 1;
        return;
    }
    if (!strcmp(mode, "history")) { s->screen = SCREEN_HISTORY; return; }
    if (!strcmp(mode, "social") || !strcmp(mode, "group") || !strcmp(mode, "bubbles") || !strcmp(mode, "msgs")) {
        const char *who = !strcmp(mode, "group") ? "Noite de jogos" : "Marina Costa";
        if (!strcmp(mode, "group")) s->active_group = 0; else if (!getenv("BRIKS_DEMO_FTAB")) s->active_friend = 0;
        add_msg(s, "m1", "Marina Costa", "Oi! Você entra na sala às 21h?", "2026-11-09T23:41:00.000Z", false);
        add_msg(s, "m2", "Ygor", "Entro sim, só terminar o jantar.", "2026-11-09T23:42:00.000Z", true);
        add_msg(s, "m3", "Marina Costa", "Perfeito, te espero lá.", "2026-11-09T23:43:00.000Z", false);
        add_msg2(s, "n1", "Rafael Lima", "Bora jogar hoje? Já abri a sala.", "2026-11-09T22:10:00.000Z", false);
        add_msg2(s, "n2", "Ygor", "Boa! Chego em 10 minutos.", "2026-11-09T22:12:00.000Z", true);
        add_msg2(s, "n3", "Rafael Lima", "Fechou, te espero.", "2026-11-09T22:13:00.000Z", false);
        if (!strcmp(mode, "bubbles")) {   /* uma linha, consecutivas e várias linhas: compara o alinhamento do balão com o avatar */
            add_msg(s, "m4", "Marina Costa", "Consegui os ingressos.", "2026-11-09T23:44:00.000Z", false);
            add_msg(s, "m5", "Marina Costa", "Só falta combinar o horário, o local de encontro e quem leva o carro, porque a vaga na garagem do prédio é limitada e eu preciso avisar a portaria com antecedência para ninguém ficar de fora.", "2026-11-09T23:45:00.000Z", false);
            add_msg(s, "m6", "Ygor", "Combinado.", "2026-11-09T23:46:00.000Z", true);
        }
        if (!strcmp(mode, "msgs")) {   /* resposta, editada, reação, fixada (capturas de interação com mensagens) */
            msgui_demo_seed();
            if (getenv("BRIKS_DEMO_ADMIN")) { s->active_group = 0; s->active_friend = -1; s->groups[0].can_moderate = true; }
        }
        (void)who;
        if (getenv("BRIKS_DEMO_ATTACH")) {   /* capturas de anexos, prévia de link, estados e digitando (sem rede) */
            add_msg(s, "m4", "Marina Costa", "Olha esse artigo: https://exemplo.com/artigo-p2p", "2026-11-09T23:44:00.000Z", false);
            add_msg(s, "m4b", "Marina Costa", "Tem **negrito**, `codigo()`, [um link](https://exemplo.com) e menção @Ygor. # isto não é título", "2026-11-09T23:44:30.000Z", false);
            add_msg(s, "m5", "Ygor", "Boa! Vou ler agora.", "2026-11-09T23:45:00.000Z", true);
            add_msg(s, "m6", "Ygor", "Essa mensagem não chegou.", "2026-11-09T23:46:00.000Z", true);
            attach_mark("m5", ATT_SENDING); attach_mark("m6", ATT_FAILED);
            attach_demo(s);
        }
    }
}
