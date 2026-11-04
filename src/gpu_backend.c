/* D3D11 render back end: everything the NV2A pushbuffer asks for, drawn on the host GPU.
 *
 * The executor decodes the pushbuffer and hands over transformed triangles (nv2a_backend.h).
 * Each guest colour surface becomes a render target, each zeta surface a depth buffer, and each
 * register-combiner program a generated pixel shader. Guest memory holds no pixels in this mode,
 * so a texture that points at a render target samples that target.
 *
 * Everything runs on the NV2A poll thread. Window size, fullscreen, render scale and vsync come
 * from spyro.ini (settings.h); RECOMP_GPU_SCALE still overrides the scale.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <mmsystem.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/nv2a_backend.h"
#include "settings.h"
#include "prompts.h"

extern void xbox_FileWritesSettle(DWORD quiet_ms, DWORD max_ms);
extern int mkb_enabled(void);
extern int mkb_last_device_keyboard(void);
extern int mkb_captured(void);

#define VB_BYTES   (64u * 1024 * 1024)
#define MAX_TARGETS 64
#define TEX_BUCKETS 4096
#define MAX_STATES  512

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* ---- device ---------------------------------------------------------- */
static HWND s_hwnd;
static ID3D11Device *s_dev;
static ID3D11DeviceContext *s_ctx;
static IDXGISwapChain1 *s_swap;
static ID3D11RenderTargetView *s_back_rtv;
static int s_tearing, s_resize, s_fullscreen, s_bound;
static RECT s_windowed;
static uint32_t s_scale = 2;
static int s_widescreen;    /* the game draws 16:9 squeezed into its 4:3 buffers */
static uint32_t s_frame;

static ID3D11Buffer *s_vb, *s_vs_cb, *s_ps_cb;
static uint32_t s_vb_pos;
static ID3D11VertexShader *s_vs, *s_blit_vs;
static ID3D11PixelShader *s_blit_ps;
static ID3D11InputLayout *s_layout;
static ID3D11RasterizerState *s_raster;
static ID3D11SamplerState *s_blit_sampler;
static ID3D11ShaderResourceView *s_white, *s_white_cube;

/* ---- targets --------------------------------------------------------- */
typedef struct {
    uint32_t va, pitch, bpp;
    uint32_t lw, lh;                    /* surface pixels; the texture is scaled */
    uint32_t frame;
    uint32_t dbg_draws, dbg_tris;
    ID3D11Texture2D *tex, *copy;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv, *copy_srv;
} Target;

typedef struct {
    uint32_t va, lw, lh;
    uint32_t frame;                     /* last use, for eviction */
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
} Depth;

static Target s_rts[MAX_TARGETS];
static Depth s_dss[MAX_TARGETS];
static int s_nrt, s_nds;
static Target *s_cur_rt, *s_present;
static Depth *s_cur_ds;
static uint32_t s_present_area;
static ID3D11ShaderResourceView *s_srv_bound[4];

/* ---- traces ---------------------------------------------------------- */
static struct { uint32_t draws, tris, uploads, rehash, shaders, bytes; } s_st;

static void fatal(const char *what, HRESULT hr)
{
    fprintf(stderr, "[gpu] %s failed: 0x%08lX\n", what, (unsigned long)hr);
    ExitProcess(1);
}
#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) fatal(#x, hr_); } while (0)

/* ---- shaders --------------------------------------------------------- */
static ID3DBlob *compile(const char *src, size_t len, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, len, NULL, NULL, NULL, "main", target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[gpu] shader failed:\n%s\n--\n%s\n", src,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        ExitProcess(1);
    }
    if (err)
        ID3D10Blob_Release(err);
    return code;
}

static const char VS_SRC[] =
    "cbuffer V : register(b0) { float4 vp; };\n"
    "struct I { float4 p : POSITION; float4 d : COLOR0; float4 s : COLOR1; float f : FOGC;\n"
    "           float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
    "struct O { float4 p : SV_Position; float4 d : COLOR0; float4 s : COLOR1; float f : FOGC;\n"
    "           float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
    "O main(I i) {\n"
    /* NV2A puts pixel centres on integers (D3D8/9 rules), D3D11 on .5: without the half-pixel shift the
     * frame and every blur layer land half a surface pixel right and down, which left a bright line
     * along the top and left edges (a few pixels wide for the 160x120 bloom layers). */
    "  O o; float w = i.p.w != 0 ? 1 / i.p.w : 1;\n"
    "  o.p = float4(((i.p.x - 0.5) * vp.x - 1) * w, (1 - (i.p.y - 0.5) * vp.y) * w, i.p.z * vp.z * w, w);\n"
    "  o.d = i.d; o.s = i.s; o.f = i.f; o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3;\n"
    "  return o;\n"
    "}\n";

static const char BLIT_VS[] =
    "void main(uint id : SV_VertexID, out float4 p : SV_Position, out float2 t : TEXCOORD0) {\n"
    "  t = float2((id << 1) & 2, id & 2); p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n";
static const char BLIT_PS[] =
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "cbuffer B : register(b0) { float4 r; };\n"
    "float4 main(float4 p : SV_Position, float2 u : TEXCOORD0) : SV_Target {\n"
    "  return float4(t.Sample(s, u * r.xy).rgb, 1);\n"
    "}\n";

/* The same layout as the cbuffer in every generated shader. */
typedef struct {
    float c0[8][4], c1[8][4];
    float fc0[4], fc1[4];
    float fog[4];
    float misc[4];          /* x: alpha reference 0..255 */
    float st[4][4];         /* per stage: uv scale, uv offset */
} PsConsts;

typedef struct {
    Nv2aCombiner rc;        /* colours zeroed: those are constants */
    uint32_t clip_mode, alpha_func, cube_mask, flags;   /* flags 1 combiners, 2 textured */
} PsKey;

typedef struct PsEntry {
    PsKey key;
    ID3D11PixelShader *ps;
    struct PsEntry *next;
} PsEntry;
static PsEntry *s_ps_table[1024];

static char s_src[256 * 1024];
static int s_src_len;

static void emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    s_src_len += vsnprintf(s_src + s_src_len, sizeof s_src - s_src_len, fmt, ap);
    va_end(ap);
}

/* nv2a_rc_eval's map_in, as an expression. */
static void map_in(char *o, size_t n, const char *x, uint32_t byte)
{
    switch ((byte >> 5) & 7) {
    case 0:  snprintf(o, n, "max(%s,0)", x); break;
    case 1:  snprintf(o, n, "(1-saturate(%s))", x); break;
    case 2:  snprintf(o, n, "(2*max(%s,0)-1)", x); break;
    case 3:  snprintf(o, n, "(-2*max(%s,0)+1)", x); break;
    case 4:  snprintf(o, n, "(max(%s,0)-0.5)", x); break;
    case 5:  snprintf(o, n, "(-max(%s,0)+0.5)", x); break;
    case 6:  snprintf(o, n, "(%s)", x); break;
    default: snprintf(o, n, "(-(%s))", x); break;
    }
}

static const char *rd_rgb(char *o, size_t n, uint32_t byte)
{
    char x[32];
    snprintf(x, sizeof x, "R[%u].%s", byte & 0xF, (byte & 0x10) ? "aaa" : "rgb");
    map_in(o, n, x, byte);
    return o;
}

static const char *rd_a(char *o, size_t n, uint32_t byte)
{
    char x[32];
    snprintf(x, sizeof x, "R[%u].%s", byte & 0xF, (byte & 0x10) ? "a" : "b");
    map_in(o, n, x, byte);
    return o;
}

static void map_out(char *o, size_t n, const char *x, uint32_t mapping)
{
    switch (mapping) {
    case 0x08: snprintf(o, n, "clamp(%s-0.5,-1,1)", x); break;
    case 0x10: snprintf(o, n, "clamp(%s*2,-1,1)", x); break;
    case 0x18: snprintf(o, n, "clamp((%s-0.5)*2,-1,1)", x); break;
    case 0x20: snprintf(o, n, "clamp(%s*4,-1,1)", x); break;
    case 0x30: snprintf(o, n, "clamp(%s*0.5,-1,1)", x); break;
    default:   snprintf(o, n, "clamp(%s,-1,1)", x); break;
    }
}

static void emit_stage_fetch(const PsKey *k, int st)
{
    uint32_t mode = (k->rc.stage_program >> (st * 5)) & 0x1F, j;
    int cube = (k->cube_mask >> st) & 1;

    switch (mode) {
    case 0:
        emit("float4 s%d = float4(0,0,0,1);\n", st);
        break;
    case 1: case 2:
        emit("float4 s%d = T%d.Sample(S%d, (i.t%d.xy / (i.t%d.w != 0 ? i.t%d.w : 1)) * st[%d].xy + st[%d].zw);\n",
             st, st, st, st, st, st, st, st);
        break;
    case 3:
        if (cube)
            emit("float4 s%d = T%d.Sample(S%d, i.t%d.xyz);\n", st, st, st, st);
        else
            emit("float4 s%d = T%d.Sample(S%d, i.t%d.xy * st[%d].xy + st[%d].zw);\n", st, st, st, st, st, st);
        break;
    case 4:
        emit("float4 s%d = saturate(i.t%d);\n", st, st);
        break;
    case 5:
        for (j = 0; j < 4; j++) {
            int ge = (k->clip_mode >> (st * 4 + j)) & 1;
            emit("if (i.t%d.%c %s 0) discard;\n", st, "xyzw"[j], ge ? ">=" : "<");
        }
        emit("float4 s%d = 0;\n", st);
        break;
    default:
        /* The executor reads these as zero as well: bump-env and dot-product stages. */
        emit("float4 s%d = 0;\n", st);
        break;
    }
}

