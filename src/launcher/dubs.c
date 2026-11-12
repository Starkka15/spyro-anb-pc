/*
 * Dubbed cutscenes from a European PS2 disc.
 *
 * The Xbox release has English audio only in its cutscene movies. The
 * European PS2 release (SLES-54359) has six audio tracks in the same movies
 * -- English, French, Spanish, German, Italian, Dutch, the order the game's
 * own language table uses -- cut from the same masters. This reads that disc
 * and writes, for each language, a read overlay per movie
 * (dubs\<LANG>\<movie>_60.xmv.ovl). The game files stay as they are; the game
 * reads them through RECOMP_READ_OVERLAY, which main.c points at the folder of
 * the chosen language.
 *
 * The source: the disc in a drive, a folder holding MEDIA_P2.RKV, an ISO
 * image of the disc (ISO 9660), or MEDIA_P2.RKV itself.
 *
 * MEDIA_P2.RKV ("RKV2"): entry count at 4, 20-byte entries from 0x80 (name
 * offset, 0, size, offset, crc), names after them.
 * Movies (.pss): MPEG program stream. Audio is private stream 1 with a
 * 4-byte sub-header ff a1 00 NN (NN = track), carrying an SShd/SSbd stream:
 * PS2 ADPCM, 48 kHz stereo, channels interleaved every 32 bytes.
 * Xbox movies (.xmv): audio is 16-bit PCM, 32 kHz stereo, one span per
 * packet; the overlay replaces those spans (the same layout tools/ps2_dubs.py
 * writes: "ROVL", version, count, {offset, length, 0} x count, then data).
 *
 * The PS2 audio runs 18 or 19 samples (at 32 kHz) behind the Xbox audio;
 * measured per movie with tools/ps2_dubs.py, and fixed here.
 */
#include <windows.h>
#include <commctrl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "dubs.h"
#include "../text_pc.h"
#include "launcher_res.h"

#define SECTOR 2048u
#define WM_DUBS_PROGRESS (WM_APP + 11)
#define WM_DUBS_DONE     (WM_APP + 12)

static const struct {
    const char *xbox, *ps2;
    int lag;                    /* PS2 samples to skip at 32 kHz */
} MOVIES[] = {
    { "attract", "attract_50.pss", 19 }, { "csc_19", "csc_19_50.pss", 18 },
    { "csc_20", "csc_20_50.pss", 18 },   { "csd_14", "csd_14_50.pss", 18 },
    { "csd_15", "csd_15_50.pss", 18 },   { "cse_18", "cse_18_50.pss", 18 },
    { "cse_19", "cse_19_50.pss", 19 },   { "csz_10", "csz_10_50.pss", 18 },
    /* The European disc has only the no-title cut; its audio is the same. */
    { "csz_80", "csz_80nt_50.pss", 18 }, { "csz_84", "csz_84_50.pss", 18 },
};
#define MOVIE_COUNT (sizeof MOVIES / sizeof *MOVIES)
static const char *const LANG_CODE[] = { "FR", "ES", "DE", "IT", "NL" };   /* tracks 1-5 */
#define LANG_N 5

typedef struct {
    wchar_t src[MAX_PATH], game[MAX_PATH], out[MAX_PATH];
    HWND dlg;
    volatile LONG cancel;
    int result;
    wchar_t message[512];
} Job;

