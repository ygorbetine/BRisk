/* Configurações (paridade com o SettingsView do Electron 1.2.2): lateral de seções + cartões de vidro, uma seção por vez.
 * Persistência local em %APPDATA%\Briks-CUI\settings.cfg (linhas chave=valor, gravadas quando algo muda).
 * Cada controle é marcado no relatório como "efeito real" ou "sem efeito real ainda" (UI + persistência, sem backend). */
#include "briks_ui.h"
#include "netdiag.h"
#include "update.h"
#include "core.h"
#include "hotkeys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include <mmsystem.h>
#include <shlobj.h>

typedef struct {
    int mic, out;                 /* índices nas listas de dispositivos (0 = padrão do sistema) */
    char mic_name[64], out_name[64];
    float in_vol, out_vol;        /* 0..100 */
    int noise_off;                /* 0 filtro leve de ruído ligado (Padrão), 1 desligado */
    bool gate_on; float gate_db;  /* porta de ruído, -70..-20 dB */
    bool echo, excl_audio;
    int net_pref, call_mode;      /* conexão: auto/internet direta/VPN privada; chamada: auto/P2P/SFU */
    bool ov_on; float ov_opacity; bool ov_live, ov_voice, ov_status, ov_mute;
    char key_mute[24], key_deafen[24], key_stream[24], key_ptt[24];
    bool ptt_on; float ptt_delay;
    bool nt_online, nt_stream;
    bool telemetry;
    int text_size;
} settings_t;
settings_t g_set;

static const struct { const char *k; char t; void *p; int cap; } F[] = {
    { "mic", 'i', &g_set.mic }, { "out", 'i', &g_set.out }, { "mic_name", 's', g_set.mic_name, 64 }, { "out_name", 's', g_set.out_name, 64 },
    { "in_vol", 'f', &g_set.in_vol }, { "out_vol", 'f', &g_set.out_vol }, { "noise_off", 'i', &g_set.noise_off }, { "gate_on", 'b', &g_set.gate_on },
    { "gate_db", 'f', &g_set.gate_db }, { "echo", 'b', &g_set.echo }, { "excl_audio", 'b', &g_set.excl_audio }, { "net_pref", 'i', &g_set.net_pref },
    { "call_mode", 'i', &g_set.call_mode }, { "ov_on", 'b', &g_set.ov_on }, { "ov_opacity", 'f', &g_set.ov_opacity }, { "ov_live", 'b', &g_set.ov_live },
    { "ov_voice", 'b', &g_set.ov_voice }, { "ov_status", 'b', &g_set.ov_status }, { "ov_mute", 'b', &g_set.ov_mute },
    { "key_mute", 's', g_set.key_mute, 24 }, { "key_deafen", 's', g_set.key_deafen, 24 }, { "key_stream", 's', g_set.key_stream, 24 }, { "key_ptt", 's', g_set.key_ptt, 24 },
    { "ptt_on", 'b', &g_set.ptt_on }, { "ptt_delay", 'f', &g_set.ptt_delay }, { "nt_online", 'b', &g_set.nt_online }, { "nt_stream", 'b', &g_set.nt_stream },
    { "telemetry", 'b', &g_set.telemetry }, { "text_size", 'i', &g_set.text_size },
};
#define NF ((int)(sizeof F / sizeof *F))

