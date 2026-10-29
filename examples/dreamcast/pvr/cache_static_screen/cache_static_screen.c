/* KallistiOS ##version##

   examples/dreamcast/pvr/cache_static_screen/cache_static_screen.c
   Copyright (C) 2026 Daniel Fairchild

   A title screen of heavy geometry (about 13000 opaque and translucent
   triangles), built once into a cache of object lists and then rendered
   every frame from the cache alone, with no TA work and almost no CPU time
   (pvr_scene_finish_cached(), dc/pvr/pvr_cache.h). For comparison, the
   same screen can be sent through the TA every frame.

   The counter and timings at the bottom are drawn by the CPU into a
   texture that one of the cached polygons uses. The cache holds the
   texture's address, not its texels, so the text still changes: proof that
   frames keep being rendered.

   Controls:
     A      switch between the cached screen and sending it every frame
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

#define PI_F        3.14159265f

#define TEXT_W      512
#define TEXT_H      64

static int fsaa, single_vbuf, cached = 1, cache_ok;
static pvr_cache_t *cache;
static pvr_ptr_t text_txr;
static uint16_t text_buf[TEXT_W * TEXT_H];
static int tris;

static float sx(float x) {
    return fsaa ? x * 2.0f : x;
}

/* Geometry ************************************************************/

static pvr_vertex_t vert;

static void vtx(float x, float y, float z, uint32_t argb, int last) {
    vert.flags = last ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    vert.x = sx(x);
    vert.y = y;
    vert.z = z;
    vert.u = vert.v = 0.0f;
    vert.argb = argb;
    vert.oargb = 0;
    pvr_prim(&vert, sizeof(vert));
}

static void quad(float x0, float y0, float x1, float y1, float z, uint32_t argb) {
    vtx(x0, y1, z, argb, 0);
    vtx(x0, y0, z, argb, 0);
    vtx(x1, y1, z, argb, 0);
    vtx(x1, y0, z, argb, 1);
    tris += 2;
}

static void ring(float cx, float cy, float r0, float r1, int n, float z,
                 uint32_t c0, uint32_t c1) {
    int i;
    float a;

    for(i = 0; i <= n; i++) {
        a = 2.0f * PI_F * i / n;
        vtx(cx + cosf(a) * r1, cy + sinf(a) * r1, z, c1, 0);
        vtx(cx + cosf(a) * r0, cy + sinf(a) * r0, z, c0, i == n);
    }

    tris += 2 * n;
}

static void header(pvr_list_t list) {
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;

    pvr_poly_cxt_col(&cxt, list);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));
}

/* 5x7 glyphs for the logo */
static const uint8_t glyph[][7] = {
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },   /* K */
    { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e },   /* O */
    { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e },   /* S */
};

static void logo(float x0, float y0) {
    int g, r, c, i, j;
    float s = 36.0f, q = s / 4.0f;

    for(g = 0; g < 3; g++) {
        for(r = 0; r < 7; r++) {
            for(c = 0; c < 5; c++) {
                if(!(glyph[g][r] & (0x10 >> c)))
                    continue;

                /* Each logo pixel is 4x4 small bevelled tiles */
                for(i = 0; i < 4; i++) {
                    for(j = 0; j < 4; j++) {
                        float x = x0 + g * 6 * s + c * s + j * q;
                        float y = y0 + r * s + i * q;

                        quad(x, y, x + q, y + q, 20.0f, 0xffffd040);
                        quad(x + 1, y + 1, x + q - 1, y + q - 1, 21.0f,
                             ((i + j) & 1) ? 0xffe08020 : 0xffc06010);
                    }
                }
            }
        }
    }
}

