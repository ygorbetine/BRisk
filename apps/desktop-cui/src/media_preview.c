/* Pré-visualização inline de vídeo e áudio nos anexos (ver media_preview.h). Tudo local: Media Foundation (decodificadores do Windows)
 * lê o arquivo; o áudio sai pelo waveOut; os quadros viram textura RGBA. Uma thread de reprodução por vez. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include "media_preview.h"
#include <windows.h>
#include <mmsystem.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cui/cui_render.h>

enum { PL_IDLE = 0, PL_OPENING, PL_PLAYING, PL_PAUSED, PL_ENDED, PL_FAILED };
#define MAXVW 640            /* largura máxima do quadro mostrado: decodifica no tamanho original e reduz aqui */
#define NHDR 96              /* blocos de áudio em voo */

/* ---------------------------------------------------------------- tipos de arquivo */
static bool ext_in(const char *name, const char *const *list) {
    const char *dot = strrchr(name ? name : "", '.'); if (!dot) return false;
    for (int i = 0; list[i]; i++) if (!_stricmp(dot + 1, list[i])) return true;
    return false;
}
int media_kind_of(const char *name) {
    static const char *const V[] = { "mp4", "m4v", "mov", "wmv", "avi", "mkv", "webm", "3gp", NULL };
    static const char *const A[] = { "mp3", "wav", "m4a", "aac", "wma", "flac", "ogg", "opus", NULL };
    return ext_in(name, V) ? MEDIA_VIDEO : ext_in(name, A) ? MEDIA_AUDIO : MEDIA_NONE;
}

/* ---------------------------------------------------------------- Media Foundation */
typedef struct { IMFSourceReader *rd; DWORD vidx, aidx; bool has_v, has_a; int w, h, stride; int ach, arate, abits; LONGLONG dur100; } mf_t;
static volatile LONG g_mf_up;
static void mf_up(void) { if (InterlockedCompareExchange(&g_mf_up, 1, 0) == 0) MFStartup(MF_VERSION, MFSTARTUP_LITE); }
static void mf_close(mf_t *m) { if (m->rd) IMFSourceReader_Release(m->rd); memset(m, 0, sizeof *m); }

