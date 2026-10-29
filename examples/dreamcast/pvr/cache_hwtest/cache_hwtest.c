/* KallistiOS ##version##

   examples/dreamcast/pvr/cache_hwtest/cache_hwtest.c
   Copyright (C) 2026 Daniel Fairchild

   Hardware tests for cached object lists (dc/pvr/pvr_cache.h). Every test
   renders into a texture and reads the pixels back, so the answers come out
   on the serial console without looking at the screen. A summary is shown
   on screen at the end. Grep the console for "Q1".."Q4" and "RESULT".

   Q1  Does the ISP render object lists that live outside the TA's area?
       A hand-built cache (one quad, one tile) in the punch-through slot of
       TA frames, from both TA buffers, and as a whole cached frame.
   Q2  Parameter base and offsets: renders started by hand with several
       parameter bases and distances. Wrong readings hit red decoy quads.
   Q3  Does a cache built with the TA render correctly in later frames,
       from both TA buffers? Compared pixel by pixel with the same polygons
       sent through the TA, with FSAA and vertex double-buffering on and off.
   Q4  Reserve layout: texture memory shrinks by exactly twice the reserve,
       textures and reserved areas don't overlap, and a texture right at the
       start of texture memory renders intact while a cache is in use.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

#include <kos/thread.h>
#include <arch/arch.h>
#include <dc/pvr.h>
#include <dc/biosfont.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

#define RTT_W       640
#define RTT_H       480
#define RTT_STRIDE  1024

#define HALF        0x400000
#define RESERVE     (1024 * 1024)

#define C_BLACK     0x0000
#define C_WHITE     0xffff
#define C_RED       0xf800
#define C_GREEN     0x07e0
#define C_BLUE      0x001f

#define VR(addr)    (*(volatile uint32_t *)(PVR_RAM_BASE + (addr)))

static pvr_ptr_t rtt_tex;
static int fsaa;
static size_t txr_avail;      /* Texture memory free after pvr_init */

/* Results for the summary: 1 pass, 0 fail, -1 not run */
static int result[5] = { -1, -1, -1, -1, -1 };
static char summary[32][80];
static int summary_lines;

#define LOG(...) do { \
        printf(__VA_ARGS__); \
        if(summary_lines < 32) \
            snprintf(summary[summary_lines++], 80, __VA_ARGS__); \
    } while(0)

static void fail(int q) {
    result[q] = 0;
}

static void pass(int q) {
    if(result[q] < 0)
        result[q] = 1;
}

static uint32_t fbits(float f) {
    union {
        float f;
        uint32_t i;
    } u = { .f = f };

    return u.i;
}

/* PVR setup ***********************************************************/

static void init_pvr(int use_fsaa, int no_doublebuf, int reserve) {
    pvr_init_params_t params = {
        .opb_sizes = { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16,
                       PVR_BINSIZE_0, PVR_BINSIZE_0 },
        .vertex_buf_size = 512 * 1024,
        .fsaa_enabled = use_fsaa,
        .opb_overflow_count = 3,
        .vbuf_doublebuf_disabled = no_doublebuf,
        .cached_list_size = reserve
    };

    fsaa = use_fsaa;

    if(pvr_init(&params) < 0) {
        printf("pvr_init failed\n");
        arch_exit();
    }

    txr_avail = pvr_mem_available();
    rtt_tex = pvr_mem_malloc(RTT_STRIDE * 512 * 2);
}

static void shutdown_pvr(void) {
    pvr_mem_free(rtt_tex);
    pvr_shutdown();
}

static float sx(float x) {
    return fsaa ? x * 2.0f : x;
}

/* Fill the render texture with white, which nothing draws */
static void rtt_fill(void) {
    volatile uint32_t *p = (volatile uint32_t *)rtt_tex;
    int i;

    for(i = 0; i < RTT_STRIDE * RTT_H / 2; i++)
        p[i] = 0xffffffff;
}

static uint16_t px(int x, int y) {
    return ((volatile uint16_t *)rtt_tex)[y * RTT_STRIDE + x];
}

static const char *cname(uint16_t c) {
    static char buf[8];

    switch(c) {
        case C_BLACK: return "black";
        case C_WHITE: return "white(not rendered)";
        case C_RED: return "RED";
        case C_GREEN: return "GREEN";
        case C_BLUE: return "blue";
    }

    snprintf(buf, sizeof(buf), "%04x", c);
    return buf;
}

typedef void (*draw_fn)(void *data);

/* A TA frame rendered to the texture, with an opaque list from op() */
static void rtt_frame(draw_fn op, void *data) {
    rtt_fill();
    pvr_wait_ready();
    pvr_scene_begin_rtt(rtt_tex, RTT_W, RTT_H, RTT_STRIDE);
    pvr_list_begin(PVR_LIST_OP_POLY);

    if(op)
        op(data);

    pvr_list_finish();
    pvr_scene_finish();
    pvr_wait_ready();
    pvr_wait_render_done();
}

/* A whole frame from a cache, rendered to the texture */
static void rtt_cached(pvr_cache_t *c) {
    rtt_fill();
    pvr_wait_ready();
    pvr_scene_begin_rtt(rtt_tex, RTT_W, RTT_H, RTT_STRIDE);
    pvr_scene_finish_cached(c);
    pvr_wait_ready();
    pvr_wait_render_done();
}