static void send_opaque(void) {
    int i, j;

    header(PVR_LIST_OP_POLY);

    /* Starburst */
    for(i = 0; i < 360; i++) {
        float a0 = 2.0f * PI_F * i / 360, a1 = 2.0f * PI_F * (i + 1) / 360;

        vtx(320, 240, 1.0f, 0xff101040, 0);
        vtx(320 + cosf(a0) * 500, 240 + sinf(a0) * 500, 1.0f,
            (i & 1) ? 0xff202080 : 0xff181860, 0);
        vtx(320 + cosf(a1) * 500, 240 + sinf(a1) * 500, 1.0f,
            (i & 1) ? 0xff202080 : 0xff181860, 1);
        tris++;
    }

    /* Concentric rings */
    for(i = 0; i < 8; i++)
        ring(320, 240, 60 + i * 30, 66 + i * 30, 180, 2.0f + i * 0.1f,
             0xff4060c0, 0xff80a0ff);

    /* Diamond grid */
    for(i = 0; i < 30; i++) {
        for(j = 0; j < 40; j++) {
            float x = 8 + j * 16, y = 8 + i * 16;

            vtx(x, y + 4, 5.0f, 0xff303070, 0);
            vtx(x + 4, y, 5.0f, 0xff5050a0, 0);
            vtx(x + 4, y + 8, 5.0f, 0xff303070, 0);
            vtx(x + 8, y + 4, 5.0f, 0xff202050, 1);
            tris += 2;
        }
    }

    logo(212, 96);

    /* Panel behind the text */
    quad(56, 392, 584, 464, 30.0f, 0xff000000);
}

static void send_translucent(void) {
    int i;

    header(PVR_LIST_TR_POLY);

    /* Glow rings over the logo */
    for(i = 0; i < 10; i++)
        ring(320, 220, 100 + i * 14, 112 + i * 14, 100, 40.0f + i,
             0x40ffffff, 0x00ffffff);
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

    v.z = 50.0f;
    v.argb = 0xffffffff;
    v.oargb = 0;
    v.flags = PVR_CMD_VERTEX;
    v.x = sx(64); v.y = 396 + TEXT_H; v.u = 0; v.v = 1;
    pvr_prim(&v, sizeof(v));
    v.x = sx(64); v.y = 396; v.u = 0; v.v = 0;
    pvr_prim(&v, sizeof(v));
    v.x = sx(64 + TEXT_W); v.y = 396 + TEXT_H; v.u = 1; v.v = 1;
    pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX_EOL;
    v.x = sx(64 + TEXT_W); v.y = 396; v.u = 1; v.v = 0;
    pvr_prim(&v, sizeof(v));
}