static void defaults(void) {
    memset(&g_set, 0, sizeof g_set);
    g_set.text_size = 1;
    g_set.in_vol = 100; g_set.out_vol = 100; g_set.gate_db = -50; g_set.echo = true; g_set.excl_audio = true;
    g_set.ov_opacity = 92; g_set.ov_live = g_set.ov_voice = g_set.ov_status = g_set.ov_mute = true;
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
static void load(void) { load_inner(); g_text_size = g_set.text_size < 0 ? 0 : g_set.text_size > 3 ? 3 : g_set.text_size; }
static void load_inner(void) {
    defaults();
    { const char *ts = getenv("BRIKS_TEXT_SIZE"); if (ts) g_set.text_size = atoi(ts); }   /* capturas */
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

/* ---- armazenamento: tamanho real da pasta de dados ---- */
static unsigned long long dir_size(const char *dir, int depth) {
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd); if (h == INVALID_HANDLE_VALUE) return 0;
    unsigned long long t = 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 4) { char sub[MAX_PATH]; snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName); t += dir_size(sub, depth + 1); } }
        else t += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileA(h, &fd));
    FindClose(h); return t;
}
static void fmt_bytes(unsigned long long b, char *o, size_t cap) {
    if (b < 1024) snprintf(o, cap, "%llu B", b); else if (b < 1048576) snprintf(o, cap, "%.1f KB", b / 1024.0); else snprintf(o, cap, "%.1f MB", b / 1048576.0);
}

/* ---- avisos (erros/avisos do app) ---- */
static char notices[8][120]; static int notice_n;
void settings_notice(const char *text) {
    if (notice_n == 8) { memmove(notices[0], notices[1], 7 * 120); notice_n = 7; }
    snprintf(notices[notice_n++], 120, "%s", text);
}

/* ---- widgets ---- */
#define SCARD(ui, title) CUI__SCOPE((cui_box_begin((ui), CUI__OPTS(cui_box, .width = CUI_FILL, .pad = 14, .gap = 10, .radius = 16, .bg = B_GLASS_CARD, \
    .border = 1, .border_color = B_GLASS_BORDER)), cui_text((ui), (title), .font = CUI_FONT_MEDIUM, .size = 13, .color = B_C3)), cui_box_end(ui))

static void sub(cui_ctx *ui, const char *t) { cui_text(ui, t, .size = 12, .color = B_MUTED); }
static void soon(cui_ctx *ui, const char *what) {   /* o que ainda não tem efeito, dito com clareza */
    char b[200]; snprintf(b, sizeof b, "Em breve: %s", what);
    cui_text(ui, b, .size = 11, .color = g_th.ph);
}
void settings_keys(const char **mute, const char **deafen, const char **stream, const char **ptt, bool *ptt_on, float *delay) { *mute = g_set.key_mute; *deafen = g_set.key_deafen; *stream = g_set.key_stream; *ptt = g_set.key_ptt; *ptt_on = g_set.ptt_on; *delay = g_set.ptt_delay; }
void settings_audio_ex(char *mic, size_t mic_cap, char *out, size_t out_cap, int *noise_off) {
    snprintf(mic, mic_cap, "%s", g_set.mic ? g_set.mic_name : ""); snprintf(out, out_cap, "%s", g_set.out ? g_set.out_name : ""); *noise_off = g_set.noise_off != 0;
}
void settings_audio(float *in_gain, float *out_gain, bool *gate_on, float *gate_db) { *in_gain = g_set.in_vol / 100.0f; *out_gain = g_set.out_vol / 100.0f; *gate_on = g_set.gate_on; *gate_db = g_set.gate_db; }
static void tag_nofx(cui_ctx *ui) {   /* selo discreto: controle salvo localmente, sem efeito real ainda */
    cui_text(ui, "Em breve · salvo neste computador, ainda sem efeito", .size = 11, .color = g_th.ph);
}
static bool menu_item(cui_ctx *ui, const char *icon, const char *label, bool active) {
    cui_box(ui, .id = label, .dir = CUI_ROW, .width = CUI_FILL, .pad_x = 12, .pad_y = 7, .gap = 10, .radius = 12, .align = CUI_ALIGN_CENTER,
            .bg = active ? B_A(B_C3, 0x33) : CUI_RGBA(0x00000000), .border = active ? 1 : 0, .border_color = B_A(B_C3, 0x66)) {
        cui_icon(ui, icon, 15, active ? B_TEXT : B_MUTED);
        cui_text(ui, label, .size = 13, .color = active ? B_TEXT : B_MUTED, .nowrap = true);
    }
    return briks_clicked(ui, label);
}
static void sw(cui_ctx *ui, const char *label, const char *hint, bool *v) {
    cui_switch(ui, label, v); if (hint) sub(ui, hint);
}
static void seg(cui_ctx *ui, const char *id, const char *const *items, int n, int *v) { cui_segmented(ui, id, items, n, v, .full_width = true, .size = CUI_SIZE_SM); }