static bool mf_open(const char *path, bool want_v, bool want_a, mf_t *m) {
    memset(m, 0, sizeof *m); mf_up();
    wchar_t wp[1200]; if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1200)) return false;
    IMFAttributes *at = NULL; if (FAILED(MFCreateAttributes(&at, 1))) return false;
    IMFAttributes_SetUINT32(at, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    HRESULT hr = MFCreateSourceReaderFromURL(wp, at, &m->rd); IMFAttributes_Release(at);
    if (FAILED(hr) || !m->rd) { m->rd = NULL; return false; }
    IMFSourceReader_SetStreamSelection(m->rd, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    PROPVARIANT pv; PropVariantInit(&pv);
    if (SUCCEEDED(IMFSourceReader_GetPresentationAttribute(m->rd, (DWORD)MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION, &pv)) && pv.vt == VT_UI8) m->dur100 = (LONGLONG)pv.uhVal.QuadPart;
    PropVariantClear(&pv);
    if (want_v) {
        IMFMediaType *mt = NULL;
        if (SUCCEEDED(MFCreateMediaType(&mt))) {
            IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video); IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
            if (SUCCEEDED(IMFSourceReader_SetCurrentMediaType(m->rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, mt))) {
                IMFMediaType *cur = NULL;
                if (SUCCEEDED(IMFSourceReader_GetCurrentMediaType(m->rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) {
                    UINT64 fs = 0; IMFMediaType_GetUINT64(cur, &MF_MT_FRAME_SIZE, &fs); UINT32 w = (UINT32)(fs >> 32), h = (UINT32)(fs & 0xffffffffu);
                    UINT32 st = 0; if (FAILED(IMFMediaType_GetUINT32(cur, &MF_MT_DEFAULT_STRIDE, &st))) st = (UINT32)(w * 4);
                    m->w = (int)w; m->h = (int)h; m->stride = (int)st; m->has_v = w > 0 && h > 0 && w <= 8192 && h <= 8192;
                    IMFMediaType_Release(cur);
                }
                if (m->has_v) IMFSourceReader_SetStreamSelection(m->rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
            }
            IMFMediaType_Release(mt);
        }
    }
    if (want_a) {
        IMFMediaType *mt = NULL;
        if (SUCCEEDED(MFCreateMediaType(&mt))) {
            IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio); IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
            IMFMediaType_SetUINT32(mt, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            if (SUCCEEDED(IMFSourceReader_SetCurrentMediaType(m->rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, mt))) {
                IMFMediaType *cur = NULL;
                if (SUCCEEDED(IMFSourceReader_GetCurrentMediaType(m->rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur))) {
                    UINT32 ch = 0, rate = 0, bits = 0;
                    IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_NUM_CHANNELS, &ch); IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate); IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
                    m->ach = (int)ch; m->arate = (int)rate; m->abits = (int)bits; m->has_a = ch >= 1 && ch <= 2 && rate >= 8000 && rate <= 192000 && bits == 16;
                    IMFMediaType_Release(cur);
                }
                if (m->has_a) IMFSourceReader_SetStreamSelection(m->rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
            }
            IMFMediaType_Release(mt);
        }
    }
    if (!(m->has_v || m->has_a)) { mf_close(m); return false; }
    /* o ReadSample devolve o índice REAL da faixa (não as constantes FIRST_*): descobre quem é quem pelo tipo atual */
    m->vidx = m->aidx = 0xFFFFFFFFu;
    for (DWORD i = 0; i < 16; i++) {
        BOOL sel = FALSE; IMFMediaType *cur = NULL;
        if (FAILED(IMFSourceReader_GetStreamSelection(m->rd, i, &sel))) break;
        if (!sel || FAILED(IMFSourceReader_GetCurrentMediaType(m->rd, i, &cur))) continue;
        GUID major; if (SUCCEEDED(IMFMediaType_GetGUID(cur, &MF_MT_MAJOR_TYPE, &major))) { if (IsEqualGUID(&major, &MFMediaType_Video) && m->has_v && m->vidx == 0xFFFFFFFFu) m->vidx = i; else if (IsEqualGUID(&major, &MFMediaType_Audio) && m->has_a && m->aidx == 0xFFFFFFFFu) m->aidx = i; }
        IMFMediaType_Release(cur);
    }
    if (m->has_v && m->vidx == 0xFFFFFFFFu) m->has_v = false;
    if (m->has_a && m->aidx == 0xFFFFFFFFu) m->has_a = false;
    if (!(m->has_v || m->has_a)) { mf_close(m); return false; }
    return true;
}
static void mf_seek(mf_t *m, LONGLONG ms) {
    PROPVARIANT v; PropVariantInit(&v); v.vt = VT_I8; v.hVal.QuadPart = ms * 10000;
    IMFSourceReader_SetCurrentPosition(m->rd, &GUID_NULL, &v);
    PropVariantClear(&v);
}
/* BGRX (bottom-up se o passo for negativo) -> RGBA reduzido para dw x dh */
static void to_rgba(const BYTE *src, int sw, int sh, int stride, BYTE *dst, int dw, int dh) {
    const bool flip = stride < 0; const int as = flip ? -stride : stride;
    for (int y = 0; y < dh; y++) {
        int sy = y * sh / dh; const BYTE *row = src + (size_t)(flip ? (sh - 1 - sy) : sy) * (size_t)as; BYTE *o = dst + (size_t)y * (size_t)dw * 4;
        for (int x = 0; x < dw; x++) { const BYTE *p = row + (size_t)(x * sw / dw) * 4; o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = 255; o += 4; }
    }
}
static void out_size(int w, int h, int *dw, int *dh) { *dw = w > MAXVW ? MAXVW : w; *dh = h * *dw / w; if (*dh < 1) *dh = 1; }
/* lê o próximo quadro de vídeo (ou a próxima amostra qualquer: stream = índice) -> bytes contíguos que o chamador libera */
static BYTE *sample_bytes(IMFSample *smp, DWORD *len) {
    IMFMediaBuffer *buf = NULL; BYTE *out = NULL; *len = 0;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(smp, &buf)) || !buf) return NULL;
    BYTE *p = NULL; DWORD cur = 0;
    if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, NULL, &cur)) && p && cur) { out = (BYTE *)malloc(cur); if (out) { memcpy(out, p, cur); *len = cur; } IMFMediaBuffer_Unlock(buf); }
    IMFMediaBuffer_Release(buf); return out;
}

