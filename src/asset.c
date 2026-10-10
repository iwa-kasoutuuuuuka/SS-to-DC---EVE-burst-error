#include "asset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <dc/biosfont.h>

/* Dummy buffer state (no longer loading FONT.bin) */
static int font_loaded = 0;

/* Pre-allocated text rendering buffers (avoid per-frame malloc/free) */
static uint16_t *text_buffer = NULL;     /* 256x256 linear work buffer */
static uint8_t prev_text_hash[256] = {0}; /* Previous text for change detection */



int asset_load_font(const char *filename) {
    (void)filename; /* Suppress unused variable warning */

    /* Set Shift-JIS encoding for biosfont API */
    bfont_set_encoding(BFONT_CODE_SJIS);
    
    font_loaded = 1;

    /* Pre-allocate text rendering buffers */
    if (!text_buffer) {
        text_buffer = (uint16_t *)memalign(32, 512 * 256 * 2);
    }

    return 0;
}

void asset_free_font() {
    if (text_buffer) {
        free(text_buffer);
        text_buffer = NULL;
    }
    font_loaded = 0;
}

/* Standard 8/24/32-bit BMP to ARGB4444 Converter */
pvr_ptr_t asset_load_bmp(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) return NULL;

    uint8_t header[54];
    if (fread(header, 1, 54, f) != 54) {
        fclose(f);
        return NULL;
    }

    if (header[0] != 'B' || header[1] != 'M') {
        fclose(f);
        return NULL;
    }

    uint32_t offset = header[10] | (header[11] << 8) | (header[12] << 16) | (header[13] << 24);
    uint32_t width = header[18] | (header[19] << 8) | (header[20] << 16) | (header[21] << 24);
    uint32_t height = header[22] | (header[23] << 8) | (header[24] << 16) | (header[25] << 24);
    uint16_t bpp = header[28] | (header[29] << 8);

    if (bpp != 8 && bpp != 24 && bpp != 32) {
        printf("asset_load_bmp: Unsupported BPP %d in %s\n", bpp, filename);
        fclose(f);
        return NULL;
    }

    uint8_t palette[1024];
    if (bpp == 8) {
        if (fread(palette, 1, 1024, f) != 1024) {
            fclose(f);
            return NULL;
        }
    }

    uint32_t tex_size = TXR_WIDTH * TXR_HEIGHT * 2;
    pvr_ptr_t vram = render_alloc_txr(tex_size);
    if (!vram) {
        fclose(f);
        return NULL;
    }

    uint16_t *tex_buf = (uint16_t *)memalign(32, tex_size);
    if (!tex_buf) {
        render_free_txr(vram);
        fclose(f);
        return NULL;
    }
    memset(tex_buf, 0, tex_size);

    /* Seek to pixel data offset */
    fseek(f, offset, SEEK_SET);

    uint32_t row_stride = 0;
    if (bpp == 8) row_stride = (width + 3) & ~3;
    else if (bpp == 24) row_stride = (width * 3 + 3) & ~3;
    else if (bpp == 32) row_stride = width * 4;

    uint8_t *row_data = (uint8_t *)malloc(row_stride);
    if (!row_data) {
        free(tex_buf);
        render_free_txr(vram);
        fclose(f);
        return NULL;
    }

    uint32_t abs_height = abs((int)height);
    for (uint32_t y = 0; y < abs_height; y++) {
        if (fread(row_data, 1, row_stride, f) != row_stride) {
            break;
        }
        
        /* BMP is bottom-to-top by default unless height is negative */
        uint32_t out_y = ((int)height > 0) ? (abs_height - 1 - y) : y;
        if (out_y >= TXR_HEIGHT) continue;

        for (uint32_t x = 0; x < width && x < TXR_WIDTH; x++) {
            uint8_t r = 0, g = 0, b = 0, a = 15;
            if (bpp == 8) {
                uint8_t idx = row_data[x];
                b = palette[idx * 4 + 0] >> 4;
                g = palette[idx * 4 + 1] >> 4;
                r = palette[idx * 4 + 2] >> 4;
            } else if (bpp == 24) {
                b = row_data[x * 3 + 0] >> 4;
                g = row_data[x * 3 + 1] >> 4;
                r = row_data[x * 3 + 2] >> 4;
            } else if (bpp == 32) {
                b = row_data[x * 4 + 0] >> 4;
                g = row_data[x * 4 + 1] >> 4;
                r = row_data[x * 4 + 2] >> 4;
                a = row_data[x * 4 + 3] >> 4;
            }
            
            /* Pack into ARGB4444 format */
            tex_buf[out_y * TXR_WIDTH + x] = (a << 12) | (r << 8) | (g << 4) | b;
        }
    }

    render_load_txr(tex_buf, vram, tex_size);
    free(row_data);
    free(tex_buf);
    fclose(f);
    return vram;
}

