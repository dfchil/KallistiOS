/* KallistiOS ##version##

   examples/dreamcast/pvr/multipass_sortmix/multipass_sortmix.c

   Presorted and autosorted translucency in one frame, with multipass
   rendering (pvr_init_params_t::pass_count and pvr_set_pass_presort()).

   Pass 0's translucent list holds layered 2D cards, like UI panels or
   decals: they must be drawn in the order they are sent, whatever their
   depth values (here, each card is sent farther than the one before, and
   the last one sent must end up on top). That needs presort.

   Pass 1's translucent list holds a turning cloud of intersecting
   translucent quads, which only per-pixel sorting draws right. That needs
   autosort.

   The sort mode of each pass can be switched: presort/autosort is right;
   presort everywhere breaks the cloud (each quad covers the ones sent
   before it); autosort everywhere breaks the card order (the first card
   sent, the nearest, ends up on top).

   Controls: A cycles the sort modes (the mode also changes by itself every
   5 seconds until A is pressed), Y toggles FSAA, Start exits. The mode and
   the PVR render time are shown on screen and printed on the serial
   console.
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
    .opb_sizes = { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_0 },
    .vertex_buf_size = 512 * 1024,
    .opb_overflow_count = 3,
};

static int fsaa;            /* FSAA on? */
static float xs = 1.0f;     /* Horizontal scale: 2 with FSAA */

#define TXT_W 512
#define TXT_H 64
static uint16_t txt_buf[TXT_W * TXT_H] __attribute__((aligned(32)));
static pvr_ptr_t txt_tex;
static pvr_poly_hdr_t hdr_txt;

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

/* Second text line: the PVR render time */
static void text_render_time(char *buf, size_t size) {
    pvr_stats_t stats;

    pvr_get_stats(&stats);

    /* Right after pvr_init(), there is no render time yet. */
    if(stats.rnd_last_time < 1000000000ULL)
        snprintf(buf, size, "PVR render %.2f ms  (A/Y/Start)",
                 (double)stats.rnd_last_time / 1e6);
    else
        snprintf(buf, size, "PVR render -- ms  (A/Y/Start)");
}

static void compile_headers(void);

/* (Re)initialize the PVR for the given number of passes and FSAA setting.
   Returns 1 if it did. */
static int setup(int passes) {
    static int cur_passes = -1, cur_fsaa = -1;
    pvr_poly_cxt_t cxt;

    if(passes == cur_passes && fsaa == cur_fsaa)
        return 0;

    if(cur_passes >= 0)
        pvr_shutdown();

    params.pass_count = passes;
    params.fsaa_enabled = fsaa;
    pvr_init(&params);
    pvr_set_bg_color(0.15f, 0.15f, 0.25f);

    xs = fsaa ? 2.0f : 1.0f;
    txt_tex = pvr_mem_malloc(sizeof(txt_buf));

    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY,
                     PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED,
                     TXT_W, TXT_H, txt_tex, PVR_FILTER_NONE);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr_txt, &cxt);
    compile_headers();

    cur_passes = passes;
    cur_fsaa = fsaa;

    return 1;
}

/* The text, in the open translucent list; submit it last if it's presorted */
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

/* Controller: returns the buttons newly pressed since the last call */
static uint32_t buttons_pressed(void) {
    static uint32_t prev;
    maple_device_t *cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *st = cont ? (cont_state_t *)maple_dev_status(cont) : NULL;
    uint32_t now = st ? st->buttons : 0, pressed = now & ~prev;

    prev = now;
    return pressed;
}

/**** 3D ***********************************************************/

typedef struct {
    float x, y, z;
} vec3_t;

/* Camera position, heading (yaw) and pitch (positive looks down) */
static vec3_t cam;
static float cam_yaw, cam_pitch;

/* Camera space: x right, y up, z forward */
static vec3_t to_camera(vec3_t p) {
    float x = p.x - cam.x, y = p.y - cam.y, z = p.z - cam.z;
    float cy = cosf(cam_yaw), sy = sinf(cam_yaw);
    float cp = cosf(cam_pitch), sp = sinf(cam_pitch);
    float x1 = x * cy - z * sy, z1 = x * sy + z * cy;
    vec3_t r = { x1, y * cp + z1 * sp, z1 * cp - y * sp };

    return r;
}

