#ifndef TERMINAL_H
#define TERMINAL_H

#include "asset.h"
#include <stdint.h>

/* In-game computer terminal (script opcodes 8041 / 8042 / 8043, used in the
 * A001_6 / A001_7 / B022_4 database scenes). Text printed by 8043 accumulates
 * on a dark console drawn over the background area; the dialogue window keeps
 * working underneath. */

void terminal_init(void);
void terminal_set_mode(int style);     /* 8041: 1 = Kojiro frame (KOJ_TERM), 2 = Marina frame (MAR_TERM), 0 = hide */
const evb1_texture_t *terminal_background(void); /* frame texture to draw instead of the scene bg, NULL if none */
void terminal_clear(void);             /* 8042 */
void terminal_set_color(uint16_t argb4444); /* 8022: colour of the text printed from now on */
void terminal_set_caret(int visible);       /* 801f: blinking caret at the end of the current line */
void terminal_append(const char *s);   /* 8043: text with "\n" line breaks and "\q" = clear (literal backslashes) */
int  terminal_active(void);
void terminal_draw(void);              /* call inside the translucent list */

#endif /* TERMINAL_H */