/* ---------------------------------------------------------------- miniatura (anexo enviado por mim) */
typedef struct {
    char path[MAX_PATH * 2]; int kind;
    volatile LONG state;           /* 0 nada, 1 carregando, 2 pronto, 3 falhou */
    unsigned char *rgba; int w, h; LONGLONG dur_ms; cui_texture tex; bool exists_checked, exists; HANDLE th;
} item_t;
#define NITEM 16
static item_t g_it[NITEM]; static int g_it_next;
static item_t *item_get(const char *path, int kind) {
    for (int i = 0; i < NITEM; i++) if (g_it[i].path[0] && !strcmp(g_it[i].path, path)) return &g_it[i];
    item_t *it = &g_it[g_it_next]; g_it_next = (g_it_next + 1) % NITEM;
    if (it->th) { WaitForSingleObject(it->th, 3000); CloseHandle(it->th); it->th = NULL; }
    free(it->rgba); memset(it, 0, sizeof *it);   /* a textura antiga fica para o lixo do quadro (ver trash) */
    snprintf(it->path, sizeof it->path, "%s", path); it->kind = kind; return it;
}
static cui_texture g_trash[16]; static int g_ntrash;
static DWORD WINAPI poster_thread(LPVOID arg) {
    item_t *it = (item_t *)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    mf_t m; unsigned char *px = NULL; int dw = 0, dh = 0;
    if (mf_open(it->path, it->kind == MEDIA_VIDEO, it->kind == MEDIA_AUDIO, &m)) {
        it->dur_ms = m.dur100 / 10000;
        if (m.has_v) {
            LONGLONG at = it->dur_ms / 10; if (at > 1500) at = 1500; if (at > 0) mf_seek(&m, at);   /* o primeiro quadro costuma ser preto */
            for (int n = 0; n < 40 && !px; n++) {
                DWORD si = 0, fl = 0; LONGLONG ts = 0; IMFSample *smp = NULL;
                if (FAILED(IMFSourceReader_ReadSample(m.rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &si, &fl, &ts, &smp))) break;
                if (fl & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) { if (smp) IMFSample_Release(smp); break; }
                if (!smp) continue;
                DWORD len = 0; BYTE *b = sample_bytes(smp, &len); IMFSample_Release(smp);
                const int as = abs(m.stride);
                if (b && len >= (DWORD)(as * (m.h - 1) + m.w * 4)) { out_size(m.w, m.h, &dw, &dh); px = (unsigned char *)malloc((size_t)dw * (size_t)dh * 4); if (px) to_rgba(b, m.w, m.h, m.stride, px, dw, dh); }
                free(b);
            }
        }
        mf_close(&m);
    }
    it->rgba = px; it->w = dw; it->h = dh;
    InterlockedExchange(&it->state, (px || it->dur_ms > 0) ? 2 : 3);
    CoUninitialize();
    return 0;
}

/* ---------------------------------------------------------------- player */
typedef struct {
    char path[MAX_PATH * 2]; int kind;
    volatile LONG state, stop, pause; volatile LONGLONG seek_ms, pos_ms, dur_ms;
    int w, h; unsigned char *frame; volatile LONG fseq; CRITICAL_SECTION cs; HANDLE th;
    bool inited;
} player_t;
static player_t P;
static ULONGLONG g_drawn;   /* último instante em que a prévia do arquivo tocando foi desenhada */
static cui_texture g_tex; static int g_vw, g_vh; static LONG g_tseq;   /* textura do quadro ao vivo (só a thread da UI mexe) */

static LONGLONG qpc_ms(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (LONGLONG)(c.QuadPart * 1000 / f.QuadPart); }
static LONGLONG wave_ms(HWAVEOUT wo, int rate) { MMTIME mt; memset(&mt, 0, sizeof mt); mt.wType = TIME_SAMPLES; if (waveOutGetPosition(wo, &mt, sizeof mt) != MMSYSERR_NOERROR || mt.wType != TIME_SAMPLES) return 0; return (LONGLONG)mt.u.sample * 1000 / rate; }
static void hdr_free(WAVEHDR *h, HWAVEOUT wo) { waveOutUnprepareHeader(wo, h, sizeof *h); free(h->lpData); free(h); }

