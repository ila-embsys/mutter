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

#define N_WINDOWS 2
#define WINDOW_WIDTH 200
#define WINDOW_HEIGHT 200

typedef struct _OverlayWindow
{
  struct wl_surface *surface;
  struct xdg_surface *xdg_surface;
  struct xdg_toplevel *xdg_toplevel;
  GList *active_buffers;
  gboolean configured;
  const char *title;
} OverlayWindow;

static OverlayWindow windows[N_WINDOWS];
static WaylandDisplay *global_display;
static gboolean running;

static void draw_window (OverlayWindow *win);

static void
handle_buffer_release (void             *user_data,
                       struct wl_buffer *buffer_resource)
{
  WaylandBuffer *buffer = user_data;
  OverlayWindow *win = g_object_get_data (G_OBJECT (buffer), "window");

  win->active_buffers = g_list_remove (win->active_buffers, buffer);
  g_object_unref (buffer);
}

static const struct wl_buffer_listener buffer_listener = {
  handle_buffer_release,
};

static void
handle_frame_callback (void               *user_data,
                       struct wl_callback *callback,
                       uint32_t            time)
{
  OverlayWindow *win = user_data;

  wl_callback_destroy (callback);

  if (running)
    draw_window (win);
}

static const struct wl_callback_listener frame_listener = {
  handle_frame_callback,
};

static void
draw_window (OverlayWindow *win)
{
  WaylandBuffer *buffer;
  DmaBufFormat *format;
  struct wl_callback *frame_callback;

  format = g_hash_table_lookup (global_display->formats,
                                GUINT_TO_POINTER (DRM_FORMAT_XRGB8888));
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

  g_object_set_data (G_OBJECT (buffer), "window", win);
  win->active_buffers = g_list_prepend (win->active_buffers, buffer);

  wl_surface_attach (win->surface,
                     wayland_buffer_get_wl_buffer (buffer), 0, 0);
  wl_surface_damage_buffer (win->surface,
                            0, 0, WINDOW_WIDTH, WINDOW_HEIGHT);

  frame_callback = wl_surface_frame (win->surface);
  wl_callback_add_listener (frame_callback, &frame_listener, win);

  wl_surface_commit (win->surface);
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
  OverlayWindow *win = user_data;

  xdg_surface_ack_configure (xdg_surface, serial);

  if (!win->configured)
    {
      win->configured = TRUE;
      draw_window (win);
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
    running = FALSE;
  else
    g_assert_not_reached ();
}

static gboolean
all_configured (void)
{
  for (int i = 0; i < N_WINDOWS; i++)
    {
      if (!windows[i].configured)
        return FALSE;
    }
  return TRUE;
}

static void
create_window (WaylandDisplay *display,
               OverlayWindow  *win,
               const char     *title)
{
  win->title = title;
  win->surface = wl_compositor_create_surface (display->compositor);
  win->xdg_surface =
    xdg_wm_base_get_xdg_surface (display->xdg_wm_base, win->surface);
  xdg_surface_add_listener (win->xdg_surface,
                            &xdg_surface_listener, win);
  win->xdg_toplevel = xdg_surface_get_toplevel (win->xdg_surface);
  xdg_toplevel_add_listener (win->xdg_toplevel,
                             &xdg_toplevel_listener, win);
  xdg_toplevel_set_title (win->xdg_toplevel, title);
  wl_surface_commit (win->surface);
}

int
main (int    argc,
      char **argv)
{
  g_autoptr (WaylandDisplay) display = NULL;

  display = wayland_display_new (WAYLAND_DISPLAY_CAPABILITY_TEST_DRIVER);
  global_display = display;
  g_signal_connect (display, "sync-event", G_CALLBACK (on_sync_event), NULL);
  wl_display_roundtrip (display->display);
  g_assert_nonnull (display->gbm_device);

  create_window (display, &windows[0], "dma-buf-two-overlays-0");
  create_window (display, &windows[1], "dma-buf-two-overlays-1");

  running = TRUE;

  while (!all_configured ())
    wayland_display_dispatch (display);

  test_driver_sync_point (display->test_driver, 0, NULL);

  while (running)
    wayland_display_dispatch (display);

  for (int i = 0; i < N_WINDOWS; i++)
    g_list_free_full (windows[i].active_buffers,
                      (GDestroyNotify) g_object_unref);

  return EXIT_SUCCESS;
}
