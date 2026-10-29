/* KallistiOS ##version##

   examples/dreamcast/pvr/multipass_viewmodel/multipass_viewmodel.c

   A first-person "view model" with its own depth range, with multipass
   rendering (pvr_init_params_t::pass_count).

   The camera stands right in front of a wall and holds a long object that
   would poke into it. Pass 0 draws the room. At its end, in a presorted
   translucent list, a full-screen polygon with alpha 0, depth compare
   "always", depth write on and the farthest depth resets the depth buffer
   and leaves the colour alone. Pass 1 then draws the held object in the
   opaque list: it is depth tested only against itself, and stays in front
   of the wall.

   With one pass, the held object goes into the same opaque list as the
   room, and clips into the wall.

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
    .opb_sizes = { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_0 },
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
    pvr_set_bg_color(0.1f, 0.1f, 0.1f);

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

/* A box turning about the vertical axis: centre, half sizes, angle, and one
   colour per face (bottom, top, then the four sides) */
static void box(vec3_t c, vec3_t h, float angle, const uint32_t colors[6]) {
    static const float corner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    static const int faces[6][4] = {
        { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 },
        { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 }
    };
    vec3_t v[8];
    float co = cosf(angle), si = sinf(angle);
    int i;

    /* Corners 0-3 at the bottom, 4-7 at the top, in order around */
    for(i = 0; i < 8; i++) {
        float x = corner[i & 3][0] * h.x, z = corner[i & 3][1] * h.z;
        vec3_t p = { c.x + x * co - z * si, c.y + ((i & 4) ? h.y : -h.y),
                     c.z + x * si + z * co };

        v[i] = to_camera(p);
    }

    for(i = 0; i < 6; i++)
        quad_cam(v[faces[i][0]], v[faces[i][1]], v[faces[i][2]], v[faces[i][3]],
                 colors[i]);
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

static pvr_poly_hdr_t hdr_op, hdr_tr, hdr_reset;

static void compile_headers(void) {
    pvr_poly_cxt_t cxt;

    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr_op, &cxt);

    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(&hdr_tr, &cxt);

    /* The depth reset: drawn whatever the depth, writes depth, and leaves
       the colour unchanged (alpha 0, and blending ZERO / ONE to be sure) */
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = PVR_DEPTHWRITE_ENABLE;
    cxt.blend.src = PVR_BLEND_ZERO;
    cxt.blend.dst = PVR_BLEND_ONE;
    pvr_poly_compile(&hdr_reset, &cxt);
}

/* The wall right in front of the camera (at z = 1), and the floor */
static void room(void) {
    int i, j;

    pvr_prim(&hdr_op, sizeof(hdr_op));

    for(i = -6; i < 6; i++) {
        for(j = -3; j < 4; j++) {
            vec3_t a = { i * 0.5f, j * 0.5f, 1.0f }, b = { i * 0.5f, j * 0.5f + 0.5f, 1.0f },
                   c = { i * 0.5f + 0.5f, j * 0.5f + 0.5f, 1.0f },
                   d = { i * 0.5f + 0.5f, j * 0.5f, 1.0f };

            quad3(a, b, c, d, ((i + j) & 1) ? 0xff806040 : 0xffa08060);
        }
    }

    {
        vec3_t a = { -3, -1.5f, 0.2f }, b = { -3, -1.5f, 1.0f },
               c = { 3, -1.5f, 1.0f }, d = { 3, -1.5f, 0.2f };

        quad3(a, b, c, d, 0xff404040);
    }
}

/* A translucent poster on the wall */
static void poster(void) {
    vec3_t a = { -0.9f, -0.2f, 0.98f }, b = { -0.9f, 0.6f, 0.98f },
           c = { 0.1f, 0.6f, 0.98f }, d = { 0.1f, -0.2f, 0.98f };

    pvr_prim(&hdr_tr, sizeof(hdr_tr));
    quad3(a, b, c, d, 0x8020c0ff);
}

/* The depth reset polygon, covering the whole screen at the farthest depth */
static void depth_reset(void) {
    pvr_prim(&hdr_reset, sizeof(hdr_reset));
    rect(0, 0, 640, 480, 0.00001f, 0x00000000);
}

/* The held object: a long box from 0.5 to 2.2 in front of the camera,
   bobbing a little */
static void held_object(float t) {
    static const uint32_t colors[6] = {
        0xff303030, 0xffc0c0c0, 0xff909090, 0xff707070, 0xffa0a0a0, 0xff606060
    };
    vec3_t c = { 0.42f, -0.35f + 0.03f * sinf(t * 2.0f), 1.35f };
    vec3_t h = { 0.08f, 0.08f, 0.85f };

    pvr_prim(&hdr_op, sizeof(hdr_op));
    box(c, h, 0.15f + 0.05f * sinf(t), colors);
}

static void do_frame(int two_pass, float t) {
    pvr_wait_ready();
    pvr_scene_begin();

    if(two_pass) {
        /* Pass 0: the room, then reset the depth */
        pvr_list_begin(PVR_LIST_OP_POLY);
        room();
        pvr_list_begin(PVR_LIST_TR_POLY);
        poster();
        depth_reset();

        /* Pass 1: the held object, and the text */
        pvr_scene_next_pass();
        pvr_list_begin(PVR_LIST_OP_POLY);
        held_object(t);
        pvr_list_begin(PVR_LIST_TR_POLY);
        draw_text();
    }
    else {
        pvr_list_begin(PVR_LIST_OP_POLY);
        room();
        held_object(t);
        pvr_list_begin(PVR_LIST_TR_POLY);
        poster();
        draw_text();
    }

    pvr_scene_finish();
}

/**** Main *********************************************************/

int main(int argc, char *argv[]) {
    uint32_t pressed;
    int two_pass = 1, autocycle = 1, frame = 0;
    uint64_t next_switch = timer_ms_gettime64() + 5000;
    float t = 0.0f;
    char l1[64], l2[64];

    (void)argc;
    (void)argv;

    printf("multipass_viewmodel: A = 1/2 passes, Y = FSAA, Start = exit\n");

    /* The camera is at the origin, looking down +z: world space is camera
       space. */

    for(;;) {
        pressed = buttons_pressed();

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

        /* Pass 0's translucent list must be presorted, so that the depth
           reset comes after the poster. The setting is kept until changed,
           and pvr_init() resets it. */
        if(setup(two_pass ? 2 : 1) && two_pass)
            pvr_set_pass_presort(0, true);

        if((frame++ % 30) == 0 || pressed) {
            snprintf(l1, sizeof(l1), "%s%s",
                     two_pass ? "2 passes: object in pass 1, depth reset" :
                                "1 pass: object clips into the wall",
                     fsaa ? ", FSAA" : "");
            text_render_time(l2, sizeof(l2));
            text_set(l1, l2);

            if((frame % 60) == 1)
                printf("multipass_viewmodel: %s; %s\n", l1, l2);
        }

        do_frame(two_pass, t);
        t += 1.0f / 60.0f;
    }

    pvr_shutdown();

    return 0;
}
