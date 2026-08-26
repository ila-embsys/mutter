/*
 * Copyright (C) 2026 Red Hat, Inc.
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

#include "backends/native/meta-scanout-strategy.h"

#include <drm_fourcc.h>

#include "backends/meta-backend-private.h"
#include "backends/meta-cursor-renderer.h"
#include "backends/meta-renderer-view.h"
#include "backends/meta-stage-view.h"
#include "backends/native/meta-crtc-kms.h"
#include "backends/native/meta-drm-buffer-gbm.h"
#include "backends/native/meta-drm-buffer.h"
#include "backends/native/meta-kms-plane.h"
#include "backends/native/meta-scanout-assignment.h"
#include "clutter/clutter-offloadable.h"
#include "compositor/meta-surface-actor-wayland.h"
#include "compositor/meta-surface-actor.h"
#include "compositor/meta-window-actor-private.h"
#include "wayland/meta-wayland-buffer.h"
#include "wayland/meta-wayland-surface-private.h"

typedef struct _CursorOverlapData
{
  MetaBackend *backend;
  ClutterStageView *stage_view;
  gboolean has_overlap;
} CursorOverlapData;

G_DEFINE_TYPE (MetaScanoutStrategy,
               meta_scanout_strategy,
               G_TYPE_OBJECT)


static gboolean
plane_supports_format_and_modifier (MetaKmsPlane *plane,
                                    uint32_t      format,
                                    uint64_t      modifier)
{
  GArray *modifiers;

  if (!meta_kms_plane_is_format_supported (plane, format))
    return FALSE;

  if (modifier == DRM_FORMAT_MOD_INVALID)
    return TRUE;

  modifiers = meta_kms_plane_get_modifiers_for_format (plane, format);
  if (!modifiers)
    return FALSE;

  for (size_t i = 0; i < modifiers->len; i++)
    {
      if (g_array_index (modifiers, uint64_t, i) == modifier)
        return TRUE;
    }

  return FALSE;
}

static gboolean
plane_supports_buffer (MetaKmsPlane  *plane,
                       MetaDrmBuffer *buffer)
{
  uint32_t format;
  uint64_t modifier;

  format = meta_drm_buffer_get_format (buffer);
  modifier = meta_drm_buffer_get_modifier (buffer);

  return plane_supports_format_and_modifier (plane, format, modifier);
}

static gboolean
has_overlapping_cursor_overlay_foreach (ClutterStage  *stage,
                                       ClutterSprite *sprite,
                                       gpointer       user_data)
{
  CursorOverlapData *data = user_data;
  MetaCursorRenderer *cursor_renderer;
  ClutterCursor *cursor;
  MtkRectangle view_rect;
  graphene_rect_t graphene_view_rect, cursor_rect;

  cursor_renderer =
    meta_backend_get_cursor_renderer_for_sprite (data->backend, sprite);
  if (!cursor_renderer)
    return TRUE;
  if (!meta_cursor_renderer_needs_overlay_on_view (cursor_renderer,
                                                   data->stage_view))
    return TRUE;

  cursor = meta_cursor_renderer_get_cursor (cursor_renderer);
  cursor_rect = meta_cursor_renderer_calculate_rect (cursor_renderer, cursor);

  clutter_stage_view_get_layout (data->stage_view, &view_rect);
  graphene_view_rect = mtk_rectangle_to_graphene_rect (&view_rect);

  data->has_overlap |=
    graphene_rect_intersection (&graphene_view_rect, &cursor_rect, NULL);
  return !data->has_overlap;
}

static gboolean
has_overlapping_cursor_overlays (MetaBackend      *backend,
                                ClutterStageView *stage_view)
{
  ClutterStage *stage = CLUTTER_STAGE (meta_backend_get_stage (backend));
  CursorOverlapData data = { backend, stage_view, FALSE };

  clutter_stage_foreach_sprite (stage,
                                has_overlapping_cursor_overlay_foreach,
                                &data);
  return data.has_overlap;
}

static gboolean
verify_color_pipeline (ClutterStageView                *stage_view,
                       MetaWaylandSurface              *surface,
                       MetaSurfaceActor                *surface_actor,
                       MetaKmsPlaneYCbCrColorEncoding  *out_encoding,
                       MetaKmsPlaneYCbCrColorRange     *out_range)
{
  ClutterColorState *output_color_state, *surface_color_state;
  MetaMultiTextureCoefficients coeffs;

  if (meta_get_debug_paint_flags () &
      META_DEBUG_PAINT_IGNORE_COLOR_STATE_FOR_DIRECT_SCANOUT)
    {
      *out_encoding = META_KMS_PLANE_YCBCR_COLOR_ENCODING_BT709;
      *out_range = META_KMS_PLANE_YCBCR_COLOR_RANGE_LIMITED;
      return TRUE;
    }

  output_color_state =
    clutter_stage_view_get_output_color_state (stage_view);
  surface_color_state =
    clutter_actor_get_color_state (CLUTTER_ACTOR (surface_actor));

  if (clutter_color_pipeline_shader_needs_color_state (surface_color_state,
                                                       output_color_state, 0))
    {
      meta_topic (META_DEBUG_RENDER,
                  "No scanout candidate: "
                  "surface color state (%s) needs mapping to the output's (%s)",
                  clutter_color_state_to_string (surface_color_state),
                  clutter_color_state_to_string (output_color_state));
      return FALSE;
    }

  coeffs = surface->applied_state.coeffs;
  if (coeffs != META_MULTI_TEXTURE_COEFFICIENTS_NONE &&
      coeffs != META_MULTI_TEXTURE_COEFFICIENTS_IDENTITY_FULL &&
      coeffs != META_MULTI_TEXTURE_COEFFICIENTS_BT709_LIMITED)
    {
      meta_topic (META_DEBUG_RENDER,
                  "No scanout candidate: unsupported color model");
      return FALSE;
    }

  *out_encoding = META_KMS_PLANE_YCBCR_COLOR_ENCODING_BT709;
  *out_range = META_KMS_PLANE_YCBCR_COLOR_RANGE_LIMITED;
  return TRUE;
}

static gboolean
validate_surface_for_scanout (ClutterStageView    *stage_view,
                              MetaWaylandSurface  *surface,
                              MetaSurfaceActor    *surface_actor)
{
  MetaWindowActor *window_actor;

  window_actor = meta_window_actor_from_actor (CLUTTER_ACTOR (surface_actor));
  if (window_actor &&
      (meta_window_actor_effect_in_progress (window_actor) ||
       clutter_actor_has_transitions (CLUTTER_ACTOR (window_actor))))
    return FALSE;

  if (!surface->buffer || surface->buffer->use_count == 0)
    return FALSE;

  return TRUE;
}

static void
compute_scanout_rects (MetaWaylandSurface *surface,
                       MetaSurfaceActor   *surface_actor,
                       ClutterStageView   *stage_view,
                       MetaDrmBuffer      *buffer,
                       graphene_rect_t    *out_src_rect,
                       MtkRectangle       *out_dst_rect)
{
  MtkMonitorTransform view_transform;
  ClutterActorBox actor_box;
  MtkRectangle view_rect;
  float view_scale;
  int view_crtc_width, view_crtc_height;
  MtkRectangle dst_rect;

  clutter_actor_get_paint_box (CLUTTER_ACTOR (surface_actor), &actor_box);
  clutter_stage_view_get_layout (stage_view, &view_rect);
  view_scale = clutter_stage_view_get_scale (stage_view);

  dst_rect = (MtkRectangle) {
    .x = (int) roundf ((actor_box.x1 - view_rect.x) * view_scale),
    .y = (int) roundf ((actor_box.y1 - view_rect.y) * view_scale),
    .width = (int) roundf ((actor_box.x2 - actor_box.x1) * view_scale),
    .height = (int) roundf ((actor_box.y2 - actor_box.y1) * view_scale),
  };

  view_transform = clutter_stage_view_get_transform (stage_view);

  if (mtk_monitor_transform_is_rotated (view_transform))
    {
      view_crtc_width = (int) roundf (view_rect.height * view_scale);
      view_crtc_height = (int) roundf (view_rect.width * view_scale);
    }
  else
    {
      view_crtc_width = (int) roundf (view_rect.width * view_scale);
      view_crtc_height = (int) roundf (view_rect.height * view_scale);
    }

  mtk_rectangle_transform (&dst_rect,
                           view_transform,
                           view_crtc_width,
                           view_crtc_height,
                           &dst_rect);

  *out_dst_rect = dst_rect;

  if (surface->viewport.has_src_rect)
    {
      int buffer_scale = surface->applied_state.scale;
      graphene_rect_t src_rect;

      src_rect = surface->viewport.src_rect;
      src_rect.origin.x *= buffer_scale;
      src_rect.origin.y *= buffer_scale;
      src_rect.size.width *= buffer_scale;
      src_rect.size.height *= buffer_scale;

      *out_src_rect = src_rect;
    }
  else
    {
      *out_src_rect = GRAPHENE_RECT_INIT (0, 0,
                                          meta_drm_buffer_get_width (buffer),
                                          meta_drm_buffer_get_height (buffer));
    }
}

static MetaWaylandSurface *
find_primary_scanout_candidate (ClutterOffloadableActors *offload_actors,
                                MetaBackend              *backend,
                                ClutterStageView         *stage_view,
                                MetaSurfaceActor        **out_surface_actor)
{
  MetaStageView *meta_view = META_STAGE_VIEW (stage_view);
  const ClutterOffloadEntry *entry;
  MtkRectangle view_rect;
  MtkRectangle entry_extents;
  MetaSurfaceActorWayland *surface_actor_wayland;
  MetaWaylandSurface *surface;

  if (meta_get_debug_paint_flags () & META_DEBUG_PAINT_DISABLE_DIRECT_SCANOUT)
    return NULL;

  if (!meta_stage_view_is_cursor_overlay_inhibited (meta_view) &&
      has_overlapping_cursor_overlays (backend, stage_view))
    {
      meta_topic (META_DEBUG_RENDER,
                  "No primary scanout candidate: "
                  "overlapping software cursors");
      return NULL;
    }

  if (clutter_stage_view_has_shadowfb (stage_view))
    {
      meta_topic (META_DEBUG_RENDER,
                  "No primary scanout candidate: "
                  "stage-view has shadowfb");
      return NULL;
    }

  if (clutter_offloadable_actors_get_n_entries (offload_actors) == 0)
    {
      meta_topic (META_DEBUG_RENDER,
                  "No primary scanout candidate: "
                  "offload plan is empty");
      return NULL;
    }

  entry = clutter_offloadable_actors_get_entry (offload_actors, 0);

  if (entry->poisoned)
    return NULL;

  if (entry->type != CLUTTER_OFFLOAD_TYPE_OPAQUE)
    {
      meta_topic (META_DEBUG_RENDER,
                  "No primary scanout candidate: "
                  "top entry is not opaque");
      return NULL;
    }

  clutter_stage_view_get_layout (stage_view, &view_rect);
  entry_extents = mtk_region_get_extents (entry->region);
  if (entry_extents.x > view_rect.x ||
      entry_extents.y > view_rect.y ||
      entry_extents.x + entry_extents.width <
        view_rect.x + view_rect.width ||
      entry_extents.y + entry_extents.height <
        view_rect.y + view_rect.height)
    {
      meta_topic (META_DEBUG_RENDER,
                  "No primary scanout candidate: "
                  "top entry does not cover the view");
      return NULL;
    }

  if (!META_IS_SURFACE_ACTOR_WAYLAND (entry->actor))
    {
      meta_topic (META_DEBUG_RENDER,
                  "No primary scanout candidate: "
                  "top entry is not a Wayland surface actor");
      return NULL;
    }

  surface_actor_wayland = META_SURFACE_ACTOR_WAYLAND (entry->actor);
  surface = meta_surface_actor_wayland_get_surface (surface_actor_wayland);
  if (!surface)
    return NULL;

  if (!validate_surface_for_scanout (stage_view, surface,
                                     META_SURFACE_ACTOR (surface_actor_wayland)))
    return NULL;

  *out_surface_actor = META_SURFACE_ACTOR (surface_actor_wayland);
  return surface;
}

static gboolean
build_primary_scanout (ClutterOffloadableActors *offload_actors,
                       MetaBackend              *backend,
                       ClutterStageView         *stage_view,
                       MetaCrtcKms              *crtc_kms,
                       PlaneAssignment          *out)
{
  MetaCrtc *crtc = META_CRTC (crtc_kms);
  const MetaCrtcConfig *crtc_config = meta_crtc_get_config (crtc);
  MetaSurfaceActor *surface_actor = NULL;
  MetaWaylandSurface *surface;
  CoglFramebuffer *framebuffer;
  CoglOnscreen *onscreen;
  MetaKmsPlane *primary_plane;

  surface = find_primary_scanout_candidate (offload_actors,
                                            backend, stage_view,
                                            &surface_actor);
  if (!surface)
    return FALSE;

  if (!verify_color_pipeline (stage_view, surface, surface_actor,
                              &out->color_encoding, &out->color_range))
    return FALSE;

  framebuffer = clutter_stage_view_get_onscreen (stage_view);
  onscreen = COGL_ONSCREEN (framebuffer);

  out->buffer =
    meta_wayland_buffer_try_acquire_drm_buffer (surface->buffer, onscreen);
  if (!out->buffer)
    {
      meta_topic (META_DEBUG_RENDER,
                  "Could not acquire DRM buffer for primary scanout");
      return FALSE;
    }

  primary_plane = meta_crtc_kms_get_assigned_primary_plane (crtc_kms);
  if (!plane_supports_buffer (primary_plane, out->buffer))
    {
      meta_topic (META_DEBUG_RENDER,
                  "No primary scanout candidate: "
                  "buffer format/modifier not supported by primary plane");
      plane_assignment_clear (out);
      return FALSE;
    }

  out->hw_transform =
    mtk_monitor_transform_transform (
      mtk_monitor_transform_invert (surface->buffer_transform),
      crtc_config->transform);

  if (out->hw_transform != MTK_MONITOR_TRANSFORM_NORMAL &&
      !meta_kms_plane_is_transform_handled (primary_plane, out->hw_transform))
    {
      meta_topic (META_DEBUG_RENDER,
                  "No primary scanout candidate: "
                  "plane does not support required transform");
      plane_assignment_clear (out);
      return FALSE;
    }

  compute_scanout_rects (surface, surface_actor, stage_view,
                         out->buffer, &out->src_rect, &out->dst_rect);

  out->plane = primary_plane;
  out->flags = META_KMS_ASSIGN_PLANE_FLAG_DISABLE_IMPLICIT_SYNC;
  out->surface = g_object_ref (surface);
  out->surface_actor = surface_actor;
  out->surface_state.format = meta_drm_buffer_get_format (out->buffer);
  out->surface_state.modifier = meta_drm_buffer_get_modifier (out->buffer);
  out->surface_state.width = meta_drm_buffer_get_width (out->buffer);
  out->surface_state.height = meta_drm_buffer_get_height (out->buffer);

  return TRUE;
}

static gboolean
build_overlay (ClutterStageView   *stage_view,
               MetaWaylandSurface *surface,
               PlaneAssignment    *out)
{
  MetaCrtc *crtc = meta_renderer_view_get_crtc (META_RENDERER_VIEW (stage_view));
  const MetaCrtcConfig *crtc_config = meta_crtc_get_config (crtc);
  MetaSurfaceActor *surface_actor;
  CoglFramebuffer *framebuffer;
  CoglOnscreen *onscreen;

  surface_actor = meta_wayland_surface_get_actor (surface);
  if (!surface_actor)
    return FALSE;
  if (!META_IS_SURFACE_ACTOR_WAYLAND (surface_actor))
    return FALSE;

  if (!validate_surface_for_scanout (stage_view, surface, surface_actor))
    return FALSE;

  if (!verify_color_pipeline (stage_view, surface, surface_actor,
                              &out->color_encoding, &out->color_range))
    return FALSE;

  framebuffer = clutter_stage_view_get_onscreen (stage_view);
  onscreen = COGL_ONSCREEN (framebuffer);

  out->buffer =
    meta_wayland_buffer_try_acquire_drm_buffer (surface->buffer, onscreen);
  if (!out->buffer)
    return FALSE;

  compute_scanout_rects (surface, surface_actor, stage_view,
                         out->buffer, &out->src_rect, &out->dst_rect);

  out->hw_transform =
    mtk_monitor_transform_transform (
      mtk_monitor_transform_invert (surface->buffer_transform),
      crtc_config->transform);
  out->flags = META_KMS_ASSIGN_PLANE_FLAG_ALLOW_FAIL |
               META_KMS_ASSIGN_PLANE_FLAG_DISABLE_IMPLICIT_SYNC;
  out->surface = g_object_ref (surface);
  out->surface_actor = surface_actor;
  out->surface_state.format = meta_drm_buffer_get_format (out->buffer);
  out->surface_state.modifier = meta_drm_buffer_get_modifier (out->buffer);
  out->surface_state.width = meta_drm_buffer_get_width (out->buffer);
  out->surface_state.height = meta_drm_buffer_get_height (out->buffer);

  return TRUE;
}

static gboolean
scanout_assignment_has_overlay_plane (ScanoutAssignment *assignment,
                                      MetaKmsPlane      *plane)
{
  size_t n_overlays;

  n_overlays = scanout_assignment_get_n_overlays (assignment);
  for (size_t i = 0; i < n_overlays; i++)
    {
      PlaneAssignment *e;

      e = scanout_assignment_get_overlay (assignment, i);
      if (e->plane == plane)
        return TRUE;
    }

  return FALSE;
}

static gboolean
scanout_assignment_has_overlay_actor (ScanoutAssignment *assignment,
                                      ClutterActor      *actor)
{
  size_t n_overlays;

  n_overlays = scanout_assignment_get_n_overlays (assignment);
  for (size_t i = 0; i < n_overlays; i++)
    {
      PlaneAssignment *e;

      e = scanout_assignment_get_overlay (assignment, i);
      if (CLUTTER_ACTOR (e->surface_actor) == actor)
        return TRUE;
    }

  return FALSE;
}

static gboolean
is_assignment_failed (GArray        *failed_assignments,
                      MetaKmsPlane  *plane,
                      MetaDrmBuffer *buffer)
{
  uint32_t format;
  uint64_t modifier;
  int width, height;

  if (failed_assignments->len == 0)
    return FALSE;

  format = meta_drm_buffer_get_format (buffer);
  modifier = meta_drm_buffer_get_modifier (buffer);
  width = meta_drm_buffer_get_width (buffer);
  height = meta_drm_buffer_get_height (buffer);

  for (size_t i = 0; i < failed_assignments->len; i++)
    {
      PlaneAssignment *failed;

      failed = &g_array_index (failed_assignments, PlaneAssignment, i);
      if (failed->plane == plane &&
          failed->surface_state.format == format &&
          failed->surface_state.modifier == modifier &&
          failed->surface_state.width == width &&
          failed->surface_state.height == height)
        return TRUE;
    }

  return FALSE;
}

static MetaKmsPlane *
find_overlay_plane (ScanoutAssignment      *assignment,
                    GList                  *available_planes,
                    const PlaneAssignment  *overlay)
{
  for (GList *l = available_planes; l; l = l->next)
    {
      MetaKmsPlane *plane = l->data;

      if (scanout_assignment_has_overlay_plane (assignment, plane))
        continue;
      if (!plane_supports_buffer (plane, overlay->buffer))
        continue;
      if (overlay->hw_transform != MTK_MONITOR_TRANSFORM_NORMAL &&
          !meta_kms_plane_is_transform_handled (plane, overlay->hw_transform))
        continue;

      return plane;
    }

  return NULL;
}

static gboolean
is_surface_in_offload_plan (ClutterOffloadableActors *offload_actors,
                            MetaSurfaceActor         *surface_actor)
{
  size_t n_entries;

  n_entries = clutter_offloadable_actors_get_n_entries (offload_actors);

  for (size_t i = 0; i < n_entries; i++)
    {
      const ClutterOffloadEntry *offload_entry;

      offload_entry =
        clutter_offloadable_actors_get_entry (offload_actors, i);
      if (offload_entry->actor != CLUTTER_ACTOR (surface_actor))
        continue;

      return !offload_entry->poisoned &&
             offload_entry->type == CLUTTER_OFFLOAD_TYPE_OPAQUE;
    }

  return FALSE;
}

static void
meta_scanout_strategy_class_init (MetaScanoutStrategyClass *klass)
{
}

static void
meta_scanout_strategy_init (MetaScanoutStrategy *strategy)
{
}

void
meta_scanout_strategy_assign_planes (MetaScanoutStrategy        *strategy,
                                     ClutterOffloadableActors   *offload_actors,
                                     MetaBackend                *backend,
                                     ClutterStageView           *stage_view,
                                     MetaCrtcKms                *crtc_kms,
                                     GList                      *available_planes,
                                     GArray                     *failed_assignments,
                                     ScanoutAssignment          *old_assignment,
                                     ScanoutAssignment          *assignment,
                                     ScanoutTryAssignmentFunc    try_func,
                                     gpointer                    try_func_data)
{
  MetaScanoutStrategyClass *klass;

  klass = META_SCANOUT_STRATEGY_GET_CLASS (strategy);
  g_return_if_fail (klass->assign_planes != NULL);
  klass->assign_planes (strategy,
                        offload_actors,
                        backend,
                        stage_view,
                        crtc_kms,
                        available_planes,
                        failed_assignments,
                        old_assignment,
                        assignment,
                        try_func,
                        try_func_data);
}

#define MIN_OVERLAY_FREQUENCY_HZ 10.0f

struct _MetaScanoutStrategyDefault
{
  MetaScanoutStrategy parent;
};

G_DEFINE_TYPE (MetaScanoutStrategyDefault,
               meta_scanout_strategy_default,
               META_TYPE_SCANOUT_STRATEGY)

static gboolean
try_primary_scanout (ClutterOffloadableActors *offload_actors,
                     MetaBackend              *backend,
                     ClutterStageView         *stage_view,
                     MetaCrtcKms              *crtc_kms,
                     GArray                   *failed_assignments,
                     ScanoutAssignment        *assignment,
                     ScanoutTryAssignmentFunc  try_func,
                     gpointer                  try_func_data)
{
  PlaneAssignment scanout = { 0 };
  PlaneAssignment *primary;

  if (!build_primary_scanout (offload_actors, backend, stage_view,
                               crtc_kms, &scanout))
    return FALSE;

  if (is_assignment_failed (failed_assignments,
                            scanout.plane, scanout.buffer))
    {
      plane_assignment_clear (&scanout);
      return FALSE;
    }

  scanout_assignment_stage (assignment);
  primary = scanout_assignment_get_primary (assignment);
  *primary = scanout;

  if (!try_func (assignment, try_func_data))
    {
      meta_topic (META_DEBUG_RENDER,
                  "Primary scanout test commit failed");
      scanout_assignment_restore (assignment);
      return FALSE;
    }

  scanout_assignment_commit (assignment);
  meta_topic (META_DEBUG_RENDER, "Assigned primary scanout");
  return TRUE;
}

static gboolean
try_reassign_previous_overlays (ClutterOffloadableActors *offload_actors,
                                ScanoutAssignment        *old_assignment,
                                GList                    *available_planes,
                                GArray                   *failed_assignments,
                                ClutterStageView         *stage_view,
                                ScanoutAssignment        *assignment,
                                ScanoutTryAssignmentFunc  try_func,
                                gpointer                  try_func_data)
{
  g_autofree PlaneAssignment *candidates = NULL;
  size_t n_old_overlays, n_candidates = 0;

  n_old_overlays = scanout_assignment_get_n_overlays (old_assignment);
  if (n_old_overlays == 0)
    return FALSE;

  candidates = g_new0 (PlaneAssignment, n_old_overlays);

  for (size_t i = 0; i < n_old_overlays; i++)
    {
      PlaneAssignment *old;
      PlaneAssignment *candidate;
      MetaSurfaceActor *surface_actor;

      old = scanout_assignment_get_overlay (old_assignment, i);
      candidate = &candidates[n_candidates];

      surface_actor = meta_wayland_surface_get_actor (old->surface);
      if (!surface_actor)
        continue;
      if (!is_surface_in_offload_plan (offload_actors, surface_actor))
        continue;

      if (!g_list_find (available_planes, old->plane))
        continue;

      if (!build_overlay (stage_view, old->surface, candidate))
        continue;

      if (!plane_supports_buffer (old->plane, candidate->buffer))
        {
          plane_assignment_clear (candidate);
          continue;
        }

      if (candidate->hw_transform != MTK_MONITOR_TRANSFORM_NORMAL &&
          !meta_kms_plane_is_transform_handled (old->plane,
                                                candidate->hw_transform))
        {
          plane_assignment_clear (candidate);
          continue;
        }

      if (is_assignment_failed (failed_assignments,
                                old->plane, candidate->buffer))
        {
          plane_assignment_clear (candidate);
          continue;
        }

      candidate->plane = old->plane;
      candidate->zpos  = old->zpos;
      n_candidates++;
    }

  if (n_candidates == 0)
    return FALSE;

  scanout_assignment_stage (assignment);
  for (size_t i = 0; i < n_candidates; i++)
    scanout_assignment_add_overlay (assignment, &candidates[i]);

  if (!try_func (assignment, try_func_data))
    {
      scanout_assignment_restore (assignment);
      return FALSE;
    }

  scanout_assignment_commit (assignment);
  meta_topic (META_DEBUG_RENDER,
              "Fast path: reassigned %zu previous overlay planes",
              n_candidates);

  return TRUE;
}

static gboolean
try_assign_overlay (const ClutterOffloadEntry *offload_entry,
                    GList                     *available_planes,
                    GArray                    *failed_assignments,
                    ClutterStageView          *stage_view,
                    ScanoutAssignment         *assignment,
                    ScanoutTryAssignmentFunc   try_func,
                    gpointer                   try_func_data)
{
  MetaSurfaceActorWayland *surface_actor_wayland;
  MetaWaylandSurface *surface;
  MetaKmsPlane *overlay_plane;
  PlaneAssignment overlay = { 0 };

  if (!META_IS_SURFACE_ACTOR_WAYLAND (offload_entry->actor))
    return FALSE;

  surface_actor_wayland = META_SURFACE_ACTOR_WAYLAND (offload_entry->actor);
  surface = meta_surface_actor_wayland_get_surface (surface_actor_wayland);
  if (!surface)
    return FALSE;

  if (!build_overlay (stage_view, surface, &overlay))
    return FALSE;

  overlay_plane = find_overlay_plane (assignment,
                                      available_planes, &overlay);
  if (!overlay_plane)
    {
      plane_assignment_clear (&overlay);
      return FALSE;
    }

  if (is_assignment_failed (failed_assignments,
                            overlay_plane, overlay.buffer))
    {
      plane_assignment_clear (&overlay);
      return FALSE;
    }

  overlay.plane = overlay_plane;
  overlay.zpos = meta_kms_plane_get_zpos (overlay_plane).zpos;
  scanout_assignment_stage (assignment);
  scanout_assignment_add_overlay (assignment, &overlay);

  if (!try_func (assignment, try_func_data))
    {
      scanout_assignment_restore (assignment);
      return FALSE;
    }

  scanout_assignment_commit (assignment);
  meta_topic (META_DEBUG_RENDER, "Assigned overlay plane");

  return TRUE;
}

static void
try_assign_overlays (ClutterOffloadableActors *offload_actors,
                     ScanoutAssignment        *old_assignment,
                     GList                    *available_planes,
                     GArray                   *failed_assignments,
                     ClutterStageView         *stage_view,
                     ScanoutAssignment        *assignment,
                     ScanoutTryAssignmentFunc  try_func,
                     gpointer                  try_func_data)
{
  PlaneAssignment *primary;
  size_t n_entries;

  try_reassign_previous_overlays (offload_actors, old_assignment,
                                  available_planes, failed_assignments,
                                  stage_view,
                                  assignment, try_func, try_func_data);

  primary = scanout_assignment_get_primary (assignment);
  n_entries = clutter_offloadable_actors_get_n_entries (offload_actors);

  for (size_t i = 0; i < n_entries; i++)
    {
      const ClutterOffloadEntry *offload_entry;
      MetaSurfaceActor *surface_actor;
      float frequency;

      offload_entry =
        clutter_offloadable_actors_get_entry (offload_actors, i);
      if (offload_entry->poisoned)
        continue;
      if (offload_entry->type != CLUTTER_OFFLOAD_TYPE_OPAQUE)
        continue;
      if (primary->surface_actor &&
          offload_entry->actor ==
            CLUTTER_ACTOR (primary->surface_actor))
        continue;
      if (scanout_assignment_has_overlay_actor (assignment,
                                                offload_entry->actor))
        continue;
      if (!META_IS_SURFACE_ACTOR (offload_entry->actor))
        continue;

      surface_actor = META_SURFACE_ACTOR (offload_entry->actor);
      frequency = meta_surface_actor_get_update_frequency (surface_actor);

      if (frequency < MIN_OVERLAY_FREQUENCY_HZ)
        {
          meta_topic (META_DEBUG_RENDER,
                      "Overlay pass: skipping entry %zu (%.1f Hz, too low)",
                      i, (double) frequency);
          continue;
        }

      meta_topic (META_DEBUG_RENDER,
                  "Overlay pass: trying entry %zu (%.1f Hz)",
                  i, (double) frequency);
      try_assign_overlay (offload_entry, available_planes,
                          failed_assignments, stage_view,
                          assignment, try_func, try_func_data);
    }
}

static void
meta_scanout_strategy_default_assign_planes (MetaScanoutStrategy        *strategy,
                                             ClutterOffloadableActors   *offload_actors,
                                             MetaBackend                *backend,
                                             ClutterStageView           *stage_view,
                                             MetaCrtcKms                *crtc_kms,
                                             GList                      *available_planes,
                                             GArray                     *failed_assignments,
                                             ScanoutAssignment          *old_assignment,
                                             ScanoutAssignment          *assignment,
                                             ScanoutTryAssignmentFunc    try_func,
                                             gpointer                    try_func_data)
{
  try_primary_scanout (offload_actors, backend, stage_view, crtc_kms,
                       failed_assignments,
                       assignment, try_func, try_func_data);

  try_assign_overlays (offload_actors, old_assignment,
                       available_planes, failed_assignments,
                       stage_view,
                       assignment, try_func, try_func_data);
}

static void
meta_scanout_strategy_default_class_init (MetaScanoutStrategyDefaultClass *klass)
{
  MetaScanoutStrategyClass *strategy_class =
    META_SCANOUT_STRATEGY_CLASS (klass);

  strategy_class->assign_planes =
    meta_scanout_strategy_default_assign_planes;
}

static void
meta_scanout_strategy_default_init (MetaScanoutStrategyDefault *strategy)
{
}

MetaScanoutStrategyDefault *
meta_scanout_strategy_default_new (void)
{
  return g_object_new (META_TYPE_SCANOUT_STRATEGY_DEFAULT, NULL);
}
