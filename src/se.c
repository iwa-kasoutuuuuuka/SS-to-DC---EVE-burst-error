#include "se.h"
#include <kos.h>
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>
#include <stdio.h>
#include <string.h>

#define SE_COUNT 9
static const char *se_names[SE_COUNT] = { "MOVE", "SET", "CANCEL", "OPEN", "SCROLL", "ERROR", "SPECIAL", "TITLE", "TEST" };
static sfxhnd_t se_hnd[SE_COUNT];
static int se_loaded = 0;

void se_init(void) {
    int n = 0;
    snd_init();   /* snd_sfx_load needs the sound driver and sound-RAM allocator up; idempotent, and
                   * nothing else initialises sound before the first voice/BGM stream starts */
    for (int i = 0; i < SE_COUNT; i++) {
        char path[48];
        snprintf(path, sizeof(path), "/cd/se/%s.wav", se_names[i]);
        se_hnd[i] = snd_sfx_load(path);
        if (se_hnd[i] != SFXHND_INVALID) n++;
    }
    se_loaded = 1;
#ifndef NDEBUG
    printf("SE: %d/%d UI sounds loaded\n", n, SE_COUNT);
#endif
}

void se_play(const char *name) {
    if (!se_loaded || !name) return;
    for (int i = 0; i < SE_COUNT; i++) {
        if (!strcmp(name, se_names[i])) {
            if (se_hnd[i] != SFXHND_INVALID) snd_sfx_play(se_hnd[i], 200, 128);
            return;
        }
    }
}

void se_shutdown(void) {
    if (!se_loaded) return;
    for (int i = 0; i < SE_COUNT; i++) {
        if (se_hnd[i] != SFXHND_INVALID) { snd_sfx_unload(se_hnd[i]); se_hnd[i] = SFXHND_INVALID; }
    }
    se_loaded = 0;
}
