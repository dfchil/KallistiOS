/* KallistiOS ##version##

   dc/pvr/pvr_cache.h
   Copyright (C) 2026 Daniel Fairchild
*/

/** \file       dc/pvr/pvr_cache.h
    \brief      Cached object lists: static layers drawn without the TA.
    \ingroup    pvr_cache

    \author Daniel Fairchild
*/

#ifndef __DC_PVR_PVR_CACHE_H
#define __DC_PVR_PVR_CACHE_H

#include <stddef.h>
#include <stdint.h>

#include <kos/cdefs.h>
__BEGIN_DECLS

/** \defgroup pvr_cache Cached Object Lists
    \brief              Build polygon lists once, draw them every frame.
    \ingroup            pvr

    Each frame, the Tile Accelerator (TA) turns the polygons it is sent into
    polygon parameters and into one object list per tile and list type. The
    ISP/TSP then renders the frame by walking the region array (tile matrix),
    which points at those object lists. Nothing in that chain needs the TA at
    render time, so lists built once can be rendered again and again.

    A cache holds such parameters and object lists in a reserved part of the
    32-bit VRAM view. It is filled once, normally by sending polygons to the TA
    with the usual list calls (pvr_cache_build()). Every frame can then draw it
    at no CPU or TA cost, in one of two ways:

    - Attached to a list slot the TA doesn't use (pvr_cache_attach()): every
      tile's pointer for that list type, in every TA frame, points at the
      cache. The typical use is a static overlay in the punch-through slot
      of a program that doesn't use punch-through polygons.
    - As a whole frame (pvr_scene_finish_cached()): a frame rendered from the
      cache alone, with no TA work at all. For static screens.

    Vertices are in screen space, so only content that stays put on screen
    can be cached. Rebuild a cache when its content changes.

    \par Memory
    Set pvr_init_params_t::cached_list_size to reserve the space. KOS keeps
    two copies of every cache, one in each half of the 32-bit VRAM layout,
    next to the TA buffer that reads it, so the object list offsets of each
    copy are relative to that TA buffer's parameter base (0 or 0x400000).
    Reserving R bytes costs 2R bytes of texture memory. With the default
    value of 0 nothing is reserved and the VRAM layout is unchanged.

    \par Limitations
    - Vertex DMA mode (pvr_init_params_t::dma_enabled) is not supported.
    - pvr_cache_build(), pvr_cache_attach() and pvr_cache_free() wait for
      the PVR to become idle. Call them between frames, never between
      pvr_scene_begin() and pvr_scene_finish().
    - Caches are tied to the tile layout (screen size and FSAA) they were
      created with. pvr_shutdown() frees all caches.

    \par Hardware results
    Tested on a Dreamcast console with examples/dreamcast/pvr/cache_hwtest
    (2026-10-29):
    - The ISP renders object lists anywhere in the 32-bit VRAM view: a
      hand-built list in the punch-through slot of TA frames from both TA
      buffers, and as a whole frame from its own region array.
    - The ISP's parameter base (PVR_ISP_VERTBUF_ADDR) is masked to 1 MB:
      0x410000 and 0x480000 both read as 0x400000.
    - Object list entry offsets have 21 bits (words): an entry with bit 20
      set reaches parameters more than 4 MB above the base. KOS doesn't
      depend on this: each copy's offsets stay below 4 MB.
    - The TA writes its offsets relative to its parameter write start
      (TA_ISP_BASE) rounded down to 1 MB, as lxdream does.
      pvr_cache_build() checks this on every build (see
      pvr_cache_info_t::ta_param_base).
    - TA-built caches, with overflow blocks, render pixel-identical to the
      same polygons sent through the TA's punch-through list, from both TA
      buffers, after a rebuild and after many TA frames, with FSAA and
      vertex double-buffering on and off.
    - Reserving R bytes takes exactly 2R from texture memory, and textures
      next to the reserve are unharmed.

    \par Performance
    Punch-through polygons cost the ISP far more than opaque ones (10000
    16x16 quads: 8.8 ms opaque, 49 ms punch-through), and a cache in the
    punch-through slot renders exactly as slowly as the same polygons sent
    as punch-through. The slot saves CPU and TA time, not render time.
    Prefer opaque and translucent lists for caches rendered as whole
    frames.

    Emulators that don't keep the TA's object lists in VRAM (Flycast) can't
    render caches, and pvr_cache_build() fails on them.
*/

