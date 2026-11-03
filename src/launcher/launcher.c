/*
 * The launcher: a settings window for spyro.ini, and a Play button.
 *
 * It sits beside spyro_anb_recomp.exe and shares settings.c with it, so the
 * file it writes is the one the game reads, comments and all. Nothing here is
 * needed to play: the game runs on its own and keeps its own defaults.
 */
#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "settings.h"
#include "launcher_res.h"
#include "import.h"

#define GAME_EXE L"spyro_anb_recomp.exe"

static SpyroSettings s_defaults;

/* Mouse settings live in mkb.ini, read by the runtime's input layer. The
 * slider counts in twentieths: 5-80 is 0.25-4.0. */
typedef struct { int sens20, invert; } MouseSettings;
static const MouseSettings MOUSE_DEFAULTS = { 20, 0 };

static const int SIZES[][2] = {
    { 960, 540 }, { 960, 720 }, { 1280, 720 }, { 1280, 960 }, { 1366, 768 }, { 1600, 900 },
    { 1600, 1200 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 },
};

/* Path of a file beside this .exe. */
static void beside_exe(wchar_t *out, size_t cap, const wchar_t *name)
{
    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)cap);
    wchar_t *slash;
    if (!n || n >= cap) {
        out[0] = 0;
        return;
    }
    slash = wcsrchr(out, L'\\');
    if (slash && (size_t)(slash + 1 - out) + wcslen(name) < cap)
        wcscpy(slash + 1, name);
    else
        out[0] = 0;
}

static void monitor_size(HWND h, int *w, int *hh)
{
    MONITORINFO mi = { sizeof mi };
    *w = 1920;
    *hh = 1080;
    if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
        *w = mi.rcMonitor.right - mi.rcMonitor.left;
        *hh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    }
}

/* The scale the game picks for RenderScale = 0 (gpu_backend.c). */
static int auto_scale(HWND h, int wide)
{
    int w, hh, sw, sh, s;
    monitor_size(h, &w, &hh);
    sw = (wide ? w : w * 3 / 4 < hh ? w : hh * 4 / 3) + 639;
    sh = hh + 479;
    s = sw / 640 > sh / 480 ? sw / 640 : sh / 480;
    return s < 1 ? 1 : s > 4 ? 4 : s;
}

static void add_item(HWND dlg, int id, const wchar_t *text, LPARAM data)
{
    LRESULT i = SendDlgItemMessageW(dlg, id, CB_ADDSTRING, 0, (LPARAM)text);
    SendDlgItemMessageW(dlg, id, CB_SETITEMDATA, (WPARAM)i, data);
}

static void select_data(HWND dlg, int id, LPARAM data)
{
    LRESULT n = SendDlgItemMessageW(dlg, id, CB_GETCOUNT, 0, 0), i;
    for (i = 0; i < n; i++)
        if (SendDlgItemMessageW(dlg, id, CB_GETITEMDATA, (WPARAM)i, 0) == data) {
            SendDlgItemMessageW(dlg, id, CB_SETCURSEL, (WPARAM)i, 0);
            return;
        }
    SendDlgItemMessageW(dlg, id, CB_SETCURSEL, 0, 0);
}

static LPARAM selected_data(HWND dlg, int id)
{
    LRESULT i = SendDlgItemMessageW(dlg, id, CB_GETCURSEL, 0, 0);
    return i < 0 ? 0 : (LPARAM)SendDlgItemMessageW(dlg, id, CB_GETITEMDATA, (WPARAM)i, 0);
}

static void fill_scales(HWND dlg)
{
    int wide = IsDlgButtonChecked(dlg, IDC_WIDE) == BST_CHECKED, s;
    LPARAM cur = SendDlgItemMessageW(dlg, IDC_SCALE, CB_GETCOUNT, 0, 0) > 0 ? selected_data(dlg, IDC_SCALE)
                                                                           : g_settings.render_scale;
    wchar_t t[80];

    SendDlgItemMessageW(dlg, IDC_SCALE, CB_RESETCONTENT, 0, 0);
    swprintf(t, 80, L"Auto (%dx for this monitor)", auto_scale(dlg, wide));
    add_item(dlg, IDC_SCALE, t, 0);
    for (s = 1; s <= 4; s++) {
        swprintf(t, 80, L"%dx \x2013 %d\x00D7%d%ls", s, 640 * s, 480 * s, s == 1 ? L" (original)" : L"");
        add_item(dlg, IDC_SCALE, t, s);
    }
    select_data(dlg, IDC_SCALE, cur);
}

