/* Glassmorfismo real: backdrop-filter: blur(20px) saturate(155%) do glass-layout.css.
 *
 * O CUI desenha o quadro inteiro numa única chamada cui_gl_render. Pelo gancho
 * cui_app_set_render_override (patches/cui-render-override.patch) o quadro é desenhado em segmentos.
 * Cada caixa cujo fundo é uma das cores B_GLASS* vira um "painel de vidro": antes de o CUI desenhar o
 * fundo translúcido, o framebuffer atual é copiado para uma textura, desfocado (dual-Kawase) e o
 * retângulo arredondado do painel é preenchido com o resultado saturado. Depois o CUI pinta a camada
 * translúcida, a borda e o conteúdo por cima. O desfoque é refeito só quando a camada muda
 * (painéis de base compartilham o fundo ambiente; popups usam a cena inteira).
 * Sem GL de hardware (ou BRIKS_GLASS=0) o gancho não é instalado e o app fica só translúcido. */
#include "briks_ui.h"
#include <windows.h>
/* GL 1.1 exportado por opengl32.dll; sem o protótipo o retorno vira int e o ponteiro é truncado em 64 bits */
__declspec(dllimport) const unsigned char *APIENTRY glGetString(unsigned int name);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef unsigned int GLuint; typedef int GLint; typedef int GLsizei; typedef unsigned int GLenum;
typedef float GLfloat; typedef char GLchar; typedef unsigned char GLboolean;
#define GLAPI_ __stdcall
#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA8 0x8058
#define GL_RGBA16F 0x881A
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_LINEAR 0x2601
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_VIEWPORT 0x0BA2
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_TRIANGLE_STRIP 5
#define GL_ONE 1
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_TEXTURE2 0x84C2
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5

