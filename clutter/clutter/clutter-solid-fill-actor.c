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

#include "config.h"

#include "clutter/clutter-solid-fill-actor.h"

#include "clutter/clutter-offloadable.h"

struct _ClutterSolidFillActor
{
  ClutterActor parent;
};

static ClutterOffloadType
clutter_solid_fill_actor_get_offload_type (ClutterOffloadable  *offloadable,
                                           ClutterStageView    *stage_view,
                                           MtkRegion          **out_region)
{
  ClutterActor *actor = CLUTTER_ACTOR (offloadable);
  graphene_rect_t extents;
  MtkRectangle rect;

  if (clutter_actor_get_paint_opacity (actor) != 0xff)
    return CLUTTER_OFFLOAD_TYPE_NONE;

  clutter_actor_get_transformed_extents (actor, &extents);

  rect = (MtkRectangle) {
    .x = (int) extents.origin.x,
    .y = (int) extents.origin.y,
    .width = (int) extents.size.width,
    .height = (int) extents.size.height,
  };

  *out_region = mtk_region_create_rectangle (&rect);

  return CLUTTER_OFFLOAD_TYPE_OPAQUE;
}

static void
offloadable_iface_init (ClutterOffloadableInterface *iface)
{
  iface->get_offload_type = clutter_solid_fill_actor_get_offload_type;
}

G_DEFINE_TYPE_WITH_CODE (ClutterSolidFillActor,
                         clutter_solid_fill_actor,
                         CLUTTER_TYPE_ACTOR,
                         G_IMPLEMENT_INTERFACE (CLUTTER_TYPE_OFFLOADABLE,
                                                offloadable_iface_init))

static void
clutter_solid_fill_actor_class_init (ClutterSolidFillActorClass *klass)
{
}

static void
clutter_solid_fill_actor_init (ClutterSolidFillActor *self)
{
}

ClutterActor *
clutter_solid_fill_actor_new (const CoglColor *color)
{
  g_autoptr (ClutterSolidFillActor) actor = NULL;

  actor = g_object_new (CLUTTER_TYPE_SOLID_FILL_ACTOR, NULL);
  clutter_actor_set_background_color (CLUTTER_ACTOR (actor), color);

  return CLUTTER_ACTOR (g_steal_pointer (&actor));
}