static void send_screen(void *data) {
    (void)data;

    tris = 0;
    pvr_list_begin(PVR_LIST_OP_POLY);
    send_opaque();
    send_text();
    pvr_list_finish();
    pvr_list_begin(PVR_LIST_TR_POLY);
    send_translucent();
    pvr_list_finish();
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

/* Setup ***************************************************************/

static void setup(void) {
    pvr_init_params_t params = {
        .opb_sizes = { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16,
                       PVR_BINSIZE_0, PVR_BINSIZE_0 },
        .vertex_buf_size = 1024 * 1024,
        .fsaa_enabled = fsaa,
        .opb_overflow_count = 3,
        .vbuf_doublebuf_disabled = single_vbuf,
        .cached_list_size = 1792 * 1024
    };
    pvr_cache_params_t cp = {
        .opb_sizes = { PVR_BINSIZE_32, 0, PVR_BINSIZE_16, 0, 0 },
        .vertex_buf_size = 960 * 1024,
        .opb_overflow_count = 3
    };
    pvr_cache_info_t info;

    if(pvr_init(&params) < 0) {
        printf("cache_static_screen: pvr_init failed\n");
        arch_exit();
    }

    text_txr = pvr_mem_malloc(TEXT_W * TEXT_H * 2);
    text_update("", "");

    cache = pvr_cache_create(&cp);

    if(!cache) {
        printf("cache_static_screen: no room for the cache\n");
        arch_exit();
    }

    /* Emulators that don't keep the TA's object lists in VRAM fail here */
    cache_ok = pvr_cache_build(cache, send_screen, NULL) == 0;

    if(!cache_ok) {
        printf("cache_static_screen: building the cache failed, sending the "
               "screen every frame\n");
        cached = 0;
    }

    pvr_cache_get_info(cache, &info);
    printf("cache_static_screen: FSAA %s, %s vertex buffer: %d triangles, "
           "%lu of %lu parameter bytes, %lu list entries, built in %lu us\n",
           fsaa ? "on" : "off", single_vbuf ? "single" : "double", tris,
           (unsigned long)info.params_used, (unsigned long)info.params_size,
           (unsigned long)info.entries, (unsigned long)(info.build_ns / 1000));
}

static void teardown(void) {
    pvr_cache_free(cache);
    pvr_mem_free(text_txr);
    pvr_shutdown();
}

int main(int argc, char **argv) {
    pvr_stats_t st;
    maple_device_t *dev;
    cont_state_t *cs;
    uint32_t prev = 0, pressed;
    uint64_t t0, cpu = 0, cpu_sum = 0, reg_sum = 0, rnd_sum = 0;
    int frame = 0, auto_mode = 1, step = 0, n = 0;
    char l1[80], l2[80];

    (void)argc;
    (void)argv;

    setup();

    for(;;) {
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
            cached = !cached && cache_ok;

        if(pressed & (CONT_X | CONT_Y)) {
            teardown();

            if(pressed & CONT_X)
                fsaa = !fsaa;

            if(pressed & CONT_Y)
                single_vbuf = !single_vbuf;

            cached = 1;
            setup();
        }

        /* Unattended: 4 s per mode, both modes in each configuration */
        if(auto_mode && frame == 240) {
            printf("cache_static_screen: FSAA %s, %s vbuf, %s: CPU %.3f ms, "
                   "TA %.3f ms, render %.3f ms, frames %lu\n",
                   fsaa ? "on " : "off", single_vbuf ? "single" : "double",
                   cached ? "cached" : "sent  ", cpu_sum / n / 1e6,
                   reg_sum / n / 1e6, rnd_sum / n / 1e6,
                   (unsigned long)st.frame_count);
            frame = 0;
            step++;

            if(step == 8)
                break;

            if(step & 1) {
                cached = 0;
            }
            else {
                teardown();
                fsaa = (step >> 1) & 1;
                single_vbuf = (step >> 2) & 1;
                cached = 1;
                setup();
            }
        }

        if(frame == 0)
            cpu_sum = reg_sum = rnd_sum = n = 0;

        /* The counter changes every frame, the rest four times a second */
        pvr_get_stats(&st);

        if(frame % 15 == 0)
            snprintf(l1, sizeof(l1), "%s  FSAA %s  vbuf %s",
                     cached ? "cached frame, no TA" : "sent every frame",
                     fsaa ? "on" : "off", single_vbuf ? "single" : "double");

        snprintf(l2, sizeof(l2), "frame %6lu  CPU %.3f ms  TA %.2f  render %.2f ms",
                 (unsigned long)st.frame_count, cpu / 1e6,
                 st.reg_last_time / 1e6, st.rnd_last_time / 1e6);
        text_update(l1, l2);

        pvr_wait_ready();

        /* With one vertex buffer the first list waits for the previous
           render: wait here so that the timing below is CPU time only */
        if(single_vbuf)
            pvr_wait_render_done();

        /* CPU time to submit the frame */
        t0 = timer_ns_gettime64();
        pvr_scene_begin();

        if(cached) {
            pvr_scene_finish_cached(cache);
        }
        else {
            send_screen(NULL);
            pvr_scene_finish();
        }

        cpu = timer_ns_gettime64() - t0;

        if(frame >= 30) {
            pvr_get_stats(&st);
            cpu_sum += cpu;
            reg_sum += st.reg_last_time;
            rnd_sum += st.rnd_last_time;
            n++;
        }

        frame++;
    }

    teardown();

    return 0;
}