/* Screen position and depth (1/z) of a camera-space point */
static void project_cam(vec3_t c, float *sx, float *sy, float *sz) {
    *sx = (320.0f + 400.0f * c.x / c.z) * xs;
    *sy = 240.0f - 400.0f * c.y / c.z;
    *sz = 1.0f / c.z;
}

static void vert_cam(uint32_t flags, vec3_t c, uint32_t argb) {
    pvr_vertex_t v __attribute__((aligned(32)));

    v.flags = flags;
    project_cam(c, &v.x, &v.y, &v.z);
    v.u = v.v = 0.0f;
    v.argb = argb;
    v.oargb = 0;
    pvr_prim(&v, sizeof(v));
}

/* A quad with corners a, b, c, d in order around it, in camera space */
static void quad_cam(vec3_t a, vec3_t b, vec3_t c, vec3_t d, uint32_t argb) {
    vert_cam(PVR_CMD_VERTEX, a, argb);
    vert_cam(PVR_CMD_VERTEX, b, argb);
    vert_cam(PVR_CMD_VERTEX, d, argb);
    vert_cam(PVR_CMD_VERTEX_EOL, c, argb);
}

/* The same in world space */
static void quad3(vec3_t a, vec3_t b, vec3_t c, vec3_t d, uint32_t argb) {
    quad_cam(to_camera(a), to_camera(b), to_camera(c), to_camera(d), argb);
}

/* A screen-space rectangle (640x480 coordinates) */
static void rect(float x0, float y0, float x1, float y1, float z, uint32_t argb) {
    pvr_vertex_t v __attribute__((aligned(32)));

    v.u = v.v = 0.0f;
    v.argb = argb;
    v.oargb = 0;
    v.z = z;
    v.flags = PVR_CMD_VERTEX;
    v.x = x0 * xs; v.y = y1;
    pvr_prim(&v, sizeof(v));
    v.y = y0;
    pvr_prim(&v, sizeof(v));
    v.x = x1 * xs; v.y = y1;
    pvr_prim(&v, sizeof(v));
    v.flags = PVR_CMD_VERTEX_EOL;
    v.y = y0;
    pvr_prim(&v, sizeof(v));
}

/**** Scene ********************************************************/

static pvr_poly_hdr_t hdr_op, hdr_card, hdr_cloud;

static void compile_headers(void) {
    pvr_poly_cxt_t cxt;

    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr_op, &cxt);

    /* Cards: 2D layers, the depth test plays no part */
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(&hdr_card, &cxt);

    /* Cloud: depth tested against the opaque floor, no depth write */
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(&hdr_cloud, &cxt);
}

/* An opaque checkerboard floor, so that the translucency shows */
static void floor_checker(void) {
    int i, j;

    pvr_prim(&hdr_op, sizeof(hdr_op));

    for(i = -6; i < 6; i++) {
        for(j = -2; j < 10; j++) {
            vec3_t a = { i, 0, j }, b = { i, 0, j + 1 }, c = { i + 1, 0, j + 1 },
                   d = { i + 1, 0, j };

            quad3(a, b, c, d, ((i + j) & 1) ? 0xff505050 : 0xffa0a0a0);
        }
    }
}

/* Five overlapping cards on the left, sent in order. Each one is sent
   farther than the one before: only the send order puts the last one on
   top. */
static void cards(void) {
    static const uint32_t colors[5] = {
        0xd0ff4040, 0xd0ff9020, 0xd0f0e020, 0xd040d040, 0xd04080ff
    };
    int i;

    pvr_prim(&hdr_card, sizeof(hdr_card));

    for(i = 0; i < 5; i++)
        rect(40 + i * 30, 120 + i * 40, 200 + i * 30, 240 + i * 40,
             50.0f - i * 10.0f, colors[i]);
}

