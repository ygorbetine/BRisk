#include "drag_stack.h"
#include <math.h>
#include "layout.h"

const float PILE_OFF_X[PILE_LAYERS] = { -10, -4, 0 };
const float PILE_OFF_Y[PILE_LAYERS] = { 6, 3, 0 };
const float PILE_REST_DEG[PILE_LAYERS] = { -5, -2, 2 };
const float PILE_ALPHA[PILE_LAYERS] = { 0.5f, 0.78f, 1.0f };

/* rigidez e amortecimento por camada: a de trás é mais mole (mais atraso e rastro) */
static const float FOLLOW_K[PILE_LAYERS] = { 170, 260, 520 };
static const float FOLLOW_Z[PILE_LAYERS] = { 0.62f, 0.70f, 0.78f };
#define ROT_K 260.0f
#define ROT_Z 0.55f
#define LIFT_K 380.0f
#define LIFT_Z 0.50f
#define END_DUR 0.26f

int pile_simple;   /* 0 normal (molas), 1 Simplificado, 2 Mínimo: ver dock.c */

void spring_step(spring1 *s, float target, float dt, float stiff, float zeta) {
    if (dt <= 0) return;
    int n = (int)ceilf(dt / 0.004f); if (n < 1) n = 1; if (n > 64) n = 64;
    float h = dt / (float)n, c = 2.0f * zeta * sqrtf(stiff);
    for (int i = 0; i < n; i++) {
        float a = stiff * (target - s->x) - c * s->v;
        s->v += a * h; s->x += s->v * h;
    }
}

float pile_tilt_deg(float vx) {
    float d = vx * 0.0085f;               /* 1000 px/s ≈ 8,5° */
    if (d > 14) d = 14; if (d < -14) d = -14;
    return d;
}

static float ease(float t) { t = t < 0 ? 0 : t > 1 ? 1 : t; return t * t * (3 - 2 * t); }

void pile_grab(pile_t *p, float fx, float fy) {
    *p = (pile_t){ 0 };
    p->phase = PILE_HELD;
    for (int i = 0; i < PILE_LAYERS; i++) { p->x[i].x = fx + PILE_OFF_X[i]; p->y[i].x = fy + PILE_OFF_Y[i]; p->rot[i].x = PILE_REST_DEG[i]; }
    p->lift.x = 0;   /* sobe com mola até 1 */
    p->last_mx = fx; p->have_last = true;
}

void pile_follow(pile_t *p, float fx, float fy, float dt) {
    if (p->phase != PILE_HELD) return;
    if (dt > 0) {
        float v = (fx - p->last_mx) / dt;
        p->vx += (v - p->vx) * (1.0f - expf(-dt / 0.06f));   /* filtro: tira o serrilhado do mouse */
        p->last_mx = fx;
    }
    p->end_x = fx; p->end_y = fy;   /* enquanto segura, end_* é o alvo do cartão da frente */
}

void pile_release(pile_t *p, float sx, float sy) {
    if (p->phase != PILE_HELD) return;
    p->from_scale = pile_scale(p); p->phase = PILE_DROP; p->t = 0; p->end_x = sx; p->end_y = sy;
}

void pile_cancel(pile_t *p, float ox, float oy) {
    if (p->phase != PILE_HELD) return;
    p->from_scale = pile_scale(p); p->phase = PILE_CANCEL; p->t = 0; p->end_x = ox; p->end_y = oy;
}

