/* LibADX for KallistiOS ##version##
 *
 * ADX Decoder Library
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
    ADXCORE (C) 2011-2013 Josh "PH3NOM" Pearson <ph3nom.dcmc@gmail.com>

    Decoder algorithm optimized for stereo streams (C) PH3NOM
    Decoder algorithm: adv2wav (C) 2001 BERO <bero@geocities.co.jp>
    See: http://www.geocities.co.jp/Playtown/2004/

    ADX info from: http://ku-www.ss.titech.ac.jp/~yatsushi/adx.html
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include <kos/thread.h>
#include <arch/timer.h>

#include "adx.h"
#include "snddrv.h"

#define BASEVOL      0x4000
#define PCM_BUF_SIZE (256*1024)   /* EVE-DC: was 1 MB; ~3 s of 44.1 kHz mono is plenty and halves heap churn per voice */

/* A few global vars, should move this into a struct/handle */
static ADX_INFO ADX_Info;
static FILE *adx_in;
/* EVE-DC: generation of the current decode. A decoder thread that outlives a
 * forced adx_stop() (or a newer adx_dec()) sees a different value, leaves its
 * loops, frees its own buffer and must not touch the driver any more. */
static volatile int adx_gen = 0;
#define ADX_ALIVE (snddrv.dec_status != SNDDEC_STATUS_DONE && my_gen == adx_gen)
static int pcm_samples, loop;
static unsigned char adx_buf[ADX_HDR_SIZE_V4];

/* Local function definitions */
static int adx_parse( unsigned char *buf );
static void adx_to_pcm( short *out, unsigned char *in, PREV *prev );
static void* adx_thread( void* __attribute__((unused)) arg );
static void* pause_thd( void* __attribute__((unused)) arg );

static int read_be16(unsigned char *buf)     /* ADX File Format is Big Endian */
{
    return (buf[0]<<8)|buf[1];
}

static long read_be32(unsigned char *buf)
{
    return (buf[0]<<24)|(buf[1]<<16)|(buf[2]<<8)|buf[3];
}

/* Start Straming the ADX in a seperate thread */
int adx_dec( const char * adx_file, int loop_enable )
{
    printf("LibADX: Checking Status\n");
    if( snddrv.dec_status!=SNDDEC_STATUS_NULL )
    {
        printf("LibADX: Already Running in another process!\n");
        return 0; 
    } 

    adx_in = fopen(adx_file,"rb");                   /* Make sure file exists */
    if(adx_in==NULL)
    {
        printf("LibADX: Can't open file %s\n", adx_file);
        return 0;
    }
    
    if(adx_parse( adx_buf ) < 1) /* Make sure we are working with an ADX file */
    {
        printf("LibADX: Invalid File Header\n");
        fclose( adx_in );
        return 0;
    } 

    printf("LibADX: Starting\n");    
    loop = loop_enable;
    adx_gen++;
    thd_create( true, adx_thread, NULL ); 
    
    return 1;    
}

/* Pause Streaming the ADX (if streaming) */
int adx_pause()
{
    if( snddrv.dec_status != SNDDEC_STATUS_STREAMING )
        return 0;

    printf("LibADX: Pausing\n");
    snddrv.dec_status = SNDDEC_STATUS_PAUSING;
    while( snddrv.dec_status != SNDDEC_STATUS_PAUSED )
       thd_sleep(1);   /* EVE-DC: yield for a tick instead of spinning (starved the CD reads) */
    
    thd_create( true, pause_thd, NULL );
       
    return 1;
}

/* Resume Streaming the ADX (if paused) */
int adx_resume()
{
    if( snddrv.dec_status != SNDDEC_STATUS_PAUSED )
        return 0;
        
    printf("LibADX: Resuming\n");
    snddrv.dec_status = SNDDEC_STATUS_RESUMING;
    while( snddrv.dec_status != SNDDEC_STATUS_STREAMING )
       thd_sleep(1);   /* EVE-DC: yield for a tick instead of spinning (starved the CD reads) */
        
    return 1;
}

