#include "terminal.h"
#include "renderer.h"
#include <kos.h>
#include <dc/biosfont.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>

#define TERM_COLS   40      /* half-width columns: 40 x 12 px + TERM_LEFT fits in the 512 px texture */
#define TERM_ROWS   9       /* 24 px lines inside the 230 px text layer */
#define TERM_MAXLINES 64
#define TERM_LEFT   26      /* texture x where text starts (Saturn x=16 of the 801f caret; the frames are plain panels) */

static pvr_ptr_t term_vram = NULL;
static uint16_t *term_buf = NULL;   /* 512x256 ARGB4444 work buffer */
static int term_on = 0;
static char lines[TERM_MAXLINES][TERM_COLS * 2 + 1];
static uint16_t line_color[TERM_MAXLINES];  /* ARGB4444 colour of each line (\cN / 8022) */
static uint16_t cur_color = 0xFFF0;
/* ARGB4444 text colours for \c0..\c7 (0 = the default amber of the Saturn console) */
static const uint16_t term_palette[8] = { 0xFFF0, 0xFF55, 0xF5F5, 0xF5FF, 0xFFFF, 0xFF8F, 0xFF90, 0xF99F };
static int line_count = 0;
static int dirty = 0;
static int term_style = 0;                 /* 8041 value: 1 = KOJ_TERM, 2 = MAR_TERM */
static evb1_texture_t term_bg = {NULL, 0, 0, 0};
static int term_bg_style = 0;              /* style the loaded frame belongs to */
static size_t pend_head = 0, pend_tail = 0; /* typewriter queue (see terminal_append) */
static int caret_visible = 0, caret_on = 0, caret_timer = 0;   /* 801f caret, blinks 20/20 frames */
static int    type_wait = 0;

void terminal_init(void) {
    if (!term_vram) term_vram = render_alloc_txr(512 * 256 * 2);
    if (!term_buf) term_buf = (uint16_t *)memalign(32, 512 * 256 * 2);
    bfont_set_encoding(BFONT_CODE_SJIS);
    terminal_clear();
}

int terminal_active(void) {
    return term_on && term_vram && term_buf;
}

/* 8041 n: the Kojiro scenes (A001_6/7) use 1, the Marina scene (B022_4) uses 2; the
 * instruction is issued again after every 8043 block, so it also acts as "present". */
void terminal_set_mode(int style) {
    term_on = (style != 0);
#ifndef NDEBUG
    if (style != term_style) printf("TERMINAL: mode %d\n", style);
#endif
    term_style = style;
    if (!term_on) { terminal_clear(); pend_head = pend_tail = 0; type_wait = 0; return; }
    if (term_bg_style != style) {
        if (term_bg.txr) asset_free_evb1(&term_bg);
        const char *path = (style == 2) ? "/cd/graphics/misc/bg/MAR_TERM.BIN" : "/cd/graphics/misc/bg/KOJ_TERM.BIN";
        if (asset_load_evb1(path, &term_bg) != 0) term_bg.txr = NULL;   /* 0 = success */
        term_bg_style = style;
    }
}

const evb1_texture_t *terminal_background(void) {
    return (terminal_active() && term_bg.txr) ? &term_bg : NULL;
}

void terminal_clear(void) {
    line_count = 0;
    lines[0][0] = '\0';
    cur_color = term_palette[0];
    line_color[0] = cur_color;
    dirty = 1;
}

void terminal_set_caret(int visible) {
    if (caret_visible != visible) dirty = 1;
    caret_visible = visible; caret_on = 1; caret_timer = 0;
}

void terminal_set_color(uint16_t argb4444) {
    cur_color = argb4444;
    if (lines[line_count][0] == '\0') line_color[line_count] = cur_color;
}

static void new_line(void) {
    if (line_count < TERM_MAXLINES - 1) {
        line_count++;
    } else {
        memmove(lines[0], lines[1], sizeof(lines) - sizeof(lines[0]));
        memmove(line_color, line_color + 1, sizeof(line_color) - sizeof(line_color[0]));
    }
    lines[line_count][0] = '\0';
    line_color[line_count] = cur_color;
}