/* nv2a_rc_eval, one combiner program at a time. */
static void emit_combiners(const PsKey *k)
{
    const Nv2aCombiner *rc = &k->rc;
    uint32_t n = rc->control & 0xFF, flags = rc->control >> 8, s;
    char a[96], b[96], c[96], d[96];
    int i;

    if (n > 8)
        n = 8;
    for (i = 0; i < 4; i++)
        emit_stage_fetch(k, i);
    emit("float4 R[16];\nfor (int n = 0; n < 16; n++) R[n] = 0;\n");
    emit("R[3] = float4(fog.rgb, i.f); R[4] = i.d; R[5] = i.s;\n");
    emit("R[8] = s0; R[9] = s1; R[10] = s2; R[11] = s3;\n");
    emit("R[12].a = %s;\n", (rc->stage_program & 0x1F) ? "s0.a" : "1");

    for (s = 0; s < n; s++) {
        uint32_t icw = rc->color_icw[s], ocw = rc->color_ocw[s];
        uint32_t aicw = rc->alpha_icw[s], aocw = rc->alpha_ocw[s];
        uint32_t fl = ocw >> 12, afl = aocw >> 12;
        char ab[96], cd[96], ms[96];

        emit("{\nR[1] = c0[%u]; R[2] = c1[%u];\n", (flags & 0x010) ? s : 0, (flags & 0x100) ? s : 0);
        emit("bool mx = %s;\n", (flags & 1) ? "R[12].a >= 0.5" : "(((int)(R[12].a * 255)) & 1) != 0");
        emit("float3 A = %s, ", rd_rgb(a, sizeof a, (icw >> 24) & 0xFF));
        emit("B = %s, ", rd_rgb(b, sizeof b, (icw >> 16) & 0xFF));
        emit("C = %s, ", rd_rgb(c, sizeof c, (icw >> 8) & 0xFF));
        emit("D = %s;\n", rd_rgb(d, sizeof d, icw & 0xFF));
        emit("float aA = %s, ", rd_a(a, sizeof a, (aicw >> 24) & 0xFF));
        emit("aB = %s, ", rd_a(b, sizeof b, (aicw >> 16) & 0xFF));
        emit("aC = %s, ", rd_a(c, sizeof c, (aicw >> 8) & 0xFF));
        emit("aD = %s;\n", rd_a(d, sizeof d, aicw & 0xFF));

        emit("float3 pab = %s, pcd = %s;\n", (fl & 2) ? "dot(A,B).xxx" : "A*B", (fl & 1) ? "dot(C,D).xxx" : "C*D");
        if (fl & 4)
            emit("float3 sum = mx ? pcd : pab;\n");
        else
            emit("float3 sum = pab + pcd;\n");
        map_out(ab, sizeof ab, "pab", fl & 0x38);
        map_out(cd, sizeof cd, "pcd", fl & 0x38);
        map_out(ms, sizeof ms, "sum", fl & 0x38);
        emit("float3 oab = %s, ocd = %s, oms = %s;\n", ab, cd, ms);

        emit("float pa = aA*aB, pc = aC*aD;\n");
        if (afl & 4)
            emit("float sa = mx ? pc : pa;\n");
        else
            emit("float sa = pa + pc;\n");
        map_out(ab, sizeof ab, "pa", afl & 0x38);
        map_out(cd, sizeof cd, "pc", afl & 0x38);
        map_out(ms, sizeof ms, "sa", afl & 0x38);
        emit("float qab = %s, qcd = %s, qms = %s;\n", ab, cd, ms);

        if ((ocw >> 4) & 0xF) {
            emit("R[%u].rgb = oab;\n", (ocw >> 4) & 0xF);
            if (fl & 0x80)
                emit("R[%u].a = oab.b;\n", (ocw >> 4) & 0xF);
        }
        if (ocw & 0xF) {
            emit("R[%u].rgb = ocd;\n", ocw & 0xF);
            if (fl & 0x40)
                emit("R[%u].a = ocd.b;\n", ocw & 0xF);
        }
        if ((ocw >> 8) & 0xF)
            emit("R[%u].rgb = oms;\n", (ocw >> 8) & 0xF);
        if ((aocw >> 4) & 0xF)
            emit("R[%u].a = qab;\n", (aocw >> 4) & 0xF);
        if (aocw & 0xF)
            emit("R[%u].a = qcd;\n", aocw & 0xF);
        if ((aocw >> 8) & 0xF)
            emit("R[%u].a = qms;\n", (aocw >> 8) & 0xF);
        emit("R[0] = 0;\n}\n");
    }

    if (rc->final0 || rc->final1) {
        uint32_t f0 = rc->final0, f1 = rc->final1, ff = f1 & 0xFF;
        emit("R[1] = fc0; R[2] = fc1;\n");
        emit("float3 fa = %s, fb = %s;\n", (ff & 0x40) ? "1-R[5].rgb" : "R[5].rgb",
             (ff & 0x20) ? "1-R[12].rgb" : "R[12].rgb");
        emit("R[14] = float4(%s, 0);\n", (ff & 0x80) ? "saturate(fa+fb)" : "fa+fb");
        emit("float3 E = %s, ", rd_rgb(a, sizeof a, (f1 >> 24) & 0xFF));
        emit("F = %s;\n", rd_rgb(b, sizeof b, (f1 >> 16) & 0xFF));
        emit("R[15] = float4(E*F, 0);\n");
        emit("float3 A = %s, ", rd_rgb(a, sizeof a, (f0 >> 24) & 0xFF));
        emit("B = %s, ", rd_rgb(b, sizeof b, (f0 >> 16) & 0xFF));
        emit("C = %s, ", rd_rgb(c, sizeof c, (f0 >> 8) & 0xFF));
        emit("D = %s;\n", rd_rgb(d, sizeof d, f0 & 0xFF));
        emit("o.rgb = D + C*(1-A) + B*A;\n");
        emit("o.a = %s;\n", rd_a(a, sizeof a, (f1 >> 8) & 0xFF));
    } else {
        emit("o = R[12];\n");
    }
}

static ID3D11PixelShader *get_ps(const PsKey *k)
{
    uint32_t h = 2166136261u, st;
    const uint8_t *p = (const uint8_t *)k;
    size_t i;
    PsEntry *e;
    ID3DBlob *code;

    for (i = 0; i < sizeof *k; i++)
        h = (h ^ p[i]) * 16777619u;
    for (e = s_ps_table[h & 1023]; e; e = e->next)
        if (!memcmp(&e->key, k, sizeof *k))
            return e->ps;

    s_src_len = 0;
    emit("cbuffer P : register(b1) { float4 c0[8], c1[8], fc0, fc1, fog, misc, st[4]; };\n");
    for (st = 0; st < 4; st++) {
        emit("%s T%u : register(t%u); SamplerState S%u : register(s%u);\n",
             ((k->cube_mask >> st) & 1) ? "TextureCube" : "Texture2D", st, st, st, st);
    }
    emit("struct O { float4 p : SV_Position; float4 d : COLOR0; float4 s : COLOR1; float f : FOGC;\n"
         "           float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n");
    emit("float4 main(O i) : SV_Target {\nfloat4 o;\n");
    if (k->flags & 1)
        emit_combiners(k);
    else if (k->flags & 2)
        emit("o = i.d * T0.Sample(S0, i.t0.xy * st[0].xy + st[0].zw);\n");
    else
        emit("o = i.d;\n");
    emit("o = saturate(o);\n");
    if (k->alpha_func) {
        static const char *ops[] = { "false", "av < ar", "av == ar", "av <= ar", "av > ar",
                                     "av != ar", "av >= ar", "true" };
        emit("int av = (int)(o.a * 255 + 0.5), ar = (int)misc.x;\n");
        emit("if (!(%s)) discard;\n", ops[(k->alpha_func - 0x200) & 7]);
    }
    emit("return o;\n}\n");

    code = compile(s_src, s_src_len, "ps_5_0");
    e = (PsEntry *)calloc(1, sizeof *e);
    e->key = *k;
    CHECK(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                         ID3D10Blob_GetBufferSize(code), NULL, &e->ps));
    ID3D10Blob_Release(code);
    e->next = s_ps_table[h & 1023];
    s_ps_table[h & 1023] = e;
    s_st.shaders++;
    return e->ps;
}

/* ---- window ---------------------------------------------------------- */
static void toggle_fullscreen(HWND h)
{
    if (!s_fullscreen) {
        HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof mi };
        GetWindowRect(h, &s_windowed);
        GetMonitorInfoA(mon, &mi);
        SetWindowLongPtrA(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtrA(h, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(h, HWND_NOTOPMOST, s_windowed.left, s_windowed.top,
                     s_windowed.right - s_windowed.left, s_windowed.bottom - s_windowed.top,
                     SWP_FRAMECHANGED);
    }
    s_fullscreen = !s_fullscreen;
}

/* The windowed client size, for the next start. */
static void remember_window_size(HWND h)
{
    RECT c;
    if (s_fullscreen || IsIconic(h) || IsZoomed(h) || !GetClientRect(h, &c))
        return;
    if (c.right == g_settings.window_width && c.bottom == g_settings.window_height)
        return;
    g_settings.window_width = c.right;
    g_settings.window_height = c.bottom;
    settings_set_int("WindowWidth", c.right);
    settings_set_int("WindowHeight", c.bottom);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CLOSE:
    case WM_DESTROY:
        /* Quit to desktop. Gone from the screen at once, but the process
         * waits for a save in progress to finish its writes. */
        ShowWindow(h, SW_HIDE);
        xbox_FileWritesSettle(1500, 8000);
        ExitProcess(0);
    case WM_SIZE:
        s_resize = 1;
        return 0;
    case WM_EXITSIZEMOVE:
        remember_window_size(h);
        return 0;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN) {
            toggle_fullscreen(h);
            g_settings.fullscreen = s_fullscreen;
            settings_set_int("Fullscreen", s_fullscreen);
            return 0;
        }
        break;
    case WM_SYSCHAR:
        return 0;
    case WM_SETCURSOR:
        /* The mouse turns the camera: no pointer over the picture while it is captured (F9 releases). */
        if (LOWORD(l) == HTCLIENT && mkb_captured()) {
            SetCursor(NULL);
            return TRUE;
        }
        break;
    }
    return DefWindowProcA(h, m, w, l);
}

/* DBG: RECOMP_SHOT=<prefix> RECOMP_SHOT_AT=<s>,<s>... saves the presented
 * frame as <prefix>-<s>.bmp at those seconds after the first frame. A screen
 * grab of the window needs it on top; this does not. */
static void dbg_shot(void)
{
    static const char *prefix;
    static int init, next;
    static int at[16], nat;
    static DWORD t0;
    ID3D11Texture2D *bb, *st;
    D3D11_TEXTURE2D_DESC d;
    D3D11_MAPPED_SUBRESOURCE m;
    char path[MAX_PATH];
    FILE *f;
    uint32_t y, x;

    if (!init) {
        const char *e = getenv("RECOMP_SHOT_AT");
        init = 1;
        prefix = getenv("RECOMP_SHOT");
        while (e && *e && nat < 16) {
            at[nat++] = atoi(e);
            e = strchr(e, ',');
            if (e)
                e++;
        }
        t0 = GetTickCount();
    }
    if (!prefix || next >= nat || GetTickCount() - t0 < (DWORD)at[next] * 1000)
        return;
    if (FAILED(IDXGISwapChain1_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&bb)))
        return;
    ID3D11Texture2D_GetDesc(bb, &d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &st))) {
        ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, (ID3D11Resource *)bb);
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
            snprintf(path, sizeof path, "%s-%d.bmp", prefix, at[next]);
            f = fopen(path, "wb");
            if (f) {
                uint8_t hd[54] = { 'B', 'M' };
                int rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                *(uint32_t *)(hd + 2) = 54 + d.Width * d.Height * 4;
                *(uint32_t *)(hd + 10) = 54;
                *(uint32_t *)(hd + 14) = 40;
                *(int32_t *)(hd + 18) = (int32_t)d.Width;
                *(int32_t *)(hd + 22) = -(int32_t)d.Height;
                *(uint16_t *)(hd + 26) = 1;
                *(uint16_t *)(hd + 28) = 32;
                fwrite(hd, 1, sizeof hd, f);
                for (y = 0; y < d.Height; y++) {
                    uint32_t *row = (uint32_t *)((uint8_t *)m.pData + (size_t)y * m.RowPitch);
                    for (x = 0; x < d.Width; x++) {
                        uint32_t p = row[x];
                        if (rgba)
                            p = (p & 0xFF00FF00u) | (p >> 16 & 0xFF) | (p & 0xFF) << 16;
                        p |= 0xFF000000u;
                        fwrite(&p, 4, 1, f);
                    }
                }
                fclose(f);
            }
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
        }
        ID3D11Texture2D_Release(st);
    }
    ID3D11Texture2D_Release(bb);
    next++;
}

static void make_back_view(void)
{
    ID3D11Texture2D *bb;
    CHECK(IDXGISwapChain1_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&bb));
    CHECK(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)bb, NULL, &s_back_rtv));
    ID3D11Texture2D_Release(bb);
}

