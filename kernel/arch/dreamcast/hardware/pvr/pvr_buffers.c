/* KallistiOS ##version##

   pvr_buffers.c
   Copyright (C) 2002, 2004 Megan Potter
   Copyright (C) 2014 Lawrence Sebald

 */

#include <assert.h>
#include <stdio.h>
#include <dc/pvr.h>
#include <dc/video.h>
#include <kos/regfield.h>

#include "pvr_internal.h"

/*

  This module handles buffer allocation for the structures that the
  TA feed into, and which the ISP/TSP read from during the scene
  rendering.

*/


/* There's quite a bit of byte vs word conversion in this file
 * these macros just help make that more readable */
#define BYTES_TO_WORDS(x) ((x) >> 2)
#define WORDS_TO_BYTES(x) ((x) << 2)

#define LIST_ENABLED(i) (pvr_state.lists_enabled & BIT(i))

uint32_t pvr_ra_pass_flags(int p, int pass_count, bool presort) {
    uint32_t flags = presort ? PVR_RA_PRESORT : 0;

    /* Every pass after the first keeps the depth and colour of the tile, and
       every pass before the last keeps the tile on chip. */
    if(p > 0)
        flags |= PVR_RA_ZKEEP;

    if(p < pass_count - 1)
        flags |= PVR_RA_NO_WRITEOUT;

    return flags;
}

size_t pvr_ra_write(uint32_t tile_matrix, int tw, int th,
                    const int bin_size[PVR_OPB_COUNT],
                    const pvr_ra_pass_t *passes, int pass_count,
                    const volatile uint64_t *skipped) {
    uint32_t *vr, *first;   /* Note: We're working in 4-byte pointer maths here */
    const pvr_ra_pass_t *pass;
    uint32_t flags;
    int x, y, p, i, tn;

    vr = (uint32_t *)PVR_RAM_BASE + BYTES_TO_WORDS(tile_matrix - PVR_TILE_MATRIX_HEADER_SIZE);

    /*
        FIXME? Is this header necessary? If we're moving the tilematrix
        register to after it, how does the Dreamcast know this is here?
    */

    /* Header of zeros */
    for(i = 0; i < PVR_TILE_MATRIX_HEADER_SIZE; i += 4)
        *vr++ = 0;

    first = vr;

    /* Initial init tile */
    vr[0] = PVR_RA_NO_WRITEOUT;
    vr[1] = PVR_RA_LIST_EMPTY;
    vr[2] = PVR_RA_LIST_EMPTY;
    vr[3] = PVR_RA_LIST_EMPTY;
    vr[4] = PVR_RA_LIST_EMPTY;
    vr[5] = PVR_RA_LIST_EMPTY;
    vr += PVR_RA_ENTRY_WORDS;

    /*
        This sets up the addresses for each list, for each tile, for each
        pass. If a list isn't enabled, then we set the address to 0x80000000
        which tells the PVR to ignore it.

        Memory for each frame is arranged sort-of like this:

        [vertex_buffer | object pointer buffers | tilematrix header | tile matrix]

        The entries of a tile's passes follow each other, in pass order.
    */
    for(x = 0; x < tw; x++) {
        for(y = 0; y < th; y++) {
            if(skipped && ((skipped[y] >> x) & 1))
                continue;

            tn = (tw * y) + x;

            for(p = 0; p < pass_count; p++) {
                pass = passes + p;
                flags = pass->flags & PVR_RA_FLAGS_MASK;

                if(pass->presort && ((pass->presort[y] >> x) & 1))
                    flags |= PVR_RA_PRESORT;

                /* Control word */
                vr[0] = PVR_RA_TILE_Y(y) | PVR_RA_TILE_X(x) | flags;

                /* Opaque poly, opaque volume mod, translucent poly,
                   translucent volume mod and punch-thru poly buffers */
                for(i = 0; i < PVR_OPB_COUNT; i++) {
                    if(pass->opb[i] & PVR_RA_LIST_EMPTY)
                        vr[1 + i] = PVR_RA_LIST_EMPTY;
                    else
                        vr[1 + i] = pass->opb[i] + (bin_size[i] * tn);
                }

                vr += PVR_RA_ENTRY_WORDS;
            }
        }
    }

    /* The hardware never finishes a render whose last entry has
       PVR_RA_NO_WRITEOUT set, as the dummy entry has. With every tile
       skipped, the array is marked empty and the render isn't started at
       all (see pvr_render_lists()). */
    vr[-PVR_RA_ENTRY_WORDS] |= PVR_RA_LAST;

    return WORDS_TO_BYTES(vr - first);
}

