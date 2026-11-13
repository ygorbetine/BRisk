/* Marco 2: mensagens e grupos P2P locais, gravados pelas rotas /p2p/* do backend local.
 * As mensagens ficam só neste computador até o briks-core (marco 3) entregá-las pelo DataChannel. */
#include "briks_ui.h"
#include "sysint.h"
#include "backend.h"
#include "core.h"
#include "json.h"
#include "msgui.h"
#include "attach.h"
#include "call.h"
#include "ginvite.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
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
void chat_dm_channel(const app_state_t *s, const char *peer, char *out, size_t cap);
static void dm_channel(const app_state_t *s, const char *peer, char *out, size_t cap) {
    const char *a = s->acct_user_id[0] ? s->acct_user_id : s->user_id, *b = peer;   /* conta online: os dois lados usam os ids das contas */
    if (strcmp(a, b) > 0) { const char *t = a; a = b; b = t; }
    snprintf(out, cap, "dm-%s-%s", a, b);
}

void app_open_friend(app_state_t *s, int idx) {
    s->active_friend = idx; s->active_group = -1; s->msg_count = 0; history_close();
    if (idx < 0) return;
    dm_channel(s, s->friends[idx].id, s->chat_channel, sizeof s->chat_channel);
    s->reload_msgs = true;
}

void app_open_group(app_state_t *s, int idx) {
    s->active_group = idx; s->active_friend = -1; s->msg_count = 0; history_close();
    if (idx < 0) return;
    snprintf(s->chat_channel, sizeof s->chat_channel, "%s", s->groups[idx].channel);
    s->reload_msgs = true;
}

static void add_msg(app_state_t *s, const char *id, const char *author, const char *content, const char *created, bool mine);

/* ---- DM fora da sala de voz ---- */
static bool in_call_with(const app_state_t *s, const char *fp) {
    if (!s->in_room || !fp[0]) return false;
    for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (s->rtc_peers[i].id[0] && !strcmp(s->rtc_peers[i].fp, fp) && !strcmp(s->rtc_peers[i].state, "autenticado")) return true;
    return false;
}

/* pede ao core o canal direto com o amigo (idempotente; a cada 20 s no máximo por amigo, para não refazer o toque a cada mensagem) */
static void dm_open_link(app_state_t *s, const person_t *f) {
    static struct { char fp[72]; ULONGLONG t; } last[8]; static int n;
    const ULONGLONG now = GetTickCount64(); int k = -1;
    for (int i = 0; i < n; i++) if (!strcmp(last[i].fp, f->fp)) k = i;
    if (k >= 0 && now - last[k].t < 20000) return;
    if (k < 0) { k = n < 8 ? n++ : 0; snprintf(last[k].fp, sizeof last[k].fp, "%s", f->fp); }
    last[k].t = now;
    char fp[160], uid[100], line[400]; json_escape(f->fp, fp, sizeof fp); json_escape(f->id, uid, sizeof uid);
    snprintf(line, sizeof line, "{\"cmd\":\"dm.open\",\"fp\":\"%s\",\"user_id\":\"%s\"}", fp, uid);
    core_send(line); (void)s;
}

bool chat_in_call_with(const app_state_t *s, const char *fp) { return in_call_with(s, fp); }
/* abre (ou reabre) o canal direto com o amigo de impressão digital `fp`, para um anexo da fila entregar quando ele aparecer */
void chat_dm_open_fp(app_state_t *s, const char *fp) {
    if (core_state() != CORE_READY || !fp || !fp[0]) return;
    for (int i = 0; i < s->friend_count; i++) if (s->friends[i].fp[0] && !strcmp(s->friends[i].fp, fp)) { if (!in_call_with(s, fp)) dm_open_link(s, &s->friends[i]); return; }
}
bool chat_dm_wants_session(const app_state_t *s) { return s->acct_user_id[0] && !s->acct_demo && core_state() == CORE_READY; }