static ID3D11ShaderResourceView *white_view(int cube)
{
    static const uint32_t white[6] = { ~0u, ~0u, ~0u, ~0u, ~0u, ~0u };
    D3D11_TEXTURE2D_DESC d;
    D3D11_SUBRESOURCE_DATA init[6];
    ID3D11Texture2D *t;
    ID3D11ShaderResourceView *v;
    int i;

    memset(&d, 0, sizeof d);
    d.Width = d.Height = 1;
    d.MipLevels = 1;
    d.ArraySize = cube ? 6 : 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    for (i = 0; i < 6; i++) {
        init[i].pSysMem = &white[i];
        init[i].SysMemPitch = 4;
        init[i].SysMemSlicePitch = 4;
    }
    CHECK(ID3D11Device_CreateTexture2D(s_dev, &d, init, &t));
    CHECK(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t, NULL, &v));
    ID3D11Texture2D_Release(t);
    return v;
}

static void gpu_init(void)
{
    static const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
    static const D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Nv2aVertex, x), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, offsetof(Nv2aVertex, diffuse), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 1, DXGI_FORMAT_B8G8R8A8_UNORM, 0, offsetof(Nv2aVertex, specular), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "FOGC", 0, DXGI_FORMAT_R32_FLOAT, 0, offsetof(Nv2aVertex, fog), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Nv2aVertex, tc[0]), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Nv2aVertex, tc[1]), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Nv2aVertex, tc[2]), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Nv2aVertex, tc[3]), D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    WNDCLASSA wc;
    RECT r = { 0, 0, g_settings.window_width, g_settings.window_height };
    IDXGIDevice *dd;
    IDXGIAdapter *ad;
    IDXGIFactory2 *fac;
    IDXGIFactory5 *f5;
    DXGI_SWAP_CHAIN_DESC1 sd;
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_SAMPLER_DESC sm;
    ID3DBlob *b;
    const char *e = getenv("RECOMP_GPU_SCALE");

    s_scale = (uint32_t)g_settings.render_scale;
    e = getenv("RECOMP_VIDEO_FLAGS");
    s_widescreen = e && (strtoul(e, NULL, 0) & 1);
    e = getenv("RECOMP_GPU_SCALE");
    if (e && atoi(e) > 0)
        s_scale = (uint32_t)atoi(e);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(wc.hInstance, MAKEINTRESOURCEA(1));
    wc.lpszClassName = "SpyroAnbGpu";
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    s_hwnd = CreateWindowExA(0, wc.lpszClassName, "The Legend of Spyro: A New Beginning",
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (g_settings.fullscreen)
        toggle_fullscreen(s_hwnd);
    if (!s_scale) {
        /* Enough to cover the monitor the window is on: in 16:9 the game's 640
         * columns span the whole width. */
        MONITORINFO mi = { sizeof mi };
        uint32_t w = 1920, h = 1080, sw, sh;
        if (GetMonitorInfoA(MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            w = (uint32_t)(mi.rcMonitor.right - mi.rcMonitor.left);
            h = (uint32_t)(mi.rcMonitor.bottom - mi.rcMonitor.top);
        }
        sw = (s_widescreen ? w : w * 3 / 4 < h ? w : h * 4 / 3) + 639;
        sh = h + 479;
        s_scale = sw / 640 > sh / 480 ? sw / 640 : sh / 480;
        if (s_scale < 1) s_scale = 1;
        if (s_scale > 4) s_scale = 4;
    }

    CHECK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                            &want, 1, D3D11_SDK_VERSION, &s_dev, NULL, &s_ctx));
    CHECK(ID3D11Device_QueryInterface(s_dev, &IID_IDXGIDevice, (void **)&dd));
    CHECK(IDXGIDevice_GetAdapter(dd, &ad));
    CHECK(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&fac));
    if (SUCCEEDED(IDXGIFactory2_QueryInterface(fac, &IID_IDXGIFactory5, (void **)&f5))) {
        BOOL t = FALSE;
        if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &t, sizeof t)))
            s_tearing = t;
        IDXGIFactory5_Release(f5);
    }
    memset(&sd, 0, sizeof sd);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = s_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    CHECK(IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown *)s_dev, s_hwnd, &sd, NULL, NULL, &s_swap));
    IDXGIFactory2_MakeWindowAssociation(fac, s_hwnd, DXGI_MWA_NO_ALT_ENTER);
    make_back_view();

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = VB_BYTES;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    CHECK(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vb));
    bd.ByteWidth = 16;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    CHECK(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vs_cb));
    bd.ByteWidth = sizeof(PsConsts);
    CHECK(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_ps_cb));

    b = compile(VS_SRC, sizeof VS_SRC - 1, "vs_5_0");
    CHECK(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_vs));
    CHECK(ID3D11Device_CreateInputLayout(s_dev, layout, 8, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), &s_layout));
    ID3D10Blob_Release(b);
    b = compile(BLIT_VS, sizeof BLIT_VS - 1, "vs_5_0");
    CHECK(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_blit_vs));
    ID3D10Blob_Release(b);
    b = compile(BLIT_PS, sizeof BLIT_PS - 1, "ps_5_0");
    CHECK(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s_blit_ps));
    ID3D10Blob_Release(b);

    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    CHECK(ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_raster));

    memset(&sm, 0, sizeof sm);
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sm.MaxLOD = D3D11_FLOAT32_MAX;
    CHECK(ID3D11Device_CreateSamplerState(s_dev, &sm, &s_blit_sampler));

    s_white = white_view(0);
    s_white_cube = white_view(1);
    fprintf(stderr, "[gpu] D3D11 up, scale %u, tearing %d\n", s_scale, s_tearing);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* ---- surfaces -------------------------------------------------------- */
static void make_color(uint32_t w, uint32_t h, ID3D11Texture2D **tex, ID3D11RenderTargetView **rtv,
                       ID3D11ShaderResourceView **srv)
{
    D3D11_TEXTURE2D_DESC d;
    memset(&d, 0, sizeof d);
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rtv ? D3D11_BIND_RENDER_TARGET : 0);
    CHECK(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, tex));
    if (rtv)
        CHECK(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)*tex, NULL, rtv));
    CHECK(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)*tex, NULL, srv));
}

static void unbind_srvs(void)
{
    static ID3D11ShaderResourceView *const none[4];
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, none);
    memset(s_srv_bound, 0, sizeof s_srv_bound);
}

static void release_copy(Target *t)
{
    if (t->copy) {
        ID3D11ShaderResourceView_Release(t->copy_srv);
        ID3D11Texture2D_Release(t->copy);
        t->copy = NULL;
        t->copy_srv = NULL;
    }
}

/* Each level load puts the game's surfaces at new addresses, so the tables fill up over a session.
 * A full table used to restart at slot 0 without releasing anything: lookups then saw only the
 * slots after the restart, and a reused slot that was still bound skipped bind_targets' viewport
 * update, so the 640x480 frame drew into a small top-left corner of stale white (seen on the Ally
 * after a long session). Now the least recently used entry that is not in use is released. */
static int free_rt_slot(void)
{
    int i, best = -1;
    Target *t;

    if (s_nrt < MAX_TARGETS)
        return s_nrt++;
    for (i = 0; i < MAX_TARGETS; i++)
        if (&s_rts[i] != s_cur_rt && &s_rts[i] != s_present && (best < 0 || s_frame - s_rts[i].frame > s_frame - s_rts[best].frame))
            best = i;
    t = &s_rts[best];
    fprintf(stderr, "[gpu] evict target %08X %ux%u, unused for %u frames\n", t->va, t->lw, t->lh, s_frame - t->frame);
    unbind_srvs();
    release_copy(t);
    ID3D11ShaderResourceView_Release(t->srv);
    ID3D11RenderTargetView_Release(t->rtv);
    ID3D11Texture2D_Release(t->tex);
    memset(t, 0, sizeof *t);
    return best;
}

static int free_ds_slot(void)
{
    int i, best = -1;
    Depth *z;

    if (s_nds < MAX_TARGETS)
        return s_nds++;
    for (i = 0; i < MAX_TARGETS; i++)
        if (&s_dss[i] != s_cur_ds && (best < 0 || s_frame - s_dss[i].frame > s_frame - s_dss[best].frame))
            best = i;
    z = &s_dss[best];
    fprintf(stderr, "[gpu] evict depth %08X %ux%u, unused for %u frames\n", z->va, z->lw, z->lh, s_frame - z->frame);
    ID3D11DepthStencilView_Release(z->dsv);
    ID3D11Texture2D_Release(z->tex);
    memset(z, 0, sizeof *z);
    return best;
}

/* A title narrows the clip rectangle freely, so a target only ever grows and keeps its pixels. */
static Target *get_rt(const Nv2aSurface *s)
{
    Target *t = NULL, old;
    int i;

    for (i = 0; i < s_nrt; i++)
        if (s_rts[i].va == s->color_va) {
            t = &s_rts[i];
            break;
        }
    /* A clip wider than the pitch is left over from the previous surface: the
     * title moves the colour offset first. Growing on it would blow a 64x64
     * blur target up to the screen's size for good. */
    if (t && (s->width * s->bytes_per_pixel > s->pitch || (s->width <= t->lw && s->height <= t->lh))) {
        t->pitch = s->pitch;
        t->bpp = s->bytes_per_pixel;
        return t;
    }
    if (!t) {
        t = &s_rts[free_rt_slot()];
        memset(t, 0, sizeof *t);
    }
    old = *t;
    t->va = s->color_va;
    t->frame = s_frame;
    t->pitch = s->pitch;
    t->bpp = s->bytes_per_pixel;
    t->lw = s->width > old.lw ? s->width : old.lw;
    t->lh = s->height > old.lh ? s->height : old.lh;
    make_color(t->lw * s_scale, t->lh * s_scale, &t->tex, &t->rtv, &t->srv);
    t->copy = NULL;
    t->copy_srv = NULL;
    fprintf(stderr, "[gpu] target %08X %ux%u pitch %u bpp %u\n", t->va, t->lw, t->lh, t->pitch, t->bpp);
    {
        FLOAT black[4] = { 0, 0, 0, 1 };
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, t->rtv, black);
    }
    if (old.tex) {
        D3D11_BOX box = { 0, 0, 0, old.lw * s_scale, old.lh * s_scale, 1 };
        ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)t->tex, 0, 0, 0, 0,
                                                  (ID3D11Resource *)old.tex, 0, &box);
        if (s_cur_rt == t) {
            ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
            s_cur_rt = NULL;
        }
        unbind_srvs();
        if (old.copy) {
            ID3D11ShaderResourceView_Release(old.copy_srv);
            ID3D11Texture2D_Release(old.copy);
        }
        ID3D11ShaderResourceView_Release(old.srv);
        ID3D11RenderTargetView_Release(old.rtv);
        ID3D11Texture2D_Release(old.tex);
    }
    return t;
}

static Depth *get_ds(uint32_t va, uint32_t lw, uint32_t lh)
{
    D3D11_TEXTURE2D_DESC d;
    Depth *z = NULL;
    int i;

    for (i = 0; i < s_nds; i++)
        if (s_dss[i].va == va) {
            z = &s_dss[i];
            break;
        }
    if (z && lw <= z->lw && lh <= z->lh) {
        z->frame = s_frame;
        return z;
    }
    if (!z) {
        z = &s_dss[free_ds_slot()];
        memset(z, 0, sizeof *z);
    } else {
        if (s_cur_ds == z) {
            ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
            s_cur_ds = NULL;
            s_cur_rt = NULL;
        }
        ID3D11DepthStencilView_Release(z->dsv);
        ID3D11Texture2D_Release(z->tex);
    }
    z->va = va;
    z->frame = s_frame;
    z->lw = lw > z->lw ? lw : z->lw;
    z->lh = lh > z->lh ? lh : z->lh;
    memset(&d, 0, sizeof d);
    d.Width = z->lw * s_scale;
    d.Height = z->lh * s_scale;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    CHECK(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &z->tex));
    CHECK(ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)z->tex, NULL, &z->dsv));
    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, z->dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    fprintf(stderr, "[gpu] depth %08X %ux%u\n", z->va, z->lw, z->lh);
    return z;
}

