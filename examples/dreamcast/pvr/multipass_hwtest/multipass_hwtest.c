/* KallistiOS ##version##

   examples/dreamcast/pvr/multipass_hwtest/multipass_hwtest.c

   Hardware tests for multipass rendering (pvr_init_params_t::pass_count).

   Runs a series of small scenes, reads the rendered pixels back from the
   frame buffer (and from a render-to-texture target) and prints on the
   serial console what the hardware did. Each result line starts with
   "HWTEST". At the end, a summary is shown on screen.

   T1  Region array bit 28: two passes, red quad in pass 0 and green quad in
       pass 1, both expected. Then bit 28 is set on the last pass too: if the
       frame buffer keeps the magenta it was filled with, bit 28 means "don't
       write the tile out". Done for the frame buffer and for render to
       texture.
   T2  Does TA_LIST_CONT need to wait for the done interrupts of the pass?
       A heavy pass 0, rendered with and without the wait, compared with a
       reference checksum over many frames.
   T3  Does opaque shading in a z-keep pass touch only that pass's pixels?
       Pass 0: grey opaque floor, 50% red translucent quad. Pass 1: small
       opaque quads in the same tiles. Also checks that pass 1 is depth
       tested against pass 0's depth.
   T4  Is moving TA_OL_BASE between passes needed? Pass 0: red opaque quad.
       Pass 1: 50% white translucent quad. Without the move (and with every
       pass's region entries on pass 0's blocks), the outcome tells what the
       TA did with the second pass's lists.
   T5  Modifier volumes across passes (cheap shadow): does a pass-0 volume
       shadow pass-1 polygons, and does a pass-1 volume shadow pass-0 pixels?

   Built twice: multipass_hwtest.elf without FSAA, multipass_hwtest_fsaa.elf
   with FSAA.

   This program uses pvr_mp_test_flags, a temporary hook of the multipass
   implementation that isn't part of the KOS API.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <arch/timer.h>
#include <kos/thread.h>
#include <dc/pvr.h>
#include <dc/asic.h>
#include <dc/video.h>
#include <dc/biosfont.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

#ifndef HWTEST_FSAA
#define HWTEST_FSAA 0
#endif

/* Test hooks of the multipass implementation (hardware/pvr/pvr_internal.h) */
extern volatile uint32_t pvr_mp_test_flags;
#define PVR_MP_TEST_NO_WAIT             (1 << 0)
#define PVR_MP_TEST_NO_OL_BASE          (1 << 1)
#define PVR_MP_TEST_LAST_NO_WRITEOUT    (1 << 2)
#define PVR_MP_TEST_ALL_WRITEOUT        (1 << 3)

#define W 640
#define H 480
#define RTT_STRIDE 1024

/* Frame buffer magenta, written before frames that shouldn't be written out */
#define MAGIC565 0xf81f

static float xs = HWTEST_FSAA ? 2.0f : 1.0f;
static pvr_ptr_t rtt;

static pvr_poly_hdr_t hdr_op, hdr_tr, hdr_op_shadow;
static pvr_mod_hdr_t hdr_mod, hdr_mod_last;

/* Error interrupts seen since the last reset_errors() */
static volatile int err_count[6];
static const char *err_name[6] = {
    "ISP_OUTOFMEM", "STRIP_HALT", "PARAM_OUTOFMEM", "OPB_OUTOFMEM",
    "TA_INPUT_ERR", "TA_INPUT_OVERFLOW"
};

static void err_handler(uint32_t code, void *data) {
    (void)data;
    if(code >= ASIC_EVT_PVR_ISP_OUTOFMEM && code <= ASIC_EVT_PVR_TA_INPUT_OVERFLOW)
        err_count[code - ASIC_EVT_PVR_ISP_OUTOFMEM]++;
}

static void reset_errors(void) {
    memset((void *)err_count, 0, sizeof(err_count));
}

/* Prints error interrupt counts; returns their total. */
static int print_errors(const char *tag) {
    int i, total = 0;

    for(i = 0; i < 6; i++) {
        if(err_count[i]) {
            printf("HWTEST %s irq %s x%d\n", tag, err_name[i], err_count[i]);
            total += err_count[i];
        }
    }

    return total;
}

/* Summary lines for the screen */
static char summary[16][64];
static int summary_n;

/* Lines of at most 52 characters (the screen is 640 pixels wide) */
static void add_summary(const char *test, const char *result) {
    if(summary_n < 16)
        snprintf(summary[summary_n++], 53, "%s %s", test, result);
}

/**** Drawing ******************************************************/

static void vert(uint32_t flags, float x, float y, float z, uint32_t argb) {
    pvr_vertex_t v __attribute__((aligned(32)));

    v.flags = flags;
    v.x = x * xs;
    v.y = y;
    v.z = z;
    v.u = v.v = 0.0f;
    v.argb = argb;
    v.oargb = 0;
    pvr_prim(&v, sizeof(v));
}

