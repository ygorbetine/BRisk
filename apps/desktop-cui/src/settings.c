/* Configurações (paridade com o SettingsView do Electron 1.2.2): lateral de seções + cartões de vidro, uma seção por vez.
 * Persistência local em %APPDATA%\Briks-CUI\settings.cfg (linhas chave=valor, gravadas quando algo muda).
 * Cada controle é marcado no relatório como "efeito real" ou "sem efeito real ainda" (UI + persistência, sem backend). */
#include "briks_ui.h"
#include "volume_ctl.h"
#include "netdiag.h"
#include "update.h"
#include "core.h"
#include "sysint.h"
#include "hotkeys.h"
#include "storage.h"
#include "callhist.h"
#include "json.h"
void update_settings_cards(cui_ctx *ui, app_state_t *s); int update_probe(const char *n);   /* ui_update.c */
int overlay_tile_count(void); const char *overlay_tile_name(int j); bool overlay_tile_enabled(int j); void overlay_tile_enable(int j, bool on);   /* overlay_host.c */
void overlay_edit_toggle(void); bool overlay_edit_active(void); void overlay_edit_set(bool on); void settings_ovl_reset(void);   /* overlay_host.c */
void nettest_card(cui_ctx *ui, app_state_t *s); int nettest_probe(const char *n, app_state_t *s);   /* ui_nettest.c */
int overlay_state(void); int overlay_probe(const char *n);   /* overlay_host.c */
void data_refresh(void); void data_ui(cui_ctx *ui, app_state_t *s); int data_probe(const char *n);   /* settings_data.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include <mmsystem.h>
#include <shlobj.h>

typedef struct {
    int mic, out;                 /* índices nas listas de dispositivos (0 = padrão do sistema) */
    char mic_name[64], out_name[64], cam_name[96];   /* cam_name vazio = câmera padrão */
    float in_vol, out_vol;        /* 0..100 */
    int noise_off;                /* 0 filtro leve de ruído ligado (Padrão), 1 desligado */
    bool gate_on; float gate_db;  /* porta de ruído, -70..-20 dB */
    bool echo, excl_audio;
    int idle_min;                 /* CHM-35: minutos sem falar até sair da chamada (0 = nunca); padrão 60 */
    int fw_choice;                /* Firewall: 0 não decidido, 1 liberado, 2 agora não, 3 não perguntar */
    bool call_mode_set;           /* o usuário escolheu o modo da chamada (sem isso, 2/SFU gravado é o padrão antigo e migra para Automático) */
    int net_pref, call_mode;      /* conexão: auto/internet direta/VPN privada; chamada: auto/P2P/SFU */
    bool ov_on; float ov_opacity; bool ov_live, ov_voice, ov_status, ov_mute, ov_selfshare, ov_through; int ov_corner;   /* sobreposição: canto 0 sup. direito, 1 sup. esquerdo, 2 inf. direito, 3 inf. esquerdo */
    char key_mute[24], key_deafen[24], key_stream[24], key_ptt[24];
    bool ptt_on; float ptt_delay;
    bool nt_online, nt_stream;
    bool telemetry;
    int text_size;
    char ov_tiles[480], ov_toff[120];   /* miniaturas de transmissão do overlay: "chave|monitor|x|y|%;..." (posição/escala) e chaves ocultas "chave;chave" */
    char grp_coll[190];   /* grupos recolhidos na lateral: hashes FNV-1a (8 hex) separados por ";" (1.4.2: persiste entre reinicios) */
    char ov_lay[190], ov_dev[48];   /* posições da sobreposição por monitor ("dev|x|y|w;...") e o monitor ativo */
} settings_t;
settings_t g_set;
int settings_call_mode(void) { return g_set.call_mode; }
int settings_idle_min(void) { return g_set.idle_min < 0 ? 0 : g_set.idle_min; }
int settings_net_pref(void) { return g_set.net_pref; }
int settings_fw_choice(void) { return g_set.fw_choice; }
void settings_fw_set(int c) { g_set.fw_choice = c; }
static void coll_hash(const char *gid, char out[9]) { unsigned h = 2166136261u; for (const unsigned char *p = (const unsigned char *)gid; *p; p++) { h ^= *p; h *= 16777619u; } snprintf(out, 9, "%08x", h); }
bool settings_group_collapsed(const char *gid) {
    char h[9]; coll_hash(gid, h); char b[190]; snprintf(b, sizeof b, "%s", g_set.grp_coll);
    for (char *e = strtok(b, ";"); e; e = strtok(NULL, ";")) if (!strcmp(e, h)) return true;
    return false;
}
void settings_group_collapse(const char *gid, bool on) {
    char h[9]; coll_hash(gid, h); char keep[190] = "", b[190]; snprintf(b, sizeof b, "%s", g_set.grp_coll);
    for (char *e = strtok(b, ";"); e; e = strtok(NULL, ";")) { if (!strcmp(e, h)) continue; size_t n = strlen(keep); snprintf(keep + n, sizeof keep - n, "%s%s", n ? ";" : "", e); }
    if (on) { size_t n = strlen(keep); while (n + 10 > sizeof keep - 1) { char *sc = strchr(keep, ';'); if (!sc) break; memmove(keep, sc + 1, strlen(sc)); n = strlen(keep); } snprintf(keep + n, sizeof keep - n, "%s%s", n ? ";" : "", h); }   /* sobra: descarta o mais antigo */
    snprintf(g_set.grp_coll, sizeof g_set.grp_coll, "%s", keep);
}
const char *settings_cam_name(void) { return g_set.cam_name; }
bool settings_cam_tab_visible(const app_state_t *s) { return s->settings_open && s->settings_tab == 0; }

static const struct { const char *k; char t; void *p; int cap; } F[] = {
    { "mic", 'i', &g_set.mic }, { "out", 'i', &g_set.out }, { "mic_name", 's', g_set.mic_name, 64 }, { "out_name", 's', g_set.out_name, 64 }, { "cam_name", 's', g_set.cam_name, 96 },
    { "in_vol", 'f', &g_set.in_vol }, { "out_vol", 'f', &g_set.out_vol }, { "noise_off", 'i', &g_set.noise_off }, { "gate_on", 'b', &g_set.gate_on },
    { "gate_db", 'f', &g_set.gate_db }, { "echo", 'b', &g_set.echo }, { "excl_audio", 'b', &g_set.excl_audio }, { "net_pref", 'i', &g_set.net_pref },
    { "call_mode", 'i', &g_set.call_mode }, { "call_mode_set", 'b', &g_set.call_mode_set }, { "fw_choice", 'i', &g_set.fw_choice }, { "ov_on", 'b', &g_set.ov_on }, { "ov_opacity", 'f', &g_set.ov_opacity }, { "ov_live", 'b', &g_set.ov_live },
    { "ov_voice", 'b', &g_set.ov_voice }, { "ov_status", 'b', &g_set.ov_status }, { "ov_mute", 'b', &g_set.ov_mute }, { "ov_selfshare", 'b', &g_set.ov_selfshare }, { "ov_through", 'b', &g_set.ov_through }, { "ov_corner", 'i', &g_set.ov_corner },
    { "key_mute", 's', g_set.key_mute, 24 }, { "key_deafen", 's', g_set.key_deafen, 24 }, { "key_stream", 's', g_set.key_stream, 24 }, { "key_ptt", 's', g_set.key_ptt, 24 },
    { "ptt_on", 'b', &g_set.ptt_on }, { "ptt_delay", 'f', &g_set.ptt_delay }, { "nt_online", 'b', &g_set.nt_online }, { "nt_stream", 'b', &g_set.nt_stream },
    { "telemetry", 'b', &g_set.telemetry }, { "idle_min", 'i', &g_set.idle_min }, { "text_size", 'i', &g_set.text_size },
    { "grp_coll", 's', g_set.grp_coll, 190 }, { "ov_lay", 's', g_set.ov_lay, 190 }, { "ov_dev", 's', g_set.ov_dev, 48 },
    { "ov_tiles", 's', g_set.ov_tiles, 480 }, { "ov_toff", 's', g_set.ov_toff, 120 },
};
#define NF ((int)(sizeof F / sizeof *F))

static void defaults(void) {
    memset(&g_set, 0, sizeof g_set);
    g_set.text_size = 1; g_set.idle_min = 60;
    g_set.in_vol = 100; g_set.out_vol = 100; g_set.gate_db = -50; g_set.echo = true; g_set.excl_audio = true;
    g_set.ov_on = true; g_set.ov_opacity = 92; g_set.ov_live = g_set.ov_voice = g_set.ov_status = g_set.ov_mute = g_set.ov_selfshare = g_set.ov_through = true;
    g_set.nt_online = g_set.nt_stream = true; g_set.ptt_delay = 200;
    snprintf(g_set.key_mute, sizeof g_set.key_mute, "Ctrl+Shift+M"); snprintf(g_set.key_deafen, sizeof g_set.key_deafen, "Ctrl+Shift+D");
    snprintf(g_set.key_stream, sizeof g_set.key_stream, "Ctrl+Shift+S");
}

