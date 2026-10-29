/* KallistiOS ##version##

   examples/dreamcast/pvr/cache_bench/cache_bench.c
   Copyright (C) 2026 Daniel Fairchild

   How much does caching object lists save? N small quads (16x16 pixels),
   for several N, drawn four ways:

     sent OP     sent through the TA every frame, in the opaque list
     sent PT     sent through the TA every frame, in the punch-through list
     cache PT    built once into a cache, attached to the punch-through slot
                 of TA frames whose lists are empty (pvr_cache_attach())
     cache frame built once, rendered as a whole frame with no TA work
                 (pvr_scene_finish_cached())

   For each, the averages over 100 frames of the CPU time to submit a frame,
   the TA registration time and the render time (pvr_get_stats()) are
   printed on the serial console as a table. "cache PT" and "cache frame"
   compare with "sent PT": the punch-through list renders a little slower
   than the opaque one.
*/

#include <stdio.h>
#include <stdlib.h>

#include <kos/timer.h>
#include <arch/arch.h>
#include <dc/pvr.h>

#define FRAMES      100
#define WARMUP      20
#define MAX_QUADS   10000

static const int counts[] = { 500, 1000, 2000, 5000, 10000 };

static int nquads;

typedef enum { SENT_OP, SENT_PT, CACHE_PT, CACHE_FRAME, MODES } bench_mode_t;

static const char *mode_name[MODES] = {
    "sent OP", "sent PT", "cache PT", "cache frame"
};

/* Quads at the same pseudo-random places every time */
static void send_quads(pvr_list_t list) {
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_vertex_t v;
    uint32_t r = 12345;
    float x, y, z = 1.0f;
    int i;

    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr, &cxt);
    hdr.cmd = (hdr.cmd & ~PVR_TA_CMD_TYPE) | FIELD_PREP(PVR_TA_CMD_TYPE, list);
    pvr_prim(&hdr, sizeof(hdr));

    v.u = v.v = 0.0f;
    v.oargb = 0;

    for(i = 0; i < nquads; i++) {
        r = r * 1103515245 + 12345;
        x = (r >> 8) % 624;
        r = r * 1103515245 + 12345;
        y = (r >> 8) % 464;
        v.argb = 0xff000000 | (r & 0xffffff);
        v.z = z;
        z += 0.0001f;

        v.flags = PVR_CMD_VERTEX;
        v.x = x; v.y = y + 16;
        pvr_prim(&v, sizeof(v));
        v.x = x; v.y = y;
        pvr_prim(&v, sizeof(v));
        v.x = x + 16; v.y = y + 16;
        pvr_prim(&v, sizeof(v));
        v.flags = PVR_CMD_VERTEX_EOL;
        v.x = x + 16; v.y = y;
        pvr_prim(&v, sizeof(v));
    }
}

static void build_quads(void *data) {
    (void)data;

    pvr_list_begin(PVR_LIST_PT_POLY);
    send_quads(PVR_LIST_PT_POLY);
    pvr_list_finish();
}

static void measure(bench_mode_t mode, pvr_cache_t *cache) {
    pvr_stats_t st;
    uint64_t t0, cpu = 0, reg = 0, rnd = 0, frm = 0;
    int f;

    for(f = 0; f < WARMUP + FRAMES; f++) {
        pvr_wait_ready();

        t0 = timer_ns_gettime64();
        pvr_scene_begin();

        switch(mode) {
            case SENT_OP:
                pvr_list_begin(PVR_LIST_OP_POLY);
                send_quads(PVR_LIST_OP_POLY);
                pvr_list_finish();
                pvr_scene_finish();
                break;
            case SENT_PT:
                pvr_list_begin(PVR_LIST_PT_POLY);
                send_quads(PVR_LIST_PT_POLY);
                pvr_list_finish();
                pvr_scene_finish();
                break;
            case CACHE_PT:
                pvr_scene_finish();
                break;
            default:
                pvr_scene_finish_cached(cache);
                break;
        }

        t0 = timer_ns_gettime64() - t0;

        /* Statistics of the previous frame */
        pvr_get_stats(&st);

        if(f >= WARMUP) {
            cpu += t0;
            reg += st.reg_last_time;
            rnd += st.rnd_last_time;
            frm += st.frame_last_time;
        }
    }

    printf("%6d  %-11s  %8.3f  %8.3f  %8.3f  %8.3f\n", nquads, mode_name[mode],
           cpu / (double)FRAMES / 1e6, reg / (double)FRAMES / 1e6,
           rnd / (double)FRAMES / 1e6, frm / (double)FRAMES / 1e6);
}

int main(int argc, char **argv) {
    pvr_init_params_t sent = {
        .opb_sizes = { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_0,
                       PVR_BINSIZE_0, PVR_BINSIZE_16 },
        .vertex_buf_size = 1024 * 1024,
        .opb_overflow_count = 7
    };
    pvr_init_params_t cached = {
        /* The TA's punch-through slot is left to the cache */
        .opb_sizes = { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_0,
                       PVR_BINSIZE_0, PVR_BINSIZE_0 },
        .vertex_buf_size = 128 * 1024,
        .opb_overflow_count = 1,
        .cached_list_size = 1408 * 1024
    };
    pvr_cache_params_t cp = {
        .opb_sizes = { 0, 0, 0, 0, PVR_BINSIZE_32 },
        .vertex_buf_size = 1024 * 1024,
        .opb_overflow_count = 5
    };
    pvr_cache_info_t info;
    pvr_cache_t *cache;
    size_t i;

    (void)argc;
    (void)argv;

    printf("cache_bench: times in ms per frame, averaged over %d frames\n\n", FRAMES);
    printf("quads   mode              CPU        TA    render     frame\n");

    for(i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        nquads = counts[i];

        pvr_init(&sent);
        measure(SENT_OP, NULL);
        measure(SENT_PT, NULL);
        pvr_shutdown();

        pvr_init(&cached);
        cache = pvr_cache_create(&cp);

        if(!cache || pvr_cache_build(cache, build_quads, NULL) < 0) {
            printf("%6d  cache build failed\n", nquads);
        }
        else {
            pvr_cache_get_info(cache, &info);
            pvr_cache_attach(cache, PVR_CACHE_SLOT(PVR_LIST_PT_POLY));
            measure(CACHE_PT, cache);
            pvr_cache_attach(NULL, PVR_CACHE_SLOT(PVR_LIST_PT_POLY));
            measure(CACHE_FRAME, cache);
            printf("%6d  (cache: %lu entries, %lu parameter bytes, built in %.3f ms)\n",
                   nquads, (unsigned long)info.entries,
                   (unsigned long)info.params_used, info.build_ns / 1e6);
        }

        pvr_cache_free(cache);
        pvr_shutdown();
    }

    printf("\ncache_bench: done\n");

    return 0;
}
