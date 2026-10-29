/* KallistiOS ##version##

   examples/dreamcast/pvr/multipass_decal/multipass_decal.c

   Opaque objects over a translucent decal, with multipass rendering
   (pvr_init_params_t::pass_count).

   A ring of turning boxes stands on a ground plane. Each box casts a shadow
   drawn as a translucent polygon without depth write, at a constant depth
   that is nearer than everything, as a screen-space shadow (for example a
   dither pattern) needs. The boxes must cover the shadows whatever the
   depths. Three ways to draw it:

   - Mode 1, one pass, boxes in the opaque list: the shadows, drawn after all
     opaque polygons and nearer, show through the boxes. Wrong.
   - Mode 2, one pass, presorted translucent list with the shadows first and
     the boxes after them, writing depth: right, but every box pixel goes
     through translucent shading, overdraw included.
   - Mode 3, two passes: shadows in pass 0's translucent list, boxes in
     pass 1's opaque list. Pass 1 is drawn after pass 0 and depth tested
     against pass 0's ground only: right, and the boxes keep the opaque
     list's deferred shading (each pixel shaded once).

   Controls: A cycles the modes (the mode also changes by itself every 5
   seconds until A is pressed), Y toggles FSAA, Start exits. The mode and the
   PVR render time are shown on screen and printed on the serial console.
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
    pvr_set_bg_color(0.45f, 0.6f, 0.85f);

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

/* Projection: screen centre and focal lengths */
static float proj_cx = 320.0f, proj_cy = 240.0f, proj_fx = 400.0f, proj_fy = 400.0f;

/* Screen position and depth (1/z) of a camera-space point */
static void project_cam(vec3_t c, float *sx, float *sy, float *sz) {
    *sx = (proj_cx + proj_fx * c.x / c.z) * xs;
    *sy = proj_cy - proj_fy * c.y / c.z;
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

/**** Scene ********************************************************/

#define NUM_BOXES 16

static pvr_poly_hdr_t hdr_op, hdr_tr, hdr_tr_box;

static void compile_headers(void) {
    pvr_poly_cxt_t cxt;

    /* No culling: the depth buffer sorts the faces out */
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr_op, &cxt);

    /* The shadows: translucent, no depth write */
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(&hdr_tr, &cxt);

    /* Boxes in a presorted translucent list: they write depth, so that
       they hide each other's faces */
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.write = PVR_DEPTHWRITE_ENABLE;
    pvr_poly_compile(&hdr_tr_box, &cxt);
}

static void ground(void) {
    vec3_t g[4] = { { -12, 0, -12 }, { -12, 0, 12 }, { 12, 0, 12 }, { 12, 0, -12 } };

    pvr_prim(&hdr_op, sizeof(hdr_op));
    quad3(g[0], g[1], g[2], g[3], 0xff6a8a5a);
}

static vec3_t box_pos(int i, float t) {
    float a = t * 0.3f + i * (2.0f * (float)M_PI / NUM_BOXES);
    vec3_t p = { 4.5f * cosf(a), 0.6f, 4.5f * sinf(a) };

    return p;
}

static void boxes(float t) {
    static const uint32_t colors[6] = {
        0xff802020, 0xffff6040, 0xffe04030, 0xffb03020, 0xffd05040, 0xff902818
    };
    vec3_t h = { 0.7f, 0.6f, 0.4f };
    int i;

    for(i = 0; i < NUM_BOXES; i++)
        box(box_pos(i, t), h, t + i, colors);
}

/* The shadows: the ground footprint of each box, pushed away from the
   light, projected to the screen and drawn at a constant near depth. */