static char g_inbox_for[64];
static void ob_tick(app_state_t *s);
void chat_dm_tick(app_state_t *s) {
    if (!s->rtc_session_ready || !chat_dm_wants_session(s)) { if (!s->rtc_session_ready) g_inbox_for[0] = 0; return; }
    ob_tick(s);
    if (!strcmp(g_inbox_for, s->acct_user_id)) return;
    snprintf(g_inbox_for, sizeof g_inbox_for, "%s", s->acct_user_id);
    char uid[100], line[200]; json_escape(s->acct_user_id, uid, sizeof uid);
    snprintf(line, sizeof line, "{\"cmd\":\"dm.inbox\",\"user_id\":\"%s\"}", uid);
    core_send(line);
}
void chat_dm_reset(app_state_t *s) {
    (void)s; g_inbox_for[0] = 0;
    if (core_state() == CORE_READY) core_send("{\"cmd\":\"dm.inbox\",\"user_id\":\"\"}");
}

static const person_t *friend_by_fp(const app_state_t *s, const char *fp) {
    if (!fp[0]) return NULL;
    for (int i = 0; i < s->friend_count; i++) if (s->friends[i].fp[0] && !strcmp(s->friends[i].fp, fp)) return &s->friends[i];
    return NULL;
}

/* um amigo me tocou: só quem está na minha lista de amigos (impressão digital conhecida pela conta) abre o canal */
void chat_dm_wake(app_state_t *s, const char *fp) {
    const person_t *f = friend_by_fp(s, fp);
    if (!f || core_state() != CORE_READY) return;
    char e[160], line[320]; json_escape(f->fp, e, sizeof e);
    snprintf(line, sizeof line, "{\"cmd\":\"dm.open\",\"fp\":\"%s\"}", e);
    core_send(line);
}

/* ---- fila de envio no aparelho (outbox.rs do core): pendente -> enviada (saiu pelo canal) -> entregue (ACK) ou falhou ----
 * O core guarda, cifrado neste PC, o que ainda não foi entregue; aqui ficam só os ids e para quem, para os tiques e para reabrir o canal
 * direto quando o amigo ficar online. Nada de conteúdo sobe ao servidor. */
#define OB_N 512
static struct { char id[72], fp[72]; } g_pend[OB_N]; static int g_pend_n;
static char g_dlv[OB_N][72]; static int g_dlv_n;
static char g_fld[OB_N][72]; static int g_fld_n;
static bool ob_in(char (*a)[72], int n, const char *id) { for (int i = 0; i < n; i++) if (!strcmp(a[i], id)) return true; return false; }
static void ob_push(char (*a)[72], int *n, const char *id) {
    if (!id[0] || ob_in(a, *n, id)) return;
    if (*n >= OB_N) { memmove(a[0], a[1], 72 * (OB_N - 1)); (*n)--; }
    snprintf(a[(*n)++], 72, "%s", id);
}
static void ob_del(char (*a)[72], int *n, const char *id) { for (int i = 0; i < *n; i++) if (!strcmp(a[i], id)) { memmove(a[i], a[i + 1], 72 * (size_t)(*n - i - 1)); (*n)--; return; } }
static int pend_find(const char *id) { for (int i = 0; i < g_pend_n; i++) if (!strcmp(g_pend[i].id, id)) return i; return -1; }
static void pend_add(const char *id, const char *fp) {
    if (!id[0] || pend_find(id) >= 0) return;
    if (g_pend_n >= OB_N) { memmove(&g_pend[0], &g_pend[1], sizeof g_pend[0] * (OB_N - 1)); g_pend_n--; }
    snprintf(g_pend[g_pend_n].id, 72, "%s", id); snprintf(g_pend[g_pend_n].fp, 72, "%s", fp); g_pend_n++;
}
int chat_pending_for_fp(const char *fp) { int n = 0; for (int i = 0; i < g_pend_n; i++) if (!strcmp(g_pend[i].fp, fp)) n++; return n; }

