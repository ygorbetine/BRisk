/* Conta, extras sem janela (testáveis): data de nascimento do cadastro (AUT-03, só maiores de 18) e dica local de login por @nick (AUT-11). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <windows.h>
#include <shlobj.h>

/* "DD/MM/AAAA" (também aceita - ou . e dia/mês de 1 dígito). Devolve false se não for uma data real. */
bool birth_parse(const char *t, int *d, int *m, int *y) {
    int a = 0, b = 0, c = 0, n = 0;
    if (!t || sscanf(t, " %d%*[/.-]%d%*[/.-]%d %n", &a, &b, &c, &n) < 3 || t[n]) return false;
    if (c < 1900 || c > 2200 || b < 1 || b > 12 || a < 1) return false;
    static const int dim[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int mx = dim[b - 1] + ((b == 2 && ((c % 4 == 0 && c % 100 != 0) || c % 400 == 0)) ? 1 : 0);
    if (a > mx) return false;
    *d = a; *m = b; *y = c; return true;
}

/* 0 = ok (tem 18 anos completos em hoje ty/tm/td); senão preenche err e devolve 1 (formato), 2 (data no futuro) ou 3 (menor de 18). A data não é guardada. */
int birth_check(const char *txt, int ty, int tm, int td, char *err, size_t cap) {
    int d, m, y;
    if (!birth_parse(txt, &d, &m, &y)) { snprintf(err, cap, "Digite a data de nascimento como dia/mês/ano, por exemplo 25/03/1998."); return 1; }
    if (y > ty || (y == ty && (m > tm || (m == tm && d > td)))) { snprintf(err, cap, "Essa data de nascimento ainda não chegou. Confira o ano."); return 2; }
    int age = ty - y - ((tm < m || (tm == m && td < d)) ? 1 : 0);
    if (age < 18) { snprintf(err, cap, "O Briks é só para maiores de 18 anos. Não foi possível criar a conta."); return 3; }
    err[0] = 0; return 0;
}

/* dica de login: @nick -> e-mail, guardada só neste computador (login-hints.txt, uma linha "nick<TAB>email") */
static bool hints_path(char *out, size_t cap) {
    const char *o = getenv("BRIKS_LOGIN_HINTS_FILE");
    if (o && o[0]) { snprintf(out, cap, "%s", o); return true; }
    const char *dd = getenv("BRIKS_CUI_DATA");
    if (dd && dd[0]) { snprintf(out, cap, "%s\\login-hints.txt", dd); return true; }
    char ad[MAX_PATH]; if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, ad) != S_OK) return false;
    char dir[MAX_PATH]; snprintf(dir, sizeof dir, "%s\\Briks-CUI", ad); CreateDirectoryA(dir, NULL);
    snprintf(out, cap, "%s\\login-hints.txt", dir); return true;
}
static void lower(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

void login_hint_save(const char *nick, const char *email) {
    char p[MAX_PATH]; char nk[40], em[120];
    if (!nick || !nick[0] || !email || !strchr(email, '@') || !hints_path(p, sizeof p)) return;
    snprintf(nk, sizeof nk, "%s", nick[0] == '@' ? nick + 1 : nick); lower(nk);
    snprintf(em, sizeof em, "%s", email);
    static char keep[40][160]; int n = 0;
    FILE *f = fopen(p, "rb");
    if (f) {
        char l[400];
        while (n < 39 && fgets(l, sizeof l, f)) {
            size_t k = strlen(l); while (k && (l[k - 1] == '\n' || l[k - 1] == '\r')) l[--k] = 0;
            char *t = strchr(l, '\t'); if (!t) continue; *t = 0;
            if (!strcmp(l, nk)) continue;
            snprintf(keep[n++], sizeof keep[0], "%s\t%s", l, t + 1);
        }
        fclose(f);
    }
    f = fopen(p, "wb"); if (!f) return;
    for (int i = 0; i < n; i++) fprintf(f, "%s\n", keep[i]);
    fprintf(f, "%s\t%s\n", nk, em); fclose(f);
}

bool login_hint_lookup(const char *nick, char *email, size_t cap) {
    char p[MAX_PATH], nk[40]; if (!nick || !hints_path(p, sizeof p)) return false;
    snprintf(nk, sizeof nk, "%s", nick[0] == '@' ? nick + 1 : nick); lower(nk);
    FILE *f = fopen(p, "rb"); if (!f) return false;
    bool ok = false; char l[400];
    while (fgets(l, sizeof l, f)) {
        size_t k = strlen(l); while (k && (l[k - 1] == '\n' || l[k - 1] == '\r')) l[--k] = 0;
        char *t = strchr(l, '\t'); if (!t) continue; *t = 0;
        if (!strcmp(l, nk)) { snprintf(email, cap, "%s", t + 1); ok = true; }
    }
    fclose(f); return ok;
}
