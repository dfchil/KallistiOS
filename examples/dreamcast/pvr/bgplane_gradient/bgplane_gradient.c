/* KallistiOS ##version##

   examples/dreamcast/pvr/bgplane_gradient/bgplane_gradient.c
   Copyright (C) 2026 Daniel Fairchild

   A vertical sky gradient on the background plane, behind a rotating cube.

   The PVR's background plane is a polygon with three vertices, shaded with
   Gouraud shading whether the vertices have one color or three, so a
   gradient costs nothing: compare the render time shown on screen in both
   modes.

   Controls:
     D-pad up/down      change the top color
     D-pad left/right   change the bottom color
     A                  switch between gradient and flat color
     X                  render through a texture (pvr_scene_begin_rtt()),
                        with FSAA off
     Y                  switch FSAA on or off
     Start              exit
*/

#include <stdio.h>
#include <string.h>
#include <math.h>

#include <kos.h>
#include <dc/pvr.h>
#include <dc/biosfont.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

#define SCREEN_W    640
#define SCREEN_H    480

/* Render target for render-to-texture mode */
#define RTT_W       1024
#define RTT_H       512

/* On-screen text: drawn with the BIOS font into an ARGB4444 texture. */
#define TEXT_W      512
#define TEXT_H      64

static const uint32_t top_colors[] = {
    0xff102060, 0xff2050a0, 0xff000000, 0xff401060, 0xff603000, 0xff004040
};

static const uint32_t bottom_colors[] = {
    0xffa0c0f0, 0xfff0d0a0, 0xff406080, 0xfff08040, 0xffffffff, 0xff20c080
};

#define NUM_TOP     (sizeof(top_colors) / sizeof(top_colors[0]))
#define NUM_BOTTOM  (sizeof(bottom_colors) / sizeof(bottom_colors[0]))

static pvr_init_params_t params = {
    { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0,
      PVR_BINSIZE_0 },
    256 * 1024, 0, 0, 0, 0, 0
};

static int top = 0, bottom = 0;
static bool gradient = true, rtt = false;

static pvr_ptr_t text_txr[2], rtt_txr;
static pvr_poly_hdr_t text_hdr[2], cube_hdr, rtt_hdr;
static uint16_t text_buf[TEXT_W * TEXT_H];
static int text_cur;

/* Average render time over the last frames, in milliseconds */
static float render_ms;

static uint32_t mix_color(uint32_t a, uint32_t b) {
    return 0xff000000
           | ((((a >> 16 & 0xff) + (b >> 16 & 0xff)) / 2) << 16)
           | ((((a >> 8 & 0xff) + (b >> 8 & 0xff)) / 2) << 8)
           | (((a & 0xff) + (b & 0xff)) / 2);
}

/* Set the background for the current mode. */
static void set_background(void) {
    uint32_t t = top_colors[top], b = bottom_colors[bottom];

    if(gradient) {
        /* A vertical gradient: the bottom color on both bottom vertices. */
        pvr_set_bg_colors(b, t, b);
    }
    else {
        t = mix_color(t, b);
        pvr_set_bg_color((t >> 16 & 0xff) / 255.0f, (t >> 8 & 0xff) / 255.0f,
                         (t & 0xff) / 255.0f);
    }
}

/* Set up the PVR and everything that lives in video memory. */
static void init_pvr(void) {
    pvr_poly_cxt_t cxt;
    int i;

    pvr_init(&params);

    for(i = 0; i < 2; i++) {
        text_txr[i] = pvr_mem_malloc(TEXT_W * TEXT_H * 2);
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY,
                         PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED,
                         TEXT_W, TEXT_H, text_txr[i], PVR_FILTER_NONE);
        pvr_poly_compile(&text_hdr[i], &cxt);
    }

    rtt_txr = pvr_mem_malloc(RTT_W * RTT_H * 2);
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
                     PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                     RTT_W, RTT_H, rtt_txr, PVR_FILTER_NONE);
    pvr_poly_compile(&rtt_hdr, &cxt);

    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&cube_hdr, &cxt);

    /* pvr_init() resets the background plane, set it again. */
    set_background();
}

/* Submit one quad as a strip: top-left, top-right, bottom-left,
   bottom-right. */
