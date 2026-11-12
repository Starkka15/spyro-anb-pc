/*
 * PC wording for the game's text.
 *
 * A handful of lines talk about the Xbox: the autosave warning ("don't turn
 * off your Xbox console"), the disc error, free blocks, and the Xbox
 * Dashboard, which on PC just closes the game. tools/text_pc.py has the
 * replacements in all six languages (text_pc.inc).
 *
 * The text is translations.<Language>.txt in data_xb.rkv: KEY[=id] TAB text,
 * CRLF, cp1252. The new text is longer, so like the PS2 import (dubs.c) the
 * overlay puts it in data_xb.rkv's largest unused gap and rewrites the file's
 * table entry (size, offset, CRC-32). Overlay format (kernel_file.c): "ROVL",
 * version 1, span count, then { u64 offset, u32 length, u32 0 } per span and
 * the spans' bytes. Ours has two spans: the 20-byte entry, then the text.
 *
 * Shared by the game (refreshed at start for the chosen language) and the
 * launcher (after the PS2 import rewrites the overlays).
 */
#include "text_pc.h"

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const LANG_FILE[TEXT_LANGS] = { "English", "French", "Spanish", "German", "Italian", "Dutch" };

static const struct { int lang; const char *key, *text; } PC[] = {
#include "text_pc.inc"
};

