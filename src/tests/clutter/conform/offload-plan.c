#include <clutter/clutter.h>

#include "clutter/clutter-mutter.h"
#include "clutter/clutter-offloadable.h"
#include "tests/clutter-test-utils.h"

/* Mock offloadable actor with configurable type and region */

#define TEST_TYPE_OFFLOADABLE_ACTOR (test_offloadable_actor_get_type ())
G_DECLARE_FINAL_TYPE (TestOffloadableActor, test_offloadable_actor,
                      TEST, OFFLOADABLE_ACTOR, ClutterActor)

struct _TestOffloadableActor
{
  ClutterActor parent;

  ClutterOffloadType offload_type;
  MtkRectangle region_rect;
};

static ClutterOffloadType
test_offloadable_actor_get_offload_type (ClutterOffloadable  *offloadable,
                                         ClutterStageView    *stage_view,
                                         MtkRegion          **out_region)
{
  TestOffloadableActor *self = TEST_OFFLOADABLE_ACTOR (offloadable);

  if (self->offload_type == CLUTTER_OFFLOAD_TYPE_NONE)
    return CLUTTER_OFFLOAD_TYPE_NONE;

  *out_region = mtk_region_create_rectangle (&self->region_rect);

  return self->offload_type;
}

static void
test_offloadable_iface_init (ClutterOffloadableInterface *iface)
{
  iface->get_offload_type = test_offloadable_actor_get_offload_type;
}

G_DEFINE_TYPE_WITH_CODE (TestOffloadableActor, test_offloadable_actor,
                         CLUTTER_TYPE_ACTOR,
                         G_IMPLEMENT_INTERFACE (CLUTTER_TYPE_OFFLOADABLE,
                                                test_offloadable_iface_init))

static void
test_offloadable_actor_class_init (TestOffloadableActorClass *klass)
{
}

static void
test_offloadable_actor_init (TestOffloadableActor *self)
{
  self->offload_type = CLUTTER_OFFLOAD_TYPE_NONE;
  self->region_rect = (MtkRectangle) { 0, 0, 0, 0 };
}

static ClutterActor *
test_offloadable_actor_new (ClutterOffloadType  type,
                            int                 x,
                            int                 y,
                            int                 width,
                            int                 height)
{
  TestOffloadableActor *self;

  self = g_object_new (TEST_TYPE_OFFLOADABLE_ACTOR, NULL);
  self->offload_type = type;
  self->region_rect = (MtkRectangle) { x, y, width, height };

  return CLUTTER_ACTOR (self);
}

static void
allocate_actor (ClutterActor *actor,
                float         x,
                float         y,
                float         width,
                float         height)
{
  ClutterActorBox box = CLUTTER_ACTOR_BOX_INIT (x, y,
                                                 x + width, y + height);

  clutter_actor_allocate (actor, &box);
}

/* Tests */

static void
offload_plan_empty (void)
{
  g_autoptr (ClutterOffloadableActors) plan = NULL;

  plan = clutter_offloadable_actors_new ();

  g_assert_cmpuint (clutter_offloadable_actors_get_n_entries (plan), ==, 0);
}

