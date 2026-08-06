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

#include "clutter/clutter-offloadable.h"
#include "clutter/clutter-stage-view.h"

struct _ClutterOffloadableActors
{
  GObject parent;

  GArray *entries; /* ClutterOffloadEntry */
};

G_DEFINE_TYPE (ClutterOffloadableActors,
               clutter_offloadable_actors,
               G_TYPE_OBJECT)

static void
clutter_offload_entry_clear (ClutterOffloadEntry *entry)
{
  g_clear_pointer (&entry->region, mtk_region_unref);
}

static void
clutter_offloadable_actors_finalize (GObject *object)
{
  ClutterOffloadableActors *actors = CLUTTER_OFFLOADABLE_ACTORS (object);

  g_clear_pointer (&actors->entries, g_array_unref);

  G_OBJECT_CLASS (clutter_offloadable_actors_parent_class)->finalize (object);
}

static void
clutter_offloadable_actors_class_init (ClutterOffloadableActorsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = clutter_offloadable_actors_finalize;
}

static void
clutter_offloadable_actors_init (ClutterOffloadableActors *actors)
{
  actors->entries = g_array_new (/* zero_terminated = */ FALSE,
                                 /* clear = */ FALSE,
                                 sizeof (ClutterOffloadEntry));
  g_array_set_clear_func (actors->entries,
                          (GDestroyNotify) clutter_offload_entry_clear);
}

ClutterOffloadableActors *
clutter_offloadable_actors_new (void)
{
  return g_object_new (CLUTTER_TYPE_OFFLOADABLE_ACTORS, NULL);
}

void
clutter_offloadable_actors_add_entry (ClutterOffloadableActors *actors,
                                      ClutterActor             *actor,
                                      ClutterOffloadType        type,
                                      MtkRegion                *region,
                                      gboolean                  poisoned)
{
  ClutterOffloadEntry entry;

  entry.actor = actor;
  entry.type = type;
  entry.region = mtk_region_ref (region);
  entry.poisoned = poisoned;

  g_array_append_val (actors->entries, entry);
}

size_t
clutter_offloadable_actors_get_n_entries (ClutterOffloadableActors *actors)
{
  return actors->entries->len;
}

const ClutterOffloadEntry *
clutter_offloadable_actors_get_entry (ClutterOffloadableActors *actors,
                                      size_t                    index)
{
  g_return_val_if_fail (index < actors->entries->len, NULL);

  return &g_array_index (actors->entries, ClutterOffloadEntry, index);
}

struct _ClutterOffloadableOffloaded
{
  GObject parent;

  GHashTable *actors; /* ClutterActor * -> NULL */
  ClutterActor *primary_actor;
};

G_DEFINE_TYPE (ClutterOffloadableOffloaded,
               clutter_offloadable_offloaded,
               G_TYPE_OBJECT)

static void
clutter_offloadable_offloaded_finalize (GObject *object)
{
  ClutterOffloadableOffloaded *offloaded =
    CLUTTER_OFFLOADABLE_OFFLOADED (object);

  g_clear_pointer (&offloaded->actors, g_hash_table_unref);

  G_OBJECT_CLASS (clutter_offloadable_offloaded_parent_class)->finalize (object);
}

static void
clutter_offloadable_offloaded_class_init (ClutterOffloadableOffloadedClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = clutter_offloadable_offloaded_finalize;
}

static void
clutter_offloadable_offloaded_init (ClutterOffloadableOffloaded *offloaded)
{
  offloaded->actors = g_hash_table_new (g_direct_hash, g_direct_equal);
}

ClutterOffloadableOffloaded *
clutter_offloadable_offloaded_new (void)
{
  return g_object_new (CLUTTER_TYPE_OFFLOADABLE_OFFLOADED, NULL);
}

void
clutter_offloadable_offloaded_add_actor (ClutterOffloadableOffloaded *offloaded,
                                         ClutterActor                *actor)
{
  g_hash_table_add (offloaded->actors, actor);
}

gboolean
clutter_offloadable_offloaded_is_empty (ClutterOffloadableOffloaded *offloaded)
{
  return g_hash_table_size (offloaded->actors) == 0;
}

gboolean
clutter_offloadable_offloaded_contains_actor (ClutterOffloadableOffloaded *offloaded,
                                              ClutterActor                *actor)
{
  return g_hash_table_contains (offloaded->actors, actor);
}

void
clutter_offloadable_offloaded_set_primary_actor (ClutterOffloadableOffloaded *offloaded,
                                                  ClutterActor                *actor)
{
  offloaded->primary_actor = actor;
}

ClutterActor *
clutter_offloadable_offloaded_get_primary_actor (ClutterOffloadableOffloaded *offloaded)
{
  return offloaded->primary_actor;
}

G_DEFINE_INTERFACE (ClutterOffloadable,
                    clutter_offloadable,
                    CLUTTER_TYPE_ACTOR)

static void
clutter_offloadable_default_init (ClutterOffloadableInterface *iface)
{
}

ClutterOffloadType
clutter_offloadable_get_offload_type (ClutterOffloadable  *offloadable,
                                      ClutterStageView    *stage_view,
                                      MtkRegion          **out_region)
{
  ClutterOffloadableInterface *iface;

  iface = CLUTTER_OFFLOADABLE_GET_IFACE (offloadable);

  return iface->get_offload_type (offloadable, stage_view, out_region);
}

static gboolean
has_active_effects (ClutterActor *actor)
{
  g_autoptr (GList) effects = NULL;
  GList *l;

  effects = clutter_actor_get_effects (actor);
  for (l = effects; l != NULL; l = l->next)
    {
      if (clutter_actor_meta_get_enabled (CLUTTER_ACTOR_META (l->data)))
        return TRUE;
    }

  return FALSE;
}