static void quad(float x0, float y0, float x1, float y1, float z, uint32_t argb) {
    vert(PVR_CMD_VERTEX, x0, y1, z, argb);
    vert(PVR_CMD_VERTEX, x0, y0, z, argb);
    vert(PVR_CMD_VERTEX, x1, y1, z, argb);
    vert(PVR_CMD_VERTEX_EOL, x1, y0, z, argb);
}

static void tri(float x0, float y0, float x1, float y1, float x2, float y2,
                float z, uint32_t argb) {
    vert(PVR_CMD_VERTEX, x0, y0, z, argb);
    vert(PVR_CMD_VERTEX, x1, y1, z, argb);
    vert(PVR_CMD_VERTEX_EOL, x2, y2, z, argb);
}

/* A flat modifier volume over a rectangle, in front of everything, as in the
   cheap_shadow example: everything behind it is inside. */
static void mod_rect(float x0, float y0, float x1, float y1) {
    pvr_modifier_vol_t m __attribute__((aligned(32)));

    memset(&m, 0, sizeof(m));
    pvr_prim(&hdr_mod, sizeof(hdr_mod));
    m.flags = PVR_CMD_VERTEX_EOL;
    m.ax = x0 * xs; m.ay = y1; m.az = 150.0f;
    m.bx = x0 * xs; m.by = y0; m.bz = 150.0f;
    m.cx = x1 * xs; m.cy = y1; m.cz = 150.0f;
    pvr_prim(&m, sizeof(m));

    pvr_prim(&hdr_mod_last, sizeof(hdr_mod_last));
    m.ax = x0 * xs; m.ay = y0; m.az = 150.0f;
    m.bx = x1 * xs; m.by = y1; m.bz = 150.0f;
    m.cx = x1 * xs; m.cy = y0; m.cz = 150.0f;
    pvr_prim(&m, sizeof(m));
}

static void compile_headers(void) {
    pvr_poly_cxt_t cxt;

    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    pvr_poly_compile(&hdr_op, &cxt);

    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    pvr_poly_compile(&hdr_tr, &cxt);

    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.modifier_mode = PVR_MODIFIER_CHEAP_SHADOW;
    cxt.fmt.modifier = PVR_MODIFIER_ENABLE;
    pvr_poly_compile(&hdr_op_shadow, &cxt);

    pvr_mod_compile(&hdr_mod, PVR_LIST_OP_MOD, PVR_MODIFIER_OTHER_POLY,
                    PVR_CULLING_NONE);
    pvr_mod_compile(&hdr_mod_last, PVR_LIST_OP_MOD,
                    PVR_MODIFIER_INCLUDE_LAST_POLY, PVR_CULLING_NONE);
}

/**** Frames and read-back ****************************************/

/* Render one frame with the given scene. Returns the 32-bit VRAM address the
   PVR wrote to (frame buffer or texture), or 0 on a timeout. */
static uint32_t render(void (*scene)(void), int to_texture) {
    if(pvr_wait_ready() < 0)
        printf("HWTEST warning: pvr_wait_ready timed out before the scene\n");

    if(to_texture)
        pvr_scene_begin_rtt(rtt, W, H, RTT_STRIDE);
    else
        pvr_scene_begin();

    scene();
    pvr_scene_finish();

    /* Wait for the render to start, then to end. */
    if(pvr_wait_ready() < 0 || pvr_wait_render_done() < 0) {
        printf("HWTEST warning: render timed out\n");
        return 0;
    }

    return PVR_GET(PVR_RENDER_ADDR) & 0x00ffffff;
}

static uint16_t fb_pixel(uint32_t addr, int x, int y) {
    return ((volatile uint16_t *)(PVR_RAM_BASE + addr))[y * W + x];
}

static uint16_t rtt_pixel(int x, int y) {
    return ((volatile uint16_t *)rtt)[y * RTT_STRIDE + x];
}

static void fb_fill(uint32_t addr, uint16_t c) {
    volatile uint32_t *p = (volatile uint32_t *)(PVR_RAM_BASE + addr);
    uint32_t v = c | ((uint32_t)c << 16);
    int i;

    for(i = 0; i < W * H / 2; i++)
        p[i] = v;
}

static void rtt_fill(uint16_t c) {
    volatile uint16_t *p = (volatile uint16_t *)rtt;
    int x, y;

    for(y = 0; y < H; y++)
        for(x = 0; x < W; x++)
            p[y * RTT_STRIDE + x] = c;
}

static uint32_t rgb565_to_888(uint16_t c);

/* A saved frame, to compare pixel by pixel */
static uint16_t *saved;

