/*
 * NV2A vertex program -> HLSL.
 *
 * One program becomes one vs_5_0 shader that does what nv2a_vsh_run does for a
 * vertex, so the field layout and every rule here mirror nv2a_vsh_interp.c. The
 * outputs then go through the same maths as the executor's screen-space
 * vertices: oPos holds surface pixels with the clip-space w beside them.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define HLSL_MAX (512 * 1024)

static char *s_o;
static size_t s_n;

static void emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    s_n += vsnprintf(s_o + s_n, HLSL_MAX - s_n, fmt, ap);
    va_end(ap);
}

static uint32_t field(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

static void src(const uint32_t *ins, int which, const char *name)
{
    static const char c[] = "xyzw";
    uint32_t neg, swz, mux, reg;

    if (which == 0) {
        neg = field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        mux = field(ins, 2, 26, 2); reg = field(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        mux = field(ins, 2, 11, 2); reg = field(ins, 2, 13, 4);
    } else {
        neg = field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        mux = field(ins, 3, 28, 2);
        reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
    }
    emit("float4 %s = %s", name, neg ? "-" : "");
    if (mux == 1)
        emit("r%u", reg <= 12 ? reg : 0);
    else if (mux == 2)
        emit("v%u", field(ins, 1, 9, 4));
    else if (field(ins, 3, 1, 1))
        emit("c[ci(%u + a0)]", field(ins, 1, 13, 8));
    else
        emit("c[%u]", field(ins, 1, 13, 8) < 192 ? field(ins, 1, 13, 8) : 0);
    emit(".%c%c%c%c;\n", c[(swz >> 6) & 3], c[(swz >> 4) & 3], c[(swz >> 2) & 3], c[swz & 3]);
}

static void write_to(const char *dst, const char *from, uint32_t mask)
{
    char m[5];
    int n = 0;

    if (mask & 8) m[n++] = 'x';
    if (mask & 4) m[n++] = 'y';
    if (mask & 2) m[n++] = 'z';
    if (mask & 1) m[n++] = 'w';
    m[n] = 0;
    emit("%s.%s = %s.%s;\n", dst, m, from, m);
}

char *gpu_vsh_hlsl(const uint32_t (*prog)[4], uint32_t count, uint32_t inputs)
{
    static const char *const mac_ops[16] = {
        "0", "a", "a * b", "a + cc", "a * b + cc",
        "dot(a.xyz, b.xyz).xxxx", "(dot(a.xyz, b.xyz) + b.w).xxxx", "dot(a, b).xxxx",
        "float4(1, a.y * b.y, a.z, b.w)", "(a < b ? a : b)", "(a >= b ? a : b)",
        "float4(a < b)", "float4(a >= b)", "a", "a", "a"
    };
    static const char *const ilu_ops[8] = {
        "0", "cc", "(1 / cc.x).xxxx", "rcc(cc.x).xxxx", "(1 / sqrt(abs(cc.x))).xxxx",
        "iexp(cc.x)", "ilog(cc.x)", "ilit(cc)"
    };
    uint32_t i, a;

    s_o = (char *)malloc(HLSL_MAX);
    s_n = 0;
    if (!s_o)
        return NULL;
    emit("cbuffer V : register(b0) { float4 vp, aa, fg; float4 imm[16]; float4 c[192]; };\n"
         "int ci(int i) { return i < 0 || i >= 192 ? 0 : i; }\n"
         "float rcc(float x) { float f = 1 / x, g = clamp(abs(f), 5.42101e-20, 1.884467e19); return f < 0 ? -g : g; }\n"
         "float4 iexp(float x) { float l = floor(x); return float4(exp2(l), x - l, exp2(x), 1); }\n"
         "float4 ilog(float x) {\n"
         "  float ax = abs(x), e = floor(log2(ax));\n"
         "  return float4(e, ax / exp2(e), log2(ax), 1);\n"
         "}\n"
         "float4 ilit(float4 s) {\n"
         "  float nl = max(s.x, 0), nh = max(s.y, 0), p = clamp(s.w, -127.9961, 127.9961);\n"
         "  return float4(1, nl, s.x > 0 ? pow(nh, p) : 0, 1);\n"
         "}\n"
         "float fogf(float d) {\n"
         "  float f;\n"
         "  if (fg.w < 0.5) return 1;\n"
         "  if (fg.z == 1) f = fg.x + exp2(d * fg.y * 16) - 1.5;\n"
         "  else if (fg.z == 2) f = fg.x + exp2(-d * d * fg.y * fg.y * 32) - 1.5;\n"
         "  else f = fg.x + d * fg.y - 1;\n"
         "  if (aa.w > 0.5) f = abs(f);\n"
         "  if (isnan(f)) f = 1;\n"
         "  return saturate(f);\n"
         "}\n");
    emit("struct I {\n");
    for (a = 0; a < 16; a++)
        if (inputs & (1u << a))
            emit("  float4 v%u : V%u;\n", a, a);
    emit("  uint unused_ : SV_VertexID;\n};\n");
    emit("struct O { float4 p : SV_Position; float4 d : COLOR0; float4 s : COLOR1; float f : FOGC;\n"
         "  float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
         "  float cd : SV_ClipDistance0; };\n");
    emit("O main(I i) {\n");
    for (a = 0; a < 16; a++)
        emit("float4 v%u = %s%u%s;\n", a, inputs & (1u << a) ? "i.v" : "imm[", a, inputs & (1u << a) ? "" : "]");
    for (a = 0; a < 13; a++)
        emit("float4 r%u = 0;\n", a);
    emit("float4 od0 = float4(0, 0, 0, 1), od1 = od0, ofog = 0, ot0 = od0, ot1 = od0, ot2 = od0, ot3 = od0;\n"
         "int a0 = 0;\n");

    for (i = 0; i < count; i++) {
        const uint32_t *ins = prog[i];
        uint32_t mac = field(ins, 1, 21, 4), ilu = field(ins, 1, 25, 3);
        uint32_t mac_mask = field(ins, 3, 24, 4), ilu_mask = field(ins, 3, 16, 4);
        uint32_t tdst = field(ins, 3, 20, 4);
        uint32_t omask = field(ins, 3, 12, 4), oaddr = field(ins, 3, 3, 8);

        emit("{\n");
        src(ins, 0, "a");
        src(ins, 1, "b");
        src(ins, 2, "cc");
        emit("float4 mres = %s, ires = %s;\n", mac_ops[mac], ilu_ops[ilu]);
        if (mac == 13)
            emit("a0 = (int)floor(a.x + 0.001);\n");
        if (mac && mac != 13 && mac_mask && tdst <= 12) {
            char d[8];
            sprintf(d, "r%u", tdst);
            write_to(d, "mres", mac_mask);
        }
        if (ilu && ilu_mask) {
            /* Paired with any MAC op the ILU can only write R1. */
            uint32_t it = mac ? 1u : tdst;
            if (it <= 12) {
                char d[8];
                sprintf(d, "r%u", it);
                write_to(d, "ires", ilu_mask);
            }
        }
        if (omask && field(ins, 3, 11, 1)) {
            const char *f = field(ins, 3, 2, 1) ? "ires" : "mres";
            static const char *const out[13] = {
                "r12", NULL, NULL, "od0", "od1", "ofog", NULL, NULL, NULL, "ot0", "ot1", "ot2", "ot3"
            };
            if (oaddr < 13 && out[oaddr])
                write_to(out[oaddr], f, omask);
        }
        emit("}\n");
    }

    emit("O o;\n"
         "float w = r12.w == 0 ? 1 : r12.w, z = r12.z * vp.z;\n"
         "o.p = float4((r12.x * aa.x * vp.x - 1) * w, (1 - r12.y * aa.y * vp.y) * w, z * w, w);\n"
         "o.cd = (r12.z - vp.w) * w;\n"
         "o.d = saturate(od0); o.s = saturate(od1); o.f = fogf(ofog.x);\n"
         "o.t0 = ot0; o.t1 = ot1; o.t2 = ot2; o.t3 = ot3;\n"
         "return o;\n}\n");
    return s_o;
}