/* Bit mask of tile columns x0 to x1 (inclusive) in one row of the per-tile
   tables. */
static inline uint64_t pvr_tile_cols(int x0, int x1) {
    uint64_t hi = (x1 >= 63) ? ~0ULL : ((1ULL << (x1 + 1)) - 1);

    return hi & ~((1ULL << x0) - 1);
}

/* (Re)build the region array of TA buffer which from the per-tile tables.

   If skip is true, tiles marked in tile_skipped are left out in every pass,
   so the PVR neither renders them nor writes them to the frame buffer. */
static void pvr_build_tile_matrix(int which, bool skip) {
    volatile pvr_ta_buffers_t *buf;
    pvr_ra_pass_t passes[PVR_MAX_PASSES];
    int bin_size[PVR_OPB_COUNT];
    size_t size;
    int p, i, y;

    buf = pvr_state.ta_buffers + which;

    for(i = 0; i < PVR_OPB_COUNT; i++)
        bin_size[i] = pvr_state.opb_size[i];

    /* Each pass has its own first object pointer blocks, one pass after the
       other in the OPB area. */
    for(p = 0; p < pvr_state.pass_count; p++) {
        passes[p].flags = pvr_ra_pass_flags(p, pvr_state.pass_count, false);
        passes[p].presort = pvr_state.tile_presort[p];

        for(i = 0; i < PVR_OPB_COUNT; i++)
            passes[p].opb[i] = LIST_ENABLED(i) ?
                buf->opb_addresses[i] + buf->opb_size * p : PVR_RA_LIST_EMPTY;
    }

    size = pvr_ra_write(buf->tile_matrix, pvr_state.tw, pvr_state.th, bin_size,
                        passes, pvr_state.pass_count,
                        skip ? pvr_state.tile_skipped : NULL);

    buf->tile_matrix_empty = (size == PVR_RA_SIZE(0, 0, 0));

    /* Remember what this array was built from */
    for(y = 0; y < pvr_state.th; y++) {
        for(p = 0; p < pvr_state.pass_count; p++)
            buf->tile_matrix_presort[p][y] = pvr_state.tile_presort[p][y];

        buf->tile_matrix_skipped[y] = pvr_state.tile_skipped[y];
    }

    buf->tile_matrix_skips = skip;
}

/* Does the region array of TA buffer which match the per-tile state? */
static bool pvr_tile_matrix_current(int which, bool skip) {
    volatile pvr_ta_buffers_t *buf = pvr_state.ta_buffers + which;
    int y, p;

    if(buf->tile_matrix_skips != skip)
        return false;

    for(y = 0; y < pvr_state.th; y++) {
        for(p = 0; p < pvr_state.pass_count; p++)
            if(buf->tile_matrix_presort[p][y] != pvr_state.tile_presort[p][y])
                return false;

        if(skip && buf->tile_matrix_skipped[y] != pvr_state.tile_skipped[y])
            return false;
    }

    return true;
}

/* Fill all tile matrices */
void pvr_init_tile_matrices(bool presort) {
    int i, p;

    /* Every tile rendered, every tile of every pass in the same sort mode */
    for(i = 0; i < PVR_TILE_MAX; i++) {
        for(p = 0; p < PVR_MAX_PASSES; p++)
            pvr_state.tile_presort[p][i] = presort ? ~0ULL : 0;

        pvr_state.tile_skipped[i] = 0;
    }

    pvr_state.tiles_skipped = 0;

    for(i = 0; i < 2; i++)
        pvr_build_tile_matrix(i, false);
}

/* Bring the region array of the TA buffer about to be filled up to date.
   Called once per scene, when the TA buffer is known to be free (no render
   reads it), and after curr_to_texture is set for the scene. */
void pvr_update_tile_matrix(void) {
    bool skip = !pvr_state.curr_to_texture && pvr_state.tiles_skipped;

    if(!pvr_tile_matrix_current(pvr_state.ta_target, skip))
        pvr_build_tile_matrix(pvr_state.ta_target, skip);
}

/* Set or clear a rectangle of tiles (inclusive tile coordinates, already
   clipped to the screen) in one of the per-tile tables. The region arrays
   are compared against the tables when a scene starts, so setting a state
   and setting it back before then costs nothing. */
