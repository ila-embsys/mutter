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

#include "backends/native/meta-scanout-manager.h"

#include "backends/meta-backend-private.h"
#include "backends/native/meta-crtc-kms.h"
#include "backends/native/meta-drm-buffer.h"
#include "backends/native/meta-frame-native.h"
#include "backends/native/meta-kms-device.h"
#include "backends/native/meta-kms-plane.h"
#include "backends/native/meta-kms-update-private.h"
#include "backends/native/meta-kms-update.h"
#include "backends/native/meta-kms.h"
#include "backends/native/meta-onscreen-native.h"
#include "backends/native/meta-renderer-native.h"
#include "backends/native/meta-scanout-assignment.h"
#include "backends/native/meta-scanout-feedback.h"
#include "backends/native/meta-scanout-strategy.h"
#include "clutter/clutter-frame-private.h"
#include "clutter/clutter-offloadable.h"
#include "compositor/compositor-private.h"
#include "compositor/meta-surface-actor-wayland.h"
#include "compositor/meta-surface-actor.h"
#include "wayland/meta-wayland-dma-buf.h"
#include "wayland/meta-wayland-surface-private.h"

typedef struct _CommitResultData
{
  MetaScanoutManager *scanout_manager;
} CommitResultData;

struct _MetaScanoutManager
{
  GObject parent;

  ClutterStageView *stage_view;
  MetaScanoutStrategy *strategy;

  ScanoutAssignment *previous_assignment;
  GArray *failed_assignments; /* PlaneAssignment */

  MetaScanoutFeedback *primary_scanout_feedback;
  MetaScanoutFeedback *overlay_scanout_feedback;
};

G_DEFINE_TYPE (MetaScanoutManager,
               meta_scanout_manager,
               G_TYPE_OBJECT)

static void
commit_result_data_free (gpointer data)
{
  CommitResultData *commit_data = data;

  g_object_unref (commit_data->scanout_manager);
  g_free (commit_data);
}

static void
free_modifiers_array (gpointer data)
{
  GArray *array = data;

  g_array_unref (array);
}

static PlaneAssignment
build_standin (MetaDrmBuffer    *standin_buffer,
               MetaCrtcKms      *crtc_kms,
               ClutterStageView *stage_view)
{
  MetaCrtc *crtc = META_CRTC (crtc_kms);
  const MetaCrtcConfig *crtc_config = meta_crtc_get_config (crtc);
  MtkMonitorTransform view_transform;
  MtkMonitorTransform hw_transform;
  MetaKmsPlane *primary_plane;
  int width, height;

  view_transform = clutter_stage_view_get_transform (stage_view);
  hw_transform =
    mtk_monitor_transform_transform (
      mtk_monitor_transform_invert (view_transform),
      crtc_config->transform);

  if (standin_buffer)
    {
      width = meta_drm_buffer_get_width (standin_buffer);
      height = meta_drm_buffer_get_height (standin_buffer);
    }
  else
    {
      CoglFramebuffer *framebuffer;

      framebuffer = clutter_stage_view_get_onscreen (stage_view);
      width = cogl_framebuffer_get_width (framebuffer);
      height = cogl_framebuffer_get_height (framebuffer);
    }

  primary_plane = meta_crtc_kms_get_assigned_primary_plane (crtc_kms);

  return (PlaneAssignment) {
    .plane = primary_plane,
    .buffer = standin_buffer ? g_object_ref (standin_buffer) : NULL,
    .src_rect = GRAPHENE_RECT_INIT (0, 0, width, height),
    .dst_rect = { 0, 0, width, height },
    .flags = META_KMS_ASSIGN_PLANE_FLAG_NONE,
    .hw_transform = hw_transform,
    .color_encoding = META_KMS_PLANE_YCBCR_COLOR_ENCODING_BT709,
    .color_range = META_KMS_PLANE_YCBCR_COLOR_RANGE_LIMITED,
    .zpos = meta_kms_plane_get_zpos (primary_plane).zpos,
  };
}

typedef struct _TryAssignmentData
{
  MetaKmsUpdate *kms_update;
  MetaCrtcKms *crtc_kms;
  MetaKmsDevice *kms_device;
  const PlaneAssignment *standin;
} TryAssignmentData;