static gboolean
actor_overlaps_view (ClutterActor     *actor,
                     ClutterStageView *stage_view)
{
  graphene_rect_t extents;
  MtkRectangle view_rect, actor_rect;

  if (!stage_view)
    return TRUE;

  clutter_actor_get_transformed_extents (actor, &extents);

  actor_rect = (MtkRectangle) {
    .x = (int) extents.origin.x,
    .y = (int) extents.origin.y,
    .width = (int) extents.size.width,
    .height = (int) extents.size.height,
  };

  clutter_stage_view_get_layout (stage_view, &view_rect);

  return mtk_rectangle_overlap (&actor_rect, &view_rect);
}

static void
get_actor_opaque_region (ClutterActor  *actor,
                         MtkRegion    **out_region)
{
  gboolean bg_color_set = FALSE;
  CoglColor bg_color;
  uint8_t opacity;
  graphene_rect_t extents;
  MtkRectangle rect;

  g_object_get (actor, "background-color-set", &bg_color_set, NULL);
  if (!bg_color_set)
    {
      *out_region = NULL;
      return;
    }

  clutter_actor_get_background_color (actor, &bg_color);
  opacity = clutter_actor_get_paint_opacity (actor);

  if (cogl_color_get_alpha (&bg_color) < 1.0f || opacity != 0xff)
    {
      *out_region = NULL;
      return;
    }

  clutter_actor_get_transformed_extents (actor, &extents);

  rect = (MtkRectangle) {
    .x = (int) extents.origin.x,
    .y = (int) extents.origin.y,
    .width = (int) extents.size.width,
    .height = (int) extents.size.height,
  };

  *out_region = mtk_region_create_rectangle (&rect);
}

static void
analyze_offloadable_actor (ClutterActor             *actor,
                           ClutterStageView         *stage_view,
                           ClutterOffloadableActors *offloadable_actors,
                           MtkRegion                *poisoned_region)
{
  g_autoptr (MtkRegion) region = NULL;
  ClutterOffloadType type;
  gboolean is_poisoned = FALSE;

  type = clutter_offloadable_get_offload_type (CLUTTER_OFFLOADABLE (actor),
                                               stage_view,
                                               &region);

  if (type == CLUTTER_OFFLOAD_TYPE_NONE || !region)
    return;

  if (poisoned_region && !mtk_region_is_empty (poisoned_region))
    {
      g_autoptr (MtkRegion) overlap = NULL;

      overlap = mtk_region_copy (region);
      mtk_region_intersect (overlap, poisoned_region);

      if (!mtk_region_is_empty (overlap))
        {
          is_poisoned = TRUE;

          if (type == CLUTTER_OFFLOAD_TYPE_ALPHA)
            mtk_region_union (poisoned_region, region);
        }
    }

  clutter_offloadable_actors_add_entry (offloadable_actors, actor, type,
                                        region, is_poisoned);

  if (type == CLUTTER_OFFLOAD_TYPE_OPAQUE && poisoned_region)
    mtk_region_subtract (poisoned_region, region);
}

static void
analyze_non_offloadable_actor (ClutterActor *actor,
                               MtkRegion    *poisoned_region)
{
  g_autoptr (MtkRegion) opaque_region = NULL;

  if (!poisoned_region)
    return;

  get_actor_opaque_region (actor, &opaque_region);

  if (opaque_region)
    {
      mtk_region_subtract (poisoned_region, opaque_region);
      return;
    }

  {
    graphene_rect_t extents;
    MtkRectangle rect;
    g_autoptr (MtkRegion) alpha_region = NULL;

    clutter_actor_get_transformed_extents (actor, &extents);

    rect = (MtkRectangle) {
      .x = (int) extents.origin.x,
      .y = (int) extents.origin.y,
      .width = (int) extents.size.width,
      .height = (int) extents.size.height,
    };

    alpha_region = mtk_region_create_rectangle (&rect);
    mtk_region_union (poisoned_region, alpha_region);
  }
}

/*
 * Walk children front-to-back (last-to-first in actor order) to discover
 * which offloadable actors can be removed from the composited scene.
 *
 * poisoned_region tracks areas where non-offloadable alpha content sits
 * above — any offloadable actor overlapping this region is marked poisoned
 * because its blending with the non-offloadable content must happen during
 * compositing. Opaque regions (offloadable or not) subtract from the
 * poisoned region since they hide everything below.
 */
void
clutter_offloadable_analyze_children (ClutterActor             *actor,
                                      ClutterStageView         *stage_view,
                                      ClutterOffloadableActors *offloadable_actors,
                                      MtkRegion                *poisoned_region)
{
  ClutterActor *child;
  ClutterActorIter iter;

  clutter_actor_iter_init (&iter, actor);
  while (clutter_actor_iter_prev (&iter, &child))
    {
      if (!clutter_actor_is_visible (child))
        continue;
      if (has_active_effects (child))
        continue;

      if (CLUTTER_IS_OFFLOADABLE (child))
        {
          if (!actor_overlaps_view (child, stage_view))
            continue;

          if (poisoned_region)
            {
              graphene_matrix_t child_transform, inverted;

              clutter_actor_get_transform (child, &child_transform);

              if (!graphene_matrix_is_identity (&child_transform) &&
                  (!graphene_matrix_inverse (&child_transform, &inverted) ||
                   !graphene_matrix_is_2d (&child_transform)))
                continue;
            }

          analyze_offloadable_actor (child, stage_view,
                                     offloadable_actors, poisoned_region);
        }
      else
        {
          clutter_offloadable_analyze_children (child, stage_view,
                                                offloadable_actors,
                                                poisoned_region);

          if (actor_overlaps_view (child, stage_view))
            analyze_non_offloadable_actor (child, poisoned_region);
        }
    }
}
