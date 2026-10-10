#ifndef VM_H
#define VM_H

#include <stdint.h>

#define VM_MAX_CHOICES     12   /* a command menu plus its save/load menu */
#define VM_MENU_STACK      8
#define VM_CHOICE_TEXT_LEN 64
#define VM_NUM_VARS        4096
#define VM_NAME_LEN        16
#define VM_WINDOW_LEN      512  /* bytes of Shift-JIS text the message window can hold */
#define VM_WINDOW_COLS     19   /* full-width characters per row before wrapping (asset.c) */
#define VM_WINDOW_ROWS     4    /* rows in the 560x120 message box at 24 px per row */
#define VM_LOG_ENTRIES     24   /* backlog: last dialogue lines */
#define VM_LOG_LEN         128  /* bytes kept per backlog line */

/* vm_step() return values */
#define VM_END     -1   /* script finished / ran off the end          */
#define VM_RUNNING  0   /* nothing to show yet, call again next frame  */
#define VM_WAIT     1   /* text or menu displayed, waiting for input   */
#define VM_CHAIN    2   /* 8020: load ctx->pending_script and continue */
#define VM_SAVE     3   /* 8028: the caller opens the save slot menu     */
#define VM_LOAD     4   /* 801e: the caller opens the load slot menu     */
#define VM_DISC     5   /* 803a: ctx->disc_req must be inserted (multi-disc) */

/* VM State */
typedef struct {
    uint8_t *script_data;
    uint32_t script_size;
    uint32_t ip;           /* Instruction Pointer (byte offset, always even) */
    int is_waiting;        /* Waiting for user input */
    int is_choosing;       /* In a choice menu */
    int select_idx;        /* Currently selected choice index */
    int choice_count;      /* Actual number of choices in menu */
    uint32_t choice_ips[VM_MAX_CHOICES];                      /* Branch targets (byte ip) */
    uint8_t  choice_text[VM_MAX_CHOICES][VM_CHOICE_TEXT_LEN]; /* Shift-JIS labels */
    int wait_timer;        /* Auto-play timer for debug */
    int delay_frames;      /* OP_WAIT countdown (script paused, no input needed) */
    int trans_speed;       /* last OP_TRANS value (3..6); fade length = speed * 4 frames */
    uint32_t last_text_addr; /* Address of last displayed dialogue */
    uint16_t vars[VM_NUM_VARS];     /* Game flags set by 8007 and tested by 8002/8003 */
    char pending_script[VM_NAME_LEN]; /* Script name from 8020 (lower case, e.g. "a001_2") */
    char bg_name[VM_NAME_LEN];        /* Last background name from 8014 (e.g. "01______") */
    char next_voice[VM_NAME_LEN];     /* Second voice token of the current line (8035 flag=2) */
    int  voice_wait;                  /* Frames since the current line was shown */
    int  route;                       /* 802a: 1 = Kojiro (A*), 2 = Marina (B*), 3 = merged (AB01/C*) */
    int  disc_req;                    /* 803a: disc required from here on (1..3) */
    int  key_lock;                    /* frames during which A is ignored ("w<N>k" lines) */
    /* Menu return stack: choosing an item pushes the menu it came from; 8006 at the
     * end of the item pops it and re-executes that menu (one-item confirmation
     * menus are skipped so control returns to the real command menu). */
    uint32_t menu_group_ip;           /* ip of the 8002 group currently displayed */
    int      menu_armed;              /* menu_group_ip belongs to the open choice */
    uint32_t menu_stack_ip[VM_MENU_STACK];
    uint8_t  menu_stack_items[VM_MENU_STACK];
    int      menu_sp;
    uint8_t window[VM_WINDOW_LEN];    /* text currently shown in the message window */
    int  window_rows;                 /* physical rows used by window (VM_WINDOW_COLS per row) */
    int  text_append;                 /* last line ended with 'n': the next one is added below */
    uint8_t log[VM_LOG_ENTRIES][VM_LOG_LEN]; /* backlog ring, newest at (log_head - 1) */
    int  log_head;                    /* next slot to write */
    int  log_count;                   /* valid entries (<= VM_LOG_ENTRIES) */
} vm_context_t;

/* Scenario opcodes: every instruction is a 16-bit big-endian word 0x80xx
 * followed by 16-bit operands. Text/name operands are WORD addresses
 * (byte offset = operand * 2) into the same .SCO file. */