/* refaz relógio/tiques das mensagens que estão na tela (depois de reabrir o app ou quando o estado do core chega) */
static bool ob_apply(app_state_t *s) {
    bool ch = false;
    for (int i = 0; i < s->msg_count; i++) {
        if (!s->msgs[i].mine) continue;
        const char *id = s->msgs[i].id; const int st = attach_state(id);
        const int want = pend_find(id) >= 0 ? ATT_SENT : ob_in(g_dlv, g_dlv_n, id) ? ATT_DELIVERED : ob_in(g_fld, g_fld_n, id) ? ATT_FAILED : 0;
        if (!want || st == want || st == ATT_SENDING) continue;
        if (want == ATT_SENT && (st == ATT_WIRE || st == ATT_DELIVERED)) continue;   /* já mais adiante nesta sessão */
        attach_mark(id, want); ch = true;
    }
    return ch;
}
bool chat_outbox_event(app_state_t *s, const jval *j) {   /* outbox.state do core */
    g_pend_n = g_dlv_n = g_fld_n = 0;
    const jval *p = json_get(j, "pending"), *d = json_get(j, "delivered"), *f = json_get(j, "failed");
    for (int i = 0; p && i < json_len(p); i++) { const jval *e = json_at(p, i); pend_add(json_str(e, "id", ""), json_str(e, "to_fp", "")); }
    for (int i = 0; d && i < json_len(d); i++) { const jval *e = json_at(d, i); if (e && e->type == J_STR) ob_push(g_dlv, &g_dlv_n, e->str); }
    for (int i = 0; f && i < json_len(f); i++) { const jval *e = json_at(f, i); if (e && e->type == J_STR) ob_push(g_fld, &g_fld_n, e->str); }
    return ob_apply(s);
}
void chat_outbox_acked(const char *id) {   /* chat.ack: entregue */
    const int k = id[0] ? pend_find(id) : -1;
    if (k >= 0) { memmove(&g_pend[k], &g_pend[k + 1], sizeof g_pend[0] * (size_t)(g_pend_n - k - 1)); g_pend_n--; }
    if (id[0]) { ob_push(g_dlv, &g_dlv_n, id); ob_del(g_fld, &g_fld_n, id); }
}

/* a cada ~4 s: com mensagem pendente para um amigo que está online (presença) e fora da sala, reabre o canal direto (toque na caixa dele);
 * é o caminho da 1.4.1/1.4.2 (dm.open, malha só de DataChannel) e o core entrega a fila ao autenticar. A cada 30 s relê a fila do core
 * (o que passou de 30 dias vira "falhou"). */
static void ob_tick(app_state_t *s) {
    static ULONGLONG t_open, t_get; const ULONGLONG now = GetTickCount64();
    if (!t_get || now - t_get > 30000) { t_get = now; core_send("{\"cmd\":\"outbox.get\"}"); }
    char qfp[72];
    if (now - t_open < 4000 || (!g_pend_n && !attach_queued_fp(0, qfp, sizeof qfp))) return;
    t_open = now;
    for (int i = 0; attach_queued_fp(i, qfp, sizeof qfp); i++) {   /* anexo esperando na fila local: o mesmo reabrir do canal direto */
        const person_t *f = friend_by_fp(s, qfp);
        if (f && friends_online(s, f) && !in_call_with(s, f->fp)) dm_open_link(s, f);
    }
    for (int i = 0; i < g_pend_n; i++) {
        const person_t *f = friend_by_fp(s, g_pend[i].fp);
        if (f && friends_online(s, f) && !in_call_with(s, f->fp)) dm_open_link(s, f);
    }
}

/* mensagens recebidas: fila gravada no backend local (INSERT OR IGNORE: o mesmo id nunca duplica) */
typedef struct { char channel[160], id[72], author[96], content[1100], created[40]; } inmsg_t;
static inmsg_t g_in[64]; static int g_in_n; static char g_in_cur[160]; static bool g_in_touch;

