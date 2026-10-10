#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "renderer.h"
#include "audio.h"

/* Extern font/text update function (implemented in main.c) */
extern void update_text_texture(const uint8_t *text);

/* Shift-JIS fragments used for the choice cursor */
#define SJIS_ARROW  "\x81\xA8"   /* right arrow */
#define SJIS_SPACE  "\x81\x40"   /* full-width space */

/* ---- helpers ------------------------------------------------------------ */

static uint32_t rd16(const vm_context_t *ctx, uint32_t off) {
    if (off + 1 >= ctx->script_size) return 0;
    return ((uint32_t)ctx->script_data[off] << 8) | ctx->script_data[off + 1];
}

static int is_opcode(uint32_t w) {
    return (w >> 8) == 0x80;
}

/* Trailing control run of a dialogue string (measured over all 90 .SCO files,
 * the run only ever appears at the end):
 *   w [N]   wait N units (default 1) and continue without a key press
 *   w [N] k wait N units, then wait for a key
 *   k       wait for a key (same as no code)
 *   n       do not wait; the next line is appended below this one.
 *           A string consisting of "n" alone clears the message window.
 *   s       the line carries two sound tokens (no effect on display) */
typedef struct {
    int wait_frames;   /* 0 = none */
    int need_key;
    int newline;
} text_code_t;

#define TEXT_WAIT_UNIT 30   /* frames per 'w' count */

/* Copy the NUL-terminated dialogue string at byte address addr into out,
 * without its trailing control run (parsed into *code when non-NULL). Other
 * bytes below 0x20 and half-width lower-case letters are dropped too: dialogue
 * never contains them; names and tokens use vm_copy_name instead. */
static int vm_copy_string(const vm_context_t *ctx, uint32_t addr, uint8_t *out, uint32_t cap,
                          text_code_t *code) {
    uint32_t n = 0, i = addr;
    if (code) memset(code, 0, sizeof(*code));
    if (addr >= ctx->script_size) { out[0] = '\0'; return 0; }
    while (i < ctx->script_size && n + 2 < cap) {
        uint8_t c = ctx->script_data[i];
        if (c == 0x00) break;
        if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) {
            if (i + 1 >= ctx->script_size) break;
            out[n++] = c;
            out[n++] = ctx->script_data[i + 1];
            i += 2;
            continue;
        }
        i++;
        if (c < 0x20 && c != '\n') continue;
        if (c >= 'a' && c <= 'z') continue;
        out[n++] = c;
    }
    out[n] = '\0';
    if (code) {
        /* Find the end of the string, then walk back over the control run. */
        uint32_t end = addr;
        while (end < ctx->script_size && ctx->script_data[end] != 0x00) end++;
        uint32_t j = end;
        while (j > addr) {
            uint8_t c = ctx->script_data[j - 1];
            if (c < 0x20 || (c >= 'a' && c <= 'z')) j--; else break;
        }
        /* Do not treat the second byte of a kanji as a control letter. */
        uint32_t k = addr, lead = 0;
        while (k < j) {
            uint8_t c = ctx->script_data[k];
            lead = ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) ? 2 : 1;
            k += lead;
        }
        if (k > j) j++;
        for (uint32_t p = j; p < end; p++) {
            uint8_t c = ctx->script_data[p];
            if (c == 'w') {
                code->wait_frames = TEXT_WAIT_UNIT;
                if (p + 1 < end && ctx->script_data[p + 1] < 0x20) {
                    code->wait_frames = TEXT_WAIT_UNIT * (int)ctx->script_data[p + 1];
                    p++;
                }
            } else if (c == 'k') {
                code->need_key = 1;
            } else if (c == 'n') {
                code->newline = 1;
            }
        }
    }
    return 1;
}

/* Rows the text occupies in the message window (VM_WINDOW_COLS full-width
 * characters per row, explicit newlines counted). */
static int vm_text_rows(const uint8_t *s) {
    int rows = 1, cols = 0;   /* cols in half-width units */
    for (; *s; s++) {
        if (*s == '\n') { rows++; cols = 0; continue; }
        int w = ((*s >= 0x81 && *s <= 0x9F) || (*s >= 0xE0 && *s <= 0xFC)) ? 2 : 1;
        if (w == 2 && s[1]) s++;
        if (cols + w > VM_WINDOW_COLS * 2) { rows++; cols = 0; }
        cols += w;
    }
    return rows;
}