static int read_at(HANDLE h, uint64_t off, void *buf, DWORD len)
{
    OVERLAPPED ov;
    DWORD got = 0;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    return ReadFile(h, buf, len, &got, &ov) && got == len;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---- finding MEDIA_P2.RKV ------------------------------------------------ */

/* Where `want` (MEDIA_P2.RKV, DATA_P2.RKV) starts in an ISO 9660 image (its
 * root directory). */
static int iso_find(HANDLE h, const char *want, uint64_t *off)
{
    uint8_t pvd[SECTOR], *dir;
    uint32_t lba, size, i;
    if (!read_at(h, 16 * SECTOR, pvd, SECTOR) || memcmp(pvd + 1, "CD001", 5))
        return 0;
    lba = rd32(pvd + 156 + 2);
    size = rd32(pvd + 156 + 10);
    if (!size || size > (1u << 20) || !(dir = (uint8_t *)malloc(size)))
        return 0;
    if (!read_at(h, (uint64_t)lba * SECTOR, dir, size)) {
        free(dir);
        return 0;
    }
    for (i = 0; i < size; ) {
        uint8_t n = dir[i];
        if (!n) {
            i = (i / SECTOR + 1) * SECTOR;
            continue;
        }
        if (i + 33 <= size && i + 33 + dir[i + 32] <= size && dir[i + 32] >= strlen(want)
            && !_strnicmp((const char *)dir + i + 33, want, strlen(want))) {
            *off = (uint64_t)rd32(dir + i + 2) * SECTOR;
            free(dir);
            return 1;
        }
        i += n;
    }
    free(dir);
    return 0;
}

typedef struct {
    HANDLE h;
    uint64_t base;
    uint32_t n;
    uint8_t *ent;               /* n x 20 bytes */
    char *names;
    uint32_t names_len;
} Rkv;

/* An RKV archive: `src` is the archive itself, a folder or drive holding
 * `name`, or an ISO image with `name` in its root. */
static int rkv_open(Rkv *r, const wchar_t *src, const char *name)
{
    wchar_t p[MAX_PATH];
    uint8_t head[0x80];
    DWORD attr = GetFileAttributesW(src);
    memset(r, 0, sizeof *r);
    if (attr == INVALID_FILE_ATTRIBUTES)
        return 0;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        size_t len = wcslen(src);
        swprintf(p, MAX_PATH, L"%ls%ls%hs", src, len && src[len - 1] == L'\\' ? L"" : L"\\", name);
    } else {
        wcsncpy(p, src, MAX_PATH - 1);
        p[MAX_PATH - 1] = 0;
    }
    r->h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    if (r->h == INVALID_HANDLE_VALUE) {
        r->h = NULL;
        return 0;
    }
    if (!read_at(r->h, 0, head, 4))
        return 0;
    if (memcmp(head, "RKV2", 4) && !iso_find(r->h, name, &r->base))
        return 0;
    if (!read_at(r->h, r->base, head, sizeof head) || memcmp(head, "RKV2", 4))
        return 0;
    r->n = rd32(head + 4);
    if (!r->n || r->n > 65536)
        return 0;
    r->names_len = 1u << 20;
    r->ent = (uint8_t *)malloc((size_t)r->n * 20);
    r->names = (char *)calloc(1, r->names_len + 1);
    return r->ent && r->names && read_at(r->h, r->base + 0x80, r->ent, r->n * 20)
        && read_at(r->h, r->base + 0x80 + r->n * 20, r->names, r->names_len);
}

static void rkv_close(Rkv *r)
{
    if (r->h)
        CloseHandle(r->h);
    free(r->ent);
    free(r->names);
}

/* Reads a whole entry. */
static uint8_t *rkv_read(Rkv *r, const char *name, uint32_t *size)
{
    uint32_t i;
    for (i = 0; i < r->n; i++) {
        const uint8_t *e = r->ent + i * 20;
        uint32_t no = rd32(e);
        if (no < r->names_len && !_stricmp(r->names + no, name)) {
            uint8_t *b;
            *size = rd32(e + 8);
            if (*size > (512u << 20) || !(b = (uint8_t *)malloc(*size)))
                return NULL;
            if (!read_at(r->h, r->base + rd32(e + 12), b, *size)) {
                free(b);
                return NULL;
            }
            return b;
        }
    }
    return NULL;
}

/* ---- PS2 audio ----------------------------------------------------------- */

typedef struct { uint8_t *b; size_t n, cap; } Buf;

static int buf_add(Buf *t, const uint8_t *p, size_t n)
{
    if (t->n + n > t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 1u << 20;
        uint8_t *b;
        while (cap < t->n + n)
            cap *= 2;
        if (!(b = (uint8_t *)realloc(t->b, cap)))
            return 0;
        t->b = b;
        t->cap = cap;
    }
    memcpy(t->b + t->n, p, n);
    t->n += n;
    return 1;
}

/* Splits a movie's audio into its tracks (SShd/SSbd streams). Returns the
 * number of tracks, 0 on an error. */
static int pss_tracks(const uint8_t *d, size_t n, Buf tr[6])
{
    size_t i = 0;
    int count = 0, k;
    while (i + 6 <= n) {
        uint8_t sid;
        size_t len, p;
        if (d[i] || d[i + 1] || d[i + 2] != 1) {
            i++;
            continue;
        }
        sid = d[i + 3];
        if (sid == 0xBA) {
            if (i + 14 > n)
                break;
            i += 14 + (d[i + 13] & 7);
            continue;
        }
        if (sid == 0xB9)
            break;
        len = (size_t)d[i + 4] << 8 | d[i + 5];
        p = i + 6;
        if (p + len > n)
            break;
        if (sid == 0xBD && len > 3) {
            size_t pl = p + 3 + d[p + 2];
            if (pl + 4 <= p + len && d[pl] == 0xFF && d[pl + 1] == 0xA1) {
                uint8_t t = d[pl + 3];
                if (t >= 6)
                    return 0;
                if (!buf_add(&tr[t], d + pl + 4, p + len - pl - 4))
                    return 0;
                if (t + 1 > count)
                    count = t + 1;
            }
        }
        i = p + len;
    }
    for (k = 0; k < count; k++)
        if (tr[k].n < 40 || memcmp(tr[k].b, "SShd", 4) || memcmp(tr[k].b + 32, "SSbd", 4))
            return 0;
    return count;
}

