/*
 * Copyright (C) 2026 Red Hat Inc.
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
 *
 */

#include "config.h"

#include "backends/native/meta-backend-native-private.h"
#include "backends/native/meta-crtc-kms.h"
#include "backends/native/meta-device-pool.h"
#include "backends/native/meta-drm-buffer.h"
#include "backends/native/meta-frame-native.h"
#include "backends/native/meta-kms-device.h"
#include "backends/native/meta-kms-device-private.h"
#include "backends/native/meta-kms-update-private.h"
#include "backends/native/meta-kms.h"
#include "backends/native/meta-onscreen-native.h"
#include "clutter/clutter-frame-private.h"
#include "clutter/clutter-offloadable.h"
#include "meta-test/meta-context-test.h"
#include "tests/meta-test-utils.h"
#include "tests/meta-wayland-test-driver.h"
#include "tests/meta-wayland-test-utils.h"
#include "wayland/meta-window-wayland.h"

static MetaContext *test_context;

static const MetaVkmsDeviceConfig overlay_device_config = {
  .name = "mutter-multi-plane-test",
  .crtcs = {
    { .name = "crtc0", .writeback = TRUE },
  },
  .planes = {
    {
      .name = "primary",
      .type = META_KMS_PLANE_TYPE_PRIMARY,
      .possible_crtcs = { "crtc0" },
    },
    {
      .name = "cursor",
      .type = META_KMS_PLANE_TYPE_CURSOR,
      .possible_crtcs = { "crtc0" },
    },
    {
      .name = "overlay0",
      .type = META_KMS_PLANE_TYPE_OVERLAY,
      .possible_crtcs = { "crtc0" },
    },
    {
      .name = "overlay1",
      .type = META_KMS_PLANE_TYPE_OVERLAY,
      .possible_crtcs = { "crtc0" },
    },
  },
  .encoders = {
    { .name = "encoder0", .possible_crtcs = { "crtc0" } },
  },
  .connectors = {
    { .name = "connector0", .possible_encoders = { "encoder0" } },
  },
};

typedef struct _ScanoutTest
{
  struct {
    gboolean has_primary;
    gboolean presented;
    uint32_t primary_fb_id;
    uint32_t crtc_fb_id;
    size_t n_overlay_assignments;
  } result;
} ScanoutTest;

static void
on_before_update (ClutterStage     *stage,
                  ClutterStageView *stage_view,
                  ClutterFrame     *frame,
                  ScanoutTest      *test)
{
  test->result = (typeof (test->result)) { 0 };
}

static void
on_before_paint (ClutterStage     *stage,
                 ClutterStageView *stage_view,
                 ClutterFrame     *frame,
                 ScanoutTest      *test)
{
  ClutterOffloadableOffloaded *offloaded;
  MetaFrameNative *frame_native;
  MetaRendererView *view;
  MetaCrtc *crtc;
  MetaKmsCrtc *kms_crtc;
  MetaKmsDevice *kms_device;
  MetaKmsUpdate *kms_update;
  MetaKmsPlaneAssignment *plane_assignment;

  offloaded = clutter_frame_get_offloaded (frame);

  test->result.has_primary =
    offloaded &&
    clutter_offloadable_offloaded_get_primary_actor (offloaded) != NULL;

  frame_native = meta_frame_native_from_frame (frame);
  view = META_RENDERER_VIEW (stage_view);
  crtc = meta_renderer_view_get_crtc (view);
  kms_crtc = meta_crtc_kms_get_kms_crtc (META_CRTC_KMS (crtc));
  kms_device = meta_kms_crtc_get_device (kms_crtc);
  kms_update = meta_frame_native_ensure_kms_update (frame_native, kms_device);

  plane_assignment =
    meta_kms_update_get_primary_plane_assignment (kms_update, kms_crtc);
  if (plane_assignment && plane_assignment->buffer)
    {
      test->result.primary_fb_id =
        meta_drm_buffer_get_fb_id (plane_assignment->buffer);
    }

  test->result.n_overlay_assignments = 0;
  for (GList *l = meta_kms_update_get_plane_assignments (kms_update);
       l; l = l->next)
    {
      MetaKmsPlaneAssignment *assignment = l->data;

      if (meta_kms_plane_get_plane_type (assignment->plane) !=
          META_KMS_PLANE_TYPE_OVERLAY)
        continue;
      if (!assignment->buffer)
        continue;

      test->result.n_overlay_assignments++;
    }
}

