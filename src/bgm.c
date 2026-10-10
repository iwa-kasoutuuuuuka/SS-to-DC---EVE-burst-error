#include "bgm.h"
#include <kos.h>
#include <dc/sound/stream.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* BGM is streamed from RAM through its own KOS sound stream (libADX voices use
 * another one; KOS allows four). The whole AICA-ADPCM data chunk of
 * /cd/bgm/BGM<n>.wav is loaded into main RAM first so that no CD access
 * happens while streaming. The AICA cannot hardware-loop samples longer than
 * 64K samples, which is why snd_sfx cannot be used for music.
 *
 * The stream is polled from the main loop (bgm_update(), once per frame), not
 * from a thread: with a polling thread the main thread hung as soon as it
 * blocked (CD read of the next track, thd_join) while a libADX voice ended at
 * the same time (A001_4, BGM10 -> BGM42 on Flycast). The ADPCM buffer holds
 * several seconds, so the main loop's own CD reads do not starve it. */

static uint8_t *bgm_data = NULL;      /* ADPCM data chunk */
static size_t   bgm_size = 0;
static size_t   bgm_pos = 0;
static uint32_t bgm_rate = 22050;
static int      bgm_track = 0;
static int      bgm_running = 0;
static snd_stream_hnd_t bgm_hnd = SND_STREAM_INVALID;
static uint8_t  bgm_chunk[SND_STREAM_BUFFER_MAX_ADPCM] __attribute__((aligned(32)));
#define BGM_VOLUME 170                 /* below the voices (255) */
static int bgm_fade_frames = 0;        /* >0: fade-out in progress (frames left) */
static int bgm_fade_total = 0;
static int bgm_muted = 0;
static int bgm_cur_vol = -1;

void bgm_init(void) {
    bgm_data = NULL; bgm_size = bgm_pos = 0; bgm_track = 0; bgm_running = 0;
    bgm_hnd = SND_STREAM_INVALID;
    bgm_fade_frames = bgm_fade_total = 0; bgm_muted = 0; bgm_cur_vol = -1;
}

/* Stream callback: hand back req bytes, wrapping around at the end (loop). */
static void *bgm_callback(snd_stream_hnd_t hnd, int req, int *actual) {
    (void)hnd;
    if (!bgm_data || bgm_size == 0 || req <= 0) { *actual = 0; return NULL; }
    if ((size_t)req > sizeof(bgm_chunk)) req = (int)sizeof(bgm_chunk);
    size_t done = 0;
    while (done < (size_t)req) {
        size_t n = bgm_size - bgm_pos;
        if (n > (size_t)req - done) n = (size_t)req - done;
        memcpy(bgm_chunk + done, bgm_data + bgm_pos, n);
        done += n;
        bgm_pos += n;
        if (bgm_pos >= bgm_size) bgm_pos = 0;   /* loop */
    }
    *actual = req;
    return bgm_chunk;
}

void bgm_stop(void) {
    bgm_fade_frames = bgm_fade_total = 0;
    if (bgm_running) {
        bgm_running = 0;
#ifndef NDEBUG
        printf("BGM: stop stream\n");
#endif
        snd_stream_stop(bgm_hnd);
#ifndef NDEBUG
        printf("BGM: destroy stream\n");
#endif
        snd_stream_destroy(bgm_hnd);
#ifndef NDEBUG
        printf("BGM: stream destroyed\n");
#endif
        bgm_hnd = SND_STREAM_INVALID;
    }
    if (bgm_data) { free(bgm_data); bgm_data = NULL; }
    bgm_size = bgm_pos = 0;
    bgm_track = 0;
    bgm_cur_vol = -1;
}

