/* KallistiOS ##version##

   examples/dreamcast/pvr/region_hwtest/region_hwtest.c
   Copyright (C) 2026 Daniel Fairchild

   Hardware test for per-tile control of the PVR region array. Not an
   example: it answers questions that only real hardware can, by rendering
   test scenes and reading the frame buffer back with the CPU. Results go to
   the serial/dc-tool console and, at the end, to the screen.

   Q1  Mixed sort modes in one frame: is the presort bit applied per tile?
   Q2  Skipped tiles: do tiles left out of the array keep their pixels?
   Q3  Every tile skipped: does the frame complete without drawing? (The
       hardware hangs on an array holding only the dummy entry, so KOS
       doesn't start such a render; this checks that path.)
   Q4  Out-of-order tiles: does a reordered array render the same image?

   Build options: TESTS (bit mask, bit 0 = Q1 ... bit 3 = Q4; Q3 runs last
   because it may hang the PVR) and USE_FSAA (0 or 1).

   Every result line starts with "RESULT". Each question ends with a line
   "RESULT Qn ANSWER: ...".
*/

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dc/biosfont.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/pvr.h>
#include <dc/video.h>
#include <kos/timer.h>

#ifndef TESTS
#define TESTS 0xf
#endif

#ifndef USE_FSAA
#define USE_FSAA 0
#endif

/* Test area: tile rows 0-12 (y 0-415). Rows 13-14 hold the status text and
   are never checked. */
#define AREA_H      416
#define SCREEN_W    640

/* ------------------------------------------------------------------ */
/* Drawing                                                            */

#define FONT_TEX_W  256
#define FONT_TEX_H  256
#define FONT_CELL_W 16

static pvr_ptr_t font_tex;
static pvr_poly_hdr_t font_hdr, op_hdr, tr_hdr, pt_hdr;
static float xs = USE_FSAA ? 2.0f : 1.0f;
static int tile_cols, tile_rows;    /* PVR tile grid (FSAA doubles cols) */
static int tile_px;                 /* Screen pixels per tile column */

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

static void tri(float x0, float y0, float x1, float y1, float x2, float y2,
                float z, uint32_t argb) {
    pvr_vertex_t v;

    v.flags = PVR_CMD_VERTEX;
    v.z = z;
    v.argb = argb;
    v.oargb = 0;
    v.u = v.v = 0.0f;

    v.x = x0 * xs; v.y = y0;
    pvr_prim(&v, sizeof(v));
    v.x = x1 * xs; v.y = y1;
    pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX_EOL;
    v.x = x2 * xs; v.y = y2;
    pvr_prim(&v, sizeof(v));
}

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

/* Status lines shown at the bottom (tile rows 13-14) */
static char status[2][64];

/* Final results, shown on the screen at the end */
#define MAX_RESULTS 12
static char results[MAX_RESULTS][64];
static int result_count;

static void say(int line, const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vsnprintf(status[line], sizeof(status[line]), fmt, args);
    va_end(args);
    printf("STATUS %s\n", status[line]);
}

static void result(bool screen, const char *fmt, ...) {
    char buf[160];
    va_list args;
    size_t len;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printf("RESULT %s\n", buf);

    if(screen && result_count < MAX_RESULTS) {
        len = strlen(buf);

        if(len > sizeof(results[0]) - 1)
            len = sizeof(results[0]) - 1;

        memcpy(results[result_count], buf, len);
        results[result_count][len] = '\0';
        result_count++;
    }
}

