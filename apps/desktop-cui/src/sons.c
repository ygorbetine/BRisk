/* Painel de sons (soundboard) do Briks nativo: lado da UI. Ver sons.h e docs/plans/painel-de-sons.md.
 * O core guarda os arquivos por hash e cuida da rede; aqui: espelho do índice, painel, diálogos e reprodução (Media Foundation + waveOut). */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include "sons.h"
#include "msgui.h"
#include "core.h"
#include "volume_ctl.h"
#include <windows.h>
#include <mmsystem.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <commdlg.h>
#include <process.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ estado espelhado do core */
#define MAXS 400
#define MAXMUTED 24
typedef struct { char hash[72], name[40], emoji[40], scope[60], by[72], fmt[8]; unsigned size, dur_ms; bool fav, have; int uses; } son_t;
static son_t g_s[MAXS];
static int g_n;
static bool g_loaded;
static struct { int volume; bool all_muted; int cooldown_ms; char muted[MAXMUTED][72]; int nmuted; } g_pr = { 100, false, 3000, { { 0 } }, 0 };
static char g_fetching[12][72];
static char g_note[200]; static double g_note_t;
static char g_room_gid[56];            /* grupo dono da sala atual ("" = sala avulsa) */
static char g_roster_room[64];         /* sala para a qual a lista de membros já foi enviada */
static bool g_open_panel;              /* painel aberto (para a barra manter visível) */
static char g_search[48];
static int g_sel = -1;                 /* som selecionado (botão direito): barra de ações */
static bool g_people;
static struct { char gid[56], hash[72]; } g_rm[48]; static int g_nrm;   /* removidos aqui e ainda no snapshot confiado */
static volatile LONG g_started, g_active, g_gen;
static volatile LONG g_pv_gen, g_pv_on, g_pv_pos_ms = -1;   /* prévia do editor: geração (parar), tocando, posição em ms */
static volatile float g_pv_vol = 1.0f;
static bool g_editmode;   /* painel em modo Editar: clicar num som seleciona em vez de tocar */
static unsigned long long g_vol_at; static bool g_vol_dirty;

static void say(const char *fmt, const char *a) { snprintf(g_note, sizeof g_note, fmt, a ? a : ""); g_note_t = (double)GetTickCount64() / 1000.0; }
static bool note_live(void) { return g_note[0] && (double)GetTickCount64() / 1000.0 - g_note_t < 6.0; }

static son_t *find_s(const char *hash, const char *scope) {
    for (int i = 0; i < g_n; i++) if (!strcmp(g_s[i].hash, hash) && (!scope || !strcmp(g_s[i].scope, scope))) return &g_s[i];
    return NULL;
}
static bool is_fetching(const char *h) { for (int i = 0; i < 12; i++) if (!strcmp(g_fetching[i], h)) return true; return false; }
static void fetch_set(const char *h, bool on) {
    for (int i = 0; i < 12; i++) if (!strcmp(g_fetching[i], h)) { if (!on) g_fetching[i][0] = 0; return; }
    if (on) for (int i = 0; i < 12; i++) if (!g_fetching[i][0]) { snprintf(g_fetching[i], 72, "%s", h); return; }
}
static bool is_muted_fp(const char *fp) { for (int i = 0; i < g_pr.nmuted; i++) if (!strcmp(g_pr.muted[i], fp)) return true; return false; }

static void take_list(const jval *j) {
    const jval *it = json_get(j, "items"); g_n = 0;
    for (int i = 0; it && it->type == J_ARR && i < json_len(it) && g_n < MAXS; i++) {
        const jval *o = json_at(it, i); son_t *x = &g_s[g_n++]; memset(x, 0, sizeof *x);
        snprintf(x->hash, sizeof x->hash, "%s", json_str(o, "hash", "")); snprintf(x->name, sizeof x->name, "%s", json_str(o, "name", ""));
        snprintf(x->emoji, sizeof x->emoji, "%s", json_str(o, "emoji", "")); snprintf(x->scope, sizeof x->scope, "%s", json_str(o, "scope", "me"));
        snprintf(x->by, sizeof x->by, "%s", json_str(o, "by", "")); snprintf(x->fmt, sizeof x->fmt, "%s", json_str(o, "fmt", ""));
        const jval *v;
        if ((v = json_get(o, "size")) && v->type == J_NUM) x->size = (unsigned)v->num;
        if ((v = json_get(o, "dur_ms")) && v->type == J_NUM) x->dur_ms = (unsigned)v->num;
        if ((v = json_get(o, "fav")) && v->type == J_BOOL) x->fav = v->b;
        if ((v = json_get(o, "have")) && v->type == J_BOOL) x->have = v->b;
        if ((v = json_get(o, "uses")) && v->type == J_NUM) x->uses = (int)v->num;
    }
    const jval *p = json_get(j, "prefs");
    if (p && p->type == J_OBJ) {
        const jval *v;
        if ((v = json_get(p, "volume")) && v->type == J_NUM) g_pr.volume = (int)v->num;
        if ((v = json_get(p, "all_muted")) && v->type == J_BOOL) g_pr.all_muted = v->b;
        if ((v = json_get(p, "cooldown_ms")) && v->type == J_NUM) g_pr.cooldown_ms = (int)v->num;
        const jval *m = json_get(p, "muted"); g_pr.nmuted = 0;
        for (int i = 0; m && m->type == J_ARR && i < json_len(m) && g_pr.nmuted < MAXMUTED; i++) { const jval *e = json_at(m, i); if (e->type == J_STR) snprintf(g_pr.muted[g_pr.nmuted++], 72, "%s", e->str); }
    }
    g_loaded = true;
    if (g_sel >= g_n) g_sel = -1;
}

/* ------------------------------------------------------------------ reprodução */
static volatile LONG g_mf_up;
static void mf_up(void) { if (InterlockedCompareExchange(&g_mf_up, 1, 0) == 0) MFStartup(MF_VERSION, MFSTARTUP_LITE); }

/* decodifica o arquivo para PCM 16 bits (no máximo ~10 s); 0 = falhou */
static bool decode(const wchar_t *wp, short **pcm, size_t *ns, int *rate, int *ch, int max_s) {
    mf_up();
    IMFSourceReader *rd = NULL; IMFAttributes *at = NULL;
    MFCreateAttributes(&at, 1);
    if (FAILED(MFCreateSourceReaderFromURL(wp, at, &rd))) { if (at) IMFAttributes_Release(at); return false; }
    if (at) IMFAttributes_Release(at);
    bool ok = false; IMFMediaType *mt = NULL;
    if (SUCCEEDED(MFCreateMediaType(&mt))) {
        IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio); IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
        IMFMediaType_SetUINT32(mt, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if (SUCCEEDED(IMFSourceReader_SetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, mt))) {
            IMFMediaType *cur = NULL; UINT32 c = 0, r = 0, b = 0;
            if (SUCCEEDED(IMFSourceReader_GetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur))) {
                IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_NUM_CHANNELS, &c); IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &r); IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_BITS_PER_SAMPLE, &b);
                IMFMediaType_Release(cur);
            }
            if (c >= 1 && c <= 2 && r >= 8000 && r <= 192000 && b == 16) {
                IMFSourceReader_SetStreamSelection(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
                const size_t cap = (size_t)r * c * (size_t)max_s;   /* reprodução: 16 s (o core aceita até 15); editor: 120 s */
                short *buf = malloc(cap * sizeof(short)); size_t n = 0;
                while (buf && n < cap) {
                    DWORD fl = 0; IMFSample *smp = NULL;
                    if (FAILED(IMFSourceReader_ReadSample(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, &fl, NULL, &smp))) break;
                    if (fl & MF_SOURCE_READERF_ENDOFSTREAM) { if (smp) IMFSample_Release(smp); break; }
                    if (!smp) continue;
                    IMFMediaBuffer *mb = NULL;
                    if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(smp, &mb))) {
                        BYTE *p = NULL; DWORD len = 0;
                        if (SUCCEEDED(IMFMediaBuffer_Lock(mb, &p, NULL, &len))) {
                            size_t k = len / sizeof(short); if (n + k > cap) k = cap - n;
                            memcpy(buf + n, p, k * sizeof(short)); n += k; IMFMediaBuffer_Unlock(mb);
                        }
                        IMFMediaBuffer_Release(mb);
                    }
                    IMFSample_Release(smp);
                }
                if (buf && n > 0) { *pcm = buf; *ns = n; *rate = (int)r; *ch = (int)c; ok = true; } else free(buf);
            }
        }
        IMFMediaType_Release(mt);
    }
    IMFSourceReader_Release(rd);
    return ok;
}

/* dispositivo de saída escolhido nas configurações (nome; vazio = padrão do sistema) */
static UINT out_device(void) {
    char mic[64], out[64]; int ni = 0; settings_audio_ex(mic, sizeof mic, out, sizeof out, &ni);
    if (!out[0] || !strncmp(out, "Padr", 4)) return WAVE_MAPPER;
    UINT n = waveOutGetNumDevs();
    for (UINT i = 0; i < n; i++) {
        WAVEOUTCAPSW c; char nm[96];
        if (waveOutGetDevCapsW(i, &c, sizeof c) != MMSYSERR_NOERROR) continue;
        if (WideCharToMultiByte(CP_UTF8, 0, c.szPname, -1, nm, sizeof nm, NULL, NULL) && !strncmp(out, nm, strlen(nm))) return i;
    }
    return WAVE_MAPPER;
}

