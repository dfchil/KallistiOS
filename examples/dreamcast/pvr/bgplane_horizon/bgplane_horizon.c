/* KallistiOS ##version##

   examples/dreamcast/pvr/bgplane_horizon/bgplane_horizon.c
   Copyright (C) 2026 Daniel Fairchild

   A sky with no sky geometry: a camera with pitch and roll flies over a
   ground grid, and the sky is a gradient on the background plane, light at
   the horizon and darker overhead. The gradient is computed every frame from
   the horizon line, so it tilts and rolls with the camera.

   The background plane is a polygon whose three vertices only define a
   plane; the plane fills the whole screen. So the three vertices can be put
   anywhere, on screen or not: two on the horizon line with the horizon
   color, one "overhead" with the zenith color. See pvr_set_bg_poly().

   Controls:
     Analog stick, D-pad   pitch and roll (switches to manual flight)
     A                     back to automatic flight
     X                     render through a texture (pvr_scene_begin_rtt()),
                           with FSAA off
     Y                     switch FSAA on or off
     Start                 exit
*/

#include <stdio.h>
#include <stdlib.h>
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

#define RTT_W       1024
#define RTT_H       512

#define TEXT_W      512
#define TEXT_H      64

/* Focal length in pixels: about 62 degrees vertical field of view */
#define FOCAL       400.0f

/* Ground grid: cells of CELL units, out to RADIUS cells around the camera */
#define CELL        6.0f
#define RADIUS      20
#define NEAR_Z      0.1f

/* Distance from the horizon line, in pixels, where the sky has the zenith
   color. Larger than any point on screen gets with the pitch limit below, so
   the extrapolated colors stay in range. */
#define ZENITH_DIST 1000.0f
#define MAX_PITCH   (50.0f * F_PI / 180.0f)

#define HORIZON     0xffc8dceb
#define ZENITH      0xff1e3c8c
#define GROUND_A    0xff3c6e32
#define GROUND_B    0xff508c3c

/* The ground has many small polygons in the tiles along the horizon: give
   the opaque list large bins and spare OPBs, so none are dropped. */
static pvr_init_params_t params = {
    { PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0,
      PVR_BINSIZE_0 },
    512 * 1024, 0, 0, 0, 4, 0
};

typedef struct {
    float x, y, z;
} vec3_t;

/* Camera: position, then right / up / forward axes in world space */
static vec3_t cam_pos = { 0.0f, 8.0f, 0.0f };
static vec3_t cam_r, cam_u, cam_f;
static float yaw, pitch, roll;
static bool autopilot = true, rtt = false;

static pvr_ptr_t text_txr[2], rtt_txr;
static pvr_poly_hdr_t text_hdr[2], ground_hdr, rtt_hdr;
static uint16_t text_buf[TEXT_W * TEXT_H];
static int text_cur;
static float render_ms;

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
    pvr_poly_compile(&ground_hdr, &cxt);
}

/* ---- Camera ------------------------------------------------------------ */

static vec3_t rot_x(vec3_t v, float a) {
    return (vec3_t){ v.x, v.y * cosf(a) + v.z * sinf(a),
                     v.z * cosf(a) - v.y * sinf(a) };
}

static vec3_t rot_y(vec3_t v, float a) {
    return (vec3_t){ v.x * cosf(a) + v.z * sinf(a), v.y,
                     v.z * cosf(a) - v.x * sinf(a) };
}

static vec3_t rot_z(vec3_t v, float a) {
    return (vec3_t){ v.x * cosf(a) - v.y * sinf(a),
                     v.x * sinf(a) + v.y * cosf(a), v.z };
}

/* Roll around the view axis, pitch up, then turn to the heading. */
static vec3_t orient(vec3_t v) {
    return rot_y(rot_x(rot_z(v, roll), pitch), yaw);
}

static void update_camera(float t, float stick_x, float stick_y) {
    float speed = 12.0f / 60.0f;

    if(autopilot) {
        pitch = 0.35f * sinf(0.31f * t) + 0.15f * sinf(0.13f * t);
        roll = 0.7f * sinf(0.23f * t) + 0.3f * sinf(0.071f * t);
    }
    else {
        pitch += stick_y * 0.02f;
        roll += stick_x * 0.03f;
    }

    if(pitch > MAX_PITCH)
        pitch = MAX_PITCH;

    if(pitch < -MAX_PITCH)
        pitch = -MAX_PITCH;

    /* A banked camera turns. */
    yaw += sinf(roll) * 0.01f;

    cam_r = orient((vec3_t){ 1.0f, 0.0f, 0.0f });
    cam_u = orient((vec3_t){ 0.0f, 1.0f, 0.0f });
    cam_f = orient((vec3_t){ 0.0f, 0.0f, 1.0f });

    cam_pos.x += cam_f.x * speed;
    cam_pos.z += cam_f.z * speed;
    cam_pos.y += cam_f.y * speed;

    if(cam_pos.y < 2.0f)
        cam_pos.y = 2.0f;

    if(cam_pos.y > 15.0f)
        cam_pos.y = 15.0f;
}

