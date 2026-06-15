/*
 * Clutter.
 *
 * An OpenGL based 'interactive canvas' library.
 *
 * Copyright (C) 2007,2008,2009,2010,2011  Intel Corporation.
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

 * Authors:
 *  Matthew Allum
 *  Robert Bragg
 *  Neil Roberts
 *  Emmanuele Bassi
 */

#define COGL_DISABLE_DEPRECATION_WARNINGS

#include "config.h"

#include "backends/meta-stage-impl-private.h"

#include <stdlib.h>
#include <math.h>

#include "backends/meta-renderer.h"
#include "backends/meta-stage-view-private.h"
#include "clutter/clutter-mutter.h"
#include "cogl/cogl.h"
#include "cogl/cogl-frame-info-private.h"
#include "core/util-private.h"
#include "meta/meta-backend.h"
#include "backends/native/meta-drm-buffer.h"

#define MAX_STACK_RECTS 256

typedef struct _MetaStageImplPrivate
{
  MetaBackend *backend;
  int64_t global_frame_counter;
} MetaStageImplPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (MetaStageImpl, meta_stage_impl, CLUTTER_TYPE_STAGE_WINDOW)

enum
{
  PROP_0,

  PROP_WRAPPER,
  PROP_BACKEND,

  N_PROPS
};

static GParamSpec *obj_props[N_PROPS];

static void
meta_stage_impl_unrealize (ClutterStageWindow *stage_window)
{
  meta_topic (META_DEBUG_BACKEND, "Unrealizing Cogl stage [%p]", stage_window);
}

static gboolean
meta_stage_impl_realize (ClutterStageWindow *stage_window)
{
  meta_topic (META_DEBUG_BACKEND,
              "Realizing stage '%s' [%p]",
              G_OBJECT_TYPE_NAME (stage_window),
              stage_window);

  return TRUE;
}

static int64_t
meta_stage_impl_get_frame_counter (ClutterStageWindow *stage_window)
{
  MetaStageImpl *stage_impl = META_STAGE_IMPL (stage_window);
  MetaStageImplPrivate *priv =
    meta_stage_impl_get_instance_private (stage_impl);

  return priv->global_frame_counter;
}

static void
meta_stage_impl_show (ClutterStageWindow *stage_window,
                      gboolean            do_raise)
{
  MetaStageImpl *stage_impl = META_STAGE_IMPL (stage_window);

  clutter_actor_map (CLUTTER_ACTOR (stage_impl->wrapper));
}

static void
meta_stage_impl_hide (ClutterStageWindow *stage_window)
{
  MetaStageImpl *stage_impl = META_STAGE_IMPL (stage_window);

  clutter_actor_unmap (CLUTTER_ACTOR (stage_impl->wrapper));
}

static void
meta_stage_impl_resize (ClutterStageWindow *stage_window,
                        gint                width,
                        gint                height)
{
}

static void
paint_damage_region (ClutterStageWindow *stage_window,
                     ClutterStageView   *view,
                     MtkRegion          *swap_region,
                     MtkRegion          *queued_redraw_clip)
{
  CoglFramebuffer *framebuffer = clutter_stage_view_get_framebuffer (view);
  CoglContext *ctx = cogl_framebuffer_get_context (framebuffer);
  static CoglPipeline *overlay_blue = NULL;
  CoglColor blue_color, red_color;
  MetaStageImpl *stage_impl = META_STAGE_IMPL (stage_window);
  ClutterActor *actor = CLUTTER_ACTOR (stage_impl->wrapper);
  graphene_matrix_t transform;
  int n_rects, i;

  COGL_TRACE_BEGIN_SCOPED (PaintDamageRegion,
                           "Meta::StageImpl::paint_damage_region()");

  cogl_framebuffer_push_matrix (framebuffer);
  clutter_actor_get_transform (actor, &transform);
  cogl_framebuffer_transform (framebuffer, &transform);

  /* Blue for the swap region */
  if (G_UNLIKELY (overlay_blue == NULL))
    {
      overlay_blue = cogl_pipeline_new (ctx);
      cogl_color_init_from_4f (&blue_color, 0.0f, 0.0f, 0.2f, 0.2f);
      cogl_pipeline_set_color (overlay_blue, &blue_color);
    }

  n_rects = mtk_region_num_rectangles (swap_region);
  for (i = 0; i < n_rects; i++)
    {
      MtkRectangle rect;
      float x_1, x_2, y_1, y_2;

      rect = mtk_region_get_rectangle (swap_region, i);
      x_1 = rect.x;
      x_2 = rect.x + rect.width;
      y_1 = rect.y;
      y_2 = rect.y + rect.height;

      cogl_framebuffer_draw_rectangle (framebuffer, overlay_blue, x_1, y_1, x_2, y_2);
    }

  /* Red for the clip */
  if (queued_redraw_clip)
    {
      static CoglPipeline *overlay_red = NULL;

      if (G_UNLIKELY (overlay_red == NULL))
        {
          overlay_red = cogl_pipeline_new (ctx);
          cogl_color_init_from_4f (&red_color, 0.2f, 0.0f, 0.0f, 0.2f);
          cogl_pipeline_set_color (overlay_red, &red_color);
        }

      n_rects = mtk_region_num_rectangles (queued_redraw_clip);
      for (i = 0; i < n_rects; i++)
        {
          MtkRectangle rect;
          float x_1, x_2, y_1, y_2;

          rect = mtk_region_get_rectangle (queued_redraw_clip, i);
          x_1 = rect.x;
          x_2 = rect.x + rect.width;
          y_1 = rect.y;
          y_2 = rect.y + rect.height;

          cogl_framebuffer_draw_rectangle (framebuffer, overlay_red, x_1, y_1, x_2, y_2);
        }
    }

  cogl_framebuffer_pop_matrix (framebuffer);
}