/** \brief   Opaque handle to a cache of object lists.
    \ingroup pvr_cache
*/
typedef struct pvr_cache pvr_cache_t;

/** \brief   Cache creation parameters.
    \ingroup pvr_cache

    \headerfile dc/pvr/pvr_cache.h
*/
typedef struct pvr_cache_params {
    /** \brief  Object list block size of each list type in the cache, in
                words (PVR_BINSIZE_*). Lists with size 0 are not in the
                cache. Same order as pvr_init_params_t::opb_sizes. */
    int     opb_sizes[5];

    /** \brief  Bytes of polygon parameter space. */
    int     vertex_buf_size;

    /** \brief  Extra sets of object list blocks for tiles whose lists don't
                fit in their first block, as in pvr_init_params_t. */
    int     opb_overflow_count;

    /** \brief  Presort the translucent list when the cache is rendered as
                a whole frame (pvr_scene_finish_cached()). */
    int     autosort_disabled;
} pvr_cache_params_t;

/** \brief   Cache layout and statistics, from pvr_cache_get_info().
    \ingroup pvr_cache

    Addresses are byte offsets in the 32-bit VRAM view (access them through
    PVR_RAM_BASE). Index [h] is copy h: copy 0 is read by TA buffer 0, whose
    parameter base is 0; copy 1 by TA buffer 1, parameter base 0x400000.
    Whole-frame renders alternate between the copies.

    The first object list block of list \p l for tile (x, y) of copy h is at
    lists[h][l] + list_block_size[l] * (y * tiles_w + x).

    To build a cache by hand, write the parameters and object lists of both
    copies. The offset in an object list entry of copy h is
    (parameter address - param_base[h]) / 4, and link entries hold absolute
    addresses. pvr_cache_create() initialises every first block to an empty
    list (0xF0000000).

    \headerfile dc/pvr/pvr_cache.h
*/
typedef struct pvr_cache_info {
    uint32_t base[2];               /**< \brief Start of each copy */
    uint32_t size;                  /**< \brief Bytes used by each copy */
    uint32_t param_base[2];         /**< \brief Parameter base of each copy */
    uint32_t params[2];             /**< \brief Parameter area of each copy */
    uint32_t params_size;           /**< \brief Size of the parameter area */
    uint32_t params_used;           /**< \brief Bytes written by the last build */
    uint32_t lists[2][5];           /**< \brief First blocks of each list, 0 if the list isn't in the cache */
    uint32_t list_block_size[5];    /**< \brief Bytes per tile in each list's first blocks */
    uint32_t overflow[2];           /**< \brief Overflow block area of each copy */
    uint32_t overflow_size;         /**< \brief Size of the overflow block area */
    uint32_t region[2];             /**< \brief Region array of each copy (whole-frame renders) */
    int      tiles_w;               /**< \brief Tiles across (doubled with FSAA) */
    int      tiles_h;               /**< \brief Tiles down */
    uint32_t entries;               /**< \brief Object list entries written by the last build */
    uint32_t ta_param_base;         /**< \brief Base the TA wrote its offsets against in the last build */
    uint32_t build_ns;              /**< \brief Duration of the last build */
} pvr_cache_info_t;

/** \brief   Attach target: one list slot of every TA frame.
    \ingroup pvr_cache

    \param  list            The list type (pvr_list_t). The TA must not use
                            it: its block size in pvr_init_params_t must be 0.
*/
#define PVR_CACHE_SLOT(list)    ((int)(list))

/** \brief   Attach target: a whole render pass (reserved).
    \ingroup pvr_cache

    Reserved for multipass rendering, where the cache will supply every list
    of one pass. pvr_cache_attach() refuses it for now.

    \param  pass            The pass number.
*/
#define PVR_CACHE_PASS(pass)    (0x100 + (pass))

