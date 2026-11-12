/*
 * Button prompts for the keyboard and mouse.
 *
 * The game draws its prompts ("Press A to jump") from a font of controller
 * glyphs, font_buttons0, one 256x128 texture. While the keyboard or mouse
 * was the last thing the player touched, gpu_backend.c samples a sheet built
 * here instead: the same glyph cells, each holding the key or mouse button
 * mkb.ini binds to that control. The cells come from font_buttons0.fnt, so
 * the game's own spacing still fits.
 *
 * The sheet is drawn at the render scale, so it stays sharp. Text comes from
 * GDI into a scratch bitmap and is used as coverage; the key caps and mouse
 * are drawn with distance functions for smooth edges.
 */
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mkb_input.h"
#include "prompts.h"
#include "settings.h"

#define SHEET_W 256
#define SHEET_H 128

/* Glyph cells of font_buttons0, in texels, and what each one shows. */
enum { SHOW_CONTROL, SHOW_MOVE, SHOW_CAMERA, SHOW_DPAD };
static const struct { int x0, y0, x1, y1, show, control; } CELLS[] = {
    {  32, 71,  61, 104, SHOW_CONTROL, MKB_WHITE },   /* ! */
    {  65,  1, 103,  34, SHOW_CONTROL, MKB_LT },      /* " */
    {   1, 36,  35,  69, SHOW_CONTROL, MKB_DUP },     /* # */
    {  36, 36,  70,  69, SHOW_CONTROL, MKB_DRIGHT },  /* $ */
    { 104,  1, 138,  34, SHOW_CONTROL, MKB_DDOWN },   /* % */
    { 139,  1, 173,  34, SHOW_CONTROL, MKB_DLEFT },   /* & */
    { 204, 36, 234,  69, SHOW_CONTROL, MKB_Y },       /* ' */
    {  62, 71,  91, 104, SHOW_CONTROL, MKB_B },       /* ( */
    { 173, 36, 203,  69, SHOW_CONTROL, MKB_A },       /* ) */
    {   1, 71,  31, 104, SHOW_CONTROL, MKB_X },       /* * */
    {  92, 71, 120, 104, SHOW_CONTROL, MKB_BLACK },   /* + */
    {  26,  1,  64,  34, SHOW_CONTROL, MKB_RT },      /* , */
    { 174,  1, 208,  34, SHOW_CONTROL, MKB_BACK },    /* - */
    { 105, 36, 138,  69, SHOW_CONTROL, MKB_START },   /* / */
    { 139, 36, 172,  69, SHOW_MOVE, 0 },              /* 0: left stick */
    {  71, 36, 104,  69, SHOW_CAMERA, 0 },            /* 1: right stick */
    { 209,  1, 243,  34, SHOW_DPAD, 0 },              /* 2: the whole D-pad */
};

/* What a cell says on its key cap: short forms of the longer names. */
static const char *cap_text(const char *label)
{
    static const struct { const char *name, *text; } SHORT_NAMES[] = {
        { "Backspace", "Bksp" }, { "CapsLock", "Caps" }, { "PageUp", "PgUp" },
        { "PageDown", "PgDn" }, { "Insert", "Ins" }, { "Delete", "Del" },
        { "Escape", "Esc" }, { "LShift", "Shift" }, { "RShift", "Shift" },
        { "LCtrl", "Ctrl" }, { "RCtrl", "Ctrl" }, { "LAlt", "Alt" }, { "RAlt", "Alt" },
        { "Up", "\xE2\x86\x91" }, { "Down", "\xE2\x86\x93" }, { "Left", "\xE2\x86\x90" },
        { "Right", "\xE2\x86\x92" },
    };
    size_t i;
    for (i = 0; i < sizeof SHORT_NAMES / sizeof *SHORT_NAMES; i++)
        if (!strcmp(label, SHORT_NAMES[i].name))
            return SHORT_NAMES[i].text;
    return label;
}

/* ---- drawing --------------------------------------------------------- */

typedef struct { uint32_t *px; int w, h; } Canvas;