/* ---- Sky --------------------------------------------------------------- */

/* Put the background plane's vertices on the horizon line and overhead. */
static void set_sky(float xs) {
    pvr_bg_vertex_t v[3];
    /* World up in camera space */
    float ux = cam_r.y, uy = cam_u.y, uz = cam_f.y;
    float n = sqrtf(ux * ux + uy * uy);
    float gx, gy, d, x0, y0;
    int i;

    /* A screen point (sx, sy) is at distance
         D = (ux (sx - cx) - uy (sy - cy) + uz F) / n
       above the horizon line, in pixels. (gx, gy) is the direction in which
       D grows, (gy, -gx) runs along the horizon. With the pitch limit, n is
       never close to 0. */
    gx = ux / n;
    gy = -uy / n;
    d = uz * FOCAL / n;

    /* The point of the horizon line closest to the screen center */
    x0 = SCREEN_W / 2 - d * gx;
    y0 = SCREEN_H / 2 - d * gy;

    v[0].x = x0;
    v[0].y = y0;
    v[0].argb = HORIZON;
    v[1].x = x0 - gy * 1000.0f;
    v[1].y = y0 + gx * 1000.0f;
    v[1].argb = HORIZON;
    v[2].x = x0 + gx * ZENITH_DIST;
    v[2].y = y0 + gy * ZENITH_DIST;
    v[2].argb = ZENITH;

    for(i = 0; i < 3; i++) {
        v[i].x *= xs;
        v[i].z = FLT_EPSILON;  /* as the default vertices */
        v[i].u = v[i].v = 0.0f;
    }

    pvr_set_bg_poly(v);
}

/* ---- Ground ------------------------------------------------------------ */

typedef struct {
    float x, y, z;      /* camera space */
    float r, g, b;
} gvert_t;

