/* pipewire.c
 *
 * Copyright 2020 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "pipewire.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include <fcntl.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/debug/types.h>
#include <spa/param/video/type-info.h>

#define REQUEST_PATH "/org/freedesktop/portal/desktop/request/%s/obs%u"
#define SESSION_PATH "/org/freedesktop/portal/desktop/session/%s/obs%u"

#define CURSOR_META_SIZE(width, height) \
 (sizeof(struct spa_meta_cursor) + \
  sizeof(struct spa_meta_bitmap) + width * height * 4)

struct _obs_pipewire_data
{
  GDBusConnection *connection;
  GCancellable    *cancellable;

  char            *sender_name;
  char            *session_handle;

  uint32_t         pipewire_node;
  int              pipewire_fd;

  obs_source_t    *source;
  obs_data_t      *settings;

  gs_texture_t *texture;

  struct pw_thread_loop *thread_loop;
  struct pw_context *context;

  struct pw_core *core;
  struct spa_hook core_listener;

  struct pw_stream *stream;
  struct spa_hook stream_listener;
  struct spa_video_info format;

  struct pw_buffer *current_pw_buffer;

  struct {
    bool valid;
    int x, y;
    int width, height;
  } crop;

  struct {
    bool visible;
    bool valid;
    int x, y;
    int hotspot_x, hotspot_y;
    int width, height;
    gs_texture_t *texture;
  } cursor;

  obs_pw_capture_type capture_type;
  bool negotiated;
};

/* auxiliary methods */

static void
new_request_path (obs_pipewire_data  *data,
                  char              **out_path,
                  char              **out_token)
{
  static uint32_t request_token_count = 0;

  request_token_count++;

  if (out_token)
    *out_token = g_strdup_printf ("obs%u", request_token_count);

  if (out_path)
    *out_path = g_strdup_printf (REQUEST_PATH, data->sender_name, request_token_count);
}

static void
new_session_path (obs_pipewire_data  *data,
                  char              **out_path,
                  char              **out_token)
{
  static uint32_t session_token_count = 0;

  session_token_count++;

  if (out_token)
    *out_token = g_strdup_printf ("obs%u", session_token_count);

  if (out_path)
    *out_path = g_strdup_printf (SESSION_PATH, data->sender_name, session_token_count);
}

typedef struct
{
  obs_pipewire_data *xdg;
  char              *request_path;
  guint              signal_id;
  gulong             cancelled_id;
} dbus_call_data;

static void
on_cancelled_cb (GCancellable *cancellable,
                 gpointer      data)
{
  dbus_call_data *call = data;

  blog (LOG_INFO, "[OBS XDG] Screencast session cancelled");

  g_dbus_connection_call (call->xdg->connection,
                          "org.freedesktop.portal.Desktop",
                          call->request_path,
                          "org.freedesktop.portal.Request",
                          "Close",
                          NULL,
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          NULL, NULL, NULL);
}

static dbus_call_data*
subscribe_to_signal (obs_pipewire_data   *xdg,
                     const char          *path,
                     GDBusSignalCallback  callback)
{
  dbus_call_data *call;

  call = g_new0 (dbus_call_data, 1);
  call->xdg = xdg;
  call->request_path = g_strdup (path);
  call->cancelled_id = g_signal_connect (xdg->cancellable, "cancelled", G_CALLBACK (on_cancelled_cb), call);
  call->signal_id = g_dbus_connection_signal_subscribe (xdg->connection,
                                                        "org.freedesktop.portal.Desktop",
                                                        "org.freedesktop.portal.Request",
                                                        "Response",
                                                        call->request_path,
                                                        NULL,
                                                        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                                        callback,
                                                        call,
                                                        NULL);

  return call;
}