static void bind_targets(Target *rt, Depth *ds)
{
    D3D11_VIEWPORT vp;
    D3D11_MAPPED_SUBRESOURCE m;
    float *c;

    if (rt == s_cur_rt && ds == s_cur_ds)
        return;
    unbind_srvs();
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rt->rtv, ds ? ds->dsv : NULL);
    if (rt != s_cur_rt) {
        vp.TopLeftX = vp.TopLeftY = 0;
        vp.Width = (float)(rt->lw * s_scale);
        vp.Height = (float)(rt->lh * s_scale);
        vp.MinDepth = 0;
        vp.MaxDepth = 1;
        ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    }
    s_cur_rt = rt;
    s_cur_ds = ds;
    CHECK(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vs_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
    c = (float *)m.pData;
    c[0] = 2.0f / (float)rt->lw;
    c[1] = 2.0f / (float)rt->lh;
    c[2] = 1.0f / 16777215.0f;
    c[3] = 0;
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vs_cb, 0);
}

/* ---- textures -------------------------------------------------------- */
typedef struct TexEntry {
    uint32_t offset, w, h, pitch, color, palette, cube, levels;
    uint32_t frame;
    uint64_t hash;
    int buttons;                /* the controller glyph sheet (prompts.c) */
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    struct TexEntry *next;
} TexEntry;

/* The keyboard version of the controller glyph sheet, built the first time
 * it is needed from the controller sheet's decoded pixels. */
static uint32_t *s_buttons_orig;
static ID3D11ShaderResourceView *s_kb_buttons;

static uint64_t hash_words(uint64_t h, const uint8_t *p, size_t n);

static void note_buttons(TexEntry *e, const Nv2aTexture *t, const uint32_t *px)
{
    e->buttons = 0;
    if (t->width != 256 || t->height != 128 || t->cube)
        return;
    if (hash_words(0xCBF29CE484222325ull, (const uint8_t *)px, 256 * 128 * 4) != PROMPTS_SHEET_HASH)
        return;
    e->buttons = 1;
    if (!s_buttons_orig && (s_buttons_orig = (uint32_t *)malloc(256 * 128 * 4)) != NULL)
        memcpy(s_buttons_orig, px, 256 * 128 * 4);
}

static ID3D11ShaderResourceView *tex_srv(const TexEntry *e)
{
    static int force = -2;  /* DBG: RECOMP_PROMPTS=kb|pad pins the prompt style */
    if (force == -2) {
        const char *p = getenv("RECOMP_PROMPTS");
        force = !p ? -1 : !strcmp(p, "kb") ? 1 : !strcmp(p, "pad") ? 0 : -1;
    }
    if (!e->buttons || (force >= 0 ? !force : !mkb_enabled() || !mkb_last_device_keyboard()))
        return e->srv;
    if (!s_kb_buttons && s_buttons_orig) {
        uint32_t *px = prompts_build(s_buttons_orig, s_scale);
        D3D11_TEXTURE2D_DESC d;
        D3D11_SUBRESOURCE_DATA sd;
        ID3D11Texture2D *tex;
        if (!px)
            return e->srv;
        memset(&d, 0, sizeof d);
        d.Width = 256 * s_scale;
        d.Height = 128 * s_scale;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        sd.pSysMem = px;
        sd.SysMemPitch = d.Width * 4;
        sd.SysMemSlicePitch = 0;
        CHECK(ID3D11Device_CreateTexture2D(s_dev, &d, &sd, &tex));
        CHECK(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)tex, NULL, &s_kb_buttons));
        ID3D11Texture2D_Release(tex);
        free(px);
    }
    return s_kb_buttons ? s_kb_buttons : e->srv;
}
static TexEntry *s_tex_table[TEX_BUCKETS];
static uint32_t *s_decode;
static size_t s_decode_n;

static uint64_t hash_words(uint64_t h, const uint8_t *p, size_t n)
{
    size_t i;
    __try {
        for (i = 0; i + 8 <= n; i += 8) {
            h = (h ^ *(const uint64_t *)(p + i)) * 0x9E3779B97F4A7C15ull;
            h ^= h >> 29;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        h ^= 0xDEAD;
    }
    return h;
}

static uint64_t tex_hash(const Nv2aTexture *t)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bytes = nv2a_backend_texture_bytes(t);
    uint64_t h = hash_words(0xCBF29CE484222325ull, mem + t->offset, bytes);
    if (t->palette)
        h = hash_words(h, mem + t->palette, 1024);
    return h;
}

/* A movie frame: YUY2 or UYVY, the executor's conversion over whole rows instead of one texel at a time. */
static int decode_yuv(const Nv2aTexture *t, uint32_t *out)
{
    const uint8_t *row = (const uint8_t *)xbox_GetMemoryOffset() + t->offset;
    uint32_t yoff = t->color == 0x24 ? 0 : 1, x, y, i;

    if ((t->color != 0x24 && t->color != 0x25) || t->cube)
        return 0;
    for (y = 0; y < t->height; y++, row += t->pitch) {
        for (x = 0; x < t->width; x += 2) {
            const uint8_t *g = row + (size_t)x * 2;
            int cu = (int)g[1 - yoff] - 128, cv = (int)g[3 - yoff] - 128;
            int dr = 409 * cv + 128, dg = -100 * cu - 208 * cv + 128, db = 516 * cu + 128;
            for (i = 0; i < 2 && x + i < t->width; i++) {
                int c = 298 * ((int)g[i * 2 + yoff] - 16);
                int r = (c + dr) >> 8, gg = (c + dg) >> 8, b = (c + db) >> 8;
                r = r < 0 ? 0 : (r > 255 ? 255 : r);
                gg = gg < 0 ? 0 : (gg > 255 ? 255 : gg);
                b = b < 0 ? 0 : (b > 255 ? 255 : b);
                *out++ = 0xFF000000u | (uint32_t)r << 16 | (uint32_t)gg << 8 | (uint32_t)b;
            }
        }
    }
    return 1;
}

/* DBG: RECOMP_TEX_DUMP_DIR=<dir> writes every distinct decoded 2D texture
 * there once, as <hash>_<w>x<h>_<fmt>.bmp, to find one by eye. */
static void tex_dump(const Nv2aTexture *t, const uint32_t *px)
{
    static const char *dir;
    static int init;
    static uint64_t seen[4096];
    static unsigned nseen;
    char path[MAX_PATH];
    uint64_t h;
    unsigned i;
    FILE *f;

    if (!init) {
        dir = getenv("RECOMP_TEX_DUMP_DIR");
        init = 1;
    }
    if (!dir || t->cube || t->width < 16 || t->height < 16)
        return;
    h = hash_words(0xCBF29CE484222325ull, (const uint8_t *)px, (size_t)t->width * t->height * 4);
    for (i = 0; i < nseen; i++)
        if (seen[i] == h)
            return;
    if (nseen < sizeof seen / sizeof *seen)
        seen[nseen++] = h;
    snprintf(path, sizeof path, "%s\\%016llX_%ux%u_%02X.bmp", dir, (unsigned long long)h, t->width, t->height,
             t->color);
    f = fopen(path, "wb");
    if (f) {
        uint32_t size = t->width * t->height * 4;
        uint8_t hd[54] = { 'B', 'M' };
        *(uint32_t *)(hd + 2) = 54 + size;
        *(uint32_t *)(hd + 10) = 54;
        *(uint32_t *)(hd + 14) = 40;
        *(int32_t *)(hd + 18) = (int32_t)t->width;
        *(int32_t *)(hd + 22) = -(int32_t)t->height;
        *(uint16_t *)(hd + 26) = 1;
        *(uint16_t *)(hd + 28) = 32;
        fwrite(hd, 1, sizeof hd, f);
        fwrite(px, 1, size, f);
        fclose(f);
    }
}

static int tex_upload(TexEntry *e, const Nv2aTexture *t)
{
    D3D11_BOX box = { 0, 0, 0, t->width, t->height, 1 };
    uint32_t faces = t->cube ? 6 : 1, f;
    size_t need = (size_t)t->width * t->height;
    Nv2aTexture face = *t;

    if (need > s_decode_n) {
        free(s_decode);
        s_decode = (uint32_t *)malloc(need * 4);
        s_decode_n = need;
    }
    for (f = 0; f < faces; f++) {
        UINT sub = f;
        face.offset = t->offset + f * nv2a_backend_face_stride(t);
        { /* DBG */ static LARGE_INTEGER fq; LARGE_INTEGER q0, q1; static double tot; static unsigned cnt; int ok_;
          if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
          QueryPerformanceCounter(&q0); ok_ = decode_yuv(&face, s_decode) || nv2a_backend_decode_texture(&face, s_decode); QueryPerformanceCounter(&q1);
          tot += (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)fq.QuadPart;
          if (!(++cnt & 63)) { fprintf(stderr, "[gpu] decode avg %.2f ms (fmt %02X %ux%u pitch %u)\n", tot / 64, t->color, t->width, t->height, t->pitch); tot = 0; }
          if (!ok_) return 0; }
        if (!f) {
            tex_dump(&face, s_decode);
            note_buttons(e, &face, s_decode);
        }
        if (e->levels > 1) {
            D3D11_TEXTURE2D_DESC d;
            ID3D11Texture2D_GetDesc(e->tex, &d);
            sub = f * d.MipLevels;
        }
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)e->tex, sub, &box, s_decode, t->width * 4, 0);
    }
    if (e->levels > 1)
        ID3D11DeviceContext_GenerateMips(s_ctx, e->srv);
    s_st.uploads++;
    s_st.bytes += (uint32_t)(need * 4 * faces);
    return 1;
}

static TexEntry *get_tex(const Nv2aTexture *t)
{
    uint32_t b = (t->offset >> 4) ^ (t->offset >> 15) ^ (t->width * 31) ^ t->color;
    uint32_t levels = t->levels > 1 ? t->levels : 1;
    uint64_t h;
    TexEntry *e;
    D3D11_TEXTURE2D_DESC d;

    b &= TEX_BUCKETS - 1;
    for (e = s_tex_table[b]; e; e = e->next)
        if (e->offset == t->offset && e->w == t->width && e->h == t->height && e->pitch == t->pitch
            && e->color == t->color && e->palette == t->palette && e->cube == t->cube && e->levels == levels)
            break;
    if (e && e->frame == s_frame)
        return e;
    h = tex_hash(t);
    s_st.rehash++;
    if (e) {
        e->frame = s_frame;
        if (e->hash != h && tex_upload(e, t))
            e->hash = h;
        return e;
    }

    e = (TexEntry *)calloc(1, sizeof *e);
    e->offset = t->offset; e->w = t->width; e->h = t->height; e->pitch = t->pitch;
    e->color = t->color; e->palette = t->palette; e->cube = t->cube; e->levels = levels;
    e->frame = s_frame;
    e->hash = h;
    memset(&d, 0, sizeof d);
    d.Width = t->width;
    d.Height = t->height;
    d.MipLevels = levels > 1 ? 0 : 1;
    d.ArraySize = t->cube ? 6 : 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (levels > 1 ? D3D11_BIND_RENDER_TARGET : 0);
    d.MiscFlags = (levels > 1 ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0)
                | (t->cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0);
    CHECK(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &e->tex));
    CHECK(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)e->tex, NULL, &e->srv));
    if (!tex_upload(e, t)) {
        static int warned;
        if (warned++ < 16)
            fprintf(stderr, "[gpu] cannot decode texture %08X fmt %02X\n", t->offset, t->color);
        ID3D11ShaderResourceView_Release(e->srv);
        ID3D11Texture2D_Release(e->tex);
        e->srv = t->cube ? s_white_cube : s_white;
        ID3D11ShaderResourceView_AddRef(e->srv);
        e->tex = NULL;
    }
    e->next = s_tex_table[b];
    s_tex_table[b] = e;
    return e;
}