static void
sync_assignment_to_kms_update (const PlaneAssignment *primary,
                               ScanoutAssignment     *assignment,
                               MetaKmsUpdate         *kms_update,
                               MetaCrtcKms           *crtc_kms,
                               MetaKmsDevice         *kms_device)
{
  MetaKmsCrtc *kms_crtc;
  size_t n_overlays;

  kms_crtc = meta_crtc_kms_get_kms_crtc (crtc_kms);

  /* Reset primary plane */
  meta_kms_update_drop_plane_assignment (kms_update, primary->plane);

  /* Reset overlay planes: unassign ones active on this CRTC,
   * drop stale assignments for the rest */
  for (GList *l = meta_kms_device_get_planes (kms_device); l; l = l->next)
    {
      MetaKmsPlane *plane = l->data;

      if (meta_kms_plane_get_plane_type (plane) != META_KMS_PLANE_TYPE_OVERLAY)
        continue;

      if (meta_kms_plane_get_assigned_crtc (plane) == kms_crtc)
        meta_kms_update_unassign_plane (kms_update, kms_crtc, plane);
      else
        meta_kms_update_drop_plane_assignment (kms_update, plane);
    }

  /* Apply the assignment */
  plane_assignment_add_to_update (primary, kms_update, crtc_kms);

  n_overlays = scanout_assignment_get_n_overlays (assignment);
  for (size_t i = 0; i < n_overlays; i++)
    {
      PlaneAssignment *e;

      e = scanout_assignment_get_overlay (assignment, i);
      plane_assignment_add_to_update (e, kms_update, crtc_kms);
    }
}

static gboolean
try_assignment_func (ScanoutAssignment *assignment,
                     gpointer           user_data)
{
  TryAssignmentData *data = user_data;
  PlaneAssignment *assignment_primary;
  const PlaneAssignment *effective_primary;
  g_autoptr (MetaKmsFeedback) kms_feedback = NULL;

  /* The strategy builds the assignment incrementally — overlays may be set
   * before a primary scanout is found. Fall back to the standin entry for
   * the primary plane so test commits always have a complete configuration. */
  assignment_primary = scanout_assignment_get_primary (assignment);
  effective_primary = assignment_primary->buffer
    ? assignment_primary
    : data->standin;

  sync_assignment_to_kms_update (effective_primary, assignment,
                                 data->kms_update,
                                 data->crtc_kms,
                                 data->kms_device);

  kms_feedback =
    meta_kms_device_process_update_sync (data->kms_device,
                                         meta_kms_update_ref (data->kms_update),
                                         META_KMS_UPDATE_FLAG_TEST_ONLY);

  return meta_kms_feedback_get_result (kms_feedback) == META_KMS_FEEDBACK_PASSED;
}

static void
merge_plane_formats (GHashTable   *formats_table,
                     MetaKmsPlane *plane)
{
  g_autoptr (GArray) format_list = NULL;

  format_list = meta_kms_plane_copy_drm_format_list (plane);

  for (size_t i = 0; i < format_list->len; i++)
    {
      uint32_t format;
      GArray *plane_modifiers, *existing_modifiers;

      format = g_array_index (format_list, uint32_t, i);
      plane_modifiers = meta_kms_plane_get_modifiers_for_format (plane, format);

      existing_modifiers = g_hash_table_lookup (formats_table,
                                                GUINT_TO_POINTER (format));
      if (existing_modifiers)
        {
          if (!plane_modifiers)
            continue;

          for (size_t j = 0; j < plane_modifiers->len; j++)
            {
              uint64_t modifier;
              gboolean found = FALSE;

              modifier = g_array_index (plane_modifiers, uint64_t, j);
              for (size_t k = 0; k < existing_modifiers->len; k++)
                {
                  if (g_array_index (existing_modifiers, uint64_t, k) ==
                      modifier)
                    {
                      found = TRUE;
                      break;
                    }
                }

              if (!found)
                g_array_append_val (existing_modifiers, modifier);
            }
        }
      else
        {
          GArray *new_modifiers;

          if (plane_modifiers)
            {
              new_modifiers = g_array_sized_new (FALSE, FALSE,
                                                 sizeof (uint64_t),
                                                 plane_modifiers->len);
              g_array_append_vals (new_modifiers,
                                  plane_modifiers->data,
                                  plane_modifiers->len);
            }
          else
            {
              new_modifiers = g_array_new (FALSE, FALSE, sizeof (uint64_t));
            }

          g_hash_table_insert (formats_table,
                               GUINT_TO_POINTER (format),
                               new_modifiers);
        }
    }
}

