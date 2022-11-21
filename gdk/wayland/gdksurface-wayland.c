/*
 * Copyright © 2010 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "gdksurface-wayland.h"

#include "gdkdeviceprivate.h"
#include "gdkdisplay-wayland.h"
#include "gdkdragsurfaceprivate.h"
#include "gdkeventsprivate.h"
#include "gdkframeclockidleprivate.h"
#include "gdkglcontext-wayland.h"
#include "gdkmonitor-wayland.h"
#include "gdkpopupprivate.h"
#include "gdkprivate-wayland.h"
#include "gdkprivate-wayland.h"
#include "gdkseat-wayland.h"
#include "gdksurfaceprivate.h"
#include "gdktoplevelprivate.h"
#include "gdkdevice-wayland-private.h"

#include <wayland/xdg-shell-unstable-v6-client-protocol.h>
#include <wayland/xdg-foreign-unstable-v2-client-protocol.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <netinet/in.h>
#include <unistd.h>

#include "gdksurface-wayland-private.h"
#include "gdktoplevel-wayland-private.h"

/**
 * GdkWaylandSurface:
 *
 * The Wayland implementation of `GdkSurface`.
 *
 * Beyond the [class@Gdk.Surface] API, the Wayland implementation offers
 * access to the Wayland `wl_surface` object with
 * [method@GdkWayland.WaylandSurface.get_wl_surface].
 */

G_DEFINE_TYPE (GdkWaylandSurface, gdk_wayland_surface, GDK_TYPE_SURFACE)

static void gdk_wayland_surface_maybe_resize (GdkSurface *surface,
                                              int         width,
                                              int         height,
                                              int         scale);

static void gdk_wayland_surface_configure (GdkSurface *surface);

static void gdk_wayland_surface_show (GdkSurface *surface);
static void gdk_wayland_surface_hide (GdkSurface *surface);

static void gdk_wayland_surface_sync_shadow (GdkSurface *surface);
static void gdk_wayland_surface_sync_input_region (GdkSurface *surface);
static void gdk_wayland_surface_sync_opaque_region (GdkSurface *surface);

static void gdk_wayland_surface_move_resize (GdkSurface *surface,
                                             int         x,
                                             int         y,
                                             int         width,
                                             int         height);

static void update_popup_layout_state (GdkWaylandPopup *wayland_popup,
                                       int              x,
                                       int              y,
                                       int              width,
                                       int              height,
                                       GdkPopupLayout  *layout);

static void configure_popup_geometry                     (GdkWaylandPopup *popup);
static void gdk_wayland_surface_configure_popup          (GdkWaylandPopup *popup);
static void frame_callback_popup                         (GdkWaylandPopup *popup);
static void gdk_wayland_popup_hide_surface               (GdkWaylandPopup *popup);

/* {{{ Utilities */

static void
fill_presentation_time_from_frame_time (GdkFrameTimings *timings,
                                        guint32          frame_time)
{
  /* The timestamp in a wayland frame is a msec time value that in some
   * way reflects the time at which the server started drawing the frame.
   * This is not useful from our perspective.
   *
   * However, for the DRM backend of Weston, on reasonably recent
   * Linux, we know that the time is the
   * clock_gettime (CLOCK_MONOTONIC) value at the vblank, and that
   * backend starts drawing immediately after receiving the vblank
   * notification. If we detect this, and make the assumption that the
   * compositor will finish drawing before the next vblank, we can
   * then determine the presentation time as the frame time we
   * received plus one refresh interval.
   *
   * If a backend is using clock_gettime(CLOCK_MONOTONIC), but not
   * picking values right at the vblank, then the presentation times
   * we compute won't be accurate, but not really worse than then
   * the alternative of not providing presentation times at all.
   *
   * The complexity here is dealing with the fact that we receive
   * only the low 32 bits of the CLOCK_MONOTONIC value in milliseconds.
   */
  gint64 now_monotonic = g_get_monotonic_time ();
  gint64 now_monotonic_msec = now_monotonic / 1000;
  uint32_t now_monotonic_low = (uint32_t)now_monotonic_msec;

  if (frame_time - now_monotonic_low < 1000 ||
      frame_time - now_monotonic_low > (uint32_t)-1000)
    {
      /* Timestamp we received is within one second of the current time.
       */
      gint64 last_frame_time = now_monotonic + (gint64)1000 * (gint32)(frame_time - now_monotonic_low);
      if ((gint32)now_monotonic_low < 0 && (gint32)frame_time > 0)
        last_frame_time += (gint64)1000 * G_GINT64_CONSTANT(0x100000000);
      else if ((gint32)now_monotonic_low > 0 && (gint32)frame_time < 0)
        last_frame_time -= (gint64)1000 * G_GINT64_CONSTANT(0x100000000);

      timings->presentation_time = last_frame_time + timings->refresh_interval;
    }
}

static const char *
get_default_title (void)
{
  const char *title;

  title = g_get_application_name ();
  if (!title)
    title = g_get_prgname ();
  if (!title)
    title = "";

  return title;
}

static gboolean
is_realized_shell_surface (GdkWaylandSurface *impl)
{
  return (impl->display_server.xdg_surface ||
          impl->display_server.zxdg_surface_v6);
}

static enum xdg_positioner_anchor
rect_anchor_to_anchor (GdkGravity rect_anchor)
{
  switch (rect_anchor)
    {
    case GDK_GRAVITY_NORTH_WEST:
    case GDK_GRAVITY_STATIC:
      return XDG_POSITIONER_ANCHOR_TOP_LEFT;
    case GDK_GRAVITY_NORTH:
      return XDG_POSITIONER_ANCHOR_TOP;
    case GDK_GRAVITY_NORTH_EAST:
      return XDG_POSITIONER_ANCHOR_TOP_RIGHT;
    case GDK_GRAVITY_WEST:
      return XDG_POSITIONER_ANCHOR_LEFT;
    case GDK_GRAVITY_CENTER:
      return XDG_POSITIONER_ANCHOR_NONE;
    case GDK_GRAVITY_EAST:
      return XDG_POSITIONER_ANCHOR_RIGHT;
    case GDK_GRAVITY_SOUTH_WEST:
      return XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
    case GDK_GRAVITY_SOUTH:
      return XDG_POSITIONER_ANCHOR_BOTTOM;
    case GDK_GRAVITY_SOUTH_EAST:
      return XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
    default:
      g_assert_not_reached ();
    }
}

static enum xdg_positioner_gravity
surface_anchor_to_gravity (GdkGravity rect_anchor)
{
  switch (rect_anchor)
    {
    case GDK_GRAVITY_NORTH_WEST:
    case GDK_GRAVITY_STATIC:
      return XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
    case GDK_GRAVITY_NORTH:
      return XDG_POSITIONER_GRAVITY_BOTTOM;
    case GDK_GRAVITY_NORTH_EAST:
      return XDG_POSITIONER_GRAVITY_BOTTOM_LEFT;
    case GDK_GRAVITY_WEST:
      return XDG_POSITIONER_GRAVITY_RIGHT;
    case GDK_GRAVITY_CENTER:
      return XDG_POSITIONER_GRAVITY_NONE;
    case GDK_GRAVITY_EAST:
      return XDG_POSITIONER_GRAVITY_LEFT;
    case GDK_GRAVITY_SOUTH_WEST:
      return XDG_POSITIONER_GRAVITY_TOP_RIGHT;
    case GDK_GRAVITY_SOUTH:
      return XDG_POSITIONER_GRAVITY_TOP;
    case GDK_GRAVITY_SOUTH_EAST:
      return XDG_POSITIONER_GRAVITY_TOP_LEFT;
    default:
      g_assert_not_reached ();
    }
}

static enum zxdg_positioner_v6_anchor
rect_anchor_to_anchor_legacy (GdkGravity rect_anchor)
{
  switch (rect_anchor)
    {
    case GDK_GRAVITY_NORTH_WEST:
    case GDK_GRAVITY_STATIC:
      return (ZXDG_POSITIONER_V6_ANCHOR_TOP |
              ZXDG_POSITIONER_V6_ANCHOR_LEFT);
    case GDK_GRAVITY_NORTH:
      return ZXDG_POSITIONER_V6_ANCHOR_TOP;
    case GDK_GRAVITY_NORTH_EAST:
      return (ZXDG_POSITIONER_V6_ANCHOR_TOP |
              ZXDG_POSITIONER_V6_ANCHOR_RIGHT);
    case GDK_GRAVITY_WEST:
      return ZXDG_POSITIONER_V6_ANCHOR_LEFT;
    case GDK_GRAVITY_CENTER:
      return ZXDG_POSITIONER_V6_ANCHOR_NONE;
    case GDK_GRAVITY_EAST:
      return ZXDG_POSITIONER_V6_ANCHOR_RIGHT;
    case GDK_GRAVITY_SOUTH_WEST:
      return (ZXDG_POSITIONER_V6_ANCHOR_BOTTOM |
              ZXDG_POSITIONER_V6_ANCHOR_LEFT);
    case GDK_GRAVITY_SOUTH:
      return ZXDG_POSITIONER_V6_ANCHOR_BOTTOM;
    case GDK_GRAVITY_SOUTH_EAST:
      return (ZXDG_POSITIONER_V6_ANCHOR_BOTTOM |
              ZXDG_POSITIONER_V6_ANCHOR_RIGHT);
    default:
      g_assert_not_reached ();
    }

  return (ZXDG_POSITIONER_V6_ANCHOR_TOP |
          ZXDG_POSITIONER_V6_ANCHOR_LEFT);
}

static enum zxdg_positioner_v6_gravity
surface_anchor_to_gravity_legacy (GdkGravity rect_anchor)
{
  switch (rect_anchor)
    {
    case GDK_GRAVITY_NORTH_WEST:
    case GDK_GRAVITY_STATIC:
      return (ZXDG_POSITIONER_V6_GRAVITY_BOTTOM |
              ZXDG_POSITIONER_V6_GRAVITY_RIGHT);
    case GDK_GRAVITY_NORTH:
      return ZXDG_POSITIONER_V6_GRAVITY_BOTTOM;
    case GDK_GRAVITY_NORTH_EAST:
      return (ZXDG_POSITIONER_V6_GRAVITY_BOTTOM |
              ZXDG_POSITIONER_V6_GRAVITY_LEFT);
    case GDK_GRAVITY_WEST:
      return ZXDG_POSITIONER_V6_GRAVITY_RIGHT;
    case GDK_GRAVITY_CENTER:
      return ZXDG_POSITIONER_V6_GRAVITY_NONE;
    case GDK_GRAVITY_EAST:
      return ZXDG_POSITIONER_V6_GRAVITY_LEFT;
    case GDK_GRAVITY_SOUTH_WEST:
      return (ZXDG_POSITIONER_V6_GRAVITY_TOP |
              ZXDG_POSITIONER_V6_GRAVITY_RIGHT);
    case GDK_GRAVITY_SOUTH:
      return ZXDG_POSITIONER_V6_GRAVITY_TOP;
    case GDK_GRAVITY_SOUTH_EAST:
      return (ZXDG_POSITIONER_V6_GRAVITY_TOP |
              ZXDG_POSITIONER_V6_GRAVITY_LEFT);
    default:
      g_assert_not_reached ();
    }

  return (ZXDG_POSITIONER_V6_GRAVITY_BOTTOM |
          ZXDG_POSITIONER_V6_GRAVITY_RIGHT);
}