static void
queue_damage_region (ClutterStageWindow *stage_window,
                     ClutterStageView   *stage_view,
                     MtkRegion          *damage_region)
{
  CoglFramebuffer *framebuffer;
  CoglOnscreen *onscreen;
  MtkMonitorTransform view_transform;

  if (mtk_region_is_empty (damage_region))
    return;

  framebuffer = clutter_stage_view_get_onscreen (stage_view);
  if (!COGL_IS_ONSCREEN (framebuffer))
    return;

  onscreen = COGL_ONSCREEN (framebuffer);

  view_transform = clutter_stage_view_get_transform (stage_view);
  if (view_transform != MTK_MONITOR_TRANSFORM_NORMAL)
    {
      g_autoptr (MtkRegion) region = NULL;
      int fb_width;
      int fb_height;
      int n_rects, i;
      g_autofree MtkRectangle *freeme = NULL;
      MtkRectangle *damage;

      fb_width = cogl_framebuffer_get_width (framebuffer);
      fb_height = cogl_framebuffer_get_height (framebuffer);

      n_rects = mtk_region_num_rectangles (damage_region);

      if (n_rects < MAX_STACK_RECTS)
        damage = g_newa (MtkRectangle, n_rects);
      else
        damage = freeme = g_new0 (MtkRectangle, n_rects);

      for (i = 0; i < n_rects; i++)
        {
          MtkRectangle rect;

          rect = mtk_region_get_rectangle (damage_region, i);

          mtk_rectangle_transform (&rect,
                                   clutter_stage_view_get_transform (stage_view),
                                   fb_width,
                                   fb_height,
                                   &damage[i]);
        }

      region = mtk_region_create_rectangles (damage, n_rects);
      cogl_onscreen_queue_damage_region (onscreen, region);
    }
  else
    {
      cogl_onscreen_queue_damage_region (onscreen, damage_region);
    }
}

static void
swap_framebuffer (ClutterStageWindow *stage_window,
                  ClutterStageView   *stage_view,
                  MtkRegion          *swap_region,
                  gboolean            swap_with_damage,
                  ClutterFrame       *frame)
{
  MetaStageImpl *stage_impl = META_STAGE_IMPL (stage_window);
  MetaStageImplPrivate *priv =
    meta_stage_impl_get_instance_private (stage_impl);
  CoglFramebuffer *framebuffer = clutter_stage_view_get_onscreen (stage_view);
  CoglContext *cogl_context = cogl_framebuffer_get_context (framebuffer);

  COGL_TRACE_BEGIN_SCOPED (SwapFramebuffer, "Meta::StageImpl::swap_framebuffer()");

  clutter_stage_view_before_swap_buffer (stage_view, swap_region);

  if (COGL_IS_ONSCREEN (framebuffer))
    {
      CoglOnscreen *onscreen = COGL_ONSCREEN (framebuffer);
      int n_rects;
      CoglFrameInfo *frame_info;


      frame_info =
        cogl_frame_info_new (cogl_context, priv->global_frame_counter,
                             frame->frame_count);
      priv->global_frame_counter++;

      n_rects = mtk_region_num_rectangles (swap_region);
      if (n_rects > 0 && !swap_with_damage)
        {
          meta_topic (META_DEBUG_BACKEND,
                      "cogl_onscreen_swap_region (onscreen: %p)",
                      onscreen);

          cogl_onscreen_swap_region (onscreen,
                                     swap_region,
                                     frame_info,
                                     frame);
        }
      else
        {
          meta_topic (META_DEBUG_BACKEND,
                      "cogl_onscreen_swap_buffers (onscreen: %p)",
                      onscreen);

          cogl_onscreen_swap_buffers_with_damage (onscreen,
                                                  swap_region,
                                                  frame_info,
                                                  frame);
        }
    }
  else
    {
      MetaStageView *view = META_STAGE_VIEW (stage_view);

      meta_topic (META_DEBUG_BACKEND,
                  "fake offscreen swap (framebuffer: %p)",
                  framebuffer);

      cogl_framebuffer_flush (framebuffer);
      meta_stage_view_perform_fake_swap (view, priv->global_frame_counter,
                                         frame->frame_count);
      priv->global_frame_counter++;
    }
}

static MtkRegion *
offset_scale_and_clamp_region (const MtkRegion *region,
                               int              offset_x,
                               int              offset_y,
                               float            scale)
{
  int n_rects, i;
  MtkRectangle *rects;
  g_autofree MtkRectangle *freeme = NULL;

  n_rects = mtk_region_num_rectangles (region);

  if (n_rects == 0)
    return mtk_region_create ();

  if (n_rects < MAX_STACK_RECTS)
    rects = g_newa (MtkRectangle, n_rects);
  else
    rects = freeme = g_new (MtkRectangle, n_rects);

  for (i = 0; i < n_rects; i++)
    {
      MtkRectangle *rect = &rects[i];
      graphene_rect_t tmp;

      *rect = mtk_region_get_rectangle (region, i);

      tmp = mtk_rectangle_to_graphene_rect (rect);
      graphene_rect_offset (&tmp, offset_x, offset_y);
      graphene_rect_scale (&tmp, scale, scale, &tmp);
      mtk_rectangle_from_graphene_rect (&tmp, MTK_ROUNDING_STRATEGY_GROW,
                                        rect);
    }

  return mtk_region_create_rectangles (rects, n_rects);
}

