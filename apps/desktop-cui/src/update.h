/* Atualização automática do app nativo. Mesmo formato do feed do electron-updater (latest.yml + instalador NSIS), para que o Briks 1.2.x
 * (Electron) instalado e o app nativo usem o mesmo release do GitHub. O instalador baixado é verificado por tamanho e SHA-512 do feed. */
#ifndef BRIKS_UPDATE_H
#define BRIKS_UPDATE_H
#include <stdbool.h>

typedef enum { UPD_IDLE, UPD_CHECKING, UPD_DOWNLOADING, UPD_READY, UPD_ERROR } upd_phase_t;

void update_start(void);                 /* thread de segundo plano: 1ª checagem ~15 s após abrir e a cada 4 h (só no app instalado ou com BRIKS_UPDATE_FEED) */
upd_phase_t update_phase(void);
int update_percent(void);
const char *update_version(void);        /* versão disponível (quando há) */
const char *update_error(void);
const char *update_current_version(void);
void update_dismiss(void);               /* "Depois": não avisa de novo nesta sessão (a instalação ao fechar continua) */
bool update_dismissed(void);
bool update_install_now(void);           /* inicia o instalador (silencioso, reabre o app); o chamador fecha o app em seguida */
void update_install_on_exit(void);       /* ao fechar: se há atualização pronta e verificada, instala em silêncio sem reabrir */

/* utilitários puros (testados em tests/smoke.c) */
int update_version_compare(const char *a, const char *b);   /* <0, 0, >0 (semver simples; pré-lançamento < final) */
bool update_parse_feed(const char *yaml, char *version, size_t vcap, char *path, size_t pcap, char *sha512, size_t scap);
bool update_policy(bool disable, bool has_feed, bool enable_env, bool enable_file, bool default_on, bool installed);   /* quando a atualizacao liga */
bool update_url_allowed(const char *url);                    /* https, ou http só em loopback (testes) */
#endif
