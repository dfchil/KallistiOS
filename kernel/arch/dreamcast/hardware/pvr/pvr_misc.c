/* KallistiOS ##version##

   pvr_misc.c
   Copyright (C) 2002 Megan Potter
   Copyright (C) 2014 Lawrence Sebald

 */

#include <assert.h>
#include <string.h>
#include <float.h>

#include <kos/timer.h>
#include <dc/pvr.h>
#include <dc/video.h>
#include <kos/regfield.h>

#include "pvr_internal.h"

/*
   These are miscellaneous parameters you can set which affect the
   rendering process.
*/

/* The background plane's ISP/TSP instruction word: depth compare GREATER,
   cull counterclockwise, Gouraud shading. This is the value KOS has always
   used (0x90800000). */
#define BKG_ISP_GOURAUD     BIT(23)
#define BKG_ISP_TSP \
    (FIELD_PREP(PVR_TA_PM1_DEPTHCMP, PVR_DEPTHCMP_GREATER) \
     | FIELD_PREP(PVR_TA_PM1_CULLING, PVR_CULLING_CCW) \
     | BKG_ISP_GOURAUD)

/* The background plane's TSP instruction word without the texture fields:
   blend ONE/ZERO, no fog, mipmap D adjust 1.0, shading MODULATE (0x20800440). */
#define BKG_TSP \
    (FIELD_PREP(PVR_TA_PM2_SRCBLEND, PVR_BLEND_ONE) \
     | FIELD_PREP(PVR_TA_PM2_DSTBLEND, PVR_BLEND_ZERO) \
     | FIELD_PREP(PVR_TA_PM2_FOG, PVR_FOG_DISABLE) \
     | FIELD_PREP(PVR_TA_PM2_MIPBIAS, PVR_MIPBIAS_1_00) \
     | FIELD_PREP(PVR_TA_PM2_TXRENV, PVR_TXRENV_MODULATE))

_Static_assert(BKG_ISP_TSP == 0x90800000, "background ISP/TSP word changed");
_Static_assert(BKG_TSP == 0x20800440, "background TSP word changed");

/* Set the background plane color (the area of the screen not covered by
   any other polygons) */
void pvr_set_bg_color(float r, float g, float b) {
    int ir, ig, ib;
    uint32_t argb;

    ir = (int)(255 * r);
    ig = (int)(255 * g);
    ib = (int)(255 * b);

    argb = (ir << 16) | (ig << 8) | (ib << 0);

    irq_disable_scoped();

    pvr_state.bg_custom = false;
    pvr_state.bg_textured = false;
    pvr_state.bg_vert[0].argb = argb;
    pvr_state.bg_vert[1].argb = argb;
    pvr_state.bg_vert[2].argb = argb;
}

/* Set a background color for each of the default vertices */
void pvr_set_bg_colors(uint32_t bottom_left, uint32_t top_left,
                       uint32_t bottom_right) {
    irq_disable_scoped();

    pvr_state.bg_custom = false;
    pvr_state.bg_vert[0].argb = bottom_left;
    pvr_state.bg_vert[1].argb = top_left;
    pvr_state.bg_vert[2].argb = bottom_right;
}

/* Set the background plane's vertices */
int pvr_set_bg_poly(const pvr_bg_vertex_t v[3]) {
    float area;
    int i, a = 1, b = 2;

    assert(v != NULL);

    for(i = 0; i < 3; i++)
        if(!(v[i].z > 0.0f))
            return -1;

    /* Twice the signed area. The ISP culls the background plane like any
       other polygon (counterclockwise), so give it the winding of the default
       vertices, which have a positive area here. */
    area = (v[1].x - v[0].x) * (v[2].y - v[0].y)
           - (v[1].y - v[0].y) * (v[2].x - v[0].x);

    if(area == 0.0f)
        return -1;

    if(area < 0.0f) {
        a = 2;
        b = 1;
    }

    irq_disable_scoped();

    pvr_state.bg_custom = true;
    pvr_state.bg_vert[0] = v[0];
    pvr_state.bg_vert[1] = v[a];
    pvr_state.bg_vert[2] = v[b];

    return 0;
}