static void shadows(float t) {
    int i, j;

    pvr_prim(&hdr_tr, sizeof(hdr_tr));

    for(i = 0; i < NUM_BOXES; i++) {
        vec3_t p = box_pos(i, t);
        float a = t + i, co = cosf(a), si = sinf(a);
        static const float corner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
        static const int order[4] = { 0, 1, 3, 2 };

        for(j = 0; j < 4; j++) {
            float x = corner[order[j]][0] * 0.9f, z = corner[order[j]][1] * 0.6f;
            vec3_t g = { p.x + x * co - z * si + 0.6f, 0.0f,
                         p.z + x * si + z * co + 0.4f };
            pvr_vertex_t v __attribute__((aligned(32)));

            v.flags = j == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            project_cam(to_camera(g), &v.x, &v.y, &v.z);
            v.z = 10.0f;    /* Deliberately too near */
            v.u = v.v = 0.0f;
            v.argb = 0x90000000;
            v.oargb = 0;
            pvr_prim(&v, sizeof(v));
        }
    }
}

static void do_frame(int mode, float t) {
    pvr_wait_ready();
    pvr_scene_begin();

    switch(mode) {
        case 0:     /* One pass, boxes opaque */
            pvr_list_begin(PVR_LIST_OP_POLY);
            ground();
            pvr_prim(&hdr_op, sizeof(hdr_op));
            boxes(t);
            pvr_list_begin(PVR_LIST_TR_POLY);
            shadows(t);
            draw_text();
            break;

        case 1:     /* One pass, presorted translucent list */
            pvr_list_begin(PVR_LIST_OP_POLY);
            ground();
            pvr_list_begin(PVR_LIST_TR_POLY);
            shadows(t);
            pvr_prim(&hdr_tr_box, sizeof(hdr_tr_box));
            boxes(t);
            draw_text();
            break;

        case 2:     /* Two passes */
            pvr_list_begin(PVR_LIST_OP_POLY);
            ground();
            pvr_list_begin(PVR_LIST_TR_POLY);
            shadows(t);

            pvr_scene_next_pass();

            pvr_list_begin(PVR_LIST_OP_POLY);
            pvr_prim(&hdr_op, sizeof(hdr_op));
            boxes(t);
            pvr_list_begin(PVR_LIST_TR_POLY);
            draw_text();
            break;
    }

    pvr_scene_finish();
}

/**** Main *********************************************************/

static const char *mode_names[3] = {
    "1 pass, boxes OP: shadows on top",
    "1 pass, boxes in presorted TR",
    "2 passes, boxes in pass-1 OP",
};

int main(int argc, char *argv[]) {
    uint32_t pressed;
    int mode = 2, autocycle = 1, frame = 0, presort = -1;
    uint64_t next_switch = timer_ms_gettime64() + 5000;
    float t = 0.0f;
    char l1[64], l2[64];

    (void)argc;
    (void)argv;

    printf("multipass_decal: A = mode, Y = FSAA, Start = exit\n");

    cam.x = 0.0f; cam.y = 6.0f; cam.z = -11.0f;
    cam_pitch = 0.5f;

    for(;;) {
        pressed = buttons_pressed();

        if(pressed & CONT_START)
            break;

        if(pressed & CONT_A) {
            mode = (mode + 1) % 3;
            autocycle = 0;
        }

        if(pressed & CONT_Y) {
            fsaa = !fsaa;
            presort = -1;
        }

        if(autocycle && timer_ms_gettime64() >= next_switch) {
            mode = (mode + 1) % 3;
            next_switch = timer_ms_gettime64() + 5000;
        }

        if(setup(mode == 2 ? 2 : 1))
            presort = -1;

        /* Set the sort mode once the previous scene has started rendering,
           so that it applies to the next scene. It is kept until changed. */
        if(presort != (mode == 1)) {
            pvr_wait_ready();
            presort = mode == 1;
            pvr_set_presort_mode(presort);
        }

        if((frame++ % 30) == 0 || pressed) {
            snprintf(l1, sizeof(l1), "%d: %s%s", mode + 1, mode_names[mode],
                     fsaa ? ", FSAA" : "");
            text_render_time(l2, sizeof(l2));
            text_set(l1, l2);

            if((frame % 60) == 1)
                printf("multipass_decal: %s; %s\n", l1, l2);
        }

        do_frame(mode, t);
        t += 1.0f / 60.0f;
    }

    pvr_shutdown();

    return 0;
}
