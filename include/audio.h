#ifndef AUDIO_H
#define AUDIO_H

/* Voice playback via libADX (kos-ports).
 * Files are looked up as /cd/audio/Disc<N>/<TOKEN>.adx where TOKEN is the
 * upper-cased resource token found in the scenario text segment
 * (e.g. "gul0093" -> GUL0093.adx). */

/* Select which disc directory voice files are read from (1..3). */
void audio_init(int disc);

/* Returns non-zero if the string looks like a voice token
 * (known speaker prefix followed by digits, e.g. "yay1578"). */
int audio_is_voice_token(const char *token);

/* Stop the current voice (if any) and start the given one.
 * Returns 1 on success, 0 if the file is missing or the decoder is busy. */
int audio_play_voice(const char *token);

/* Stop any voice currently streaming. Safe to call when idle. */
void audio_stop_voice(void);

/* Non-zero while a voice started by audio_play_voice() is still decoding. */
int audio_voice_playing(void);

#endif /* AUDIO_H */