static MtkRegion *
scale_offset_and_clamp_region (const MtkRegion *region,
                               float            scale,
                               int              offset_x,
                               int              offset_y)
{
  int n_rects, i;
  MtkRectangle *rects;
  g_autofree MtkRectangle *freeme = NULL;

  n_rects = mtk_region_num_rectangles (region);

  if (n_rects == 0)
    return mtk_region_create ();

  if (n_rects < MAX_STACK_RECTS)
    rects = g_newa (MtkRectangle, n_rects);
  else
    rects = freeme = g_new (MtkRectangle, n_rects);

  for (i = 0; i < n_rects; i++)
    {
      MtkRectangle *rect = &rects[i];
      graphene_rect_t tmp;

      *rect = mtk_region_get_rectangle (region, i);

      tmp = mtk_rectangle_to_graphene_rect (rect);
      graphene_rect_scale (&tmp, scale, scale, &tmp);
      graphene_rect_offset (&tmp, offset_x, offset_y);
      mtk_rectangle_from_graphene_rect (&tmp,
                                        MTK_ROUNDING_STRATEGY_GROW,
                                        rect);
    }

  return mtk_region_create_rectangles (rects, n_rects);
}

static void
paint_stage (MetaStageImpl    *stage_impl,
             ClutterStageView *stage_view,
             MtkRegion        *redraw_clip,
             ClutterFrame     *frame)
{
  ClutterStage *stage = stage_impl->wrapper;

  _clutter_stage_maybe_setup_viewport (stage, stage_view);
  clutter_stage_paint_view (stage, stage_view, redraw_clip, frame);

  clutter_stage_view_after_paint (stage_view, redraw_clip);
}

static MtkRegion *
transform_swap_region_to_onscreen (ClutterStageView *stage_view,
                                   MtkRegion        *swap_region)
{
  CoglFramebuffer *onscreen = clutter_stage_view_get_onscreen (stage_view);
  int n_rects, i;
  MtkRectangle *rects;
  MtkRegion *transformed_region;
  int width, height;

  width = cogl_framebuffer_get_width (onscreen);
  height = cogl_framebuffer_get_height (onscreen);

  n_rects = mtk_region_num_rectangles (swap_region);
  rects = g_newa (MtkRectangle, n_rects);
  for (i = 0; i < n_rects; i++)
    {
      rects[i] = mtk_region_get_rectangle (swap_region, i);
      mtk_rectangle_transform (&rects[i],
                               clutter_stage_view_get_transform (stage_view),
                               width,
                               height,
                               &rects[i]);
    }
  transformed_region = mtk_region_create_rectangles (rects, n_rects);

  return transformed_region;
}

static gboolean
should_use_clipped_redraw (gboolean              is_full_redraw,
                           gboolean              has_buffer_age,
                           gboolean              buffer_has_valid_damage_history,
                           ClutterDrawDebugFlag  paint_debug_flags,
                           CoglFramebuffer      *framebuffer,
                           ClutterStageWindow   *stage_window)
{
  gboolean can_blit_sub_buffer;
  gboolean can_use_clipped_redraw;
  gboolean is_warmed_up;
  CoglContext *context = cogl_framebuffer_get_context (framebuffer);

  if (is_full_redraw)
    return FALSE;

  if (paint_debug_flags & CLUTTER_DEBUG_DISABLE_CLIPPED_REDRAWS)
    return FALSE;

  if (COGL_IS_OFFSCREEN (framebuffer))
    return TRUE;

  if (has_buffer_age && !buffer_has_valid_damage_history)
    {
      meta_topic (META_DEBUG_BACKEND,
                  "Invalid back buffer age: forcing full redraw");
      return FALSE;
    }

  can_blit_sub_buffer =
    cogl_context_has_winsys_feature (context, COGL_WINSYS_FEATURE_SWAP_REGION);
  can_use_clipped_redraw =
    _clutter_stage_window_can_clip_redraws (stage_window) &&
    (can_blit_sub_buffer || has_buffer_age);
  /* Some drivers struggle to get going and produce some junk
   * frames when starting up... */
  is_warmed_up =
    cogl_onscreen_get_frame_counter (COGL_ONSCREEN (framebuffer)) > 3;
  return is_warmed_up && can_use_clipped_redraw;
}

