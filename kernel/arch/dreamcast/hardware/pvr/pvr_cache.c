/* KallistiOS ##version##

   pvr_cache.c
   Copyright (C) 2026 Daniel Fairchild

 */

#include <assert.h>
#include <stdlib.h>
#include <dc/pvr.h>
#include <dc/asic.h>
#include <arch/irq.h>
#include <kos/dbglog.h>
#include <kos/genwait.h>
#include <kos/timer.h>

#include "pvr_internal.h"

/*

  Cached object lists: polygon parameters and per-tile object lists that are
  built once and rendered in many frames without the TA.

  Memory. The ISP reads the region array, the object lists and the polygon
  parameters through the 32-bit view of VRAM, so a cache must live in the
  32-bit layout made by pvr_allocate_buffers(), not in pvr_mem (64-bit
  view). pvr_allocate_buffers() reserves the same amount at the end of both
  TA buffer sets, at cache_area and cache_area + 0x400000.

  Parameter base. An object list entry holds the offset, in words, of its
  polygon's parameters from the render's parameter base (PVR_ISP_VERTBUF_ADDR),
  which KOS sets to the TA buffer's vertex area: 0 for buffer 0, 0x400000
  for buffer 1. So every cache has two copies at the same offset in each
  half, and copy h has its offsets relative to h * 0x400000. Copy h is read
  by TA buffer h when the cache is attached to a list slot. Keeping both
  copies within their own half also keeps every offset below 4 MB, so they
  don't depend on how many offset bits the ISP decodes.

  Building with the TA. The TA is pointed at copy 1 and fed with the usual
  list calls. Meanwhile pvr_int_handler() keeps its list-done events away
  from the scene logic. The TA writes each entry's offset relative to its
  own parameter write start (TA_ISP_BASE) rounded down to 1 MB, according to
  lxdream. We check that against what was actually written, accept the
  unrounded base as well, rebase the offsets to 0x400000, then copy
  parameters and lists to copy 0, moving the links between blocks down by
  0x400000.

  Object list entries (lxdream tacore.c / tileiter.h):
    bit 31 = 0              triangle strip, 30:25 triangle mask
    31:29 = 100 / 101       triangle / quad array, 28:25 count - 1
    0xE0000000 | address    link to the next block
    0xF0000000              end of list
  Polygon entries: 24 shadow, 23:21 skip, 20:0 parameter offset in words.

*/

#define HALF            0x400000

#define OL_END          0xf0000000
#define OL_LINK         0xe0000000
#define OL_ADDR_MASK    0x00fffffc
#define OL_OFFSET_MASK  0x001fffff
#define OL_NO_LIST      0x80000000

#define PVR_TILE_MATRIX_HEADER_SIZE 0x48

#define VR(addr)        (*(volatile uint32_t *)(PVR_RAM_BASE + (addr)))

static inline uint32_t align128(uint32_t x) {
    return (x + 127) & ~127u;
}

/* Address of the first block of list l for tile tn in copy h */
static inline uint32_t first_block(const struct pvr_cache *c, int h, int l, int tn) {
    return c->base[h] + c->opb_addresses[l] + c->opb_block[l] * tn;
}

/* Wait until no TA scene is pending and no render is running */
static int cache_wait_idle(void) {
    if(pvr_wait_ready() < 0)
        return -1;

    return pvr_wait_render_done() < 0 ? -1 : 0;
}

/* Mark every tile's lists empty, in both copies */
static void cache_clear(struct pvr_cache *c) {
    int h, l, tn, tiles = pvr_state.tw * pvr_state.th;

    for(h = 0; h < 2; h++)
        for(l = 0; l < PVR_OPB_COUNT; l++)
            if(c->lists & BIT(l))
                for(tn = 0; tn < tiles; tn++)
                    VR(first_block(c, h, l, tn)) = OL_END;

    c->params_used = 0;
    c->entries = 0;
}