/* Textures a title stopped using, every minute or so. */
static void tex_sweep(void)
{
    int i;
    for (i = 0; i < TEX_BUCKETS; i++) {
        TexEntry **p = &s_tex_table[i];
        while (*p) {
            TexEntry *e = *p;
            if (s_frame - e->frame > 3600) {
                *p = e->next;
                ID3D11ShaderResourceView_Release(e->srv);
                if (e->tex)
                    ID3D11Texture2D_Release(e->tex);
                free(e);
            } else {
                p = &e->next;
            }
        }
    }
}

/* ---- cached state ---------------------------------------------------- */
typedef struct { uint32_t k[6]; void *obj; } StateEntry;
static StateEntry s_blends[MAX_STATES], s_depths[MAX_STATES], s_samplers[MAX_STATES];
static int s_nblend, s_ndepth, s_nsampler;

static void *state_find(StateEntry *t, int n, const uint32_t *k, int words)
{
    int i;
    for (i = 0; i < n; i++)
        if (!memcmp(t[i].k, k, words * 4))
            return t[i].obj;
    return NULL;
}

static D3D11_BLEND blend_factor(uint32_t f, int alpha)
{
    switch (f) {
    case 0:      return D3D11_BLEND_ZERO;
    case 1:      return D3D11_BLEND_ONE;
    case 0x300:  return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x301:  return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x302:  return D3D11_BLEND_SRC_ALPHA;
    case 0x303:  return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304:  return D3D11_BLEND_DEST_ALPHA;
    case 0x305:  return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306:  return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x307:  return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x308:  return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND_OP blend_op(uint32_t e)
{
    switch (e) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

static ID3D11BlendState *get_blend(const Nv2aRenderState *r)
{
    uint32_t k[6] = { r->blend_enable, r->blend_src, r->blend_dst, r->blend_eq, r->color_mask, 0 };
    ID3D11BlendState *o;
    D3D11_BLEND_DESC d;
    D3D11_RENDER_TARGET_BLEND_DESC *t = &d.RenderTarget[0];

    if (!r->blend_enable)
        k[1] = k[2] = k[3] = 0;
    o = (ID3D11BlendState *)state_find(s_blends, s_nblend, k, 5);
    if (o)
        return o;
    memset(&d, 0, sizeof d);
    t->BlendEnable = r->blend_enable != 0;
    t->SrcBlend = blend_factor(r->blend_src, 0);
    t->DestBlend = blend_factor(r->blend_dst, 0);
    t->BlendOp = blend_op(r->blend_eq);
    t->SrcBlendAlpha = blend_factor(r->blend_src, 1);
    t->DestBlendAlpha = blend_factor(r->blend_dst, 1);
    t->BlendOpAlpha = t->BlendOp;
    t->RenderTargetWriteMask = ((r->color_mask & 0x00010000) ? D3D11_COLOR_WRITE_ENABLE_RED : 0)
                             | ((r->color_mask & 0x00000100) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0)
                             | ((r->color_mask & 0x00000001) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0)
                             | ((r->color_mask & 0x01000000) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0);
    CHECK(ID3D11Device_CreateBlendState(s_dev, &d, &o));
    if (s_nblend < MAX_STATES) {
        memcpy(s_blends[s_nblend].k, k, sizeof k);
        s_blends[s_nblend++].obj = o;
    }
    return o;
}

static D3D11_STENCIL_OP stencil_op(uint32_t op)
{
    switch (op) {
    case 0:      return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

static ID3D11DepthStencilState *get_depth(const Nv2aRenderState *r)
{
    uint32_t k[6];
    ID3D11DepthStencilState *o;
    D3D11_DEPTH_STENCIL_DESC d;

    memset(k, 0, sizeof k);
    if (r->zeta_va) {
        k[0] = (r->depth_test_enable ? 1 : 0) | (r->depth_write ? 2 : 0) | (r->stencil_enable ? 4 : 0);
        k[1] = r->depth_func;
    }
    if (k[0] & 4) {
        k[2] = r->stencil_func;
        k[3] = (r->stencil_func_mask & 0xFF) | (r->stencil_write_mask & 0xFF) << 8;
        k[4] = r->stencil_fail;
        k[5] = r->stencil_zfail | r->stencil_zpass << 16;
    }
    o = (ID3D11DepthStencilState *)state_find(s_depths, s_ndepth, k, 6);
    if (o)
        return o;
    memset(&d, 0, sizeof d);
    d.DepthEnable = (k[0] & 1) != 0;
    d.DepthWriteMask = (k[0] & 2) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = (D3D11_COMPARISON_FUNC)((k[1] & 7) + 1);
    d.StencilEnable = (k[0] & 4) != 0 && !getenv("RECOMP_GPU_NOSTENCIL"); /* DBG */
    d.StencilReadMask = (UINT8)r->stencil_func_mask;
    d.StencilWriteMask = (UINT8)r->stencil_write_mask;
    d.FrontFace.StencilFailOp = stencil_op(r->stencil_fail);
    d.FrontFace.StencilDepthFailOp = stencil_op(r->stencil_zfail);
    d.FrontFace.StencilPassOp = stencil_op(r->stencil_zpass);
    d.FrontFace.StencilFunc = (D3D11_COMPARISON_FUNC)((r->stencil_func & 7) + 1);
    d.BackFace = d.FrontFace;
    CHECK(ID3D11Device_CreateDepthStencilState(s_dev, &d, &o));
    if (s_ndepth < MAX_STATES) {
        memcpy(s_depths[s_ndepth].k, k, sizeof k);
        s_depths[s_ndepth++].obj = o;
    }
    return o;
}

static D3D11_TEXTURE_ADDRESS_MODE addr_mode(uint32_t a)
{
    switch (a) {
    case 1:  return D3D11_TEXTURE_ADDRESS_WRAP;
    case 2:  return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 4:  return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

static ID3D11SamplerState *get_sampler(const Nv2aTexture *t)
{
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    uint32_t mips = t->levels > 1;
    uint32_t k[6] = { t->addr_u, t->addr_v, mag, min, mips, 0 };
    ID3D11SamplerState *o = (ID3D11SamplerState *)state_find(s_samplers, s_nsampler, k, 5);
    D3D11_SAMPLER_DESC d;
    int lmag = mag == 2, lmin = min == 2 || min == 4 || min == 6, lmip = min == 5 || min == 6;

    if (o)
        return o;
    memset(&d, 0, sizeof d);
    d.Filter = (D3D11_FILTER)((lmin ? 0x10 : 0) | (lmag ? 0x04 : 0) | (lmip ? 0x01 : 0));
    d.AddressU = addr_mode(t->addr_u);
    d.AddressV = addr_mode(t->addr_v);
    d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MaxLOD = (mips && min > 2) ? D3D11_FLOAT32_MAX : 0.0f;
    CHECK(ID3D11Device_CreateSamplerState(s_dev, &d, &o));
    if (s_nsampler < MAX_STATES) {
        memcpy(s_samplers[s_nsampler].k, k, sizeof k);
        s_samplers[s_nsampler++].obj = o;
    }
    return o;
}

/* ---- the back end ---------------------------------------------------- */
/* The target a texture lies in, if the title rendered there recently. */
static Target *rt_for_texture(const Nv2aTexture *t, float *ox, float *oy)
{
    int i;
    /* A target can be taller than its surface after a stale clip, which runs its range over the next
     * surface along: the blur's 64x64 pair sits back to back. The surface that starts here is the one. */
    for (i = 0; i < s_nrt; i++)
        if (s_rts[i].va == t->offset && s_frame - s_rts[i].frame <= 300) {
            *ox = *oy = 0;
            return &s_rts[i];
        }
    for (i = 0; i < s_nrt; i++) {
        Target *r = &s_rts[i];
        uint32_t off;
        if (t->offset < r->va || t->offset >= r->va + r->pitch * r->lh)
            continue;
        if (s_frame - r->frame > 300)
            continue;
        off = t->offset - r->va;
        *ox = (float)((off % r->pitch) / r->bpp);
        *oy = (float)(off / r->pitch);
        return r;
    }
    return NULL;
}

static void bind_common(void)
{
    UINT stride = sizeof(Nv2aVertex), offs = 0;

    if (s_bound == 1)
        return;
    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &offs);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_vs_cb);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 1, 1, &s_ps_cb);
    ID3D11DeviceContext_RSSetState(s_ctx, s_raster);
    s_bound = 1;
}

/* ---- vertex programs on the GPU -------------------------------------- */
extern char *gpu_vsh_hlsl(const uint32_t (*prog)[4], uint32_t count, uint32_t inputs);

#define VS_BUCKETS 256
#define IB_BYTES (16u * 1024 * 1024)

typedef struct VsEntry {
    uint64_t hash;
    ID3D11VertexShader *vs;             /* NULL: failed to compile, stays on the CPU */
    ID3D11InputLayout *layout;
    uint32_t stride;
    struct VsEntry *next;
} VsEntry;

typedef struct {
    float vp[4], aa[4], fg[4];
    float imm[16][4];
    float c[192][4];
} VsConsts;

static VsEntry *s_vs_table[VS_BUCKETS];
static VsEntry *s_last_vs;
static ID3D11Buffer *s_ib, *s_prog_cb;
static ID3D11RasterizerState *s_raster_prog;
static uint32_t s_ib_pos;
static VsConsts s_prog_c;
static int s_have_prog_c;

static VsEntry *vs_find(uint64_t hash)
{
    VsEntry *e;
    for (e = s_vs_table[hash & (VS_BUCKETS - 1)]; e; e = e->next)
        if (e->hash == hash)
            return e;
    return NULL;
}

static int gpu_prepare_program(uint64_t hash, const uint32_t (*ins)[4], uint32_t count, uint32_t inputs)
{
    VsEntry *e = vs_find(hash);
    char *src;
    ID3DBlob *b;
    LARGE_INTEGER t0, t1, f;

    if (e)
        return e->vs != NULL;
    if (!s_dev)
        gpu_init();
    e = (VsEntry *)calloc(1, sizeof *e);
    e->hash = hash;
    e->next = s_vs_table[hash & (VS_BUCKETS - 1)];
    s_vs_table[hash & (VS_BUCKETS - 1)] = e;
    src = gpu_vsh_hlsl(ins, count, inputs);
    if (!src)
        return 0;
    QueryPerformanceCounter(&t0);
    b = compile(src, strlen(src), "vs_5_0");
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    free(src);
    if (!b)
        return 0;
    CHECK(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b),
                                          NULL, &e->vs));
    ID3D10Blob_Release(b);
    fprintf(stderr, "[gpu] vertex program %016llX: %u ins, inputs %04X, %.1f ms\n", (unsigned long long)hash, count,
            inputs, (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart);
    return 1;
}

/* One layout per set of inputs. */
typedef struct LayoutEntry {
    uint64_t key;
    ID3D11InputLayout *layout;
    struct LayoutEntry *next;
} LayoutEntry;

static LayoutEntry *s_layouts[64];

static ID3D11InputLayout *get_prog_layout(uint32_t streamed)
{
    LayoutEntry *e;
    D3D11_INPUT_ELEMENT_DESC el[16];
    char src[2048], *o = src;
    ID3DBlob *b;
    uint32_t a, n = 0;

    for (e = s_layouts[streamed & 63]; e; e = e->next)
        if (e->key == streamed)
            return e->layout;
    /* Input signatures match by semantic, so a stub shader with the same inputs serves every program. */
    o += sprintf(o, "struct I {");
    for (a = 0; a < 16; a++)
        if (streamed >> a & 1) {
            memset(&el[n], 0, sizeof el[n]);
            el[n].SemanticName = "V";
            el[n].SemanticIndex = a;
            el[n].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            el[n].AlignedByteOffset = n * 16;
            n++;
            o += sprintf(o, " float4 v%u : V%u;", a, a);
        }
    sprintf(o, " }; float4 main(I i) : SV_Position { return 0; }");
    b = compile(src, strlen(src), "vs_5_0");
    e = (LayoutEntry *)calloc(1, sizeof *e);
    e->key = streamed;
    CHECK(ID3D11Device_CreateInputLayout(s_dev, el, n, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b),
                                         &e->layout));
    ID3D10Blob_Release(b);
    e->next = s_layouts[streamed & 63];
    s_layouts[streamed & 63] = e;
    return e->layout;
}

static void gpu_prog_setup(void)
{
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;

    memset(&bd, 0, sizeof bd);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = IB_BYTES;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    CHECK(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_ib));
    bd.ByteWidth = sizeof(VsConsts);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    CHECK(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_prog_cb));
    ID3D11RasterizerState_GetDesc(s_raster, &rd);
    /* The near plane is SV_ClipDistance0; there is no far plane, as in the executor's clipper. */
    rd.DepthClipEnable = FALSE;
    CHECK(ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_raster_prog));
}

