#ifndef SE_H
#define SE_H

/* UI sound effects (the MISC WAV files of the Saturn disc: MOVE, SET, CANCEL, OPEN,
 * SCROLL, ERROR, SPECIAL, TITLE, TEST). They are 6-bit log DPCM on the disc
 * (decoded by tools/satvoice.py) and shipped as small AICA
 * ADPCM WAVs as /cd/se/NAME.wav, loaded into sound RAM once with snd_sfx so
 * they play instantly and independently of the voice and BGM streams. */

void se_init(void);                 /* load every UI sound from /cd/se (after snd_init) */
void se_play(const char *name);     /* "MOVE", "SET", "CANCEL", ... ; unknown names are ignored */
void se_shutdown(void);             /* unload (call before a movie, which resets the sound driver) */

#endif /* SE_H */
