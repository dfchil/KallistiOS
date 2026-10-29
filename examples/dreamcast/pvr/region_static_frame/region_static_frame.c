/* KallistiOS ##version##

   examples/dreamcast/pvr/region_static_frame/region_static_frame.c
   Copyright (C) 2026 Daniel Fairchild

   Partial redraw with skipped tiles.

   A small 3D scene runs in a window, surrounded by a heavy frame that never
   changes: thousands of polygons in ornate rosettes, with translucent glints
   over them. Normally all of it has to be sent and rendered every frame.

   The frame is drawn on two frames only, into both frame buffers. After
   that, only the tiles of the window and of the status rows are left in the
   PVR's region array: the other tiles are neither rendered nor written, so
   the frame stays on the screen, and its geometry isn't sent at all. The
   status line shows the PVR render time and the CPU time spent sending the
   scene, for this mode and for full redraw.

   Controls:
     A          partial redraw / full redraw
     Y          FSAA on/off
     Start      quit
*/

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <dc/biosfont.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/pvr.h>
#include <dc/video.h>
#include <kos/timer.h>

/* The window, aligned to 32-pixel tiles */
#define WIN_X   128
#define WIN_Y   64
#define WIN_W   384
#define WIN_H   288

/* The status text, in the last two tile rows */
#define HUD_Y   416

/* ------------------------------------------------------------------ */
/* Text, from a texture holding the BIOS font                         */

#define FONT_TEX_W  256
#define FONT_TEX_H  256
#define FONT_CELL_W 16

static pvr_ptr_t font_tex;
static pvr_poly_hdr_t font_hdr;
static float xs = 1.0f;     /* Render coordinates per screen pixel across */

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

static void vert(float x, float y, float z, uint32_t argb, float u, float v,
                 bool eol) {
    pvr_vertex_t vx;

    vx.flags = eol ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    vx.x = x * xs;
    vx.y = y;
    vx.z = z;
    vx.u = u;
    vx.v = v;
    vx.argb = argb;
    vx.oargb = 0;
    pvr_prim(&vx, sizeof(vx));
}

static void quad(float x0, float y0, float x1, float y1, float z,
                 uint32_t argb, float u0, float v0, float u1, float v1) {
    vert(x0, y1, z, argb, u0, v1, false);
    vert(x0, y0, z, argb, u0, v0, false);
    vert(x1, y1, z, argb, u1, v1, false);
    vert(x1, y0, z, argb, u1, v0, true);
}

static void text_printf(float x, float y, uint32_t argb, const char *fmt, ...) {
    char buf[96], *s;
    va_list args;
    float u, v, sx;
    int i, pass;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    /* Shadow, then the text */
    for(pass = 0; pass < 2; pass++) {
        for(s = buf, sx = x + 2 - 2 * pass; *s; s++, sx += BFONT_THIN_WIDTH) {
            i = (unsigned char)*s - 32;

            if(i <= 0 || i >= 95)
                continue;

            u = (float)((i % 16) * FONT_CELL_W) / FONT_TEX_W;
            v = (float)((i / 16) * BFONT_HEIGHT) / FONT_TEX_H;
            quad(sx, y + 2 - 2 * pass, sx + BFONT_THIN_WIDTH,
                 y + 2 - 2 * pass + BFONT_HEIGHT, 100.0f + pass,
                 pass ? argb : 0xff000000,
                 u, v, u + (float)BFONT_THIN_WIDTH / FONT_TEX_W,
                 v + (float)BFONT_HEIGHT / FONT_TEX_H);
        }
    }
}

/* ------------------------------------------------------------------ */
/* The frame: built once into RAM, sent with one pvr_prim() per list  */

#define RINGS       3
#define PETALS      16
#define RING_VERTS  (2 * (PETALS + 1))

static pvr_poly_hdr_t op_hdr, tr_hdr, pt_hdr;
static pvr_vertex_t *frame_op, *frame_tr;
static size_t frame_op_size, frame_tr_size;
static int frame_polys;

static bool in_window(int x, int y) {
    return x >= WIN_X && x < WIN_X + WIN_W && y >= WIN_Y && y < WIN_Y + WIN_H;
}