/* Put a line into the message window: appended below the previous one when
 * that line ended with 'n' and it still fits, otherwise replacing it. */
static void vm_window_put(vm_context_t *ctx, const uint8_t *text) {
    /* A line that starts with three or more full-width spaces was centred by
     * hand for the Saturn window (the title: 9 spaces + 15 characters). Centre
     * it for our 19-column window instead so that it does not wrap (QA Q1). */
    static uint8_t centred[VM_WINDOW_LEN];
    int lead = 0;
    while (text[lead * 2] == 0x81 && text[lead * 2 + 1] == 0x40) lead++;
    if (lead >= 3) {
        const uint8_t *body = text + lead * 2;
        int width = 0;
        for (const uint8_t *p = body; *p; p++) {
            if ((*p >= 0x81 && *p <= 0x9F) || (*p >= 0xE0 && *p <= 0xFC)) { width += 2; if (p[1]) p++; }
            else width += 1;
        }
        int pad = (VM_WINDOW_COLS * 2 - width) / 4;        /* full-width spaces */
        if (pad < 0) pad = 0;
        size_t n = 0;
        for (int i = 0; i < pad && n + 2 < sizeof(centred); i++) { centred[n++] = 0x81; centred[n++] = 0x40; }
        strncpy((char *)centred + n, (const char *)body, sizeof(centred) - n - 1);
        centred[sizeof(centred) - 1] = '\0';
        text = centred;
    }
    int rows = vm_text_rows(text);
    size_t used = strlen((const char *)ctx->window);
    if (!ctx->text_append || used == 0 || ctx->window_rows + rows > VM_WINDOW_ROWS ||
        used + 1 + strlen((const char *)text) + 1 > VM_WINDOW_LEN) {
        ctx->window[0] = '\0';
        ctx->window_rows = 0;
        used = 0;
    }
    if (used) ctx->window[used++] = '\n';
    strncpy((char *)ctx->window + used, (const char *)text, VM_WINDOW_LEN - used - 1);
    ctx->window[VM_WINDOW_LEN - 1] = '\0';
    ctx->window_rows += rows;
    update_text_texture(ctx->window);

    /* Backlog ring */
    uint8_t *slot = ctx->log[ctx->log_head];
    strncpy((char *)slot, (const char *)text, VM_LOG_LEN - 1);
    slot[VM_LOG_LEN - 1] = '\0';
    /* Do not cut a Shift-JIS pair at the end of a truncated entry. */
    if (strlen((const char *)text) >= VM_LOG_LEN - 1) {
        int lead = 0;
        for (int i = 0; i < VM_LOG_LEN - 1; i++) {
            if (lead) { lead = 0; continue; }
            if ((slot[i] >= 0x81 && slot[i] <= 0x9F) || (slot[i] >= 0xE0 && slot[i] <= 0xFC)) lead = 1;
        }
        if (lead) slot[VM_LOG_LEN - 2] = '\0';
    }
    ctx->log_head = (ctx->log_head + 1) % VM_LOG_ENTRIES;
    if (ctx->log_count < VM_LOG_ENTRIES) ctx->log_count++;
}

int vm_log_count(const vm_context_t *ctx) {
    return ctx->log_count;
}

const uint8_t *vm_log_get(const vm_context_t *ctx, int age) {
    if (age < 0 || age >= ctx->log_count) return (const uint8_t *)"";
    int idx = (ctx->log_head - 1 - age + VM_LOG_ENTRIES * 2) % VM_LOG_ENTRIES;
    return ctx->log[idx];
}

/* Copy a raw NUL-terminated name/token (no filtering). */
static void vm_copy_raw(const vm_context_t *ctx, uint32_t addr, uint8_t *out, uint32_t cap) {
    uint32_t n = 0;
    if (addr >= ctx->script_size) { out[0] = '\0'; return; }
    while (addr + n < ctx->script_size && n + 1 < cap) {
        uint8_t c = ctx->script_data[addr + n];
        if (c == 0x00) break;
        out[n++] = c;
    }
    out[n] = '\0';
}