static void
dbus_call_data_free (dbus_call_data *call)
{
  if (!call)
    return;

  if (call->signal_id)
    g_dbus_connection_signal_unsubscribe (call->xdg->connection, call->signal_id);

  if (call->cancelled_id > 0)
    g_signal_handler_disconnect (call->xdg->cancellable, call->cancelled_id);

  g_clear_pointer (&call->request_path, g_free);
  g_free (call);
}

static void
maybe_queue_buffer (obs_pipewire_data *xdg)
{
  if (xdg->current_pw_buffer)
    {
      pw_stream_queue_buffer (xdg->stream, xdg->current_pw_buffer);
      xdg->current_pw_buffer = NULL;
    }
}

static void
teardown_pipewire (obs_pipewire_data *xdg)
{
  maybe_queue_buffer (xdg);

  if (xdg->thread_loop)
    pw_thread_loop_stop (xdg->thread_loop);

  g_clear_pointer (&xdg->stream, pw_stream_destroy);
  g_clear_pointer (&xdg->thread_loop, pw_thread_loop_destroy);

  xdg->negotiated = false;
}

static void
destroy_session (obs_pipewire_data *xdg)
{
  if (xdg->session_handle)
    {
      g_dbus_connection_call (xdg->connection,
                              "org.freedesktop.portal.Desktop",
                              xdg->session_handle,
                              "org.freedesktop.portal.Session",
                              "Close",
                              NULL,
                              NULL,
                              G_DBUS_CALL_FLAGS_NONE,
                              -1, NULL, NULL, NULL);

      g_clear_pointer (&xdg->session_handle, g_free);
    }

  g_clear_pointer (&xdg->cursor.texture, gs_texture_destroy);
  g_clear_pointer (&xdg->texture, gs_texture_destroy);
  g_cancellable_cancel (xdg->cancellable);
  g_clear_object (&xdg->cancellable);
  g_clear_object (&xdg->connection);
  g_clear_pointer (&xdg->sender_name, g_free);
}

static inline bool
has_effective_crop (obs_pipewire_data *xdg)
{
  return xdg->crop.valid &&
         (xdg->crop.x != 0 ||
          xdg->crop.y != 0 ||
          xdg->crop.width < xdg->format.info.raw.size.width ||
          xdg->crop.height < xdg->format.info.raw.size.height);
}

static bool
spa_pixel_format_to_obs_pixel_format (uint32_t              spa_format,
                                      enum gs_color_format *out_format)
{
  switch (spa_format)
    {
    case SPA_VIDEO_FORMAT_RGBA:
    case SPA_VIDEO_FORMAT_RGBx:
      *out_format = GS_RGBA;
      break;

    case SPA_VIDEO_FORMAT_BGRA:
      *out_format = GS_BGRA;
      break;

    case SPA_VIDEO_FORMAT_BGRx:
      *out_format = GS_BGRX;
      break;

    default:
      return false;
    }

  return true;
}

/* ------------------------------------------------- */

