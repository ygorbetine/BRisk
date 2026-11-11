#include "hotkeys.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static int ieq(const char *a, const char *b) { for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0; return !*a && !*b; }

static const struct { const char *name; int vk; } NAMED[] = {
    { "Tab", 0x09 }, { "Enter", 0x0D }, { "CapsLock", 0x14 }, { "Espaço", 0x20 }, { "PageUp", 0x21 }, { "PageDown", 0x22 }, { "End", 0x23 }, { "Home", 0x24 },
    { "Left", 0x25 }, { "Up", 0x26 }, { "Right", 0x27 }, { "Down", 0x28 }, { "Insert", 0x2D }, { "Delete", 0x2E }, { "Pause", 0x13 }, { "ScrollLock", 0x91 },
    { "Mouse3", 0x04 }, { "Mouse4", 0x05 }, { "Mouse5", 0x06 },
};
#define NNAMED ((int)(sizeof NAMED / sizeof *NAMED))

bool hk_is_mouse(int vk) { return vk >= 0x04 && vk <= 0x06; }

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
        else if (ieq(t, "space") || ieq(t, "espaco")) key = 0x20;
        else if (ieq(t, "meio")) key = 0x04;
        else if (ieq(t, "pgup")) key = 0x21;
        else if (ieq(t, "pgdn")) key = 0x22;
        else if (ieq(t, "ins")) key = 0x2D;
        else if (ieq(t, "del")) key = 0x2E;
        else { for (int i = 0; i < NNAMED; i++) if (ieq(t, NAMED[i].name)) { key = NAMED[i].vk; break; } if (!key) return false; }
    }
    if (!key) return false;
    if (hk_is_mouse(key) && m) return false;   /* botão do mouse não combina com modificador */
    *mods = m; *vk = key; return true;
}

bool hk_format(unsigned mods, int vk, char *out, size_t cap) {
    char k[16];
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) snprintf(k, sizeof k, "%c", vk);
    else if (vk >= 0x70 && vk <= 0x87) snprintf(k, sizeof k, "F%d", vk - 0x70 + 1);
    else { int i; for (i = 0; i < NNAMED; i++) if (NAMED[i].vk == vk) { snprintf(k, sizeof k, "%s", NAMED[i].name); break; } if (i == NNAMED) return false; }
    snprintf(out, cap, "%s%s%s%s", (mods & HK_CTRL) ? "Ctrl+" : "", (mods & HK_SHIFT) ? "Shift+" : "", (mods & HK_ALT) ? "Alt+" : "", k);
    return true;
}

bool hk_same(const char *a, const char *b) {
    unsigned ma, mb; int va, vb;
    return hk_parse(a, &ma, &va) && hk_parse(b, &mb, &vb) && ma == mb && va == vb;
}

bool hk_ptt_step(hk_ptt_t *p, bool down_now, double now_ms, double delay_ms) {
    if (down_now) { p->held = true; p->open = true; }
    else {
        if (p->held) { p->held = false; p->released_at = now_ms; }
        if (p->open && now_ms - p->released_at >= delay_ms) p->open = false;
    }
    return p->open;
}
