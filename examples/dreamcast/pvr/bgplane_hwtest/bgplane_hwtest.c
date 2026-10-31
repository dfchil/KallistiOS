/* KallistiOS ##version##

   examples/dreamcast/pvr/bgplane_hwtest/bgplane_hwtest.c
   Copyright (C) 2026 Daniel Fairchild

   Hardware test for the background plane (pvr_set_bg_colors(),
   pvr_set_bg_poly(), pvr_set_bg_texture()).

   Part 1 runs by itself and prints its results on the serial console. It
   renders background planes with nothing in front, reads the pixels back
   from the frame buffer (and from a render-to-texture target) and compares
   them with what the plane equation predicts:

     Q0  pvr_set_bg_color(): one color everywhere, as before
     Q1  three corner colors: a gradient over the whole screen, inside and
         outside the triangle of the three vertices (screen, RTT, FSAA)
     Q2  colors extrapolated beyond 0..255: clamped or wrapped?
     Q3  vertices off screen (negative, beyond the screen, huge), either
         winding, z other than the default
     Q4  textured plane: texture coordinates over the whole screen, repeat,
         and render time against a flat plane for several texture sizes
     Q6  (extra) vertices with different z: perspective-correct or affine?
     Q7  (extra) does the vertices' z change the plane's depth, or is it
         always the pvr_set_zclip() value?

   Dithering is switched off while reading back, so the values are exact.
   A render-to-texture target is filled with magenta (255 0 255) first, so
   "got 255 0 255" means the render did not write there.

   Emulators are no reference here: Flycast, for one, ignores the positions
   of the background vertices and draws its own full-screen quad, so it fails
   Q1-Q4 by design.

   Part 2 shows the same planes on screen for a visual check, with a text
   overlay saying what to look for:

     D-pad left/right   previous/next page
     A                  page action (next vertex set, filter, gradient)
     B                  dithering on/off
     Y                  FSAA on/off
     Start              exit
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <float.h>

#include <kos.h>
#include <dc/pvr.h>
#include <dc/video.h>
#include <dc/biosfont.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

#define SCREEN_W    640
#define SCREEN_H    480

#define RTT_W       1024
#define RTT_H       512

#define TEXT_W      512
#define TEXT_H      512
#define TEXT_LINES  (TEXT_H / BFONT_HEIGHT)

#define COORD_SIZE  32

#define PM_DITHER   8

enum target { TGT_SCREEN, TGT_RTT };

static pvr_init_params_t params = {
    { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0,
      PVR_BINSIZE_0 },
    256 * 1024, 0, 0, 0, 0, 0
};

static pvr_ptr_t rtt_txr, coord_txr, text_txr[2];
static pvr_ptr_t cost_txr[4];
static const int cost_size[4] = { 8, 64, 256, 1024 };
static pvr_poly_hdr_t text_hdr[2], flat_hdr, tex_hdr;
static uint16_t text_buf[TEXT_W * TEXT_H];
static uint16_t big_buf[1024 * 1024];
static int text_cur;
static uint32_t fb_cfg2;

/* Summary lines shown on the first page of part 2 */
static char summary[TEXT_LINES][48];
static int summary_lines;

static void report(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static void add_summary(const char *fmt, ...) {
    va_list ap;

    if(summary_lines >= (int)(sizeof(summary) / sizeof(summary[0])))
        return;

    va_start(ap, fmt);
    vsnprintf(summary[summary_lines++], sizeof(summary[0]), fmt, ap);
    va_end(ap);
    printf("SUMMARY %s\n", summary[summary_lines - 1]);
}

/* ---- Setup ------------------------------------------------------------- */

static void make_textures(void) {
    pvr_poly_cxt_t cxt;
    int i, x, y, n;

    for(i = 0; i < 2; i++) {
        text_txr[i] = pvr_mem_malloc(TEXT_W * TEXT_H * 2);
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY,
                         PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED,
                         TEXT_W, TEXT_H, text_txr[i], PVR_FILTER_NONE);
        pvr_poly_compile(&text_hdr[i], &cxt);
    }

    rtt_txr = pvr_mem_malloc(RTT_W * RTT_H * 2);

    /* Coordinate texture: red = s, green = 2 * t, blue = s ^ t. Non-twiddled
       so it can be written directly. */
    coord_txr = pvr_mem_malloc(COORD_SIZE * COORD_SIZE * 2);

    for(y = 0; y < COORD_SIZE; y++)
        for(x = 0; x < COORD_SIZE; x++)
            big_buf[y * COORD_SIZE + x] = (x << 11) | ((2 * y) << 5) |
                                          ((x ^ y) & 31);

    pvr_txr_load(big_buf, coord_txr, COORD_SIZE * COORD_SIZE * 2);

    /* Textures for the render time table, twiddled as usual: a checker of
       cells with a color per cell, so it is also good to look at. */
    for(i = 0; i < 4; i++) {
        n = cost_size[i];
        cost_txr[i] = pvr_mem_malloc(n * n * 2);

        for(y = 0; y < n; y++)
            for(x = 0; x < n; x++) {
                int cx = x * 8 / n, cy = y * 8 / n;
                int r = (cx * 4 + 2) & 31, g = (cy * 8 + 4) & 63;
                int b = ((cx ^ cy) & 1) ? 28 : 6;

                if(((x * 64 / n) ^ (y * 64 / n)) & 1)
                    r = r / 2, g = g / 2;

                big_buf[y * n + x] = (r << 11) | (g << 5) | b;
            }

        pvr_txr_load_ex(big_buf, cost_txr[i], n, n, PVR_TXRLOAD_16BPP);
    }

    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&flat_hdr, &cxt);

    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
                     PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED, 256, 256,
                     cost_txr[2], PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&tex_hdr, &cxt);
}