static void pvr_tile_table_set(volatile uint64_t *table, int tx0, int ty0,
                               int tx1, int ty1, bool set) {
    uint64_t mask = pvr_tile_cols(tx0, tx1);
    int y, n;

    for(y = ty0; y <= ty1; y++)
        table[y] = set ? (table[y] | mask) : (table[y] & ~mask);

    if(table == pvr_state.tile_skipped) {
        for(y = 0, n = 0; y < pvr_state.th; y++)
            n += __builtin_popcountll(pvr_state.tile_skipped[y]);

        pvr_state.tiles_skipped = n;
    }
}

/* Set the sort mode of a rectangle of tiles in every pass. */
static void pvr_tile_presort_set(int tx0, int ty0, int tx1, int ty1,
                                 bool presort) {
    int p;

    for(p = 0; p < pvr_state.pass_count; p++)
        pvr_tile_table_set(pvr_state.tile_presort[p], tx0, ty0, tx1, ty1,
                           presort);
}

/* Clip a rectangle in tiles to the screen. Returns false if nothing is
   left. */
static bool pvr_tile_clip(int *tx0, int *ty0, int *tx1, int *ty1) {
    if(*tx0 < 0) *tx0 = 0;
    if(*ty0 < 0) *ty0 = 0;
    if(*tx1 >= pvr_state.tw) *tx1 = pvr_state.tw - 1;
    if(*ty1 >= pvr_state.th) *ty1 = pvr_state.th - 1;

    return pvr_state.valid && *tx0 <= *tx1 && *ty0 <= *ty1;
}

/* Convert a rectangle in screen pixels to the tiles it touches. With FSAA,
   each tile covers 16 screen pixels horizontally. */
static bool pvr_tile_rect(int x, int y, int w, int h,
                          int *tx0, int *ty0, int *tx1, int *ty1) {
    int tpw = pvr_state.fsaa ? 16 : 32;

    if(w <= 0 || h <= 0)
        return false;

    /* Clip first so that the divisions below never see negative values */
    if(x < 0) { w += x; x = 0; }
    if(y < 0) { h += y; y = 0; }

    if(w <= 0 || h <= 0)
        return false;

    *tx0 = x / tpw;
    *ty0 = y / 32;
    *tx1 = (x + w - 1) / tpw;
    *ty1 = (y + h - 1) / 32;

    return pvr_tile_clip(tx0, ty0, tx1, ty1);
}

int pvr_set_tile_presort(int tx, int ty, bool presort) {
    if(!pvr_state.valid || tx < 0 || ty < 0 ||
       tx >= pvr_state.tw || ty >= pvr_state.th)
        return -1;

    pvr_tile_presort_set(tx, ty, tx, ty, presort);

    return 0;
}

int pvr_set_tile_enabled(int tx, int ty, bool enabled) {
    if(!pvr_state.valid || tx < 0 || ty < 0 ||
       tx >= pvr_state.tw || ty >= pvr_state.th)
        return -1;

    pvr_tile_table_set(pvr_state.tile_skipped, tx, ty, tx, ty, !enabled);

    return 0;
}

int pvr_set_rect_presort(int x, int y, int w, int h, bool presort) {
    int tx0, ty0, tx1, ty1;

    if(!pvr_tile_rect(x, y, w, h, &tx0, &ty0, &tx1, &ty1))
        return -1;

    pvr_tile_presort_set(tx0, ty0, tx1, ty1, presort);

    return 0;
}

int pvr_set_rect_enabled(int x, int y, int w, int h, bool enabled) {
    int tx0, ty0, tx1, ty1;

    if(!pvr_tile_rect(x, y, w, h, &tx0, &ty0, &tx1, &ty1))
        return -1;

    pvr_tile_table_set(pvr_state.tile_skipped, tx0, ty0, tx1, ty1, !enabled);

    return 0;
}

void pvr_get_tile_grid(int *tw, int *th) {
    if(tw)
        *tw = pvr_state.tw;

    if(th)
        *th = pvr_state.th;
}

/* As before per-tile control existed, a change to the sort mode of the whole
   screen or of a pass also applies to the scene being registered (if any),
   so rewrite the array of the TA buffer being filled right away. The other
   one follows at its next scene. */
static void pvr_apply_presort(void) {
    bool skip = !(pvr_state.ta_checked_ready ? pvr_state.curr_to_texture :
                  pvr_state.next_to_texture) && pvr_state.tiles_skipped;

    if(pvr_tile_matrix_current(pvr_state.ta_target, skip))
        return;

    if(__predict_false(!pvr_state.vbuf_doublebuf))
            pvr_wait_render_done();

    pvr_build_tile_matrix(pvr_state.ta_target, skip);
}