static void fb_save(uint32_t addr) {
    int i;

    if(!saved)
        saved = malloc(W * H * 2);

    for(i = 0; i < W * H; i++)
        saved[i] = ((volatile uint16_t *)(PVR_RAM_BASE + addr))[i];
}

/* Returns the number of pixels that differ from the saved frame, and prints
   the first few. */
static int fb_compare(const char *tag, uint32_t addr) {
    int i, n = 0;
    uint16_t c;

    for(i = 0; i < W * H; i++) {
        c = ((volatile uint16_t *)(PVR_RAM_BASE + addr))[i];

        if(c != saved[i]) {
            if(n < 8)
                printf("HWTEST %s   (%3d,%3d) %06lx != %06lx\n", tag, i % W,
                       i / W, (unsigned long)rgb565_to_888(c),
                       (unsigned long)rgb565_to_888(saved[i]));
            n++;
        }
    }

    return n;
}

static uint32_t fb_checksum(uint32_t addr) {
    volatile uint32_t *p = (volatile uint32_t *)(PVR_RAM_BASE + addr);
    uint32_t sum = 0;
    int i;

    for(i = 0; i < W * H / 2; i++)
        sum = (sum * 31) ^ p[i];

    return sum;
}

/* Colour comparison, 8-bit components with a tolerance for 565 dithering */
#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

static uint32_t rgb565_to_888(uint16_t c) {
    uint32_t r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;

    return RGB((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
}

static int near(uint16_t c, uint32_t want) {
    uint32_t got = rgb565_to_888(c);
    int i;

    for(i = 0; i < 24; i += 8) {
        int d = (int)((got >> i) & 0xff) - (int)((want >> i) & 0xff);

        if(d < -20 || d > 20)
            return 0;
    }

    return 1;
}

/* Named probe points and their expected colours */
typedef struct {
    const char *name;
    int x, y;
} probe_t;

static void print_probe(const char *tag, const probe_t *p, uint16_t c) {
    printf("HWTEST %s   %-12s (%3d,%3d) = %06lx\n", tag, p->name, p->x, p->y,
           (unsigned long)rgb565_to_888(c));
}

/**** T1: bit 28 ***************************************************/

static void scene_t1(void) {
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&hdr_op, sizeof(hdr_op));
    quad(64, 160, 288, 320, 1.0f, 0xffff0000);
    pvr_list_finish();

    pvr_scene_next_pass();

    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&hdr_op, sizeof(hdr_op));
    quad(352, 160, 576, 320, 1.0f, 0xff00ff00);
    pvr_list_finish();
}

static const probe_t t1_probes[3] = {
    { "red/pass0", 176, 240 }, { "green/pass1", 464, 240 }, { "bg", 320, 240 }
};

static const uint32_t t1_want[3] = {
    RGB(255, 0, 0), RGB(0, 255, 0), RGB(0, 0, 128)
};

/* Returns 1 if all three probes match, 0 if they are all still magenta,
   -1 otherwise. */
static int t1_classify(const char *tag, uint32_t addr, int texture) {
    int i, ok = 1, magic = 1;

    for(i = 0; i < 3; i++) {
        uint16_t c = texture ? rtt_pixel(t1_probes[i].x, t1_probes[i].y) :
                     fb_pixel(addr, t1_probes[i].x, t1_probes[i].y);

        print_probe(tag, &t1_probes[i], c);
        ok &= near(c, t1_want[i]);
        magic &= c == MAGIC565;
    }

    return ok ? 1 : magic ? 0 : -1;
}

/* Average render time in microseconds of the T1 scene with the given hooks */
static uint64_t t1_time(uint32_t flags) {
    pvr_stats_t st;
    uint64_t sum = 0;
    int i;

    pvr_mp_test_flags = flags;
    render(scene_t1, 0);
    render(scene_t1, 0);

    for(i = 0; i < 32; i++) {
        render(scene_t1, 0);
        pvr_get_stats(&st);
        sum += st.rnd_last_time;
    }

    pvr_mp_test_flags = 0;

    return sum / 32 / 1000;
}

