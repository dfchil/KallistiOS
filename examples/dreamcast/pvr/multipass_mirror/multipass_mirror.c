/* KallistiOS ##version##

   examples/dreamcast/pvr/multipass_mirror/multipass_mirror.c

   A rear-view mirror drawn in a second render pass (pvr_init_params_t::
   pass_count).

   Pass 0 draws the view ahead: road, posts, and translucent banners over
   the road. Pass 1 draws the mirror. It starts with an opaque rectangle
   over the mirror area, with depth compare "always" and the farthest
   depth: that gives the mirror its backdrop and clears the depth there.
   Then it draws the scene seen from a camera looking back, with its own
   opaque and translucent lists. The mirror rectangle is aligned to 32-pixel
   tiles, and the mirror's polygons use user clipping (PVR_USERCLIP_INSIDE)
   to stay inside it.

   With one pass, the same polygons share one set of lists: the opaque
   ones still work, since the opaque list is drawn in submission order,
   but the banners of the view ahead are drawn over the mirror, and the
   translucent polygons of both views are sorted together.

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
    pvr_set_bg_color(0.5f, 0.7f, 0.95f);

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

/* The mirror, in tiles (inclusive) and in pixels */
#define MIRROR_TX0  7
#define MIRROR_TY0  3
#define MIRROR_TX1  12
#define MIRROR_TY1  5
#define MIRROR_X0   (MIRROR_TX0 * 32)
#define MIRROR_Y0   (MIRROR_TY0 * 32)
#define MIRROR_X1   ((MIRROR_TX1 + 1) * 32)
#define MIRROR_Y1   ((MIRROR_TY1 + 1) * 32)

static pvr_poly_hdr_t hdr_op, hdr_tr, hdr_backdrop, hdr_op_clip, hdr_tr_clip;

/* User clip area, in tiles */
typedef struct {
    uint32_t cmd;
    uint32_t pad[3];
    uint32_t x0, y0, x1, y1;
} __attribute__((aligned(32))) user_clip_t;

static void compile_headers(void) {
    pvr_poly_cxt_t cxt;

    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr_op, &cxt);

    cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
    pvr_poly_compile(&hdr_op_clip, &cxt);

    /* Mirror backdrop: drawn whatever the depth, writes the farthest depth */
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    pvr_poly_compile(&hdr_backdrop, &cxt);

    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(&hdr_tr, &cxt);

    cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
    pvr_poly_compile(&hdr_tr_clip, &cxt);
}

/* Send the mirror's user clip area to the open list */
static void user_clip(void) {
    user_clip_t uc;
    int f = fsaa ? 2 : 1;

    memset(&uc, 0, sizeof(uc));
    uc.cmd = PVR_CMD_USERCLIP;
    uc.x0 = MIRROR_TX0 * f;
    uc.y0 = MIRROR_TY0;
    uc.x1 = MIRROR_TX1 * f + f - 1;
    uc.y1 = MIRROR_TY1;
    pvr_prim(&uc, sizeof(uc));
}

/* Is a world-space point in front of the camera? */
static int visible(vec3_t p) {
    return to_camera(p).z > 0.5f;
}

/* Road, posts and banners from z0 to z1 (z0 < z1), as seen from the current
   camera. The road starts at the camera, the rest is skipped when behind. */
static void scene_op(float z0, float z1) {
    static const uint32_t colors[6] = {
        0xff303030, 0xffffffff, 0xffd0d0d0, 0xffe02020, 0xffd0d0d0, 0xffe02020
    };
    vec3_t h = { 0.15f, 0.8f, 0.15f };
    float z;

    {
        vec3_t a = { -4, 0, z0 }, b = { -4, 0, z1 }, c = { 4, 0, z1 },
               d = { 4, 0, z0 };
        vec3_t e = { -0.1f, 0.01f, z0 }, f = { -0.1f, 0.01f, z1 },
               g = { 0.1f, 0.01f, z1 }, k = { 0.1f, 0.01f, z0 };

        quad3(a, b, c, d, 0xff505058);
        quad3(e, f, g, k, 0xfff0f0f0);
    }

    for(z = ceilf(z0 / 4.0f) * 4.0f; z < z1; z += 4.0f) {
        vec3_t l = { -4.5f, 0.8f, z }, r = { 4.5f, 0.8f, z };

        if(!visible(l))
            continue;

        box(l, h, 0.0f, colors);
        box(r, h, 0.0f, colors);
    }
}

static void scene_tr(float z0, float z1) {
    float z;

    for(z = ceilf(z0 / 20.0f) * 20.0f + 10.0f; z < z1; z += 20.0f) {
        vec3_t a = { -4.5f, 2.0f, z }, b = { -4.5f, 4.0f, z },
               c = { 4.5f, 4.0f, z }, d = { 4.5f, 2.0f, z };

        if(!visible(a))
            continue;

        quad3(a, b, c, d, 0xa0ffa000);
    }
}

