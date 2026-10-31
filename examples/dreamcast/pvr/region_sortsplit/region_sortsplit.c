/* KallistiOS ##version##

   examples/dreamcast/pvr/region_sortsplit/region_sortsplit.c
   Copyright (C) 2026 Daniel Fairchild

   Translucent sort mode per tile.

   The PVR sorts translucent polygons either itself, per pixel (autosort), or
   not at all, drawing them in the order they were sent (presort). Normally
   one mode applies to the whole screen. Here the left part of the screen
   presorts and the right part autosorts, in the same frame, with the same
   polygons.

   The scene is a stack of overlapping translucent bands, each at its own
   depth, sent front to back. Where two bands overlap, the autosorted side
   shows the nearer band on top (depth order), and the presorted side shows
   the band sent last (send order). The number on each band is its depth
   rank: 1 is the nearest.

   Controls:
     D-pad left/right   move the split line one tile
     A                  swap the sides
     Y                  FSAA on/off
     Start              quit
*/

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <dc/biosfont.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/pvr.h>
#include <dc/video.h>

/* Text drawn from a texture holding the BIOS font, in the punch-through
   list so that it doesn't take part in the translucent sorting. */
#define FONT_TEX_W  256
#define FONT_TEX_H  256
#define FONT_CELL_W 16

static pvr_ptr_t font_tex;
static pvr_poly_hdr_t font_hdr;

/* Render coordinates per screen pixel across: 2 with FSAA */
static float xs = 1.0f;

static void font_init(void) {
    uint16_t *buf = calloc(FONT_TEX_W * FONT_TEX_H, sizeof(uint16_t));
    pvr_poly_cxt_t cxt;
    int c, i;

    for(c = 33; c < 127; c++) {
        i = c - 32;
        bfont_draw_ex(buf + (i / 16) * BFONT_HEIGHT * FONT_TEX_W +
                      (i % 16) * FONT_CELL_W, FONT_TEX_W, 0xffff, 0x0000, 16,
                      false, c, false, false);
    }

    font_tex = pvr_mem_malloc(FONT_TEX_W * FONT_TEX_H * 2);
    pvr_txr_load(buf, font_tex, FONT_TEX_W * FONT_TEX_H * 2);
    free(buf);

    pvr_poly_cxt_txr(&cxt, PVR_LIST_PT_POLY,
                     PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED,
                     FONT_TEX_W, FONT_TEX_H, font_tex, PVR_FILTER_NONE);
    pvr_poly_compile(&font_hdr, &cxt);
}

static void quad(float x0, float y0, float x1, float y1, float z,
                 uint32_t argb, float u0, float v0, float u1, float v1) {
    pvr_vertex_t v;

    v.flags = PVR_CMD_VERTEX;
    v.z = z;
    v.argb = argb;
    v.oargb = 0;

    v.x = x0 * xs; v.y = y1; v.u = u0; v.v = v1;
    pvr_prim(&v, sizeof(v));
    v.x = x0 * xs; v.y = y0; v.u = u0; v.v = v0;
    pvr_prim(&v, sizeof(v));
    v.x = x1 * xs; v.y = y1; v.u = u1; v.v = v1;
    pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX_EOL;
    v.x = x1 * xs; v.y = y0; v.u = u1; v.v = v0;
    pvr_prim(&v, sizeof(v));
}

/* Draw text in the punch-through list (after font_hdr was sent) */
static void text_draw(float x, float y, float z, uint32_t argb, const char *s) {
    float u, v;
    int i;

    for(; *s; s++, x += BFONT_THIN_WIDTH) {
        i = (unsigned char)*s - 32;

        if(i <= 0 || i >= 95)
            continue;

        u = (float)((i % 16) * FONT_CELL_W) / FONT_TEX_W;
        v = (float)((i / 16) * BFONT_HEIGHT) / FONT_TEX_H;
        quad(x, y, x + BFONT_THIN_WIDTH, y + BFONT_HEIGHT, z, argb,
             u, v, u + (float)BFONT_THIN_WIDTH / FONT_TEX_W,
             v + (float)BFONT_HEIGHT / FONT_TEX_H);
    }
}

static void text_printf(float x, float y, uint32_t argb, const char *fmt, ...) {
    char buf[96];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    text_draw(x + 2, y + 2, 100.0f, 0xff000000, buf);
    text_draw(x, y, 101.0f, argb, buf);
}

/* The bands, listed in the order they are sent: front to back, the wrong
   order for presorted translucency. Depth rank 1 is the nearest. Each band
   overlaps the next one, which is farther away but sent later, so the
   overlaps show the sort mode: autosort puts the nearer (upper) band on top,
   presort the later (lower) one. */
#define BANDS       6
#define BAND_STEP   60
#define BAND_H      90
#define BAND_Y(i)   (40 + (i) * BAND_STEP)

static const struct {
    int rank;
    uint32_t argb;
} bands[BANDS] = {
    { 1, 0xa0ff2020 },
    { 2, 0xa020ff20 },
    { 3, 0xa02060ff },
    { 4, 0xa0ffff20 },
    { 5, 0xa0ff20ff },
    { 6, 0xa020ffff },
};

static pvr_poly_hdr_t op_hdr, tr_hdr, pt_hdr;