static void test1(void) {
    uint32_t addr, fb[2] = { 0, 0 };
    int i, r, a_ok = 1, b_fb, b_tx, rec = 1;
    const char *verdict;

    printf("HWTEST T1 ---- bit 28 (writeout control)\n");
    pvr_set_bg_color(0.0f, 0.0f, 0.5f);
    pvr_mp_test_flags = 0;
    reset_errors();

    /* A: normal two-pass frames, frame buffer and texture */
    for(i = 0; i < 4; i++) {
        addr = render(scene_t1, 0);
        fb[i & 1] = addr;
        if(i >= 2 && t1_classify("T1A-fb", addr, 0) != 1)
            a_ok = 0;
    }

    /* Render to texture (with FSAA the texture layout differs; skipped) */
    if(!HWTEST_FSAA) {
        addr = render(scene_t1, 1);
        if(t1_classify("T1A-txr", addr, 1) != 1)
            a_ok = 0;
    }

    printf("HWTEST T1A two passes, red in pass 0 and green in pass 1: %s\n",
           a_ok ? "PASS" : "FAIL");
    add_summary("T1A 2-pass frame:", a_ok ? "PASS" : "FAIL");

    /* B: bit 28 on the last pass too, frame buffers filled with magenta.
       0 = all frames untouched, 1 = all written, -1 = anything else */
    pvr_mp_test_flags = PVR_MP_TEST_LAST_NO_WRITEOUT;
    b_fb = 2;

    for(i = 0; i < 4; i++) {
        if(fb[0]) fb_fill(fb[0], MAGIC565);
        if(fb[1]) fb_fill(fb[1], MAGIC565);
        addr = render(scene_t1, 0);
        r = t1_classify("T1B-fb", addr, 0);
        b_fb = (b_fb == 2 || b_fb == r) ? r : -1;
    }

    if(!HWTEST_FSAA) {
        rtt_fill(MAGIC565);
        addr = render(scene_t1, 1);
        b_tx = t1_classify("T1B-txr", addr, 1);
    }
    else {
        b_tx = b_fb;
    }

    pvr_mp_test_flags = 0;

    verdict = (b_fb == 0 && b_tx == 0) ? "CONFIRMED: bit 28 = 1 keeps the tile on chip (no writeout)" :
              (b_fb == 1 && b_tx == 1) ? "REFUTED: tiles are written out with bit 28 set on the last pass" :
              "UNEXPECTED: see probe values";
    printf("HWTEST T1B bit 28 on the last pass: frame buffer %s, texture %s\n",
           b_fb == 0 ? "untouched" : b_fb == 1 ? "written" : "mixed/corrupt",
           HWTEST_FSAA ? "not tested" :
           b_tx == 0 ? "untouched" : b_tx == 1 ? "written" : "corrupt");
    printf("HWTEST T1B RESULT %s\n", verdict);
    add_summary("T1B bit 28 on last pass:",
                (b_fb == 0 && b_tx == 0) ? "NO WRITEOUT (confirmed)" :
                (b_fb == 1 && b_tx == 1) ? "WRITTEN (refuted)" : "UNEXPECTED");

    /* Back to normal: frames must come out right again. */
    for(i = 0; i < 2; i++) {
        addr = render(scene_t1, 0);
        if(t1_classify("T1C-fb", addr, 0) != 1)
            rec = 0;
    }

    printf("HWTEST T1C recovery after T1B: %s\n", rec ? "PASS" : "FAIL");
    add_summary("T1C recovery:", rec ? "PASS" : "FAIL");

    /* D: bit 28 clear on every pass (the tile is written out after each
       pass): is the picture still right? And render times of the three
       settings: bit 28 on the first pass (default), on no pass, on both. */
    pvr_mp_test_flags = PVR_MP_TEST_ALL_WRITEOUT;
    r = 1;
    for(i = 0; i < 4; i++) {
        addr = render(scene_t1, 0);
        if(i >= 2 && t1_classify("T1D-fb", addr, 0) != 1)
            r = 0;
    }
    pvr_mp_test_flags = 0;
    printf("HWTEST T1D bit 28 clear on every pass (writeout after each "
           "pass): picture %s\n", r ? "right" : "WRONG");
    add_summary("T1D writeout after each pass:", r ? "picture right" : "WRONG");

    printf("HWTEST T1E average render time (us): bit 28 on pass 0 only %lu, "
           "on no pass %lu, on both passes %lu\n",
           (unsigned long)t1_time(0),
           (unsigned long)t1_time(PVR_MP_TEST_ALL_WRITEOUT),
           (unsigned long)t1_time(PVR_MP_TEST_LAST_NO_WRITEOUT));
    print_errors("T1");
}

/**** T2: waiting before TA_LIST_CONT ******************************/

static uint32_t lcg;

static uint32_t rnd(void) {
    lcg = lcg * 1103515245 + 12345;
    return lcg >> 8;
}

#define T2_SMALL 3000
#define T2_BIG   150

static int t2_one_pass;     /* Everything in pass 0 (reference) */
static uint64_t t2_cont_us; /* Time spent in pvr_scene_next_pass() */

/* The pass-1 content; with the OP list already open in one-pass mode */
static void t2_pass1(void) {
    if(!t2_one_pass)
        pvr_list_begin(PVR_LIST_OP_POLY);

    pvr_prim(&hdr_op, sizeof(hdr_op));
    quad(100, 100, 200, 200, 50.0f, 0xff00ffff);
    quad(400, 300, 560, 420, 50.0f, 0xffffff00);
    pvr_list_finish();

    pvr_list_begin(PVR_LIST_TR_POLY);
    pvr_prim(&hdr_tr, sizeof(hdr_tr));
    quad(250, 150, 450, 350, 60.0f, 0x80ffffff);
    pvr_list_finish();
}

