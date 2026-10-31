/* KallistiOS ##version##

   examples/dreamcast/pvr/region_multipass/region_multipass.c
   Copyright (C) 2026 Daniel Fairchild

   Hardware test for per-tile region array control in a two-pass scene. Not
   an example: it renders test scenes, reads the frame buffer back with the
   CPU, and checks the region array KOS wrote.

   T1  Skipped tiles: are they left out of both passes, keeping their pixels,
       while the other tiles show both passes?
   T2  Sort modes: does each pass of each tile sort as the last call to
       pvr_set_presort_mode(), pvr_set_pass_presort() or pvr_set_rect_presort()
       said? (Pass 0 draws the top of the screen, pass 1 the bottom.)
   T3  One tile enabled, then none: does the frame complete? The last entry
       of the array must be able to write out, or the render never ends.

   Build option: USE_FSAA (0 or 1).

   Every result line starts with "RESULT". The test exits back to the loader.
*/

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <dc/pvr.h>
#include <dc/video.h>
#include <kos/timer.h>

#ifndef USE_FSAA
#define USE_FSAA 0
#endif

/* Tile rows 0-14. The sort test's bands meet at tile row 7. */
#define SCREEN_W    640
#define SCREEN_H    480
#define BAND_ROW    7

/* Region array control word bits, as KOS writes them */
#define RA_LAST         BIT(31)
#define RA_ZKEEP        BIT(30)
#define RA_PRESORT      BIT(29)
#define RA_NO_WRITEOUT  BIT(28)
#define RA_ENTRY_WORDS  6

#define PASSES 2

static pvr_poly_hdr_t op_hdr, tr_hdr;
static float xs = USE_FSAA ? 2.0f : 1.0f;
static int tile_cols, tile_rows;    /* PVR tile grid (FSAA doubles cols) */
static int tile_px;                 /* Screen pixels per tile column */

static uint32_t tile_ref[64][64];

static void quad(float x0, float y0, float x1, float y1, float z, uint32_t c) {
    pvr_vertex_t v __attribute__((aligned(32)));

    v.flags = PVR_CMD_VERTEX;
    v.z = z;
    v.argb = c;
    v.oargb = 0;
    v.u = v.v = 0.0f;

    v.x = x0 * xs; v.y = y1;
    pvr_prim(&v, sizeof(v));
    v.x = x0 * xs; v.y = y0;
    pvr_prim(&v, sizeof(v));
    v.x = x1 * xs; v.y = y1;
    pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX_EOL;
    v.x = x1 * xs; v.y = y0;
    pvr_prim(&v, sizeof(v));
}

/* Every tile shows both passes: the top half of each tile row is pass 0's
   colour, the bottom half pass 1's. */
static void scene_tiles(uint32_t under, uint32_t over) {
    int y;

    pvr_wait_ready();
    pvr_scene_begin();

    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&op_hdr, sizeof(op_hdr));
    quad(0, 0, SCREEN_W, SCREEN_H, 1.0f, under);
    pvr_list_finish();

    pvr_scene_next_pass();

    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&op_hdr, sizeof(op_hdr));

    for(y = 16; y < SCREEN_H; y += 32)
        quad(0, y, SCREEN_W, y + 16, 2.0f, over);

    pvr_list_finish();

    pvr_scene_finish();
}

/* Two translucent layers, the nearer one sent first. Presorted, the farther
   one ends up on top; autosorted, the nearer one does. */
static void sort_layers(float y0, float y1) {
    pvr_list_begin(PVR_LIST_TR_POLY);
    pvr_prim(&tr_hdr, sizeof(tr_hdr));
    quad(0, y0, SCREEN_W, y1, 4.0f, 0x80ff0000);
    quad(0, y0, SCREEN_W, y1, 2.0f, 0x8000ff00);
    pvr_list_finish();
}

static void scene_sort(void) {
    pvr_wait_ready();
    pvr_scene_begin();

    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&op_hdr, sizeof(op_hdr));
    quad(0, 0, SCREEN_W, SCREEN_H, 1.0f, 0xff000000);
    pvr_list_finish();

    sort_layers(0, BAND_ROW * 32);

    pvr_scene_next_pass();

    sort_layers(BAND_ROW * 32, SCREEN_H);

    pvr_scene_finish();
}

/* Wait for the last scene to reach the screen. Returns false on a timeout. */
static bool settle(void) {
    bool ok = pvr_wait_ready() >= 0 && pvr_wait_render_done() >= 0;

    vid_waitvbl();
    vid_waitvbl();

    return ok;
}

