/*
 * Copyright (C) 2026 Red Hat Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <glib.h>
#include <wayland-client.h>

#include "wayland-test-client-utils.h"

#define WINDOW_WIDTH 200
#define WINDOW_HEIGHT 200

static struct wl_surface *surface;
static WaylandDisplay *global_display;
static GList *active_buffers;
static gboolean running;
static gboolean configured;
static gboolean mark_opaque = TRUE;

static void
handle_buffer_release (void             *user_data,
                       struct wl_buffer *buffer_resource)
{
  WaylandBuffer *buffer = user_data;

  active_buffers = g_list_remove (active_buffers, buffer);
  g_object_unref (buffer);
}

static const struct wl_buffer_listener buffer_listener = {
  handle_buffer_release
};

static void draw (void);

static void
handle_frame_callback (void               *user_data,
                       struct wl_callback *callback,
                       uint32_t            time)
{
  wl_callback_destroy (callback);

  if (running)
    draw ();
}

static const struct wl_callback_listener frame_listener = {
  handle_frame_callback,
};

static void
draw (void)
{
  WaylandBuffer *buffer;
  DmaBufFormat *format;
  struct wl_callback *frame_callback;

  format = g_hash_table_lookup (global_display->formats,
                                GUINT_TO_POINTER (DRM_FORMAT_ARGB8888));
  g_assert_nonnull (format);

  buffer = wayland_buffer_create (global_display,
                                  &buffer_listener,
                                  WINDOW_WIDTH, WINDOW_HEIGHT,
                                  format->format,
                                  format->modifiers,
                                  format->n_modifiers,
                                  GBM_BO_USE_RENDERING |
                                  GBM_BO_USE_SCANOUT);
  g_assert_nonnull (buffer);

  active_buffers = g_list_prepend (active_buffers, buffer);

  wl_surface_attach (surface, wayland_buffer_get_wl_buffer (buffer), 0, 0);
  wl_surface_damage_buffer (surface, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT);

  if (mark_opaque)
    {
      struct wl_region *region;

      region = wl_compositor_create_region (global_display->compositor);
      wl_region_add (region, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT);
      wl_surface_set_opaque_region (surface, region);
      wl_region_destroy (region);
    }
  else
    {
      wl_surface_set_opaque_region (surface, NULL);
    }

  frame_callback = wl_surface_frame (surface);
  wl_callback_add_listener (frame_callback, &frame_listener, NULL);

  wl_surface_commit (surface);
}

static void
handle_xdg_toplevel_configure (void                *user_data,
                               struct xdg_toplevel *xdg_toplevel,
                               int32_t              width,
                               int32_t              height,
                               struct wl_array     *states)
{
}

static void
handle_xdg_toplevel_close (void                *user_data,
                           struct xdg_toplevel *xdg_toplevel)
{
  g_assert_not_reached ();
}

static void
handle_xdg_toplevel_configure_bounds (void                *user_data,
                                      struct xdg_toplevel *xdg_toplevel,
                                      int32_t              bounds_width,
                                      int32_t              bounds_height)
{
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
  handle_xdg_toplevel_configure,
  handle_xdg_toplevel_close,
  handle_xdg_toplevel_configure_bounds,
};

static void
handle_xdg_surface_configure (void               *user_data,
                              struct xdg_surface *xdg_surface,
                              uint32_t            serial)
{
  WaylandDisplay *display = user_data;

  xdg_surface_ack_configure (xdg_surface, serial);

  if (!configured)
    {
      configured = TRUE;
      draw ();
      test_driver_sync_point (display->test_driver, 0, NULL);
    }
}

static const struct xdg_surface_listener xdg_surface_listener = {
  handle_xdg_surface_configure,
};

static void
on_sync_event (WaylandDisplay *display,
               uint32_t        serial)
{
  if (serial == 0)
    {
      running = FALSE;
    }
  else if (serial == 1)
    {
      mark_opaque = FALSE;
      test_driver_sync_point (display->test_driver, 1, NULL);
    }
  else
    {
      g_assert_not_reached ();
    }
}

int
main (int    argc,
      char **argv)
{
  g_autoptr (WaylandDisplay) display = NULL;
  struct xdg_toplevel *xdg_toplevel;
  struct xdg_surface *xdg_surface;

  display = wayland_display_new (WAYLAND_DISPLAY_CAPABILITY_TEST_DRIVER);
  global_display = display;
  g_signal_connect (display, "sync-event", G_CALLBACK (on_sync_event), NULL);
  wl_display_roundtrip (display->display);
  g_assert_nonnull (display->gbm_device);

  surface = wl_compositor_create_surface (display->compositor);
  xdg_surface = xdg_wm_base_get_xdg_surface (display->xdg_wm_base, surface);
  xdg_surface_add_listener (xdg_surface, &xdg_surface_listener, display);
  xdg_toplevel = xdg_surface_get_toplevel (xdg_surface);
  xdg_toplevel_add_listener (xdg_toplevel, &xdg_toplevel_listener, display);

  xdg_toplevel_set_title (xdg_toplevel, "dma-buf-overlay-alpha-test");
  wl_surface_commit (surface);

  running = TRUE;
  while (running)
    wayland_display_dispatch (display);

  g_list_free_full (active_buffers, (GDestroyNotify) g_object_unref);

  return EXIT_SUCCESS;
}