static void blend(Canvas *c, int x, int y, uint32_t rgb, float a)
{
    uint32_t *p, d;
    float da, oa;
    int i;
    if (x < 0 || y < 0 || x >= c->w || y >= c->h || a <= 0.0f)
        return;
    if (a > 1.0f)
        a = 1.0f;
    p = &c->px[(size_t)y * c->w + x];
    d = *p;
    da = (float)(d >> 24) / 255.0f;
    oa = a + da * (1.0f - a);
    if (oa <= 0.0f)
        return;
    {
        uint32_t out = 0;
        for (i = 0; i < 3; i++) {
            float s = (float)((rgb >> (i * 8)) & 0xFF), t = (float)((d >> (i * 8)) & 0xFF);
            float v = (s * a + t * da * (1.0f - a)) / oa;
            out |= (uint32_t)(v + 0.5f) << (i * 8);
        }
        *p = out | (uint32_t)(oa * 255.0f + 0.5f) << 24;
    }
}

/* Signed distance to a rounded rectangle (negative inside). */
static float sd_round_rect(float px, float py, float cx, float cy, float hw, float hh, float r)
{
    float qx = fabsf(px - cx) - hw + r, qy = fabsf(py - cy) - hh + r;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

static void fill_round_rect(Canvas *c, float x0, float y0, float x1, float y1, float r, uint32_t rgb, float alpha)
{
    int x, y;
    float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, hw = (x1 - x0) / 2, hh = (y1 - y0) / 2;
    for (y = (int)y0 - 1; y <= (int)y1 + 1; y++)
        for (x = (int)x0 - 1; x <= (int)x1 + 1; x++) {
            float d = sd_round_rect(x + 0.5f, y + 0.5f, cx, cy, hw, hh, r);
            blend(c, x, y, rgb, (0.5f - d) * alpha);
        }
}

/* Text (UTF-8) centred in a box, as large as fits. */
static void draw_text(Canvas *c, const char *text, int bx, int by, int bw, int bh, uint32_t rgb)
{
    BITMAPINFO bi;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp;
    HFONT font = NULL;
    uint32_t *bits = NULL;
    SIZE sz = { 0, 0 };
    wchar_t wtext[32];
    int h, x, y, n;

    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = bw;
    bi.bmiHeader.biHeight = -bh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (!bmp || !bits) {
        DeleteDC(dc);
        return;
    }
    SelectObject(dc, bmp);
    n = MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext, 32) - 1;
    for (h = bh * 4 / 5; h >= 6 && n > 0; h--) {
        font = CreateFontW(-h, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        SelectObject(dc, font);
        GetTextExtentPoint32W(dc, wtext, n, &sz);
        if (sz.cx <= bw)
            break;
        SelectObject(dc, GetStockObject(SYSTEM_FONT));
        DeleteObject(font);
        font = NULL;
    }
    if (font) {
        SetBkColor(dc, RGB(0, 0, 0));
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkMode(dc, OPAQUE);
        TextOutW(dc, (bw - sz.cx) / 2, (bh - sz.cy) / 2, wtext, n);
        GdiFlush();
        for (y = 0; y < bh; y++)
            for (x = 0; x < bw; x++) {
                float a = (float)(bits[(size_t)y * bw + x] & 0xFF) / 255.0f;
                if (a > 0)
                    blend(c, bx + x, by + y, rgb, a);
            }
        SelectObject(dc, GetStockObject(SYSTEM_FONT));
        DeleteObject(font);
    }
    DeleteObject(bmp);
    DeleteDC(dc);
}

#define CAP_FILL   0x2A2C33u
#define CAP_EDGE   0xD8DCE6u
#define CAP_TEXT   0xFFFFFFu
#define ACCENT     0xFF9B1Eu   /* the orange of the game's D-pad arrows */

static void draw_cap(Canvas *c, float x0, float y0, float x1, float y1, const char *text)
{
    float s = (y1 - y0) / 32.0f;            /* one texel of the original sheet */
    float r = 6.0f * s, edge = 2.0f * s;
    fill_round_rect(c, x0 + s, y0 + 3 * s, x1 - s, y1 - s, r, 0x000000u, 0.45f);        /* shadow */
    fill_round_rect(c, x0, y0, x1 - 2 * s, y1 - 4 * s, r, CAP_EDGE, 1.0f);
    fill_round_rect(c, x0 + edge, y0 + edge, x1 - 2 * s - edge, y1 - 4 * s - edge, r - edge, CAP_FILL, 1.0f);
    draw_text(c, cap_text(text), (int)(x0 + edge * 2), (int)(y0 + edge * 2), (int)(x1 - x0 - 2 * s - edge * 4),
              (int)(y1 - y0 - 4 * s - edge * 4), CAP_TEXT);
}

/* A mouse seen from above; button 1-3 lit, 0 for none (the camera), 9 for
 * the wheel. */
static void draw_mouse(Canvas *c, float x0, float y0, float x1, float y1, int button)
{
    float s = (y1 - y0) / 32.0f;
    float h = y1 - y0 - 2 * s, w = h * 0.68f;
    float cx = (x0 + x1) / 2, top = y0 + s, bot = top + h, mid = top + h * 0.45f;
    float edge = 2.0f * s;
    int x, y;

    fill_round_rect(c, cx - w / 2, top, cx + w / 2, bot, w / 2, CAP_EDGE, 1.0f);
    fill_round_rect(c, cx - w / 2 + edge, top + edge, cx + w / 2 - edge, bot - edge, w / 2 - edge, CAP_FILL, 1.0f);
    /* Light the button: the part of the body above the split, on its side. */
    if (button == 1 || button == 2) {
        for (y = (int)top; y < (int)mid; y++)
            for (x = (int)(cx - w / 2); x <= (int)(cx + w / 2); x++) {
                float px = x + 0.5f, py = y + 0.5f;
                float d = sd_round_rect(px, py, cx, (top + bot) / 2, w / 2 - edge, h / 2 - edge, w / 2 - edge);
                int side = button == 1 ? px < cx - s : px > cx + s;
                if (side && py < mid - s)
                    blend(c, x, y, ACCENT, 0.5f - d);
            }
    }
    /* The split between the buttons, and the line under them. */
    fill_round_rect(c, cx - s * 0.75f, top + edge, cx + s * 0.75f, mid, 0, CAP_EDGE, 1.0f);
    fill_round_rect(c, cx - w / 2 + edge, mid - s * 0.75f, cx + w / 2 - edge, mid + s * 0.75f, 0, CAP_EDGE, 1.0f);
    /* The wheel. */
    fill_round_rect(c, cx - 2.2f * s, top + h * 0.12f, cx + 2.2f * s, top + h * 0.34f, 2.2f * s,
                    button == 3 || button == 9 ? ACCENT : CAP_EDGE, 1.0f);
    if (!button) {  /* moving: arrows either side */
        float ay = (top + bot) / 2;
        for (x = 0; x < 2; x++) {
            float dir = x ? 1.0f : -1.0f, tip = cx + dir * (w / 2 + 5.5f * s);
            int i;
            for (i = 0; i < (int)(4 * s); i++) {
                float t = (float)i / (4 * s);
                fill_round_rect(c, tip - dir * t * 4 * s - 0.6f * s, ay - (1 - t) * 3.5f * s,
                                tip - dir * t * 4 * s + 0.6f * s, ay + (1 - t) * 3.5f * s, 0, ACCENT, 1.0f);
            }
        }
    }
}

static void draw_binding(Canvas *c, float x0, float y0, float x1, float y1, const char *label)
{
    if (!strcmp(label, "Mouse1"))
        draw_mouse(c, x0, y0, x1, y1, 1);
    else if (!strcmp(label, "Mouse2"))
        draw_mouse(c, x0, y0, x1, y1, 2);
    else if (!strcmp(label, "Mouse3"))
        draw_mouse(c, x0, y0, x1, y1, 3);
    else if (!strcmp(label, "WheelUp") || !strcmp(label, "WheelDown"))
        draw_mouse(c, x0, y0, x1, y1, 9);
    else if (!strcmp(label, "Mouse4") || !strcmp(label, "Mouse5")) {
        char t[4] = { 'M', label[5], 0 };
        draw_cap(c, x0, y0, x1, y1, t);
    } else
        draw_cap(c, x0, y0, x1, y1, label);
}

/* Up to four single-character labels run together ("WASD", "1234"); NULL
 * when any of them is longer. */
static const char *joined(const int *controls, char *buf)
{
    int i, n = 0;
    for (i = 0; i < 4; i++) {
        const char *l = mkb_binding_label(controls[i]);
        if (!l || l[1])
            return NULL;
        buf[n++] = l[0];
    }
    buf[n] = 0;
    return buf;
}

uint32_t *prompts_build(const uint32_t *orig, uint32_t scale)
{
    static const int MOVE[4] = { MKB_MOVE_UP, MKB_MOVE_LEFT, MKB_MOVE_DOWN, MKB_MOVE_RIGHT };
    static const int DPAD[4] = { MKB_DUP, MKB_DRIGHT, MKB_DDOWN, MKB_DLEFT };
    Canvas c;
    size_t i;
    uint32_t x, y;

    if (scale < 1)
        scale = 1;
    c.w = SHEET_W * (int)scale;
    c.h = SHEET_H * (int)scale;
    c.px = (uint32_t *)calloc((size_t)c.w * c.h, 4);
    if (!c.px)
        return NULL;
    /* Start from the controller sheet, so any glyph left alone (and the
     * unbound ones) still reads as before. */
    for (y = 0; y < (uint32_t)c.h; y++)
        for (x = 0; x < (uint32_t)c.w; x++)
            c.px[(size_t)y * c.w + x] = orig[(y / scale) * SHEET_W + x / scale];

    for (i = 0; i < sizeof CELLS / sizeof *CELLS; i++) {
        float x0 = (float)(CELLS[i].x0 * (int)scale), y0 = (float)(CELLS[i].y0 * (int)scale);
        float x1 = (float)(CELLS[i].x1 * (int)scale), y1 = (float)(CELLS[i].y1 * (int)scale);
        const char *label = NULL;
        char buf[8];

        switch (CELLS[i].show) {
        case SHOW_CONTROL: label = mkb_binding_label(CELLS[i].control); break;
        case SHOW_MOVE:    label = joined(MOVE, buf); break;
        case SHOW_DPAD:
            label = joined(DPAD, buf);
            if (!label && mkb_binding_label(MKB_DUP) && !strcmp(mkb_binding_label(MKB_DUP), "Up"))
                label = "\xE2\x86\x95\xE2\x86\x94";     /* the arrow keys */
            break;
        case SHOW_CAMERA:  label = "";                break;
        }
        if (!label)
            continue;
        for (y = (uint32_t)y0; y < (uint32_t)y1; y++)
            memset(&c.px[(size_t)y * c.w + (uint32_t)x0], 0, (size_t)(x1 - x0) * 4);
        if (CELLS[i].show == SHOW_CAMERA)
            draw_mouse(&c, x0, y0, x1, y1, 0);
        else
            draw_binding(&c, x0, y0, x1, y1, label);
    }
    return c.px;
}

/* ---- other controllers' buttons ---------------------------------------- */
/*
 * The same cells, holding another controller's buttons in the places a pad
 * maps to the Xbox ones (input/xinput_device.c, and SDL/Steam's default,
 * which go by position): Black is the left bumper, White the right one.
 *
 *   PlayStation: A cross, B circle, X square, Y triangle, L1 R1, L2 R2,
 *                Back SELECT, Start START.
 *   Switch:      by position, so the bottom button (Xbox A) is Nintendo's B,
 *                the right one A, the left one Y, the top one X; L R, ZL ZR,
 *                Back -, Start +.
 *
 * The sticks and the D-pad are the same on all of them and stay as the game
 * draws them.
 */
enum { PS_CROSS, PS_CIRCLE, PS_SQUARE, PS_TRIANGLE, FACE_LETTER };
typedef struct { int cell; int symbol; const char *text; } StyleCell;
static const StyleCell PS_CELLS[] = {
    { 8, PS_CROSS, NULL },     /* ) A */
    { 7, PS_CIRCLE, NULL },    /* ( B */
    { 9, PS_SQUARE, NULL },    /* * X */
    { 6, PS_TRIANGLE, NULL },  /* ' Y */
    { 10, -1, "L1" },          /* + Black */
    { 0, -1, "R1" },           /* ! White */
    { 1, -1, "L2" },           /* " left trigger */
    { 11, -1, "R2" },          /* , right trigger */
    { 12, -1, "SELECT" },      /* - Back */
    { 13, -1, "START" },       /* / Start */
    { -1, 0, NULL },
};
static const StyleCell SWITCH_CELLS[] = {
    { 8, FACE_LETTER, "B" },   /* ) A, the bottom button */
    { 7, FACE_LETTER, "A" },   /* ( B, the right one */
    { 9, FACE_LETTER, "Y" },   /* * X, the left one */
    { 6, FACE_LETTER, "X" },   /* ' Y, the top one */
    { 10, -1, "L" },           /* + Black */
    { 0, -1, "R" },            /* ! White */
    { 1, -1, "ZL" },           /* " left trigger */
    { 11, -1, "ZR" },          /* , right trigger */
    { 12, -1, "\xE2\x88\x92" },     /* - Back: minus */
    { 13, -1, "+" },           /* / Start: plus */
    { -1, 0, NULL },
};

static float sd_segment(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float t = (wx * vx + wy * vy) / (vx * vx + vy * vy);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    wx -= vx * t;
    wy -= vy * t;
    return sqrtf(wx * wx + wy * wy);
}

/* A face button: a dark disc with the symbol outlined in its colour. */
static void draw_face(Canvas *c, float x0, float y0, float x1, float y1, int symbol, const char *letter)
{
    static const uint32_t COLOUR[4] = { 0x7CA9F0u, 0xF2646Eu, 0xE68FD2u, 0x3FD6A6u };
    float s = (y1 - y0) / 32.0f;
    float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2 - s;
    float r = ((x1 - x0) < (y1 - y0) ? (x1 - x0) : (y1 - y0)) / 2 - 2 * s;
    float sym = r * 0.52f, t = 1.6f * s;    /* symbol radius, half the stroke */
    int x, y;

    fill_round_rect(c, cx - r + s, cy - r + 2 * s, cx + r + s, cy + r + 2 * s, r, 0x000000u, 0.45f);  /* shadow */
    fill_round_rect(c, cx - r, cy - r, cx + r, cy + r, r, CAP_EDGE, 1.0f);
    fill_round_rect(c, cx - r + 2 * s, cy - r + 2 * s, cx + r - 2 * s, cy + r - 2 * s, r - 2 * s, CAP_FILL, 1.0f);
    if (symbol == FACE_LETTER) {
        float in = r * 0.62f;
        draw_text(c, letter, (int)(cx - in), (int)(cy - in), (int)(2 * in), (int)(2 * in), CAP_TEXT);
        return;
    }
    for (y = (int)(cy - r); y <= (int)(cy + r); y++)
        for (x = (int)(cx - r); x <= (int)(cx + r); x++) {
            float px = x + 0.5f, py = y + 0.5f, d;
            switch (symbol) {
            case PS_CROSS: {
                float a = sd_segment(px, py, cx - sym * 0.8f, cy - sym * 0.8f, cx + sym * 0.8f, cy + sym * 0.8f);
                float b = sd_segment(px, py, cx - sym * 0.8f, cy + sym * 0.8f, cx + sym * 0.8f, cy - sym * 0.8f);
                d = (a < b ? a : b) - t;
                break;
            }
            case PS_CIRCLE:
                d = fabsf(sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy)) - sym * 0.85f) - t;
                break;
            case PS_SQUARE:
                d = fabsf(sd_round_rect(px, py, cx, cy, sym * 0.72f, sym * 0.72f, 0.5f * s)) - t;
                break;
            default: {      /* triangle, point up, centred on its centroid */
                float h = sym * 1.5f, ty = cy - h * 0.62f, by = cy + h * 0.38f, hw = h * 0.577f;
                float a = sd_segment(px, py, cx, ty, cx - hw, by);
                float b = sd_segment(px, py, cx - hw, by, cx + hw, by);
                float e = sd_segment(px, py, cx + hw, by, cx, ty);
                d = (a < b ? (a < e ? a : e) : (b < e ? b : e)) - t;
                break;
            }
            }
            blend(c, x, y, COLOUR[symbol], 0.5f - d);
        }
}