/* Minimal RIFF/WAVE reader: returns the data chunk (malloc) and the sample rate. */
static uint8_t *load_wav_adpcm(const char *path, size_t *size_out, uint32_t *rate_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) { fclose(f); return NULL; }
    uint8_t *data = NULL; size_t size = 0; uint32_t rate = 22050; int fmt = 0;
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) break;
        uint32_t len = ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((uint32_t)ch[7] << 24);
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t fm[16];
            if (len < 16 || fread(fm, 1, 16, f) != 16) break;
            fmt = fm[0] | (fm[1] << 8);
            rate = fm[4] | (fm[5] << 8) | (fm[6] << 16) | ((uint32_t)fm[7] << 24);
            fseek(f, (long)(len - 16 + (len & 1)), SEEK_CUR);
        } else if (!memcmp(ch, "data", 4)) {
            data = (uint8_t *)memalign(32, len);
            if (!data) break;
            /* Read in 32 KB slices and yield between them: a single ~1 MB read from
             * the main thread while a libADX voice streams (its thread also reads
             * the CD) hung on Flycast; giving the other threads a turn avoids it. */
            size_t got = 0;
            while (got < len) {
                size_t want = len - got; if (want > 32768) want = 32768;
                size_t r = fread(data + got, 1, want, f);
                if (r == 0) break;
                got += r;
                thd_sleep(1);
            }
            if (got != len) { free(data); data = NULL; break; }
            size = len;
            break;
        } else {
            fseek(f, (long)(len + (len & 1)), SEEK_CUR);
        }
    }
    fclose(f);
    if (data && fmt != 0x20 && fmt != 0x14) {
        printf("BGM: %s is not AICA ADPCM (fmt %d)\n", path, fmt);
        free(data);
        return NULL;
    }
    *size_out = size; *rate_out = rate;
    return data;
}

int bgm_play(int track) {
    if (track <= 0) { bgm_stop(); return 0; }
    if (track == bgm_track && bgm_running) return 1;

    bgm_stop();   /* silence before the CD read; a short gap at a scene change is fine */
    char path[64];
    snprintf(path, sizeof(path), "/cd/bgm/BGM%d.wav", track);
    size_t size = 0; uint32_t rate = 22050;
#ifndef NDEBUG
    printf("BGM: loading %s\n", path);
#endif
    uint8_t *data = load_wav_adpcm(path, &size, &rate);
#ifndef NDEBUG
    printf("BGM: loaded\n");
#endif
    if (!data) {
#ifndef NDEBUG
        printf("BGM: %s not found\n", path);
#endif
        return 0;
    }
    bgm_data = data; bgm_size = size; bgm_pos = 0; bgm_rate = rate; bgm_track = track;
    bgm_muted = 0;
    snd_stream_init();
    bgm_hnd = snd_stream_alloc(bgm_callback, SND_STREAM_BUFFER_MAX_ADPCM);
    if (bgm_hnd == SND_STREAM_INVALID) {
        printf("BGM: snd_stream_alloc failed\n");
        bgm_stop();
        return 0;
    }
    snd_stream_start_adpcm(bgm_hnd, bgm_rate, 0);
    snd_stream_volume(bgm_hnd, BGM_VOLUME);
    bgm_cur_vol = BGM_VOLUME;
    bgm_running = 1;
#ifndef NDEBUG
    printf("BGM: streaming %s (%lu bytes, %lu Hz)\n", path, (unsigned long)size, (unsigned long)rate);
#endif
    return 1;
}

/* Called once per frame from the main loop: feeds the stream and runs fades. */
void bgm_update(void) {
    if (!bgm_running) return;
    snd_stream_poll(bgm_hnd);
    int vol = bgm_muted ? 0 : BGM_VOLUME;
    if (bgm_fade_frames > 0) {
        vol = vol * bgm_fade_frames / (bgm_fade_total > 0 ? bgm_fade_total : 1);
        bgm_fade_frames--;
        if (bgm_fade_frames == 0) { bgm_stop(); return; }
    }
    if (vol != bgm_cur_vol) { snd_stream_volume(bgm_hnd, vol); bgm_cur_vol = vol; }
}

/* 800f n: fade out over `frames` frames (0 = stop at once), then stop. */
void bgm_fade_out(int frames) {
    if (!bgm_running) return;
    if (frames <= 0) { bgm_stop(); return; }
    bgm_fade_total = frames;
    bgm_fade_frames = frames;
}

void bgm_set_mute(int mute) {
    bgm_muted = mute ? 1 : 0;
}

int bgm_current(void) {
    return bgm_track;
}