/* PS2 ADPCM to 16-bit stereo at 48 kHz. Returns frames (sample pairs). */
static int16_t *decode_adpcm(const Buf *t, size_t *frames)
{
    static const int F0[5] = { 0, 60, 115, 98, 122 }, F1[5] = { 0, 0, -52, -55, -60 };
    const uint8_t *d = t->b + 40;
    size_t n = t->n - 40, blocks, b, out_n;
    uint32_t inter = rd32(t->b + 20);
    int16_t *out;
    int hist[2][2] = { { 0, 0 }, { 0, 0 } };

    if (rd32(t->b + 8) != 0x10 || rd32(t->b + 16) != 2 || !inter || inter % 16)
        return NULL;
    blocks = n / (inter * 2);
    out_n = blocks * (inter / 16) * 28;
    out = (int16_t *)malloc(out_n * 2 * sizeof *out);
    if (!out)
        return NULL;
    for (b = 0; b < blocks; b++) {
        int ch;
        for (ch = 0; ch < 2; ch++) {
            const uint8_t *blk = d + (b * 2 + ch) * inter;
            size_t base = b * (inter / 16) * 28;
            uint32_t f;
            for (f = 0; f < inter / 16; f++) {
                const uint8_t *fr = blk + f * 16;
                int shift = fr[0] & 15, filt = fr[0] >> 4, s;
                if (filt > 4)
                    filt = 0;
                if (shift > 12)
                    shift = 9;
                for (s = 0; s < 28; s++) {
                    int nib = (fr[2 + s / 2] >> ((s & 1) * 4)) & 15;
                    int v = (int16_t)(nib << 12) >> shift;
                    v += (hist[ch][0] * F0[filt] + hist[ch][1] * F1[filt] + 32) >> 6;
                    v = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
                    hist[ch][1] = hist[ch][0];
                    hist[ch][0] = v;
                    out[(base + f * 28 + s) * 2 + ch] = (int16_t)v;
                }
            }
        }
    }
    *frames = out_n;
    return out;
}

/* 48 kHz to 32 kHz: a windowed-sinc low-pass at 15 kHz, output sample n at
 * input position 1.5 n (two filter phases). */
#define TAPS 48
static int16_t *resample(const int16_t *in, size_t n, size_t *out_n)
{
    static float k[2][TAPS];
    static int init;
    size_t m = n * 2 / 3, o;
    int16_t *out = (int16_t *)malloc((m ? m : 1) * 2 * sizeof *out);
    if (!out)
        return NULL;
    if (!init) {
        int ph, t;
        for (ph = 0; ph < 2; ph++) {
            double sum = 0, frac = ph * 0.5;
            for (t = 0; t < TAPS; t++) {
                double x = t - (TAPS / 2 - 1) - frac;      /* distance from the output position */
                double c = 15000.0 / 48000.0 * 2;          /* cutoff, in Nyquists of the input */
                double s = fabs(x) < 1e-9 ? c : sin(3.14159265358979 * c * x) / (3.14159265358979 * x);
                double w = 0.42 + 0.5 * cos(3.14159265358979 * x / (TAPS / 2))
                         + 0.08 * cos(2 * 3.14159265358979 * x / (TAPS / 2));
                if (fabs(x) >= TAPS / 2)
                    w = 0;
                k[ph][t] = (float)(s * w);
                sum += s * w;
            }
            for (t = 0; t < TAPS; t++)
                k[ph][t] = (float)(k[ph][t] / sum);
        }
        init = 1;
    }
    for (o = 0; o < m; o++) {
        size_t pos = o * 3 / 2;                 /* integer part of 1.5 o */
        int ph = (int)(o & 1), t, ch;
        for (ch = 0; ch < 2; ch++) {
            float acc = 0;
            for (t = 0; t < TAPS; t++) {
                long long i = (long long)pos + t - (TAPS / 2 - 1);
                if (i >= 0 && (size_t)i < n)
                    acc += k[ph][t] * in[i * 2 + ch];
            }
            acc = acc < -32768.f ? -32768.f : acc > 32767.f ? 32767.f : acc;
            out[o * 2 + ch] = (int16_t)lrintf(acc);
        }
    }
    *out_n = m;
    return out;
}

/* ---- Xbox movies --------------------------------------------------------- */

typedef struct { uint64_t off; uint32_t len; } Span;

