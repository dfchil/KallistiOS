/* KallistiOS ##version##

   examples/dreamcast/pvr/cache_multipass/cache_multipass.c
   Copyright (C) 2026 Daniel Fairchild

   Hardware test for cached object lists together with multipass rendering,
   skipped tiles and the background plane. Not an example: it renders test
   scenes, reads the frame buffer back with the CPU, and checks the region
   array KOS wrote.

   T1  A cache in the punch-through slot of a two-pass scene: is it drawn in
       pass 0 alone, under pass 1?
   T2  Does the attachment survive the region array being rebuilt for skipped
       tiles and sort modes, leave skipped tiles alone, and end on detach?
   T3  A frame from a cache alone in a two-pass setup: does it show the
       gradient and the textured background as a TA frame does, and do TA
       frames work afterwards?
   T4  Does pvr_scene_finish_cached() refuse a scene past pass 0?

   Build option: USE_FSAA (0 or 1).

   Every result line starts with "RESULT". The test exits back to the loader.
*/

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <dc/pvr.h>
#include <dc/video.h>

#ifndef USE_FSAA
#define USE_FSAA 0
#endif

#define SCREEN_W    640
#define SCREEN_H    480
#define PASSES      2

#define RED         0xffff0000
#define GREEN       0xff00ff00
#define BLUE        0xff0000ff

#define RA_ENTRY_WORDS  6
#define RA_LIST_EMPTY   0x80000000
#define TXR_SIZE        64

static pvr_poly_hdr_t op_hdr, pt_hdr;
static float xs = USE_FSAA ? 2.0f : 1.0f;
static int tile_cols, tile_rows;    /* PVR tile grid (FSAA doubles cols) */
static int tile_px;                 /* Screen pixels per tile column */

static pvr_cache_t *cache;
static pvr_cache_info_t info;

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

/* The cached polygon: green over the left half of the screen. */
static void cache_send(void *data) {
    (void)data;

    pvr_list_begin(PVR_LIST_PT_POLY);
    pvr_prim(&pt_hdr, sizeof(pt_hdr));
    quad(0, 0, SCREEN_W / 2, SCREEN_H, 2.0f, GREEN);
    pvr_list_finish();
}

/* Pass 0 is red, with the cache on top of it. Pass 1 is blue, over the
   bottom half of each tile row, in front of both. */
static void scene_passes(void) {
    int y;

    pvr_wait_ready();
    pvr_scene_begin();

    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&op_hdr, sizeof(op_hdr));
    quad(0, 0, SCREEN_W, SCREEN_H, 1.0f, RED);
    pvr_list_finish();

    pvr_scene_next_pass();

    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&op_hdr, sizeof(op_hdr));

    for(y = 16; y < SCREEN_H; y += 32)
        quad(0, y, SCREEN_W, y + 16, 3.0f, BLUE);

    pvr_list_finish();

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

static uint16_t rgb565(uint32_t argb) {
    return ((argb >> 8) & 0xf800) | ((argb >> 5) & 0x07e0) |
           ((argb >> 3) & 0x001f);
}

static int pixel_diff(int x, int y, uint16_t want) {
    uint16_t got = ((const uint16_t *)vram_s)[y * SCREEN_W + x];

    return abs((got >> 11) - (want >> 11)) +
           abs(((got >> 5) & 63) - ((want >> 5) & 63)) +
           abs((got & 31) - (want & 31));
}

static bool checker(int tx, int ty) {
    return ((tx ^ ty) & 1) != 0;
}

/* Check the punch-through pointers of the array of the last render: pass 0
   holds the cache if attached, every other entry is empty. Returns the
   number of wrong entries. */
static int slot_check(bool (*skipped)(int, int), bool attached) {
    const volatile uint32_t *e;
    uint32_t addr, want;
    int x, y, p, h, bad = 0;

    addr = PVR_GET(PVR_ISP_TILEMAT_ADDR);
    h = addr >= 0x400000;
    e = (const volatile uint32_t *)(PVR_RAM_BASE + addr);
    e += RA_ENTRY_WORDS;    /* Dummy entry */

    for(x = 0; x < tile_cols; x++) {
        for(y = 0; y < tile_rows; y++) {
            if(skipped && skipped(x, y))
                continue;

            for(p = 0; p < PASSES; p++) {
                want = RA_LIST_EMPTY;

                if(attached && !p)
                    want = info.lists[h][PVR_LIST_PT_POLY] +
                           info.list_block_size[PVR_LIST_PT_POLY] *
                           (tile_cols * y + x);

                if(e[1 + PVR_LIST_PT_POLY] != want)
                    bad++;

                e += RA_ENTRY_WORDS;
            }
        }
    }

    return bad;
}

/* Count tiles that don't show the two-pass picture. With cached, the left
   half has the cache under pass 1. Skipped tiles aren't looked at. */