#define OP_TEXT    0x35  /* addr, n, voice_addr x n (n = 0..2)          */
#define OP_MENU    0x02  /* N, N x { k, (var,val) x k, text, target }   */
#define OP_BRANCH  0x03  /* k, (var,val) x k, target : jump if all eq   */
#define OP_GOTO    0x05  /* target                                      */
#define OP_SETVAR  0x07  /* var, val                                    */
#define OP_BG      0x14  /* name_addr : background image                */
#define OP_SCRIPT  0x20  /* name_addr : load another .SCO               */
#define OP_SE_A    0x39  /* name_addr : sound effect                    */
#define OP_SE_B    0x0B  /* name_addr : sound effect                    */
#define OP_ROUTE   0x2A  /* n : current route, see vm_context_t.route    */
#define OP_SAVE    0x28  /* save game (menu item)                       */
#define OP_LOAD    0x1E  /* load game (menu item / title)               */
#define OP_MENU_END 0x06 /* end of a menu item: back to its menu        */
#define OP_DISC    0x3A  /* n : disc that holds the following voices    */
#define OP_SPRITE  0x36  /* name_addr, x, y : overlay image at Saturn coords */
#define OP_WAIT    0x2F  /* frames : pause script for N frames          */
#define OP_FADEOUT 0x12  /* x : 1 = fade to black before a scene change, 0 = cut */
#define OP_FADEIN  0x13  /* x : 1 = fade in the new scene, 0 = show at once      */
#define OP_TRANS   0x25  /* n : transition speed hint (3..6) stored for OP_FADEIN */
#define OP_POSTWAIT 0x26 /* n : BGM play: start track n (Windows eve_NN.mid); same track = keep playing */
#define OP_MOVIE   0x08  /* name_addr : play MOVIES/<name>.CPK (-> /cd/movies/<NAME>.mpg) */
#define OP_TERM_MODE 0x41 /* x : show the terminal with frame x (1 = KOJ_TERM, 2 = MAR_TERM) */
#define OP_TERM_CLR  0x42 /* clear the terminal text                              */
#define OP_TERM_TEXT 0x43 /* str_addr : print text on the terminal ("\n" breaks)  */
#define OP_BGM_A   0x0E  /* n : 0 = silence the music (before a movie), else restore  */
#define OP_BGM_B   0x0F  /* n : fade the music out (n = speed 0..12, 0 = stop at once) */
#define OP_TERM_CUR 0x1F /* y x y2 x2 : terminal/quiz caret at Saturn pixel (x,y); (14,320) = hidden */
#define OP_PALETTE 0x33  /* slot r g b : VDP2 palette entry (5-bit); 12..14 animate night lights / fireworks */
#define OP_TEXTPOS 0x40  /* x y : draw the following TEXTs at Saturn pixel (x,y) instead of the window */
#define OP_TEXTCOL 0x22  /* slot 0 : text colour palette slot (15 = white)      */

/* Callbacks implemented by the host (main.c) */
void vm_on_background(const char *name);                  /* OP_BG   */
void vm_on_sprite(const char *name, int x, int y);        /* OP_SPRITE */
void vm_on_fade(int to_black, int frames);                /* OP_FADEOUT / OP_FADEIN: 0 frames = instant */
void vm_on_movie(const char *name);                       /* OP_MOVIE (blocking playback) */
void vm_on_terminal(int opcode, int value, const char *text); /* OP_TERM_* */
void vm_on_bgm(int opcode, int value);                    /* OP_POSTWAIT (play) / OP_BGM_A / OP_BGM_B */
void vm_on_palette(int slot, int r, int g, int b);        /* OP_PALETTE */
void vm_on_text_pos(int x, int y);                        /* OP_TEXTPOS */
void vm_on_text_color(int slot);                          /* OP_TEXTCOL */
void vm_on_terminal_cursor(int y, int x);                 /* OP_TERM_CUR: caret at Saturn (x,y); x >= 320 hides it */

/* Initialize VM (clears variables) and load the first script */
void vm_init(vm_context_t *ctx, uint8_t *data, uint32_t size);
/* Replace the running script, keeping variables (used for OP_SCRIPT) */
void vm_load_script(vm_context_t *ctx, uint8_t *data, uint32_t size);
void vm_cleanup(vm_context_t *ctx);

/* Main Execution Loop */
int vm_step(vm_context_t *ctx);

/* Input Handling for VM */
void vm_handle_input(vm_context_t *ctx, uint32_t buttons);

/* Choice menu API (also usable from outside the script, e.g. debug menus).
 * labels are Shift-JIS strings; targets are the byte IP to jump to per item.
 * The menu is drawn through update_text_texture() with a cursor marker. */
void vm_begin_choice(vm_context_t *ctx, const uint8_t *const *labels,
                     const uint32_t *targets, int count);
int  vm_is_choosing(const vm_context_t *ctx);

/* Redraw the current menu or the last dialogue line (after a route switch). */
void vm_redraw(vm_context_t *ctx);
/* Backlog: entry 0 is the newest line, up to vm_log_count(ctx) - 1 the oldest. */
int  vm_log_count(const vm_context_t *ctx);
const uint8_t *vm_log_get(const vm_context_t *ctx, int age);
/* Byte ip of the first "8001 id" block header, 0 if absent (A01: 100 = title). */
uint32_t vm_find_block(const vm_context_t *ctx, uint32_t id);

#endif /* VM_H */