pvr_ptr_t asset_load_bg(const char *filename) {
    /* Dispatch to BMP loader if needed */
    if (strstr(filename, ".bmp") || strstr(filename, ".BMP")) {
        return asset_load_bmp(filename);
    }

    FILE *f = fopen(filename, "rb");
    if (!f) return NULL;
    
    uint32_t img_size = 512 * 256 * 2;
    uint32_t alloc_size = 512 * 256 * 2;
    pvr_ptr_t vram = render_alloc_txr(alloc_size);
    if (!vram) { fclose(f); return NULL; }
    
    /* BIN files have no headers; PVR files have 16-byte headers we skip */
    fseek(f, 0, SEEK_END);
    uint32_t file_size = ftell(f);
    fseek(f, (file_size > img_size) ? (file_size - img_size) : 0, SEEK_SET);
    
    void *temp = memalign(32, alloc_size);
    if (!temp) { fclose(f); render_free_txr(vram); return NULL; }
    /* Note: no memset needed - fread overwrites the entire buffer */
    size_t bytes_read = fread(temp, 1, img_size, f);
#ifndef NDEBUG
    printf("asset_load_bg: Read %zu bytes from %s (expected %u)\n", bytes_read, filename, img_size);
#endif
    (void)bytes_read; /* suppress unused warning in release */
    fclose(f);
    
    /* Load linear texture into VRAM without twiddling */
    render_load_txr(temp, vram, img_size);
    free(temp);
    
    return vram;
}

uint8_t* asset_load_scenario(const char *filename, uint32_t *size_out) {
    FILE *f = fopen(filename, "rb");
    if (!f) return NULL;
    
    fseek(f, 0, SEEK_END);
    uint32_t size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    uint8_t *data = (uint8_t *)malloc(size);
    fread(data, 1, size, f);
    fclose(f);
    
    if (size_out) *size_out = size;
    return data;
}

/* Shift-JIS characters that must not start a line. */
static bool sjis_kinsoku_head(uint16_t c) {
    switch (c) {
        case 0x8141: case 0x8142: case 0x8143: case 0x8144: case 0x8145: /* 、。，．・ */
        case 0x8148: case 0x8149: case 0x815B: case 0x8163: case 0x8164: /* ？！ー‥… */
        case 0x8166: case 0x8168: case 0x816A: case 0x816C: case 0x816E: /* ’”）〕］ */
        case 0x8170: case 0x8172: case 0x8174: case 0x8176: case 0x8178: /* ｝〉》」』 */
        case 0x817A: case 0x8181:                                         /* 】 ＝ */
            return true;
        default:
            return false;
    }
}

static const uint8_t* align_sjis_string(const uint8_t *text) {
    if (!text || !text[0] || !text[1]) return text;
    
    uint8_t c1 = text[0];
    uint8_t c2 = text[1];
    
    /* If c1 is a lowercase alphabet (like 't' for text control command) 
       and c2 starts a valid Shift-JIS character, skip the command prefix! */
    if (c1 >= 'a' && c1 <= 'z' && ((c2 >= 0x81 && c2 <= 0x9F) || (c2 >= 0xE0 && c2 <= 0xFC))) {
        return text + 1;
    }
    
    /* Standard Shift-JIS 1-byte shift mojibake fix */
    int c1_is_sjis_start = ((c1 >= 0x81 && c1 <= 0x9F) || (c1 >= 0xE0 && c1 <= 0xFC));
    int c1_is_printable_ascii = (c1 >= 0x20 && c1 <= 0x7E);
    int c1_is_whitespace = (c1 == '\n' || c1 == '\r' || c1 == '\t');
    
    int c2_is_sjis_start = ((c2 >= 0x81 && c2 <= 0x9F) || (c2 >= 0xE0 && c2 <= 0xFC));
    
    if (!c1_is_sjis_start && !c1_is_printable_ascii && !c1_is_whitespace && c2_is_sjis_start) {
        return text + 1; /* Skip the garbage byte to restore alignment */
    }
    return text;
}

