/* KallistiOS ##version##

   dc/pvr/pvr_misc.h
   Copyright (C) 2002 Megan Potter
   Copyright (C) 2014 Lawrence Sebald
   Copyright (C) 2023 Ruslan Rostovtsev
   Copyright (C) 2024 Falco Girgis
*/

/** \file       dc/pvr/pvr_misc.h
    \brief      Miscellaneous utilities for the PVR API
    \ingroup    pvr

    \author Megan Potter
    \author Roger Cattermole
    \author Paul Boese
    \author Brian Paul
    \author Lawrence Sebald
    \author Benoit Miller
    \author Ruslan Rostovtsev
    \author Falco Girgis
*/

#ifndef __DC_PVR_PVR_MISC_H
#define __DC_PVR_PVR_MISC_H

#include <stdbool.h>
#include <stdint.h>

#include <kos/cdefs.h>

#include <dc/pvr/pvr_mem.h>
#include <dc/pvr/pvr_header.h>

__BEGIN_DECLS


/** \brief   Pack four floating point color values into a 32-bit integer form.

    All of the color values should be between 0 and 1.

    \param  a               Alpha value
    \param  r               Red value
    \param  g               Green value
    \param  b               Blue value
    \return                 The packed color value
*/
#define PVR_PACK_COLOR(a, r, g, b) ( \
        ( ((uint8_t)( (a) * 255 ) ) << 24 ) | \
        ( ((uint8_t)( (r) * 255 ) ) << 16 ) | \
        ( ((uint8_t)( (g) * 255 ) ) <<  8 ) | \
        ( ((uint8_t)( (b) * 255 ) ) <<  0 ) )

/** \brief   Pack two floating point coordinates into one 32-bit value,
             truncating them to 16-bits each.

    \param  u               First coordinate to pack
    \param  v               Second coordinate to pack
    \return                 The packed coordinates
*/
static inline uint32_t PVR_PACK_16BIT_UV(float u, float v) {
    union {
        float f;
        uint32_t i;
    } u2, v2;

    u2.f = u;
    v2.f = v;

    return (u2.i & 0xFFFF0000) | (v2.i >> 16);
}


/** \defgroup pvr_global Global State
    \brief               PowerVR functionality which is managed globally
    \ingroup             pvr

    These are miscellaneous parameters you can set which affect the
    rendering process.
*/

/** \defgroup pvr_bg Background Plane
    \brief          The polygon the PVR draws behind everything else
    \ingroup        pvr_global

    The PVR fills the area of the screen not covered by any other polygon with
    the background plane. It is a real polygon: three vertices with a position,
    a color and optionally texture coordinates. The ISP draws it into every
    tile when it clears the depth buffer, at the depth set with
    pvr_set_zclip().

    The three vertices do not limit the area that is drawn. They only define a
    plane, and the plane fills the whole render target: color (and texture
    coordinates) are extended from the three vertices over the full screen.
    The vertices may lie anywhere, on screen or not. By default they are the
    bottom-left, top-left and bottom-right corners of the render target, which
    is the screen or, while rendering to a texture, the render-to-texture area.

    With the default vertices, the top-right corner lies outside the triangle
    and gets color(top-left) + color(bottom-right) - color(bottom-left). A
    gradient along one axis (top to bottom, or left to right) stays in range;
    a tilted gradient can go below 0 or above 255 there. To keep the colors in
    range, place the vertices with pvr_set_bg_poly() so that the colors are
    known where the gradient is extreme, or pick colors that keep all four
    screen corners in range.

    Shading costs nothing extra: the hardware shades the plane with Gouraud
    shading whether its vertices have one color or three.

    \note   When FSAA is enabled (see pvr_init_params_t::fsaa_enabled), the
            render target is twice as wide as the screen, and the x positions
            given to pvr_set_bg_poly() are in that doubled space, like the
            positions of all other vertices.

    All the functions in this group take effect from the next frame that
    begins to render, and the setting stays until it is changed. Call them
    between frames (e.g. after pvr_wait_ready()), not during a frame.
*/