typedef struct { wchar_t path[1040]; short *buf; size_t bns; int brate, bch; float vol; LONG gen; } job_t;   /* buf != NULL: toca amostras já decodificadas (prévia do editor) */
static unsigned __stdcall play_thread(void *arg) {
    job_t *jb = arg; short *pcm = NULL; size_t ns = 0; int rate = 0, ch = 0;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool got; if (jb->buf) { pcm = jb->buf; ns = jb->bns; rate = jb->brate; ch = jb->bch; jb->buf = NULL; got = true; } else got = decode(jb->path, &pcm, &ns, &rate, &ch, 16);
    if (getenv("BRIKS_SONS_LOG")) fprintf(stderr, "[sons] decode=%d ns=%zu rate=%d ch=%d gen_ok=%d dev=%u\n", got, ns, rate, ch, jb->gen == g_gen, out_device());
    if (got && jb->gen == g_gen) {
        const float g = sons_peak_gain(pcm, ns, jb->vol);   /* limita o pico a -3 dBFS (mp3/ogg) e aplica o volume do painel */
        for (size_t i = 0; i < ns; i++) { float v = pcm[i] * g; pcm[i] = (short)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : v); }
        WAVEFORMATEX wf = { WAVE_FORMAT_PCM, (WORD)ch, (DWORD)rate, (DWORD)(rate * ch * 2), (WORD)(ch * 2), 16, 0 };
        HWAVEOUT ho = NULL;
        MMRESULT mr = waveOutOpen(&ho, out_device(), &wf, 0, 0, CALLBACK_NULL);
        if (getenv("BRIKS_SONS_LOG")) fprintf(stderr, "[sons] waveOutOpen=%u\n", (unsigned)mr);
        if (mr == MMSYSERR_NOERROR) {
            WAVEHDR h; memset(&h, 0, sizeof h); h.lpData = (LPSTR)pcm; h.dwBufferLength = (DWORD)(ns * sizeof(short));
            if (waveOutPrepareHeader(ho, &h, sizeof h) == MMSYSERR_NOERROR) {
                waveOutWrite(ho, &h, sizeof h);
                while (!(h.dwFlags & WHDR_DONE) && jb->gen == g_gen) Sleep(15);
                waveOutReset(ho);
                waveOutUnprepareHeader(ho, &h, sizeof h);
            }
            waveOutClose(ho);
        }
    }
    free(pcm); free(jb);
    InterlockedDecrement(&g_active);
    CoUninitialize();
    return 0;
}

static void play_path(const char *utf8, float vol01) {
    if (!utf8 || !utf8[0]) return;
    InterlockedIncrement(&g_started);   /* contado antes de tocar: os roteiros de teste não dependem de placa de som */
    if (g_active >= 4) return;          /* no máximo 4 ao mesmo tempo */
    job_t *jb = calloc(1, sizeof *jb); if (!jb) return;
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, jb->path, (int)(sizeof jb->path / sizeof jb->path[0]))) { free(jb); return; }
    jb->vol = vol01; jb->gen = g_gen;
    InterlockedIncrement(&g_active);
    uintptr_t th = _beginthreadex(NULL, 0, play_thread, jb, 0, NULL);
    if (!th) { InterlockedDecrement(&g_active); free(jb); } else CloseHandle((HANDLE)th);
}

/* o core resolve hash -> caminho; a reprodução espera essa resposta */
typedef struct { char hash[72]; bool preview; unsigned long long at; } pend_t;
static pend_t g_pend[8];
static void play_hash(const char *hash, bool preview) {
    for (int i = 0; i < 8; i++) if (!g_pend[i].hash[0] || GetTickCount64() - g_pend[i].at > 5000) {
        snprintf(g_pend[i].hash, sizeof g_pend[i].hash, "%s", hash); g_pend[i].preview = preview; g_pend[i].at = GetTickCount64();
        char l[200]; snprintf(l, sizeof l, "{\"cmd\":\"sound.path\",\"hash\":\"%s\"}", hash); core_send(l);
        return;
    }
}

void sons_stop_all(void) { InterlockedIncrement(&g_gen); }
void sons_shutdown(void) { InterlockedIncrement(&g_gen); InterlockedIncrement(&g_pv_gen); for (int i = 0; i < 100 && (g_active > 0 || g_pv_on); i++) Sleep(10); }
bool sons_panel_open(void) { return g_open_panel; }

/* ------------------------------------------------------------------ envio de preferências */
static void send_prefs(const char *extra) {
    char l[260]; snprintf(l, sizeof l, "{\"cmd\":\"sound.prefs\",\"volume\":%d,\"all_muted\":%s,\"cooldown_ms\":%d%s}", g_pr.volume, g_pr.all_muted ? "true" : "false", g_pr.cooldown_ms, extra ? extra : "");
    core_send(l);
}
static void send_scoped(const char *cmd, const son_t *x, const char *more) {
    char en[160], ee[160], l[700]; json_escape(x->name, en, sizeof en); json_escape(x->emoji, ee, sizeof ee);
    snprintf(l, sizeof l, "{\"cmd\":\"%s\",\"hash\":\"%s\",\"scope\":\"%s\",\"name\":\"%s\",\"emoji\":\"%s\"%s}", cmd, x->hash, x->scope, en, ee, more ? more : "");
    core_send(l);
}

/* ------------------------------------------------------------------ catálogo do grupo (manifesto assinado) */
static bool removed_here(const char *gid, const char *hash) { for (int i = 0; i < g_nrm; i++) if (!strcmp(g_rm[i].gid, gid) && !strcmp(g_rm[i].hash, hash)) return true; return false; }

static void app_entry(char **buf, size_t *n, size_t *cap, bool *first, const char *h, const char *nm, const char *em, unsigned sz, unsigned d, const char *by) {
    char en[160], ee[160], eb[160], t[600];
    json_escape(nm, en, sizeof en); json_escape(em, ee, sizeof ee); json_escape(by, eb, sizeof eb);
    int k = snprintf(t, sizeof t, "%s{\"h\":\"%s\",\"n\":\"%s\",\"e\":\"%s\",\"s\":%u,\"d\":%u,\"b\":\"%s\"}", *first ? "" : ",", h, en, ee, sz, d, eb);
    if (k <= 0) return;
    if (*n + (size_t)k + 2 > *cap) { *cap = (*n + (size_t)k) * 2 + 64; *buf = realloc(*buf, *cap); }
    memcpy(*buf + *n, t, (size_t)k + 1); *n += (size_t)k; *first = false;
}

char *sons_group_catalog_json(const char *gid, const char *stored) {
    char scope[80]; snprintf(scope, sizeof scope, "g:%s", gid);
    size_t n = 1, cap = 512; char *buf = malloc(cap); if (!buf) return NULL; buf[0] = '['; buf[1] = 0; bool first = true; int count = 0;
    jval *sn = stored ? json_parse(stored) : NULL; const jval *ss = sn ? json_get(sn, "sounds") : NULL;
    for (int i = 0; ss && ss->type == J_ARR && i < json_len(ss) && count < 50; i++) {   /* o que o grupo já tem, menos o que apaguei aqui */
        const jval *o = json_at(ss, i); const char *h = json_str(o, "h", "");
        if (!h[0] || removed_here(gid, h)) continue;
        const jval *sz = json_get(o, "s"), *d = json_get(o, "d");
        app_entry(&buf, &n, &cap, &first, h, json_str(o, "n", ""), json_str(o, "e", ""), sz && sz->type == J_NUM ? (unsigned)sz->num : 0, d && d->type == J_NUM ? (unsigned)d->num : 0, json_str(o, "b", "")); count++;
    }
    for (int i = 0; i < g_n && count < 50; i++) {   /* e o que acabei de importar para o grupo (ainda fora do snapshot) */
        const son_t *x = &g_s[i]; if (strcmp(x->scope, scope)) continue;
        bool dup = false;
        for (int k = 0; ss && ss->type == J_ARR && k < json_len(ss); k++) if (!strcmp(json_str(json_at(ss, k), "h", ""), x->hash)) dup = true;
        if (dup) continue;
        app_entry(&buf, &n, &cap, &first, x->hash, x->name, x->emoji, x->size, x->dur_ms, x->by); count++;
    }
    if (sn) json_free(sn);
    if (n + 2 > cap) buf = realloc(buf, n + 2);
    buf[n++] = ']'; buf[n] = 0;
    return buf;
}

void sons_note_snapshot(const char *gid, const jval *snapshot) {   /* um snapshot confiado chegou: os "removidos aqui" que ele já não tem saem da lista */
    const jval *ss = snapshot ? json_get(snapshot, "sounds") : NULL;
    for (int i = 0; i < g_nrm;) {
        bool still = false;
        for (int k = 0; ss && ss->type == J_ARR && k < json_len(ss); k++) if (!strcmp(json_str(json_at(ss, k), "h", ""), g_rm[i].hash)) still = true;
        if (!strcmp(g_rm[i].gid, gid) && !still) { g_rm[i] = g_rm[--g_nrm]; } else i++;
    }
}

void sons_catalog_sync(const char *snapshot_json) {
    if (!snapshot_json || !strstr(snapshot_json, "\"sounds\"")) { return; }   /* grupo sem sons: nada a espelhar (e o core só espelha quem tem assinatura) */
    size_t l = strlen(snapshot_json) + 40; char *c = malloc(l); if (!c) return;
    snprintf(c, l, "{\"cmd\":\"sound.catalog\",\"snapshot\":%s}", snapshot_json);
    core_send(c); free(c);
}

void sons_ask_missing(void) { for (int i = 0; i < g_n; i++) if (!g_s[i].have && !is_fetching(g_s[i].hash)) { char l[200]; snprintf(l, sizeof l, "{\"cmd\":\"sound.fetch\",\"hash\":\"%s\"}", g_s[i].hash); core_send(l); } }

/* ------------------------------------------------------------------ eventos do core */
static const char *peer_name(const app_state_t *s, const char *fp) {
    if (!strcmp(fp, s->rtc_fp)) return "Você";
    for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (s->rtc_peers[i].fp[0] && !strcmp(s->rtc_peers[i].fp, fp) && s->rtc_peers[i].name[0]) return s->rtc_peers[i].name;
    return "Alguém";
}

static char g_click_hash[72]; static unsigned long long g_click_at;   /* som clicado esperando o eco do core (sound.play/denied) */
static void add_done(app_state_t *s, const jval *j);
static void check_done(const jval *j);
static void ogg_done(const jval *j);