static int picture_check(bool (*skipped)(int, int), bool cached) {
    int tx, ty, x, y, bad = 0;
    uint16_t top;

    for(tx = 0; tx < tile_cols; tx++) {
        for(ty = 0; ty < tile_rows; ty++) {
            if(skipped && skipped(tx, ty))
                continue;

            x = tx * tile_px + tile_px / 2;
            y = ty * 32;
            top = rgb565((cached && x < SCREEN_W / 2) ? GREEN : RED);

            if(pixel_diff(x, y + 8, top) > 8 ||
               pixel_diff(x, y + 24, rgb565(BLUE)) > 8)
                bad++;
        }
    }

    return bad;
}

static void frames(int count) {
    while(count--)
        scene_passes();

    settle();
}

static bool t1(void) {
    int pic, arr;
    bool ok;

    if(pvr_cache_attach(cache, PVR_CACHE_SLOT(PVR_LIST_PT_POLY)) < 0) {
        printf("RESULT T1 FAIL: pvr_cache_attach\n");
        return false;
    }

    frames(4);
    pic = picture_check(NULL, true);
    arr = slot_check(NULL, true);
    ok = !pic && !arr;

    printf("RESULT T1 %s: %d tiles wrong, %d array entries wrong\n",
           ok ? "PASS" : "FAIL", pic, arr);

    return ok;
}

static bool t2(void) {
    int tx, ty, i, pic, arr, moved = 0;
    bool ok = true;

    /* Skipped tiles: the picture before them, which is the red, green and
       blue one, stays there. A new sort mode rebuilds the array as well. */
    for(tx = 0; tx < tile_cols; tx++)
        for(ty = 0; ty < tile_rows; ty++)
            tile_ref[tx][ty] = tile_sum(tx, ty);

    for(tx = 0; tx < tile_cols; tx++)
        for(ty = 0; ty < tile_rows; ty++)
            if(checker(tx, ty))
                pvr_set_tile_enabled(tx, ty, false);

    pvr_set_rect_presort(0, 0, SCREEN_W / 2, SCREEN_H, true);

    for(i = 0; i < 8; i++) {
        scene_passes();
        settle();

        for(tx = 0; tx < tile_cols; tx++)
            for(ty = 0; ty < tile_rows; ty++)
                if(checker(tx, ty) && tile_sum(tx, ty) != tile_ref[tx][ty])
                    moved++;
    }

    pic = picture_check(checker, true);
    arr = slot_check(checker, true);
    printf("RESULT T2 skipped and resorted: %d tiles wrong, %d array entries "
           "wrong, %d skipped tile frames changed\n", pic, arr, moved);

    if(pic || arr || moved)
        ok = false;

    pvr_set_rect_enabled(0, 0, SCREEN_W, SCREEN_H, true);
    pvr_set_presort_mode(false);
    frames(4);

    pic = picture_check(NULL, true);
    arr = slot_check(NULL, true);
    printf("RESULT T2 back: %d tiles wrong, %d array entries wrong\n", pic,
           arr);

    if(pic || arr)
        ok = false;

    /* The new picture has the cache gone from pass 0 */
    pvr_cache_attach(NULL, PVR_CACHE_SLOT(PVR_LIST_PT_POLY));
    frames(4);

    pic = picture_check(NULL, false);
    arr = slot_check(NULL, false);
    printf("RESULT T2 detached: %d tiles wrong, %d array entries wrong\n", pic,
           arr);

    if(pic || arr)
        ok = false;

    printf("RESULT T2 %s\n", ok ? "PASS" : "FAIL");

    return ok;
}

/* An empty TA scene shows the background alone. */
static void scene_empty(void) {
    pvr_wait_ready();
    pvr_scene_begin();
    pvr_scene_finish();
}

static void scene_cached(void) {
    pvr_wait_ready();
    pvr_scene_begin();
    pvr_scene_finish_cached(cache);
}

/* Compare the right half, which the cache doesn't touch, of frames from the
   cache with the background shown by the TA path. Returns how many tile
   frames differ, and counts the left half tiles that aren't the cache's. */
static int t3_compare(int *left) {
    int tx, ty, i, diff = 0, x;

    for(i = 0; i < 4; i++)
        scene_empty();

    settle();

    for(tx = 0; tx < tile_cols; tx++)
        for(ty = 0; ty < tile_rows; ty++)
            tile_ref[tx][ty] = tile_sum(tx, ty);

    *left = 0;

    for(i = 0; i < 6; i++) {
        scene_cached();
        settle();

        for(tx = 0; tx < tile_cols; tx++) {
            for(ty = 0; ty < tile_rows; ty++) {
                x = tx * tile_px + tile_px / 2;

                if(x >= SCREEN_W / 2) {
                    if(tile_sum(tx, ty) != tile_ref[tx][ty])
                        diff++;
                }
                else if(pixel_diff(x, ty * 32 + 16, rgb565(GREEN)) > 8)
                    (*left)++;
            }
        }
    }

    return diff;
}