/** \brief   Background plane vertex.
    \ingroup pvr_bg

    One of the three vertices given to pvr_set_bg_poly().
*/
typedef struct pvr_bg_vertex {
    float    x;     /**< \brief X position, in render target pixels */
    float    y;     /**< \brief Y position, in render target pixels */
    float    z;     /**< \brief 1/w, must be greater than zero.

                         Used only to interpolate color and texture
                         coordinates. Give all three vertices the same value
                         (e.g. 1.0f) for interpolation that is linear across
                         the screen. The depth of the plane is the
                         pvr_set_zclip() value, not this one. */
    float    u;     /**< \brief Texture U coordinate (textured plane only) */
    float    v;     /**< \brief Texture V coordinate (textured plane only) */
    uint32_t argb;  /**< \brief Color, packed ARGB (alpha is ignored) */
} pvr_bg_vertex_t;

/** \brief   Set the background plane color.
    \ingroup pvr_bg

    This function sets the color of the area of the screen not covered by any
    other polygons.

    It also returns the background plane to its default: the default vertices,
    one color everywhere, and no texture.

    \param  r               Red component of the color to set
    \param  g               Green component of the color to set
    \param  b               Blue component of the color to set

    \sa pvr_set_bg_colors(), pvr_set_bg_poly(), pvr_set_bg_texture()
*/
void pvr_set_bg_color(float r, float g, float b);

/** \brief   Set a background plane color for each default vertex.
    \ingroup pvr_bg

    This function places the background plane's vertices on the bottom-left,
    top-left and bottom-right corners of the render target and gives each its
    own color. The plane is shaded with a linear gradient over the whole
    screen.

    The top-right corner gets top_left + bottom_right - bottom_left, per
    channel. For a vertical gradient, pass the bottom color twice
    (bottom_left = bottom_right); for a horizontal one, pass the left color
    twice (bottom_left = top_left).

    If a background texture is set (see pvr_set_bg_texture()), it is kept: the
    colors modulate it, and the texture is stretched once over the render
    target (u from 0 at the left to 1 at the right, v from 0 at the top to 1
    at the bottom).

    \param  bottom_left     Packed ARGB color at the bottom-left corner
    \param  top_left        Packed ARGB color at the top-left corner
    \param  bottom_right    Packed ARGB color at the bottom-right corner

    \sa PVR_PACK_COLOR()
*/
void pvr_set_bg_colors(uint32_t bottom_left, uint32_t top_left,
                       uint32_t bottom_right);

/** \brief   Set the background plane's three vertices.
    \ingroup pvr_bg

    This function gives full control over the background plane: the position,
    color and texture coordinates of its three vertices. The vertices need not
    be on screen; they only define the plane, which always fills the whole
    render target.

    The vertices may be given in either winding order. If a texture is set
    (see pvr_set_bg_texture()), it is kept.

    \param  v               The three vertices
    \retval 0               On success
    \retval -1              If the vertices are collinear (they do not define
                            a plane), or a z is not greater than zero. The
                            background plane is not changed.
*/
int pvr_set_bg_poly(const pvr_bg_vertex_t v[3]);

/** \brief   Set or remove the background plane's texture.
    \ingroup pvr_bg

    This function textures the background plane, giving a full-screen backdrop
    without any geometry. The texture coordinates are those of the vertices:
    stretched once over the render target after pvr_set_bg_colors(), or as
    given to pvr_set_bg_poly(). Texture coordinates outside 0 to 1 repeat the
    texture.

    The texture is modulated by the vertex colors (use white, 0xffffffff, for
    the texture's own colors). Its alpha is ignored. Mipmaps are not used.

    The texture stays set until this function is called again with NULL, or
    until pvr_set_bg_color() is called. The caller must keep the texture in
    video memory as long as it is set.

    \param  txr             The texture, or NULL to remove the texture
    \param  textureformat   The texture format, see \ref pvr_txr_fmts
    \param  tw              Texture width, a power of two from 8 to 1024
    \param  th              Texture height, a power of two from 8 to 1024
    \param  filtering       Texture filtering mode (PVR_FILTER_NEAREST or
                            PVR_FILTER_BILINEAR)
    \retval 0               On success
    \retval -1              If the size or filter is invalid. The background
                            plane is not changed.
*/
int pvr_set_bg_texture(pvr_ptr_t txr, int textureformat, int tw, int th,
                       pvr_filter_mode_t filtering);

