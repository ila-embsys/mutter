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

#include "backends/meta-crtc.h"
#include "backends/native/meta-crtc-kms.h"
#include "backends/native/meta-onscreen-native.h"
#include "backends/native/meta-kms-plane.h"
#include "backends/native/meta-kms-device.h"
#include "clutter/clutter.h"
#include "compositor/compositor-private.h"
#include "compositor/meta-window-actor-private.h"
#include "compositor/meta-window-actor-wayland.h"
#include "compositor/meta-surface-actor-wayland.h"
#include "compositor/meta-shaped-texture-private.h"
#include "core/window-private.h"
#include "wayland/meta-wayland-surface-private.h"
#include "wayland/meta-wayland-buffer.h"

#include <graphene-rect.h>
#include "mtk/mtk-region.h"

static void update_fullscreen_actor (MetaCompositorViewNative *view_native,
                                     MetaSurfaceActor         *fullscreen_actor);

typedef struct _MetaCursorOverlapData MetaCursorOverlapData;
struct _MetaCursorOverlapData
{
  MetaCompositor *compositor;
  ClutterStageView *stage_view;
  gboolean has_overlap;
};

struct _MetaCompositorViewNative
{
  MetaCompositorView parent;

  MetaWaylandSurface *scanout_candidate;

  MetaSurfaceActor *fullscreen_actor;

  gulong fullscreen_surface_repaint_scheduled_id;
  gulong fullscreen_surface_update_scheduled_id;
  gulong fullscreen_actor_destroy_id;
};

G_DEFINE_TYPE (MetaCompositorViewNative, meta_compositor_view_native,
               META_TYPE_COMPOSITOR_VIEW)