#define GL_FNS(X) \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, Enable, (GLenum)) X(void, Disable, (GLenum)) \
    X(void, BlendFunc, (GLenum, GLenum)) \
    X(void, Scissor, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, GetIntegerv, (GLenum, GLint *)) \
    X(void, GenTextures, (GLsizei, GLuint *)) X(void, DeleteTextures, (GLsizei, const GLuint *)) \
    X(void, BindTexture, (GLenum, GLuint)) \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, TexParameteri, (GLenum, GLenum, GLint)) \
    X(void, CopyTexSubImage2D, (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei)) \
    X(void, DrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, ActiveTexture, (GLenum)) \
    X(void, GenFramebuffers, (GLsizei, GLuint *)) X(void, DeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void, BindFramebuffer, (GLenum, GLuint)) \
    X(void, FramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(GLenum, CheckFramebufferStatus, (GLenum)) \
    X(GLuint, CreateShader, (GLenum)) X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, CompileShader, (GLuint)) X(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
    X(GLuint, CreateProgram, (void)) X(void, AttachShader, (GLuint, GLuint)) X(void, LinkProgram, (GLuint)) \
    X(void, GetProgramiv, (GLuint, GLenum, GLint *)) X(void, DeleteShader, (GLuint)) \
    X(void, DeleteProgram, (GLuint)) X(void, UseProgram, (GLuint)) \
    X(GLint, GetUniformLocation, (GLuint, const GLchar *)) \
    X(void, Uniform1i, (GLint, GLint)) X(void, Uniform1f, (GLint, GLfloat)) \
    X(void, Uniform2f, (GLint, GLfloat, GLfloat)) X(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, GenVertexArrays, (GLsizei, GLuint *)) X(void, BindVertexArray, (GLuint)) \
    X(void, Finish, (void))

#define X(r, n, a) static r (GLAPI_ *gl##n) a;
GL_FNS(X)
#undef X

#define MAX_LEVELS 5

typedef struct {
    bool ok;
    int w, h;
    int levels;
    float offset;
    GLuint src;                      /* cópia do quadro, tamanho cheio */
    GLuint lvl[MAX_LEVELS + 1];      /* lvl[i]: tamanho cheio / 2^i */
    int lw[MAX_LEVELS + 1], lh[MAX_LEVELS + 1];
    GLuint fbo, vao;
    GLuint p_down, p_up, p_comp, p_dith;
    GLuint hdr_tex, hdr_fbo; int hdr_w, hdr_h;   /* quadro em RGBA16F: o dither entra antes de quantizar para 8 bits */
    GLint t_tex, t_vp, t_rect;
    GLint d_tex, d_out, d_texel, d_off, d_vp, d_rect;
    GLint u_tex, u_out, u_texel, u_off, u_vp, u_rect;
    GLint c_tex, c_vp, c_rect, c_box, c_rad, c_sat, c_hl, c_rot, c_alpha, c_card, c_bub, c_a1, c_a2, c_a3, c_src, c_m1, c_m2, c_m3;
    double ms_last, ms_sum; int frames;
    bool log;
} glass_t;
static glass_t G;

static void *gl_proc(const char *name) {
    static HMODULE lib;
    void *p = (void *)wglGetProcAddress(name);
    if (!p || p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1) {
        if (!lib) lib = LoadLibraryA("opengl32.dll");
        p = (void *)GetProcAddress(lib, name);
    }
    return p;
}

static bool load_gl(void) {
#define X(r, n, a) if (!(gl##n = (r (GLAPI_ *) a)gl_proc("gl" #n))) return false;
    GL_FNS(X)
#undef X
    return true;
}

static const char *VS =
    "#version 330 core\n"
    "uniform vec4 u_rect; uniform vec2 u_vp; out vec2 v_px;\n"
    "void main(){ vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));\n"
    "  vec2 p = u_rect.xy + c * u_rect.zw; v_px = p;\n"
    "  gl_Position = vec4(p.x / u_vp.x * 2.0 - 1.0, 1.0 - p.y / u_vp.y * 2.0, 0.0, 1.0); }\n";

static const char *FS_DOWN =
    "#version 330 core\n"
    "uniform sampler2D u_tex; uniform vec2 u_out; uniform vec2 u_texel; uniform float u_off; out vec4 f;\n"
    "void main(){ vec2 uv = gl_FragCoord.xy / u_out; vec2 o = u_texel * u_off;\n"
    "  vec4 s = texture(u_tex, uv) * 4.0;\n"
    "  s += texture(u_tex, uv + vec2(-o.x, -o.y)); s += texture(u_tex, uv + vec2(o.x, -o.y));\n"
    "  s += texture(u_tex, uv + vec2(-o.x, o.y));  s += texture(u_tex, uv + vec2(o.x, o.y));\n"
    "  f = vec4((s / 8.0).rgb, 1.0); }\n";

static const char *FS_UP =
    "#version 330 core\n"
    "uniform sampler2D u_tex; uniform vec2 u_out; uniform vec2 u_texel; uniform float u_off; out vec4 f;\n"
    "void main(){ vec2 uv = gl_FragCoord.xy / u_out; vec2 o = u_texel * u_off;\n"
    "  vec4 s = texture(u_tex, uv + vec2(-o.x * 2.0, 0.0)) + texture(u_tex, uv + vec2(o.x * 2.0, 0.0))\n"
    "         + texture(u_tex, uv + vec2(0.0, -o.y * 2.0)) + texture(u_tex, uv + vec2(0.0, o.y * 2.0));\n"
    "  s += 2.0 * (texture(u_tex, uv + vec2(-o.x, -o.y)) + texture(u_tex, uv + vec2(o.x, -o.y))\n"
    "            + texture(u_tex, uv + vec2(-o.x, o.y)) + texture(u_tex, uv + vec2(o.x, o.y)));\n"
    "  f = vec4((s / 12.0).rgb, 1.0); }\n";

static const char *FS_COMP =
    "#version 330 core\n"
    "in vec2 v_px; uniform sampler2D u_tex; uniform vec2 u_vp; uniform vec4 u_box; uniform vec4 u_rad;\n"
    "uniform float u_sat; uniform float u_hl; uniform vec2 u_rot; uniform float u_alpha; uniform vec4 u_card; uniform vec4 u_bub; uniform vec4 u_a1; uniform vec4 u_a2; uniform vec4 u_a3; uniform sampler2D u_src; uniform vec4 u_m1; uniform vec4 u_m2; uniform vec4 u_m3; out vec4 f;\n"
    "float sd(vec2 p, vec2 b, vec4 r){ float rr = p.x < 0.0 ? (p.y < 0.0 ? r.x : r.w) : (p.y < 0.0 ? r.y : r.z);\n"
    "  rr = min(rr, min(b.x, b.y)); vec2 q = abs(p) - b + rr; return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - rr; }\n"
    "vec3 smp(vec2 uv, float bl){ return mix(texture(u_src, uv).rgb, texture(u_tex, uv).rgb, bl); }\n"
    "float wob(vec2 v, float ph){ float a = atan(v.y, v.x); return sin(2.0 * a + ph) * 0.6 + sin(3.0 * a + ph * 1.7 + 1.0) * 0.4; }\n"
    "float smin(float a, float b, float k){ float h = max(k - abs(a - b), 0.0) / k; return min(a, b) - h * h * k * 0.25; }\n"
    "void main(){ vec2 p = v_px - u_box.xy; p = vec2(u_rot.x * p.x + u_rot.y * p.y, -u_rot.y * p.x + u_rot.x * p.y); vec2 b = u_box.zw;\n"
    "  float d = sd(p, b, u_rad);\n"
    "  float dBal = d;\n"
    "  if (u_card.w > 0.5) {   /* bolha organica: harmonicos de baixa frequencia, fase fixa por balao (u_m3.x), respiracao lenta (u_m3.y/z) so no Normal */\n"
    "    float ph = u_m3.x, br = u_m3.z * sin(u_m3.y);\n"
    "    d += (0.012 * b.y) * wob(p, ph) + 0.35 * br + 0.0 * (p.x / max(b.x, 1.0)) * (p.y / max(b.y, 1.0));   /* feijao: um lado mais cheio */\n"
    "    dBal = d;\n"
    "    if (u_m1.w > 0.5) { vec2 v1 = v_px - u_m1.xy, v2 = v_px - u_m2.xy; float d1 = length(v1) - u_m1.z - 0.3 * br;\n"
    "      d = smin(d, d1, 19.0 + 2.0 * br);\n"
    "      if (u_m2.w > 0.5) { float d2 = length(v2) - u_m2.z; d = smin(d, d2, 11.0 + 1.5 * br); } }\n"
    "  }\n"
    " float cov = clamp(0.5 - d, 0.0, 1.0) * u_alpha; float sa = 0.0;\n"
    "  if (u_card.w > 0.5) { vec2 gs = vec2(dFdx(d), dFdy(d)); gs = gs / max(length(gs), 1e-4); float dS = d - 3.5 * gs.y; sa = 0.30 * exp(-max(dS, 0.0) / 4.5) * (1.0 - clamp(0.5 - d, 0.0, 1.0)) * u_alpha; }   /* sombra violeta curta embaixo */\n"
    "  if (cov <= 0.0 && sa < 0.004) discard;\n"
    "  vec2 g0 = vec2(dFdx(d), dFdy(d)); g0 = g0 / max(length(g0), 1e-4);\n"
    "  vec2 uv0 = gl_FragCoord.xy / u_vp; vec3 c;\n"
    "  /* bolha de status (Liquid Glass 2026): corpo quase transparente, refracao + aberracao cromatica concentradas na borda, desfoque so para legibilidade */\n"
    "  float bl = 0.3, ssat = 1.15, br = 1.04; float tailw = u_m1.w > 0.5 ? smoothstep(-2.0, 3.0, dBal) : 0.0;   /* 0 no balao, 1 na cauda (pescoco e bolinhas): visual mais simples */\n"
    "  if (u_card.w > 0.5) {\n"
    "    vec2 o = vec2(0.0); float dsp = 0.0;\n"
    "    if (u_card.w > 1.5) { float s = exp(-max(-d, 0.0) / 9.0); o = vec2(g0.x, -g0.y) * (-s) * u_bub.x * 1.8 * (1.0 - 0.5 * tailw) / u_vp; dsp = (0.35 + u_bub.y * 1.5) * (1.0 - 0.9 * tailw); }   /* Normal: lente + dispersao R/G/B */\n"
    "    else bl = 0.4;   /* Simplificado: sem refracao nem aberracao */\n"
    "    c = vec3(smp(uv0 + o * (1.0 - dsp), bl).r, smp(uv0 + o, bl).g, smp(uv0 + o * (1.0 + dsp), bl).b);\n"
    "    c = clamp(mix(vec3(dot(c, vec3(0.2126, 0.7152, 0.0722))), c, ssat) * br, 0.0, 1.0);\n"
    "  } else c = texture(u_tex, uv0).rgb;\n"
    "  float l = dot(c, vec3(0.2126, 0.7152, 0.0722)); c = clamp(mix(vec3(l), c, u_sat), 0.0, 1.0);\n"
    "  /* vidro: luz de borda suave vinda do canto superior esquerdo; normal da borda pelo gradiente do SDF, caimento suave para dentro */\n"
    "  vec2 g = vec2(dFdx(d), dFdy(d)); g = g / max(length(g), 1e-4);          /* aponta para fora */\n"
    "  float lit = dot(g, normalize(vec2(-0.7, -0.7)));                         /* +1 virado para a luz (y cresce para baixo) */\n"
    "  float edge = exp(-max(-d, 0.0) / 3.0);                                   /* caimento ~3px */\n"
    "  float rimL = max(lit, 0.0) * edge, rimD = max(-lit, 0.0) * edge;\n"
    "  if (u_card.w < 0.5) { c = mix(c, mix(u_a3.rgb, vec3(1.0), 0.8), u_hl * 1.6 * rimL); c *= 1.0 - u_hl * 1.1 * rimD; }\n"
    "  if (u_card.w < 0.5 && u_card.x + u_card.y + u_card.z > 0.0) {   /* cartão de vidro: reflexo diagonal + fio de luz no alto + tinta lilás; tudo dentro do SDF do próprio elemento */\n"
    "    vec2 lp = p / max(b, vec2(1.0));\n"
    "    float gd = clamp(0.5 - 0.5 * (lp.x * 0.6 + lp.y * 0.8), 0.0, 1.0);\n"
    "    c = mix(c, mix(u_a3.rgb, vec3(1.0), 0.25), u_card.z);\n"
    "    c = mix(c, mix(u_a3.rgb, vec3(1.0), 0.75), u_card.x * gd * gd);\n"
    "    float top = max(dot(g, vec2(0.0, -1.0)), 0.0) * 0.75 + max(lit, 0.0) * 0.25;\n"
    "    c = mix(c, mix(u_a3.rgb, vec3(1.0), 0.75), u_card.y * top * exp(-max(-d, 0.0) / 1.3));\n"
    "  }\n"
    "  if (u_card.w > 0.5) {   /* bolha: tinta violeta leve no centro (mais forte sobre fundo claro, para o texto manter contraste), bisel iridescente por angulo, brilho largo no canto superior esquerdo, caustic no inferior direito */\n"
    "    float k = u_card.x; float depth = max(-d, 0.0);\n"
    "    float bgl = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
    "    float tA = (0.10 + 0.42 * smoothstep(0.25, 0.75, bgl)) * mix(1.0, 0.5, exp(-depth / 6.0));\n"
    "    c = mix(c, vec3(0.30, 0.16, 0.62), clamp(tA * k, 0.0, 1.0));\n"
    "    float w = 1.0 - smoothstep(0.0, mix(4.5, 2.4, tailw), depth); w = w * w * (3.0 - 2.0 * w);\n"
    "    float tt = 0.5 + 0.5 * sin(atan(g0.y, g0.x) + 0.6 + u_m3.x * 0.1);\n"
    "    vec3 cy = vec3(0.35, 0.85, 1.0), vi = vec3(0.62, 0.42, 1.0), pk = vec3(1.0, 0.45, 0.82);\n"
    "    vec3 ir = tt < 0.5 ? mix(cy, vi, tt * 2.0) : mix(vi, pk, (tt - 0.5) * 2.0); ir = mix(ir, vec3(0.80, 0.75, 1.0), 0.8 * tailw);\n"
    "    float curv = clamp(length(fwidth(g0)) * 7.0, 0.0, 1.0);\n"
    "    c = mix(c, ir, clamp(w * (0.50 + 0.45 * curv) * 0.8 * k, 0.0, 1.0));\n"
    "    c = mix(c, vec3(0.95, 0.92, 1.0), clamp(0.35 * clamp(1.5 - depth, 0.0, 1.0) * k, 0.0, 1.0));\n"
    "    float tl = max(dot(g0, normalize(vec2(-0.7, -0.7))), 0.0), brc = max(dot(g0, normalize(vec2(0.7, 0.7))), 0.0);\n"
    "    c = mix(c, vec3(1.0), clamp(0.55 * (1.0 - 0.5 * tailw) * tl * tl * exp(-depth / 6.5) * k, 0.0, 1.0));\n"
    "    c = mix(c, vec3(0.80, 0.90, 1.0), clamp(0.45 * (1.0 - tailw) * pow(brc, 1.5) * exp(-max(depth - 1.5, 0.0) / 7.0) * k, 0.0, 1.0));\n"
    "  }\n"
    "  f = vec4(c * cov + vec3(0.27, 0.12, 0.55) * sa, cov + sa); }\n";

/* Dither final: ruído triangular (2 amostras de ruído de gradiente intercalado) de +-1 LSB somado antes de gravar em 8 bits. */
static const char *FS_DITH =
    "#version 330 core\n"
    "uniform sampler2D u_tex; uniform vec2 u_vp; out vec4 f;\n"
    "float ign(vec2 p){ return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }\n"
    "void main(){ vec2 px = gl_FragCoord.xy; vec4 t = texture(u_tex, px / u_vp);\n"
    "  float d = (ign(px) + ign(px + vec2(47.0, 17.0)) - 1.0) / 255.0;\n"
    "  f = vec4(t.rgb + vec3(d), t.a); }\n";

static GLuint make_prog(const char *fs, const char *name) {
    GLuint v = glCreateShader(GL_VERTEX_SHADER), s = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v, 1, &VS, NULL); glCompileShader(v);
    glShaderSource(s, 1, &fs, NULL); glCompileShader(s);
    GLint okv = 0, oks = 0; glGetShaderiv(v, GL_COMPILE_STATUS, &okv); glGetShaderiv(s, GL_COMPILE_STATUS, &oks);
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, s); glLinkProgram(p);
    GLint okp = 0; glGetProgramiv(p, GL_LINK_STATUS, &okp);
    glDeleteShader(v); glDeleteShader(s);
    if (!okv || !oks || !okp) { fprintf(stderr, "glass: shader %s falhou\n", name); glDeleteProgram(p); return 0; }
    return p;
}