static void
offload_plan_add_entries (void)
{
  g_autoptr (ClutterOffloadableActors) plan = NULL;
  g_autoptr (ClutterActor) actor1 = NULL;
  g_autoptr (ClutterActor) actor2 = NULL;
  g_autoptr (ClutterActor) actor3 = NULL;
  g_autoptr (MtkRegion) region1 = NULL;
  g_autoptr (MtkRegion) region2 = NULL;
  g_autoptr (MtkRegion) region3 = NULL;
  MtkRectangle rect1 = { 0, 0, 100, 100 };
  MtkRectangle rect2 = { 50, 50, 200, 200 };
  MtkRectangle rect3 = { 100, 100, 300, 300 };
  const ClutterOffloadEntry *entry;

  plan = clutter_offloadable_actors_new ();
  actor1 = clutter_actor_new ();
  g_object_ref_sink (actor1);
  actor2 = clutter_actor_new ();
  g_object_ref_sink (actor2);
  actor3 = clutter_actor_new ();
  g_object_ref_sink (actor3);

  region1 = mtk_region_create_rectangle (&rect1);
  region2 = mtk_region_create_rectangle (&rect2);
  region3 = mtk_region_create_rectangle (&rect3);

  clutter_offloadable_actors_add_entry (plan, actor1,
                                  CLUTTER_OFFLOAD_TYPE_OPAQUE,
                                  region1,
                                  /* poisoned = */ FALSE,
                                  /* occupied = */ FALSE);
  clutter_offloadable_actors_add_entry (plan, actor2,
                                  CLUTTER_OFFLOAD_TYPE_ALPHA,
                                  region2,
                                  /* poisoned = */ FALSE,
                                  /* occupied = */ TRUE);

  clutter_offloadable_actors_add_entry (plan, actor3,
                                  CLUTTER_OFFLOAD_TYPE_OPAQUE,
                                  region3,
                                  /* poisoned = */ TRUE,
                                  /* occupied = */ FALSE);

  g_assert_cmpuint (clutter_offloadable_actors_get_n_entries (plan), ==, 3);

  entry = clutter_offloadable_actors_get_entry (plan, 0);
  g_assert_nonnull (entry);
  g_assert_true (entry->actor == actor1);
  g_assert_cmpint (entry->type, ==, CLUTTER_OFFLOAD_TYPE_OPAQUE);
  g_assert_false (entry->poisoned);
  g_assert_false (entry->occupied);

  entry = clutter_offloadable_actors_get_entry (plan, 1);
  g_assert_nonnull (entry);
  g_assert_true (entry->actor == actor2);
  g_assert_cmpint (entry->type, ==, CLUTTER_OFFLOAD_TYPE_ALPHA);
  g_assert_false (entry->poisoned);
  g_assert_true (entry->occupied);

  entry = clutter_offloadable_actors_get_entry (plan, 2);
  g_assert_nonnull (entry);
  g_assert_true (entry->actor == actor3);
  g_assert_cmpint (entry->type, ==, CLUTTER_OFFLOAD_TYPE_OPAQUE);
  g_assert_true (entry->poisoned);
  g_assert_false (entry->occupied);
}

static void
analyze_single_opaque (void)
{
  g_autoptr (ClutterOffloadableActors) plan = NULL;
  g_autoptr (MtkRegion) poisoned = NULL;
  g_autoptr (MtkRegion) occupied = NULL;
  ClutterActor *stage, *container, *offloadable;
  const ClutterOffloadEntry *entry;

  stage = clutter_test_get_stage ();

  container = clutter_actor_new ();
  clutter_actor_add_child (stage, container);

  offloadable = test_offloadable_actor_new (CLUTTER_OFFLOAD_TYPE_OPAQUE,
                                            0, 0, 1920, 1080);
  clutter_actor_add_child (container, offloadable);

  clutter_actor_show (stage);
  allocate_actor (container, 0, 0, 1920, 1080);
  allocate_actor (offloadable, 0, 0, 1920, 1080);

  plan = clutter_offloadable_actors_new ();
  poisoned = mtk_region_create ();
  occupied = mtk_region_create ();

  clutter_offloadable_analyze_children (container, NULL, plan, poisoned, occupied);

  g_assert_cmpuint (clutter_offloadable_actors_get_n_entries (plan), ==, 1);

  entry = clutter_offloadable_actors_get_entry (plan, 0);
  g_assert_true (entry->actor == offloadable);
  g_assert_cmpint (entry->type, ==, CLUTTER_OFFLOAD_TYPE_OPAQUE);
  g_assert_false (entry->poisoned);
  g_assert_false (entry->occupied);

  clutter_actor_destroy (container);
}

static void
analyze_two_opaque_non_overlapping (void)
{
  g_autoptr (ClutterOffloadableActors) plan = NULL;
  g_autoptr (MtkRegion) poisoned = NULL;
  g_autoptr (MtkRegion) occupied = NULL;
  ClutterActor *stage, *container, *top, *bottom;
  const ClutterOffloadEntry *entry;

  stage = clutter_test_get_stage ();

  container = clutter_actor_new ();
  clutter_actor_add_child (stage, container);

  bottom = test_offloadable_actor_new (CLUTTER_OFFLOAD_TYPE_OPAQUE,
                                       0, 0, 960, 1080);
  top = test_offloadable_actor_new (CLUTTER_OFFLOAD_TYPE_OPAQUE,
                                    960, 0, 960, 1080);
  clutter_actor_add_child (container, bottom);
  clutter_actor_add_child (container, top);

  clutter_actor_show (stage);
  allocate_actor (container, 0, 0, 1920, 1080);
  allocate_actor (bottom, 0, 0, 960, 1080);
  allocate_actor (top, 960, 0, 960, 1080);

  plan = clutter_offloadable_actors_new ();
  poisoned = mtk_region_create ();
  occupied = mtk_region_create ();

  clutter_offloadable_analyze_children (container, NULL, plan, poisoned, occupied);

  /* Front-to-back: top is encountered first */
  g_assert_cmpuint (clutter_offloadable_actors_get_n_entries (plan), ==, 2);

  entry = clutter_offloadable_actors_get_entry (plan, 0);
  g_assert_true (entry->actor == top);
  g_assert_false (entry->poisoned);
  g_assert_false (entry->occupied);

  entry = clutter_offloadable_actors_get_entry (plan, 1);
  g_assert_true (entry->actor == bottom);
  g_assert_false (entry->poisoned);
  g_assert_false (entry->occupied);

  clutter_actor_destroy (container);
}

