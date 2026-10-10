#ifndef ASSET_H
#define ASSET_H

#include <stdint.h>
#include "renderer.h"

/* Load font data from binary blob */
int asset_load_font(const char *filename);
void asset_free_font();

/* Load background into VRAM */
pvr_ptr_t asset_load_bg(const char *filename);

/* Load scenario file into memory */
uint8_t* asset_load_scenario(const char *filename, uint32_t *size_out);

/* Font Rendering logic */
void asset_update_text(const uint8_t *text, pvr_ptr_t vram_ptr);
void asset_set_text_color(uint16_t argb4444);   /* colour for the next asset_update_text() (default 0xFFFF) */
/* Saturn text font (MISC/FONT.CMP via font_convert.py): 16x16 4-bit glyphs keyed by
 * Shift-JIS code. Loaded into RAM; full-width characters are drawn with it, the
 * BIOS font remains the fallback (and the only font with -DUSE_BIOS_FONT). */
int  asset_load_satfont(const char *path);      /* 0 = ok, -1 = not loaded (BIOS font used) */

/* EVB1 texture (written by tools/convert.py):
 *   "EVB1" u16 w u16 h u16 fmt(0=RGB565,1=ARGB4444) u16 reserved, then w*h*2 bytes.
 * The pixels are placed top-left in a 512x256 VRAM texture. */
typedef struct {
    pvr_ptr_t txr;   /* 512x256 texture in VRAM (NULL if not loaded) */
    int w, h;        /* valid pixel area inside the texture */
    int fmt;         /* 0 = RGB565, 1 = ARGB4444 */
} evb1_texture_t;

int  asset_load_evb1(const char *filename, evb1_texture_t *out);
void asset_free_evb1(evb1_texture_t *t);

#endif /* ASSET_H */