static void
on_process_cb (void *user_data)
{
  obs_pipewire_data *xdg = user_data;
  struct spa_meta_cursor *cursor;
  struct spa_meta_region *region;
  struct spa_buffer *buffer;
  struct pw_buffer *b;

  /* Find the most recent buffer */
  b = NULL;
  while (true)
    {
      struct pw_buffer *aux = pw_stream_dequeue_buffer (xdg->stream);
      if (!aux)
        break;
      if (b)
        pw_stream_queue_buffer (xdg->stream, b);
      b = aux;
    }

  if (!b)
    {
      blog (LOG_DEBUG, "[pipewire] Out of buffers!");
      return;
    }

  buffer = b->buffer;

  obs_enter_graphics ();

  xdg->current_pw_buffer = b;

  if (buffer->datas[0].chunk->size == 0)
    {
      /* Do nothing; empty chunk means this is a metadata-only frame */
    }
  else if (buffer->datas[0].type == SPA_DATA_DmaBuf)
    {
      uint32_t offsets[1];
      uint32_t strides[1];
      int fds[1];

      blog (LOG_DEBUG, "[pipewire] DMA-BUF info: fd:%ld, stride:%d, offset:%u, size:%dx%d",
            buffer->datas[0].fd,
            buffer->datas[0].chunk->stride,
            buffer->datas[0].chunk->offset,
            xdg->format.info.raw.size.width,
            xdg->format.info.raw.size.height);

      fds[0] = buffer->datas[0].fd;
      offsets[0] = buffer->datas[0].chunk->offset;
      strides[0] = buffer->datas[0].chunk->stride;

      g_clear_pointer (&xdg->texture, gs_texture_destroy);
      xdg->texture =
        gs_texture_create_from_dmabuf (xdg->format.info.raw.size.width,
                                       xdg->format.info.raw.size.height,
                                       GS_BGRX,
                                       1,
                                       fds,
                                       strides,
                                       offsets,
                                       NULL);
    }
  else
    {
      blog (LOG_DEBUG, "[pipewire] Buffer has memory texture");

      g_clear_pointer (&xdg->texture, gs_texture_destroy);
      xdg->texture =
        gs_texture_create (xdg->format.info.raw.size.width,
                           xdg->format.info.raw.size.height,
                           GS_BGRX,
                           1,
                           (const uint8_t **)&buffer->datas[0].data,
                           GS_DYNAMIC);
    }

  /* Video Crop */
  region = spa_buffer_find_meta_data (buffer, SPA_META_VideoCrop, sizeof (*region));
  if (region && spa_meta_region_is_valid (region))
    {
      blog (LOG_DEBUG, "[pipewire] Crop Region available (%dx%d+%d+%d)",
            region->region.position.x,
            region->region.position.y,
            region->region.size.width,
            region->region.size.height);

      xdg->crop.x = region->region.position.x;
      xdg->crop.y = region->region.position.y;
      xdg->crop.width = region->region.size.width;
      xdg->crop.height = region->region.size.height;
      xdg->crop.valid = true;
    }
  else
    {
      xdg->crop.valid = false;
    }

  /* Cursor */
  cursor = spa_buffer_find_meta_data (buffer, SPA_META_Cursor, sizeof (*cursor));
  xdg->cursor.valid = cursor && spa_meta_cursor_is_valid (cursor);
  if (xdg->cursor.visible && xdg->cursor.valid)
    {
      struct spa_meta_bitmap *bitmap = NULL;
      enum gs_color_format format;

      if (cursor->bitmap_offset)
        bitmap = SPA_MEMBER (cursor, cursor->bitmap_offset, struct spa_meta_bitmap);

      if (bitmap &&
          bitmap->size.width > 0 &&
          bitmap->size.height > 0 &&
          spa_pixel_format_to_obs_pixel_format (bitmap->format, &format))
        {
          const uint8_t *bitmap_data;

          bitmap_data = SPA_MEMBER (bitmap, bitmap->offset, uint8_t);
          xdg->cursor.hotspot_x = cursor->hotspot.x;
          xdg->cursor.hotspot_y = cursor->hotspot.y;
          xdg->cursor.width = bitmap->size.width;
          xdg->cursor.height = bitmap->size.height;

          g_clear_pointer (&xdg->cursor.texture, gs_texture_destroy);
          xdg->cursor.texture =
            gs_texture_create (xdg->cursor.width,
                               xdg->cursor.height,
                               format,
                               1,
                               &bitmap_data,
                               GS_DYNAMIC);
        }

      xdg->cursor.x = cursor->position.x;
      xdg->cursor.y = cursor->position.y;
    }

  obs_leave_graphics ();

  /*
   * Don't immediately queue the buffer now. Instead, wait until the next
   * call to obs_pipewire_portal_render_video() to queue it back.
   */
}