/* Track-0 audio spans of an XMV with 32 kHz 16-bit stereo PCM. */
static Span *xmv_spans(const wchar_t *path, uint32_t *count, uint64_t *bytes)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    uint8_t head[36 + 12 * 8], ph[12 + 4 * 8];
    LARGE_INTEGER fs;
    uint32_t nt, k, first, size, cap = 1024, n = 0;
    uint64_t pos;
    Span *s = NULL;

    *bytes = 0;
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    if (!GetFileSizeEx(h, &fs) || !read_at(h, 0, head, 36) || memcmp(head + 12, "xobX", 4))
        goto fail;
    nt = head[32] | head[33] << 8;
    if (!nt || nt > 8 || !read_at(h, 36, head + 36, 12 * nt))
        goto fail;
    for (k = 0; k < nt; k++) {
        const uint8_t *t = head + 36 + 12 * k;
        if ((t[0] | t[1] << 8) != 1 || (t[2] | t[3] << 8) != 2 || rd32(t + 4) != 32000 || (t[8] | t[9] << 8) != 16)
            goto fail;
    }
    first = rd32(head + 4);
    pos = 36 + 12 * nt;
    size = first - (uint32_t)pos;
    if (!(s = (Span *)malloc(cap * sizeof *s)))
        goto fail;
    while (pos < (uint64_t)fs.QuadPart && size) {
        uint32_t next, vsz, asz;
        if (!read_at(h, pos, ph, 12 + 4 * nt))
            goto fail;
        next = rd32(ph);
        vsz = (rd32(ph + 4) & 0x7FFFFF) - 4 * nt;
        asz = rd32(ph + 12) & 0x7FFFFF;
        if (n == cap) {
            Span *t = (Span *)realloc(s, (cap *= 2) * sizeof *s);
            if (!t)
                goto fail;
            s = t;
        }
        s[n].off = pos + 12 + 4 * nt + vsz;
        s[n].len = asz;
        if (s[n].off + asz > (uint64_t)fs.QuadPart)
            goto fail;
        *bytes += asz;
        n++;
        pos += size;
        size = next;
    }
    CloseHandle(h);
    *count = n;
    return s;
fail:
    CloseHandle(h);
    free(s);
    return NULL;
}

static int write_all(HANDLE h, const void *p, size_t n);

/* ---- text ---------------------------------------------------------------- */
/*
 * The Xbox translations leave lines in English (Italian 95, the others 40 to
 * 53). The European PS2 release finished many of them. A line is taken from
 * it when the Xbox line in that language is still the English one, and:
 *   - the PS2 English line is the Xbox English line: the PS2 line as it is;
 *   - they differ only in button glyphs: the PS2 line with the Xbox glyphs,
 *     in order;
 *   - anything else (memory card wording, PS2 button combinations) stays
 *     English, except Italian, which has the port's own lines (text_it.inc).
 * translations.<Language>.txt: KEY[=id] TAB text, CRLF, cp1252.
 *
 * The merged file is longer than the original, so it goes in data_xb.rkv's
 * largest unused gap, and the overlay also rewrites its table entry (size,
 * offset, CRC-32).
 */
static const char *const LANG_FILE[] = { "French", "Spanish", "German", "Italian", "Dutch" };
static const struct { const char *key, *text; } TEXT_IT[] = {
#include "text_it.inc"
};
#define GLYPH_OPEN "<font=font_buttons:0>"
#define GLYPH_CLOSE "</font>"

typedef struct { const char *key; size_t key_len; const char *val; size_t val_len; } Line;
typedef struct { Line *l; size_t n; } Lines;

static int lines_parse(const char *t, size_t n, Lines *out)
{
    size_t i = 0, cap = 512;
    out->n = 0;
    out->l = (Line *)malloc(cap * sizeof *out->l);
    if (!out->l)
        return 0;
    while (i < n) {
        size_t e = i, tab;
        Line *L;
        while (e < n && t[e] != '\r' && t[e] != '\n')
            e++;
        for (tab = i; tab < e && t[tab] != '\t'; tab++) {}
        if (tab < e) {
            if (out->n == cap) {
                Line *g = (Line *)realloc(out->l, (cap *= 2) * sizeof *g);
                if (!g)
                    return 0;
                out->l = g;
            }
            L = &out->l[out->n++];
            L->key = t + i;
            for (L->key_len = 0; i + L->key_len < tab && t[i + L->key_len] != '='; L->key_len++) {}
            L->val = t + tab + 1;
            L->val_len = e - tab - 1;
        }
        i = e;
        while (i < n && (t[i] == '\r' || t[i] == '\n'))
            i++;
    }
    return 1;
}

static const Line *lines_find(const Lines *ls, const char *key, size_t len)
{
    size_t i;
    for (i = 0; i < ls->n; i++)
        if (ls->l[i].key_len == len && !memcmp(ls->l[i].key, key, len))
            return &ls->l[i];
    return NULL;
}

static int same(const Line *a, const Line *b)
{
    return a && b && a->val_len == b->val_len && !memcmp(a->val, b->val, a->val_len);
}

/* The glyph tags of a line, in order: start and length of each. */
static int glyphs(const char *v, size_t n, size_t *at, size_t *len, int max)
{
    size_t i = 0, ol = strlen(GLYPH_OPEN), cl = strlen(GLYPH_CLOSE);
    int c = 0;
    while (i + ol <= n) {
        if (!memcmp(v + i, GLYPH_OPEN, ol)) {
            size_t e = i + ol;
            while (e + cl <= n && memcmp(v + e, GLYPH_CLOSE, cl))
                e++;
            if (e + cl > n)
                break;
            if (c < max) {
                at[c] = i;
                len[c] = e + cl - i;
            }
            c++;
            i = e + cl;
        } else {
            i++;
        }
    }
    return c;
}