static void
gdk_wayland_surface_get_window_geometry (GdkSurface   *surface,
                                         GdkRectangle *geometry)
{     
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);
      
  *geometry = (GdkRectangle) {
    .x = impl->shadow_left,
    .y = impl->shadow_top,
    .width = surface->width - (impl->shadow_left + impl->shadow_right),
    .height = surface->height - (impl->shadow_top + impl->shadow_bottom)
  };
}

static struct wl_region *
wl_region_from_cairo_region (GdkWaylandDisplay *display,
                             cairo_region_t    *region)
{     
  struct wl_region *wl_region;
  int i, n_rects;
  
  wl_region = wl_compositor_create_region (display->compositor);
  if (wl_region == NULL)
    return NULL;

  n_rects = cairo_region_num_rectangles (region);
  for (i = 0; i < n_rects; i++)
    {
      cairo_rectangle_int_t rect;
      cairo_region_get_rectangle (region, i, &rect);
      wl_region_add (wl_region, rect.x, rect.y, rect.width, rect.height);
    }

  return wl_region;
}

/* }}} */
/* {{{ Surface implementation */

static void
gdk_wayland_surface_init (GdkWaylandSurface *impl)
{
  impl->scale = 1;
  impl->saved_width = -1;
  impl->saved_height = -1;
}

static void
gdk_wayland_surface_freeze_state (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  impl->state_freeze_count++;
}

static void
gdk_wayland_surface_thaw_state (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  g_assert (impl->state_freeze_count > 0);

  impl->state_freeze_count--;

  if (impl->state_freeze_count > 0)
    return;

  if (impl->pending.is_dirty)
    gdk_wayland_surface_configure (surface);
}

void
_gdk_wayland_surface_save_size (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  if (surface->state & (GDK_TOPLEVEL_STATE_FULLSCREEN |
                        GDK_TOPLEVEL_STATE_MAXIMIZED |
                        GDK_TOPLEVEL_STATE_TILED))
    return;

  if (surface->width <= 1 || surface->height <= 1)
    return;

  impl->saved_width = surface->width - impl->shadow_left - impl->shadow_right;
  impl->saved_height = surface->height - impl->shadow_top - impl->shadow_bottom;
}

static void
_gdk_wayland_surface_clear_saved_size (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  if (surface->state & (GDK_TOPLEVEL_STATE_FULLSCREEN | GDK_TOPLEVEL_STATE_MAXIMIZED))
    return;

  impl->saved_width = -1;
  impl->saved_height = -1;
}

void
gdk_wayland_surface_update_size (GdkSurface *surface,
                                 int32_t     width,
                                 int32_t     height,
                                 int         scale)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);
  gboolean width_changed, height_changed, scale_changed;

  width_changed = surface->width != width;
  height_changed = surface->height != height;
  scale_changed = impl->scale != scale;

  if (!width_changed && !height_changed && !scale_changed)
    return;

  surface->width = width;
  surface->height = height;
  impl->scale = scale;

  if (impl->display_server.egl_window)
    wl_egl_window_resize (impl->display_server.egl_window, width * scale, height * scale, 0, 0);
  if (impl->display_server.wl_surface)
    wl_surface_set_buffer_scale (impl->display_server.wl_surface, scale);

  gdk_surface_invalidate_rect (surface, NULL);

  if (width_changed)
    g_object_notify (G_OBJECT (surface), "width");
  if (height_changed)
    g_object_notify (G_OBJECT (surface), "height");
  if (scale_changed)
    g_object_notify (G_OBJECT (surface), "scale-factor");

  _gdk_surface_update_size (surface);
}

static GdkSurface *
get_popup_toplevel (GdkSurface *surface)
{
  if (surface->parent)
    return get_popup_toplevel (surface->parent);
  else
    return surface;
}

static void
freeze_popup_toplevel_state (GdkWaylandPopup *wayland_popup)
{
  GdkSurface *toplevel;

  toplevel = get_popup_toplevel (GDK_SURFACE (wayland_popup));
  gdk_wayland_surface_freeze_state (toplevel);
}

static void
thaw_popup_toplevel_state (GdkWaylandPopup *wayland_popup)
{
  GdkSurface *toplevel;

  toplevel = get_popup_toplevel (GDK_SURFACE (wayland_popup));
  gdk_wayland_surface_thaw_state (toplevel);
}

static void
frame_callback (void               *data,
                struct wl_callback *callback,
                uint32_t            time)
{
  GdkSurface *surface = data;
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);
  GdkWaylandDisplay *display_wayland =
    GDK_WAYLAND_DISPLAY (gdk_surface_get_display (surface));
  GdkFrameClock *clock = gdk_surface_get_frame_clock (surface);
  GdkFrameTimings *timings;

  gdk_profiler_add_mark (GDK_PROFILER_CURRENT_TIME, 0, "wayland", "frame event");
  GDK_DISPLAY_DEBUG (GDK_DISPLAY (display_wayland), EVENTS, "frame %p", surface);

  wl_callback_destroy (callback);

  if (GDK_SURFACE_DESTROYED (surface))
    return;

  if (!impl->awaiting_frame)
    return;

  if (GDK_IS_WAYLAND_POPUP (surface))
    frame_callback_popup (GDK_WAYLAND_POPUP (surface));

  impl->awaiting_frame = FALSE;
  if (impl->awaiting_frame_frozen)
    {
      impl->awaiting_frame_frozen = FALSE;
      gdk_surface_thaw_updates (surface);
    }

  timings = gdk_frame_clock_get_timings (clock, impl->pending_frame_counter);
  impl->pending_frame_counter = 0;

  if (timings == NULL)
    return;

  timings->refresh_interval = 16667; /* default to 1/60th of a second */
  if (impl->display_server.outputs)
    {
      /* We pick a random output out of the outputs that the surface touches
       * The rate here is in milli-hertz */
      int refresh_rate =
        gdk_wayland_display_get_output_refresh_rate (display_wayland,
                                                     impl->display_server.outputs->data);
      if (refresh_rate != 0)
        timings->refresh_interval = G_GINT64_CONSTANT(1000000000) / refresh_rate;
    }

  fill_presentation_time_from_frame_time (timings, time);

  timings->complete = TRUE;

#ifdef G_ENABLE_DEBUG
  if ((_gdk_debug_flags & GDK_DEBUG_FRAMES) != 0)
    _gdk_frame_clock_debug_print_timings (clock, timings);
#endif

  if (GDK_PROFILER_IS_RUNNING)
    _gdk_frame_clock_add_timings_to_profiler (clock, timings);
}

static const struct wl_callback_listener frame_listener = {
  frame_callback
};

static void
on_frame_clock_before_paint (GdkFrameClock *clock,
                             GdkSurface     *surface)
{
  GdkFrameTimings *timings = gdk_frame_clock_get_current_timings (clock);
  gint64 presentation_time;
  gint64 refresh_interval;

  if (surface->update_freeze_count > 0)
    return;

  gdk_frame_clock_get_refresh_info (clock,
                                    timings->frame_time,
                                    &refresh_interval, &presentation_time);

  if (presentation_time != 0)
    {
      /* Assume the algorithm used by the DRM backend of Weston - it
       * starts drawing at the next vblank after receiving the commit
       * for this frame, and presentation occurs at the vblank
       * after that.
       */
      timings->predicted_presentation_time = presentation_time + refresh_interval;
    }
  else
    {
      /* As above, but we don't actually know the phase of the vblank,
       * so just assume that we're half way through a refresh cycle.
       */
      timings->predicted_presentation_time = timings->frame_time + refresh_interval / 2 + refresh_interval;
    }

  gdk_surface_apply_state_change (surface);
}

static void
configure_drag_surface_geometry (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  gdk_wayland_surface_update_size (surface,
                                   impl->next_layout.configured_width,
                                   impl->next_layout.configured_height,
                                   impl->scale);
}

static gboolean
gdk_wayland_surface_compute_size (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  if (impl->next_layout.surface_geometry_dirty)
    {
      if (GDK_IS_WAYLAND_TOPLEVEL (impl))
        configure_toplevel_geometry (GDK_WAYLAND_TOPLEVEL (surface));
      else if (GDK_IS_WAYLAND_POPUP (impl))
        configure_popup_geometry (GDK_WAYLAND_POPUP (surface));
      else if (GDK_IS_DRAG_SURFACE (impl))
        configure_drag_surface_geometry (surface);

      impl->next_layout.surface_geometry_dirty = FALSE;
    }

  return FALSE;
}

static void
gdk_wayland_surface_request_layout (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  impl->next_layout.surface_geometry_dirty = TRUE;
}

void
gdk_wayland_surface_request_frame (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);
  struct wl_callback *callback;
  GdkFrameClock *clock;

  if (impl->awaiting_frame)
    return;

  clock = gdk_surface_get_frame_clock (surface);

  callback = wl_surface_frame (impl->display_server.wl_surface);
  wl_proxy_set_queue ((struct wl_proxy *) callback, NULL);
  wl_callback_add_listener (callback, &frame_listener, surface);
  impl->pending_frame_counter = gdk_frame_clock_get_frame_counter (clock);
  impl->awaiting_frame = TRUE;
}

gboolean
gdk_wayland_surface_has_surface (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  return !!impl->display_server.wl_surface;
}

void
gdk_wayland_surface_commit (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  wl_surface_commit (impl->display_server.wl_surface);
}

void
gdk_wayland_surface_notify_committed (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  impl->has_uncommitted_ack_configure = FALSE;
}

static void
on_frame_clock_after_paint (GdkFrameClock *clock,
                            GdkSurface    *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  if (surface->update_freeze_count == 0 && impl->has_uncommitted_ack_configure)
    {
      gdk_wayland_surface_commit (surface);
      gdk_wayland_surface_notify_committed (surface);
    }

  if (impl->awaiting_frame &&
      impl->pending_frame_counter == gdk_frame_clock_get_frame_counter (clock))
    {
      impl->awaiting_frame_frozen = TRUE;
      gdk_surface_freeze_updates (surface);
    }
}

void
gdk_wayland_surface_update_scale (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);
  GdkWaylandDisplay *display_wayland = GDK_WAYLAND_DISPLAY (gdk_surface_get_display (surface));
  guint32 scale;
  GSList *l;

  if (display_wayland->compositor_version < WL_SURFACE_HAS_BUFFER_SCALE)
    {
      /* We can't set the scale on this surface */
      return;
    }

  if (!impl->display_server.outputs)
    {
      scale = impl->scale;
    }
  else
    {
      scale = 1;
      for (l = impl->display_server.outputs; l != NULL; l = l->next)
        {
          struct wl_output *output = l->data;
          uint32_t output_scale;

          output_scale = gdk_wayland_display_get_output_scale (display_wayland,
                                                               output);
          scale = MAX (scale, output_scale);
        }
    }

  /* Notify app that scale changed */
  gdk_wayland_surface_maybe_resize (surface,
                                    surface->width, surface->height,
                                    scale);
}