static void
on_presented (ClutterStage     *stage,
              ClutterStageView *stage_view,
              ClutterFrameInfo *frame_info,
              ScanoutTest      *test)
{
  MetaBackend *backend = meta_context_get_backend (test_context);
  MetaBackendNative *backend_native = META_BACKEND_NATIVE (backend);
  MetaDevicePool *device_pool;
  CoglFramebuffer *fb;
  MetaCrtc *crtc;
  MetaKmsCrtc *kms_crtc;
  MetaKmsDevice *kms_device;
  MetaDeviceFile *device_file;
  GError *error = NULL;
  drmModeCrtc *drm_crtc;

  device_pool = meta_backend_native_get_device_pool (backend_native);

  fb = clutter_stage_view_get_onscreen (stage_view);
  crtc = meta_onscreen_native_get_crtc (META_ONSCREEN_NATIVE (fb));
  kms_crtc = meta_crtc_kms_get_kms_crtc (META_CRTC_KMS (crtc));
  kms_device = meta_kms_crtc_get_device (kms_crtc);

  device_file = meta_device_pool_open (device_pool,
                                       meta_kms_device_get_path (kms_device),
                                       META_DEVICE_FILE_FLAG_TAKE_CONTROL,
                                       &error);
  if (!device_file)
    g_error ("Failed to open KMS device: %s", error->message);

  drm_crtc = drmModeGetCrtc (meta_device_file_get_fd (device_file),
                             meta_kms_crtc_get_id (kms_crtc));
  g_assert_nonnull (drm_crtc);
  test->result.crtc_fb_id = drm_crtc->buffer_id;
  drmModeFreeCrtc (drm_crtc);

  meta_device_file_release (device_file);

  test->result.presented = TRUE;
}

static void
connect_scanout_signals (ClutterStage *stage,
                         ScanoutTest  *test,
                         gulong       *before_update_id,
                         gulong       *before_paint_id,
                         gulong       *presented_id)
{
  *before_update_id =
    g_signal_connect (stage, "before-update",
                      G_CALLBACK (on_before_update), test);
  *before_paint_id =
    g_signal_connect (stage, "before-paint",
                      G_CALLBACK (on_before_paint), test);
  *presented_id =
    g_signal_connect (stage, "presented",
                      G_CALLBACK (on_presented), test);
}

static void
disconnect_scanout_signals (ClutterStage *stage,
                            gulong        before_update_id,
                            gulong        before_paint_id,
                            gulong        presented_id)
{
  g_signal_handler_disconnect (stage, before_update_id);
  g_signal_handler_disconnect (stage, before_paint_id);
  g_signal_handler_disconnect (stage, presented_id);
}

/*
 * Test that a fullscreen DMA-BUF client gets primary plane scanout
 * on a device that has overlay planes available. The presence of
 * overlay planes should not prevent primary scanout.
 */
static void
meta_test_multi_plane_primary_scanout (void)
{
  MetaBackend *backend = meta_context_get_backend (test_context);
  MetaWaylandCompositor *wayland_compositor =
    meta_context_get_wayland_compositor (test_context);
  ClutterStage *stage = CLUTTER_STAGE (meta_backend_get_stage (backend));
  MetaKms *kms = meta_backend_native_get_kms (META_BACKEND_NATIVE (backend));
  MetaKmsDevice *kms_device = meta_kms_get_devices (kms)->data;
  ScanoutTest test = { 0 };
  MetaWaylandTestClient *wayland_test_client;
  g_autoptr (MetaWaylandTestDriver) test_driver = NULL;
  MetaWindow *window;
  gulong before_update_id, before_paint_id, presented_id;

  test_driver = meta_wayland_test_driver_new (wayland_compositor);
  meta_wayland_test_driver_set_property (test_driver,
                                         "gpu-path",
                                         meta_kms_device_get_path (kms_device));

  wayland_test_client =
    meta_wayland_test_client_new (test_context, "dma-buf-scanout");
  g_assert_nonnull (wayland_test_client);

  window = meta_wait_for_client_window (test_context,
                                        "dma-buf-scanout-test");

  meta_wait_for_window_shown (window);
  meta_wait_for_effects (window);

  connect_scanout_signals (stage, &test,
                           &before_update_id,
                           &before_paint_id,
                           &presented_id);

  clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));

  while (!test.result.has_primary ||
         test.result.primary_fb_id == 0 ||
         test.result.primary_fb_id != test.result.crtc_fb_id)
    {
      g_main_context_iteration (NULL, TRUE);
      clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));
    }

  g_assert_cmpuint (test.result.primary_fb_id, !=, 0);
  g_assert_cmpuint (test.result.primary_fb_id, ==, test.result.crtc_fb_id);
  g_assert_cmpuint (test.result.n_overlay_assignments, ==, 0);

  disconnect_scanout_signals (stage, before_update_id,
                              before_paint_id, presented_id);

  meta_wayland_test_driver_emit_sync_event (test_driver, 0);
  meta_wayland_test_client_finish (wayland_test_client);
}