static void vm_copy_name(const vm_context_t *ctx, uint32_t addr, char *out, uint32_t cap) {
    vm_copy_raw(ctx, addr, (uint8_t *)out, cap);
}

/* Number of 16-bit operand words for opcodes with a fixed layout.
 * Measured over all 90 .SCO files of the Saturn discs (356k instructions):
 * every opcode below has exactly one operand count, see
 * eve-sco-scenario-format. -1 = variable/unknown (skip to the next 0x80xx). */
static int vm_fixed_operands(uint8_t code) {
    switch (code) {
        case 0x00: case 0x04: case 0x06: case 0x09: case 0x0A: case 0x0C:
        case 0x0D: case 0x11: case 0x16: case 0x17: case 0x18: case 0x1E:
        case 0x21: case 0x24: case 0x27: case 0x28: case 0x2D: case 0x2E:
        case 0x34: case 0x3C: case 0x42:
            return 0;
        case 0x01: case 0x05: case 0x08: case 0x0B: case 0x0E: case 0x0F:
        case 0x10: case 0x12: case 0x13: case 0x14: case 0x19: case 0x1A:
        case 0x20: case 0x23: case 0x25: case 0x26: case 0x29: case 0x2A:
        case 0x2B: case 0x2F: case 0x31: case 0x32: case 0x39: case 0x3A:
        case 0x41: case 0x43:
            return 1;
        case 0x07: case 0x22: case 0x3B: case 0x40:
            return 2;
        case 0x36:
            return 3;
        case 0x1F: case 0x33:
            return 4;
        default:
            return -1;
    }
}

/* ---- lifecycle ---------------------------------------------------------- */

void vm_init(vm_context_t *ctx, uint8_t *data, uint32_t size) {
    /* A context that already runs a script (load game, back to title) must give
     * its old buffer back first: memset() below would lose the pointer and leak
     * 50-70 KB per load (seen as +145 KB/min in the 30-minute soak run). */
    if (ctx->script_data && ctx->script_data != data) free(ctx->script_data);
    memset(ctx, 0, sizeof(vm_context_t));
    vm_load_script(ctx, data, size);
}

void vm_load_script(vm_context_t *ctx, uint8_t *data, uint32_t size) {
    if (ctx->script_data && ctx->script_data != data) free(ctx->script_data);
    ctx->script_data = data;
    ctx->script_size = size;
    ctx->ip = 0;
    ctx->is_waiting = 0;
    ctx->is_choosing = 0;
    ctx->wait_timer = 0;
    ctx->delay_frames = 0;
    ctx->trans_speed = 3;
    ctx->last_text_addr = 0;
    ctx->pending_script[0] = '\0';
    ctx->menu_sp = 0;
    ctx->menu_group_ip = 0;
    ctx->menu_armed = 0;

    /* Decrypt script data (EVE SCORPION format uses bitwise NOT) */
    /* Process 4 bytes at a time for the SH-4 32-bit datapath */
    if (data) {
        uint32_t *p32 = (uint32_t *)data;
        uint32_t words = size >> 2;
        for (uint32_t i = 0; i < words; i++) {
            p32[i] = ~p32[i];
        }
        for (uint32_t i = words << 2; i < size; i++) {
            data[i] = ~data[i];
        }
    }
}

void vm_cleanup(vm_context_t *ctx) {
    if (ctx->script_data) free(ctx->script_data);
    memset(ctx, 0, sizeof(vm_context_t));
}

/* ---- Choice menu -------------------------------------------------------- */

static void vm_render_choice(vm_context_t *ctx) {
    static uint8_t menu[VM_WINDOW_LEN + VM_MAX_CHOICES * (VM_CHOICE_TEXT_LEN + 4) + 2];
    uint32_t pos = 0;
    /* A line that ended with 'n' (e.g. the title "EVE burst error") stays
     * above the menu that follows it. */
    if (ctx->text_append && ctx->window[0]) {
        size_t n = strlen((const char *)ctx->window);
        memcpy(menu, ctx->window, n);
        pos = n;
        menu[pos++] = '\n';
    }
    for (int i = 0; i < ctx->choice_count; i++) {
        const char *marker = (i == ctx->select_idx) ? SJIS_ARROW : SJIS_SPACE;
        memcpy(menu + pos, marker, 2); pos += 2;
        size_t len = strlen((const char *)ctx->choice_text[i]);
        memcpy(menu + pos, ctx->choice_text[i], len); pos += len;
        if (i + 1 < ctx->choice_count) menu[pos++] = '\n';
    }
    menu[pos] = '\0';
    update_text_texture(menu);
}

