/* KallistiOS ##version##

   dc/pvr/pvr_region.h
   Copyright (C) 2026 Daniel Fairchild
*/

/** \file       dc/pvr/pvr_region.h
    \brief      Per-tile control of the PVR region array
    \ingroup    pvr_region

    \author Daniel Fairchild
*/

#ifndef __DC_PVR_PVR_REGION_H
#define __DC_PVR_PVR_REGION_H

#include <stdbool.h>

#include <kos/cdefs.h>
__BEGIN_DECLS

/** \defgroup   pvr_region  Per-Tile Rendering
    \brief                  Per-tile sort mode and partial redraw
    \ingroup                pvr_global

    The PVR renders the screen one 32x32 pixel tile at a time. For each
    scene it walks a list of tiles, the region array (KOS also calls it the
    tile matrix). Each entry of that list says which tile to render, whether
    its translucent polygons are presorted or autosorted, and where its
    polygon lists are. By default KOS lists every tile of the screen, all in
    the sort mode set with pvr_init_params_t::autosort_disabled or
    pvr_set_presort_mode().

    The functions here change that per tile:

    - **Sort mode per tile.** pvr_set_tile_presort() and
      pvr_set_rect_presort() choose presort or autosort for the translucent
      list of single tiles or a rectangle. pvr_set_presort_mode() still sets
      every tile at once.
    - **Skipped tiles.** pvr_set_tile_enabled() and pvr_set_rect_enabled()
      leave tiles out of the region array. A tile that isn't listed is neither
      rendered nor written to the frame buffer, so the frame buffer keeps the
      pixels it held there. This allows partial redraw: a static part of the
      screen costs no rendering time, and its geometry need not be sent.
      Polygons that cover skipped tiles may still be submitted; they are
      binned by the TA as usual but not drawn there.

    \par Tile coordinates
    Tiles are numbered from the top left, starting at 0. The grid is
    returned by pvr_get_tile_grid(). With FSAA the PVR renders at twice the
    horizontal resolution, so there are twice as many tile columns, and each
    covers 16 pixels of the screen. The rectangle functions take screen
    pixels and handle this; the single-tile functions take tile coordinates
    as the PVR sees them.

    \par Double buffering
    KOS renders into two frame buffers in turn. A skipped tile shows what the
    frame buffer being rendered into held two frames earlier. Static content
    in skipped tiles must therefore first be rendered into both frame
    buffers: render at least two consecutive frames with the tiles enabled
    before skipping them. Anything that changes the static content (a
    different background color, say) needs the same two frames again.

    \par When changes take effect
    Each of KOS's two vertex buffers has its own region array. A change is
    written into the array of the buffer that the next scene fills, when that
    scene starts to send data (its first pvr_list_begin(), or
    pvr_scene_finish() in DMA mode). A change made while a scene is being
    registered applies from the next scene on. pvr_set_presort_mode() is the
    exception: as before, it also applies to the scene being registered.
    An array is rewritten only when it doesn't match the per-tile state, so
    a program may set its whole layout every frame: if the result is the
    same as before, nothing is written. A rewrite costs one uncached write
    per word, 6 words per tile and pass (about 7 KB at 640x480 in one pass,
    twice that with FSAA).

    \par Multipass scenes
    With pvr_init_params_t::pass_count above 1, a tile has one entry per
    pass. A skipped tile is left out of every pass. Each pass of each tile
    has its own sort mode: pvr_set_tile_presort() and pvr_set_rect_presort()
    set a tile in every pass, pvr_set_pass_presort() sets every tile of one
    pass, and pvr_set_presort_mode() sets everything. The last call wins, so
    to presort one pass everywhere and choose per tile in the others, set
    the tiles first and the pass after them.

    \par Render to texture
    Scenes begun with pvr_scene_begin_txr() or pvr_scene_begin_rtt() use the
    per-tile sort mode but ignore skipped tiles: every tile is rendered, so
    the texture is always complete. While tiles are skipped, switching
    between texture and screen scenes rewrites the array each time.

    \par Hardware notes
    Tested on a Dreamcast (NTSC, 640x480, with and without FSAA) by
    reading the frame buffer back:
    - The presort bit is read per tile. Presorted and autosorted tiles in
      one frame (halves, a checkerboard, a random pattern) each sort as
      set, with no seam between them.
    - A tile left out of the region array is not written: over 200 frames
      of geometry changing on top of them, skipped tiles kept their pixels
      exactly. Render time drops roughly in proportion to the tiles
      skipped.
    - The hardware does not finish a render whose last region array entry
      has the no-writeout bit (bit 28) set. KOS's dummy first entry has it,
      so an array holding only the dummy entry hangs the PVR. When every
      tile is skipped, KOS therefore doesn't start the render at all: the
      frame completes at once and the view flips as usual.
    - An entry with the no-writeout bit set elsewhere in the array is
      rendered but not written out, and that tile keeps its pixels.
    - The order of the entries doesn't matter: row order, reversed and
      shuffled arrays render the same picture. KOS keeps its column order.
    - Rewriting the array costs about 1.3 microseconds per tile: about
      0.4 ms at 640x480, 0.7 ms with FSAA.

    @{
*/