static void
meta_stage_impl_redraw_view_primary (MetaStageImpl    *stage_impl,
                                     ClutterStageView *stage_view,
                                     ClutterFrame     *frame)
{
  ClutterStageWindow *stage_window = CLUTTER_STAGE_WINDOW (stage_impl);
  MetaStageView *view = META_STAGE_VIEW (stage_view);
  CoglFramebuffer *fb = clutter_stage_view_get_framebuffer (stage_view);
  CoglContext *context = cogl_framebuffer_get_context (fb);
  CoglFramebuffer *onscreen = clutter_stage_view_get_onscreen (stage_view);
  MtkRectangle view_rect;
  gboolean is_full_redraw;
  gboolean use_clipped_redraw;
  gboolean buffer_has_valid_damage_history = FALSE;
  gboolean has_buffer_age;
  gboolean swap_with_damage;
  g_autoptr (MtkRegion) redraw_clip = NULL;
  g_autoptr (MtkRegion) queued_redraw_clip = NULL;
  g_autoptr (MtkRegion) fb_clip_region = NULL;
  g_autoptr (MtkRegion) swap_region = NULL;
  ClutterDrawDebugFlag paint_debug_flags;
  ClutterDamageHistory *damage_history;
  float fb_scale;
  int fb_width, fb_height;
  int buffer_age = 0;

  COGL_TRACE_BEGIN_SCOPED (RedrawViewPrimary,
                           "Meta::StageImpl::redraw_view_primary()");
  COGL_TRACE_DEFINE_COUNTER_INT (RedrawViewPrimaryDamageArea,
                                 "RedrawDamageArea",
                                 "the damaged area of the redraw");

  clutter_stage_view_get_layout (stage_view, &view_rect);
  fb_scale = clutter_stage_view_get_scale (stage_view);
  fb_width = cogl_framebuffer_get_width (fb);
  fb_height = cogl_framebuffer_get_height (fb);

  has_buffer_age =
    COGL_IS_ONSCREEN (onscreen) &&
    cogl_context_has_winsys_feature (context, COGL_WINSYS_FEATURE_BUFFER_AGE);

  redraw_clip = clutter_stage_view_take_accumulated_redraw_clip (stage_view);

  /* NB: a NULL redraw clip == full stage redraw */
  if (!redraw_clip)
    is_full_redraw = TRUE;
  else
    is_full_redraw = FALSE;

  damage_history = meta_stage_view_get_damage_history (view);

  if (has_buffer_age)
    {
      buffer_age = cogl_onscreen_get_buffer_age (COGL_ONSCREEN (onscreen));
      buffer_has_valid_damage_history =
        clutter_damage_history_is_age_valid (damage_history,
                                             buffer_age);
    }

  clutter_get_debug_flags (NULL, &paint_debug_flags, NULL);

  use_clipped_redraw =
    should_use_clipped_redraw (is_full_redraw,
                               has_buffer_age,
                               buffer_has_valid_damage_history,
                               paint_debug_flags,
                               onscreen,
                               stage_window);

  if (use_clipped_redraw)
    {
      fb_clip_region = offset_scale_and_clamp_region (redraw_clip,
                                                      -view_rect.x,
                                                      -view_rect.y,
                                                      fb_scale);

      if (G_UNLIKELY (paint_debug_flags & CLUTTER_DEBUG_PAINT_DAMAGE_REGION))
        {
          queued_redraw_clip =
            scale_offset_and_clamp_region (fb_clip_region,
                                           1.0f / fb_scale,
                                           view_rect.x,
                                           view_rect.y);
        }
    }
  else
    {
      MtkRectangle fb_rect;

      fb_rect = (MtkRectangle) {
        .width = fb_width,
        .height = fb_height,
      };
      fb_clip_region = mtk_region_create_rectangle (&fb_rect);

      g_clear_pointer (&redraw_clip, mtk_region_unref);
      redraw_clip = mtk_region_create_rectangle (&view_rect);

      if (G_UNLIKELY (paint_debug_flags & CLUTTER_DEBUG_PAINT_DAMAGE_REGION))
        queued_redraw_clip = mtk_region_ref (redraw_clip);
    }

  g_return_if_fail (!mtk_region_is_empty (fb_clip_region));

  /* XXX: It seems there will be a race here in that the stage
   * window may be resized before the cogl_onscreen_swap_region
   * is handled and so we may copy the wrong region. I can't
   * really see how we can handle this with the current state of X
   * but at least in this case a full redraw should be queued by
   * the resize anyway so it should only exhibit temporary
   * artefacts.
   */
  /* swap_region does not need damage history, set it up before that */
  if (!use_clipped_redraw)
    swap_region = mtk_region_create ();
  else if (clutter_stage_view_has_shadowfb (stage_view))
    swap_region = mtk_region_ref (fb_clip_region);
  else
    swap_region = mtk_region_copy (fb_clip_region);

  swap_with_damage = FALSE;
  if (has_buffer_age)
    {
      clutter_damage_history_record (damage_history, fb_clip_region);

      if (use_clipped_redraw)
        {
          int age;

          for (age = 1; age <= buffer_age; age++)
            {
              const MtkRegion *old_damage;

              old_damage =
                clutter_damage_history_lookup (damage_history, age);
              mtk_region_union (fb_clip_region, old_damage);
            }

          meta_topic (META_DEBUG_BACKEND,
                      "Reusing back buffer(age=%d) - repairing region: num rects: %d",
                      buffer_age,
                      mtk_region_num_rectangles (fb_clip_region));

          swap_with_damage = TRUE;
        }

      clutter_damage_history_step (damage_history);
    }

  if (use_clipped_redraw)
    {
      /* Regenerate redraw_clip because:
       *  1. It's missing the regions added from damage_history above; and
       *  2. If using fractional scaling then it might be a fraction of a
       *     logical pixel (or one physical pixel) smaller than
       *     fb_clip_region, due to the clamping from
       *     offset_scale_and_clamp_region. So we need to ensure redraw_clip
       *     is a superset of fb_clip_region to avoid such gaps.
       */
      g_clear_pointer (&redraw_clip, mtk_region_unref);
      redraw_clip = scale_offset_and_clamp_region (fb_clip_region,
                                                   1.0f / fb_scale,
                                                   view_rect.x,
                                                   view_rect.y);
    }

  if (paint_debug_flags & CLUTTER_DEBUG_PAINT_DAMAGE_REGION)
    {
      g_autoptr (MtkRegion) debug_redraw_clip = NULL;

      debug_redraw_clip = mtk_region_create_rectangle (&view_rect);
      paint_stage (stage_impl, stage_view, debug_redraw_clip, frame);
    }
  else if (use_clipped_redraw)
    {
      queue_damage_region (stage_window, stage_view, fb_clip_region);

      cogl_framebuffer_push_region_clip (fb, fb_clip_region);

      paint_stage (stage_impl, stage_view, redraw_clip, frame);

      cogl_framebuffer_pop_clip (fb);
    }
  else
    {
      meta_topic (META_DEBUG_BACKEND, "Unclipped stage paint");

      paint_stage (stage_impl, stage_view, redraw_clip, frame);
    }

#ifdef HAVE_PROFILER
  if (G_UNLIKELY (cogl_is_tracing_enabled ()))
    {
      g_autoptr (GString) rects_str = NULL;
      g_autofree char *area_str = NULL;
      int n_rectangles, i;
      int area = 0;

      rects_str = g_string_new ("");
      n_rectangles = mtk_region_num_rectangles (redraw_clip);

      for (i = 0; i < n_rectangles; i++)
        {
          MtkRectangle rect = mtk_region_get_rectangle (redraw_clip, i);

          area += mtk_rectangle_area (&rect);

          g_string_append_printf (rects_str, " %d,%d,%d,%d",
                                  rect.x, rect.y, rect.width, rect.height);
        }

      area_str = g_strdup_printf ("%d", area);
      g_string_prepend (rects_str, area_str);

      COGL_TRACE_DESCRIBE (RedrawViewPrimary, rects_str->str);
      COGL_TRACE_SET_COUNTER_INT (RedrawViewPrimaryDamageArea, area);
    }
#endif

  g_clear_pointer (&redraw_clip, mtk_region_unref);
  g_clear_pointer (&fb_clip_region, mtk_region_unref);

  if (queued_redraw_clip)
    {
      g_autoptr (MtkRegion) swap_region_in_stage_space = NULL;

      swap_region_in_stage_space =
        scale_offset_and_clamp_region (swap_region,
                                       1.0f / fb_scale,
                                       view_rect.x,
                                       view_rect.y);

      mtk_region_subtract (swap_region_in_stage_space, queued_redraw_clip);

      paint_damage_region (stage_window, stage_view,
                           swap_region_in_stage_space, queued_redraw_clip);
    }

  if (clutter_stage_view_get_onscreen (stage_view) !=
      clutter_stage_view_get_framebuffer (stage_view) &&
      mtk_region_num_rectangles (swap_region) != 0)
    {
      g_autoptr (MtkRegion) transformed_swap_region = NULL;

      transformed_swap_region =
        transform_swap_region_to_onscreen (stage_view, swap_region);
      g_clear_pointer (&swap_region, mtk_region_unref);
      swap_region = g_steal_pointer (&transformed_swap_region);
    }

  swap_framebuffer (stage_window,
                    stage_view,
                    swap_region,
                    swap_with_damage,
                    frame);
}

