/*
 * Copyright (C) 2026 Red Hat, Inc.
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
 */

#include "config.h"

#include <drm_fourcc.h>

#include "backends/native/meta-scanout-feedback.h"

struct _MetaScanoutFeedback
{
  GPtrArray *format_sets; /* GHashTable (uint32_t format -> GArray of uint64_t modifiers) */
};

MetaScanoutFeedback *
meta_scanout_feedback_new (void)
{
  g_autoptr (MetaScanoutFeedback) feedback = NULL;

  feedback = g_new0 (MetaScanoutFeedback, 1);
  feedback->format_sets =
    g_ptr_array_new_with_free_func ((GDestroyNotify) g_hash_table_unref);

  return g_steal_pointer (&feedback);
}

void
meta_scanout_feedback_free (MetaScanoutFeedback *feedback)
{
  if (!feedback)
    return;

  g_clear_pointer (&feedback->format_sets, g_ptr_array_unref);
  g_free (feedback);
}

void
meta_scanout_feedback_add_format_set (MetaScanoutFeedback *feedback,
                                      GHashTable          *formats)
{
  g_ptr_array_add (feedback->format_sets, formats);
}

size_t
meta_scanout_feedback_get_n_format_sets (MetaScanoutFeedback *feedback)
{
  return feedback->format_sets->len;
}

gboolean
meta_scanout_feedback_format_set_supports (MetaScanoutFeedback *feedback,
                                           size_t               set_index,
                                           uint32_t             drm_format,
                                           uint64_t             drm_modifier)
{
  GHashTable *formats;
  GArray *modifiers;

  g_return_val_if_fail (set_index < feedback->format_sets->len, FALSE);

  formats = g_ptr_array_index (feedback->format_sets, set_index);

  if (!g_hash_table_contains (formats, GUINT_TO_POINTER (drm_format)))
    return FALSE;

  if (drm_modifier == DRM_FORMAT_MOD_INVALID)
    return TRUE;

  modifiers = g_hash_table_lookup (formats, GUINT_TO_POINTER (drm_format));
  if (!modifiers || modifiers->len == 0)
    return FALSE;

  for (size_t i = 0; i < modifiers->len; i++)
    {
      if (g_array_index (modifiers, uint64_t, i) == drm_modifier)
        return TRUE;
    }

  return FALSE;
}