static int split;          /* First tile column of the right side */
static int tile_cols;      /* Tile columns, from pvr_get_tile_grid() */
static bool swapped;       /* Left side autosorts, right side presorts */
static bool fsaa;

static void apply_split(void) {
    int split_px = split * vid_mode->width / tile_cols;

    pvr_set_rect_presort(0, 0, split_px, vid_mode->height, !swapped);
    pvr_set_rect_presort(split_px, 0, vid_mode->width - split_px,
                         vid_mode->height, swapped);
}

static void setup(void) {
    pvr_init_params_t params = {
        { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0,
          PVR_BINSIZE_16 },
        512 * 1024, 0, 0, 0, 3, 0
    };
    pvr_poly_cxt_t cxt;
    int th;

    params.fsaa_enabled = fsaa;
    pvr_init(&params);
    pvr_set_bg_color(0.1f, 0.1f, 0.15f);

    xs = fsaa ? 2.0f : 1.0f;

    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    pvr_poly_compile(&op_hdr, &cxt);
    /* Without depth writes, presorted bands blend in send order instead of
       hiding the farther ones */
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(&tr_hdr, &cxt);
    pvr_poly_cxt_col(&cxt, PVR_LIST_PT_POLY);
    pvr_poly_compile(&pt_hdr, &cxt);

    font_init();

    /* Keep the split at the same place on the screen */
    pvr_get_tile_grid(&tile_cols, &th);

    if(!split)
        split = tile_cols / 2;

    apply_split();
}

static void draw_frame(void) {
    pvr_stats_t stats;
    float y, split_x;
    int i, x, yy;

    pvr_wait_ready();
    pvr_scene_begin();

    /* Opaque checkerboard behind the bands, so their blending shows */
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&op_hdr, sizeof(op_hdr));

    for(yy = 0; yy < 480; yy += 40)
        for(x = 0; x < 640; x += 40)
            quad(x, yy, x + 40, yy + 40, 0.5f,
                 ((x + yy) / 40) & 1 ? 0xff404040 : 0xff606060, 0, 0, 0, 0);

    pvr_list_finish();

    /* The bands, all across the screen, overlapping their neighbours */
    pvr_list_begin(PVR_LIST_TR_POLY);
    pvr_prim(&tr_hdr, sizeof(tr_hdr));

    for(i = 0; i < BANDS; i++) {
        y = BAND_Y(i);
        quad(16, y, 624, y + BAND_H, 10.0f - bands[i].rank, bands[i].argb,
             0, 0, 0, 0);
    }

    pvr_list_finish();

    /* Split line, labels and status in the punch-through list */
    pvr_list_begin(PVR_LIST_PT_POLY);
    pvr_prim(&pt_hdr, sizeof(pt_hdr));

    split_x = (float)(split * vid_mode->width) / tile_cols;
    quad(split_x - 1, 0, split_x + 1, 480, 50.0f, 0xffffffff, 0, 0, 0, 0);

    pvr_prim(&font_hdr, sizeof(font_hdr));

    pvr_get_stats(&stats);
    text_printf(16, 8, 0xffffffff, "Left: %s   Right: %s",
                swapped ? "AUTOSORT" : "PRESORT", swapped ? "PRESORT" : "AUTOSORT");
    text_printf(16, 430, 0xffffffff, "Split at tile column %d of %d  %s",
                split, tile_cols, fsaa ? "FSAA" : "");
    text_printf(16, 454, 0xffc0c0c0, "PVR render %.2f ms   <-/->  A swap  Y FSAA",
                (double)stats.rnd_last_time / 1000000.0);

    /* Depth rank of each band, where it doesn't overlap the others */
    for(i = 0; i < BANDS; i++) {
        y = BAND_Y(i) + (BAND_H - BAND_STEP) + 3;
        text_printf(24, y, 0xffffffff, "%d", bands[i].rank);
        text_printf(600, y, 0xffffffff, "%d", bands[i].rank);
    }

    pvr_list_finish();

    pvr_scene_finish();
}

int main(int argc, char *argv[]) {
    maple_device_t *cont;
    cont_state_t *st;
    uint32_t buttons, prev = 0, pressed;

    (void)argc;
    (void)argv;

    printf("region_sortsplit: left tiles presort, right tiles autosort\n");

    setup();

    for(;;) {
        buttons = 0;

        if((cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER)) &&
           (st = (cont_state_t *)maple_dev_status(cont)))
            buttons = st->buttons;

        pressed = buttons & ~prev;
        prev = buttons;

        if(pressed & CONT_START)
            break;

        if((pressed & CONT_DPAD_LEFT) && split > 0) {
            split--;
            apply_split();
        }

        if((pressed & CONT_DPAD_RIGHT) && split < tile_cols) {
            split++;
            apply_split();
        }

        if(pressed & CONT_A) {
            swapped = !swapped;
            apply_split();
        }

        if(pressed & CONT_Y) {
            /* Restart the PVR with or without FSAA. FSAA doubles the
               number of tile columns, so keep the split at the same place
               on the screen. */
            pvr_wait_ready();
            pvr_shutdown();
            split = fsaa ? split / 2 : split * 2;
            fsaa = !fsaa;
            setup();
        }

        draw_frame();
    }

    pvr_wait_ready();
    pvr_shutdown();

    return 0;
}
