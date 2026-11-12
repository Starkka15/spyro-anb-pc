/*
 * spyro.ini: see settings.h.
 *
 * The format is the plain key = value most PC games use, with ; comments and
 * [section] headers that are there for the reader only: keys are unique, so
 * a key moved to another section still works. Unknown keys are left alone,
 * and a value out of range falls back to its default.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"

SpyroSettings g_settings = {
    .fullscreen     = 0,
    .window_width   = 1280,
    .window_height  = 720,
    .render_scale   = 0,
    .vsync          = 0,
    .widescreen     = 1,
    .keyboard_mouse = 1,
    .skip_intros    = 1,
    .language       = LANGUAGE_AUTO,
};

static const char *const PROMPTS_NAMES[PROMPTS_COUNT] = { "Auto", "Xbox", "PlayStation", "Switch" };

const char *settings_prompts_name(int prompts)
{
    return prompts >= 0 && prompts < PROMPTS_COUNT ? PROMPTS_NAMES[prompts] : PROMPTS_NAMES[0];
}

static const char *const LANGUAGE_NAMES[LANGUAGE_COUNT] = {
    "Auto", "English", "French", "Spanish", "German", "Italian", "Dutch",
};

const char *settings_language_name(int language)
{
    return language >= 0 && language < LANGUAGE_COUNT ? LANGUAGE_NAMES[language] : LANGUAGE_NAMES[0];
}

int settings_language_resolve(int language)
{
    if (language > LANGUAGE_AUTO && language < LANGUAGE_COUNT)
        return language;
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_FRENCH:  return LANGUAGE_FRENCH;
    case LANG_SPANISH: return LANGUAGE_SPANISH;
    case LANG_GERMAN:  return LANGUAGE_GERMAN;
    case LANG_ITALIAN: return LANGUAGE_ITALIAN;
    case LANG_DUTCH:   return LANGUAGE_DUTCH;
    default:           return LANGUAGE_ENGLISH;
    }
}

static const char DEFAULT_FILE[] =
    "; The Legend of Spyro: A New Beginning -- settings\r\n"
    "; Delete this file to get the defaults back.\r\n"
    "\r\n"
    "[Display]\r\n"
    "; 1 = borderless fullscreen. Alt+Enter switches in game and is remembered.\r\n"
    "Fullscreen = 0\r\n"
    "; Window size in windowed mode. Remembered when you resize the window.\r\n"
    "WindowWidth = 1280\r\n"
    "WindowHeight = 720\r\n"
    "; 1 = 16:9, the game's own widescreen mode. 0 = the original 4:3.\r\n"
    "Widescreen = 1\r\n"
    "; Sharpness: the game draws at 640x480 times this (1-4). Higher costs more GPU.\r\n"
    "; 0 = enough to cover your monitor (3 at 1080p, 4 at 1440p and up); anything\r\n"
    "; above the screen's size is scaled down, which smooths jagged edges.\r\n"
    "RenderScale = 0\r\n"
    "; 1 = wait for the monitor's refresh (no tearing, can add a little input lag).\r\n"
    "VSync = 0\r\n"
    "\r\n"
    "[Controls]\r\n"
    "; 1 = keyboard and mouse play as player 1, alongside any controller.\r\n"
    "; Key bindings are in mkb.ini beside the game.\r\n"
    "KeyboardMouse = 1\r\n"
    "; Controller buttons in the game's prompts: Auto, Xbox, PlayStation or Switch.\r\n"
    "; Auto shows the buttons of a connected Sony or Nintendo controller, Xbox otherwise.\r\n"
    "; Choose PlayStation for a PS2 controller on an adapter, which can't be told apart.\r\n"
    "ButtonPrompts = Auto\r\n"
    "\r\n"
    "[Game]\r\n"
    "; 1 = start at the title screen, without the company logo movies.\r\n"
    "SkipIntros = 1\r\n"
    "; Text language: Auto, English, French, Spanish, German, Italian or Dutch.\r\n"
    "; Auto follows the Windows display language, and English when the game lacks it.\r\n"
    "Language = Auto\r\n"
    "; The console the game thinks it runs on: US or PAL. On a European (PAL) Xbox\r\n"
    "; the game takes its language from the console and switches everything it has\r\n"
    "; for it; on a US one it only knows English and Language is applied by the port.\r\n"
    "; PAL still runs at 60 Hz (the console's PAL60 setting).\r\n"
    "Console = US\r\n"
    "\r\n"
    "[Troubleshooting]\r\n"
    "; 1 = write spyro.log beside the game, for bug reports. It costs a little speed.\r\n"
    "Log = 0\r\n";

/* The keyboard and mouse layout, read by the runtime's input layer
 * (mkb_input.c). Glyph names in the comments are the game's own prompts. */
