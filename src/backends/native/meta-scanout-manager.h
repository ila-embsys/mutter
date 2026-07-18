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

#include "clutter/clutter-mutter.h"
#include "clutter/clutter-offloadable.h"
#include "meta/compositor.h"

#define META_TYPE_SCANOUT_MANAGER (meta_scanout_manager_get_type ())
G_DECLARE_FINAL_TYPE (MetaScanoutManager,
                      meta_scanout_manager,
                      META, SCANOUT_MANAGER,
                      GObject)

MetaScanoutManager * meta_scanout_manager_new (ClutterStageView *stage_view);

void meta_scanout_manager_assign_planes (MetaScanoutManager       *scanout_manager,
                                         ClutterOffloadableActors *offload_actors,
                                         MetaCompositor           *compositor,
                                         ClutterFrame             *frame);

void meta_scanout_manager_invalidate_offloaded (MetaScanoutManager    *scanout_manager);