static GLuint make_tex(int w, int h) {
    GLuint t = 0; glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, NULL);
    return t;
}

static bool ensure_hdr(int w, int h) {
    if (G.hdr_tex && G.hdr_w == w && G.hdr_h == h) return true;
    if (G.hdr_tex) glDeleteTextures(1, &G.hdr_tex);
    G.hdr_tex = make_tex(w, h); G.hdr_w = w; G.hdr_h = h;
    glBindFramebuffer(GL_FRAMEBUFFER, G.hdr_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, G.hdr_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { G.p_dith = 0; return false; }
    return true;
}

static void free_targets(void) {
    if (G.src) glDeleteTextures(1, &G.src);
    G.src = 0;
    for (int i = 0; i <= MAX_LEVELS; i++) { if (G.lvl[i]) glDeleteTextures(1, &G.lvl[i]); G.lvl[i] = 0; }
}

static void ensure_targets(int w, int h) {
    if (G.src && G.w == w && G.h == h) return;
    free_targets();
    G.w = w; G.h = h;
    G.src = make_tex(w, h);
    for (int i = 1; i <= G.levels; i++) {
        G.lw[i] = (w >> i) > 1 ? (w >> i) : 1; G.lh[i] = (h >> i) > 1 ? (h >> i) : 1;
        G.lvl[i] = make_tex(G.lw[i], G.lh[i]);
    }
}

static void fs_pass(GLuint prog, GLuint src_tex, int sw, int sh, GLuint dst_tex, int dw, int dh, float off, bool down) {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex, 0);
    glViewport(0, 0, dw, dh);
    glUseProgram(prog);
    glUniform1i(down ? G.d_tex : G.u_tex, 0);
    glUniform2f(down ? G.d_out : G.u_out, (float)dw, (float)dh);
    glUniform2f(down ? G.d_texel : G.u_texel, 1.0f / (float)sw, 1.0f / (float)sh);
    glUniform1f(down ? G.d_off : G.u_off, off);
    glUniform2f(down ? G.d_vp : G.u_vp, (float)dw, (float)dh);
    glUniform4f(down ? G.d_rect : G.u_rect, 0, 0, (float)dw, (float)dh);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Copia o framebuffer padrão e o desfoca; o resultado fica em lvl[1] (metade da resolução). */
static void capture_and_blur(int w, int h, GLuint target_fb) {
    ensure_targets(w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, target_fb);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, G.src);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    glBindVertexArray(G.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, G.fbo);
    fs_pass(G.p_down, G.src, w, h, G.lvl[1], G.lw[1], G.lh[1], G.offset * g_th.blur_offset, true);
    for (int i = 2; i <= G.levels; i++)
        fs_pass(G.p_down, G.lvl[i - 1], G.lw[i - 1], G.lh[i - 1], G.lvl[i], G.lw[i], G.lh[i], G.offset * g_th.blur_offset, true);
    for (int i = G.levels; i >= 2; i--)
        fs_pass(G.p_up, G.lvl[i], G.lw[i], G.lh[i], G.lvl[i - 1], G.lw[i - 1], G.lh[i - 1], G.offset * g_th.blur_offset, false);
}