static void quad(const float x[4], const float y[4], const float z[4],
                 const float u[4], const float v[4], uint32_t argb) {
    pvr_vertex_t vert;
    int i;

    for(i = 0; i < 4; i++) {
        vert.flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        vert.x = x[i];
        vert.y = y[i];
        vert.z = z[i];
        vert.u = u ? u[i] : 0.0f;
        vert.v = v ? v[i] : 0.0f;
        vert.argb = argb;
        vert.oargb = 0;
        pvr_prim(&vert, sizeof(vert));
    }
}

/* Screen-aligned rectangle. xs scales x for FSAA. */
static void rect(float x0, float y0, float x1, float y1, float z, float u1,
                 float v1, float xs) {
    const float x[4] = { x0 * xs, x1 * xs, x0 * xs, x1 * xs };
    const float y[4] = { y0, y0, y1, y1 };
    const float zz[4] = { z, z, z, z };
    const float u[4] = { 0.0f, u1, 0.0f, u1 };
    const float v[4] = { 0.0f, 0.0f, v1, v1 };

    quad(x, y, zz, u, v, 0xffffffff);
}

/* A cube rotating in front of the background. Opaque list must be open. */
static void draw_cube(float t, float xs) {
    static const float corners[8][3] = {
        { -1, -1, -1 }, { 1, -1, -1 }, { -1, 1, -1 }, { 1, 1, -1 },
        { -1, -1,  1 }, { 1, -1,  1 }, { -1, 1,  1 }, { 1, 1,  1 }
    };
    /* Each face as a strip of four corners */
    static const int faces[6][4] = {
        { 2, 3, 0, 1 }, { 7, 6, 5, 4 }, { 6, 2, 4, 0 },
        { 3, 7, 1, 5 }, { 6, 7, 2, 3 }, { 0, 1, 4, 5 }
    };
    static const uint32_t colors[6] = {
        0xffe04040, 0xff40e040, 0xff4040e0, 0xffe0e040, 0xff40e0e0, 0xffe040e0
    };
    float sx[8], sy[8], sz[8];
    float ca = cosf(t), sa = sinf(t), cb = cosf(t * 0.7f), sb = sinf(t * 0.7f);
    float x, y, z, x2, z2, y3, z3;
    float fx[4], fy[4], fz[4];
    int i, j;

    for(i = 0; i < 8; i++) {
        x = corners[i][0];
        y = corners[i][1];
        z = corners[i][2];

        /* Rotate around y, then x, then move away from the camera. */
        x2 = x * ca + z * sa;
        z2 = z * ca - x * sa;
        y3 = y * cb - z2 * sb;
        z3 = y * sb + z2 * cb + 5.0f;

        sx[i] = (SCREEN_W / 2 + 400.0f * x2 / z3) * xs;
        sy[i] = SCREEN_H / 2 - 400.0f * y3 / z3;
        sz[i] = 1.0f / z3;
    }

    pvr_prim(&cube_hdr, sizeof(cube_hdr));

    for(i = 0; i < 6; i++) {
        for(j = 0; j < 4; j++) {
            fx[j] = sx[faces[i][j]];
            fy[j] = sy[faces[i][j]];
            fz[j] = sz[faces[i][j]];
        }

        quad(fx, fy, fz, NULL, NULL, colors[i]);
    }
}

/* Draw the text lines in str (separated by '\n') at the top left of the
   screen. Translucent list must be open. */
static void draw_text(const char *str, float xs) {
    char line[TEXT_W / BFONT_THIN_WIDTH + 1];
    const char *end;
    size_t len;
    int row = 0;

    memset(text_buf, 0, sizeof(text_buf));

    while(*str && row < TEXT_H / BFONT_HEIGHT) {
        end = strchr(str, '\n');
        len = end ? (size_t)(end - str) : strlen(str);

        if(len >= sizeof(line))
            len = sizeof(line) - 1;

        memcpy(line, str, len);
        line[len] = '\0';
        bfont_draw_str_ex(text_buf + row * BFONT_HEIGHT * TEXT_W, TEXT_W,
                          0xffff, 0x8000, 16, true, line);

        str += end ? (size_t)(end - str) + 1 : len;
        row++;
    }

    /* The PVR may still be reading the texture of the previous frame, so
       alternate between two. */
    text_cur ^= 1;
    pvr_txr_load(text_buf, text_txr[text_cur], sizeof(text_buf));
    pvr_prim(&text_hdr[text_cur], sizeof(pvr_poly_hdr_t));
    rect(16.0f, 16.0f, 16.0f + TEXT_W, 16.0f + TEXT_H, 10.0f, 1.0f, 1.0f, xs);
}