static const char DEFAULT_MKB[] =
    "; The Legend of Spyro: A New Beginning -- keyboard and mouse\r\n"
    "; Delete this file to get the defaults back.\r\n"
    ";\r\n"
    "; Each line gives an Xbox controller input up to four keys or mouse\r\n"
    "; buttons, separated by commas. Leave the right side empty to unbind.\r\n"
    "; Names: A-Z, 0-9, F1-F12 (F9 is taken: it releases the mouse), Space,\r\n"
    "; Enter, Esc, Tab, Backspace, CapsLock, Shift, LShift, RShift, Ctrl, LCtrl,\r\n"
    "; RCtrl, Alt, LAlt, RAlt, Up, Down, Left, Right, Insert, Delete, Home, End,\r\n"
    "; PageUp, PageDown, Num0-Num9, Mouse1 (left), Mouse2 (right), Mouse3\r\n"
    "; (middle), Mouse4, Mouse5, WheelUp, WheelDown.\r\n"
    "; The on-screen button prompts show the first key of each line.\r\n"
    "\r\n"
    "[Mouse]\r\n"
    "; The mouse turns the camera.\r\n"
    "sens = 1.0\r\n"
    "invert_y = 0\r\n"
    "\r\n"
    "[Movement]\r\n"
    "MoveForward = W\r\n"
    "MoveBack = S\r\n"
    "MoveLeft = A\r\n"
    "MoveRight = D\r\n"
    "; Camera from the keyboard, for playing without a mouse.\r\n"
    "CameraUp =\r\n"
    "CameraDown =\r\n"
    "CameraLeft =\r\n"
    "CameraRight =\r\n"
    "\r\n"
    "[Actions]\r\n"
    "; Jump, glide; confirm in menus.\r\n"
    "A = Space, Enter\r\n"
    "; Melee attack; back in menus.\r\n"
    "B = Mouse1, Backspace\r\n"
    "; Breath (hold).\r\n"
    "X = Mouse2\r\n"
    "; Ranged breath shot.\r\n"
    "Y = E, Mouse3\r\n"
    "; Charge, speed up.\r\n"
    "RightTrigger = Shift\r\n"
    "; Strafe, slow down.\r\n"
    "LeftTrigger = Ctrl\r\n"
    "; Fury.\r\n"
    "Black = F\r\n"
    "White = R\r\n"
    "LeftStickClick =\r\n"
    "RightStickClick = Mouse4\r\n"
    "\r\n"
    "[Menus and breaths]\r\n"
    "; Move in menus; select a breath element (1-4 work too).\r\n"
    "DpadUp = Up, 1\r\n"
    "DpadRight = Right, 2\r\n"
    "DpadDown = Down, 3\r\n"
    "DpadLeft = Left, 4\r\n"
    "Start = Esc\r\n"
    "Back = Tab\r\n";

static char s_path[MAX_PATH];
static char s_mkb_path[MAX_PATH];

static int beside_exe(char *out, size_t cap, const char *name)
{
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    char *slash;

    if (!n || n >= cap)
        return 0;
    slash = strrchr(out, '\\');
    if (!slash || (size_t)(slash + 1 - out) + strlen(name) + 1 > cap)
        return 0;
    strcpy(slash + 1, name);
    return 1;
}

static int ini_path(void)
{
    return s_path[0] || beside_exe(s_path, sizeof s_path, "spyro.ini");
}

static int mkb_path(void)
{
    return s_mkb_path[0] || beside_exe(s_mkb_path, sizeof s_mkb_path, "mkb.ini");
}

void settings_ensure_mkb(void)
{
    FILE *f;
    if (!mkb_path())
        return;
    f = fopen(s_mkb_path, "rb");
    if (f) {
        fclose(f);
        return;
    }
    f = fopen(s_mkb_path, "wb");
    if (f) {
        fwrite(DEFAULT_MKB, 1, sizeof DEFAULT_MKB - 1, f);
        fclose(f);
    }
}