typedef struct { int idx; float cx, cy, hw, hh, r[4]; int layer; float tint_a; int cmd; int grp; float rc, rs, alpha; bool card, bubble, skip; float m1[4], m2[4]; } glass_quad;

static int glass_layer(cui_color c) {
    if (c.r == B_GLASS.r && c.g == B_GLASS.g && c.b == B_GLASS.b && c.a == B_GLASS.a) return 0;
    if (c.r == B_GLASS_SOFT.r && c.g == B_GLASS_SOFT.g && c.b == B_GLASS_SOFT.b && c.a == B_GLASS_SOFT.a) return 0;
    if (c.r == B_GLASS_CARD.r && c.g == B_GLASS_CARD.g && c.b == B_GLASS_CARD.b && c.a == B_GLASS_CARD.a) return 0;
    if (c.r == B_GLASS_BUBBLE.r && c.g == B_GLASS_BUBBLE.g && c.b == B_GLASS_BUBBLE.b && c.a == B_GLASS_BUBBLE.a) return 0;
    if (c.r == B_GLASS_STRONG.r && c.g == B_GLASS_STRONG.g && c.b == B_GLASS_STRONG.b && c.a == B_GLASS_STRONG.a) return 0;
    if (c.r == B_GLASS_POPUP.r && c.g == B_GLASS_POPUP.g && c.b == B_GLASS_POPUP.b && c.a == B_GLASS_POPUP.a) return 1;
    return -1;
}