GdkSurface *
_gdk_wayland_display_create_surface (GdkDisplay     *display,
                                     GdkSurfaceType  surface_type,
                                     GdkSurface     *parent,
                                     int             x,
                                     int             y,
                                     int             width,
                                     int             height)
{
  GdkWaylandDisplay *display_wayland = GDK_WAYLAND_DISPLAY (display);
  GdkSurface *surface;
  GdkWaylandSurface *impl;
  GdkFrameClock *frame_clock;

  if (parent)
    frame_clock = g_object_ref (gdk_surface_get_frame_clock (parent));
  else
    frame_clock = _gdk_frame_clock_idle_new ();

  switch (surface_type)
    {
    case GDK_SURFACE_TOPLEVEL:
      surface = g_object_new (GDK_TYPE_WAYLAND_TOPLEVEL,
                              "display", display,
                              "frame-clock", frame_clock,
                              "title", get_default_title (),
                              NULL);
      display_wayland->toplevels = g_list_prepend (display_wayland->toplevels,
                                                   surface);
      g_warn_if_fail (!parent);
      break;
    case GDK_SURFACE_POPUP:
      surface = g_object_new (GDK_TYPE_WAYLAND_POPUP,
                              "parent", parent,
                              "display", display,
                              "frame-clock", frame_clock,
                              NULL);
      break;
    case GDK_SURFACE_DRAG:
      surface = g_object_new (GDK_TYPE_WAYLAND_DRAG_SURFACE,
                              "display", display,
                              "frame-clock", frame_clock,
                              NULL);
      break;
    default:
      g_assert_not_reached ();
      break;
    }

  impl = GDK_WAYLAND_SURFACE (surface);

  if (width > 65535)
    {
      g_warning ("Native Surfaces wider than 65535 pixels are not supported");
      width = 65535;
    }
  if (height > 65535)
    {
      g_warning ("Native Surfaces taller than 65535 pixels are not supported");
      height = 65535;
    }

  surface->x = x;
  surface->y = y;
  surface->width = width;
  surface->height = height;

  g_object_ref (surface);

  /* More likely to be right than just assuming 1 */
  if (display_wayland->compositor_version >= WL_SURFACE_HAS_BUFFER_SCALE)
    {
      GdkMonitor *monitor = g_list_model_get_item (gdk_display_get_monitors (display), 0);
      if (monitor)
        {
          impl->scale = gdk_monitor_get_scale_factor (monitor);
          g_object_unref (monitor);
        }
    }

  gdk_wayland_surface_create_wl_surface (surface);

  g_signal_connect (frame_clock, "before-paint", G_CALLBACK (on_frame_clock_before_paint), surface);
  g_signal_connect (frame_clock, "after-paint", G_CALLBACK (on_frame_clock_after_paint), surface);

  g_object_unref (frame_clock);

  return surface;
}

void
gdk_wayland_surface_attach_image (GdkSurface           *surface,
                                  cairo_surface_t      *cairo_surface,
                                  const cairo_region_t *damage)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);
  GdkWaylandDisplay *display;
  cairo_rectangle_int_t rect;
  int i, n;

  if (GDK_SURFACE_DESTROYED (surface))
    return;

  g_assert (_gdk_wayland_is_shm_surface (cairo_surface));

  /* Attach this new buffer to the surface */
  wl_surface_attach (impl->display_server.wl_surface,
                     _gdk_wayland_shm_surface_get_wl_buffer (cairo_surface),
                     impl->pending_buffer_offset_x,
                     impl->pending_buffer_offset_y);
  impl->pending_buffer_offset_x = 0;
  impl->pending_buffer_offset_y = 0;

  /* Only set the buffer scale if supported by the compositor */
  display = GDK_WAYLAND_DISPLAY (gdk_surface_get_display (surface));
  if (display->compositor_version >= WL_SURFACE_HAS_BUFFER_SCALE)
    wl_surface_set_buffer_scale (impl->display_server.wl_surface, impl->scale);

  n = cairo_region_num_rectangles (damage);
  for (i = 0; i < n; i++)
    {
      cairo_region_get_rectangle (damage, i, &rect);
      wl_surface_damage (impl->display_server.wl_surface, rect.x, rect.y, rect.width, rect.height);
    }
}

static void
gdk_wayland_surface_sync_offset (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  if (wl_surface_get_version (impl->display_server.wl_surface) <
      WL_SURFACE_OFFSET_SINCE_VERSION)
    return;

  if (impl->pending_buffer_offset_x == 0 &&
      impl->pending_buffer_offset_y == 0)
    return;

  wl_surface_offset (impl->display_server.wl_surface,
                     impl->pending_buffer_offset_x,
                     impl->pending_buffer_offset_y);
  impl->pending_buffer_offset_x = 0;
  impl->pending_buffer_offset_y = 0;
}

void
gdk_wayland_surface_sync (GdkSurface *surface)
{
  gdk_wayland_surface_sync_shadow (surface);
  gdk_wayland_surface_sync_opaque_region (surface);
  gdk_wayland_surface_sync_input_region (surface);
  gdk_wayland_surface_sync_offset (surface);
}

static gboolean
gdk_wayland_surface_beep (GdkSurface *surface)
{
  gdk_wayland_display_system_bell (gdk_surface_get_display (surface), surface);

  return TRUE;
}

static void
gdk_wayland_surface_constructed (GObject *object)
{
  GdkSurface *surface = GDK_SURFACE (object);
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);
  GdkWaylandDisplay *display_wayland =
    GDK_WAYLAND_DISPLAY (gdk_surface_get_display (surface));

  G_OBJECT_CLASS (gdk_wayland_surface_parent_class)->constructed (object);

  impl->event_queue = wl_display_create_queue (display_wayland->wl_display);
  display_wayland->event_queues = g_list_prepend (display_wayland->event_queues,
                                                  impl->event_queue);
}

static void
gdk_wayland_surface_dispose (GObject *object)
{
  GdkSurface *surface = GDK_SURFACE (object);
  GdkWaylandSurface *impl;

  g_return_if_fail (GDK_IS_WAYLAND_SURFACE (surface));

  impl = GDK_WAYLAND_SURFACE (surface);

  if (impl->event_queue)
    {
      GdkWaylandDisplay *display_wayland =
        GDK_WAYLAND_DISPLAY (gdk_surface_get_display (surface));

      display_wayland->event_queues =
        g_list_remove (display_wayland->event_queues, surface);
      g_clear_pointer (&impl->event_queue, wl_event_queue_destroy);
    }

  G_OBJECT_CLASS (gdk_wayland_surface_parent_class)->dispose (object);
}

static void
gdk_wayland_surface_finalize (GObject *object)
{
  GdkWaylandSurface *impl;

  g_return_if_fail (GDK_IS_WAYLAND_SURFACE (object));

  impl = GDK_WAYLAND_SURFACE (object);

  g_clear_pointer (&impl->opaque_region, cairo_region_destroy);
  g_clear_pointer (&impl->input_region, cairo_region_destroy);

  G_OBJECT_CLASS (gdk_wayland_surface_parent_class)->finalize (object);
}

static void
gdk_wayland_surface_maybe_resize (GdkSurface *surface,
                                  int         width,
                                  int         height,
                                  int         scale)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);
  gboolean is_xdg_popup;
  gboolean is_visible;

  if (surface->width == width &&
      surface->height == height &&
      impl->scale == scale)
    return;

  /* For xdg_popup using an xdg_positioner, there is a race condition if
   * the application tries to change the size after it's mapped, but before
   * the initial configure is received, so hide and show the surface again
   * force the new size onto the compositor. See bug #772505.
   */

  is_xdg_popup = GDK_IS_WAYLAND_POPUP (surface);
  is_visible = gdk_surface_get_mapped (surface);

  if (is_xdg_popup && is_visible && !impl->initial_configure_received)
    gdk_wayland_surface_hide (surface);

  gdk_wayland_surface_update_size (surface, width, height, scale);

  if (is_xdg_popup && is_visible && !impl->initial_configure_received)
    gdk_wayland_surface_show (surface);
}

static void
gdk_wayland_surface_sync_shadow (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);
  GdkWaylandDisplay *display_wayland = GDK_WAYLAND_DISPLAY (gdk_surface_get_display (surface));
  GdkRectangle geometry;

  if (!is_realized_shell_surface (impl))
    return;

  gdk_wayland_surface_get_window_geometry (surface, &geometry);
  if (GDK_IS_WAYLAND_TOPLEVEL (impl))
    {
      GdkWaylandToplevel *toplevel = GDK_WAYLAND_TOPLEVEL (impl);
      gdk_wayland_toplevel_set_geometry_hints (toplevel, NULL, 0);
    }

  if (gdk_rectangle_equal (&geometry, &impl->last_sent_window_geometry))
    return;

  switch (display_wayland->shell_variant)
    {
    case GDK_WAYLAND_SHELL_VARIANT_XDG_SHELL:
      xdg_surface_set_window_geometry (impl->display_server.xdg_surface,
                                       geometry.x,
                                       geometry.y,
                                       geometry.width,
                                       geometry.height);
      break;
    case GDK_WAYLAND_SHELL_VARIANT_ZXDG_SHELL_V6:
      zxdg_surface_v6_set_window_geometry (impl->display_server.zxdg_surface_v6,
                                           geometry.x,
                                           geometry.y,
                                           geometry.width,
                                           geometry.height);
      break;
    default:
      g_assert_not_reached ();
    }

  impl->last_sent_window_geometry = geometry;
}

static void
gdk_wayland_surface_sync_opaque_region (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);
  struct wl_region *wl_region = NULL;

  if (!impl->display_server.wl_surface)
    return;

  if (!impl->opaque_region_dirty)
    return;

  if (impl->opaque_region != NULL)
    wl_region = wl_region_from_cairo_region (GDK_WAYLAND_DISPLAY (gdk_surface_get_display (surface)),
                                             impl->opaque_region);

  wl_surface_set_opaque_region (impl->display_server.wl_surface, wl_region);

  if (wl_region != NULL)
    wl_region_destroy (wl_region);

  impl->opaque_region_dirty = FALSE;
}

static void
gdk_wayland_surface_sync_input_region (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);
  struct wl_region *wl_region = NULL;

  if (!impl->display_server.wl_surface)
    return;

  if (!impl->input_region_dirty)
    return;

  if (impl->input_region != NULL)
    wl_region = wl_region_from_cairo_region (GDK_WAYLAND_DISPLAY (gdk_surface_get_display (surface)),
                                             impl->input_region);

  wl_surface_set_input_region (impl->display_server.wl_surface, wl_region);

  if (wl_region != NULL)
    wl_region_destroy (wl_region);

  impl->input_region_dirty = FALSE;
}

static void
surface_enter (void              *data,
               struct wl_surface *wl_surface,
               struct wl_output  *output)
{
  GdkSurface *surface = GDK_SURFACE (data);
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);
  GdkDisplay *display = gdk_surface_get_display (surface);
  GdkMonitor *monitor;

  GDK_DISPLAY_DEBUG(gdk_surface_get_display (surface), EVENTS,
                    "surface enter, surface %p output %p", surface, output);

  impl->display_server.outputs = g_slist_prepend (impl->display_server.outputs, output);

  gdk_wayland_surface_update_scale (surface);

  monitor = gdk_wayland_display_get_monitor_for_output (display, output);
  gdk_surface_enter_monitor (surface, monitor);
}

static void
surface_leave (void              *data,
               struct wl_surface *wl_surface,
               struct wl_output  *output)
{
  GdkSurface *surface = GDK_SURFACE (data);
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);
  GdkDisplay *display = gdk_surface_get_display (surface);
  GdkMonitor *monitor;

  GDK_DISPLAY_DEBUG (gdk_surface_get_display (surface), EVENTS,
                     "surface leave, surface %p output %p", surface, output);

  impl->display_server.outputs = g_slist_remove (impl->display_server.outputs, output);

  if (impl->display_server.outputs)
    gdk_wayland_surface_update_scale (surface);

  monitor = gdk_wayland_display_get_monitor_for_output (display, output);
  gdk_surface_leave_monitor (surface, monitor);
}