/** \brief   Reserved VRAM still free for caches.
    \ingroup pvr_cache

    \return                 Largest cache size, in bytes per copy, that
                            pvr_cache_create() can still place.
*/
size_t pvr_cache_mem_available(void);

/** \brief   Create an empty cache.
    \ingroup pvr_cache

    Places the cache in the area reserved with
    pvr_init_params_t::cached_list_size, writes its region arrays and
    marks every tile's lists empty.

    \param  params          Lists, sizes and sort mode of the cache.
    \return                 The new cache, or NULL if it doesn't fit, nothing
                            was reserved, or the parameters are invalid.
*/
pvr_cache_t *pvr_cache_create(const pvr_cache_params_t *params);

/** \brief   Fill a cache with the TA.
    \ingroup pvr_cache

    Waits for the PVR to be idle, points the TA at the cache and calls
    \p send, which submits polygons with pvr_list_begin(), pvr_prim(),
    pvr_list_finish() or direct rendering, exactly as in a scene. Lists the
    cache has but \p send didn't submit are sent empty. The TA's done
    interrupts are kept away from the scene logic, so no render starts.
    Afterwards the object list offsets are rebased and the result copied to
    the second copy.

    Rebuilding a cache that is attached or used for whole frames is allowed;
    the next frame shows the new content.

    \param  cache           The cache to fill. Its old content is replaced.
    \param  send            Callback that submits the polygons.
    \param  data            Passed to \p send.
    \retval 0               On success.
    \retval -1              On error: called with vertex DMA enabled, the TA
                            ran out of parameter or object list space, the
                            TA didn't finish, or its object lists didn't
                            look as expected (details in the debug log).
                            The cache is left empty.
*/
int pvr_cache_build(pvr_cache_t *cache, void (*send)(void *data), void *data);

/** \brief   Attach a cache to every TA frame, or detach it.
    \ingroup pvr_cache

    With PVR_CACHE_SLOT(list), every tile's pointer for that list type in
    both TA buffers' region arrays points at the cache's list of the same
    type, so every following frame (including render-to-texture frames)
    draws it. The list must be disabled for the TA and present in the cache.
    Punch-through is the usual choice: it is drawn after the opaque list and
    before the translucent one, depth-tested against the opaque polygons.
    It saves the CPU and TA time of the cached polygons, but the ISP renders
    punch-through polygons several times slower than opaque ones.

    \param  cache           The cache to attach, or NULL to detach whatever
                            is attached to \p target.
    \param  target          PVR_CACHE_SLOT(list). PVR_CACHE_PASS(pass) is
                            reserved for multipass rendering.
    \retval 0               On success.
    \retval -1              If the target is invalid, used by the TA, or not
                            in the cache.
*/
int pvr_cache_attach(pvr_cache_t *cache, int target);

/** \brief   Finish a scene that is rendered from a cache alone.
    \ingroup pvr_scene_mgmt

    Use instead of pvr_scene_finish(), after pvr_scene_begin() or
    pvr_scene_begin_rtt() and without submitting any list. The frame is
    rendered from the cache's own region array with no TA work; it goes
    through the usual pipeline (render start, page flip, statistics).

    \param  cache           The cache to render.
    \retval 0               On success.
    \retval -1              If \p cache is NULL, vertex DMA is enabled, or
                            lists were submitted in this scene.
*/
int pvr_scene_finish_cached(pvr_cache_t *cache);

/** \brief   Get a cache's layout and statistics.
    \ingroup pvr_cache

    \param  cache           The cache.
    \param  info            Filled in.
    \retval 0               On success.
    \retval -1              If an argument is NULL.
*/
int pvr_cache_get_info(const pvr_cache_t *cache, pvr_cache_info_t *info);

/** \brief   Free a cache.
    \ingroup pvr_cache

    Waits for the PVR to be idle, detaches the cache from any slot and
    releases its VRAM.

    \param  cache           The cache to free. NULL is ignored.
*/
void pvr_cache_free(pvr_cache_t *cache);

__END_DECLS

#endif  /* __DC_PVR_PVR_CACHE_H */