/* \b: delete the last character of the current line (2 bytes for a Shift-JIS pair). */
static void backspace(void) {
    char *l = lines[line_count];
    size_t len = strlen(l);
    if (len == 0) return;
    size_t i = 0, last = 0;
    while (i < len) {
        unsigned char c = (unsigned char)l[i];
        last = i;
        i += (((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) && i + 1 < len) ? 2 : 1;
    }
    l[last] = '\0';
}

/* 8043 text is queued and revealed a few characters per frame (the Saturn
 * terminal types its output); "\wN" inserts a pause of N units. Control codes:
 * "\n" (also a real newline) new line, "\q" clear, "\cN" colour, "\b" backspace,
 * "\u" cursor up. The queue is a byte ring; set_mode(0) discards it. */
#define TERM_QUEUE 8192
#define TERM_CHARS_PER_FRAME 3
#define TERM_WAIT_UNIT 6            /* frames per "\w" unit (guess) */
static char   pend[TERM_QUEUE];

static size_t pend_len(void) { return (pend_tail - pend_head) & (TERM_QUEUE - 1); }
static char   pend_at(size_t i) { return pend[(pend_head + i) & (TERM_QUEUE - 1)]; }
static void   pend_skip(size_t n) { pend_head = (pend_head + n) & (TERM_QUEUE - 1); }

static void put_char(unsigned char c, char c2, int wide) {
    size_t len = strlen(lines[line_count]);
    if (len + (wide ? 2 : 1) >= sizeof(lines[0]) - 1) {
        new_line();
        len = 0;
    }
    lines[line_count][len++] = (char)c;
    if (wide) lines[line_count][len++] = c2;
    lines[line_count][len] = '\0';
    dirty = 1;
}

/* Reveal up to `budget` characters from the queue; returns when the budget is
 * spent, the queue is empty, or a "\w" pause starts. */
static void consume(int budget) {
    while (budget > 0 && pend_len() > 0) {
        unsigned char c = (unsigned char)pend_at(0);
        char n1 = pend_len() > 1 ? pend_at(1) : 0;
        if (c == '\n' || (c == '\\' && n1 == 'n')) { new_line(); dirty = 1; pend_skip(c == '\\' ? 2 : 1); continue; }
        if (c == '\\' && n1 == 'q') { terminal_clear(); pend_skip(2); continue; }
        if (c == '\\' && n1 == 'c') {
            char d = pend_len() > 2 ? pend_at(2) : 0;
            if (d >= '0' && d <= '9') { cur_color = term_palette[(d - '0') & 7]; line_color[line_count] = cur_color; pend_skip(3); continue; }
        }
        if (c == '\\' && n1 == 'w') {
            size_t i = 2; int n = 0;
            while (i < pend_len() && pend_at(i) >= '0' && pend_at(i) <= '9') { n = n * 10 + (pend_at(i) - '0'); i++; }
            pend_skip(i);
            type_wait = n * TERM_WAIT_UNIT;
            return;
        }
        if (c == '\\' && n1 == 'b') { backspace(); dirty = 1; pend_skip(2); continue; }
        if (c == '\\' && n1 == 'u') { if (line_count > 0) line_count--; pend_skip(2); continue; }
        int wide = (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
        if (wide && pend_len() < 2) return;     /* second byte not queued yet */
        put_char(c, wide ? n1 : 0, wide);
        pend_skip(wide ? 2 : 1);
        budget--;
    }
}

void terminal_append(const char *s) {
    if (!s) return;
    if (line_count == 0 && lines[0][0] == '\0') dirty = 1;
    size_t n = strlen(s);
    if (n >= TERM_QUEUE - 1 - pend_len()) {       /* no room: flush what is pending */
        type_wait = 0;
        consume(TERM_QUEUE);
        if (n >= TERM_QUEUE - 1) n = TERM_QUEUE - 1;
    }
    for (size_t i = 0; i < n; i++) {
        pend[pend_tail] = s[i];
        pend_tail = (pend_tail + 1) & (TERM_QUEUE - 1);
    }
#ifndef NDEBUG
    printf("TERMINAL: queued %u bytes (%u pending)\n", (unsigned)n, (unsigned)pend_len());
#endif
}

/* Per-frame typing step; called from terminal_draw(). */
static void terminal_update(void) {
    if (type_wait > 0) { type_wait--; return; }
    consume(TERM_CHARS_PER_FRAME);
}

static void render_lines(void) {
    memset(term_buf, 0, 512 * 256 * 2);
    int first = (line_count + 1 > TERM_ROWS) ? line_count + 1 - TERM_ROWS : 0;
    int cy = 4;
    for (int li = first; li <= line_count; li++) {
        const uint8_t *p = (const uint8_t *)lines[li];
        int cx = TERM_LEFT;
        while (*p) {
            uint16_t ch = *p++;
            bool wide = false, kana = false;
            if (((ch >= 0x81 && ch <= 0x9F) || (ch >= 0xE0 && ch <= 0xFC)) && *p) {
                ch = (ch << 8) | *p++;
                wide = true;
            } else if (ch >= 0xA1 && ch <= 0xDF) {
                kana = true;
            }
            int cw = wide ? 24 : 12;
            if (cx + cw > 512 - 8) break;
            bfont_draw_ex(term_buf + cy * 512 + cx, 512, line_color[li], 0x0000, 16, false, ch, wide, kana);
            cx += cw;
        }
        if (li == line_count && caret_visible && caret_on) {
            /* caret: a filled cell right after the last character of the current line */
            int cw = 12, ch = 16;
            if (cx + cw <= 512 - 8 && cy + ch <= 256) {
                for (int yy = 0; yy < ch; yy++)
                    for (int xx = 0; xx < cw; xx++)
                        term_buf[(cy + yy) * 512 + cx + xx] = line_color[li];
            }
        }
        cy += 24;
    }
    pvr_txr_load(term_buf, term_vram, 512 * 256 * 2);
    dirty = 0;
}

void terminal_draw(void) {
    if (!terminal_active()) return;
    terminal_update();
    if (caret_visible && ++caret_timer >= 20) { caret_timer = 0; caret_on = !caret_on; dirty = 1; }
    if (dirty) render_lines();
    /* Dark console over the Saturn background area (320x160 -> 640x343) when the
     * KOJ_TERM / MAR_TERM frame is missing (otherwise main.c draws the frame). */
    if (!term_bg.txr) draw_message_box(0, 0, 640, 160 * SAT_SCALE_Y, 0xE8000000);
    draw_font_layer(term_vram, 64, 24, 4.5f);
}