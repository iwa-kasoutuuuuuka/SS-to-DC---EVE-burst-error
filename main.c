#include <kos.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include "renderer.h"
#include "vm.h"
#include "asset.h"
#include "input.h"
#include "video.h"
#include "audio.h"
#include "terminal.h"
#include "bgm.h"
#include "save.h"

/* Build-time switches (pass via EXTRA_CFLAGS, e.g. make EXTRA_CFLAGS=-DDEMO_AUTO_ADVANCE=1)
 *   DEMO_AUTO_ADVANCE : N>0 presses A automatically every N frames (3N frames inside a menu,
 *                       always picking the first entry). Unattended demo / emulator tests only.
 *   DEMO_RANDOM       : seed>0 makes the auto advance pick a pseudo-random menu entry instead
 *                       (walks the cursor down to it first). Needs DEMO_AUTO_ADVANCE.
 *   DEMO_SWEEP        : N>0 = smoke test: every N frames restart with the next .SCO of
 *                       /cd/scenario/Disc<EVE_DISC> (all 90 in name order), log "SWEEP: start".
 *   DEMO_EXPLORE      : 1 = at each menu prefer an entry not chosen there before and never
 *                       pick セーブ / ロード / スロット entries (coverage runs). Needs DEMO_RANDOM
 *                       as the fallback once every entry of a menu has been tried.
 *   DEMO_SWITCH       : N>0 swaps between the Kojiro and Marina routes every N frames
 *                       (same as pressing Start). Unattended tests of the route switch.
 *   START_BLOCK       : block id ("8001 id") to start at inside START_SCRIPT_NAME; 100 is the
 *                       title menu of A01. 0 = start at ip 0.
 *   DEMO_KEYS         : N>0 injects Start / X / Y in turn every N frames (system menu,
 *                       backlog and auto toggle get exercised by unattended tests).
 *   NO_INTRO          : skip the logo and opening movies at boot.
 *
 * Controls: A advance / confirm, B cancel (system menus) or hold to
 * fast-forward dialogue, Start system menu, X backlog, Y auto mode on/off,
 * D-pad up/down move the cursor.
 *   EVE_DISC          : which /cd/audio/DiscN and /cd/scenario/DiscN directories are used
 *   MULTI_DISC        : honour 803a disc requests: ask for the disc, wait for /cd/DISC<n>.ID
 *                       (set by `make cdi DISC=n`; a DISC=0 build ignores 803a)
 *   START_SCRIPT_NAME : scenario to start from, e.g. -DSTART_SCRIPT_NAME=A001_6 (no quotes,
 *                       the kos-cc wrapper cannot pass them). Default A001. */
#ifndef DEMO_AUTO_ADVANCE
#define DEMO_AUTO_ADVANCE 0
#endif
#ifndef DEMO_RANDOM
#define DEMO_RANDOM 0
#endif
#ifndef DEMO_SWITCH
#define DEMO_SWITCH 0
#endif
#ifndef DEMO_KEYS
#define DEMO_KEYS 0
#endif
#ifndef DEMO_EXPLORE
#define DEMO_EXPLORE 0
#endif
#ifndef DEMO_SWEEP
#define DEMO_SWEEP 0
#endif
#ifndef EVE_DISC
#define EVE_DISC 1
#endif
#ifndef START_SCRIPT_NAME
#define START_SCRIPT_NAME A01      /* title screen: load / new game / character select */
#define START_BLOCK_DEFAULT 100
#else
#define START_BLOCK_DEFAULT 0
#define START_SCRIPT_NAME_GIVEN 1  /* test entry point: no logo / opening movies */
#endif
#ifndef START_BLOCK
#define START_BLOCK START_BLOCK_DEFAULT
#endif
#define EVE_STR_(x) #x
#define EVE_STR(x)  EVE_STR_(x)
/* Scenario files live on the CD (scenario/DiscN/, all 90 per disc, written by
 * tools/convert.py). The romdisk is an optional fallback: a .SCO placed in
 * romdisk/ is used when the CD does not have it. */
#define SCENARIO_DIR "/cd/scenario/Disc" EVE_STR(EVE_DISC)

/* Global pointers for VM callbacks */
static pvr_ptr_t font_vram = NULL;
static pvr_ptr_t msgbox_vram = NULL;
static pvr_ptr_t bg_vram = NULL;          /* fallback full-screen background */
static vm_context_t vm_ctx;
static int game_state = 0;               /* 0: playing, 1: ending screen, 2: waiting for a disc */
#ifdef MULTI_DISC
static int cur_disc = EVE_DISC;          /* disc currently in the drive (audio/Disc<n>) */
static int want_disc = 0;                /* disc requested by 803a while game_state == 2 */
#endif
static int game_state_ending(void) { return game_state != 0; }

/* Route switching: the Kojiro (A*) and Marina (B*) scenarios are two
 * independent script positions sharing the game flags. vm_ctx runs one of them,
 * other_ctx parks the other; Start (or DEMO_SWITCH) swaps them. */
static vm_context_t other_ctx;
static int  other_started = 0;
static char cur_script[VM_NAME_LEN] = "";
static char other_script[VM_NAME_LEN] = "";

#if DEMO_EXPLORE
/* Chosen-entry bitmask per menu (keyed by script name hash + menu ip). */
#define EXPLORE_SLOTS 512
static struct { uint32_t key; uint32_t chosen; } explore_tab[EXPLORE_SLOTS];
static int demo_skip_label(const uint8_t *label) {
    /* skip "セーブ" / "ロード" / "スロット" so the run does not save or load */
    static const char *bad[] = { "\x83\x5a\x81\x5b\x83\x75", "\x83\x8d\x81\x5b\x83\x68", "\x83\x58\x83\x8d\x83\x62\x83\x67", NULL };
    for (int i = 0; bad[i]; i++) if (strstr((const char *)label, bad[i])) return 1;
    return 0;
}
static int demo_explore_pick(void) {
    uint32_t h = 5381;
    for (const char *p = cur_script; *p; p++) h = h * 33u + (uint8_t)*p;
    uint32_t key = h ^ (vm_ctx.menu_group_ip * 2654435761u) ^ 0x9e3779b9u;
    if (key == 0) key = 1;
    int slot = (int)(key % EXPLORE_SLOTS), found = -1;
    for (int i = 0; i < EXPLORE_SLOTS; i++) {
        int j = (slot + i) % EXPLORE_SLOTS;
        if (explore_tab[j].key == key) { found = j; break; }
        if (explore_tab[j].key == 0) { explore_tab[j].key = key; explore_tab[j].chosen = 0; found = j; break; }
    }
    uint32_t chosen = (found >= 0) ? explore_tab[found].chosen : 0;
    int pick = -1;
    for (int e = 0; e < vm_ctx.choice_count; e++) {
        if (demo_skip_label(vm_ctx.choice_text[e])) continue;
        if (!(chosen & (1u << e))) { pick = e; break; }
    }
    if (pick < 0) {                       /* everything tried: any non-save entry */
        for (int e = 0; e < vm_ctx.choice_count; e++) if (!demo_skip_label(vm_ctx.choice_text[e])) { pick = e; break; }
        if (pick < 0) pick = 0;
        if (found >= 0) explore_tab[found].chosen = 0;   /* start over for this menu */
    }
    if (found >= 0) explore_tab[found].chosen |= 1u << pick;
    return pick;
}
#endif

