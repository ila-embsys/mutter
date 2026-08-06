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

#pragma once

#include <glib-object.h>

#include "backends/native/meta-crtc-kms.h"
#include "backends/native/meta-drm-buffer.h"
#include "backends/native/meta-kms-plane.h"
#include "backends/native/meta-kms-types.h"

typedef struct _MetaSurfaceActor MetaSurfaceActor;
typedef struct _MetaWaylandSurface MetaWaylandSurface;

typedef struct _PlaneAssignment
{
  MetaKmsPlane *plane;
  MetaDrmBuffer *buffer; /* owned ref, or NULL for standin placeholder */
  graphene_rect_t src_rect;
  MtkRectangle dst_rect;
  MetaKmsAssignPlaneFlag flags;
  MtkMonitorTransform hw_transform;
  MetaKmsPlaneYCbCrColorEncoding color_encoding;
  MetaKmsPlaneYCbCrColorRange color_range;

  MetaWaylandSurface *surface;     /* (owned) (nullable) NULL for composited primary */
  MetaSurfaceActor *surface_actor; /* (unowned) (nullable) NULL for composited primary */

  struct
  {
    uint32_t format;
    uint64_t modifier;
    int width;
    int height;
  } surface_state;
} PlaneAssignment;

typedef struct _ScanoutAssignment ScanoutAssignment;

ScanoutAssignment * scanout_assignment_new (void);

void scanout_assignment_free (ScanoutAssignment *assignment);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ScanoutAssignment, scanout_assignment_free)

PlaneAssignment * scanout_assignment_get_primary (ScanoutAssignment *assignment);

void scanout_assignment_add_overlay (ScanoutAssignment *assignment,
                                     PlaneAssignment   *plane_assignment);

PlaneAssignment * scanout_assignment_get_overlay (ScanoutAssignment *assignment,
                                                  size_t             index);

size_t scanout_assignment_get_n_overlays (ScanoutAssignment *assignment);

void scanout_assignment_stage (ScanoutAssignment *assignment);

void scanout_assignment_commit (ScanoutAssignment *assignment);

void scanout_assignment_restore (ScanoutAssignment *assignment);

void plane_assignment_clear (PlaneAssignment *plane_assignment);

void plane_assignment_add_to_update (const PlaneAssignment *plane_assignment,
                                     MetaKmsUpdate         *kms_update,
                                     MetaCrtcKms           *crtc_kms);