uint32_t *prompts_build_style(const uint32_t *orig, uint32_t scale, int style)
{
    const StyleCell *cells = style == PROMPTS_SWITCH ? SWITCH_CELLS : PS_CELLS;
    Canvas c;
    size_t i;
    uint32_t x, y;

    if (scale < 1)
        scale = 1;
    c.w = SHEET_W * (int)scale;
    c.h = SHEET_H * (int)scale;
    c.px = (uint32_t *)calloc((size_t)c.w * c.h, 4);
    if (!c.px)
        return NULL;
    for (y = 0; y < (uint32_t)c.h; y++)
        for (x = 0; x < (uint32_t)c.w; x++)
            c.px[(size_t)y * c.w + x] = orig[(y / scale) * SHEET_W + x / scale];
    for (i = 0; cells[i].cell >= 0; i++) {
        int k = cells[i].cell;
        float x0 = (float)(CELLS[k].x0 * (int)scale), y0 = (float)(CELLS[k].y0 * (int)scale);
        float x1 = (float)(CELLS[k].x1 * (int)scale), y1 = (float)(CELLS[k].y1 * (int)scale);
        for (y = (uint32_t)y0; y < (uint32_t)y1; y++)
            memset(&c.px[(size_t)y * c.w + (uint32_t)x0], 0, (size_t)(x1 - x0) * 4);
        if (cells[i].symbol < 0)
            draw_cap(&c, x0, y0, x1, y1, cells[i].text);
        else
            draw_face(&c, x0, y0, x1, y1, cells[i].symbol, cells[i].text);
    }
    return c.px;
}