static void scene_t2(void) {
    uint64_t t;
    int i;

    lcg = 1;

    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&hdr_op, sizeof(hdr_op));

    /* Many small triangles in front */
    for(i = 0; i < T2_SMALL; i++) {
        float x = (float)(rnd() % (W - 24)), y = (float)(rnd() % (H - 24));
        float z = 2.0f + (float)(rnd() % 1000) / 100.0f;

        tri(x, y + 20, x + 10, y, x + 20, y + 20, z, 0xff000000 | rnd());
    }

    /* Then big triangles behind them, ending the pass: each one touches most
       tiles, so the TA has a lot of binning left when the CPU is done. */
    for(i = 0; i < T2_BIG; i++) {
        float z = 1.0f + (float)(rnd() % 100) / 1000.0f;

        tri((float)(rnd() % 64), (float)(H - 1 - rnd() % 64),
            (float)(W / 2 - 32 + rnd() % 64), (float)(rnd() % 64),
            (float)(W - 1 - rnd() % 64), (float)(H - 1 - rnd() % 64),
            z, 0xff000000 | rnd());
    }

    if(t2_one_pass) {
        t2_pass1();
        return;
    }

    pvr_list_finish();

    t = timer_us_gettime64();
    pvr_scene_next_pass();
    t = timer_us_gettime64() - t;

    if(t > t2_cont_us)
        t2_cont_us = t;

    t2_pass1();
}

static int t2_run(const char *tag, int frames, uint32_t ref) {
    int i, bad = 0, timeouts = 0;
    uint32_t addr, sum;

    t2_cont_us = 0;

    for(i = 0; i < frames; i++) {
        addr = render(scene_t2, 0);

        if(!addr) {
            timeouts++;
            continue;
        }

        sum = fb_checksum(addr);

        if(sum != ref) {
            if(bad < 5)
                printf("HWTEST %s frame %d checksum %08lx != %08lx\n", tag, i,
                       (unsigned long)sum, (unsigned long)ref);
            bad++;
        }
    }

    printf("HWTEST %s %d frames: %d mismatched, %d timeouts, "
           "max time in pvr_scene_next_pass %lu us\n", tag, frames, bad,
           timeouts, (unsigned long)t2_cont_us);

    return bad + timeouts;
}

static void test2(void) {
    uint32_t addr, ref, ref2;
    int bad_wait, bad_nowait, errs_wait, errs_nowait;
    const char *verdict, *brief;

    printf("HWTEST T2 ---- wait for the done interrupts before TA_LIST_CONT?\n");
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    pvr_mp_test_flags = 0;
    reset_errors();

    /* Information: the same picture with everything in pass 0 */
    t2_one_pass = 1;
    render(scene_t2, 0);
    addr = render(scene_t2, 0);
    fb_save(addr);
    t2_one_pass = 0;

    /* Reference: two passes, waiting for the done interrupts */
    render(scene_t2, 0);
    addr = render(scene_t2, 0);
    ref = fb_checksum(addr);
    printf("HWTEST T2 pixels that differ between one pass and two passes: %d "
           "(expected 0)\n", fb_compare("T2-1v2", addr));
    addr = render(scene_t2, 0);
    ref2 = fb_checksum(addr);
    printf("HWTEST T2 reference checksum %08lx (repeat %08lx: %s)\n",
           (unsigned long)ref, (unsigned long)ref2,
           ref == ref2 ? "stable" : "UNSTABLE, results below are meaningless");
    print_errors("T2-ref");

    reset_errors();
    bad_wait = t2_run("T2-wait", 200, ref);
    errs_wait = print_errors("T2-wait");

    reset_errors();
    pvr_mp_test_flags = PVR_MP_TEST_NO_WAIT;
    bad_nowait = t2_run("T2-nowait", 400, ref);
    errs_nowait = print_errors("T2-nowait");
    pvr_mp_test_flags = 0;

    if(ref != ref2 || bad_wait || errs_wait) {
        verdict = "INCONCLUSIVE: errors even with the wait";
        brief = "INCONCLUSIVE";
    }
    else if(bad_nowait || errs_nowait) {
        verdict = "WAIT NEEDED: corruption without the wait";
        brief = "NEEDED";
    }
    else {
        verdict = "NO WAIT NEEDED (in this test): identical without the wait";
        brief = "not needed (see serial)";
    }

    printf("HWTEST T2 RESULT %s\n", verdict);
    add_summary("T2 wait before LIST_CONT:", brief);
}

/**** T3: deferred shading in a z-keep pass ************************/

