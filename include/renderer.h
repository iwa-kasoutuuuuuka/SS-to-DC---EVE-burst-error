#ifndef RENDERER_H
#define RENDERER_H

#include <kos.h>
#include <stdint.h>

/* PVR Constants */
#define SCREEN_WIDTH  640
#define SCREEN_HEIGHT 480
#define TXR_WIDTH     512
#define TXR_HEIGHT    256

/* Result codes */
#define RENDER_OK    0
#define RENDER_ERROR -1

/* Initialization */
int render_init();
void render_shutdown();

/* Texture Management */
pvr_ptr_t render_alloc_txr(uint32_t size);
void render_free_txr(pvr_ptr_t ptr);
int render_load_txr(const void *src, pvr_ptr_t dst, uint32_t size);
int render_load_txr_twiddled(const void *src, pvr_ptr_t dst, uint32_t w, uint32_t h);

/* Scene Management */
void render_begin();
void render_end();
void render_opaque_begin();
void render_opaque_end();
void render_translucent_begin();
void render_translucent_end();

/* Saturn screen (320x224) -> Dreamcast screen (640x480) scale factors */
#define SAT_SCALE_X (640.0f / 320.0f)
#define SAT_SCALE_Y (480.0f / 224.0f)

/* Drawing Primitives */
void draw_background(pvr_ptr_t txr);
/* Draw the top-left w x h pixels of a 512x256 RGB565 texture as the Saturn
 * background area (origin 0,0, scaled by SAT_SCALE_*). */
void draw_background_area(pvr_ptr_t txr, int w, int h);
/* Draw a w x h ARGB4444 sprite (top-left of a 512x256 texture) at Saturn
 * coordinates (x, y), between the background and the message window. */
void draw_sprite(pvr_ptr_t txr, int w, int h, int x, int y);
/* Full-screen black overlay with the given alpha (0 = invisible, 255 = black),
 * drawn in the translucent list above everything else. */
void draw_fade_overlay(int alpha);
void draw_message_box(float x, float y, float w, float h, uint32_t color);
void draw_text_layer(pvr_ptr_t txr, float x, float y, float z);
void draw_font_layer(pvr_ptr_t txr, float x, float y, float z);

/* Low-level PVR Helper */
void render_wait();

#endif /* RENDERER_H */