static void
on_param_changed_cb (void                 *user_data,
                     uint32_t              id,
                     const struct spa_pod *param)
{
  obs_pipewire_data *xdg = user_data;
  struct spa_pod_builder pod_builder;
  const struct spa_pod *params[2];
  uint8_t params_buffer[1024];
  int result;

  if (!param || id != SPA_PARAM_Format)
    return;

  result = spa_format_parse (param,
                             &xdg->format.media_type,
                             &xdg->format.media_subtype);
  if (result < 0)
    return;

  if (xdg->format.media_type != SPA_MEDIA_TYPE_video ||
      xdg->format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
    return;

  spa_format_video_raw_parse (param, &xdg->format.info.raw);

  blog (LOG_DEBUG, "[pipewire] Negotiated format:");

  blog (LOG_DEBUG, "[pipewire]     Format: %d (%s)",
        xdg->format.info.raw.format,
        spa_debug_type_find_name(spa_type_video_format,
                                 xdg->format.info.raw.format));

  blog (LOG_DEBUG, "[pipewire]     Size: %dx%d",
        xdg->format.info.raw.size.width,
        xdg->format.info.raw.size.height);

  blog (LOG_DEBUG, "[pipewire]     Framerate: %d/%d",
        xdg->format.info.raw.framerate.num,
        xdg->format.info.raw.framerate.denom);

  /* Video crop */
  pod_builder = SPA_POD_BUILDER_INIT (params_buffer, sizeof (params_buffer));
  params[0] = spa_pod_builder_add_object (
    &pod_builder,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id (SPA_META_VideoCrop),
		SPA_PARAM_META_size, SPA_POD_Int (sizeof (struct spa_meta_region)));

  /* Cursor */
  params[1] = spa_pod_builder_add_object (
    &pod_builder,
    SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
    SPA_PARAM_META_type, SPA_POD_Id (SPA_META_Cursor),
    SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int (CURSOR_META_SIZE (64, 64),
                                                   CURSOR_META_SIZE (1, 1),
                                                   CURSOR_META_SIZE (1024, 1024)));

  pw_stream_update_params (xdg->stream, params, 2);

  xdg->negotiated = true;
  pw_thread_loop_signal (xdg->thread_loop, FALSE);
}

static const struct pw_stream_events stream_events =
{
  PW_VERSION_STREAM_EVENTS,
  .param_changed = on_param_changed_cb,
  .process = on_process_cb,
};

static void
on_core_error_cb (void       *user_data,
                  uint32_t    id,
                  int         seq,
                  int         res,
                  const char *message)
{
  obs_pipewire_data *xdg = user_data;

  blog (LOG_ERROR, "[pipewire] Error id:%u seq:%d res:%d (%s): %s",
        id, seq, res, g_strerror (res), message);

  pw_thread_loop_signal (xdg->thread_loop, FALSE);
}

static void
on_core_done_cb (void     *user_data,
                 uint32_t  id,
                 int       seq)
{
  obs_pipewire_data *xdg = user_data;

  if (id == PW_ID_CORE)
    pw_thread_loop_signal (xdg->thread_loop, FALSE);
}

static const struct pw_core_events core_events = {
  PW_VERSION_CORE_EVENTS,
  .done = on_core_done_cb,
  .error = on_core_error_cb,
};

static void
play_pipewire_stream (obs_pipewire_data *xdg)
{
  struct spa_pod_builder pod_builder;
  const struct spa_pod *params[1];
  uint8_t params_buffer[1024];

  xdg->thread_loop = pw_thread_loop_new ("PipeWire thread loop", NULL);
  xdg->context = pw_context_new (pw_thread_loop_get_loop (xdg->thread_loop), NULL, 0);

  if (pw_thread_loop_start (xdg->thread_loop) < 0)
    {
      blog (LOG_WARNING, "Error starting threaded mainloop");
      return;
    }

  pw_thread_loop_lock (xdg->thread_loop);

  /* Core */
  xdg->core = pw_context_connect_fd (xdg->context,
                                     fcntl (xdg->pipewire_fd, F_DUPFD_CLOEXEC, 3),
                                     NULL,
                                     0);
  if (!xdg->core)
    {
      blog (LOG_WARNING, "Error creating PipeWire core: %m");
      pw_thread_loop_unlock (xdg->thread_loop);
      return;
    }

  pw_core_add_listener (xdg->core, &xdg->core_listener, &core_events, xdg);

  /* Stream */
  xdg->stream = pw_stream_new (xdg->core,
                               "OBS Studio",
                               pw_properties_new (PW_KEY_MEDIA_TYPE, "Video",
                                                  PW_KEY_MEDIA_CATEGORY, "Capture",
                                                  PW_KEY_MEDIA_ROLE, "Screen",
                                                  NULL));
  pw_stream_add_listener (xdg->stream, &xdg->stream_listener, &stream_events, xdg);

  /* Stream parameters */
  pod_builder = SPA_POD_BUILDER_INIT (params_buffer, sizeof (params_buffer));
  params[0] = spa_pod_builder_add_object (
    &pod_builder,
    SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
    SPA_FORMAT_mediaType, SPA_POD_Id (SPA_MEDIA_TYPE_video),
    SPA_FORMAT_mediaSubtype, SPA_POD_Id (SPA_MEDIA_SUBTYPE_raw),
    SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id (5,
                                                     SPA_VIDEO_FORMAT_RGB,
                                                     SPA_VIDEO_FORMAT_RGB,
                                                     SPA_VIDEO_FORMAT_RGBA,
                                                     SPA_VIDEO_FORMAT_RGBx,
                                                     SPA_VIDEO_FORMAT_BGRx),
    SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle (&SPA_RECTANGLE (320, 240),
                                                           &SPA_RECTANGLE (1, 1),
                                                           &SPA_RECTANGLE (4096, 4096)),
    SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction (&SPA_FRACTION (60, 1),
                                                               &SPA_FRACTION (0, 1),
                                                               &SPA_FRACTION (144, 1)));

  pw_stream_connect (xdg->stream,
                     PW_DIRECTION_INPUT,
                     xdg->pipewire_node,
                     PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                     params,
                     1);

  blog (LOG_INFO, "[OBS XDG] Starting monitor screencast…");

  pw_thread_loop_wait (xdg->thread_loop);
  pw_thread_loop_unlock (xdg->thread_loop);
}

/* ------------------------------------------------- */

static void
on_pipewire_remote_opened_cb (GObject      *source,
                              GAsyncResult *res,
                              gpointer      user_data)
{
  g_autoptr (GUnixFDList) fd_list = NULL;
  g_autoptr (GVariant) result = NULL;
  g_autoptr (GError) error = NULL;
  obs_pipewire_data *xdg = user_data;
  int fd_index;

  result = g_dbus_connection_call_with_unix_fd_list_finish (G_DBUS_CONNECTION (source),
                                                            &fd_list,
                                                            res,
                                                            &error);
  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        blog (LOG_ERROR, "[OBS XDG] Error retrieving pipewire fd: %s", error->message);
      return;
    }

  g_variant_get (result, "(h)", &fd_index, &error);

  xdg->pipewire_fd = g_unix_fd_list_get (fd_list, fd_index, &error);
  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        blog (LOG_ERROR, "[OBS XDG] Error retrieving pipewire fd: %s", error->message);
      return;
    }

  play_pipewire_stream (xdg);
}