static void fill_sizes(HWND dlg, int cur_w, int cur_h)
{
    int mw, mh, have = 0;
    size_t i;
    wchar_t t[64];

    monitor_size(dlg, &mw, &mh);
    SendDlgItemMessageW(dlg, IDC_WINSIZE, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < sizeof SIZES / sizeof *SIZES; i++) {
        int w = SIZES[i][0], h = SIZES[i][1];
        if ((w > mw || h > mh) && !(w == cur_w && h == cur_h))
            continue;
        swprintf(t, 64, L"%d \x00D7 %d", w, h);
        add_item(dlg, IDC_WINSIZE, t, MAKELPARAM(h, w));
        have |= w == cur_w && h == cur_h;
    }
    if (!have) {           /* a size the player dragged the window to */
        swprintf(t, 64, L"%d \x00D7 %d (current)", cur_w, cur_h);
        add_item(dlg, IDC_WINSIZE, t, MAKELPARAM(cur_h, cur_w));
    }
    select_data(dlg, IDC_WINSIZE, MAKELPARAM(cur_h, cur_w));
}

static void update_enabled(HWND dlg)
{
    BOOL windowed = selected_data(dlg, IDC_MODE) == 0;
    wchar_t mkb[MAX_PATH];
    EnableWindow(GetDlgItem(dlg, IDC_WINSIZE), windowed);
    EnableWindow(GetDlgItem(dlg, IDC_SIZE_LBL), windowed);
    beside_exe(mkb, MAX_PATH, L"mkb.ini");
    EnableWindow(GetDlgItem(dlg, IDC_KEYS), IsDlgButtonChecked(dlg, IDC_MKB) == BST_CHECKED &&
                                                GetFileAttributesW(mkb) != INVALID_FILE_ATTRIBUTES);
    {
        BOOL mouse = IsDlgButtonChecked(dlg, IDC_MKB) == BST_CHECKED;
        EnableWindow(GetDlgItem(dlg, IDC_SENS), mouse);
        EnableWindow(GetDlgItem(dlg, IDC_SENS_LBL), mouse);
        EnableWindow(GetDlgItem(dlg, IDC_SENS_VAL), mouse);
        EnableWindow(GetDlgItem(dlg, IDC_INVERT), mouse);
    }
}

static void show_sens(HWND dlg)
{
    wchar_t t[16];
    int v = (int)SendDlgItemMessageW(dlg, IDC_SENS, TBM_GETPOS, 0, 0);
    swprintf(t, 16, L"%.2f", v / 20.0);
    SetDlgItemTextW(dlg, IDC_SENS_VAL, t);
}