static void
ensure_scanout_feedbacks (MetaScanoutManager *scanout_manager,
                          MetaCrtcKms        *crtc_kms)
{
  MetaCrtc *crtc = META_CRTC (crtc_kms);
  MetaGpuKms *gpu_kms;
  MetaKmsDevice *kms_device;
  MetaKmsCrtc *kms_crtc;
  g_autoptr (GHashTable) primary_formats = NULL;
  g_autoptr (GHashTable) overlay_formats = NULL;

  if (scanout_manager->primary_scanout_feedback)
    return;

  gpu_kms = META_GPU_KMS (meta_crtc_get_gpu (crtc));
  kms_device = meta_gpu_kms_get_kms_device (gpu_kms);
  kms_crtc = meta_crtc_kms_get_kms_crtc (crtc_kms);

  primary_formats = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                           NULL, free_modifiers_array);
  overlay_formats = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                           NULL, free_modifiers_array);

  for (GList *l = meta_kms_device_get_planes (kms_device); l; l = l->next)
    {
      MetaKmsPlane *plane = l->data;
      MetaKmsPlaneType plane_type;

      plane_type = meta_kms_plane_get_plane_type (plane);
      if (plane_type != META_KMS_PLANE_TYPE_PRIMARY &&
          plane_type != META_KMS_PLANE_TYPE_OVERLAY)
        continue;

      if (!meta_kms_plane_is_usable_with (plane, kms_crtc))
        continue;

      if (plane_type == META_KMS_PLANE_TYPE_PRIMARY)
        merge_plane_formats (primary_formats, plane);
      else
        merge_plane_formats (overlay_formats, plane);
    }

  scanout_manager->primary_scanout_feedback = meta_scanout_feedback_new ();
  if (g_hash_table_size (primary_formats) > 0)
    {
      meta_scanout_feedback_add_format_set (
        scanout_manager->primary_scanout_feedback,
        g_hash_table_ref (primary_formats));
    }
  if (g_hash_table_size (overlay_formats) > 0)
    {
      meta_scanout_feedback_add_format_set (
        scanout_manager->primary_scanout_feedback,
        g_hash_table_ref (overlay_formats));
    }

  scanout_manager->overlay_scanout_feedback = meta_scanout_feedback_new ();
  if (g_hash_table_size (overlay_formats) > 0)
    {
      meta_scanout_feedback_add_format_set (
        scanout_manager->overlay_scanout_feedback,
        g_hash_table_ref (overlay_formats));
    }
  if (g_hash_table_size (primary_formats) > 0)
    {
      meta_scanout_feedback_add_format_set (
        scanout_manager->overlay_scanout_feedback,
        g_hash_table_ref (primary_formats));
    }
}

static MetaWaylandSurface *
get_surface_from_offload_entry (const ClutterOffloadEntry *entry)
{
  MetaSurfaceActorWayland *surface_actor_wayland;

  if (!META_IS_SURFACE_ACTOR_WAYLAND (entry->actor))
    return NULL;

  surface_actor_wayland = META_SURFACE_ACTOR_WAYLAND (entry->actor);
  return meta_surface_actor_wayland_get_surface (surface_actor_wayland);
}

static void
update_scanout_feedback (MetaScanoutManager       *scanout_manager,
                         ClutterOffloadableActors *offload_actors,
                         MetaCrtcKms              *crtc_kms)
{
  ScanoutAssignment *prev = scanout_manager->previous_assignment;
  PlaneAssignment *primary;
  size_t n_entries;

  if (!crtc_kms)
    return;

  ensure_scanout_feedbacks (scanout_manager, crtc_kms);

  primary = scanout_assignment_get_primary (prev);
  n_entries = clutter_offloadable_actors_get_n_entries (offload_actors);

  for (size_t i = 0; i < n_entries; i++)
    {
      const ClutterOffloadEntry *entry;
      MetaWaylandSurface *surface;
      MetaScanoutFeedback *feedback;

      entry = clutter_offloadable_actors_get_entry (offload_actors, i);
      if (entry->poisoned)
        continue;

      surface = get_surface_from_offload_entry (entry);
      if (!surface)
        continue;

      if (surface == primary->surface)
        feedback = scanout_manager->primary_scanout_feedback;
      else
        feedback = scanout_manager->overlay_scanout_feedback;

      meta_wayland_dma_buf_update_scanout_feedback (surface, feedback);
    }
}

