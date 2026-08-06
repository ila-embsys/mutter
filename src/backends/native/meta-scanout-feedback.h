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

#pragma once

#include <glib-object.h>
#include <stdint.h>

typedef struct _MetaScanoutFeedback MetaScanoutFeedback;

MetaScanoutFeedback * meta_scanout_feedback_new (void);
void meta_scanout_feedback_free (MetaScanoutFeedback *feedback);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (MetaScanoutFeedback, meta_scanout_feedback_free)

void meta_scanout_feedback_add_format_set (MetaScanoutFeedback *feedback,
                                           GHashTable          *formats);

size_t meta_scanout_feedback_get_n_format_sets (MetaScanoutFeedback *feedback);

gboolean meta_scanout_feedback_format_set_supports (MetaScanoutFeedback *feedback,
                                                    size_t               set_index,
                                                    uint32_t             drm_format,
                                                    uint64_t             drm_modifier);