void vm_begin_choice(vm_context_t *ctx, const uint8_t *const *labels,
                     const uint32_t *targets, int count) {
    if (count > VM_MAX_CHOICES) count = VM_MAX_CHOICES;
    if (count <= 0) return;

    ctx->choice_count = count;
    ctx->select_idx = 0;
    for (int i = 0; i < count; i++) {
        strncpy((char *)ctx->choice_text[i], (const char *)labels[i], VM_CHOICE_TEXT_LEN - 1);
        ctx->choice_text[i][VM_CHOICE_TEXT_LEN - 1] = '\0';
        ctx->choice_ips[i] = targets[i];
    }
    ctx->is_choosing = 1;
    ctx->is_waiting = 0;
    ctx->wait_timer = 0;
    vm_render_choice(ctx);
}

int vm_is_choosing(const vm_context_t *ctx) {
    return ctx->is_choosing;
}

/* Evaluate k (var,val) pairs starting at byte offset off. Returns 1 if all match. */
static int vm_conditions_hold(const vm_context_t *ctx, uint32_t off, uint32_t k) {
    for (uint32_t i = 0; i < k; i++) {
        uint32_t var = rd16(ctx, off + i * 4);
        uint32_t val = rd16(ctx, off + i * 4 + 2);
        if (var >= VM_NUM_VARS) return 0;
        if (ctx->vars[var] != val) return 0;
    }
    return 1;
}

/* OP_MENU: 80 02 N { k, (var,val) x k, text_addr, target } x N
 * Returns the byte length of the instruction and fills the menu. */
static uint32_t vm_exec_menu(vm_context_t *ctx) {
    uint32_t off = ctx->ip;
    uint32_t entries = 0;

    static uint8_t labels[VM_MAX_CHOICES][VM_CHOICE_TEXT_LEN];
    const uint8_t *label_ptrs[VM_MAX_CHOICES];
    uint32_t targets[VM_MAX_CHOICES];
    int visible = 0;

    /* One selection screen can be several 8002 instructions in a row: a
     * location menu is followed by its "セーブ / ロード" menu (489 of 2237
     * menus). They are merged into one list. */
    uint32_t all_targets[64];
    int n_targets = 0;
    while (off + 3 < ctx->script_size && rd16(ctx, off) == 0x8002) {
        /* A following 8002 that is itself the target of an item above is a
         * sub-menu (the title's 「オールクリア」 confirmation), not part of
         * this screen: stop merging there. */
        int is_target = 0;
        for (int t = 0; t < n_targets; t++) if (all_targets[t] == off) is_target = 1;
        if (is_target) break;
        off += 2;
        uint32_t n = rd16(ctx, off); off += 2;
        entries += n;
        for (uint32_t e = 0; e < n && off + 1 < ctx->script_size; e++) {
            uint32_t k = rd16(ctx, off); off += 2;
            int ok = vm_conditions_hold(ctx, off, k);
            off += k * 4;
            uint32_t text_addr = rd16(ctx, off) * 2; off += 2;
            uint32_t target    = rd16(ctx, off) * 2; off += 2;
            if (n_targets < 64) all_targets[n_targets++] = target;
            if (ok && visible < VM_MAX_CHOICES) {
                vm_copy_string(ctx, text_addr, labels[visible], VM_CHOICE_TEXT_LEN, NULL);
                label_ptrs[visible] = labels[visible];
                targets[visible] = target;
                visible++;
            }
        }
    }
    /* A menu that lies before the command menu we came from belongs to another
     * part of the script: forget the stale return stack. */
    if (ctx->menu_sp > 0 && ctx->ip < ctx->menu_stack_ip[0]) ctx->menu_sp = 0;
    ctx->menu_group_ip = ctx->ip;
    ctx->menu_armed = 0;
    if (visible > 0) {
        vm_begin_choice(ctx, label_ptrs, targets, visible);
        ctx->menu_armed = 1;
    }
#ifndef NDEBUG
    printf("VM: menu at %lu with %lu entries, %d visible\n",
           (unsigned long)ctx->ip, (unsigned long)entries, visible);
#endif
    return off - ctx->ip;
}