static void iso_from_ms(double ms, char *out, size_t cap) {
    time_t t = (time_t)(ms / 1000.0); struct tm tmv; if (gmtime_s(&tmv, &t)) { now_iso(out, cap); return; }
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)fmod(ms, 1000.0));
}
static bool id_ok(const char *id) {
    size_t n = strlen(id); if (n < 1 || n > 64) return false;
    for (size_t i = 0; i < n; i++) { char c = id[i]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_')) return false; }
    return true;
}

bool chat_in_message(app_state_t *s, const jval *j, bool history) {
    const char *fp = json_str(j, "from_fp", ""); const person_t *f = friend_by_fp(s, fp);
    const char *id = json_str(j, "id", ""), *content = json_str(j, "content", "");
    if (!f) { if (!history) notify_chat_message(s, json_str(j, "channel", ""), json_str(j, "author", ""), content); return false; }   /* quem não é amigo: sem histórico, só o aviso de antes */
    if (!id_ok(id) || !content[0]) return false;
    const jval *mv = json_get(j, "mine"); const bool mine = history && mv && mv->type == J_BOOL && mv->b;
    char shown[640], summ[160]; const char *notif = content;
    if (ginvite_is(content)) {   /* convite de grupo: o código fica só em invites.dat; na conversa e no banco vai o texto curto */
        if (!ginvite_receive(s, id, f->id, content, mine, shown, sizeof shown)) return false;
        ginvite_summary(shown, summ, sizeof summ); notif = summ; content = shown;
    }
    char ch[160]; dm_channel(s, f->id, ch, sizeof ch);
    const bool active = !strcmp(ch, s->chat_channel) || (s->active_friend >= 0 && s->active_friend < s->friend_count && &s->friends[s->active_friend] == f);
    if (active) for (int i = 0; i < s->msg_count; i++) if (!strcmp(s->msgs[i].id, id)) return false;   /* já está na tela */
    const jval *tv = json_get(j, "ts"); char iso[40];
    if (tv && tv->type == J_NUM && tv->num > 0) iso_from_ms(tv->num, iso, sizeof iso); else now_iso(iso, sizeof iso);
    const char *author = mine ? s->user_name : json_str(j, "author", f->name);
    if (!history) {   /* aviso uma vez por mensagem (ela pode chegar pelos dois canais) */
        static char seen[16][72]; static int sn; bool dup = false;
        for (int i = 0; i < 16; i++) if (!strcmp(seen[i], id)) dup = true;
        if (dup) return false;
        snprintf(seen[sn++ % 16], sizeof seen[0], "%s", id);
        notify_chat_message(s, ch, author, notif);
    }
    if (!s->token[0]) {   /* sem backend (capturas e testes de tela): só mostra */
        if (active && s->msg_count < MAX_MSGS) add_msg(s, id, author, content, iso, mine);
        return true;
    }
    if (g_in_n >= 64) return false;   /* fila cheia: o histórico do amigo volta a oferecer na próxima autenticação */
    inmsg_t *m = &g_in[g_in_n++];
    snprintf(m->channel, sizeof m->channel, "%s", ch); snprintf(m->id, sizeof m->id, "%s", id); snprintf(m->author, sizeof m->author, "%.79s", author);
    snprintf(m->content, sizeof m->content, "%.1000s", content); snprintf(m->created, sizeof m->created, "%s", iso);
    return false;
}

/* mensagens que a UI mesma escreve (convite de grupo): o core recebe na hora (fila local), o banco de mensagens quando estiver livre */
typedef struct { char channel[160], id[72], shown[640], created[40]; } outmsg_t;
static outmsg_t g_out[8]; static int g_out_n;
bool chat_dm_post(app_state_t *s, int fi, const char *wire, const char *package) {
    if (fi < 0 || fi >= s->friend_count || !s->friends[fi].fp[0] || core_state() != CORE_READY || g_out_n >= 8) return false;
    const person_t *f = &s->friends[fi];
    char id[40], iso[40], shown[640]; new_id(id, sizeof id); now_iso(iso, sizeof iso);
    snprintf(shown, sizeof shown, "%s", wire);
    if (ginvite_is(wire) && !ginvite_receive(s, id, f->id, wire, true, shown, sizeof shown)) return false;
    const size_t pl = package ? strlen(package) : 0, cap = strlen(wire) * 2 + pl * 2 + 400;
    char *line = malloc(cap), *t = malloc(strlen(wire) * 2 + 8), *pk = malloc(pl * 2 + 8); if (!line || !t || !pk) { free(line); free(t); free(pk); return false; }
    char fp[160]; json_escape(f->fp, fp, sizeof fp); json_escape(wire, t, strlen(wire) * 2 + 8); pk[0] = 0; if (pl) json_escape(package, pk, pl * 2 + 8);
    /* o pacote (convite de grupo, ~32 KB) vai em campo próprio, nunca no texto da mensagem (limite de 4000 caracteres) */
    if (pl) snprintf(line, cap, "{\"cmd\":\"chat.send\",\"to_fp\":\"%s\",\"id\":\"%s\",\"text\":\"%s\",\"package\":\"%s\"}", fp, id, t, pk);
    else snprintf(line, cap, "{\"cmd\":\"chat.send\",\"to_fp\":\"%s\",\"id\":\"%s\",\"text\":\"%s\"}", fp, id, t);
    if (!in_call_with(s, f->fp)) dm_open_link(s, f);
    core_send(line); free(line); free(t); free(pk);
    pend_add(id, f->fp); attach_mark(id, ATT_SENT);
    outmsg_t *o = &g_out[g_out_n++]; dm_channel(s, f->id, o->channel, sizeof o->channel);
    snprintf(o->id, sizeof o->id, "%s", id); snprintf(o->shown, sizeof o->shown, "%s", shown); snprintf(o->created, sizeof o->created, "%s", iso);
    return true;
}
static void out_pump(app_state_t *s) {
    if (!g_out_n || !s->token[0] || s->busy) return;
    outmsg_t o = g_out[0]; memmove(&g_out[0], &g_out[1], sizeof g_out[0] * (size_t)(g_out_n - 1)); g_out_n--;
    char a[200], c[1400], b[2200], path[256];
    json_escape(s->user_name, a, sizeof a); json_escape(o.shown, c, sizeof c);
    snprintf(b, sizeof b, "{\"id\":\"%s\",\"channelId\":\"%s\",\"author\":\"%s\",\"content\":\"%s\",\"createdAt\":\"%s\",\"reactions\":{}}", o.id, o.channel, a, c, o.created);
    snprintf(path, sizeof path, "/p2p/messages/%s", o.channel);
    if (!strcmp(o.channel, s->chat_channel)) add_msg(s, o.id, s->user_name, o.shown, o.created, true);
    attach_set_inflight(o.id);
    app_begin(s, OP_MSG_SEND, "POST", path, b);
}

void chat_in_pump(app_state_t *s) {
    out_pump(s); if (s->busy) return;
    if (!g_in_n || !s->token[0]) return;
    inmsg_t m = g_in[0]; memmove(&g_in[0], &g_in[1], sizeof g_in[0] * (size_t)(g_in_n - 1)); g_in_n--;
    char a[240], c[2200], b[3000], path[256];
    json_escape(m.author, a, sizeof a); json_escape(m.content, c, sizeof c);
    snprintf(b, sizeof b, "{\"id\":\"%s\",\"channelId\":\"%s\",\"author\":\"%s\",\"content\":\"%s\",\"createdAt\":\"%s\",\"reactions\":{}}", m.id, m.channel, a, c, m.created);
    snprintf(path, sizeof path, "/p2p/messages/%s?insertOnly=true", m.channel);
    snprintf(g_in_cur, sizeof g_in_cur, "%s", m.channel);
    if (!strcmp(m.channel, s->chat_channel)) g_in_touch = true;
    app_begin(s, OP_MSG_RECV, "POST", path, b);
}
void chat_in_done(app_state_t *s) {
    if (!g_in_n && g_in_touch) { g_in_touch = false; if (!strcmp(g_in_cur, s->chat_channel)) s->reload_msgs = true; }
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
    if (!strncmp(s->draft, "briks-grp1.", 11)) {   /* o convite longo não cabe numa mensagem de texto (a caixa corta em 255 caracteres e ele chegaria corrompido) */
        snprintf(s->notice, sizeof s->notice, "Convite de grupo não vai como texto: use Convidar no grupo (envia o cartão com o código inteiro) ou cole o código em Entrar com convite.");
        settings_notice(s->notice); return;
    }
    char id[40], iso[40], a[200], c[1100], b[1700], path[256];
    new_id(id, sizeof id); now_iso(iso, sizeof iso);
    json_escape(s->user_name, a, sizeof a); json_escape(s->draft, c, sizeof c);
    snprintf(b, sizeof b, "{\"id\":\"%s\",\"channelId\":\"%s\",\"author\":\"%s\",\"content\":\"%s\",\"createdAt\":\"%s\",\"reactions\":{}}",
             id, s->chat_channel, a, c, iso);
    snprintf(path, sizeof path, "/p2p/messages/%s", s->chat_channel);
    char c_text[256]; snprintf(c_text, sizeof c_text, "%s", s->draft);
    add_msg(s, id, s->user_name, s->draft, iso, true);   /* otimista: some se a gravação falhar */
    msgui_after_send(id);                                 /* liga a resposta pendente (citação) ao id novo */
    s->draft[0] = 0;
    attach_set_inflight(id);
    app_begin(s, OP_MSG_SEND, "POST", path, b);
    /* DM: o mesmo id segue pelo canal assinado, com ou sem sala de voz. Se o amigo não está na chamada, o core abre um canal direto com ele
     * (toque na caixa de chegada dele, depois malha P2P só de DataChannel); sem ele online a mensagem espera na caixa de saída deste PC.
     * Estados da mensagem: só neste PC (check) -> enviada (chat.sent) -> entregue (chat.ack, check duplo). */
    if (s->active_friend >= 0 && s->active_friend < s->friend_count && s->friends[s->active_friend].fp[0] && core_state() == CORE_READY) {
        pend_add(id, s->friends[s->active_friend].fp);   /* pendente até o ACK (o core guarda a cópia assinada em disco) */
        if (!in_call_with(s, s->friends[s->active_friend].fp)) dm_open_link(s, &s->friends[s->active_friend]);
        char fp[160], t[1100], line[1500]; json_escape(s->friends[s->active_friend].fp, fp, sizeof fp); json_escape(c_text, t, sizeof t);
        snprintf(line, sizeof line, "{\"cmd\":\"chat.send\",\"to_fp\":\"%s\",\"id\":\"%s\",\"text\":\"%s\"}", fp, id, t);
        core_send(line);
    }
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
    ob_apply(s);   /* depois de reabrir o app: relógio/tiques voltam do que o core guardou */
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
      if (getenv("BRIKS_DEMO_GROSTER")) groups_demo_roster();   /* captura: quem está em cada sala de voz, com um ao vivo */
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
    if (getenv("BRIKS_DEMO_P2P")) { snprintf(s->friends[0].fp, sizeof s->friends[0].fp, "fp-demo-0"); s->in_room = true; }   /* captura: chamada aberta e identidade conhecida (menu Desconectar P2P) */
    s->active_group = -1; s->active_friend = -1;
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
        s->screen = SCREEN_CALL; s->mic_on = g_sys.mic_on;
        add_msg(s, "m1", "Marina Costa", "Oi! Você entra na sala às 21h?", "2026-11-09T23:41:00.000Z", false);
        add_msg(s, "m2", "Ygor", "Entro sim, só terminar o jantar.", "2026-11-09T23:42:00.000Z", true);
        if (!strcmp(mode, "call-chat")) { s->call_view = 1; for (int i = 0; i < s->msg_count; i++) call_chat_add(s->msgs[i].id, s->msgs[i].author, s->msgs[i].content, 1794267660000LL + i * 60000LL, s->msgs[i].mine); }   /* chat da sala de exemplo */
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
void chat_dm_channel(const app_state_t *s, const char *peer, char *out, size_t cap) { dm_channel(s, peer, out, cap); }