static void init_pvr(bool fsaa) {
    params.fsaa_enabled = fsaa;
    pvr_init(&params);
    make_textures();
}

static void reinit_pvr(bool fsaa) {
    pvr_wait_ready();
    pvr_wait_render_done();
    pvr_shutdown();
    init_pvr(fsaa);
}

static void set_dither(bool on) {
    PVR_SET(PVR_FB_CFG_2, on ? (fb_cfg2 | PM_DITHER) : (fb_cfg2 & ~PM_DITHER));
}

/* ---- Drawing helpers --------------------------------------------------- */

static void quad(const float x[4], const float y[4], float z, const float u[4],
                 const float v[4], uint32_t argb) {
    pvr_vertex_t vert;
    int i;

    for(i = 0; i < 4; i++) {
        vert.flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        vert.x = x[i];
        vert.y = y[i];
        vert.z = z;
        vert.u = u ? u[i] : 0.0f;
        vert.v = v ? v[i] : 0.0f;
        vert.argb = argb;
        vert.oargb = 0;
        pvr_prim(&vert, sizeof(vert));
    }
}

static void rect(float x0, float y0, float x1, float y1, float z, float u1,
                 float v1) {
    float xs = params.fsaa_enabled ? 2.0f : 1.0f;
    const float x[4] = { x0 * xs, x1 * xs, x0 * xs, x1 * xs };
    const float y[4] = { y0, y0, y1, y1 };
    const float u[4] = { 0.0f, u1, 0.0f, u1 };
    const float v[4] = { 0.0f, 0.0f, v1, v1 };

    quad(x, y, z, u, v, 0xffffffff);
}

/* Draw text lines (separated by '\n'). Translucent list must be open. */
static void draw_text(const char *str) {
    char line[TEXT_W / BFONT_THIN_WIDTH + 1];
    const char *end;
    size_t len;
    int row = 0;

    memset(text_buf, 0, sizeof(text_buf));

    while(*str && row < TEXT_LINES) {
        end = strchr(str, '\n');
        len = end ? (size_t)(end - str) : strlen(str);

        if(len >= sizeof(line))
            len = sizeof(line) - 1;

        memcpy(line, str, len);
        line[len] = '\0';

        if(len)
            bfont_draw_str_ex(text_buf + row * BFONT_HEIGHT * TEXT_W, TEXT_W,
                              0xffff, 0x8000, 16, true, line);

        str += end ? (size_t)(end - str) + 1 : len;
        row++;
    }

    text_cur ^= 1;
    pvr_txr_load(text_buf, text_txr[text_cur], sizeof(text_buf));
    pvr_prim(&text_hdr[text_cur], sizeof(pvr_poly_hdr_t));
    rect(16.0f, 16.0f, 16.0f + TEXT_W, 16.0f + TEXT_H, 10.0f, 1.0f, 1.0f);
}

/* ---- Rendering and reading back ---------------------------------------- */

/* Render the background plane alone, three times so both frame buffers have
   it, and wait until it is on screen. Returns -1 if the PVR timed out, -2 if
   the render-to-texture target was not written at all. */
static int render_bg(enum target tgt) {
    int i, rv = 0;

    /* Fill the render-to-texture target with a marker (magenta, which reads
       back as 255 0 255), to tell "not written" from a black result. */
    if(tgt == TGT_RTT) {
        pvr_wait_render_done();

        for(i = 0; i < RTT_W * SCREEN_H; i++)
            ((volatile uint16_t *)rtt_txr)[i] = 0xf81f;
    }

    for(i = 0; i < 3; i++) {
        if(pvr_wait_ready() < 0)
            rv = -1;

        if(tgt == TGT_RTT)
            pvr_scene_begin_rtt(rtt_txr, SCREEN_W, SCREEN_H, RTT_W);
        else
            pvr_scene_begin();

        pvr_scene_finish();
    }

    if(pvr_wait_ready() < 0)
        rv = -1;

    if(pvr_wait_render_done() < 0)
        rv = -1;

    vid_waitvbl();
    vid_waitvbl();

    if(rv == 0 && tgt == TGT_RTT) {
        for(i = 0; i < SCREEN_H; i += 7)
            if(((volatile uint16_t *)rtt_txr)[i * RTT_W + i] != 0xf81f)
                break;

        if(i >= SCREEN_H)
            rv = -2;
    }

    return rv;
}

static const char *fail_name(int rv) {
    return rv == -2 ? "NOT WRITTEN" : "TIMEOUT";
}

static uint16_t read_px(enum target tgt, int x, int y) {
    if(tgt == TGT_RTT)
        return ((volatile uint16_t *)rtt_txr)[y * RTT_W + x];

    return ((volatile uint16_t *)vram_s)[y * SCREEN_W + x];
}

static void expand565(uint16_t p, int c[3]) {
    int r = p >> 11, g = (p >> 5) & 63, b = p & 31;

    c[0] = (r << 3) | (r >> 2);
    c[1] = (g << 2) | (g >> 4);
    c[2] = (b << 3) | (b >> 2);
}

/* Barycentric coordinates of (x, y) for the triangle v, extended over the
   whole plane. */
static void bary(const pvr_bg_vertex_t v[3], float x, float y, float l[3]) {
    float det = (v[1].x - v[0].x) * (v[2].y - v[0].y)
                - (v[2].x - v[0].x) * (v[1].y - v[0].y);

    l[1] = ((x - v[0].x) * (v[2].y - v[0].y)
            - (v[2].x - v[0].x) * (y - v[0].y)) / det;
    l[2] = ((v[1].x - v[0].x) * (y - v[0].y)
            - (x - v[0].x) * (v[1].y - v[0].y)) / det;
    l[0] = 1.0f - l[1] - l[2];
}

