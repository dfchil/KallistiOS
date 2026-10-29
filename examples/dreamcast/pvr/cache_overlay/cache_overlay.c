/* KallistiOS ##version##

   examples/dreamcast/pvr/cache_overlay/cache_overlay.c
   Copyright (C) 2026 Daniel Fairchild

   A moving 3D scene under a static overlay of a few thousand polygons (an
   ornate frame and a dashboard). The overlay is either sent through the TA
   every frame, or built once into a cache of object lists and drawn from
   the punch-through slot of every frame (dc/pvr/pvr_cache.h). The screen
   shows what the overlay costs in each mode: CPU time for submitting the
   frame, TA registration time and render time.

   The program doesn't use punch-through polygons, so the PT list is
   disabled for the TA and its slot is free for the cache. When sent every
   frame, the overlay goes in the opaque list instead: for opaque polygons
   the result is the same.

   Controls:
     A      switch between sending the overlay and drawing it from the cache
     X      FSAA on/off
     Y      vertex buffer double-buffering on/off
     Start  quit

   Without input, the program cycles through both modes in all four
   FSAA/double-buffering configurations, prints the timings of each on the
   serial console and quits.
*/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <kos/timer.h>
#include <arch/arch.h>
#include <dc/pvr.h>
#include <dc/biosfont.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

#define OVERLAY_Z   100.0f
#define TEXT_Z      200.0f

#define PI_F        3.14159265f

#define TEXT_W      512
#define TEXT_H      64

static int fsaa, single_vbuf, cached, cache_ok;
static pvr_cache_t *cache;
static pvr_ptr_t text_txr;
static uint16_t text_buf[TEXT_W * TEXT_H];
static int overlay_tris;

static float sx(float x) {
    return fsaa ? x * 2.0f : x;
}

/* Overlay *************************************************************/

static pvr_vertex_t vert;

static void vtx(float x, float y, uint32_t argb, int last) {
    vert.flags = last ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    vert.x = sx(x);
    vert.y = y;
    vert.z = OVERLAY_Z;
    vert.u = vert.v = 0.0f;
    vert.argb = argb;
    vert.oargb = 0;
    pvr_prim(&vert, sizeof(vert));
}

static void quad(float x0, float y0, float x1, float y1, uint32_t argb) {
    vtx(x0, y1, argb, 0);
    vtx(x0, y0, argb, 0);
    vtx(x1, y1, argb, 0);
    vtx(x1, y0, argb, 1);
    overlay_tris += 2;
}

/* A ring of n segments as one strip */
static void ring(float cx, float cy, float r0, float r1, int n, float a0,
                 float a1, uint32_t argb) {
    int i;
    float a;

    for(i = 0; i <= n; i++) {
        a = a0 + (a1 - a0) * i / n;
        vtx(cx + cosf(a) * r1, cy + sinf(a) * r1, argb, 0);
        vtx(cx + cosf(a) * r0, cy + sinf(a) * r0, argb, i == n);
    }

    overlay_tris += 2 * n;
}

static void gauge(float cx, float cy, float r) {
    int i;
    float a;

    ring(cx, cy, r - 6, r, 120, 0.0f, 2.0f * PI_F, 0xffc0c0c0);
    ring(cx, cy, r - 9, r - 6, 120, 0.0f, 2.0f * PI_F, 0xff404040);

    /* Coloured arc and tick marks */
    ring(cx, cy, r - 16, r - 10, 60, 0.75f * PI_F, 1.75f * PI_F, 0xff20a020);
    ring(cx, cy, r - 16, r - 10, 30, 1.75f * PI_F, 2.25f * PI_F, 0xffc02020);

    for(i = 0; i <= 40; i++) {
        a = 0.75f * PI_F + 1.5f * PI_F * i / 40;
        float len = (i % 5) ? 6.0f : 12.0f;
        float c = cosf(a), s = sinf(a), px = -s * 1.2f, py = c * 1.2f;
        float x0 = cx + c * (r - 18 - len), y0 = cy + s * (r - 18 - len);
        float x1 = cx + c * (r - 18), y1 = cy + s * (r - 18);

        vtx(x0 - px, y0 - py, 0xffffffff, 0);
        vtx(x0 + px, y0 + py, 0xffffffff, 0);
        vtx(x1 - px, y1 - py, 0xffffffff, 0);
        vtx(x1 + px, y1 + py, 0xffffffff, 1);
        overlay_tris += 2;
    }

    /* Hub */
    ring(cx, cy, 0.0f, 8.0f, 16, 0.0f, 2.0f * PI_F, 0xff808080);
}