/** \brief   Get the size of the tile grid.

    Returns the number of tile columns and rows the PVR renders. With FSAA,
    the number of columns is twice the screen width divided by 32.

    \param  tw              Set to the number of tile columns, if not NULL.
    \param  th              Set to the number of tile rows, if not NULL.
*/
void pvr_get_tile_grid(int *tw, int *th);

/** \brief   Set the translucent sort mode of one tile.

    In a multipass scene this sets the tile in every pass.

    \param  tx              Tile column (see \ref pvr_region for FSAA).
    \param  ty              Tile row.
    \param  presort         True to presort the tile's translucent polygons
                            (draw them in submission order), false to
                            autosort them.
    \retval 0               On success.
    \retval -1              If the tile is outside the grid or the PVR is
                            not initialized.

    \sa pvr_set_rect_presort(), pvr_set_presort_mode()
*/
int pvr_set_tile_presort(int tx, int ty, bool presort);

/** \brief   Set the translucent sort mode of a rectangle of the screen.

    Sets the sort mode of every tile the rectangle touches, in every pass of
    a multipass scene. The rectangle is clipped to the screen.

    \param  x               Left edge, in screen pixels.
    \param  y               Top edge, in screen pixels.
    \param  w               Width, in screen pixels.
    \param  h               Height, in screen pixels.
    \param  presort         True for presort, false for autosort.
    \retval 0               On success.
    \retval -1              If the rectangle doesn't touch the screen or the
                            PVR is not initialized.
*/
int pvr_set_rect_presort(int x, int y, int w, int h, bool presort);

/** \brief   Render or skip one tile.

    A skipped tile is left out of the region array: it is neither rendered
    nor written, and the frame buffer keeps its earlier pixels there. See
    \ref pvr_region for the double buffering caveat.

    \param  tx              Tile column (see \ref pvr_region for FSAA).
    \param  ty              Tile row.
    \param  enabled         True to render the tile, false to skip it.
    \retval 0               On success.
    \retval -1              If the tile is outside the grid or the PVR is
                            not initialized.

    \sa pvr_set_rect_enabled()
*/
int pvr_set_tile_enabled(int tx, int ty, bool enabled);

/** \brief   Render or skip a rectangle of the screen.

    Sets every tile the rectangle touches. The rectangle is clipped to the
    screen. To redraw only a window, skip the whole screen and then enable
    the window:

    \code
    pvr_set_rect_enabled(0, 0, 640, 480, false);
    pvr_set_rect_enabled(win_x, win_y, win_w, win_h, true);
    \endcode

    \param  x               Left edge, in screen pixels.
    \param  y               Top edge, in screen pixels.
    \param  w               Width, in screen pixels.
    \param  h               Height, in screen pixels.
    \param  enabled         True to render the tiles, false to skip them.
    \retval 0               On success.
    \retval -1              If the rectangle doesn't touch the screen or the
                            PVR is not initialized.
*/
int pvr_set_rect_enabled(int x, int y, int w, int h, bool enabled);

/** @} */

__END_DECLS

#endif /* __DC_PVR_PVR_REGION_H */