/* Polygons through the TA **********************************************/

static void send_quad(pvr_list_t list, float x0, float y0, float x1, float y1,
                      float z, uint32_t argb) {
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_vertex_t v;

    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr, &cxt);
    hdr.cmd = (hdr.cmd & ~PVR_TA_CMD_TYPE) | FIELD_PREP(PVR_TA_CMD_TYPE, list);
    pvr_prim(&hdr, sizeof(hdr));

    v.z = z;
    v.u = v.v = 0.0f;
    v.argb = argb;
    v.oargb = 0;

    v.flags = PVR_CMD_VERTEX;
    v.x = sx(x0); v.y = y1;
    pvr_prim(&v, sizeof(v));
    v.x = sx(x0); v.y = y0;
    pvr_prim(&v, sizeof(v));
    v.x = sx(x1); v.y = y1;
    pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX_EOL;
    v.x = sx(x1); v.y = y0;
    pvr_prim(&v, sizeof(v));
}

/* Hand-built parameters ***********************************************/

/* ISP/TSP word: depth compare always, Gouraud. TSP word: src one, dst zero,
   no fog. No texture. Vertices are x, y, z, packed colour (skip 1). */
#define ISP_WORD    0xe0800000
#define TSP_WORD    0x20800000

/* Object list entry for a 4-vertex strip with both triangles in the tile */
#define STRIP_ENTRY 0x60200000

#define OL_END      0xf0000000

static void put_vertex(uint32_t a, float x, float y, float z, uint32_t argb) {
    VR(a) = fbits(x);
    VR(a + 4) = fbits(y);
    VR(a + 8) = fbits(z);
    VR(a + 12) = argb;
}

/* A flat quad as a 4-vertex strip: 19 words */
static void put_quad(uint32_t a, float x0, float y0, float x1, float y1,
                     uint32_t argb) {
    VR(a) = ISP_WORD;
    VR(a + 4) = TSP_WORD;
    VR(a + 8) = 0;
    put_vertex(a + 12, x0, y1, 1.0f, argb);
    put_vertex(a + 28, x0, y0, 1.0f, argb);
    put_vertex(a + 44, x1, y1, 1.0f, argb);
    put_vertex(a + 60, x1, y0, 1.0f, argb);
}

/* Background plane, as KOS writes it: 13 words */
static void put_bg(uint32_t a, uint32_t argb) {
    VR(a) = 0x90800000;
    VR(a + 4) = 0x20800440;
    VR(a + 8) = 0;
    put_vertex(a + 12, 0.0f, RTT_H, FLT_EPSILON, argb);
    put_vertex(a + 28, 0.0f, 0.0f, FLT_EPSILON, argb);
    put_vertex(a + 44, RTT_W, RTT_H, FLT_EPSILON, argb);
}

static void zero_vram(uint32_t a, uint32_t size) {
    uint32_t e = a + size;

    for(; a < e; a += 4)
        VR(a) = 0;
}

/* Q1 ******************************************************************/

static void q1_op(void *data) {
    (void)data;

    /* A TA quad in tile (5, 2), next to the cached one in tile (2, 2) */
    send_quad(PVR_LIST_OP_POLY, 160, 64, 192, 96, 1.0f, 0xff0000ff);
}

static int expect(int q, const char *what, int x, int y, uint16_t want) {
    uint16_t got = px(x, y);

    printf("Q%d   %s: pixel (%d,%d) = %s, want %s: %s\n", q, what, x, y,
           cname(got), cname(want), got == want ? "ok" : "WRONG");

    if(got != want) {
        fail(q);
        return 0;
    }

    return 1;
}

