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

/*
 * Infrastructure for offloading actor content from the composited scene.
 *
 * Some actors can be presented without compositing them into the stage's
 * framebuffer — for example by placing their content on a separate display
 * plane. This avoids a texture copy and saves rendering bandwidth.
 *
 * The pipeline has three stages:
 *
 * 1. Analysis — clutter_offloadable_analyze_children() walks the actor tree
 *    front-to-back and collects a ClutterOffloadableActors list of candidates.
 *    Each candidate is a ClutterOffloadable actor that reported a non-NONE
 *    offload type. Candidates may be marked "poisoned" if non-offloadable
 *    alpha content above them prevents offloading.
 *
 * 2. Assignment — the backend takes the candidate list, decides which actors
 *    can actually be offloaded given its constraints, and records the result
 *    in a ClutterOffloadableOffloaded set.
 *
 * 3. Painting — the offloaded set is attached to the ClutterPaintContext.
 *    clutter_actor_paint() checks the set and skips rendering for any actor
 *    that was offloaded, since it is presented through a separate path.
 */

#pragma once

#include "clutter/clutter.h"

G_BEGIN_DECLS

/*
 * ClutterOffloadType:
 * @CLUTTER_OFFLOAD_TYPE_NONE: Actor cannot be offloaded.
 * @CLUTTER_OFFLOAD_TYPE_OPAQUE: Actor is fully opaque — it does not need to
 *   be blended with content below and hides anything underneath.
 * @CLUTTER_OFFLOAD_TYPE_ALPHA: Actor has alpha — it must be blended with
 *   content below it.
 */
typedef enum _ClutterOffloadType
{
  CLUTTER_OFFLOAD_TYPE_NONE,
  CLUTTER_OFFLOAD_TYPE_OPAQUE,
  CLUTTER_OFFLOAD_TYPE_ALPHA,
} ClutterOffloadType;

/* Offload candidate, ordered front-to-back (array index = implicit z-index) */
typedef struct _ClutterOffloadEntry
{
  ClutterActor *actor; /* the actor implementing ClutterOffloadable */
  ClutterOffloadType type; /* whether the actor is fully opaque or has alpha */
  MtkRegion *region; /* screen-space region covered by the actor */
  gboolean poisoned; /* TRUE if non-offloadable alpha content above prevents standalone offloading */
  gboolean occupied; /* TRUE if offloadable/non-offloadable non-alpha content overlaps above*/
} ClutterOffloadEntry;

/*
 * ClutterOffloadableActors:
 *
 * Ordered list of offload candidates produced by the analysis pass.
 * Consumed by the backend to decide which actors to actually offload.
 */
#define CLUTTER_TYPE_OFFLOADABLE_ACTORS (clutter_offloadable_actors_get_type ())
CLUTTER_EXPORT
G_DECLARE_FINAL_TYPE (ClutterOffloadableActors,
                      clutter_offloadable_actors,
                      CLUTTER, OFFLOADABLE_ACTORS,
                      GObject)

CLUTTER_EXPORT
ClutterOffloadableActors * clutter_offloadable_actors_new (void);

CLUTTER_EXPORT
void clutter_offloadable_actors_add_entry (ClutterOffloadableActors *actors,
                                           ClutterActor             *actor,
                                           ClutterOffloadType        type,
                                           MtkRegion                *region,
                                           gboolean                  poisoned,
                                           gboolean                  occupied);

CLUTTER_EXPORT
size_t clutter_offloadable_actors_get_n_entries (ClutterOffloadableActors *actors);

CLUTTER_EXPORT
const ClutterOffloadEntry * clutter_offloadable_actors_get_entry (ClutterOffloadableActors *actors,
                                                                  size_t                    index);

/*
 * ClutterOffloadableOffloaded:
 *
 * Set of actors that were successfully offloaded by the backend. Attached
 * to the ClutterPaintContext so that clutter_actor_paint() can skip
 * rendering for these actors.
 */
#define CLUTTER_TYPE_OFFLOADABLE_OFFLOADED (clutter_offloadable_offloaded_get_type ())
CLUTTER_EXPORT
G_DECLARE_FINAL_TYPE (ClutterOffloadableOffloaded,
                      clutter_offloadable_offloaded,
                      CLUTTER, OFFLOADABLE_OFFLOADED,
                      GObject)

CLUTTER_EXPORT
ClutterOffloadableOffloaded * clutter_offloadable_offloaded_new (void);

CLUTTER_EXPORT
void clutter_offloadable_offloaded_add_actor (ClutterOffloadableOffloaded *offloaded,
                                              ClutterActor                *actor);

CLUTTER_EXPORT
gboolean clutter_offloadable_offloaded_is_empty (ClutterOffloadableOffloaded *offloaded);

CLUTTER_EXPORT
gboolean clutter_offloadable_offloaded_contains_actor (ClutterOffloadableOffloaded *offloaded,
                                                       ClutterActor                *actor);

CLUTTER_EXPORT
void clutter_offloadable_offloaded_set_primary_actor (ClutterOffloadableOffloaded *offloaded,
                                                       ClutterActor                *actor);

CLUTTER_EXPORT
ClutterActor * clutter_offloadable_offloaded_get_primary_actor (ClutterOffloadableOffloaded *offloaded);

/*
 * ClutterOffloadable:
 *
 * Interface for actors whose content can be offloaded from the composited
 * scene. Implementors return their offload type (opaque or alpha) and the
 * screen-space region they cover. The analysis pass calls get_offload_type()
 * on each implementing actor it encounters during the tree walk.
 */
#define CLUTTER_TYPE_OFFLOADABLE (clutter_offloadable_get_type ())
CLUTTER_EXPORT
G_DECLARE_INTERFACE (ClutterOffloadable,
                     clutter_offloadable,
                     CLUTTER, OFFLOADABLE,
                     ClutterActor)

struct _ClutterOffloadableInterface
{
  GTypeInterface g_iface;

  ClutterOffloadType (* get_offload_type) (ClutterOffloadable  *offloadable,
                                           ClutterStageView    *stage_view,
                                           MtkRegion          **out_region);
};

CLUTTER_EXPORT
ClutterOffloadType clutter_offloadable_get_offload_type (ClutterOffloadable  *offloadable,
                                                         ClutterStageView    *stage_view,
                                                         MtkRegion          **out_region);

CLUTTER_EXPORT
void clutter_offloadable_analyze_children (ClutterActor             *actor,
                                           ClutterStageView         *stage_view,
                                           ClutterOffloadableActors *offloadable_actors,
                                           MtkRegion                *poisoned_region,
                                           MtkRegion                *occupied_region);

G_END_DECLS