static void scene_t3(void) {
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&hdr_op, sizeof(hdr_op));
    quad(32, 32, 608, 448, 1.0f, 0xff808080);
    pvr_list_finish();

    pvr_list_begin(PVR_LIST_TR_POLY);
    pvr_prim(&hdr_tr, sizeof(hdr_tr));
    quad(32, 32, 320, 448, 2.0f, 0x80ff0000);
    pvr_list_finish();

    pvr_scene_next_pass();

    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&hdr_op, sizeof(hdr_op));
    /* In the red area, tile (5, 6) */
    quad(160, 200, 176, 216, 3.0f, 0xff0000ff);
    /* In the grey area, tile (15, 6) */
    quad(480, 200, 496, 216, 3.0f, 0xff0000ff);
    /* Behind the floor, tile (12, 6): must stay hidden */
    quad(400, 200, 416, 216, 0.5f, 0xff0000ff);
    pvr_list_finish();
}

static void test3(void) {
    static const probe_t pr[] = {
        { "tint", 186, 220 },      /* red-tinted floor, same tile as a blue quad */
        { "grey", 506, 220 },      /* grey floor, same tile as a blue quad */
        { "blue-red", 168, 208 },  /* pass-1 quad over the red area */
        { "blue-grey", 488, 208 }, /* pass-1 quad over the grey area */
        { "hidden", 408, 208 },    /* pass-1 quad behind the floor */
        { "bg", 16, 16 },          /* background outside the floor */
    };
    const uint32_t tint = RGB(191, 64, 64), grey = RGB(128, 128, 128),
                   blue = RGB(0, 0, 255), bg = RGB(0, 96, 0);
    const uint32_t want[] = { tint, grey, blue, blue, grey, bg };
    uint32_t addr;
    uint16_t c[6];
    int i, f, ok = 1;
    const char *verdict, *brief;

    printf("HWTEST T3 ---- opaque shading in a z-keep pass\n");
    pvr_set_bg_color(0.0f, 96.0f / 255.0f, 0.0f);
    pvr_mp_test_flags = 0;
    reset_errors();

    for(f = 0; f < 3; f++) {
        addr = render(scene_t3, 0);
        if(f < 2) continue;

        for(i = 0; i < 6; i++) {
            c[i] = fb_pixel(addr, pr[i].x, pr[i].y);
            print_probe("T3", &pr[i], c[i]);
            ok &= near(c[i], want[i]);
        }
    }

    if(ok) {
        verdict = "PASS: pass-1 opaque shading leaves pass-0 pixels alone, depth kept";
        brief = "own pixels only (PASS)";
    }
    else if(near(c[0], grey)) {
        verdict = "FAIL: pass 1 re-shaded pass-0 pixels (red tint lost)";
        brief = "RE-SHADES pass 0";
    }
    else if(near(c[0], bg) || near(c[1], bg)) {
        verdict = "FAIL: background came back in pass 1";
        brief = "BACKGROUND back";
    }
    else if(near(c[4], blue)) {
        verdict = "FAIL: pass 1 not depth tested against pass 0 (depth cleared)";
        brief = "DEPTH cleared";
    }
    else {
        verdict = "UNEXPECTED: see probe values";
        brief = "UNEXPECTED";
    }

    printf("HWTEST T3 RESULT %s\n", verdict);
    add_summary("T3 z-keep OP shading:", brief);
    print_errors("T3");
}

/**** T4: TA_OL_BASE between passes ********************************/

static void scene_t4(void) {
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&hdr_op, sizeof(hdr_op));
    quad(64, 160, 288, 320, 1.0f, 0xffff0000);
    pvr_list_finish();

    pvr_scene_next_pass();

    pvr_list_begin(PVR_LIST_TR_POLY);
    pvr_prim(&hdr_tr, sizeof(hdr_tr));
    quad(352, 160, 576, 320, 1.0f, 0x80ffffff);
    pvr_list_finish();
}

/* 'S'eparate passes (correct), 'O'verwritten, 'A'ppended, 'E'lsewhere,
   '?' anything else */
static char t4_classify(const char *tag, uint32_t addr) {
    static const probe_t pr[] = { { "left", 176, 240 }, { "right", 464, 240 } };
    uint16_t l = fb_pixel(addr, pr[0].x, pr[0].y);
    uint16_t r = fb_pixel(addr, pr[1].x, pr[1].y);
    int red = near(l, RGB(255, 0, 0)), black = near(l, 0);

    if(tag) {
        print_probe(tag, &pr[0], l);
        print_probe(tag, &pr[1], r);
    }

    if(red && near(r, RGB(128, 128, 128))) return 'S';
    if(black && near(r, RGB(191, 191, 191))) return 'O';
    if(red && near(r, RGB(191, 191, 191))) return 'A';
    if(red && near(r, 0)) return 'E';
    return '?';
}

