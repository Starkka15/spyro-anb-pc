/*
 * spyro.ini, beside the .exe: the player's settings.
 *
 * Read once at startup, before the window and the input layer come up. A
 * missing file is written with the defaults and a comment per key, so there
 * is always one to edit. Environment variables of the same meaning
 * (RECOMP_GPU_SCALE, RECOMP_MKB) still win, for test runs.
 */
#ifndef SPYRO_SETTINGS_H
#define SPYRO_SETTINGS_H

typedef struct {
    int fullscreen;         /* borderless, on the window's monitor */
    int window_width;       /* client area while windowed */
    int window_height;
    int render_scale;       /* times the game's 640x480, 1-4; 0 = fit the monitor */
    int vsync;              /* wait for the display's refresh to present */
    int widescreen;         /* the game's own 16:9 mode */
    int keyboard_mouse;     /* keyboard and mouse act as player 1 */
    int skip_intros;        /* straight to the title screen, no logo movies */
    int log;                /* write spyro.log beside the game */
} SpyroSettings;

extern SpyroSettings g_settings;

void settings_load(void);
/* Writes one key back to the file, keeping everything else in it. */
void settings_set_int(const char *key, int value);

/* mkb.ini, the keyboard and mouse layout beside the .exe: written with the
 * game's layout when missing, and read by the runtime's input layer. */
void settings_ensure_mkb(void);
void settings_set_mkb(const char *key, const char *value);
double settings_get_mkb(const char *key, double dflt);

#endif