static gboolean
meta_stage_impl_scanout_view (MetaStageImpl     *stage_impl,
                              ClutterStageView  *stage_view,
                              GList              *scanout_list,
                              ClutterFrame      *frame,
                              GError           **error)
{
  // meta_topic (META_DEBUG_MOHAN, "meta_stage_impl_scanout_view: scanout_list=%p", scanout_list);

  MetaStageImplPrivate *priv =
    meta_stage_impl_get_instance_private (stage_impl);
  CoglFramebuffer *framebuffer =
    clutter_stage_view_get_onscreen (stage_view);
  CoglContext *cogl_context = cogl_framebuffer_get_context (framebuffer);
  CoglOnscreen *onscreen;
  CoglFrameInfo *frame_info;

  g_assert (COGL_IS_ONSCREEN (framebuffer));

  onscreen = COGL_ONSCREEN (framebuffer);

  frame_info = cogl_frame_info_new (cogl_context, priv->global_frame_counter,
                                    frame->frame_count);

  if (!cogl_onscreen_direct_scanout (onscreen,
                                     scanout_list,
                                     frame_info,
                                     frame,
                                     error))
    {
      g_object_unref (frame_info);
      return FALSE;
    }

  priv->global_frame_counter++;
  return TRUE;
}

static void
primary_paint_phase (MetaStageImpl    *stage_impl,
                     ClutterStageView *stage_view,
                     ClutterFrame     *frame,
                     gboolean         *swap_with_damage,
                     gboolean         *swap_with_region,
                     MtkRegion        **swap_region_out
                     )
{
  ClutterStageWindow *stage_window = CLUTTER_STAGE_WINDOW (stage_impl);
  MetaStageView *view = META_STAGE_VIEW (stage_view);
  CoglFramebuffer *fb = clutter_stage_view_get_framebuffer (stage_view);
  CoglContext *context = cogl_framebuffer_get_context (fb);
  CoglFramebuffer *onscreen = clutter_stage_view_get_onscreen (stage_view);
  MtkRectangle view_rect;
  gboolean is_full_redraw;
  gboolean use_clipped_redraw;
  gboolean buffer_has_valid_damage_history = FALSE;
  gboolean has_buffer_age;
  g_autoptr (MtkRegion) redraw_clip = NULL;
  g_autoptr (MtkRegion) queued_redraw_clip = NULL;
  g_autoptr (MtkRegion) fb_clip_region = NULL;
  ClutterDrawDebugFlag paint_debug_flags;
  ClutterDamageHistory *damage_history;
  float fb_scale;
  int fb_width, fb_height;
  int buffer_age = 0;

  COGL_TRACE_BEGIN_SCOPED (RedrawViewPrimary,
                           "Meta::StageImpl::redraw_view_primary()");
  COGL_TRACE_DEFINE_COUNTER_INT (RedrawViewPrimaryDamageArea,
                                 "RedrawDamageArea",
                                 "the damaged area of the redraw");

  clutter_stage_view_get_layout (stage_view, &view_rect);
  fb_scale = clutter_stage_view_get_scale (stage_view);
  fb_width = cogl_framebuffer_get_width (fb);
  fb_height = cogl_framebuffer_get_height (fb);

  has_buffer_age =
    COGL_IS_ONSCREEN (onscreen) &&
    cogl_context_has_winsys_feature (context, COGL_WINSYS_FEATURE_BUFFER_AGE);

  redraw_clip = clutter_stage_view_take_accumulated_redraw_clip (stage_view);

  /* NB: a NULL redraw clip == full stage redraw */
  if (!redraw_clip)
    is_full_redraw = TRUE;
  else
    is_full_redraw = FALSE;

  damage_history = meta_stage_view_get_damage_history (view);

  if (has_buffer_age)
    {
      buffer_age = cogl_onscreen_get_buffer_age (COGL_ONSCREEN (onscreen));
      buffer_has_valid_damage_history =
        clutter_damage_history_is_age_valid (damage_history,
                                             buffer_age);
    }

  clutter_get_debug_flags (NULL, &paint_debug_flags, NULL);

  use_clipped_redraw =
    should_use_clipped_redraw (is_full_redraw,
                               has_buffer_age,
                               buffer_has_valid_damage_history,
                               paint_debug_flags,
                               onscreen,
                               stage_window);

  if (use_clipped_redraw)
    {
      fb_clip_region = offset_scale_and_clamp_region (redraw_clip,
                                                      -view_rect.x,
                                                      -view_rect.y,
                                                      fb_scale);

      if (G_UNLIKELY (paint_debug_flags & CLUTTER_DEBUG_PAINT_DAMAGE_REGION))
        {
          queued_redraw_clip =
            scale_offset_and_clamp_region (fb_clip_region,
                                           1.0f / fb_scale,
                                           view_rect.x,
                                           view_rect.y);
        }
    }
  else
    {
      MtkRectangle fb_rect;

      fb_rect = (MtkRectangle) {
        .width = fb_width,
        .height = fb_height,
      };
      fb_clip_region = mtk_region_create_rectangle (&fb_rect);

      g_clear_pointer (&redraw_clip, mtk_region_unref);
      redraw_clip = mtk_region_create_rectangle (&view_rect);

      if (G_UNLIKELY (paint_debug_flags & CLUTTER_DEBUG_PAINT_DAMAGE_REGION))
        queued_redraw_clip = mtk_region_ref (redraw_clip);
    }

  g_return_if_fail (!mtk_region_is_empty (fb_clip_region));

  /* XXX: It seems there will be a race here in that the stage
   * window may be resized before the cogl_onscreen_swap_region
   * is handled and so we may copy the wrong region. I can't
   * really see how we can handle this with the current state of X
   * but at least in this case a full redraw should be queued by
   * the resize anyway so it should only exhibit temporary
   * artefacts.
   */
  /* swap_region does not need damage history, set it up before that */
  if (!use_clipped_redraw)
    *swap_region_out = mtk_region_create ();
  else if (clutter_stage_view_has_shadowfb (stage_view))
    *swap_region_out = mtk_region_ref (fb_clip_region);
  else
    *swap_region_out = mtk_region_copy (fb_clip_region);

  *swap_with_damage = FALSE;
  if (has_buffer_age)
    {
      clutter_damage_history_record (damage_history, fb_clip_region);

      if (use_clipped_redraw)
        {
          int age;

          for (age = 1; age <= buffer_age; age++)
            {
              const MtkRegion *old_damage;

              old_damage =
                clutter_damage_history_lookup (damage_history, age);
              mtk_region_union (fb_clip_region, old_damage);
            }

          meta_topic (META_DEBUG_BACKEND,
                      "Reusing back buffer(age=%d) - repairing region: num rects: %d",
                      buffer_age,
                      mtk_region_num_rectangles (fb_clip_region));

          *swap_with_damage = TRUE;
        }

      clutter_damage_history_step (damage_history);
    }

  if (use_clipped_redraw)
    {
      /* Regenerate redraw_clip because:
       *  1. It's missing the regions added from damage_history above; and
       *  2. If using fractional scaling then it might be a fraction of a
       *     logical pixel (or one physical pixel) smaller than
       *     fb_clip_region, due to the clamping from
       *     offset_scale_and_clamp_region. So we need to ensure redraw_clip
       *     is a superset of fb_clip_region to avoid such gaps.
       */
      g_clear_pointer (&redraw_clip, mtk_region_unref);
      redraw_clip = scale_offset_and_clamp_region (fb_clip_region,
                                                   1.0f / fb_scale,
                                                   view_rect.x,
                                                   view_rect.y);
    }

  if (paint_debug_flags & CLUTTER_DEBUG_PAINT_DAMAGE_REGION)
    {
      g_autoptr (MtkRegion) debug_redraw_clip = NULL;

      debug_redraw_clip = mtk_region_create_rectangle (&view_rect);
      paint_stage (stage_impl, stage_view, debug_redraw_clip, frame);
    }
  else if (use_clipped_redraw)
    {
      queue_damage_region (stage_window, stage_view, fb_clip_region);

      cogl_framebuffer_push_region_clip (fb, fb_clip_region);

      paint_stage (stage_impl, stage_view, redraw_clip, frame);

      cogl_framebuffer_pop_clip (fb);
    }
  else
    {
      meta_topic (META_DEBUG_BACKEND, "Unclipped stage paint");

      paint_stage (stage_impl, stage_view, redraw_clip, frame);
    }

#ifdef HAVE_PROFILER
  if (G_UNLIKELY (cogl_is_tracing_enabled ()))
    {
      g_autoptr (GString) rects_str = NULL;
      g_autofree char *area_str = NULL;
      int n_rectangles, i;
      int area = 0;

      rects_str = g_string_new ("");
      n_rectangles = mtk_region_num_rectangles (redraw_clip);

      for (i = 0; i < n_rectangles; i++)
        {
          MtkRectangle rect = mtk_region_get_rectangle (redraw_clip, i);

          area += mtk_rectangle_area (&rect);

          g_string_append_printf (rects_str, " %d,%d,%d,%d",
                                  rect.x, rect.y, rect.width, rect.height);
        }

      area_str = g_strdup_printf ("%d", area);
      g_string_prepend (rects_str, area_str);

      COGL_TRACE_DESCRIBE (RedrawViewPrimary, rects_str->str);
      COGL_TRACE_SET_COUNTER_INT (RedrawViewPrimaryDamageArea, area);
    }
#endif

  g_clear_pointer (&redraw_clip, mtk_region_unref);
  g_clear_pointer (&fb_clip_region, mtk_region_unref);

  if (queued_redraw_clip)
    {
      g_autoptr (MtkRegion) swap_region_in_stage_space = NULL;

      swap_region_in_stage_space =
        scale_offset_and_clamp_region (*swap_region_out,
                                       1.0f / fb_scale,
                                       view_rect.x,
                                       view_rect.y);

      mtk_region_subtract (swap_region_in_stage_space, queued_redraw_clip);

      paint_damage_region (stage_window, stage_view,
                           swap_region_in_stage_space, queued_redraw_clip);
    }

  if (clutter_stage_view_get_onscreen (stage_view) !=
      clutter_stage_view_get_framebuffer (stage_view) &&
      mtk_region_num_rectangles (*swap_region_out) != 0)
    {
      g_autoptr (MtkRegion) transformed_swap_region = NULL;

      transformed_swap_region =
        transform_swap_region_to_onscreen (stage_view, *swap_region_out);
      g_clear_pointer (swap_region_out, mtk_region_unref);
      *swap_region_out = g_steal_pointer (&transformed_swap_region);
    }

    if (mtk_region_num_rectangles (*swap_region_out) > 0 && !(*swap_with_damage))
      *swap_with_region = TRUE;
    else
      *swap_with_region = FALSE;
}