static void q1(void) {
    pvr_cache_params_t cp = {
        .opb_sizes = { 0, 0, 0, 0, PVR_BINSIZE_16 },
        .vertex_buf_size = 4096
    };
    pvr_cache_info_t info;
    pvr_cache_t *c;
    int h, i, tn, ok;
    uint32_t off;
    char what[64];

    printf("\nQ1: does the ISP render lists outside the TA's area?\n");
    init_pvr(0, 0, RESERVE);

    c = pvr_cache_create(&cp);

    if(!c) {
        LOG("Q1 FAIL: pvr_cache_create\n");
        fail(1);
        shutdown_pvr();
        return;
    }

    pvr_cache_get_info(c, &info);
    tn = info.tiles_w * 2 + 2;

    /* One green quad covering tile (2, 2), in both copies */
    for(h = 0; h < 2; h++) {
        put_quad(info.params[h], 64, 64, 96, 96, 0xff00ff00);
        off = (info.params[h] - info.param_base[h]) >> 2;
        VR(info.lists[h][PVR_LIST_PT_POLY] + info.list_block_size[PVR_LIST_PT_POLY] * tn) =
            STRIP_ENTRY | off;
        VR(info.lists[h][PVR_LIST_PT_POLY] + info.list_block_size[PVR_LIST_PT_POLY] * tn + 4) =
            OL_END;
        printf("Q1   copy %d: params at %06lx, entry %08lx, list at %06lx\n", h,
               (unsigned long)info.params[h], (unsigned long)(STRIP_ENTRY | off),
               (unsigned long)(info.lists[h][PVR_LIST_PT_POLY] +
                               info.list_block_size[PVR_LIST_PT_POLY] * tn));
    }

    if(pvr_cache_attach(c, PVR_CACHE_SLOT(PVR_LIST_PT_POLY)) < 0) {
        LOG("Q1 FAIL: pvr_cache_attach\n");
        fail(1);
    }

    /* Four TA frames: they alternate between the two TA buffers */
    ok = 1;

    for(i = 0; i < 4; i++) {
        rtt_frame(q1_op, NULL);
        snprintf(what, sizeof(what), "PT slot, TA frame %d, cached quad", i);
        ok &= expect(1, what, 80, 80, C_GREEN);
        snprintf(what, sizeof(what), "PT slot, TA frame %d, TA quad", i);
        ok &= expect(1, what, 176, 80, C_BLUE);
        snprintf(what, sizeof(what), "PT slot, TA frame %d, background", i);
        ok &= expect(1, what, 300, 300, C_BLACK);
    }

    LOG("Q1 PT slot in TA frames: %s\n", ok ? "PASS" : "FAIL");

    /* Whole frames from the cache: they alternate between the copies */
    ok = 1;

    for(i = 0; i < 2; i++) {
        rtt_cached(c);
        snprintf(what, sizeof(what), "cached frame %d, cached quad", i);
        ok &= expect(1, what, 80, 80, C_GREEN);
        snprintf(what, sizeof(what), "cached frame %d, no TA quad", i);
        ok &= expect(1, what, 176, 80, C_BLACK);
    }

    LOG("Q1 whole cached frame: %s\n", ok ? "PASS" : "FAIL");

    /* Detached: the quad must go */
    pvr_cache_attach(NULL, PVR_CACHE_SLOT(PVR_LIST_PT_POLY));
    rtt_frame(q1_op, NULL);
    ok = expect(1, "detached, no cached quad", 80, 80, C_BLACK);
    LOG("Q1 detach: %s\n", ok ? "PASS" : "FAIL");

    pass(1);
    pvr_cache_free(c);
    shutdown_pvr();
}

/* Q2 ******************************************************************/

/* Start a render by hand, into the render texture. The last KOS frame was
   rendered to a texture, so the render-done interrupt won't flip pages. */
static void raw_render(uint32_t region, uint32_t param_base, uint32_t bg_addr,
                       uint32_t bg_base) {
    uint32_t tex = (uint32_t)rtt_tex - PVR_RAM_INT_BASE;

    rtt_fill();
    PVR_SET(PVR_ISP_TILEMAT_ADDR, region);
    PVR_SET(PVR_ISP_VERTBUF_ADDR, param_base);
    PVR_SET(PVR_RENDER_ADDR, tex | BIT(24));
    PVR_SET(PVR_RENDER_ADDR_2, tex | BIT(24));
    PVR_SET(PVR_BGPLANE_CFG, 0x01000000 | ((bg_addr - bg_base) << 1));
    PVR_SET(PVR_BGPLANE_Z, fbits(0.0001f));
    PVR_SET(PVR_PCLIP_X, (RTT_W - 1) << 16);
    PVR_SET(PVR_PCLIP_Y, (RTT_H - 1) << 16);
    PVR_SET(PVR_RENDER_MODULO, RTT_STRIDE * 2 / 8);
    PVR_SET(PVR_ISP_START, PVR_ISP_START_GO);
    thd_sleep(30);
}

typedef struct {
    const char *name;
    uint32_t reg;           /* PARAM_BASE register value */
    uint32_t base;          /* Base the offsets are computed against */
    uint32_t quad;          /* Green quad, relative to the upper parameter area */
    int      lower;         /* Green quad in the lower copy instead */
    int32_t  decoy;         /* Red decoy, relative to the green quad (0: none) */
    int      entry_to_decoy;/* Control: the entry points at the decoy */
    const char *green, *red;
} q2_case_t;