/* twiddle_256x256 removed: using pvr_txr_load_ex() for combined twiddle+SQ transfer */

/* Text colour (ARGB4444) used by asset_update_text(); 8022 changes it for the
 * free-positioned quiz text, the dialogue window stays white. */
static uint16_t text_fg = 0xFFFF;

/* ---- Saturn text font (SATFONT.BIN: "EVF1" u16 n u16 0, n x u16 code, n x 128 bytes) ---- */
static uint8_t  *satfont_data = NULL;
static uint16_t *satfont_codes = NULL;
static uint8_t  *satfont_glyphs = NULL;
static int       satfont_count = 0;

int asset_load_satfont(const char *path) {
#ifdef USE_BIOS_FONT
    (void)path; return -1;
#else
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 8) { fclose(f); return -1; }
    uint8_t *buf = (uint8_t *)memalign(32, (size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz || memcmp(buf, "EVF1", 4)) { fclose(f); free(buf); return -1; }
    fclose(f);
    int n = buf[4] | (buf[5] << 8);
    if (8 + n * 2 + n * 128 > sz) { free(buf); return -1; }
    satfont_data = buf; satfont_count = n;
    satfont_codes = (uint16_t *)(buf + 8);          /* little-endian u16, 4-byte aligned base */
    satfont_glyphs = buf + 8 + n * 2;
    printf("Saturn font: %d glyphs from %s\n", n, path);
    return 0;
#endif
}