static const struct wl_surface_listener surface_listener = {
  surface_enter,
  surface_leave
};

void
gdk_wayland_surface_create_wl_surface (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);
  GdkWaylandDisplay *display_wayland = GDK_WAYLAND_DISPLAY (gdk_surface_get_display (surface));
  struct wl_surface *wl_surface;

  wl_surface = wl_compositor_create_surface (display_wayland->compositor);
  wl_proxy_set_queue ((struct wl_proxy *) wl_surface, impl->event_queue);
  wl_surface_add_listener (wl_surface, &surface_listener, surface);

  impl->display_server.wl_surface = wl_surface;
}

static void
maybe_notify_mapped (GdkSurface *surface)
{
  if (surface->destroyed)
    return;

  if (!GDK_SURFACE_IS_MAPPED (surface))
    gdk_surface_set_is_mapped (surface, TRUE);
}

static void
gdk_wayland_surface_configure (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  if (!impl->initial_configure_received)
    {
      gdk_surface_thaw_updates (surface);
      impl->initial_configure_received = TRUE;
      impl->pending.is_initial_configure = TRUE;
      maybe_notify_mapped (surface);
    }

  impl->has_uncommitted_ack_configure = TRUE;

  if (GDK_IS_WAYLAND_POPUP (surface))
    gdk_wayland_surface_configure_popup (GDK_WAYLAND_POPUP (surface));
  else if (GDK_IS_WAYLAND_TOPLEVEL (surface))
    gdk_wayland_surface_configure_toplevel (GDK_WAYLAND_TOPLEVEL (surface));
  else
    g_warn_if_reached ();

  impl->last_configure_serial = impl->pending.serial;

  memset (&impl->pending, 0, sizeof (impl->pending));
}

static void
gdk_wayland_surface_handle_configure (GdkWaylandSurface *impl,
                                      uint32_t           serial)
{
  impl->pending.is_dirty = TRUE;
  impl->pending.serial = serial;

  if (impl->state_freeze_count > 0)
    return;

  gdk_wayland_surface_configure (GDK_SURFACE (impl));
}

static void
xdg_surface_configure (void               *data,
                       struct xdg_surface *xdg_surface,
                       uint32_t            serial)
{
  gdk_wayland_surface_handle_configure (GDK_WAYLAND_SURFACE (data), serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
  xdg_surface_configure,
};

static void
zxdg_surface_v6_configure (void                   *data,
                           struct zxdg_surface_v6 *xdg_surface,
                           uint32_t                serial)
{
  gdk_wayland_surface_handle_configure (GDK_WAYLAND_SURFACE (data), serial);
}

static const struct zxdg_surface_v6_listener zxdg_surface_v6_listener = {
  zxdg_surface_v6_configure,
};

void
gdk_wayland_surface_create_xdg_surface_resources (GdkSurface *surface)
{
  GdkWaylandDisplay *display =
    GDK_WAYLAND_DISPLAY (gdk_surface_get_display (surface));
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  switch (display->shell_variant)
    {
    case GDK_WAYLAND_SHELL_VARIANT_XDG_SHELL:
      impl->display_server.xdg_surface =
        xdg_wm_base_get_xdg_surface (display->xdg_wm_base,
                                     impl->display_server.wl_surface);
      wl_proxy_set_queue ((struct wl_proxy *) impl->display_server.xdg_surface,
                          impl->event_queue);
      xdg_surface_add_listener (impl->display_server.xdg_surface,
                                &xdg_surface_listener,
                                surface);
      break;
    case GDK_WAYLAND_SHELL_VARIANT_ZXDG_SHELL_V6:
      impl->display_server.zxdg_surface_v6 =
        zxdg_shell_v6_get_xdg_surface (display->zxdg_shell_v6,
                                       impl->display_server.wl_surface);
      zxdg_surface_v6_add_listener (impl->display_server.zxdg_surface_v6,
                                    &zxdg_surface_v6_listener,
                                    surface);
      break;
    default:
      g_assert_not_reached ();
    }
}

static void
gdk_wayland_surface_map_toplevel (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  if (!GDK_IS_WAYLAND_TOPLEVEL (surface))
    return;

  if (impl->mapped)
    return;

  gdk_wayland_surface_create_xdg_toplevel (GDK_WAYLAND_TOPLEVEL (surface));

  impl->mapped = TRUE;
}

static void
gdk_wayland_surface_show (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  if (!impl->display_server.wl_surface)
    gdk_wayland_surface_create_wl_surface (surface);

  gdk_wayland_surface_map_toplevel (surface);
}

static void
unmap_popups_for_surface (GdkSurface *surface)
{
  GdkWaylandDisplay *display_wayland;
  GList *l;

  display_wayland = GDK_WAYLAND_DISPLAY (gdk_surface_get_display (surface));
  for (l = display_wayland->current_popups; l; l = l->next)
    {
       GdkSurface *popup = l->data;

       if (popup->parent == surface)
         {
           g_warning ("Tried to unmap the parent of a popup");
           gdk_surface_hide (popup);

           return;
         }
    }
}

static void
gdk_wayland_surface_hide_surface (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  unmap_popups_for_surface (surface);

  if (impl->display_server.wl_surface)
    {
      if (impl->display_server.egl_window)
        {
          gdk_surface_set_egl_native_window (surface, NULL);
          wl_egl_window_destroy (impl->display_server.egl_window);
          impl->display_server.egl_window = NULL;
        }

      if (impl->display_server.xdg_surface)
        {
          xdg_surface_destroy (impl->display_server.xdg_surface);
          impl->display_server.xdg_surface = NULL;
          if (!impl->initial_configure_received)
            gdk_surface_thaw_updates (surface);
          else
            impl->initial_configure_received = FALSE;
        }
      if (impl->display_server.zxdg_surface_v6)
        {
          g_clear_pointer (&impl->display_server.zxdg_surface_v6, zxdg_surface_v6_destroy);
          if (!impl->initial_configure_received)
            gdk_surface_thaw_updates (surface);
          else
            impl->initial_configure_received = FALSE;
        }

      impl->awaiting_frame = FALSE;
      if (impl->awaiting_frame_frozen)
        {
          impl->awaiting_frame_frozen = FALSE;
          gdk_surface_thaw_updates (surface);
        }

      if (GDK_IS_WAYLAND_TOPLEVEL (surface))
        gdk_wayland_toplevel_hide_surface (GDK_WAYLAND_TOPLEVEL (surface));

      if (GDK_IS_WAYLAND_POPUP (surface))
        gdk_wayland_popup_hide_surface (GDK_WAYLAND_POPUP (surface));

      g_clear_pointer (&impl->display_server.wl_surface, wl_surface_destroy);

      g_slist_free (impl->display_server.outputs);
      impl->display_server.outputs = NULL;
    }

  impl->has_uncommitted_ack_configure = FALSE;
  impl->input_region_dirty = TRUE;
  impl->opaque_region_dirty = TRUE;

  unset_transient_for_exported (surface);

  impl->last_sent_window_geometry = (GdkRectangle) { 0 };

  _gdk_wayland_surface_clear_saved_size (surface);
  impl->mapped = FALSE;
}

static void
gdk_wayland_surface_hide (GdkSurface *surface)
{
  GdkSeat *seat;

  seat = gdk_display_get_default_seat (surface->display);
  if (seat)
    {
      if (surface->autohide)
        gdk_seat_ungrab (seat);

      gdk_wayland_seat_clear_touchpoints (GDK_WAYLAND_SEAT (seat), surface);
    }
  gdk_wayland_surface_hide_surface (surface);
  _gdk_surface_clear_update_area (surface);
}

static void
gdk_wayland_surface_move_resize (GdkSurface *surface,
                                 int         x,
                                 int         y,
                                 int         width,
                                 int         height)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  surface->x = x;
  surface->y = y;
  gdk_wayland_surface_maybe_resize (surface, width, height, impl->scale);
}

static void
gdk_wayland_surface_get_geometry (GdkSurface *surface,
                                  int        *x,
                                  int        *y,
                                  int        *width,
                                  int        *height)
{
  if (!GDK_SURFACE_DESTROYED (surface))
    {
      if (x)
        *x = surface->x;
      if (y)
        *y = surface->y;
      if (width)
        *width = surface->width;
      if (height)
        *height = surface->height;
    }
}

static void
gdk_wayland_surface_get_root_coords (GdkSurface *surface,
                                     int         x,
                                     int         y,
                                     int        *root_x,
                                     int        *root_y)
{
  /*
   * Wayland does not have a global coordinate space shared between surfaces. In
   * fact, for regular toplevels, we have no idea where our surfaces are
   * positioned, relatively.
   *
   * However, there are some cases like popups and subsurfaces where we do have
   * some amount of control over the placement of our surface, and we can
   * semi-accurately control the x/y position of these surfaces, if they are
   * relative to another surface.
   *
   * To pretend we have something called a root coordinate space, assume all
   * parent-less surfaces are positioned in (0, 0), and all relative positioned
   * popups and subsurfaces are placed within this fake root coordinate space.
   *
   * For example a 200x200 large toplevel surface will have the position (0, 0).
   * If a popup positioned in the middle of the toplevel will have the fake
   * position (100,100). Furthermore, if a positioned is placed in the middle
   * that popup, will have the fake position (150,150), even though it has the
   * relative position (50,50). These three surfaces would make up one single
   * fake root coordinate space.
   */

  if (root_x)
    *root_x = surface->x + x;

  if (root_y)
    *root_y = surface->y + y;
}

static gboolean
gdk_wayland_surface_get_device_state (GdkSurface       *surface,
                                      GdkDevice        *device,
                                      double           *x,
                                      double           *y,
                                      GdkModifierType  *mask)
{
  if (GDK_SURFACE_DESTROYED (surface))
    return FALSE;

  gdk_wayland_device_query_state (device, surface, x, y, mask);

  return *x >= 0 && *y >= 0 && *x < surface->width && *y < surface->height;
}

static void
gdk_wayland_surface_set_input_region (GdkSurface     *surface,
                                      cairo_region_t *input_region)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  if (GDK_SURFACE_DESTROYED (surface))
    return;

  g_clear_pointer (&impl->input_region, cairo_region_destroy);

  if (input_region)
    impl->input_region = cairo_region_copy (input_region);

  impl->input_region_dirty = TRUE;
}

static void
gdk_wayland_surface_destroy (GdkSurface *surface,
                             gboolean    foreign_destroy)
{
  GdkWaylandDisplay *display;
  GdkFrameClock *frame_clock;

  g_return_if_fail (GDK_IS_SURFACE (surface));

  /* Wayland surfaces can't be externally destroyed; we may possibly
   * eventually want to use this path at display close-down
   */
  g_return_if_fail (!foreign_destroy);

  gdk_wayland_surface_hide_surface (surface);

  frame_clock = gdk_surface_get_frame_clock (surface);
  g_signal_handlers_disconnect_by_func (frame_clock, on_frame_clock_before_paint, surface);
  g_signal_handlers_disconnect_by_func (frame_clock, on_frame_clock_after_paint, surface);

  display = GDK_WAYLAND_DISPLAY (gdk_surface_get_display (surface));
  display->toplevels = g_list_remove (display->toplevels, surface);
}