static void setup(void) {
    pvr_init_params_t params = {
        { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0,
          PVR_BINSIZE_16 },
        1024 * 1024, 0, USE_FSAA, 0, 3, 0
    };
    uint16_t *buf;
    pvr_poly_cxt_t cxt;
    int c, i;

    pvr_init(&params);
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    pvr_get_tile_grid(&tile_cols, &tile_rows);
    tile_px = SCREEN_W / tile_cols;

    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    pvr_poly_compile(&op_hdr, &cxt);
    /* No depth writes: presorted layers blend in send order */
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(&tr_hdr, &cxt);
    pvr_poly_cxt_col(&cxt, PVR_LIST_PT_POLY);
    pvr_poly_compile(&pt_hdr, &cxt);

    buf = calloc(FONT_TEX_W * FONT_TEX_H, sizeof(uint16_t));

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

/* ------------------------------------------------------------------ */
/* Scenes                                                             */

enum {
    SCENE_SORT,         /* Q1: four translucent layers in scrambled order */
    SCENE_STATIC,       /* Q2-Q4: fixed pattern */
    SCENE_ANIM,         /* Q2-Q3: pattern that changes every frame */
    SCENE_RESULTS       /* End: results on the screen */
};

/* Q1 layers, in send order. Higher z is nearer. */
#define LAYERS 4
static const uint32_t layer_argb[LAYERS] = {
    0x80ff0000, 0x8000ff00, 0x800000ff, 0x80ffffff
};
static const float layer_z[LAYERS] = { 2.0f, 4.0f, 1.0f, 3.0f };

static uint32_t tile_color(int tx, int ty, int frame) {
    uint32_t h = (uint32_t)(tx * 73856093) ^ (uint32_t)(ty * 19349663) ^
                 (uint32_t)(frame * 83492791);

    h ^= h >> 13;
    h *= 0x5bd1e995;
    h ^= h >> 15;

    return 0xff000000 | (h & 0x00ffffff);
}

static void scene_pattern(int frame) {
    int tx, ty, x, y;

    /* One square per screen tile, with a smaller square and a triangle in
       other colors, so every tile differs from its neighbours */
    for(ty = 0; ty < AREA_H / 32; ty++) {
        for(tx = 0; tx < SCREEN_W / 32; tx++) {
            x = tx * 32;
            y = ty * 32;
            quad(x, y, x + 32, y + 32, 1.0f, tile_color(tx, ty, frame),
                 0, 0, 0, 0);
            quad(x + 4 + (tx % 5), y + 6 + (ty % 7), x + 20, y + 22, 2.0f,
                 tile_color(ty, tx, frame + 7), 0, 0, 0, 0);
            tri(x + 2, y + 30, x + 30, y + 30, x + 30, y + 2, 3.0f,
                tile_color(tx + 3, ty + 5, frame + 3));
        }
    }
}

static void draw_scene(int scene, int frame) {
    int i, y;

    pvr_wait_ready();
    pvr_scene_begin();

    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&op_hdr, sizeof(op_hdr));

    if(scene == SCENE_STATIC)
        scene_pattern(0);
    else if(scene == SCENE_ANIM)
        scene_pattern(frame + 1);

    /* Black backing for the status text */
    quad(0, AREA_H, SCREEN_W, 480, 0.5f, 0xff000000, 0, 0, 0, 0);
    pvr_list_finish();

    pvr_list_begin(PVR_LIST_TR_POLY);
    pvr_prim(&tr_hdr, sizeof(tr_hdr));

    if(scene == SCENE_SORT) {
        for(i = 0; i < LAYERS; i++)
            quad(0, 0, SCREEN_W, AREA_H, layer_z[i], layer_argb[i],
                 0, 0, 0, 0);
    }
    else if(scene == SCENE_STATIC) {
        /* Large translucent shapes crossing many tiles */
        tri(40, 20, 600, 120, 200, 400, 5.0f, 0x80ffffff);
        tri(620, 10, 100, 300, 500, 410, 6.0f, 0x6000ffff);
    }

    pvr_list_finish();

    pvr_list_begin(PVR_LIST_PT_POLY);
    pvr_prim(&font_hdr, sizeof(font_hdr));

    if(scene == SCENE_RESULTS) {
        for(i = 0, y = 16; i < result_count; i++, y += 28)
            text_draw(16, y, 10.0f, 0xffffffff, results[i]);
    }

    text_draw(8, AREA_H + 6, 10.0f, 0xffffff00, status[0]);
    text_draw(8, AREA_H + 34, 10.0f, 0xffc0c0c0, status[1]);
    pvr_list_finish();

    pvr_scene_finish();
}

