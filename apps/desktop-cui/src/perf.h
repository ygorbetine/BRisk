/* Modo Desempenho (Configurações > Desempenho): Normal / Simplificado / Mínimo. Aplicação ao vivo; guardado em
   %APPDATA%\Briks-CUI\settings.json ({"perf":"normal|simples|minimo"}); BRIKS_PERF=normal|simples|minimo sobrepõe (teste). */
#ifndef BRIKS_PERF_H
#define BRIKS_PERF_H
#include <cui/cui.h>
#include <stdbool.h>

enum { PERF_NORMAL = 0, PERF_SIMPLE = 1, PERF_MIN = 2 };
extern int g_perf;          /* nível atual */
extern bool g_perf_hint;    /* o app detectou hardware fraco e sugere o Simplificado (aviso discreto) */

void briks_perf_init(void);                         /* lê BRIKS_PERF, settings.json e a preferência de animações do Windows */
void briks_perf_set(cui_ctx *ui, int level, bool save);
void briks_perf_note_frame(double ms, int index);   /* glass.c: tempo dos primeiros quadros, para a detecção automática */
void briks_perf_note_renderer(const char *name);    /* glass.c: GL_RENDERER (software => sugere Simplificado) */
const char *briks_perf_label(int level);
const char *briks_perf_help(int level);             /* uma linha explicando o nível */

/* sombra por nível: Normal como hoje, Simplificado uma camada só (RESTING), Mínimo nenhuma */
static inline cui_elevation briks_elev(cui_elevation e) {
    if (g_perf >= PERF_MIN) return CUI_ELEVATION_NONE;
    if (g_perf == PERF_SIMPLE && e > CUI_ELEVATION_RESTING) return CUI_ELEVATION_RESTING;
    return e;
}
#endif