#define GLYPH_OPEN "<font=font_buttons:0>"
#define GLYPH_CLOSE "</font>"

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int read_at(HANDLE h, uint64_t off, void *p, DWORD n, DWORD *got)
{
    LARGE_INTEGER li;
    DWORD r = 0;
    li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN) || !ReadFile(h, p, n, &r, NULL))
        return 0;
    if (got)
        *got = r;
    return got ? 1 : r == n;
}

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int b;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (b = 0; b < 8; b++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* The largest gap between the archive's entries, where nothing points. */
static int largest_gap(const uint8_t *ent, uint32_t n, uint64_t *at, uint64_t *size)
{
    uint64_t *e = (uint64_t *)malloc((size_t)n * 2 * sizeof *e);
    uint32_t i;
    if (!e)
        return 0;
    for (i = 0; i < n; i++) {
        e[i * 2] = rd32(ent + i * 20 + 12);
        e[i * 2 + 1] = rd32(ent + i * 20 + 8);
    }
    qsort(e, n, 2 * sizeof *e, cmp_u64);
    *size = 0;
    for (i = 0; i + 1 < n; i++) {
        uint64_t end = e[i * 2] + e[i * 2 + 1];
        if (e[(i + 1) * 2] > end && e[(i + 1) * 2] - end > *size) {
            *size = e[(i + 1) * 2] - end;
            *at = end;
        }
    }
    free(e);
    return *size > 0;
}

typedef struct { char *b; size_t n, cap; } Out;

static int out_add(Out *o, const char *p, size_t n)
{
    if (o->n + n > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 65536;
        char *g;
        while (cap < o->n + n)
            cap *= 2;
        if (!(g = (char *)realloc(o->b, cap)))
            return 0;
        o->b = g;
        o->cap = cap;
    }
    memcpy(o->b + o->n, p, n);
    o->n += n;
    return 1;
}

/* `text` with each '@' replaced, in order, by the glyph tags of `old`. */
static int put_pc(Out *o, const char *text, const char *old, size_t old_n)
{
    size_t ol = strlen(GLYPH_OPEN), cl = strlen(GLYPH_CLOSE), i = 0;
    for (; *text; text++) {
        if (*text != '@') {
            if (!out_add(o, text, 1))
                return 0;
            continue;
        }
        while (i + ol <= old_n && memcmp(old + i, GLYPH_OPEN, ol))
            i++;
        if (i + ol <= old_n) {
            size_t e = i + ol;
            while (e + cl <= old_n && memcmp(old + e, GLYPH_CLOSE, cl))
                e++;
            if (e + cl <= old_n) {
                if (!out_add(o, old + i, e + cl - i))
                    return 0;
                i = e + cl;
            }
        }
    }
    return 1;
}

/* The text with the language's PC lines in place; everything else as it was. */
static int apply(const char *t, size_t n, int lang, Out *o)
{
    size_t i = 0;
    while (i < n) {
        size_t e = i, tab, key_len, k;
        int done = 0;
        while (e < n && t[e] != '\r' && t[e] != '\n')
            e++;
        for (tab = i; tab < e && t[tab] != '\t'; tab++) {}
        for (key_len = 0; i + key_len < tab && t[i + key_len] != '='; key_len++) {}
        if (tab < e)
            for (k = 0; k < sizeof PC / sizeof *PC && !done; k++)
                if (PC[k].lang == lang && strlen(PC[k].key) == key_len && !memcmp(PC[k].key, t + i, key_len)) {
                    if (!out_add(o, t + i, tab + 1 - i) || !put_pc(o, PC[k].text, t + tab + 1, e - tab - 1))
                        return 0;
                    done = 1;
                }
        if (!done && !out_add(o, t + i, e - i))
            return 0;
        i = e;
        while (i < n && (t[i] == '\r' || t[i] == '\n'))
            i++;
        if (!out_add(o, t + e, i - e))
            return 0;
    }
    return 1;
}

static int write_all(HANDLE h, const void *p, DWORD n)
{
    DWORD w;
    return WriteFile(h, p, n, &w, NULL) && w == n;
}

int text_pc_refresh(const wchar_t *game_dir, const wchar_t *lang_dir, int lang)
{
    wchar_t path[MAX_PATH], tmp[MAX_PATH];
    char name[64];
    uint8_t head[0x80], *ent = NULL, rec[20];
    char *names = NULL, *cur = NULL;
    uint32_t n, i, idx = 0, cur_n = 0;
    DWORD got;
    HANDLE h, f;
    Out o = { 0 };
    uint64_t gap = 0, gap_size = 0, ent_off;
    int ok = 0, found = 0, had_overlay = 0;

    if (lang < 0 || lang >= TEXT_LANGS)
        return 0;
    swprintf(path, MAX_PATH, L"%ls\\data_xb.rkv", game_dir);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    if (!read_at(h, 0, head, sizeof head, NULL) || memcmp(head, "RKV2", 4) || !(n = rd32(head + 4)) || n > 65536)
        goto done;
    ent = (uint8_t *)malloc((size_t)n * 20);
    names = (char *)calloc(1, (1u << 20) + 1);
    if (!ent || !names || !read_at(h, 0x80, ent, n * 20, NULL) || !read_at(h, 0x80 + (uint64_t)n * 20, names, 1u << 20, &got))
        goto done;
    snprintf(name, sizeof name, "translations.%s.txt", LANG_FILE[lang]);
    for (i = 0; i < n && !found; i++) {
        uint32_t no = rd32(ent + i * 20);
        if (no < got && !_stricmp(names + no, name)) {
            idx = i;
            found = 1;
        }
    }
    if (!found || !largest_gap(ent, n, &gap, &gap_size))
        goto done;
    ent_off = 0x80 + (uint64_t)idx * 20;

    /* The text now: the overlay's, when it is ours (entry + text), else the game's. */
    swprintf(path, MAX_PATH, L"%ls\\data_xb.rkv.ovl", lang_dir);
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        uint8_t oh[44];
        if (read_at(f, 0, oh, sizeof oh, NULL) && !memcmp(oh, "ROVL", 4) && rd32(oh + 4) == 1 && rd32(oh + 8) == 2
            && rd32(oh + 12) == (uint32_t)ent_off && rd32(oh + 16) == 0 && rd32(oh + 20) == 20
            && rd32(oh + 36) < (64u << 20) && (cur_n = rd32(oh + 36)) != 0
            && (cur = (char *)malloc(cur_n)) != NULL && read_at(f, sizeof oh + 20, cur, cur_n, NULL))
            had_overlay = 1;
        CloseHandle(f);
        if (!had_overlay) {
            free(cur);
            cur = NULL;
        }
    }
    if (!had_overlay) {
        cur_n = rd32(ent + idx * 20 + 8);
        if (cur_n > (64u << 20) || !(cur = (char *)malloc(cur_n))
            || !read_at(h, rd32(ent + idx * 20 + 12), cur, cur_n, NULL))
            goto done;
    }
    if (!apply(cur, cur_n, lang, &o))
        goto done;
    if (o.n == cur_n && !memcmp(o.b, cur, cur_n)) {
        ok = 1;                 /* already current */
        goto done;
    }
    if (o.n > gap_size)
        goto done;

    /* The table entry: name offset and flags kept; size, offset, CRC new. */
    memcpy(rec, ent + idx * 20, 20);
    *(uint32_t *)(rec + 8) = (uint32_t)o.n;
    *(uint32_t *)(rec + 12) = (uint32_t)gap;
    *(uint32_t *)(rec + 16) = crc32_of((const uint8_t *)o.b, o.n);

    {
        wchar_t parent[MAX_PATH], *slash;
        wcsncpy(parent, lang_dir, MAX_PATH - 1);
        parent[MAX_PATH - 1] = 0;
        if ((slash = wcsrchr(parent, L'\\')) != NULL) {
            *slash = 0;
            CreateDirectoryW(parent, NULL);
        }
        CreateDirectoryW(lang_dir, NULL);
    }
    /* Written beside it, then moved over it: a crash leaves the old file. */
    swprintf(tmp, MAX_PATH, L"%ls\\data_xb.rkv.ovl.new", lang_dir);
    f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        goto done;
    {
        uint8_t oh[44];
        uint32_t len = 20, size = (uint32_t)o.n, zero = 0;
        memcpy(oh, "ROVL", 4);
        *(uint32_t *)(oh + 4) = 1;
        *(uint32_t *)(oh + 8) = 2;
        memcpy(oh + 12, &ent_off, 8);
        memcpy(oh + 20, &len, 4);
        memcpy(oh + 24, &zero, 4);
        memcpy(oh + 28, &gap, 8);
        memcpy(oh + 36, &size, 4);
        memcpy(oh + 40, &zero, 4);
        ok = write_all(f, oh, sizeof oh) && write_all(f, rec, 20) && write_all(f, o.b, (DWORD)o.n);
    }
    CloseHandle(f);
    if (ok)
        ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
    if (!ok)
        DeleteFileW(tmp);
done:
    free(o.b);
    free(cur);
    free(names);
    free(ent);
    CloseHandle(h);
    return ok;
}