static uint32_t mix(uint32_t a, uint32_t b, float t) {
    int i;
    uint32_t c = 0, ca, cb;

    for(i = 0; i < 32; i += 8) {
        ca = (a >> i) & 0xff;
        cb = (b >> i) & 0xff;
        c |= (uint32_t)(ca + (cb - (float)ca) * t) << i;
    }

    return c;
}

static pvr_vertex_t *put(pvr_vertex_t *v, float x, float y, float z,
                         uint32_t argb, bool eol) {
    v->flags = eol ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    v->x = x * xs;
    v->y = y;
    v->z = z;
    v->u = v->v = 0.0f;
    v->argb = argb;
    v->oargb = 0;

    return v + 1;
}

static void frame_build(void) {
    static const uint32_t gold = 0xffd8a838, wine = 0xff701830,
                          teal = 0xff106870, ivory = 0xfff0e8d0;
    pvr_vertex_t *v, *t;
    int cells = 0, cx, cy, r, i;
    float a, r0, r1, x, y, z;

    /* Count the cells of the frame first */
    for(cy = 0; cy < HUD_Y; cy += 32)
        for(cx = 0; cx < 640; cx += 32)
            cells += !in_window(cx, cy);

    free(frame_op);
    free(frame_tr);
    frame_op_size = (1 + cells * (4 + RINGS * RING_VERTS)) *
                    sizeof(pvr_vertex_t);
    frame_tr_size = (1 + cells * 2 * 4) * sizeof(pvr_vertex_t);
    frame_op = aligned_alloc(32, frame_op_size);
    frame_tr = aligned_alloc(32, frame_tr_size);
    frame_polys = 0;

    /* One polygon header per list, then strips. A header is as large as a
       vertex. */
    v = frame_op;
    t = frame_tr;
    *(pvr_poly_hdr_t *)v++ = op_hdr;
    *(pvr_poly_hdr_t *)t++ = tr_hdr;

    for(cy = 0; cy < HUD_Y; cy += 32) {
        for(cx = 0; cx < 640; cx += 32) {
            if(in_window(cx, cy))
                continue;

            x = cx + 16.0f;
            y = cy + 16.0f;

            /* Opaque rosette: rings of petals, each ring a strip between
               two circles. Everything stays inside the cell, so that none of
               it reaches into the window. */

            v = put(v, cx, cy + 32, 0.9f, wine, false);
            v = put(v, cx, cy, 0.9f, mix(wine, gold, 0.3f), false);
            v = put(v, cx + 32, cy + 32, 0.9f, mix(wine, teal, 0.5f), false);
            v = put(v, cx + 32, cy, 0.9f, wine, true);
            frame_polys += 2;

            for(r = 0; r < RINGS; r++) {
                r0 = 2.0f + r * 4.0f;
                r1 = 7.0f + r * 4.5f;
                z = 1.0f + (RINGS - r) * 0.1f;

                for(i = 0; i <= PETALS; i++) {
                    a = (i + 0.5f * (r & 1) + (cx + cy) / 64.0f) *
                        2.0f * (float)M_PI / PETALS;
                    v = put(v, x + r0 * cosf(a), y + r0 * sinf(a), z,
                            mix(r & 1 ? teal : wine, ivory, 0.15f * r),
                            false);
                    v = put(v, x + r1 * cosf(a + (float)M_PI / PETALS),
                            y + r1 * sinf(a + (float)M_PI / PETALS), z,
                            (i & 1) ? gold : mix(gold, wine, 0.6f),
                            i == PETALS);
                }

                frame_polys += 2 * PETALS;
            }

            /* Two translucent glints over the cell */

            for(i = 0; i < 2; i++) {
                r0 = 10.0f + 6.0f * i;
                z = 2.0f + i * 0.1f;
                t = put(t, x - r0, y + r0, z, 0x30ffffff, false);
                t = put(t, x - r0, y - r0, z, 0x10fff0c0, false);
                t = put(t, x + r0, y + r0, z, 0x40ffe080, false);
                t = put(t, x + r0, y - r0, z, 0x20ffffff, true);
                frame_polys += 2;
            }
        }
    }

    frame_op_size = (v - frame_op) * sizeof(pvr_vertex_t);
    frame_tr_size = (t - frame_tr) * sizeof(pvr_vertex_t);
}