/* Write the region array of copy h, for whole-frame renders. Same layout as
   pvr_init_tile_matrix(): a zero header, a dummy entry, then one entry per
   tile in column order. */
static void cache_write_region(struct pvr_cache *c, int h) {
    volatile uint32_t *vr;
    int x, y, l, tn;

    vr = &VR(c->base[h] + c->region - PVR_TILE_MATRIX_HEADER_SIZE);

    for(x = 0; x < PVR_TILE_MATRIX_HEADER_SIZE; x += 4)
        *vr++ = 0;

    vr[0] = 0x10000000;
    vr[1] = vr[2] = vr[3] = vr[4] = vr[5] = OL_NO_LIST;
    vr += 6;

    for(x = 0; x < pvr_state.tw; x++) {
        for(y = 0; y < pvr_state.th; y++) {
            tn = (pvr_state.tw * y) + x;

            vr[0] = (y << 8) | (x << 2) | ((uint32_t)c->presort << 29);

            for(l = 0; l < PVR_OPB_COUNT; l++)
                vr[1 + l] = (c->lists & BIT(l)) ? first_block(c, h, l, tn) : OL_NO_LIST;

            vr += 6;
        }
    }

    vr[-6] |= BIT(31);
}

/* Point list slot l of every tile of both TA buffers' region arrays at the
   cache (copy i for TA buffer i), or mark it empty if c is NULL. */
static void cache_write_slot(int l, struct pvr_cache *c) {
    volatile uint32_t *vr;
    int i, x, y, tn;

    for(i = 0; i < 2; i++) {
        /* Copy i's offsets are relative to TA buffer i's parameter base */
        assert(pvr_state.ta_buffers[i].vertex == (uint32_t)i * HALF);

        vr = &VR(pvr_state.ta_buffers[i].tile_matrix) + 6;

        for(x = 0; x < pvr_state.tw; x++) {
            for(y = 0; y < pvr_state.th; y++) {
                tn = (pvr_state.tw * y) + x;
                vr[1 + l] = c ? first_block(c, i, l, tn) : OL_NO_LIST;
                vr += 6;
            }
        }
    }
}

/* Find room for size bytes in the reserved area. Returns the lower-half
   address and the link to insert the cache at, or 0. */
static uint32_t cache_place(uint32_t size, struct pvr_cache * volatile **link_out) {
    struct pvr_cache * volatile *link = &pvr_state.caches;
    uint32_t start = pvr_state.cache_area;
    uint32_t end = pvr_state.cache_area + pvr_state.cache_area_size;
    struct pvr_cache *c;

    for(;;) {
        c = *link;

        if((c ? c->base[0] : end) - start >= size) {
            *link_out = link;
            return start;
        }

        if(!c)
            return 0;

        start = c->base[0] + c->size;
        link = &c->next;
    }
}

size_t pvr_cache_mem_available(void) {
    struct pvr_cache *c;
    uint32_t start = pvr_state.cache_area;
    uint32_t end = pvr_state.cache_area + pvr_state.cache_area_size;
    uint32_t best = 0;

    if(!pvr_state.valid || !pvr_state.cache_area_size)
        return 0;

    for(c = pvr_state.caches; ; c = c->next) {
        uint32_t gap_end = c ? c->base[0] : end;

        if(gap_end - start > best)
            best = gap_end - start;

        if(!c)
            break;

        start = c->base[0] + c->size;
    }

    return best;
}