static void draw_frames(int scene, int count, int *frame) {
    while(count--)
        draw_scene(scene, (*frame)++);
}

/* ------------------------------------------------------------------ */
/* Frame buffer readback (RGB565, the frame being displayed)          */

static void unpack565(uint16_t p, int *r, int *g, int *b) {
    *r = ((p >> 11) & 31) * 255 / 31;
    *g = ((p >> 5) & 63) * 255 / 63;
    *b = (p & 31) * 255 / 31;
}

/* Wait for the last scene to reach the screen */
static void settle(void) {
    pvr_wait_ready();
    pvr_wait_render_done();
    vid_waitvbl();
    vid_waitvbl();
}

/* Checksum of one tile column x row (in PVR tiles) of the displayed frame */
static uint32_t tile_sum(const uint16_t *fb, int tx, int ty) {
    uint32_t s = 2166136261u;
    int x, y;

    for(y = ty * 32; y < ty * 32 + 32; y++)
        for(x = tx * tile_px; x < (tx + 1) * tile_px; x++)
            s = (s ^ fb[y * SCREEN_W + x]) * 16777619u;

    return s;
}

/* ------------------------------------------------------------------ */
/* Region array inspection                                            */

/* Print a summary of the region array of the most recently started
   render. Checks what KOS wrote, on whatever runs this. */
static void array_summary(const char *label) {
    const volatile uint32_t *e;
    int n = 0, presort = 0, minx = 99, maxx = -1, miny = 99, maxy = -1;
    uint32_t addr = PVR_GET(PVR_ISP_TILEMAT_ADDR);

    e = (const volatile uint32_t *)(PVR_RAM_BASE + addr);

    /* Dummy entry first */
    printf("ARRAY %s at %08lx: dummy %08lx", label, addr, e[0]);

    if(!(e[0] & BIT(31))) {
        for(e += 6; n < 64 * 64; e += 6) {
            int x = (e[0] >> 2) & 63, y = (e[0] >> 8) & 63;

            n++;
            presort += !!(e[0] & BIT(29));
            if(x < minx) minx = x;
            if(x > maxx) maxx = x;
            if(y < miny) miny = y;
            if(y > maxy) maxy = y;

            if(e[0] & BIT(31))
                break;
        }
    }

    if(n)
        printf(", %d tile entries, %d presorted, x %d-%d, y %d-%d\n",
               n, presort, minx, maxx, miny, maxy);
    else
        printf(", no tile entries\n");
}

/* ------------------------------------------------------------------ */
/* Q1: mixed sort modes                                               */

static int exp_send[3], exp_depth[3];

static void q1_expected(void) {
    float c[3], src[3], a;
    int order[LAYERS], i, j, k, t;

    for(k = 0; k < 2; k++) {
        for(i = 0; i < LAYERS; i++)
            order[i] = i;

        if(k == 1) {
            /* Far to near */
            for(i = 0; i < LAYERS; i++)
                for(j = i + 1; j < LAYERS; j++)
                    if(layer_z[order[j]] < layer_z[order[i]]) {
                        t = order[i];
                        order[i] = order[j];
                        order[j] = t;
                    }
        }

        c[0] = c[1] = c[2] = 0.0f;

        for(i = 0; i < LAYERS; i++) {
            a = (float)(layer_argb[order[i]] >> 24) / 255.0f;
            src[0] = (float)((layer_argb[order[i]] >> 16) & 0xff);
            src[1] = (float)((layer_argb[order[i]] >> 8) & 0xff);
            src[2] = (float)(layer_argb[order[i]] & 0xff);

            for(j = 0; j < 3; j++)
                c[j] = a * src[j] + (1.0f - a) * c[j];
        }

        for(j = 0; j < 3; j++)
            (k ? exp_depth : exp_send)[j] = (int)c[j];
    }

    printf("Q1 expected: send order %d,%d,%d  depth order %d,%d,%d\n",
           exp_send[0], exp_send[1], exp_send[2],
           exp_depth[0], exp_depth[1], exp_depth[2]);
}