static void q2(void) {
    pvr_cache_params_t cp = {
        .opb_sizes = { PVR_BINSIZE_16, 0, 0, 0, 0 },
        .vertex_buf_size = 768 * 1024
    };
    static const q2_case_t cases[] = {
        { "A base 0x400000, quad ~1.3 MB above", 0x400000, 0x400000, 0x100, 0, 0, 0,
          "renders", NULL },
        { "B base 0x400000, quad ~1.8 MB above", 0x400000, 0x400000, 0x80100, 0, 0, 0,
          "renders", NULL },
        { "C base 0, quad in upper half (offset bit 20 set)", 0, 0, 0x100, 0, -HALF, 0,
          "21-bit offsets", "only 20 offset bits decoded" },
        { "D base reg 0x410000, offsets from 0x400000", 0x410000, 0x400000, 0x100, 0, 0x10000, 0,
          "PARAM_BASE masked to 1 MB", "PARAM_BASE not masked (64 KB)" },
        { "E base reg 0x480000, offsets from 0x400000", 0x480000, 0x400000, 0x100, 0, 0x80000, 0,
          "PARAM_BASE masked to 1 MB", "PARAM_BASE not masked (512 KB)" },
        { "F control: entry points at the decoy", 0x400000, 0x400000, 0x100, 0, 0x10000, 1,
          NULL, "decoys show (control ok)" }
    };
    pvr_cache_info_t info;
    pvr_cache_t *c;
    uint32_t up, lo, quad, decoy, bg, bgbase, list, off;
    uint16_t got, bgc;
    size_t i;
    int ok = 1;

    printf("\nQ2: parameter base and offsets\n");
    init_pvr(0, 0, RESERVE);

    c = pvr_cache_create(&cp);

    if(!c) {
        LOG("Q2 FAIL: pvr_cache_create\n");
        fail(2);
        shutdown_pvr();
        return;
    }

    pvr_cache_get_info(c, &info);
    up = info.params[1];
    lo = info.params[0];
    list = info.lists[1][PVR_LIST_OP_POLY] +
           info.list_block_size[PVR_LIST_OP_POLY] * (info.tiles_w * 2 + 2);
    printf("Q2   parameter areas %06lx (copy 0), %06lx (copy 1), region array %06lx\n",
           (unsigned long)lo, (unsigned long)up, (unsigned long)info.region[1]);

    /* A KOS frame to a texture first, so the hand-started renders' done
       interrupts don't flip pages */
    rtt_frame(NULL, NULL);

    for(i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const q2_case_t *k = cases + i;

        zero_vram(lo, info.params_size);
        zero_vram(up, info.params_size);

        quad = up + k->quad;
        decoy = k->decoy ? quad + k->decoy : 0;

        put_quad(quad, 64, 64, 96, 96, 0xff00ff00);

        if(decoy)
            put_quad(decoy, 64, 64, 96, 96, 0xffff0000);

        /* Background plane: in the lower half for base 0, and wherever an
           unmasked base would look for it too */
        bgbase = k->base;
        bg = k->base ? up : lo;
        put_bg(bg, 0xff000000);

        if(k->decoy > 0 && !k->entry_to_decoy)
            put_bg(bg + k->decoy, 0xff000000);

        off = ((k->entry_to_decoy ? decoy : quad) - k->base) >> 2;
        VR(list) = STRIP_ENTRY | (off & 0x1fffff);
        VR(list + 4) = OL_END;

        raw_render(info.region[1], k->reg, bg, bgbase);

        got = px(80, 80);
        bgc = px(300, 300);

        printf("Q2   case %s: PARAM_BASE %06lx, quad %06lx, entry %08lx -> %s, background %s\n",
               k->name, (unsigned long)k->reg, (unsigned long)quad,
               (unsigned long)(STRIP_ENTRY | off), cname(got), cname(bgc));

        if(got == C_GREEN && k->green) {
            LOG("Q2 %c: GREEN = %s\n", k->name[0], k->green);
        }
        else if(got == C_RED && k->red) {
            LOG("Q2 %c: RED = %s\n", k->name[0], k->red);

            if(!k->entry_to_decoy)
                ok = 0;
        }
        else {
            LOG("Q2 %c: unexpected %s\n", k->name[0], cname(got));
            ok = 0;
        }
    }

    /* Clean up: lists empty again */
    VR(list) = OL_END;

    if(ok)
        pass(2);
    else
        fail(2);

    pvr_cache_free(c);
    shutdown_pvr();
}

/* Q3 ******************************************************************/

static uint32_t rng;

static uint32_t rnd(void) {
    rng = rng * 1103515245 + 12345;
    return (rng >> 8) & 0xffffff;
}

static const uint32_t palette[8] = {
    0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00,
    0xffff00ff, 0xff00ffff, 0xffffffff, 0xff808080
};

typedef struct {
    pvr_list_t list;
    uint32_t seed;
} pattern_t;

/* A deterministic mix of strips, single triangles and sprites, each at its
   own depth so the result doesn't depend on drawing order */
