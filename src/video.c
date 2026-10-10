#include "video.h"
#include "renderer.h"
#include <mpeg.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/sound/stream.h>
#include <dc/sound/sound.h>
#include <stdio.h>
#include <string.h>

/* Play an MPEG-1 file (converted from the Saturn Cinepak .CPK movies by
 * ffmpeg, see eve-dc-asset-pipeline). Blocks until the movie ends or A/Start
 * is pressed. Returns 0 on success, -1 if the file is missing or the player
 * could not be created. The caller must stop any libADX stream first because
 * libmpeg uses the same snd_stream driver. */
int video_play(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        printf("Video: %s not found\n", filename);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fclose(f);
    if (fsize <= 0) {
        printf("Video: %s is empty\n", filename);
        return -1;
    }

    snd_stream_init();   /* idempotent; libmpeg expects the stream driver to be up */
    mpeg_player_t *player = mpeg_player_create(filename);
    if (!player) {
        printf("Video: failed to create player for %s\n", filename);
        return -1;
    }
    mpeg_player_set_loop(player, 0);
#ifndef NDEBUG
    printf("Video: playing %s (%ld bytes)\n", filename, fsize);
#endif
    int r = mpeg_play(player, CONT_A | CONT_START);
    (void)r;   /* only reported in debug builds */
    mpeg_player_destroy(player);

    /* After libmpeg the AICA stream callbacks never fire again: the next libADX
     * voice blocks forever in adx_stop()/NEEDBUF (A001 hangs at its 76th line)
     * or faults. Reload the whole sound driver; nothing else is streaming here
     * (voices and BGM are stopped by vm_on_movie before the movie starts). */
    snd_stream_shutdown();
    snd_shutdown();
    snd_init();
    snd_stream_init();

    /* libmpeg drives the PVR itself; restore the texture stride our 512x256
     * non-twiddled textures rely on before the main loop draws again. */
    pvr_txr_set_stride(512);
#ifndef NDEBUG
    printf("Video: finished %s (%d)\n", filename, r);
#endif
    return 0;
}