static bool cfg_path(char *out, size_t cap) {
    char ad[MAX_PATH]; if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, ad) != S_OK) return false;
    char dir[MAX_PATH]; snprintf(dir, sizeof dir, "%s\\Briks-CUI", ad); CreateDirectoryA(dir, NULL);
    snprintf(out, cap, "%s\\settings.cfg", dir); return true;
}
static void load(void);
static void load_inner(void);
static void save(void);
static void load(void) { load_inner(); g_text_size = g_set.text_size < 0 ? 0 : g_set.text_size > 3 ? 3 : g_set.text_size; }
static void load_inner(void) {
    defaults();
    { const char *ts = getenv("BRIKS_TEXT_SIZE"); if (ts) g_set.text_size = atoi(ts); }
    { const char *im = getenv("BRIKS_IDLE_MIN"); if (im) g_set.idle_min = atoi(im); }   /* testes: prazo de inatividade em minutos (0 = nunca) */   /* capturas */
    char p[MAX_PATH]; if (getenv("BRIKS_DEMO") || !cfg_path(p, sizeof p)) return;
    FILE *f = fopen(p, "rb"); if (!f) return;
    char ln[200];
    while (fgets(ln, sizeof ln, f)) {
        char *eq = strchr(ln, '='); if (!eq) continue; *eq = 0; char *v = eq + 1; v[strcspn(v, "\r\n")] = 0;
        for (int i = 0; i < NF; i++) if (!strcmp(F[i].k, ln)) {
            if (F[i].t == 'i') *(int *)F[i].p = atoi(v); else if (F[i].t == 'f') *(float *)F[i].p = (float)atof(v);
            else if (F[i].t == 'b') *(bool *)F[i].p = atoi(v) != 0; else snprintf((char *)F[i].p, (size_t)F[i].cap, "%s", v);
        }
    }
    fclose(f);
    const int cm = settings_migrate_call_mode(g_set.call_mode, g_set.call_mode_set);   /* padrão antigo "Sempre SFU" -> Automático (uma vez; quem escolheu fica) */
    if (cm != g_set.call_mode) { g_set.call_mode = cm; save(); }
}
static void save(void) {
    char p[MAX_PATH]; if (getenv("BRIKS_DEMO") || !cfg_path(p, sizeof p)) return;
    FILE *f = fopen(p, "wb"); if (!f) return;
    for (int i = 0; i < NF; i++) {
        if (F[i].t == 'i') fprintf(f, "%s=%d\n", F[i].k, *(int *)F[i].p); else if (F[i].t == 'f') fprintf(f, "%s=%.2f\n", F[i].k, *(float *)F[i].p);
        else if (F[i].t == 'b') fprintf(f, "%s=%d\n", F[i].k, *(bool *)F[i].p ? 1 : 0); else fprintf(f, "%s=%s\n", F[i].k, (char *)F[i].p);
    }
    fclose(f);
}

/* ---- dispositivos de áudio reais (winmm) ---- */
#define MAXDEV 12
static char in_names[MAXDEV + 1][64], out_names[MAXDEV + 1][64];
static const char *in_ptr[MAXDEV + 1], *out_ptr[MAXDEV + 1];
static int in_n, out_n;
static void enum_devices(void) {
    in_n = out_n = 1;
    snprintf(in_names[0], 64, "Padrão do sistema"); snprintf(out_names[0], 64, "Padrão do sistema");
    UINT n = waveInGetNumDevs(); for (UINT i = 0; i < n && in_n <= MAXDEV - 1; i++) {
        WAVEINCAPSW c; if (waveInGetDevCapsW(i, &c, sizeof c) == MMSYSERR_NOERROR) WideCharToMultiByte(CP_UTF8, 0, c.szPname, -1, in_names[in_n++], 64, NULL, NULL); }
    n = waveOutGetNumDevs(); for (UINT i = 0; i < n && out_n <= MAXDEV - 1; i++) {
        WAVEOUTCAPSW c; if (waveOutGetDevCapsW(i, &c, sizeof c) == MMSYSERR_NOERROR) WideCharToMultiByte(CP_UTF8, 0, c.szPname, -1, out_names[out_n++], 64, NULL, NULL); }
    if (getenv("BRIKS_DEMO") && in_n == 1) { snprintf(in_names[1], 64, "Microfone (USB Audio)"); snprintf(in_names[2], 64, "Headset (Realtek)"); in_n = 3; snprintf(out_names[1], 64, "Alto-falantes (Realtek)"); snprintf(out_names[2], 64, "Headset (Realtek)"); out_n = 3; }
    for (int i = 0; i < in_n; i++) in_ptr[i] = in_names[i];
    for (int i = 0; i < out_n; i++) out_ptr[i] = out_names[i];
    if (g_set.mic >= in_n || strcmp(g_set.mic ? in_names[g_set.mic] : "", g_set.mic ? g_set.mic_name : "")) g_set.mic = 0;   /* dispositivo salvo sumiu: volta ao padrão */
    if (g_set.out >= out_n || strcmp(g_set.out ? out_names[g_set.out] : "", g_set.out ? g_set.out_name : "")) g_set.out = 0;
}

/* ---- avisos (erros/avisos do app) ---- */
static char notices[8][120]; static int notice_n;
void settings_notice(const char *text) {
    if (!text || !text[0]) return;
    if (notice_n && !strncmp(notices[notice_n - 1], text, 119)) return;   /* o mesmo aviso em seguida não se repete */
    if (notice_n == 8) { memmove(notices[0], notices[1], 7 * 120); notice_n = 7; }
    snprintf(notices[notice_n++], 120, "%s", text);
}

/* ---- widgets ---- */
#define SCARD(ui, title) CUI__SCOPE((cui_box_begin((ui), CUI__OPTS(cui_box, .width = CUI_FILL, .pad = 14, .gap = 10, .radius = 16, .bg = B_GLASS_CARD, \
    .border = 1, .border_color = B_GLASS_BORDER)), cui_text((ui), (title), .font = CUI_FONT_MEDIUM, .size = 13, .color = B_C3)), cui_box_end(ui))

static void sub(cui_ctx *ui, const char *t) { cui_text(ui, t, .size = 12, .color = B_MUTED); }
static void soon(cui_ctx *ui, const char *what) {   /* o que ainda não tem efeito, dito com clareza */
    char b[200]; snprintf(b, sizeof b, "Em breve: %s", what);
    cui_text(ui, b, .size = 12, .color = g_th.ph);
}
void settings_keys(const char **mute, const char **deafen, const char **stream, const char **ptt, bool *ptt_on, float *delay) { *mute = g_set.key_mute; *deafen = g_set.key_deafen; *stream = g_set.key_stream; *ptt = g_set.key_ptt; *ptt_on = g_set.ptt_on; *delay = g_set.ptt_delay; }
void settings_audio_ex(char *mic, size_t mic_cap, char *out, size_t out_cap, int *noise_off) {
    snprintf(mic, mic_cap, "%s", g_set.mic ? g_set.mic_name : ""); snprintf(out, out_cap, "%s", g_set.out ? g_set.out_name : ""); *noise_off = g_set.noise_off != 0;
}
void settings_overlay(bool *on, float *opacity, int *flags, int *corner) {
    *on = g_set.ov_on; *opacity = g_set.ov_opacity; *corner = g_set.ov_corner < 0 || g_set.ov_corner > 3 ? 0 : g_set.ov_corner;
    *flags = (g_set.ov_live ? 1 : 0) | (g_set.ov_voice ? 2 : 0) | (g_set.ov_status ? 4 : 0) | (g_set.ov_mute ? 8 : 0);
}
bool settings_ovl_through(void) { return g_set.ov_through; }   /* 1.5.20: clique atravessa a sobreposição (padrão); desligado, as miniaturas arrastam sem o modo de edição */
bool settings_ovl_selfshare(void) { return g_set.ov_selfshare; }   /* 1.4.1: aviso "Você está transmitindo a tela" por cima do jogo (padrão ligado) */
void settings_audio(float *in_gain, float *out_gain, bool *gate_on, float *gate_db) { *in_gain = g_set.in_vol / 100.0f; *out_gain = g_set.out_vol / 100.0f; *gate_on = g_set.gate_on; *gate_db = g_set.gate_db; }
/* Posições da sobreposição por monitor (CFG-25): lista "dev|x|y|w;dev|x|y|w" e o monitor ativo (onde o painel foi solto por último). */
bool settings_ovl_place(char *dev, size_t cap, int *x, int *y, int *w) {
    if (!g_set.ov_dev[0]) return false;
    char b[190]; snprintf(b, sizeof b, "%s", g_set.ov_lay);
    for (char *e = strtok(b, ";"); e; e = strtok(NULL, ";")) {
        char d[48]; int a, c, ww;
        char *bar = strchr(e, '|'); if (!bar) continue; *bar = 0; snprintf(d, sizeof d, "%s", e);
        if (sscanf(bar + 1, "%d|%d|%d", &a, &c, &ww) == 3 && !strcmp(d, g_set.ov_dev)) { snprintf(dev, cap, "%s", d); *x = a; *y = c; *w = ww; return true; }
    }
    return false;
}
void settings_ovl_set(const char *dev, int x, int y, int w) {
    char keep[190] = "", b[190]; snprintf(b, sizeof b, "%s", g_set.ov_lay);
    for (char *e = strtok(b, ";"); e; e = strtok(NULL, ";")) {
        if (!strncmp(e, dev, strlen(dev)) && e[strlen(dev)] == '|') continue;   /* troca a entrada deste monitor */
        size_t n = strlen(keep); if (n + strlen(e) + 2 < sizeof keep) snprintf(keep + n, sizeof keep - n, "%s%s", n ? ";" : "", e);
    }
    char ent[80]; snprintf(ent, sizeof ent, "%s|%d|%d|%d", dev, x, y, w);
    char now[190]; snprintf(now, sizeof now, "%s%s%s", keep, keep[0] ? ";" : "", ent);
    while (strlen(now) > 180) { char *sc = strchr(now, ';'); if (!sc) break; memmove(now, sc + 1, strlen(sc)); }   /* sobra: descarta o mais antigo */
    snprintf(g_set.ov_lay, sizeof g_set.ov_lay, "%s", now); snprintf(g_set.ov_dev, sizeof g_set.ov_dev, "%s", dev);
}
/* Barra de edição do overlay: liga/desliga um item ("live", "voice", "status", "mute" ou "all") e muda a opacidade. Vão para g_set e são salvos
 * por settings_tick; Configurações > Sobreposição lê o mesmo g_set, então as duas telas ficam sempre iguais. */