/* Interpolate per-vertex values a over the plane: affine, or perspective
   correct with the vertices' z as 1/w. */
static float interp(const pvr_bg_vertex_t v[3], const float l[3],
                    const float a[3], bool persp) {
    if(!persp)
        return l[0] * a[0] + l[1] * a[1] + l[2] * a[2];

    return (l[0] * v[0].z * a[0] + l[1] * v[1].z * a[1] + l[2] * v[2].z * a[2])
           / (l[0] * v[0].z + l[1] * v[1].z + l[2] * v[2].z);
}

static void expect_color(const pvr_bg_vertex_t v[3], const float l[3],
                         bool persp, float e[3]) {
    float a[3];
    int c, i;

    for(c = 0; c < 3; c++) {
        for(i = 0; i < 3; i++)
            a[i] = (v[i].argb >> (16 - 8 * c)) & 0xff;

        e[c] = interp(v, l, a, persp);
    }
}

/* Texel of the coordinate texture expected at barycentric l. Returns false
   if the sample is too close to a texel edge to be sure. */
static bool expect_texel(const pvr_bg_vertex_t v[3], const float l[3],
                         bool persp, uint16_t *texel) {
    float au[3] = { v[0].u, v[1].u, v[2].u };
    float av[3] = { v[0].v, v[1].v, v[2].v };
    float u = interp(v, l, au, persp) * COORD_SIZE;
    float t = interp(v, l, av, persp) * COORD_SIZE;
    float fu = u - floorf(u), ft = t - floorf(t);
    int s, r;

    if(fu < 0.2f || fu > 0.8f || ft < 0.2f || ft > 0.8f)
        return false;

    s = (int)floorf(u) & (COORD_SIZE - 1);
    r = (int)floorf(t) & (COORD_SIZE - 1);
    *texel = (s << 11) | ((2 * r) << 5) | ((s ^ r) & 31);

    return true;
}

typedef struct {
    int n[2];           /* samples inside, outside the triangle */
    int maxerr[2][3];   /* color: max error per channel, in 8-bit units */
    int bad[2];         /* samples over tolerance (color), mismatches (tex) */
    int skipped;        /* expected color out of 0..255 / near texel edge */
    double sqerr[2];    /* summed squared error: affine, perspective */
    int fail;           /* render_bg() result, 0 if the render worked */
} result_t;

/* Compare the picture with the plane given by v (in render target
   coordinates). With FSAA a screen pixel is the average of two render
   pixels, whose mean position is 2x + 1. */
static void check(enum target tgt, const pvr_bg_vertex_t v[3], bool textured,
                  bool persp, result_t *res) {
    static const int tol[3] = { 12, 8, 12 };
    float x, y, l[3], e[3], ea[3];
    uint16_t p, texel;
    int sx, sy, c, o[3], t[3], err, in, k, bad;

    memset(res, 0, sizeof(*res));

    for(sy = 6; sy < SCREEN_H; sy += 12) {
        for(sx = 4; sx < SCREEN_W; sx += 8) {
            x = params.fsaa_enabled && tgt == TGT_SCREEN ? 2.0f * sx + 1.0f
                                                         : sx + 0.5f;
            y = sy + 0.5f;
            bary(v, x, y, l);
            in = (l[0] < 0.0f || l[1] < 0.0f || l[2] < 0.0f);
            p = read_px(tgt, sx, sy);

            if(textured) {
                if(!expect_texel(v, l, persp, &texel)) {
                    res->skipped++;
                    continue;
                }

                expand565(p, o);
                expand565(texel, t);
                bad = 0;

                for(c = 0; c < 3; c++)
                    if(abs(o[c] - t[c]) > 9)
                        bad = 1;

                res->n[in]++;
                res->bad[in] += bad;
                continue;
            }

            expect_color(v, l, persp, e);

            for(c = 0; c < 3; c++)
                if(e[c] < 0.0f || e[c] > 255.0f)
                    break;

            if(c < 3) {
                res->skipped++;
                continue;
            }

            expand565(p, o);
            res->n[in]++;
            bad = 0;

            for(c = 0; c < 3; c++) {
                err = abs(o[c] - (int)e[c]);

                if(err > res->maxerr[in][c])
                    res->maxerr[in][c] = err;

                if(err > tol[c])
                    bad = 1;
            }

            res->bad[in] += bad;

            /* For the z test, how well both models fit */
            expect_color(v, l, !persp, ea);

            for(c = 0; c < 3; c++) {
                for(k = 0; k < 2; k++) {
                    float d = o[c] - (k == (int)persp ? e[c] : ea[c]);
                    res->sqerr[k] += d * d;
                }
            }
        }
    }
}

static const char *tgt_name(enum target tgt) {
    if(tgt == TGT_RTT)
        return "rtt";

    return params.fsaa_enabled ? "fsaa" : "screen";
}