static uint32_t tile_sum(int tx, int ty) {
    const uint16_t *fb = vram_s;
    uint32_t s = 2166136261u;
    int x, y;

    for(y = ty * 32; y < ty * 32 + 32; y++)
        for(x = tx * tile_px; x < (tx + 1) * tile_px; x++)
            s = (s ^ fb[y * SCREEN_W + x]) * 16777619u;

    return s;
}

/* Sum of the channel differences between a displayed pixel and an RGB565
   colour. */
static int pixel_diff(int x, int y, uint16_t want) {
    uint16_t got = ((const uint16_t *)vram_s)[y * SCREEN_W + x];

    return abs((got >> 11) - (want >> 11)) +
           abs(((got >> 5) & 63) - ((want >> 5) & 63)) +
           abs((got & 31) - (want & 31));
}

static uint16_t rgb565(uint32_t argb) {
    return ((argb >> 8) & 0xf800) | ((argb >> 5) & 0x07e0) |
           ((argb >> 3) & 0x001f);
}

/* Check the array of the last render against what's asked for. Returns the
   number of wrong entries. */
static int array_check(bool (*skipped)(int, int),
                       const bool want[PASSES][2], int *entries) {
    const volatile uint32_t *e;
    uint32_t word, expect;
    int x, y, p, bad = 0, n = 0, last = 0;
    bool presort;

    e = (const volatile uint32_t *)(PVR_RAM_BASE +
                                    PVR_GET(PVR_ISP_TILEMAT_ADDR));
    e += RA_ENTRY_WORDS;    /* Dummy entry */

    for(x = 0; x < tile_cols; x++) {
        for(y = 0; y < tile_rows; y++) {
            if(skipped && skipped(x, y))
                continue;

            for(p = 0; p < PASSES; p++) {
                presort = want[p][x * tile_px >= SCREEN_W / 2];
                expect = ((uint32_t)y << 8) | ((uint32_t)x << 2) |
                         (p ? RA_ZKEEP : RA_NO_WRITEOUT) |
                         (presort ? RA_PRESORT : 0);
                word = e[0];

                if(word & RA_LAST)
                    last++;

                if((word & ~RA_LAST) != expect) {
                    if(bad++ < 3)
                        printf("  tile %d,%d pass %d: %08lx, wanted %08lx\n",
                               x, y, p, (unsigned long)word,
                               (unsigned long)expect);
                }

                e += RA_ENTRY_WORDS;
                n++;
            }
        }
    }

    /* Exactly one end mark, on the final entry */
    if(last != 1 || !n || !(e[-RA_ENTRY_WORDS] & RA_LAST))
        bad++;

    *entries = n;

    return bad;
}

/* ------------------------------------------------------------------ */

static bool t1_skipped(int tx, int ty) {
    return ((tx ^ ty) & 1) != 0;
}

static bool t1(void) {
    static const bool none[PASSES][2] = { { true, false }, { true, false } };
    static const uint32_t under[2] = { 0xff00ff00, 0xffffff00 };
    static const uint32_t over[2] = { 0xffff00ff, 0xff00ffff };
    int tx, ty, i, kept = 0, changed = 0, bad, entries, timeouts = 0;
    int bad_kept = 0, bad_new = 0, tiles = 0, expect;
    uint16_t top, bottom;
    int cx, y;
    bool ok;

    pvr_set_presort_mode(false);
    pvr_set_rect_presort(0, 0, SCREEN_W / 2, SCREEN_H, true);

    for(i = 0; i < 6; i++)
        scene_tiles(0xffff0000, 0xff0000ff);

    if(!settle())
        timeouts++;

    /* Both passes must show everywhere before anything is skipped */
    for(tx = 0; tx < tile_cols; tx++) {
        for(ty = 0; ty < tile_rows; ty++) {
            cx = tx * tile_px + tile_px / 2;
            y = ty * 32;

            if(pixel_diff(cx, y + 8, rgb565(0xffff0000)) > 8 ||
               pixel_diff(cx, y + 24, rgb565(0xff0000ff)) > 8)
                bad_new++;

            tile_ref[tx][ty] = tile_sum(tx, ty);
        }
    }

    printf("RESULT T1 before skipping: %d tiles without both passes\n",
           bad_new);

    bad_new = 0;

    for(tx = 0; tx < tile_cols; tx++)
        for(ty = 0; ty < tile_rows; ty++)
            if(t1_skipped(tx, ty)) {
                pvr_set_tile_enabled(tx, ty, false);
                tiles++;
            }

    /* A new picture in every frame, read back from each frame buffer */
    for(i = 0; i < 12; i++) {
        top = rgb565(under[i & 1]);
        bottom = rgb565(over[i & 1]);

        scene_tiles(under[i & 1], over[i & 1]);

        if(!settle())
            timeouts++;

        for(tx = 0; tx < tile_cols; tx++) {
            for(ty = 0; ty < tile_rows; ty++) {
                if(t1_skipped(tx, ty)) {
                    if(tile_sum(tx, ty) == tile_ref[tx][ty])
                        kept++;
                    else
                        bad_kept++;
                }
                else {
                    cx = tx * tile_px + tile_px / 2;
                    y = ty * 32;

                    if(pixel_diff(cx, y + 8, top) <= 8 &&
                       pixel_diff(cx, y + 24, bottom) <= 8)
                        changed++;
                    else
                        bad_new++;
                }
            }
        }
    }

    bad = array_check(t1_skipped, none, &entries);
    expect = (tile_cols * tile_rows - tiles) * PASSES;

    ok = !timeouts && !bad_kept && !bad_new && !bad && entries == expect;

    printf("RESULT T1 %s: %d tile frames kept, %d changed; %d skipped tiles "
           "drawn on, %d others wrong; array %d entries (wanted %d), %d "
           "wrong; %d timeouts\n", ok ? "PASS" : "FAIL", kept, changed,
           bad_kept, bad_new, entries, expect, bad, timeouts);

    pvr_set_rect_enabled(0, 0, SCREEN_W, SCREEN_H, true);

    return ok;
}