/* Set or remove the background plane's texture */
int pvr_set_bg_texture(pvr_ptr_t txr, int textureformat, int tw, int th,
                       pvr_filter_mode_t filtering) {
    uint32_t tsp, tcw;

    if(!txr) {
        irq_disable_scoped();
        pvr_state.bg_textured = false;
        return 0;
    }

    if(tw < 8 || tw > 1024 || __builtin_popcount(tw) != 1 ||
       th < 8 || th > 1024 || __builtin_popcount(th) != 1 ||
       (filtering != PVR_FILTER_NEAREST && filtering != PVR_FILTER_BILINEAR))
        return -1;

    /* Texture fields as pvr_poly_compile() sets them: repeat in U and V, no
       flip, texture alpha ignored, no mipmaps. */
    tsp = PVR_TA_PM2_TXRALPHA
          | FIELD_PREP(PVR_TA_PM2_FILTER, filtering)
          | FIELD_PREP(PVR_TA_PM2_USIZE, __builtin_ctz(tw) - 3)
          | FIELD_PREP(PVR_TA_PM2_VSIZE, __builtin_ctz(th) - 3);
    tcw = (textureformat & PVR_TA_PM3_TXRFMT) | to_pvr_txr_ptr(txr);

    irq_disable_scoped();

    pvr_state.bg_textured = true;
    pvr_state.bg_tsp = tsp;
    pvr_state.bg_tcw = tcw;

    return 0;
}

/* Enable/disable cheap shadow mode and set the cheap shadow scale register. */
void pvr_set_shadow_scale(bool enable, float scale_value) {
    int s = (int)(scale_value * 255);

    PVR_SET(PVR_CHEAP_SHADOW, (enable << 8) | (s & 0xFF));
}

/* Set the Z-Clip value (that is to say the depth of the background layer). */
void pvr_set_zclip(float zc) {
    pvr_state.zclip = zc;
}

/* Return the current VBlank count */
int pvr_get_vbl_count(void) {
    return pvr_state.vbl_count;
}

/* Fill in a statistics structure (above) from current data. This
   is a super-set of frame count. */
int pvr_get_stats(pvr_stats_t *stat) {
    if(!pvr_state.valid)
        return -1;

    assert(stat != NULL);

    stat->enabled_list_mask = pvr_state.lists_enabled;
    stat->vbl_count = pvr_state.vbl_count;
    stat->frame_last_time = pvr_state.frame_last_len;
    stat->reg_last_time = pvr_state.reg_last_len;
    stat->rnd_last_time = pvr_state.rnd_last_len;

    if(stat->frame_last_time != 0)
        stat->frame_rate = 1000000000.0f / stat->frame_last_time;
    else
        stat->frame_rate = -1.0f;

    stat->vtx_buffer_used = pvr_state.vtx_buf_used;
    stat->vtx_buffer_used_max = pvr_state.vtx_buf_used_max;
    stat->buf_last_time = pvr_state.buf_last_len;
    stat->frame_count = pvr_state.frame_count;

    return 0;
}

int pvr_vertex_dma_enabled(void) {
    return pvr_state.dma_mode;
}

/******** INTERNAL STUFF ************************************************/