/* Wait until the scene just finished has been rendered, and return its
   render time in milliseconds. */
static float finish_and_time(void) {
    pvr_stats_t stats;

    pvr_scene_finish();
    pvr_wait_ready();
    pvr_wait_render_done();
    pvr_get_stats(&stats);

    return stats.rnd_last_time / 1000000.0f;
}

static void draw_frame(float t) {
    float xs = params.fsaa_enabled ? 2.0f : 1.0f;
    float ms;
    char str[128];

    snprintf(str, sizeof(str), "%s%s%s  render %.3f ms\n"
             "D-pad colors  A flat  X rtt  Y fsaa",
             gradient ? "gradient" : "flat color", rtt ? "  rtt" : "",
             params.fsaa_enabled ? "  fsaa" : "", (double)render_ms);

    if(rtt) {
        /* The scene goes to a texture first, with the background plane
           sized to the render-to-texture area... */
        pvr_wait_ready();
        pvr_scene_begin_rtt(rtt_txr, SCREEN_W, SCREEN_H, RTT_W);
        pvr_list_begin(PVR_LIST_OP_POLY);
        draw_cube(t, 1.0f);
        pvr_list_finish();
        ms = finish_and_time();

        /* ...then the texture goes to the screen. */
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);
        pvr_prim(&rtt_hdr, sizeof(rtt_hdr));
        rect(0.0f, 0.0f, SCREEN_W, SCREEN_H, 1.0f, (float)SCREEN_W / RTT_W,
             (float)SCREEN_H / RTT_H, xs);
        pvr_list_finish();
        pvr_list_begin(PVR_LIST_TR_POLY);
        draw_text(str, xs);
        pvr_list_finish();
        finish_and_time();
    }
    else {
        pvr_wait_ready();
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);
        draw_cube(t, xs);
        pvr_list_finish();
        pvr_list_begin(PVR_LIST_TR_POLY);
        draw_text(str, xs);
        pvr_list_finish();
        ms = finish_and_time();
    }

    render_ms += (ms - render_ms) * 0.05f;
}

int main(int argc, char *argv[]) {
    maple_device_t *cont;
    cont_state_t *state;
    uint32_t buttons, prev = 0, pressed;
    float t = 0.0f;

    (void)argc;
    (void)argv;

    vid_set_mode(DM_640x480, PM_RGB565);
    init_pvr();

    for(;;) {
        cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        state = cont ? (cont_state_t *)maple_dev_status(cont) : NULL;
        buttons = state ? state->buttons : 0;
        pressed = buttons & ~prev;
        prev = buttons;

        if(buttons & CONT_START)
            break;

        if(pressed & CONT_DPAD_UP)
            top = (top + 1) % NUM_TOP;

        if(pressed & CONT_DPAD_DOWN)
            top = (top + NUM_TOP - 1) % NUM_TOP;

        if(pressed & CONT_DPAD_RIGHT)
            bottom = (bottom + 1) % NUM_BOTTOM;

        if(pressed & CONT_DPAD_LEFT)
            bottom = (bottom + NUM_BOTTOM - 1) % NUM_BOTTOM;

        if(pressed & CONT_A)
            gradient = !gradient;

        if((pressed & CONT_X) && !params.fsaa_enabled)
            rtt = !rtt;

        if(pressed & CONT_Y) {
            pvr_wait_ready();
            pvr_wait_render_done();
            pvr_shutdown();
            params.fsaa_enabled = !params.fsaa_enabled;
            rtt = false;
            init_pvr();
        }

        /* Background settings apply from the next scene on. */
        if(pressed & (CONT_DPAD_UP | CONT_DPAD_DOWN | CONT_DPAD_LEFT |
                      CONT_DPAD_RIGHT | CONT_A))
            set_background();

        draw_frame(t);
        t += 1.0f / 60.0f;
    }

    pvr_shutdown();

    return 0;
}