void vm_redraw(vm_context_t *ctx) {
    if (ctx->is_choosing) {
        vm_render_choice(ctx);
    } else if (ctx->window[0]) {
        update_text_texture(ctx->window);
    }
}

uint32_t vm_find_block(const vm_context_t *ctx, uint32_t id) {
    for (uint32_t ip = 0; ip + 3 < ctx->script_size; ip += 2) {
        if (rd16(ctx, ip) == 0x8001 && rd16(ctx, ip + 2) == id) return ip;
    }
    return 0;
}

/* ---- Execution ---------------------------------------------------------- */

int vm_step(vm_context_t *ctx) {
    if (!ctx->script_data || ctx->ip + 1 >= ctx->script_size) return VM_END;

    if (ctx->is_waiting) {
        if (ctx->next_voice[0]) {
            /* Give the first voice ~0.25 s to reach the streaming state before
             * polling for its end, then chain the second file. */
            if (++ctx->voice_wait > 15 && !audio_voice_playing()) {
                char token[VM_NAME_LEN];
                strcpy(token, ctx->next_voice);
                ctx->next_voice[0] = '\0';
                if (audio_is_voice_token(token)) audio_play_voice(token);
            }
        }
        if (ctx->key_lock > 0) ctx->key_lock--;
        if (ctx->wait_timer > 0) {
            ctx->wait_timer--;
            if (ctx->wait_timer == 0) {
                ctx->is_waiting = 0; /* Timer finished, stop waiting */
            }
        }
        /* If wait_timer is 0, wait indefinitely for A button input (cleared in vm_handle_input) */
        return VM_RUNNING;
    }

    if (ctx->is_choosing) {
        /* Selection is resolved in vm_handle_input(); just wait. */
        return VM_RUNNING;
    }

    if (ctx->delay_frames > 0) {
        ctx->delay_frames--;
        return VM_RUNNING;
    }

    /* Guard against runaway loops in one frame */
    int budget = 4096;

    while (ctx->ip + 1 < ctx->script_size && budget-- > 0) {
        uint32_t op = rd16(ctx, ctx->ip);

        if (!is_opcode(op)) {
            /* Stray data word (or we ran into the text segment) */
            ctx->ip += 2;
            continue;
        }

        uint8_t code = op & 0xFF;
        uint32_t a1 = rd16(ctx, ctx->ip + 2);
        uint32_t a2 = rd16(ctx, ctx->ip + 4);
        uint32_t a3 = rd16(ctx, ctx->ip + 6);

        switch (code) {
        case OP_TEXT: {
            uint32_t addr = a1 * 2;
            /* a2 = number of voice tokens that follow the text (0, 1 or 2) */
            uint32_t len  = 6 + a2 * 2;
            static uint8_t safe_text[256];
            text_code_t tc;
            if (!vm_copy_string(ctx, addr, safe_text, sizeof(safe_text), &tc)) {
                printf("VM: text address %lu out of range (size %lu)\n",
                       (unsigned long)addr, (unsigned long)ctx->script_size);
                ctx->ip += len;
                break;
            }
            ctx->ip += len;
            if (safe_text[0] == '\0') {
                if (tc.newline) {
                    /* A bare "n": clear the message window, no wait. */
                    ctx->window[0] = '\0';
                    ctx->window_rows = 0;
                    ctx->text_append = 0;
                    update_text_texture(ctx->window);
                }
                break;
            }

            vm_window_put(ctx, safe_text);
            ctx->text_append = tc.newline;
            ctx->next_voice[0] = '\0';
            ctx->voice_wait = 0;
            if (a2) {
                char token[VM_NAME_LEN];
                vm_copy_name(ctx, a3 * 2, token, sizeof(token));
                if (audio_is_voice_token(token)) audio_play_voice(token);
                if (a2 >= 2) {
                    /* A long line is split over two voice files (e.g. kyo0221a /
                     * kyo0221b). The second starts when the first ends (vm_step). */
                    vm_copy_name(ctx, rd16(ctx, ctx->ip - len + 8) * 2,
                                 ctx->next_voice, sizeof(ctx->next_voice));
                }
            }
            ctx->last_text_addr = addr;
            if (tc.newline) break;             /* 'n': no wait, next line follows at once */
            ctx->is_waiting = 1;
            ctx->wait_timer = (tc.wait_frames && !tc.need_key) ? tc.wait_frames : 0;
            ctx->key_lock   = (tc.wait_frames && tc.need_key) ? tc.wait_frames : 0;
            return VM_WAIT;
        }

        case OP_MENU: {
            uint32_t len = vm_exec_menu(ctx);
            ctx->ip += len;
            if (ctx->is_choosing) return VM_WAIT;
            break;
        }

        case OP_BRANCH: {
            uint32_t k = a1;
            uint32_t len = 4 + k * 4 + 2;
            if (vm_conditions_hold(ctx, ctx->ip + 4, k)) {
                ctx->ip = rd16(ctx, ctx->ip + 4 + k * 4) * 2;
            } else {
                ctx->ip += len;
            }
            break;
        }

        case OP_GOTO:
            ctx->ip = a1 * 2;
            break;

        case OP_SETVAR:
            if (a1 < VM_NUM_VARS) ctx->vars[a1] = (uint16_t)a2;
            ctx->ip += 6;
            break;

        case OP_BG:
            vm_copy_name(ctx, a1 * 2, ctx->bg_name, sizeof(ctx->bg_name));
            vm_on_background(ctx->bg_name);
            ctx->ip += 4;
            break;

        case OP_SPRITE: {
            char name[VM_NAME_LEN];
            vm_copy_name(ctx, a1 * 2, name, sizeof(name));
            vm_on_sprite(name, (int)a2, (int)a3);
            ctx->ip += 8;
            break;
        }

        case OP_WAIT:
            ctx->delay_frames = (int)a1;
            ctx->ip += 4;
            return VM_RUNNING;

        case OP_FADEOUT: {
            /* 8012 1 precedes almost every BG change: darken the screen so the
             * background swap happens behind black. 8012 0 = hard cut. */
            int frames = a1 ? 12 : 0;
            update_text_texture((const uint8_t *)"");
            vm_on_fade(1, frames);
            ctx->delay_frames = frames;
            ctx->ip += 4;
            return VM_RUNNING;
        }

        case OP_FADEIN: {
            /* 8013 follows the BG/overlay setup. 1 = fade in with the speed from
             * 8025 (3..6 -> 12..24 frames), 0 = show immediately. */
            int frames = a1 ? (ctx->trans_speed > 0 ? ctx->trans_speed * 4 : 12) : 0;
            vm_on_fade(0, frames);
            ctx->delay_frames = frames;
            ctx->ip += 4;
            return VM_RUNNING;
        }

        case OP_TRANS:
            ctx->trans_speed = (int)a1;
            ctx->ip += 4;
            break;

        case OP_POSTWAIT:   /* 8026 n = BGM track n (see vm.h) */
            vm_on_bgm(code, (int)a1);
            ctx->ip += 4;
            break;

        case OP_MENU_END:
            ctx->ip += 2;
            /* End of a menu item: back to the location's command menu, i.e. the
             * outermost menu on the stack. Sub-menus (「もちもの」, 「使う」,
             * confirmations such as 「ぐっすりと」) have no "back" entry, so
             * returning to the innermost menu would trap the player there. */
            if (ctx->menu_sp > 0) {
                uint32_t mip = ctx->menu_stack_ip[0];
                ctx->menu_sp = 0;
                if (mip < ctx->ip) ctx->ip = mip;
            }
            break;

        case OP_SAVE:
            ctx->ip += 2;
            return VM_SAVE;

        case OP_LOAD:
            ctx->ip += 2;
            return VM_LOAD;

        case OP_DISC:
            /* 803a n: the Saturn asked for disc n here (A01 = 1, B01 = 2, and
             * right after the "1206" eyecatch in A001_4 / B022_2 = 3). */
            ctx->disc_req = (int)a1;
            ctx->ip += 4;
#ifndef NDEBUG
            printf("VM: disc %d required\n", ctx->disc_req);
#endif
            return VM_DISC;

        case OP_ROUTE:
            ctx->route = (int)a1;
#ifndef NDEBUG
            printf("VM: route %d\n", ctx->route);
#endif
            ctx->ip += 4;
            break;

        case OP_SCRIPT:
            vm_copy_name(ctx, a1 * 2, ctx->pending_script, sizeof(ctx->pending_script));
#ifndef NDEBUG
            printf("VM: chain to script '%s'\n", ctx->pending_script);
#endif
            ctx->ip += 4;
            return VM_CHAIN;

        case OP_SE_A:
        case OP_SE_B: {
            char token[VM_NAME_LEN];
            vm_copy_name(ctx, a1 * 2, token, sizeof(token));
            if (audio_is_voice_token(token)) audio_play_voice(token);
            ctx->ip += 4;
            break;
        }

        case OP_MOVIE: {
            char name[VM_NAME_LEN];
            vm_copy_name(ctx, a1 * 2, name, sizeof(name));
            ctx->ip += 4;
            vm_on_movie(name);
            return VM_RUNNING;
        }

        case OP_TERM_MODE:
            vm_on_terminal(code, (int)a1, NULL);
            ctx->ip += 4;
            break;

        case OP_TERM_CLR:
            vm_on_terminal(code, 0, NULL);
            ctx->ip += 2;
            break;

        case OP_TERM_TEXT: {
            static uint8_t term_text[512];
            vm_copy_raw(ctx, a1 * 2, term_text, sizeof(term_text));
            vm_on_terminal(code, 0, (const char *)term_text);
            ctx->ip += 4;
            break;
        }

        case OP_BGM_A:
        case OP_BGM_B:
            vm_on_bgm(code, (int)a1);
            ctx->ip += 4;
            break;

        case OP_TERM_CUR:   /* 801f y x y2 x2: typing caret (search/quiz screens) */
            vm_on_terminal_cursor((int)a1, (int)a2);
            ctx->ip += 10;
            break;

        case OP_PALETTE:
            vm_on_palette((int)a1, (int)a2, (int)a3, (int)rd16(ctx, ctx->ip + 8));
            ctx->ip += 10;
            break;

        case OP_TEXTPOS:
            vm_on_text_pos((int)a1, (int)a2);
            ctx->ip += 6;
            break;

        case OP_TEXTCOL:
            vm_on_text_color((int)a1);
            ctx->ip += 6;
            break;

        default: {
            int n = vm_fixed_operands(code);
            if (n >= 0) {
                ctx->ip += 2 + n * 2;
            } else {
                /* Unknown opcode: skip to the next 0x80xx word */
#ifndef NDEBUG
                printf("VM: unknown opcode 80%02x at %lu\n", code, (unsigned long)ctx->ip);
#endif
                ctx->ip += 2;
                while (ctx->ip + 1 < ctx->script_size && !is_opcode(rd16(ctx, ctx->ip))) ctx->ip += 2;
            }
            break;
        }
        }
    }
    return (ctx->ip + 1 >= ctx->script_size) ? VM_END : VM_RUNNING;
}