static void send_pattern(void *data) {
    const pattern_t *p = data;
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_sprite_cxt_t scxt;
    pvr_sprite_hdr_t shdr;
    pvr_sprite_col_t spr;
    pvr_vertex_t v;
    float x, y, w, h, z = 1.0f;
    int i, j, n;

    rng = p->seed;

    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr, &cxt);
    hdr.cmd = (hdr.cmd & ~PVR_TA_CMD_TYPE) | FIELD_PREP(PVR_TA_CMD_TYPE, p->list);
    pvr_prim(&hdr, sizeof(hdr));

    v.u = v.v = 0.0f;
    v.oargb = 0;

    /* Zig-zag strips of 4 to 14 vertices, many crossing tiles */
    for(i = 0; i < 300; i++) {
        x = rnd() % 560;
        y = rnd() % 440;
        w = 8 + rnd() % 40;
        h = 8 + rnd() % 120;
        n = 4 + rnd() % 11;
        v.argb = palette[rnd() & 7];
        v.z = z;
        z += 0.01f;

        for(j = 0; j < n; j++) {
            v.flags = j == n - 1 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            v.x = sx(x + (j >> 1) * w * 0.5f);
            v.y = (j & 1) ? y : y + h;

            if(v.x > sx(639)) v.x = sx(639);
            if(v.y > 479) v.y = 479;

            pvr_prim(&v, sizeof(v));
        }
    }

    /* Single triangles in small clusters (the TA may group them) */
    for(i = 0; i < 200; i++) {
        x = 32 * (rnd() % 19) + (rnd() % 16);
        y = 32 * (rnd() % 14) + (rnd() % 16);
        v.argb = palette[rnd() & 7];
        v.z = z;
        z += 0.01f;

        v.flags = PVR_CMD_VERTEX;
        v.x = sx(x); v.y = y + 12;
        pvr_prim(&v, sizeof(v));
        v.x = sx(x + 6); v.y = y;
        pvr_prim(&v, sizeof(v));
        v.flags = PVR_CMD_VERTEX_EOL;
        v.x = sx(x + 12); v.y = y + 12;
        pvr_prim(&v, sizeof(v));
    }

    /* Sprites */
    pvr_sprite_cxt_col(&scxt, PVR_LIST_OP_POLY);
    scxt.gen.culling = PVR_CULLING_NONE;
    pvr_sprite_compile(&shdr, &scxt);
    shdr.cmd = (shdr.cmd & ~PVR_TA_CMD_TYPE) | FIELD_PREP(PVR_TA_CMD_TYPE, p->list);

    for(i = 0; i < 100; i++) {
        x = rnd() % 600;
        y = rnd() % 440;
        w = 4 + rnd() % 36;
        h = 4 + rnd() % 36;
        shdr.argb = palette[rnd() & 7];
        pvr_prim(&shdr, sizeof(shdr));

        spr.flags = PVR_CMD_VERTEX_EOL;
        spr.ax = sx(x); spr.ay = y + h; spr.az = z;
        spr.bx = sx(x); spr.by = y; spr.bz = z;
        spr.cx = sx(x + w); spr.cy = y; spr.cz = z;
        spr.dx = sx(x + w); spr.dy = y + h;
        spr.d1 = spr.d2 = spr.d3 = spr.d4 = 0;
        pvr_prim(&spr, sizeof(spr));
        z += 0.01f;
    }
}

/* Build callback: the pattern as one list */
static void build_pattern(void *data) {
    const pattern_t *p = data;

    pvr_list_begin(p->list);
    send_pattern(data);
    pvr_list_finish();
}

static uint16_t *reference;

static void save_reference(void) {
    int x, y;

    for(y = 0; y < RTT_H; y++)
        for(x = 0; x < RTT_W; x++)
            reference[y * RTT_W + x] = px(x, y);
}

static int compare_reference(int *fx, int *fy) {
    int x, y, n = 0;

    *fx = *fy = -1;

    for(y = 0; y < RTT_H; y++) {
        for(x = 0; x < RTT_W; x++) {
            if(reference[y * RTT_W + x] != px(x, y)) {
                if(!n) {
                    *fx = x;
                    *fy = y;
                }

                n++;
            }
        }
    }

    return n;
}

/* Count entries and links of a cache's list, and check that copy 0 is
   copy 1 with links moved down by 0x400000 */
static void count_lists(const pvr_cache_info_t *info, int l, int *entries,
                        int *links, int *mismatch) {
    int tn, tiles = info->tiles_w * info->tiles_h, steps;
    uint32_t a, w, w0;

    *entries = *links = *mismatch = 0;

    for(tn = 0; tn < tiles; tn++) {
        a = info->lists[1][l] + info->list_block_size[l] * tn;

        for(steps = 0; steps < 100000; steps++) {
            w = VR(a);
            w0 = VR(a - HALF);

            if((w & 0xf0000000) == 0xf0000000) {
                *mismatch += w0 != w;
                break;
            }

            if((w & 0xf0000000) == 0xe0000000) {
                (*links)++;
                *mismatch += w0 != (w - HALF);
                a = w & 0x00fffffc;
                continue;
            }

            (*entries)++;
            *mismatch += w0 != w;
            a += 4;
        }
    }
}

static void q3_screen_frame(int i) {
    pattern_t p = { PVR_LIST_OP_POLY, 1000 + i };

    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    send_pattern(&p);
    pvr_list_finish();
    pvr_scene_finish();
}

static int q3_check(const char *what) {
    int n, fx, fy;

    n = compare_reference(&fx, &fy);

    if(n)
        printf("Q3   %s: %d pixels differ, first at (%d,%d): %04x, want %04x\n",
               what, n, fx, fy, px(fx, fy), reference[fy * RTT_W + fx]);
    else
        printf("Q3   %s: identical\n", what);

    return n == 0;
}