/* Render and check one plane, print the result. Returns true on pass. */
static bool run_check(const char *name, enum target tgt,
                      const pvr_bg_vertex_t v[3], bool textured, bool persp,
                      result_t *res) {
    bool pass;

    int rv = render_bg(tgt);

    memset(res, 0, sizeof(*res));

    if(rv) {
        res->fail = rv;
        report("%s %s: %s\n", name, tgt_name(tgt), fail_name(rv));
        return false;
    }

    check(tgt, v, textured, persp, res);
    pass = res->n[0] + res->n[1] > 0 && res->bad[0] == 0 && res->bad[1] == 0;

    if(textured)
        report("%s %s: %s  texel mismatches inside %d/%d, outside %d/%d "
               "(%d near edges skipped)\n", name, tgt_name(tgt),
               pass ? "PASS" : "FAIL", res->bad[0], res->n[0], res->bad[1],
               res->n[1], res->skipped);
    else
        report("%s %s: %s  max err inside r%d g%d b%d (%d), outside r%d g%d "
               "b%d (%d); over tolerance %d/%d; %d out of range skipped\n",
               name, tgt_name(tgt), pass ? "PASS" : "FAIL",
               res->maxerr[0][0], res->maxerr[0][1], res->maxerr[0][2],
               res->n[0], res->maxerr[1][0], res->maxerr[1][1],
               res->maxerr[1][2], res->n[1], res->bad[0] + res->bad[1],
               res->n[0] + res->n[1], res->skipped);

    return pass;
}

/* Print what is on screen at a few points. */
static void dump_points(enum target tgt, const pvr_bg_vertex_t v[3]) {
    static const int pts[][2] = {
        { 4, 474 }, { 4, 6 }, { 634, 474 }, { 634, 6 }, { 320, 240 },
        { 480, 120 }
    };
    float l[3], e[3], x;
    int i, o[3];

    for(i = 0; i < 6; i++) {
        x = params.fsaa_enabled && tgt == TGT_SCREEN ? 2.0f * pts[i][0] + 1.0f
                                                     : pts[i][0] + 0.5f;
        bary(v, x, pts[i][1] + 0.5f, l);
        expect_color(v, l, false, e);
        expand565(read_px(tgt, pts[i][0], pts[i][1]), o);
        report("    (%3d,%3d) got %3d %3d %3d  plane %4d %4d %4d\n",
               pts[i][0], pts[i][1], o[0], o[1], o[2],
               (int)floorf(e[0]), (int)floorf(e[1]), (int)floorf(e[2]));
    }
}

/* ---- The tests --------------------------------------------------------- */

static void make_vert(pvr_bg_vertex_t *v, float x, float y, float z, float u,
                      float vv, uint32_t argb) {
    v->x = x;
    v->y = y;
    v->z = z;
    v->u = u;
    v->v = vv;
    v->argb = argb;
}

/* The default vertices as KOS places them, for a w x h render target. */
static void default_verts(pvr_bg_vertex_t v[3], float w, float h,
                          uint32_t bl, uint32_t tl, uint32_t br) {
    make_vert(&v[0], 0.0f, h, FLT_EPSILON, 0.0f, 1.0f, bl);
    make_vert(&v[1], 0.0f, 0.0f, FLT_EPSILON, 0.0f, 0.0f, tl);
    make_vert(&v[2], w, h, FLT_EPSILON, 1.0f, 1.0f, br);
}

static float render_w(enum target tgt) {
    return tgt == TGT_SCREEN && params.fsaa_enabled ? 2.0f * SCREEN_W
                                                    : SCREEN_W;
}

/* Q0: pvr_set_bg_color() as before, one color everywhere. */
static void test_q0(enum target tgt) {
    pvr_bg_vertex_t v[3];
    result_t res;
    bool pass;

    pvr_set_bg_color(0.2f, 0.4f, 0.6f);
    default_verts(v, render_w(tgt), SCREEN_H, 0x00336699, 0x00336699,
                  0x00336699);
    pass = run_check("Q0 flat color", tgt, v, false, false, &res);
    add_summary("Q0 %-6s flat %s", tgt_name(tgt),
                res.fail ? fail_name(res.fail) : pass ? "PASS" : "FAIL");
}

/* Q1 colors: red grows to the right, green to the top, blue constant. All
   four corners are in range (top-right is 255, 255, 128). */
#define Q1_BL 0xff000080
#define Q1_TL 0xff00ff80
#define Q1_BR 0xffff0080

static void test_q1(enum target tgt) {
    pvr_bg_vertex_t v[3];
    result_t res;
    bool pass;

    default_verts(v, render_w(tgt), SCREEN_H, Q1_BL, Q1_TL, Q1_BR);
    pvr_set_bg_colors(Q1_BL, Q1_TL, Q1_BR);
    pass = run_check("Q1 gradient", tgt, v, false, false, &res);

    if(!res.fail)
        dump_points(tgt, v);

    add_summary("Q1 %-6s %s in %d/%d out %d/%d", tgt_name(tgt),
                res.fail ? fail_name(res.fail) : pass ? "PASS" : "FAIL",
                res.n[0] - res.bad[0], res.n[0], res.n[1] - res.bad[1],
                res.n[1]);
}

/* Q2 colors: red goes up to 400 at the top-right, green down to -200. */
#define Q2_BL 0xff00c864
#define Q2_TL 0xffc80064
#define Q2_BR 0xffc80064

