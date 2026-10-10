#include "audio.h"
#include <kos.h>
#include <adx.h>
#include <snddrv.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static int audio_ready = 0;
static int current_disc = 1;

/* Token prefixes used by the scenario files for voices (speaker code + number,
 * e.g. gul0093) and sound effects (j317, se04_a, m601). Both kinds are
 * stored as /cd/audio/DiscN/<TOKEN>.adx, so one lookup serves both. */
static const char *const VOICE_PREFIXES[] = {
    "aka", "aqu", "boon", "dep", "eve", "goo", "gul", "hon", "kag", "kat",
    "koe", "koj", "kou", "kyo", "mar", "mat", "may", "mid", "nik", "pul",
    "sir", "yay", "m", "j", "se", NULL
};

void audio_init(int disc) {
    current_disc = (disc >= 1 && disc <= 3) ? disc : 1;
    audio_ready = 1;
}

int audio_is_voice_token(const char *token) {
    if (!token || !token[0]) return 0;

    /* Split into leading letters and trailing digits */
    int letters = 0;
    while (token[letters] >= 'a' && token[letters] <= 'z') letters++;
    if (letters == 0 || letters > 4) return 0;

    int digits = 0;
    while (isdigit((unsigned char)token[letters + digits])) digits++;
    if (digits < 2) return 0;

    /* Anything after the digits must be [a-z0-9_] (e.g. "_a" suffix) */
    for (const char *p = token + letters + digits; *p; p++) {
        if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_')) return 0;
    }

    for (int i = 0; VOICE_PREFIXES[i]; i++) {
        if ((int)strlen(VOICE_PREFIXES[i]) == letters &&
            strncmp(VOICE_PREFIXES[i], token, letters) == 0) {
            return 1;
        }
    }
    return 0;
}

int audio_voice_playing(void) {
    if (!audio_ready) return 0;
    switch (snddrv.dec_status) {
        case SNDDEC_STATUS_INITIALIZING:
        case SNDDEC_STATUS_READY:
        case SNDDEC_STATUS_STREAMING:
        case SNDDEC_STATUS_PAUSING:
        case SNDDEC_STATUS_PAUSED:
        case SNDDEC_STATUS_RESUMING:
            return 1;
        default:
            return 0;
    }
}

void audio_stop_voice(void) {
    if (!audio_ready) return;
    /* adx_stop() only acts while streaming; it is harmless otherwise. */
    adx_stop();
}

int audio_play_voice(const char *token) {
    if (!audio_ready || !token || !token[0]) return 0;

    char name[32];
    int i;
    for (i = 0; token[i] && i < (int)sizeof(name) - 1; i++) {
        name[i] = (char)toupper((unsigned char)token[i]);
    }
    name[i] = '\0';

    char path[80];
    snprintf(path, sizeof(path), "/cd/audio/Disc%d/%s.adx", current_disc, name);

    /* Cheap existence check so a missing file does not cost the retry loop below. */
    FILE *f = fopen(path, "rb");
    if (!f) {
#ifndef NDEBUG
        printf("audio: voice file not found: %s\n", path);
#endif
        return 0;
    }
    fclose(f);

    /* libADX refuses to start while its decoder thread is alive.  adx_stop()
     * returns immediately if the thread is not yet in STREAMING state, so
     * retry a few times to cover the start-up / drain windows. */
    for (int tries = 0; tries < 30; tries++) {
        adx_stop();
        if (adx_dec(path, 0)) {
#ifndef NDEBUG
            printf("audio: playing %s\n", path);
#endif
            return 1;
        }
        thd_sleep(10);
    }
#ifndef NDEBUG
    printf("audio: decoder busy, gave up on %s\n", path);
#endif
    return 0;
}
