/* LibADX for KallistiOS ##version##
 *
 * Direct Access to Sound Driver
 *
 * Copyright (C) 2011-2013 Josh 'PH3NOM' Pearson
 * Copyright (C) 2024-2025 The KOS Team and contributors
 *
 * This code was contributed to KallistiOS (KOS) by Mickaël Cardoso (SiZiOUS).
 * It was originally made by Josh Pearson (PH3NOM). Some portions of code were
 * made by BERO. Sightly improved by Headshotnoby and Ian Micheal.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*    
    To anyone looking at this code:

    This driver runs in its own thread on the SH4.

    When the AICA driver requests more samples,
    it will signal sndbuf_status=SNDDRV_STATUS_NEEDBUF 
    and assign the number of requested samples to snddrv.pcm_needed.

    The decoders need to check sndbuf_status,
    when more samples are requested by the driver the decoders will loop
    decoding into pcm_buffer untill pcm_bytes==snddrv.pcm_needed
    at that point the decoder signals sndbuf_status=SNDDRV_STATUS_HAVEBUF.
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include <kos/thread.h>
#include <dc/sound/stream.h>

#include "snddrv.h"

snd_stream_hnd_t shnd;
kthread_t * snddrv_thd;
static int snddrv_vol = 255;

struct snddrv snddrv;
struct snddrv_song_info snd_sinfo;

/* Increase the Sound Driver volume */
int snddrv_volume_up() {
    
    if( snddrv_vol <= 245 ) {
        snddrv_vol += 10;
  	    snd_stream_volume(shnd, snddrv_vol);
    }
    return snddrv_vol;
}

/* Decrease the Sound Driver volume */
int snddrv_volume_down() {
    
    if( snddrv_vol >= 10 ) {
        snddrv_vol -= 10;
  	    snd_stream_volume(shnd, snddrv_vol);
    }
    return snddrv_vol;
}

/* Exit the Sound Driver */
int snddrv_exit() {
    
    if( snddrv.drv_status != SNDDRV_STATUS_NULL ) {  
        snddrv.drv_status = SNDDRV_STATUS_DONE;
        snddrv.buf_status = SNDDRV_STATUS_BUFEND;
       
        while( snddrv.drv_status != SNDDRV_STATUS_NULL ) 
          thd_sleep(1);   /* EVE-DC: yield for a tick instead of spinning (starved the CD reads) */
          
          
        printf("SNDDRV: Exited\n");   
    }
          
    memset( snddrv.pcm_buffer, 0, sizeof(snddrv.pcm_buffer) );
    snddrv.pcm_bytes = 0;
    snddrv.pcm_needed = 0;
    
    SNDDRV_FREE_STRUCT();
    
    return snddrv.drv_status;
}   

/* EVE-DC ring buffer */
static unsigned char ring_buf[SNDDRV_RING_SIZE];
static volatile int ring_rd = 0, ring_wr = 0;   /* byte indices, rd == wr means empty */

int snddrv_ring_avail(void) { return (ring_wr - ring_rd + SNDDRV_RING_SIZE) % SNDDRV_RING_SIZE; }
int snddrv_ring_free(void)  { return SNDDRV_RING_SIZE - 1 - snddrv_ring_avail(); }
void snddrv_ring_reset(void) { ring_rd = ring_wr = 0; }
void snddrv_ring_write(const unsigned char *src, int n) {
    int wr = ring_wr;
    while (n > 0) {
        int chunk = SNDDRV_RING_SIZE - wr; if (chunk > n) chunk = n;
        memcpy(ring_buf + wr, src, chunk);
        src += chunk; n -= chunk; wr = (wr + chunk) % SNDDRV_RING_SIZE;
    }
    ring_wr = wr;
}
void snddrv_ring_read(unsigned char *dst, int n) {
    int rd = ring_rd;
    while (n > 0) {
        int chunk = SNDDRV_RING_SIZE - rd; if (chunk > n) chunk = n;
        memcpy(dst, ring_buf + rd, chunk);
        dst += chunk; n -= chunk; rd = (rd + chunk) % SNDDRV_RING_SIZE;
    }
    ring_rd = rd;
}

/* Give the AICA `len` bytes: whatever the decoder has queued, silence for the rest. */
static void *snddrv_callback(snd_stream_hnd_t hnd, int len, int * actual) {
    (void)hnd;
    unsigned char *dst = (unsigned char *)snddrv.pcm_buffer;
    int avail = snddrv_ring_avail();
    int n = avail < len ? avail : len;
    if (n > 0) snddrv_ring_read(dst, n);
    if (n < len) memset(dst + n, 0, len - n);
    snddrv.pcm_needed = len;
    snddrv.pcm_bytes = n;
    snddrv.buf_status = SNDDRV_STATUS_HAVEBUF;
    *actual = len;
    return snddrv.pcm_buffer;
}

static void* snddrv_thread(void* __attribute__((unused)) arg) {

    printf("SNDDRV: Rate - %i, Channels - %i\n", snddrv.rate, snddrv.channels);
    
	shnd = snd_stream_alloc(snddrv_callback, SND_STREAM_BUFFER_MAX/4);
	
    snd_stream_start(shnd, snddrv.rate, snddrv.channels-1);
    snddrv.drv_status = SNDDRV_STATUS_STREAMING;

	while( snddrv.drv_status != SNDDRV_STATUS_DONE && snddrv.drv_status != SNDDRV_STATUS_ERROR ) {
           
		snd_stream_poll(shnd);		
		thd_sleep(20);
		
	}
    snddrv.drv_status = SNDDRV_STATUS_NULL;

    snd_stream_destroy(shnd);
    /* EVE-DC: keep the stream subsystem alive, the BGM stream (src/bgm.c) shares it */

    printf("SNDDRV: Finished\n");
	return NULL;

}

/* Start the AICA Sound Stream Thread */
int snddrv_start( int rate, int chans ) {
    
    snddrv.rate = rate;
    snddrv.channels = chans;
    if( snddrv.channels > 2) {
        printf("SNDDRV: ERROR - Exceeds maximum channels\n");
        return -1;
    }

    printf("SNDDRV: Creating Driver Thread\n");

    snddrv.drv_status = SNDDRV_STATUS_INITIALIZING;
    snddrv_ring_reset();

    snd_stream_init();
        
    snddrv_thd = thd_create( true, snddrv_thread, NULL );
    
    return snddrv.drv_status;

}