/* Restart Streaming the ADX */
int adx_restart()
{
    if( snddrv.dec_status != SNDDEC_STATUS_STREAMING )
        return 0;

    printf("LibADX: Restarting\n");
    snddrv.dec_status = SNDDEC_STATUS_PAUSING;
    while( snddrv.dec_status != SNDDEC_STATUS_PAUSED )
       thd_sleep(1);   /* EVE-DC: yield for a tick instead of spinning (starved the CD reads) */
       
    pcm_samples = ADX_Info.samples;
    fseek( adx_in, ADX_Info.sample_offset+ADX_CRI_SIZE, SEEK_SET );
    
    snddrv.dec_status = SNDDEC_STATUS_STREAMING;
    
    return 1;       
}

/* Stop Straming the ADX */
int adx_stop()
{
    if( snddrv.dec_status != SNDDEC_STATUS_STREAMING )
        return 0;

    printf("LibADX: Stopping t=%lu\n", (unsigned long)timer_ms_gettime64());    
    loop=0;     
    snddrv.dec_status = SNDDEC_STATUS_DONE;
    /* EVE-DC: release a driver callback that is spinning for HAVEBUF and never
     * wait forever (the original deadlocked after long CD reads). */
    snddrv.buf_status = SNDDRV_STATUS_BUFEND;
    { int guard = 0;
      /* 1.5 s is plenty with the read-ahead buffer; a thread that is still
       * stuck in a CD read is abandoned safely thanks to adx_gen. */
      while( snddrv.dec_status != SNDDEC_STATUS_NULL && guard++ < 150 ) {
          thd_sleep(10);
#ifndef NDEBUG
          if (guard == 75) printf("LibADX: still waiting: dec=%d buf=%d drv=%d\n", snddrv.dec_status, snddrv.buf_status, snddrv.drv_status);
#endif
      }
      if( snddrv.dec_status != SNDDEC_STATUS_NULL ) {
          printf("LibADX: decoder thread did not exit, forcing driver shutdown\n");
          adx_gen++;                 /* the stuck thread will exit on its own */
          snddrv_exit();
          snddrv.dec_status = SNDDEC_STATUS_NULL;
      }
    }
         
    return 1;
}

/* Read and parse the ADX File header then seek to the begining sample offset */
static int adx_parse( unsigned char *buf )
{
    fseek( adx_in, 0, SEEK_SET );          /* Read the ADX Header into memory */
    fread( buf, 1, ADX_HDR_SIZE_V4, adx_in );
    if(buf[0]!=ADX_HDR_SIG ) return -1;           /* Check ADX File Signature */
    
    /* Parse the ADX File header */
    ADX_Info.sample_offset = read_be16(buf+ADX_ADDR_START)-2;
    ADX_Info.chunk_size    = buf[ADX_ADDR_CHUNK];
    ADX_Info.channels      = buf[ADX_ADDR_CHAN];
    ADX_Info.rate          = read_be32(buf+ADX_ADDR_RATE);
    ADX_Info.samples       = read_be32(buf+ADX_ADDR_SAMP);
    ADX_Info.loop_type     = buf[ADX_ADDR_TYPE]; 
    
    /* Two known variations for possible loop informations: type 3 and type 4 */
    if( ADX_Info.loop_type == 3 )
        ADX_Info.loop = read_be32(buf+ADX_ADDR_LOOP);
    else if( ADX_Info.loop_type == 4 )
        ADX_Info.loop = read_be32(buf+ADX_ADDR_LOOP+0x0c);
    if( ADX_Info.loop > 1 || ADX_Info.loop < 0 )    /* Invalid header check */
        ADX_Info.loop = 0;      
    if( ADX_Info.loop && ADX_Info.loop_type == 3 )
    {
        ADX_Info.loop_samp_start = read_be32(buf+ADX_ADDR_SAMP_START);
        ADX_Info.loop_start      = read_be32(buf+ADX_ADDR_BYTE_START);
        ADX_Info.loop_samp_end   = read_be32(buf+ADX_ADDR_SAMP_END);
        ADX_Info.loop_end        = read_be32(buf+ADX_ADDR_BYTE_END);
    }
    else if( ADX_Info.loop && ADX_Info.loop_type == 4  )
    {
        ADX_Info.loop_samp_start = read_be32(buf+ADX_ADDR_SAMP_START+0x0c);
        ADX_Info.loop_start      = read_be32(buf+ADX_ADDR_BYTE_START+0x0c);
        ADX_Info.loop_samp_end   = read_be32(buf+ADX_ADDR_SAMP_END+0x0c);
        ADX_Info.loop_end        = read_be32(buf+ADX_ADDR_BYTE_END+0x0c);
    }
    if( ADX_Info.loop )
     ADX_Info.loop_samples = ADX_Info.loop_samp_end-ADX_Info.loop_samp_start;
    
    fseek( adx_in, ADX_Info.sample_offset, SEEK_SET ); /* CRI File Signature */
    fread( buf, 1, 6, adx_in );
           
    if ( memcmp(buf,"(c)CRI",6) )
    {
        printf("Invalid ADX header!\n");
        return -1;
    }

    return 1;
}