pvr_cache_t *pvr_cache_create(const pvr_cache_params_t *params) {
    struct pvr_cache * volatile *link;
    struct pvr_cache *c;
    uint32_t off, sconst, tiles;
    int l, o;

    if(!pvr_state.valid || !pvr_state.cache_area_size || !params)
        return NULL;

    if(params->vertex_buf_size <= 0 || params->opb_overflow_count < 0)
        return NULL;

    c = calloc(1, sizeof(*c));

    if(!c)
        return NULL;

    tiles = pvr_state.tw * pvr_state.th;

    /* Region array, after its header */
    off = PVR_TILE_MATRIX_HEADER_SIZE;
    c->region = off;
    off = align128(off + 4 * (6 + 6 * tiles));

    /* Background plane parameters for whole-frame renders */
    c->bg = off;
    off = align128(off + sizeof(pvr_bkg_poly_t));

    /* TA parameter area */
    c->params = off;
    c->params_size = align128(params->vertex_buf_size);
    off += c->params_size;

    /* First blocks of each list in TA order, then the overflow sets */
    c->opb = off;

    for(l = 0; l < PVR_OPB_COUNT; l++) {
        switch(params->opb_sizes[l]) {
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
                free(c);
                return NULL;
        }

        c->opb_addresses[l] = off;
        c->opb_block[l] = params->opb_sizes[l] * 4;
        off += c->opb_block[l] * tiles;

        if(sconst) {
            c->lists |= BIT(l);
            c->list_reg_mask |= sconst << (4 * l);
        }
    }

    if(!c->lists) {
        free(c);
        return NULL;
    }

    c->opb_size = off - c->opb;
    c->opb_overflow_count = params->opb_overflow_count;
    off += c->opb_size * c->opb_overflow_count;

    c->size = align128(off);
    c->presort = !!params->autosort_disabled;

    o = irq_disable();

    c->base[0] = cache_place(c->size, &link);

    if(!c->base[0]) {
        irq_restore(o);
        free(c);
        return NULL;
    }

    c->base[1] = c->base[0] + HALF;
    c->next = *link;
    *link = c;

    irq_restore(o);

    /* Nothing reads this memory yet, so no need to wait for the PVR */
    cache_clear(c);
    cache_write_region(c, 0);
    cache_write_region(c, 1);

    return c;
}

/* TA error events while building */
static const uint16_t cache_err_evts[] = {
    ASIC_EVT_PVR_PARAM_OUTOFMEM,
    ASIC_EVT_PVR_OPB_OUTOFMEM,
    ASIC_EVT_PVR_TA_INPUT_ERR,
    ASIC_EVT_PVR_TA_INPUT_OVERFLOW
};

#define CACHE_ERR_EVTS  (sizeof(cache_err_evts) / sizeof(cache_err_evts[0]))

static asic_evt_handler_entry_t cache_err_old[CACHE_ERR_EVTS];

static void cache_err_handler(uint32_t code, void *data) {
    size_t i;

    (void)data;

    pvr_state.cache_errors |= BIT(code & 0x1f);

    for(i = 0; i < CACHE_ERR_EVTS; i++)
        if(cache_err_evts[i] == code && cache_err_old[i].hdl)
            cache_err_old[i].hdl(code, cache_err_old[i].data);
}

/* Walk every object list of copy 1. With fix == 0, only gather the range
   of parameter offsets. With fix != 0, add delta words to every offset and
   write each word to copy 0 as well, with links moved to copy 0. Returns
   the number of polygon entries, or -1 if a list is malformed. */