static void
open_pipewire_remote (obs_pipewire_data *xdg)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);

  g_dbus_connection_call_with_unix_fd_list (xdg->connection,
                                            "org.freedesktop.portal.Desktop",
                                            "/org/freedesktop/portal/desktop",
                                            "org.freedesktop.portal.ScreenCast",
                                            "OpenPipeWireRemote",
                                            g_variant_new ("(oa{sv})", xdg->session_handle, &builder),
                                            NULL,
                                            G_DBUS_CALL_FLAGS_NONE,
                                            -1,
                                            NULL,
                                            xdg->cancellable,
                                            on_pipewire_remote_opened_cb,
                                            xdg);
}

/* ------------------------------------------------- */

static void
on_start_response_received_cb (GDBusConnection *connection,
                               const char      *sender_name,
                               const char      *object_path,
                               const char      *interface_name,
                               const char      *signal_name,
                               GVariant        *parameters,
                               gpointer         user_data)
{
  g_autoptr (GVariant) stream_properties = NULL;
  g_autoptr (GVariant) streams = NULL;
  g_autoptr (GVariant) result = NULL;
  dbus_call_data *call = user_data;
  obs_pipewire_data *xdg = call->xdg;
  GVariantIter iter;
  uint32_t response;

  g_clear_pointer (&call, dbus_call_data_free);

  g_variant_get (parameters, "(u@a{sv})", &response, &result);

  if (response != 0)
    {
      blog (LOG_WARNING, "[OBS XDG] Failed to start screencast, denied or cancelled by user");
      return;
    }

  streams = g_variant_lookup_value (result, "streams", G_VARIANT_TYPE_ARRAY);

  g_variant_iter_init (&iter, streams);
  g_assert (g_variant_iter_n_children (&iter) == 1);

  g_variant_iter_loop (&iter, "(u@a{sv})", &xdg->pipewire_node, &stream_properties);

  blog (LOG_INFO, "[OBS XDG] Monitor selected, setting up screencast");

  open_pipewire_remote (xdg);
}