static void
analyze_non_offloadable_alpha_poisons_below (void)
{
  g_autoptr (ClutterOffloadableActors) plan = NULL;
  g_autoptr (MtkRegion) poisoned = NULL;
  g_autoptr (MtkRegion) occupied = NULL;
  ClutterActor *stage, *container, *bottom_offloadable, *top_alpha;
  const ClutterOffloadEntry *entry;

  stage = clutter_test_get_stage ();

  container = clutter_actor_new ();
  clutter_actor_add_child (stage, container);

  bottom_offloadable = test_offloadable_actor_new (CLUTTER_OFFLOAD_TYPE_OPAQUE,
                                                   0, 0, 1920, 1080);
  clutter_actor_add_child (container, bottom_offloadable);

  /* Non-offloadable alpha actor overlapping the same area */
  top_alpha = clutter_actor_new ();
  clutter_actor_set_opacity (top_alpha, 128);
  clutter_actor_add_child (container, top_alpha);

  clutter_actor_show (stage);
  allocate_actor (container, 0, 0, 1920, 1080);
  allocate_actor (bottom_offloadable, 0, 0, 1920, 1080);
  allocate_actor (top_alpha, 0, 0, 1920, 1080);

  plan = clutter_offloadable_actors_new ();
  poisoned = mtk_region_create ();
  occupied = mtk_region_create ();

  clutter_offloadable_analyze_children (container, NULL, plan, poisoned, occupied);

  g_assert_cmpuint (clutter_offloadable_actors_get_n_entries (plan), ==, 1);

  entry = clutter_offloadable_actors_get_entry (plan, 0);
  g_assert_true (entry->actor == bottom_offloadable);
  g_assert_true (entry->poisoned);
  g_assert_false (entry->occupied);

  clutter_actor_destroy (container);
}

static void
analyze_non_offloadable_opaque_does_not_poison (void)
{
  g_autoptr (ClutterOffloadableActors) plan = NULL;
  g_autoptr (MtkRegion) poisoned = NULL;
  g_autoptr (MtkRegion) occupied = NULL;
  ClutterActor *stage, *container, *bottom_offloadable, *top_opaque;
  CoglColor black;
  const ClutterOffloadEntry *entry;

  stage = clutter_test_get_stage ();

  container = clutter_actor_new ();
  clutter_actor_add_child (stage, container);

  bottom_offloadable = test_offloadable_actor_new (CLUTTER_OFFLOAD_TYPE_OPAQUE,
                                                   0, 0, 1920, 1080);
  clutter_actor_add_child (container, bottom_offloadable);

  /* Non-offloadable but opaque: plain actor with solid background color */
  top_opaque = clutter_actor_new ();
  cogl_color_init_from_4f (&black, 0.0f, 0.0f, 0.0f, 1.0f);
  clutter_actor_set_background_color (top_opaque, &black);
  clutter_actor_add_child (container, top_opaque);

  clutter_actor_show (stage);
  allocate_actor (container, 0, 0, 1920, 1080);
  allocate_actor (bottom_offloadable, 0, 0, 1920, 1080);
  allocate_actor (top_opaque, 0, 0, 1920, 1080);

  plan = clutter_offloadable_actors_new ();
  poisoned = mtk_region_create ();
  occupied = mtk_region_create ();

  clutter_offloadable_analyze_children (container, NULL, plan, poisoned, occupied);

  g_assert_cmpuint (clutter_offloadable_actors_get_n_entries (plan), ==, 1);

  entry = clutter_offloadable_actors_get_entry (plan, 0);
  g_assert_true (entry->actor == bottom_offloadable);
  g_assert_false (entry->poisoned);
  g_assert_true (entry->occupied);

  clutter_actor_destroy (container);
}