/* Which maker's controller is connected: a HID game pad or joystick with
 * Sony's or Nintendo's vendor id. Asked at most every two seconds. A pad
 * behind Steam Input or an adapter (PS2) doesn't show as either;
 * ButtonPrompts covers it. */
static int connected_style(void)
{
    static ULONGLONG next;
    static int found = PROMPTS_XBOX;
    ULONGLONG now = GetTickCount64();
    UINT n = 0, i;
    RAWINPUTDEVICELIST *list;

    if (now < next)
        return found;
    next = now + 2000;
    found = PROMPTS_XBOX;
    if (GetRawInputDeviceList(NULL, &n, sizeof *list) != 0 || !n)
        return found;
    list = (RAWINPUTDEVICELIST *)malloc(n * sizeof *list);
    if (!list)
        return found;
    n = GetRawInputDeviceList(list, &n, sizeof *list);
    for (i = 0; i < n && n != (UINT)-1; i++) {
        RID_DEVICE_INFO info;
        UINT size = sizeof info;
        if (list[i].dwType != RIM_TYPEHID)
            continue;
        info.cbSize = sizeof info;
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &size) == (UINT)-1)
            continue;
        if (info.hid.usUsagePage != 0x01 || (info.hid.usUsage != 0x05 && info.hid.usUsage != 0x04))
            continue;
        if (info.hid.dwVendorId == 0x054C)
            found = PROMPTS_PLAYSTATION;
        else if (info.hid.dwVendorId == 0x057E && found == PROMPTS_XBOX)
            found = PROMPTS_SWITCH;
    }
    free(list);
    return found;
}

int prompts_style(int setting)
{
    return setting == PROMPTS_AUTO ? connected_style() : setting;
}