static void bind_prog_common(void)
{
    if (s_bound == 2)
        return;
    if (!s_ib)
        gpu_prog_setup();
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ib, DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_prog_cb);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 1, 1, &s_ps_cb);
    ID3D11DeviceContext_RSSetState(s_ctx, s_raster_prog);
    s_last_vs = NULL;
    s_bound = 2;
}

static void prog_draw(const Target *rt, const Nv2aProgramDraw *p)
{
    VsEntry *e = vs_find(p->hash);
    D3D11_MAPPED_SUBRESOURCE m;
    VsConsts c;
    uint32_t stride = p->slots * 16, vbytes = p->nverts * stride, ibytes = p->nindices * 4;
    uint32_t base, first;
    UINT offs = 0;

    if (!e || !e->vs || vbytes > VB_BYTES / 2 || ibytes > IB_BYTES / 2)
        return;
    c.vp[0] = 2.0f / (float)rt->lw;
    c.vp[1] = 2.0f / (float)rt->lh;
    c.vp[2] = 1.0f / 16777215.0f;
    c.vp[3] = p->near_z;
    c.aa[0] = p->aa[0];
    c.aa[1] = p->aa[1];
    c.aa[2] = 0;
    c.aa[3] = p->fog_abs ? 1.0f : 0.0f;
    memcpy(c.fg, p->fog, sizeof c.fg);
    memcpy(c.imm, p->imm, sizeof c.imm);
    memcpy(c.c, p->constants, sizeof c.c);
    if (!s_have_prog_c || memcmp(&c, &s_prog_c, sizeof c)) {
        CHECK(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_prog_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
        memcpy(m.pData, &c, sizeof c);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_prog_cb, 0);
        s_prog_c = c;
        s_have_prog_c = 1;
    }

    s_vb_pos = (s_vb_pos + stride - 1) / stride * stride;
    if (s_vb_pos + vbytes > VB_BYTES)
        s_vb_pos = 0;
    CHECK(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0,
                                  s_vb_pos ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, 0, &m));
    memcpy((uint8_t *)m.pData + s_vb_pos, p->verts, vbytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
    base = s_vb_pos / stride;
    s_vb_pos += vbytes;

    if (s_ib_pos + ibytes > IB_BYTES)
        s_ib_pos = 0;
    CHECK(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_ib, 0,
                                  s_ib_pos ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, 0, &m));
    memcpy((uint8_t *)m.pData + s_ib_pos, p->indices, ibytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ib, 0);
    first = s_ib_pos / 4;
    s_ib_pos += ibytes;

    if (e != s_last_vs) {
        ID3D11DeviceContext_VSSetShader(s_ctx, e->vs, NULL, 0);
        s_last_vs = e;
    }
    ID3D11DeviceContext_IASetInputLayout(s_ctx, get_prog_layout(p->inputs));
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &offs);
    ID3D11DeviceContext_DrawIndexed(s_ctx, p->nindices, first, (INT)base);
    s_st.draws++;
    s_st.tris += p->nindices / 3;
}

static PsConsts s_last_c;
static int s_have_last;
static ID3D11PixelShader *s_last_ps;
static ID3D11BlendState *s_last_blend;
static ID3D11DepthStencilState *s_last_depth;
static uint32_t s_last_bc, s_last_sr;

static void gpu_invalidate_state(void)
{
    s_have_last = 0;
    s_last_ps = NULL;
    s_last_blend = NULL;
    s_last_depth = NULL;
}

/* DBG: while logs/dump.now exists, log one frame's draws and save each surface it touched. */
static int s_dump;
static FILE *s_dump_log;

/* DBG: RECOMP_GPU_TRAP logs every frame to two files in turn and keeps them when a frame goes black.
 * s_dump_arm asks for a full dump named by s_dump_pre. */
static int s_trap = -1, s_trap_hit, s_trap_n, s_dump_arm, s_dump_full;
static char s_dump_pre[32];

static void dump_begin(void)
{
    char a[96];
    if (s_trap < 0)
        s_trap = getenv("RECOMP_GPU_TRAP") != NULL;
    if (s_dump_arm) {
        s_dump_arm = 0;
        s_dump_full = 1;
        sprintf(a, "H:/spyro-xbox/logs/dump/%sframe.txt", s_dump_pre);
        s_dump_log = fopen(a, "w");
        s_dump = s_dump_log != NULL;
        return;
    }
    s_dump_full = 0;
    if (s_trap) {
        s_dump_log = fopen((s_frame & 1) ? "H:/spyro-xbox/logs/dump/t1.txt" : "H:/spyro-xbox/logs/dump/t0.txt", "w");
        s_dump = s_dump_log != NULL;
        return;
    }
    if (GetFileAttributesA("H:/spyro-xbox/logs/dump.now") == INVALID_FILE_ATTRIBUTES)
        return;
    DeleteFileA("H:/spyro-xbox/logs/dump.now");
    s_dump_full = 1;
    s_dump_pre[0] = 0;
    s_dump_log = fopen("H:/spyro-xbox/logs/dump/frame.txt", "w");
    s_dump = s_dump_log != NULL;
}

static void dump_target(const Target *t)
{
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D *st;
    D3D11_MAPPED_SUBRESOURCE m;
    char name[128];
    FILE *f;
    uint8_t h[18] = { 0 };
    uint32_t y;

    ID3D11Texture2D_GetDesc(t->tex, &d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    d.MipLevels = 1;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &st)))
        return;
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, (ID3D11Resource *)t->tex);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        sprintf(name, "H:/spyro-xbox/logs/dump/%s%08X.tga", s_dump_pre, t->va);
        f = fopen(name, "wb");
        if (f) {
            h[2] = 2;
            h[12] = (uint8_t)d.Width; h[13] = (uint8_t)(d.Width >> 8);
            h[14] = (uint8_t)d.Height; h[15] = (uint8_t)(d.Height >> 8);
            h[16] = 32; h[17] = 0x20;
            fwrite(h, 1, 18, f);
            for (y = 0; y < d.Height; y++)
                fwrite((uint8_t *)m.pData + y * m.RowPitch, 4, d.Width, f);
            fclose(f);
        }
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
}

/* DBG: RECOMP_GPU_LUMA watches presented brightness; a jump logs the frame and saves the surface. */
static void luma_check(void)
{
    static int on = -1, saved;
    static ID3D11Texture2D *st;
    static float prev;
    D3D11_TEXTURE2D_DESC d;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_BOX box;
    float mean = 0;
    uint32_t x, n = 0;
    int i;

    if (on < 0)
        on = getenv("RECOMP_GPU_LUMA") != NULL;
    if (!on || !s_present)
        return;
    ID3D11Texture2D_GetDesc(s_present->tex, &d);
    if (!st) {
        D3D11_TEXTURE2D_DESC o = d;
        o.Height = 16; o.Usage = D3D11_USAGE_STAGING; o.BindFlags = 0;
        o.CPUAccessFlags = D3D11_CPU_ACCESS_READ; o.MiscFlags = 0; o.MipLevels = 1;
        CHECK(ID3D11Device_CreateTexture2D(s_dev, &o, NULL, &st));
    }
    box.left = 0; box.right = d.Width < 1280 ? d.Width : 1280;
    box.top = d.Height * 2 / 3; box.bottom = box.top + 16; box.front = 0; box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)st, 0, 0, 0, 0, (ID3D11Resource *)s_present->tex, 0, &box);
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m)))
        return;
    for (x = 0; x < box.right; x += 7, n++) {
        const uint8_t *q = (const uint8_t *)m.pData + 8 * m.RowPitch + x * 4;
        mean += (q[0] + q[1] + q[2]) / 3.0f;
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
    mean /= (float)n;
    if (prev > 0 && (mean > prev * 1.35f + 4 || mean < prev / 1.35f - 4)) {
        fprintf(stderr, "[luma] frame %u present %08X %ux%u mean %.1f prev %.1f:", s_frame, s_present->va, s_present->lw, s_present->lh, mean, prev);
        for (i = 0; i < s_nrt; i++)
            if (s_rts[i].frame == s_frame)
                fprintf(stderr, " %08X", s_rts[i].va);
        fputc('\n', stderr);
        if (saved < 8) {
            Target t = *s_present;
            t.va = s_frame;
            dump_target(&t);
            saved++;
        }
    }
    if (s_trap > 0 && s_trap_n < 6 && mean < 1 && prev > 5)
        s_trap_hit = 1;
    prev = mean;
}

