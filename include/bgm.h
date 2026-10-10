#ifndef BGM_H
#define BGM_H

/* Background music. The Saturn BGM is SCSP sequence data we cannot render, so
 * BGM tracks are pre-rendered loops stored as AICA ADPCM WAV files in
 * /cd/bgm/BGM<n>.wav (see convert_bgm.py). They are streamed from RAM through a
 * KOS sound stream polled by the main loop (bgm_update), independent of the
 * libADX voice stream, so dialogue and music play at the same time. */

void bgm_init(void);
/* Start track n (looping). Stops the previous track. Returns 1 if a file was found. */
int  bgm_play(int track);
void bgm_stop(void);
/* Feed the stream and run fades; call once per frame from the main loop. */
void bgm_update(void);
/* 800f n: fade the music out over `frames` frames (0 = stop at once), then stop. */
void bgm_fade_out(int frames);
/* 800e 0 / non-zero: silence or restore the running track without stopping it. */
void bgm_set_mute(int mute);
int  bgm_current(void);   /* track number playing, 0 = none */

#endif /* BGM_H */