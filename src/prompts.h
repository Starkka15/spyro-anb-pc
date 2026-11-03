/* Keyboard and mouse button prompts: see prompts.c. */
#ifndef SPYRO_PROMPTS_H
#define SPYRO_PROMPTS_H

#include <stdint.h>

/* The decoded pixel hash of the game's controller glyph sheet
 * (font_buttons0.tex, 256x128). */
#define PROMPTS_SHEET_HASH 0x1A6DF870BDE5674Eull

/* The keyboard sheet, (256 * scale) x (128 * scale) BGRA, built from the
 * decoded controller sheet `orig`. The caller frees it. */
uint32_t *prompts_build(const uint32_t *orig, uint32_t scale);

#endif