/// TODO change the func name
static void
meta_stage_impl_view (MetaStageImpl    *stage_impl,
                      ClutterStageView *stage_view,
                      ClutterFrame     *frame,
                      GList            *scanout_list,
                      gboolean          enable_primary_refresh,
                      gboolean          enable_scanout)
{
  MetaStageImplPrivate *priv =
    meta_stage_impl_get_instance_private (stage_impl);
  CoglFramebuffer *framebuffer =
    clutter_stage_view_get_onscreen (stage_view);
  CoglContext *cogl_context = cogl_framebuffer_get_context (framebuffer);
  CoglOnscreen *onscreen;
  CoglFrameInfo *frame_info;
  MtkRegion     *swap_region;
  gboolean      swap_with_damage = FALSE;
  gboolean      swap_with_region = FALSE;
  gboolean      onscreen_result;

  g_assert (COGL_IS_ONSCREEN (framebuffer));

  onscreen = COGL_ONSCREEN (framebuffer);

  if (enable_primary_refresh)
    {
      primary_paint_phase (stage_impl, stage_view, frame, &swap_with_damage, &swap_with_region, &swap_region);
      COGL_TRACE_BEGIN_SCOPED (SwapFramebuffer, "Meta::StageImpl::swap_framebuffer()");
      clutter_stage_view_before_swap_buffer (stage_view, swap_region);

      int n_rects = mtk_region_num_rectangles (swap_region);

      if (n_rects > 0 && !swap_with_damage)
        {
          swap_with_region = TRUE;
        }
      else
        {
          // For Fullscreen Redraw
          swap_with_damage = TRUE;
        }

      if (!onscreen)
        {
          MetaStageView *view = META_STAGE_VIEW (stage_view);

          meta_topic (META_DEBUG_BACKEND,
                      "fake offscreen swap (framebuffer: %p)",
                      framebuffer);

          cogl_framebuffer_flush (framebuffer);
          meta_stage_view_perform_fake_swap (view, priv->global_frame_counter, frame->frame_count);
        }
    }

    frame_info = cogl_frame_info_new (cogl_context, priv->global_frame_counter,
                                      frame->frame_count);
    g_autoptr (GError) error = NULL;
    onscreen_result = cogl_onscreen_compose_with_hw_plane (onscreen,
                                                           swap_region,
                                                           enable_scanout ? scanout_list : NULL,
                                                           frame_info,
                                                           swap_with_region,
                                                           swap_with_damage,
                                                           frame,
                                                           &error
                                                          );
    
    if ( onscreen_result )
     {
        priv->global_frame_counter++;

        if (enable_scanout)
          clutter_stage_view_accumulate_scanout_clip (stage_view);
     }
    else
     {
        g_object_unref (frame_info);
        if (g_error_matches (error,
                               COGL_SCANOUT_ERROR,
                               COGL_SCANOUT_ERROR_INHIBITED))
          {
            meta_topic (META_DEBUG_RENDER,
                        "Skipping scanout: inhibited");
          }
        else
          {
            g_warning ("Failed to scan out client buffer: %s", error->message);
          }
     }
}