static int cache_walk(struct pvr_cache *c, int fix, uint32_t delta,
                      uint32_t *min_off, uint32_t *max_off) {
    uint32_t a, w, off, lo, hi, steps, max_steps, n = 0;
    int l, tn, tiles = pvr_state.tw * pvr_state.th;

    lo = c->base[1] + c->opb;
    hi = lo + c->opb_size * (1 + c->opb_overflow_count);
    max_steps = (hi - lo) / 4;

    for(l = 0; l < PVR_OPB_COUNT; l++) {
        if(!(c->lists & BIT(l)))
            continue;

        for(tn = 0; tn < tiles; tn++) {
            a = first_block(c, 1, l, tn);

            for(steps = 0; ; steps++) {
                if(steps > max_steps) {
                    dbglog(DBG_ERROR, "pvr_cache: list %d tile %d doesn't end\n",
                           l, tn);
                    return -1;
                }

                w = VR(a);

                if((w & OL_END) == OL_END) {
                    if(fix)
                        VR(a - HALF) = w;

                    break;
                }

                if((w & OL_END) == OL_LINK) {
                    uint32_t next = w & OL_ADDR_MASK;

                    if(next < lo || next >= hi) {
                        dbglog(DBG_ERROR, "pvr_cache: list %d tile %d: link "
                               "%08lx at %06lx leaves %06lx-%06lx\n", l, tn,
                               (unsigned long)w, (unsigned long)a,
                               (unsigned long)lo, (unsigned long)hi);
                        return -1;
                    }

                    if(fix)
                        VR(a - HALF) = OL_LINK | (next - HALF);

                    a = next;
                    continue;
                }

                off = w & OL_OFFSET_MASK;

                if(fix) {
                    off = (off + delta) & OL_OFFSET_MASK;
                    w = (w & ~OL_OFFSET_MASK) | off;
                    VR(a) = w;
                    VR(a - HALF) = w;
                }
                else {
                    if(off < *min_off)
                        *min_off = off;

                    if(off > *max_off)
                        *max_off = off;
                }

                n++;
                a += 4;
            }
        }
    }

    return (int)n;
}

/* After a build into copy 1, with the TA's parameters at ib up to pos: find
   the base the TA wrote its offsets against, rebase them to copy 1's
   parameter base and mirror the lists to copy 0. */
static int cache_rebase(struct pvr_cache *c, uint32_t ib, uint32_t pos) {
    uint32_t min_off = OL_OFFSET_MASK, max_off = 0, tb;
    int n;

    n = cache_walk(c, 0, 0, &min_off, &max_off);

    if(n < 0) {
        dbglog(DBG_ERROR, "pvr_cache_build: malformed object list\n");
        return -1;
    }

    /* lxdream: the TA's write start rounded down to 1 MB. Otherwise the
       write start itself. Every entry must point into what the TA wrote. */
    tb = ib & 0xf00000;

    if(n > 0 && (tb + min_off * 4 < ib || tb + max_off * 4 >= pos)) {
        tb = ib;

        if(tb + max_off * 4 >= pos) {
            dbglog(DBG_ERROR, "pvr_cache_build: offsets %05lx-%05lx don't fit "
                   "parameters %06lx-%06lx\n", (unsigned long)min_off,
                   (unsigned long)max_off, (unsigned long)ib, (unsigned long)pos);
            return -1;
        }
    }

    c->ta_param_base = tb;

    n = cache_walk(c, 1, (tb - HALF) >> 2, NULL, NULL);

    if(n < 0)
        return -1;

    c->entries = n;

    return 0;
}

