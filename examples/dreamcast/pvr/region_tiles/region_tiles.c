/* KallistiOS ##version##

   examples/dreamcast/pvr/region_tiles/region_tiles.c
   Copyright (C) 2026 Daniel Fairchild

   Switch single PVR tiles off and on.

   The whole screen animates, with the tile grid drawn over it. A skipped
   tile is left out of the region array: the PVR neither renders nor writes
   it, so it freezes with its last picture (in fact the last two, since KOS
   renders into two frame buffers in turn). Each tile also holds a small
   pair of overlapping translucent squares, sent nearer one first: in an
   autosorted tile the nearer (white) square is on top, in a presorted tile
   the farther (red) one.

   Controls:
     D-pad      move the cursor
     A          skip / render the tile under the cursor
     B          presort / autosort the tile under the cursor
     X          render every tile again, all autosorted
     Y          FSAA on/off
     Start      quit
*/

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <dc/biosfont.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/pvr.h>
#include <dc/video.h>

/* The bottom tile row holds the status text and is always rendered */
#define HUD_Y   448

#define FONT_TEX_W  256
#define FONT_TEX_H  256
#define FONT_CELL_W 16

static pvr_ptr_t font_tex;
static pvr_poly_hdr_t font_hdr, op_hdr, tr_hdr, pt_hdr;
static float xs = 1.0f;     /* Render coordinates per screen pixel across */

static int tile_cols, tile_rows, tile_px;
static bool skipped[64][64], presorted[64][64];
static int cur_x, cur_y;
static bool fsaa;

static void font_init(void) {
    uint16_t *buf = calloc(FONT_TEX_W * FONT_TEX_H, sizeof(uint16_t));
    pvr_poly_cxt_t cxt;
    int c, i;

    for(c = 33; c < 127; c++) {
        i = c - 32;
        bfont_draw_ex(buf + (i / 16) * BFONT_HEIGHT * FONT_TEX_W +
                      (i % 16) * FONT_CELL_W, FONT_TEX_W, 0xffff, 0x0000, 16,
                      false, c, false, false);
    }

    font_tex = pvr_mem_malloc(FONT_TEX_W * FONT_TEX_H * 2);
    pvr_txr_load(buf, font_tex, FONT_TEX_W * FONT_TEX_H * 2);
    free(buf);

    pvr_poly_cxt_txr(&cxt, PVR_LIST_PT_POLY,
                     PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED,
                     FONT_TEX_W, FONT_TEX_H, font_tex, PVR_FILTER_NONE);
    pvr_poly_compile(&font_hdr, &cxt);
}

static void vert(float x, float y, float z, uint32_t argb, float u, float v,
                 bool eol) {
    pvr_vertex_t vx;

    vx.flags = eol ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    vx.x = x * xs;
    vx.y = y;
    vx.z = z;
    vx.u = u;
    vx.v = v;
    vx.argb = argb;
    vx.oargb = 0;
    pvr_prim(&vx, sizeof(vx));
}

static void quad(float x0, float y0, float x1, float y1, float z,
                 uint32_t argb) {
    vert(x0, y1, z, argb, 0, 0, false);
    vert(x0, y0, z, argb, 0, 0, false);
    vert(x1, y1, z, argb, 0, 0, false);
    vert(x1, y0, z, argb, 0, 0, true);
}

static void text_printf(float x, float y, uint32_t argb, const char *fmt, ...) {
    char buf[96], *s;
    va_list args;
    float u, v;
    int i;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    for(s = buf; *s; s++, x += BFONT_THIN_WIDTH) {
        i = (unsigned char)*s - 32;

        if(i <= 0 || i >= 95)
            continue;

        u = (float)((i % 16) * FONT_CELL_W) / FONT_TEX_W;
        v = (float)((i / 16) * BFONT_HEIGHT) / FONT_TEX_H;
        vert(x, y + BFONT_HEIGHT, 100.0f, argb, u, v + (float)BFONT_HEIGHT / FONT_TEX_H, false);
        vert(x, y, 100.0f, argb, u, v, false);
        vert(x + BFONT_THIN_WIDTH, y + BFONT_HEIGHT, 100.0f, argb,
             u + (float)BFONT_THIN_WIDTH / FONT_TEX_W,
             v + (float)BFONT_HEIGHT / FONT_TEX_H, false);
        vert(x + BFONT_THIN_WIDTH, y, 100.0f, argb,
             u + (float)BFONT_THIN_WIDTH / FONT_TEX_W, v, true);
    }
}

static void setup(void) {
    pvr_init_params_t params = {
        { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0,
          PVR_BINSIZE_16 },
        512 * 1024, 0, 0, 0, 3, 0
    };
    pvr_poly_cxt_t cxt;

    params.fsaa_enabled = fsaa;
    pvr_init(&params);
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    xs = fsaa ? 2.0f : 1.0f;

    pvr_get_tile_grid(&tile_cols, &tile_rows);
    tile_px = 640 / tile_cols;

    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    pvr_poly_compile(&op_hdr, &cxt);
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(&tr_hdr, &cxt);
    pvr_poly_cxt_col(&cxt, PVR_LIST_PT_POLY);
    pvr_poly_compile(&pt_hdr, &cxt);
    font_init();

    /* The table starts out with every tile rendered and autosorted */
    for(cur_y = 0; cur_y < 64; cur_y++)
        for(cur_x = 0; cur_x < 64; cur_x++)
            skipped[cur_y][cur_x] = presorted[cur_y][cur_x] = false;

    cur_x = tile_cols / 2;
    cur_y = tile_rows / 2;
}

