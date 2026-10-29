/* KallistiOS ##version##

   examples/dreamcast/pvr/multipass_basic/multipass_basic.c

   Multipass rendering basics (pvr_init_params_t::pass_count).

   Pass 0 draws a grey opaque floor and a 50% red translucent wall standing
   on it. Pass 1 draws a blue opaque cube that cuts through the floor and
   turns behind the wall.

   - With two passes, the cube is depth tested against the floor drawn in
     pass 0 (the floor cuts the cube), and the red translucency of pass 0
     survives around the cube. Since all of pass 1 is drawn after all of
     pass 0, the cube covers the wall even where it is behind it: the wall
     doesn't write depth.
   - With one pass, the same polygons go into one set of lists: the
     translucent wall is blended over the cube where the wall is in front.

   Controls: A switches between one and two passes (the mode also changes by
   itself every 5 seconds until A is pressed), Y toggles FSAA, Start exits.
   The mode and the PVR render time are shown on screen and printed on the
   serial console.
*/

#include <stdio.h>
#include <string.h>
#include <math.h>

#include <arch/timer.h>
#include <dc/pvr.h>
#include <dc/biosfont.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

/**** Setup and text overlay ***************************************/

static pvr_init_params_t params = {
    .opb_sizes = { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16,
                   PVR_BINSIZE_0, PVR_BINSIZE_0 },
    .vertex_buf_size = 512 * 1024,
    .opb_overflow_count = 3,
};

static int fsaa;            /* FSAA on? */
static float xs = 1.0f;     /* Horizontal scale: 2 with FSAA */

#define TXT_W 512
#define TXT_H 64
static uint16_t txt_buf[TXT_W * TXT_H] __attribute__((aligned(32)));
static pvr_ptr_t txt_tex;
static pvr_poly_hdr_t hdr_txt, hdr_op, hdr_tr;

/* Two lines of text, drawn into an ARGB4444 texture */
static void text_set(const char *l1, const char *l2) {
    int i;

    for(i = 0; i < TXT_W * TXT_H; i++)
        txt_buf[i] = 0x8000;

    bfont_draw_str_ex(txt_buf + 4 * TXT_W + 4, TXT_W, 0xffff, 0x8000, 16,
                      true, l1);
    bfont_draw_str_ex(txt_buf + 32 * TXT_W + 4, TXT_W, 0xffff, 0x8000, 16,
                      true, l2);
    pvr_txr_load(txt_buf, txt_tex, sizeof(txt_buf));
}

static void compile_headers(void) {
    pvr_poly_cxt_t cxt;

    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY,
                     PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED,
                     TXT_W, TXT_H, txt_tex, PVR_FILTER_NONE);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr_txt, &cxt);

    /* No culling: the depth buffer sorts the cube's faces out */
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr_op, &cxt);

    /* The wall doesn't write depth */
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(&hdr_tr, &cxt);
}

/* (Re)initialize the PVR for the given number of passes and FSAA setting */
static void setup(int passes) {
    static int cur_passes = -1, cur_fsaa = -1;

    if(passes == cur_passes && fsaa == cur_fsaa)
        return;

    if(cur_passes >= 0)
        pvr_shutdown();

    params.pass_count = passes;
    params.fsaa_enabled = fsaa;
    pvr_init(&params);
    pvr_set_bg_color(0.1f, 0.1f, 0.2f);

    xs = fsaa ? 2.0f : 1.0f;
    txt_tex = pvr_mem_malloc(sizeof(txt_buf));
    compile_headers();

    cur_passes = passes;
    cur_fsaa = fsaa;
}

static void draw_text(void) {
    pvr_vertex_t v __attribute__((aligned(32)));

    pvr_prim(&hdr_txt, sizeof(hdr_txt));
    v.argb = 0xffffffff;
    v.oargb = 0;
    v.z = 1000.0f;

    v.flags = PVR_CMD_VERTEX;
    v.x = 16.0f * xs; v.y = 16.0f + TXT_H; v.u = 0.0f; v.v = 1.0f;
    pvr_prim(&v, sizeof(v));
    v.y = 16.0f; v.v = 0.0f;
    pvr_prim(&v, sizeof(v));
    v.x = (16.0f + TXT_W) * xs; v.y = 16.0f + TXT_H; v.u = 1.0f; v.v = 1.0f;
    pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX_EOL;
    v.y = 16.0f; v.v = 0.0f;
    pvr_prim(&v, sizeof(v));
}

/**** 3D ***********************************************************/

typedef struct {
    float x, y, z;
} vec3_t;

/* Camera: position, then pitch (looking down) */
static const vec3_t cam = { 0.0f, 3.0f, -7.0f };
static const float cam_pitch = 0.35f;

static void project(vec3_t p, float *sx, float *sy, float *sz) {
    float x = p.x - cam.x, y = p.y - cam.y, z = p.z - cam.z;
    float c = cosf(cam_pitch), s = sinf(cam_pitch);
    float y2 = y * c + z * s, z2 = z * c - y * s;

    *sx = (320.0f + 400.0f * x / z2) * xs;
    *sy = 240.0f - 400.0f * y2 / z2;
    *sz = 1.0f / z2;
}

static void vert3(uint32_t flags, vec3_t p, uint32_t argb) {
    pvr_vertex_t v __attribute__((aligned(32)));

    v.flags = flags;
    project(p, &v.x, &v.y, &v.z);
    v.u = v.v = 0.0f;
    v.argb = argb;
    v.oargb = 0;
    pvr_prim(&v, sizeof(v));
}