/* DBG: the blurred result that gets added to the frame; two dumps a second and a half apart when it lights up. */
static void bloom_check(void)
{
    static const uint32_t vas[] = { 0x81069C00, 0x810BCA00, 0x810CF700, 0x810EF900, 0x810F3A00, 0x81B6D900, 0x81A01D00, 0x819C1C00 };
    static uint32_t next_dump;
    static int n;
    float top = 0;
    int v, i;

    if (s_trap <= 0 || s_frame % 30)
        return;
    fprintf(stderr, "[bloom] frame %u:", s_frame);
    for (v = 0; v < 8; v++) {
        Target *t = NULL;
        D3D11_TEXTURE2D_DESC d;
        ID3D11Texture2D *st;
        D3D11_MAPPED_SUBRESOURCE m;
        uint32_t x, y;
        float mean = 0;
        for (i = 0; i < s_nrt; i++)
            if (s_rts[i].va == vas[v])
                t = &s_rts[i];
        if (!t)
            continue;
        ID3D11Texture2D_GetDesc(t->tex, &d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &d, NULL, &st)))
            continue;
        ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)st, (ID3D11Resource *)t->tex);
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
            for (y = 0; y < d.Height; y += 8)
                for (x = 0; x < d.Width; x += 8) {
                    const uint8_t *q = (const uint8_t *)m.pData + y * m.RowPitch + x * 4;
                    mean += (q[0] + q[1] + q[2]) / 3.0f;
                }
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)st, 0);
            mean /= (float)((d.Height + 7) / 8 * ((d.Width + 7) / 8));
        }
        ID3D11Texture2D_Release(st);
        fprintf(stderr, " %08X=%.1f%s", vas[v], mean, t->frame == s_frame ? "" : "(old)");
        if (v < 5 && mean > top)
            top = mean;
    }
    fputc('\n', stderr);
    if (n < 4 && top > 1 && s_frame >= next_dump) {
        sprintf(s_dump_pre, "b%d_", n++);
        s_dump_arm = 1;
        next_dump = s_frame + 90;
    }
}

static void dump_end(void)
{
    int i;
    if (!s_dump)
        return;
    fprintf(s_dump_log, "present %08X\n", s_present ? s_present->va : 0);
    if (!s_dump_full) {
        const char *cur = (s_frame & 1) ? "H:/spyro-xbox/logs/dump/t1.txt" : "H:/spyro-xbox/logs/dump/t0.txt";
        const char *was = (s_frame & 1) ? "H:/spyro-xbox/logs/dump/t0.txt" : "H:/spyro-xbox/logs/dump/t1.txt";
        fclose(s_dump_log);
        s_dump_log = NULL;
        s_dump = 0;
        if (s_trap_hit) {
            char a[96];
            s_trap_hit = 0;
            sprintf(a, "H:/spyro-xbox/logs/dump/hit%u_cur.txt", s_trap_n);
            CopyFileA(cur, a, FALSE);
            sprintf(a, "H:/spyro-xbox/logs/dump/hit%u_prev.txt", s_trap_n);
            CopyFileA(was, a, FALSE);
            fprintf(stderr, "[trap] hit %u at frame %u\n", s_trap_n, s_frame);
            s_trap_n++;
        }
        return;
    }
    for (i = 0; i < s_nrt; i++)
        if (s_rts[i].frame == s_frame) {
            fprintf(s_dump_log, "target %08X %ux%u pitch %u bpp %u\n", s_rts[i].va, s_rts[i].lw, s_rts[i].lh,
                    s_rts[i].pitch, s_rts[i].bpp);
            dump_target(&s_rts[i]);
        }
    fclose(s_dump_log);
    s_dump_log = NULL;
    s_dump = 0;
}

static void dump_draw(const Target *rt, const Depth *ds, const Nv2aBatch *b)
{
    const Nv2aRenderState *r = b->state;
    const Nv2aVertex *v = b->vertices;
    static const Nv2aVertex none;
    int i;
    if (b->program) v = &none;
    fprintf(s_dump_log, "draw rt %08X ds %08X n %u p0 %.1f %.1f z %.1f rhw %.4f c %08X t", rt->va, ds ? ds->va : 0,
            b->count, v[0].x, v[0].y, v[0].z, v[0].rhw, v[0].diffuse);
    for (i = 0; i < 4; i++)
        fprintf(s_dump_log, " %08X", b->stage[i] ? b->stage[i]->offset : (b->texture && !i ? b->texture->offset : 0));
    fprintf(s_dump_log, " bl %u %X %X zt %u f %X w %u at %u f %X r %u cm %X cull %u st %u rc %d prog %X sf %X ref %X m %X wm %X ops %X %X %X\n",
            r->blend_enable, r->blend_src, r->blend_dst,
            r->depth_test_enable, r->depth_func, r->depth_write, r->alpha_test_enable, r->alpha_func, r->alpha_ref,
            r->color_mask, r->cull_enable, r->stencil_enable, b->rc != NULL, b->rc ? b->rc->stage_program : 0,
            r->stencil_func, r->stencil_ref, r->stencil_func_mask, r->stencil_write_mask,
            r->stencil_fail, r->stencil_zfail, r->stencil_zpass);
}

static void gpu_draw_on(Target *rt, Depth *ds, const Nv2aBatch *b)
{
    const Nv2aRenderState *r = b->state;
    PsConsts c;
    PsKey key;
    ID3D11PixelShader *ps;
    ID3D11BlendState *bs;
    ID3D11DepthStencilState *dss;
    ID3D11ShaderResourceView *srv[4];
    ID3D11SamplerState *smp[4];
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t bytes = b->count * sizeof(Nv2aVertex), first, bc;
    int st, presentable = 1;

    if (b->program)
        bind_prog_common();
    else
        bind_common();
    bind_targets(rt, ds);
    rt->dbg_draws++; rt->dbg_tris += (b->program ? b->program->nindices : b->count) / 3; /* DBG */
    if (s_dump) dump_draw(rt, ds, b);
    rt->frame = s_frame;
    if (presentable && rt->lw * rt->lh >= s_present_area) {
        s_present_area = rt->lw * rt->lh;
        s_present = rt;
    }

    memset(&key, 0, sizeof key);
    memset(&c, 0, sizeof c);
    if (b->rc) {
        int i;
        key.rc = *b->rc;
        for (i = 0; i < 8; i++) {
            nv2a_rc_unpack(b->rc->factor0[i], c.c0[i]);
            nv2a_rc_unpack(b->rc->factor1[i], c.c1[i]);
        }
        nv2a_rc_unpack(b->rc->final_c0, c.fc0);
        nv2a_rc_unpack(b->rc->final_c1, c.fc1);
        memset(key.rc.factor0, 0, sizeof key.rc.factor0);
        memset(key.rc.factor1, 0, sizeof key.rc.factor1);
        key.rc.final_c0 = key.rc.final_c1 = 0;
        key.clip_mode = b->clip_plane_mode;
        key.flags = 1;
    } else if (b->texture) {
        key.flags = 2;
    }
    c.fog[0] = b->fog_color[0];
    c.fog[1] = b->fog_color[1];
    c.fog[2] = b->fog_color[2];
    c.misc[0] = (float)(r->alpha_ref & 0xFF);
    key.alpha_func = r->alpha_test_enable ? (r->alpha_func ? r->alpha_func : 0x207) : 0;

    for (st = 0; st < 4; st++) {
        uint32_t mode = b->rc ? (b->rc->stage_program >> (st * 5)) & 0x1F : (st == 0 && b->texture ? 1 : 0);
        const Nv2aTexture *t = b->rc ? b->stage[st] : (st == 0 ? b->texture : NULL);
        int fetches = mode >= 1 && mode <= 3;
        srv[st] = s_white;
        smp[st] = s_blit_sampler;
        c.st[st][0] = c.st[st][1] = 1.0f;
        if (!fetches)
            continue;
        if (!t)
            continue;               /* the executor's white */
        smp[st] = get_sampler(t);
        {
            float ox = 0, oy = 0;
            Target *src = t->cube ? NULL : rt_for_texture(t, &ox, &oy);
            if (src) {
                float sw = t->normalized ? (float)t->width : 1.0f;
                float sh = t->normalized ? (float)t->height : 1.0f;
                if (src == rt) {
                    /* Sampling the surface being drawn: read the pixels as they stand. */
                    if (!src->copy)
                        make_color(src->lw * s_scale, src->lh * s_scale, &src->copy, NULL, &src->copy_srv);
                    unbind_srvs();
                    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)src->copy, (ID3D11Resource *)src->tex);
                    srv[st] = src->copy_srv;
                } else {
                    srv[st] = src->srv;
                }
                c.st[st][0] = sw / (float)src->lw;
                c.st[st][1] = sh / (float)src->lh;
                c.st[st][2] = ox / (float)src->lw;
                c.st[st][3] = oy / (float)src->lh;
            } else {
                TexEntry *e = get_tex(t);
                srv[st] = tex_srv(e);
                if (t->cube)
                    key.cube_mask |= 1u << st;
                if (!t->normalized) {
                    c.st[st][0] = 1.0f / (float)t->width;
                    c.st[st][1] = 1.0f / (float)t->height;
                }
            }
        }
    }

    for (st = 0; st < 4; st++)
        if (srv[st] != s_srv_bound[st]) {
            ID3D11DeviceContext_PSSetShaderResources(s_ctx, st, 1, &srv[st]);
            s_srv_bound[st] = srv[st];
        }
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, smp);

    if (!s_have_last || memcmp(&c, &s_last_c, sizeof c)) {
        CHECK(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_ps_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
        memcpy(m.pData, &c, sizeof c);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ps_cb, 0);
        s_last_c = c;
        s_have_last = 1;
    }

    ps = get_ps(&key);
    if (ps != s_last_ps) {
        ID3D11DeviceContext_PSSetShader(s_ctx, ps, NULL, 0);
        s_last_ps = ps;
    }
    bs = get_blend(r);
    bc = r->blend_color;
    if (bs != s_last_blend || bc != s_last_bc) {
        FLOAT f[4];
        float u[4];
        nv2a_rc_unpack(bc, u);
        f[0] = u[0]; f[1] = u[1]; f[2] = u[2]; f[3] = u[3];
        if (r->blend_src == 0x8003 || r->blend_src == 0x8004 || r->blend_dst == 0x8003 || r->blend_dst == 0x8004)
            f[0] = f[1] = f[2] = f[3];
        ID3D11DeviceContext_OMSetBlendState(s_ctx, bs, f, 0xFFFFFFFF);
        s_last_blend = bs;
        s_last_bc = bc;
    }
    dss = get_depth(r);
    if (dss != s_last_depth || r->stencil_ref != s_last_sr) {
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, dss, r->stencil_ref & 0xFF);
        s_last_depth = dss;
        s_last_sr = r->stencil_ref;
    }

    if (b->program) {
        prog_draw(rt, b->program);
        return;
    }
    if (bytes > VB_BYTES)
        return;
    s_vb_pos = (s_vb_pos + sizeof(Nv2aVertex) - 1) / sizeof(Nv2aVertex) * sizeof(Nv2aVertex);
    if (s_vb_pos + bytes > VB_BYTES)
        s_vb_pos = 0;
    CHECK(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0,
                                  s_vb_pos ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, 0, &m));
    memcpy((uint8_t *)m.pData + s_vb_pos, b->vertices, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
    first = s_vb_pos / sizeof(Nv2aVertex);
    s_vb_pos += bytes;
    ID3D11DeviceContext_Draw(s_ctx, b->count, first);
    s_st.draws++;
    s_st.tris += b->count / 3;
}

static int64_t s_tm_draw, s_tm_present, s_tm_pace, s_tm_flip_end, s_tm_outside; /* DBG */
static int64_t tm_now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

/* DBG: game thread's kickoff spin, and the GPU thread's passes. */
static __declspec(thread) int64_t s_kick_t0;
static volatile LONG64 s_kick_ticks, s_kick_max;
static volatile LONG s_kicks, s_passes;
static int64_t s_pass_last;
static volatile LONG64 s_pass_max;

void dbg_kick(int begin)
{
    if (begin) {
        s_kick_t0 = tm_now();
    } else {
        int64_t d = tm_now() - s_kick_t0;
        InterlockedAdd64(&s_kick_ticks, d);
        InterlockedIncrement(&s_kicks);
        if (d > s_kick_max) s_kick_max = d;
    }
}

