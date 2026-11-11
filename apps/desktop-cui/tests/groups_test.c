/* Teste do save de grupos (ui_groups.c) sem janela: carrega um grupo "assinado" do backend, edita (canal, admin, membro) e confere que o JSON
 * enviado preserva os campos do protocolo P2P (concessões, revogações, segredo) e troca só o que a interface edita. */
#include "../src/ui_groups.c"
#include <assert.h>
briks_theme_t g_th; int g_perf;
int briks_input_ex(cui_ctx *ui, const char *id, char *buf, size_t cap, const cui_input_opts *o) { return cui_input_ex(ui, id, buf, cap, o); }   /* o app real (msgui.c) acrescenta o menu de clique direito */
static char sent[8192]; static char sent_path[160]; static int sent_n;
void app_begin(app_state_t *s, op_t op, const char *m, const char *p, const char *b) { (void)s; (void)op; (void)m; snprintf(sent_path, sizeof sent_path, "%s", p); snprintf(sent, sizeof sent, "%s", b); sent_n++; }
void rtc_join_room(app_state_t *s, const char *r) { (void)s; (void)r; }
char g_tip[160]; void app_open_group(app_state_t *s, int i) { s->active_group = i; } bool friends_online(const app_state_t *s, const person_t *p) { (void)s; (void)p; return false; }
static char core_sent[16384]; static int core_n;
core_status_t core_state(void) { return CORE_STOPPED; }
bool core_send(const char *l) { snprintf(core_sent, sizeof core_sent, "%s", l); core_n++; return true; }
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
    /* ---- grupo P2P assinado: montagem do snapshot, aplicação, convite e fila de gravações ---- */
    snprintf(s.rtc_fp, sizeof s.rtc_fp, "fpME"); snprintf(s.my_jwk, sizeof s.my_jwk, "{\"kty\":\"EC\",\"crv\":\"P-256\",\"x\":\"mx\",\"y\":\"my\"}");
    groups_load(&s, GROUP);
    char why[120] = ""; char *snap = snap_build(&s, &s.groups[0], 1, why, sizeof why);
    CHECK(!snap && strstr(why, "Ana"));                                       /* sem identidade publicada não dá para assinar */
    snprintf(s.groups[0].m[1].fp, sizeof s.groups[0].m[1].fp, "fpANA");
    snap = snap_build(&s, &s.groups[0], 7, why, sizeof why);
    CHECK(snap && strstr(snap, "\"owner_fp\":\"fpME\"") && strstr(snap, "\"version\":7") && strstr(snap, "\"fp\":\"fpME\",\"name\":\"Eu\",\"admin\":true,\"jwk\":{\"kty\""));
    CHECK(snap && strstr(snap, "{\"fp\":\"fpANA\",\"name\":\"Ana\",\"admin\":false}") && strstr(snap, "{\"id\":\"c2\",\"name\":\"Voz\",\"kind\":\"voice\"}"));
    free(snap);
    static char SN[3000];
    snprintf(SN, sizeof SN, "{\"v\":1,\"group_id\":\"g1\",\"name\":\"Renomeado\",\"owner_fp\":\"fpME\",\"version\":8,\"ts\":1,\"members\":["
        "{\"fp\":\"fpME\",\"name\":\"Eu\",\"admin\":true,\"jwk\":{\"kty\":\"EC\",\"crv\":\"P-256\",\"x\":\"mx\",\"y\":\"my\"}},{\"fp\":\"fpANA\",\"name\":\"Ana\",\"admin\":false},{\"fp\":\"fpBIA\",\"name\":\"Bia\",\"admin\":false}],"
        "\"channels\":[{\"id\":\"c1\",\"name\":\"geral\",\"kind\":\"text\"},{\"id\":\"v9\",\"name\":\"Sala\",\"kind\":\"voice\"}],\"signer_fp\":\"fpME\",\"signature\":\"sig\"}");
    jval *sn = json_parse(SN);
    snap_apply(&s, &s.groups[0], sn);
    CHECK(!strcmp(s.groups[0].name, "Renomeado") && s.groups[0].nm == 3 && !strcmp(s.groups[0].m[0].id, "me") && !strcmp(s.groups[0].m[1].id, "u2") && !strcmp(s.groups[0].m[2].id, "fpBIA"));
    CHECK(s.groups[0].nch == 2 && s.groups[0].ch[1].voice && !strcmp(s.groups[0].ch[1].room, "v9") && s.groups[0].is_owner && s.groups[0].can_moderate);
    /* quem aceita o convite: o grupo novo nasce com o snapshot dentro, eu com o meu user_id e o dono pela impressão digital */
    static app_state_t s2; memset(&s2, 0, sizeof s2); snprintf(s2.user_id, sizeof s2.user_id, "meu-id"); snprintf(s2.rtc_fp, sizeof s2.rtc_fp, "fpBIA");
    char *doc = doc_from_snap(&s2, sn, SN);
    CHECK(doc && strstr(doc, "\"ownerPeerId\":\"fpME\"") && strstr(doc, "\"peerId\":\"meu-id\"") && strstr(doc, "\"briksSnap\":{\"v\":1") && strstr(doc, "\"voiceRoomId\":\"v9\"") && strstr(doc, "\"administratorPeerIds\":[\"fpME\"]"));
    jval *dj = doc ? json_parse(doc) : NULL; CHECK(dj != NULL); json_free(dj); free(doc);
    static char ev[4000];
    snprintf(ev, sizeof ev, "{\"ev\":\"group.result\",\"op\":\"verify\",\"ok\":true,\"tag\":\"invite\",\"decision\":\"invite\",\"snapshot\":%s}", SN);
    jval *e1 = json_parse(ev); sent_n = 0;
    groups_core_event(&s2, e1, "group.result"); json_free(e1);
    CHECK(sent_n == 0);                                                       /* só enfileira; grava no próximo quadro */
    groups_tick(&s2);
    CHECK(sent_n == 1 && !strcmp(sent_path, "/p2p/groups") && strstr(sent, "\"groupId\":\"g1\""));
    jval *e2 = json_parse("{\"ev\":\"group.result\",\"op\":\"verify\",\"ok\":false,\"tag\":\"invite\",\"message\":\"esse convite não é para você\"}");
    groups_core_event(&s2, e2, "group.result"); json_free(e2);
    CHECK(strstr(s2.join_err, "não é para você"));
    /* atualização recebida de um peer: aplica e grava com o snapshot novo; "versão antiga" (ok=false) não faz nada */
    groups_load(&s, GROUP); sent_n = 0;
    snprintf(ev, sizeof ev, "{\"ev\":\"group.result\",\"op\":\"verify\",\"ok\":true,\"tag\":\"dc\",\"decision\":\"update\",\"snapshot\":%s}", SN);
    jval *e3 = json_parse(ev); groups_core_event(&s, e3, "group.result"); json_free(e3);
    groups_tick(&s);
    CHECK(sent_n == 1 && strstr(sent, "\"name\":\"Renomeado\"") && strstr(sent, "\"briksSnap\":{\"v\":1") && strstr(sent, "\"administratorGrants\":[{\"sig\":\"abc\"}]"));
    /* snapshot que chega pelo DataChannel: só vira pergunta ao core se eu já confio num estado desse grupo */
    jval *e4 = json_parse("{\"ev\":\"group.snapshot\",\"from_fp\":\"x\",\"snapshot\":{\"group_id\":\"g1\"}}"); core_n = 0;
    groups_core_event(&s, e4, "group.snapshot");
    CHECK(core_n == 0);                                                       /* o grupo de teste ainda não tem briksSnap guardado */
    char gtrust[6000]; snprintf(gtrust, sizeof gtrust, "[{\"groupId\":\"g1\",\"name\":\"N\",\"channels\":[],\"members\":[],\"ownerPeerId\":\"me\",\"administratorPeerIds\":[],\"briksSnap\":%s}]", SN);
    groups_load(&s, gtrust);
    groups_core_event(&s, e4, "group.snapshot"); json_free(e4);
    CHECK(core_n == 1 && strstr(core_sent, "\"cmd\":\"group.verify\"") && strstr(core_sent, "\"trusted\":{\"v\":1") && strstr(core_sent, "\"tag\":\"dc\""));
    json_free(sn);
    /* canais: renomear, apagar (sempre sobra um de texto), membro: admin, remover, duplicados */
    groups_load(&s, GROUP); s.active_group = 0; sent_n = 0;
    rename_channel(&s, 0, 0, "novidades");
    CHECK(!strcmp(s.groups[0].ch[0].name, "novidades") && sent_n == 1 && strstr(sent, "\"name\":\"novidades\"") && strstr(sent, "\"x\":1"));
    CHECK(last_text_channel(&s.groups[0], 0) && !last_text_channel(&s.groups[0], 1));
    remove_channel(&s, 0, 0); CHECK(s.groups[0].nch == 2);                       /* único canal de texto: recusa */
    s.groups[0].m[1].admin = false; set_admin(&s, 0, 1, true); CHECK(s.groups[0].m[1].admin);
    s.groups[0].nm = 3; s.groups[0].m[2] = s.groups[0].m[1]; CHECK(dup_count(&s.groups[0]) == 1);
    s.busy = false; remove_duplicates(&s, 0); CHECK(s.groups[0].nm == 2 && dup_count(&s.groups[0]) == 0);
    s.busy = false; remove_member(&s, 0, 0); CHECK(s.groups[0].nm == 2);          /* o dono não sai */
    s.busy = false; remove_member(&s, 0, 1); CHECK(s.groups[0].nm == 1);
    s.groups[0].can_moderate = s.groups[0].is_owner = false; s.busy = false; rename_channel(&s, 0, 0, "x"); CHECK(strcmp(s.groups[0].ch[0].name, "x"));   /* membro comum não edita */
    CHECK(groups_probe(&s, "grp_nm") == 1 && groups_probe(&s, "grp_moderate") == 0);
    printf(fails ? "groups_test: %d falha(s)\n" : "groups_test: ok\n", fails);
    return fails ? 1 : 0;
}