static void
calc_scanout_region (GList* scanout_list,
                     MtkRegion **scanout_region_out)
{
  MtkRegion *scanout_region = mtk_region_create ();

  for (GList *l = scanout_list; l; l = l->next)
    {
      CoglScanout *scanout = l->data;
      MtkRectangle scanout_rect;
      /*
      CoglScanoutBuffer* scanout_buffer = cogl_scanout_get_buffer(scanout);
      if(scanout_buffer)
      {
        MetaDrmBuffer* buffer = META_DRM_BUFFER (cogl_scanout_get_buffer(scanout));
        meta_topic (META_DEBUG_MOHAN, "Scanout buffer while calc region: %u", buffer ? meta_drm_buffer_get_fb_id(buffer) : 0);
      }*/
      cogl_scanout_get_dst_rect (scanout, &scanout_rect);
      mtk_region_union_rectangle (scanout_region, &scanout_rect);
    }
  
  *scanout_region_out = scanout_region;
}

static void
probe_region (ClutterStageView *stage_view, GList *scanout_list)
{
  MtkRegion *scanout_region = NULL;

  calc_scanout_region (scanout_list, &scanout_region);
  const MtkRegion *redraw_clip = clutter_stage_view_peek_redraw_clip (stage_view);
  const MtkRegion *scanout_clip = clutter_stage_view_peek_scanout_clip (stage_view);

  int num_scanout_rects = mtk_region_num_rectangles (scanout_region);
  int num_redraw_rects = mtk_region_num_rectangles (redraw_clip);
  int num_scanout_clip = mtk_region_num_rectangles (scanout_clip);
  int iter;

  meta_topic (META_DEBUG_MOHAN, "***** Probe Scanout & Redraw Region Starts *****");
  meta_topic (META_DEBUG_MOHAN, "scanout candidates: %d", g_list_length(scanout_list));
  meta_topic (META_DEBUG_MOHAN, "scanout_region: rects: %d", num_scanout_rects);
  meta_topic (META_DEBUG_MOHAN, "scanout_clip: rects: %d", num_scanout_clip);

  for(iter = 0; iter < num_scanout_rects; iter++)
  {
    MtkRectangle rect = mtk_region_get_rectangle (scanout_region, iter);
    meta_topic (META_DEBUG_MOHAN, "scanout_rect: %d,%d %dx%d", rect.x, rect.y, rect.width, rect.height);
  }

  meta_topic (META_DEBUG_MOHAN, "redraw_clip region: rects: %d has_redraw_clip: %d", num_redraw_rects, clutter_stage_view_has_redraw_clip (stage_view));
  for(iter = 0; iter < num_redraw_rects; iter++)
  {
    MtkRectangle rect = mtk_region_get_rectangle (redraw_clip, iter);
    meta_topic (META_DEBUG_MOHAN, "redraw_rect: %d,%d %dx%d", rect.x, rect.y, rect.width, rect.height);
  }

  meta_topic (META_DEBUG_MOHAN, "scanout_clip: rects: %d has_scanout_clip: %d", num_scanout_clip, clutter_stage_view_has_scanout_clip (stage_view));
  for(iter = 0; iter < num_scanout_clip; iter++)
  {
    MtkRectangle rect = mtk_region_get_rectangle (scanout_clip, iter);
    meta_topic (META_DEBUG_MOHAN, "scanout_rect: %d,%d %dx%d", rect.x, rect.y, rect.width, rect.height);
  }

  meta_topic (META_DEBUG_MOHAN, "***** Probe Scanout & Redraw Region Ends *****");
}