static void draw_glass(const glass_quad *q, int W, int H, const cui_rect *clip, GLuint target_fb) {
    glBindFramebuffer(GL_FRAMEBUFFER, target_fb);
    glViewport(0, 0, W, H);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_SCISSOR_TEST);
    int x0 = (int)(clip->x), y0 = (int)(clip->y), x1 = (int)(clip->x + clip->w + 0.999f), y1 = (int)(clip->y + clip->h + 0.999f);
    glScissor(x0, H - y1, x1 - x0, y1 - y0);
    glUseProgram(G.p_comp);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, G.lvl[1]);
    glUniform1i(G.c_tex, 0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, G.src); glUniform1i(G.c_src, 1);   /* fundo nítido (bolha de status) */
    glActiveTexture(GL_TEXTURE0);
    glUniform2f(G.c_vp, (float)W, (float)H);
    float ext = q->grp >= 0 ? sqrtf(q->hw * q->hw + q->hh * q->hh) + 2 : 0;   /* girado: caixa envolvente maior */
    if (ext > 0) glUniform4f(G.c_rect, q->cx - ext, q->cy - ext, ext * 2, ext * 2);
    else glUniform4f(G.c_rect, q->cx - q->hw - 8, q->cy - q->hh - 8, q->hw * 2 + 16, q->hh * 2 + 16);
    glUniform4f(G.c_m1, q->m1[0], q->m1[1], q->m1[2], q->m1[3]); glUniform4f(G.c_m2, q->m2[0], q->m2[1], q->m2[2], q->m2[3]);
    {   /* fase fixa por balão (derivada da largura = texto), respiração lenta só no Normal */
        static int nobreath = -1; if (nobreath < 0) nobreath = getenv("BRIKS_NO_BREATH") != NULL;
        float tsec = (float)(GetTickCount64() % 600000ULL) / 1000.0f;
        glUniform4f(G.c_m3, q->hw * 1.713f, tsec * 1.4f, (g_perf == PERF_NORMAL && !nobreath) ? 1.0f : 0.0f, 0);
    }
    if (q->m1[3] > 0.5f) {   /* balão unido às bolinhas: o retângulo cobre a união */
        float x0 = q->cx - q->hw, y0 = q->cy - q->hh, x1 = q->cx + q->hw, y1 = q->cy + q->hh;
        for (int k = 0; k < 2; k++) { const float *m = k ? q->m2 : q->m1; if (m[3] < 0.5f) continue;
            if (m[0] - m[2] < x0) x0 = m[0] - m[2]; if (m[1] - m[2] < y0) y0 = m[1] - m[2]; if (m[0] + m[2] > x1) x1 = m[0] + m[2]; if (m[1] + m[2] > y1) y1 = m[1] + m[2]; }
        glUniform4f(G.c_rect, x0 - 8, y0 - 8, x1 - x0 + 16, y1 - y0 + 16);
    }
    glUniform2f(G.c_rot, q->grp >= 0 ? q->rc : 1.0f, q->grp >= 0 ? q->rs : 0.0f);
    glUniform1f(G.c_alpha, q->grp >= 0 ? q->alpha : 1.0f);
    glUniform4f(G.c_box, q->cx, q->cy, q->hw, q->hh);
    glUniform4f(G.c_rad, q->r[0], q->r[1], q->r[2], q->r[3]);
    glUniform4f(G.c_a1, g_th.c1.r / 255.0f, g_th.c1.g / 255.0f, g_th.c1.b / 255.0f, 1); glUniform4f(G.c_a2, g_th.c2.r / 255.0f, g_th.c2.g / 255.0f, g_th.c2.b / 255.0f, 1); glUniform4f(G.c_a3, g_th.c3.r / 255.0f, g_th.c3.g / 255.0f, g_th.c3.b / 255.0f, 1);
    glUniform1f(G.c_sat, g_th.saturation * (q->card ? g_th.card_sat : 1.0f));
    {   /* reflexo dos cartões: compensa a tinta que o CUI desenha por cima; Simplificado = metade, sem tinta extra */
        float inv2 = 1.0f - q->tint_a; if (inv2 < 0.2f) inv2 = 0.2f;
        float k = !q->card ? 0.0f : g_perf == PERF_SIMPLE ? 0.5f : 1.0f;
        if (q->bubble) {   /* bolha de status: Normal = refração + dispersão + aro + gota; Simplificado = aro e gota estáticos, sem refração */
            glUniform4f(G.c_card, 1.0f / inv2, 0, 0, g_perf == PERF_SIMPLE ? 1.0f : 2.0f);
            glUniform4f(G.c_bub, g_th.bubble_refract, g_th.bubble_disp, g_th.bubble_spec, g_th.bubble_tint);
        } else
        glUniform4f(G.c_card, k * g_th.sheen / inv2, k * g_th.sheen_line / inv2, g_perf == PERF_SIMPLE || !q->card ? 0.0f : g_th.card_fill / inv2, 0.0f);
    }
    /* o CSS soma o brilho sobre a camada translúcida; aqui ele vai por baixo, então compensa a opacidade */
    float inv = 1.0f - q->tint_a; if (inv < 0.2f) inv = 0.2f;
    glUniform1f(G.c_hl, g_th.highlight / inv);
    glBindVertexArray(G.vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(0);
    glBindVertexArray(0);
}