static void test_q2(enum target tgt) {
    pvr_bg_vertex_t v[3];
    float l[3], e[3];
    int o[3], c, sx, sy, k, i;
    int rv, n[2] = { 0, 0 }, clampok[2] = { 0, 0 }, wrapok[2] = { 0, 0 };
    int shown[2] = { 0, 0 };
    const char *verdict[2];

    default_verts(v, render_w(tgt), SCREEN_H, Q2_BL, Q2_TL, Q2_BR);
    pvr_set_bg_colors(Q2_BL, Q2_TL, Q2_BR);

    if((rv = render_bg(tgt)) != 0) {
        report("Q2 %s: %s\n", tgt_name(tgt), fail_name(rv));
        add_summary("Q2 %-6s %s", tgt_name(tgt), fail_name(rv));
        return;
    }

    report("Q2 overflow %s: samples (x,y) channel plane got clamp wrap\n",
           tgt_name(tgt));

    /* The top-right quarter, where red is above 255 and green below 0. */
    for(sy = 2; sy < 200; sy += 12) {
        for(sx = 320; sx < SCREEN_W; sx += 8) {
            float x = params.fsaa_enabled && tgt == TGT_SCREEN
                      ? 2.0f * sx + 1.0f : sx + 0.5f;

            bary(v, x, sy + 0.5f, l);
            expect_color(v, l, false, e);
            expand565(read_px(tgt, sx, sy), o);

            for(c = 0; c < 2; c++) {
                /* red is the over case (k = 0), green the under case */
                float ev = e[c];
                int clampv, wrapv, dc, dw;

                k = c;

                if((k == 0 && ev < 268.0f) || (k == 1 && ev > -12.0f))
                    continue;

                clampv = ev < 0.0f ? 0 : 255;
                wrapv = (int)floorf(ev) & 255;
                dc = abs(o[c] - clampv);
                dw = abs(o[c] - wrapv);

                if(256 - dw < dw)
                    dw = 256 - dw;

                n[k]++;
                clampok[k] += dc <= 12;
                wrapok[k] += dw <= 12;

                if(shown[k] < 4 && (sx % 96) == 32) {
                    report("    (%3d,%3d) %s plane %4d got %3d clamp %3d "
                           "wrap %3d\n", sx, sy, c ? "g" : "r", (int)ev,
                           o[c], clampv, wrapv);
                    shown[k]++;
                }
            }
        }
    }

    for(i = 0; i < 2; i++) {
        if(n[i] == 0)
            verdict[i] = "NO SAMPLES";
        else if(clampok[i] == n[i])
            verdict[i] = "CLAMP";
        else if(wrapok[i] == n[i])
            verdict[i] = "WRAP";
        else
            verdict[i] = "OTHER";

        report("Q2 %s %s: %s (clamp fits %d/%d, wrap fits %d/%d)\n",
               tgt_name(tgt), i ? "below 0" : "above 255", verdict[i],
               clampok[i], n[i], wrapok[i], n[i]);
    }

    dump_points(tgt, v);
    add_summary("Q2 %-6s >255 %s, <0 %s", tgt_name(tgt), verdict[0],
                verdict[1]);
}

/* Q3: vertex sets, all on the plane red = 64 + 96 x / 640,
   green = 64 + 96 y / 480, blue = 100 (or flat / nearly flat). */
typedef struct {
    const char *name;
    float x[3], y[3], z;
    int plane;      /* 0: the plane above, 1: flat, 2: very gentle */
} vset_t;

static const vset_t vsets[] = {
    { "a screen corners z=1", { 0, 0, 640 }, { 480, 0, 480 }, 1.0f, 0 },
    { "b off screen top-left", { -320, -160, -320 }, { -240, -240, -120 },
      1.0f, 0 },
    { "c off screen bot-right", { 960, 800, 960 }, { 720, 720, 600 }, 1.0f,
      0 },
    { "d covers the screen", { -320, 320, 960 }, { 720, -240, 720 }, 1.0f,
      0 },
    { "e reversed winding", { 0, 640, 0 }, { 480, 480, 0 }, 1.0f, 0 },
    { "f z=1000", { 0, 0, 640 }, { 480, 0, 480 }, 1000.0f, 0 },
    { "g tiny (flat)", { 100, 100, 100.5f }, { 100.5f, 100, 100.5f },
      1.0f, 0 },
    { "h flat at +-10000", { -10000, -10000, 10000 }, { -10000, 10000, -10000 },
      1.0f, 1 },
    { "i gentle at +-4000", { -4000, 4000, -4000 }, { -4000, -4000, 4000 },
      1.0f, 2 },
};

#define NUM_VSETS (int)(sizeof(vsets) / sizeof(vsets[0]))

static uint32_t plane_color(int plane, float x, float y) {
    int r, g, b;

    if(plane == 1)
        return 0xff5a96d2;

    if(plane == 2) {
        r = (int)(128.0f + 0.02f * x + 0.5f);
        g = (int)(128.0f + 0.02f * y + 0.5f);
        b = 100;
    }
    else {
        r = (int)(64.0f + 96.0f * x / 640.0f + 0.5f);
        g = (int)(64.0f + 96.0f * y / 480.0f + 0.5f);
        b = 100;
    }

    return 0xff000000 | (r << 16) | (g << 8) | b;
}

/* Fill v with vertex set i, in render target coordinates. */
static void make_vset(int i, enum target tgt, pvr_bg_vertex_t v[3]) {
    float xs = render_w(tgt) / SCREEN_W;
    int k;

    for(k = 0; k < 3; k++)
        make_vert(&v[k], vsets[i].x[k] * xs, vsets[i].y[k], vsets[i].z,
                  0.0f, 0.0f,
                  plane_color(vsets[i].plane, vsets[i].x[k], vsets[i].y[k]));
}

static void test_q3(enum target tgt) {
    pvr_bg_vertex_t v[3];
    result_t res;
    char name[48];
    int i, passed = 0, timeouts = 0;

    for(i = 0; i < NUM_VSETS; i++) {
        make_vset(i, tgt, v);

        if(pvr_set_bg_poly(v) < 0) {
            report("Q3 set %s: pvr_set_bg_poly() refused it\n", vsets[i].name);
            continue;
        }

        snprintf(name, sizeof(name), "Q3 set %s", vsets[i].name);

        if(run_check(name, tgt, v, false, false, &res))
            passed++;
        else if(res.fail)
            timeouts++;
        else
            dump_points(tgt, v);
    }

    add_summary("Q3 %-6s %d/%d sets pass%s", tgt_name(tgt), passed, NUM_VSETS,
                timeouts ? ", RENDER FAILED" : "");
}

