/* KallistiOS ##version##

   examples/dreamcast/pvr/multipass_trdepth/multipass_trdepth.c

   Hardware test: does a translucent polygon without depth write hide the
   polygons of a later pass?

   Pass 0 draws a grey opaque floor at z 1 and a 50% red translucent quad at
   z 5 with depth write off. Pass 1 draws opaque quads: a blue one at z 3
   under the translucent quad, a blue one at z 3 outside it, and a green one
   at z 0.5, behind the floor, as a control.

   Renders that scene with pass 0 autosorted and presorted, each with depth
   write off and on for the translucent quad, reads the pixels back from the
   frame buffer and prints them on the serial console. Each result line
   starts with "HWTEST".

   Result on hardware: after an autosorted translucent list, later passes are
   depth tested against its polygons even with depth write off. A presorted
   list follows the depth write setting.
*/

#include <stdio.h>
#include <stdint.h>

#include <dc/pvr.h>

static pvr_poly_hdr_t hdr_op, hdr_tr_nowrite, hdr_tr_write;
static int tr_write;

static void quad(float x0, float y0, float x1, float y1, float z, uint32_t c) {
    pvr_vertex_t v __attribute__((aligned(32)));

    v.u = v.v = 0;
    v.oargb = 0;
    v.argb = c;
    v.z = z;

    v.flags = PVR_CMD_VERTEX;
    v.x = x0;
    v.y = y1;
    pvr_prim(&v, 32);

    v.y = y0;
    pvr_prim(&v, 32);

    v.x = x1;
    v.y = y1;
    pvr_prim(&v, 32);

    v.flags = PVR_CMD_VERTEX_EOL;
    v.y = y0;
    pvr_prim(&v, 32);
}

static void scene(void) {
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&hdr_op, 32);
    quad(32, 32, 608, 448, 1.0f, 0xff808080);

    pvr_list_begin(PVR_LIST_TR_POLY);
    pvr_prim(tr_write ? &hdr_tr_write : &hdr_tr_nowrite, 32);
    quad(100, 100, 540, 380, 5.0f, 0x80ff0000);

    pvr_scene_next_pass();

    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&hdr_op, 32);
    quad(200, 200, 300, 300, 3.0f, 0xff0000ff);     /* Between floor and TR */
    quad(340, 200, 440, 300, 0.5f, 0xff00ff00);     /* Behind the floor */
    quad(560, 400, 600, 440, 3.0f, 0xff0000ff);     /* Outside the TR quad */
}

static uint32_t rgb565_to_888(uint16_t c) {
    return (((c >> 11) & 31) << 19) | (((c >> 5) & 63) << 10) | ((c & 31) << 3);
}

int main(void) {
    pvr_init_params_t params = {
        .opb_sizes = { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16,
                       PVR_BINSIZE_0, PVR_BINSIZE_0 },
        .vertex_buf_size = 512 * 1024,
        .opb_overflow_count = 3,
        .pass_count = 2,
    };
    pvr_poly_cxt_t cxt;
    int mode, f;

    pvr_init(&params);
    pvr_set_bg_color(0, 0, 0);

    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr_op, &cxt);

    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile(&hdr_tr_nowrite, &cxt);
    cxt.depth.write = PVR_DEPTHWRITE_ENABLE;
    pvr_poly_compile(&hdr_tr_write, &cxt);

    printf("HWTEST multipass_trdepth start\n");

    for(mode = 0; mode < 4; mode++) {
        volatile uint16_t *fb;
        uint32_t addr;

        tr_write = mode >> 1;
        pvr_wait_ready();
        pvr_set_pass_presort(0, mode & 1);

        for(f = 0; f < 4; f++) {
            pvr_wait_ready();
            pvr_scene_begin();
            scene();
            pvr_scene_finish();
        }

        pvr_wait_ready();
        pvr_wait_render_done();
        addr = PVR_GET(PVR_RENDER_ADDR) & 0xffffff;
        fb = (volatile uint16_t *)(PVR_RAM_BASE + addr);

        printf("HWTEST pass 0 %s, TR depth write %s: under-TR quad %06lx "
               "(blue=not occluded, c64142=occluded), behind-floor %06lx, "
               "outside %06lx, tint %06lx\n",
               (mode & 1) ? "presort " : "autosort", tr_write ? "on " : "off",
               rgb565_to_888(fb[250 * 640 + 250]),
               rgb565_to_888(fb[250 * 640 + 390]),
               rgb565_to_888(fb[420 * 640 + 580]),
               rgb565_to_888(fb[150 * 640 + 150]));
    }

    printf("HWTEST done\n");
    pvr_shutdown();

    return 0;
}