void sons_core_event(app_state_t *s, const jval *j, const char *ev) {
    if (!strcmp(ev, "sound.list")) {
        take_list(j);
        const jval *m = json_get(j, "missing");
        for (int i = 0; m && m->type == J_ARR && i < json_len(m); i++) { const jval *e = json_at(m, i); if (e->type == J_STR) fetch_set(e->str, true); }
        const char *err = json_str(j, "error", ""); if (err[0]) say("%s", err);
    } else if (!strcmp(ev, "sound.checked")) check_done(j);
    else if (!strcmp(ev, "sound.decoded")) ogg_done(j);
    else if (!strcmp(ev, "sound.imported")) add_done(s, j);
    else if (!strcmp(ev, "sound.path")) {
        const char *h = json_str(j, "hash", ""), *p = json_str(j, "path", "");
        for (int i = 0; i < 8; i++) if (g_pend[i].hash[0] && !strcmp(g_pend[i].hash, h)) {
            if (p[0] && GetTickCount64() - g_pend[i].at < 5000) play_path(p, (float)g_pr.volume / 100.0f);
            g_pend[i].hash[0] = 0;
        }
    } else if (!strcmp(ev, "sound.play")) {
        const char *h = json_str(j, "hash", ""), *fp = json_str(j, "from_fp", "");
        const jval *sf = json_get(j, "self"); const bool me = sf && sf->type == J_BOOL && sf->b;
        if (me) g_click_hash[0] = 0;
        if (s->deaf) return;   /* ensurdecido não ouve som (o core já descarta, aqui é a segunda trava) */
        play_hash(h, false);
        if (!me) { char b[160]; snprintf(b, sizeof b, "%s tocou %s", peer_name(s, fp), json_str(j, "name", "um som")); snprintf(g_note, sizeof g_note, "%s", b); g_note_t = (double)GetTickCount64() / 1000.0; }
    } else if (!strcmp(ev, "sound.fetching")) fetch_set(json_str(j, "hash", ""), true);
    else if (!strcmp(ev, "sound.got")) fetch_set(json_str(j, "hash", ""), false);
    else if (!strcmp(ev, "sound.fail")) { fetch_set(json_str(j, "hash", ""), false); say("Não consegui baixar um som: %s", json_str(j, "message", "falhou")); }
    else if (!strcmp(ev, "sound.denied")) { g_click_hash[0] = 0; say("%s", json_str(j, "message", "Som barrado")); }
    else if (!strcmp(ev, "sound.drop")) { const char *r = json_str(j, "reason", ""); if (!strcmp(r, "silenciado") || !strcmp(r, "intervalo")) return; say("Som descartado: %s", r); }
}

/* ------------------------------------------------------------------ diálogos: adicionar (mini editor), renomear, apagar */
#define NBARS 120
#define MAX_CLIP_MS 15000.0f
#define MIN_CLIP_MS 200.0f
#define MAX_FILE_BYTES (512 * 1024)
static struct {
    bool open, loaded, converting; char path[1040], err[200], name[48], emoji[40]; int dest; bool ack;
    short *pcm; size_t ns; int rate, ch; float dur_ms, start_ms, end_ms, vol; float bars[NBARS];
} g_add;
static char g_tmp_import[1040];   /* arquivo temporário gerado pelo editor, apagado depois da importação */
static struct { bool open; char hash[72], scope[60], name[48], emoji[40]; } g_ren;
static struct { bool open; char hash[72], scope[60], name[48]; } g_del;

static void ed_free(void) { free(g_add.pcm); g_add.pcm = NULL; g_add.ns = 0; g_add.loaded = false; }

static int admin_groups(const app_state_t *s, int *idx, int max) {
    int n = 0; for (int i = 0; i < s->group_count && n < max; i++) if (s->groups[i].can_moderate) idx[n++] = i; return n;
}

/* ---- conversão: recorte + volume + limite de pico, reamostra para 44,1 kHz e codifica em MP3 (Media Foundation); sem codificador, WAV mono de 16 kHz ---- */
static short *resample(const short *in, size_t frames, int ch, int r_in, int r_out, size_t *out_frames) {
    if (r_in == r_out) { short *o = malloc(frames * (size_t)ch * sizeof(short)); if (!o) return NULL; memcpy(o, in, frames * (size_t)ch * sizeof(short)); *out_frames = frames; return o; }
    size_t nf = (size_t)((double)frames * r_out / r_in); if (nf < 1) nf = 1;
    short *o = malloc(nf * (size_t)ch * sizeof(short)); if (!o) return NULL;
    for (size_t i = 0; i < nf; i++) {
        double pos = (double)i * r_in / r_out; size_t i0 = (size_t)pos; double fr = pos - (double)i0; size_t i1 = i0 + 1 < frames ? i0 + 1 : i0;
        for (int c = 0; c < ch; c++) o[i * (size_t)ch + (size_t)c] = (short)((double)in[i0 * (size_t)ch + (size_t)c] * (1.0 - fr) + (double)in[i1 * (size_t)ch + (size_t)c] * fr);
    }
    *out_frames = nf; return o;
}

static bool write_wav(const wchar_t *wp, const short *pcm, size_t frames, int rate, int ch) {
    FILE *f = _wfopen(wp, L"wb"); if (!f) return false;
    const unsigned data = (unsigned)(frames * (size_t)ch * 2), riff = 36 + data; const unsigned short fmt = 1, chs = (unsigned short)ch, ba = (unsigned short)(ch * 2), bits = 16; const unsigned fl = 16, sr = (unsigned)rate, br = (unsigned)(rate * ch * 2);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fl, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&chs, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f); fwrite(pcm, 2, frames * (size_t)ch, f);
    fclose(f); return true;
}

static bool encode_mp3(const wchar_t *wp, const short *pcm, size_t frames, int rate, int ch) {
    mf_up();
    IMFSinkWriter *sw = NULL; IMFMediaType *ot = NULL, *it = NULL; bool ok = false; DWORD idx = 0;
    if (FAILED(MFCreateSinkWriterFromURL(wp, NULL, NULL, &sw))) return false;
    if (FAILED(MFCreateMediaType(&ot)) || FAILED(MFCreateMediaType(&it))) goto done;
    IMFMediaType_SetGUID(ot, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio); IMFMediaType_SetGUID(ot, &MF_MT_SUBTYPE, &MFAudioFormat_MP3);
    IMFMediaType_SetUINT32(ot, &MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)rate); IMFMediaType_SetUINT32(ot, &MF_MT_AUDIO_NUM_CHANNELS, (UINT32)ch);
    IMFMediaType_SetUINT32(ot, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, ch == 1 ? 12000 : 16000); IMFMediaType_SetUINT32(ot, &MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);
    if (FAILED(IMFSinkWriter_AddStream(sw, ot, &idx))) goto done;
    IMFMediaType_SetGUID(it, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio); IMFMediaType_SetGUID(it, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    IMFMediaType_SetUINT32(it, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16); IMFMediaType_SetUINT32(it, &MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)rate); IMFMediaType_SetUINT32(it, &MF_MT_AUDIO_NUM_CHANNELS, (UINT32)ch);
    IMFMediaType_SetUINT32(it, &MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32)(ch * 2)); IMFMediaType_SetUINT32(it, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(rate * ch * 2));
    if (FAILED(IMFSinkWriter_SetInputMediaType(sw, idx, it, NULL))) goto done;
    if (FAILED(IMFSinkWriter_BeginWriting(sw))) goto done;
    {
        const size_t chunk = (size_t)rate / 2; LONGLONG t = 0; bool bad = false;
        for (size_t at = 0; at < frames && !bad; at += chunk) {
            size_t n = frames - at < chunk ? frames - at : chunk; DWORD bytes = (DWORD)(n * (size_t)ch * 2);
            IMFMediaBuffer *mb = NULL; IMFSample *sm = NULL; BYTE *p = NULL;
            if (FAILED(MFCreateMemoryBuffer(bytes, &mb))) { bad = true; break; }
            if (SUCCEEDED(IMFMediaBuffer_Lock(mb, &p, NULL, NULL))) { memcpy(p, pcm + at * (size_t)ch, bytes); IMFMediaBuffer_Unlock(mb); } else bad = true;
            IMFMediaBuffer_SetCurrentLength(mb, bytes);
            if (!bad && SUCCEEDED(MFCreateSample(&sm))) {
                const LONGLONG dur = (LONGLONG)n * 10000000LL / rate;
                IMFSample_AddBuffer(sm, mb); IMFSample_SetSampleTime(sm, t); IMFSample_SetSampleDuration(sm, dur);
                if (FAILED(IMFSinkWriter_WriteSample(sw, idx, sm))) bad = true;
                t += dur;
            } else bad = true;
            if (sm) IMFSample_Release(sm);
            IMFMediaBuffer_Release(mb);
        }
        ok = !bad && SUCCEEDED(IMFSinkWriter_Finalize(sw));
    }
done:
    if (ot) IMFMediaType_Release(ot);
    if (it) IMFMediaType_Release(it);
    IMFSinkWriter_Release(sw);
    return ok;
}

static long long file_size_w(const wchar_t *wp) { WIN32_FILE_ATTRIBUTE_DATA a; if (!GetFileAttributesExW(wp, GetFileExInfoStandard, &a)) return -1; return ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow; }

/* recorte atual como amostras novas (malloc): [início, fim) em quadros */
static short *render_sel(size_t *frames_out) {
    const size_t total = g_add.ns / (size_t)g_add.ch;
    size_t a = (size_t)(g_add.start_ms * (float)g_add.rate / 1000.0f), b = (size_t)(g_add.end_ms * (float)g_add.rate / 1000.0f);
    if (b > total) b = total; if (a >= b) return NULL;
    short *o = malloc((b - a) * (size_t)g_add.ch * sizeof(short)); if (!o) return NULL;
    memcpy(o, g_add.pcm + a * (size_t)g_add.ch, (b - a) * (size_t)g_add.ch * sizeof(short));
    *frames_out = b - a; return o;
}