/* Cameras: ahead (full screen) and back (mirror, flipped left to right) */
static float drive_z;

static void camera_ahead(void) {
    cam.x = 0.8f; cam.y = 1.2f; cam.z = drive_z;
    cam_yaw = 0.0f;
    cam_pitch = 0.05f;
    proj_cx = 320.0f; proj_cy = 240.0f; proj_fx = 400.0f; proj_fy = 400.0f;
}

static void camera_back(void) {
    cam.x = 0.8f; cam.y = 1.2f; cam.z = drive_z;
    cam_yaw = (float)M_PI;
    cam_pitch = 0.05f;
    proj_cx = (MIRROR_X0 + MIRROR_X1) / 2.0f;
    proj_cy = (MIRROR_Y0 + MIRROR_Y1) / 2.0f;
    proj_fx = -150.0f; proj_fy = 150.0f;
}

static void ahead_op(void) {
    camera_ahead();
    pvr_prim(&hdr_op, sizeof(hdr_op));
    scene_op(drive_z + 0.3f, drive_z + 80.0f);
}

static void ahead_tr(void) {
    camera_ahead();
    pvr_prim(&hdr_tr, sizeof(hdr_tr));
    scene_tr(drive_z, drive_z + 80.0f);
}

static void mirror_op(void) {
    /* Backdrop and depth clear */
    pvr_prim(&hdr_backdrop, sizeof(hdr_backdrop));
    rect(MIRROR_X0, MIRROR_Y0, MIRROR_X1, MIRROR_Y1, 0.00001f, 0xff6080c0);

    /* The view back, clipped to the mirror */
    camera_back();
    user_clip();
    pvr_prim(&hdr_op_clip, sizeof(hdr_op_clip));
    scene_op(drive_z - 60.0f, drive_z - 0.3f);

    /* Frame, at the nearest depth */
    pvr_prim(&hdr_op, sizeof(hdr_op));
    rect(MIRROR_X0 - 6, MIRROR_Y0 - 6, MIRROR_X1 + 6, MIRROR_Y0, 100.0f, 0xff101010);
    rect(MIRROR_X0 - 6, MIRROR_Y1, MIRROR_X1 + 6, MIRROR_Y1 + 6, 100.0f, 0xff101010);
    rect(MIRROR_X0 - 6, MIRROR_Y0, MIRROR_X0, MIRROR_Y1, 100.0f, 0xff101010);
    rect(MIRROR_X1, MIRROR_Y0, MIRROR_X1 + 6, MIRROR_Y1, 100.0f, 0xff101010);
}

static void mirror_tr(void) {
    camera_back();
    user_clip();
    pvr_prim(&hdr_tr_clip, sizeof(hdr_tr_clip));
    scene_tr(drive_z - 60.0f, drive_z);
}

static void do_frame(int two_pass) {
    pvr_wait_ready();
    pvr_scene_begin();

    if(two_pass) {
        /* Pass 0: the view ahead */
        pvr_list_begin(PVR_LIST_OP_POLY);
        ahead_op();
        pvr_list_begin(PVR_LIST_TR_POLY);
        ahead_tr();

        /* Pass 1: the mirror, and the text */
        pvr_scene_next_pass();
        pvr_list_begin(PVR_LIST_OP_POLY);
        mirror_op();
        pvr_list_begin(PVR_LIST_TR_POLY);
        mirror_tr();
        draw_text();
    }
    else {
        pvr_list_begin(PVR_LIST_OP_POLY);
        ahead_op();
        mirror_op();
        pvr_list_begin(PVR_LIST_TR_POLY);
        ahead_tr();
        mirror_tr();
        draw_text();
    }

    pvr_scene_finish();
}

/**** Main *********************************************************/

int main(int argc, char *argv[]) {
    uint32_t pressed;
    int two_pass = 1, autocycle = 1, frame = 0;
    uint64_t next_switch = timer_ms_gettime64() + 5000;
    char l1[64], l2[64];

    (void)argc;
    (void)argv;

    printf("multipass_mirror: A = 1/2 passes, Y = FSAA, Start = exit\n");

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

        setup(two_pass ? 2 : 1);

        if((frame++ % 30) == 0 || pressed) {
            snprintf(l1, sizeof(l1), "%s%s",
                     two_pass ? "2 passes: mirror in pass 1" :
                                "1 pass: banners over the mirror",
                     fsaa ? ", FSAA" : "");
            text_render_time(l2, sizeof(l2));
            text_set(l1, l2);

            if((frame % 60) == 1)
                printf("multipass_mirror: %s; %s\n", l1, l2);
        }

        do_frame(two_pass);
        drive_z += 0.25f;
    }

    pvr_shutdown();

    return 0;
}