/* Whether two lines are the same apart from what their glyph tags hold. */
static int same_but_glyphs(const Line *a, const Line *b)
{
    size_t aa[32], al[32], ba[32], bl[32], ia = 0, ib = 0;
    int na = glyphs(a->val, a->val_len, aa, al, 32), nb = glyphs(b->val, b->val_len, ba, bl, 32), k;
    if (na != nb || na > 32)
        return 0;
    for (k = 0; k <= na; k++) {
        size_t ea = k < na ? aa[k] : a->val_len, eb = k < nb ? ba[k] : b->val_len;
        if (ea - ia != eb - ib || memcmp(a->val + ia, b->val + ib, ea - ia))
            return 0;
        if (k < na) {
            ia = aa[k] + al[k];
            ib = ba[k] + bl[k];
        }
    }
    return 1;
}

static int out_add(Buf *o, const char *p, size_t n) { return buf_add(o, (const uint8_t *)p, n); }

/* `text` with its glyphs (or '@' marks, when marks) replaced, in order, by the
 * glyph tags of `glyph_src`. */
static int put_with_glyphs(Buf *o, const char *text, size_t n, int marks, const Line *glyph_src)
{
    size_t sa[32], sl[32], ta[32], tl[32], i = 0;
    int ns = glyphs(glyph_src->val, glyph_src->val_len, sa, sl, 32), nt = 0, k = 0;
    if (ns > 32)
        return 0;
    if (!marks)
        nt = glyphs(text, n, ta, tl, 32);
    while (i < n) {
        int at_glyph = marks ? text[i] == '@' : (k < nt && i == ta[k]);
        if (at_glyph && k < ns) {
            if (!out_add(o, glyph_src->val + sa[k], sl[k]))
                return 0;
            i += marks ? 1 : tl[k];
            k++;
        } else {
            if (!out_add(o, text + i, 1))
                return 0;
            i++;
        }
    }
    return 1;
}

static int has_letters(const Line *l)
{
    size_t i;
    for (i = 0; i < l->val_len; i++)
        if ((l->val[i] | 32) >= 'a' && (l->val[i] | 32) <= 'z')
            return 1;
    return 0;
}

/* Merges one language. Returns lines taken, -1 on an error. */
static int merge_text(const Lines *xe, const Lines *xl, const Lines *ee, const Lines *el, int italian, Buf *o)
{
    size_t i, k;
    int taken = 0;
    for (i = 0; i < xl->n; i++) {
        const Line *x = &xl->l[i], *en = lines_find(xe, x->key, x->key_len);
        const Line *pe = lines_find(ee, x->key, x->key_len), *pl = lines_find(el, x->key, x->key_len);
        const char *eq = memchr(x->key, '\t', 512);
        int done = 0;
        /* The whole key field (KEY=id), then the tab. */
        if (!eq || !out_add(o, x->key, (size_t)(eq - x->key) + 1))
            return -1;
        if (same(x, en) && has_letters(x)) {
            if (italian)
                for (k = 0; k < sizeof TEXT_IT / sizeof *TEXT_IT && !done; k++)
                    if (strlen(TEXT_IT[k].key) == x->key_len && !memcmp(TEXT_IT[k].key, x->key, x->key_len)) {
                        if (!put_with_glyphs(o, TEXT_IT[k].text, strlen(TEXT_IT[k].text), 1, en))
                            return -1;
                        done = 1;
                    }
            if (!done && pl && pe && !same(pl, x)) {
                if (same(pe, en)) {
                    if (!out_add(o, pl->val, pl->val_len))
                        return -1;
                    done = 1;
                } else if (same_but_glyphs(pe, en)) {
                    size_t a1[32], l1[32], a2[32], l2[32];
                    if (glyphs(pl->val, pl->val_len, a1, l1, 32) == glyphs(pe->val, pe->val_len, a2, l2, 32)) {
                        if (!put_with_glyphs(o, pl->val, pl->val_len, 0, en))
                            return -1;
                        done = 1;
                    }
                }
            }
            taken += done;
        }
        if (!done && !out_add(o, x->val, x->val_len))
            return -1;
        if (!out_add(o, "\r\n", 2))
            return -1;
    }
    return taken;
}

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    static uint32_t t[256];
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    if (!t[1])
        for (i = 0; i < 256; i++) {
            uint32_t v = (uint32_t)i;
            int b;
            for (b = 0; b < 8; b++)
                v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            t[i] = v;
        }
    for (i = 0; i < n; i++)
        c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