static void
disable_scanout_candidate (ClutterActor *actor)
{
  clutter_actor_disable_render_hole (actor);
}

static void
update_scanout_candidate (ScanoutAssignment *assignment,
                          ScanoutAssignment *old_assignment,
                          ClutterFrame      *frame)
{
  g_return_if_fail (assignment);
  g_return_if_fail (old_assignment);
  g_return_if_fail (frame);

  ClutterOffloadableOffloaded * current_offloadable = clutter_frame_get_offloaded (frame);

  PlaneAssignment *current_primary = scanout_assignment_get_primary (assignment);
  PlaneAssignment *old_primary = scanout_assignment_get_primary (old_assignment);
  if (old_primary->surface && !current_primary->surface)
    {
      disable_scanout_candidate (CLUTTER_ACTOR(old_primary->surface_actor));
    }

  size_t n_overlays = scanout_assignment_get_n_overlays (old_assignment);
  for (size_t i = 0; i < n_overlays; i++)
    {
      PlaneAssignment *old_overlay = scanout_assignment_get_overlay (old_assignment, i);
      ClutterActor* clutter_actor = CLUTTER_ACTOR (old_overlay->surface_actor);

      if (current_offloadable)
        {
          if (!clutter_offloadable_offloaded_contains_actor (current_offloadable, 
                                                             clutter_actor))
            disable_scanout_candidate (clutter_actor);
        }
      else
        disable_scanout_candidate (clutter_actor);
    }
}

static GList *
get_available_overlay_planes (MetaKmsDevice *kms_device,
                              MetaKmsCrtc   *kms_crtc)
{
  GList *available_planes = NULL;

  for (GList *l = meta_kms_device_get_planes (kms_device); l; l = l->next)
    {
      MetaKmsPlane *plane = l->data;
      MetaKmsPlaneType plane_type;
      MetaKmsCrtc *assigned_crtc;

      plane_type = meta_kms_plane_get_plane_type (plane);
      if (plane_type != META_KMS_PLANE_TYPE_OVERLAY)
        continue;
      if (!meta_kms_plane_is_usable_with (plane, kms_crtc))
        continue;

      assigned_crtc = meta_kms_plane_get_assigned_crtc (plane);
      if (assigned_crtc && assigned_crtc != kms_crtc)
        continue;

      available_planes = g_list_prepend (available_planes, plane);
    }

  return available_planes;
}

static void
set_offloaded_actors (ScanoutAssignment          *assignment,
                      MetaBackend                *backend,
                      ClutterOffloadableOffloaded *offloaded)
{
  PlaneAssignment *primary;
  size_t n_overlays;

  primary = scanout_assignment_get_primary (assignment);

  if (primary->surface)
    {
      ClutterActor *stage = meta_backend_get_stage (backend);

      clutter_offloadable_offloaded_set_primary_actor (offloaded,
                                                       CLUTTER_ACTOR (primary->surface_actor));
      clutter_offloadable_offloaded_add_actor (offloaded,
                                               CLUTTER_ACTOR (primary->surface_actor));
      clutter_offloadable_offloaded_add_actor (offloaded, stage);
    }

  n_overlays = scanout_assignment_get_n_overlays (assignment);
  for (size_t i = 0; i < n_overlays; i++)
    {
      PlaneAssignment *e;

      e = scanout_assignment_get_overlay (assignment, i);
      clutter_offloadable_offloaded_add_actor (offloaded,
                                               CLUTTER_ACTOR (e->surface_actor));
    }
}

static void
record_commit_failure (MetaScanoutManager *scanout_manager)
{
  ScanoutAssignment *prev = scanout_manager->previous_assignment;

  if (prev)
    {
      PlaneAssignment *primary;
      size_t n_overlays;

      primary = scanout_assignment_get_primary (prev);
      if (primary->buffer)
        g_array_append_val (scanout_manager->failed_assignments, *primary);

      n_overlays = scanout_assignment_get_n_overlays (prev);
      for (size_t i = 0; i < n_overlays; i++)
        {
          PlaneAssignment *e;

          e = scanout_assignment_get_overlay (prev, i);
          g_array_append_val (scanout_manager->failed_assignments, *e);
        }
    }

  clutter_stage_view_add_redraw_clip (scanout_manager->stage_view, NULL);
  clutter_stage_view_schedule_update_now (scanout_manager->stage_view);
}