/* A quad with corners a, b, c, d in order around it */
static void quad3(vec3_t a, vec3_t b, vec3_t c, vec3_t d, uint32_t argb) {
    vert3(PVR_CMD_VERTEX, a, argb);
    vert3(PVR_CMD_VERTEX, b, argb);
    vert3(PVR_CMD_VERTEX, d, argb);
    vert3(PVR_CMD_VERTEX_EOL, c, argb);
}

static void floor_quad(void) {
    vec3_t f[4] = { { -6, 0, -5 }, { -6, 0, 8 }, { 6, 0, 8 }, { 6, 0, -5 } };

    pvr_prim(&hdr_op, sizeof(hdr_op));
    quad3(f[0], f[1], f[2], f[3], 0xff808080);
}

static void wall_quad(void) {
    vec3_t w[4] = { { -3, 0, 0.5f }, { -3, 2.5f, 0.5f }, { 0.5f, 2.5f, 0.5f },
                    { 0.5f, 0, 0.5f } };

    pvr_prim(&hdr_tr, sizeof(hdr_tr));
    quad3(w[0], w[1], w[2], w[3], 0x80ff0000);
}

/* Blue cube turning about the vertical axis, partly under the floor */
static void cube(float angle) {
    static const float corner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    static const int faces[6][4] = {
        { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 },
        { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 }
    };
    static const uint32_t colors[6] = {
        0xff0020a0, 0xff4080ff, 0xff2040ff, 0xff1030c0, 0xff3060ff, 0xff102080
    };
    vec3_t v[8];
    float c = cosf(angle), s = sinf(angle);
    int i;

    /* Corners 0-3 at the bottom, 4-7 at the top, in order around */
    for(i = 0; i < 8; i++) {
        float x = corner[i & 3][0], z = corner[i & 3][1];

        v[i].x = 0.5f + x * c - z * s;
        v[i].y = (i & 4) ? 1.2f : -0.8f;
        v[i].z = 1.5f + x * s + z * c;
    }

    pvr_prim(&hdr_op, sizeof(hdr_op));

    for(i = 0; i < 6; i++)
        quad3(v[faces[i][0]], v[faces[i][1]], v[faces[i][2]], v[faces[i][3]],
              colors[i]);
}

/**** Main *********************************************************/

static void do_frame(int two_pass, float angle) {
    pvr_wait_ready();
    pvr_scene_begin();

    if(two_pass) {
        /* Pass 0: floor and wall */
        pvr_list_begin(PVR_LIST_OP_POLY);
        floor_quad();
        pvr_list_begin(PVR_LIST_TR_POLY);
        wall_quad();

        /* Pass 1: the cube, and the text on top of everything */
        pvr_scene_next_pass();
        pvr_list_begin(PVR_LIST_OP_POLY);
        cube(angle);
        pvr_list_begin(PVR_LIST_TR_POLY);
        draw_text();
    }
    else {
        pvr_list_begin(PVR_LIST_OP_POLY);
        floor_quad();
        cube(angle);
        pvr_list_begin(PVR_LIST_TR_POLY);
        wall_quad();
        draw_text();
    }

    pvr_scene_finish();
}

int main(int argc, char *argv[]) {
    maple_device_t *cont;
    cont_state_t *st;
    uint32_t prev = 0, pressed;
    int two_pass = 1, autocycle = 1, frame = 0;
    uint64_t next_switch = timer_ms_gettime64() + 5000;
    float angle = 0.0f;
    pvr_stats_t stats;
    char l1[64], l2[64];

    (void)argc;
    (void)argv;

    printf("multipass_basic: A = 1/2 passes, Y = FSAA, Start = exit\n");

    for(;;) {
        cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        st = cont ? (cont_state_t *)maple_dev_status(cont) : NULL;
        pressed = st ? st->buttons & ~prev : 0;
        prev = st ? st->buttons : 0;

        if(pressed & CONT_START)
            break;

        if(pressed & CONT_A) {
            two_pass = !two_pass;
            autocycle = 0;
        }

        if(pressed & CONT_Y)
            fsaa = !fsaa;

        if(autocycle && timer_ms_gettime64() >= next_switch) {
            two_pass = !two_pass;
            next_switch = timer_ms_gettime64() + 5000;
        }

        setup(two_pass ? 2 : 1);

        /* Refresh the text twice per second, or on a mode change */
        if((frame++ % 30) == 0 || pressed) {
            pvr_get_stats(&stats);
            snprintf(l1, sizeof(l1), "%s, FSAA %s",
                     two_pass ? "2 passes: cube in pass-1 OP" :
                                "1 pass: cube in the same OP",
                     fsaa ? "on" : "off");
            /* Right after pvr_init(), there is no render time yet. */
            if(stats.rnd_last_time < 1000000000ULL)
                snprintf(l2, sizeof(l2), "PVR render %.2f ms  (A/Y/Start)",
                         (double)stats.rnd_last_time / 1e6);
            else
                snprintf(l2, sizeof(l2), "PVR render -- ms  (A/Y/Start)");
            text_set(l1, l2);

            if((frame % 60) == 1)
                printf("multipass_basic: %s; %s\n", l1, l2);
        }

        do_frame(two_pass, angle);
        angle += 0.01f;
    }

    pvr_shutdown();

    return 0;
}
