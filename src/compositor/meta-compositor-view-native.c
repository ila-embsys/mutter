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
#include "core/window-private.h"
#include "wayland/meta-wayland-surface-private.h"
#include "wayland/meta-wayland-buffer.h"

#include <pixman.h>
#include "mtk/mtk-region.h"

#include <drm_fourcc.h>
#include <linux/dma-buf.h>

static void update_fullscreen_actor (MetaCompositorViewNative *view_native,
                                     MetaSurfaceActor         *fullscreen_actor);

struct _MetaCompositorViewNative
{
  MetaCompositorView parent;

  MetaWaylandSurface *scanout_candidate;
  GList* scanound_candidate_list;

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

#if 0
static void
update_scanout_candidate (MetaCompositorViewNative *view_native,
                          CoglOnscreen             *onscreen,
                          GList                    *scanout_candidates,
                          MetaCrtc                 *crtc)
{
  MetaCompositorView *compositor_view = META_COMPOSITOR_VIEW (view_native);
  ClutterStageView *stage_view =
    meta_compositor_view_get_stage_view (compositor_view);
  CoglFramebuffer *framebuffer = 
    clutter_stage_view_get_onscreen (stage_view);

  if (!COGL_IS_ONSCREEN (framebuffer))
    {
      meta_topic (META_DEBUG_MOHAN,
                  "No onscreen framebuffer");
      return;
    }

  onscreen = COGL_ONSCREEN (framebuffer);
  MetaOnscreenNative *onscreen_native = META_ONSCREEN_NATIVE (onscreen);
  ClutterFrame* presented_frame = meta_onscreen_native_get_presented_frame (onscreen_native);
  
  meta_
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
#else
static void
update_scanout_candidate (MetaCompositorViewNative *view_native,
                          GList                    *scanout_candidates,
                          MetaCrtc                 *crtc)
{
  GList *iter;
  for (iter = view_native->scanound_candidate_list; iter != NULL; iter = iter->next)
  {
    MetaSurfaceActor *surface_actor = iter->data;
    MetaWaylandSurface *surface = meta_surface_actor_wayland_get_surface ( META_SURFACE_ACTOR_WAYLAND(surface_actor) );
    
    if(!g_list_find(scanout_candidates, surface_actor))
     {
      if(surface)
        meta_wayland_surface_set_scanout_candidate (surface, NULL);
     }
  }

  for (iter = scanout_candidates; iter != NULL; iter = iter->next)
  {
    MetaSurfaceActor *surface_actor = iter->data;
    MetaWaylandSurface *surface = meta_surface_actor_wayland_get_surface ( META_SURFACE_ACTOR_WAYLAND(surface_actor) );

    if(surface)
      meta_wayland_surface_set_scanout_candidate (surface, crtc);
  }

  view_native->scanound_candidate_list = scanout_candidates;
/*
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
*/
}
#endif
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

static gboolean surfaceHasDMABufAttribs (MetaWaylandSurface *surface)
{
  if (!surface)
    return FALSE;

  MetaWaylandBuffer *buffer = meta_wayland_surface_get_buffer (surface);

  if (!buffer)
    return FALSE;

  return buffer->type == META_WAYLAND_BUFFER_TYPE_DMA_BUF && buffer->dma_buf.dma_buf != NULL;
}

static gboolean region_intersects_rect (MtkRegion *region, MtkRectangle *rect)
{
  return mtk_region_contains_rectangle (region, rect) != MTK_REGION_OVERLAP_OUT;
}

static void map_rect_to_device_space (MetaCompositorView *compositor_view,
                                      ClutterActor *clutter_actor,
                                      MtkRectangle *device_space_rect)
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

static void map_region_to_device_space (MetaCompositorView *compositor_view,
                                        ClutterActor *clutter_actor,
                                        MtkRegion *region,
                                        MtkRegion **device_space_region)
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

gboolean find_overlay_candidates (MetaCompositorView *compositor_view,
                                  MetaCompositor     *compositor,
                                  ClutterActor       *clutter_actor,
                                  guint32             max_scanout_count,
                                  guint32             max_overlay_count,
                                  MtkRegion          *occupied_region,
                                  MtkRegion          *opaque_region,
                                  MtkRegion          *effected_region,
                                  GList              **overlay_candidates,
                                  GList              **underlay_candidates
                                // TODO maybe it needs to keep track window corners for better candidate selection
                                        )
{
  ClutterStageView* stage_view;
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
  MtkRegion *device_space_opaque_region = mtk_region_create();

  MetaWindowActor* window_actor = NULL;
  MetaSurfaceActor* surface_actor = NULL;
  MetaSurfaceActorWayland *surface_actor_wayland = NULL;
  MetaWaylandSurface *surface = NULL;

  if( META_IS_WINDOW_ACTOR (clutter_actor) )
  {
    window_actor = META_WINDOW_ACTOR (clutter_actor);

    if( meta_window_actor_effect_in_progress (window_actor) )
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
      {
        meta_topic (META_DEBUG_MOHAN, "[Debug] No Scanout Candidate For Window Actor");
        return FALSE;      
      }
  }

  gboolean has_surface_actor = surface_actor != NULL;
  gboolean has_dmabuf_attribs = FALSE;
  gboolean is_fully_opaque = FALSE;
  guint8 actor_opacity = 0;
  MtkRegionOverlap opaque_region_overlap;
  gboolean notCoveredByOpaque;
  gboolean notIntersectsEffected;

  map_rect_to_device_space (compositor_view, clutter_actor, &device_space_actor_rect);
  opaque_region_overlap = mtk_region_contains_rectangle (opaque_region, &device_space_actor_rect);
  notCoveredByOpaque = (opaque_region_overlap != MTK_REGION_OVERLAP_IN);
  notIntersectsEffected = !region_intersects_rect (effected_region, &device_space_actor_rect);

  if (has_surface_actor)
    {
      has_dmabuf_attribs = surfaceHasDMABufAttribs (surface);
      is_fully_opaque = meta_surface_actor_is_opaque (surface_actor);

      map_region_to_device_space (compositor_view,
                                  clutter_actor,
                                  meta_surface_actor_get_opaque_region (surface_actor),
                                  &device_space_opaque_region);
    }

    /* TODO maybe check whether the associated window has any additional
       shader effects and exclude if it causes issues on overlay planes */

    // TODO Check the window's children potential candidates for overlay planes

    // TODO check whether surface updates frequently
    // TODO check whether effected region is intersects with device rect

    if (has_surface_actor &&
        has_dmabuf_attribs &&
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
            meta_topic (META_DEBUG_MOHAN, "Adding Underlay Candidate (%#x): (%dx%d)", clutter_actor, (int) clutter_actor_box_get_width (&actor_bounding_box), (int) clutter_actor_box_get_height (&actor_bounding_box));
            *underlay_candidates = g_list_prepend (*underlay_candidates, surface_actor);
          }
        else {
          meta_topic (META_DEBUG_MOHAN, "Adding Overlay Candidate (%#x): (%dx%d)", clutter_actor, (int) clutter_actor_box_get_width (&actor_bounding_box), (int) clutter_actor_box_get_height (&actor_bounding_box));
          meta_wayland_dma_buf_supported_planes (meta_wayland_surface_get_buffer (surface), stage_view);
          *overlay_candidates = g_list_prepend (*overlay_candidates, surface_actor);
        }
        // TODO Also Check Max Scanout & Underlay Count once underlay is considered
        if (g_list_length (*overlay_candidates) > max_overlay_count)
          {
            meta_topic (META_DEBUG_MOHAN, "Plane Budget exhausted: Overlay: %d MaxOverlay: %d", g_list_length (*overlay_candidates), max_overlay_count);
            return FALSE;
          }
      }
    else {
//      meta_topic (META_DEBUG_MOHAN, "Adding Candidate (%#x) to Occupied Region: Rect: (%dx%d)", clutter_actor, (int) clutter_actor_box_get_width (&actor_bounding_box), (int) clutter_actor_box_get_height (&actor_bounding_box));
//      meta_topic (META_DEBUG_MOHAN, "HasSurfaceActor: %d HasDmaBuf: %d IsFullyOpaque: %d notCoveredOpaque: %d", has_surface_actor, has_dmabuf_attribs, is_fully_opaque, notCoveredByOpaque);
      mtk_region_union_rectangle (occupied_region, &device_space_actor_rect);
    }

//    meta_topic (META_DEBUG_MOHAN, "Adding Candidate (%#x) to Opaque Region: %d, %d, (%dx%d)", clutter_actor, mtk_region_get_extents(device_space_opaque_region).x, mtk_region_get_extents(device_space_opaque_region).y, mtk_region_get_extents(device_space_opaque_region).width, mtk_region_get_extents(device_space_opaque_region).height );
    mtk_region_union (opaque_region, device_space_opaque_region);
    
    *overlay_candidates = g_list_reverse (*overlay_candidates);
    *underlay_candidates = g_list_reverse (*underlay_candidates);
  
    GList* children = clutter_actor_get_children (clutter_actor);

    for(GList* iter = children; iter != NULL; iter = iter->next)
      {
        ClutterActor *child_actor = CLUTTER_ACTOR (iter->data);

        if (child_actor)
          {
            if (!find_overlay_candidates (compositor_view, compositor,
                                          (child_actor),
                                          max_scanout_count,
                                          max_overlay_count,
                                          occupied_region, opaque_region, effected_region,
                                          overlay_candidates, underlay_candidates))
              {
                return FALSE;
              }
          }
      }

  return TRUE;
}

#if 0
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
  MetaBackend *backend = meta_compositor_get_backend (compositor);
  MetaCursorTracker *cursor_tracker =
    meta_backend_get_cursor_tracker (backend);
  MetaCursorRenderer *cursor_renderer =
    meta_backend_get_cursor_renderer (backend);
  MetaCrtc *crtc;
  CoglFramebuffer *framebuffer;
  MetaSurfaceActor *surface_actor;
  MetaSurfaceActorWayland *surface_actor_wayland;
  MetaWaylandSurface *surface;