/* Update statistical counters */
void pvr_sync_stats(int event) {
    uint64_t t;
    volatile pvr_ta_buffers_t *buf;

    if(event == PVR_SYNC_VBLANK) {
        pvr_state.vbl_count++;
    }
    else {
        /* Get the current time */
        t = timer_ns_gettime64();

        switch(event) {
            case PVR_SYNC_REGSTART:
                pvr_state.reg_start_time = t;
                break;

            case PVR_SYNC_REGDONE:
                pvr_state.reg_last_len = t - pvr_state.reg_start_time;

                buf = pvr_state.ta_buffers + pvr_state.ta_target;
                pvr_state.vtx_buf_used = PVR_GET(PVR_TA_VERTBUF_POS) - buf->vertex;

                if(pvr_state.vtx_buf_used > pvr_state.vtx_buf_used_max)
                    pvr_state.vtx_buf_used_max = pvr_state.vtx_buf_used;

                break;

            case PVR_SYNC_RNDSTART:
                pvr_state.rnd_start_time = t;
                break;

            case PVR_SYNC_RNDDONE:
                pvr_state.rnd_last_len = t - pvr_state.rnd_start_time;
                break;

            case PVR_SYNC_BUFSTART:
                pvr_state.buf_start_time = t;
                break;

            case PVR_SYNC_BUFDONE:
                pvr_state.buf_last_len = t - pvr_state.buf_start_time;
                break;

            case PVR_SYNC_PAGEFLIP:
                pvr_state.frame_last_len = t - pvr_state.frame_last_time;
                pvr_state.frame_last_time = t;
                pvr_state.frame_count++;
                break;
        }
    }
}

/* Synchronize the viewed page with what's in pvr_state */
void pvr_sync_view(void) {
    vid_set_start(pvr_state.frame_buffers[pvr_state.view_target].frame);
}

/* Synchronize the registration buffer with what's in pvr_state */
void pvr_sync_reg_buffer(void) {
    volatile pvr_ta_buffers_t *buf;

    buf = pvr_state.ta_buffers + pvr_state.ta_target;

    /* Reset TA */
    //PVR_SET(PVR_RESET, PVR_RESET_TA);
    //PVR_SET(PVR_RESET, PVR_RESET_NONE);

    /* Set buffer pointers */
    /* The first OPBs of each render pass, then the overflow OPBs */
    PVR_SET(PVR_TA_OPB_START,       buf->opb);
    PVR_SET(PVR_TA_OPB_INIT,        buf->opb + buf->opb_size * pvr_state.pass_count);
    PVR_SET(PVR_TA_OPB_END,         buf->opb + buf->opb_size * (pvr_state.pass_count + buf->opb_overflow_count));
    PVR_SET(PVR_TA_VERTBUF_START,   buf->vertex);
    PVR_SET(PVR_TA_VERTBUF_END,     buf->vertex + buf->vertex_size
                                    - PVR_BKG_POLY_RESERVE);

    /* Misc config parameters */
    PVR_SET(PVR_TILEMAT_CFG,        pvr_state.tsize_const);     /* Tile count: (H/32-1) << 16 | (W/32-1) */
    PVR_SET(PVR_OPB_CFG,            pvr_state.list_reg_mask);   /* List enables */
    PVR_SET(PVR_TA_INIT,            PVR_TA_INIT_GO);            /* Confirm settings */
    (void)PVR_GET(PVR_TA_INIT);

#if 0
    printf("== SYNC REG BUFFER:\n");
    printf("TA_OL_BASE: %08lx\nTA_OL_LIMIT: %08lx\nTA_NEXT_OPB: %08lx\n",
           PVR_GET(TA_OL_BASE), PVR_GET(TA_OL_LIMIT), PVR_GET(TA_NEXT_OPB) << 2);
#endif
}

/* Fields of PVR_BGPLANE_CFG (ISP_BACKGND_T) */
#define PVR_BKG_TAG_ADDR    GENMASK(23, 3)
#define PVR_BKG_SKIP        GENMASK(26, 24)

static inline uint32_t float_bits(float f) {
    union {
        float    f;
        uint32_t i;
    } u = { .f = f };

    return u.i;
}

/* Write the background plane polygon (see pvr_set_bg_poly() and friends) for
   a render target of w x h pixels to dst, which is in video RAM. Returns the
   size of one vertex in 32-bit words. */