static bool t3(pvr_ptr_t txr) {
    int diff, left, pic, x, y, varied = 0;
    uint16_t first;
    bool ok = true;

    pvr_set_bg_colors(RED, GREEN, BLUE);
    diff = t3_compare(&left);

    /* The reference must be a gradient for this to mean anything */
    scene_empty();
    settle();
    first = ((const uint16_t *)vram_s)[8 * SCREEN_W + SCREEN_W / 2 + 8];

    for(y = 8; y < SCREEN_H; y += 64)
        for(x = SCREEN_W / 2 + 8; x < SCREEN_W; x += 64)
            if(((const uint16_t *)vram_s)[y * SCREEN_W + x] != first)
                varied++;

    printf("RESULT T3 gradient: %d tile frames differ from the TA's, %d left "
           "tiles wrong, %d samples vary\n", diff, left, varied);

    if(diff || left || !varied)
        ok = false;

    pvr_set_bg_texture(txr, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                       TXR_SIZE, TXR_SIZE, PVR_FILTER_NEAREST);
    diff = t3_compare(&left);
    printf("RESULT T3 textured: %d tile frames differ from the TA's, %d left "
           "tiles wrong\n", diff, left);

    if(diff || left)
        ok = false;

    pvr_set_bg_texture(NULL, 0, 0, 0, PVR_FILTER_NEAREST);
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);

    /* And TA frames with both passes afterwards */
    frames(4);
    pic = picture_check(NULL, false);
    printf("RESULT T3 two-pass TA frames afterwards: %d tiles wrong\n", pic);

    if(pic)
        ok = false;

    printf("RESULT T3 %s\n", ok ? "PASS" : "FAIL");

    return ok;
}

static bool t4(void) {
    int rc;

    pvr_wait_ready();
    pvr_scene_begin();
    pvr_scene_next_pass();
    rc = pvr_scene_finish_cached(cache);
    pvr_scene_finish();
    settle();

    printf("RESULT T4 %s: pvr_scene_finish_cached() in pass 1 returned %d\n",
           rc < 0 ? "PASS" : "FAIL", rc);

    return rc < 0;
}

int main(int argc, char *argv[]) {
    pvr_init_params_t params = {
        .opb_sizes = { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_0,
                       PVR_BINSIZE_0, PVR_BINSIZE_0 },
        .vertex_buf_size = 512 * 1024,
        .fsaa_enabled = USE_FSAA,
        .opb_overflow_count = 3,
        .pass_count = PASSES,
        .cached_list_size = 256 * 1024,
    };
    pvr_cache_params_t cache_params = {
        .opb_sizes = { PVR_BINSIZE_0, PVR_BINSIZE_0, PVR_BINSIZE_0,
                       PVR_BINSIZE_0, PVR_BINSIZE_16 },
        .vertex_buf_size = 16 * 1024,
        .opb_overflow_count = 1,
    };
    pvr_poly_cxt_t cxt;
    pvr_ptr_t txr;
    uint16_t *pixels;
    int i;
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
    pvr_poly_cxt_col(&cxt, PVR_LIST_PT_POLY);
    pvr_poly_compile(&pt_hdr, &cxt);

    /* A texture that differs from the plain background in every texel */
    pixels = malloc(TXR_SIZE * TXR_SIZE * 2);
    txr = pvr_mem_malloc(TXR_SIZE * TXR_SIZE * 2);

    for(i = 0; i < TXR_SIZE * TXR_SIZE; i++)
        pixels[i] = (uint16_t)(0x1234 + i * 2654435761u);

    pvr_txr_load(pixels, txr, TXR_SIZE * TXR_SIZE * 2);
    free(pixels);

    printf("RESULT cache_multipass: FSAA %d, grid %dx%d, video %dx%d pm %d\n",
           USE_FSAA, tile_cols, tile_rows, vid_mode->width, vid_mode->height,
           vid_mode->pm);

    if(vid_mode->pm != PM_RGB565 || vid_mode->width != SCREEN_W) {
        printf("RESULT needs 640 wide RGB565 video, stopping\n");
        return 1;
    }

    cache = pvr_cache_create(&cache_params);

    if(!cache || pvr_cache_build(cache, cache_send, NULL) < 0) {
        printf("RESULT cache_create or build failed\n");
        return 1;
    }

    pvr_cache_get_info(cache, &info);
    printf("RESULT cache: %lu entries\n", (unsigned long)info.entries);

    ok = t1();
    ok = t2() && ok;
    ok = t3(txr) && ok;
    ok = t4() && ok;

    printf("RESULT %s\n", ok ? "ALL PASS" : "FAILED");
    printf("RESULT done\n");

    pvr_wait_ready();
    pvr_shutdown();

    return 0;
}