static void
on_started_cb (GObject      *source,
               GAsyncResult *res,
               gpointer      user_data)
{
  g_autoptr (GVariant) result = NULL;
  g_autoptr (GError) error = NULL;

  result = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        blog (LOG_ERROR, "[OBS XDG] Error selecting screencast source: %s", error->message);
      return;
    }
}

static void
start (obs_pipewire_data *xdg)
{
  g_autofree char *request_token = NULL;
  g_autofree char *request_path = NULL;
  GVariantBuilder builder;
  dbus_call_data *call;

  new_request_path (xdg, &request_path, &request_token);

  blog (LOG_INFO, "[OBS XDG] Asking for monitor…");

  call = subscribe_to_signal (xdg, request_path, on_start_response_received_cb);

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "handle_token", g_variant_new_string (request_token));

  g_dbus_connection_call (xdg->connection,
                          "org.freedesktop.portal.Desktop",
                          "/org/freedesktop/portal/desktop",
                          "org.freedesktop.portal.ScreenCast",
                          "Start",
                          g_variant_new ("(osa{sv})", xdg->session_handle, "", &builder),
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          xdg->cancellable,
                          on_started_cb,
                          call);
}

/* ------------------------------------------------- */

static void
on_select_source_response_received_cb (GDBusConnection *connection,
                                       const char      *sender_name,
                                       const char      *object_path,
                                       const char      *interface_name,
                                       const char      *signal_name,
                                       GVariant        *parameters,
                                       gpointer         user_data)
{
  g_autoptr (GVariant) ret = NULL;
  dbus_call_data *call = user_data;
  obs_pipewire_data *xdg = call->xdg;
  uint32_t response;

  blog (LOG_DEBUG, "[OBS XDG] Response to select source received");

  g_clear_pointer (&call, dbus_call_data_free);

  g_variant_get (parameters, "(u@a{sv})", &response, &ret);

  if (response != 0)
    {
      blog (LOG_WARNING, "[OBS XDG] Failed to select source, denied or cancelled by user");
      return;
    }

  start (xdg);
}

static void
on_source_selected_cb (GObject      *source,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  g_autoptr (GVariant) result = NULL;
  g_autoptr (GError) error = NULL;

  result = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        blog (LOG_ERROR, "[OBS XDG] Error selecting screencast source: %s", error->message);
      return;
    }
}

