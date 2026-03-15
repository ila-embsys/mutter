#include "meta-test/meta-context-test.h"
#include "tests/meta-test-utils.h"
#include "tests/meta-wayland-test-driver.h"
#include "tests/meta-wayland-test-utils.h"
#include "wayland/meta-window-wayland.h"
#include "backends/native/meta-kms.h"
#include "backends/native/meta-kms-device.h"
#include "compositor/meta-window-actor-private.h"
#include "core/window-private.h"
#include "meta-test/meta-context-test.h"
#include "tests/meta-test-utils.h"
#include "tests/meta-wayland-test-driver.h"
#include "tests/meta-wayland-test-utils.h"
#include "backends/native/meta-renderer-native.h"
#include "wayland/meta-wayland-surface-private.h"

static MetaContext *test_context;
static MetaWaylandTestDriver *test_driver;
static MetaVirtualMonitor *virtual_monitor;

static void
on_first_frame (MetaWindowActor *window_actor,
                gboolean        *done)
{
  *done = TRUE;
}

static void
wait_for_first_frame (MetaWindow *window)
{
  MetaWindowActor *window_actor;
  gboolean done = FALSE;
  glong handler_id;

  window_actor = meta_window_actor_from_window (window);
  handler_id = g_signal_connect (window_actor, "first-frame",
                                 G_CALLBACK (on_first_frame), &done);

  while (!done)
    g_main_context_iteration (NULL, TRUE);

  g_signal_handler_disconnect (window_actor, handler_id);
}

static void
on_effects_completed (MetaWindowActor *window_actor,
                      gboolean        *done)
{
  *done = TRUE;
}

static void
wait_for_window_added (MetaWindow *window)
{
  MetaWindowActor *window_actor;
  gboolean done = FALSE;
  gulong handler_id;

  window_actor = meta_window_actor_from_window (window);
  handler_id = g_signal_connect (window_actor, "effects-completed",
                                 G_CALLBACK (on_effects_completed), &done);

  while (!done)
    g_main_context_iteration (NULL, TRUE);

  g_signal_handler_disconnect (window_actor, handler_id);
}

static MetaWaylandTestClient* create_fullscreen_client (void)
{
    MetaWaylandTestClient *test_client;
    MetaWindow *window;
    MetaWindowWayland *wl_window;
    MtkRectangle rect;

    test_client = meta_wayland_test_client_new (test_context, "fullscreen" );

    while(!(window = meta_find_window_from_title (test_context, "fullscreen")))
      g_main_context_iteration (NULL, TRUE);

    wait_for_window_added (window);

    meta_window_make_fullscreen (window);

    wl_window = META_WINDOW_WAYLAND (window);

    while (!meta_window_wayland_is_acked_fullscreen (wl_window))
      g_main_context_iteration (NULL, FALSE);

    meta_window_get_frame_rect (window, &rect);
    g_assert_cmpint (rect.width, ==, 1920);
    g_assert_cmpint (rect.height, ==, 1080);
    g_assert_cmpint (rect.x, ==, 0);
    g_assert_cmpint (rect.y, ==, 0);

    return test_client;
}

static MetaWaylandTestClient* create_keyboard_client (void)
{
    MetaWaylandTestClient *test_client;
    MetaWindow *window;

    test_client = meta_wayland_test_client_new (test_context, "keyboard" );

    while(!(window = meta_find_window_from_title (test_context, "keyboard")))
      g_main_context_iteration (NULL, TRUE);

    wait_for_window_added (window);

    return test_client;
}

meta_wait_for asdda

static void 
meta_test_kms_overlay_candidates_accepts (void)
{
    MetaBackend *backend = meta_context_get_backend (test_context);

    MetaWaylandTestClient *fullscreen_client;
    MetaWaylandTestClient *keyboard_client;
    //fullscreen_client = create_fullscreen_client();
    keyboard_client = create_keyboard_client();

//    meta_wayland_test_driver_emit_sync_event (test_driver, 0);
//    meta_wayland_test_client_finish (fullscreen_client);

    meta_wayland_test_driver_emit_sync_event (test_driver, 0);
    meta_wayland_test_client_finish (keyboard_client);
}

static void
on_before_tests (void)
{
    MetaWaylandCompositor *compositor =
        meta_context_get_wayland_compositor (test_context);

    test_driver = meta_wayland_test_driver_new (compositor);
    virtual_monitor = meta_create_test_monitor (test_context,
                                              1920, 1080, 60.0);
}

static void
on_after_tests (void)
{
    g_clear_object (&test_driver);
    g_clear_object (&virtual_monitor);
}

static void
init_tests (void)
{
    g_test_add_func ("/overlays/candidates/accepts",
                     meta_test_kms_overlay_candidates_accepts);
}

int
main (int    argc,
      char **argv)
{
  g_autoptr (MetaContext) context = NULL;

#if 0
  context = meta_create_test_context (META_CONTEXT_TEST_TYPE_VKMS,
                                      META_CONTEXT_TEST_FLAG_NO_X11 |
                                      META_CONTEXT_TEST_FLAG_TEST_CLIENT);
#else
    context = meta_create_test_context (META_CONTEXT_TEST_TYPE_HEADLESS,
                                        META_CONTEXT_TEST_FLAG_NO_X11 |
                                        META_CONTEXT_TEST_FLAG_TEST_CLIENT);
#endif
  g_assert_true (meta_context_configure (context, &argc, &argv, NULL));

  test_context = context;

  init_tests ();

  g_signal_connect (context, "before-tests",
                    G_CALLBACK (on_before_tests), NULL);
  g_signal_connect (context, "after-tests",
                    G_CALLBACK (on_after_tests), NULL);

  return meta_context_test_run_tests (META_CONTEXT_TEST (context),
                                      META_TEST_RUN_FLAG_CAN_SKIP);
}