static void q3_config(int use_fsaa, int no_doublebuf) {
    pvr_cache_params_t cp = {
        .opb_sizes = { 0, 0, 0, 0, PVR_BINSIZE_8 },
        .vertex_buf_size = 256 * 1024,
        .opb_overflow_count = 6
    };
    pattern_t ref = { PVR_LIST_OP_POLY, 1 }, cached = { PVR_LIST_PT_POLY, 1 };
    pvr_cache_info_t info;
    pvr_cache_t *c;
    int i, ok = 1, entries, links, mismatch, r;
    char what[64];

    printf("\nQ3 config: FSAA %s, vertex double-buffering %s\n",
           use_fsaa ? "on" : "off", no_doublebuf ? "off" : "on");
    init_pvr(use_fsaa, no_doublebuf, RESERVE);

    c = pvr_cache_create(&cp);

    if(!c) {
        LOG("Q3 FAIL: pvr_cache_create\n");
        fail(3);
        shutdown_pvr();
        return;
    }

    for(r = 0; r < 2; r++) {
        /* The second round rebuilds the cache with another pattern */
        ref.seed = cached.seed = 1 + r;

        rtt_frame(send_pattern, &ref);
        save_reference();

        if(pvr_cache_build(c, build_pattern, &cached) < 0) {
            printf("Q3   pvr_cache_build failed\n");
            ok = 0;
            break;
        }

        pvr_cache_get_info(c, &info);
        count_lists(&info, PVR_LIST_PT_POLY, &entries, &links, &mismatch);
        printf("Q3   build %d: %lu entries, %d links to overflow blocks, %lu of %lu "
               "parameter bytes, %lu us\n", r, (unsigned long)info.entries, links,
               (unsigned long)info.params_used, (unsigned long)info.params_size,
               (unsigned long)(info.build_ns / 1000));
        printf("Q3   TA offsets relative to %06lx; parameters start at %06lx: %s\n",
               (unsigned long)info.ta_param_base, (unsigned long)info.params[1],
               info.ta_param_base == (info.params[1] & 0xf00000) ?
               "TA base rounded down to 1 MB (as lxdream)" :
               "TA base NOT rounded (exact TA_ISP_BASE)");

        if(mismatch) {
            printf("Q3   copy 0 differs from copy 1 in %d words\n", mismatch);
            ok = 0;
        }

        if(!links)
            printf("Q3   note: no overflow blocks used\n");

        if(pvr_cache_attach(c, PVR_CACHE_SLOT(PVR_LIST_PT_POLY)) < 0) {
            ok = 0;
            break;
        }

        /* TA frames with an empty opaque list, the cache in the PT slot */
        for(i = 0; i < 4; i++) {
            rtt_frame(NULL, NULL);
            snprintf(what, sizeof(what), "build %d, PT slot, TA frame %d", r, i);
            ok &= q3_check(what);
        }

        /* Whole cached frames */
        for(i = 0; i < 2; i++) {
            rtt_cached(c);
            snprintf(what, sizeof(what), "build %d, whole cached frame %d", r, i);
            ok &= q3_check(what);
        }

        /* Later frames: a minute of heavy TA frames on screen, then again */
        for(i = 0; i < 60; i++)
            q3_screen_frame(i);

        for(i = 0; i < 2; i++) {
            rtt_frame(NULL, NULL);
            snprintf(what, sizeof(what), "build %d, PT slot after 60 TA frames, %d", r, i);
            ok &= q3_check(what);
        }
    }

    LOG("Q3 FSAA %s, %s vbuf: %s\n", use_fsaa ? "on " : "off",
        no_doublebuf ? "single" : "double", ok ? "PASS" : "FAIL");

    if(ok)
        pass(3);
    else
        fail(3);

    pvr_cache_attach(NULL, PVR_CACHE_SLOT(PVR_LIST_PT_POLY));
    pvr_cache_free(c);
    shutdown_pvr();
}

static void q3(void) {
    reference = malloc(RTT_W * RTT_H * 2);

    printf("\nQ3: TA-built cache in later frames, from both TA buffers\n");
    q3_config(0, 0);
    q3_config(0, 1);
    q3_config(1, 0);
    q3_config(1, 1);

    free(reference);
}

/* Q4 ******************************************************************/

static pvr_ptr_t q4_txr;

static void q4_op(void *data) {
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_vertex_t v;

    (void)data;

    /* The 64x64 texture, one texel per pixel, at (128, 128) */
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
                     PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, 64, 64,
                     q4_txr, PVR_FILTER_NONE);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));

    v.z = 1.0f;
    v.argb = 0xffffffff;
    v.oargb = 0;
    v.flags = PVR_CMD_VERTEX;
    v.x = sx(128); v.y = 192; v.u = 0.0f; v.v = 1.0f;
    pvr_prim(&v, sizeof(v));
    v.x = sx(128); v.y = 128; v.u = 0.0f; v.v = 0.0f;
    pvr_prim(&v, sizeof(v));
    v.x = sx(192); v.y = 192; v.u = 1.0f; v.v = 1.0f;
    pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX_EOL;
    v.x = sx(192); v.y = 128; v.u = 1.0f; v.v = 0.0f;
    pvr_prim(&v, sizeof(v));
}

static uint16_t q4_texel(int x, int y) {
    /* 8x8 cells */
    return (((x >> 3) ^ (y >> 3)) & 1) ? C_GREEN : C_RED;
}