static void
meta_test_multi_plane_overlay_scanout (void)
{
  MetaBackend *backend = meta_context_get_backend (test_context);
  MetaWaylandCompositor *wayland_compositor =
    meta_context_get_wayland_compositor (test_context);
  ClutterStage *stage = CLUTTER_STAGE (meta_backend_get_stage (backend));
  MetaKms *kms = meta_backend_native_get_kms (META_BACKEND_NATIVE (backend));
  MetaKmsDevice *kms_device = meta_kms_get_devices (kms)->data;
  ScanoutTest test = { 0 };
  MetaWaylandTestClient *wayland_test_client;
  g_autoptr (MetaWaylandTestDriver) test_driver = NULL;
  MetaWindow *window;
  gulong before_update_id, before_paint_id, presented_id;

  test_driver = meta_wayland_test_driver_new (wayland_compositor);
  meta_wayland_test_driver_set_property (test_driver,
                                         "gpu-path",
                                         meta_kms_device_get_path (kms_device));

  wayland_test_client =
    meta_wayland_test_client_new (test_context, "dma-buf-overlay");
  g_assert_nonnull (wayland_test_client);

  window = meta_wait_for_client_window (test_context,
                                        "dma-buf-overlay-test");

  meta_wait_for_window_shown (window);
  meta_wait_for_effects (window);

  connect_scanout_signals (stage, &test,
                           &before_update_id,
                           &before_paint_id,
                           &presented_id);

  clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));

  while (test.result.n_overlay_assignments == 0)
    {
      g_main_context_iteration (NULL, TRUE);
      clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));
    }

  g_assert_false (test.result.has_primary);
  g_assert_cmpuint (test.result.n_overlay_assignments, >=, 1);

  disconnect_scanout_signals (stage, before_update_id,
                              before_paint_id, presented_id);

  meta_wayland_test_driver_emit_sync_event (test_driver, 0);
  meta_wayland_test_client_finish (wayland_test_client);
}

/*
 * Test that an ARGB8888 surface with a full opaque region IS offloaded,
 * and that removing the opaque region causes it to be rejected. Uses a
 * single client that toggles the opaque region via sync_event.
 */
static void
meta_test_multi_plane_alpha_not_offloaded (void)
{
  MetaBackend *backend = meta_context_get_backend (test_context);
  MetaWaylandCompositor *wayland_compositor =
    meta_context_get_wayland_compositor (test_context);
  ClutterStage *stage = CLUTTER_STAGE (meta_backend_get_stage (backend));
  MetaKms *kms = meta_backend_native_get_kms (META_BACKEND_NATIVE (backend));
  MetaKmsDevice *kms_device = meta_kms_get_devices (kms)->data;
  ScanoutTest test = { 0 };
  MetaWaylandTestClient *wayland_test_client;
  g_autoptr (MetaWaylandTestDriver) test_driver = NULL;
  MetaWindow *window;
  gulong before_update_id, before_paint_id, presented_id;

  test_driver = meta_wayland_test_driver_new (wayland_compositor);
  meta_wayland_test_driver_set_property (test_driver,
                                         "gpu-path",
                                         meta_kms_device_get_path (kms_device));

  wayland_test_client =
    meta_wayland_test_client_new (test_context, "dma-buf-overlay-alpha");
  g_assert_nonnull (wayland_test_client);

  window = meta_wait_for_client_window (test_context,
                                        "dma-buf-overlay-alpha-test");

  meta_wait_for_window_shown (window);
  meta_wait_for_effects (window);

  connect_scanout_signals (stage, &test,
                           &before_update_id,
                           &before_paint_id,
                           &presented_id);

  clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));

  while (test.result.n_overlay_assignments == 0)
    {
      g_main_context_iteration (NULL, TRUE);
      clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));
    }

  g_assert_cmpuint (test.result.n_overlay_assignments, >=, 1);

  meta_wayland_test_driver_emit_sync_event (test_driver, 1);
  meta_wayland_test_driver_wait_for_sync_point (test_driver, 1);

  test.result.presented = FALSE;

  clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));

  while (!test.result.presented ||
         test.result.n_overlay_assignments != 0)
    {
      g_main_context_iteration (NULL, TRUE);
      clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));
    }

  g_assert_cmpuint (test.result.n_overlay_assignments, ==, 0);

  disconnect_scanout_signals (stage, before_update_id,
                              before_paint_id, presented_id);

  meta_wayland_test_driver_emit_sync_event (test_driver, 0);
  meta_wayland_test_client_finish (wayland_test_client);
}