void vm_handle_input(vm_context_t *ctx, uint32_t buttons) {
    if (ctx->is_waiting) {
        if ((buttons & CONT_A) && ctx->key_lock <= 0) {
            ctx->is_waiting = 0;
            ctx->next_voice[0] = '\0';  /* skipped line: drop its second voice */
        }
    }
    else if (ctx->is_choosing) {
        int prev = ctx->select_idx;
        if (buttons & CONT_DPAD_UP) {
            ctx->select_idx = (ctx->select_idx > 0) ? ctx->select_idx - 1 : ctx->choice_count - 1;
        }
        if (buttons & CONT_DPAD_DOWN) {
            ctx->select_idx = (ctx->select_idx < ctx->choice_count - 1) ? ctx->select_idx + 1 : 0;
        }
        if (ctx->select_idx != prev) {
            vm_render_choice(ctx);
        }
        if (buttons & CONT_A) {
            if (ctx->menu_armed && ctx->menu_sp < VM_MENU_STACK) {
                ctx->menu_stack_ip[ctx->menu_sp] = ctx->menu_group_ip;
                ctx->menu_stack_items[ctx->menu_sp] = (uint8_t)ctx->choice_count;
                ctx->menu_sp++;
            }
            ctx->menu_armed = 0;
            ctx->ip = ctx->choice_ips[ctx->select_idx];
            ctx->is_choosing = 0;
            ctx->last_text_addr = 0;
        }
    }
}