/* ------------------------------------------------------------------ */

#define T2_CASES 6

static const struct {
    const char *name;
    bool want[PASSES][2];   /* Presort of [pass][left, right] */
} t2_cases[T2_CASES] = {
    { "all autosort",              { { false, false }, { false, false } } },
    { "all presort",               { { true,  true  }, { true,  true  } } },
    { "rect left",                 { { true,  false }, { true,  false } } },
    { "rect left, pass 0 auto",    { { false, false }, { true,  false } } },
    { "rect left, pass 1 pre",     { { true,  false }, { true,  true  } } },
    { "pass 0 auto, rect right",   { { false, true  }, { true,  true  } } },
};

static void t2_apply(int c) {
    switch(c) {
        case 0:
            pvr_set_presort_mode(false);
            break;
        case 1:
            pvr_set_presort_mode(true);
            break;
        case 2:
        case 3:
        case 4:
            pvr_set_presort_mode(false);
            pvr_set_rect_presort(0, 0, SCREEN_W / 2, SCREEN_H, true);

            if(c == 3)
                pvr_set_pass_presort(0, false);
            else if(c == 4)
                pvr_set_pass_presort(1, true);

            break;
        case 5:
            /* The rectangle comes last, so it wins over the pass */
            pvr_set_presort_mode(true);
            pvr_set_pass_presort(0, false);
            pvr_set_rect_presort(SCREEN_W / 2, 0, SCREEN_W / 2, SCREEN_H,
                                 true);
            break;
    }
}

static bool t2(void) {
    uint16_t refs[2] = { 0, 0 };    /* Displayed colour of [autosort, presort] */
    int c, i, tx, ty, p, side, bad, wrong, entries, x, y;
    bool ok = true, want;

    for(c = 0; c < T2_CASES; c++) {
        t2_apply(c);

        for(i = 0; i < 4; i++)
            scene_sort();

        if(!settle())
            ok = false;

        /* The first two cases give the reference colours */
        if(c < 2)
            refs[c] = ((const uint16_t *)vram_s)[16 * SCREEN_W + tile_px / 2];

        wrong = 0;

        for(tx = 0; tx < tile_cols; tx++) {
            for(ty = 0; ty < tile_rows; ty++) {
                x = tx * tile_px + tile_px / 2;
                y = ty * 32 + 16;
                p = ty >= BAND_ROW;
                side = x >= SCREEN_W / 2;
                want = t2_cases[c].want[p][side];

                if(c < 2 && want != (bool)c)
                    wrong++;
                else if(pixel_diff(x, y, refs[want ? 1 : 0]) > 8)
                    wrong++;
            }
        }

        bad = array_check(NULL, t2_cases[c].want, &entries);

        if(wrong || bad)
            ok = false;

        printf("RESULT T2 %s: %d tiles sorted wrong, %d array entries wrong "
               "(%d entries)\n", t2_cases[c].name, wrong, bad, entries);
    }

    printf("RESULT T2 reference colours: autosort %04x, presort %04x\n",
           refs[0], refs[1]);

    if(refs[0] == refs[1])
        ok = false;

    printf("RESULT T2 %s\n", ok ? "PASS" : "FAIL");

    pvr_set_presort_mode(false);

    return ok;
}