/*
 * Test that two non-overlapping overlay surfaces both get assigned to
 * overlay planes, and that overlapping them causes only one to be
 * assigned.
 */
static void
meta_test_multi_plane_two_overlay_scanout (void)
{
  MetaBackend *backend = meta_context_get_backend (test_context);
  MetaWaylandCompositor *wayland_compositor =
    meta_context_get_wayland_compositor (test_context);
  ClutterStage *stage = CLUTTER_STAGE (meta_backend_get_stage (backend));
  MetaKms *kms = meta_backend_native_get_kms (META_BACKEND_NATIVE (backend));
  MetaKmsDevice *kms_device = meta_kms_get_devices (kms)->data;
  ScanoutTest test = { 0 };
  MetaWaylandTestClient *wayland_test_client;
  g_autoptr (MetaWaylandTestDriver) test_driver = NULL;
  MetaWindow *window0, *window1;
  gulong before_update_id, before_paint_id, presented_id;

  test_driver = meta_wayland_test_driver_new (wayland_compositor);
  meta_wayland_test_driver_set_property (test_driver,
                                         "gpu-path",
                                         meta_kms_device_get_path (kms_device));

  wayland_test_client =
    meta_wayland_test_client_new (test_context, "dma-buf-two-overlays");
  g_assert_nonnull (wayland_test_client);

  window0 = meta_wait_for_client_window (test_context,
                                         "dma-buf-two-overlays-0");
  window1 = meta_wait_for_client_window (test_context,
                                         "dma-buf-two-overlays-1");

  meta_wait_for_window_shown (window0);
  meta_wait_for_window_shown (window1);
  meta_wait_for_effects (window0);
  meta_wait_for_effects (window1);

  meta_window_move_frame (window0, /* user_op = */ TRUE, 0, 0);
  meta_window_move_frame (window1, /* user_op = */ TRUE, 400, 0);

  connect_scanout_signals (stage, &test,
                           &before_update_id,
                           &before_paint_id,
                           &presented_id);

  clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));

  while (test.result.n_overlay_assignments < 2)
    {
      g_main_context_iteration (NULL, TRUE);
      clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));
    }

  g_assert_cmpuint (test.result.n_overlay_assignments, ==, 2);

  meta_window_move_frame (window1, /* user_op = */ TRUE, 50, 50);
  test.result.n_overlay_assignments = 0;

  clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));

  while (test.result.n_overlay_assignments == 0 ||
         test.result.n_overlay_assignments >= 2)
    {
      g_main_context_iteration (NULL, TRUE);
      clutter_actor_queue_redraw (CLUTTER_ACTOR (stage));
    }

  g_assert_cmpuint (test.result.n_overlay_assignments, ==, 1);

  disconnect_scanout_signals (stage, before_update_id,
                              before_paint_id, presented_id);

  meta_wayland_test_driver_emit_sync_event (test_driver, 0);
  meta_wayland_test_client_finish (wayland_test_client);
}

static void
init_tests (void)
{
  g_test_add_func ("/backends/native/kms/multi-plane/primary-scanout",
                   meta_test_multi_plane_primary_scanout);
  g_test_add_func ("/backends/native/kms/multi-plane/overlay-scanout",
                   meta_test_multi_plane_overlay_scanout);
  g_test_add_func ("/backends/native/kms/multi-plane/alpha-not-offloaded",
                   meta_test_multi_plane_alpha_not_offloaded);
  g_test_add_func ("/backends/native/kms/multi-plane/two-overlay-scanout",
                   meta_test_multi_plane_two_overlay_scanout);
}

int
main (int    argc,
      char **argv)
{
  g_autoptr (MetaContext) context = NULL;

  context = test_context =
    meta_create_test_context (META_CONTEXT_TEST_TYPE_VKMS,
                              META_CONTEXT_TEST_FLAG_NO_X11);

  meta_context_test_set_vkms_device_configs (META_CONTEXT_TEST (context),
                                             &overlay_device_config, 1);

  g_assert_true (meta_context_configure (context, &argc, &argv, NULL));

  init_tests ();

  return meta_context_test_run_tests (META_CONTEXT_TEST (context),
                                      META_TEST_RUN_FLAG_CAN_SKIP);
}