static void
maybe_set_fullscreen_update_time (MetaSurfaceActor         *surface_actor,
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
on_fullscreen_actor_destroyed (MetaSurfaceActor         *surface_actor,
                               MetaCompositorViewNative *view_native)
{
  update_fullscreen_actor (view_native, NULL);
}

static void
update_scanout_candidate (MetaCompositorViewNative *view_native,
                          MetaWaylandSurface       *surface,
                          MetaCrtc                 *crtc)
{
  if (view_native->scanout_candidate &&
      view_native->scanout_candidate != surface)
    {
      meta_wayland_surface_set_scanout_candidate (view_native->scanout_candidate,
                                                  NULL);
      g_clear_weak_pointer (&view_native->scanout_candidate);
    }

  if (surface)
    {
      meta_wayland_surface_set_scanout_candidate (surface, crtc);
      g_set_weak_pointer (&view_native->scanout_candidate,
                          surface);
    }
}

static MetaSurfaceActor *
find_candidate (MetaCompositorView *compositor_view,
                MetaCompositor     *compositor,
                const char         *topic)
{
  ClutterStageView *stage_view =
    meta_compositor_view_get_stage_view (compositor_view);
  MetaWindowActor *window_actor;
  MetaSurfaceActor *surface_actor;
  MtkRectangle view_rect;
  ClutterActorBox actor_box;

  if (meta_compositor_is_unredirect_inhibited (compositor))
    {
      meta_topic (META_DEBUG_RENDER,
                  "No %s candidate: unredirect inhibited",
                  topic);
      return NULL;
    }

  window_actor =
    meta_compositor_view_get_top_window_actor (compositor_view);
  if (!window_actor)
    {
      meta_topic (META_DEBUG_RENDER,
                  "No %s candidate: no top window actor",
                  topic);
      return NULL;
    }

  if (meta_window_actor_effect_in_progress (window_actor))
    {
      meta_topic (META_DEBUG_RENDER,
                  "No %s candidate: window-actor effects in progress",
                  topic);
      return NULL;
    }

  if (clutter_actor_has_transitions (CLUTTER_ACTOR (window_actor)))
    {
      meta_topic (META_DEBUG_RENDER,
                  "No %s candidate: window-actor has transition",
                  topic);
      return NULL;
    }

  clutter_stage_view_get_layout (stage_view, &view_rect);

  if (!clutter_actor_get_paint_box (CLUTTER_ACTOR (window_actor),
                                    &actor_box))
    {
      meta_topic (META_DEBUG_RENDER,
                  "No %s candidate: no window actor paint-box",
                  topic);
      return NULL;
    }

  if (actor_box.x1 < view_rect.x ||
      actor_box.y1 < view_rect.y ||
      actor_box.x2 > view_rect.x + view_rect.width ||
      actor_box.y2 > view_rect.y + view_rect.height)
    {
      meta_topic (META_DEBUG_RENDER,
                  "No %s candidate: paint-box (%f,%f,%f,%f) does "
                  "not match stage-view layout (%d,%d,%d,%d)",
                  topic,
                  actor_box.x1, actor_box.y1,
                  actor_box.x2 - actor_box.x1, actor_box.y2 - actor_box.y1,
                  view_rect.x, view_rect.y, view_rect.width, view_rect.height);
      return NULL;
    }

  surface_actor = meta_window_actor_get_scanout_candidate (window_actor);
  if (!surface_actor)
    {
      meta_topic (META_DEBUG_RENDER,
                  "No %s candidate: window-actor has no scanout candidate",
                  topic);
      return NULL;
    }

  if (meta_surface_actor_is_effectively_obscured (surface_actor))
    {
      meta_topic (META_DEBUG_RENDER,
                  "No %s candidate: surface-actor is obscured",
                  topic);
      return NULL;
    }

  return surface_actor;
}

static gboolean
surfaceHasDMABufAttribs (MetaWaylandSurface *surface)
{
  if (!surface)
    return FALSE;

  MetaWaylandBuffer *buffer = meta_wayland_surface_get_buffer (surface);

  if (!buffer)
    return FALSE;

  MetaWaylandDmaBufBuffer *dma_buf = meta_wayland_dma_buf_from_buffer (buffer);

  return buffer->type == META_WAYLAND_BUFFER_TYPE_DMA_BUF && dma_buf != NULL;
}

static gboolean
region_intersects_rect (MtkRegion     *region,
                        MtkRectangle  *rect)
{
  return mtk_region_contains_rectangle (region, rect) != MTK_REGION_OVERLAP_OUT;
}

static void
map_rect_to_device_space (MetaCompositorView *compositor_view,
                          ClutterActor       *clutter_actor,
                          MtkRectangle       *device_space_rect)
{
  ClutterStageView *stage_view = meta_compositor_view_get_stage_view (compositor_view);
  graphene_rect_t actor_rect;

  clutter_actor_get_transformed_extents (clutter_actor, &actor_rect);

  float device_scale = clutter_stage_view_get_scale (stage_view);

  *device_space_rect = (MtkRectangle) {
    .x = (int) floorf (actor_rect.origin.x * device_scale),
    .y = (int) floorf (actor_rect.origin.y * device_scale),
    .width = (int) floorf (actor_rect.size.width * device_scale),
    .height = (int) floorf (actor_rect.size.height * device_scale)
  };
}

static void
map_region_to_device_space (MetaCompositorView  *compositor_view,
                            ClutterActor        *clutter_actor,
                            MtkRegion           *region,
                            MtkRegion          **device_space_region)
{
  MtkRegionBuilder builder;
  mtk_region_builder_init (&builder);

  int num_rectangles = mtk_region_num_rectangles (region);

  for(int i = 0; i < num_rectangles; i++)
    {
      MtkRectangle rect = mtk_region_get_rectangle (region, i);
      map_rect_to_device_space (compositor_view, clutter_actor, &rect);
      mtk_region_builder_add_rectangle (&builder, rect.x, rect.y, rect.width, rect.height);
    }

  *device_space_region = mtk_region_builder_finish (&builder);
}

static gboolean
validate_color_state_for_scanout (MetaCompositorView *compositor_view,
                                  MetaSurfaceActor   *surface_actor)
{
  ClutterStageView *stage_view =
    meta_compositor_view_get_stage_view (compositor_view);
  MetaStageView *view = META_STAGE_VIEW (stage_view);
  MetaSurfaceActorWayland *surface_actor_wayland = META_SURFACE_ACTOR_WAYLAND (surface_actor);
  MetaWaylandSurface *surface = meta_surface_actor_wayland_get_surface (surface_actor_wayland);

  if (!(meta_get_debug_paint_flags () &
        META_DEBUG_PAINT_IGNORE_COLOR_STATE_FOR_DIRECT_SCANOUT))
    {
      ClutterColorState *output_color_state;
      ClutterColorState *surface_color_state;

      output_color_state =
        clutter_stage_view_get_output_color_state (CLUTTER_STAGE_VIEW (view));
      surface_color_state =
        clutter_actor_get_color_state (CLUTTER_ACTOR (surface_actor));
      if (clutter_color_pipeline_shader_needs_color_state (surface_color_state,
                                                           output_color_state, 0))
        {
          meta_topic (META_DEBUG_RENDER,
                      "No direct scanout candidate: "
                      "surface color state (%s) needs mapping to the output's (%s)",
                      clutter_color_state_to_string (surface_color_state),
                      clutter_color_state_to_string (output_color_state));
          return FALSE;
        }
    }

  if (!(meta_get_debug_paint_flags () &
        META_DEBUG_PAINT_IGNORE_COLOR_STATE_FOR_DIRECT_SCANOUT))
    {
      MetaMultiTextureCoefficients coeffs;

      coeffs = surface->applied_state.coeffs;
      if (coeffs != META_MULTI_TEXTURE_COEFFICIENTS_NONE &&
          coeffs != META_MULTI_TEXTURE_COEFFICIENTS_IDENTITY_FULL &&
          coeffs != META_MULTI_TEXTURE_COEFFICIENTS_BT709_LIMITED)
        {
          meta_topic (META_DEBUG_RENDER,
                      "No direct scanout candidate: unsupported color model");
          return FALSE;
        }
    }

  return TRUE;
}

static gboolean
has_overlapping_cursor_overlay_foreach (ClutterStage  *stage,
                                        ClutterSprite *sprite,
                                        gpointer       user_data)
{
  MetaCursorOverlapData *data = user_data;
  MetaBackend *backend = meta_compositor_get_backend (data->compositor);
  MetaCursorRenderer *cursor_renderer;
  ClutterCursor *cursor;
  MtkRectangle view_rect;
  graphene_rect_t graphene_view_rect;
  graphene_rect_t cursor_rect;

  cursor_renderer = meta_backend_get_cursor_renderer_for_sprite (backend,
                                                                 sprite);
  if (!cursor_renderer)
    return TRUE;
  if (!meta_cursor_renderer_needs_overlay_on_view (cursor_renderer,
                                                   data->stage_view))
    return TRUE;

  cursor = meta_cursor_renderer_get_cursor (cursor_renderer);
  cursor_rect = meta_cursor_renderer_calculate_rect (cursor_renderer,
                                                     cursor);

  clutter_stage_view_get_layout (data->stage_view, &view_rect);
  graphene_view_rect = mtk_rectangle_to_graphene_rect (&view_rect);

  data->has_overlap |= graphene_rect_intersection (&graphene_view_rect,
                                                   &cursor_rect,
                                                   NULL);
  return !data->has_overlap;
}

static gboolean
surface_actor_has_overlapping_cursor_overlays (MetaCompositor     *compositor,
                                               MetaCompositorView *compositor_view,
                                               MetaSurfaceActor   *surface_actor)
{
  MetaBackend *backend = meta_compositor_get_backend (compositor);
  ClutterStage *stage = CLUTTER_STAGE (meta_backend_get_stage (backend));
  ClutterStageView *stage_view =
    meta_compositor_view_get_stage_view (compositor_view);
  MetaCursorOverlapData data = { compositor, stage_view, FALSE };

  clutter_stage_foreach_sprite (stage,
                                has_overlapping_cursor_overlay_foreach,
                                &data);

  return data.has_overlap;
}

static gboolean __attribute__((unused))
find_overlay_candidates (MetaCompositorView *compositor_view,
                         MetaCompositor     *compositor,
                         ClutterActor       *clutter_actor,
                         guint32             max_scanout_count,
                         guint32             max_overlay_count,
                         MtkRegion          *occupied_region,
                         MtkRegion          *opaque_region,
                         MtkRegion          *effected_region,
                         GList             **overlay_candidates)
{
  ClutterStageView *stage_view;
  ClutterActorBox actor_bounding_box;
  MtkRectangle view_rect;

  stage_view = meta_compositor_view_get_stage_view (compositor_view);
  clutter_actor_get_content_box (clutter_actor, &actor_bounding_box);
  clutter_stage_view_get_layout (stage_view, &view_rect);

  graphene_rect_t graphene_view_rect;
  graphene_rect_t graphene_actor_rect_projected;
  graphene_view_rect = mtk_rectangle_to_graphene_rect (&view_rect);
  clutter_actor_get_transformed_extents (clutter_actor, &graphene_actor_rect_projected);

  gboolean is_visible = clutter_actor_is_visible (clutter_actor);
  guint8 opacity = clutter_actor_get_opacity (clutter_actor);
  gfloat actor_area = clutter_actor_box_get_area (&actor_bounding_box);
  bool intersects = graphene_rect_intersection (&graphene_view_rect, &graphene_actor_rect_projected, NULL);
  if (!is_visible ||
      opacity == 0 ||
      actor_area == 0.0 ||
      !intersects)
    {
      meta_topic (META_DEBUG_RENDER,
                  "No overlay candidate: window is hidden or transparent or has no content");
      return TRUE;
    }

  MtkRectangle device_space_actor_rect;
  MtkRegion *device_space_opaque_region = mtk_region_create ();

  MetaWindowActor *window_actor = NULL;
  MetaSurfaceActor *surface_actor = NULL;
  MetaSurfaceActorWayland *surface_actor_wayland = NULL;
  MetaWaylandSurface *surface = NULL;

  if(META_IS_WINDOW_ACTOR (clutter_actor))
    {
      window_actor = META_WINDOW_ACTOR (clutter_actor);

      if(meta_window_actor_effect_in_progress (window_actor))
        {
          map_rect_to_device_space (compositor_view, clutter_actor, &device_space_actor_rect);
          mtk_region_union_rectangle (effected_region, &device_space_actor_rect);
          return TRUE;
        }

      surface_actor = meta_window_actor_get_scanout_candidate (window_actor);
      if (surface_actor)
        {
          surface_actor_wayland = META_SURFACE_ACTOR_WAYLAND (surface_actor);
          surface = meta_surface_actor_wayland_get_surface (surface_actor_wayland);
        }
      else
        return FALSE;
    }

  gboolean has_surface_actor = surface_actor != NULL;
  gboolean has_dmabuf_attribs = FALSE;
  gboolean surface_frequent_updates = FALSE;
  gboolean is_fully_opaque = FALSE;
  gboolean valid_color_state_for_scanout = FALSE;
  MtkRegionOverlap opaque_region_overlap;
  gboolean notCoveredByOpaque;
  gboolean notIntersectsEffected;

  map_rect_to_device_space (compositor_view, clutter_actor, &device_space_actor_rect);
  opaque_region_overlap = mtk_region_contains_rectangle (opaque_region, &device_space_actor_rect);
  notCoveredByOpaque = (opaque_region_overlap != MTK_REGION_OVERLAP_IN);
  notIntersectsEffected = !region_intersects_rect (effected_region, &device_space_actor_rect);

  if (has_surface_actor)
    {
      MetaShapedTexture *texture;

      has_dmabuf_attribs = surfaceHasDMABufAttribs (surface);
      valid_color_state_for_scanout = validate_color_state_for_scanout (compositor_view, surface_actor);

      gint64 frametime_us;

      if (meta_surface_actor_get_frame_time_estimation (surface_actor, &frametime_us))
        {
          surface_frequent_updates = frametime_us < (G_TIME_SPAN_SECOND / 20);
        }

      is_fully_opaque = meta_surface_actor_is_opaque (surface_actor);

      texture = meta_surface_actor_get_texture (surface_actor);
      map_region_to_device_space (compositor_view,
                                  clutter_actor,
                                  meta_shaped_texture_get_opaque_region (texture),
                                  &device_space_opaque_region);
    }

  if (has_surface_actor &&
      surface_frequent_updates &&
      has_dmabuf_attribs &&
      valid_color_state_for_scanout &&
      is_fully_opaque &&
      notCoveredByOpaque &&
      notIntersectsEffected)
    {
      gboolean is_occupied_intersects = region_intersects_rect (occupied_region, &device_space_actor_rect);
      if (is_occupied_intersects)
        {
          gboolean is_opaque = mtk_region_contains_rectangle (device_space_opaque_region, &device_space_actor_rect) == MTK_REGION_OVERLAP_IN;
          if (!is_opaque)
            return FALSE;

          // *underlay_candidates = g_list_prepend (*underlay_candidates, surface_actor);
        }
      else
        *overlay_candidates = g_list_prepend (*overlay_candidates, surface_actor);

      // TODO Also Check Max Scanout & Underlay Count once underlay is considered
      if (g_list_length (*overlay_candidates) > max_overlay_count)
        {
          meta_topic (META_DEBUG_RENDER, "Plane Limits Reached");
          return FALSE;
        }
    }
  else
    mtk_region_union_rectangle (occupied_region, &device_space_actor_rect);


  mtk_region_union (opaque_region, device_space_opaque_region);


  *overlay_candidates = g_list_reverse (*overlay_candidates);
  //  *underlay_candidates = g_list_reverse (*underlay_candidates);

  GList *children = clutter_actor_get_children (clutter_actor);

  for(GList *iter = children; iter != NULL; iter = iter->next)
    {
      ClutterActor *child_actor = CLUTTER_ACTOR (iter->data);

      if (child_actor)
        {
          if (!find_overlay_candidates (compositor_view, compositor,
                                        (child_actor),
                                        max_scanout_count,
                                        max_overlay_count,
                                        occupied_region, opaque_region, effected_region,
                                        overlay_candidates))
            {
              return FALSE;
            }
        }
    }

  return TRUE;
}

static gboolean
find_scanout_candidate (MetaCompositorView  *compositor_view,
                        MetaCompositor      *compositor,
                        MetaCrtc           **crtc_out,
                        CoglOnscreen       **onscreen_out,
                        MetaWaylandSurface **surface_out)
{
  ClutterStageView *stage_view =
    meta_compositor_view_get_stage_view (compositor_view);
  MetaStageView *view = META_STAGE_VIEW (stage_view);
  MetaRendererView *renderer_view = META_RENDERER_VIEW (stage_view);
  MetaCrtc *crtc;
  CoglFramebuffer *framebuffer;
  MetaSurfaceActor *surface_actor;
  MetaSurfaceActorWayland *surface_actor_wayland;
  MetaWaylandSurface *surface;
  CoglOnscreen *onscreen;

  if (meta_get_debug_paint_flags () & META_DEBUG_PAINT_DISABLE_DIRECT_SCANOUT)
    return FALSE;


  surface_actor = find_candidate (compositor_view,
                                  compositor,
                                  "direct scanout");
  if (!surface_actor)
    return FALSE;

  if (!meta_stage_view_is_cursor_overlay_inhibited (view) &&
      surface_actor_has_overlapping_cursor_overlays (compositor,
                                                     compositor_view,
                                                     surface_actor))
    {
      meta_topic (META_DEBUG_RENDER,
                  "No direct scanout candidate: overlapping software cursors");
      return FALSE;
    }

  crtc = meta_renderer_view_get_crtc (renderer_view);
  if (!META_IS_CRTC_KMS (crtc))
    {
      meta_topic (META_DEBUG_RENDER,
                  "No direct scanout candidate: no KMS CRTC");
      return FALSE;
    }

  framebuffer = clutter_stage_view_get_onscreen (stage_view);
  if (!COGL_IS_ONSCREEN (framebuffer))
    {
      meta_topic (META_DEBUG_RENDER,
                  "No direct scanout candidate: no onscreen framebuffer");
      return FALSE;
    }

  onscreen = COGL_ONSCREEN (framebuffer);
  if (!meta_onscreen_native_supports_direct_scanout (onscreen))
    {
      meta_topic (META_DEBUG_RENDER,
                  "No direct scanout candidate: "
                  "onscreen framebuffer doesn't support direct scanout");
      return FALSE;
    }

  if (clutter_stage_view_has_shadowfb (stage_view))
    {
      meta_topic (META_DEBUG_RENDER,
                  "No direct scanout candidate: stage-view has shadowfb");
      return FALSE;
    }

  if (!(meta_get_debug_paint_flags () &
        META_DEBUG_PAINT_IGNORE_COLOR_STATE_FOR_DIRECT_SCANOUT))
    {
      ClutterColorState *output_color_state;
      ClutterColorState *surface_color_state;

      output_color_state =
        clutter_stage_view_get_output_color_state (CLUTTER_STAGE_VIEW (view));
      surface_color_state =
        clutter_actor_get_color_state (CLUTTER_ACTOR (surface_actor));
      if (clutter_color_pipeline_shader_needs_color_state (surface_color_state,
                                                           output_color_state, 0))
        {
          meta_topic (META_DEBUG_RENDER,
                      "No direct scanout candidate: "
                      "surface color state (%s) needs mapping to the output's (%s)",
                      clutter_color_state_to_string (surface_color_state),
                      clutter_color_state_to_string (output_color_state));
          return FALSE;
        }
    }

  surface_actor_wayland = META_SURFACE_ACTOR_WAYLAND (surface_actor);
  surface = meta_surface_actor_wayland_get_surface (surface_actor_wayland);
  if (!surface)
    {
      meta_topic (META_DEBUG_RENDER,
                  "No direct scanout candidate: no surface");
      return FALSE;
    }

  if (!(meta_get_debug_paint_flags () &
        META_DEBUG_PAINT_IGNORE_COLOR_STATE_FOR_DIRECT_SCANOUT))
    {
      MetaMultiTextureCoefficients coeffs;

      coeffs = surface->applied_state.coeffs;
      if (coeffs != META_MULTI_TEXTURE_COEFFICIENTS_NONE &&
          coeffs != META_MULTI_TEXTURE_COEFFICIENTS_IDENTITY_FULL &&
          coeffs != META_MULTI_TEXTURE_COEFFICIENTS_BT709_LIMITED)
        {
          meta_topic (META_DEBUG_RENDER,
                      "No direct scanout candidate: unsupported color model");
          return FALSE;
        }
    }

  *crtc_out = crtc;
  *onscreen_out = onscreen;
  *surface_out = surface;

  return TRUE;
}

static void
try_assign_next_scanout (MetaCompositorView *compositor_view,
                         CoglOnscreen       *onscreen,
                         MetaWaylandSurface *surface)
{
  ClutterStageView *stage_view;
  g_autoptr (CoglScanout) scanout = NULL;

  stage_view = meta_compositor_view_get_stage_view (compositor_view);
  scanout = meta_wayland_surface_try_acquire_scanout (surface,
                                                      onscreen,
                                                      stage_view);
  if (!scanout)
    {
      meta_topic (META_DEBUG_RENDER,
                  "Could not acquire scanout");
      return;
    }

  meta_topic (META_DEBUG_RENDER, "Assigning scanout to stage view");
  clutter_stage_view_assign_next_scanout (stage_view, scanout);
}

void
meta_compositor_view_native_maybe_assign_scanout (MetaCompositorViewNative *view_native,
                                                  MetaCompositor           *compositor)
{
  MetaCompositorView *compositor_view = META_COMPOSITOR_VIEW (view_native);
  MetaCrtc *crtc = NULL;
  CoglOnscreen *onscreen = NULL;
  MetaWaylandSurface *surface = NULL;
  gboolean candidate_found;

  candidate_found = find_scanout_candidate (compositor_view,
                                            compositor,
                                            &crtc,
                                            &onscreen,
                                            &surface);
  if (candidate_found)
    {
      try_assign_next_scanout (compositor_view,
                               onscreen,
                               surface);
    }

  update_scanout_candidate (view_native, surface, crtc);
}

static MetaSurfaceActor *
find_fullscreen_candidate (MetaCompositorView *compositor_view,
                           MetaCompositor     *compositor)
{
  return find_candidate (compositor_view, compositor, "fullscreen");
}

static void
update_fullscreen_actor (MetaCompositorViewNative *view_native,
                         MetaSurfaceActor         *surface_actor)
{
  MetaCompositorView *compositor_view =
    META_COMPOSITOR_VIEW (view_native);
  ClutterStageView *stage_view;
  CoglFramebuffer *framebuffer;

  g_clear_signal_handler (&view_native->fullscreen_surface_repaint_scheduled_id,
                          view_native->fullscreen_actor);
  g_clear_signal_handler (&view_native->fullscreen_surface_update_scheduled_id,
                          view_native->fullscreen_actor);
  g_clear_signal_handler (&view_native->fullscreen_actor_destroy_id,
                          view_native->fullscreen_actor);

  if (surface_actor)
    {
      view_native->fullscreen_surface_repaint_scheduled_id =
        g_signal_connect (surface_actor, "repaint-scheduled",
                          G_CALLBACK (maybe_set_fullscreen_update_time),
                          view_native);
      view_native->fullscreen_surface_update_scheduled_id =
        g_signal_connect (surface_actor, "update-scheduled",
                          G_CALLBACK (maybe_set_fullscreen_update_time),
                          view_native);
      view_native->fullscreen_actor_destroy_id =
        g_signal_connect (surface_actor, "destroy",
                          G_CALLBACK (on_fullscreen_actor_destroyed),
                          view_native);
    }

  view_native->fullscreen_actor = surface_actor;

  stage_view = meta_compositor_view_get_stage_view (compositor_view);

  framebuffer = clutter_stage_view_get_onscreen (stage_view);
  if (!META_IS_ONSCREEN_NATIVE (framebuffer))
    return;

  meta_onscreen_native_allow_vrr (META_ONSCREEN_NATIVE (framebuffer),
                                  surface_actor != NULL);
}

void
meta_compositor_view_native_maybe_update_fullscreen_actor (MetaCompositorViewNative *view_native,
                                                           MetaCompositor           *compositor)
{
  MetaCompositorView *compositor_view = META_COMPOSITOR_VIEW (view_native);
  MetaSurfaceActor *fullscreen_actor;

  fullscreen_actor = find_fullscreen_candidate (compositor_view, compositor);

  if (G_LIKELY (fullscreen_actor == view_native->fullscreen_actor))
    return;

  update_fullscreen_actor (view_native, fullscreen_actor);
}

MetaCompositorViewNative *
meta_compositor_view_native_new (ClutterStageView *stage_view)
{
  g_assert (stage_view != NULL);

  return g_object_new (META_TYPE_COMPOSITOR_VIEW_NATIVE,
                       "stage-view", stage_view,
                       NULL);
}

static void
meta_compositor_view_native_dispose (GObject *object)
{
  MetaCompositorViewNative *view_native = META_COMPOSITOR_VIEW_NATIVE (object);

  g_clear_signal_handler (&view_native->fullscreen_surface_repaint_scheduled_id,
                          view_native->fullscreen_actor);
  g_clear_signal_handler (&view_native->fullscreen_surface_update_scheduled_id,
                          view_native->fullscreen_actor);
  g_clear_signal_handler (&view_native->fullscreen_actor_destroy_id,
                          view_native->fullscreen_actor);
  view_native->fullscreen_actor = NULL;

  G_OBJECT_CLASS (meta_compositor_view_native_parent_class)->dispose (object);
}

static void
meta_compositor_view_native_finalize (GObject *object)
{
  MetaCompositorViewNative *view_native = META_COMPOSITOR_VIEW_NATIVE (object);

  g_clear_weak_pointer (&view_native->scanout_candidate);

  G_OBJECT_CLASS (meta_compositor_view_native_parent_class)->finalize (object);
}

static void
meta_compositor_view_native_class_init (MetaCompositorViewNativeClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = meta_compositor_view_native_dispose;
  object_class->finalize = meta_compositor_view_native_finalize;
}

static void
meta_compositor_view_native_init (MetaCompositorViewNative *view_native)
{
}