/* Scene graphics driven by the script (OP_BG / OP_SPRITE) */
static evb1_texture_t scene_bg = {0};
static evb1_texture_t scene_spr = {0};
static int scene_spr_x = 0, scene_spr_y = 0;

/* Screen fade (OP_FADEOUT / OP_FADEIN): fade_level 0..255 moves towards fade_target */
static int fade_level = 0, fade_target = 0, fade_step = 255;

void vm_on_fade(int to_black, int frames) {
    fade_target = to_black ? 255 : 0;
    fade_step = (frames > 0) ? (255 + frames - 1) / frames : 255;
}

static void upper_name(const char *src, char *dst, int cap);

/* OP_MOVIE: /cd/movies/<NAME>.mpg (ffmpeg conversion of MOVIES/<name>.CPK).
 * Blocking; A/Start skips. Names like "m1718m" that have no file fall back to
 * the name without the trailing letter. */
void vm_on_movie(const char *name) {
#ifdef NO_VIDEO
    printf("Video: skipped %s (NO_VIDEO)\n", name);
    return;
#endif
    char upper[VM_NAME_LEN], path[64];
    upper_name(name, upper, sizeof(upper));
    snprintf(path, sizeof(path), "/cd/movies/%s.mpg", upper);
    audio_stop_voice();
    bgm_stop();   /* libmpeg owns the sound hardware while the movie plays */
    if (video_play(path) < 0) {
        size_t n = strlen(upper);
        if (n > 1 && upper[n - 1] >= 'A' && upper[n - 1] <= 'Z') {
            upper[n - 1] = '\0';
            snprintf(path, sizeof(path), "/cd/movies/%s.mpg", upper);
            video_play(path);
        }
    }
    fade_level = 0; fade_target = 0;   /* the movie replaced the faded-out scene */
}

/* OP_TERM_*: the in-game computer console (src/terminal.c). */
void vm_on_terminal(int opcode, int value, const char *text) {
    if (opcode == OP_TERM_MODE) terminal_set_mode(value);   /* 1 = Kojiro frame, 2 = Marina frame */
    else if (opcode == OP_TERM_CLR) terminal_clear();
    else if (opcode == OP_TERM_TEXT && text) {
        if (!terminal_active()) terminal_set_mode(1);
        terminal_append(text);
    }
}

/* BGM (src/bgm.c, tracks are /cd/bgm/BGM<n>.wav rendered from the Windows MIDI):
 *   8026 n  play track n (the Windows build replaces "8026 52" with "800b eve_52",
 *           so n is the eve_NN number; 0 = stop). Re-issuing the running track is a no-op.
 *   800f n  fade out and stop; n = speed (1 before returning to a menu, 4/8 at
 *           scene changes, 12 rarely; 0 = cut).
 *   800e n  0 = silence (issued right before a movie), non-zero = restore. */
void vm_on_bgm(int opcode, int value) {
#ifndef NDEBUG
    printf("BGM: op %02x value %d\n", opcode, value);
#endif
    if (opcode == OP_POSTWAIT) {
        if (value > 0) bgm_play(value); else bgm_stop();
    } else if (opcode == OP_BGM_B) {
        bgm_fade_out(value * 15);
    } else if (opcode == OP_BGM_A) {
        bgm_set_mute(value == 0);
    }
}

#if 0   /* previous provisional mapping (800e value -> BGM1..4), kept for reference */
void vm_on_bgm_old(int opcode, int value) {
    if (opcode == OP_BGM_A) {
        /* Saturn values are 1/2/4/8 (title plays 1). The Saturn bank GAME.BIN holds
         * four sequences (GAMEA..D), so these look like the four songs of a route;
         * map them onto the Windows-version tracks BGM1..4 (provisional, see
         * eve-dc-asset-pipeline). Anything else is used as a track number. */
        int track;
        switch (value) {
            case 0: track = 0; break;
            case 1: track = 1; break;
            case 2: track = 2; break;
            case 4: track = 3; break;
            case 8: track = 4; break;
            default: track = value; break;
        }
        if (track > 0) bgm_play(track); else bgm_stop();
    } else {
        bgm_stop();
    }
}
#endif

/* OP_PALETTE: the Saturn keeps a few VDP2 palette slots for animated lights.
 * Our backgrounds are direct colour, so the animation is approximated by a
 * white flash over the background area whose strength follows how far the
 * brightest animated slot (14) rises above its night-scene default (6,7,10).
 * Slots 1/8/15 are set by text-only screens (black/grey/white) and ignored. */
static int palette_flash = 0;   /* 0..255 */
/* Text colour palette for 8022 (ARGB4444). The slots are NOT the 8033 entries:
 * C01_3 writes 8033 1 = (0,0,0) and then uses 8022 1 for the quiz question, which
 * would be invisible on the dark Y000 frame, so 8033 must address a different
 * (background) bank. Fixed readable colours: 1 white, 2 yellow, 3 red (quiz),
 * 4 amber, 5 cyan (terminal), 15 white (dialogue). */