/* ------------------------------------------------------------------ */
/* The window: a spinning cube with a ring of translucent cards       */

static void project(float x, float y, float z, float *sx, float *sy,
                    float *sz) {
    float w = 1.0f / (z + 4.0f);

    *sx = WIN_X + WIN_W / 2 + x * w * 300.0f;
    *sy = WIN_Y + WIN_H / 2 + y * w * 300.0f;
    *sz = w * 10.0f;
}

static void rot(float *x, float *y, float *z, float ax, float ay) {
    float t;

    t = *y * cosf(ax) - *z * sinf(ax);
    *z = *y * sinf(ax) + *z * cosf(ax);
    *y = t;
    t = *x * cosf(ay) + *z * sinf(ay);
    *z = -*x * sinf(ay) + *z * cosf(ay);
    *x = t;
}

static void window_op(float time) {
    static const int faces[6][4] = {
        { 0, 1, 2, 3 }, { 5, 4, 7, 6 }, { 4, 0, 6, 2 },
        { 1, 5, 3, 7 }, { 4, 5, 0, 1 }, { 2, 3, 6, 7 }
    };
    static const uint32_t colors[6] = {
        0xffe04040, 0xff40e040, 0xff4040e0, 0xffe0e040, 0xffe040e0, 0xff40e0e0
    };
    float px[8], py[8], pz[8], x, y, z, nz;
    int i, f;

    /* Window background */
    vert(WIN_X, WIN_Y + WIN_H, 0.01f, 0xff102040, 0, 0, false);
    vert(WIN_X, WIN_Y, 0.01f, 0xff304870, 0, 0, false);
    vert(WIN_X + WIN_W, WIN_Y + WIN_H, 0.01f, 0xff081020, 0, 0, false);
    vert(WIN_X + WIN_W, WIN_Y, 0.01f, 0xff203050, 0, 0, true);

    for(i = 0; i < 8; i++) {
        x = (i & 1) ? 0.6f : -0.6f;
        y = (i & 2) ? 0.6f : -0.6f;
        z = (i & 4) ? 0.6f : -0.6f;
        rot(&x, &y, &z, time * 0.7f, time);
        project(x, y, z, &px[i], &py[i], &pz[i]);
    }

    for(f = 0; f < 6; f++) {
        const int *q = faces[f];

        /* Back-face cull by winding */
        nz = (px[q[1]] - px[q[0]]) * (py[q[2]] - py[q[0]]) -
             (py[q[1]] - py[q[0]]) * (px[q[2]] - px[q[0]]);

        if(nz > 0.0f)
            continue;

        for(i = 0; i < 4; i++)
            vert(px[q[i]], py[q[i]], pz[q[i]],
                 i == 1 ? 0xffffffff : colors[f], 0, 0, i == 3);
    }
}

static void window_tr(float time) {
    float a, x, y, z, sx[4], sy[4], sz[4];
    int i, k;

    for(i = 0; i < 12; i++) {
        for(k = 0; k < 4; k++) {
            a = time * 0.5f + (i + (k & 1) * 0.35f) * 2.0f * (float)M_PI / 12;
            x = 1.8f * cosf(a);
            z = 1.8f * sinf(a);
            y = (k & 2) ? 0.4f : -0.4f;
            rot(&x, &y, &z, 0.3f, 0.0f);
            project(x, y, z, &sx[k], &sy[k], &sz[k]);
        }

        vert(sx[0], sy[0], sz[0], 0x80ff8040, 0, 0, false);
        vert(sx[2], sy[2], sz[2], 0x40ffe0a0, 0, 0, false);
        vert(sx[1], sy[1], sz[1], 0x80ff8040, 0, 0, false);
        vert(sx[3], sy[3], sz[3], 0x40ffe0a0, 0, 0, true);
    }
}

/* ------------------------------------------------------------------ */

static bool full_redraw;    /* Redraw everything, every frame */
static int warmup;          /* Frames left to draw in full before skipping */
static bool fsaa;