/* grava o trecho no formato final. outpath vazio = arquivo temporário; devolve o caminho escrito em outpath (UTF-8) */
static bool export_sel(char *outpath, size_t cap, char *err, size_t ecap) {
    size_t fr = 0; short *sel = render_sel(&fr);
    if (!sel) { snprintf(err, ecap, "Nada para exportar: o trecho está vazio"); return false; }
    const int ch = g_add.ch; const float g = sons_peak_gain(sel, fr * (size_t)ch, g_add.vol / 100.0f);   /* volume do editor + limite de -3 dBFS */
    for (size_t i = 0; i < fr * (size_t)ch; i++) { float v = sel[i] * g; sel[i] = (short)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : v); }
    wchar_t wp[1040]; bool mp3 = true;
    if (outpath[0]) { MultiByteToWideChar(CP_UTF8, 0, outpath, -1, wp, 1040); const char *dot = strrchr(outpath, '.'); mp3 = dot && !_stricmp(dot, ".mp3"); }
    else { wchar_t td[900]; GetTempPathW(900, td); swprintf(wp, 1040, L"%lsbriks-som-%llu.mp3", td, (unsigned long long)GetTickCount64()); }
    bool ok = false; size_t rf = 0; short *rs = NULL;
    if (mp3) {
        rs = resample(sel, fr, ch, g_add.rate, g_add.rate == 48000 ? 48000 : 44100, &rf);
        if (rs && encode_mp3(wp, rs, rf, g_add.rate == 48000 ? 48000 : 44100, ch) && file_size_w(wp) > 0 && file_size_w(wp) <= MAX_FILE_BYTES) ok = true;
        free(rs); rs = NULL;
    }
    if (!ok) {   /* sem codificador (ou ficou grande): WAV mono de 16 kHz, 15 s cabem em 480 KB */
        short *mono = malloc(fr * sizeof(short)); size_t mf = 0;
        if (mono) { for (size_t i = 0; i < fr; i++) { int a = 0; for (int c = 0; c < ch; c++) a += sel[i * (size_t)ch + (size_t)c]; mono[i] = (short)(a / ch); }
            rs = resample(mono, fr, 1, g_add.rate, 16000, &mf); free(mono); }
        if (rs) {
            size_t n = wcslen(wp); if (n > 4) { wp[n - 4] = 0; wcscat(wp, L".wav"); }
            ok = write_wav(wp, rs, mf, 16000, 1) && file_size_w(wp) <= MAX_FILE_BYTES;
            free(rs);
        }
    }
    free(sel);
    if (!ok) { snprintf(err, ecap, "Não consegui converter o áudio"); return false; }
    WideCharToMultiByte(CP_UTF8, 0, wp, -1, outpath, (int)cap, NULL, NULL);
    return true;
}

static bool unedited_original_ok(void) {   /* arquivo já serve como está (mp3/wav/ogg curto e leve, sem recorte nem volume): vai sem reconverter */
    if (g_add.start_ms > 1.0f || g_add.end_ms < g_add.dur_ms - 1.0f || g_add.vol < 99.5f || g_add.dur_ms > MAX_CLIP_MS) return false;
    const char *dot = strrchr(g_add.path, '.'); if (!dot || (_stricmp(dot, ".mp3") && _stricmp(dot, ".wav"))) return false;   /* OGG sempre passa pelo editor (vira MP3, que toca em qualquer Windows) */
    wchar_t wp[1040]; MultiByteToWideChar(CP_UTF8, 0, g_add.path, -1, wp, 1040);
    const long long sz = file_size_w(wp); return sz > 0 && sz <= MAX_FILE_BYTES;
}

static bool editor_load(const char *path) {
    wchar_t wp[1040]; if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1040)) return false;
    short *pcm = NULL; size_t ns = 0; int rate = 0, ch = 0;
    if (!decode(wp, &pcm, &ns, &rate, &ch, 120)) return false;   /* até 120 s do arquivo (vídeos incluídos) entram no editor */
    ed_free(); g_add.pcm = pcm; g_add.ns = ns; g_add.rate = rate; g_add.ch = ch;
    const size_t frames = ns / (size_t)ch; g_add.dur_ms = (float)frames * 1000.0f / (float)rate;
    g_add.start_ms = 0; g_add.end_ms = g_add.dur_ms < MAX_CLIP_MS ? g_add.dur_ms : MAX_CLIP_MS; g_add.vol = 100;
    for (int b = 0; b < NBARS; b++) {
        size_t a = frames * (size_t)b / NBARS, e = frames * (size_t)(b + 1) / NBARS; if (e <= a) e = a + 1; int pk = 0;
        for (size_t i = a; i < e && i < frames; i++) for (int c = 0; c < ch; c++) { int v = pcm[i * (size_t)ch + (size_t)c]; if (v < 0) v = -v; if (v > pk) pk = v; }
        g_add.bars[b] = (float)pk / 32768.0f;
    }
    float mx = 0.05f; for (int b = 0; b < NBARS; b++) if (g_add.bars[b] > mx) mx = g_add.bars[b];
    for (int b = 0; b < NBARS; b++) g_add.bars[b] /= mx;
    snprintf(g_add.path, sizeof g_add.path, "%s", path); g_add.loaded = true; g_add.err[0] = 0;
    return true;
}

static void begin_load(const char *path, bool keep);
static void pick_file(void) {
    wchar_t wp[1040] = { 0 }; OPENFILENAMEW of = { sizeof of };
    of.hwndOwner = GetActiveWindow(); of.lpstrFilter = L"Áudio e vídeo\0*.mp3;*.wav;*.ogg;*.m4a;*.aac;*.wma;*.flac;*.mp4;*.m4v;*.mov;*.webm;*.mkv\0"; of.lpstrFile = wp; of.nMaxFile = (DWORD)(sizeof wp / sizeof wp[0]);
    of.lpstrTitle = L"Escolher o áudio do som (você corta e ajusta depois)"; of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&of)) return;
    char path[1040]; if (!WideCharToMultiByte(CP_UTF8, 0, wp, -1, path, (int)sizeof path, NULL, NULL)) return;
    begin_load(path, g_add.open);
}

static char g_keep_nm[48], g_keep_em[40]; static int g_keep_dest; static bool g_keep_on; static char g_ogg_src[1040];   /* o que o editor lembra ao trocar de arquivo; origem do OGG em conversão */

static void after_load(const char *path) {
    g_add.open = true;
    if (g_keep_on && g_keep_nm[0]) { snprintf(g_add.name, sizeof g_add.name, "%s", g_keep_nm); snprintf(g_add.emoji, sizeof g_add.emoji, "%s", g_keep_em); g_add.dest = g_keep_dest; }
    else { memset(g_add.name, 0, sizeof g_add.name); g_add.dest = 0; g_add.ack = false; }
    if (!g_add.name[0]) {
        const char *b = strrchr(path, '\\'); const char *b2 = strrchr(path, '/'); if (b2 > b) b = b2; b = b ? b + 1 : path; char stem[96]; snprintf(stem, sizeof stem, "%s", b); char *dot = strrchr(stem, '.'); if (dot) *dot = 0;
        sons_clean_name(stem, g_add.name, sizeof g_add.name);
    }
}

static bool is_ogg(const char *path) { const char *dot = strrchr(path, '.'); return dot && !_stricmp(dot, ".ogg"); }

/* abre o arquivo no editor. OGG não depende do Windows: o core decodifica (Vorbis embutido) para um WAV temporário e o editor abre esse WAV */
static void begin_load(const char *path, bool keep) {
    g_keep_on = keep; snprintf(g_keep_nm, sizeof g_keep_nm, "%s", g_add.name); snprintf(g_keep_em, sizeof g_keep_em, "%s", g_add.emoji); g_keep_dest = g_add.dest;
    if (is_ogg(path)) {
        ed_free(); g_add.open = true; g_add.err[0] = 0; g_add.converting = true; snprintf(g_ogg_src, sizeof g_ogg_src, "%s", path);
        char e[2200], l[2300]; json_escape(path, e, sizeof e); snprintf(l, sizeof l, "{\"cmd\":\"sound.ogg\",\"path\":\"%s\"}", e); core_send(l);
        return;
    }
    if (!editor_load(path)) { g_add.open = true; ed_free(); snprintf(g_add.err, sizeof g_add.err, "Não consegui ler o áudio desse arquivo. Use MP3, WAV, OGG ou um vídeo com som."); return; }
    after_load(path);
}

static void ogg_done(const jval *j) {
    if (!g_add.converting) return;
    g_add.converting = false;
    const jval *ok = json_get(j, "ok"); const char *wav = json_str(j, "wav", "");
    if (ok && ok->type == J_BOOL && ok->b && wav[0] && editor_load(wav)) { snprintf(g_add.path, sizeof g_add.path, "%s", g_ogg_src); after_load(g_ogg_src); }
    else { ed_free(); g_add.open = true; snprintf(g_add.err, sizeof g_add.err, "Não consegui ler esse OGG: %s", json_str(j, "message", "formato não suportado")); }
    { const char *bn = strrchr(wav, '\\'); bn = bn ? bn + 1 : wav; if (!strncmp(bn, "briks-ogg-", 10)) DeleteFileA(wav); }   /* só apaga o temporário que o core criou */
}

static void check_done(const jval *j) { (void)j; }   /* o editor decodifica o arquivo aqui mesmo; o core confere o resultado na importação */

static void add_done(app_state_t *s, const jval *j) {
    const jval *ok = json_get(j, "ok");
    if (g_tmp_import[0]) { DeleteFileA(g_tmp_import); g_tmp_import[0] = 0; }
    if (ok && ok->type == J_BOOL && ok->b) {
        const jval *it = json_get(j, "item"); const char *sc = it ? json_str(it, "scope", "me") : "me";
        g_add.open = false; ed_free(); say("Som adicionado: %s", g_add.name);
        if (!strncmp(sc, "g:", 2)) groups_republish(s, sc + 2);   /* o catálogo do grupo vai no manifesto assinado */
    } else snprintf(g_add.err, sizeof g_add.err, "%s", json_str(j, "message", "Não consegui adicionar"));
}

static void emoji_cb(const char *e, void *user) { char *dst = user; if (e) snprintf(dst, 40, "%s", e); }

