#include "hotkeys.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static int ieq(const char *a, const char *b) { for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0; return !*a && !*b; }

bool hk_parse(const char *acc, unsigned *mods, int *vk) {
    if (!acc || !acc[0]) return false;
    char buf[48]; snprintf(buf, sizeof buf, "%s", acc);
    unsigned m = 0; int key = 0;
    for (char *t = strtok(buf, "+"); t; t = strtok(NULL, "+")) {
        while (*t == ' ') t++;
        size_t n = strlen(t); while (n && t[n - 1] == ' ') t[--n] = 0;
        if (!n) return false;
        if (ieq(t, "ctrl") || ieq(t, "control")) m |= HK_CTRL;
        else if (ieq(t, "shift")) m |= HK_SHIFT;
        else if (ieq(t, "alt")) m |= HK_ALT;
        else if (key) return false;   /* duas teclas principais */
        else if (n == 1 && isalpha((unsigned char)t[0])) key = toupper((unsigned char)t[0]);
        else if (n == 1 && isdigit((unsigned char)t[0])) key = t[0];
        else if ((t[0] == 'F' || t[0] == 'f') && n <= 3 && atoi(t + 1) >= 1 && atoi(t + 1) <= 24) key = 0x70 + atoi(t + 1) - 1;
        else if (ieq(t, "space") || ieq(t, "espaço") || ieq(t, "espaco")) key = 0x20;
        else if (ieq(t, "mouse3") || ieq(t, "meio")) key = 0x04;
        else if (ieq(t, "mouse4")) key = 0x05;
        else if (ieq(t, "mouse5")) key = 0x06;
        else if (ieq(t, "tab")) key = 0x09;
        else if (ieq(t, "capslock")) key = 0x14;
        else if (ieq(t, "enter")) key = 0x0D;
        else return false;
    }
    if (!key) return false;
    *mods = m; *vk = key; return true;
}

bool hk_ptt_step(hk_ptt_t *p, bool down_now, double now_ms, double delay_ms) {
    if (down_now) { p->held = true; p->open = true; }
    else {
        if (p->held) { p->held = false; p->released_at = now_ms; }
        if (p->open && now_ms - p->released_at >= delay_ms) p->open = false;
    }
    return p->open;
}