static void setup(void) {
    pvr_init_params_t params = {
        { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0,
          PVR_BINSIZE_16 },
        1024 * 1024, 0, 0, 0, 3, 0
    };
    pvr_poly_cxt_t cxt;

    params.fsaa_enabled = fsaa;
    pvr_init(&params);
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    xs = fsaa ? 2.0f : 1.0f;

    /* The cube culls its back faces itself */
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&op_hdr, &cxt);
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(&tr_hdr, &cxt);
    pvr_poly_cxt_col(&cxt, PVR_LIST_PT_POLY);
    pvr_poly_compile(&pt_hdr, &cxt);

    font_init();
    frame_build();

    /* Draw everything into both frame buffers first */
    warmup = 2;
}

/* Choose the tiles for the next frame */
static void set_tiles(void) {
    bool partial = !full_redraw && !warmup;

    pvr_set_rect_enabled(0, 0, 640, 480, !partial);

    if(partial) {
        pvr_set_rect_enabled(WIN_X, WIN_Y, WIN_W, WIN_H, true);
        pvr_set_rect_enabled(0, HUD_Y, 640, 480 - HUD_Y, true);
    }
}

int main(int argc, char *argv[]) {
    maple_device_t *cont;
    cont_state_t *st;
    uint32_t buttons, prev = 0, pressed;
    pvr_stats_t stats;
    uint64_t t0, cpu_ns = 0, rnd_ns = 0;
    float time = 0.0f, cpu_avg = 0.0f, rnd_avg = 0.0f;
    bool send_frame;

    (void)argc;
    (void)argv;

    setup();
    printf("region_static_frame: %d polygons in the frame\n", frame_polys);

    for(;;) {
        buttons = 0;

        if((cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER)) &&
           (st = (cont_state_t *)maple_dev_status(cont)))
            buttons = st->buttons;

        pressed = buttons & ~prev;
        prev = buttons;

        if(pressed & CONT_START)
            break;

        if(pressed & CONT_A) {
            full_redraw = !full_redraw;
            warmup = 2;
        }

        if(pressed & CONT_Y) {
            pvr_wait_ready();
            pvr_shutdown();
            fsaa = !fsaa;
            setup();
        }

        set_tiles();
        send_frame = full_redraw || warmup;

        if(warmup)
            warmup--;

        pvr_wait_ready();
        pvr_get_stats(&stats);
        rnd_ns = stats.rnd_last_time;
        t0 = timer_ns_gettime64();

        pvr_scene_begin();

        pvr_list_begin(PVR_LIST_OP_POLY);

        if(send_frame)
            pvr_prim(frame_op, frame_op_size);

        pvr_prim(&op_hdr, sizeof(op_hdr));
        window_op(time);

        /* Black behind the status text */
        quad(0, HUD_Y, 640, 480, 0.01f, 0xff000000, 0, 0, 0, 0);
        pvr_list_finish();

        pvr_list_begin(PVR_LIST_TR_POLY);

        if(send_frame)
            pvr_prim(frame_tr, frame_tr_size);

        pvr_prim(&tr_hdr, sizeof(tr_hdr));
        window_tr(time);
        pvr_list_finish();

        pvr_list_begin(PVR_LIST_PT_POLY);
        pvr_prim(&font_hdr, sizeof(font_hdr));
        text_printf(8, HUD_Y + 4, 0xffffff00, "%s%s  frame: %d polygons",
                    full_redraw ? "FULL redraw" : "PARTIAL redraw",
                    fsaa ? " FSAA" : "", frame_polys);
        text_printf(8, HUD_Y + 32, 0xffffffff,
                    "PVR %5.2f ms  CPU %5.2f ms  A: mode  Y: FSAA",
                    (double)rnd_avg, (double)cpu_avg);
        pvr_list_finish();

        pvr_scene_finish();
        cpu_ns = timer_ns_gettime64() - t0;

        /* Smooth the numbers a little (no render time before the first) */
        cpu_avg += (cpu_ns / 1e6f - cpu_avg) * 0.1f;

        if(rnd_ns < 1000000000)
            rnd_avg += (rnd_ns / 1e6f - rnd_avg) * 0.1f;
        time += 1.0f / 60.0f;
    }

    pvr_wait_ready();
    pvr_shutdown();

    return 0;
}