static void
analyze_invisible_actor_skipped (void)
{
  g_autoptr (ClutterOffloadableActors) plan = NULL;
  g_autoptr (MtkRegion) poisoned = NULL;
  g_autoptr (MtkRegion) occupied = NULL;
  ClutterActor *stage, *container, *offloadable;

  stage = clutter_test_get_stage ();

  container = clutter_actor_new ();
  clutter_actor_add_child (stage, container);

  offloadable = test_offloadable_actor_new (CLUTTER_OFFLOAD_TYPE_OPAQUE,
                                            0, 0, 1920, 1080);
  clutter_actor_hide (offloadable);
  clutter_actor_add_child (container, offloadable);

  clutter_actor_show (stage);
  allocate_actor (container, 0, 0, 1920, 1080);
  allocate_actor (offloadable, 0, 0, 1920, 1080);

  plan = clutter_offloadable_actors_new ();
  poisoned = mtk_region_create ();
  occupied = mtk_region_create ();

  clutter_offloadable_analyze_children (container, NULL, plan, poisoned, occupied);

  g_assert_cmpuint (clutter_offloadable_actors_get_n_entries (plan), ==, 0);

  clutter_actor_destroy (container);
}

static void
analyze_offloadable_type_none_skipped (void)
{
  g_autoptr (ClutterOffloadableActors) plan = NULL;
  g_autoptr (MtkRegion) poisoned = NULL;
  g_autoptr (MtkRegion) occupied = NULL;
  ClutterActor *stage, *container, *offloadable;

  stage = clutter_test_get_stage ();

  container = clutter_actor_new ();
  clutter_actor_add_child (stage, container);

  offloadable = test_offloadable_actor_new (CLUTTER_OFFLOAD_TYPE_NONE,
                                            0, 0, 100, 100);
  clutter_actor_add_child (container, offloadable);

  clutter_actor_show (stage);
  allocate_actor (container, 0, 0, 1920, 1080);
  allocate_actor (offloadable, 0, 0, 100, 100);

  plan = clutter_offloadable_actors_new ();
  poisoned = mtk_region_create ();
  occupied = mtk_region_create ();

  clutter_offloadable_analyze_children (container, NULL, plan, poisoned, occupied);

  g_assert_cmpuint (clutter_offloadable_actors_get_n_entries (plan), ==, 0);

  clutter_actor_destroy (container);
}

static void
analyze_alpha_offloadable_not_poisoned_when_clean (void)
{
  g_autoptr (ClutterOffloadableActors) plan = NULL;
  g_autoptr (MtkRegion) poisoned = NULL;
  g_autoptr (MtkRegion) occupied = NULL;
  ClutterActor *stage, *container, *offloadable;
  const ClutterOffloadEntry *entry;

  stage = clutter_test_get_stage ();

  container = clutter_actor_new ();
  clutter_actor_add_child (stage, container);

  offloadable = test_offloadable_actor_new (CLUTTER_OFFLOAD_TYPE_ALPHA,
                                            0, 0, 500, 500);
  clutter_actor_add_child (container, offloadable);

  clutter_actor_show (stage);
  allocate_actor (container, 0, 0, 1920, 1080);
  allocate_actor (offloadable, 0, 0, 500, 500);

  plan = clutter_offloadable_actors_new ();
  poisoned = mtk_region_create ();
  occupied = mtk_region_create ();

  clutter_offloadable_analyze_children (container, NULL, plan, poisoned, occupied);

  g_assert_cmpuint (clutter_offloadable_actors_get_n_entries (plan), ==, 1);

  entry = clutter_offloadable_actors_get_entry (plan, 0);
  g_assert_cmpint (entry->type, ==, CLUTTER_OFFLOAD_TYPE_ALPHA);
  g_assert_false (entry->poisoned);
  g_assert_false (entry->occupied);

  clutter_actor_destroy (container);
}