static int entry_index(const Rkv *r, const char *name)
{
    uint32_t i;
    for (i = 0; i < r->n; i++) {
        uint32_t no = rd32(r->ent + i * 20);
        if (no < r->names_len && !_stricmp(r->names + no, name))
            return (int)i;
    }
    return -1;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* The largest gap between entries of an archive: where nothing points. */
static int largest_gap(const Rkv *r, uint64_t *at, uint64_t *size)
{
    uint64_t *e = (uint64_t *)malloc((size_t)r->n * 2 * sizeof *e);
    uint32_t i;
    if (!e)
        return 0;
    for (i = 0; i < r->n; i++) {
        e[i * 2] = rd32(r->ent + i * 20 + 12);
        e[i * 2 + 1] = rd32(r->ent + i * 20 + 8);
    }
    qsort(e, r->n, 2 * sizeof *e, cmp_u64);
    *size = 0;
    for (i = 0; i + 1 < r->n; i++) {
        uint64_t end = e[i * 2] + e[i * 2 + 1];
        if (e[(i + 1) * 2] > end && e[(i + 1) * 2] - end > *size) {
            *size = e[(i + 1) * 2] - end;
            *at = end;
        }
    }
    free(e);
    return *size > 0;
}

/* Writes <out>\<LANG>\data_xb.rkv.ovl for each language. 0 on an error,
 * with the message set. */
static int import_text(Job *j)
{
    Rkv xb, eu;
    wchar_t path[MAX_PATH];
    uint8_t *xe_t = NULL, *ee_t = NULL;
    uint32_t xe_n, ee_n;
    Lines xe = { 0 }, ee = { 0 };
    uint64_t gap = 0, gap_size = 0;
    int l, ok = 0;

    swprintf(path, MAX_PATH, L"%ls\\data_xb.rkv", j->game);
    if (!rkv_open(&xb, path, "data_xb.rkv")) {
        swprintf(j->message, 512, L"%ls could not be read. Install the game files first.", path);
        rkv_close(&xb);
        return 0;
    }
    if (!rkv_open(&eu, j->src, "DATA_P2.RKV")) {
        wcscpy(j->message, L"DATA_P2.RKV could not be read from the PS2 disc.");
        goto done;
    }
    xe_t = rkv_read(&xb, "translations.English.txt", &xe_n);
    ee_t = rkv_read(&eu, "translations.English.txt", &ee_n);
    if (!xe_t || !ee_t || !lines_parse((char *)xe_t, xe_n, &xe) || !lines_parse((char *)ee_t, ee_n, &ee)
        || !largest_gap(&xb, &gap, &gap_size)) {
        wcscpy(j->message, L"The game's text could not be read.");
        goto done;
    }
    for (l = 0; l < LANG_N; l++) {
        char name[64];
        uint8_t *xl_t, *el_t, rec[20];
        uint32_t xl_n, el_n;
        Lines xl = { 0 }, el = { 0 };
        Buf o = { 0 };
        int idx, taken = -1;
        HANDLE h;

        snprintf(name, sizeof name, "translations.%s.txt", LANG_FILE[l]);
        idx = entry_index(&xb, name);
        xl_t = rkv_read(&xb, name, &xl_n);
        el_t = rkv_read(&eu, name, &el_n);
        if (idx >= 0 && xl_t && el_t && lines_parse((char *)xl_t, xl_n, &xl) && lines_parse((char *)el_t, el_n, &el))
            taken = merge_text(&xe, &xl, &ee, &el, !strcmp(LANG_FILE[l], "Italian"), &o);
        free(xl_t);
        free(el_t);
        free(xl.l);
        free(el.l);
        if (taken < 0 || o.n > gap_size) {
            free(o.b);
            swprintf(j->message, 512, L"The %hs text could not be merged.", LANG_FILE[l]);
            goto done;
        }
        /* The table entry: name offset and flags kept; size, offset, CRC new. */
        memcpy(rec, xb.ent + idx * 20, 20);
        *(uint32_t *)(rec + 8) = (uint32_t)o.n;
        *(uint32_t *)(rec + 12) = (uint32_t)gap;
        *(uint32_t *)(rec + 16) = crc32_of(o.b, o.n);
        swprintf(path, MAX_PATH, L"%ls\\%hs\\data_xb.rkv.ovl", j->out, LANG_CODE[l]);
        h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            uint8_t head[12 + 32];
            uint64_t ent_off = xb.base + 0x80 + (uint64_t)idx * 20;
            uint32_t len = 20, size = (uint32_t)o.n, zero = 0;
            memcpy(head, "ROVL", 4);
            *(uint32_t *)(head + 4) = 1;
            *(uint32_t *)(head + 8) = 2;
            memcpy(head + 12, &ent_off, 8);
            memcpy(head + 20, &len, 4);
            memcpy(head + 24, &zero, 4);
            memcpy(head + 28, &gap, 8);
            memcpy(head + 36, &size, 4);
            memcpy(head + 40, &zero, 4);
            ok = write_all(h, head, sizeof head) && write_all(h, rec, 20) && write_all(h, o.b, o.n);
            CloseHandle(h);
        }
        free(o.b);
        if (!ok) {
            DeleteFileW(path);
            swprintf(j->message, 512, L"%ls could not be written.", path);
            goto done;
        }
        ok = 0;
    }
    ok = 1;
done:
    free(xe_t);
    free(ee_t);
    free(xe.l);
    free(ee.l);
    rkv_close(&xb);
    rkv_close(&eu);
    return ok;
}

