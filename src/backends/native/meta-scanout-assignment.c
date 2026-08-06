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

#include "backends/native/meta-scanout-assignment.h"

#include "backends/native/meta-kms-update.h"

struct _ScanoutAssignment
{
  PlaneAssignment primary;
  GArray *overlays; /* PlaneAssignment */

  PlaneAssignment staged_primary;
  size_t staged_n_overlays;
  gboolean is_staged;
};

void
plane_assignment_clear (PlaneAssignment *plane_assignment)
{
  g_clear_object (&plane_assignment->surface);
  plane_assignment->surface_actor = NULL;
  g_clear_object (&plane_assignment->buffer);
}

static void
plane_assignment_clear_func (gpointer data)
{
  plane_assignment_clear (data);
}

ScanoutAssignment *
scanout_assignment_new (void)
{
  g_autoptr (ScanoutAssignment) assignment = NULL;

  assignment = g_new0 (ScanoutAssignment, 1);
  assignment->overlays =
    g_array_new (/* zero_terminated = */ FALSE,
                 /* clear_ = */ FALSE,
                 sizeof (PlaneAssignment));
  g_array_set_clear_func (assignment->overlays,
                          plane_assignment_clear_func);

  return g_steal_pointer (&assignment);
}

void
scanout_assignment_free (ScanoutAssignment *assignment)
{
  if (assignment->is_staged)
    plane_assignment_clear (&assignment->staged_primary);
  plane_assignment_clear (&assignment->primary);
  g_clear_pointer (&assignment->overlays, g_array_unref);
  g_free (assignment);
}

PlaneAssignment *
scanout_assignment_get_primary (ScanoutAssignment *assignment)
{
  return &assignment->primary;
}

void
scanout_assignment_add_overlay (ScanoutAssignment *assignment,
                                PlaneAssignment   *plane_assignment)
{
  g_array_append_val (assignment->overlays, *plane_assignment);
  *plane_assignment = (PlaneAssignment) { 0 };
}

PlaneAssignment *
scanout_assignment_get_overlay (ScanoutAssignment *assignment,
                                      size_t             index)
{
  return &g_array_index (assignment->overlays, PlaneAssignment, index);
}

size_t
scanout_assignment_get_n_overlays (ScanoutAssignment *assignment)
{
  return assignment->overlays->len;
}

void
scanout_assignment_stage (ScanoutAssignment *assignment)
{
  g_assert (!assignment->is_staged);

  assignment->staged_primary = assignment->primary;
  if (assignment->staged_primary.surface)
    g_object_ref (assignment->staged_primary.surface);
  if (assignment->staged_primary.buffer)
    g_object_ref (assignment->staged_primary.buffer);
  assignment->staged_n_overlays = assignment->overlays->len;
  assignment->is_staged = TRUE;
}

void
scanout_assignment_commit (ScanoutAssignment *assignment)
{
  g_assert (assignment->is_staged);

  plane_assignment_clear (&assignment->staged_primary);
  assignment->is_staged = FALSE;
}

void
scanout_assignment_restore (ScanoutAssignment *assignment)
{
  g_assert (assignment->is_staged);

  plane_assignment_clear (&assignment->primary);
  assignment->primary = assignment->staged_primary;
  assignment->staged_primary = (PlaneAssignment) { 0 };

  g_array_set_size (assignment->overlays,
                    assignment->staged_n_overlays);
  assignment->is_staged = FALSE;
}

void
plane_assignment_add_to_update (const PlaneAssignment *self,
                               MetaKmsUpdate         *kms_update,
                               MetaCrtcKms           *crtc_kms)
{
  MetaKmsCrtc *kms_crtc;
  MetaKmsPlaneAssignment *kms_plane_assignment;
  MetaFixed16Rectangle src_rect_fixed16;

  kms_crtc = meta_crtc_kms_get_kms_crtc (crtc_kms);

  src_rect_fixed16 = (MetaFixed16Rectangle) {
    .x = meta_fixed_16_from_double (self->src_rect.origin.x),
    .y = meta_fixed_16_from_double (self->src_rect.origin.y),
    .width = meta_fixed_16_from_double (self->src_rect.size.width),
    .height = meta_fixed_16_from_double (self->src_rect.size.height),
  };

  kms_plane_assignment =
    meta_kms_update_assign_plane (kms_update,
                                  kms_crtc,
                                  self->plane,
                                  self->buffer,
                                  src_rect_fixed16,
                                  self->dst_rect,
                                  self->flags);

  if (meta_kms_plane_is_transform_handled (self->plane, self->hw_transform))
    {
      meta_kms_plane_update_set_rotation (self->plane,
                                          kms_plane_assignment,
                                          self->hw_transform);
    }

  if (meta_kms_plane_is_color_encoding_handled (self->plane,
                                                self->color_encoding))
    {
      meta_kms_plane_update_set_color_encoding (self->plane,
                                                kms_plane_assignment,
                                                self->color_encoding);
    }

  if (meta_kms_plane_is_color_range_handled (self->plane,
                                             self->color_range))
    {
      meta_kms_plane_update_set_color_range (self->plane,
                                             kms_plane_assignment,
                                             self->color_range);
    }
}