static int dist(int r, int g, int b, const int *e) {
    return abs(r - e[0]) + abs(g - e[1]) + abs(b - e[2]);
}

/* Classify every pixel of every tile of the displayed frame. Returns the
   number of tiles that don't match want(tx, ty) (true = presort). */
static int q1_check(const char *name, bool (*want)(int, int)) {
    static char map[16][65];
    const uint16_t *fb = vram_s;
    int tx, ty, x, y, r, g, b, ds, dd, send, depth, other, bad = 0;
    int worst = 0;

    for(ty = 0; ty < AREA_H / 32; ty++) {
        for(tx = 0; tx < tile_cols; tx++) {
            send = depth = other = 0;

            for(y = ty * 32; y < ty * 32 + 32; y++) {
                for(x = tx * tile_px; x < (tx + 1) * tile_px; x++) {
                    unpack565(fb[y * SCREEN_W + x], &r, &g, &b);
                    ds = dist(r, g, b, exp_send);
                    dd = dist(r, g, b, exp_depth);

                    if(ds < 40 && ds < dd)
                        send++;
                    else if(dd < 40)
                        depth++;
                    else {
                        other++;

                        if(ds > worst && dd > worst)
                            worst = ds < dd ? ds : dd;
                    }
                }
            }

            if(other == 0 && send && !depth)
                map[ty][tx] = 'P';
            else if(other == 0 && depth && !send)
                map[ty][tx] = 'a';
            else
                map[ty][tx] = '?';

            map[ty][tx + 1] = 0;

            if(map[ty][tx] != (want(tx, ty) ? 'P' : 'a'))
                bad++;
        }
    }

    result(false, "Q1 %-10s %s: %d of %d tiles wrong", name,
           bad ? "FAIL" : "ok", bad, tile_cols * (AREA_H / 32));

    if(bad) {
        printf("  map (P = send order, a = depth order, ? = mixed or "
               "neither, worst distance %d):\n", worst);

        for(ty = 0; ty < AREA_H / 32; ty++)
            printf("  %s\n", map[ty]);
    }

    return bad;
}

static bool want_all(int tx, int ty) { (void)tx; (void)ty; return true; }
static bool want_none(int tx, int ty) { (void)tx; (void)ty; return false; }
static bool want_left(int tx, int ty) { (void)ty; return tx < tile_cols / 2; }
static bool want_checker(int tx, int ty) { return (tx ^ ty) & 1; }
static bool want_random(int tx, int ty) {
    uint32_t h = (uint32_t)(tx * 2654435761u) ^ (uint32_t)(ty * 40503u);
    h ^= h >> 7;
    return (h >> 3) & 1;
}

