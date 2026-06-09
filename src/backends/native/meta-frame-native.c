/*
 * Copyright (C) 2022 Red Hat
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

#include "backends/native/meta-frame-native.h"

#include "backends/native/meta-kms-update.h"
#include "clutter/clutter-mutter.h"
#include "core/util-private.h"

#include <glib/gstdio.h>

struct _MetaFrameNative
{
  ClutterFrame base;

  MetaDrmBuffer *buffer;
  CoglScanout *scanout;
  GList* scanout_list;

  MetaKmsUpdate *kms_update;

  MtkRegion *damage;

  GPollFD sync;
};

static void
meta_frame_native_release (ClutterFrame *frame)
{
  MetaFrameNative *frame_native = meta_frame_native_from_frame (frame);
  // meta_topic (META_DEBUG_MOHAN, "meta_frame_native_release: frame=%p Scanout_list:%p", frame_native, frame_native->scanout_list);

  g_clear_fd (&frame_native->sync.fd, NULL);
  g_clear_pointer (&frame_native->damage, mtk_region_unref);
  g_clear_object (&frame_native->buffer);
  g_clear_object (&frame_native->scanout);
  g_list_free_full (frame_native->scanout_list, g_object_unref);
  //g_clear_object (&frame_native->scanout_list);

  g_return_if_fail (!frame_native->kms_update);
}

MetaFrameNative *
meta_frame_native_new (void)
{
  MetaFrameNative *frame_native =
    clutter_frame_new (MetaFrameNative, meta_frame_native_release);

  // meta_topic (META_DEBUG_MOHAN, "meta_frame_native_new: frame_native=%p", frame_native);
  frame_native->sync.fd = -1;
  frame_native->sync.events = 0;

  return frame_native;
}

MetaFrameNative *
meta_frame_native_from_frame (ClutterFrame *frame)
{
  return META_CONTAINER_OF (frame, MetaFrameNative, base);
}

MetaKmsUpdate *
meta_frame_native_ensure_kms_update (MetaFrameNative *frame_native,
                                     MetaKmsDevice   *kms_device)
{
  if (frame_native->kms_update)
    {
      g_warn_if_fail (meta_kms_update_get_device (frame_native->kms_update) ==
                      kms_device);
      return frame_native->kms_update;
    }

  frame_native->kms_update = meta_kms_update_new (kms_device);
  return frame_native->kms_update;
}

MetaKmsUpdate *
meta_frame_native_steal_kms_update (MetaFrameNative *frame_native)
{
  return g_steal_pointer (&frame_native->kms_update);
}

gboolean
meta_frame_native_has_kms_update (MetaFrameNative *frame_native)
{
  return !!frame_native->kms_update;
}

void
meta_frame_native_set_buffer (MetaFrameNative *frame_native,
                              MetaDrmBuffer   *buffer)
{
  g_set_object (&frame_native->buffer, buffer);
}

MetaDrmBuffer *
meta_frame_native_get_buffer (MetaFrameNative *frame_native)
{
#if 1
  if(frame_native->scanout_list)
  {
    CoglScanout* scanout = frame_native->scanout_list->data;
    MetaDrmBuffer* buffer = META_DRM_BUFFER (cogl_scanout_get_buffer(scanout));
    // meta_topic (META_DEBUG_MOHAN, "meta_frame_native_get_buffer: Scanout_list=%p, buffer=%p", frame_native->scanout_list, buffer);
    return buffer;
  }
  else
  {
    // meta_topic (META_DEBUG_MOHAN, "meta_frame_native_get_buffer: primary buffer=%p", frame_native->buffer);
    return frame_native->buffer;
  }
#else
  return frame_native->buffer;
#endif
}

MetaDrmBuffer *
meta_frame_native_get_primary_buffer (MetaFrameNative *frame_native)
{
  return frame_native->buffer;
}

void
meta_frame_native_set_scanout (MetaFrameNative *frame_native,
                               GList     *scanout_list)
{
  // g_set_object (&frame_native->scanout, scanout);
  //g_set_object (&frame_native->scanout_list, scanout_list);
  // frame_native->scanout_list = g_steal_pointer (&scanout_list);
  frame_native->scanout_list = scanout_list;
  // meta_topic (META_DEBUG_MOHAN, "meta_frame_native_set_scanout: frame_native:%p scanout_list=%p", frame_native, frame_native->scanout_list);
  //meta_topic (META_DEBUG_MOHAN, "meta_frame_native_set_scanout done");
}

GList*
meta_frame_native_get_scanout (MetaFrameNative *frame_native)
{
  return frame_native->scanout_list;
}

void
meta_frame_native_set_damage (MetaFrameNative *frame_native,
                              const MtkRegion *damage)
{
  g_clear_pointer (&frame_native->damage, mtk_region_unref);
  frame_native->damage = mtk_region_copy (damage);
}

MtkRegion *
meta_frame_native_get_damage (MetaFrameNative *frame_native)
{
  return frame_native->damage;
}

void
meta_frame_native_set_sync_fd (MetaFrameNative *frame_native,
                               int              sync_fd)
{
  g_clear_fd (&frame_native->sync.fd, NULL);
  frame_native->sync.fd = sync_fd;
}

int
meta_frame_native_steal_sync_fd (MetaFrameNative *frame_native)
{
  g_warn_if_fail (frame_native->sync.events == 0);

  return g_steal_fd (&frame_native->sync.fd);
}

void
meta_frame_native_add_source (MetaFrameNative *frame_native,
                              GSource         *source)
{
  g_return_if_fail (frame_native->sync.fd >= 0);
  g_return_if_fail (frame_native->sync.events == 0);

  frame_native->sync.events = G_IO_IN;
  g_source_add_poll (source, &frame_native->sync);
}

void
meta_frame_native_remove_source (MetaFrameNative *frame_native,
                                 GSource         *source)
{
  g_return_if_fail (frame_native->sync.fd >= 0);
  g_return_if_fail (frame_native->sync.events != 0);

  g_source_remove_poll (source, &frame_native->sync);
  frame_native->sync.events = 0;
}

gboolean
meta_frame_native_is_ready (MetaFrameNative *frame_native)
{
  g_return_val_if_fail (frame_native->sync.fd >= 0, FALSE);
  g_return_val_if_fail (frame_native->sync.events != 0, FALSE);

  return !!(frame_native->sync.revents & G_IO_IN);
}