static int pvr_write_bkg_poly(volatile uint32_t *dst, uint32_t w, uint32_t h) {
    /* Default vertices: bottom-left, top-left, bottom-right. */
    const float def_x[3] = { 0.0f, 0.0f, (float)w };
    const float def_y[3] = { (float)h, 0.0f, (float)h };
    const float def_u[3] = { 0.0f, 0.0f, 1.0f };
    const float def_v[3] = { 1.0f, 0.0f, 1.0f };
    bool custom = pvr_state.bg_custom;
    bool textured = pvr_state.bg_textured;
    volatile pvr_bg_vertex_t *v;
    int i;

    *dst++ = BKG_ISP_TSP | (textured ? PVR_TA_PM1_TXRENABLE : 0);
    *dst++ = BKG_TSP | (textured ? pvr_state.bg_tsp : 0);
    *dst++ = textured ? pvr_state.bg_tcw : 0;

    for(i = 0; i < 3; i++) {
        v = &pvr_state.bg_vert[i];

        *dst++ = float_bits(custom ? v->x : def_x[i]);
        *dst++ = float_bits(custom ? v->y : def_y[i]);
        *dst++ = float_bits(custom ? v->z : FLT_EPSILON);

        if(textured) {
            *dst++ = float_bits(custom ? v->u : def_u[i]);
            *dst++ = float_bits(custom ? v->v : def_v[i]);
        }

        *dst++ = v->argb;
    }

    return textured ? 6 : 4;
}

/* Begin a render operation that has been queued completely (i.e., the
   opposite of ta_target) */
void pvr_begin_queued_render(void) {
    volatile pvr_ta_buffers_t   *tbuf;

    /* Get the appropriate buffer */
    tbuf = pvr_state.ta_buffers + (pvr_state.ta_target ^ pvr_state.vbuf_doublebuf);

    /* Throw the background polygon on the end of the TA's data. There is
       always room for it: pvr_sync_reg_buffer() keeps PVR_BKG_POLY_RESERVE
       bytes free at the end of the vertex buffer. */
    pvr_begin_render(tbuf->tile_matrix, tbuf->vertex,
                     PVR_GET(PVR_TA_VERTBUF_POS));
}

/* Begin a render from the given region array and parameter base, with the
   background plane's parameters written at bg_addr */