static void q1(int *frame) {
    static const struct {
        const char *name;
        bool (*want)(int, int);
    } patterns[] = {
        { "all-pre", want_all },
        { "all-auto", want_none },
        { "left-pre", want_left },
        { "checker", want_checker },
        { "random", want_random },
    };
    int i, tx, ty, fails = 0, n = sizeof(patterns) / sizeof(patterns[0]);
    uint64_t t0, t1;

    q1_expected();

    for(i = 0; i < n; i++) {
        say(0, "Q1 mixed sort modes: pattern %s", patterns[i].name);

        if(patterns[i].want == want_all) {
            t0 = timer_ns_gettime64();
            pvr_set_presort_mode(true);
            t1 = timer_ns_gettime64();
            result(false, "Q1 array rewrite by pvr_set_presort_mode: %u us "
                   "(%d tiles)", (unsigned)((t1 - t0) / 1000),
                   tile_cols * tile_rows);
        }
        else if(patterns[i].want == want_none)
            pvr_set_presort_mode(false);
        else if(patterns[i].want == want_left) {
            pvr_set_rect_presort(0, 0, SCREEN_W / 2, 480, true);
            pvr_set_rect_presort(SCREEN_W / 2, 0, SCREEN_W / 2, 480, false);
        }
        else {
            for(ty = 0; ty < tile_rows; ty++)
                for(tx = 0; tx < tile_cols; tx++)
                    pvr_set_tile_presort(tx, ty, patterns[i].want(tx, ty));
        }

        draw_frames(SCENE_SORT, 6, frame);
        settle();
        array_summary(patterns[i].name);
        fails += q1_check(patterns[i].name, patterns[i].want) ? 1 : 0;
    }

    pvr_set_presort_mode(false);

    if(!fails)
        result(true, "Q1 ANSWER: YES, sort mode works per tile (%d/%d)",
               n, n);
    else
        result(true, "Q1 ANSWER: NO, %d of %d sort patterns wrong",
               fails, n);
}

/* ------------------------------------------------------------------ */
/* Q2: skipped tiles keep their pixels                                */

static bool q2_skipped(int tx, int ty) {
    int bx0 = 4 * 32 / tile_px, bx1 = 10 * 32 / tile_px - 1;

    /* A block in the middle and three single tiles at the corners */
    if(tx >= bx0 && tx <= bx1 && ty >= 3 && ty <= 7)
        return true;

    return (tx == 0 && ty == 0) || (tx == tile_cols - 1 && ty == 0) ||
           (tx == tile_cols - 1 && ty == AREA_H / 32 - 1);
}

#define Q2_FRAMES 200

static void q2(int *frame) {
    static uint32_t ref[16][64], prev[16][64];
    const uint16_t *fb;
    int tx, ty, i, bad_frames = 0, bad_tiles_max = 0, bad, live_changed = 0;
    int live, frozen_after = 0;
    pvr_stats_t st;
    uint64_t rnd_full = 0, rnd_skip = 0;

    say(0, "Q2 skipped tiles: static pattern into both buffers");
    pvr_set_rect_enabled(0, 0, SCREEN_W, 480, true);
    draw_frames(SCENE_STATIC, 6, frame);
    settle();

    fb = vram_s;

    for(ty = 0; ty < AREA_H / 32; ty++)
        for(tx = 0; tx < tile_cols; tx++)
            ref[ty][tx] = prev[ty][tx] = tile_sum(fb, tx, ty);

    /* Render time of the animated scene with every tile */
    draw_frames(SCENE_ANIM, 4, frame);
    settle();
    pvr_get_stats(&st);
    rnd_full = st.rnd_last_time;

    /* Back to the static picture in both buffers before skipping */
    draw_frames(SCENE_STATIC, 4, frame);
    settle();

    for(ty = 0; ty < AREA_H / 32; ty++)
        for(tx = 0; tx < tile_cols; tx++)
            if(q2_skipped(tx, ty))
                pvr_set_tile_enabled(tx, ty, false);

    say(0, "Q2 skipped tiles: animating around them, %d frames", Q2_FRAMES);

    for(i = 0; i < Q2_FRAMES; i++) {
        draw_scene(SCENE_ANIM, (*frame)++);

        if(i == 2)
            array_summary("q2-skipping");

        /* Read whichever buffer is on screen; over the run that is both */
        fb = vram_s;
        bad = 0;
        live = 0;

        for(ty = 0; ty < AREA_H / 32; ty++) {
            for(tx = 0; tx < tile_cols; tx++) {
                uint32_t s = tile_sum(fb, tx, ty);

                if(q2_skipped(tx, ty)) {
                    if(s != ref[ty][tx])
                        bad++;
                }
                else if(s != prev[ty][tx]) {
                    live++;
                }

                prev[ty][tx] = s;
            }
        }

        if(bad) {
            bad_frames++;

            if(bad > bad_tiles_max)
                bad_tiles_max = bad;
        }

        if(live)
            live_changed++;
    }

    settle();
    pvr_get_stats(&st);
    rnd_skip = st.rnd_last_time;

    result(false, "Q2 %d frames: skipped tiles changed in %d frames (max %d "
           "tiles), live tiles changed in %d frames", Q2_FRAMES, bad_frames,
           bad_tiles_max, live_changed);
    result(false, "Q2 render time all tiles %u us, with skipped tiles %u us",
           (unsigned)(rnd_full / 1000), (unsigned)(rnd_skip / 1000));

    /* Enable them again: they must start changing */
    pvr_set_rect_enabled(0, 0, SCREEN_W, 480, true);
    draw_frames(SCENE_ANIM, 6, frame);
    settle();
    fb = vram_s;

    for(ty = 0; ty < AREA_H / 32; ty++)
        for(tx = 0; tx < tile_cols; tx++)
            if(q2_skipped(tx, ty) && tile_sum(fb, tx, ty) == ref[ty][tx])
                frozen_after++;

    result(false, "Q2 after enabling again: %d skipped tiles still frozen",
           frozen_after);

    if(!bad_frames && live_changed >= Q2_FRAMES - 2 && !frozen_after)
        result(true, "Q2 ANSWER: YES, skipped tiles keep their pixels");
    else if(bad_frames)
        result(true, "Q2 ANSWER: NO, skipped tiles changed in %d frames",
               bad_frames);
    else
        result(true, "Q2 ANSWER: UNCLEAR, live %d/%d, frozen after %d",
               live_changed, Q2_FRAMES, frozen_after);
}