static void q4(void) {
    pvr_cache_params_t cp = {
        .opb_sizes = { 0, 0, 0, 0, PVR_BINSIZE_16 },
        .vertex_buf_size = 64 * 1024
    };
    pattern_t pat = { PVR_LIST_PT_POLY, 7 };
    pvr_cache_info_t info;
    pvr_cache_t *c, *big;
    size_t avail0, avail1, area;
    uint32_t tbase, a, lo_end, bad;
    pvr_ptr_t blocks[64];
    size_t sizes[64];
    int nblocks = 0, i, x, y, ok = 1, cx, cy;
    volatile uint32_t *t;

    printf("\nQ4: reserve layout\n");

    init_pvr(0, 0, 0);
    avail0 = txr_avail;
    shutdown_pvr();

    init_pvr(0, 0, RESERVE);
    avail1 = txr_avail;

    LOG("Q4 texture memory %lu without, %lu with %d KB reserved: %s\n",
        (unsigned long)avail0, (unsigned long)avail1, RESERVE / 1024,
        avail0 - avail1 == 2 * RESERVE ? "2x reserve, ok" : "WRONG");

    if(avail0 - avail1 != 2 * RESERVE)
        ok = 0;

    /* The whole reserve as one cache shows where the area is */
    area = pvr_cache_mem_available();
    cp.vertex_buf_size = area - 128 * 1024;
    big = pvr_cache_create(&cp);

    if(!big) {
        LOG("Q4 FAIL: can't place a cache of %lu bytes\n", (unsigned long)area);
        fail(4);
        shutdown_pvr();
        return;
    }

    pvr_cache_get_info(big, &info);
    lo_end = info.base[0] + RESERVE;
    printf("Q4   reserved areas %06lx-%06lx and %06lx-%06lx (%lu free for caches)\n",
           (unsigned long)info.base[0], (unsigned long)lo_end,
           (unsigned long)info.base[1], (unsigned long)(lo_end + HALF),
           (unsigned long)area);

    /* Texture memory starts at twice the end of the lower reserve. Check the
       mapping: 64-bit offset T holds 32-bit words at T/2 and T/2 + 4 MB. */
    tbase = (uint32_t)rtt_tex - PVR_RAM_INT_BASE;
    t = (volatile uint32_t *)rtt_tex;
    t[0] = 0x11223344;
    t[1] = 0x55667788;
    printf("Q4   first texture block at 64-bit offset %06lx; 32-bit words at %06lx: "
           "%08lx, %06lx: %08lx\n", (unsigned long)tbase,
           (unsigned long)(tbase / 2), (unsigned long)VR(tbase / 2),
           (unsigned long)(tbase / 2 + HALF), (unsigned long)VR(tbase / 2 + HALF));

    if(VR(tbase / 2) != 0x11223344 || VR(tbase / 2 + HALF) != 0x55667788) {
        LOG("Q4 64-bit/32-bit mapping: WRONG\n");
        ok = 0;
    }

    if(tbase / 2 < lo_end) {
        LOG("Q4 texture memory overlaps the reserve\n");
        ok = 0;
    }

    /* Fill all texture memory and both reserves with different patterns */
    while(nblocks < 64) {
        size_t n = pvr_mem_available();

        if(n < 4096)
            break;

        if(n > 256 * 1024)
            n = 256 * 1024;

        n &= ~31;
        blocks[nblocks] = pvr_mem_malloc(n);

        if(!blocks[nblocks])
            break;

        sizes[nblocks] = n;

        t = (volatile uint32_t *)blocks[nblocks];

        for(i = 0; i < (int)n / 4; i++)
            t[i] = ((uint32_t)(t + i)) ^ 0xa5a5a5a5;

        nblocks++;
    }

    t = (volatile uint32_t *)rtt_tex;

    for(i = 0; i < RTT_STRIDE * 512 / 2; i++)
        t[i] = ((uint32_t)(t + i)) ^ 0xa5a5a5a5;

    for(a = info.base[0]; a < lo_end; a += 4) {
        VR(a) = a ^ 0x5a5a5a5a;
        VR(a + HALF) = (a + HALF) ^ 0x5a5a5a5a;
    }

    bad = 0;

    for(i = 0; i < RTT_STRIDE * 512 / 2; i++)
        bad += t[i] != (((uint32_t)(t + i)) ^ 0xa5a5a5a5);

    for(i = 0; i < nblocks; i++) {
        volatile uint32_t *b = (volatile uint32_t *)blocks[i];
        int j;

        for(j = 0; j < (int)sizes[i] / 4; j++)
            bad += b[j] != (((uint32_t)(b + j)) ^ 0xa5a5a5a5);
    }

    for(a = info.base[0]; a < lo_end; a += 4) {
        bad += VR(a) != (a ^ 0x5a5a5a5a);
        bad += VR(a + HALF) != ((a + HALF) ^ 0x5a5a5a5a);
    }

    LOG("Q4 textures (%d blocks) vs reserves: %lu words corrupted\n", nblocks,
        (unsigned long)bad);

    if(bad)
        ok = 0;

    for(i = 0; i < nblocks; i++)
        pvr_mem_free(blocks[i]);

    pvr_cache_free(big);

    /* A texture right at the start of texture memory, rendered while a
       cache is attached */
    pvr_mem_free(rtt_tex);
    q4_txr = pvr_mem_malloc(64 * 64 * 2);
    rtt_tex = pvr_mem_malloc(RTT_STRIDE * 512 * 2);
    printf("Q4   test texture at 64-bit offset %06lx (texture memory starts at %06lx)\n",
           (unsigned long)((uint32_t)q4_txr - PVR_RAM_INT_BASE), (unsigned long)tbase);

    for(y = 0; y < 64; y++)
        for(x = 0; x < 64; x++)
            ((volatile uint16_t *)q4_txr)[y * 64 + x] = q4_texel(x, y);

    cp.vertex_buf_size = 256 * 1024;
    cp.opb_overflow_count = 6;
    c = pvr_cache_create(&cp);

    if(!c || pvr_cache_build(c, build_pattern, &pat) < 0 ||
            pvr_cache_attach(c, PVR_CACHE_SLOT(PVR_LIST_PT_POLY)) < 0) {
        LOG("Q4 FAIL: cache setup\n");
        ok = 0;
    }
    else {
        reference = malloc(RTT_W * RTT_H * 2);

        /* Where the cache draws by itself */
        rtt_frame(NULL, NULL);
        save_reference();

        for(i = 0; i < 4; i++) {
            /* Frames 0-1 with the cache attached, 2-3 without */
            if(i == 2)
                pvr_cache_attach(NULL, PVR_CACHE_SLOT(PVR_LIST_PT_POLY));

            rtt_frame(q4_op, NULL);
            bad = 0;

            for(cy = 4; cy < 64; cy += 8) {
                for(cx = 4; cx < 64; cx += 8) {
                    int X = 128 + cx, Y = 128 + cy;

                    /* Skip cells the cache's polygons cover (they are in
                       front of the texture) */
                    if(i < 2 && reference[Y * RTT_W + X] != C_BLACK)
                        continue;

                    if(px(X, Y) != q4_texel(cx, cy))
                        bad++;
                }
            }

            printf("Q4   textured quad, cache %s, frame %d: %lu cells wrong\n",
                   i < 2 ? "attached" : "detached", i, (unsigned long)bad);

            if(bad)
                ok = 0;
        }

        free(reference);

        LOG("Q4 texture at start of texture memory renders: %s\n", ok ? "ok" : "WRONG");
    }

    pvr_cache_attach(NULL, PVR_CACHE_SLOT(PVR_LIST_PT_POLY));
    pvr_cache_free(c);
    pvr_mem_free(q4_txr);

    LOG("Q4 reserve layout: %s\n", ok ? "PASS" : "FAIL");

    if(ok)
        pass(4);
    else
        fail(4);

    shutdown_pvr();
}