static void
gdk_wayland_surface_destroy_notify (GdkSurface *surface)
{
  if (!GDK_SURFACE_DESTROYED (surface))
    {
      g_warning ("GdkSurface %p unexpectedly destroyed", surface);
      _gdk_surface_destroy (surface, TRUE);
    }

  g_object_unref (surface);
}

static int
gdk_wayland_surface_get_scale_factor (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  if (GDK_SURFACE_DESTROYED (surface))
    return 1;

  return impl->scale;
}

static void
gdk_wayland_surface_set_opaque_region (GdkSurface     *surface,
                                       cairo_region_t *region)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  if (GDK_SURFACE_DESTROYED (surface))
    return;

  g_clear_pointer (&impl->opaque_region, cairo_region_destroy);
  impl->opaque_region = cairo_region_reference (region);
  impl->opaque_region_dirty = TRUE;
}

static void
gdk_wayland_surface_class_init (GdkWaylandSurfaceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GdkSurfaceClass *impl_class = GDK_SURFACE_CLASS (klass);

  object_class->constructed = gdk_wayland_surface_constructed;
  object_class->dispose = gdk_wayland_surface_dispose;
  object_class->finalize = gdk_wayland_surface_finalize;

  impl_class->hide = gdk_wayland_surface_hide;
  impl_class->get_geometry = gdk_wayland_surface_get_geometry;
  impl_class->get_root_coords = gdk_wayland_surface_get_root_coords;
  impl_class->get_device_state = gdk_wayland_surface_get_device_state;
  impl_class->set_input_region = gdk_wayland_surface_set_input_region;
  impl_class->destroy = gdk_wayland_surface_destroy;
  impl_class->beep = gdk_wayland_surface_beep;

  impl_class->destroy_notify = gdk_wayland_surface_destroy_notify;
  impl_class->drag_begin = _gdk_wayland_surface_drag_begin;
  impl_class->get_scale_factor = gdk_wayland_surface_get_scale_factor;
  impl_class->set_opaque_region = gdk_wayland_surface_set_opaque_region;
  impl_class->request_layout = gdk_wayland_surface_request_layout;
  impl_class->compute_size = gdk_wayland_surface_compute_size;
}

/* }}} */
/* {{{ Private Surface API */

struct wl_output *
gdk_wayland_surface_get_wl_output (GdkSurface *surface)
{
  GdkWaylandSurface *impl;

  g_return_val_if_fail (GDK_IS_WAYLAND_SURFACE (surface), NULL);

  impl = GDK_WAYLAND_SURFACE (surface);
  /* We pick the head of the list as this is the last entered output */
  if (impl->display_server.outputs)
    return (struct wl_output *) impl->display_server.outputs->data;

  return NULL;
}

void
_gdk_wayland_surface_offset_next_wl_buffer (GdkSurface *surface,
                                            int         x,
                                            int         y)
{
  GdkWaylandSurface *impl;

  g_return_if_fail (GDK_IS_WAYLAND_SURFACE (surface));

  impl = GDK_WAYLAND_SURFACE (surface);

  impl->pending_buffer_offset_x = x;
  impl->pending_buffer_offset_y = y;
}

void
gdk_wayland_surface_ensure_wl_egl_window (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  if (impl->display_server.egl_window == NULL)
    {
      impl->display_server.egl_window =
        wl_egl_window_create (impl->display_server.wl_surface,
                              surface->width * impl->scale,
                              surface->height * impl->scale);
      wl_surface_set_buffer_scale (impl->display_server.wl_surface, impl->scale);

      gdk_surface_set_egl_native_window (surface, impl->display_server.egl_window);
    }
}

/* }}} */
/* {{{ Surface API */

/**
 * gdk_wayland_surface_get_wl_surface: (skip)
 * @surface: (type GdkWaylandSurface): a `GdkSurface`
 *
 * Returns the Wayland `wl_surface` of a `GdkSurface`.
 *
 * Returns: (transfer none): a Wayland `wl_surface`
 */
struct wl_surface *
gdk_wayland_surface_get_wl_surface (GdkSurface *surface)
{
  g_return_val_if_fail (GDK_IS_WAYLAND_SURFACE (surface), NULL);

  return GDK_WAYLAND_SURFACE (surface)->display_server.wl_surface;
}

/* }}}} */
/* {{{ GdkWaylandPopup definition */

/**
 * GdkWaylandPopup:
 *
 * The Wayland implementation of `GdkPopup`.
 */

struct _GdkWaylandPopup
{
  GdkWaylandSurface parent_instance;

  struct {
    struct xdg_popup *xdg_popup;
    struct zxdg_popup_v6 *zxdg_popup_v6;
  } display_server;

  PopupState state;
  unsigned int thaw_upon_show : 1;
  GdkPopupLayout *layout;
  int unconstrained_width;
  int unconstrained_height;

  struct {
    int x;
    int y;
    int width;
    int height;
    uint32_t repositioned_token;
    gboolean has_repositioned_token;
  } pending;

  struct {
    int x;
    int y;
  } next_layout;

  uint32_t reposition_token;
  uint32_t received_reposition_token;

  GdkSeat *grab_input_seat;
};

typedef struct
{
  GdkWaylandSurfaceClass parent_class;
} GdkWaylandPopupClass;

static void gdk_wayland_popup_iface_init (GdkPopupInterface *iface);

G_DEFINE_TYPE_WITH_CODE (GdkWaylandPopup, gdk_wayland_popup, GDK_TYPE_WAYLAND_SURFACE,
                         G_IMPLEMENT_INTERFACE (GDK_TYPE_POPUP,
                                                gdk_wayland_popup_iface_init))

/* }}} */
/* {{{ Popup implementation */

static void
gdk_wayland_popup_hide_surface (GdkWaylandPopup *popup)
{
  GdkSurface *surface = GDK_SURFACE (popup);
  GdkDisplay *display = gdk_surface_get_display (surface);
  GdkWaylandDisplay *display_wayland = GDK_WAYLAND_DISPLAY (display);

  g_clear_pointer (&popup->display_server.xdg_popup, xdg_popup_destroy);
  g_clear_pointer (&popup->display_server.zxdg_popup_v6, zxdg_popup_v6_destroy);
  display_wayland->current_popups =
      g_list_remove (display_wayland->current_popups, surface);
  display_wayland->current_grabbing_popups =
      g_list_remove (display_wayland->current_grabbing_popups, surface);

  popup->thaw_upon_show = TRUE;
  gdk_surface_freeze_updates (surface);

  switch (popup->state)
    {
    case POPUP_STATE_WAITING_FOR_REPOSITIONED:
      gdk_surface_thaw_updates (surface);
      G_GNUC_FALLTHROUGH;
    case POPUP_STATE_WAITING_FOR_CONFIGURE:
    case POPUP_STATE_WAITING_FOR_FRAME:
      thaw_popup_toplevel_state (popup);
      break;
    case POPUP_STATE_IDLE:
      break;
    default:
      g_assert_not_reached ();
    }

  popup->state = POPUP_STATE_IDLE;

  g_clear_pointer (&popup->layout, gdk_popup_layout_unref);
}

static gboolean
is_realized_popup (GdkWaylandSurface *impl)
{
  GdkWaylandPopup *popup;

  if (!GDK_IS_WAYLAND_POPUP (impl))
    return FALSE;

  popup = GDK_WAYLAND_POPUP (impl);

  return (popup->display_server.xdg_popup ||
          popup->display_server.zxdg_popup_v6);
}

static void
finish_pending_relayout (GdkWaylandPopup *wayland_popup)
{
  g_assert (wayland_popup->state == POPUP_STATE_WAITING_FOR_FRAME);
  wayland_popup->state = POPUP_STATE_IDLE;

  thaw_popup_toplevel_state (wayland_popup);
}

static void
frame_callback_popup (GdkWaylandPopup *wayland_popup)
{
      switch (wayland_popup->state)
        {
        case POPUP_STATE_IDLE:
        case POPUP_STATE_WAITING_FOR_REPOSITIONED:
        case POPUP_STATE_WAITING_FOR_CONFIGURE:
          break;
        case POPUP_STATE_WAITING_FOR_FRAME:
          finish_pending_relayout (wayland_popup);
          break;
        default:
          g_assert_not_reached ();
        }
}

static void
configure_popup_geometry (GdkWaylandPopup *wayland_popup)
{
  GdkWaylandSurface *wayland_surface = GDK_WAYLAND_SURFACE (wayland_popup);
  int x, y;
  int width, height;

  x = wayland_popup->next_layout.x - wayland_surface->shadow_left;
  y = wayland_popup->next_layout.y - wayland_surface->shadow_top;
  width =
    wayland_surface->next_layout.configured_width +
    (wayland_surface->shadow_left + wayland_surface->shadow_right);
  height =
    wayland_surface->next_layout.configured_height +
    (wayland_surface->shadow_top + wayland_surface->shadow_bottom);

  gdk_wayland_surface_move_resize (GDK_SURFACE (wayland_popup), x, y, width, height);
}

static void
gdk_wayland_surface_configure_popup (GdkWaylandPopup *wayland_popup)
{
  GdkSurface *surface = GDK_SURFACE (wayland_popup);
  GdkWaylandSurface *wayland_surface = GDK_WAYLAND_SURFACE (wayland_popup);
  GdkRectangle parent_geometry;
  int x, y, width, height;

  if (wayland_popup->display_server.xdg_popup)
    {
      xdg_surface_ack_configure (wayland_surface->display_server.xdg_surface,
                                 wayland_surface->pending.serial);
    }
  else if (wayland_popup->display_server.zxdg_popup_v6)
    {
      zxdg_surface_v6_ack_configure (wayland_surface->display_server.zxdg_surface_v6,
                                     wayland_surface->pending.serial);
    }
  else
    g_warn_if_reached ();

  if (wayland_popup->pending.has_repositioned_token)
    wayland_popup->received_reposition_token = wayland_popup->pending.repositioned_token;

  switch (wayland_popup->state)
    {
    case POPUP_STATE_WAITING_FOR_REPOSITIONED:
      if (wayland_popup->received_reposition_token != wayland_popup->reposition_token)
        return;
      else
        gdk_surface_thaw_updates (surface);
      G_GNUC_FALLTHROUGH;
    case POPUP_STATE_WAITING_FOR_CONFIGURE:
      wayland_popup->state = POPUP_STATE_WAITING_FOR_FRAME;
      break;
    case POPUP_STATE_IDLE:
    case POPUP_STATE_WAITING_FOR_FRAME:
      break;
    default:
      g_assert_not_reached ();
    }

  x = wayland_popup->pending.x;
  y = wayland_popup->pending.y;
  width = wayland_popup->pending.width;
  height = wayland_popup->pending.height;

  gdk_wayland_surface_get_window_geometry (surface->parent, &parent_geometry);
  x += parent_geometry.x;
  y += parent_geometry.y;

  update_popup_layout_state (wayland_popup,
                             x, y,
                             width, height,
                             wayland_popup->layout);

  wayland_popup->next_layout.x = x;
  wayland_popup->next_layout.y = y;
  wayland_surface->next_layout.configured_width = width;
  wayland_surface->next_layout.configured_height = height;
  wayland_surface->next_layout.surface_geometry_dirty = TRUE;
  gdk_surface_request_layout (surface);
}