/* ---- pilha levantada (prévia de arraste): cartões girados ----
 * O dock desenha cada cartão em coordenadas sem giro, entre marcadores (retângulos 2x2 com cor mágica). Aqui os vértices entre
 * marcadores são girados/escalados/esmaecidos em torno do pivô do grupo; os vidros dos cartões (cor B_GLASS_POPUP, camada 1) ganham o
 * giro no shader e o degradê de 145° de branco 17% -> 5% nos vértices. */
typedef struct { bool on; float px, py, rc, rs, scale, alpha; } pile_xf;
static pile_xf PXF[PILE_GROUPS];
void briks_pile_set(int g, float px, float py, float ang_deg, float scale, float alpha) {
    if (g < 0 || g >= PILE_GROUPS) return;
    float a = ang_deg * 3.14159265f / 180.0f;
    PXF[g] = (pile_xf){ true, px, py, cosf(a), sinf(a), scale, alpha };
}
void briks_pile_clear(void) { memset(PXF, 0, sizeof PXF); }

static bool pile_marker(const cui_vertex *v, int *grp) {
    if (v->mode != CUI_SHADE_RRECT || v->color.r != 0x5A || v->color.g != 0x17 || v->color.b != 0xC1) return false;
    *grp = v->color.a == 0xFF ? -1 : (int)v->color.a - 1;
    return true;
}
static uint8_t scale_a(uint8_t a, float k) { float f = a * k; return (uint8_t)(f < 0 ? 0 : f > 255 ? 255 : f + 0.5f); }

/* devolve dd (sem marcadores) ou uma cópia transformada; tag[i] = grupo do vértice i (ou -1) */
static const cui_draw_data *pile_transform(const cui_draw_data *dd, cui_draw_data *out, cui_vertex **vcopy, int **tag) {
    *vcopy = NULL; *tag = NULL;
    bool any = false;
    for (int i = 0; i + 2 < dd->index_count && !any; i += 3) { int g; any = pile_marker(&dd->vertices[dd->indices[i]], &g); }
    if (!any) return dd;
    cui_vertex *vv = malloc(sizeof *vv * (size_t)dd->vertex_count); int *tg = malloc(sizeof *tg * (size_t)dd->vertex_count);
    memcpy(vv, dd->vertices, sizeof *vv * (size_t)dd->vertex_count);
    for (int i = 0; i < dd->vertex_count; i++) tg[i] = -1;
    int grp = -1;
    for (int i = 0; i + 2 < dd->index_count; i += 3) {
        int g;
        if (pile_marker(&dd->vertices[dd->indices[i]], &g)) {
            grp = g;
            for (int k = 0; k < 3; k++) { cui_vertex *v = &vv[dd->indices[i + k]]; v->color.a = 0; v->border_color.a = 0; }
            continue;
        }
        if (grp < 0 || grp >= PILE_GROUPS || !PXF[grp].on) continue;
        const pile_xf *x = &PXF[grp];
        for (int k = 0; k < 3; k++) {
            uint32_t id = dd->indices[i + k];
            if (tg[id] >= 0) continue;
            tg[id] = grp;
            cui_vertex *v = &vv[id];
            float dx = (v->x - x->px) * x->scale, dy = (v->y - x->py) * x->scale;
            v->x = x->px + dx * x->rc - dy * x->rs; v->y = x->py + dx * x->rs + dy * x->rc;
            if (v->mode == CUI_SHADE_RRECT || v->mode == CUI_SHADE_SHADOW) {
                v->lx *= x->scale; v->ly *= x->scale; v->hw *= x->scale; v->hh *= x->scale;
                v->r_tl *= x->scale; v->r_tr *= x->scale; v->r_br *= x->scale; v->r_bl *= x->scale; v->border *= x->scale;
            }
            if (glass_layer(v->color) < 0) v->color.a = scale_a(v->color.a, x->alpha);   /* vidro: a cor exata é a chave de detecção; o alfa entra no degradê */
            v->border_color.a = scale_a(v->border_color.a, x->alpha);
        }
    }
    *out = *dd; out->vertices = vv; *vcopy = vv; *tag = tg;
    return out;
}