/* ------------------------------------------------------------------ */

/* Returns the number of frames that didn't complete, and counts the frames
   in which tile 0,0 and some other tile changed. */
static int t3_frames(int count, int *first, int *others) {
    int i, tx, ty, timeouts = 0;
    bool moved;

    *first = *others = 0;

    for(i = 0; i < count; i++) {
        scene_tiles(0xff000000 | (0x010101 * (i * 8 + 16)), 0xff808080);

        if(pvr_wait_ready() < 0 || pvr_wait_render_done() < 0) {
            timeouts++;
            printf("  frame %d did not complete in 100 ms\n", i);

            if(timeouts >= 3)
                break;
        }

        vid_waitvbl();
        vid_waitvbl();

        if(tile_sum(0, 0) != tile_ref[0][0])
            (*first)++;

        moved = false;

        for(tx = 1; tx < tile_cols && !moved; tx++)
            for(ty = 0; ty < tile_rows && !moved; ty++)
                moved = tile_sum(tx, ty) != tile_ref[tx][ty];

        if(moved)
            (*others)++;
    }

    return timeouts;
}

static void t3_static(void) {
    int i, tx, ty;

    pvr_set_rect_enabled(0, 0, SCREEN_W, SCREEN_H, true);

    for(i = 0; i < 6; i++)
        scene_tiles(0xff00ff00, 0xffff0000);

    settle();

    for(tx = 0; tx < tile_cols; tx++)
        for(ty = 0; ty < tile_rows; ty++)
            tile_ref[tx][ty] = tile_sum(tx, ty);
}

static bool t3(void) {
    int timeouts, first, others, total = 0;
    bool ok = true;

    t3_static();

    pvr_set_rect_enabled(0, 0, SCREEN_W, SCREEN_H, false);
    pvr_set_tile_enabled(0, 0, true);
    timeouts = t3_frames(30, &first, &others);
    total += timeouts;
    printf("RESULT T3 one tile: %d timeouts, tile changed in %d of 30 frames, "
           "others in %d\n", timeouts, first, others);

    if(timeouts || first < 20 || others)
        ok = false;

    if(!timeouts) {
        t3_static();

        pvr_set_rect_enabled(0, 0, SCREEN_W, SCREEN_H, false);
        timeouts = t3_frames(60, &first, &others);
        total += timeouts;
        printf("RESULT T3 no tiles: %d timeouts, changed in %d and %d of 60 "
               "frames\n", timeouts, first, others);

        if(timeouts || first || others)
            ok = false;
    }

    if(!total) {
        pvr_set_rect_enabled(0, 0, SCREEN_W, SCREEN_H, true);
        timeouts = t3_frames(10, &first, &others);
        printf("RESULT T3 all tiles again: %d timeouts, changed in %d of 10 "
               "frames\n", timeouts, others);

        if(timeouts || others < 8)
            ok = false;
    }

    printf("RESULT T3 %s\n", ok ? "PASS" : "FAIL");

    return ok;
}

/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    pvr_init_params_t params = {
        .opb_sizes = { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16,
                       PVR_BINSIZE_0, PVR_BINSIZE_0 },
        .vertex_buf_size = 512 * 1024,
        .fsaa_enabled = USE_FSAA,
        .opb_overflow_count = 3,
        .pass_count = PASSES,
    };
    pvr_poly_cxt_t cxt;
    bool ok;

    (void)argc;
    (void)argv;

    if(pvr_init(&params) < 0) {
        printf("RESULT pvr_init failed\n");
        return 1;
    }

    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    pvr_get_tile_grid(&tile_cols, &tile_rows);
    tile_px = SCREEN_W / tile_cols;

    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    pvr_poly_compile(&op_hdr, &cxt);

    /* No depth writes: presorted layers blend in send order */
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(&tr_hdr, &cxt);

    printf("RESULT region_multipass: FSAA %d, grid %dx%d, video %dx%d pm %d\n",
           USE_FSAA, tile_cols, tile_rows, vid_mode->width, vid_mode->height,
           vid_mode->pm);

    if(vid_mode->pm != PM_RGB565 || vid_mode->width != SCREEN_W) {
        printf("RESULT needs 640 wide RGB565 video, stopping\n");
        return 1;
    }

    ok = t1();
    ok = t2() && ok;
    ok = t3() && ok;

    printf("RESULT %s\n", ok ? "ALL PASS" : "FAILED");
    printf("RESULT done\n");

    pvr_wait_ready();
    pvr_shutdown();

    return 0;
}