int pvr_cache_build(pvr_cache_t *c, void (*send)(void *data), void *data) {
    uint32_t ib, ie, ob, pos, closed, done, errors, a;
    int saved_checked, o, l, timeouts;
    size_t i;
    uint64_t t0;

    if(!c || !send || !pvr_state.valid)
        return -1;

    if(pvr_state.dma_mode) {
        dbglog(DBG_ERROR, "pvr_cache_build: not supported with vertex DMA\n");
        return -1;
    }

    if(pvr_state.list_reg_open != PVR_LIST_NONE) {
        dbglog(DBG_ERROR, "pvr_cache_build: called inside a scene\n");
        return -1;
    }

    t0 = timer_ns_gettime64();

    /* The previous scene must be rendering or done, and nothing may be
       reading the cache while it is rewritten */
    if(cache_wait_idle() < 0) {
        dbglog(DBG_ERROR, "pvr_cache_build: PVR didn't become idle\n");
        return -1;
    }

    /* The TA fills copy 1 */
    ib = c->base[1] + c->params;
    ie = ib + c->params_size;
    ob = c->base[1] + c->opb;

    for(i = 0; i < CACHE_ERR_EVTS; i++) {
        cache_err_old[i] = asic_evt_set_handler(cache_err_evts[i], cache_err_handler, NULL);
        asic_evt_enable(cache_err_evts[i], ASIC_IRQ_DEFAULT);
    }

    o = irq_disable();

    pvr_state.cache_building = 1;
    pvr_state.cache_lists_done = 0;
    pvr_state.cache_errors = 0;

    /* Make the list calls skip the scene start, and allow every list */
    saved_checked = pvr_state.ta_checked_ready;
    pvr_state.ta_checked_ready = 1;
    pvr_state.lists_closed = 0;

    PVR_SET(PVR_TA_OPB_START,       ob);
    PVR_SET(PVR_TA_OPB_INIT,        ob + c->opb_size);
    PVR_SET(PVR_TA_OPB_END,         ob + c->opb_size * (1 + c->opb_overflow_count));
    PVR_SET(PVR_TA_VERTBUF_START,   ib);
    PVR_SET(PVR_TA_VERTBUF_END,     ie);
    PVR_SET(PVR_TILEMAT_CFG,        pvr_state.tsize_const);
    PVR_SET(PVR_OPB_CFG,            c->list_reg_mask);
    PVR_SET(PVR_TA_INIT,            PVR_TA_INIT_GO);
    (void)PVR_GET(PVR_TA_INIT);

    irq_restore(o);

    send(data);

    if(pvr_state.list_reg_open != PVR_LIST_NONE)
        pvr_list_finish();

    /* Lists of the cache that weren't sent are sent empty, so that the TA
       initialises their blocks */
    for(l = 0; l < PVR_OPB_COUNT; l++) {
        if((c->lists & BIT(l)) && !(pvr_state.lists_closed & BIT(l))) {
            pvr_list_begin(l);
            pvr_list_finish();
        }
    }

    /* Wait for the done event of every list sent, so none arrives after
       the scene logic takes the events back */
    closed = pvr_state.lists_closed;
    timeouts = 0;

    o = irq_disable();

    while((pvr_state.cache_lists_done & closed) != closed && timeouts < 10) {
        if(genwait_wait((void *)&pvr_state.cache_lists_done, "PVR cache build", 100) < 0)
            timeouts++;
    }

    done = pvr_state.cache_lists_done;
    pos = PVR_GET(PVR_TA_VERTBUF_POS);
    errors = pvr_state.cache_errors;

    pvr_state.cache_building = 0;
    pvr_state.ta_checked_ready = saved_checked;
    pvr_state.lists_closed = 0;
    pvr_state.list_reg_open = PVR_LIST_NONE;

    /* Point the TA back at the next frame's buffer */
    pvr_sync_reg_buffer();

    irq_restore(o);

    for(i = 0; i < CACHE_ERR_EVTS; i++) {
        asic_evt_set_handler(cache_err_evts[i], cache_err_old[i].hdl, cache_err_old[i].data);

        if(!cache_err_old[i].hdl)
            asic_evt_disable(cache_err_evts[i], ASIC_IRQ_DEFAULT);
    }

    if((done & closed) != closed) {
        dbglog(DBG_ERROR, "pvr_cache_build: TA didn't finish (lists %02lx of %02lx)\n",
               (unsigned long)done, (unsigned long)closed);
        goto fail;
    }

    if(errors || pos >= ie) {
        dbglog(DBG_ERROR, "pvr_cache_build: TA ran out of space (errors %08lx, "
               "parameters %lu of %lu bytes)\n", (unsigned long)errors,
               (unsigned long)(pos - ib), (unsigned long)(ie - ib));
        goto fail;
    }

    c->params_used = pos - ib;

    if(cache_rebase(c, ib, pos) < 0)
        goto fail;

    /* Mirror the parameters to copy 0 */
    for(a = ib; a < pos; a += 4)
        VR(a - HALF) = VR(a);

    c->build_ns = timer_ns_gettime64() - t0;

    return 0;

fail:
    cache_clear(c);
    c->build_ns = timer_ns_gettime64() - t0;

    return -1;
}