/** \brief   Set cheap shadow parameters.
    \ingroup pvr_global

    This function sets up the PVR cheap shadow parameters for use. You can only
    specify one scale value per frame, so the effect that you can get from this
    is somewhat limited, but if you want simple shadows, this is the easiest way
    to do it.

    Polygons affected by a shadow modifier volume will effectively multiply
    their final color by the scale value set here when shadows are enabled and
    the polygon is inside the modifier (or outside for exclusion volumes).

    \param  enable          Set to true to enable cheap shadow mode.
    \param  scale_value     Floating point value (between 0 and 1) representing
                            how colors of polygons affected by and inside the
                            volume will be modified by the shadow volume.
*/
void pvr_set_shadow_scale(bool enable, float scale_value);

/** \brief   Set Z clipping depth.
    \ingroup pvr_global

    This function sets the Z clipping depth. The default value for this is
    0.0001.

    \param  zc              The new value to set the z clip parameter to.
*/
void pvr_set_zclip(float zc);

/** \brief   Set the vertical scale factor.
    \ingroup pvr_global

    This function sets the vertical scale factor used when the PVR scene is
    rendered to the framebuffer. Generally you want 1.0f or near-1.0f values
    here. The default used by the PVR driver is 1.0f when using VGA, and 0.999f
    otherwise. Having a value slightly below 1.0f gives the image a pleasant
    smoothing.

    \retval 0               On success
    \retval -1              On invalid factor value
*/
int pvr_set_vertical_scale(float factor);

/** \brief   Set the translucent polygon sort mode for the next frame.
    \ingroup pvr_scene_mgmt

    This function sets the translucent polygon sort mode for the next frame of
    output, potentially switching between autosort and presort mode.

    For most programs, you'll probably want to set this at initialization time
    (with the autosort_disabled field in the pvr_init_params_t structure) and
    not mess with it per-frame. It is recommended that if you do use this
    function to change the mode that you should set it each frame to ensure that
    the mode is set properly.

    \param  presort         Set to true to set the presort mode for translucent
                            polygons, set to false to use autosort mode.
*/
void pvr_set_presort_mode(bool presort);

/** \brief   Retrieve the current VBlank count.
    \ingroup pvr_stats

    This function retrieves the number of VBlank interrupts that have occurred
    since the PVR was initialized.

    \return                 The number of VBlanks since init
*/
int pvr_get_vbl_count(void);

/** \defgroup pvr_stats         Profiling
    \brief                      Rendering stats and metrics for profiling
    \ingroup                    pvr
*/

/** \brief   PVR statistics structure.
    \ingroup pvr_stats

    This structure is used to hold various statistics about the operation of the
    PVR since initialization.
*/
typedef struct pvr_stats {
    uint64_t frame_last_time;     /**< \brief Ready-to-Ready length for the last frame in nanoseconds */
    uint64_t reg_last_time;       /**< \brief Registration time for the last frame in nanoseconds */
    uint64_t rnd_last_time;       /**< \brief Rendering time for the last frame in nanoseconds */
    uint64_t buf_last_time;       /**< \brief DMA buffer file time for the last frame in nanoseconds */
    size_t   frame_count;         /**< \brief Total number of rendered/viewed frames */
    size_t   vbl_count;           /**< \brief VBlank count */
    size_t   vtx_buffer_used;     /**< \brief Number of bytes used in the vertex buffer for the last frame */
    size_t   vtx_buffer_used_max; /**< \brief Number of bytes used in the vertex buffer for the largest frame */
    float    frame_rate;          /**< \brief Current frame rate (per second) */
    uint32_t enabled_list_mask;   /**< \brief Which lists are enabled? */
    /* ... more later as it's implemented ... */
} pvr_stats_t;

/** \brief   Get the current statistics from the PVR.
    \ingroup pvr_stats

    This function fills in the pvr_stats_t structure passed in with the current
    statistics of the system.

    \param  stat            The statistics structure to fill in. Must not be
                            NULL
    \retval 0               On success
    \retval -1              If the PVR is not initialized
*/
int pvr_get_stats(pvr_stats_t *stat);

__END_DECLS

#endif /* __DC_PVR_PVR_MISC_H */