/* ------------------------------------------------------------------ */
/* Q3: every tile skipped                                             */

static uint32_t area_sum(const uint16_t *fb) {
    uint32_t s = 2166136261u;
    int i;

    for(i = 0; i < SCREEN_W * AREA_H; i++)
        s = (s ^ fb[i]) * 16777619u;

    return s;
}

static int q3_frames(int count, int *frame, uint32_t ref, int *changed,
                     uint64_t *rnd) {
    int i, timeouts = 0;
    pvr_stats_t st;

    *changed = 0;

    for(i = 0; i < count; i++) {
        draw_scene(SCENE_ANIM, (*frame)++);

        /* Wait for this scene's render to start, then to finish */
        if(pvr_wait_ready() < 0 || pvr_wait_render_done() < 0) {
            timeouts++;
            printf("Q3 frame %d: render did not complete in 100 ms\n", i);

            if(timeouts >= 3)
                break;
        }

        if(area_sum(vram_s) != ref)
            (*changed)++;
    }

    pvr_get_stats(&st);
    *rnd = st.rnd_last_time;

    return timeouts;
}

static bool q3(int *frame) {
    int timeouts, changed, empty_changed;
    uint32_t ref;
    uint64_t rnd;

    say(0, "Q3 all tiles skipped: static pattern into both buffers");
    pvr_set_rect_enabled(0, 0, SCREEN_W, 480, true);
    draw_frames(SCENE_STATIC, 6, frame);
    settle();
    ref = area_sum(vram_s);

    /* First the smallest non-empty array: one tile */
    say(0, "Q3 one tile enabled");
    pvr_set_rect_enabled(0, 0, SCREEN_W, 480, false);
    pvr_set_tile_enabled(0, 0, true);
    timeouts = q3_frames(30, frame, ref, &changed, &rnd);
    array_summary("q3-one-tile");
    result(false, "Q3 one tile: %d timeouts, render %u us",
           timeouts, (unsigned)(rnd / 1000));

    if(timeouts) {
        result(true, "Q3 ANSWER: ? one-tile array already fails");
        return false;
    }

    /* The static picture into both buffers again, since the one-tile
       frames changed tile 0,0 */
    pvr_set_rect_enabled(0, 0, SCREEN_W, 480, true);
    draw_frames(SCENE_STATIC, 6, frame);
    settle();
    ref = area_sum(vram_s);

    /* Then the empty array: only the dummy entry, marked last */
    say(0, "Q3 every tile skipped (dummy entry only)");
    printf("Q3 starting empty-array frames (a hang here answers Q3: NO)\n");
    pvr_set_rect_enabled(0, 0, SCREEN_W, 480, false);
    timeouts = q3_frames(60, frame, ref, &changed, &rnd);
    array_summary("q3-empty");
    result(false, "Q3 empty array: 60 frames, %d timeouts, %d frames "
           "changed the picture, render %u us", timeouts, changed,
           (unsigned)(rnd / 1000));

    if(timeouts) {
        result(true, "Q3 ANSWER: NO, empty array hangs the render");
        return false;
    }

    empty_changed = changed;

    /* And back */
    pvr_set_rect_enabled(0, 0, SCREEN_W, 480, true);
    timeouts = q3_frames(10, frame, ref, &changed, &rnd);
    result(false, "Q3 all tiles again: %d timeouts, %d of 10 frames changed",
           timeouts, changed);

    if(timeouts || changed < 8)
        result(true, "Q3 ANSWER: ? no hang, but no recovery");
    else if(!empty_changed)
        result(true, "Q3 ANSWER: YES, all-skipped frames done, nothing drawn");
    else
        result(true, "Q3 ANSWER: ? accepted, but drew in %d frames",
               empty_changed);

    return true;
}