static void render_hook(cui_app *app, const cui_draw_data *dd_in, cui_gl_renderer *gl, void *user) {
    (void)app; (void)user;
    cui_draw_data dd_copy; cui_vertex *vcopy; int *tag;
    const cui_draw_data *dd = pile_transform(dd_in, &dd_copy, &vcopy, &tag);
    LARGE_INTEGER t0, t1, fq;
    static int seen; seen++;
    const bool timing = G.log || seen <= 10;   /* os primeiros quadros alimentam a detecção de hardware fraco */
    const bool nog = g_perf >= PERF_MIN;       /* Mínimo: sem desfoque de vidro nem dither (fundo translúcido) */
    if (timing) { QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&fq); }
    int W = dd->width_px, H = dd->height_px;
    GLint fb = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
    GLuint tgt = (GLuint)fb;
    bool hdr = !nog && G.p_dith && ensure_hdr(W, H);
    if (hdr) tgt = G.hdr_fbo;
    glBindFramebuffer(GL_FRAMEBUFFER, tgt);

    /* 1. acha os painéis de vidro: grupos de triângulos RRECT com a cor de vidro, 2 triângulos por painel */
    int nq = 0, qcap = 0; glass_quad *qs = NULL;
    for (int c = 0; c < dd->command_count; c++) {
        const cui_draw_cmd *cm = &dd->commands[c];
        int run = 0;
        for (uint32_t i = cm->index_offset; i + 2 < cm->index_offset + cm->index_count; i += 3) {
            const cui_vertex *v = &dd->vertices[dd->indices[i]];
            const cui_vertex *ov = &dd_in->vertices[dd->indices[i]];   /* cor ORIGINAL: o degradê da pilha reescreve a cópia */
            int layer = (ov->mode == CUI_SHADE_RRECT) ? glass_layer(ov->color) : -1;
            if (layer < 0) { run = 0; continue; }
            int pg = tag ? tag[dd->indices[i]] : -1;
            if (pg >= 0) {   /* vidro de cartão da pilha: degradê de 145° (branco 17% -> 5%) pelos vértices, já com a opacidade do grupo */
                for (int k = 0; k < 3; k++) {
                    cui_vertex *w = &vcopy[dd->indices[i + k]];
                    float t = 0.5f + 0.5f * (0.5736f * (w->hw > 0 ? w->lx / w->hw : 0) + 0.8192f * (w->hh > 0 ? w->ly / w->hh : 0));
                    t = t < 0 ? 0 : t > 1 ? 1 : t;
                    w->color = (cui_color){ 255, 255, 255, scale_a((uint8_t)255, (0.17f + (0.05f - 0.17f) * t) * PXF[pg].alpha) };
                }
            }
            if (run % 2 == 0) {
                if (nq == qcap) { qcap = qcap ? qcap * 2 : 16; qs = realloc(qs, sizeof *qs * (size_t)qcap); }
                glass_quad *q = &qs[nq++]; memset(q, 0, sizeof *q);
                q->grp = pg; q->alpha = pg >= 0 ? PXF[pg].alpha : 1; q->rc = pg >= 0 ? PXF[pg].rc : 1; q->rs = pg >= 0 ? PXF[pg].rs : 0;
                q->idx = (int)i; q->hw = v->hw; q->hh = v->hh;
                if (pg >= 0) { q->cx = v->x - (q->rc * v->lx - q->rs * v->ly); q->cy = v->y - (q->rs * v->lx + q->rc * v->ly); }
                else { q->cx = v->x - v->lx; q->cy = v->y - v->ly; }
                q->r[0] = v->r_tl; q->r[1] = v->r_tr; q->r[2] = v->r_br; q->r[3] = v->r_bl;
                q->layer = layer; q->tint_a = pg >= 0 ? 0.11f : v->color.a / 255.0f; q->cmd = c;
                q->bubble = pg < 0 && ov->color.r == B_GLASS_BUBBLE.r && ov->color.g == B_GLASS_BUBBLE.g && ov->color.b == B_GLASS_BUBBLE.b && ov->color.a == B_GLASS_BUBBLE.a;
                q->card = pg < 0 && ov->color.r == B_GLASS_CARD.r && ov->color.g == B_GLASS_CARD.g && ov->color.b == B_GLASS_CARD.b && ov->color.a == B_GLASS_CARD.a;
            }
            run++;
        }
    }
    /* metaballs: cada balão de status absorve as duas bolinhas da cauda (as menores, logo abaixo dele) */
    for (int i = 0; i < nq; i++) {
        glass_quad *bq = &qs[i];
        if (!bq->bubble || bq->hw <= 9.0f) continue;
        int d1 = -1, d2 = -1;
        for (int j = 0; j < nq; j++) {
            const glass_quad *dq = &qs[j];
            if (j == i || !dq->bubble || dq->hw > 9.0f || dq->skip) continue;
            if (dq->cy < bq->cy + bq->hh - 4 || dq->cy > bq->cy + bq->hh + 40 || dq->cx < bq->cx - bq->hw - 12 || dq->cx > bq->cx + bq->hw) continue;
            if (d1 < 0 || dq->hw > qs[d1].hw) { d2 = d1; d1 = j; } else if (d2 < 0 || dq->hw > qs[d2].hw) d2 = j;
        }
        if (d1 >= 0) { bq->m1[0] = qs[d1].cx; bq->m1[1] = qs[d1].cy; bq->m1[2] = qs[d1].hw; bq->m1[3] = 1; qs[d1].skip = true; }
        if (d2 >= 0) { bq->m2[0] = qs[d2].cx; bq->m2[1] = qs[d2].cy; bq->m2[2] = qs[d2].hw; bq->m2[3] = 1; qs[d2].skip = true; }
    }
    if (!nq || nog) { free(qs); cui_gl_render(gl, dd, true); goto done; }   /* (já ligado ao alvo) */

    /* 2. desenha em segmentos; em cada painel, captura/desfoca (se a camada mudou) e pinta o vidro */
    cui_draw_cmd *out = malloc(sizeof *out * (size_t)(dd->command_count + nq * 2 + 2));
    int nout = 0, qi = 0, cur_layer = -1; bool first = true;
    for (int c = 0; c < dd->command_count; c++) {
        const cui_draw_cmd *cm = &dd->commands[c];
        uint32_t cur = cm->index_offset, end = cm->index_offset + cm->index_count;
        while (qi < nq && qs[qi].cmd == c) {
            const glass_quad *q = &qs[qi];
            if ((uint32_t)q->idx > cur) { out[nout] = *cm; out[nout].index_offset = cur; out[nout].index_count = (uint32_t)q->idx - cur; nout++; }
            cui_draw_data part = *dd; part.commands = out; part.command_count = nout;
            cui_gl_render(gl, &part, first); first = false; nout = 0;
            if (q->layer != cur_layer) { capture_and_blur(W, H, tgt); cur_layer = q->layer; }
            if (!q->skip && (q->grp < 0 || q->alpha > 0.004f)) draw_glass(q, W, H, &cm->clip, tgt);
            cur = (uint32_t)q->idx;
            qi++;
        }
        if (cur < end) { out[nout] = *cm; out[nout].index_offset = cur; out[nout].index_count = end - cur; nout++; }
    }
    { cui_draw_data part = *dd; part.commands = out; part.command_count = nout; cui_gl_render(gl, &part, first); }
    free(out); free(qs);