/* Q4: the coordinate texture, default and custom texture coordinates. */
static void test_q4(enum target tgt) {
    pvr_bg_vertex_t v[3];
    result_t res;
    bool p1, p2;

    pvr_set_bg_colors(0xffffffff, 0xffffffff, 0xffffffff);
    pvr_set_bg_texture(coord_txr, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                       COORD_SIZE, COORD_SIZE, PVR_FILTER_NEAREST);
    default_verts(v, render_w(tgt), SCREEN_H, 0xffffffff, 0xffffffff,
                  0xffffffff);
    p1 = run_check("Q4 texture, default uv", tgt, v, true, false, &res);

    /* Offset, stretched, partly negative coordinates: tests repeat. */
    make_vert(&v[0], 0.0f, SCREEN_H, 1.0f, 0.5f, 1.25f, 0xffffffff);
    make_vert(&v[1], 0.0f, 0.0f, 1.0f, 0.5f, -0.25f, 0xffffffff);
    make_vert(&v[2], SCREEN_W, SCREEN_H, 1.0f, 2.0f, 1.25f, 0xffffffff);
    pvr_set_bg_poly(v);
    p2 = run_check("Q4 texture, repeat uv", tgt, v, true, false, &res);

    add_summary("Q4 %-6s tex default %s, repeat %s", tgt_name(tgt),
                p1 ? "PASS" : "FAIL", p2 ? "PASS" : "FAIL");

    pvr_set_bg_texture(NULL, 0, 0, 0, PVR_FILTER_NEAREST);
}

/* Q6 (extra): different z per vertex. Which model fits, affine or
   perspective correct? Untextured, then textured. */
static void test_q6(void) {
    pvr_bg_vertex_t v[3];
    result_t res;
    const char *col, *tex;
    bool pa, pp;

    make_vert(&v[0], 0.0f, SCREEN_H, 1.0f, 0.0f, 1.0f, 0xff000000);
    make_vert(&v[1], 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0xff00ff00);
    make_vert(&v[2], SCREEN_W, SCREEN_H, 0.25f, 1.0f, 1.0f, 0xffff00ff);
    pvr_set_bg_poly(v);

    if(render_bg(TGT_SCREEN) < 0) {
        add_summary("Q6 z TIMEOUT");
        return;
    }

    check(TGT_SCREEN, v, false, true, &res);
    col = res.sqerr[1] < res.sqerr[0] ? "PERSPECTIVE" : "AFFINE";
    report("Q6 color with z 1,1,0.25: rms err affine %.1f, perspective %.1f "
           "-> %s\n", sqrt(res.sqerr[0] / (3 * (res.n[0] + res.n[1]) + 1)),
           sqrt(res.sqerr[1] / (3 * (res.n[0] + res.n[1]) + 1)), col);
    dump_points(TGT_SCREEN, v);

    pvr_set_bg_texture(coord_txr, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                       COORD_SIZE, COORD_SIZE, PVR_FILTER_NEAREST);
    v[0].argb = v[1].argb = v[2].argb = 0xffffffff;
    pvr_set_bg_poly(v);
    pa = run_check("Q6 texture z 1,1,0.25 as affine", TGT_SCREEN, v, true,
                   false, &res);
    pp = run_check("Q6 texture z 1,1,0.25 as perspective", TGT_SCREEN, v,
                   true, true, &res);
    tex = pp && !pa ? "PERSPECTIVE" : pa && !pp ? "AFFINE" : "UNCLEAR";
    pvr_set_bg_texture(NULL, 0, 0, 0, PVR_FILTER_NEAREST);

    add_summary("Q6 z color %s, tex %s", col, tex);
}

/* Q7 (extra): the plane's vertices at z (1/w) = 1.0, an opaque quad over
   the whole screen at 0.5. If the plane's depth is the pvr_set_zclip() value
   (0.0001), the quad is in front of it and covers the screen; if the vertex
   z were the plane's depth, the plane would hide the quad. */
static void test_q7(void) {
    static const float zs[2] = { 1.0f, FLT_EPSILON };
    const float x[4] = { 0.0f, render_w(TGT_SCREEN), 0.0f,
                         render_w(TGT_SCREEN) };
    const float y[4] = { 0.0f, 0.0f, SCREEN_H, SCREEN_H };
    pvr_bg_vertex_t v[3];
    const char *what[2];
    int i, k, o[3];

    for(k = 0; k < 2; k++) {
        default_verts(v, render_w(TGT_SCREEN), SCREEN_H, 0xffff0000,
                      0xffff0000, 0xffff0000);

        for(i = 0; i < 3; i++)
            v[i].z = zs[k];

        pvr_set_bg_poly(v);

        for(i = 0; i < 3; i++) {
            pvr_wait_ready();
            pvr_scene_begin();
            pvr_list_begin(PVR_LIST_OP_POLY);
            pvr_prim(&flat_hdr, sizeof(flat_hdr));
            quad(x, y, 0.5f, NULL, NULL, 0xff00ff00);
            pvr_list_finish();
            pvr_scene_finish();
        }

        pvr_wait_ready();
        pvr_wait_render_done();
        vid_waitvbl();
        vid_waitvbl();
        expand565(read_px(TGT_SCREEN, 320, 240), o);

        what[k] = o[1] > 200 && o[0] < 50 ? "quad in front" :
                  o[0] > 200 && o[1] < 50 ? "PLANE IN FRONT" : "OTHER";
        report("Q7 plane z %g, quad z 0.5: got %d %d %d -> %s\n",
               (double)zs[k], o[0], o[1], o[2], what[k]);
    }

    add_summary("Q7 z=1 %s, eps %s", what[0], what[1]);
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
}

