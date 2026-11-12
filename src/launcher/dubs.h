/* Dubbed cutscene audio from a European PS2 disc: see dubs.c. */
#ifndef SPYRO_DUBS_H
#define SPYRO_DUBS_H

#include <windows.h>

enum { DUBS_OK = 0, DUBS_FAILED = 1, DUBS_CANCELLED = 3 };

/* Reads the European PS2 disc at `src` (a drive, folder, ISO image or
 * MEDIA_P2.RKV) and writes the dubbed audio for the movies in `game`\video
 * into `out`\<LANG>, with a progress window. Errors are shown to the player;
 * `quiet` leaves out the "installed" message. Returns a DUBS_ code. */
int dubs_import(HWND parent, const wchar_t *src, const wchar_t *game, const wchar_t *out, int quiet);

/* Whether `out` holds the full set. */
int dubs_installed(const wchar_t *out);

#endif