static const char *TAB_NAMES[] = { "Voz e vídeo", "Rede", "Temas", "Armazenamento", "Sobreposição", "Atalhos", "Notificações", "Avisos", "Atualizações", "Sobre", "Desempenho", "Presença", "Painéis" };
static const char *const TAB_ICONS[] = { CUI_ICON_MIC, CUI_ICON_GLOBE, CUI_ICON_SETTINGS, CUI_ICON_HISTORY, CUI_ICON_MONITOR, CUI_ICON_KEYBOARD, CUI_ICON_BELL, CUI_ICON_MESSAGE_SQUARE,
    CUI_ICON_PLUS, CUI_ICON_USERS, CUI_ICON_SETTINGS, CUI_ICON_USER, CUI_ICON_SETTINGS };
#define NTABS 13

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

void settings_tick(void) {
    static bool loaded; static settings_t last;
    if (!loaded) { loaded = true; load(); enum_devices(); last = g_set; return; }
    if (memcmp(&last, &g_set, sizeof g_set)) {
        snprintf(g_set.mic_name, sizeof g_set.mic_name, "%s", g_set.mic ? in_names[g_set.mic] : "");
        snprintf(g_set.out_name, sizeof g_set.out_name, "%s", g_set.out ? out_names[g_set.out] : "");
        last = g_set; save();
    }
}