static void draw(float t) {
    pvr_stats_t stats;
    int x, y;
    float px, py, s;

    pvr_wait_ready();
    pvr_scene_begin();

    /* Moving stripes over the whole screen */
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&op_hdr, sizeof(op_hdr));

    for(y = 0; y < HUD_Y; y += 16) {
        s = sinf(t * 2.0f + y * 0.02f);
        vert(0, y + 16, 1.0f, 0xff202060, 0, 0, false);
        vert(0, y, 1.0f, 0xff4040a0, 0, 0, false);
        vert(320 + 300 * s, y + 16, 1.0f, 0xff20a040, 0, 0, false);
        vert(320 + 300 * s, y, 1.0f, 0xff60e080, 0, 0, false);
        vert(640, y + 16, 1.0f, 0xffa04020, 0, 0, false);
        vert(640, y, 1.0f, 0xffe08040, 0, 0, true);
    }

    quad(0, HUD_Y, 640, 480, 1.0f, 0xff000000);
    pvr_list_finish();

    /* In every tile, a white square in front of a red one, sent first */
    pvr_list_begin(PVR_LIST_TR_POLY);
    pvr_prim(&tr_hdr, sizeof(tr_hdr));

    for(y = 0; y < HUD_Y / 32; y++) {
        for(x = 0; x < tile_cols; x++) {
            px = x * tile_px + tile_px / 2.0f;
            py = y * 32 + 16;
            s = tile_px / 4.0f;
            quad(px - s, py - 8, px + s / 2, py + 4, 5.0f, 0xc0ffffff);
            quad(px - s / 2, py - 4, px + s, py + 8, 4.0f, 0xc0ff2020);
        }
    }

    pvr_list_finish();

    /* Grid, cursor and status */
    pvr_list_begin(PVR_LIST_PT_POLY);
    pvr_prim(&pt_hdr, sizeof(pt_hdr));

    for(x = 1; x < tile_cols; x++)
        quad(x * tile_px, 0, x * tile_px + 1, HUD_Y, 50.0f, 0xff000000);

    for(y = 1; y <= HUD_Y / 32; y++)
        quad(0, y * 32, 640, y * 32 + 1, 50.0f, 0xff000000);

    px = cur_x * tile_px;
    py = cur_y * 32;
    quad(px, py, px + tile_px, py + 2, 60.0f, 0xffffff00);
    quad(px, py + 30, px + tile_px, py + 32, 60.0f, 0xffffff00);
    quad(px, py, px + 2, py + 32, 60.0f, 0xffffff00);
    quad(px + tile_px - 2, py, px + tile_px, py + 32, 60.0f, 0xffffff00);

    pvr_prim(&font_hdr, sizeof(font_hdr));
    pvr_get_stats(&stats);
    text_printf(8, HUD_Y + 4, 0xffffffff, "Tile %d,%d %s %s  PVR %.2f ms%s",
                cur_x, cur_y,
                skipped[cur_y][cur_x] ? "SKIPPED" : "rendered",
                presorted[cur_y][cur_x] ? "presort" : "autosort",
                (double)stats.rnd_last_time / 1e6, fsaa ? "  FSAA" : "");
    pvr_list_finish();

    pvr_scene_finish();
}

int main(int argc, char *argv[]) {
    maple_device_t *cont;
    cont_state_t *st;
    uint32_t buttons, prev = 0, pressed;
    float t = 0.0f;
    int x, y;

    (void)argc;
    (void)argv;

    setup();

    for(;;) {
        buttons = 0;

        if((cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER)) &&
           (st = (cont_state_t *)maple_dev_status(cont)))
            buttons = st->buttons;

        pressed = buttons & ~prev;
        prev = buttons;

        if(pressed & CONT_START)
            break;

        /* The cursor stays out of the status row */
        if((pressed & CONT_DPAD_LEFT) && cur_x > 0)
            cur_x--;
        if((pressed & CONT_DPAD_RIGHT) && cur_x < tile_cols - 1)
            cur_x++;
        if((pressed & CONT_DPAD_UP) && cur_y > 0)
            cur_y--;
        if((pressed & CONT_DPAD_DOWN) && cur_y < HUD_Y / 32 - 1)
            cur_y++;

        if(pressed & CONT_A) {
            skipped[cur_y][cur_x] = !skipped[cur_y][cur_x];
            pvr_set_tile_enabled(cur_x, cur_y, !skipped[cur_y][cur_x]);
        }

        if(pressed & CONT_B) {
            presorted[cur_y][cur_x] = !presorted[cur_y][cur_x];
            pvr_set_tile_presort(cur_x, cur_y, presorted[cur_y][cur_x]);
        }

        if(pressed & CONT_X) {
            for(y = 0; y < tile_rows; y++)
                for(x = 0; x < tile_cols; x++)
                    skipped[y][x] = presorted[y][x] = false;

            pvr_set_rect_enabled(0, 0, 640, 480, true);
            pvr_set_presort_mode(false);
        }

        if(pressed & CONT_Y) {
            pvr_wait_ready();
            pvr_shutdown();
            fsaa = !fsaa;
            setup();
        }

        draw(t);
        t += 1.0f / 60.0f;
    }

    pvr_wait_ready();
    pvr_shutdown();

    return 0;
}