/* A border band of chevrons along one edge */
static void chevrons(float x0, float y0, float dx, float dy, float nx, float ny,
                     int n, float w) {
    int i;

    for(i = 0; i < n; i++) {
        float x = x0 + dx * i, y = y0 + dy * i;
        uint32_t c = (i & 1) ? 0xffb08030 : 0xff705020;

        vtx(x, y, c, 0);
        vtx(x + nx * w, y + ny * w, c, 0);
        vtx(x + dx, y + dy, c, 0);
        vtx(x + dx + nx * w, y + dy + ny * w, c, 1);
        vtx(x + dx * 0.5f + nx * w * 0.5f, y + dy * 0.5f + ny * w * 0.5f,
            0xffffe080, 0);
        vtx(x + dx * 0.5f + nx * w * 0.3f + dy * 0.2f,
            y + dy * 0.5f + ny * w * 0.3f + dx * 0.2f, 0xffffe080, 0);
        vtx(x + dx * 0.5f + nx * w * 0.3f - dy * 0.2f,
            y + dy * 0.5f + ny * w * 0.3f - dx * 0.2f, 0xffffe080, 1);
        overlay_tris += 3;
    }
}

static void rivet(float cx, float cy) {
    ring(cx, cy, 0.0f, 4.0f, 8, 0.0f, 2.0f * PI_F, 0xffe0e0e0);
}

/* The overlay, in the given list (the header carries the list type) */
static void send_overlay(pvr_list_t list) {
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    int i;

    overlay_tris = 0;

    /* Same header in both modes: opaque blending, only the list differs */
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr, &cxt);
    hdr.cmd = (hdr.cmd & ~PVR_TA_CMD_TYPE) | FIELD_PREP(PVR_TA_CMD_TYPE, list);
    pvr_prim(&hdr, sizeof(hdr));

    /* Frame */
    chevrons(0, 0, 8, 0, 0, 1, 80, 16);
    chevrons(0, 480, 8, 0, 0, -1, 80, 16);
    chevrons(0, 16, 0, 8, 1, 0, 48, 16);
    chevrons(640, 16, 0, 8, -1, 0, 48, 16);

    for(i = 0; i < 40; i++) {
        rivet(24 + i * 15, 24);
        rivet(24 + i * 15, 376);
    }

    for(i = 0; i < 22; i++) {
        rivet(24, 40 + i * 15);
        rivet(616, 40 + i * 15);
    }

    /* Dashboard */
    quad(16, 384, 624, 464, 0xff202830);
    quad(16, 382, 624, 384, 0xffc0c0c0);
    gauge(110, 424, 38);
    gauge(320, 424, 38);
    gauge(530, 424, 38);

    for(i = 0; i < 12; i++) {
        quad(180 + i * 8, 400, 185 + i * 8, 448, i < 8 ? 0xff20c020 : 0xffc02020);
        quad(390 + i * 8, 400, 395 + i * 8, 448, i < 6 ? 0xff2080e0 : 0xff404040);
    }
}

static void build_overlay(void *data) {
    (void)data;

    pvr_list_begin(PVR_LIST_PT_POLY);
    send_overlay(PVR_LIST_PT_POLY);
    pvr_list_finish();
}

/* Moving scene ********************************************************/