static void
commit_result_feedback (const MetaKmsFeedback *kms_feedback,
                        gpointer               user_data)
{
  CommitResultData *commit_data = user_data;
  const GError *error;

  error = meta_kms_feedback_get_error (kms_feedback);
  if (!error)
    return;

  if (g_error_matches (error,
                       META_KMS_ERROR,
                       META_KMS_ERROR_DISCARDED) ||
      g_error_matches (error,
                       G_IO_ERROR,
                       G_IO_ERROR_PERMISSION_DENIED))
    return;

  record_commit_failure (commit_data->scanout_manager);
}

static const MetaKmsResultListenerVtable commit_result_listener_vtable = {
  .feedback = commit_result_feedback,
};

void
meta_scanout_manager_assign_planes (MetaScanoutManager       *scanout_manager,
                                    ClutterOffloadableActors *offload_actors,
                                    MetaCompositor           *compositor,
                                    ClutterFrame             *frame)
{
  ClutterStageView *stage_view = scanout_manager->stage_view;
  MetaRendererView *renderer_view = META_RENDERER_VIEW (stage_view);
  g_autoptr (ClutterOffloadableOffloaded) offloaded = NULL;
  g_autoptr (ScanoutAssignment) old_assignment = NULL;
  g_autoptr (ScanoutAssignment) assignment = NULL;
  CommitResultData *commit_data;
  PlaneAssignment *assignment_primary;
  PlaneAssignment standin;
  TryAssignmentData try_data;
  CoglFramebuffer *framebuffer;
  CoglOnscreen *onscreen;
  MetaOnscreenNative *onscreen_native;
  MetaCrtc *crtc;
  MetaCrtcKms *crtc_kms;
  MetaGpuKms *gpu_kms;
  MetaKmsDevice *kms_device;
  MetaBackend *backend;
  MetaFrameNative *frame_native;
  MetaKmsUpdate *kms_update;
  MetaDrmBuffer *standin_buffer;

  offloaded = clutter_offloadable_offloaded_new ();
  assignment = scanout_assignment_new ();
  assignment_primary = scanout_assignment_get_primary (assignment);

  old_assignment =
    g_steal_pointer (&scanout_manager->previous_assignment);
  if (!old_assignment)
    old_assignment = scanout_assignment_new ();

  framebuffer = clutter_stage_view_get_onscreen (stage_view);
  if (!COGL_IS_ONSCREEN (framebuffer))
    return;

  onscreen = COGL_ONSCREEN (framebuffer);
  onscreen_native = META_ONSCREEN_NATIVE (onscreen);

  crtc = meta_renderer_view_get_crtc (renderer_view);
  if (!META_IS_CRTC_KMS (crtc))
    return;

  crtc_kms = META_CRTC_KMS (crtc);
  gpu_kms = META_GPU_KMS (meta_crtc_get_gpu (crtc));
  kms_device = meta_gpu_kms_get_kms_device (gpu_kms);
  backend = meta_gpu_get_backend (META_GPU (gpu_kms));

  standin_buffer =
    meta_onscreen_native_get_standin_buffer (onscreen_native);

  frame_native = meta_frame_native_from_frame (frame);
  kms_update = meta_frame_native_ensure_kms_update (frame_native, kms_device);

  standin = build_standin (standin_buffer, crtc_kms, stage_view);

  try_data = (TryAssignmentData) {
    .kms_update = kms_update,
    .crtc_kms = crtc_kms,
    .kms_device = kms_device,
    .standin = &standin,
  };

  if (standin.buffer &&
      !meta_compositor_is_unredirect_inhibited (compositor))
    {
      g_autoptr (GList) available_planes = NULL;
      MetaKmsCrtc *kms_crtc;

      kms_crtc = meta_crtc_kms_get_kms_crtc (crtc_kms);
      available_planes = get_available_overlay_planes (kms_device, kms_crtc);

      meta_scanout_strategy_assign_planes (scanout_manager->strategy,
                                           offload_actors,
                                           backend,
                                           stage_view,
                                           crtc_kms,
                                           available_planes,
                                           scanout_manager->failed_assignments,
                                           old_assignment,
                                           assignment,
                                           try_assignment_func,
                                           &try_data);
    }

  if (!assignment_primary->buffer)
    {
      *assignment_primary = standin;
      standin = (PlaneAssignment) { 0 };
    }

  sync_assignment_to_kms_update (assignment_primary, assignment,
                                 kms_update, crtc_kms, kms_device);

  if (assignment_primary->buffer)
    meta_frame_native_add_buffer (frame_native, assignment_primary->buffer);

  for (size_t i = 0; i < scanout_assignment_get_n_overlays (assignment); i++)
    {
      PlaneAssignment *overlay;

      overlay = scanout_assignment_get_overlay (assignment, i);
      if (overlay->buffer)
        meta_frame_native_add_buffer (frame_native, overlay->buffer);
    }

  set_offloaded_actors (assignment, backend, offloaded);

  commit_data = g_new0 (CommitResultData, 1);
  commit_data->scanout_manager = g_object_ref (scanout_manager);

  meta_kms_update_add_result_listener (kms_update,
                                       &commit_result_listener_vtable,
                                       NULL,
                                       commit_data,
                                       commit_result_data_free);

  if (!clutter_offloadable_offloaded_is_empty (offloaded))
    clutter_frame_set_offloaded (frame, offloaded);

  update_scanout_candidate (assignment, old_assignment, frame);

  scanout_manager->previous_assignment = g_steal_pointer (&assignment);

  g_array_set_size (scanout_manager->failed_assignments, 0);

  update_scanout_feedback (scanout_manager, offload_actors, crtc_kms);

  plane_assignment_clear (&standin);
}