bool settings_ovl_widget(const char *id, bool on) {
    bool all = !strcmp(id, "all"), hit = all;
    if (all || !strcmp(id, "live")) { g_set.ov_live = on; hit = true; }
    if (all || !strcmp(id, "voice")) { g_set.ov_voice = on; hit = true; }
    if (all || !strcmp(id, "status")) { g_set.ov_status = on; hit = true; }
    if (all || !strcmp(id, "mute")) { g_set.ov_mute = on; hit = true; }
    if (all || !strcmp(id, "selfshare")) { g_set.ov_selfshare = on; hit = true; }
    return hit;
}
void settings_ovl_opacity(int pct) { g_set.ov_opacity = (float)(pct < 10 ? 10 : pct > 100 ? 100 : pct); }
/* Miniaturas de transmissão (até 4, cada uma com chave estável da pessoa): posição/escala por monitor e liga/desliga por transmissão. */
bool settings_tile_place(const char *key, char *dev, size_t cap, int *x, int *y, int *pct) {
    char b[480]; snprintf(b, sizeof b, "%s", g_set.ov_tiles); bool got = false;
    for (char *e = strtok(b, ";"); e; e = strtok(NULL, ";")) {
        char k[24], d[48]; int a, c, p; char *t1 = strchr(e, '|'); if (!t1) continue; *t1 = 0; snprintf(k, sizeof k, "%s", e);
        char *t2 = strchr(t1 + 1, '|'); if (!t2) continue; *t2 = 0; snprintf(d, sizeof d, "%s", t1 + 1);
        if (strcmp(k, key) || sscanf(t2 + 1, "%d|%d|%d", &a, &c, &p) != 3) continue;
        if (!got || !strcmp(d, g_set.ov_dev)) { snprintf(dev, cap, "%s", d); *x = a; *y = c; *pct = p; got = true; }   /* vale o do monitor ativo; senão o último salvo */
    }
    return got;
}
void settings_tile_set(const char *key, const char *dev, int x, int y, int pct) {
    char keep[480] = "", b[480]; snprintf(b, sizeof b, "%s", g_set.ov_tiles);
    const size_t kl = strlen(key), dl = strlen(dev);
    for (char *e = strtok(b, ";"); e; e = strtok(NULL, ";")) {
        if (!strncmp(e, key, kl) && e[kl] == '|' && !strncmp(e + kl + 1, dev, dl) && e[kl + 1 + dl] == '|') continue;   /* troca a entrada (chave, monitor) */
        size_t n = strlen(keep); if (n + strlen(e) + 2 < sizeof keep) snprintf(keep + n, sizeof keep - n, "%s%s", n ? ";" : "", e);
    }
    pct = pct < 50 ? 50 : pct > 200 ? 200 : pct;
    char now[560]; snprintf(now, sizeof now, "%s%s%s|%s|%d|%d|%d", keep, keep[0] ? ";" : "", key, dev, x, y, pct);
    while (strlen(now) > 470) { char *sc = strchr(now, ';'); if (!sc) break; memmove(now, sc + 1, strlen(sc)); }   /* sobra: descarta o mais antigo */
    snprintf(g_set.ov_tiles, sizeof g_set.ov_tiles, "%s", now);
}
bool settings_tile_enabled(const char *key) {
    char b[120]; snprintf(b, sizeof b, "%s", g_set.ov_toff);
    for (char *e = strtok(b, ";"); e; e = strtok(NULL, ";")) if (!strcmp(e, key)) return false;
    return true;
}
void settings_tile_enable(const char *key, bool on) {
    char keep[120] = "", b[120]; snprintf(b, sizeof b, "%s", g_set.ov_toff);
    for (char *e = strtok(b, ";"); e; e = strtok(NULL, ";")) { if (!strcmp(e, key)) continue; size_t n = strlen(keep); if (n + strlen(e) + 2 < sizeof keep) snprintf(keep + n, sizeof keep - n, "%s%s", n ? ";" : "", e); }
    if (!on) { char now[160]; snprintf(now, sizeof now, "%s%s%s", keep, keep[0] ? ";" : "", key); while (strlen(now) > 110) { char *sc = strchr(now, ';'); if (!sc) break; memmove(now, sc + 1, strlen(sc)); } snprintf(keep, sizeof keep, "%s", now); }
    snprintf(g_set.ov_toff, sizeof g_set.ov_toff, "%s", keep);
}
void settings_ovl_reset(void) { g_set.ov_tiles[0] = 0;  g_set.ov_lay[0] = 0; g_set.ov_dev[0] = 0; }
bool settings_echo(void) { return g_set.echo; }
bool settings_nt_online(void) { return g_set.nt_online; }
bool settings_nt_stream(void) { return g_set.nt_stream; }
void settings_audio_set(float in_vol, float out_vol, bool gate_on, float gate_db) { g_set.in_vol = in_vol < 0 ? 0 : in_vol > 100 ? 100 : in_vol; g_set.out_vol = out_vol < 0 ? 0 : out_vol > 100 ? 100 : out_vol; g_set.gate_on = gate_on; g_set.gate_db = gate_db < -70 ? -70 : gate_db > -20 ? -20 : gate_db; }   /* ajustes de áudio da chamada (ui_call.c): settings_tick grava */
static void tag_nofx(cui_ctx *ui) {   /* selo discreto: controle salvo localmente, sem efeito real ainda */
    cui_text(ui, "Em breve · salvo neste computador, ainda sem efeito", .size = 12, .color = g_th.ph);
}
static void upd_dot_paint(cui_ctx *ui, cui_rect r, void *user) { (void)user; cui_draw_circle(ui, (cui_vec2){ r.x + r.w * 0.5f, r.y + r.h * 0.5f }, r.w * 0.5f, CUI_RGB(0x4ade80)); }
static bool menu_item(cui_ctx *ui, const char *icon, const char *label, bool active) {
    cui_box(ui, .id = label, .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 12, .pad_y = 7, .gap = 10, .radius = 12, .align = CUI_ALIGN_CENTER,
            .bg = active ? B_A(B_C3, 0x33) : briks_hover(ui, label) ? (cui_mouse_down(ui, CUI_MOUSE_LEFT) ? CUI_RGBA(0xffffff24) : CUI_RGBA(0xffffff12)) : CUI_RGBA(0x00000000), .border = active ? 1 : 0, .border_color = B_A(B_C3, 0x66)) {   /* hover e clique visíveis também dentro do diálogo */
        cui_icon(ui, icon, 15, active ? B_TEXT : B_MUTED);
        cui_text(ui, label, .size = 13, .color = active ? B_TEXT : B_MUTED, .nowrap = true);
        if (!strcmp(label, "Atualizações") && update_phase() == UPD_READY) cui_canvas(ui, "upd-dot", CUI_PX(8), CUI_PX(8), upd_dot_paint, NULL, 0);   /* ATU-05: bolinha quando há atualização pronta */
    }
    return briks_clicked(ui, label);
}
static void sw(cui_ctx *ui, const char *label, const char *hint, bool *v) {
    cui_switch(ui, label, v); if (hint) sub(ui, hint);
}
static void seg(cui_ctx *ui, const char *id, const char *const *items, int n, int *v) { cui_segmented(ui, id, items, n, v, .full_width = true, .size = CUI_SIZE_SM); }

static const char *TAB_NAMES[] = { "Voz e vídeo", "Rede", "Temas", "Armazenamento", "Sobreposição", "Atalhos", "Notificações", "Avisos", "Atualizações", "Sobre", "Desempenho", "Presença", "Painéis", "Apoiar o Briks" };
static const char *const TAB_ICONS[] = { CUI_ICON_MIC, CUI_ICON_GLOBE, CUI_ICON_SETTINGS, CUI_ICON_HISTORY, CUI_ICON_MONITOR, CUI_ICON_KEYBOARD, CUI_ICON_BELL, CUI_ICON_MESSAGE_SQUARE,
    CUI_ICON_PLUS, CUI_ICON_USERS, CUI_ICON_SETTINGS, CUI_ICON_USER, CUI_ICON_SETTINGS, CUI_ICON_HEART };