int pvr_cache_attach(pvr_cache_t *c, int target) {
    int o;

    if(!pvr_state.valid)
        return -1;

    if(target >= PVR_CACHE_PASS(0)) {
        dbglog(DBG_ERROR, "pvr_cache_attach: cached passes need multipass "
                          "rendering\n");
        return -1;
    }

    if(target < 0 || target >= PVR_OPB_COUNT)
        return -1;

    if(pvr_state.lists_enabled & BIT(target)) {
        dbglog(DBG_ERROR, "pvr_cache_attach: list %d is used by the TA\n", target);
        return -1;
    }

    if(c && !(c->lists & BIT(target))) {
        dbglog(DBG_ERROR, "pvr_cache_attach: list %d is not in the cache\n", target);
        return -1;
    }

    if(cache_wait_idle() < 0)
        return -1;

    o = irq_disable();
    cache_write_slot(target, c);
    pvr_state.slot_cache[target] = c;
    irq_restore(o);

    return 0;
}

void pvr_begin_cached_render(struct pvr_cache *c) {
    int h = pvr_state.cache_half ^ 1;

    /* Alternate between the copies, so both are exercised */
    pvr_state.cache_half = h;

    pvr_begin_render(c->base[h] + c->region, (uint32_t)h * HALF,
                     c->base[h] + c->bg);
}

int pvr_cache_get_info(const pvr_cache_t *c, pvr_cache_info_t *info) {
    int h, l;

    if(!c || !info)
        return -1;

    for(h = 0; h < 2; h++) {
        info->base[h] = c->base[h];
        info->param_base[h] = h * HALF;
        info->params[h] = c->base[h] + c->params;
        info->overflow[h] = c->base[h] + c->opb + c->opb_size;
        info->region[h] = c->base[h] + c->region;

        for(l = 0; l < PVR_OPB_COUNT; l++)
            info->lists[h][l] = (c->lists & BIT(l)) ? c->base[h] + c->opb_addresses[l] : 0;
    }

    for(l = 0; l < PVR_OPB_COUNT; l++)
        info->list_block_size[l] = c->opb_block[l];

    info->size = c->size;
    info->params_size = c->params_size;
    info->params_used = c->params_used;
    info->overflow_size = c->opb_size * c->opb_overflow_count;
    info->tiles_w = pvr_state.tw;
    info->tiles_h = pvr_state.th;
    info->entries = c->entries;
    info->ta_param_base = c->ta_param_base;
    info->build_ns = c->build_ns;

    return 0;
}

void pvr_cache_free(pvr_cache_t *c) {
    struct pvr_cache * volatile *link;
    int l, o;

    if(!c)
        return;

    cache_wait_idle();

    o = irq_disable();

    for(l = 0; l < PVR_OPB_COUNT; l++) {
        if(pvr_state.slot_cache[l] == c) {
            cache_write_slot(l, NULL);
            pvr_state.slot_cache[l] = NULL;
        }
    }

    if(pvr_state.frame_cache == c)
        pvr_state.frame_cache = NULL;

    for(link = &pvr_state.caches; *link; link = &(*link)->next) {
        if(*link == c) {
            *link = c->next;
            break;
        }
    }

    irq_restore(o);

    free(c);
}

void pvr_cache_shutdown(void) {
    struct pvr_cache *c, *next;
    int l;

    if(pvr_state.caches)
        cache_wait_idle();

    for(c = pvr_state.caches; c; c = next) {
        next = c->next;
        free(c);
    }

    pvr_state.caches = NULL;
    pvr_state.frame_cache = NULL;

    for(l = 0; l < PVR_OPB_COUNT; l++)
        pvr_state.slot_cache[l] = NULL;
}
