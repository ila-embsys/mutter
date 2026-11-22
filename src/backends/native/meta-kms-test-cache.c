#include "backends/native/meta-kms-test-cache.h"

#include "backends/native/meta-kms-update-private.h"
#include "backends/native/meta-kms-crtc.h"
#include "backends/native/meta-kms-plane.h"
#include "backends/native/meta-crtc-kms.h"
#include "backends/native/meta-crtc-mode-kms.h"
#include "backends/native/meta-kms-mode.h"


struct _MetaKmsTestCache
{
  GObject parent;

  GHashTable *test_failures;
  guint generation;
};

G_DEFINE_TYPE (MetaKmsTestCache, meta_kms_test_cache, G_TYPE_OBJECT)

typedef struct _KmsTestKey
{
  guint32 pixel_clock_khz;
  guint16 hdisplay, vdisplay;
  guint16 htotal, vtotal;
  guint8 cursor_plane_assignments;
  guint8 overlay_plane_assignments;
  guint8 primary_plane_assignments;
  guint8 padding[1]; // Explicit 16th byte to be sure it doesn't come in the way for hashing
} KmsTestKey;

G_STATIC_ASSERT (sizeof (KmsTestKey) == 16);

typedef struct _KmsTestFailEntry
{
  gint64 last_fail_us;
  guint fail_count;
  guint generation;
} KmsTestFailEntry;

MetaKmsTestCache *
meta_kms_test_cache_new ()
{
  MetaKmsTestCache *kms_test_cache;

  kms_test_cache = g_object_new (META_TYPE_KMS_TEST_CACHE, NULL);

  return kms_test_cache;
}

GBytes *
meta_kms_test_cache_build_key (MetaKmsTestCache      *kms_test_cache,
                               const drmModeModeInfo *drm_mode,
                               guint                  cursor_assignments,
                               guint                  overlay_assignments,
                               guint                  primary_assignments)
{
  KmsTestKey *key;

  g_return_val_if_fail (drm_mode != NULL, NULL);
  g_return_val_if_fail (cursor_assignments <= G_MAXUINT8, NULL);
  g_return_val_if_fail (overlay_assignments <= G_MAXUINT8, NULL);
  g_return_val_if_fail (primary_assignments <= G_MAXUINT8, NULL);

  key = g_new (KmsTestKey, 1);

  key->pixel_clock_khz = drm_mode->clock;
  key->hdisplay = drm_mode->hdisplay;
  key->vdisplay = drm_mode->vdisplay;
  key->htotal = drm_mode->htotal;
  key->vtotal = drm_mode->vtotal;
  key->cursor_plane_assignments = cursor_assignments;
  key->overlay_plane_assignments = overlay_assignments;
  key->primary_plane_assignments = primary_assignments;

  return g_bytes_new_take (key, sizeof (KmsTestKey));
}

gboolean
meta_kms_test_cache_should_skip_atomic_check (MetaKmsTestCache  *kms_test_cache,
                                              GBytes            *key)
{
  KmsTestFailEntry *entry;

  if (meta_get_debug_paint_flags () & META_DEBUG_PAINT_DISABLE_SCANOUT_CACHE) // TODO Remove this Before Raising MR
    return FALSE;

  g_return_val_if_fail (kms_test_cache != NULL, FALSE);
  g_return_val_if_fail (key != NULL, FALSE);

  entry = g_hash_table_lookup (kms_test_cache->test_failures, key);

  if (entry != NULL && entry->generation != kms_test_cache->generation)
    return FALSE;
  else if (entry == NULL)
    return FALSE;

  // Maybe check if the failure entry is recently done ?

  return TRUE;
}

void
meta_kms_test_cache_record_failure (MetaKmsTestCache *kms_test_cache,
                                    GBytes           *key)
{
  KmsTestFailEntry *entry;

  if (meta_get_debug_paint_flags () & META_DEBUG_PAINT_DISABLE_SCANOUT_CACHE) // TODO Remove this Before Raising MR
    return;

  g_return_if_fail (kms_test_cache != NULL);
  g_return_if_fail (key != NULL);

  entry = g_hash_table_lookup (kms_test_cache->test_failures, key);

  if (entry != NULL && entry->generation == kms_test_cache->generation)
    {
      entry->fail_count++;
      entry->last_fail_us = g_get_monotonic_time ();
      return;
    }

  entry = g_new0 (KmsTestFailEntry, 1);
  entry->fail_count = 1;
  entry->generation = kms_test_cache->generation;
  entry->last_fail_us = g_get_monotonic_time ();

  g_hash_table_replace (kms_test_cache->test_failures, g_bytes_ref (key), entry);
}

static gboolean
is_cache_entry_stale (gpointer key,
                      gpointer value,
                      gpointer user_data)
{
  MetaKmsTestCache *kms_test_cache = user_data;
  KmsTestFailEntry *entry = value;

  if (entry->generation != kms_test_cache->generation)
    return TRUE;

  // Maybe check the age till last fail ?
  return FALSE;
}

void
meta_kms_test_cache_invalidate (MetaKmsTestCache *kms_test_cache)
{
  g_return_if_fail (kms_test_cache != NULL);

  kms_test_cache->generation++;
  g_hash_table_foreach_remove (kms_test_cache->test_failures, is_cache_entry_stale, kms_test_cache);
}

static void
meta_kms_test_cache_finalize (GObject *object)
{
  MetaKmsTestCache *test_cache = META_KMS_TEST_CACHE (object);

  g_clear_pointer (&test_cache->test_failures, g_hash_table_unref);

  G_OBJECT_CLASS (meta_kms_test_cache_parent_class)->finalize (object);
}

static void
meta_kms_test_cache_init (MetaKmsTestCache *test_cache)
{
  test_cache->test_failures = g_hash_table_new_full (g_bytes_hash,
                                                     g_bytes_equal,
                                                     (GDestroyNotify) g_bytes_unref,
                                                     g_free);
  test_cache->generation = 0;
}

static void
meta_kms_test_cache_class_init (MetaKmsTestCacheClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = meta_kms_test_cache_finalize;
}