static const uint16_t text_palette[16] = {
    0xFFFF, 0xFFFF, 0xFFF5, 0xFF55, 0xFFF0, 0xF5FF, 0xFFFF, 0xFFFF,
    0xF888, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
void vm_on_palette(int slot, int r, int g, int b) {
    if (slot == 14 || slot == 13) {
        int lum = (r + g + b) * 255 / 93;          /* 0..255 */
        int base = (slot == 14) ? (6 + 7 + 10) * 255 / 93 : (2 + 3 + 7) * 255 / 93;
        int f = lum - base;
        if (f < 0) f = 0;
        if (f > palette_flash) palette_flash = f;
        else if (slot == 14) palette_flash = f;    /* slot 14 is written last in a triplet */
    }
}

/* OP_TEXTPOS / OP_TEXTCOL: free-positioned text (quiz / title screens). The
 * next texts are drawn at Saturn (x, y) instead of the message window until
 * the scene changes. Colour slots are ignored (text is always white). */
static int free_text = 0, free_text_x = 0, free_text_y = 0;
static uint16_t cur_text_color = 0xFFFF;
/* 801f caret: the Saturn shows a blinking cursor where the next typed character
 * goes (database search in A001_6/7, B022_4; quiz answer in C01_3). A001_6 steps
 * x by 19 px per character at y=10; (14,320) parks it off screen. */
static int caret_x = 320, caret_y = 0, caret_frames = 0;
void vm_on_terminal_cursor(int y, int x) {
    caret_x = x; caret_y = y; caret_frames = 0;
    terminal_set_caret(x >= 0 && x < 320);   /* on the terminal the caret follows our own line layout */
}
static void draw_caret(void) {
    if (caret_x >= 320 || caret_x < 0) return;
    if (terminal_active()) return;              /* terminal.c draws its own caret */
    caret_frames++;
    if ((caret_frames / 20) & 1) return;                    /* blink: 20 frames on, 20 off */
    float x = caret_x * SAT_SCALE_X, y = (caret_y + 12) * SAT_SCALE_Y;   /* underline below the cell */
    draw_message_box(x, y, 16 * SAT_SCALE_X, 2 * SAT_SCALE_Y, 0xFF000000 | ((cur_text_color & 0x0F00) << 12) | ((cur_text_color & 0x00F0) << 8) | ((cur_text_color & 0x000F) << 4));
}
void vm_on_text_pos(int x, int y) {
    free_text = 1; free_text_x = x; free_text_y = y;
    asset_set_text_color(cur_text_color);   /* quiz text takes the 8022 colour */
}
/* 8022 slot: Saturn text palette slot. 15 = white (dialogue), 4 = amber and 5 = cyan
 * on the terminal screens, 1/2/3 on the C01_3 quiz. The dialogue window itself
 * stays white; the colour applies to the terminal and to 8040 free text. */
void vm_on_text_color(int slot) {
    uint16_t c = (slot >= 0 && slot < 16) ? text_palette[slot] : 0xFFFF;
    cur_text_color = c;
    terminal_set_color(c);
    if (free_text) asset_set_text_color(c);
}

static void fade_update(void) {
    if (fade_level < fade_target) {
        fade_level += fade_step;
        if (fade_level > fade_target) fade_level = fade_target;
    } else if (fade_level > fade_target) {
        fade_level -= fade_step;
        if (fade_level < fade_target) fade_level = fade_target;
    }
}

/* Script names are lower case and may use '-' where the ISO 9660 file name has
 * '_' (the script says "a020-2", the file is A020_2.SCO). */
static void upper_name(const char *src, char *dst, int cap) {
    int i;
    for (i = 0; src[i] && i < cap - 1; i++) {
        char ch = src[i];
        if (ch == '-') ch = '_';
        dst[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
    }
    dst[i] = '\0';
}

/* Load <NAME>.SCO (NAME upper case) from the CD, falling back to the romdisk.
 * path_out receives the path that was actually opened (for logging). */
static uint8_t *load_scenario_by_name(const char *name_upper, uint32_t *size_out,
                                      char *path_out, size_t cap) {
    snprintf(path_out, cap, SCENARIO_DIR "/%s.SCO", name_upper);
    uint8_t *data = asset_load_scenario(path_out, size_out);
    if (!data) {
        snprintf(path_out, cap, "/rd/%s.SCO", name_upper);
        data = asset_load_scenario(path_out, size_out);
    }
    return data;
}

void update_text_texture(const uint8_t *text);   /* defined below, used by the slot menu */

/* ---- Save / load ------------------------------------------
 * 8028 / 801e make vm_step() return VM_SAVE / VM_LOAD; the main loop then opens
 * a slot menu through vm_begin_choice() and acts on the chosen slot here. */
static int sys_menu = 0;                 /* 0, VM_SAVE / VM_LOAD (slot menu), SYS_MAIN, SYS_BACKLOG, SYS_MSG */
#define SYS_MAIN    10
#define SYS_BACKLOG 11
#define SYS_MSG     12
static vm_context_t sys_backup;          /* VM state under the system menu (restored on cancel) */
static int  sys_has_backup = 0;
static int  auto_mode = 0;               /* Y: advance dialogue automatically */
static int  skip_mode = 0;               /* system menu: auto-advance lines already read */
static int  cur_line_read = 0;           /* the line on screen had been read before it was shown */

/* Read-line memory for the skip mode (QA Q2): one bit per (script, text ip).
 * Scripts are hashed to 128 buckets, ip/2 is at most 36k (files < 72 KB), so
 * 128 x 32768 bits = 512 KB of RAM. Session only (not saved to the VMU). */
#define READ_BUCKETS 128
#define READ_BITS_PER_BUCKET 32768
static uint32_t read_bits[READ_BUCKETS * READ_BITS_PER_BUCKET / 32];

static uint32_t read_index(const char *script, uint32_t ip) {
    uint32_t h = 5381;
    for (const char *p = script; *p; p++) h = h * 33u + (uint8_t)*p;
    uint32_t slot = (ip / 2) % READ_BITS_PER_BUCKET;
    return (h % READ_BUCKETS) * READ_BITS_PER_BUCKET + slot;
}
static int  line_was_read(const char *script, uint32_t ip) {
    uint32_t i = read_index(script, ip);
    return (read_bits[i >> 5] >> (i & 31)) & 1u;
}
static void mark_line_read(const char *script, uint32_t ip) {
    uint32_t i = read_index(script, ip);
    read_bits[i >> 5] |= 1u << (i & 31);
}
static int  backlog_page = 0;

/* Shift-JIS labels (the source file is UTF-8, so they are spelled out). */
#define SJ_SLOT   "\x83\x58\x83\x8d\x83\x62\x83\x67"           /* スロット */
#define SJ_CANCEL "\x83\x4c\x83\x83\x83\x93\x83\x5a\x83\x8b"   /* キャンセル */
#define SJ_SAVED  "\x83\x5a\x81\x5b\x83\x75\x82\xb5\x82\xdc\x82\xb5\x82\xbd"  /* セーブしました */
#define SJ_LOADED "\x83\x8d\x81\x5b\x83\x68\x82\xb5\x82\xdc\x82\xb5\x82\xbd"  /* ロードしました */
#define SJ_NODATA "\x83\x66\x81\x5b\x83\x5e\x82\xaa\x82\xa0\x82\xe8\x82\xdc\x82\xb9\x82\xf1"  /* データがありません */
#define SJ_NOVMU  "\x82\x75\x82\x6c\x82\x74\x82\xaa\x82\xa0\x82\xe8\x82\xdc\x82\xb9\x82\xf1"  /* ＶＭＵがありません */
#define SJ_FAILED "\x83\x5a\x81\x5b\x83\x75\x82\xc5\x82\xab\x82\xdc\x82\xb9\x82\xf1\x82\xc5\x82\xb5\x82\xbd" /* セーブできませんでした */

static void collect_save(save_data_t *d) {
    memset(d, 0, sizeof(*d));
    memcpy(d->magic, SAVE_MAGIC, 4);
    d->version = SAVE_VERSION;
    d->route = (uint16_t)vm_ctx.route;
    memcpy(d->cur_script, cur_script, VM_NAME_LEN);
    /* Resume at the command menu the save item was chosen from. */
    d->cur_ip = (vm_ctx.menu_sp > 0) ? vm_ctx.menu_stack_ip[vm_ctx.menu_sp - 1] : vm_ctx.ip;
    d->other_started = (uint16_t)other_started;
    d->other_route = (uint16_t)other_ctx.route;
    memcpy(d->other_script, other_script, VM_NAME_LEN);
    d->other_ip = other_ctx.ip;
    memcpy(d->bg_name, vm_ctx.bg_name, VM_NAME_LEN);
    memcpy(d->other_bg, other_ctx.bg_name, VM_NAME_LEN);
    d->bgm = (uint16_t)bgm_current();
    memcpy(d->vars, vm_ctx.vars, sizeof(d->vars));
}

static int apply_save(const save_data_t *d) {
    char path[64];
    uint32_t size = 0;
    uint8_t *data = load_scenario_by_name(d->cur_script, &size, path, sizeof(path));
    if (!data) {
        printf("LOAD: script %s not found\n", d->cur_script);
        return 0;
    }
    vm_init(&vm_ctx, data, size);
    memcpy(vm_ctx.vars, d->vars, sizeof(vm_ctx.vars));
    vm_ctx.ip = d->cur_ip;
    vm_ctx.route = d->route;
    if (vm_ctx.route == 0) {
        /* Saved from a run that skipped the title (802a never executed):
         * derive the route from the script name so switching still works. */
        vm_ctx.route = (d->cur_script[0] == 'B') ? 2 : (d->cur_script[0] == 'C' || d->cur_script[1] == 'B') ? 3 : 1;
    }
    memcpy(vm_ctx.bg_name, d->bg_name, VM_NAME_LEN);
    memcpy(cur_script, d->cur_script, VM_NAME_LEN);

    other_started = 0;
    if (d->other_started) {
        uint8_t *odata = load_scenario_by_name(d->other_script, &size, path, sizeof(path));
        if (odata) {
            vm_init(&other_ctx, odata, size);
            other_ctx.ip = d->other_ip;
            other_ctx.route = d->other_route;
            memcpy(other_ctx.bg_name, d->other_bg, VM_NAME_LEN);
            memcpy(other_script, d->other_script, VM_NAME_LEN);
            other_started = 1;
        }
    }
    if (vm_ctx.bg_name[0]) vm_on_background(vm_ctx.bg_name);
    if (d->bgm) bgm_play(d->bgm); else bgm_stop();   /* QA Q3: restore the music */
    printf("LOAD: %s ip=%lu route=%d bgm=%d\n", cur_script, (unsigned long)vm_ctx.ip, vm_ctx.route, d->bgm);
    return 1;
}

/* Open the slot menu for VM_SAVE / VM_LOAD. Every entry jumps back to the
 * current ip; the choice is read in handle_slot_choice(). */
static void open_slot_menu(int kind) {
    static uint8_t labels[SAVE_SLOTS + 1][VM_CHOICE_TEXT_LEN];
    const uint8_t *ptrs[SAVE_SLOTS + 1];
    uint32_t targets[SAVE_SLOTS + 1];
    for (int i = 0; i < SAVE_SLOTS; i++) {
        char name[VM_NAME_LEN];
        save_peek(i, name, sizeof(name));
        snprintf((char *)labels[i], VM_CHOICE_TEXT_LEN, SJ_SLOT "\x82%c  %s",
                 0x50 + i, name[0] ? name : "----");          /* １ ２ ３ */
        ptrs[i] = labels[i];
        targets[i] = vm_ctx.ip;
    }
    strncpy((char *)labels[SAVE_SLOTS], SJ_CANCEL, VM_CHOICE_TEXT_LEN - 1);
    ptrs[SAVE_SLOTS] = labels[SAVE_SLOTS];
    targets[SAVE_SLOTS] = vm_ctx.ip;
    vm_begin_choice(&vm_ctx, ptrs, targets, SAVE_SLOTS + 1);
    sys_menu = kind;
}

/* Leave the system menu: put the VM back where it was and redraw. */
static void sys_restore(void) {
    if (sys_has_backup) {
        memcpy(&vm_ctx, &sys_backup, sizeof(vm_ctx));
        sys_has_backup = 0;
    }
    sys_menu = 0;
    vm_redraw(&vm_ctx);
}

/* Show a one-line message; A (handled in the main loop) then restores or continues. */
static void sys_message(const char *msg) {
    update_text_texture((const uint8_t *)msg);
    vm_ctx.is_choosing = 0;
    vm_ctx.is_waiting = 1;
    vm_ctx.wait_timer = 0;
    vm_ctx.key_lock = 0;
    sys_menu = SYS_MSG;
}

static void handle_slot_choice(void) {
    int kind = sys_menu, slot = vm_ctx.select_idx;
    if (slot >= SAVE_SLOTS) {                             /* キャンセル */
        if (sys_has_backup) sys_restore(); else sys_menu = 0;
        return;
    }
    const char *msg;
    if (kind == VM_SAVE) {
        static save_data_t d;
        if (sys_has_backup) {
            /* Save the state under the menu, not the menu itself. */
            memcpy(&vm_ctx, &sys_backup, sizeof(vm_ctx));
            collect_save(&d);
            memcpy(&vm_ctx, &sys_backup, sizeof(vm_ctx));   /* keep the backup valid */
        } else {
            collect_save(&d);
        }
        int rc = save_write(slot, &d);
        msg = (rc == SAVE_OK) ? SJ_SAVED : (rc == SAVE_NO_VMU) ? SJ_NOVMU : SJ_FAILED;
    } else {
        static save_data_t d;
        int rc = save_read(slot, &d);
        if (rc == SAVE_OK && apply_save(&d)) {
            sys_has_backup = 0;                           /* the old state is gone */
            msg = SJ_LOADED;
        } else {
            msg = (rc == SAVE_NO_VMU) ? SJ_NOVMU : SJ_NODATA;
        }
    }
    sys_message(msg);
}

/* ---- System menu (Start), auto mode, fast-forward, backlog ---- */
static int  switch_route(void);   /* defined below */
static void start_title(void);
#define SJ_SWITCH "\x83\x4c\x83\x83\x83\x89\x83\x4e\x83\x5e\x81\x5b\x90\xd8\x91\xd6"   /* キャラクター切替 */
#define SJ_AUTO   "\x83\x49\x81\x5b\x83\x67\x91\x97\x82\xe8"                            /* オート送り */
#define SJ_ON     "\x81\x46\x82\x6e\x82\x6d"                                           /* ：ＯＮ */
#define SJ_OFF    "\x81\x46\x82\x6e\x82\x65\x82\x65"                                   /* ：ＯＦＦ */
#define SJ_TITLE  "\x83\x5e\x83\x43\x83\x67\x83\x8b\x82\xd6"                            /* タイトルへ */
#define SJ_BACK   "\x82\xe0\x82\xc7\x82\xe9"                                           /* もどる */
#define SJ_SAVE   "\x83\x5a\x81\x5b\x83\x75"                                           /* セーブ */
#define SJ_LOAD   "\x83\x8d\x81\x5b\x83\x68"                                           /* ロード */
#define SJ_LOG    "\x83\x6f\x83\x62\x83\x4e\x83\x8d\x83\x4f"                            /* バックログ */
#define SJ_SKIP   "\x8a\xf9\x93\xc7\x83\x58\x83\x4c\x83\x62\x83\x76"                    /* 既読スキップ */

enum { SM_SAVE, SM_LOAD, SM_SWITCH, SM_AUTO, SM_SKIP, SM_TITLE, SM_BACK, SM_COUNT };
static int sys_items[SM_COUNT];
static int sys_item_count = 0;

static int can_switch_route(void) {
    return (vm_ctx.route == 1 || vm_ctx.route == 2) && !audio_voice_playing();
}

static void open_system_menu(void) {
    if (sys_menu || game_state_ending()) return;
    if (!(vm_ctx.is_waiting || vm_ctx.is_choosing)) return;
    memcpy(&sys_backup, &vm_ctx, sizeof(vm_ctx));
    sys_has_backup = 1;
    vm_ctx.menu_armed = 0;                                /* not a script menu: no return stack push */

    static uint8_t labels[SM_COUNT][VM_CHOICE_TEXT_LEN];
    const uint8_t *ptrs[SM_COUNT];
    uint32_t targets[SM_COUNT];
    sys_item_count = 0;
#define ADD(id, text) do { strncpy((char *)labels[sys_item_count], (text), VM_CHOICE_TEXT_LEN - 1); \
        ptrs[sys_item_count] = labels[sys_item_count]; targets[sys_item_count] = vm_ctx.ip; \
        sys_items[sys_item_count++] = (id); } while (0)
    ADD(SM_SAVE, SJ_SAVE);
    ADD(SM_LOAD, SJ_LOAD);
    if (can_switch_route()) ADD(SM_SWITCH, SJ_SWITCH);
    ADD(SM_AUTO, auto_mode ? SJ_AUTO SJ_ON : SJ_AUTO SJ_OFF);
    ADD(SM_SKIP, skip_mode ? SJ_SKIP SJ_ON : SJ_SKIP SJ_OFF);
    ADD(SM_TITLE, SJ_TITLE);
    ADD(SM_BACK, SJ_BACK);
#undef ADD
    vm_begin_choice(&vm_ctx, ptrs, targets, sys_item_count);
    sys_menu = SYS_MAIN;
#ifndef NDEBUG
    printf("SYS: menu open (%d items)\n", sys_item_count);
#endif
}

static void handle_system_choice(void) {
    int sel = vm_ctx.select_idx;
    int id = (sel >= 0 && sel < sys_item_count) ? sys_items[sel] : SM_BACK;
#ifndef NDEBUG
    printf("SYS: choice %d (id %d)\n", sel, id);
#endif
    switch (id) {
        case SM_SAVE:   open_slot_menu(VM_SAVE); break;    /* sys_has_backup stays set */
        case SM_LOAD:   open_slot_menu(VM_LOAD); break;
        case SM_SWITCH: sys_restore(); switch_route(); break;
        case SM_AUTO:   auto_mode = !auto_mode; sys_restore(); break;
        case SM_SKIP:   skip_mode = !skip_mode; sys_restore(); break;
        case SM_TITLE:  sys_has_backup = 0; sys_menu = 0; start_title(); break;
        default:        sys_restore(); break;
    }
}

/* Backlog view (X): four lines per page, newest page first. */
static void show_backlog(void) {
    static uint8_t buf[4 * VM_LOG_LEN + 16];
    int total = vm_log_count(&vm_ctx);
    int pages = (total + 3) / 4;
    if (pages == 0) pages = 1;
    if (backlog_page >= pages) backlog_page = pages - 1;
    if (backlog_page < 0) backlog_page = 0;
    size_t pos = 0;
    /* Oldest of the page at the top, newest at the bottom. */
    for (int k = 3; k >= 0; k--) {
        int age = backlog_page * 4 + k;
        if (age >= total) continue;
        const uint8_t *line = vm_log_get(&vm_ctx, age);
        size_t n = strlen((const char *)line);
        if (pos + n + 2 >= sizeof(buf)) break;
        if (pos) buf[pos++] = '\n';
        memcpy(buf + pos, line, n);
        pos += n;
    }
    if (pos == 0) {
        strcpy((char *)buf, SJ_LOG);
        pos = strlen((char *)buf);
    }
    buf[pos] = '\0';
    update_text_texture(buf);
}

static void open_backlog(void) {
    if (sys_menu || game_state_ending()) return;
    if (!(vm_ctx.is_waiting || vm_ctx.is_choosing)) return;
    memcpy(&sys_backup, &vm_ctx, sizeof(vm_ctx));
    sys_has_backup = 1;
    vm_ctx.is_choosing = 0;
    vm_ctx.is_waiting = 1;                                /* freeze the script */
    backlog_page = 0;
    sys_menu = SYS_BACKLOG;
#ifndef NDEBUG
    printf("SYS: backlog open (%d lines)\n", vm_log_count(&vm_ctx));
#endif
    show_backlog();
}

/* Swap the running route with the parked one. Allowed only while the script waits
 * for input and the current route is 1 or 2 (not in the merged AB01/C* part).
 * Game flags are shared, so the current vars are copied to the other context. */
static int switch_route(void) {
    int cur = vm_ctx.route;
    if ((cur != 1 && cur != 2) || !(vm_ctx.is_waiting || vm_ctx.is_choosing)) return 0;
    /* Wait for the current voice to end: reading the next script from the CD while
     * the libADX decoder thread streams from it wedged the decoder (adx_stop never
     * returned afterwards). */
    if (audio_voice_playing()) return 0;
    int to = 3 - cur;
    if (!other_started) {
        /* First visit: the other route starts from its entry script. */
        const char *entry = (to == 1) ? "A01" : "B01";
        char path[64];
        uint32_t size = 0;
        uint8_t *data = load_scenario_by_name(entry, &size, path, sizeof(path));
        if (!data) {
            printf("ROUTE: entry %s not found\n", path);
            return 0;
        }
        vm_init(&other_ctx, data, size);
        other_ctx.route = to;
        strncpy(other_script, entry, VM_NAME_LEN - 1);
        other_started = 1;
    }
    /* The current voice is left to finish: calling adx_stop() here hangs the
     * engine (seen as a freeze right after "LibADX: Stopping"); the next voiced
     * line restarts the decoder through audio_play_voice() as usual. */
    static vm_context_t tmp;
    memcpy(&tmp, &vm_ctx, sizeof(tmp));
    memcpy(&vm_ctx, &other_ctx, sizeof(tmp));
    memcpy(&other_ctx, &tmp, sizeof(tmp));
    memcpy(vm_ctx.vars, other_ctx.vars, sizeof(vm_ctx.vars));   /* shared flags */
    char name[VM_NAME_LEN];
    memcpy(name, cur_script, VM_NAME_LEN);
    memcpy(cur_script, other_script, VM_NAME_LEN);
    memcpy(other_script, name, VM_NAME_LEN);
    printf("ROUTE: switched to %d (%s ip=%lu)\n", vm_ctx.route, cur_script, (unsigned long)vm_ctx.ip);
    if (vm_ctx.bg_name[0]) vm_on_background(vm_ctx.bg_name);
    vm_redraw(&vm_ctx);
    return 1;
}

/* Multi-disc. Each disc image carries a marker /cd/DISC<n>.ID. */
#ifdef MULTI_DISC
#define SJ_DISC_A "\x83\x66\x83\x42\x83\x58\x83\x4e"                   /* ディスク */
#define SJ_DISC_B "\x82\xc9\x8c\xf0\x8a\xb7\x82\xb5\x82\xc4\x82\xad\x82\xbe\x82\xb3\x82\xa2"  /* に交換してください */

static int disc_present(int n) {
    char path[32];
    snprintf(path, sizeof(path), "/cd/DISC%d.ID", n);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void request_disc(int n) {
    static uint8_t msg[64];
    snprintf((char *)msg, sizeof(msg), SJ_DISC_A "\x82%c" SJ_DISC_B, 0x4f + n);   /* １２３ */
    audio_stop_voice();
    bgm_stop();
    update_text_texture(msg);
    want_disc = n;
    game_state = 2;
    printf("DISC: waiting for disc %d\n", n);
}
#endif /* MULTI_DISC */

#if DEMO_SWEEP
/* Smoke test over every scenario file on the CD. */
#define SWEEP_MAX 128
static char sweep_names[SWEEP_MAX][VM_NAME_LEN];
static int  sweep_count = 0, sweep_idx = -1, sweep_timer = 0;

static int sweep_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void sweep_init(void) {
    file_t d = fs_open(SCENARIO_DIR, O_RDONLY | O_DIR);
    if (d < 0) { printf("SWEEP: cannot open " SCENARIO_DIR "\n"); return; }
    const dirent_t *de;
    while ((de = fs_readdir(d)) && sweep_count < SWEEP_MAX) {
        size_t n = strlen(de->name);
        if (n < 5 || n - 4 >= VM_NAME_LEN) continue;
        const char *ext = de->name + n - 4;
        if (strcasecmp(ext, ".SCO") != 0) continue;
        char *dst = sweep_names[sweep_count++];
        upper_name(de->name, dst, VM_NAME_LEN);
        dst[n - 4] = '\0';
    }
    fs_close(d);
    qsort(sweep_names, sweep_count, VM_NAME_LEN, sweep_cmp);
    /* Begin at START_SCRIPT_NAME when it is in the list (resume a partial sweep). */
    for (int i = 0; i < sweep_count; i++) {
        if (strcmp(sweep_names[i], EVE_STR(START_SCRIPT_NAME)) == 0) { sweep_idx = i - 1; break; }
    }
    printf("SWEEP: %d scripts, starting at index %d\n", sweep_count, sweep_idx + 1);
}

static void sweep_next(void) {
    sweep_idx++;
    if (sweep_idx >= sweep_count) {
        printf("SWEEP: done\n");
        game_state = 1;
        return;
    }
    const char *name = sweep_names[sweep_idx];
    char path[64];
    uint32_t size = 0;
    uint8_t *data = load_scenario_by_name(name, &size, path, sizeof(path));
    if (!data) {
        printf("SWEEP: start %s FAILED (not found)\n", name);
        return;
    }
    audio_stop_voice();
    bgm_stop();
    sys_menu = 0;
    sys_has_backup = 0;
    vm_init(&vm_ctx, data, size);
    vm_ctx.route = (name[0] == 'B') ? 2 : (name[0] == 'C' || name[1] == 'B') ? 3 : 1;
    memcpy(cur_script, name, VM_NAME_LEN);
    update_text_texture((const uint8_t *)"");
    printf("SWEEP: start %s (%d/%d, %lu bytes)\n", name, sweep_idx + 1, sweep_count, (unsigned long)size);
}
#endif

/* Back to the title menu of A01 (block 100): used after an ending. */
static void start_title(void) {
    char path[64];
    uint32_t size = 0;
    uint8_t *data = load_scenario_by_name("A01", &size, path, sizeof(path));
    if (!data) {
        printf("TITLE: A01 not found\n");
        return;
    }
    audio_stop_voice();
    bgm_stop();
    if (other_ctx.script_data) vm_cleanup(&other_ctx);
    other_started = 0;
    other_script[0] = '\0';
    vm_init(&vm_ctx, data, size);
    vm_ctx.ip = vm_find_block(&vm_ctx, 100);
    memcpy(cur_script, "A01", 4);
    asset_free_evb1(&scene_bg);
    asset_free_evb1(&scene_spr);
    update_text_texture((const uint8_t *)"");
}

/* OP_BG: /cd/graphics/bg/<NAME>.BIN (converted by convert_images.py). A new
 * background also clears the sprite layer, matching how the script sequences
 * "BG, SPRITE, text" blocks. */
void vm_on_background(const char *name) {
    char upper[VM_NAME_LEN], path[64];
    upper_name(name, upper, sizeof(upper));
    snprintf(path, sizeof(path), "/cd/graphics/bg/%s.BIN", upper);

    evb1_texture_t t;
    if (asset_load_evb1(path, &t) == 0) {
        asset_free_evb1(&scene_bg);
        scene_bg = t;
#ifndef NDEBUG
        printf("BG: %s (%dx%d)\n", path, t.w, t.h);
#endif
    } else {
        printf("BG: %s not found, keeping previous\n", path);
    }
    asset_free_evb1(&scene_spr);
    terminal_set_mode(0);
    free_text = 0;
    asset_set_text_color(0xFFFF);   /* dialogue window text is always white */
    palette_flash = 0;
}

/* OP_SPRITE: /cd/graphics/spr/<NAME>.BIN drawn at Saturn coordinates (x, y). */
void vm_on_sprite(const char *name, int x, int y) {
    char upper[VM_NAME_LEN], path[64];
    upper_name(name, upper, sizeof(upper));
    snprintf(path, sizeof(path), "/cd/graphics/spr/%s.BIN", upper);

    evb1_texture_t t;
    if (asset_load_evb1(path, &t) == 0) {
        asset_free_evb1(&scene_spr);
        scene_spr = t;
        scene_spr_x = x; scene_spr_y = y;
#ifndef NDEBUG
        printf("SPRITE: %s (%dx%d) at %d,%d\n", path, t.w, t.h, x, y);
#endif
    } else {
        printf("SPRITE: %s not found\n", path);
    }
}

/* Callback implementation for VM */
void update_text_texture(const uint8_t *text) {
#ifndef NDEBUG
    /* One line per displayed text; the serial port throttles unattended runs to a
     * few lines per second, so -DSHORT_TEXT_LOG keeps only a prefix (auto_run.sh
     * counts the lines, it does not need the text). */
#ifdef SHORT_TEXT_LOG
    printf("update_text_texture: '%.24s'\n", text);
#else
    printf("update_text_texture: '%s'\n", text);
#endif
#endif
    if (font_vram) {
        asset_update_text(text, font_vram);
    }
}

int main(int argc, char **argv) {
    /* Mount Romdisk */
    extern uint8_t romdisk[];
    if (fs_romdisk_mount("/rd", romdisk, 0) < 0) {
#ifndef NDEBUG
        printf("FAILED TO MOUNT ROMDISK\n");
#endif
    }
#ifndef NDEBUG
    else {
        printf("ROMDISK MOUNTED SUCCESSFULLY\n");
    }
#endif

    /* Initialize Hardware & Modules */
    render_init();
    input_init();
    audio_init(EVE_DISC);   /* voices: /cd/audio/Disc<EVE_DISC>/<TOKEN>.adx via libADX */
    
#ifdef TEST_MOVIE_AT_START
    /* Isolated libmpeg test: play one movie before anything else is loaded. */
    printf("TEST_MOVIE_AT_START: playing /cd/movies/K02.mpg\n");
    video_play("/cd/movies/K02.mpg");
    printf("TEST_MOVIE_AT_START: done\n");
#endif

    /* Wait for CD-ROM to be ready */
#ifndef NDEBUG
    printf("Waiting for CD-ROM to be ready...\n");
#endif
    int retries = 30;
    FILE *cd_test = NULL;
    while (retries-- > 0) {
        cd_test = fopen("/cd/S0005_LINEAR.BIN", "rb");
        if (cd_test) {
            fclose(cd_test);
#ifndef NDEBUG
            printf("CD-ROM is ready!\n");
#endif
            break;
        }
        thd_sleep(100);
    }
#ifndef NDEBUG
    if (!cd_test) {
        printf("CD-ROM check timed out!\n");
    }
#endif

#if !defined(NO_INTRO) && !defined(NO_VIDEO) && !DEMO_AUTO_ADVANCE && !defined(START_SCRIPT_NAME_GIVEN)
    /* Boot sequence: C's ware logo, then the opening movie.
     * A or Start skips each one. Skipped in unattended test builds. */
    video_play("/cd/movies/CSLOGO30.mpg");
    video_play("/cd/movies/OPENING.mpg");
#endif
    /* Load Assets */
    /* BIOS font (half-width and fallback), then the Saturn typeface made by
     * tools/convert.py. The file name argument of asset_load_font is unused. */
    if (asset_load_font(NULL) == 0) {
        asset_load_satfont("/cd/graphics/ui/SATFONT.BIN");
    }
    font_vram = render_alloc_txr(512 * 256 * 2);
    terminal_init();
    bgm_init();
    
#ifndef NDEBUG
    printf("Loading msgbox: /cd/graphics/ui_assets/SPR3.bmp\n");
#endif
    msgbox_vram = asset_load_bg("/cd/graphics/ui_assets/SPR3.bmp"); 
    if (!msgbox_vram) {
#ifndef NDEBUG
        printf("FAILED TO LOAD MSGBOX /cd/graphics/ui_assets/SPR3.bmp\n");
#endif
    }
#ifndef NDEBUG
    else {
        printf("Msgbox loaded successfully.\n");
    }
#endif
    
#ifndef NDEBUG
    printf("Loading background: /cd/S0005_LINEAR.BIN\n");
#endif
    bg_vram = asset_load_bg("/cd/S0005_LINEAR.BIN");
    if (!bg_vram) {
#ifndef NDEBUG
        printf("FAILED TO LOAD BACKGROUND /cd/S0005_LINEAR.BIN\n");
#endif
    }
#ifndef NDEBUG
    else {
        printf("Background loaded successfully.\n");
    }
#endif
    
    /* Load the first scenario (START_SCRIPT_NAME, default A001) from the CD. */
    char sc_path[64];
    uint32_t sc_size = 0;
    uint8_t *sc_data = load_scenario_by_name(EVE_STR(START_SCRIPT_NAME), &sc_size,
                                             sc_path, sizeof(sc_path));
    if (sc_data) {
#ifndef NDEBUG
        printf("Loaded scenario: %s (%lu bytes)\n", sc_path, (unsigned long)sc_size);
#endif
        vm_init(&vm_ctx, sc_data, sc_size);
        strncpy(cur_script, EVE_STR(START_SCRIPT_NAME), VM_NAME_LEN - 1);
#if START_BLOCK
        vm_ctx.ip = vm_find_block(&vm_ctx, START_BLOCK);
#ifndef NDEBUG
        printf("Start block %d at ip %lu\n", START_BLOCK, (unsigned long)vm_ctx.ip);
#endif
#endif
        update_text_texture((const uint8_t *)"SCENARIO LOADED: " EVE_STR(START_SCRIPT_NAME) ".SCO");
    }
#ifndef NDEBUG
    else {
        printf("FAILED TO LOAD SCENARIO %s (tried " SCENARIO_DIR " and /rd)\n", sc_path);
    }
#endif

    /* Main Loop */
    static uint32_t prev_buttons = 0;
    static int auto_advance_timer = 0;
    while (1) {
        uint32_t buttons = input_poll();
        uint32_t pressed = buttons & ~prev_buttons;
        prev_buttons = buttons;
#ifndef NDEBUG
        if (pressed) printf("input: buttons=%08lx pressed=%08lx\n", (unsigned long)buttons, (unsigned long)pressed);
#endif
        
        if (game_state == 0) {
#if DEMO_AUTO_ADVANCE
            /* Auto advance: press A every DEMO_AUTO_ADVANCE frames for text, 3x that in a menu
             * (always picks the first entry). Unattended emulator tests only. */
            auto_advance_timer++;
            if (vm_is_choosing(&vm_ctx)) {
#if DEMO_RANDOM
                /* Pseudo-random entry (LCG): move the cursor down until it is on the target. */
                static int demo_target = -1;
                static uint32_t demo_rng = DEMO_RANDOM;
                if (demo_target < 0) {
                    demo_rng = demo_rng * 1103515245u + 12345u;
                    demo_target = (int)((demo_rng >> 16) % (uint32_t)vm_ctx.choice_count);
#if DEMO_EXPLORE
                    if (sys_menu == 0) demo_target = demo_explore_pick();
#endif
                }
                if (auto_advance_timer >= DEMO_AUTO_ADVANCE) {
                    if (vm_ctx.select_idx != demo_target) {
                        pressed |= CONT_DPAD_DOWN;
                    } else {
                        pressed |= CONT_A;
                        demo_target = -1;
                    }
                    auto_advance_timer = 0;
                }
#else
                if (auto_advance_timer >= 3 * DEMO_AUTO_ADVANCE) {
                    pressed |= CONT_A;
                    auto_advance_timer = 0;
                }
#endif
            } else if (auto_advance_timer >= DEMO_AUTO_ADVANCE) {
                pressed |= CONT_A;
                auto_advance_timer = 0;
            }
#else
            (void)auto_advance_timer;
#endif
            
#if DEMO_SWITCH
            static int demo_switch_timer = 0;
            if (++demo_switch_timer >= DEMO_SWITCH && switch_route()) demo_switch_timer = 0;
#endif
#if DEMO_SWEEP
            if (sweep_count == 0) sweep_init();
            if (sweep_idx < 0 || ++sweep_timer >= DEMO_SWEEP) { sweep_timer = 0; sweep_next(); }
#endif
#if DEMO_KEYS
            static int demo_key_timer = 0, demo_key_idx = 0;
            if (++demo_key_timer >= DEMO_KEYS) {
                static const uint32_t keys[3] = { CONT_START, CONT_X, CONT_Y };
                pressed |= keys[demo_key_idx++ % 3];
                demo_key_timer = 0;
            }
#endif
            /* System menu / backlog / auto */
            if (sys_menu == SYS_BACKLOG) {
                if (pressed & CONT_DPAD_UP)   { backlog_page++; show_backlog(); }
                if (pressed & CONT_DPAD_DOWN) { backlog_page--; show_backlog(); }
                if (pressed & (CONT_X | CONT_B | CONT_A)) sys_restore();
                pressed = 0;                              /* nothing reaches the script */
            } else if (sys_menu == SYS_MSG) {
                if (pressed & (CONT_A | CONT_B)) {
                    if (sys_has_backup) sys_restore();
                    else { sys_menu = 0; vm_ctx.is_waiting = 0; }
                }
                pressed = 0;
            } else if (sys_menu == SYS_MAIN || sys_menu == VM_SAVE || sys_menu == VM_LOAD) {
                if (pressed & CONT_B) {                   /* B = cancel */
                    if (sys_has_backup) sys_restore(); else { sys_menu = 0; vm_ctx.is_choosing = 0; }
                    pressed = 0;
                }
            } else {
                if (pressed & CONT_START) { open_system_menu(); pressed = 0; }
                else if (pressed & CONT_X) { open_backlog(); pressed = 0; }
                else if (pressed & CONT_Y) {
                    auto_mode = !auto_mode;
#ifndef NDEBUG
                    printf("SYS: auto %d\n", auto_mode);
#endif
                }
                /* Hold B: fast-forward dialogue (not menus). Skip mode does the
                 * same on its own for lines that were already read this session. */
                if (!vm_ctx.is_choosing && vm_ctx.is_waiting &&
                    ((buttons & CONT_B) ||
                     (skip_mode && cur_line_read))) {
                    static int ff_timer = 0;
                    if (++ff_timer >= 2) { pressed |= CONT_A; ff_timer = 0; }
                }
                /* Auto mode: advance once the voice has ended and a short pause passed. */
                if (auto_mode && !vm_ctx.is_choosing && vm_ctx.is_waiting && vm_ctx.wait_timer == 0 &&
                    vm_ctx.voice_wait > 45 && !vm_ctx.next_voice[0] && !audio_voice_playing()) {
                    pressed |= CONT_A;
                }
            }

            vm_handle_input(&vm_ctx, pressed);
            if (sys_menu == SYS_MAIN && !vm_is_choosing(&vm_ctx)) handle_system_choice();
            else if ((sys_menu == VM_SAVE || sys_menu == VM_LOAD) && !vm_is_choosing(&vm_ctx)) handle_slot_choice();
            int step_ret = (sys_menu == SYS_BACKLOG || sys_menu == SYS_MSG) ? VM_RUNNING : vm_step(&vm_ctx);
            if (step_ret == VM_WAIT && vm_ctx.is_waiting && vm_ctx.last_text_addr) {
                /* A new dialogue line is on screen: note whether it had been read
                 * before (skip mode uses that), then remember it. */
                cur_line_read = line_was_read(cur_script, vm_ctx.last_text_addr);
                mark_line_read(cur_script, vm_ctx.last_text_addr);
            }
            if (step_ret == VM_SAVE || step_ret == VM_LOAD) {
                open_slot_menu(step_ret);
                step_ret = VM_WAIT;
            }
            if (step_ret == VM_DISC) {
#ifdef MULTI_DISC
                if (vm_ctx.disc_req >= 1 && vm_ctx.disc_req <= 3 && vm_ctx.disc_req != cur_disc) {
                    request_disc(vm_ctx.disc_req);
                }
#endif
                step_ret = VM_RUNNING;
            }
            if (step_ret == VM_CHAIN) {
                /* 8020: load the next scenario file from the CD (name is lower case in the script) */
                char path[64];
                char upper[VM_NAME_LEN];
                upper_name(vm_ctx.pending_script, upper, sizeof(upper));
                uint32_t next_size = 0;
                uint8_t *next_data = load_scenario_by_name(upper, &next_size, path, sizeof(path));
                if (next_data) {
#ifndef NDEBUG
                    printf("Chaining to %s (%lu bytes)\n", path, (unsigned long)next_size);
#endif
                    vm_load_script(&vm_ctx, next_data, next_size);
                    memcpy(cur_script, upper, VM_NAME_LEN);   /* upper is NUL-terminated */
                    step_ret = VM_RUNNING;
                } else {
                    printf("Chain target %s not found. Scenario ending.\n", path);
                    step_ret = VM_END;
                }
            }
            if (step_ret < 0) {
                game_state = 1;
                audio_stop_voice();
                /* おわり / Ａボタンでタイトルへ (Shift-JIS) */
                update_text_texture((const uint8_t *)"\x82\xa8\x82\xed\x82\xe8\n\n"
                                    "\x82\x60\x83\x7b\x83\x5e\x83\x93\x82\xc5\x83\x5e\x83\x43\x83\x67\x83\x8b\x82\xd6");
            }
        } else if (game_state == 1) {
            if (pressed & CONT_A) {
                start_title();
                game_state = 0;
            }
        }
#ifdef MULTI_DISC
        else if (game_state == 2) {
            /* Poll the drive once a second until the requested disc is in. */
            static int disc_timer = 0;
            if (++disc_timer >= 60) {
                disc_timer = 0;
                cdrom_reinit();
                if (disc_present(want_disc)) {
                    cur_disc = want_disc;
                    audio_init(cur_disc);
                    printf("DISC: disc %d detected\n", cur_disc);
                    game_state = 0;
                    vm_redraw(&vm_ctx);
                }
            }
        }
#endif
        
#ifndef NDEBUG
        {
            /* Heap watermark every ~60 s for soak tests: a steadily
             * rising "MEM: used" means a leak (texture, script or save buffers). */
            static int mem_timer = 0;
#ifndef MEM_LOG_FRAMES
#define MEM_LOG_FRAMES 3600   /* ~60 s at 60 fps; use 600 for serial-throttled soak runs */
#endif
            if (++mem_timer >= MEM_LOG_FRAMES) {
                struct mallinfo mi = mallinfo();
                printf("MEM: used=%d free=%d\n", (int)mi.uordblks, (int)mi.fordblks);
                mem_timer = 0;
            }
        }
#endif
        bgm_update();   /* feed the music stream once per frame (src/bgm.c) */
        render_begin();
        
        /* Submit Opaque Polygons */
        render_opaque_begin();
        const evb1_texture_t *term_bg = terminal_background();
        if (term_bg) {
            draw_background_area(term_bg->txr, term_bg->w, term_bg->h);   /* KOJ_TERM / MAR_TERM frame */
        } else if (scene_bg.txr) {
            draw_background_area(scene_bg.txr, scene_bg.w, scene_bg.h);
        } else if (bg_vram) {
            draw_background(bg_vram);
        }
        render_opaque_end();
        
        /* Submit Translucent Polygons */
        render_translucent_begin();
        if (scene_spr.txr) draw_sprite(scene_spr.txr, scene_spr.w, scene_spr.h, scene_spr_x, scene_spr_y);
        terminal_draw();
        draw_caret();
        if (palette_flash > 0) {
            draw_message_box(0, 0, 640, 160 * SAT_SCALE_Y, ((uint32_t)palette_flash << 24) | 0x00FFFFFF);
        }
        if (free_text) {
            if (font_vram) draw_font_layer(font_vram, free_text_x * SAT_SCALE_X, free_text_y * SAT_SCALE_Y, 10.0f);
        } else {
            if (msgbox_vram) {
                draw_text_layer(msgbox_vram, 64, 340, 5.0f);
            } else {
                draw_message_box(40, 340, 560, 120, 0xA0000000); 
            }
            if (font_vram) draw_font_layer(font_vram, 64, 355, 10.0f);
        }
        fade_update();
        draw_fade_overlay(fade_level);
        render_translucent_end();
        
        render_end();
        /* No thd_sleep needed: pvr_wait_ready() in render_begin() provides VBlank sync */
    }

    asset_free_font();
    vm_cleanup(&vm_ctx);
    render_shutdown();
    
    return 0;
}