static void field_row(cui_ctx *ui, const char *name_id, char *name, size_t cap, char *emoji, const char *emoji_id) {
    cui_row(ui, .gap = 8, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
        cui_push_id(ui, emoji_id);
        cui_box(ui, .id = "e", .width = CUI_PX(40), .height = CUI_PX(40), .radius = 12, .bg = CUI_RGBA(0xffffff12), .border = 1, .border_color = B_GLASS_BORDER_SOFT, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
            if (emoji[0]) cui_emoji(ui, emoji, 22); else cui_icon(ui, CUI_ICON_SMILE, 18, B_MUTED);
        }
        if (briks_hover(ui, "e")) briks_tip("Escolher um emoji (opcional)");
        if (briks_clicked_r(ui, "e", 12.0f) && !msgui_emoji_picking()) { const cui_rect er = cui_get_rect(ui, cui_make_id(ui, "e")); msgui_emoji_pick(er.x, er.y + er.h + 6.0f, emoji_cb, emoji); }
        cui_pop_id(ui);
        cui_input(ui, name_id, name, cap, .placeholder = "Nome do som", .width = CUI_FILL);
    }
}

typedef struct { float bars[NBARS]; float a, b, play; } wf_t;   /* a, b: início e fim do recorte (0..1); play: posição da prévia (0..1, <0 = parada) */
static void paint_wf(cui_ctx *ui, cui_rect r, void *user) {
    const wf_t *w = (const wf_t *)user;
    cui_draw_rect(ui, r, CUI_CORNERS(10), CUI_RGBA(0x00000050));
    const float bw = (r.w - 16.0f) / NBARS, mid = r.y + r.h * 0.5f;
    for (int i = 0; i < NBARS; i++) {
        float h = w->bars[i] * (r.h - 22.0f); if (h < 3.0f) h = 3.0f;
        const float pos = ((float)i + 0.5f) / NBARS; const bool in = pos >= w->a && pos <= w->b;
        cui_draw_rect(ui, (cui_rect){ r.x + 8.0f + (float)i * bw + 0.5f, mid - h * 0.5f, bw > 2.0f ? bw - 1.5f : 1.0f, h }, CUI_CORNERS(1), in ? CUI_RGB(0xc4b0ff) : CUI_RGBA(0xffffff30));
    }
    const float xa = r.x + 8.0f + w->a * (r.w - 16.0f), xb = r.x + 8.0f + w->b * (r.w - 16.0f);
    cui_draw_rect(ui, (cui_rect){ xa - 1.0f, r.y + 6.0f, 2.0f, r.h - 12.0f }, CUI_CORNERS(1), CUI_RGB(0xffffff));
    cui_draw_rect(ui, (cui_rect){ xb - 1.0f, r.y + 6.0f, 2.0f, r.h - 12.0f }, CUI_CORNERS(1), CUI_RGB(0xffffff));
    cui_draw_rect(ui, (cui_rect){ xa - 5.0f, r.y + 2.0f, 10.0f, 6.0f }, CUI_CORNERS(3), CUI_RGB(0xffffff));   /* pegadores: arraste para mover início e fim */
    cui_draw_rect(ui, (cui_rect){ xb - 5.0f, r.y + r.h - 8.0f, 10.0f, 6.0f }, CUI_CORNERS(3), CUI_RGB(0xffffff));
    if (w->play >= 0.0f && w->play <= 1.0f) cui_draw_rect(ui, (cui_rect){ r.x + 8.0f + w->play * (r.w - 16.0f) - 1.0f, r.y + 4.0f, 2.0f, r.h - 8.0f }, CUI_CORNERS(1), CUI_RGB(0x57d28b));
}

/* Prévia do editor: toca só o trecho entre as barras (do início ao fim), uma por vez (clicar de novo para), e o volume vale em tempo real:
   o áudio sai em pedaços de 30 ms e cada pedaço é multiplicado pelo volume do editor no momento em que é escrito. */
typedef struct { short *buf; size_t frames; int rate, ch; float base; LONG gen; } pv_t;
#define PV_NB 3
static unsigned __stdcall pv_thread(void *arg) {
    pv_t *p = arg; CoInitializeEx(NULL, COINIT_MULTITHREADED);
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, (WORD)p->ch, (DWORD)p->rate, (DWORD)(p->rate * p->ch * 2), (WORD)(p->ch * 2), 16, 0 };
    HWAVEOUT ho = NULL;
    if (waveOutOpen(&ho, out_device(), &wf, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR) {
        const size_t chunk = (size_t)p->rate * 30 / 1000 > 0 ? (size_t)p->rate * 30 / 1000 : 1;
        short *bufs[PV_NB]; WAVEHDR hd[PV_NB]; bool used[PV_NB] = { false, false, false };
        for (int k = 0; k < PV_NB; k++) { bufs[k] = malloc(chunk * (size_t)p->ch * sizeof(short)); memset(&hd[k], 0, sizeof hd[k]); }
        size_t at = 0, queued = 0; int k = 0;
        while (p->gen == g_pv_gen && (at < p->frames || queued > 0)) {
            if (at < p->frames && queued < PV_NB) {
                if (used[k]) { waveOutUnprepareHeader(ho, &hd[k], sizeof hd[k]); used[k] = false; }
                const size_t n = p->frames - at < chunk ? p->frames - at : chunk; const float g = p->base * g_pv_vol;
                for (size_t i = 0; i < n * (size_t)p->ch; i++) { float v = p->buf[at * (size_t)p->ch + i] * g; bufs[k][i] = (short)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : v); }
                memset(&hd[k], 0, sizeof hd[k]); hd[k].lpData = (LPSTR)bufs[k]; hd[k].dwBufferLength = (DWORD)(n * (size_t)p->ch * sizeof(short));
                if (waveOutPrepareHeader(ho, &hd[k], sizeof hd[k]) != MMSYSERR_NOERROR) break;
                used[k] = true; waveOutWrite(ho, &hd[k], sizeof hd[k]); at += n; k = (k + 1) % PV_NB;
            } else Sleep(5);
            queued = 0; for (int j = 0; j < PV_NB; j++) if (used[j] && !(hd[j].dwFlags & WHDR_DONE)) queued++;
            MMTIME mt; mt.wType = TIME_SAMPLES;
            if (waveOutGetPosition(ho, &mt, sizeof mt) == MMSYSERR_NOERROR && mt.wType == TIME_SAMPLES) InterlockedExchange(&g_pv_pos_ms, (LONG)((double)mt.u.sample * 1000.0 / p->rate));
        }
        waveOutReset(ho);
        for (int j = 0; j < PV_NB; j++) { if (used[j]) waveOutUnprepareHeader(ho, &hd[j], sizeof hd[j]); free(bufs[j]); }
        waveOutClose(ho);
    }
    InterlockedExchange(&g_pv_pos_ms, -1);
    free(p->buf); free(p); InterlockedExchange(&g_pv_on, 0);
    CoUninitialize();
    return 0;
}
static void preview_stop(void) { InterlockedIncrement(&g_pv_gen); }
static void preview_toggle(void) {
    if (g_pv_on) { preview_stop(); return; }   /* tocando: clicar de novo para (nunca um por cima do outro) */
    size_t fr = 0; short *sel = render_sel(&fr); if (!sel) return;
    InterlockedIncrement(&g_started);
    pv_t *p = calloc(1, sizeof *p); if (!p) { free(sel); return; }
    p->buf = sel; p->frames = fr; p->rate = g_add.rate; p->ch = g_add.ch; p->gen = g_pv_gen;
    p->base = sons_peak_gain(sel, fr * (size_t)g_add.ch, 1.0f); g_pv_vol = g_add.vol / 100.0f;
    InterlockedExchange(&g_pv_pos_ms, 0); InterlockedExchange(&g_pv_on, 1);
    uintptr_t th = _beginthreadex(NULL, 0, pv_thread, p, 0, NULL);
    if (!th) { InterlockedExchange(&g_pv_on, 0); free(sel); free(p); } else CloseHandle((HANDLE)th);
}

/* regras do recorte (mínimo 0,2 s, máximo 15 s, dentro do arquivo); moved: 1 = mexeu no início, 2 = no fim */
static void apply_clip(float st, float en, int moved) {
    if (moved == 1) { if (en - st < MIN_CLIP_MS) en = st + MIN_CLIP_MS; if (en - st > MAX_CLIP_MS) en = st + MAX_CLIP_MS; }
    else if (moved == 2) { if (en - st < MIN_CLIP_MS) st = en - MIN_CLIP_MS; if (en - st > MAX_CLIP_MS) st = en - MAX_CLIP_MS; }
    if (st < 0) { st = 0; if (en < MIN_CLIP_MS) en = MIN_CLIP_MS; }
    if (en > g_add.dur_ms) { en = g_add.dur_ms; if (st > en - MIN_CLIP_MS && en >= MIN_CLIP_MS) st = en - MIN_CLIP_MS; }
    g_add.start_ms = st; g_add.end_ms = en;
}