/* Summary *************************************************************/

static void show_summary(void) {
    pvr_ptr_t txt;
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_vertex_t v;
    uint16_t *buf;
    int i, frames;
    maple_device_t *cont;
    cont_state_t *st;

    init_pvr(0, 0, 0);
    txt = pvr_mem_malloc(1024 * 1024 * 2);
    buf = calloc(1024 * 1024, 2);

    for(i = 0; i < summary_lines; i++) {
        char *nl = strchr(summary[i], '\n');

        if(nl)
            *nl = 0;

        bfont_draw_str_ex(buf + (8 + i * 26) * 1024 + 8, 1024,
                          strstr(summary[i], "FAIL") || strstr(summary[i], "WRONG") ||
                          strstr(summary[i], "unexpected") ? C_RED : C_WHITE,
                          C_BLACK, 16, true, summary[i]);
    }

    pvr_txr_load(buf, txt, 1024 * 1024 * 2);
    free(buf);

    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
                     PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, 1024, 1024,
                     txt, PVR_FILTER_NONE);
    pvr_poly_compile(&hdr, &cxt);

    for(frames = 0; frames < 60 * 30; frames++) {
        cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);

        if(cont && (st = maple_dev_status(cont)) && (st->buttons & CONT_START))
            break;

        pvr_wait_ready();
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);
        pvr_prim(&hdr, sizeof(hdr));
        v.z = 1.0f;
        v.argb = 0xffffffff;
        v.oargb = 0;
        v.flags = PVR_CMD_VERTEX;
        v.x = 0; v.y = 1024; v.u = 0; v.v = 1;
        pvr_prim(&v, sizeof(v));
        v.x = 0; v.y = 0; v.u = 0; v.v = 0;
        pvr_prim(&v, sizeof(v));
        v.x = 1024; v.y = 1024; v.u = 1; v.v = 1;
        pvr_prim(&v, sizeof(v));
        v.flags = PVR_CMD_VERTEX_EOL;
        v.x = 1024; v.y = 0; v.u = 1; v.v = 0;
        pvr_prim(&v, sizeof(v));
        pvr_list_finish();
        pvr_scene_finish();
    }

    pvr_mem_free(txt);
    shutdown_pvr();
}

static const char *rstr(int r) {
    return r > 0 ? "PASS" : r == 0 ? "FAIL" : "not-run";
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("cache_hwtest: cached object lists, hardware questions\n");

    q1();
    q2();
    q3();
    q4();

    printf("\nRESULT Q1=%s Q2=%s Q3=%s Q4=%s\n", rstr(result[1]), rstr(result[2]),
           rstr(result[3]), rstr(result[4]));
    snprintf(summary[summary_lines < 31 ? summary_lines++ : 31], 80,
             "RESULT Q1=%s Q2=%s Q3=%s Q4=%s", rstr(result[1]), rstr(result[2]),
             rstr(result[3]), rstr(result[4]));

    show_summary();

    return 0;
}