static void trim(char **s)
{
    char *e;
    while (**s == ' ' || **s == '\t')
        (*s)++;
    e = *s + strlen(*s);
    while (e > *s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
}

static void apply(const char *k, const char *v)
{
    int n = atoi(v);

    if (!_stricmp(k, "Fullscreen"))
        g_settings.fullscreen = n != 0;
    else if (!_stricmp(k, "WindowWidth") && n >= 320 && n <= 16384)
        g_settings.window_width = n;
    else if (!_stricmp(k, "WindowHeight") && n >= 240 && n <= 16384)
        g_settings.window_height = n;
    else if (!_stricmp(k, "RenderScale") && n >= 0 && n <= 4)
        g_settings.render_scale = n;
    else if (!_stricmp(k, "Widescreen"))
        g_settings.widescreen = n != 0;
    else if (!_stricmp(k, "VSync"))
        g_settings.vsync = n != 0;
    else if (!_stricmp(k, "KeyboardMouse"))
        g_settings.keyboard_mouse = n != 0;
    else if (!_stricmp(k, "SkipIntros"))
        g_settings.skip_intros = n != 0;
    else if (!_stricmp(k, "Console"))
        g_settings.pal = !_stricmp(v, "PAL");
    else if (!_stricmp(k, "Log"))
        g_settings.log = n != 0;
    else if (!_stricmp(k, "ButtonPrompts")) {
        int i;
        for (i = 0; i < PROMPTS_COUNT; i++)
            if (!_stricmp(v, PROMPTS_NAMES[i]))
                g_settings.button_prompts = i;
    }
    else if (!_stricmp(k, "Language")) {
        int i;
        for (i = 0; i < LANGUAGE_COUNT; i++)
            if (!_stricmp(v, LANGUAGE_NAMES[i]))
                g_settings.language = i;
    }
}

void settings_load(void)
{
    FILE *f;
    char line[256];

    if (!ini_path())
        return;
    f = fopen(s_path, "rb");
    if (!f) {
        f = fopen(s_path, "wb");
        if (f) {
            fwrite(DEFAULT_FILE, 1, sizeof DEFAULT_FILE - 1, f);
            fclose(f);
        }
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *k = line, *v, *eq;
        trim(&k);
        if (!*k || *k == ';' || *k == '#' || *k == '[')
            continue;
        eq = strchr(k, '=');
        if (!eq)
            continue;
        *eq = 0;
        v = eq + 1;
        trim(&k);
        trim(&v);
        apply(k, v);
    }
    fclose(f);
}

/* Line by line, replacing the value of the key's line and keeping the rest
 * (comments, other keys, the reader's own edits) as it was. */
static void set_line(const char *path, const char *dflt, const char *key, const char *value)
{
    FILE *f;
    char *buf = NULL, *out, *p;
    long size = 0;
    size_t klen = strlen(key), used = 0, cap;
    int found = 0;

    f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = (char *)malloc((size_t)size + 1);
        if (!buf) {
            fclose(f);
            return;
        }
        size = (long)fread(buf, 1, (size_t)size, f);
        buf[size] = 0;
        fclose(f);
    } else {
        buf = _strdup(dflt);
        if (!buf)
            return;
        size = (long)strlen(buf);
    }
    cap = (size_t)size + klen + strlen(value) + 32;
    out = (char *)malloc(cap);
    if (!out) {
        free(buf);
        return;
    }
    for (p = buf; *p; ) {
        char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) + 1 : strlen(p);
        char *k = p;
        while (k < p + len && (*k == ' ' || *k == '\t'))
            k++;
        if (!found && !_strnicmp(k, key, klen)) {
            char *q = k + klen;
            while (*q == ' ' || *q == '\t')
                q++;
            if (*q == '=') {
                used += (size_t)sprintf(out + used, "%s = %s\r\n", key, value);
                found = 1;
                p += len;
                continue;
            }
        }
        memcpy(out + used, p, len);
        used += len;
        p += len;
    }
    if (!found) {
        if (used && out[used - 1] != '\n')
            used += (size_t)sprintf(out + used, "\r\n");
        used += (size_t)sprintf(out + used, "%s = %s\r\n", key, value);
    }
    f = fopen(path, "wb");
    if (f) {
        fwrite(out, 1, used, f);
        fclose(f);
    }
    free(out);
    free(buf);
}

void settings_set_int(const char *key, int value)
{
    char v[16];
    if (!ini_path())
        return;
    snprintf(v, sizeof v, "%d", value);
    set_line(s_path, DEFAULT_FILE, key, v);
}

void settings_set_str(const char *key, const char *value)
{
    if (ini_path())
        set_line(s_path, DEFAULT_FILE, key, value);
}

void settings_set_mkb(const char *key, const char *value)
{
    if (mkb_path())
        set_line(s_mkb_path, DEFAULT_MKB, key, value);
}

double settings_get_mkb(const char *key, double dflt)
{
    FILE *f;
    char line[256];
    size_t klen = strlen(key);

    if (!mkb_path() || !(f = fopen(s_mkb_path, "rb")))
        return dflt;
    while (fgets(line, sizeof line, f)) {
        char *k = line;
        while (*k == ' ' || *k == '\t')
            k++;
        if (!_strnicmp(k, key, klen)) {
            char *q = k + klen;
            while (*q == ' ' || *q == '\t')
                q++;
            if (*q == '=') {
                dflt = atof(q + 1);
                break;
            }
        }
    }
    fclose(f);
    return dflt;
}
