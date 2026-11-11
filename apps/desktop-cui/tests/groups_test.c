/* Teste do save de grupos (ui_groups.c) sem janela: carrega um grupo "assinado" do backend, edita (canal, admin, membro) e confere que o JSON
 * enviado preserva os campos do protocolo P2P (concessões, revogações, segredo) e troca só o que a interface edita. */
#include "../src/ui_groups.c"
#include <assert.h>
briks_theme_t g_th; int g_perf;
static char sent[8192]; static char sent_path[160]; static int sent_n;
void app_begin(app_state_t *s, op_t op, const char *m, const char *p, const char *b) { (void)s; (void)op; (void)m; snprintf(sent_path, sizeof sent_path, "%s", p); snprintf(sent, sizeof sent, "%s", b); sent_n++; }
void rtc_join_room(app_state_t *s, const char *r) { (void)s; (void)r; }
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FALHOU: %s (linha %d)\n  enviado: %s\n", #c, __LINE__, sent); fails++; } } while (0)

static const char *GROUP = "[{\"groupId\":\"g1\",\"name\":\"Noite\",\"avatar\":null,\"channels\":[{\"id\":\"c1\",\"name\":\"geral\",\"kind\":\"text\",\"x\":1},{\"id\":\"c2\",\"name\":\"Voz\",\"type\":\"voice\"}],"
    "\"members\":[{\"peerId\":\"me\",\"displayName\":\"Eu\",\"publicKey\":{\"kty\":\"EC\"}},{\"peerId\":\"u2\",\"displayName\":\"Ana\"}],\"joinedAt\":5,\"ownerPeerId\":\"me\",\"membershipVersion\":3,\"manifestVersion\":4,"
    "\"administratorPeerIds\":[\"me\"],\"removedPeerIds\":[],\"removedMembers\":[],\"administratorEpoch\":2,\"administratorGrants\":[{\"sig\":\"abc\"}],\"revocations\":[],\"rendezvousVersion\":1,\"rendezvousSecret\":\"sec\"}]";

int main(void) {
    static app_state_t s; memset(&s, 0, sizeof s);
    snprintf(s.user_id, sizeof s.user_id, "me");
    groups_load(&s, GROUP);
    CHECK(s.group_count == 1 && s.groups[0].nch == 2 && s.groups[0].nm == 2 && s.groups[0].is_owner && s.groups[0].ch[1].voice);   /* "type" antigo também vira voz */
    s.active_group = 0;
    snprintf(s.chan_name, sizeof s.chan_name, "novidades"); s.chan_kind = 0;
    add_channel(&s, 0);
    CHECK(sent_n == 1 && !strcmp(sent_path, "/p2p/groups"));
    CHECK(strstr(sent, "\"administratorGrants\":[{\"sig\":\"abc\"}]"));      /* campos do protocolo intactos */
    CHECK(strstr(sent, "\"rendezvousSecret\":\"sec\"") && strstr(sent, "\"administratorEpoch\":2"));
    CHECK(strstr(sent, "\"name\":\"novidades\",\"kind\":\"text\""));
    CHECK(strstr(sent, "\"x\":1"));                                           /* campo extra do canal preservado */
    CHECK(strstr(sent, "\"publicKey\":{\"kty\":\"EC\"}"));                    /* dados do membro preservados */
    CHECK(strstr(sent, "\"membershipVersion\":4") && strstr(sent, "\"manifestVersion\":5"));
    CHECK(strstr(sent, "\"kind\":\"voice\",\"voiceRoomId\":\"c2\""));         /* voz herda a sala do id quando não havia voiceRoomId */
    /* recarrega o que foi enviado (simula o GET) e promove a Ana */
    char copy1[8192]; snprintf(copy1, sizeof copy1, "[%s]", sent);
    groups_load(&s, copy1);
    CHECK(s.groups[0].nch == 3);
    set_admin(&s, 0, 1, true);
    CHECK(strstr(sent, "\"administratorPeerIds\":[\"me\",\"u2\"]"));
    /* um administrador (não dono) não remove outro administrador nem promove */
    s.groups[0].is_owner = false; sent_n = 0;
    set_admin(&s, 0, 1, false); remove_member(&s, 0, 1);
    CHECK(sent_n == 0);
    s.groups[0].is_owner = true;
    remove_member(&s, 0, 0);                  /* dono não se remove */
    CHECK(sent_n == 0);
    remove_member(&s, 0, 1);
    CHECK(sent_n == 1 && !strstr(sent, "u2"));
    /* sempre sobra um canal de texto */
    groups_load(&s, GROUP); sent_n = 0;
    remove_channel(&s, 0, 0);
    CHECK(sent_n == 0);
    remove_channel(&s, 0, 1);
    CHECK(sent_n == 1 && !strstr(sent, "\"Voz\""));
    char create[2200]; snprintf(s.new_group, sizeof s.new_group, "Novo \"grupo\""); groups_create_json(&s, create, sizeof create);
    CHECK(strstr(create, "\"kind\":\"text\"") && strstr(create, "\"kind\":\"voice\"") && strstr(create, "Novo \\\"grupo\\\""));
    printf(fails ? "groups_test: %d falha(s)\n" : "groups_test: ok\n", fails);
    return fails ? 1 : 0;
}