static void
select_source (obs_pipewire_data *xdg)
{
  g_autofree char *request_token = NULL;
  g_autofree char *request_path = NULL;
  GVariantBuilder builder;
  dbus_call_data *call;

  new_request_path (xdg, &request_path, &request_token);

  call = subscribe_to_signal (xdg, request_path, on_select_source_response_received_cb);

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "types", g_variant_new_uint32 (xdg->capture_type));
  g_variant_builder_add (&builder, "{sv}", "multiple", g_variant_new_boolean (FALSE));
  g_variant_builder_add (&builder, "{sv}", "cursor_mode", g_variant_new_uint32 (4));
  g_variant_builder_add (&builder, "{sv}", "handle_token", g_variant_new_string (request_token));

  g_dbus_connection_call (xdg->connection,
                          "org.freedesktop.portal.Desktop",
                          "/org/freedesktop/portal/desktop",
                          "org.freedesktop.portal.ScreenCast",
                          "SelectSources",
                          g_variant_new ("(oa{sv})", xdg->session_handle, &builder),
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          xdg->cancellable,
                          on_source_selected_cb,
                          call);
}

/* ------------------------------------------------- */

static void
on_create_session_response_received_cb (GDBusConnection *connection,
                                        const char      *sender_name,
                                        const char      *object_path,
                                        const char      *interface_name,
                                        const char      *signal_name,
                                        GVariant        *parameters,
                                        gpointer         user_data)
{
  g_autoptr (GVariant) result = NULL;
  dbus_call_data *call = user_data;
  obs_pipewire_data *xdg = call->xdg;
  uint32_t response;

  g_clear_pointer (&call, dbus_call_data_free);

  g_variant_get (parameters, "(u@a{sv})", &response, &result);

  if (response != 0)
    {
      blog (LOG_WARNING, "[OBS XDG] Failed to create session, denied or cancelled by user");
      return;
    }

  blog (LOG_INFO, "[OBS XDG] Screencast session created");

  g_variant_lookup (result, "session_handle", "s", &xdg->session_handle);

  select_source (xdg);
}

static void
on_session_created_cb (GObject      *source,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  g_autoptr (GVariant) result = NULL;
  g_autoptr (GError) error = NULL;

  result = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        blog (LOG_ERROR, "[OBS XDG] Error creating screencast session: %s", error->message);
      return;
    }
}

static void
create_session (obs_pipewire_data *xdg)
{
  GVariantBuilder builder;
  g_autofree char *request_token = NULL;
  g_autofree char *request_path = NULL;
  g_autofree char *session_token = NULL;
  dbus_call_data *call;

  new_request_path (xdg, &request_path, &request_token);
  new_session_path (xdg, NULL, &session_token);

  call = subscribe_to_signal (xdg, request_path, on_create_session_response_received_cb);

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "handle_token", g_variant_new_string (request_token));
  g_variant_builder_add (&builder, "{sv}", "session_handle_token", g_variant_new_string (session_token));

  g_dbus_connection_call (xdg->connection,
                          "org.freedesktop.portal.Desktop",
                          "/org/freedesktop/portal/desktop",
                          "org.freedesktop.portal.ScreenCast",
                          "CreateSession",
                          g_variant_new ("(a{sv})", &builder),
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1,
                          xdg->cancellable,
                          on_session_created_cb,
                          call);
}

/* ------------------------------------------------- */

static gboolean
init_obs_xdg (obs_pipewire_data *xdg)
{
  g_autoptr (GError) error = NULL;
  char *aux;

  xdg->cancellable = g_cancellable_new ();
  xdg->connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (error)
    {
      g_error ("Error getting session bus: %s", error->message);
      return FALSE;
    }

  xdg->sender_name = g_strdup (g_dbus_connection_get_unique_name (xdg->connection) + 1);

  /* Replace dots by underscores */
  while ((aux = g_strstr_len (xdg->sender_name, -1, ".")) != NULL)
    *aux = '_';

  blog (LOG_INFO, "OBS XDG initialized (sender name: %s)", xdg->sender_name);

  create_session (xdg);

  return TRUE;
}

