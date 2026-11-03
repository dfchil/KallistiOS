/* KallistiOS ##version##

   examples/dreamcast/pvr/bgplane_texture/bgplane_texture.c
   Copyright (C) 2026 Daniel Fairchild

   A full-screen textured backdrop with no geometry: the texture is on the
   background plane (pvr_set_bg_texture()), and its texture coordinates scroll
   and rotate every frame (pvr_set_bg_poly()). A cube turns in front of it.

   The texture coordinates are given at the three vertices of the background
   plane and extend over the whole screen; outside 0..1 the texture repeats.

   Controls:
     A                  textured / flat background (compare the render time)
     B                  nearest / bilinear filtering
     X                  small (64x64) / large (1024x1024) texture
     Y                  white / tinted vertex colors (they modulate the
                        texture)
     D-pad up/down      rotation speed
     D-pad left/right   scroll speed
     Start              exit
*/

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>

#include <kos.h>
#include <dc/pvr.h>
#include <dc/fmath.h>
#include <dc/biosfont.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

#define SCREEN_W    640
#define SCREEN_H    480

#define TEXT_W      512
#define TEXT_H      64

/* Screen pixels per repeat of the texture */
#define REPEAT_PX   320.0f

static pvr_init_params_t params = {
    { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0,
      PVR_BINSIZE_0 },
    256 * 1024, 0, 0, 0, 0, 0
};

static const int txr_size[2] = { 64, 1024 };

static pvr_ptr_t txr[2], text_txr[2];
static pvr_poly_hdr_t text_hdr[2], cube_hdr;
static uint16_t text_buf[TEXT_W * TEXT_H];
static uint16_t pix[1024 * 1024];
static int text_cur;

static bool textured = true, bilinear = true, large = false, tint = false;
static float angle, rot_speed = 0.2f, scroll_x, scroll_y, scroll_speed = 0.1f;
static float render_ms;

/* A pattern that tiles seamlessly: sums of sines with whole periods over the
   texture, through a sine table. */
static void make_texture(int n, pvr_ptr_t dst) {
    static int16_t sine[1024];
    int x, y, a, b, c, r, g, bl;

    for(x = 0; x < 1024; x++)
        sine[x] = (int16_t)(sinf(x * 2.0f * F_PI / 1024.0f) * 256.0f);

    for(y = 0; y < n; y++) {
        for(x = 0; x < n; x++) {
            int u = x * 1024 / n, v = y * 1024 / n;

            a = sine[(3 * u + v) & 1023];
            b = sine[(2 * v - u + 256) & 1023];
            c = sine[(5 * u + 4 * v + (a >> 2)) & 1023];

            r = 64 + ((a + c) >> 3);
            g = 40 + ((b + c) >> 4);
            bl = 128 + ((a + b) >> 3);

            /* Fine grid lines, to see the filtering */
            if(((x * 16 / n) != ((x + 1) * 16 / n)) ||
               ((y * 16 / n) != ((y + 1) * 16 / n)))
                r = g = bl = 255;

            pix[y * n + x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3);
        }
    }

    pvr_txr_load_ex(pix, dst, n, n, PVR_TXRLOAD_16BPP);
}

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

        txr[i] = pvr_mem_malloc(txr_size[i] * txr_size[i] * 2);
        make_texture(txr_size[i], txr[i]);
    }

    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&cube_hdr, &cxt);
}

/* Texture coordinates for screen point (x, y): the screen center is at
   (scroll_x, scroll_y) in the texture, turned by angle. */
static void screen_to_uv(float x, float y, float *u, float *v) {
    float c = cosf(angle), s = sinf(angle);
    float dx = (x - SCREEN_W / 2) / REPEAT_PX, dy = (y - SCREEN_H / 2) / REPEAT_PX;

    *u = scroll_x + c * dx - s * dy;
    *v = scroll_y + s * dx + c * dy;
}

static void set_background(void) {
    static const float px[3] = { 0.0f, 0.0f, SCREEN_W };
    static const float py[3] = { SCREEN_H, 0.0f, SCREEN_H };
    float xs = params.fsaa_enabled ? 2.0f : 1.0f;
    pvr_bg_vertex_t v[3];
    int i;

    if(!textured) {
        pvr_set_bg_color(0.25f, 0.2f, 0.45f);
        return;
    }

    for(i = 0; i < 3; i++) {
        v[i].x = px[i] * xs;
        v[i].y = py[i];
        v[i].z = FLT_EPSILON;  /* as the default vertices */
        screen_to_uv(px[i], py[i], &v[i].u, &v[i].v);
        v[i].argb = 0xffffffff;
    }

    /* Tinted: warm at the bottom, white at the top */
    if(tint)
        v[0].argb = v[2].argb = 0xffffa060;

    pvr_set_bg_poly(v);
    pvr_set_bg_texture(txr[large], PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED,
                       txr_size[large], txr_size[large],
                       bilinear ? PVR_FILTER_BILINEAR : PVR_FILTER_NEAREST);
}

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