static void test4(void) {
    uint32_t addr;
    int i, n[5] = { 0, 0, 0, 0, 0 }, ctl = 1;
    char k;
    const char *verdict, *brief;

    printf("HWTEST T4 ---- is moving TA_OL_BASE between passes needed?\n");
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    pvr_mp_test_flags = 0;
    reset_errors();

    for(i = 0; i < 3; i++) {
        addr = render(scene_t4, 0);
        if(i == 2 && t4_classify("T4-control", addr) != 'S')
            ctl = 0;
    }

    printf("HWTEST T4 control (TA_OL_BASE moved): %s\n", ctl ? "PASS" : "FAIL");
    print_errors("T4-control");

    reset_errors();
    pvr_mp_test_flags = PVR_MP_TEST_NO_OL_BASE;

    for(i = 0; i < 30; i++) {
        addr = render(scene_t4, 0);
        if(!addr) { n[4]++; continue; }
        k = t4_classify(i < 3 ? "T4-noolbase" : NULL, addr);
        n[k == 'S' ? 0 : k == 'O' ? 1 : k == 'A' ? 2 : k == 'E' ? 3 : 4]++;
    }

    pvr_mp_test_flags = 0;
    printf("HWTEST T4 without moving TA_OL_BASE, 30 frames: separate %d, "
           "overwritten %d, appended %d, elsewhere %d, other/timeout %d\n",
           n[0], n[1], n[2], n[3], n[4]);
    print_errors("T4-noolbase");

    if(!ctl) {
        verdict = "INCONCLUSIVE: control failed";
        brief = "INCONCLUSIVE";
    }
    else if(n[1] == 30) {
        verdict = "NEEDED: without it, pass 1 overwrites pass 0's first blocks";
        brief = "NEEDED (overwrites)";
    }
    else if(n[2] == 30) {
        verdict = "NEEDED: without it, pass 1 is appended to pass 0's lists";
        brief = "NEEDED (appends)";
    }
    else if(n[3] == 30) {
        verdict = "UNCLEAR: pass 1's lists went elsewhere";
        brief = "UNCLEAR (elsewhere)";
    }
    else if(n[0] == 30) {
        verdict = "UNCLEAR: passes stayed separate without it";
        brief = "UNCLEAR (separate)";
    }
    else {
        verdict = "NEEDED: without it the result is unstable or corrupt";
        brief = "NEEDED (corrupt)";
    }

    printf("HWTEST T4 RESULT %s\n", verdict);
    add_summary("T4 TA_OL_BASE move:", brief);

    /* Back to normal */
    for(i = 0; i < 2; i++)
        addr = render(scene_t4, 0);
    k = t4_classify("T4-recovery", addr);
    printf("HWTEST T4 recovery: %s\n", k == 'S' ? "PASS" : "FAIL");
    add_summary("T4 recovery:", k == 'S' ? "PASS" : "FAIL");
}

/**** T5: modifier volumes across passes ***************************/

static int t5_pass1_volume;

static void scene_t5(void) {
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&hdr_op_shadow, sizeof(hdr_op_shadow));
    quad(32, 32, 608, 448, 1.0f, 0xff808080);
    pvr_list_finish();

    pvr_list_begin(PVR_LIST_OP_MOD);
    mod_rect(32, 32, 320, 448);
    pvr_list_finish();

    pvr_scene_next_pass();

    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&hdr_op_shadow, sizeof(hdr_op_shadow));
    quad(200, 160, 440, 320, 2.0f, 0xffffffff);
    pvr_list_finish();

    if(t5_pass1_volume) {
        pvr_list_begin(PVR_LIST_OP_MOD);
        mod_rect(320, 32, 608, 448);
        pvr_list_finish();
    }
}