/* ------------------------------------------------------------------ */
/* Q4: out-of-order tiles                                             */

#define MAX_ENTRIES (64 * 16 + 1)
static uint32_t entries[MAX_ENTRIES][6];

/* Read the array at addr, reorder its tile entries and write it back. */
static int reorder(uint32_t addr, int order) {
    volatile uint32_t *e = (volatile uint32_t *)(PVR_RAM_BASE + addr);
    static int idx[MAX_ENTRIES];
    int n, i, j, t, x, y;
    uint32_t seed = 12345;

    /* Entry 0 is the dummy; keep it first */
    for(n = 0; n < MAX_ENTRIES - 1; n++) {
        for(j = 0; j < 6; j++)
            entries[n][j] = e[6 + n * 6 + j];

        entries[n][0] &= ~BIT(31);

        if(e[6 + n * 6] & BIT(31)) {
            n++;
            break;
        }
    }

    for(i = 0; i < n; i++)
        idx[i] = i;

    if(order == 0) {
        /* Row order: y outer, x inner (KOS uses x outer, y inner) */
        for(i = 0; i < n; i++)
            for(j = i + 1; j < n; j++) {
                x = (entries[idx[i]][0] >> 2) & 63;
                y = (entries[idx[i]][0] >> 8) & 63;
                t = (entries[idx[j]][0] >> 8) & 63;

                if(t < y || (t == y && (int)((entries[idx[j]][0] >> 2) & 63) < x)) {
                    t = idx[i];
                    idx[i] = idx[j];
                    idx[j] = t;
                }
            }
    }
    else if(order == 1) {
        /* Reversed */
        for(i = 0; i < n; i++)
            idx[i] = n - 1 - i;
    }
    else {
        /* Shuffled */
        for(i = n - 1; i > 0; i--) {
            seed = seed * 1103515245 + 12345;
            j = (seed >> 8) % (i + 1);
            t = idx[i];
            idx[i] = idx[j];
            idx[j] = t;
        }
    }

    for(i = 0; i < n; i++)
        for(j = 0; j < 6; j++)
            e[6 + i * 6 + j] = entries[idx[i]][j] |
                               ((j == 0 && i == n - 1) ? BIT(31) : 0);

    return n;
}

