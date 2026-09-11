/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2022-2023 Dor Askayo
 * Copyright (C) 2024 GNOME Foundation Inc.
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
 *
 * Written by:
 *     Dor Askayo <dor.askayo@gmail.com>
 */

#include "config.h"

#include "compositor/meta-compositor-view-native.h"

#include "backends/native/meta-onscreen-native.h"
#include "backends/native/meta-scanout-manager.h"
#include "clutter/clutter.h"
#include "clutter/clutter-offloadable.h"
#include "compositor/compositor-private.h"
#include "compositor/meta-surface-actor.h"

struct _MetaCompositorViewNative
{
  MetaCompositorView parent;

  MetaScanoutManager *scanout_manager;
  gulong stage_view_destroy_handler_id;

  MetaSurfaceActor *fullscreen_surface;
  gulong fullscreen_repaint_scheduled_id;
  gulong fullscreen_update_scheduled_id;
  gulong fullscreen_destroy_id;
};

G_DEFINE_TYPE (MetaCompositorViewNative, meta_compositor_view_native,
               META_TYPE_COMPOSITOR_VIEW)

static void
on_fullscreen_surface_update (MetaSurfaceActor         *surface_actor,
                              MetaCompositorViewNative *view_native)
{
  MetaCompositorView *compositor_view = META_COMPOSITOR_VIEW (view_native);
  ClutterStageView *stage_view;
  CoglFramebuffer *framebuffer;
  ClutterFrameClock *frame_clock;

  stage_view = meta_compositor_view_get_stage_view (compositor_view);

  framebuffer = clutter_stage_view_get_onscreen (stage_view);
  if (!META_IS_ONSCREEN_NATIVE (framebuffer))
    return;

  frame_clock = clutter_stage_view_get_frame_clock (stage_view);
  if (frame_clock)
    {
      clutter_frame_clock_set_fullscreen_update_time (frame_clock,
                                                      g_get_monotonic_time ());
    }
}

static void
clear_fullscreen_surface (MetaCompositorViewNative *view_native)
{
  if (!view_native->fullscreen_surface)
    return;

  g_clear_signal_handler (&view_native->fullscreen_repaint_scheduled_id,
                           view_native->fullscreen_surface);
  g_clear_signal_handler (&view_native->fullscreen_update_scheduled_id,
                           view_native->fullscreen_surface);
  g_clear_signal_handler (&view_native->fullscreen_destroy_id,
                           view_native->fullscreen_surface);
  view_native->fullscreen_surface = NULL;
}

static void
on_fullscreen_surface_destroyed (MetaSurfaceActor         *surface_actor,
                                 MetaCompositorViewNative *view_native)
{
  view_native->fullscreen_repaint_scheduled_id = 0;
  view_native->fullscreen_update_scheduled_id = 0;
  view_native->fullscreen_destroy_id = 0;
  view_native->fullscreen_surface = NULL;
}

static void
update_fullscreen_surface_tracking (MetaCompositorViewNative *view_native,
                                    MetaSurfaceActor         *fullscreen_surface)
{
  if (view_native->fullscreen_surface == fullscreen_surface)
    return;

  clear_fullscreen_surface (view_native);

  if (!fullscreen_surface)
    return;

  view_native->fullscreen_surface = fullscreen_surface;
  view_native->fullscreen_repaint_scheduled_id =
    g_signal_connect (fullscreen_surface, "repaint-scheduled",
                      G_CALLBACK (on_fullscreen_surface_update),
                      view_native);
  view_native->fullscreen_update_scheduled_id =
    g_signal_connect (fullscreen_surface, "update-scheduled",
                      G_CALLBACK (on_fullscreen_surface_update),
                      view_native);
  view_native->fullscreen_destroy_id =
    g_signal_connect (fullscreen_surface, "destroy",
                      G_CALLBACK (on_fullscreen_surface_destroyed),
                      view_native);
}