static uint32_t pack(float r, float g, float b) {
    return 0xff000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void submit(const gvert_t *v, int idx, int last, float xs) {
    pvr_vertex_t pv;

    pv.flags = last ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    pv.x = (SCREEN_W / 2 + FOCAL * v[idx].x / v[idx].z) * xs;
    pv.y = SCREEN_H / 2 - FOCAL * v[idx].y / v[idx].z;
    pv.z = 1.0f / v[idx].z;
    pv.u = pv.v = 0.0f;
    pv.argb = pack(v[idx].r, v[idx].g, v[idx].b);
    pv.oargb = 0;
    pvr_prim(&pv, sizeof(pv));
}

/* Clip a polygon to z > NEAR_Z and draw it. */
static void draw_poly(const gvert_t *in, int n, float xs) {
    gvert_t out[8];
    float t;
    int i, j, k = 0;

    for(i = 0; i < n; i++) {
        const gvert_t *a = &in[i], *b = &in[(i + 1) % n];

        if(a->z > NEAR_Z)
            out[k++] = *a;

        if((a->z > NEAR_Z) != (b->z > NEAR_Z)) {
            t = (NEAR_Z - a->z) / (b->z - a->z);
            out[k].x = a->x + (b->x - a->x) * t;
            out[k].y = a->y + (b->y - a->y) * t;
            out[k].z = NEAR_Z;
            out[k].r = a->r + (b->r - a->r) * t;
            out[k].g = a->g + (b->g - a->g) * t;
            out[k].b = a->b + (b->b - a->b) * t;
            k++;
        }
    }

    if(k < 3)
        return;

    if(k == 4 && n == 4) {
        /* Unclipped quad: one strip */
        submit(out, 0, 0, xs);
        submit(out, 1, 0, xs);
        submit(out, 3, 0, xs);
        submit(out, 2, 1, xs);
        return;
    }

    /* Clipped: a fan of triangles */
    for(j = 1; j + 1 < k; j++) {
        submit(out, 0, 0, xs);
        submit(out, j, 0, xs);
        submit(out, j + 1, 1, xs);
    }
}

/* The ground: a checkerboard fading into the horizon color. */
static void draw_ground(float xs) {
    static const int corner[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    int ci = (int)floorf(cam_pos.x / CELL), cj = (int)floorf(cam_pos.z / CELL);
    int i, j, k, behind;
    gvert_t q[4];
    float wx, wz, dx, dy, dz, dist, fade;
    uint32_t c;

    pvr_prim(&ground_hdr, sizeof(ground_hdr));

    for(j = -RADIUS; j < RADIUS; j++) {
        for(i = -RADIUS; i < RADIUS; i++) {
            if(i * i + j * j > RADIUS * RADIUS)
                continue;

            c = ((ci + i + cj + j) & 1) ? GROUND_A : GROUND_B;
            behind = 0;

            for(k = 0; k < 4; k++) {
                wx = (ci + i + corner[k][0]) * CELL;
                wz = (cj + j + corner[k][1]) * CELL;
                dx = wx - cam_pos.x;
                dy = -cam_pos.y;
                dz = wz - cam_pos.z;

                q[k].x = dx * cam_r.x + dy * cam_r.y + dz * cam_r.z;
                q[k].y = dx * cam_u.x + dy * cam_u.y + dz * cam_u.z;
                q[k].z = dx * cam_f.x + dy * cam_f.y + dz * cam_f.z;
                behind += q[k].z <= NEAR_Z;

                dist = sqrtf(dx * dx + dz * dz) / (RADIUS * CELL);
                fade = dist > 1.0f ? 1.0f : dist * sqrtf(dist);
                q[k].r = (c >> 16 & 0xff) + (float)((int)(HORIZON >> 16 & 0xff)
                         - (int)(c >> 16 & 0xff)) * fade;
                q[k].g = (c >> 8 & 0xff) + (float)((int)(HORIZON >> 8 & 0xff)
                         - (int)(c >> 8 & 0xff)) * fade;
                q[k].b = (c & 0xff) + (float)((int)(HORIZON & 0xff)
                         - (int)(c & 0xff)) * fade;
            }

            if(behind < 4)
                draw_poly(q, 4, xs);
        }
    }
}

/* ---- Frame ------------------------------------------------------------- */

static void rect(float x0, float y0, float x1, float y1, float z, float u1,
                 float v1, float xs) {
    const float x[4] = { x0, x1, x0, x1 }, y[4] = { y0, y0, y1, y1 };
    const float u[4] = { 0.0f, u1, 0.0f, u1 }, v[4] = { 0.0f, 0.0f, v1, v1 };
    pvr_vertex_t vert;
    int i;

    for(i = 0; i < 4; i++) {
        vert.flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        vert.x = x[i] * xs;
        vert.y = y[i];
        vert.z = z;
        vert.u = u[i];
        vert.v = v[i];
        vert.argb = 0xffffffff;
        vert.oargb = 0;
        pvr_prim(&vert, sizeof(vert));
    }
}

/* Draw the text lines in str (separated by '\n'). Translucent list must be
   open. */
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

static void draw_frame(void) {
    float xs = params.fsaa_enabled ? 2.0f : 1.0f;
    float ms;
    char str[128];

    snprintf(str, sizeof(str), "pitch %3d roll %4d%s%s  render %.3f ms\n"
             "stick fly  A %s  X rtt  Y fsaa",
             (int)(pitch * 180.0f / F_PI), (int)(roll * 180.0f / F_PI),
             rtt ? "  rtt" : "", params.fsaa_enabled ? "  fsaa" : "",
             (double)render_ms, autopilot ? "(auto)" : "auto");

    pvr_wait_ready();

    if(rtt) {
        /* The sky for the render-to-texture area, which here has the size of
           the screen, without FSAA. */
        set_sky(1.0f);
        pvr_scene_begin_rtt(rtt_txr, SCREEN_W, SCREEN_H, RTT_W);
        pvr_list_begin(PVR_LIST_OP_POLY);
        draw_ground(1.0f);
        pvr_list_finish();
        ms = finish_and_time();

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
        set_sky(xs);
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);
        draw_ground(xs);
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
    float t = 0.0f, sx, sy;

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
        sx = sy = 0.0f;

        if(buttons & CONT_START)
            break;

        if(state) {
            if(abs(state->joyx) > 16)
                sx = state->joyx / 128.0f;

            if(abs(state->joyy) > 16)
                sy = state->joyy / 128.0f;
        }

        if(buttons & CONT_DPAD_LEFT)
            sx = -1.0f;

        if(buttons & CONT_DPAD_RIGHT)
            sx = 1.0f;

        if(buttons & CONT_DPAD_UP)
            sy = -1.0f;

        if(buttons & CONT_DPAD_DOWN)
            sy = 1.0f;

        if(sx != 0.0f || sy != 0.0f)
            autopilot = false;

        if(pressed & CONT_A)
            autopilot = true;

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

        /* Stick down pulls the nose up, like a plane. */
        update_camera(t, sx, sy);
        draw_frame();
        t += 1.0f / 60.0f;
    }

    pvr_shutdown();

    return 0;
}