static void q4(int *frame) {
    static const char *const names[] = { "row-order", "reversed", "shuffled" };
    static uint32_t ref[16][64];
    uint32_t addr[2];
    const uint16_t *fb;
    int o, tx, ty, bad, fails = 0, n = 0;

    say(0, "Q4 out-of-order tiles: reference picture");
    pvr_set_rect_enabled(0, 0, SCREEN_W, 480, true);
    pvr_set_presort_mode(false);
    draw_frames(SCENE_STATIC, 6, frame);
    settle();
    fb = vram_s;

    for(ty = 0; ty < AREA_H / 32; ty++)
        for(tx = 0; tx < tile_cols; tx++)
            ref[ty][tx] = tile_sum(fb, tx, ty);

    /* Find both arrays: the ones the last two renders used */
    draw_scene(SCENE_STATIC, (*frame)++);
    pvr_wait_ready();
    addr[0] = PVR_GET(PVR_ISP_TILEMAT_ADDR);
    draw_scene(SCENE_STATIC, (*frame)++);
    pvr_wait_ready();
    addr[1] = PVR_GET(PVR_ISP_TILEMAT_ADDR);
    printf("Q4 arrays at %08lx and %08lx\n", addr[0], addr[1]);

    for(o = 0; o < 3; o++) {
        say(0, "Q4 out-of-order tiles: %s", names[o]);

        /* Nothing renders or registers now; KOS rewrites the arrays only
           when the per-tile state changes, which it doesn't here */
        settle();
        n = reorder(addr[0], o);
        reorder(addr[1], o);

        draw_frames(SCENE_STATIC, 6, frame);
        settle();
        array_summary(names[o]);
        fb = vram_s;
        bad = 0;

        for(ty = 0; ty < AREA_H / 32; ty++)
            for(tx = 0; tx < tile_cols; tx++)
                if(tile_sum(fb, tx, ty) != ref[ty][tx])
                    bad++;

        result(false, "Q4 %-10s %d entries: %d tiles differ from KOS order",
               names[o], n, bad);
        fails += bad ? 1 : 0;
    }

    /* Let KOS write its own order again */
    pvr_set_tile_enabled(0, 0, false);
    pvr_set_tile_enabled(0, 0, true);

    if(!fails)
        result(true, "Q4 ANSWER: YES, tile order doesn't matter (3/3)");
    else
        result(true, "Q4 ANSWER: NO, %d of 3 orders render differently",
               fails);
}

/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    maple_device_t *cont;
    cont_state_t *st;
    int frame = 0, i;
    bool ok = true;

    (void)argc;
    (void)argv;

    setup();

    printf("region_hwtest: tests %x, FSAA %d, tile grid %dx%d, video %dx%d "
           "pm %d\n", TESTS, USE_FSAA, tile_cols, tile_rows,
           vid_mode->width, vid_mode->height, vid_mode->pm);
    result(true, "region_hwtest %s, grid %dx%d", USE_FSAA ? "FSAA" : "no FSAA",
           tile_cols, tile_rows);

    if(vid_mode->pm != PM_RGB565 || vid_mode->width != SCREEN_W) {
        result(true, "needs 640 wide RGB565 video, stopping");
        return 1;
    }

    say(1, "Results on serial, lines starting RESULT");

    if(TESTS & 1)
        q1(&frame);

    if(TESTS & 2)
        q2(&frame);

    if(TESTS & 8)
        q4(&frame);

    if(TESTS & 4)
        ok = q3(&frame);

    printf("RESULT done\n");

    if(!ok) {
        /* The PVR may be stuck; don't try to show anything */
        return 0;
    }

    /* Show the results until Start or 5 seconds, then exit (back to the
       loader) */
    pvr_set_rect_enabled(0, 0, SCREEN_W, 480, true);
    say(0, "Done. Start to exit.");

    for(i = 0; i < 5 * 60; i++) {
        if((cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER)) &&
           (st = (cont_state_t *)maple_dev_status(cont)) &&
           (st->buttons & CONT_START))
            break;

        draw_scene(SCENE_RESULTS, frame++);
    }

    pvr_wait_ready();
    pvr_shutdown();

    return 0;
}
