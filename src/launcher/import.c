/*
 * Installing the game files from the player's own copy of the game.
 *
 * The port ships without any of the game: Setup (and the launcher, when the
 * files are missing) asks where the game is and copies it into game_files
 * beside the .exe. The source can be
 *
 *   - a folder, or a disc drive, holding the game's files (default.xbe at
 *     its top), or
 *   - an Xbox disc image. Windows cannot open these -- the Xbox's disc file
 *     system, XDVDFS, is its own -- so it is read here. Both common layouts
 *     are found: a trimmed "xiso" with the file system at the start, and a
 *     full disc image (redump) with the video partition ahead of it.
 *
 * Before anything is copied, default.xbe is checked against the one this
 * port was recompiled from. The recompiled code is that program, so any
 * other version or region would start and then go wrong in strange ways;
 * better to say so up front.
 *
 * XDVDFS: 2048-byte sectors. Sector 32 is the volume descriptor:
 * "MICROSOFT*XBOX*MEDIA", then the root directory's sector and size. A
 * directory is a binary tree of entries packed into its sectors: left and
 * right child offsets (16-bit, in dwords from the directory's start), the
 * file's sector and size, attributes, name length, name. 0xFFFF fills the
 * unused end of a sector.
 */
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "import.h"
#include "launcher_res.h"

/* SHA-1 of the default.xbe this build was recompiled from. */
static const char XBE_SHA1[] = "475b618a459ad788da063269f29a4eb82206f09f";
#define SECTOR 2048u
#define WM_IMPORT_PROGRESS (WM_APP + 1)
#define WM_IMPORT_DONE     (WM_APP + 2)

typedef struct {
    wchar_t rel[MAX_PATH];      /* path below the game's root */
    uint64_t size;
    uint64_t offset;            /* image: byte offset of the data */
} Entry;

typedef struct {
    /* in */
    wchar_t src[MAX_PATH], dest[MAX_PATH];
    HWND dlg;
    volatile LONG cancel;
    /* out */
    int result;
    wchar_t message[512];
    /* work */
    HANDLE image;               /* NULL: a folder */
    uint64_t base;              /* image: where the file system starts */
    Entry *list;
    size_t n, cap;
    uint64_t total, done;
} Job;

static int add(Job *j, const wchar_t *rel, uint64_t size, uint64_t offset)
{
    if (j->n == j->cap) {
        size_t cap = j->cap ? j->cap * 2 : 256;
        Entry *l = (Entry *)realloc(j->list, cap * sizeof *l);
        if (!l)
            return 0;
        j->list = l;
        j->cap = cap;
    }
    wcsncpy(j->list[j->n].rel, rel, MAX_PATH - 1);
    j->list[j->n].rel[MAX_PATH - 1] = 0;
    j->list[j->n].size = size;
    j->list[j->n].offset = offset;
    j->n++;
    j->total += size;
    return 1;
}

static int read_at(HANDLE h, uint64_t off, void *buf, DWORD len)
{
    OVERLAPPED ov;
    DWORD got = 0;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    return ReadFile(h, buf, len, &got, &ov) && got == len;
}

/* ---- the file list ----------------------------------------------------- */