void pile_step(pile_t *p, float dt) {
    if (p->phase == PILE_IDLE) return;
    bool held = p->phase == PILE_HELD;
    if (!held) p->vx *= expf(-dt / 0.08f);       /* ao soltar a velocidade some e o giro volta ao repouso */
    float tilt = held ? pile_tilt_deg(p->vx) : 0;
    /* a parada do mouse zera vx aos poucos mesmo sem eventos */
    if (held && fabsf(p->vx) < 1.0f) p->vx = 0;
    for (int i = 0; i < PILE_LAYERS; i++) {
        float tx = p->end_x + PILE_OFF_X[i] * (held ? 1.0f : 1.0f - ease(p->t)), ty = p->end_y + PILE_OFF_Y[i] * (held ? 1.0f : 1.0f - ease(p->t));
        if (pile_simple) {   /* modo Simplificado/Mínimo: prévia simples, sem mola nem rastro; os cartões acompanham o cursor sem atraso */
            p->x[i].x = tx; p->x[i].v = 0; p->y[i].x = ty; p->y[i].v = 0;
            p->rot[i].x = PILE_REST_DEG[i] * (held ? 1.0f : 1.0f - 0.5f * ease(p->t)); p->rot[i].v = 0;
            continue;
        }
        spring_step(&p->x[i], tx, dt, held ? FOLLOW_K[i] : 620.0f, held ? FOLLOW_Z[i] : 0.95f);
        spring_step(&p->y[i], ty, dt, held ? FOLLOW_K[i] : 620.0f, held ? FOLLOW_Z[i] : 0.95f);
        /* camadas de trás inclinam um pouco mais (delay do rastro) */
        float k = 1.0f + 0.25f * (float)(PILE_LAYERS - 1 - i);
        spring_step(&p->rot[i], PILE_REST_DEG[i] * (held ? 1.0f : 1.0f - 0.5f * ease(p->t)) + tilt * k, dt, ROT_K, ROT_Z);
    }
    if (pile_simple) { p->lift.x = held ? 1.0f : 0.0f; p->lift.v = 0; }
    else spring_step(&p->lift, held ? 1.0f : 0.0f, dt, LIFT_K, LIFT_Z);
    if (!held) {
        p->t += dt / (pile_simple >= 2 ? 0.001f : pile_simple ? 0.10f : END_DUR);   /* Mínimo: some na hora */
        if (p->t >= 1.0f) { p->t = 1.0f; p->phase = PILE_IDLE; }
    }
}

bool pile_active(const pile_t *p) { return p->phase != PILE_IDLE; }
float pile_scale(const pile_t *p) {
    float s = 1.0f + 0.04f * p->lift.x;
    if (p->phase == PILE_DROP || p->phase == PILE_CANCEL) s = p->from_scale + (0.78f - p->from_scale) * ease(p->t);
    return s;
}
float pile_alpha(const pile_t *p) {
    if (p->phase == PILE_DROP || p->phase == PILE_CANCEL) return 1.0f - ease((p->t - 0.25f) / 0.75f);
    return p->phase == PILE_IDLE ? 0.0f : 1.0f;
}

/* ---- arte do cartão: tamanho e grades ---- */
void stack_card_size(int kind, float pw, float ph, float *w, float *h) {
    float minw = 150, aspect = ph > 1 ? pw / ph : 1.5f;
    switch (kind) {
    case P_SERVERS: minw = 196; break;
    case P_VOICE: minw = 188; break;
    case P_HISTORY: minw = 176; break;
    case P_NAV: case P_CHAT: case P_CHAT2: minw = 164; break;
    default: break;
    }
    float hh = 118, ww = hh * aspect;
    if (ww > 200) { ww = 200; hh = ww / (aspect > 0.1f ? aspect : 1); }
    if (ww < minw) ww = minw;
    if (hh < 100) hh = 100;
    if (kind == P_VOICE || kind == P_SERVERS) { if (hh < 124) hh = 124; }   /* grades precisam de altura */
    if (hh > 128) hh = 128;
    *w = ww; *h = hh;
}

stack_grid stack_voice_grid(int n) {
    stack_grid g = { 1, 1, 1, 0 };
    if (n <= 1) return g;
    if (n <= 3) { g.cols = n; g.rows = 1; g.cells = n; return g; }
    if (n == 4) { g.cols = 2; g.rows = 2; g.cells = 4; return g; }
    g.cols = 3; g.rows = 2;
    if (n <= 6) { g.cells = n; return g; }
    g.cells = 6; g.plus_n = n - 5;
    return g;
}

stack_grid stack_servers_grid(int communities) {
    int total = 1 + (communities < 0 ? 0 : communities) + 1;   /* B + comunidades + "+" */
    stack_grid g = { 6, 1, total, 0 };
    if (total > 12) { g.cells = 12; g.plus_n = total - 11; }
    g.rows = (g.cells + 5) / 6;
    return g;
}