void
meta_scanout_manager_invalidate_offloaded (MetaScanoutManager *scanout_manager)
{
  g_autoptr (ScanoutAssignment) old_assignment = NULL;

  old_assignment = g_steal_pointer (&scanout_manager->previous_assignment);

  PlaneAssignment *old_primary = scanout_assignment_get_primary (old_assignment);
  if (old_primary->surface)
    {
      disable_scanout_candidate (CLUTTER_ACTOR(old_primary->surface_actor));
    }

  size_t n_overlays = scanout_assignment_get_n_overlays (old_assignment);
  for (size_t i = 0; i < n_overlays; i++)
    {
      PlaneAssignment *old_overlay = scanout_assignment_get_overlay (old_assignment, i);
      ClutterActor* clutter_actor = CLUTTER_ACTOR (old_overlay->surface_actor);

      disable_scanout_candidate (clutter_actor);
    }
}

MetaScanoutManager *
meta_scanout_manager_new (ClutterStageView *stage_view)
{
  g_autoptr (MetaScanoutManager) scanout_manager = NULL;

  scanout_manager = g_object_new (META_TYPE_SCANOUT_MANAGER, NULL);
  scanout_manager->stage_view = stage_view;
  scanout_manager->strategy =
    META_SCANOUT_STRATEGY (meta_scanout_strategy_default_new ());

  return g_steal_pointer (&scanout_manager);
}

static void
meta_scanout_manager_finalize (GObject *object)
{
  MetaScanoutManager *scanout_manager = META_SCANOUT_MANAGER (object);

  g_clear_object (&scanout_manager->strategy);
  g_clear_pointer (&scanout_manager->previous_assignment,
                    scanout_assignment_free);
  g_clear_pointer (&scanout_manager->failed_assignments, g_array_unref);
  g_clear_pointer (&scanout_manager->primary_scanout_feedback,
                    meta_scanout_feedback_free);
  g_clear_pointer (&scanout_manager->overlay_scanout_feedback,
                    meta_scanout_feedback_free);

  G_OBJECT_CLASS (meta_scanout_manager_parent_class)->finalize (object);
}

static void
meta_scanout_manager_class_init (MetaScanoutManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = meta_scanout_manager_finalize;
}

static void
meta_scanout_manager_init (MetaScanoutManager *scanout_manager)
{
  scanout_manager->failed_assignments =
    g_array_new (FALSE, FALSE, sizeof (PlaneAssignment));
}