  if (meta_get_debug_paint_flags () & META_DEBUG_PAINT_DISABLE_DIRECT_SCANOUT)
    return FALSE;

  GList* window_actors = meta_compositor_get_window_actors (compositor);
  MtkRegion* occupied_region = mtk_region_create();
  MtkRegion* opaque_region = mtk_region_create();
  GList* overlay_candidates = NULL;
  GList* underlay_candidates = NULL;
  GList* reverse_iter = NULL;
  
  meta_topic (META_DEBUG_MOHAN, "***** -- Finding Overlay Candidates for Window Group: %d window actors -- *****", g_list_length (window_actors));
  for (reverse_iter = g_list_last (window_actors); reverse_iter != NULL; reverse_iter = reverse_iter->prev)
    {
      ClutterActor *clutter_actor = CLUTTER_ACTOR (reverse_iter->data);
      find_overlay_candidates (compositor_view, compositor,
                               clutter_actor,
                               occupied_region, opaque_region,
                               &overlay_candidates, &underlay_candidates);
      // meta_topic (META_DEBUG_MOHAN, "Next Candidate Tree");
    }
   meta_topic (META_DEBUG_MOHAN, "Overlay Candidates: %d Underlay Candidates: %d", g_list_length (overlay_candidates), g_list_length (underlay_candidates));
   meta_topic (META_DEBUG_MOHAN, "***** -- Finished Candidate Findings -- *****");
  /*surface_actor = find_candidate (compositor_view,
                                  compositor,
                                  "direct scanout");
  if (!surface_actor)
    return FALSE;
  */
  if (meta_cursor_renderer_needs_overlay (cursor_renderer) &&
      !meta_stage_view_is_cursor_overlay_inhibited (view) &&
      meta_cursor_tracker_get_pointer_visible (cursor_tracker))
    {
      CoglTexture *cursor_sprite;
      MtkRectangle view_rect;
      graphene_rect_t graphene_view_rect;
      graphene_rect_t cursor_rect;
      graphene_point_t position;
      float scale;
      int hotspot_x;
      int hotspot_y;

      meta_cursor_tracker_get_pointer (cursor_tracker, &position, NULL);
      meta_cursor_tracker_get_hot (cursor_tracker, &hotspot_x, &hotspot_y);

      scale = (clutter_stage_view_get_scale (stage_view) *
               meta_cursor_tracker_get_scale (cursor_tracker));

      cursor_sprite = meta_cursor_tracker_get_sprite (cursor_tracker);
      g_return_val_if_fail (cursor_sprite != NULL, FALSE);

      graphene_rect_init (&cursor_rect,
                          position.x - (hotspot_x * scale),
                          position.y - (hotspot_y * scale),
                          cogl_texture_get_width (cursor_sprite) * scale,
                          cogl_texture_get_height (cursor_sprite) * scale);

      clutter_stage_view_get_layout (stage_view, &view_rect);
      graphene_view_rect = mtk_rectangle_to_graphene_rect (&view_rect);
      if (graphene_rect_intersection (&graphene_view_rect,
                                      &cursor_rect,
                                      NULL))
        {
          meta_topic (META_DEBUG_RENDER,
                      "No direct scanout candidate: using software cursor");
          return FALSE;
        }
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
      if (clutter_color_state_needs_mapping (surface_color_state, output_color_state))
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
  *onscreen_out = COGL_ONSCREEN (framebuffer);
  *surface_out = surface;

  return TRUE;
}
#else

static void
probe_plane_budget (MetaCompositorView  *compositor_view,
                    guint32             *max_scanout_plane_count,
                    guint32             *overlay_plane_count)
{
  ClutterStageView *stage_view =
    meta_compositor_view_get_stage_view (compositor_view);
  MetaStageView *view = META_STAGE_VIEW (stage_view);
  MetaRendererView *renderer_view = META_RENDERER_VIEW (stage_view);
  MetaCrtcKms *crtc_kms = META_CRTC_KMS (meta_renderer_view_get_crtc (renderer_view));
  MetaKmsCrtc *kms_crtc = meta_crtc_kms_get_kms_crtc (crtc_kms);
  MetaKmsDevice *kms_device = meta_kms_crtc_get_device (kms_crtc);

  GList *planes = meta_kms_device_get_planes (kms_device);
  GList *iter = NULL;

  *max_scanout_plane_count = 0;
  *overlay_plane_count = 0;

  // TODO Need to consider budget for underlays
  // TODO Maybe determine plane budget based on ZPos property of KMS planes
  for (iter = planes; iter != NULL; iter = iter->next)
    {
      MetaKmsPlane *kms_plane = iter->data;

      if (meta_kms_plane_get_plane_type (kms_plane) == META_KMS_PLANE_TYPE_OVERLAY)
        (*overlay_plane_count)++;
    }

  *max_scanout_plane_count = *overlay_plane_count;
}

static gboolean
find_scanout_candidate (MetaCompositorView  *compositor_view,
                        MetaCompositor      *compositor,
                        MetaCrtc           **crtc_out,
                        CoglOnscreen       **onscreen_out,
                        GList              **overlay_candidates_out)
{
  ClutterStageView *stage_view =
    meta_compositor_view_get_stage_view (compositor_view);
  MetaStageView *view = META_STAGE_VIEW (stage_view);
  MetaRendererView *renderer_view = META_RENDERER_VIEW (stage_view);
  MetaCrtc *crtc;
  CoglFramebuffer *framebuffer;

  if (meta_get_debug_paint_flags () & META_DEBUG_PAINT_DISABLE_DIRECT_SCANOUT)
    return FALSE;

  GList* window_actors = meta_compositor_get_window_actors (compositor);
  MtkRegion* occupied_region = mtk_region_create();
  MtkRegion* opaque_region = mtk_region_create();
  MtkRegion* effected_region = mtk_region_create();
  guint32 max_scanout_count;
  guint32 max_overlay_count;
  GList* overlay_candidates = NULL;
  GList* underlay_candidates = NULL;
  GList* iter = NULL;
  
  probe_plane_budget (compositor_view, &max_scanout_count, &max_overlay_count);
  meta_topic (META_DEBUG_MOHAN, "***** -- Finding Overlay Candidates for Window Group: %d window actors -- *****", g_list_length (window_actors));
  meta_topic (META_DEBUG_MOHAN, "Plane Budget: %d scanout planes %d overlay planes", max_scanout_count, max_overlay_count);
  for (iter = g_list_last (window_actors); iter != NULL; iter = iter->prev)
    {
      ClutterActor *clutter_actor = CLUTTER_ACTOR (iter->data);
      if (!find_overlay_candidates (compositor_view, compositor,
                                     clutter_actor,
                                     max_scanout_count,
                                     max_overlay_count,
                                     occupied_region, opaque_region, effected_region,
                                     &overlay_candidates, &underlay_candidates))
        {
          // Fallback to Gfx compositing
          meta_topic (META_DEBUG_MOHAN, "Falling back to Gfx Compositing");
          g_list_free (overlay_candidates);
          g_list_free (underlay_candidates);
          return FALSE;
        }
      // meta_topic (META_DEBUG_MOHAN, "Next Candidate Tree");
    }
  meta_topic (META_DEBUG_MOHAN, "Overlay Candidates: %d Underlay Candidates: %d", g_list_length (overlay_candidates), g_list_length (underlay_candidates));
  meta_topic (META_DEBUG_MOHAN, "***** -- Finished Candidate Findings -- *****");
  
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

  if (clutter_stage_view_has_shadowfb (stage_view))
    {
      meta_topic (META_DEBUG_RENDER,
                  "No direct scanout candidate: stage-view has shadowfb");
      return FALSE;
    }
  
  /* TODO Add Mutter's Post Candidate Selection cases*/

  *crtc_out = crtc;
  *onscreen_out = COGL_ONSCREEN (framebuffer);
  *overlay_candidates_out = overlay_candidates;

  return *overlay_candidates_out != NULL;
}
#endif

static void
try_assign_next_scanout (MetaCompositorView *compositor_view,
                         CoglOnscreen       *onscreen,
                         MetaWaylandSurface *surface)
{
  ClutterStageView *stage_view;
  //g_autoptr (CoglScanout) scanout = NULL;
  CoglScanout *scanout = NULL;

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

static void
try_assign_scanout_candidates (MetaCompositorView *compositor_view,
                               CoglOnscreen       *onscreen,
                               GList              *scanout_candidates)
{
  if (!scanout_candidates)
    return;

  ClutterStageView *stage_view;

  stage_view = meta_compositor_view_get_stage_view (compositor_view);

  if(!meta_onscreen_native_is_scanout_candidates_compatible (onscreen, scanout_candidates))
  {
    meta_topic (META_DEBUG_RENDER,
                "Scanout candidates are not compatible with the onscreen");
    return;
  }

  meta_topic (META_DEBUG_RENDER, "Assigning scanout to stage view");
  clutter_stage_view_assign_scanout (stage_view, scanout_candidates);
}

static void
acquire_scanout_for_candidates (MetaCompositorView *compositor_view,
                                CoglOnscreen       *onscreen,
                                GList              *overlay_candidates,
                                GList              **scanout_candidates_out)
{
  ClutterStageView *stage_view;
  
  GList* iter = NULL;

  stage_view = meta_compositor_view_get_stage_view (compositor_view);

  for(iter = overlay_candidates; iter != NULL; iter = iter->next)
    {
      MetaSurfaceActor* surface_actor = iter->data;
      MetaWaylandSurface* surface = meta_surface_actor_wayland_get_surface (META_SURFACE_ACTOR_WAYLAND (surface_actor));
      CoglScanout *scanout = NULL;

      scanout = meta_wayland_surface_try_acquire_scanout (surface,
                                                          onscreen,
                                                          stage_view);

      if (scanout)
        {
          *scanout_candidates_out = g_list_prepend (*scanout_candidates_out, scanout);
        }
    }
}

#if 0
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
#else

void RefreshPlaneAssignation (MetaCompositorView  *compositor_view)
{
  ClutterStageView *stage_view =
    meta_compositor_view_get_stage_view (compositor_view);
  MetaStageView *view = META_STAGE_VIEW (stage_view);
  MetaRendererView *renderer_view = META_RENDERER_VIEW (stage_view); 

  MetaCrtc *crtc = meta_renderer_view_get_crtc (renderer_view);

  MetaGpuKms *gpu_kms = META_GPU_KMS (meta_crtc_get_gpu (crtc));
  MetaKmsDevice *kms_device = meta_gpu_kms_get_kms_device (gpu_kms);
  GList *l;

  for (l = meta_kms_device_get_planes (kms_device); l; l = l->next)
    {
      MetaKmsPlane *plane = META_KMS_PLANE (l->data);
      meta_kms_plane_set_assigned(plane, FALSE);
    }
}

void
meta_compositor_view_native_maybe_assign_scanout (MetaCompositorViewNative *view_native,
                                                  MetaCompositor           *compositor)
{
  MetaCompositorView *compositor_view = META_COMPOSITOR_VIEW (view_native);
  MetaCrtc *crtc = NULL;
  CoglOnscreen *onscreen = NULL;
  GList* overlay_candidates = NULL;
  gboolean candidate_found;

  candidate_found = find_scanout_candidate (compositor_view,
                                            compositor,
                                            &crtc,
                                            &onscreen,
                                            &overlay_candidates);
  
  if (candidate_found)
    {
      GList* scanout_candidates = NULL;

      RefreshPlaneAssignation (compositor_view);
      acquire_scanout_for_candidates (compositor_view, onscreen, overlay_candidates, &scanout_candidates);
      try_assign_scanout_candidates (compositor_view, onscreen, scanout_candidates);
    }

  update_scanout_candidate (view_native, overlay_candidates, crtc);
}

#endif

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