void dbg_pass(void)
{
    /* The clock every 16th pass only: there are tens of thousands a frame, and
     * reading it on each was most of a tenth of the GPU thread. The longest
     * pass is then measured over 16, which still shows a stall. */
    if ((InterlockedIncrement(&s_passes) & 15) == 0) {
        int64_t n = tm_now();
        if (s_pass_last && n - s_pass_last > s_pass_max) s_pass_max = n - s_pass_last;
        s_pass_last = n;
    }
}

static void gpu_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    Target *rt;
    int64_t t0 = tm_now();
    if (!s_dev)
        gpu_init();
    rt = get_rt(s);
    gpu_draw_on(rt, b->state->zeta_va ? get_ds(b->state->zeta_va, rt->lw, rt->lh) : NULL, b);
    s_tm_draw += tm_now() - t0;
}

static void gpu_clear(const Nv2aSurface *s, const Nv2aRenderState *rs, uint32_t flags,
                      uint32_t argb, uint32_t zstencil)
{
    uint32_t x0 = rs->clear_rect_h & 0xFFFF, x1 = rs->clear_rect_h >> 16;
    uint32_t y0 = rs->clear_rect_v & 0xFFFF, y1 = rs->clear_rect_v >> 16;
    Target *rt;
    Depth *ds;
    float depth, stencil;
    int full;

    if (!s_dev)
        gpu_init();
    if (s_dump) fprintf(s_dump_log, "clear rt %08X zeta %08X flags %X argb %08X zs %08X rect %08X %08X bits %u\n", s->color_va, rs->zeta_va,
                        flags, argb, zstencil, rs->clear_rect_h, rs->clear_rect_v, rs->depth_bits);
    rt = get_rt(s);
    ds = (rs->zeta_va && (flags & 3)) ? get_ds(rs->zeta_va, rt->lw, rt->lh) : NULL;
    if (!rs->clear_rect_h && !rs->clear_rect_v) {
        x1 = rt->lw - 1;
        y1 = rt->lh - 1;
    }
    full = x0 == 0 && y0 == 0 && x1 + 1 >= rt->lw && y1 + 1 >= rt->lh;
    if (rs->depth_bits == 16) {
        depth = (float)(zstencil & 0xFFFF) * 256.0f;
        stencil = 0;
    } else {
        depth = (float)(zstencil >> 8);
        stencil = (float)(zstencil & 0xFF);
    }

    if (full && ((flags & 0xF0) == 0xF0 || !(flags & 0xF0))) {
        if (flags & 0xF0) {
            FLOAT c[4];
            float u[4];
            nv2a_rc_unpack(argb, u);
            c[0] = u[0]; c[1] = u[1]; c[2] = u[2]; c[3] = u[3];
            ID3D11DeviceContext_ClearRenderTargetView(s_ctx, rt->rtv, c);
            rt->frame = s_frame;
            if (rt->lw * rt->lh >= s_present_area) {
                s_present_area = rt->lw * rt->lh;
                s_present = rt;
            }
        }
        if (ds && (flags & 3)) {
            UINT f = ((flags & 1) ? D3D11_CLEAR_DEPTH : 0) | ((flags & 2) ? D3D11_CLEAR_STENCIL : 0);
            ID3D11DeviceContext_ClearDepthStencilView(s_ctx, ds->dsv, f, depth / 16777215.0f, (UINT8)stencil);
        }
        return;
    }

    /* A partial clear is a quad through the ordinary draw path. */
    {
        Nv2aVertex v[6];
        Nv2aRenderState q;
        Nv2aBatch b;
        float l = (float)x0, t = (float)y0, r = (float)(x1 + 1), bt = (float)(y1 + 1);
        int i;

        memset(v, 0, sizeof v);
        for (i = 0; i < 6; i++) {
            v[i].x = (i == 1 || i == 2 || i == 4) ? r : l;
            v[i].y = (i == 2 || i == 4 || i == 5) ? bt : t;
            v[i].z = depth;
            v[i].rhw = 1.0f;
            v[i].diffuse = argb;
            v[i].fog = 1.0f;
        }
        memset(&q, 0, sizeof q);
        q.color_mask = ((flags & 0x10) ? 0x00010000u : 0) | ((flags & 0x20) ? 0x00000100u : 0)
                     | ((flags & 0x40) ? 0x00000001u : 0) | ((flags & 0x80) ? 0x01000000u : 0);
        q.zeta_va = ds ? rs->zeta_va : 0;
        q.depth_test_enable = (flags & 1) != 0;
        q.depth_func = 0x207;
        q.depth_write = (flags & 1) != 0;
        q.stencil_enable = (flags & 2) != 0;
        q.stencil_func = 0x207;
        q.stencil_ref = (uint32_t)stencil;
        q.stencil_func_mask = 0xFF;
        q.stencil_write_mask = 0xFF;
        q.stencil_fail = q.stencil_zfail = 0x1E00;
        q.stencil_zpass = 0x1E01;
        memset(&b, 0, sizeof b);
        b.vertices = v;
        b.count = 6;
        b.state = &q;
        gpu_draw_on(rt, ds, &b);
    }
}

static void gpu_flip(void)
{
    static LARGE_INTEGER freq, t0;
    static uint32_t frames;
    LARGE_INTEGER now;
    RECT rc;
    D3D11_VIEWPORT vp;
    FLOAT black[4] = { 0, 0, 0, 1 };

    if (!s_dev)
        gpu_init();
    if (s_tm_flip_end) s_tm_outside += tm_now() - s_tm_flip_end;
    pump();
    if (s_resize) {
        s_resize = 0;
        ID3D11RenderTargetView_Release(s_back_rtv);
        s_back_rtv = NULL;
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
        s_cur_rt = NULL;
        ID3D11DeviceContext_Flush(s_ctx);
        CHECK(IDXGISwapChain1_ResizeBuffers(s_swap, 0, 0, 0, DXGI_FORMAT_UNKNOWN,
                                            s_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
        make_back_view();
    }
    luma_check();
    bloom_check();
    dump_end();
    dump_begin();
    GetClientRect(s_hwnd, &rc);

    unbind_srvs();
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_back_rtv, NULL);
    s_cur_rt = NULL;
    s_cur_ds = NULL;
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_back_rtv, black);
    if (s_present && rc.right > 0 && rc.bottom > 0) {
        float cw = (float)rc.right, ch = (float)rc.bottom;
        float want = s_widescreen ? 16.0f / 9.0f : (float)s_present->lw / (float)s_present->lh;
        float vw = cw / ch > want ? ch * want : cw, vh = cw / ch > want ? ch : cw / want;
        D3D11_MAPPED_SUBRESOURCE m;
        float *u;

        vp.TopLeftX = (cw - vw) * 0.5f;
        vp.TopLeftY = (ch - vh) * 0.5f;
        vp.Width = vw;
        vp.Height = vh;
        vp.MinDepth = 0;
        vp.MaxDepth = 1;
        ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
        CHECK(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_ps_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
        u = (float *)m.pData;
        u[0] = u[1] = 1.0f;
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ps_cb, 0);
        ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
        ID3D11DeviceContext_VSSetShader(s_ctx, s_blit_vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(s_ctx, s_blit_ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_ps_cb);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &s_present->srv);
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_blit_sampler);
        ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFF);
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
        ID3D11DeviceContext_RSSetState(s_ctx, s_raster);
        ID3D11DeviceContext_Draw(s_ctx, 3, 0);
        unbind_srvs();
    }
    dbg_shot();
    { int64_t t_ = tm_now(); IDXGISwapChain1_Present(s_swap, g_settings.vsync ? 1 : 0, !g_settings.vsync && s_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0); s_tm_present += tm_now() - t_; s_tm_pace -= tm_now(); }

    /* The console's flip waits for vblank, and that wait is what paces the title: holding this thread
     * holds the pushbuffer behind the flip. Sleep most of it and spin the rest. */
    {
        static LARGE_INTEGER pf, next;
        LARGE_INTEGER n;
        int64_t step;

        if (!pf.QuadPart) {
            QueryPerformanceFrequency(&pf);
            timeBeginPeriod(1);
        }
        step = pf.QuadPart * 1001 / 60000;
        QueryPerformanceCounter(&n);
        /* Behind already: start the schedule again from now, with no wait on top. */
        if (!next.QuadPart || n.QuadPart - next.QuadPart > step)
            next.QuadPart = n.QuadPart - step;
        next.QuadPart += step;
        while (n.QuadPart < next.QuadPart) {
            int64_t left_ms = (next.QuadPart - n.QuadPart) * 1000 / pf.QuadPart;
            if (left_ms > 2)
                Sleep((DWORD)(left_ms - 2));
            else
                YieldProcessor();
            QueryPerformanceCounter(&n);
        }
    }

    s_tm_pace += tm_now();
    s_tm_flip_end = tm_now();
    { /* DBG */ int i_; if (!(s_frame % 300)) { fprintf(stderr, "[gpt] frame %u present %08X:", s_frame, s_present ? s_present->va : 0);
      for (i_ = 0; i_ < s_nrt; i_++) { fprintf(stderr, " %08X=%ud/%ut", s_rts[i_].va, s_rts[i_].dbg_draws, s_rts[i_].dbg_tris); }
      fprintf(stderr, "\n"); }
      if (!(s_frame % 300) || 1) for (i_ = 0; i_ < s_nrt; i_++) { if (!(s_frame % 300)) s_rts[i_].dbg_draws = s_rts[i_].dbg_tris = 0; } }
    /* Everything the blit changed comes back on the next draw. */
    s_bound = 0;
    gpu_invalidate_state();
    s_present_area = 0;
    s_frame++;
    frames++;
    if ((s_frame & 0xFFF) == 0)
        tex_sweep();

    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
    }
    QueryPerformanceCounter(&now);
    if (now.QuadPart - t0.QuadPart >= freq.QuadPart * 5) {
        double secs = (double)(now.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        { double k_ = 1000.0 / (double)freq.QuadPart / (frames ? frames : 1);
          fprintf(stderr, "[tm] ms/frame: outside-flip %.1f (draw %.1f) present %.1f pace %.1f\n", (double)s_tm_outside * k_, (double)s_tm_draw * k_, (double)s_tm_present * k_, (double)s_tm_pace * k_);
          fprintf(stderr, "[tk] kicks/frame %.1f spin ms/frame %.1f longest %.1f ms; passes/frame %.1f longest %.1f ms\n",
                  (double)s_kicks / (frames ? frames : 1), (double)s_kick_ticks * k_,
                  (double)s_kick_max * 1000.0 / (double)freq.QuadPart, (double)s_passes / (frames ? frames : 1),
                  (double)s_pass_max * 1000.0 / (double)freq.QuadPart);
          s_kicks = s_passes = 0; s_kick_ticks = s_kick_max = s_pass_max = 0;
          s_tm_outside = s_tm_draw = s_tm_present = s_tm_pace = 0; }
        fflush(stderr);
        fprintf(stderr, "[gpu] %.1f fps, per frame: %u draws %u tris %u tex-checks; uploads %u (%u MB) new shaders %u\n",
                frames / secs, s_st.draws / (frames ? frames : 1), s_st.tris / (frames ? frames : 1),
                s_st.rehash / (frames ? frames : 1), s_st.uploads, s_st.bytes >> 20, s_st.shaders);
        memset(&s_st, 0, sizeof s_st);
        frames = 0;
        t0 = now;
    }
}

static const Nv2aBackend s_backend = { gpu_clear, gpu_draw, gpu_flip, gpu_prepare_program };

void gpu_backend_register(void)
{
    nv2a_backend_register(&s_backend);
}
