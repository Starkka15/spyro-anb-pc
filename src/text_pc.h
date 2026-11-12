/* PC wording for the lines that talk about the Xbox console. See text_pc.c. */
#ifndef TEXT_PC_H
#define TEXT_PC_H

#include <wchar.h>

/* Languages, in the order of the game's translations.<Language>.txt files. */
enum { TEXT_LANG_EN, TEXT_LANG_FR, TEXT_LANG_ES, TEXT_LANG_DE, TEXT_LANG_IT, TEXT_LANG_NL, TEXT_LANGS };

/* Makes <lang_dir>\data_xb.rkv.ovl (a read overlay, kernel_file.c) give the
 * language's text the PC lines. It starts from the text the overlay already
 * holds (the PS2 import's merge), else the game's own, and rewrites the file
 * only when something changes. `game_dir` holds data_xb.rkv. Returns 1 when
 * the overlay is up to date, 0 on an error (nothing is left half written). */
int text_pc_refresh(const wchar_t *game_dir, const wchar_t *lang_dir, int lang);

#endif
