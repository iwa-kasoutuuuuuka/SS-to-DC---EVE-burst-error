#include "renderer.h"

static pvr_init_params_t pvr_params = {
    .opb_sizes = { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_0 },
    .vertex_buf_size = 256 * 1024  /* Reduced from 512KB: only a few polygons needed */
};

int render_init() {
    vid_set_mode(DM_640x480, PM_RGB565);
    int ret = pvr_init(&pvr_params);
    pvr_txr_set_stride(512);
    return ret;
}

void render_shutdown() {
    pvr_shutdown();
}

pvr_ptr_t render_alloc_txr(uint32_t size) {
    return pvr_mem_malloc(size);
}

void render_free_txr(pvr_ptr_t ptr) {
    if (ptr) pvr_mem_free(ptr);
}

int render_load_txr(const void *src, pvr_ptr_t dst, uint32_t size) {
    if (!src || !dst) return RENDER_ERROR;
    
    pvr_txr_load(src, dst, size);
    
    return RENDER_OK;
}

int render_load_txr_twiddled(const void *src, pvr_ptr_t dst, uint32_t w, uint32_t h) {
    if (!src || !dst) return RENDER_ERROR;
    
    pvr_txr_load_ex(src, dst, w, h, PVR_TXRLOAD_16BPP);
    
    return RENDER_OK;
}

void render_opaque_begin() {
    pvr_list_begin(PVR_LIST_OP_POLY);
}

void render_opaque_end() {
    pvr_list_finish();
}

void render_translucent_begin() {
    pvr_list_begin(PVR_LIST_TR_POLY);
}

void render_translucent_end() {
    pvr_list_finish();
}

void render_begin() {
    pvr_wait_ready();
    pvr_scene_begin();
}

void render_end() {
    pvr_scene_finish();
}

/* Cached polygon headers (compiled once, reused every frame) */
static pvr_poly_hdr_t bg_hdr;
static pvr_ptr_t bg_hdr_txr = NULL; /* Track which texture the header was compiled for */

void draw_background(pvr_ptr_t txr) {
    if (!txr) return;

    /* Recompile header only when texture changes */
    if (txr != bg_hdr_txr) {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED | PVR_TXRFMT_X32_STRIDE, 512, 256, txr, PVR_FILTER_BILINEAR);
        pvr_poly_compile(&bg_hdr, &cxt);
        bg_hdr_txr = txr;
    }
    pvr_prim(&bg_hdr, sizeof(bg_hdr));

    pvr_vertex_t vert;
    vert.flags = PVR_CMD_VERTEX; vert.z = 1.0f; vert.argb = 0xFFFFFFFF; vert.oargb = 0;
    vert.x = 0; vert.y = 0; vert.u = 0; vert.v = 0; pvr_prim(&vert, sizeof(vert));
    vert.x = 640; vert.y = 0; vert.u = 1.0f; vert.v = 0; pvr_prim(&vert, sizeof(vert));
    vert.x = 0; vert.y = 480; vert.u = 0; vert.v = 1.0f; pvr_prim(&vert, sizeof(vert));
    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = 640; vert.y = 480; vert.u = 1.0f; vert.v = 1.0f; pvr_prim(&vert, sizeof(vert));
}

static void draw_textured_quad(pvr_poly_hdr_t *hdr, float x0, float y0, float x1, float y1,
                               float u1, float v1, float z) {
    pvr_prim(hdr, sizeof(*hdr));
    pvr_vertex_t vert;
    vert.flags = PVR_CMD_VERTEX; vert.z = z; vert.argb = 0xFFFFFFFF; vert.oargb = 0;
    vert.x = x0; vert.y = y0; vert.u = 0;  vert.v = 0;  pvr_prim(&vert, sizeof(vert));
    vert.x = x1; vert.y = y0; vert.u = u1; vert.v = 0;  pvr_prim(&vert, sizeof(vert));
    vert.x = x0; vert.y = y1; vert.u = 0;  vert.v = v1; pvr_prim(&vert, sizeof(vert));
    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = x1; vert.y = y1; vert.u = u1; vert.v = v1; pvr_prim(&vert, sizeof(vert));
}

/* Cached header for the Saturn-sized background */
static pvr_poly_hdr_t bga_hdr;
static pvr_ptr_t bga_hdr_txr = NULL;

void draw_background_area(pvr_ptr_t txr, int w, int h) {
    if (!txr || w <= 0 || h <= 0) return;
    if (txr != bga_hdr_txr) {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED | PVR_TXRFMT_X32_STRIDE, 512, 256, txr, PVR_FILTER_BILINEAR);
        pvr_poly_compile(&bga_hdr, &cxt);
        bga_hdr_txr = txr;
    }
    draw_textured_quad(&bga_hdr, 0, 0, w * SAT_SCALE_X, h * SAT_SCALE_Y,
                       (float)w / 512.0f, (float)h / 256.0f, 1.0f);
}

/* Cached header for the sprite layer */
static pvr_poly_hdr_t spr_hdr;
static pvr_ptr_t spr_hdr_txr = NULL;