static gboolean
go_for_scanout ( const MtkRegion* scanout_region, const MtkRectangle* redraw_rect)
{
  int num_rects = mtk_region_num_rectangles (scanout_region);

  for (int i = 0; i < num_rects; i++)
    {
      MtkRectangle scanout_rect = mtk_region_get_rectangle (scanout_region, i);

      if ( mtk_rectangle_contains_rect (&scanout_rect, redraw_rect) )
        return TRUE;
    }
  return FALSE;
}

static void
meta_stage_impl_redraw_view (ClutterStageWindow *stage_window,
                             ClutterStageView   *stage_view,
                             ClutterFrame       *frame)
{
  // meta_topic (META_DEBUG_MOHAN, "meta_stage_impl_redraw_view: stage_view");
  MetaStageImpl *stage_impl = META_STAGE_IMPL (stage_window);
  GList *scanout_list = NULL;

  scanout_list = clutter_stage_view_take_scanout (stage_view);

  gboolean enable_primary_refresh = FALSE;
  gboolean enable_scanout = FALSE;

  MtkRegion *scanout_region;

  probe_region (stage_view, scanout_list);
  if (scanout_list)
    {
      enable_scanout = true;
      enable_primary_refresh = clutter_stage_view_has_redraw_clip (stage_view);
    }
  else
    {
      enable_primary_refresh = TRUE;
    }
  meta_stage_impl_view (stage_impl, stage_view, frame, scanout_list, enable_primary_refresh, enable_scanout);
}

void
meta_stage_impl_add_onscreen_frame_info (MetaStageImpl    *stage_impl,
                                         ClutterStageView *stage_view,
                                         ClutterFrame     *frame)
{
  MetaStageImplPrivate *priv =
    meta_stage_impl_get_instance_private (stage_impl);
  CoglFramebuffer *framebuffer = clutter_stage_view_get_onscreen (stage_view);
  CoglContext *cogl_context = cogl_framebuffer_get_context (framebuffer);
  CoglFrameInfo *frame_info;

  frame_info = cogl_frame_info_new (cogl_context, priv->global_frame_counter,
                                    frame->frame_count);
  priv->global_frame_counter++;

  cogl_onscreen_add_frame_info (COGL_ONSCREEN (framebuffer), frame_info);
}

static void
meta_stage_impl_set_property (GObject      *gobject,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  MetaStageImpl *self = META_STAGE_IMPL (gobject);
  MetaStageImplPrivate *priv = meta_stage_impl_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_WRAPPER:
      self->wrapper = g_value_get_object (value);
      break;

    case PROP_BACKEND:
      priv->backend = g_value_get_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (gobject, prop_id, pspec);
      break;
    }
}

static void
meta_stage_impl_class_init (MetaStageImplClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterStageWindowClass *window_class = CLUTTER_STAGE_WINDOW_CLASS (klass);

  object_class->set_property = meta_stage_impl_set_property;

  window_class->realize = meta_stage_impl_realize;
  window_class->unrealize = meta_stage_impl_unrealize;
  window_class->resize = meta_stage_impl_resize;
  window_class->show = meta_stage_impl_show;
  window_class->hide = meta_stage_impl_hide;
  window_class->get_frame_counter = meta_stage_impl_get_frame_counter;
  window_class->redraw_view = meta_stage_impl_redraw_view;

  obj_props[PROP_WRAPPER] =
    g_param_spec_object ("wrapper", NULL, NULL,
                         CLUTTER_TYPE_STAGE,
                         G_PARAM_WRITABLE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);
  obj_props[PROP_BACKEND] =
    g_param_spec_object ("backend", NULL, NULL,
                         META_TYPE_BACKEND,
                         G_PARAM_WRITABLE |
                         G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, N_PROPS, obj_props);
}

static void
meta_stage_impl_init (MetaStageImpl *stage)
{
}

MetaBackend *
meta_stage_impl_get_backend (MetaStageImpl *stage_impl)
{
  MetaStageImplPrivate *priv =
    meta_stage_impl_get_instance_private (stage_impl);

  return priv->backend;
}

void
meta_stage_impl_rebuild_views (MetaStageImpl *stage_impl)
{
  MetaBackend *backend = meta_stage_impl_get_backend (stage_impl);
  MetaRenderer *renderer = meta_backend_get_renderer (backend);
  ClutterActor *stage = meta_backend_get_stage (backend);

  meta_renderer_rebuild_views (renderer);
  clutter_stage_clear_stage_views (CLUTTER_STAGE (stage));
}