/* Convert ADX samples to PCM samples */
static void adx_to_pcm(short *out,unsigned char *in,PREV *prev)
{
    int scale = ((in[0]<<8)|(in[1]));
    int i;
    int s0,s1,s2,d;

    in+=2;
    s1 = prev->s1;
    s2 = prev->s2;
    for(i=0;i<16;i++) {
        d = in[i]>>4;
        if (d&8) d-=16;
        s0 = (BASEVOL*d*scale + 0x7298*s1 - 0x3350*s2)>>14;
        if (s0>32767) s0=32767;
        else if (s0<-32768) s0=-32768;
        *out++=s0;
        s2 = s1;
        s1 = s0;

        d = in[i]&15;
        if (d&8) d-=16;
        s0 = (BASEVOL*d*scale + 0x7298*s1 - 0x3350*s2)>>14;
        if (s0>32767) s0=32767;
        else if (s0<-32768) s0=-32768;
        *out++=s0;
        s2 = s1;
        s1 = s0;
    }
    prev->s1 = s1;
    prev->s2 = s2;
}

/* EVE-DC: read-ahead buffer. The original code did one fread() of 18 bytes per
 * 32 samples (about 1400 CD reads per second); every cache miss caused by the
 * main thread reading another file cost 200-800 ms on Flycast, which starved
 * the AICA and made adx_stop() wait out its 3 s guard. One 32 KB read per
 * ~0.75 s of audio keeps the CD mostly idle. */
#define ADX_RBUF_SIZE 32768
typedef struct { unsigned char *buf; int len, pos; long fpos; } adx_rd_t;

static size_t adx_read(FILE *f, adx_rd_t *r, unsigned char *dst, size_t n)
{
    size_t done = 0;
    while (done < n) {
        if (r->pos >= r->len) {
            r->len = (int)fread(r->buf, 1, ADX_RBUF_SIZE, f);
            r->pos = 0;
            if (r->len <= 0) break;
        }
        size_t take = (size_t)(r->len - r->pos);
        if (take > n - done) take = n - done;
        memcpy(dst + done, r->buf + r->pos, take);
        r->pos += (int)take; done += take; r->fpos += (long)take;
    }
    return done;
}

static void adx_seek(FILE *f, adx_rd_t *r, long pos)
{
    fseek(f, pos, SEEK_SET);
    r->fpos = pos; r->len = r->pos = 0;
}