static void editor_dialog(cui_ctx *ui, app_state_t *s) {
    cui_dialog(ui, "Adicionar som", &g_add.open, .width = 480) {
        if (!g_add.loaded) {
            cui_text(ui, g_add.converting ? "Lendo o OGG…" : g_add.err[0] ? g_add.err : "Escolha um arquivo de áudio ou vídeo.", .color = g_add.err[0] ? CUI_RGB(0xfca5a5) : B_MUTED, .size = 13);
            cui_dialog_footer(ui) {
                if (cui_button(ui, "Fechar", .variant = CUI_BUTTON_SECONDARY)) g_add.open = false;
                if (cui_button(ui, "Procurar arquivo", .variant = CUI_BUTTON_PRIMARY)) pick_file();
            }
            return;
        }
        char d1[24], d2[24], line[200];
        wf_t w; memcpy(w.bars, g_add.bars, sizeof w.bars); w.a = g_add.start_ms / g_add.dur_ms; w.b = g_add.end_ms / g_add.dur_ms;
        w.play = g_pv_on && g_pv_pos_ms >= 0 ? (g_add.start_ms + (float)g_pv_pos_ms) / g_add.dur_ms : -1.0f;
        if (g_pv_on) cui_request_wakeup(ui, 0.016f);
        cui_row(ui, .gap = 8, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
            cui_box(ui, .id = "sn-play", .width = CUI_PX(44), .height = CUI_PX(44), .radius = 22, .bg = g_pv_on ? CUI_RGB(0x7c5cff) : CUI_RGBA(0xffffff1a), .border = 1, .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                cui_icon(ui, g_pv_on ? CUI_ICON_PAUSE : CUI_ICON_PLAY, 18, B_TEXT);
            }
            if (briks_hover(ui, "sn-play")) briks_tip(g_pv_on ? "Parar" : "Ouvir o trecho (do início ao fim)");
            if (briks_clicked_r(ui, "sn-play", 22.0f)) preview_toggle();
            cui_canvas(ui, "sn-wave", CUI_FILL, CUI_PX(76), paint_wf, &w, sizeof w);
        }
        {   /* arrastar as barras brancas da onda muda o início e o fim (a mais perto do ponteiro) */
            static int drag; static bool was_down;
            const cui_rect wr = cui_get_rect(ui, cui_make_id(ui, "sn-wave")); const cui_vec2 m = cui_mouse_pos(ui); const bool down = cui_mouse_down(ui, CUI_MOUSE_LEFT);
            if (!down) drag = 0;
            else if (!was_down && wr.w > 20 && m.x >= wr.x && m.x < wr.x + wr.w && m.y >= wr.y && m.y < wr.y + wr.h) {
                const float xa = wr.x + 8.0f + w.a * (wr.w - 16.0f), xb = wr.x + 8.0f + w.b * (wr.w - 16.0f);
                drag = fabsf(m.x - xa) <= fabsf(m.x - xb) ? 1 : 2;
            }
            was_down = down;
            if (drag && wr.w > 20) {
                float f = (m.x - (wr.x + 8.0f)) / (wr.w - 16.0f); if (f < 0) f = 0; if (f > 1) f = 1;
                const float tms = f * g_add.dur_ms;
                if (drag == 1) apply_clip(tms, g_add.end_ms, 1); else apply_clip(g_add.start_ms, tms, 2);
            }
        }
        snprintf(line, sizeof line, "Trecho de %s (arquivo com %s). Máximo de 15 s.", sons_fmt_dur((unsigned)(g_add.end_ms - g_add.start_ms + 0.5f), d1, sizeof d1), sons_fmt_dur((unsigned)(g_add.dur_ms + 0.5f), d2, sizeof d2));
        cui_text(ui, line, .size = 12.5f, .color = B_MUTED);
        cui_row(ui, .gap = 8, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
            const char *b = strrchr(g_add.path, '\\'); b = b ? b + 1 : g_add.path; char fn[80]; sons_label("", b, 34, fn, sizeof fn);
            cui_text(ui, fn, .size = 13, .width = CUI_FILL, .nowrap = true);
            if (cui_button(ui, "Procurar", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) pick_file();
        }
        field_row(ui, "sn-name", g_add.name, sizeof g_add.name, g_add.emoji, "sn-emo");
        {   /* recorte: início e fim em segundos; o trecho nunca passa de 15 s */
            float a = g_add.start_ms / 1000.0f, bnd = g_add.end_ms / 1000.0f; const float top = g_add.dur_ms / 1000.0f;
            const float a0 = a, b0 = bnd;
            cui_row(ui, .gap = 10, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) { cui_text(ui, "Início", .size = 13, .width = CUI_PX(52)); cui_slider(ui, "##sn-ini", &a, 0.0f, top, .width = CUI_FILL); cui_text(ui, sons_fmt_dur((unsigned)(a * 1000.0f + 0.5f), d1, sizeof d1), .size = 12, .color = B_MUTED, .width = CUI_PX(48)); }
            cui_row(ui, .gap = 10, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) { cui_text(ui, "Fim", .size = 13, .width = CUI_PX(52)); cui_slider(ui, "##sn-fim", &bnd, 0.0f, top, .width = CUI_FILL); cui_text(ui, sons_fmt_dur((unsigned)(bnd * 1000.0f + 0.5f), d2, sizeof d2), .size = 12, .color = B_MUTED, .width = CUI_PX(48)); }
            if (fabsf(a - a0) > 1e-4f) apply_clip(a * 1000.0f, bnd * 1000.0f, 1);
            else if (fabsf(bnd - b0) > 1e-4f) apply_clip(a * 1000.0f, bnd * 1000.0f, 2);
        }
        cui_row(ui, .gap = 10, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) { cui_text(ui, "Volume", .size = 13, .width = CUI_PX(52)); briks_volume(ui, "sn-evol", &g_add.vol, 0.0f, 100.0f, 1.0f, NULL, false); g_pv_vol = g_add.vol / 100.0f; char p[12]; snprintf(p, sizeof p, "%d%%", (int)(g_add.vol + 0.5f)); cui_text(ui, p, .size = 12, .color = B_MUTED, .width = CUI_PX(48)); }
        int idx[32]; int na = admin_groups(s, idx, 32);
        cui_text(ui, "Onde fica", .size = 12.5f, .color = B_MUTED);
        cui_scroll(ui, .id = "sn-dest", .width = CUI_FILL, .height = CUI_PX(na > 1 ? 76 : 40 + na * 38), .pad = 2) {
            for (int i = 0; i <= na; i++) {
                cui_push_id_int(ui, 600 + i); char lab[140];
                if (i == 0) snprintf(lab, sizeof lab, "Meus sons (só eu; tocam em qualquer chamada)");
                else snprintf(lab, sizeof lab, "Grupo · %s (todos do grupo)", s->groups[idx[i - 1]].name);
                if (cui_button(ui, lab, .variant = g_add.dest == i ? CUI_BUTTON_PRIMARY : CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM, .width = CUI_FILL)) g_add.dest = i;
                cui_pop_id(ui);
            }
        }
        cui_text(ui, "O áudio é convertido e comprimido aqui; o arquivo vai direto de você para quem está na chamada, sem servidor.", .size = 12, .color = g_th.ph);
        cui_checkbox(ui, "Tenho direito de usar este som (só adicione o que pode usar)", &g_add.ack);
        if (g_add.err[0]) cui_text(ui, g_add.err, .size = 12.5f, .color = CUI_RGB(0xfca5a5));
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Cancelar", .variant = CUI_BUTTON_SECONDARY)) g_add.open = false;
            char nm[48]; const bool okname = sons_clean_name(g_add.name, nm, sizeof nm);
            if (cui_button(ui, "Adicionar", .variant = CUI_BUTTON_PRIMARY, .disabled = !g_add.ack || !okname)) {
                char path[1040] = ""; char err[200] = "";
                if (unedited_original_ok()) snprintf(path, sizeof path, "%s", g_add.path);
                else if (export_sel(path, sizeof path, err, sizeof err)) snprintf(g_tmp_import, sizeof g_tmp_import, "%s", path);
                if (!path[0]) snprintf(g_add.err, sizeof g_add.err, "%s", err[0] ? err : "Não consegui preparar o áudio");
                else {
                    char scope[80]; if (g_add.dest > 0 && g_add.dest <= na) snprintf(scope, sizeof scope, "g:%s", s->groups[idx[g_add.dest - 1]].id); else snprintf(scope, sizeof scope, "me");
                    char e[2200], en[160], ee[160], l[2800]; json_escape(path, e, sizeof e); json_escape(nm, en, sizeof en); json_escape(g_add.emoji, ee, sizeof ee);
                    snprintf(l, sizeof l, "{\"cmd\":\"sound.import\",\"path\":\"%s\",\"name\":\"%s\",\"emoji\":\"%s\",\"scope\":\"%s\"}", e, en, ee, scope);
                    snprintf(g_add.name, sizeof g_add.name, "%s", nm); g_add.err[0] = 0; core_send(l);
                }
            }
        }
    }
}