/* ---- Render time ------------------------------------------------------- */

enum { FG_NONE, FG_FLAT, FG_TEX };

/* Average render time in ms over frames, for the current background, with
   an optional full-screen quad in front of it. */
static float measure(int fg) {
    pvr_stats_t stats;
    double sum = 0.0;
    int i, n = 0;

    for(i = 0; i < 34; i++) {
        pvr_wait_ready();
        pvr_scene_begin();

        if(fg != FG_NONE) {
            pvr_list_begin(PVR_LIST_OP_POLY);

            if(fg == FG_FLAT)
                pvr_prim(&flat_hdr, sizeof(flat_hdr));
            else
                pvr_prim(&tex_hdr, sizeof(tex_hdr));

            rect(0.0f, 0.0f, SCREEN_W, SCREEN_H, 1.0f, 2.5f, 1.875f);
            pvr_list_finish();
        }

        pvr_scene_finish();

        if(pvr_wait_ready() < 0 || pvr_wait_render_done() < 0)
            return -1.0f;

        pvr_get_stats(&stats);

        if(i >= 4) {
            sum += stats.rnd_last_time;
            n++;
        }
    }

    return sum / n / 1000000.0;
}

static void time_row(const char *name, int fg, float base) {
    float ms = measure(fg);

    report("Q4 time %-6s %-40s %7.3f ms  (%+.3f)\n",
           params.fsaa_enabled ? "fsaa" : "screen", name, (double)ms,
           (double)(ms - base));
}

static void test_q4_time(void) {
    pvr_bg_vertex_t v[3];
    char name[48];
    float base;
    int i, f;

    pvr_set_bg_color(0.2f, 0.3f, 0.4f);
    base = measure(FG_NONE);
    report("Q4 time %-6s %-40s %7.3f ms\n",
           params.fsaa_enabled ? "fsaa" : "screen", "flat color", (double)base);

    pvr_set_bg_colors(Q1_BL, Q1_TL, Q1_BR);
    time_row("gradient", FG_NONE, base);

    for(i = 0; i < 4; i++) {
        for(f = 0; f < 2; f++) {
            pvr_set_bg_colors(0xffffffff, 0xffffffff, 0xffffffff);
            pvr_set_bg_texture(cost_txr[i], PVR_TXRFMT_RGB565 |
                               PVR_TXRFMT_TWIDDLED, cost_size[i],
                               cost_size[i], f ? PVR_FILTER_BILINEAR
                                               : PVR_FILTER_NEAREST);
            snprintf(name, sizeof(name), "texture %dx%d stretched, %s",
                     cost_size[i], cost_size[i], f ? "bilinear" : "nearest");
            time_row(name, FG_NONE, base);
        }
    }

    /* The large texture at one texel per pixel */
    for(f = 0; f < 2; f++) {
        float w = render_w(TGT_SCREEN);

        make_vert(&v[0], 0.0f, SCREEN_H, 1.0f, 0.0f, SCREEN_H / 1024.0f,
                  0xffffffff);
        make_vert(&v[1], 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0xffffffff);
        make_vert(&v[2], w, SCREEN_H, 1.0f, w / 1024.0f, SCREEN_H / 1024.0f,
                  0xffffffff);
        pvr_set_bg_poly(v);
        pvr_set_bg_texture(cost_txr[3], PVR_TXRFMT_RGB565 |
                           PVR_TXRFMT_TWIDDLED, 1024, 1024,
                           f ? PVR_FILTER_BILINEAR : PVR_FILTER_NEAREST);
        snprintf(name, sizeof(name), "texture 1024x1024 1:1, %s",
                 f ? "bilinear" : "nearest");
        time_row(name, FG_NONE, base);
    }

    /* For comparison: the same pixels as an opaque quad from the TA, with
       a flat background behind it. */
    pvr_set_bg_color(0.2f, 0.3f, 0.4f);
    time_row("TA quad, flat, over flat bg", FG_FLAT, base);
    time_row("TA quad, 256 bilinear, over flat bg", FG_TEX, base);
}

/* ---- Part 2: pages to look at ------------------------------------------ */

enum { PG_SUMMARY, PG_Q1, PG_Q2, PG_Q3, PG_Q4, PG_Q5, PG_COUNT };

static const uint32_t q5_grad[][2] = {
    { 0xff284c94, 0xffb4cdf0 },     /* daytime sky: top, bottom */
    { 0xff000010, 0xff102040 },     /* night: few steps, wide bands */
    { 0xff303030, 0xff707070 },     /* grey ramp */
};