/* EVE-DC: move decoded PCM from pcm_buf into the driver ring; returns bytes moved. */
static int adx_push(unsigned char *pcm_buf, int *pcm_size)
{
    int n = snddrv_ring_free();
    if (n > *pcm_size) n = *pcm_size;
    if (n <= 0) return 0;
    snddrv_ring_write(pcm_buf, n);
    *pcm_size -= n;
    if (*pcm_size > 0) memmove(pcm_buf, pcm_buf + n, *pcm_size);
    return n;
}

/* Decode the ADX in a seperate thread */
static void* adx_thread(void* __attribute__((unused)) arg)
{
    //FILE *my_in;
    //unsigned char buf[ADX_HDR_SIZE];
    unsigned char *pcm_buf=NULL;
    const int my_gen = adx_gen;
    FILE *my_in = adx_in;
    adx_rd_t rd = { NULL, 0, 0, 0 };
    short outbuf[32*2];
    PREV prev[2];
    //ADX_INFO ADX_Info;
    int pcm_size=0,wsize;
        
    printf("LibADX: %ikHz, %i channel\n", ADX_Info.rate, ADX_Info.channels);
    if( ADX_Info.loop && loop )             
        printf("LibADX: Loop Enabled\n");

    pcm_buf = malloc(PCM_BUF_SIZE);                    /* allocate PCM buffer */
    if( pcm_buf == NULL )
        goto exit;
    rd.buf = malloc(ADX_RBUF_SIZE);
    if( rd.buf == NULL ) { free(pcm_buf); goto exit; }
    rd.fpos = ftell(my_in);

    snddrv_start( ADX_Info.rate, ADX_Info.channels );/*Start AICA audio driver*/ 
    snddrv.dec_status = SNDDEC_STATUS_STREAMING;

    prev[0].s1 = 0;
    prev[0].s2 = 0;
    prev[1].s1 = 0;
    prev[1].s2 = 0;

adx_dec:
    pcm_samples = ADX_Info.samples;    
    if (ADX_Info.channels==1)                          /* MONO Decode Routine */
    while(pcm_samples && ADX_ALIVE) {

        /* Check for request to pause stream */
        if( snddrv.dec_status == SNDDEC_STATUS_PAUSING )
        {
            snddrv.dec_status = SNDDEC_STATUS_PAUSED;
            while( snddrv.dec_status != SNDDEC_STATUS_STREAMING && my_gen == adx_gen )
               thd_sleep(1);   /* EVE-DC: yield for a tick instead of spinning (starved the CD reads) */    
        }  

        /* If looping is enabled, check for loop point */ 
        if( loop && ADX_Info.loop )
            if( rd.fpos >= ADX_Info.loop_end  )
                   goto dec_finished;                   
        
        /* If there is room in PCM buffer, decode next chunk of ADX samples */        
        if( pcm_size < PCM_BUF_SIZE-16384 )
        {
            /* Read the current chunk */
            { uint64_t t0 = timer_ms_gettime64();
              adx_read(my_in, &rd, adx_buf, ADX_Info.chunk_size);
              uint64_t dt = timer_ms_gettime64() - t0; (void)dt;
#ifndef NDEBUG
              if (dt > 200) printf("LibADX: slow fread %lu ms\n", (unsigned long)dt);
#endif
            }
            /* Convert ADX chunk to PCM */
            adx_to_pcm(outbuf,adx_buf,prev);
            if (pcm_samples>32) wsize=32; else wsize = pcm_samples;
            pcm_samples-=wsize;
            /* Copy the deocded samples to sample buffer */
            memcpy(pcm_buf+pcm_size, outbuf, wsize*2*2);
            pcm_size+=wsize*2;
        }
        if( !ADX_ALIVE ) break;
        if( adx_push(pcm_buf, &pcm_size) == 0 && pcm_size >= PCM_BUF_SIZE - 16384*2 )
            thd_sleep(1);                    /* both buffers full: let the AICA drain */
    }
    else if (ADX_Info.channels==2)                   /* STEREO Decode Routine */
    while(pcm_samples && ADX_ALIVE) {
        
        /* Check for request to pause stream */
        if( snddrv.dec_status == SNDDEC_STATUS_PAUSING )
        {
            snddrv.dec_status = SNDDEC_STATUS_PAUSED;
            while( snddrv.dec_status != SNDDEC_STATUS_STREAMING && my_gen == adx_gen )
               thd_sleep(1);   /* EVE-DC: yield for a tick instead of spinning (starved the CD reads) */    
        }    
                            
        /* If looping is enabled, check for loop point */   
        if( loop && ADX_Info.loop )
            if( rd.fpos >= ADX_Info.loop_end  )
                   goto dec_finished;

        /* If there is room in the PCM buffer, decode some ADX samples */        
        if( pcm_size < PCM_BUF_SIZE - 16384*2 )
        {
            short tmpbuf[32*2];
            int i;

            adx_read(my_in, &rd, adx_buf, ADX_Info.chunk_size*2);
            adx_to_pcm(tmpbuf,adx_buf,prev);
            adx_to_pcm(tmpbuf+32,adx_buf+ADX_Info.chunk_size,prev+1);
            for(i=0;i<32;i++) {
                outbuf[i*2]   = tmpbuf[i];
                outbuf[i*2+1] = tmpbuf[i+32];
            }
            if (pcm_samples>32) wsize=32; else wsize = pcm_samples;
            pcm_samples-=wsize;
            memcpy(pcm_buf+pcm_size, outbuf, wsize*2*2);
            pcm_size+=wsize*2*2;
        }
        if( !ADX_ALIVE ) break;
        if( adx_push(pcm_buf, &pcm_size) == 0 && pcm_size >= PCM_BUF_SIZE - 16384*2 )
            thd_sleep(1);                    /* both buffers full: let the AICA drain */
    }

dec_finished:         /* Indicated samples finished or loop point reached */
#ifndef NDEBUG
    printf("LibADX: dec loop left (alive=%d) t=%lu\n", ADX_ALIVE ? 1 : 0, (unsigned long)timer_ms_gettime64());
#endif

    /* If loop is enabled seek to loop starting offset and continue streaming
     * (not after a stop: the original looped forever on adx_stop() of a looping file) */
    if( loop && ADX_ALIVE )
    {
        if( ADX_Info.loop )
        {
            adx_seek( my_in, &rd, ADX_Info.loop_start );
            ADX_Info.samples = ADX_Info.loop_samples;
        }
        else
            adx_seek( my_in, &rd, ADX_Info.sample_offset+ADX_CRI_SIZE );
        goto adx_dec;
    }

    /* At this point, we are finished decoding: push the rest and wait until the
     * AICA has consumed the ring (otherwise the tail of the voice is cut). */
    while( pcm_size > 0 && ADX_ALIVE )
        if( adx_push(pcm_buf, &pcm_size) == 0 ) thd_sleep(1);
    while( snddrv_ring_avail() > 0 && ADX_ALIVE )
        thd_sleep(1);

    printf("LibADX: decode finished t=%lu\n", (unsigned long)timer_ms_gettime64());

    free( pcm_buf );
    free( rd.buf );
    pcm_size = 0;
    if( my_gen == adx_gen )
        snddrv_exit();                               /* Exit the Sound Driver */
    else
        printf("LibADX: superseded decoder thread exiting\n");

exit:
    fclose(my_in);
    return NULL;
}

/* This thread will handle the 'pausing' routine */
static void* pause_thd(void* __attribute__((unused)) arg)
{
    while( snddrv.dec_status != SNDDEC_STATUS_RESUMING )
    { 
        if ( snddrv.buf_status == SNDDRV_STATUS_NEEDBUF )
        {
            memset( snddrv.pcm_buffer, 0, snddrv.pcm_needed );
            snddrv.buf_status = SNDDRV_STATUS_HAVEBUF;
        }   
        thd_sleep(20);
    }
    snddrv.dec_status = SNDDEC_STATUS_STREAMING; 
    return NULL;
}