void sons_dialogs(cui_ctx *ui, app_state_t *s) {
    if (g_add.open) editor_dialog(ui, s);
    else { if (g_pv_on) preview_stop(); if (g_add.pcm) ed_free(); }   /* fechou o editor: para a prévia e solta o áudio decodificado */
    if (g_ren.open) {
        cui_dialog(ui, "Renomear som", &g_ren.open, .width = 400) {
            field_row(ui, "rn-name", g_ren.name, sizeof g_ren.name, g_ren.emoji, "rn-emo");
            cui_dialog_footer(ui) {
                if (cui_button(ui, "Cancelar", .variant = CUI_BUTTON_SECONDARY)) g_ren.open = false;
                char nm[48];
                if (cui_button(ui, "Salvar", .variant = CUI_BUTTON_PRIMARY, .disabled = !sons_clean_name(g_ren.name, nm, sizeof nm))) {
                    son_t x; memset(&x, 0, sizeof x); snprintf(x.hash, sizeof x.hash, "%s", g_ren.hash); snprintf(x.scope, sizeof x.scope, "%s", g_ren.scope); snprintf(x.name, sizeof x.name, "%s", nm); snprintf(x.emoji, sizeof x.emoji, "%s", g_ren.emoji);
                    send_scoped("sound.update", &x, NULL);
                    if (!strncmp(g_ren.scope, "g:", 2)) { son_t *m = find_s(g_ren.hash, g_ren.scope); if (m) { snprintf(m->name, sizeof m->name, "%s", nm); snprintf(m->emoji, sizeof m->emoji, "%s", g_ren.emoji); } groups_republish(s, g_ren.scope + 2); }
                    g_ren.open = false;
                }
            }
        }
    }
    if (g_del.open) {
        cui_dialog(ui, "Apagar som?", &g_del.open, .width = 400) {
            const bool grp = !strncmp(g_del.scope, "g:", 2);
            char t[200]; snprintf(t, sizeof t, grp ? "“%s” sai do painel de todo o grupo." : "“%s” sai dos seus sons.", g_del.name);
            cui_text(ui, t, .size = 13);
            cui_dialog_footer(ui) {
                if (cui_button(ui, "Cancelar", .variant = CUI_BUTTON_SECONDARY)) g_del.open = false;
                if (cui_button(ui, "Apagar", .variant = CUI_BUTTON_DANGER)) {
                    son_t x; memset(&x, 0, sizeof x); snprintf(x.hash, sizeof x.hash, "%s", g_del.hash); snprintf(x.scope, sizeof x.scope, "%s", g_del.scope);
                    if (grp && g_nrm < 48) { snprintf(g_rm[g_nrm].gid, sizeof g_rm[g_nrm].gid, "%s", g_del.scope + 2); snprintf(g_rm[g_nrm].hash, sizeof g_rm[g_nrm].hash, "%s", g_del.hash); g_nrm++; }
                    send_scoped("sound.remove", &x, NULL);
                    if (grp) groups_republish(s, g_del.scope + 2);
                    g_sel = -1; g_del.open = false;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ painel */
static void pump(app_state_t *s) {
    if (g_click_hash[0] && GetTickCount64() - g_click_at > 1500) {   /* o core não respondeu: toca só aqui e avisa (diagnóstico) */
        char h[72]; snprintf(h, sizeof h, "%s", g_click_hash); g_click_hash[0] = 0;
        say("%s", "O serviço de chamadas não respondeu ao som: tocando só neste aparelho"); if (!s->deaf) play_hash(h, false);
    }
    if (g_vol_dirty && GetTickCount64() - g_vol_at > 250) { g_vol_dirty = false; send_prefs(NULL); }
    if (!s->in_room) { if (g_roster_room[0]) { g_roster_room[0] = 0; g_room_gid[0] = 0; core_send("{\"cmd\":\"sound.roster\",\"fps\":[]}"); sons_stop_all(); } return; }
    /* assinatura da lista de grupos/canais/membros: se a chamada começou antes dos grupos carregarem, o dono da sala é descoberto assim que eles chegam */
    unsigned gsig = (unsigned)s->group_count * 1000003u; for (int i = 0; i < s->group_count; i++) gsig = gsig * 31u + (unsigned)s->groups[i].nch * 131u + (unsigned)s->groups[i].nm;
    static unsigned g_gsig; const bool regroup = gsig != g_gsig && !g_room_gid[0]; g_gsig = gsig;
    if (strcmp(g_roster_room, s->room_name) || regroup) {   /* sala nova (ou grupos que chegaram depois): quem pode disparar aqui = membros do grupo dono da sala (vazio = sem restrição) */
        snprintf(g_roster_room, sizeof g_roster_room, "%s", s->room_name);
        char *fps = groups_room_roster(s, s->room_name, g_room_gid, sizeof g_room_gid);
        if (!fps) g_room_gid[0] = 0;
        size_t l = (fps ? strlen(fps) : 2) + 60; char *c = malloc(l); if (c) { snprintf(c, l, "{\"cmd\":\"sound.roster\",\"fps\":%s}", fps ? fps : "[]"); core_send(c); free(c); }
        free(fps);
        core_send("{\"cmd\":\"sound.list\"}");
    }
}
void sons_on_room_joined(app_state_t *s) { g_roster_room[0] = 0; pump(s); }

static bool usable_here(const son_t *x) {
    if (!strcmp(x->scope, "me")) return true;
    return !strncmp(x->scope, "g:", 2) && g_room_gid[0] && !strcmp(x->scope + 2, g_room_gid);
}

static const char *group_name(const app_state_t *s, const char *gid) { for (int i = 0; i < s->group_count; i++) if (!strcmp(s->groups[i].id, gid)) return s->groups[i].name; return "grupo"; }

/* ficha de um som; devolve 1 = clique (tocar) · 2 = botão direito (opções) */
static int chip(cui_ctx *ui, int i, bool usable, bool selected) {
    const son_t *x = &g_s[i]; char lab[120]; sons_label("", x->name, 18, lab, sizeof lab);
    cui_push_id_int(ui, i);
    const bool busy = !x->have;
    cui_box(ui, .id = "c", .dir = CUI_ROW, .gap = 6, .pad_x = 12, .height = CUI_PX(34), .radius = 17, .bg = selected ? CUI_RGBA(0x57d28b22) : CUI_RGBA(0xffffff12), .border = 1,
            .border_color = selected ? CUI_RGBA(0x57d28b99) : B_GLASS_BORDER, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
        if (x->fav) briks_icon(ui, CUI_ICON_STAR, 12, CUI_RGB(0xf5c542));
        if (x->emoji[0]) cui_emoji(ui, x->emoji, 17);
        cui_text(ui, lab, .size = 13.5f, .color = usable && !busy ? B_TEXT : B_MUTED, .nowrap = true);
        if (busy) cui_text(ui, is_fetching(x->hash) ? "baixando…" : "falta o arquivo", .size = 11, .color = B_MUTED, .nowrap = true);
    }
    int r = 0;
    if (briks_hover(ui, "c")) { char tip[200]; char d[24]; snprintf(tip, sizeof tip, "%s · %s", x->name, sons_fmt_dur(x->dur_ms, d, sizeof d)); briks_tip(tip); }
    if (briks_clicked_r(ui, "c", 17.0f)) r = 1;
    else {
        cui_rect rc = cui_get_rect(ui, cui_make_id(ui, "c")); cui_vec2 m = cui_mouse_pos(ui);
        if (!briks_blocked() && cui_mouse_clicked(ui, CUI_MOUSE_RIGHT) && rc.w > 0 && m.x >= rc.x && m.y >= rc.y && m.x < rc.x + rc.w && m.y < rc.y + rc.h) r = 2;
    }
    cui_pop_id(ui);
    return r;
}

static void section(cui_ctx *ui, app_state_t *s, int sec, const char *title, const int *ids, int n) {
    if (!n) return;
    cui_push_id_int(ui, 7000 + sec);
    cui_text(ui, title, .size = 12, .color = B_MUTED);
    cui_row(ui, .gap = 6, .width = CUI_FILL, .wrap = true) {
        for (int k = 0; k < n; k++) {
            const int i = ids[k]; const bool us = usable_here(&g_s[i]);
            int r = chip(ui, i, us, g_sel == i);
            if (r == 2 || (r == 1 && g_editmode)) g_sel = i;   /* botão direito, ou clique no modo Editar: só seleciona (renomear, apagar, favoritar) */
            else if (r == 1) {
                if (!g_s[i].have) say("%s", "O arquivo ainda não chegou");
                else if (!s->in_room) say("%s", "Entre numa chamada para tocar para todos");
                else if (!us) { char b[160]; snprintf(b, sizeof b, "Este som é do grupo %s: só toca na chamada dele", group_name(s, g_s[i].scope + 2)); say("%s", b); }
                else if (s->deaf) say("%s", "Você está ensurdecido: ninguém ouve sons assim");
                else { char l[200]; snprintf(l, sizeof l, "{\"cmd\":\"sound.play\",\"hash\":\"%s\"}", g_s[i].hash); core_send(l); g_sel = i; snprintf(g_click_hash, sizeof g_click_hash, "%s", g_s[i].hash); g_click_at = GetTickCount64(); }
            }
        }
    }
    cui_pop_id(ui);
}

static bool can_edit(const app_state_t *s, const son_t *x) {
    if (!strcmp(x->scope, "me")) return true;
    if (strncmp(x->scope, "g:", 2)) return false;
    for (int i = 0; i < s->group_count; i++) if (!strcmp(s->groups[i].id, x->scope + 2)) return s->groups[i].can_moderate;
    return false;
}

void sons_popover(cui_ctx *ui, app_state_t *s) {
    pump(s);
    g_open_panel = false;
    const cui_vec2 vp = cui_viewport_size(ui);
    const bool wide = vp.x >= 504;   /* a partir de 480 px o CUI deixa o painel crescer até a janela; abaixo disso ele pára em 420 */
    float lh = (wide ? vp.y - 100.0f : 420.0f) - 385.0f; if (lh < 70) lh = 70; if (lh > 260) lh = 260;
    cui_popover(ui, "sonpop", .placement = CUI_PLACE_TOP, .width = wide ? 480 : vp.x - 24, .pad = 14, .gap = 8) {
        g_open_panel = true;
        cui_row(ui, .gap = 8, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, "Sons", .font = CUI_FONT_DISPLAY, .size = 16, .width = CUI_FILL);
            if (cui_button(ui, g_editmode ? "Concluir" : "Editar", .variant = g_editmode ? CUI_BUTTON_PRIMARY : CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) { g_editmode = !g_editmode; if (!g_editmode) g_sel = -1; }
            if (cui_button(ui, "Adicionar", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) pick_file();
        }
        cui_search(ui, "sn-q", g_search, sizeof g_search, .placeholder = "Buscar som", .width = CUI_FILL);
        int fav[MAXS], top[MAXS], mine[MAXS], nfav = 0, ntop = 0, nmine = 0; int ord[MAXS]; int nord = 0;
        for (int i = 0; i < g_n; i++) { if (!sons_match(g_s[i].name, g_search)) continue; ord[nord++] = i; }
        for (int k = 0; k < nord; k++) { const int i = ord[k]; if (g_s[i].fav) fav[nfav++] = i; else if (g_s[i].uses >= 3 && ntop < 6) top[ntop++] = i; if (!strcmp(g_s[i].scope, "me")) mine[nmine++] = i; }
        cui_scroll(ui, .id = "sn-scroll", .width = CUI_FILL, .height = CUI_PX(lh), .pad = 2) {
            if (!g_n) cui_text(ui, g_loaded ? "Nenhum som ainda. Use “Adicionar”: escolha um áudio ou vídeo, corte o trecho (até 15 s) e ajuste o volume." : "Carregando…", .size = 13, .color = B_MUTED);
            section(ui, s, 1, "Favoritos", fav, nfav);
            section(ui, s, 2, "Mais usados", top, ntop);
            for (int g = 0; g < s->group_count; g++) {   /* um bloco por grupo, na ordem da lista de grupos */
                char scope[80]; snprintf(scope, sizeof scope, "g:%s", s->groups[g].id); int ids[MAXS], n = 0;
                for (int k = 0; k < nord; k++) if (!strcmp(g_s[ord[k]].scope, scope)) ids[n++] = ord[k];
                char t[140]; snprintf(t, sizeof t, "Grupo · %s", s->groups[g].name); section(ui, s, 10 + g, t, ids, n);
            }
            section(ui, s, 3, "Meus sons", mine, nmine);
            if (g_n && !nord) cui_text(ui, "Nada com esse nome.", .size = 13, .color = B_MUTED);
        }
        cui_box(ui, .width = CUI_FILL, .height = CUI_PX(34), .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_START) {   /* altura fixa: selecionar um som não muda o tamanho do painel */
        if (g_sel >= 0 && g_sel < g_n) {   /* barra de opções do som selecionado (botão direito) */
            son_t *x = &g_s[g_sel];
            cui_row(ui, .gap = 6, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
                char lab[100]; sons_label("", x->name, 10, lab, sizeof lab); cui_text(ui, lab, .size = 12.5f, .color = B_MUTED, .nowrap = true);
                if (cui_button(ui, "Testar", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM, .disabled = !x->have)) play_hash(x->hash, true);
                if (cui_button(ui, x->fav ? "Desfavoritar" : "Favoritar", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) { send_scoped("sound.fav", x, x->fav ? ",\"fav\":false" : ",\"fav\":true"); }
                if (can_edit(s, x)) {
                    if (cui_button(ui, "Renomear", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) { g_ren.open = true; snprintf(g_ren.hash, sizeof g_ren.hash, "%s", x->hash); snprintf(g_ren.scope, sizeof g_ren.scope, "%s", x->scope); snprintf(g_ren.name, sizeof g_ren.name, "%s", x->name); snprintf(g_ren.emoji, sizeof g_ren.emoji, "%s", x->emoji); }
                    if (cui_button(ui, "Apagar", .variant = CUI_BUTTON_DANGER, .size = CUI_SIZE_SM)) { g_del.open = true; snprintf(g_del.hash, sizeof g_del.hash, "%s", x->hash); snprintf(g_del.scope, sizeof g_del.scope, "%s", x->scope); snprintf(g_del.name, sizeof g_del.name, "%s", x->name); }
                }
            }
        }
        }
        if (note_live()) cui_text(ui, g_note, .size = 12.5f, .color = CUI_RGB(0x7be8a8));
        else cui_text(ui, g_editmode ? "Escolha um som para renomear, apagar ou favoritar" : "Clique para tocar para todos · Editar para renomear e apagar", .size = 12, .color = g_th.ph);
        cui_separator(ui);
        float vol = (float)g_pr.volume;
        cui_row(ui, .gap = 10, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) { cui_text(ui, "Volume dos sons", .size = 13, .width = CUI_FILL); char p[12]; snprintf(p, sizeof p, "%d%%", g_pr.volume); cui_text(ui, p, .size = 12, .color = B_MUTED); }
        briks_volume(ui, "sn-vol", &vol, 0.0f, 100.0f, 1.0f, NULL, false);
        if ((int)(vol + 0.5f) != g_pr.volume) { g_pr.volume = (int)(vol + 0.5f); g_vol_dirty = true; g_vol_at = GetTickCount64(); }
        bool am = g_pr.all_muted;
        if (cui_switch(ui, "Silenciar todos os sons", &am)) { g_pr.all_muted = am; send_prefs(NULL); if (am) sons_stop_all(); }
        cui_row(ui, .gap = 8, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, "Intervalo por pessoa", .size = 13, .width = CUI_FILL);
            static const char *const CD[] = { "1 s", "3 s", "5 s", "10 s" }; static const int CDV[] = { 1000, 3000, 5000, 10000 };
            int ci = 1; for (int i = 0; i < 4; i++) if (CDV[i] == g_pr.cooldown_ms) ci = i;
            if (cui_segmented(ui, "sn-cd", CD, 4, &ci, .size = CUI_SIZE_SM)) { g_pr.cooldown_ms = CDV[ci]; send_prefs(NULL); }
        }
        int np = 0; for (int i = 0; i < BRIKS_MAX_PEERS; i++) if (s->rtc_peers[i].id[0] && s->rtc_peers[i].fp[0]) np++;
        if (np) {
            cui_push_id(ui, "ppl");
            if (cui_button(ui, g_people ? "Esconder pessoas" : "Silenciar pessoas…", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) g_people = !g_people;
            if (g_people) cui_scroll(ui, .id = "sn-ppl", .width = CUI_FILL, .height = CUI_PX(np > 3 ? 120 : np * 40 + 6), .pad = 2) {
                for (int i = 0; i < BRIKS_MAX_PEERS; i++) {
                    if (!s->rtc_peers[i].id[0] || !s->rtc_peers[i].fp[0]) continue;
                    cui_push_id_int(ui, i); bool mu = is_muted_fp(s->rtc_peers[i].fp);
                    if (cui_switch(ui, s->rtc_peers[i].name[0] ? s->rtc_peers[i].name : "Participante", &mu)) { char ex[200]; snprintf(ex, sizeof ex, ",\"mute_fp\":\"%s\",\"muted\":%s", s->rtc_peers[i].fp, mu ? "true" : "false"); send_prefs(ex); }
                    cui_pop_id(ui);
                }
            }
            cui_pop_id(ui);
        }
    }
}

/* ------------------------------------------------------------------ roteiros de teste (BRIKS_SCRIPT) */
void sons_script(app_state_t *s, const char *cmd, const char *arg) {
    (void)s;
    if (!strcmp(cmd, "seed")) {   /* itens locais de mentira: mostra o painel sem core/rede */
        g_n = 0;
        static const char *const N[] = { "Buzina", "Aplausos", "Risada", "Tambor", "Bem-vindo", "Alerta" }; static const char *const E[] = { "📯", "👏", "😂", "🥁", "👋", "🚨" };
        for (int i = 0; i < 6; i++) { son_t *x = &g_s[g_n++]; memset(x, 0, sizeof *x); snprintf(x->hash, sizeof x->hash, "%064d", i + 1); snprintf(x->name, sizeof x->name, "%s", N[i]); snprintf(x->emoji, sizeof x->emoji, "%s", E[i]); snprintf(x->scope, sizeof x->scope, "me"); snprintf(x->fmt, sizeof x->fmt, "wav"); x->size = 40000; x->dur_ms = 1200 + (unsigned)i * 300; x->have = true; x->fav = i == 0; x->uses = i * 2; }
        g_loaded = true;
    } else if (!strcmp(cmd, "add")) {   /* sons add <arquivo>: abre o editor já com o arquivo carregado (sem janela de escolha) */
        if (arg && arg[0]) begin_load(arg, false); else { g_add.open = true; }
    } else if (!strcmp(cmd, "oggdone")) { char e[2200], l[2500]; json_escape(arg ? arg : "", e, sizeof e); snprintf(l, sizeof l, "{\"ev\":\"sound.decoded\",\"ok\":true,\"wav\":\"%s\"}", e); jval *j = json_parse(l); if (j) { ogg_done(j); json_free(j); } }   /* sons oggdone <wav>: o que o core responderia a sound.ogg */
    else if (!strcmp(cmd, "trim")) { float a2 = 0, b2 = 0; if (arg && sscanf(arg, "%f %f", &a2, &b2) == 2 && g_add.loaded) { g_add.start_ms = a2; g_add.end_ms = b2; } }   /* sons trim <ini_ms> <fim_ms> */
    else if (!strcmp(cmd, "vol")) { if (arg && g_add.loaded) g_add.vol = (float)atof(arg); }
    else if (!strcmp(cmd, "export")) { char out[1040] = "", err[200] = ""; if (arg && g_add.loaded) { snprintf(out, sizeof out, "%s", arg); if (!export_sel(out, sizeof out, err, sizeof err)) fprintf(stderr, "sons export: %s\n", err); } }   /* sons export <saida.mp3|.wav> */
    else if (!strcmp(cmd, "ack")) g_add.ack = true;
    else if (!strcmp(cmd, "sel")) g_sel = arg ? atoi(arg) : -1;
    else if (!strcmp(cmd, "stop")) sons_stop_all();
    else if (!strcmp(cmd, "pv")) { if (g_add.loaded) preview_toggle(); }   /* sons pv: liga/desliga a prévia do editor (o mesmo do botão play) */
    else if (!strcmp(cmd, "edit")) g_editmode = arg && atoi(arg);
}

int sons_probe(const char *n) {
    if (!strcmp(n, "sons_count")) return g_n;
    if (!strcmp(n, "sons_started")) return (int)g_started;
    if (!strcmp(n, "sons_active")) return (int)g_active;
    if (!strcmp(n, "sons_panel")) return g_open_panel;
    if (!strcmp(n, "sons_add_dialog")) return g_add.open;
    if (!strcmp(n, "sons_ed_loaded")) return g_add.loaded;
    if (!strcmp(n, "sons_ed_dur_ms")) return (int)(g_add.dur_ms + 0.5f);
    if (!strcmp(n, "sons_pv_on")) return g_pv_on ? 1 : 0;
    if (!strcmp(n, "sons_ed_start_ms")) return (int)(g_add.start_ms + 0.5f);
    if (!strcmp(n, "sons_ed_end_ms")) return (int)(g_add.end_ms + 0.5f);
    if (!strcmp(n, "sons_editmode")) return g_editmode;
    if (!strcmp(n, "sons_ed_clip_ms")) return (int)(g_add.end_ms - g_add.start_ms + 0.5f);
    if (!strcmp(n, "sons_volume")) return g_pr.volume;
    if (!strcmp(n, "sons_all_muted")) return g_pr.all_muted;
    if (!strcmp(n, "sons_cooldown")) return g_pr.cooldown_ms;
    if (!strcmp(n, "sons_fetching")) { int c = 0; for (int i = 0; i < 12; i++) if (g_fetching[i][0]) c++; return c; }
    if (!strcmp(n, "sons_note")) return note_live();
    if (!strcmp(n, "sons_missing")) { int c = 0; for (int i = 0; i < g_n; i++) if (!g_s[i].have) c++; return c; }
    return -1;
}