static void set_page_bg(int page, int sub, float t, char *help, size_t hsz) {
    pvr_bg_vertex_t v[3];
    float w = render_w(TGT_SCREEN), a, s, c, u0, v0;
    int k;

    switch(page) {
        case PG_SUMMARY:
            pvr_set_bg_color(0.1f, 0.1f, 0.15f);
            help[0] = '\0';

            for(k = 0; k < summary_lines && k < TEXT_LINES - 2; k++) {
                strncat(help, summary[k], hsz - strlen(help) - 2);
                strcat(help, "\n");
            }

            break;

        case PG_Q1:
            pvr_set_bg_colors(Q1_BL, Q1_TL, Q1_BR);
            snprintf(help, hsz, "Q1 three corner colors\n"
                     "Smooth over the WHOLE screen, also\n"
                     "above the TL-BR diagonal (outside\n"
                     "the triangle). No seam on the diag.\n"
                     "TL green, BL dark blue, BR magenta,\n"
                     "TR yellow-white (255,255,128).");
            break;

        case PG_Q2:
            pvr_set_bg_colors(Q2_BL, Q2_TL, Q2_BR);
            snprintf(help, hsz, "Q2 red > 255, green < 0 at top right\n"
                     "Clamp: top-right goes flat magenta,\n"
                     "smoothly. Wrap: sharp edges, bands\n"
                     "of color repeating towards the corner.");
            break;

        case PG_Q3:
            sub %= NUM_VSETS;
            make_vset(sub, TGT_SCREEN, v);
            pvr_set_bg_poly(v);
            snprintf(help, hsz, "Q3 vertex set %s\n"
                     "A: next set. a-g must look the same:\n"
                     "red grows right, green grows down.\n"
                     "h is flat blue; i nearly flat.", vsets[sub].name);
            break;

        case PG_Q4:
            /* Scrolling and rotating texture coordinates */
            a = t * 0.3f;
            s = sinf(a);
            c = cosf(a);
            u0 = t * 0.05f;
            v0 = t * 0.02f;
            make_vert(&v[0], 0.0f, SCREEN_H, 1.0f, u0 - 1.5f * s,
                      v0 + 1.5f * c, 0xffffffff);
            make_vert(&v[1], 0.0f, 0.0f, 1.0f, u0, v0, 0xffffffff);
            make_vert(&v[2], w, SCREEN_H, 1.0f, u0 + 2.0f * c - 1.5f * s,
                      v0 + 2.0f * s + 1.5f * c, 0xffffffff);
            pvr_set_bg_poly(v);
            pvr_set_bg_texture(cost_txr[2], PVR_TXRFMT_RGB565 |
                               PVR_TXRFMT_TWIDDLED, 256, 256,
                               (sub & 1) ? PVR_FILTER_BILINEAR
                                         : PVR_FILTER_NEAREST);
            snprintf(help, hsz, "Q4 textured background, %s\n"
                     "A: filter. The checker must cover the\n"
                     "WHOLE screen, also the top-right half,\n"
                     "scrolling and turning, no seam.",
                     (sub & 1) ? "bilinear" : "nearest");
            break;

        case PG_Q5:
            sub %= 3;
            pvr_set_bg_colors(q5_grad[sub][1], q5_grad[sub][0],
                              q5_grad[sub][1]);
            snprintf(help, hsz, "Q5 banding in RGB565, gradient %d/3\n"
                     "A: next gradient, B: dithering.\n"
                     "How visible are the bands?", sub + 1);
            break;
    }
}

static void pages(void) {
    maple_device_t *cont;
    cont_state_t *state;
    uint32_t buttons, prev = 0, pressed;
    pvr_stats_t stats;
    char help[TEXT_LINES * 48], str[TEXT_LINES * 48 + 96];
    int page = PG_SUMMARY, sub = 0;
    bool dither = true;
    float t = 0.0f, ms = 0.0f;

    set_dither(dither);

    for(;;) {
        cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        state = cont ? (cont_state_t *)maple_dev_status(cont) : NULL;
        buttons = state ? state->buttons : 0;
        pressed = buttons & ~prev;
        prev = buttons;

        if(buttons & CONT_START)
            break;

        if(pressed & CONT_DPAD_RIGHT) {
            page = (page + 1) % PG_COUNT;
            sub = 0;
        }

        if(pressed & CONT_DPAD_LEFT) {
            page = (page + PG_COUNT - 1) % PG_COUNT;
            sub = 0;
        }

        if(pressed & CONT_A)
            sub++;

        if(pressed & CONT_B) {
            dither = !dither;
            set_dither(dither);
        }

        if(pressed & CONT_Y)
            reinit_pvr(!params.fsaa_enabled);

        pvr_set_bg_texture(NULL, 0, 0, 0, PVR_FILTER_NEAREST);
        set_page_bg(page, sub, t, help, sizeof(help));

        snprintf(str, sizeof(str), "page %d/%d  render %.3f ms%s%s\n%s",
                 page + 1, PG_COUNT, (double)ms,
                 params.fsaa_enabled ? "  FSAA" : "",
                 dither ? "  dither" : "  no dither", help);

        pvr_wait_ready();
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_TR_POLY);
        draw_text(str);
        pvr_list_finish();
        pvr_scene_finish();
        pvr_wait_ready();
        pvr_wait_render_done();
        pvr_get_stats(&stats);
        ms += (stats.rnd_last_time / 1000000.0f - ms) * 0.1f;
        t += 1.0f / 60.0f;
    }
}

/* ---- Main -------------------------------------------------------------- */

static void run_all(enum target tgt) {
    test_q0(tgt);
    test_q1(tgt);
    test_q2(tgt);

    if(!params.fsaa_enabled)
        test_q4(tgt);

    test_q3(tgt);
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    vid_set_mode(DM_640x480, PM_RGB565);
    fb_cfg2 = PVR_GET(PVR_FB_CFG_2);
    init_pvr(false);

    printf("---bgplane_hwtest---\n");
    printf("video: %dx%d, cable %d, FB_CFG_2 %08lx\n", vid_mode->width,
           vid_mode->height, vid_mode->cable_type, (unsigned long)fb_cfg2);

    /* Part 1: read back, dithering off */
    set_dither(false);

    run_all(TGT_SCREEN);
    run_all(TGT_RTT);
    test_q6();
    test_q7();
    test_q4_time();
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);

    reinit_pvr(true);
    set_dither(false);
    run_all(TGT_SCREEN);
    test_q4_time();
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);

    reinit_pvr(false);
    printf("---bgplane_hwtest part 1 done---\n");

    /* Part 2: look at the screen */
    pages();

    set_dither(true);
    pvr_shutdown();

    return 0;
}