void pvr_set_presort_mode(bool presort) {
    if(!pvr_state.valid)
        return;

    pvr_tile_presort_set(0, 0, pvr_state.tw - 1, pvr_state.th - 1, presort);
    pvr_apply_presort();
}

int pvr_set_pass_presort(int pass, bool presort) {
    if(!pvr_state.valid || pass < 0 || pass >= pvr_state.pass_count)
        return -1;

    pvr_tile_table_set(pvr_state.tile_presort[pass], 0, 0, pvr_state.tw - 1,
                       pvr_state.th - 1, presort);
    pvr_apply_presort();

    return 0;
}

/* Allocate PVR buffers given a set of parameters

There's some confusion in here that is explained more fully in pvr_internal.h.

The other confusing thing is that texture ram is a 64-bit multiplexed space
rather than a copy of the flat 32-bit VRAM. So in order to maximize the
available texture RAM, the PVR structures for the two frames are broken
up and placed at 0x000000 and 0x400000.

*/
void pvr_allocate_buffers(const pvr_init_params_t *params) {
    volatile pvr_ta_buffers_t   *buf;
    volatile pvr_frame_buffers_t    *fbuf;
    int i, j;
    uint32_t  outaddr, sconst, opb_size_accum, opb_total_size;

    /* Set screen sizes; pvr_init has ensured that we have a valid mode
       and all that by now, so we can freely dig into the vid_mode
       structure here. */
    pvr_state.w = vid_mode->width;
    pvr_state.h = vid_mode->height;
    pvr_state.tw = pvr_state.w / 32;
    pvr_state.th = pvr_state.h / 32;

    /* FSAA -> double the tile buffer width */
    if(pvr_state.fsaa)
        pvr_state.tw *= 2;

    /* We can actually handle non-mod-32 heights pretty easily -- just extend
       the frame buffer a bit, but use a pixel clip for the real mode. */
    if(!__is_aligned(pvr_state.h, 32)) {
        pvr_state.h = (pvr_state.h + 32) & ~31;
        pvr_state.th++;
    }

    /* The region array addresses tiles with 6-bit coordinates */
    assert(pvr_state.tw <= PVR_TILE_MAX && pvr_state.th <= PVR_TILE_MAX);

    pvr_state.tsize_const = ((pvr_state.th - 1) << 16)
                            | ((pvr_state.tw - 1) << 0);

    /* Set clipping parameters */
    pvr_state.zclip = 0.0001f;
    pvr_state.pclip_left = 0;
    pvr_state.pclip_right = vid_mode->width - 1;
    pvr_state.pclip_top = 0;
    pvr_state.pclip_bottom = vid_mode->height - 1;
    pvr_state.pclip_x = (pvr_state.pclip_right << 16) | (pvr_state.pclip_left);
    pvr_state.pclip_y = (pvr_state.pclip_bottom << 16) | (pvr_state.pclip_top);

    /* Look at active lists and figure out how much to allocate
       for each poly type */
    opb_total_size = 0;

    /* Previously, we specified BIT(20) to say that the OPB grows "down" when
       the TA needs more than one. To make it grow "up" instead (increasing addresses),
       we set 0 as the default value.
     */
#if 0
    pvr_state.list_reg_mask = BIT(20);
#else
    pvr_state.list_reg_mask = 0;
#endif

    for(i = 0; i < PVR_OPB_COUNT; i++) {
        pvr_state.opb_size[i] = WORDS_TO_BYTES(params->opb_sizes[i]);   /* in bytes */

        /* Calculate the total size of the OPBs for this list */
        opb_total_size += pvr_state.opb_size[i] * pvr_state.tw * pvr_state.th;

        switch(params->opb_sizes[i]) {
            case PVR_BINSIZE_0:
                sconst = 0;
                break;
            case PVR_BINSIZE_8:
                sconst = 1;
                break;
            case PVR_BINSIZE_16:
                sconst = 2;
                break;
            case PVR_BINSIZE_32:
                sconst = 3;
                break;
            default:
                assert_msg(0, "invalid poly_buf_size");
                sconst = 2;
                break;
        }

        if(sconst > 0) {
            pvr_state.lists_enabled |= BIT(i);
            pvr_state.list_reg_mask |= sconst << (4 * i);
        }
    }

    /* Initialize each buffer set */
    for(i = 0; i < 2; i++) {
        /* Frame 0 goes at 0, Frame 1 goes at 0x400000 (half way) */
        if(i == 0)
            outaddr = 0;
        else
            outaddr = 0x400000;

        /* Select a pvr_buffers_t. Note that there's no good reason
           to allocate the frame buffers at the same time as the TA
           buffers except that it's handy to do it all in one place. */
        buf = pvr_state.ta_buffers + i;
        fbuf = pvr_state.frame_buffers + i;

        /* Vertex buffer */
        buf->vertex = outaddr;
        buf->vertex_size = params->vertex_buf_size;
        outaddr += buf->vertex_size;
        /* N-byte align */
        outaddr = __align_up(outaddr, 128);

        /* Object Pointer Blocks */
        buf->opb = outaddr;
        buf->opb_size = opb_total_size;

        /* One set of first OPBs per render pass, then extra space for
           overflow (when one OPB isn't big enough). */
        buf->opb_overflow_count = params->opb_overflow_count;
        outaddr += opb_total_size * (pvr_state.pass_count + buf->opb_overflow_count);

        /* Set up the opb pointers to each section */
        opb_size_accum = 0;
        for(j = 0; j < PVR_OPB_COUNT; j++) {
            buf->opb_addresses[j] = buf->opb + opb_size_accum;
            opb_size_accum += pvr_state.opb_size[j] * pvr_state.tw * pvr_state.th;
        }

        assert(buf->opb_size == opb_size_accum);

        /* N-byte align */
        outaddr = __align_up(outaddr, 128);

        /* Tile Matrix header */
        outaddr += PVR_TILE_MATRIX_HEADER_SIZE;

        /* Tile Matrix */
        buf->tile_matrix = outaddr;
        buf->tile_matrix_size = PVR_RA_SIZE(pvr_state.tw, pvr_state.th, pvr_state.pass_count);
        outaddr += buf->tile_matrix_size;

        /* N-byte align */
        outaddr = __align_up(outaddr, 128);

        /* Output buffer */
        fbuf->frame = outaddr;
        fbuf->frame_size = pvr_state.w * pvr_state.h * vid_pmode_bpp[vid_mode->pm];
        outaddr += fbuf->frame_size;

        /* N-byte align */
        outaddr = __align_up(outaddr, 128);
    }

    /* Texture ram is whatever is left */
    pvr_state.texture_base = (outaddr - 0x400000) * 2;

#if 0
    dbglog(DBG_KDEBUG, "pvr: initialized PVR buffers:\n");
    dbglog(DBG_KDEBUG, "  texture RAM begins at %08lx\n", pvr_state.texture_base);

    for(i = 0; i < 2; i++) {
        buf = pvr_state.ta_buffers + i;
        fbuf = pvr_state.frame_buffers + i;
        dbglog(DBG_KDEBUG, "  vertex/vertex_size: %08lx/%08lx\n", buf->vertex, buf->vertex_size);
        dbglog(DBG_KDEBUG, "  opb base/opb_size: %08lx/%08lx\n", buf->opb, buf->opb_size);
        dbglog(DBG_KDEBUG, "  opbs per type: %08lx %08lx %08lx %08lx %08lx\n",
               buf->opb_type[0],
               buf->opb_type[1],
               buf->opb_type[2],
               buf->opb_type[3],
               buf->opb_type[4]);
        dbglog(DBG_KDEBUG, "  tile_matrix/tile_matrix_size: %08lx/%08lx\n", buf->tile_matrix, buf->tile_matrix_size);
        dbglog(DBG_KDEBUG, "  frame/frame_size: %08lx/%08lx\n", fbuf->frame, fbuf->frame_size);
    }

    dbglog(DBG_KDEBUG, "  list_mask %08lx\n", pvr_state.list_reg_mask);
    dbglog(DBG_KDEBUG, "  w/h = %d/%d, tw/th = %d/%d\n", pvr_state.w, pvr_state.h,
           pvr_state.tw, pvr_state.th);
    dbglog(DBG_KDEBUG, "  zclip %08lx\n", *((uint32_t *)&pvr_state.zclip));
    dbglog(DBG_KDEBUG, "  pclip_left/right %08lx/%08lx\n", pvr_state.pclip_left, pvr_state.pclip_right);
    dbglog(DBG_KDEBUG, "  pclip_top/bottom %08lx/%08lx\n", pvr_state.pclip_top, pvr_state.pclip_bottom);
    dbglog(DBG_KDEBUG, "  lists_enabled %08lx\n", pvr_state.lists_enabled);
    dbglog(DBG_KDEBUG, "Free texture memory: %ld bytes\n",
           0x800000 - pvr_state.texture_base);
#endif  /* !NDEBUG */
}