static MetaSurfaceActor *
get_fullscreen_surface (ClutterOffloadableActors *offload_actors,
                        ClutterStageView         *stage_view)
{
  const ClutterOffloadEntry *entry;
  MtkRectangle view_rect, entry_extents;

  if (clutter_offloadable_actors_get_n_entries (offload_actors) == 0)
    return NULL;

  entry = clutter_offloadable_actors_get_entry (offload_actors, 0);

  if (entry->type != CLUTTER_OFFLOAD_TYPE_OPAQUE)
    return NULL;

  if (!META_IS_SURFACE_ACTOR (entry->actor))
    return NULL;

  clutter_stage_view_get_layout (stage_view, &view_rect);
  entry_extents = mtk_region_get_extents (entry->region);

  if (entry_extents.x > view_rect.x ||
      entry_extents.y > view_rect.y ||
      entry_extents.x + entry_extents.width <
        view_rect.x + view_rect.width ||
      entry_extents.y + entry_extents.height <
        view_rect.y + view_rect.height)
    return NULL;

  return META_SURFACE_ACTOR (entry->actor);
}

void
meta_compositor_view_native_before_paint (MetaCompositorViewNative *view_native,
                                          MetaCompositor           *compositor,
                                          ClutterFrame             *frame)
{
  MetaCompositorView *compositor_view = META_COMPOSITOR_VIEW (view_native);
  ClutterStageView *stage_view;
  ClutterStage *stage;
  CoglFramebuffer *framebuffer;
  MetaSurfaceActor *fullscreen_surface;
  g_autoptr (ClutterOffloadableActors) offload_actors = NULL;
  g_autoptr (MtkRegion) poisoned = NULL;
  g_autoptr (MtkRegion) occupied = NULL;

  stage_view = meta_compositor_view_get_stage_view (compositor_view);
  stage = meta_compositor_get_stage (compositor);

  offload_actors = clutter_offloadable_actors_new ();
  poisoned = mtk_region_create ();
  occupied = mtk_region_create ();

  clutter_offloadable_analyze_children (CLUTTER_ACTOR (stage),
                                        stage_view,
                                        offload_actors,
                                        poisoned, occupied);

  fullscreen_surface = get_fullscreen_surface (offload_actors, stage_view);
  update_fullscreen_surface_tracking (view_native, fullscreen_surface);

  framebuffer = clutter_stage_view_get_onscreen (stage_view);
  if (META_IS_ONSCREEN_NATIVE (framebuffer))
    {
      meta_onscreen_native_allow_vrr (META_ONSCREEN_NATIVE (framebuffer),
                                      fullscreen_surface != NULL);
    }

  meta_scanout_manager_assign_planes (view_native->scanout_manager,
                                      offload_actors,
                                      compositor,
                                      frame);
}

/* The scanout manager's previous assignment holds a ref on a
 * MetaDrmBufferGbm whose gbm_surface is destroyed when the stage view
 * drops its framebuffer in finalize. Clear the scanout manager before
 * that happens to avoid a use-after-free.
 *
 * This is a workaround for MetaDrmBufferGbm holding a borrowed pointer
 * to a gbm_surface it doesn't own — the surface lifetime should be tied
 * to the buffer, not the other way around. */
static void
on_stage_view_destroy (ClutterStageView         *stage_view,
                       MetaCompositorViewNative *view_native)
{
  g_clear_object (&view_native->scanout_manager);
  view_native->stage_view_destroy_handler_id = 0;
}

MetaCompositorViewNative *
meta_compositor_view_native_new (ClutterStageView *stage_view)
{
  MetaCompositorViewNative *view_native;

  g_assert (stage_view != NULL);

  view_native = g_object_new (META_TYPE_COMPOSITOR_VIEW_NATIVE,
                              "stage-view", stage_view,
                              NULL);
  view_native->scanout_manager = meta_scanout_manager_new (stage_view);
  view_native->stage_view_destroy_handler_id =
    g_signal_connect (stage_view, "destroy",
                      G_CALLBACK (on_stage_view_destroy),
                      view_native);

  return view_native;
}

static void
meta_compositor_view_native_dispose (GObject *object)
{
  MetaCompositorViewNative *view_native = META_COMPOSITOR_VIEW_NATIVE (object);
  MetaCompositorView *compositor_view = META_COMPOSITOR_VIEW (view_native);
  ClutterStageView *stage_view;

  stage_view = meta_compositor_view_get_stage_view (compositor_view);

  clear_fullscreen_surface (view_native);
  g_clear_signal_handler (&view_native->stage_view_destroy_handler_id,
                           stage_view);
  g_clear_object (&view_native->scanout_manager);

  G_OBJECT_CLASS (meta_compositor_view_native_parent_class)->dispose (object);
}

static void
meta_compositor_view_native_class_init (MetaCompositorViewNativeClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = meta_compositor_view_native_dispose;
}

static void
meta_compositor_view_native_init (MetaCompositorViewNative *view_native)
{
}