static void
gdk_wayland_surface_handle_configure_popup (GdkWaylandPopup *wayland_popup,
                                            int32_t          x,
                                            int32_t          y,
                                            int32_t          width,
                                            int32_t          height)
{
  wayland_popup->pending.x = x;
  wayland_popup->pending.y = y;
  wayland_popup->pending.width = width;
  wayland_popup->pending.height = height;
}

static void
xdg_popup_configure (void             *data,
                     struct xdg_popup *xdg_popup,
                     int32_t           x,
                     int32_t           y,
                     int32_t           width,
                     int32_t           height)
{
  GdkWaylandPopup *wayland_popup = GDK_WAYLAND_POPUP (data);

  gdk_wayland_surface_handle_configure_popup (wayland_popup, x, y, width, height);
}

static void
xdg_popup_done (void             *data,
                struct xdg_popup *xdg_popup)
{
  GdkWaylandPopup *wayland_popup = GDK_WAYLAND_POPUP (data);
  GdkSurface *surface = GDK_SURFACE (wayland_popup);

  GDK_DISPLAY_DEBUG (gdk_surface_get_display (surface), EVENTS, "done %p", surface);

  gdk_surface_hide (surface);
}

static void
xdg_popup_repositioned (void             *data,
                        struct xdg_popup *xdg_popup,
                        uint32_t          token)
{
  GdkWaylandPopup *wayland_popup = GDK_WAYLAND_POPUP (data);

  GDK_DISPLAY_DEBUG (gdk_surface_get_display (GDK_SURFACE (wayland_popup)), EVENTS,
                     "repositioned %p", wayland_popup);

  if (wayland_popup->state != POPUP_STATE_WAITING_FOR_REPOSITIONED)
    {
      g_warning ("Unexpected xdg_popup.repositioned event, probably buggy compositor");
      return;
    }

  wayland_popup->pending.repositioned_token = token;
  wayland_popup->pending.has_repositioned_token = TRUE;
}

static const struct xdg_popup_listener xdg_popup_listener = {
  xdg_popup_configure,
  xdg_popup_done,
  xdg_popup_repositioned,
};

static void
zxdg_popup_v6_configure (void                 *data,
                         struct zxdg_popup_v6 *xdg_popup,
                         int32_t               x,
                         int32_t               y,
                         int32_t               width,
                         int32_t               height)
{
  GdkWaylandPopup *wayland_popup = GDK_WAYLAND_POPUP (data);

  gdk_wayland_surface_handle_configure_popup (wayland_popup, x, y, width, height);
}

static void
zxdg_popup_v6_done (void                 *data,
                    struct zxdg_popup_v6 *xdg_popup)
{
  GdkWaylandPopup *wayland_popup = GDK_WAYLAND_POPUP (data);
  GdkSurface *surface = GDK_SURFACE (wayland_popup);

  GDK_DEBUG (EVENTS, "done %p", surface);

  gdk_surface_hide (surface);
}

static const struct zxdg_popup_v6_listener zxdg_popup_v6_listener = {
  zxdg_popup_v6_configure,
  zxdg_popup_v6_done,
};

static void
calculate_popup_rect (GdkWaylandPopup *wayland_popup,
                      GdkPopupLayout  *layout,
                      GdkRectangle    *out_rect)
{
  int width, height;
  GdkRectangle anchor_rect;
  int dx, dy;
  int shadow_left, shadow_right, shadow_top, shadow_bottom;
  int x = 0, y = 0;

  gdk_popup_layout_get_shadow_width (layout,
                                     &shadow_left,
                                     &shadow_right,
                                     &shadow_top,
                                     &shadow_bottom);

  width = (wayland_popup->unconstrained_width - (shadow_left + shadow_right));
  height = (wayland_popup->unconstrained_height - (shadow_top + shadow_bottom));

  anchor_rect = *gdk_popup_layout_get_anchor_rect (layout);
  gdk_popup_layout_get_offset (layout, &dx, &dy);
  anchor_rect.x += dx;
  anchor_rect.y += dy;

  switch (gdk_popup_layout_get_rect_anchor (layout))
    {
    default:
    case GDK_GRAVITY_STATIC:
    case GDK_GRAVITY_NORTH_WEST:
      x = anchor_rect.x;
      y = anchor_rect.y;
      break;
    case GDK_GRAVITY_NORTH:
      x = anchor_rect.x + (anchor_rect.width / 2);
      y = anchor_rect.y;
      break;
    case GDK_GRAVITY_NORTH_EAST:
      x = anchor_rect.x + anchor_rect.width;
      y = anchor_rect.y;
      break;
    case GDK_GRAVITY_WEST:
      x = anchor_rect.x;
      y = anchor_rect.y + (anchor_rect.height / 2);
      break;
    case GDK_GRAVITY_CENTER:
      x = anchor_rect.x + (anchor_rect.width / 2);
      y = anchor_rect.y + (anchor_rect.height / 2);
      break;
    case GDK_GRAVITY_EAST:
      x = anchor_rect.x + anchor_rect.width;
      y = anchor_rect.y + (anchor_rect.height / 2);
      break;
    case GDK_GRAVITY_SOUTH_WEST:
      x = anchor_rect.x;
      y = anchor_rect.y + anchor_rect.height;
      break;
    case GDK_GRAVITY_SOUTH:
      x = anchor_rect.x + (anchor_rect.width / 2);
      y = anchor_rect.y + anchor_rect.height;
      break;
    case GDK_GRAVITY_SOUTH_EAST:
      x = anchor_rect.x + anchor_rect.width;
      y = anchor_rect.y + anchor_rect.height;
      break;
    }

  switch (gdk_popup_layout_get_surface_anchor (layout))
    {
    default:
    case GDK_GRAVITY_STATIC:
    case GDK_GRAVITY_NORTH_WEST:
      break;
    case GDK_GRAVITY_NORTH:
      x -= width / 2;
      break;
    case GDK_GRAVITY_NORTH_EAST:
      x -= width;
      break;
    case GDK_GRAVITY_WEST:
      y -= height / 2;
      break;
    case GDK_GRAVITY_CENTER:
      x -= width / 2;
      y -= height / 2;
      break;
    case GDK_GRAVITY_EAST:
      x -= width;
      y -= height / 2;
      break;
    case GDK_GRAVITY_SOUTH_WEST:
      y -= height;
      break;
    case GDK_GRAVITY_SOUTH:
      x -= width / 2;
      y -= height;
      break;
    case GDK_GRAVITY_SOUTH_EAST:
      x -= width;
      y -= height;
      break;
    }

  *out_rect = (GdkRectangle) {
    .x = x,
    .y = y,
    .width = width,
    .height = height
  };
}

static void
update_popup_layout_state (GdkWaylandPopup *wayland_popup,
                           int              x,
                           int              y,
                           int              width,
                           int              height,
                           GdkPopupLayout  *layout)
{
  GdkRectangle best_rect;
  GdkRectangle flipped_rect;
  GdkGravity rect_anchor;
  GdkGravity surface_anchor;
  GdkAnchorHints anchor_hints;

  rect_anchor = gdk_popup_layout_get_rect_anchor (layout);
  surface_anchor = gdk_popup_layout_get_surface_anchor (layout);
  anchor_hints = gdk_popup_layout_get_anchor_hints (layout);

  calculate_popup_rect (wayland_popup, layout, &best_rect);

  flipped_rect = best_rect;

  if (x != best_rect.x &&
      anchor_hints & GDK_ANCHOR_FLIP_X)
    {
      GdkRectangle flipped_x_rect;
      GdkGravity flipped_rect_anchor;
      GdkGravity flipped_surface_anchor;
      GdkPopupLayout *flipped_layout;

      flipped_rect_anchor = gdk_gravity_flip_horizontally (rect_anchor);
      flipped_surface_anchor = gdk_gravity_flip_horizontally (surface_anchor);
      flipped_layout = gdk_popup_layout_copy (layout);
      gdk_popup_layout_set_rect_anchor (flipped_layout,
                                        flipped_rect_anchor);
      gdk_popup_layout_set_surface_anchor (flipped_layout,
                                           flipped_surface_anchor);
      calculate_popup_rect (wayland_popup,
                            flipped_layout,
                            &flipped_x_rect);
      gdk_popup_layout_unref (flipped_layout);

      if (flipped_x_rect.x == x)
        flipped_rect.x = x;
    }
  if (y != best_rect.y &&
      anchor_hints & GDK_ANCHOR_FLIP_Y)
    {
      GdkRectangle flipped_y_rect;
      GdkGravity flipped_rect_anchor;
      GdkGravity flipped_surface_anchor;
      GdkPopupLayout *flipped_layout;

      flipped_rect_anchor = gdk_gravity_flip_vertically (rect_anchor);
      flipped_surface_anchor = gdk_gravity_flip_vertically (surface_anchor);
      flipped_layout = gdk_popup_layout_copy (layout);
      gdk_popup_layout_set_rect_anchor (flipped_layout,
                                        flipped_rect_anchor);
      gdk_popup_layout_set_surface_anchor (flipped_layout,
                                           flipped_surface_anchor);
      calculate_popup_rect (wayland_popup,
                            flipped_layout,
                            &flipped_y_rect);
      gdk_popup_layout_unref (flipped_layout);

      if (flipped_y_rect.y == y)
        flipped_rect.y = y;
    }

  if (flipped_rect.x != best_rect.x)
    {
      rect_anchor = gdk_gravity_flip_horizontally (rect_anchor);
      surface_anchor = gdk_gravity_flip_horizontally (surface_anchor);
    }
  if (flipped_rect.y != best_rect.y)
    {
      rect_anchor = gdk_gravity_flip_vertically (rect_anchor);
      surface_anchor = gdk_gravity_flip_vertically (surface_anchor);
    }

  GDK_SURFACE (wayland_popup)->popup.rect_anchor = rect_anchor;
  GDK_SURFACE (wayland_popup)->popup.surface_anchor = surface_anchor;
}