static void show(HWND dlg, const SpyroSettings *s, const MouseSettings *m)
{
    SendDlgItemMessageW(dlg, IDC_SENS, TBM_SETRANGE, TRUE, MAKELPARAM(5, 80));
    SendDlgItemMessageW(dlg, IDC_SENS, TBM_SETPAGESIZE, 0, 5);
    SendDlgItemMessageW(dlg, IDC_SENS, TBM_SETPOS, TRUE, m->sens20);
    show_sens(dlg);
    CheckDlgButton(dlg, IDC_INVERT, m->invert ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_SKIP, s->skip_intros ? BST_CHECKED : BST_UNCHECKED);
    select_data(dlg, IDC_MODE, s->fullscreen);
    fill_sizes(dlg, s->window_width, s->window_height);
    CheckDlgButton(dlg, IDC_WIDE, s->widescreen ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_VSYNC, s->vsync ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_MKB, s->keyboard_mouse ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageW(dlg, IDC_SCALE, CB_RESETCONTENT, 0, 0);
    g_settings.render_scale = s->render_scale;
    fill_scales(dlg);
    update_enabled(dlg);
}

static void save(HWND dlg)
{
    LPARAM size = selected_data(dlg, IDC_WINSIZE);
    SpyroSettings s = g_settings;

    s.fullscreen = selected_data(dlg, IDC_MODE) != 0;
    if (size) {
        s.window_width = HIWORD(size);
        s.window_height = LOWORD(size);
    }
    s.render_scale = (int)selected_data(dlg, IDC_SCALE);
    s.widescreen = IsDlgButtonChecked(dlg, IDC_WIDE) == BST_CHECKED;
    s.vsync = IsDlgButtonChecked(dlg, IDC_VSYNC) == BST_CHECKED;
    s.keyboard_mouse = IsDlgButtonChecked(dlg, IDC_MKB) == BST_CHECKED;
    s.skip_intros = IsDlgButtonChecked(dlg, IDC_SKIP) == BST_CHECKED;

    settings_set_int("Fullscreen", s.fullscreen);
    settings_set_int("WindowWidth", s.window_width);
    settings_set_int("WindowHeight", s.window_height);
    settings_set_int("Widescreen", s.widescreen);
    settings_set_int("RenderScale", s.render_scale);
    settings_set_int("VSync", s.vsync);
    settings_set_int("KeyboardMouse", s.keyboard_mouse);
    settings_set_int("SkipIntros", s.skip_intros);
    g_settings = s;
    {
        char v[16];
        snprintf(v, sizeof v, "%.2f", SendDlgItemMessageW(dlg, IDC_SENS, TBM_GETPOS, 0, 0) / 20.0);
        settings_set_mkb("sens", v);
        settings_set_mkb("invert_y", IsDlgButtonChecked(dlg, IDC_INVERT) == BST_CHECKED ? "1" : "0");
    }
}

static void open_saves(HWND dlg)
{
    wchar_t dir[MAX_PATH], udata[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"RECOMP_SAVE_DIR", dir, MAX_PATH);

    if (!n || n >= MAX_PATH) {
        n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
        if (!n || n >= MAX_PATH - 16)
            return;
        wcscat(dir, L"\\xboxrecomp");
    }
    swprintf(udata, MAX_PATH, L"%ls\\UDATA", dir);
    if (GetFileAttributesW(udata) != INVALID_FILE_ATTRIBUTES)
        ShellExecuteW(dlg, L"open", udata, NULL, NULL, SW_SHOWNORMAL);
    else if (GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES)
        ShellExecuteW(dlg, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
    else
        MessageBoxW(dlg, L"There are no saves yet. The folder is made the first time the game saves.",
                    L"Saves folder", MB_ICONINFORMATION);
}

/* game_files beside the launcher, where Setup puts the game. */
static void game_dir(wchar_t *out)
{
    beside_exe(out, MAX_PATH, L"game_files");
}

/* Whether the game will find its files: game_files, or a development tree
 * the build was configured with. */
static int game_present(void)
{
    wchar_t d[MAX_PATH];
    game_dir(d);
    if (import_installed(d))
        return 1;
#ifdef SPYRO_DEV_GAME_DIR
    return import_installed(L"" SPYRO_DEV_GAME_DIR);
#else
    return 0;
#endif
}

static void show_game_status(HWND dlg)
{
    int ok = game_present();
    SetDlgItemTextW(dlg, IDC_GAME_STATUS, ok ? L"Game files: installed" : L"Game files: not installed yet");
    SetDlgItemTextW(dlg, IDC_GAMEFILES, ok ? L"Reinstall game files..." : L"Install game files...");
}

/* A folder or disc drive (pick_folder) or a disc image file. */
static int pick(HWND dlg, int pick_folder, wchar_t *out)
{
    IFileOpenDialog *fd;
    IShellItem *item;
    PWSTR path = NULL;
    int ok = 0;

    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog,
                                (void **)&fd)))
        return 0;
    if (pick_folder) {
        IFileOpenDialog_SetOptions(fd, FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        IFileOpenDialog_SetTitle(fd, L"The game's folder or disc drive (the one with default.xbe)");
    } else {
        static const COMDLG_FILTERSPEC TYPES[] = {
            { L"Xbox disc images (*.iso, *.xiso)", L"*.iso;*.xiso" }, { L"All files", L"*.*" },
        };
        IFileOpenDialog_SetOptions(fd, FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
        IFileOpenDialog_SetFileTypes(fd, 2, TYPES);
        IFileOpenDialog_SetTitle(fd, L"The game's Xbox disc image");
    }
    if (SUCCEEDED(IFileOpenDialog_Show(fd, dlg)) && SUCCEEDED(IFileOpenDialog_GetResult(fd, &item))) {
        if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
            wcsncpy(out, path, MAX_PATH - 1);
            out[MAX_PATH - 1] = 0;
            CoTaskMemFree(path);
            ok = 1;
        }
        IShellItem_Release(item);
    }
    IFileOpenDialog_Release(fd);
    return ok;
}