static void cube(float cx, float cy, float cz, float ay, float ax, float s) {
    static const float corner[8][3] = {
        { -1, -1, -1 }, { 1, -1, -1 }, { -1, 1, -1 }, { 1, 1, -1 },
        { -1, -1, 1 }, { 1, -1, 1 }, { -1, 1, 1 }, { 1, 1, 1 }
    };
    static const int face[6][4] = {
        { 0, 1, 2, 3 }, { 5, 4, 7, 6 }, { 4, 0, 6, 2 },
        { 1, 5, 3, 7 }, { 4, 5, 0, 1 }, { 2, 3, 6, 7 }
    };
    static const uint32_t col[6] = {
        0xffe04040, 0xff40e040, 0xff4040e0, 0xffe0e040, 0xff40e0e0, 0xffe040e0
    };
    float p[8][3], cy_ = cosf(ay), sy_ = sinf(ay), cx_ = cosf(ax), sx_ = sinf(ax);
    pvr_vertex_t v;
    int i, j;

    for(i = 0; i < 8; i++) {
        float x = corner[i][0] * s, y = corner[i][1] * s, z = corner[i][2] * s;
        float x1 = x * cy_ + z * sy_, z1 = -x * sy_ + z * cy_;
        float y1 = y * cx_ - z1 * sx_, z2 = y * sx_ + z1 * cx_;
        float w = 1.0f / (z2 + cz);

        p[i][0] = 320.0f + (x1 + cx) * 400.0f * w;
        p[i][1] = 200.0f + (y1 + cy) * 400.0f * w;
        p[i][2] = w;
    }

    v.u = v.v = 0.0f;
    v.oargb = 0;

    for(i = 0; i < 6; i++) {
        v.argb = col[i];

        for(j = 0; j < 4; j++) {
            v.flags = j == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            v.x = sx(p[face[i][j]][0]);
            v.y = p[face[i][j]][1];
            v.z = p[face[i][j]][2];
            pvr_prim(&v, sizeof(v));
        }
    }
}

static void send_scene(float t) {
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    int x, y;

    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));

    for(y = 0; y < 6; y++)
        for(x = 0; x < 10; x++)
            cube((x - 4.5f) * 1.6f + sinf(t + y) * 0.5f, (y - 2.5f) * 1.4f,
                 9.0f + 2.0f * sinf(t * 0.7f + x * 0.5f), t + x * 0.3f,
                 t * 0.6f + y * 0.4f, 0.5f);
}

/* Text ****************************************************************/

static void text_update(const char *l1, const char *l2) {
    int i;

    for(i = 0; i < TEXT_W * TEXT_H; i++)
        text_buf[i] = 0;

    bfont_draw_str_ex(text_buf + 4 * TEXT_W + 4, TEXT_W, 0xffff, 0, 16, true, l1);
    bfont_draw_str_ex(text_buf + 32 * TEXT_W + 4, TEXT_W, 0xffe0, 0, 16, true, l2);
    pvr_txr_load(text_buf, text_txr, sizeof(text_buf));
}

static void send_text(void) {
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_vertex_t v;

    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
                     PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, TEXT_W, TEXT_H,
                     text_txr, PVR_FILTER_NONE);
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));

    v.z = TEXT_Z;
    v.argb = 0xffffffff;
    v.oargb = 0;
    v.flags = PVR_CMD_VERTEX;
    v.x = sx(32); v.y = 32 + TEXT_H; v.u = 0; v.v = 1;
    pvr_prim(&v, sizeof(v));
    v.x = sx(32); v.y = 32; v.u = 0; v.v = 0;
    pvr_prim(&v, sizeof(v));
    v.x = sx(32 + TEXT_W); v.y = 32 + TEXT_H; v.u = 1; v.v = 1;
    pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX_EOL;
    v.x = sx(32 + TEXT_W); v.y = 32; v.u = 1; v.v = 0;
    pvr_prim(&v, sizeof(v));
}

/* Setup ***************************************************************/

static void setup(void) {
    pvr_init_params_t params = {
        /* No punch-through for the TA: its slot holds the cache */
        .opb_sizes = { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_0,
                       PVR_BINSIZE_0, PVR_BINSIZE_0 },
        .vertex_buf_size = 512 * 1024,
        .fsaa_enabled = fsaa,
        .opb_overflow_count = 3,
        .vbuf_doublebuf_disabled = single_vbuf,
        .cached_list_size = 640 * 1024
    };
    pvr_cache_params_t cp = {
        .opb_sizes = { 0, 0, 0, 0, PVR_BINSIZE_16 },
        .vertex_buf_size = 384 * 1024,
        .opb_overflow_count = 2
    };
    pvr_cache_info_t info;

    pvr_init(&params);
    text_txr = pvr_mem_malloc(TEXT_W * TEXT_H * 2);

    cache = pvr_cache_create(&cp);

    if(!cache) {
        printf("cache_overlay: no room for the cache\n");
        arch_exit();
    }

    /* Emulators that don't keep the TA's object lists in VRAM fail here */
    cache_ok = pvr_cache_build(cache, build_overlay, NULL) == 0;

    if(!cache_ok)
        printf("cache_overlay: building the cache failed, sending the overlay "
               "every frame\n");

    pvr_cache_get_info(cache, &info);
    printf("cache_overlay: FSAA %s, %s vertex buffer: overlay of %d triangles, "
           "%lu parameter bytes, %lu list entries, built in %lu us\n",
           fsaa ? "on" : "off", single_vbuf ? "single" : "double", overlay_tris,
           (unsigned long)info.params_used, (unsigned long)info.entries,
           (unsigned long)(info.build_ns / 1000));

    if(!cache_ok)
        cached = 0;

    if(cached)
        pvr_cache_attach(cache, PVR_CACHE_SLOT(PVR_LIST_PT_POLY));
}