/* Six translucent quads through a common centre, at different angles,
   turning: they cut through each other. */
static void cloud(float t) {
    static const uint32_t colors[6] = {
        0x80ff2020, 0x8020ff20, 0x802020ff, 0x80ffff20, 0x8020ffff, 0x80ff20ff
    };
    vec3_t ctr = { 2.0f, 1.6f, 3.0f };
    int i;

    pvr_prim(&hdr_cloud, sizeof(hdr_cloud));

    for(i = 0; i < 6; i++) {
        float a = t * 0.7f + i * (float)M_PI / 6.0f;
        float tilt = (i & 1) ? 0.5f : -0.5f;
        float cx = cosf(a) * 1.4f, cz = sinf(a) * 1.4f;
        vec3_t p[4] = {
            { ctr.x - cx, ctr.y - 1.2f - tilt, ctr.z - cz },
            { ctr.x - cx, ctr.y + 1.2f - tilt, ctr.z - cz },
            { ctr.x + cx, ctr.y + 1.2f + tilt, ctr.z + cz },
            { ctr.x + cx, ctr.y - 1.2f + tilt, ctr.z + cz },
        };

        quad3(p[0], p[1], p[2], p[3], colors[i]);
    }
}

static void do_frame(float t) {
    pvr_wait_ready();
    pvr_scene_begin();

    /* Pass 0: floor, cards */
    pvr_list_begin(PVR_LIST_OP_POLY);
    floor_checker();
    pvr_list_begin(PVR_LIST_TR_POLY);
    cards();

    /* Pass 1: cloud, text */
    pvr_scene_next_pass();
    pvr_list_begin(PVR_LIST_TR_POLY);
    cloud(t);
    draw_text();

    pvr_scene_finish();
}

/**** Main *********************************************************/

/* Sort mode of pass 0 and pass 1 */
static const struct {
    bool presort0, presort1;
    const char *name;
} modes[4] = {
    { true, false, "cards presort, cloud autosort: right" },
    { true, true, "presort both: cloud wrong" },
    { false, false, "autosort both: card order wrong" },
    { false, true, "cards autosort, cloud presort: wrong" },
};

int main(int argc, char *argv[]) {
    uint32_t pressed;
    int mode = 0, applied = -1, autocycle = 1, frame = 0;
    uint64_t next_switch = timer_ms_gettime64() + 5000;
    float t = 0.0f;
    char l1[64], l2[64];

    (void)argc;
    (void)argv;

    printf("multipass_sortmix: A = sort modes, Y = FSAA, Start = exit\n");

    cam.x = 0.0f; cam.y = 3.0f; cam.z = -5.0f;
    cam_pitch = 0.3f;

    for(;;) {
        pressed = buttons_pressed();

        if(pressed & CONT_START)
            break;

        if(pressed & CONT_A) {
            mode = (mode + 1) % 4;
            autocycle = 0;
        }

        if(pressed & CONT_Y)
            fsaa = !fsaa;

        if(autocycle && timer_ms_gettime64() >= next_switch) {
            mode = (mode + 1) % 4;
            next_switch = timer_ms_gettime64() + 5000;
        }

        if(setup(2))
            applied = -1;

        /* Set the sort modes once the previous scene has started rendering,
           so that they apply to the next scene. They are kept until
           changed. */
        if(applied != mode) {
            pvr_wait_ready();
            pvr_set_pass_presort(0, modes[mode].presort0);
            pvr_set_pass_presort(1, modes[mode].presort1);
            applied = mode;
        }

        if((frame++ % 30) == 0 || pressed) {
            snprintf(l1, sizeof(l1), "%d: %s%s", mode + 1, modes[mode].name,
                     fsaa ? ", FSAA" : "");
            text_render_time(l2, sizeof(l2));
            text_set(l1, l2);

            if((frame % 60) == 1)
                printf("multipass_sortmix: %s; %s\n", l1, l2);
        }

        do_frame(t);
        t += 1.0f / 60.0f;
    }

    pvr_shutdown();

    return 0;
}