/* ---- the job ------------------------------------------------------------- */

static int write_all(HANDLE h, const void *p, size_t n)
{
    DWORD w;
    while (n) {
        DWORD c = n > (1u << 30) ? (1u << 30) : (DWORD)n;
        if (!WriteFile(h, p, c, &w, NULL) || w != c)
            return 0;
        p = (const uint8_t *)p + c;
        n -= c;
    }
    return 1;
}

static int write_overlay(const wchar_t *path, const Span *sp, uint32_t count, uint64_t bytes,
                         const int16_t *pcm, size_t frames, int lag)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    uint8_t head[12];
    uint8_t *tab, *data;
    uint32_t i;
    size_t want = (size_t)(bytes / 4), have = frames > (size_t)lag ? frames - lag : 0;
    int ok;
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    memcpy(head, "ROVL", 4);
    *(uint32_t *)(head + 4) = 1;
    *(uint32_t *)(head + 8) = count;
    tab = (uint8_t *)calloc(count, 16);
    data = (uint8_t *)calloc(1, (size_t)bytes);
    ok = tab && data;
    if (ok) {
        for (i = 0; i < count; i++) {
            memcpy(tab + i * 16, &sp[i].off, 8);
            memcpy(tab + i * 16 + 8, &sp[i].len, 4);
        }
        memcpy(data, pcm + (size_t)lag * 2, (have < want ? have : want) * 4);
        ok = write_all(h, head, sizeof head) && write_all(h, tab, (size_t)count * 16)
          && write_all(h, data, (size_t)bytes);
    }
    free(tab);
    free(data);
    CloseHandle(h);
    if (!ok)
        DeleteFileW(path);
    return ok;
}

static void progress(Job *j, int step, int steps)
{
    PostMessageW(j->dlg, WM_DUBS_PROGRESS, 0, (LPARAM)(step * 1000 / steps));
}

static DWORD WINAPI worker(LPVOID arg)
{
    Job *j = (Job *)arg;
    Rkv r;
    size_t m;
    int l, step = 0, steps = (int)MOVIE_COUNT * (LANG_N + 1);
    wchar_t path[MAX_PATH];

    j->result = DUBS_FAILED;
    if (!rkv_open(&r, j->src, "MEDIA_P2.RKV")) {
        wcscpy(j->message, L"That isn't the PS2 disc of The Legend of Spyro: A New Beginning: no MEDIA_P2.RKV was "
                           L"found. Choose the disc's drive, or an .iso image of the disc.");
        goto done;
    }
    for (l = 0; l < LANG_N; l++) {
        swprintf(path, MAX_PATH, L"%ls\\%hs", j->out, LANG_CODE[l]);
        CreateDirectoryW(j->out, NULL);
        CreateDirectoryW(path, NULL);
    }
    for (m = 0; m < MOVIE_COUNT; m++) {
        Buf tr[6];
        uint8_t *pss;
        uint32_t size, count;
        uint64_t bytes;
        Span *sp;
        int n;
        wchar_t text[128];

        swprintf(text, 128, L"Reading %hs...", MOVIES[m].ps2);
        SetDlgItemTextW(j->dlg, IDC_IMPORT_TEXT, text);
        if (j->cancel)
            goto cancelled;
        swprintf(path, MAX_PATH, L"%ls\\video\\%hs_60.xmv", j->game, MOVIES[m].xbox);
        sp = xmv_spans(path, &count, &bytes);
        if (!sp) {
            swprintf(j->message, 512, L"%ls could not be read. Install the game files first.", path);
            goto done;
        }
        pss = rkv_read(&r, MOVIES[m].ps2, &size);
        memset(tr, 0, sizeof tr);
        n = pss ? pss_tracks(pss, size, tr) : 0;
        free(pss);
        if (n != 6) {
            int k;
            for (k = 0; k < 6; k++)
                free(tr[k].b);
            free(sp);
            if (!pss)
                swprintf(j->message, 512, L"%hs could not be read from the disc. If it is a disc, it may be "
                                          L"scratched or dirty.", MOVIES[m].ps2);
            else
                wcscpy(j->message, L"This is not the European PS2 disc: its movies have no dubbed audio. The "
                                   L"dubs are on the PAL release (SLES-54359).");
            goto done;
        }
        progress(j, ++step, steps);
        for (l = 0; l < LANG_N && !j->cancel; l++) {
            size_t f48, f32;
            int16_t *a48 = decode_adpcm(&tr[l + 1], &f48), *a32 = a48 ? resample(a48, f48, &f32) : NULL;
            int ok;
            swprintf(text, 128, L"Writing %hs, %hs...", MOVIES[m].xbox, LANG_CODE[l]);
            SetDlgItemTextW(j->dlg, IDC_IMPORT_TEXT, text);
            swprintf(path, MAX_PATH, L"%ls\\%hs\\%hs_60.xmv.ovl", j->out, LANG_CODE[l], MOVIES[m].xbox);
            ok = a32 && write_overlay(path, sp, count, bytes, a32, f32, MOVIES[m].lag);
            free(a48);
            free(a32);
            if (!ok) {
                int k;
                for (k = 0; k < 6; k++)
                    free(tr[k].b);
                free(sp);
                swprintf(j->message, 512, L"%ls could not be written. Is there enough free space (about 700 MB)?",
                         path);
                goto done;
            }
            progress(j, ++step, steps);
        }
        for (l = 0; l < 6; l++)
            free(tr[l].b);
        free(sp);
        if (j->cancel)
            goto cancelled;
    }
    SetDlgItemTextW(j->dlg, IDC_IMPORT_TEXT, L"Merging the translations...");
    if (!import_text(j))
        goto done;
    /* The port's PC wording on top (text_pc.c; the game refreshes it too). */
    for (l = 0; l < LANG_N; l++) {
        swprintf(path, MAX_PATH, L"%ls\\%hs", j->out, LANG_CODE[l]);
        text_pc_refresh(j->game, path, TEXT_LANG_FR + l);
    }
    j->result = DUBS_OK;
    goto done;
cancelled:
    j->result = DUBS_CANCELLED;
    wcscpy(j->message, L"Cancelled.");
done:
    rkv_close(&r);
    if (j->result != DUBS_OK) {
        /* Half a set would leave some movies dubbed and some not. */
        for (l = 0; l < LANG_N; l++) {
            for (m = 0; m < MOVIE_COUNT; m++) {
                swprintf(path, MAX_PATH, L"%ls\\%hs\\%hs_60.xmv.ovl", j->out, LANG_CODE[l], MOVIES[m].xbox);
                DeleteFileW(path);
            }
            swprintf(path, MAX_PATH, L"%ls\\%hs\\data_xb.rkv.ovl", j->out, LANG_CODE[l]);
            DeleteFileW(path);
            swprintf(path, MAX_PATH, L"%ls\\%hs", j->out, LANG_CODE[l]);
            RemoveDirectoryW(path);
        }
        RemoveDirectoryW(j->out);
    }
    PostMessageW(j->dlg, WM_DUBS_DONE, 0, 0);
    return 0;
}