/* obs_source_info methods */

void*
obs_pipewire_create (obs_pw_capture_type  capture_type,
                     obs_data_t          *settings,
                     obs_source_t        *source)
{
  obs_pipewire_data *xdg = g_new0 (obs_pipewire_data, 1);

  xdg->source = source;
  xdg->settings = settings;
  xdg->capture_type = capture_type;
  xdg->cursor.visible = true;

  if (!init_obs_xdg (xdg))
    g_clear_pointer (&xdg, g_free);

  return xdg;
}

void
obs_pipewire_destroy (obs_pipewire_data *xdg)
{
  if (!xdg)
    return;

  teardown_pipewire (xdg);
  destroy_session (xdg);

  g_free (xdg);
}

void
obs_pipewire_get_defaults (obs_data_t *settings)
{
  obs_data_set_default_bool (settings, "ShowCursor", true);
}

obs_properties_t *
obs_pipewire_get_properties (obs_pipewire_data *xdg)
{
  obs_properties_t *properties;

  properties = obs_properties_create ();
  obs_properties_add_bool (properties, "ShowCursor", obs_module_text ("ShowCursor"));

  return properties;
}

void
obs_pipewire_update (obs_pipewire_data *xdg,
                     obs_data_t        *settings)
{
  xdg->cursor.visible = obs_data_get_bool (settings, "ShowCursor");
}

void
obs_pipewire_show (obs_pipewire_data *xdg)
{
  if (xdg->stream)
    pw_stream_set_active (xdg->stream, true);
}

void
obs_pipewire_hide (obs_pipewire_data *xdg)
{
  if (xdg->stream)
    pw_stream_set_active (xdg->stream, false);
}

uint32_t
obs_pipewire_get_width (obs_pipewire_data *xdg)
{
  if (!xdg->negotiated)
    return 0;

  if (xdg->crop.valid)
    return xdg->crop.width;
  else
    return xdg->format.info.raw.size.width;
}

uint32_t
obs_pipewire_get_height (obs_pipewire_data *xdg)
{
  if (!xdg->negotiated)
    return 0;

  if (xdg->crop.valid)
    return xdg->crop.height;
  else
    return xdg->format.info.raw.size.height;
}

void
obs_pipewire_video_render (obs_pipewire_data *xdg,
                           gs_effect_t       *effect)
{
  gs_eparam_t *image;

  if (!xdg->texture)
    return;

  image = gs_effect_get_param_by_name (effect, "image");
  gs_effect_set_texture (image, xdg->texture);

  if (has_effective_crop (xdg))
    {
      gs_draw_sprite_subregion (xdg->texture,
                                0,
                                xdg->crop.x,
                                xdg->crop.y,
                                xdg->crop.x + xdg->crop.width,
                                xdg->crop.y + xdg->crop.height);
    }
  else
    {
      gs_draw_sprite (xdg->texture, 0, 0, 0);
    }

  if (xdg->cursor.visible && xdg->cursor.valid && xdg->cursor.texture)
    {
      gs_matrix_push ();
      gs_matrix_translate3f ((float)xdg->cursor.x, (float)xdg->cursor.y, 0.0f);

      gs_effect_set_texture (image, xdg->cursor.texture);
      gs_draw_sprite (xdg->texture, 0, xdg->cursor.width, xdg->cursor.height);

      gs_matrix_pop ();
    }

  /* Now that the buffer is consumed, queue it again */
  maybe_queue_buffer (xdg);
}

void
obs_pipewire_load (void)
{
  pw_init (NULL, NULL);
}