static DWORD WINAPI play_thread(LPVOID arg) {
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    mf_t m; if (!mf_open(P.path, P.kind == MEDIA_VIDEO, true, &m)) { InterlockedExchange(&P.state, PL_FAILED); CoUninitialize(); return 0; }
    P.dur_ms = m.dur100 / 10000;
    HWAVEOUT wo = NULL; WAVEHDR *hdrs[NHDR]; memset(hdrs, 0, sizeof hdrs);
    LONGLONG written = 0;   /* amostras (quadros) de áudio enviadas ao waveOut desde o último reset */
    if (m.has_a) {
        WAVEFORMATEX wf = { WAVE_FORMAT_PCM, (WORD)m.ach, (DWORD)m.arate, (DWORD)(m.arate * m.ach * 2), (WORD)(m.ach * 2), 16, 0 };
        if (waveOutOpen(&wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { wo = NULL; m.has_a = false; IMFSourceReader_SetStreamSelection(m.rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, FALSE); }
    }
    if (m.has_v) { int dw, dh; out_size(m.w, m.h, &dw, &dh); P.w = dw; P.h = dh; P.frame = (unsigned char *)calloc((size_t)dw * (size_t)dh, 4); }
    LONGLONG base_ms = 0, t0 = qpc_ms(), pause_at = 0; bool paused = false;
    unsigned char *pend = NULL; LONGLONG pend_ms = 0; bool eos_a = false, eos_v = false;
    InterlockedExchange(&P.state, PL_PLAYING);
    const int dw = P.w, dh = P.h;
#define CLOCK_MS() ((m.has_a && wo) ? (base_ms + wave_ms(wo, m.arate)) : (base_ms + (paused ? pause_at : qpc_ms()) - t0))
    for (;;) {
        if (P.stop) break;
        LONGLONG sk = P.seek_ms;
        if (sk >= 0) {
            P.seek_ms = -1; if (sk > P.dur_ms && P.dur_ms > 0) sk = P.dur_ms - 1;
            mf_seek(&m, sk); free(pend); pend = NULL;
            if (wo) { waveOutReset(wo); for (int i = 0; i < NHDR; i++) if (hdrs[i]) { hdr_free(hdrs[i], wo); hdrs[i] = NULL; } if (paused) waveOutPause(wo); }
            eos_a = eos_v = false; written = 0; base_ms = sk; t0 = qpc_ms() ; pause_at = t0;
            if (P.state == PL_ENDED) InterlockedExchange(&P.state, paused ? PL_PAUSED : PL_PLAYING);
        }
        if (P.pause != paused) {
            paused = P.pause != 0;
            if (paused) { pause_at = qpc_ms(); if (wo) waveOutPause(wo); InterlockedExchange(&P.state, PL_PAUSED); }
            else { t0 += qpc_ms() - pause_at; if (wo) waveOutRestart(wo); InterlockedExchange(&P.state, PL_PLAYING); }
        }
        if (paused) { Sleep(20); continue; }
        /* libera os blocos de áudio já tocados */
        if (wo) for (int i = 0; i < NHDR; i++) if (hdrs[i] && (hdrs[i]->dwFlags & WHDR_DONE)) { hdr_free(hdrs[i], wo); hdrs[i] = NULL; }
        const LONGLONG now_ms = CLOCK_MS();
        P.pos_ms = now_ms < 0 ? 0 : now_ms;
        if (pend && pend_ms <= now_ms + 8) { EnterCriticalSection(&P.cs); memcpy(P.frame, pend, (size_t)dw * (size_t)dh * 4); P.fseq++; LeaveCriticalSection(&P.cs); free(pend); pend = NULL; }
        if (wo && (LONGLONG)written * 1000 / m.arate - wave_ms(wo, m.arate) > 900) { Sleep(10); continue; }   /* áudio na frente: espera */
        if (pend && !m.has_a) { Sleep(4); continue; }   /* sem áudio o relógio é o QPC: segura a leitura até o quadro pendente vencer */
        DWORD si = 0, fl = 0; LONGLONG ts = 0; IMFSample *smp = NULL;
        HRESULT hr = IMFSourceReader_ReadSample(m.rd, (DWORD)MF_SOURCE_READER_ANY_STREAM, 0, &si, &fl, &ts, &smp);
        if (FAILED(hr) || (fl & MF_SOURCE_READERF_ERROR)) { if (smp) IMFSample_Release(smp); InterlockedExchange(&P.state, PL_FAILED); break; }
        if (fl & MF_SOURCE_READERF_ENDOFSTREAM) {   /* o fim vem POR FAIXA (áudio costuma acabar antes do vídeo): só termina quando todas acabaram */
            if (smp) IMFSample_Release(smp);
            if (si == m.aidx) eos_a = true; else eos_v = true;
            if (getenv("BRIKS_MEDIA_DEBUG")) fprintf(stderr, "[media] fim da faixa %lu em %lld ms (a=%d v=%d) relogio=%lld qpc=%lld wo=%d dur=%lld\n", (unsigned long)si, (long long)(ts / 10000), (int)eos_a, (int)eos_v, (long long)CLOCK_MS(), (long long)(qpc_ms() - t0), wo != NULL, (long long)P.dur_ms);
            if ((m.has_a && !eos_a) || (m.has_v && !eos_v)) continue;
            /* termina: mostra o último quadro pendente e espera o áudio acabar */
            while (!P.stop && P.seek_ms < 0 && ((pend && CLOCK_MS() < pend_ms) || (wo && (LONGLONG)written * 1000 / m.arate - wave_ms(wo, m.arate) > 30))) {
                if (pend && pend_ms <= CLOCK_MS() + 8) { EnterCriticalSection(&P.cs); memcpy(P.frame, pend, (size_t)dw * (size_t)dh * 4); P.fseq++; LeaveCriticalSection(&P.cs); free(pend); pend = NULL; }
                P.pos_ms = CLOCK_MS(); Sleep(15);
            }
            if (P.stop) break;
            if (P.seek_ms >= 0) continue;   /* o usuário puxou a barra no fim */
            if (pend) { EnterCriticalSection(&P.cs); memcpy(P.frame, pend, (size_t)dw * (size_t)dh * 4); P.fseq++; LeaveCriticalSection(&P.cs); free(pend); pend = NULL; }
            P.pos_ms = P.dur_ms; InterlockedExchange(&P.state, PL_ENDED);
            while (!P.stop && P.seek_ms < 0) Sleep(30);   /* parado no fim: ▶ de novo (seek 0) recomeça */
            continue;
        }
        if (!smp) continue;
        DWORD len = 0; BYTE *b = sample_bytes(smp, &len); IMFSample_Release(smp);
        if (!b) continue;
        if (wo && si == m.aidx && m.has_a) {
            int slot = -1;
            for (;;) {
                for (int i = 0; i < NHDR; i++) if (hdrs[i] && (hdrs[i]->dwFlags & WHDR_DONE)) { hdr_free(hdrs[i], wo); hdrs[i] = NULL; }
                for (int i = 0; i < NHDR; i++) if (!hdrs[i]) { slot = i; break; }
                if (slot >= 0 || P.stop) break;
                Sleep(5);
            }
            if (slot >= 0) {
                WAVEHDR *h = (WAVEHDR *)calloc(1, sizeof *h); h->lpData = (LPSTR)b; h->dwBufferLength = len; b = NULL;
                if (waveOutPrepareHeader(wo, h, sizeof *h) == MMSYSERR_NOERROR && waveOutWrite(wo, h, sizeof *h) == MMSYSERR_NOERROR) { hdrs[slot] = h; written += len / (DWORD)(m.ach * 2); }
                else { free(h->lpData); free(h); }
            }
            free(b);
        } else if (m.has_v && si == m.vidx) {
            const int as = abs(m.stride);
            if (len >= (DWORD)(as * (m.h - 1) + m.w * 4)) {
                unsigned char *fr = (unsigned char *)malloc((size_t)dw * (size_t)dh * 4);
                if (fr) {
                    to_rgba(b, m.w, m.h, m.stride, fr, dw, dh);
                    if (pend) {   /* já havia um quadro esperando a hora: espera ele vencer antes de guardar este */
                        while (!P.stop && P.seek_ms < 0 && !paused && pend_ms > CLOCK_MS() + 8) { P.pos_ms = CLOCK_MS(); Sleep(5); }
                        EnterCriticalSection(&P.cs); memcpy(P.frame, pend, (size_t)dw * (size_t)dh * 4); P.fseq++; LeaveCriticalSection(&P.cs); free(pend);
                    }
                    pend = fr; pend_ms = ts / 10000;
                }
            }
            free(b);
        } else free(b);
    }
    free(pend);
    if (wo) { waveOutReset(wo); for (int i = 0; i < NHDR; i++) if (hdrs[i]) hdr_free(hdrs[i], wo); waveOutClose(wo); }
    mf_close(&m);
    EnterCriticalSection(&P.cs); free(P.frame); P.frame = NULL; LeaveCriticalSection(&P.cs);
    if (P.state != PL_FAILED) InterlockedExchange(&P.state, PL_IDLE);
    CoUninitialize();
    return 0;
}

/* controles (só a thread da UI chama) */
static void pl_stop_wait(void) {
    if (!P.inited) return;
    if (P.th) { InterlockedExchange(&P.stop, 1); WaitForSingleObject(P.th, 4000); CloseHandle(P.th); P.th = NULL; }
    InterlockedExchange(&P.state, PL_IDLE); P.path[0] = 0; P.pos_ms = 0; P.dur_ms = 0;
}
static void pl_start(const char *path, int kind) {
    if (!P.inited) { InitializeCriticalSection(&P.cs); P.inited = true; P.seek_ms = -1; }
    pl_stop_wait();
    { static bool once; if (!once) { once = true; atexit(media_shutdown); } }
    g_drawn = GetTickCount64();
    snprintf(P.path, sizeof P.path, "%s", path); P.kind = kind; P.stop = 0; P.pause = 0; P.seek_ms = -1; P.pos_ms = 0; P.dur_ms = 0; P.fseq = 0; P.w = P.h = 0;
    InterlockedExchange(&P.state, PL_OPENING);
    P.th = CreateThread(NULL, 0, play_thread, NULL, 0, NULL);
    if (!P.th) InterlockedExchange(&P.state, PL_FAILED);
}
static bool pl_is(const char *path) { return P.inited && P.path[0] && !strcmp(P.path, path); }
static void pl_toggle(void) {
    if (P.state == PL_ENDED) { P.pause = 0; P.seek_ms = 0; InterlockedExchange(&P.state, PL_PLAYING); }
    else if (P.state == PL_PLAYING) P.pause = 1; else if (P.state == PL_PAUSED) P.pause = 0;
}
void media_shutdown(void) {
    pl_stop_wait();
    for (int i = 0; i < NITEM; i++) { if (g_it[i].th) { WaitForSingleObject(g_it[i].th, 3000); CloseHandle(g_it[i].th); g_it[i].th = NULL; } free(g_it[i].rgba); g_it[i].rgba = NULL; }
}
static void trash_flush(cui_ctx *ui);
void media_frame(cui_ctx *ui) {
    trash_flush(ui);
    if (P.inited && P.th && P.state != PL_IDLE && !getenv("BRIKS_MEDIA_KEEP") && GetTickCount64() - g_drawn > 2500) pl_stop_wait();   /* a conversa saiu da tela: sem controles não deve continuar tocando */
}
static void trash_flush(cui_ctx *ui) { while (g_ntrash > 0) cui_image_destroy(ui, g_trash[--g_ntrash]); }

/* sobe o quadro mais novo do player para a textura */
static bool live_tex(cui_ctx *ui, app_state_t *s) {
    if (!P.inited || !P.frame) return g_tex != 0 && P.state != PL_IDLE;
    bool ok = g_tex != 0;
    EnterCriticalSection(&P.cs);
    if (P.frame && P.fseq != g_tseq && P.w > 0 && P.h > 0) {
        cui_gl_renderer *gl = s->app ? cui_app_gl_renderer(s->app) : NULL;
        if (g_tex && g_vw == P.w && g_vh == P.h && gl) { cui_renderer_iface r = cui_gl_iface(gl); r.texture_update(r.user, g_tex, 0, 0, P.w, P.h, P.frame, P.w * 4); }
        else { if (g_tex && g_ntrash < 16) g_trash[g_ntrash++] = g_tex; g_tex = cui_image_create(ui, P.w, P.h, P.frame); g_vw = P.w; g_vh = P.h; }
        g_tseq = P.fseq; ok = g_tex != 0;
    }
    LeaveCriticalSection(&P.cs);
    return ok;
}

static void fmt_time(LONGLONG ms, char *o, size_t cap) {
    if (ms < 0) ms = 0; LONGLONG s = ms / 1000;
    if (s >= 3600) snprintf(o, cap, "%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60); else snprintf(o, cap, "%lld:%02lld", s / 60, s % 60);
}
static bool in_r(cui_rect r, cui_vec2 p) { return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h; }

/* barra de progresso clicável: devolve a fração clicada (0..1) ou -1 */
static float seek_bar(cui_ctx *ui, const char *id, float frac, float width) {
    float out = -1.0f;
    cui_box(ui, .id = id, .width = CUI_PX(width), .height = CUI_PX(16), .align = CUI_ALIGN_CENTER) {
        cui_progress(ui, frac < 0 ? 0 : frac > 1 ? 1 : frac, .height = 5, .width = CUI_PX(width));
    }
    cui_rect r = cui_get_rect(ui, cui_make_id(ui, id));
    const cui_vec2 mp = cui_mouse_pos(ui);
    if (r.w > 0 && cui_mouse_clicked(ui, CUI_MOUSE_LEFT) && in_r(r, mp) && !briks_blocked() && !briks_covered(ui)) { out = (mp.x - r.x) / r.w; if (out < 0) out = 0; if (out > 1) out = 1; }
    return out;
}

bool media_inline(cui_ctx *ui, app_state_t *s, const char *id, const char *path, const char *name, bool mine, float max_w) {
    const int kind = media_kind_of(name);
    if (!kind || !path || !path[0]) return false;
    trash_flush(ui);
    item_t *it = item_get(path, kind);
    if (!it->exists_checked) { it->exists_checked = true; it->exists = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }
    if (!it->exists) return false;
    if (mine && it->state == 0) { InterlockedExchange(&it->state, 1); it->th = CreateThread(NULL, 0, poster_thread, it, 0, NULL); if (!it->th) InterlockedExchange(&it->state, 3); }
    if (it->state == 2 && it->rgba && !it->tex) { it->tex = cui_image_create(ui, it->w, it->h, it->rgba); free(it->rgba); it->rgba = NULL; }
    if (it->state == 3 && mine) { }   /* sem decodificador para o formato: mostra só o cartão do arquivo (a ▶ abaixo tenta mesmo assim) */
    const bool cur = pl_is(path);
    if (cur) g_drawn = GetTickCount64();
    const int st = cur ? (int)P.state : PL_IDLE;
    const bool playing = cur && st == PL_PLAYING, opening = cur && st == PL_OPENING, failed = cur && st == PL_FAILED;
    const LONGLONG dur = cur && P.dur_ms > 0 ? P.dur_ms : it->dur_ms, pos = cur ? P.pos_ms : 0;
    const float frac = dur > 0 ? (float)pos / (float)dur : 0.0f;
    char tp[32], td[32], tl[80]; fmt_time(pos, tp, sizeof tp); fmt_time(dur, td, sizeof td);
    if (dur > 0) snprintf(tl, sizeof tl, "%s / %s", tp, td); else snprintf(tl, sizeof tl, "%s", cur ? tp : "");
    cui_push_id(ui, id);
    if (playing || opening) cui_request_wakeup(ui, 0.03f);
    else if (cur && st == PL_PAUSED) cui_request_wakeup(ui, 0.2f);
    if (it->state == 1) cui_request_wakeup(ui, 0.1f);
    bool toggle = false; float seek = -1.0f;
    if (kind == MEDIA_VIDEO) {
        const float w = fminf(max_w > 120.0f ? max_w : 320.0f, 360.0f);
        float aspect = it->tex && it->h > 0 ? (float)it->h / (float)it->w : (cur && P.w > 0 ? (float)P.h / (float)P.w : 9.0f / 16.0f);
        cui_texture tex = 0; if (cur && live_tex(ui, s)) tex = g_tex; else if (it->tex) tex = it->tex;
        if (cur && P.w > 0) aspect = (float)P.h / (float)P.w;
        const float h = fmaxf(60.0f, w * aspect);
        cui_box(ui, .id = "mv", .dir = CUI_STACK, .width = CUI_PX(w), .height = CUI_PX(h), .radius = 14, .bg = CUI_RGBA(0x0a0410e0), .border = 1, .border_color = B_GLASS_BORDER, .clip = true) {
            if (tex) cui_image(ui, tex, .width = CUI_PX(w), .height = CUI_PX(h));
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                if (opening) cui_text(ui, "Abrindo…", .size = 13, .color = CUI_RGB(0xffffff));
                else if (failed) cui_text(ui, "Não foi possível reproduzir este vídeo aqui.", .size = 12, .color = CUI_RGB(0xffb4b4), .width = CUI_PX(w - 24));
                else if (!playing) cui_box(ui, .width = CUI_PX(48), .height = CUI_PX(48), .radius = 24, .bg = CUI_RGBA(0x000000a0), .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) { cui_icon(ui, CUI_ICON_PLAY, 22, CUI_RGB(0xffffff)); }
            }
        }
        if (!failed && !opening && briks_clicked(ui, "mv")) toggle = true;
        else if (failed && briks_clicked(ui, "mv")) { pl_start(path, kind); }
        if (cur && !failed) cui_box(ui, .dir = CUI_ROW, .width = CUI_PX(w), .gap = 8, .align = CUI_ALIGN_CENTER) {
            seek = seek_bar(ui, "mseek", frac, w - 120.0f);
            cui_text(ui, tl, .size = 11, .color = B_MUTED, .nowrap = true);
        }
        else if (!cur && dur > 0) cui_text(ui, td, .size = 11, .color = B_MUTED);
    } else {
        const float w = fminf(max_w > 200.0f ? max_w : 320.0f, 380.0f);
        cui_box(ui, .dir = CUI_ROW, .width = CUI_PX(w), .gap = 10, .pad_x = 10, .pad_y = 8, .radius = 14, .bg = B_GLASS_SOFT, .border = 1, .border_color = B_GLASS_BORDER_SOFT, .align = CUI_ALIGN_CENTER) {
            cui_box(ui, .id = "mp", .width = CUI_PX(34), .height = CUI_PX(34), .radius = 17, .bg = CUI_RGBA(0x8b5cf6cc), .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                cui_icon(ui, playing ? CUI_ICON_PAUSE : CUI_ICON_PLAY, 16, CUI_RGB(0xffffff));
            }
            if (failed) cui_text(ui, "Não foi possível reproduzir este áudio aqui.", .size = 12, .color = CUI_RGB(0xffb4b4));
            else { seek = seek_bar(ui, "mseek", frac, w - 34 - 20 - 10 - 90); cui_text(ui, opening ? "Abrindo…" : tl, .size = 11, .color = B_MUTED, .nowrap = true); }
        }
        if (briks_clicked(ui, "mp")) { if (cur && !failed) toggle = true; else pl_start(path, kind); }
    }
    cui_pop_id(ui);
    if (toggle) { if (cur) pl_toggle(); else pl_start(path, kind); }
    if (seek >= 0.0f && cur && dur > 0) { P.seek_ms = (LONGLONG)(seek * (float)dur); }
    return true;
}

/* ---------------------------------------------------------------- testes */
void media_script(const char *cmd) {
    if (!cmd) return;
    if (!strncmp(cmd, "play ", 5)) pl_start(cmd + 5, media_kind_of(cmd + 5) ? media_kind_of(cmd + 5) : MEDIA_VIDEO);
    else if (!strcmp(cmd, "pause")) { if (P.inited) P.pause = 1; }
    else if (!strcmp(cmd, "resume")) { if (P.inited) P.pause = 0; }
    else if (!strncmp(cmd, "seek ", 5)) { if (P.inited) P.seek_ms = atoi(cmd + 5); }
    else if (!strcmp(cmd, "stop")) pl_stop_wait();
}
int media_probe(const char *name) {
    if (!strcmp(name, "state")) return P.inited ? (int)P.state : 0;      /* 0 parado, 1 abrindo, 2 tocando, 3 pausado, 4 terminou, 5 falhou */
    if (!strcmp(name, "playing")) return P.inited && P.state == PL_PLAYING;
    if (!strcmp(name, "pos_ms")) return P.inited ? (int)P.pos_ms : 0;
    if (!strcmp(name, "dur_ms")) return P.inited ? (int)P.dur_ms : 0;
    if (!strcmp(name, "frames")) return P.inited ? (int)P.fseq : 0;
    return -9999;
}