static void test5(void) {
    static const probe_t pr[] = {
        { "floor-L", 100, 100 },   /* pass-0 floor in the pass-0 volume */
        { "floor-R", 540, 100 },   /* pass-0 floor outside it */
        { "quad-L", 260, 240 },    /* pass-1 quad in the pass-0 volume */
        { "quad-R", 380, 240 },    /* pass-1 quad outside it */
    };
    uint32_t addr;
    uint16_t a[4], b[4];
    int i;
    const char *v1, *v2, *b1, *b2;

    printf("HWTEST T5 ---- modifier volumes across passes (cheap shadow)\n");
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    pvr_set_shadow_scale(true, 0.5f);
    pvr_mp_test_flags = 0;
    reset_errors();

    t5_pass1_volume = 0;
    for(i = 0; i < 3; i++)
        addr = render(scene_t5, 0);
    for(i = 0; i < 4; i++) {
        a[i] = fb_pixel(addr, pr[i].x, pr[i].y);
        print_probe("T5A", &pr[i], a[i]);
    }

    t5_pass1_volume = 1;
    for(i = 0; i < 3; i++)
        addr = render(scene_t5, 0);
    for(i = 0; i < 4; i++) {
        b[i] = fb_pixel(addr, pr[i].x, pr[i].y);
        print_probe("T5B", &pr[i], b[i]);
    }

    pvr_set_shadow_scale(false, 1.0f);

    if(!near(a[0], RGB(64, 64, 64)) || !near(a[1], RGB(128, 128, 128))) {
        v1 = "INCONCLUSIVE: the pass-0 volume didn't shadow the floor as expected";
        b1 = "INCONCLUSIVE";
    }
    else if(near(a[2], RGB(255, 255, 255)) && near(a[3], RGB(255, 255, 255))) {
        v1 = "a pass-0 volume does NOT affect pass-1 polygons (stencil cleared)";
        b1 = "no (cleared)";
    }
    else if(near(a[2], RGB(128, 128, 128)) && near(a[3], RGB(255, 255, 255))) {
        v1 = "a pass-0 volume DOES shadow pass-1 polygons (stencil kept)";
        b1 = "YES (kept)";
    }
    else {
        v1 = "UNEXPECTED: see probe values";
        b1 = "UNEXPECTED";
    }

    if(near(b[1], RGB(128, 128, 128)) && near(b[3], RGB(128, 128, 128))
       && near(b[2], rgb565_to_888(a[2]))) {
        v2 = "a pass-1 volume shadows pass-1 polygons only";
        b2 = "own pass only";
    }
    else if(near(b[1], RGB(64, 64, 64)) && near(b[3], RGB(128, 128, 128))) {
        v2 = "a pass-1 volume also shadows pass-0 pixels";
        b2 = "ALSO pass-0 pixels";
    }
    else {
        v2 = "UNEXPECTED: see probe values";
        b2 = "UNEXPECTED";
    }

    printf("HWTEST T5 RESULT A: %s\n", v1);
    printf("HWTEST T5 RESULT B: %s\n", v2);
    add_summary("T5A pass-0 vol on pass 1:", b1);
    add_summary("T5B pass-1 vol on pass 0:", b2);
    print_errors("T5");
}

/**** Main *********************************************************/

static pvr_init_params_t params = {
    .opb_sizes = { PVR_BINSIZE_16, PVR_BINSIZE_16, PVR_BINSIZE_16,
                   PVR_BINSIZE_0, PVR_BINSIZE_0 },
    .vertex_buf_size = 1024 * 1024,
    .fsaa_enabled = HWTEST_FSAA,
    .opb_overflow_count = 8,
    .pass_count = 2,
};

static void show_summary(void) {
    int i;
    maple_device_t *cont;
    cont_state_t *st;
    uint64_t end = timer_ms_gettime64() + 60000;

    pvr_shutdown();
    vid_set_mode(DM_640x480, PM_RGB565);
    vid_clear(0, 0, 0);

    bfont_draw_str(vram_s + 16 * W + 16, W, true,
                   HWTEST_FSAA ? "multipass_hwtest (FSAA)" :
                                 "multipass_hwtest (no FSAA)");

    for(i = 0; i < summary_n; i++)
        bfont_draw_str(vram_s + (56 + i * 26) * W + 8, W, true, summary[i]);

    bfont_draw_str(vram_s + (H - 32) * W + 16, W, true,
                   "Details on serial. Start or 60 s to exit.");

    while(timer_ms_gettime64() < end) {
        cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        if(cont && (st = (cont_state_t *)maple_dev_status(cont)) &&
           (st->buttons & CONT_START))
            break;
        thd_sleep(50);
    }
}

int main(int argc, char *argv[]) {
    uint16_t code;

    (void)argc;
    (void)argv;

    printf("HWTEST multipass_hwtest start, FSAA %s\n", HWTEST_FSAA ? "on" : "off");

    if(pvr_init(&params) < 0) {
        printf("HWTEST pvr_init failed\n");
        return 1;
    }

    for(code = ASIC_EVT_PVR_ISP_OUTOFMEM; code <= ASIC_EVT_PVR_TA_INPUT_OVERFLOW; code++) {
        asic_evt_set_handler(code, err_handler, NULL);
        asic_evt_enable(code, ASIC_IRQ_DEFAULT);
    }

    rtt = pvr_mem_malloc(RTT_STRIDE * 512 * 2);
    compile_headers();

    test1();
    test3();
    test5();
    test2();
    test4();

    printf("HWTEST done\n");

    for(code = ASIC_EVT_PVR_ISP_OUTOFMEM; code <= ASIC_EVT_PVR_TA_INPUT_OVERFLOW; code++) {
        asic_evt_disable(code, ASIC_IRQ_DEFAULT);
        asic_evt_remove_handler(code);
    }

    show_summary();

    return 0;
}