#define NTABS 14

/* Ctrl+mais / Ctrl+menos / Ctrl+0 (BAS-39): muda o tamanho do texto um degrau (0 normal .. 3 máximo); 0 volta ao padrão (grande). Grava junto com as demais configurações. */
void settings_text_step(cui_ctx *ui, int delta) {
    int v = delta == 0 ? 1 : g_set.text_size + delta;
    if (v < 0) v = 0; if (v > 3) v = 3;
    if (v == g_set.text_size && v == g_text_size) return;
    g_set.text_size = v; g_text_size = v; briks_text_scale_apply(ui);
}

/* ---- atalhos: gravar a combinação apertada, remover, avisar de conflito ---- */
static int rec_slot = -1;                 /* 0..2 globais, 3 apertar para falar */
static char key_msg[160]; static int key_msg_slot = -1;
static char *key_field(int i) { return i == 0 ? g_set.key_mute : i == 1 ? g_set.key_deafen : i == 2 ? g_set.key_stream : g_set.key_ptt; }
static const char *const KEY_LABEL[4] = { "Mutar / desmutar microfone", "Ensurdecer", "Iniciar / parar transmissão", "Tecla de apertar para falar" };
static void key_apply(int i, const char *t) {   /* aceita a combinação gravada ou explica por que não */
    unsigned m; int v; char fin[48];
    if (!hk_parse(t, &m, &v)) return;
    if (i == 3) { m = 0; if (!hk_format(0, v, fin, sizeof fin)) return; t = fin; }   /* apertar para falar segue só a tecla */
    key_msg_slot = i; key_msg[0] = 0;
    for (int j = 0; j < 4; j++) if (j != i && hk_same(key_field(j), t)) { snprintf(key_msg, sizeof key_msg, "%s já usa %s. Escolha outra combinação.", KEY_LABEL[j], t); return; }
    if (i < 3 && !hk_is_mouse(v) && !hk_same(key_field(i), t)) {
        const char *busy = getenv("BRIKS_HK_BUSY");   /* testes: finge que outro programa usa este combo */
        const bool hidden = getenv("BRIKS_HIDDEN") || getenv("BRIKS_SCRIPT") || getenv("BRIKS_DEMO");
        if ((busy && hk_same(busy, t)) || (!busy && !hidden && !hkw_probe(m, v))) { snprintf(key_msg, sizeof key_msg, "Outro programa já usa %s. Escolha outra combinação.", t); return; }
    }
    snprintf(key_field(i), 24, "%s", t);
}
static void key_row(cui_ctx *ui, app_state_t *s, int i) {
    char *f = key_field(i); const bool rec = rec_slot == i;
    cui_text(ui, KEY_LABEL[i], .size = 12, .color = B_MUTED);
    cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
        cui_box(ui, .width = CUI_FILL, .pad_x = 12, .pad_y = 9, .radius = 12, .bg = B_PANEL, .border = rec ? 2 : 1, .border_color = rec ? B_C3 : B_LINE) {
            cui_text(ui, rec ? "Aperte a combinação… (Esc cancela)" : f[0] ? f : "Sem atalho", .size = 13, .color = rec ? B_C3 : f[0] ? B_TEXT : g_th.ph, .nowrap = true);
        }
        cui_push_id_int(ui, 6000 + i);
        if (cui_button(ui, rec ? "Cancelar" : f[0] ? "Alterar" : "Gravar", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) {
            if (rec) rec_slot = -1; else { rec_slot = i; hkw_capture_begin(true); key_msg[0] = 0; }
        }
        if (f[0] && !rec && cui_button(ui, "Remover", .variant = CUI_BUTTON_LINK, .size = CUI_SIZE_SM)) { f[0] = 0; key_msg_slot = -1; }
        cui_pop_id(ui);
    }
    if (key_msg_slot == i && key_msg[0]) cui_text(ui, key_msg, .size = 12, .color = B_DANGER, .width = CUI_FILL);
    else if (i < 3 && f[0]) {
        if (!s->in_room) sub(ui, "Passa a valer quando você entra numa chamada.");
        else if (s->hk_status[i] == HKS_BUSY) cui_text(ui, "Outro programa já usa esta combinação: ela não vai funcionar. Escolha outra.", .size = 12, .color = B_DANGER, .width = CUI_FILL);
        else if (s->hk_status[i] == HKS_OK || s->hk_status[i] == HKS_POLLED) cui_text(ui, "Ativo agora, mesmo com o Briks em segundo plano.", .size = 12, .color = B_ONLINE);
    }
}

/* ajustes para a exportação de dados: só preferências, nunca senha, token ou chave */
void settings_export_json(FILE *f) {
    for (int i = 0; i < NF; i++) {
        fprintf(f, "%s\"%s\": ", i ? ", " : "", F[i].k);
        if (F[i].t == 'i') fprintf(f, "%d", *(int *)F[i].p); else if (F[i].t == 'f') fprintf(f, "%.2f", *(float *)F[i].p);
        else if (F[i].t == 'b') fputs(*(bool *)F[i].p ? "true" : "false", f);
        else { char b[200]; json_escape((char *)F[i].p, b, sizeof b); fprintf(f, "\"%s\"", b); }
    }
}

/* sondas dos roteiros de teste (script.c: expect <nome> <valor>); -9999 = nome desconhecido */
int settings_probe(const char *n, app_state_t *s) {
    unsigned m; int v;
    { int dp = data_probe(n); if (dp != -9999) return dp; }
    { int np = nettest_probe(n, s); if (np != -9999) return np; }
    { int up = update_probe(n); if (up != -9999) return up; }
    { int op = overlay_probe(n); if (op != -9999) return op; }
    { int cp = cam_probe(n); if (cp != -9999) return cp; }
    if (!strcmp(n, "cfg_ov_on")) return g_set.ov_on;
    if (!strcmp(n, "cfg_ov_tiles")) { int c = 0; for (const char *p = g_set.ov_tiles; *p; p++) if (*p == ';') c++; return g_set.ov_tiles[0] ? c + 1 : 0; }
    if (!strcmp(n, "cfg_ov_toff")) { int c = 0; for (const char *p = g_set.ov_toff; *p; p++) if (*p == ';') c++; return g_set.ov_toff[0] ? c + 1 : 0; }
    if (!strcmp(n, "cfg_ov_selfshare")) return g_set.ov_selfshare;
    if (!strcmp(n, "cfg_ov_flags")) return (g_set.ov_live ? 1 : 0) | (g_set.ov_voice ? 2 : 0) | (g_set.ov_status ? 4 : 0) | (g_set.ov_mute ? 8 : 0);
    if (!strcmp(n, "cfg_ov_opacity")) return (int)(g_set.ov_opacity + 0.5f);
    if (!strcmp(n, "rec_slot")) return rec_slot;
    if (!strcmp(n, "key_msg")) return key_msg[0] != 0;
    if (!strcmp(n, "mic_test_on")) return s->mic_test_on;
    if (!strcmp(n, "mic_test_level")) return (int)(s->mic_test_level * 100.0f + 0.5f);
    if (!strncmp(n, "mic_test_ge", 11)) return s->mic_test_level * 100.0f >= (float)atoi(n + 11);   /* nível do teste >= N % */
    if (!strcmp(n, "mic_test_err")) return s->mic_test_msg[0] != 0;
    if (!strcmp(n, "mic_problem")) return rtc_mic_problem(s);
    if (!strcmp(n, "dev_notice")) return s->dev_notice[0] != 0;
    if (!strncmp(n, "hk_status", 9)) return s->hk_status[n[9] - '0' < 3 && n[9] >= '0' ? n[9] - '0' : 0];
    if (!strncmp(n, "key_", 4) && (!strcmp(n + 4, "mute") || !strcmp(n + 4, "deafen") || !strcmp(n + 4, "stream") || !strcmp(n + 4, "ptt"))) return key_field(n[4] == 'm' ? 0 : n[4] == 'd' ? 1 : n[4] == 's' ? 2 : 3)[0] != 0;
    if (!strncmp(n, "keyvk_", 6)) { const char *w = n + 6; int i = !strcmp(w, "mute") ? 0 : !strcmp(w, "deafen") ? 1 : !strcmp(w, "stream") ? 2 : 3; return hk_parse(key_field(i), &m, &v) ? v : 0; }
    if (!strncmp(n, "keymods_", 8)) { const char *w = n + 8; int i = !strcmp(w, "mute") ? 0 : !strcmp(w, "deafen") ? 1 : !strcmp(w, "stream") ? 2 : 3; return hk_parse(key_field(i), &m, &v) ? (int)m : 0; }
    if (!strcmp(n, "cfg_cm")) return g_set.call_mode;
    if (!strcmp(n, "cfg_mic")) return g_set.mic;
    if (!strcmp(n, "cfg_noise_off")) return g_set.noise_off;
    if (!strcmp(n, "cfg_echo")) return g_set.echo;
    if (!strcmp(n, "cfg_ov_saved")) return g_set.ov_dev[0] != 0;
    if (!strcmp(n, "cfg_ov_monitors")) { int c = 0; for (const char *p = g_set.ov_lay; *p; p++) if (*p == ';') c++; return g_set.ov_lay[0] ? c + 1 : 0; }
    return -9999;
}