static gpointer
create_dynamic_positioner (GdkWaylandPopup *wayland_popup,
                           int              width,
                           int              height,
                           GdkPopupLayout  *layout,
                           gboolean         ack_parent_configure)
{
  GdkSurface *surface = GDK_SURFACE (wayland_popup);
  GdkSurface *parent = surface->parent;
  GdkWaylandSurface *parent_impl = GDK_WAYLAND_SURFACE (parent);
  GdkWaylandDisplay *display =
    GDK_WAYLAND_DISPLAY (gdk_surface_get_display (surface));
  GdkRectangle geometry;
  uint32_t constraint_adjustment = ZXDG_POSITIONER_V6_CONSTRAINT_ADJUSTMENT_NONE;
  const GdkRectangle *anchor_rect;
  int real_anchor_rect_x, real_anchor_rect_y;
  int anchor_rect_width, anchor_rect_height;
  int rect_anchor_dx;
  int rect_anchor_dy;
  GdkGravity rect_anchor;
  GdkGravity surface_anchor;
  GdkAnchorHints anchor_hints;
  GdkRectangle parent_geometry;
  int shadow_left;
  int shadow_right;
  int shadow_top;
  int shadow_bottom;

  gdk_popup_layout_get_shadow_width (layout,
                                     &shadow_left,
                                     &shadow_right,
                                     &shadow_top,
                                     &shadow_bottom);
  geometry = (GdkRectangle) {
    .x = shadow_left,
    .y = shadow_top,
    .width = width - (shadow_left + shadow_right),
    .height = height - (shadow_top + shadow_bottom),
  };

  gdk_wayland_surface_get_window_geometry (surface->parent, &parent_geometry);

  anchor_rect = gdk_popup_layout_get_anchor_rect (layout);
  real_anchor_rect_x = anchor_rect->x - parent_geometry.x;
  real_anchor_rect_y = anchor_rect->y - parent_geometry.y;

  anchor_rect_width = MAX (anchor_rect->width, 1);
  anchor_rect_height = MAX (anchor_rect->height, 1);

  gdk_popup_layout_get_offset (layout, &rect_anchor_dx, &rect_anchor_dy);

  rect_anchor = gdk_popup_layout_get_rect_anchor (layout);
  surface_anchor = gdk_popup_layout_get_surface_anchor (layout);

  anchor_hints = gdk_popup_layout_get_anchor_hints (layout);

  switch (display->shell_variant)
    {
    case GDK_WAYLAND_SHELL_VARIANT_XDG_SHELL:
      {
        struct xdg_positioner *positioner;
        enum xdg_positioner_anchor anchor;
        enum xdg_positioner_gravity gravity;

        positioner = xdg_wm_base_create_positioner (display->xdg_wm_base);

        xdg_positioner_set_size (positioner, geometry.width, geometry.height);
        xdg_positioner_set_anchor_rect (positioner,
                                        real_anchor_rect_x,
                                        real_anchor_rect_y,
                                        anchor_rect_width,
                                        anchor_rect_height);
        xdg_positioner_set_offset (positioner, rect_anchor_dx, rect_anchor_dy);

        anchor = rect_anchor_to_anchor (rect_anchor);
        xdg_positioner_set_anchor (positioner, anchor);

        gravity = surface_anchor_to_gravity (surface_anchor);
        xdg_positioner_set_gravity (positioner, gravity);

        if (anchor_hints & GDK_ANCHOR_FLIP_X)
          constraint_adjustment |= XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X;
        if (anchor_hints & GDK_ANCHOR_FLIP_Y)
          constraint_adjustment |= XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y;
        if (anchor_hints & GDK_ANCHOR_SLIDE_X)
          constraint_adjustment |= XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X;
        if (anchor_hints & GDK_ANCHOR_SLIDE_Y)
          constraint_adjustment |= XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y;
        if (anchor_hints & GDK_ANCHOR_RESIZE_X)
          constraint_adjustment |= XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X;
        if (anchor_hints & GDK_ANCHOR_RESIZE_Y)
          constraint_adjustment |= XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y;
        xdg_positioner_set_constraint_adjustment (positioner,
                                                  constraint_adjustment);

        if (xdg_positioner_get_version (positioner) >=
            XDG_POSITIONER_SET_REACTIVE_SINCE_VERSION)
          xdg_positioner_set_reactive (positioner);

        if (ack_parent_configure &&
            xdg_positioner_get_version (positioner) >=
            XDG_POSITIONER_SET_PARENT_CONFIGURE_SINCE_VERSION)
          {
            xdg_positioner_set_parent_size (positioner,
                                            parent_geometry.width,
                                            parent_geometry.height);
            xdg_positioner_set_parent_configure (positioner,
                                                 parent_impl->last_configure_serial);
          }

        return positioner;
      }
    case GDK_WAYLAND_SHELL_VARIANT_ZXDG_SHELL_V6:
      {
        struct zxdg_positioner_v6 *positioner;
        enum zxdg_positioner_v6_anchor anchor;
        enum zxdg_positioner_v6_gravity gravity;

        positioner = zxdg_shell_v6_create_positioner (display->zxdg_shell_v6);

        zxdg_positioner_v6_set_size (positioner, geometry.width, geometry.height);
        zxdg_positioner_v6_set_anchor_rect (positioner,
                                            real_anchor_rect_x,
                                            real_anchor_rect_y,
                                            anchor_rect_width,
                                            anchor_rect_height);
        zxdg_positioner_v6_set_offset (positioner,
                                       rect_anchor_dx,
                                       rect_anchor_dy);

        anchor = rect_anchor_to_anchor_legacy (rect_anchor);
        zxdg_positioner_v6_set_anchor (positioner, anchor);

        gravity = surface_anchor_to_gravity_legacy (surface_anchor);
        zxdg_positioner_v6_set_gravity (positioner, gravity);

        if (anchor_hints & GDK_ANCHOR_FLIP_X)
          constraint_adjustment |= ZXDG_POSITIONER_V6_CONSTRAINT_ADJUSTMENT_FLIP_X;
        if (anchor_hints & GDK_ANCHOR_FLIP_Y)
          constraint_adjustment |= ZXDG_POSITIONER_V6_CONSTRAINT_ADJUSTMENT_FLIP_Y;
        if (anchor_hints & GDK_ANCHOR_SLIDE_X)
          constraint_adjustment |= ZXDG_POSITIONER_V6_CONSTRAINT_ADJUSTMENT_SLIDE_X;
        if (anchor_hints & GDK_ANCHOR_SLIDE_Y)
          constraint_adjustment |= ZXDG_POSITIONER_V6_CONSTRAINT_ADJUSTMENT_SLIDE_Y;
        if (anchor_hints & GDK_ANCHOR_RESIZE_X)
          constraint_adjustment |= ZXDG_POSITIONER_V6_CONSTRAINT_ADJUSTMENT_RESIZE_X;
        if (anchor_hints & GDK_ANCHOR_RESIZE_Y)
          constraint_adjustment |= ZXDG_POSITIONER_V6_CONSTRAINT_ADJUSTMENT_RESIZE_Y;
        zxdg_positioner_v6_set_constraint_adjustment (positioner,
                                                      constraint_adjustment);

        return positioner;
      }
    default:
      g_assert_not_reached ();
    }

  g_assert_not_reached ();
}

static gboolean
can_map_grabbing_popup (GdkSurface *surface,
                        GdkSurface *parent)
{
  GdkDisplay *display = gdk_surface_get_display (surface);
  GdkWaylandDisplay *display_wayland = GDK_WAYLAND_DISPLAY (display);
  GdkSurface *top_most_popup;

  if (!display_wayland->current_grabbing_popups)
    return TRUE;

  top_most_popup = g_list_first (display_wayland->current_grabbing_popups)->data;
  return top_most_popup == parent;
}

static gboolean
gdk_wayland_surface_create_xdg_popup (GdkWaylandPopup *wayland_popup,
                                      GdkSurface      *parent,
                                      GdkWaylandSeat  *grab_input_seat,
                                      int              width,
                                      int              height,
                                      GdkPopupLayout  *layout)
{
  GdkSurface *surface = GDK_SURFACE (wayland_popup);
  GdkWaylandDisplay *display = GDK_WAYLAND_DISPLAY (gdk_surface_get_display (surface));
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);
  GdkWaylandSurface *parent_impl = GDK_WAYLAND_SURFACE (parent);
  gpointer positioner;

  if (!impl->display_server.wl_surface)
    return FALSE;

  if (!is_realized_shell_surface (parent_impl))
    return FALSE;

  if (is_realized_popup (impl))
    {
      g_warning ("Can't map popup, already mapped");
      return FALSE;
    }

  if (grab_input_seat &&
      !can_map_grabbing_popup (surface, parent))
    {
      g_warning ("Tried to map a grabbing popup with a non-top most parent");
      return FALSE;
    }

  gdk_surface_freeze_updates (surface);

  positioner = create_dynamic_positioner (wayland_popup, width, height, layout, FALSE);
  gdk_wayland_surface_create_xdg_surface_resources (surface);

  switch (display->shell_variant)
    {
    case GDK_WAYLAND_SHELL_VARIANT_XDG_SHELL:
      wayland_popup->display_server.xdg_popup =
        xdg_surface_get_popup (impl->display_server.xdg_surface,
                               parent_impl->display_server.xdg_surface,
                               positioner);
      xdg_popup_add_listener (wayland_popup->display_server.xdg_popup,
                              &xdg_popup_listener,
                              wayland_popup);
      xdg_positioner_destroy (positioner);
      break;
    case GDK_WAYLAND_SHELL_VARIANT_ZXDG_SHELL_V6:
      wayland_popup->display_server.zxdg_popup_v6 =
        zxdg_surface_v6_get_popup (impl->display_server.zxdg_surface_v6,
                                   parent_impl->display_server.zxdg_surface_v6,
                                   positioner);
      zxdg_popup_v6_add_listener (wayland_popup->display_server.zxdg_popup_v6,
                                  &zxdg_popup_v6_listener,
                                  wayland_popup);
      zxdg_positioner_v6_destroy (positioner);
      break;
    default:
      g_assert_not_reached ();
    }

  gdk_popup_layout_get_shadow_width (layout,
                                     &impl->shadow_left,
                                     &impl->shadow_right,
                                     &impl->shadow_top,
                                     &impl->shadow_bottom);

  if (grab_input_seat)
    {
      struct wl_seat *seat;
      guint32 serial;

      seat = gdk_wayland_seat_get_wl_seat (GDK_SEAT (grab_input_seat));
      serial = _gdk_wayland_seat_get_last_implicit_grab_serial (grab_input_seat, NULL);

      switch (display->shell_variant)
        {
        case GDK_WAYLAND_SHELL_VARIANT_XDG_SHELL:
          xdg_popup_grab (wayland_popup->display_server.xdg_popup, seat, serial);
          break;
        case GDK_WAYLAND_SHELL_VARIANT_ZXDG_SHELL_V6:
          zxdg_popup_v6_grab (wayland_popup->display_server.zxdg_popup_v6, seat, serial);
          break;
        default:
          g_assert_not_reached ();
        }
    }

  gdk_profiler_add_mark (GDK_PROFILER_CURRENT_TIME, 0, "wayland", "surface commit");
  wl_surface_commit (impl->display_server.wl_surface);

  if (GDK_IS_POPUP (surface))
    {
      g_assert (wayland_popup->state == POPUP_STATE_IDLE);
      wayland_popup->state = POPUP_STATE_WAITING_FOR_CONFIGURE;
      freeze_popup_toplevel_state (wayland_popup);
    }

  display->current_popups = g_list_append (display->current_popups, surface);
  if (grab_input_seat)
    {
      display->current_grabbing_popups =
        g_list_prepend (display->current_grabbing_popups, surface);
    }

  return TRUE;
}


#define LAST_PROP 1

static void
gdk_wayland_popup_init (GdkWaylandPopup *popup)
{
}