void draw_sprite(pvr_ptr_t txr, int w, int h, int x, int y) {
    if (!txr || w <= 0 || h <= 0) return;
    if (txr != spr_hdr_txr) {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED | PVR_TXRFMT_X32_STRIDE, 512, 256, txr, PVR_FILTER_BILINEAR);
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
        cxt.blend.src_enable = 1;
        cxt.blend.dst_enable = 1;
        cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
        pvr_poly_compile(&spr_hdr, &cxt);
        spr_hdr_txr = txr;
    }
    float x0 = x * SAT_SCALE_X, y0 = y * SAT_SCALE_Y;
    draw_textured_quad(&spr_hdr, x0, y0, x0 + w * SAT_SCALE_X, y0 + h * SAT_SCALE_Y,
                       (float)w / 512.0f, (float)h / 256.0f, 3.0f);
}

void draw_fade_overlay(int alpha) {
    if (alpha <= 0) return;
    if (alpha > 255) alpha = 255;
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_vertex_t vert;

    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));

    uint32_t color = ((uint32_t)alpha << 24);
    /* z = 4: above the background (OP list) and the sprite layer (z 3), below the
     * message box (z 5) and text (z 10). On the Saturn the fade is a colour
     * calculation on the background plane; the message window stays readable
     * (the A01 title shows its menu right after a FADEOUT 0). */
    vert.flags = PVR_CMD_VERTEX; vert.z = 4.0f; vert.argb = color; vert.oargb = 0;
    vert.x = 0;   vert.y = 0;   pvr_prim(&vert, sizeof(vert));
    vert.x = 640; vert.y = 0;   pvr_prim(&vert, sizeof(vert));
    vert.x = 0;   vert.y = 480; pvr_prim(&vert, sizeof(vert));
    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = 640; vert.y = 480; pvr_prim(&vert, sizeof(vert));
}

void draw_message_box(float x, float y, float w, float h, uint32_t color) {
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_vertex_t vert;

    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));

    vert.flags = PVR_CMD_VERTEX; vert.z = 2.0f; vert.argb = color; vert.oargb = 0;
    vert.x = x; vert.y = y; pvr_prim(&vert, sizeof(vert));
    vert.x = x + w; vert.y = y; pvr_prim(&vert, sizeof(vert));
    vert.x = x; vert.y = y + h; pvr_prim(&vert, sizeof(vert));
    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = x + w; vert.y = y + h; pvr_prim(&vert, sizeof(vert));
}

/* Cached text layer header */
static pvr_poly_hdr_t text_hdr;
static pvr_ptr_t text_hdr_txr = NULL;

void draw_text_layer(pvr_ptr_t txr, float x, float y, float z) {
    if (!txr) return;

    /* Recompile header only when texture changes */
    if (txr != text_hdr_txr) {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED | PVR_TXRFMT_X32_STRIDE, 512, 256, txr, PVR_FILTER_BILINEAR);
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
        cxt.blend.src_enable = 1;
        cxt.blend.dst_enable = 1;
        cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
        pvr_poly_compile(&text_hdr, &cxt);
        text_hdr_txr = txr;
    }
    pvr_prim(&text_hdr, sizeof(text_hdr));

    float v_range = 230.0f / 256.0f;

    pvr_vertex_t vert;
    vert.flags = PVR_CMD_VERTEX; vert.z = z; vert.argb = 0xFFFFFFFF; vert.oargb = 0;
    vert.x = x; vert.y = y; vert.u = 0; vert.v = 0; pvr_prim(&vert, sizeof(vert));
    vert.x = x + 512; vert.y = y; vert.u = 1; vert.v = 0; pvr_prim(&vert, sizeof(vert));
    vert.x = x; vert.y = y + 230; vert.u = 0; vert.v = v_range; pvr_prim(&vert, sizeof(vert));
    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = x + 512; vert.y = y + 230; vert.u = 1; vert.v = v_range; pvr_prim(&vert, sizeof(vert));
}

/* Cached font layer header */
static pvr_poly_hdr_t font_hdr;
static pvr_ptr_t font_hdr_txr = NULL;

void draw_font_layer(pvr_ptr_t txr, float x, float y, float z) {
    if (!txr) return;

    /* Recompile header only when texture changes */
    if (txr != font_hdr_txr) {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED | PVR_TXRFMT_X32_STRIDE, 512, 256, txr, PVR_FILTER_BILINEAR);
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
        cxt.blend.src_enable = 1;
        cxt.blend.dst_enable = 1;
        cxt.txr.alpha = PVR_TXRALPHA_ENABLE;
        pvr_poly_compile(&font_hdr, &cxt);
        font_hdr_txr = txr;
    }
    pvr_prim(&font_hdr, sizeof(font_hdr));

    float v_range = 230.0f / 256.0f;

    pvr_vertex_t vert;
    vert.flags = PVR_CMD_VERTEX; vert.z = z; vert.argb = 0xFFFFFFFF; vert.oargb = 0;
    vert.x = x; vert.y = y; vert.u = 0; vert.v = 0; pvr_prim(&vert, sizeof(vert));
    vert.x = x + 512; vert.y = y; vert.u = 1; vert.v = 0; pvr_prim(&vert, sizeof(vert));
    vert.x = x; vert.y = y + 230; vert.u = 0; vert.v = v_range; pvr_prim(&vert, sizeof(vert));
    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = x + 512; vert.y = y + 230; vert.u = 1; vert.v = v_range; pvr_prim(&vert, sizeof(vert));
}

void render_wait() {
    pvr_wait_ready();
}