static int satfont_index(uint16_t code) {
    int lo = 0, hi = satfont_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint16_t v = satfont_codes[mid];
        if (v == code) return mid;
        if (v < code) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

/* Draw glyph idx into a 24x24 cell (the BIOS-font cell size the window layout
 * uses) by nearest-neighbour upscaling the 16x16 glyph by 1.5; the 4-bit
 * intensity becomes the ARGB4444 alpha, the colour is text_fg. */
static void satfont_draw(uint16_t *dst, int stride, uint16_t fg, int idx) {
    const uint8_t *g = satfont_glyphs + idx * 128;
    for (int y = 0; y < 24; y++) {
        const uint8_t *row = g + (y * 2 / 3) * 8;
        uint16_t *out = dst + y * stride;
        for (int x = 0; x < 24; x++) {
            int sx = x * 2 / 3;
            uint8_t b = row[sx >> 1];
            uint16_t v = (sx & 1) ? (b & 15) : (b >> 4);
            if (v) out[x] = (uint16_t)((v << 12) | (fg & 0x0FFF));
        }
    }
}
void asset_set_text_color(uint16_t argb4444) { text_fg = argb4444; }

void asset_update_text(const uint8_t *orig_text, pvr_ptr_t vram_ptr) {
    if (!font_loaded || !vram_ptr || !text_buffer) return;
    
    /* Skip re-render if text hasn't changed */
    if (strcmp((const char *)prev_text_hash, (const char *)orig_text) == 0) return;
    strncpy((char *)prev_text_hash, (const char *)orig_text, sizeof(prev_text_hash) - 1);
    prev_text_hash[sizeof(prev_text_hash) - 1] = '\0';
    
#ifdef TEXT_DEBUG_DUMP
    printf("update_text_texture (raw): '%s'\n", orig_text);
#endif
    
    const uint8_t *p = orig_text;
    
    /* Skip 't' prefix if present */
    if (p[0] == 't') {
        p++;
    }
    
    /* Clear text buffer and reset cursor */
    memset(text_buffer, 0, 512 * 256 * 2);
    int text_cx = 20;
    int text_cy = 0;
    
    /* Draw string character by character onto text_buffer */
    while (*p) {
        uint16_t c = *p++;
        bool is_wide = false;
        bool is_kana = false;
        
        if (c == '\n') {
            text_cx = 20;
            text_cy += 24; /* Font height is 24 */
            continue;
        }
        else if (c == '\r') {
            continue;
        }
        else if (c == '\t') {
            text_cx += 12 * 4;
            if (text_cx >= 512 - 20 - 12) { text_cx = 20; text_cy += 24; }
            continue;
        }
        
        /* Shift-JIS Multi-byte character detection */
        if (((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) && *p) {
            c = (c << 8) | *p++;
            is_wide = true;
        }
        else if (c >= 0xA1 && c <= 0xDF) {
            is_kana = true;
        }
        
        int char_width = is_wide ? 24 : 12;
        
        /* Line wrapping after VM_WINDOW_COLS (19) full-width characters. A
         * character that must not begin a line (、。」！？‥ etc.) may hang one
         * position past the margin instead (行頭禁則). */
        int limit = 512 - 20 - 24;
        if (is_wide && sjis_kinsoku_head(c)) limit = 512 - 20;
        if (text_cx + char_width > limit) {
            text_cx = 20;
            text_cy += 24;
        }
        if (text_cy >= 256 - 24) {
            /* Buffer overflow: wrap around to top */
            text_cy = 0;
            memset(text_buffer, 0, 512 * 256 * 2);
        }
        
#ifdef TEXT_DEBUG_DUMP
        printf("draw char: c=%04X, cx=%d, cy=%d, wide=%d\n", c, text_cx, text_cy, is_wide);
#endif
        
        uint16_t *draw_ptr = text_buffer + text_cy * 512 + text_cx;
        int gi = (is_wide && satfont_count) ? satfont_index(c) : -1;
        if (gi >= 0) satfont_draw(draw_ptr, 512, text_fg, gi);
        else bfont_draw_ex(draw_ptr, 512, text_fg, 0x0000, 16, false, c, is_wide, is_kana);
        
        text_cx += char_width;
    }
    
#ifdef TEXT_DEBUG_DUMP
    /* Print ASCII art of the text buffer (32x64 grid). Costs ~2.5 KB of serial
     * output per line of dialogue, which throttles the whole game on Flycast. */
    printf("--- TEXT BUFFER ASCII ART ---\n");
    for (int y = 0; y < 32; y++) {
        char line[65];
        for (int x = 0; x < 64; x++) {
            int active = 0;
            for (int sy = 0; sy < 8; sy++) {
                for (int sx = 0; sx < 8; sx++) {
                    uint16_t val = text_buffer[(y * 8 + sy) * 512 + (x * 8 + sx)];
                    if (val != 0) {
                        active = 1;
                        break;
                    }
                }
                if (active) break;
            }
            line[x] = active ? '#' : '.';
        }
        line[64] = '\0';
        printf("%s\n", line);
    }
    printf("-----------------------------\n");
#endif
    
    /* Load linear texture into VRAM without twiddling */
    pvr_txr_load(text_buffer, vram_ptr, 512 * 256 * 2);
}

/* ---- EVB1 textures (backgrounds / sprites converted from Saturn .B1) ---- */

int asset_load_evb1(const char *filename, evb1_texture_t *out) {
    if (!out) return -1;
    out->txr = NULL; out->w = out->h = out->fmt = 0;

    FILE *f = fopen(filename, "rb");
    if (!f) return -1;

    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "EVB1", 4) != 0) {
        fclose(f);
        return -1;
    }
    int w   = hdr[4] | (hdr[5] << 8);
    int h   = hdr[6] | (hdr[7] << 8);
    int fmt = hdr[8] | (hdr[9] << 8);
    if (w <= 0 || h <= 0 || w > TXR_WIDTH || h > TXR_HEIGHT) {
        fclose(f);
        return -1;
    }

    uint32_t tex_size = TXR_WIDTH * TXR_HEIGHT * 2;
    uint16_t *buf = (uint16_t *)memalign(32, tex_size);
    if (!buf) { fclose(f); return -1; }
    memset(buf, 0, tex_size);
    for (int y = 0; y < h; y++) {
        if (fread(buf + y * TXR_WIDTH, 2, w, f) != (size_t)w) break;
    }
    fclose(f);

    pvr_ptr_t vram = render_alloc_txr(tex_size);
    if (!vram) { free(buf); return -1; }
    render_load_txr(buf, vram, tex_size);
    free(buf);

    out->txr = vram; out->w = w; out->h = h; out->fmt = fmt;
    return 0;
}

void asset_free_evb1(evb1_texture_t *t) {
    if (t && t->txr) {
        render_free_txr(t->txr);
        t->txr = NULL;
    }
    if (t) t->w = t->h = 0;
}