static int list_folder(Job *j, const wchar_t *rel)
{
    wchar_t pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    int ok = 1;

    swprintf(pat, MAX_PATH, L"%ls\\%ls%ls*", j->src, rel, *rel ? L"\\" : L"");
    f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return 1;
    do {
        wchar_t sub[MAX_PATH];
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
            continue;
        swprintf(sub, MAX_PATH, L"%ls%ls%ls", rel, *rel ? L"\\" : L"", fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ok = list_folder(j, sub);
        else
            ok = add(j, sub, (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow, 0);
    } while (ok && FindNextFileW(f, &fd));
    FindClose(f);
    return ok;
}

static int list_image_dir(Job *j, uint32_t sector, uint32_t size, const wchar_t *rel, int depth)
{
    uint8_t *buf;
    uint32_t *stack, sp = 0, visited = 0, limit;
    int ok = 1;

    if (!size || depth > 32)
        return 1;
    if (size > 64u << 20)
        return 0;
    buf = (uint8_t *)malloc(size);
    limit = size / 14 + 1;
    stack = (uint32_t *)malloc((size_t)limit * 2 * sizeof *stack);
    if (!buf || !stack || !read_at(j->image, j->base + (uint64_t)sector * SECTOR, buf, size)) {
        free(buf);
        free(stack);
        return 0;
    }
    stack[sp++] = 0;
    while (ok && sp && visited++ < limit) {
        uint32_t off = stack[--sp];
        const uint8_t *e = buf + off;
        uint16_t left, right;
        uint32_t fsector, fsize;
        uint8_t attr, nlen;
        wchar_t name[256], sub[MAX_PATH];
        int i;

        if (off + 14 > size)
            continue;
        left = (uint16_t)(e[0] | e[1] << 8);
        right = (uint16_t)(e[2] | e[3] << 8);
        if (left == 0xFFFF)                     /* sector padding */
            continue;
        fsector = (uint32_t)e[4] | (uint32_t)e[5] << 8 | (uint32_t)e[6] << 16 | (uint32_t)e[7] << 24;
        fsize = (uint32_t)e[8] | (uint32_t)e[9] << 8 | (uint32_t)e[10] << 16 | (uint32_t)e[11] << 24;
        attr = e[12];
        nlen = e[13];
        if (off + 14 + nlen > size)
            continue;
        if (left && sp < limit * 2)
            stack[sp++] = (uint32_t)left * 4;
        if (right && sp < limit * 2)
            stack[sp++] = (uint32_t)right * 4;
        for (i = 0; i < nlen; i++)
            name[i] = e[14 + i];
        name[nlen] = 0;
        if (!nlen || wcschr(name, L'\\') || wcschr(name, L'/') || !wcscmp(name, L"..") || !wcscmp(name, L"."))
            continue;
        swprintf(sub, MAX_PATH, L"%ls%ls%ls", rel, *rel ? L"\\" : L"", name);
        if (attr & 0x10)
            ok = list_image_dir(j, fsector, fsize, sub, depth + 1);
        else
            ok = add(j, sub, fsize, j->base + (uint64_t)fsector * SECTOR);
    }
    free(buf);
    free(stack);
    return ok;
}

/* Where the XDVDFS volume starts: a trimmed image, a full disc (redump,
 * XGD1), and the later disc layouts' offsets. */
static int find_volume(Job *j, uint32_t *root_sector, uint32_t *root_size)
{
    static const uint64_t BASES[] = { 0, 0x18300000ull, 0xFD90000ull, 0x2080000ull };
    uint8_t vd[28];
    size_t i;
    for (i = 0; i < sizeof BASES / sizeof *BASES; i++) {
        if (read_at(j->image, BASES[i] + 32 * SECTOR, vd, sizeof vd) && !memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20)) {
            j->base = BASES[i];
            *root_sector = (uint32_t)vd[20] | (uint32_t)vd[21] << 8 | (uint32_t)vd[22] << 16 | (uint32_t)vd[23] << 24;
            *root_size = (uint32_t)vd[24] | (uint32_t)vd[25] << 8 | (uint32_t)vd[26] << 16 | (uint32_t)vd[27] << 24;
            return 1;
        }
    }
    return 0;
}

/* ---- copying ----------------------------------------------------------- */

static void progress(Job *j)
{
    PostMessageW(j->dlg, WM_IMPORT_PROGRESS, 0, j->total ? (LPARAM)(j->done * 1000 / j->total) : 0);
}

/* Streams one file's bytes into `out` (NULL: nowhere) and the hash (NULL:
 * none). Returns 0 on a read or write error, -1 when cancelled. */
static int stream(Job *j, const Entry *e, HANDLE out, BCRYPT_HASH_HANDLE hash)
{
    static uint8_t buf[4u << 20];
    HANDLE in = j->image;
    uint64_t left = e->size, pos = e->offset;
    int ret = 1;

    if (!in) {
        wchar_t p[MAX_PATH];
        swprintf(p, MAX_PATH, L"%ls\\%ls", j->src, e->rel);
        in = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (in == INVALID_HANDLE_VALUE)
            return 0;
        pos = 0;
    }
    while (left && ret > 0) {
        DWORD n = left > sizeof buf ? (DWORD)sizeof buf : (DWORD)left, wrote;
        if (j->cancel) {
            ret = -1;
            break;
        }
        if (!read_at(in, pos, buf, n)) {
            ret = 0;
            break;
        }
        if (hash)
            BCryptHashData(hash, buf, n, 0);
        if (out && (!WriteFile(out, buf, n, &wrote, NULL) || wrote != n)) {
            ret = 0;
            break;
        }
        pos += n;
        left -= n;
        if (out) {
            j->done += n;
            progress(j);
        }
    }
    if (in != j->image)
        CloseHandle(in);
    return ret;
}

static int xbe_matches(Job *j, const Entry *xbe)
{
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hash;
    uint8_t digest[20];
    char hex[41];
    int ok = 0, i;

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, 0) != 0)
        return 1;           /* cannot check: let the game decide */
    if (BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) == 0) {
        if (stream(j, xbe, NULL, hash) > 0 && BCryptFinishHash(hash, digest, sizeof digest, 0) == 0) {
            for (i = 0; i < 20; i++)
                sprintf(hex + i * 2, "%02x", digest[i]);
            ok = !strcmp(hex, XBE_SHA1);
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static int make_dirs(const wchar_t *path)
{
    wchar_t p[MAX_PATH], *s;
    wcsncpy(p, path, MAX_PATH - 1);
    p[MAX_PATH - 1] = 0;
    for (s = p + 3; (s = wcschr(s, L'\\')) != NULL; s++) {
        *s = 0;
        CreateDirectoryW(p, NULL);
        *s = L'\\';
    }
    return 1;
}

static DWORD WINAPI worker(LPVOID arg)
{
    Job *j = (Job *)arg;
    DWORD attr = GetFileAttributesW(j->src);
    const Entry *xbe = NULL;
    ULARGE_INTEGER avail;
    size_t i;

    j->result = IMPORT_FAILED;
    if (attr == INVALID_FILE_ATTRIBUTES) {
        swprintf(j->message, 512, L"%ls could not be opened.", j->src);
        goto done;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        size_t len = wcslen(j->src);
        if (len > 3 && j->src[len - 1] == L'\\')
            j->src[len - 1] = 0;
        if (!list_folder(j, L"")) {
            wcscpy(j->message, L"Out of memory while listing the game's files.");
            goto done;
        }
    } else {
        uint32_t root, rsize;
        j->image = CreateFileW(j->src, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
        if (j->image == INVALID_HANDLE_VALUE) {
            j->image = NULL;
            swprintf(j->message, 512, L"%ls could not be opened.", j->src);
            goto done;
        }
        if (!find_volume(j, &root, &rsize)) {
            wcscpy(j->message, L"This file is not an Xbox disc image.");
            goto done;
        }
        if (!list_image_dir(j, root, rsize, L"", 0)) {
            wcscpy(j->message, L"The disc image is damaged: its file list could not be read.");
            goto done;
        }
    }
    for (i = 0; i < j->n; i++)
        if (!_wcsicmp(j->list[i].rel, L"default.xbe"))
            xbe = &j->list[i];
    if (!xbe) {
        wcscpy(j->message, L"No default.xbe there. Choose the top folder of the game, the disc drive, or an "
                           L"Xbox disc image of The Legend of Spyro: A New Beginning.");
        goto done;
    }
    SetDlgItemTextW(j->dlg, IDC_IMPORT_TEXT, L"Checking the game's version...");
    if (!xbe_matches(j, xbe)) {
        j->result = IMPORT_WRONG_VERSION;
        wcscpy(j->message, L"This is a different version of the game than the one this port was made from "
                           L"(The Legend of Spyro: A New Beginning for the original Xbox, title VU-074, "
                           L"version 1). Other versions do not work with it.");
        goto done;
    }
    if (GetDiskFreeSpaceExW(j->dest, &avail, NULL, NULL) && avail.QuadPart < j->total + (64u << 20)) {
        swprintf(j->message, 512, L"There is not enough free space: the game needs %.1f GB.",
                 (double)j->total / (1u << 30));
        goto done;
    }
    for (i = 0; i < j->n; i++) {
        wchar_t out[MAX_PATH], text[MAX_PATH + 32];
        HANDLE h;
        int r;
        swprintf(out, MAX_PATH, L"%ls\\%ls", j->dest, j->list[i].rel);
        swprintf(text, MAX_PATH + 32, L"Copying %ls", j->list[i].rel);
        SetDlgItemTextW(j->dlg, IDC_IMPORT_TEXT, text);
        make_dirs(out);
        h = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            swprintf(j->message, 512, L"%ls could not be written.", out);
            goto done;
        }
        r = stream(j, &j->list[i], h, NULL);
        CloseHandle(h);
        if (r <= 0) {
            DeleteFileW(out);
            if (r < 0) {
                j->result = IMPORT_CANCELLED;
                wcscpy(j->message, L"Cancelled.");
            } else {
                swprintf(j->message, 512, L"%ls could not be copied. If it is on a disc, the disc may be "
                                          L"scratched or dirty.", j->list[i].rel);
            }
            goto done;
        }
    }
    j->result = IMPORT_OK;
done:
    if (j->image)
        CloseHandle(j->image);
    free(j->list);
    PostMessageW(j->dlg, WM_IMPORT_DONE, 0, 0);
    return 0;
}

static INT_PTR CALLBACK import_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    Job *j = (Job *)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG:
        j = (Job *)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)j);
        j->dlg = dlg;
        SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1)));
        SendDlgItemMessageW(dlg, IDC_IMPORT_BAR, PBM_SETRANGE32, 0, 1000);
        SetDlgItemTextW(dlg, IDC_IMPORT_TEXT, L"Reading the game's file list...");
        CloseHandle(CreateThread(NULL, 0, worker, j, 0, NULL));
        return TRUE;
    case WM_IMPORT_PROGRESS:
        SendDlgItemMessageW(dlg, IDC_IMPORT_BAR, PBM_SETPOS, (WPARAM)lp, 0);
        return TRUE;
    case WM_IMPORT_DONE:
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

int import_game(HWND parent, const wchar_t *src, const wchar_t *dest, int quiet)
{
    Job *j = (Job *)calloc(1, sizeof *j);
    int result;
    if (!j)
        return IMPORT_FAILED;
    wcsncpy(j->src, src, MAX_PATH - 1);
    wcsncpy(j->dest, dest, MAX_PATH - 1);
    CreateDirectoryW(dest, NULL);
    DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_IMPORT), parent, import_proc, (LPARAM)j);
    result = j->result;
    if (result == IMPORT_OK && !quiet)
        MessageBoxW(parent, L"The game files are installed.", L"The Legend of Spyro: A New Beginning",
                    MB_ICONINFORMATION);
    else if (result != IMPORT_OK && result != IMPORT_CANCELLED)
        MessageBoxW(parent, j->message, L"Installing the game files", MB_ICONERROR);
    free(j);
    return result;
}

int import_installed(const wchar_t *dest)
{
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%ls\\default.xbe", dest);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}