static INT_PTR CALLBACK dubs_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    Job *j = (Job *)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG:
        j = (Job *)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)j);
        j->dlg = dlg;
        SetWindowTextW(dlg, L"Adding the dubbed cutscenes");
        SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1)));
        SendDlgItemMessageW(dlg, IDC_IMPORT_BAR, PBM_SETRANGE32, 0, 1000);
        SetDlgItemTextW(dlg, IDC_IMPORT_TEXT, L"Opening the PS2 disc...");
        CloseHandle(CreateThread(NULL, 0, worker, j, 0, NULL));
        return TRUE;
    case WM_DUBS_PROGRESS:
        SendDlgItemMessageW(dlg, IDC_IMPORT_BAR, PBM_SETPOS, (WPARAM)lp, 0);
        return TRUE;
    case WM_DUBS_DONE:
        EndDialog(dlg, 0);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL) {
            InterlockedExchange(&j->cancel, 1);
            EnableWindow(GetDlgItem(dlg, IDCANCEL), FALSE);
            SetDlgItemTextW(dlg, IDC_IMPORT_TEXT, L"Cancelling...");
        }
        return TRUE;
    }
    return FALSE;
}

int dubs_import(HWND parent, const wchar_t *src, const wchar_t *game, const wchar_t *out, int quiet)
{
    Job *j = (Job *)calloc(1, sizeof *j);
    int result;
    if (!j)
        return DUBS_FAILED;
    wcsncpy(j->src, src, MAX_PATH - 1);
    wcsncpy(j->game, game, MAX_PATH - 1);
    wcsncpy(j->out, out, MAX_PATH - 1);
    DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_IMPORT), parent, dubs_proc, (LPARAM)j);
    result = j->result;
    if (result == DUBS_OK && !quiet)
        MessageBoxW(parent, L"The dubbed cutscenes are installed. They play in the language chosen under "
                            L"Language (French, Spanish, German, Italian or Dutch).",
                    L"The Legend of Spyro: A New Beginning", MB_ICONINFORMATION);
    else if (result != DUBS_OK && result != DUBS_CANCELLED)
        MessageBoxW(parent, j->message, L"Adding the dubbed cutscenes", MB_ICONERROR);
    free(j);
    return result;
}

int dubs_installed(const wchar_t *out)
{
    wchar_t p[MAX_PATH];
    int l;
    size_t m;
    for (l = 0; l < LANG_N; l++)
        for (m = 0; m < MOVIE_COUNT; m++) {
            swprintf(p, MAX_PATH, L"%ls\\%hs\\%hs_60.xmv.ovl", out, LANG_CODE[l], MOVIES[m].xbox);
            if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES)
                return 0;
        }
    return 1;
}