void settings_tick(void) {
    static bool loaded; static settings_t last;
    if (!loaded) { loaded = true; load(); enum_devices(); last = g_set; return; }
    if (memcmp(&last, &g_set, sizeof g_set)) {
        snprintf(g_set.mic_name, sizeof g_set.mic_name, "%s", g_set.mic ? in_names[g_set.mic] : "");
        snprintf(g_set.out_name, sizeof g_set.out_name, "%s", g_set.out ? out_names[g_set.out] : "");
        last = g_set; save();
    }
}

/* BAS-37: "Licenças de software de terceiros": o texto vem de assets\licenses.txt ao lado do executável (gerado do THIRD_PARTY_NOTICES, LICENSES e Cargo.lock) */
static bool g_lic_open; static char *g_lic_text;
static void lic_load(void) {
    if (g_lic_text) return;
    char dir[MAX_PATH], path[MAX_PATH + 40]; GetModuleFileNameA(NULL, dir, sizeof dir);
    char *sl = strrchr(dir, '\\'); if (sl) *sl = 0;
    snprintf(path, sizeof path, "%s\\assets\\licenses.txt", dir);
    FILE *f = fopen(path, "rb");
    if (f) { fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); if (n > 0 && n < (8 << 20)) { g_lic_text = malloc((size_t)n + 1); if (g_lic_text) { size_t r = fread(g_lic_text, 1, (size_t)n, f); g_lic_text[r] = 0; } } fclose(f); }
    if (!g_lic_text) g_lic_text = _strdup("O texto das licenças não foi encontrado junto do aplicativo.\nO Briks é licenciado sob a GNU Affero General Public License v3.0. Código-fonte e avisos de terceiros: github.com/ygorbetine/Briks (arquivos LICENSE e THIRD_PARTY_NOTICES.md).");
}
static void lic_dialog(cui_ctx *ui) {
    if (!g_lic_open) return;
    lic_load();
    cui_vec2 vp = cui_viewport_size(ui);
    const float w = fminf(760.0f, fmaxf(vp.x - 48.0f, 320.0f)), h = fminf(640.0f, fmaxf(vp.y - 190.0f, 200.0f));
    cui_dialog(ui, "Licenças de software de terceiros", &g_lic_open, .width = w) {
        cui_scroll(ui, .id = "lic-scroll", .width = CUI_FILL, .height = CUI_PX(h), .gap = 0, .pad = 4) {
            const char *c = g_lic_text; int shown = 0;
            while (*c && shown < 4000) {
                const char *e = strchr(c, '\n'); size_t n = e ? (size_t)(e - c) : strlen(c);
                char line[400]; if (n >= sizeof line) n = sizeof line - 1; memcpy(line, c, n); line[n] = 0;
                if (n && line[n - 1] == '\r') line[n - 1] = 0;
                if (line[0]) cui_text(ui, line, .size = 13, .color = line[0] == '=' ? B_MUTED : B_TEXT, .width = CUI_FILL); else cui_gap(ui, 6);
                shown++; c = e ? e + 1 : c + n;
            }
        }
    }
}