/* Asks where the game is and installs it. */
static void install_game_files(HWND dlg)
{
    static const TASKDIALOG_BUTTON BUTTONS[] = {
        { 100, L"From a folder or disc\nThe game's files on your PC, or the disc in a drive that can read it." },
        { 101, L"From a disc image\nAn .iso of the Xbox disc (a full image or a trimmed xiso)." },
    };
    TASKDIALOGCONFIG tc;
    wchar_t src[MAX_PATH], dest[MAX_PATH];
    int choice = 0;

    memset(&tc, 0, sizeof tc);
    tc.cbSize = sizeof tc;
    tc.hwndParent = dlg;
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.pszWindowTitle = L"Install game files";
    tc.pszMainInstruction = L"Where is your copy of the game?";
    tc.pszContent = L"The game's files are copied from your own copy of The Legend of Spyro: A New Beginning "
                    L"for the original Xbox (about 2 GB).";
    tc.pButtons = BUTTONS;
    tc.cButtons = 2;
    if (FAILED(TaskDialogIndirect(&tc, &choice, NULL, NULL)) || (choice != 100 && choice != 101))
        return;
    if (!pick(dlg, choice == 100, src))
        return;
    game_dir(dest);
    import_game(dlg, src, dest, 0);
    show_game_status(dlg);
}

static int play(HWND dlg)
{
    wchar_t exe[MAX_PATH], dir[MAX_PATH], *slash;
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;

    if (!game_present()) {
        if (MessageBoxW(dlg, L"The game files are not installed yet. Install them now?",
                        L"The Legend of Spyro: A New Beginning", MB_YESNO | MB_ICONQUESTION) == IDYES)
            install_game_files(dlg);
        if (!game_present())
            return 0;
    }
    beside_exe(exe, MAX_PATH, GAME_EXE);
    wcscpy(dir, exe);
    slash = wcsrchr(dir, L'\\');
    if (slash)
        *slash = 0;
    if (!exe[0] || !CreateProcessW(exe, NULL, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        wchar_t msg[MAX_PATH + 64];
        swprintf(msg, MAX_PATH + 64, L"Could not start the game:\n%ls", exe);
        MessageBoxW(dlg, msg, L"Play", MB_ICONERROR);
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

static INT_PTR CALLBACK dlg_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1)));
        SendMessageW(dlg, WM_SETICON, ICON_SMALL,
                     (LPARAM)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0));
        add_item(dlg, IDC_MODE, L"Windowed", 0);
        add_item(dlg, IDC_MODE, L"Fullscreen (borderless)", 1);
        {
            MouseSettings m;
            double sens = settings_get_mkb("sens", 1.0);
            m.sens20 = (int)(sens * 20.0 + 0.5);
            m.sens20 = m.sens20 < 5 ? 5 : m.sens20 > 80 ? 80 : m.sens20;
            m.invert = settings_get_mkb("invert_y", 0.0) != 0.0;
            show(dlg, &g_settings, &m);
        }
        show_game_status(dlg);
        return TRUE;
    case WM_HSCROLL:
        if ((HWND)lp == GetDlgItem(dlg, IDC_SENS))
            show_sens(dlg);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_MODE:
            if (HIWORD(wp) == CBN_SELCHANGE)
                update_enabled(dlg);
            return TRUE;
        case IDC_WIDE:
            fill_scales(dlg);
            return TRUE;
        case IDC_MKB:
            update_enabled(dlg);
            return TRUE;
        case IDC_KEYS: {
            wchar_t mkb[MAX_PATH];
            beside_exe(mkb, MAX_PATH, L"mkb.ini");
            ShellExecuteW(dlg, L"open", mkb, NULL, NULL, SW_SHOWNORMAL);
            return TRUE;
        }
        case IDC_SAVES:
            open_saves(dlg);
            return TRUE;
        case IDC_GAMEFILES:
            install_game_files(dlg);
            return TRUE;
        case IDC_DEFAULTS:
            show(dlg, &s_defaults, &MOUSE_DEFAULTS);
            return TRUE;
        case IDC_SAVE:
            save(dlg);
            return TRUE;
        case IDOK:
            save(dlg);
            if (play(dlg))
                EndDialog(dlg, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_DPICHANGED:
        fill_scales(dlg);   /* moved to another monitor: its auto scale */
        break;
    }
    return FALSE;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show_cmd)
{
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_BAR_CLASSES | ICC_PROGRESS_CLASS };
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    (void)prev;
    (void)cmd;
    (void)show_cmd;
    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    /* Setup: spyro_launcher.exe /import <folder, drive or image>. The exit
     * code is the IMPORT_ result. */
    if (argv && argc >= 3 && !_wcsicmp(argv[1], L"/import")) {
        wchar_t dest[MAX_PATH];
        game_dir(dest);
        return import_game(NULL, argv[2], dest, 1);
    }
    s_defaults = g_settings;
    settings_load();
    settings_ensure_mkb();
    DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_LAUNCHER), NULL, dlg_proc, 0);
    return 0;
}