static void
gdk_wayland_popup_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  GdkSurface *surface = GDK_SURFACE (object);

  switch (prop_id)
    {
    case LAST_PROP + GDK_POPUP_PROP_PARENT:
      g_value_set_object (value, surface->parent);
      break;

    case LAST_PROP + GDK_POPUP_PROP_AUTOHIDE:
      g_value_set_boolean (value, surface->autohide);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
gdk_wayland_popup_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  GdkSurface *surface = GDK_SURFACE (object);

  switch (prop_id)
    {
    case LAST_PROP + GDK_POPUP_PROP_PARENT:
      surface->parent = g_value_dup_object (value);
      if (surface->parent != NULL)
        surface->parent->children = g_list_prepend (surface->parent->children, surface);
      break;

    case LAST_PROP + GDK_POPUP_PROP_AUTOHIDE:
      surface->autohide = g_value_get_boolean (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
gdk_wayland_popup_class_init (GdkWaylandPopupClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->get_property = gdk_wayland_popup_get_property;
  object_class->set_property = gdk_wayland_popup_set_property;

  gdk_popup_install_properties (object_class, 1);
}

static gboolean
is_fallback_relayout_possible (GdkWaylandPopup *wayland_popup)
{
  GList *l;

  for (l = GDK_SURFACE (wayland_popup)->children; l; l = l->next)
    {
      GdkSurface *child = l->data;

      if (GDK_WAYLAND_SURFACE (child)->mapped)
        return FALSE;
    }

  return TRUE;
}

static gboolean gdk_wayland_surface_present_popup (GdkWaylandPopup *wayland_popup,
                                                   int              width,
                                                   int              height,
                                                   GdkPopupLayout  *layout);

static void
queue_relayout_fallback (GdkWaylandPopup *wayland_popup,
                         GdkPopupLayout  *layout)
{
  if (!is_fallback_relayout_possible (wayland_popup))
    return;

  gdk_wayland_surface_hide_surface (GDK_SURFACE (wayland_popup));
  gdk_wayland_surface_present_popup (wayland_popup,
                                     wayland_popup->unconstrained_width,
                                     wayland_popup->unconstrained_height,
                                     layout);
}

static void
do_queue_relayout (GdkWaylandPopup *wayland_popup,
                   int              width,
                   int              height,
                   GdkPopupLayout  *layout)
{
  GdkWaylandSurface *wayland_surface = GDK_WAYLAND_SURFACE (wayland_popup);
  struct xdg_positioner *positioner;

  g_assert (is_realized_popup (wayland_surface));
  g_assert (wayland_popup->state == POPUP_STATE_IDLE ||
            wayland_popup->state == POPUP_STATE_WAITING_FOR_FRAME);

  g_clear_pointer (&wayland_popup->layout, gdk_popup_layout_unref);
  wayland_popup->layout = gdk_popup_layout_copy (layout);
  wayland_popup->unconstrained_width = width;
  wayland_popup->unconstrained_height = height;

  if (!wayland_popup->display_server.xdg_popup ||
      xdg_popup_get_version (wayland_popup->display_server.xdg_popup) <
      XDG_POPUP_REPOSITION_SINCE_VERSION)
    {
      g_warning_once ("Compositor doesn't support moving popups, "
                      "relying on remapping");
      queue_relayout_fallback (wayland_popup, layout);

      return;
    }

  positioner = create_dynamic_positioner (wayland_popup,
                                          width, height, layout,
                                          TRUE);
  xdg_popup_reposition (wayland_popup->display_server.xdg_popup,
                        positioner,
                        ++wayland_popup->reposition_token);
  xdg_positioner_destroy (positioner);

  gdk_surface_freeze_updates (GDK_SURFACE (wayland_popup));

  switch (wayland_popup->state)
    {
    case POPUP_STATE_IDLE:
      freeze_popup_toplevel_state (wayland_popup);
      break;
    case POPUP_STATE_WAITING_FOR_FRAME:
      break;
    case POPUP_STATE_WAITING_FOR_CONFIGURE:
    case POPUP_STATE_WAITING_FOR_REPOSITIONED:
    default:
      g_assert_not_reached ();
    }

  wayland_popup->state = POPUP_STATE_WAITING_FOR_REPOSITIONED;
}

static gboolean
is_relayout_finished (GdkSurface *surface)
{
  GdkWaylandSurface *impl = GDK_WAYLAND_SURFACE (surface);

  if (!impl->initial_configure_received)
    return FALSE;

  if (GDK_IS_WAYLAND_POPUP (surface))
    {
      GdkWaylandPopup *popup = GDK_WAYLAND_POPUP (surface);
      if (popup->reposition_token != popup->received_reposition_token)
        return FALSE;
    }

  return TRUE;
}

static GdkWaylandSeat *
find_grab_input_seat (GdkSurface *surface,
                      GdkSurface *parent)
{
  GdkWaylandPopup *popup = GDK_WAYLAND_POPUP (surface);
  GdkWaylandPopup *tmp_popup;

  /* Use the device that was used for the grab as the device for
   * the popup surface setup - so this relies on GTK taking the
   * grab before showing the popup surface.
   */
  if (popup->grab_input_seat)
    return GDK_WAYLAND_SEAT (popup->grab_input_seat);

  while (parent)
    {
      if (!GDK_IS_WAYLAND_POPUP (parent))
        break;

      tmp_popup = GDK_WAYLAND_POPUP (parent);

      if (tmp_popup->grab_input_seat)
        return GDK_WAYLAND_SEAT (tmp_popup->grab_input_seat);

      parent = parent->parent;
    }

  return NULL;
}

static void
gdk_wayland_surface_map_popup (GdkWaylandPopup *wayland_popup,
                               int              width,
                               int              height,
                               GdkPopupLayout  *layout)
{
  GdkSurface *surface = GDK_SURFACE (wayland_popup);
  GdkWaylandSurface *wayland_surface = GDK_WAYLAND_SURFACE (wayland_popup);
  GdkSurface *parent;
  GdkWaylandSeat *grab_input_seat;

  parent = surface->parent;
  if (!parent)
    {
      g_warning ("Couldn't map as surface %p as popup because it doesn't have a parent",
                 surface);
      return;
    }

  if (surface->autohide)
    grab_input_seat = find_grab_input_seat (surface, parent);
  else
    grab_input_seat = NULL;

  if (!gdk_wayland_surface_create_xdg_popup (wayland_popup,
                                             parent,
                                             grab_input_seat,
                                             width, height,
                                             layout))
    return;

  wayland_popup->layout = gdk_popup_layout_copy (layout);
  wayland_popup->unconstrained_width = width;
  wayland_popup->unconstrained_height = height;
  wayland_surface->mapped = TRUE;
}

static void
show_popup (GdkWaylandPopup *wayland_popup,
            int              width,
            int              height,
            GdkPopupLayout  *layout)
{
  GdkWaylandSurface *wayland_surface = GDK_WAYLAND_SURFACE (wayland_popup);

  if (!wayland_surface->display_server.wl_surface)
    gdk_wayland_surface_create_wl_surface (GDK_SURFACE (wayland_popup));

  if (wayland_popup->thaw_upon_show)
    {
      wayland_popup->thaw_upon_show = FALSE;
      gdk_surface_thaw_updates (GDK_SURFACE (wayland_popup));
    }

  gdk_wayland_surface_map_popup (wayland_popup, width, height, layout);
}

typedef struct
{
  int width;
  int height;
  GdkPopupLayout *layout;
} GrabPrepareData;

static void
show_grabbing_popup (GdkSeat    *seat,
                     GdkSurface *surface,
                     gpointer    user_data)
{
  GrabPrepareData *data = user_data;

  g_return_if_fail (GDK_IS_WAYLAND_POPUP (surface));
  GdkWaylandPopup *wayland_popup = GDK_WAYLAND_POPUP (surface);

  show_popup (wayland_popup, data->width, data->height, data->layout);
}

static void
reposition_popup (GdkWaylandPopup *wayland_popup,
                  int              width,
                  int              height,
                  GdkPopupLayout  *layout)
{
  switch (wayland_popup->state)
    {
    case POPUP_STATE_IDLE:
    case POPUP_STATE_WAITING_FOR_FRAME:
      do_queue_relayout (wayland_popup, width, height, layout);
      break;
    case POPUP_STATE_WAITING_FOR_REPOSITIONED:
    case POPUP_STATE_WAITING_FOR_CONFIGURE:
      g_warn_if_reached ();
      break;
    default:
      g_assert_not_reached ();
    }
}

static gboolean
gdk_wayland_surface_present_popup (GdkWaylandPopup *wayland_popup,
                                   int              width,
                                   int              height,
                                   GdkPopupLayout  *layout)
{
  GdkSurface *surface = GDK_SURFACE (wayland_popup);
  GdkWaylandDisplay *display_wayland =
    GDK_WAYLAND_DISPLAY (gdk_surface_get_display (surface));
  GdkWaylandSurface *wayland_surface = GDK_WAYLAND_SURFACE (wayland_popup);

  if (!wayland_surface->mapped)
    {
      if (surface->autohide)
        {
          GdkSeat *seat;

          seat = gdk_display_get_default_seat (surface->display);
          if (seat)
            {
              GrabPrepareData data;
              GdkGrabStatus result;

              data = (GrabPrepareData) {
                .width = width,
                .height = height,
                .layout = layout,
              };

              result = gdk_seat_grab (seat,
                                      surface,
                                      GDK_SEAT_CAPABILITY_ALL,
                                      TRUE,
                                      NULL, NULL,
                                      show_grabbing_popup, &data);
              if (result != GDK_GRAB_SUCCESS)
                {
                  const char *grab_status[] = {
                    "success", "already grabbed", "invalid time",
                    "not viewable", "frozen", "failed"
                  };
                  g_warning ("Grab failed: %s", grab_status[result]);
                }
            }
        }
      else
        {
          show_popup (wayland_popup, width, height, layout);
        }
    }
  else
    {
      if (wayland_popup->unconstrained_width == width &&
          wayland_popup->unconstrained_height == height &&
          gdk_popup_layout_equal (wayland_popup->layout, layout))
        return TRUE;

      reposition_popup (wayland_popup, width, height, layout);
    }

  while (wayland_popup->display_server.xdg_popup && !is_relayout_finished (surface))
    wl_display_dispatch_queue (display_wayland->wl_display, wayland_surface->event_queue);

  if (wayland_popup->display_server.xdg_popup)
    {
      gdk_surface_invalidate_rect (surface, NULL);
      return TRUE;
    }
  else
    {
      return FALSE;
    }
}

static gboolean
gdk_wayland_popup_present (GdkPopup       *popup,
                           int             width,
                           int             height,
                           GdkPopupLayout *layout)
{
  return gdk_wayland_surface_present_popup (GDK_WAYLAND_POPUP (popup), width, height, layout);
}

static GdkGravity
gdk_wayland_popup_get_surface_anchor (GdkPopup *popup)
{
  return GDK_SURFACE (popup)->popup.surface_anchor;
}

static GdkGravity
gdk_wayland_popup_get_rect_anchor (GdkPopup *popup)
{
  return GDK_SURFACE (popup)->popup.rect_anchor;
}

static int
gdk_wayland_popup_get_position_x (GdkPopup *popup)
{
  return GDK_SURFACE (popup)->x;
}

static int
gdk_wayland_popup_get_position_y (GdkPopup *popup)
{
  return GDK_SURFACE (popup)->y;
}

static void
gdk_wayland_popup_iface_init (GdkPopupInterface *iface)
{
  iface->present = gdk_wayland_popup_present;
  iface->get_surface_anchor = gdk_wayland_popup_get_surface_anchor;
  iface->get_rect_anchor = gdk_wayland_popup_get_rect_anchor;
  iface->get_position_x = gdk_wayland_popup_get_position_x;
  iface->get_position_y = gdk_wayland_popup_get_position_y;
}

/* }}} */
/* {{{ Private Popup API */

void
_gdk_wayland_surface_set_grab_seat (GdkSurface *surface,
                                    GdkSeat    *seat)
{
  GdkWaylandPopup *popup;

  g_return_if_fail (surface != NULL);

  popup = GDK_WAYLAND_POPUP (surface);
  popup->grab_input_seat = seat;
}

/* }}} */
/* vim:set foldmethod=marker expandtab: */
