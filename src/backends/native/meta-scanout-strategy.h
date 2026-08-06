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

#include "backends/native/meta-scanout-assignment.h"
#include "clutter/clutter-offloadable.h"

typedef struct _MetaBackend MetaBackend;

typedef gboolean (*ScanoutTryAssignmentFunc) (ScanoutAssignment *assignment,
                                              gpointer           user_data);

#define META_TYPE_SCANOUT_STRATEGY (meta_scanout_strategy_get_type ())
G_DECLARE_DERIVABLE_TYPE (MetaScanoutStrategy,
                          meta_scanout_strategy,
                          META, SCANOUT_STRATEGY,
                          GObject)

struct _MetaScanoutStrategyClass
{
  GObjectClass parent_class;

  void (* assign_planes) (MetaScanoutStrategy        *strategy,
                          ClutterOffloadableActors   *offload_actors,
                          MetaBackend                *backend,
                          ClutterStageView           *stage_view,
                          MetaCrtcKms                *crtc_kms,
                          GList                      *available_planes,
                          GArray                     *failed_assignments,
                          ScanoutAssignment          *old_assignment,
                          ScanoutAssignment          *assignment,
                          ScanoutTryAssignmentFunc    try_func,
                          gpointer                    try_func_data);
};

void meta_scanout_strategy_assign_planes (MetaScanoutStrategy        *strategy,
                                          ClutterOffloadableActors   *offload_actors,
                                          MetaBackend                *backend,
                                          ClutterStageView           *stage_view,
                                          MetaCrtcKms                *crtc_kms,
                                          GList                      *available_planes,
                                          GArray                     *failed_assignments,
                                          ScanoutAssignment          *old_assignment,
                                          ScanoutAssignment          *assignment,
                                          ScanoutTryAssignmentFunc    try_func,
                                          gpointer                    try_func_data);

#define META_TYPE_SCANOUT_STRATEGY_DEFAULT (meta_scanout_strategy_default_get_type ())
G_DECLARE_FINAL_TYPE (MetaScanoutStrategyDefault,
                      meta_scanout_strategy_default,
                      META, SCANOUT_STRATEGY_DEFAULT,
                      MetaScanoutStrategy)

MetaScanoutStrategyDefault * meta_scanout_strategy_default_new (void);