static void
analyze_nested_offloadable (void)
{
  g_autoptr (ClutterOffloadableActors) plan = NULL;
  g_autoptr (MtkRegion) poisoned = NULL;
  g_autoptr (MtkRegion) occupied = NULL;
  ClutterActor *stage, *container, *subcontainer, *offloadable;
  const ClutterOffloadEntry *entry;

  stage = clutter_test_get_stage ();

  container = clutter_actor_new ();
  clutter_actor_add_child (stage, container);

  /* Offloadable nested inside a non-offloadable subcontainer */
  subcontainer = clutter_actor_new ();
  clutter_actor_add_child (container, subcontainer);

  offloadable = test_offloadable_actor_new (CLUTTER_OFFLOAD_TYPE_OPAQUE,
                                            0, 0, 800, 600);
  clutter_actor_add_child (subcontainer, offloadable);

  clutter_actor_show (stage);
  allocate_actor (container, 0, 0, 1920, 1080);
  allocate_actor (subcontainer, 0, 0, 1920, 1080);
  allocate_actor (offloadable, 0, 0, 800, 600);

  plan = clutter_offloadable_actors_new ();
  poisoned = mtk_region_create ();
  occupied = mtk_region_create ();

  clutter_offloadable_analyze_children (container, NULL, plan, poisoned, occupied);

  g_assert_cmpuint (clutter_offloadable_actors_get_n_entries (plan), ==, 1);

  entry = clutter_offloadable_actors_get_entry (plan, 0);
  g_assert_true (entry->actor == offloadable);
  g_assert_false (entry->poisoned);
  g_assert_false (entry->occupied);

  clutter_actor_destroy (container);
}

static void
analyze_partial_overlap_poisons (void)
{
  g_autoptr (ClutterOffloadableActors) plan = NULL;
  g_autoptr (MtkRegion) poisoned = NULL;
  g_autoptr (MtkRegion) occupied = NULL;
  ClutterActor *stage, *container, *bottom, *top_alpha;
  const ClutterOffloadEntry *entry;

  stage = clutter_test_get_stage ();

  container = clutter_actor_new ();
  clutter_actor_add_child (stage, container);

  /* Bottom offloadable: 0,0 -> 1920,1080 */
  bottom = test_offloadable_actor_new (CLUTTER_OFFLOAD_TYPE_OPAQUE,
                                       0, 0, 1920, 1080);
  clutter_actor_add_child (container, bottom);

  /* Top: non-offloadable alpha covering only a small region */
  top_alpha = clutter_actor_new ();
  clutter_actor_set_opacity (top_alpha, 128);
  clutter_actor_add_child (container, top_alpha);

  clutter_actor_show (stage);
  allocate_actor (container, 0, 0, 1920, 1080);
  allocate_actor (bottom, 0, 0, 1920, 1080);
  allocate_actor (top_alpha, 100, 100, 200, 200);

  plan = clutter_offloadable_actors_new ();
  poisoned = mtk_region_create ();
  occupied = mtk_region_create ();

  clutter_offloadable_analyze_children (container, NULL, plan, poisoned, occupied);

  g_assert_cmpuint (clutter_offloadable_actors_get_n_entries (plan), ==, 1);

  entry = clutter_offloadable_actors_get_entry (plan, 0);
  g_assert_true (entry->actor == bottom);
  g_assert_true (entry->poisoned);
  g_assert_false (entry->occupied);

  clutter_actor_destroy (container);
}

static void
analyze_no_overlap_not_poisoned (void)
{
  g_autoptr (ClutterOffloadableActors) plan = NULL;
  g_autoptr (MtkRegion) poisoned = NULL;
  g_autoptr (MtkRegion) occupied = NULL;
  ClutterActor *stage, *container, *bottom, *top_alpha;
  const ClutterOffloadEntry *entry;

  stage = clutter_test_get_stage ();

  container = clutter_actor_new ();
  clutter_actor_add_child (stage, container);

  /* Bottom offloadable at 0,0 -> 500,500 */
  bottom = test_offloadable_actor_new (CLUTTER_OFFLOAD_TYPE_OPAQUE,
                                       0, 0, 500, 500);
  clutter_actor_add_child (container, bottom);

  /* Top non-offloadable alpha at 600,600 -> 800,800 — no overlap */
  top_alpha = clutter_actor_new ();
  clutter_actor_set_opacity (top_alpha, 128);
  clutter_actor_add_child (container, top_alpha);

  clutter_actor_show (stage);
  allocate_actor (container, 0, 0, 1920, 1080);
  allocate_actor (bottom, 0, 0, 500, 500);
  allocate_actor (top_alpha, 600, 600, 200, 200);

  plan = clutter_offloadable_actors_new ();
  poisoned = mtk_region_create ();
  occupied = mtk_region_create ();

  clutter_offloadable_analyze_children (container, NULL, plan, poisoned, occupied);

  g_assert_cmpuint (clutter_offloadable_actors_get_n_entries (plan), ==, 1);

  entry = clutter_offloadable_actors_get_entry (plan, 0);
  g_assert_true (entry->actor == bottom);
  g_assert_false (entry->poisoned);
  g_assert_false (entry->occupied);

  clutter_actor_destroy (container);
}