static void teardown(void) {
    pvr_cache_free(cache);
    pvr_mem_free(text_txr);
    pvr_shutdown();
}

static void set_cached(int on) {
    cached = on && cache_ok;
    pvr_cache_attach(on ? cache : NULL, PVR_CACHE_SLOT(PVR_LIST_PT_POLY));
}

int main(int argc, char **argv) {
    pvr_stats_t st;
    maple_device_t *dev;
    cont_state_t *cs;
    uint32_t prev = 0, pressed;
    uint64_t t0, cpu = 0, cpu_sum = 0, reg_sum = 0, rnd_sum = 0;
    int frame = 0, auto_mode = 1, step = 0, n = 0;
    float t = 0.0f;
    char l1[64], l2[80];

    (void)argc;
    (void)argv;

    setup();

    for(;;) {
        /* Input */
        pressed = 0;
        dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);

        if(dev && (cs = maple_dev_status(dev))) {
            pressed = cs->buttons & ~prev;
            prev = cs->buttons;
        }

        if(pressed)
            auto_mode = 0;

        if(pressed & CONT_START)
            break;

        if(pressed & CONT_A)
            set_cached(!cached);

        if(pressed & (CONT_X | CONT_Y)) {
            teardown();

            if(pressed & CONT_X)
                fsaa = !fsaa;

            if(pressed & CONT_Y)
                single_vbuf = !single_vbuf;

            setup();
        }

        /* Unattended: 4 s per mode, both modes in each configuration */
        if(auto_mode && frame == 240) {
            printf("cache_overlay: FSAA %s, %s vbuf, overlay %s: CPU %.3f ms, "
                   "TA %.3f ms, render %.3f ms\n", fsaa ? "on " : "off",
                   single_vbuf ? "single" : "double", cached ? "cached" : "sent  ",
                   cpu_sum / n / 1e6, reg_sum / n / 1e6, rnd_sum / n / 1e6);
            frame = 0;
            step++;

            if(step == 8)
                break;

            if(step & 1) {
                set_cached(1);
            }
            else {
                teardown();
                cached = 0;
                fsaa = (step >> 1) & 1;
                single_vbuf = (step >> 2) & 1;
                setup();
            }
        }

        if(frame == 0)
            cpu_sum = reg_sum = rnd_sum = n = 0;

        /* Text, four times a second */
        if(frame % 15 == 0) {
            pvr_get_stats(&st);
            snprintf(l1, sizeof(l1), "overlay %s  FSAA %s  vbuf %s",
                     cached ? "cached (PT slot)" : "sent every frame",
                     fsaa ? "on" : "off", single_vbuf ? "single" : "double");
            snprintf(l2, sizeof(l2), "CPU %.2f ms  TA %.2f ms  render %.2f ms  %.1f fps",
                     cpu / 1e6, st.reg_last_time / 1e6, st.rnd_last_time / 1e6,
                     st.frame_rate);
            text_update(l1, l2);
        }

        pvr_wait_ready();

        /* With one vertex buffer the first list waits for the previous
           render: wait here so that the timing below is CPU time only */
        if(single_vbuf)
            pvr_wait_render_done();

        /* CPU time to submit the frame */
        t0 = timer_ns_gettime64();
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);
        send_scene(t);
        send_text();

        if(!cached)
            send_overlay(PVR_LIST_OP_POLY);

        pvr_list_finish();
        pvr_scene_finish();
        cpu = timer_ns_gettime64() - t0;

        /* Statistics of the previous frame */
        pvr_get_stats(&st);

        if(frame >= 30) {
            cpu_sum += cpu;
            reg_sum += st.reg_last_time;
            rnd_sum += st.rnd_last_time;
            n++;
        }

        t += 1.0f / 60.0f;
        frame++;
    }

    teardown();

    return 0;
}