void pvr_begin_render(uint32_t tile_matrix, uint32_t param_base, uint32_t bg_addr) {
    volatile pvr_frame_buffers_t    *rbuf;
    uint32_t      vert_end;
    int           vert_words;
    uint32_t      target_w, target_h;
    int bufn = pvr_state.view_target;
    union {
        float    f;
        uint32_t i;
    } zclip;

    rbuf = pvr_state.frame_buffers + (bufn ^ 1);
    target_w = pvr_state.curr_to_texture ? pvr_state.to_txr_w : (uint32_t)pvr_state.w;
    target_h = pvr_state.curr_to_texture ? pvr_state.to_txr_h : (uint32_t)pvr_state.h;

    /* With FSAA the render target is twice the screen width. */
    vert_words = pvr_write_bkg_poly((volatile uint32_t *)(PVR_RAM_BASE | bg_addr),
        pvr_state.curr_to_texture ? target_w : target_w << !!pvr_state.fsaa,
        target_h);

    /* Background plane location (ISP_BACKGND_T): the skip field is the
       vertex size in words minus 3, the tag address is the polygon's word
       offset from the parameter base, at bit 3. */
    vert_end = FIELD_PREP(PVR_BKG_SKIP, vert_words - 3)
               | FIELD_PREP(PVR_BKG_TAG_ADDR, (bg_addr - param_base) >> 2);

    /* Reset the ISP/TSP, just in case */
    //PVR_SET(PVR_RESET, PVR_RESET_ISPTSP);
    //PVR_SET(PVR_RESET, PVR_RESET_NONE);

    /* Finish up rendering the current frame (into the other buffer) */
    PVR_SET(PVR_ISP_TILEMAT_ADDR, tile_matrix);
    PVR_SET(PVR_ISP_VERTBUF_ADDR, param_base);

    if(!pvr_state.curr_to_texture)
        PVR_SET(PVR_RENDER_ADDR, rbuf->frame);
    else {
        PVR_SET(PVR_RENDER_ADDR, pvr_state.to_txr_addr | BIT(24));
        PVR_SET(PVR_RENDER_ADDR_2, pvr_state.to_txr_addr | BIT(24));
    }

    PVR_SET(PVR_BGPLANE_CFG, vert_end); /* Bkg plane location */
    zclip.f = pvr_state.zclip;
    PVR_SET(PVR_BGPLANE_Z, zclip.i);
    if(!pvr_state.curr_to_texture) {
        PVR_SET(PVR_PCLIP_X, pvr_state.pclip_x);
        PVR_SET(PVR_PCLIP_Y, pvr_state.pclip_y);
    }
    else {
        PVR_SET(PVR_PCLIP_X, ((target_w - 1) << 16) | 0);
        PVR_SET(PVR_PCLIP_Y, ((target_h - 1) << 16) | 0);
    }

    if(!pvr_state.curr_to_texture)
        PVR_SET(PVR_RENDER_MODULO, (pvr_state.w * vid_pmode_bpp[vid_mode->pm]) / 8);
    else
        PVR_SET(PVR_RENDER_MODULO, pvr_state.to_txr_rp);

    // XXX Do we _really_ need this every time?
    // SETREG(PVR_FB_CFG_2, 0x00000009);        /* Alpha mode */
    PVR_SET(PVR_ISP_START, PVR_ISP_START_GO);   /* Start render */
}

void pvr_blank_polyhdr(int type) {
    pvr_poly_hdr_t poly;

    // Make it.
    pvr_blank_polyhdr_buf(type, &poly);

    // Submit it
    pvr_prim(&poly, sizeof(poly));
}

void pvr_blank_polyhdr_buf(int type, pvr_poly_hdr_t *poly) {
    /* Empty it out */
    memset(poly, 0, sizeof(pvr_poly_hdr_t));

    /* Put in the list type */
    poly->cmd = FIELD_PREP(PVR_TA_CMD_TYPE, type) | 0x80840012;
}

static pvr_ptr_t pvr_get_frame_buffer(bool back) {
    unsigned int idx;
    uint32_t addr;

    irq_disable_scoped();

    /* The front buffer may not have been fully rendered or submitted to the
       video hardware yet. In case this has yet to happen, we want the second
       view target, aka. the one not currently being displayed. */
    idx = pvr_state.view_target ^ (pvr_state.render_busy | pvr_state.render_completed) ^ back;

    addr = pvr_state.frame_buffers[idx].frame & (PVR_RAM_SIZE - 1);

    /* The front buffer is in 32-bit memory, convert its address to make it
       addressable from the 64-bit memory */
    return (pvr_ptr_t)(addr * 2 + PVR_RAM_BASE);
}

pvr_ptr_t pvr_get_front_buffer(void) {
    return pvr_get_frame_buffer(false);
}

pvr_ptr_t pvr_get_back_buffer(void) {
    return pvr_get_frame_buffer(true);
}

int pvr_set_vertical_scale(float factor) {
    uint32_t f16;
    uint32_t cfg;

    if(factor == 0.0f)
        return -1;

    f16 = 1024.0f / factor;

    if(f16 == 0 || f16 >= 65536)
        return -1;

    irq_disable_scoped();

    cfg = PVR_GET(PVR_SCALER_CFG);

    cfg &= ~PVR_SCALER_CFG_VSCALE_FACTOR;
    cfg |= FIELD_PREP(PVR_SCALER_CFG_VSCALE_FACTOR, f16);

    PVR_SET(PVR_SCALER_CFG, cfg);

    return 0;
}
