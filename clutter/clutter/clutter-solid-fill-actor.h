/*
 * Clutter.
 *
 * An OpenGL based 'interactive canvas' library.
 *
 * Copyright (C) 2026  Red Hat Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "clutter/clutter.h"

#define CLUTTER_TYPE_SOLID_FILL_ACTOR (clutter_solid_fill_actor_get_type ())
CLUTTER_EXPORT
G_DECLARE_FINAL_TYPE (ClutterSolidFillActor,
                      clutter_solid_fill_actor,
                      CLUTTER, SOLID_FILL_ACTOR,
                      ClutterActor)

CLUTTER_EXPORT
ClutterActor * clutter_solid_fill_actor_new (const CoglColor *color);