done:
    if (hdr) {   /* passada final: copia o quadro float para o framebuffer real somando o dither */
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fb);
        glViewport(0, 0, W, H);
        glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST);
        glUseProgram(G.p_dith);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, G.hdr_tex);
        glUniform1i(G.t_tex, 0); glUniform2f(G.t_vp, (float)W, (float)H); glUniform4f(G.t_rect, 0, 0, (float)W, (float)H);
        glBindVertexArray(G.vao); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0); glUseProgram(0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fb);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    free(vcopy); free(tag);
    if (timing) {
        glFinish();
        QueryPerformanceCounter(&t1);
        double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
        G.ms_sum += ms; G.frames++;
        briks_perf_note_frame(ms, seen);
        if (G.log) fprintf(stderr, "glass: quadro %d: %.2f ms (media %.2f ms), paineis=%d\n", G.frames, ms, G.ms_sum / G.frames, nq);
    }
}

bool briks_glass_init(cui_app *app) {
    const char *e = getenv("BRIKS_GLASS");
    if (e && e[0] == '0') return false;
    if (cui_app_is_software(app)) return false;
    if (!load_gl()) return false;
    G.levels = 4; G.offset = 1.0f;
    if ((e = getenv("BRIKS_GLASS_LEVELS"))) { int n = atoi(e); if (n >= 2 && n <= MAX_LEVELS) G.levels = n; }
    if ((e = getenv("BRIKS_GLASS_OFFSET"))) { float o = (float)atof(e); if (o > 0.1f && o < 8.0f) G.offset = o; }
    G.log = getenv("BRIKS_GLASS_LOG") != NULL;
    if (!(e = getenv("BRIKS_DITHER")) || e[0] != '0') G.p_dith = make_prog(FS_DITH, "dither");
    G.p_down = make_prog(FS_DOWN, "down"); G.p_up = make_prog(FS_UP, "up"); G.p_comp = make_prog(FS_COMP, "comp");
    if (!G.p_down || !G.p_up || !G.p_comp) return false;
#define U(p, n) glGetUniformLocation(p, n)
    G.d_tex = U(G.p_down, "u_tex"); G.d_out = U(G.p_down, "u_out"); G.d_texel = U(G.p_down, "u_texel"); G.d_off = U(G.p_down, "u_off");
    G.d_vp = U(G.p_down, "u_vp"); G.d_rect = U(G.p_down, "u_rect");
    G.u_tex = U(G.p_up, "u_tex"); G.u_out = U(G.p_up, "u_out"); G.u_texel = U(G.p_up, "u_texel"); G.u_off = U(G.p_up, "u_off");
    G.u_vp = U(G.p_up, "u_vp"); G.u_rect = U(G.p_up, "u_rect");
    G.c_tex = U(G.p_comp, "u_tex"); G.c_vp = U(G.p_comp, "u_vp"); G.c_rect = U(G.p_comp, "u_rect"); G.c_box = U(G.p_comp, "u_box");
    G.c_rot = U(G.p_comp, "u_rot"); G.c_alpha = U(G.p_comp, "u_alpha"); G.c_rad = U(G.p_comp, "u_rad"); G.c_sat = U(G.p_comp, "u_sat"); G.c_hl = U(G.p_comp, "u_hl"); G.c_card = U(G.p_comp, "u_card"); G.c_bub = U(G.p_comp, "u_bub"); G.c_a1 = U(G.p_comp, "u_a1"); G.c_a2 = U(G.p_comp, "u_a2"); G.c_a3 = U(G.p_comp, "u_a3"); G.c_src = U(G.p_comp, "u_src"); G.c_m1 = U(G.p_comp, "u_m1"); G.c_m2 = U(G.p_comp, "u_m2"); G.c_m3 = U(G.p_comp, "u_m3");
#undef U
    if (G.p_dith) { G.t_tex = glGetUniformLocation(G.p_dith, "u_tex"); G.t_vp = glGetUniformLocation(G.p_dith, "u_vp"); G.t_rect = glGetUniformLocation(G.p_dith, "u_rect"); glGenFramebuffers(1, &G.hdr_fbo); }
    glGenFramebuffers(1, &G.fbo);
    glGenVertexArrays(1, &G.vao);
    G.ok = true;
    briks_perf_note_renderer((const char *)glGetString(0x1F01));   /* GL_RENDERER */
    cui_app_set_render_override(app, render_hook, NULL);
    return true;
}

void briks_glass_shutdown(void) { G.ok = false; }
