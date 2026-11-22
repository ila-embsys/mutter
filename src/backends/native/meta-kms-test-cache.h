#pragma once

#include <glib-object.h>
#include <xf86drmMode.h>

#include "backends/native/meta-kms-types.h"
#include "core/util-private.h"

#define META_TYPE_KMS_TEST_CACHE meta_kms_test_cache_get_type ()

G_DECLARE_FINAL_TYPE (MetaKmsTestCache, meta_kms_test_cache,
                      META, KMS_TEST_CACHE, GObject)

MetaKmsTestCache *meta_kms_test_cache_new ();

GBytes * meta_kms_test_cache_build_key (MetaKmsTestCache      *kms_test_cache,
                                        const drmModeModeInfo *drm_mode,
                                        guint                  cursor_assignments,
                                        guint                  overlay_assignments,
                                        guint                  primary_assignments);

gboolean meta_kms_test_cache_should_skip_atomic_check (MetaKmsTestCache  *kms_test_cache,
                                                       GBytes            *key);

void meta_kms_test_cache_record_failure (MetaKmsTestCache *kms_test_cache,
                                         GBytes           *key);

void meta_kms_test_cache_invalidate (MetaKmsTestCache *kms_test_cache);