void settings_dialog(cui_ctx *ui, app_state_t *s) {
    static float restored_at = -10, cleared_at = -10; static bool was_open, confirm_hist, confirm_all;
    static unsigned long long store_bytes; static float level_sim;
    if (s->settings_open && !was_open) {
        char ad[MAX_PATH]; if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, ad) == S_OK) { strcat(ad, "\\Briks-CUI"); store_bytes = dir_size(ad, 0); }
        enum_devices(); confirm_hist = confirm_all = false; rec_slot = -1; key_msg[0] = 0; key_msg_slot = -1;
    }
    was_open = s->settings_open;
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
                        cui_slider(ui, "##cfg-invol", &g_set.in_vol, 0, 100);
                        sub(ui, "O ganho vale na hora, na sua voz que sai para a sala.");
                    }
                    SCARD(ui, "SAÍDA") {
                        cui_text(ui, "Saída de som", .size = 12, .color = B_MUTED);
                        cui_select(ui, "cfg-out", out_ptr, out_n, &g_set.out);
                        sub(ui, "Vale na hora para as vozes da sala. Se o aparelho sumir, volta ao padrão do sistema.");
                        cui_text(ui, "Volume de saída", .size = 12, .color = B_MUTED);
                        cui_slider(ui, "##cfg-outvol", &g_set.out_vol, 0, 100);
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
                        cui_push_disabled(ui, true);
                        cui_switch(ui, "Cancelamento de eco", &g_set.echo);
                        cui_switch(ui, "Excluir áudio do Briks da transmissão", &g_set.excl_audio);
                        cui_pop_disabled(ui);
                        soon(ui, "cancelamento de eco e excluir o áudio do Briks da transmissão (os interruptores ficam desligados até lá). Dica: use fones para evitar eco");
                    }
                    break;
                case 1: { /* Rede */
                    static const char *CM[] = { "Automático", "Sempre P2P", "Sempre SFU" };
                    static const char *NP[] = { "Automático", "Internet direta", "VPN privada" };
                    SCARD(ui, "MODO DA CHAMADA") {
                        seg(ui, "cfg-cm", CM, 3, &g_set.call_mode);
                        sub(ui, g_set.call_mode == 0 ? "Conexão direta; passa para o servidor em sala grande ou rede ruim." : g_set.call_mode == 1 ? "Conexão direta entre todos, até 8 pessoas. Sem servidor no meio." : "Passa por um servidor. Melhor para salas grandes e redes difíceis.");
                        sub(ui, "Todos da sala precisam usar o mesmo modo."); tag_nofx(ui);
                    }
                    SCARD(ui, "PREFERÊNCIA DE CONEXÃO") {
                        seg(ui, "cfg-np", NP, 3, &g_set.net_pref);
                        sub(ui, g_set.net_pref == 0 ? "Deixa o WebRTC escolher entre VPN, conexão direta e TURN." : g_set.net_pref == 1 ? "Ignora adaptadores VPN e mantém STUN/TURN disponíveis." : "Prioriza ZeroTier, Tailscale ou WireGuard e usa a internet como fallback.");
                        tag_nofx(ui);
                    }
                    SCARD(ui, "TESTE DE CONEXÃO") {
                        netdiag_t nd = netdiag_get();
                        static bool scanned; if (!scanned) { scanned = true; netdiag_vpn_scan(); }
                        sub(ui, "Verifica se o servidor STUN responde e mede atraso, variação e perda (5 pacotes, uns 3 segundos).");
                        if (cui_button(ui, nd.running ? "Testando…" : nd.done ? "Testar de novo" : "Testar conexão", .variant = CUI_BUTTON_PRIMARY, .loading = nd.running)) netdiag_start();
                        if (nd.running) cui_request_wakeup(ui, 0.2f);
                        if (nd.done) {
                            if (!nd.reachable) cui_text(ui, "Sem resposta do STUN: a rede pode bloquear UDP. Chamadas diretas podem falhar; tente outra rede ou configure um TURN.", .size = 12, .color = B_DANGER);
                            else {
                                cui_textf(ui, "Até o STUN: %.0f ms", nd.st.avg_ms);
                                cui_textf(ui, "Variação: %.0f ms", nd.st.jitter_ms);
                                cui_textf(ui, "Perda: %.0f%%", nd.st.loss_pct);
                                cui_text(ui, nd.st.avg_ms < 80 && nd.st.loss_pct < 5 ? "Conexão boa para chamadas." : "A conexão está instável; a qualidade da chamada pode cair.", .size = 12, .color = B_MUTED);
                            }
                        }
                        soon(ui, "ida e volta pelo relay (TURN) e se o relay é necessário");
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
                case 3: { /* Armazenamento */
                    SCARD(ui, "USO NESTE COMPUTADOR") {
                        char b[32]; fmt_bytes(store_bytes, b, sizeof b);
                        cui_row(ui, .width = CUI_FILL) { cui_text(ui, "Dados do Briks", .color = B_MUTED, .width = CUI_FILL); cui_text(ui, b); }
                        sub(ui, "Tudo fica neste dispositivo: mensagens, grupos e anexos permanecem somente neste computador.");
                        sub(ui, "O detalhe por tipo (mensagens, grupos, anexos) ainda não está disponível.");
                    }
                    SCARD(ui, "LIMPEZA") {
                        cui_push_disabled(ui, true);
                        cui_row(ui, .gap = 8, .width = CUI_FILL, .wrap = true) {
                            cui_button(ui, "Limpar anexos", .variant = CUI_BUTTON_SECONDARY);
                            cui_button(ui, "Apagar histórico de mensagens", .variant = CUI_BUTTON_SECONDARY);
                            cui_button(ui, "Exportar meus dados", .variant = CUI_BUTTON_SECONDARY);
                        }
                        cui_pop_disabled(ui);
                        soon(ui, "limpar anexos, apagar o histórico e exportar os dados; os botões ficam desligados até lá");
                    }
                    break; }
                case 4: /* Sobreposição */
                    SCARD(ui, "SOBREPOSIÇÃO EM JOGOS") {
                        sw(ui, "Mostrar sobreposição em jogos", "Mostra a live e quem está na sala por cima do jogo, sem tirar você dele.", &g_set.ov_on);
                        cui_text(ui, "O que aparece", .size = 12, .color = B_MUTED);
                        cui_switch(ui, "Transmissão", &g_set.ov_live); cui_switch(ui, "Quem está na sala", &g_set.ov_voice);
                        cui_switch(ui, "Sala conectada", &g_set.ov_status); cui_switch(ui, "Aviso de mudo", &g_set.ov_mute);
                        cui_textf(ui, "Opacidade: %d%%", (int)g_set.ov_opacity); cui_slider(ui, "##cfg-ovop", &g_set.ov_opacity, 0, 100);
                        cui_button(ui, "Editar posições", .variant = CUI_BUTTON_SECONDARY);
                        tag_nofx(ui);
                    }
                    break;
                case 5: /* Atalhos */
                    SCARD(ui, "ATALHOS GLOBAIS") {
                        sub(ui, "Funcionam numa chamada, mesmo com o Briks em segundo plano. Clique em Gravar e aperte a combinação; o botão do meio e os laterais do mouse também servem.");
                        key_row(ui, s, 0); key_row(ui, s, 1); key_row(ui, s, 2);
                        sub(ui, "Se outro programa já usa a combinação, o Briks avisa e não a grava.");
                        soon(ui, "editar as posições da sobreposição por atalho (Ctrl+Shift+O)");
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
                        sw(ui, "Amigo ficou online", "Avisa quando alguém da sua lista abre o Briks.", &g_set.nt_online);
                        sw(ui, "Amigo começou a transmitir", "Avisa quando alguém começa a compartilhar a tela numa chamada.", &g_set.nt_stream);
                        tag_nofx(ui);
                    }
                    break;
                case 7: /* Avisos */
                    SCARD(ui, "AVISOS") {
                        if (!notice_n) { cui_text(ui, "Nenhum aviso", .font = CUI_FONT_MEDIUM); sub(ui, "Erros e avisos do Briks aparecem aqui. Tudo certo por enquanto."); }
                        for (int i = 0; i < notice_n; i++) cui_text(ui, notices[i], .size = 13);
                        if (notice_n && cui_button(ui, "Limpar avisos", .variant = CUI_BUTTON_SECONDARY)) notice_n = 0;
                    }
                    break;
                case 8: /* Atualizações */
                    SCARD(ui, "ATUALIZAÇÕES") {
                        cui_textf(ui, "Versão %s", update_current_version());
                        { upd_phase_t ph = update_phase();
                          sub(ui, ph == UPD_READY ? "Atualização pronta: ela é instalada ao fechar o Briks." : ph == UPD_DOWNLOADING ? "Baixando a atualização…" :
                                  ph == UPD_ERROR ? "Não foi possível verificar agora; o Briks tenta de novo mais tarde." :
                                  "O Briks verifica novas versões sozinho (a cada 4 horas) quando instalado. BRIKS_DISABLE_UPDATE=1 desliga."); }
                    }
                    break;
                case 9: /* Sobre */
                    SCARD(ui, "BRIKS") {
                        cui_text(ui, "Briks", .font = CUI_FONT_DISPLAY, .size = 20);
                        sub(ui, "Chamadas e chat ponto a ponto, com grupos assinados e @nick. Compartilhamento de tela: em breve.");
                        sw(ui, "Ajudar a melhorar as chamadas", "Envia só contadores anônimos por dia: se a conexão deu certo e se foi direta ou pelo relay. Nunca quem, com quem, sala, IP ou conteúdo.", &g_set.telemetry);
                        tag_nofx(ui);
                        sub(ui, "Feito por Becine Tools · Código-fonte: github.com/ygorbetine/Briks · Licença AGPL");
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
                default: /* Painéis */
                    SCARD(ui, "PAINÉIS") {
                        sub(ui, "Volta a disposição original: Servidores | Menu | Conversa.");
                        if (cui_button(ui, "Restaurar layout padrão", .variant = CUI_BUTTON_SECONDARY)) { dock_restore_default(); restored_at = (float)cui_time(ui); }
                        if ((float)cui_time(ui) - restored_at < 3.0f) cui_text(ui, "Layout restaurado.", .size = 12, .color = B_ONLINE);
                    }
                    break;
                }
            }
        }
    }
}