void settings_dialog(cui_ctx *ui, app_state_t *s) {
    static float restored_at = -10, cleared_at = -10; static bool was_open, confirm_hist, confirm_all;
    static float level_sim;
    if (s->settings_open && !was_open) {
        data_refresh();
        enum_devices(); confirm_hist = confirm_all = false; rec_slot = -1; key_msg[0] = 0; key_msg_slot = -1;
    }
    was_open = s->settings_open;
    cam_tick(ui, s);   /* lista as câmeras e solta o teste ao sair desta aba */
    if (rec_slot >= 0 && (!s->settings_open || s->settings_tab != 5)) rec_slot = -1;
    if (rec_slot >= 0) {   /* captura da tecla apertada: o laço de atalhos reais fica parado enquanto isso */
        char t[48]; g_hk_recording = true; cui_request_wakeup(ui, 0.03f);
        int r = hkw_capture_poll(t, sizeof t);
        if (r == 1) { key_apply(rec_slot, t); rec_slot = -1; } else if (r == 2) rec_slot = -1;
    }
    cui_vec2 vp = cui_viewport_size(ui);
    const float dw = fminf(900.0f, fmaxf(vp.x - 48.0f, 340.0f)), dh = fminf(900.0f, fmaxf(vp.y - 150.0f, 260.0f));
    cui_dialog(ui, "Configurações", &s->settings_open, .width = dw) {
        cui_row(ui, .width = CUI_FILL, .height = CUI_PX(dh), .gap = 16) {
            cui_box(ui, .width = CUI_PX(220), .height = CUI_FILL, .pad = 10, .gap = 2, .radius = 18, .bg = B_GLASS_CARD, .border = 1, .border_color = B_GLASS_BORDER) {
                cui_scroll(ui, .id = "cfg-menu", .width = CUI_FILL, .height = CUI_PX(fmaxf(dh - 20.0f - 36.0f - 2.0f, 80.0f)), .gap = 2) {   /* altura exata: a lista rola e o botão Desconectar não sobrepõe */
                    for (int i = 0; i < NTABS; i++) {
                        cui_push_id_int(ui, 5000 + i);
                        if (menu_item(ui, TAB_ICONS[i], TAB_NAMES[i], s->settings_tab == i)) s->settings_tab = i;
                        cui_pop_id(ui);
                    }
                }
                if (cui_button(ui, "Desconectar", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM, .full_width = true)) { s->settings_open = false; app_logout(s); }
            }
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 12, .scroll_y = true) {
                switch (s->settings_tab) {
                case 0: /* Voz e vídeo */
                    SCARD(ui, "ENTRADA") {
                        cui_text(ui, "Microfone", .size = 12, .color = B_MUTED);
                        cui_select(ui, "cfg-mic", in_ptr, in_n, &g_set.mic);
                        sub(ui, "Vale na hora, inclusive numa chamada em andamento. Se o aparelho sumir, volta ao padrão do sistema.");
                        if (rtc_mic_problem(s)) {   /* mesmo padrão do aviso da câmera: texto em vermelho + o caminho para resolver */
                            cui_text(ui, s->mic_text, .size = 12, .color = B_DANGER, .width = CUI_FILL);
                            if (!strcmp(s->mic_state, "denied") && cui_button(ui, "Abrir Configurações do Windows", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) rtc_open_mic_privacy();
                        }
                        if (s->dev_notice[0]) sub(ui, s->dev_notice);
                        cui_text(ui, "Nível do microfone", .size = 12, .color = B_MUTED);
                        level_sim += ((s->in_room ? s->me_level : s->mic_test_level) - level_sim) * 0.3f;
                        cui_progress(ui, level_sim);
                        if (s->in_room) sub(ui, "A barra mostra o que o Briks está ouvindo na chamada.");
                        else {
                            cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER, .width = CUI_FILL, .wrap = true) {
                                if (cui_button(ui, s->mic_test_on ? "Parar teste" : "Testar microfone", .variant = s->mic_test_on ? CUI_BUTTON_SECONDARY : CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM)) {
                                    s->mic_test_on = !s->mic_test_on; s->mic_test_msg[0] = 0; if (!s->mic_test_on) s->mic_test_level = 0;
                                }
                            }
                            if (s->mic_test_msg[0]) cui_text(ui, s->mic_test_msg, .size = 12, .color = B_DANGER, .width = CUI_FILL);
                            else sub(ui, s->mic_test_on ? "Fale: a barra mostra o que sai com o seu ganho, a porta e a supressão. Nada é gravado nem enviado." : "Mede o microfone sem entrar numa chamada. Nada é gravado nem enviado; para sozinho ao sair desta tela.");
                            if (s->mic_test_on) cui_request_wakeup(ui, 0.05f);
                        }
                        cui_text(ui, "Volume de entrada (ganho)", .size = 12, .color = B_MUTED);
                        briks_volume(ui, "cfg-invol", &g_set.in_vol, 0.0f, 100.0f, 1.0f, NULL, true);
                        sub(ui, "O ganho vale na hora, na sua voz que sai para a sala.");
                    }
                    SCARD(ui, "CÂMERA") {
                        static const char *cam_ptr[9]; static int cam_sel;
                        const int nd = cam_dev_count();
                        cam_ptr[0] = nd ? "Câmera padrão" : cam_devs_loaded() ? "Nenhuma câmera encontrada" : "Procurando câmeras…";
                        for (int i = 0; i < nd && i < 8; i++) cam_ptr[i + 1] = cam_dev_name(i);
                        cam_sel = 0; for (int i = 0; g_set.cam_name[0] && i < nd; i++) if (!strcmp(cam_dev_name(i), g_set.cam_name)) cam_sel = i + 1;
                        const int before_sel = cam_sel;
                        cui_text(ui, "Câmera", .size = 12, .color = B_MUTED);
                        cui_select(ui, "cfg-cam", cam_ptr, nd + 1, &cam_sel);
                        if (cam_sel != before_sel) { snprintf(g_set.cam_name, sizeof g_set.cam_name, "%s", cam_sel ? cam_dev_name(cam_sel - 1) : ""); if (cam_preview_wanted()) { cam_preview_set(false); cam_preview_set(true); } }
                        sub(ui, "Vale na próxima vez que você ligar a câmera. A câmera só abre quando você liga; a luz apaga ao desligar.");
                        cam_settings_view(ui, s, 288.0f, 162.0f);
                        cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER, .width = CUI_FILL, .wrap = true) {
                            const bool on = cam_preview_wanted() || cam_active();
                            cui_push_disabled(ui, cam_active() || (nd == 0));
                            if (cui_button(ui, cam_preview_wanted() ? "Parar teste" : "Testar câmera", .variant = on ? CUI_BUTTON_SECONDARY : CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM)) cam_preview_set(!cam_preview_wanted());
                            cui_pop_disabled(ui);
                        }
                        if (cam_preview_error()[0]) cui_text(ui, cam_preview_error(), .size = 12, .color = B_DANGER, .width = CUI_FILL);
                        else if (nd == 0 && cam_devs_loaded()) cui_text(ui, "Não encontramos uma câmera neste computador.", .size = 12, .color = B_DANGER, .width = CUI_FILL);
                        else sub(ui, cam_active() ? "Sua câmera já está ligada na chamada: esta é a imagem que sai." : "A imagem aparece só para você. Nada é gravado nem enviado; para sozinho ao sair desta tela.");
                        if (cam_preview_wanted()) cui_request_wakeup(ui, 0.05f);
                    }
                    SCARD(ui, "SAÍDA") {
                        cui_text(ui, "Saída de som", .size = 12, .color = B_MUTED);
                        cui_select(ui, "cfg-out", out_ptr, out_n, &g_set.out);
                        sub(ui, "Vale na hora para as vozes da sala. Se o aparelho sumir, volta ao padrão do sistema.");
                        cui_text(ui, "Volume de saída", .size = 12, .color = B_MUTED);
                        briks_volume(ui, "cfg-outvol", &g_set.out_vol, 0.0f, 100.0f, 1.0f, NULL, true);
                        sub(ui, "O volume vale na hora, para tudo que você ouve na sala.");
                    }
                    SCARD(ui, "PROCESSAMENTO") {
                        static const char *NZ[] = { "Padrão", "Desligada" };
                        cui_text(ui, "Supressão de ruído", .size = 12, .color = B_MUTED);
                        seg(ui, "cfg-nz", NZ, 2, &g_set.noise_off);
                        sub(ui, g_set.noise_off ? "Sem filtro algum: a voz sai como o microfone captou." : "Filtro leve no seu computador: corta ronco grave e abaixa chiado constante (ventilador). Não é a redução por rede neural (RNNoise), que chega depois.");
                        sw(ui, "Porta de ruído", "Silencia o microfone abaixo do limite (teclado, ventilador).", &g_set.gate_on);
                        if (g_set.gate_on) { cui_textf(ui, "Limite: %d dB", (int)g_set.gate_db); cui_slider(ui, "##cfg-gate", &g_set.gate_db, -70, -20); }
                        sub(ui, "A porta de ruído e a supressão valem na hora.");
                        sw(ui, "Cancelamento de eco", "Tira do seu microfone o som das caixas que volta para ele (o filtro aprende o caminho do som em poucos segundos de chamada). Vale na hora. Com fones o eco nem existe: pode desligar.", &g_set.echo);
                        cui_push_disabled(ui, true);
                        cui_switch(ui, "Excluir áudio do Briks da transmissão", &g_set.excl_audio);
                        cui_pop_disabled(ui);
                        soon(ui, "excluir o áudio do Briks da transmissão (o interruptor fica desligado até lá)");
                    }
                    SCARD(ui, "CHAMADA") {   /* CHM-35: desconexão por inatividade configurável */
                        static const char *const IDL[] = { "Nunca", "15 min", "30 min", "1 hora", "2 horas" };
                        static const int IDM[] = { 0, 15, 30, 60, 120 };
                        int k = 3; for (int i = 0; i < 5; i++) if (IDM[i] == g_set.idle_min) k = i;
                        cui_text(ui, "Sair da chamada depois de ficar sem falar", .size = 12, .color = B_MUTED);
                        const int before = k; seg(ui, "cfg-idle", IDL, 5, &k);
                        if (k != before) g_set.idle_min = IDM[k];
                        sub(ui, g_set.idle_min ? "Faltando 5 minutos (ou 1/3 do prazo) aparece um aviso com o botão Continuar. Falar, transmitir a tela ou assistir a uma transmissão zera a contagem. Vale na hora." : "Você só sai da chamada quando decidir sair.");
                    }
                    break;
                case 1: { /* Rede */
                    static const char *CM[] = { "Automático", "Sempre P2P", "Sempre SFU" };
                    static const char *NP[] = { "Automático", "Internet direta", "VPN privada" };
                    SCARD(ui, "MODO DA CHAMADA") {
                        static int pend_cm = -1;   /* troca pedida com a chamada aberta: espera a confirmação (reconecta) */
                        { const int before = g_set.call_mode; seg(ui, "cfg-cm", CM, 3, &g_set.call_mode);
                          if (g_set.call_mode != before) g_set.call_mode_set = true;   /* escolha explícita do usuário */
                          if (g_set.call_mode != before && s->in_room) { pend_cm = g_set.call_mode; g_set.call_mode = before; } }
                        if (!s->in_room) pend_cm = -1;
                        if (pend_cm >= 0 && pend_cm != g_set.call_mode) {
                            cui_text(ui, "Você está em uma chamada. Trocar o modo agora reconecta a chamada (um corte de alguns segundos).", .size = 12, .color = B_MUTED, .width = CUI_FILL);
                            cui_row(ui, .gap = 8, .width = CUI_FILL, .wrap = true) {
                                if (cui_button(ui, "Trocar e reconectar", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM)) {
                                    char room[sizeof s->room_name]; snprintf(room, sizeof room, "%s", s->room_name);
                                    g_set.call_mode = pend_cm; pend_cm = -1;
                                    rtc_leave_room(s); s->rtc_session_sent = s->rtc_session_ready; rtc_join_room(s, room);
                                }
                                if (cui_button(ui, "Só na próxima chamada", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) { g_set.call_mode = pend_cm; pend_cm = -1; }
                                if (cui_button(ui, "Cancelar", .variant = CUI_BUTTON_LINK, .size = CUI_SIZE_SM)) pend_cm = -1;
                            }
                        }
                        sub(ui, g_set.call_mode == 0 ? "Padrão: usa o servidor de chamadas (SFU) quando ele está configurado; sem ele, P2P. Se o servidor falhar, mostra o erro e oferece entrar em P2P; nunca troca sozinho." : g_set.call_mode == 1 ? "Conexão direta entre todos, até 8 pessoas. Sem servidor no meio, mesmo com o servidor de chamadas configurado." : "Passa sempre pelo servidor, nunca cai em P2P sozinho. Melhor para salas grandes e redes difíceis.");
                        sub(ui, "Todos da sala precisam usar o mesmo modo. Vale a partir da próxima chamada.");
                        if (s->net_cfg_known) sub(ui, s->net_cfg_err[0] ? s->net_cfg_err : s->net_cfg_sfu ? "Servidor de chamadas: configurado." : g_set.call_mode == 2 ? "Servidor de chamadas: não configurado; \"Sempre SFU\" usa P2P até configurar." : "Servidor de chamadas: não configurado; as chamadas usam P2P.");
                    }
                    SCARD(ui, "PREFERÊNCIA DE CONEXÃO") {
                        seg(ui, "cfg-np", NP, 3, &g_set.net_pref);
                        sub(ui, g_set.net_pref == 0 ? "Deixa o WebRTC escolher entre VPN, conexão direta e TURN." : g_set.net_pref == 1 ? "Não oferece os adaptadores de VPN (ZeroTier, Tailscale, WireGuard) à chamada; STUN/TURN seguem disponíveis." : "Mantém os adaptadores de VPN disponíveis junto com a internet.");
                        if (g_set.net_pref == 2) sub(ui, "A prioridade entre VPN e internet ainda é decidida pelo WebRTC; esta opção não bloqueia nenhum adaptador.");
                        else sub(ui, "Vale a partir da próxima chamada.");
                    }
                    SCARD(ui, "FIREWALL DO WINDOWS") {
                        fw_refresh();
                        sub(ui, fw_state_text());
                        sub(ui, "A regra vale só para o Briks (entrada), sem abrir porta para mais nada. O Windows pede a sua permissão.");
                        cui_row(ui, .gap = 8, .width = CUI_FILL, .wrap = true) {
                            if (cui_button(ui, fw_exists() == 1 ? "Liberar de novo" : "Liberar o Briks", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM, .loading = fw_busy())) fw_request(true);
                            if (cui_button(ui, "Remover a regra", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM, .disabled = fw_exists() != 1 || fw_busy())) fw_request(false);
                        }
                        if (fw_msg()[0]) sub(ui, fw_msg());
                        if (fw_busy()) cui_request_wakeup(ui, 0.3f);
                    }
                    SCARD(ui, "TESTE DE CONEXÃO") {
                        static bool scanned; if (!scanned) { scanned = true; netdiag_vpn_scan(); }
                        nettest_card(ui, s);   /* ui_nettest.c: STUN + NAT + relay, com selos e veredito */
                    }
                    SCARD(ui, "REDES PRIVADAS DETECTADAS") {
                        netdiag_t nd = netdiag_get();
                        if (!nd.vpn_n) sub(ui, "Nenhuma interface ZeroTier, Tailscale ou WireGuard foi detectada.");
                        for (int i = 0; i < nd.vpn_n; i++) cui_textf(ui, "%s  ·  detectado", nd.vpn_names[i]);
                        sub(ui, "Os endereços ficam neste computador e não entram em diagnósticos.");
                        if (cui_button(ui, "Atualizar detecção", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) netdiag_vpn_scan();
                    }
                    SCARD(ui, "SERVIDOR TURN") {
                        { static double last; const double now = cui_time(ui);   /* reler: o TURN da conta chega depois do login */
                          if (core_state() == CORE_READY && (!s->ice_asked || now - last > 5.0)) { account_ice_get(s); last = now; } }
                        cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER, .width = CUI_FILL) {
                            cui_box(ui, .width = CUI_PX(8), .height = CUI_PX(8), .radius = 4, .bg = s->ice_turn ? B_ONLINE : CUI_RGB(0xe8c46a)) {}
                            cui_text(ui, !s->ice_turn ? "Somente STUN" : !strcmp(s->ice_source, "account") ? "TURN da sua conta" : "TURN configurado", .font = CUI_FONT_MEDIUM, .size = 13);
                        }
                        sub(ui, s->ice_turn ? (!strcmp(s->ice_source, "account") ? "Recebido automaticamente da sua conta Briks e renovado sozinho. Preencher os campos abaixo usa o seu servidor no lugar."
                                                           : "Chamadas conseguem passar por um servidor de retransmissão quando a conexão direta falha.")
                                            : "Funciona na maioria das redes. Em redes restritivas (CGNAT, empresa, 4G) a chamada pode não conectar. Um servidor TURN resolve.");
                        if (!strcmp(s->ice_source, "env")) sub(ui, "Em uso: variável BRIKS_ICE_SERVERS_JSON (tem prioridade sobre o que for salvo aqui).");
                        cui_text(ui, "Endereços (separe por vírgula)", .size = 12, .color = B_MUTED);
                        cui_input(ui, "ice-urls", s->ice_urls, sizeof s->ice_urls, .placeholder = "turn:meuservidor.com:3478");
                        cui_text(ui, "Usuário", .size = 12, .color = B_MUTED);
                        cui_input(ui, "ice-user", s->ice_user, sizeof s->ice_user, .placeholder = "usuário do TURN");
                        cui_text(ui, "Senha", .size = 12, .color = B_MUTED);
                        cui_input(ui, "ice-pass", s->ice_pass, sizeof s->ice_pass, .password = true, .placeholder = s->ice_has_cred ? "•••••• (guardada; deixe vazio para manter)" : "senha do TURN");
                        sub(ui, "A senha é cifrada neste computador (Windows) e nunca aparece de novo, nem em registros.");
                        if (s->ice_msg[0]) cui_text(ui, s->ice_msg, .size = 12, .color = CUI_RGB(0xd9cdea), .width = CUI_FILL);
                        cui_row(ui, .gap = 8, .width = CUI_FILL, .wrap = true) {
                            if (cui_button(ui, "Salvar", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM, .loading = s->ice_busy, .disabled = !s->ice_urls[0])) account_ice_save(s);
                            if (cui_button(ui, "Testar conexão", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM, .loading = s->ice_testing)) account_ice_test(s);
                            if (cui_button(ui, "Remover", .variant = CUI_BUTTON_LINK, .size = CUI_SIZE_SM, .disabled = !strcmp(s->ice_source, "default"))) account_ice_clear(s);
                        }
                        if (s->ice_test_msg[0]) {
                            const cui_color c = s->ice_test_level == 3 ? B_ONLINE : s->ice_test_level == 2 ? CUI_RGB(0xe8c46a) : s->ice_test_level == 1 ? CUI_RGB(0xff9a9a) : B_MUTED;
                            cui_text(ui, s->ice_test_msg, .size = 12, .color = c, .width = CUI_FILL);
                        }
                        sub(ui, "O teste usa os servidores já salvos. Salve antes de testar.");
                    }
                    SCARD(ui, "DIAGNÓSTICO") {
                        char b[96]; cui_textf(ui, "Signaling: %s", s->rtc_state[0] ? s->rtc_state : "indisponível");
                        cui_textf(ui, "Canal: %s", s->in_room ? "na sala" : "indisponível");
                        cui_textf(ui, "Pessoas na sala: %d (autenticadas: %d)", s->rtc_peer_count, s->rtc_authed_count);
                        cui_textf(ui, "Modo da chamada: %s", s->net_note[0] ? s->net_note : "ainda sem chamada nesta sessão");
                        if (s->rtc_msg[0]) sub(ui, s->rtc_msg);
                        (void)b; sub(ui, s->ice_note[0] ? s->ice_note : "Servidores: STUN público (TURN só se configurado)."); 
                    }
                    break; }
                case 2: /* Temas */
                    SCARD(ui, "APARÊNCIA · TAMANHO DO TEXTO") {
                        static const char *TS[] = { "Normal", "Grande", "Maior", "Máximo" };
                        int before = g_set.text_size;
                        seg(ui, "cfg-ts", TS, 4, &g_set.text_size);
                        if (g_set.text_size != before) { g_text_size = g_set.text_size; briks_text_scale_apply(ui); }
                        sub(ui, "Vale na hora em todo o app e fica salvo. O padrão é Grande.");
                    }
                    themes_tab(ui);
                    break;
                case 3: /* Armazenamento */
                    data_ui(ui, s);
                    break;
                case 4: /* Sobreposição */
                    SCARD(ui, "SOBREPOSIÇÃO EM JOGOS") {
                        sw(ui, "Mostrar sobreposição em jogos", "Mostra quem está na sala por cima do jogo, sem tirar você dele. Só aparece enquanto você está numa chamada.", &g_set.ov_on);
                        { const int os = overlay_state();
                          if (g_set.ov_on && os == 1) cui_text(ui, "Ativa agora.", .size = 12, .color = B_ONLINE);
                          else if (g_set.ov_on && os == 2) cui_text(ui, "Não encontrei o briks_overlay.exe ao lado do Briks: reinstale o app para usar a sobreposição.", .size = 12, .color = B_DANGER, .width = CUI_FILL);
                          else if (g_set.ov_on && os == 3) cui_text(ui, "A sobreposição fechou sozinha. O Briks tenta abrir de novo em instantes.", .size = 12, .color = B_DANGER, .width = CUI_FILL);
                          else if (g_set.ov_on) sub(ui, "Aparece quando você entra numa chamada."); }
                        cui_text(ui, "O que aparece", .size = 12, .color = B_MUTED);
                        sw(ui, "Quem está na sala", "Você e as pessoas da chamada; quem fala fica destacado.", &g_set.ov_voice);
                        sw(ui, "Sala conectada", "Uma linha dizendo que a sala está conectada.", &g_set.ov_status);
                        /* 1.5.20: o botão só aparece quando alguém está transmitindo na sala (ou se já estiver ligado, para dar para desligar) */
                        if (overlay_tile_count() > 0 || g_set.ov_live)
                              sw(ui, "Transmissão", "Miniaturas ao vivo das transmissões da sala (até 4), cada uma com posição e tamanho próprios.", &g_set.ov_live);
                        if (g_set.ov_live) {   /* uma chave por transmissão: oculta não gasta CPU nem recebe quadros */
                            const int nt = overlay_tile_count();
                            if (!nt) sub(ui, "As transmissões aparecem aqui quando alguém transmite na sua sala.");
                            for (int j = 0; j < nt; j++) {
                                char lb[120]; snprintf(lb, sizeof lb, "Miniatura de %s", overlay_tile_name(j));
                                bool on = overlay_tile_enabled(j); const bool was = on;
                                sw(ui, lb, "Mostra ou oculta só esta transmissão (no modo de edição também dá para usar o botão da barra ou o × da alça).", &on);
                                if (on != was) overlay_tile_enable(j, on);
                            }
                        }
                        sw(ui, "Aviso \"Você está transmitindo a tela\"", "Uma linha por cima do jogo enquanto você divide a sua tela. Desligado, o texto nem é desenhado (as miniaturas continuam).", &g_set.ov_selfshare);
                        sw(ui, "Atravessar cliques", "Ligado: o clique passa pela sobreposição e chega ao jogo. Desligado: dá para arrastar as miniaturas direto, sem entrar no modo de edição.", &g_set.ov_through);
                        sw(ui, "Aviso de mudo", "Mostra quando seu microfone está mudo ou você está ensurdecido.", &g_set.ov_mute);
                        cui_textf(ui, "Opacidade: %d%%", (int)g_set.ov_opacity); cui_slider(ui, "##cfg-ovop", &g_set.ov_opacity, 10, 100);
                        static const char *COR[] = { "Sup. direito", "Sup. esquerdo", "Inf. direito", "Inf. esquerdo" };
                        cui_text(ui, "Canto da tela", .size = 12, .color = B_MUTED);
                        seg(ui, "cfg-ovc", COR, 4, &g_set.ov_corner);
                        sub(ui, "A sobreposição ocupa o canto escolhido do monitor principal e deixa os cliques passarem para o jogo.");
                        sub(ui, "Jogos em tela cheia exclusiva cobrem qualquer sobreposição: use o modo janela ou janela sem bordas.");
                        cui_text(ui, "Posição e tamanho", .size = 12, .color = B_MUTED);
                        cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER, .width = CUI_FILL, .wrap = true) {
                            const bool ed = overlay_edit_active();
                            if (cui_button(ui, ed ? "Concluir edição" : "Editar posições", .variant = ed ? CUI_BUTTON_SECONDARY : CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM, .disabled = !g_set.ov_on)) overlay_edit_toggle();
                            if (g_set.ov_dev[0] && cui_button(ui, "Voltar ao canto", .variant = CUI_BUTTON_LINK, .size = CUI_SIZE_SM)) { settings_ovl_reset(); overlay_edit_set(false); }
                        }
                        sub(ui, g_set.ov_on ? "No modo de edição a sobreposição aceita o mouse: arraste para mover, puxe o canto inferior direito para mudar o tamanho e clique em Concluir. A posição fica salva por monitor. Atalho em qualquer tela: Ctrl+Shift+O."
                                            : "Ligue a sobreposição para editar a posição. Atalho em qualquer tela: Ctrl+Shift+O.");
                        if (overlay_edit_active()) cui_text(ui, "Editando: a sobreposição está na tela, pronta para arrastar.", .size = 12, .color = B_ONLINE, .width = CUI_FILL);
                        else if (g_set.ov_dev[0]) sub(ui, "Usando a posição salva neste monitor. Os cantos acima valem onde não há posição salva.");
                    }
                    break;
                case 5: /* Atalhos */
                    SCARD(ui, "ATALHOS GLOBAIS") {
                        sub(ui, "Funcionam numa chamada, mesmo com o Briks em segundo plano. Clique em Gravar e aperte a combinação; o botão do meio e os laterais do mouse também servem.");
                        key_row(ui, s, 0); key_row(ui, s, 1); key_row(ui, s, 2);
                        sub(ui, "Se outro programa já usa a combinação, o Briks avisa e não a grava.");
                        { const int hs = overlay_probe("overlay_hotkey");
                          cui_text(ui, "Editar a sobreposição: Ctrl+Shift+O", .size = 12, .color = B_MUTED);
                          if (!g_set.ov_on) sub(ui, "Liga junto com a sobreposição (Configurações > Sobreposição). Entra e sai do modo de arrastar e redimensionar em qualquer tela.");
                          else if (hs == 2) cui_text(ui, "Ctrl+Shift+O está em uso por outro programa: o Briks usa Ctrl+Alt+O.", .size = 12, .color = CUI_RGB(0xe8c46a), .width = CUI_FILL);
                          else if (hs == 3) cui_text(ui, "Ctrl+Shift+O e Ctrl+Alt+O estão em uso por outros programas: use o botão Editar posições.", .size = 12, .color = B_DANGER, .width = CUI_FILL);
                          else cui_text(ui, "Ativo agora em qualquer tela: entra e sai do modo de editar a sobreposição.", .size = 12, .color = B_ONLINE, .width = CUI_FILL); }
                    }
                    SCARD(ui, "APERTAR PARA FALAR") {
                        sw(ui, "Apertar para falar", "O microfone fica mudo e só abre enquanto você segura a tecla (numa chamada).", &g_set.ptt_on);
                        if (g_set.ptt_on) {
                            key_row(ui, s, 3);
                            cui_textf(ui, "Atraso ao soltar: %d ms", (int)g_set.ptt_delay); cui_slider(ui, "##cfg-ptt", &g_set.ptt_delay, 0, 1000);
                            if (!g_set.key_ptt[0]) cui_text(ui, "Escolha uma tecla: sem ela o apertar para falar não faz nada.", .size = 12, .color = B_DANGER, .width = CUI_FILL);
                        }
                    }
                    break;
                case 6: /* Notificações */
                    SCARD(ui, "AVISOS DE AMIGOS") {
                        sw(ui, "Amigo ficou online", "Avisa quando alguém da sua lista abre o Briks (ou entra na sua chamada), com o Briks fora de foco.", &g_set.nt_online);
                        sw(ui, "Amigo começou a transmitir", "Avisa quando alguém começa a compartilhar a tela numa chamada.", &g_set.nt_stream);
                    }
                    sys_settings_notifications(ui);   /* winstate.c: nova mensagem e sons */
                    break;
                case 7: /* Avisos */
                    SCARD(ui, "AVISOS") {
                        if (!notice_n) { cui_text(ui, "Nenhum aviso", .font = CUI_FONT_MEDIUM); sub(ui, "Erros e avisos do Briks aparecem aqui. Tudo certo por enquanto."); }
                        for (int i = 0; i < notice_n; i++) cui_text(ui, notices[i], .size = 13);
                        if (notice_n && cui_button(ui, "Limpar avisos", .variant = CUI_BUTTON_SECONDARY)) notice_n = 0;
                    }
                    break;
                case 8: /* Atualizações */
                    update_settings_cards(ui, s);   /* ui_update.c: verificar agora + novidades da versão */
                    break;
                case 9: /* Sobre */
                    sys_settings_system(ui);   /* winstate.c: fechar para a bandeja e iniciar com o Windows */
                    SCARD(ui, "BRIKS") {
                        cui_text(ui, "Briks", .font = CUI_FONT_DISPLAY, .size = 20);
                        sub(ui, "Chamadas e chat ponto a ponto, com grupos assinados e @nick. Compartilhamento de tela em beta.");
                        sw(ui, "Ajudar a melhorar as chamadas", "Envia só contadores anônimos por dia: se a conexão deu certo e se foi direta ou pelo relay. Nunca quem, com quem, sala, IP ou conteúdo.", &g_set.telemetry);
                        tag_nofx(ui);
                        sub(ui, "Feito por Becine Tools · Código-fonte: github.com/ygorbetine/Briks · Licença AGPL");
                        if (cui_button(ui, "Licenças de software de terceiros", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) g_lic_open = true;
                    }
                    break;
                case 10: /* Desempenho */
                    SCARD(ui, "DESEMPENHO") {
                        cui_row(ui, .gap = 8) {
                            for (int l = 0; l < 3; l++) {
                                cui_push_id_int(ui, 700 + l);
                                if (cui_button(ui, briks_perf_label(l), .variant = g_perf == l ? CUI_BUTTON_PRIMARY : CUI_BUTTON_SECONDARY)) briks_perf_set(ui, l, true);
                                cui_pop_id(ui);
                            }
                        }
                        sub(ui, briks_perf_help(g_perf));
                    }
                    break;
                case 11: /* Presença */
                    SCARD(ui, "PRESENÇA") {
                        cui_switch(ui, "Mostrar o programa que estou usando", &s->rp_enabled);
                        if (s->rp_enabled) {
                            static const char *AUD[] = { "Todos os amigos", "Só na chamada" };
                            seg(ui, "cfg-aud", AUD, 2, &s->rp_audience);
                        }
                        sub(ui, "Programas ignorados e status ficam em Perfil e conta.");
                    }
                    break;
                case 12: /* Painéis */
                    SCARD(ui, "PAINÉIS") {
                        sub(ui, "Volta a disposição original: Servidores | Menu | Conversa.");
                        if (cui_button(ui, "Restaurar layout padrão", .variant = CUI_BUTTON_SECONDARY)) { dock_restore_default(); restored_at = (float)cui_time(ui); }
                        if ((float)cui_time(ui) - restored_at < 3.0f) cui_text(ui, "Layout restaurado.", .size = 12, .color = B_ONLINE);
                    }
                    break;
                default: /* Apoiar o Briks */
                    SCARD(ui, "APOIAR O BRIKS") {
                        sub(ui, "O Briks é grátis e não tem anúncios. Se ele é útil para você, uma doação ajuda a pagar os servidores e a manter o desenvolvimento.");
                        if (cui_button(ui, "Apoiar no Ko-fi", .variant = CUI_BUTTON_PRIMARY, .icon = CUI_ICON_HEART)) briks_open_kofi();
                        sub(ui, "Abre a página do Ko-fi no seu navegador. Doar é opcional e não muda nada no app.");
                    }
                    break;
                }
            }
        }
    }
    lic_dialog(ui);   /* BAS-37: por cima das Configurações */
}