static void draw_cube(float t, float xs) {
    static const float corners[8][3] = {
        { -1, -1, -1 }, { 1, -1, -1 }, { -1, 1, -1 }, { 1, 1, -1 },
        { -1, -1,  1 }, { 1, -1,  1 }, { -1, 1,  1 }, { 1, 1,  1 }
    };
    static const int faces[6][4] = {
        { 2, 3, 0, 1 }, { 7, 6, 5, 4 }, { 6, 2, 4, 0 },
        { 3, 7, 1, 5 }, { 6, 7, 2, 3 }, { 0, 1, 4, 5 }
    };
    static const uint32_t colors[6] = {
        0xffe04040, 0xff40e040, 0xff4040e0, 0xffe0e040, 0xff40e0e0, 0xffe040e0
    };
    float sx[8], sy[8], sz[8], fx[4], fy[4], fz[4];
    float ca = cosf(t), sa = sinf(t), cb = cosf(t * 0.7f), sb = sinf(t * 0.7f);
    float x, y, z, x2, z2, y3, z3;
    int i, j;

    for(i = 0; i < 8; i++) {
        x = corners[i][0];
        y = corners[i][1];
        z = corners[i][2];
        x2 = x * ca + z * sa;
        z2 = z * ca - x * sa;
        y3 = y * cb - z2 * sb;
        z3 = y * sb + z2 * cb + 6.0f;
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

static void draw_text(const char *str, float xs) {
    const float z[4] = { 10.0f, 10.0f, 10.0f, 10.0f };
    const float x[4] = { 16.0f * xs, (16.0f + TEXT_W) * xs, 16.0f * xs,
                         (16.0f + TEXT_W) * xs };
    const float y[4] = { 16.0f, 16.0f, 16.0f + TEXT_H, 16.0f + TEXT_H };
    const float u[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    const float v[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
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
    quad(x, y, z, u, v, 0xffffffff);
}

int main(int argc, char *argv[]) {
    maple_device_t *cont;
    cont_state_t *state;
    uint32_t buttons, prev = 0, pressed;
    pvr_stats_t stats;
    float t = 0.0f, xs;
    char str[128];

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

        if(pressed & CONT_A)
            textured = !textured;

        if(pressed & CONT_B)
            bilinear = !bilinear;

        if(pressed & CONT_X)
            large = !large;

        if(pressed & CONT_Y)
            tint = !tint;

        if(pressed & CONT_DPAD_UP)
            rot_speed += 0.1f;

        if(pressed & CONT_DPAD_DOWN)
            rot_speed -= 0.1f;

        if(pressed & CONT_DPAD_RIGHT)
            scroll_speed += 0.05f;

        if(pressed & CONT_DPAD_LEFT)
            scroll_speed -= 0.05f;

        /* Scroll along a slow curve, turn steadily. */
        angle += rot_speed / 60.0f;
        scroll_x += scroll_speed * cosf(t * 0.2f) / 60.0f;
        scroll_y += scroll_speed * sinf(t * 0.13f) / 60.0f;

        xs = params.fsaa_enabled ? 2.0f : 1.0f;
        snprintf(str, sizeof(str), "%s  render %.3f ms\n"
                 "A tex  B filter  X size  Y tint  D-pad",
                 textured ? (large ? (bilinear ? "1024 bilinear" :
                                      "1024 nearest") :
                             (bilinear ? "64 bilinear" : "64 nearest"))
                          : "flat color", (double)render_ms);

        pvr_wait_ready();
        set_background();
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);
        draw_cube(t, xs);
        pvr_list_finish();
        pvr_list_begin(PVR_LIST_TR_POLY);
        draw_text(str, xs);
        pvr_list_finish();
        pvr_scene_finish();

        /* Wait for this frame's render, so the time is this frame's. */
        pvr_wait_ready();
        pvr_wait_render_done();
        pvr_get_stats(&stats);
        render_ms += (stats.rnd_last_time / 1000000.0f - render_ms) * 0.05f;

        t += 1.0f / 60.0f;
    }

    pvr_set_bg_texture(NULL, 0, 0, 0, PVR_FILTER_NEAREST);
    pvr_shutdown();

    return 0;
}