static void
analyze_opaque_subtracts_poison (void)
{
  g_autoptr (ClutterOffloadableActors) plan = NULL;
  g_autoptr (MtkRegion) poisoned = NULL;
  g_autoptr (MtkRegion) occupied = NULL;
  ClutterActor *stage, *container, *bottom, *middle_opaque, *top_alpha;
  const ClutterOffloadEntry *entry;

  stage = clutter_test_get_stage ();

  container = clutter_actor_new ();
  clutter_actor_add_child (stage, container);

  /* Bottom: offloadable opaque covering full area */
  bottom = test_offloadable_actor_new (CLUTTER_OFFLOAD_TYPE_OPAQUE,
                                       0, 0, 1920, 1080);
  clutter_actor_add_child (container, bottom);

  /* Middle: offloadable opaque covering full area — hides bottom */
  middle_opaque = test_offloadable_actor_new (CLUTTER_OFFLOAD_TYPE_OPAQUE,
                                              0, 0, 1920, 1080);
  clutter_actor_add_child (container, middle_opaque);

  /* Top: non-offloadable alpha covering full area */
  top_alpha = clutter_actor_new ();
  clutter_actor_set_opacity (top_alpha, 128);
  clutter_actor_add_child (container, top_alpha);

  clutter_actor_show (stage);
  allocate_actor (container, 0, 0, 1920, 1080);
  allocate_actor (bottom, 0, 0, 1920, 1080);
  allocate_actor (middle_opaque, 0, 0, 1920, 1080);
  allocate_actor (top_alpha, 0, 0, 1920, 1080);

  plan = clutter_offloadable_actors_new ();
  poisoned = mtk_region_create ();
  occupied = mtk_region_create ();

  clutter_offloadable_analyze_children (container, NULL, plan, poisoned, occupied);

  /*
   * Front-to-back: top_alpha poisons, middle_opaque is poisoned but
   * still opaque — it hides everything below it, so the poison does not
   * propagate through. Bottom should NOT be poisoned.
   */
  g_assert_cmpuint (clutter_offloadable_actors_get_n_entries (plan), ==, 2);

  entry = clutter_offloadable_actors_get_entry (plan, 0);
  g_assert_true (entry->actor == middle_opaque);
  g_assert_true (entry->poisoned);
  g_assert_false (entry->occupied);

  entry = clutter_offloadable_actors_get_entry (plan, 1);
  g_assert_true (entry->actor == bottom);
  g_assert_false (entry->poisoned);
  g_assert_true (entry->occupied);

  clutter_actor_destroy (container);
}

CLUTTER_TEST_SUITE (
  CLUTTER_TEST_UNIT ("/offload-plan/empty", offload_plan_empty)
  CLUTTER_TEST_UNIT ("/offload-plan/add-entries", offload_plan_add_entries)
  CLUTTER_TEST_UNIT ("/offload-plan/analyze/single-opaque", analyze_single_opaque)
  CLUTTER_TEST_UNIT ("/offload-plan/analyze/two-opaque-non-overlapping", analyze_two_opaque_non_overlapping)
  CLUTTER_TEST_UNIT ("/offload-plan/analyze/non-offloadable-alpha-poisons", analyze_non_offloadable_alpha_poisons_below)
  CLUTTER_TEST_UNIT ("/offload-plan/analyze/non-offloadable-opaque-no-poison", analyze_non_offloadable_opaque_does_not_poison)
  CLUTTER_TEST_UNIT ("/offload-plan/analyze/invisible-skipped", analyze_invisible_actor_skipped)
  CLUTTER_TEST_UNIT ("/offload-plan/analyze/type-none-skipped", analyze_offloadable_type_none_skipped)
  CLUTTER_TEST_UNIT ("/offload-plan/analyze/alpha-clean", analyze_alpha_offloadable_not_poisoned_when_clean)
  CLUTTER_TEST_UNIT ("/offload-plan/analyze/nested", analyze_nested_offloadable)
  CLUTTER_TEST_UNIT ("/offload-plan/analyze/partial-overlap-poisons", analyze_partial_overlap_poisons)
  CLUTTER_TEST_UNIT ("/offload-plan/analyze/no-overlap-not-poisoned", analyze_no_overlap_not_poisoned)
  CLUTTER_TEST_UNIT ("/offload-plan/analyze/opaque-subtracts-poison", analyze_opaque_subtracts_poison)
)
