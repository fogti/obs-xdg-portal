/* obs-xdg-portal.c
 *
 * Copyright 2019 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *
 * Parts of this code were inspired by obs-gnome-mutter-screencast from
 * Florian Zwoch <fzwoch@gmail.com>.
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

#include <obs/obs-module.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gst/gst.h>
#include <gst/app/app.h>
#include <gst/video/video.h>

#define REQUEST_PATH "/org/freedesktop/portal/desktop/request/%s/obs%u"
#define SESSION_PATH "/org/freedesktop/portal/desktop/session/%s/obs%u"

OBS_DECLARE_MODULE()

typedef struct
{
  GDBusConnection *connection;
  GCancellable    *cancellable;

  char            *sender_name;
  char            *session_handle;

  uint32_t         pipewire_node;
  int              pipewire_fd;

  GstElement      *gst_element;

        obs_source_t *source;
        obs_data_t *settings;
} obs_xdg_data;

/* auxiliary methods */

static void
new_request_path (obs_xdg_data  *data,
                  char         **out_path,
                  char         **out_token)
{
  static uint32_t request_token_count = 0;

  request_token_count++;

  if (out_token)
    *out_token = g_strdup_printf ("obs%u", request_token_count);

  if (out_path)
    *out_path = g_strdup_printf (REQUEST_PATH, data->sender_name, request_token_count);
}

static void
new_session_path (obs_xdg_data  *data,
                  char         **out_path,
                  char         **out_token)
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
  obs_xdg_data *xdg;
  char         *request_path;
  guint         signal_id;
  gulong        cancelled_id;
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
subscribe_to_signal (obs_xdg_data        *xdg,
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

/* ------------------------------------------------- */

static GstFlowReturn
new_appsink_sample_cb (GstAppSink *appsink,
                       gpointer    user_data)
{
  struct obs_source_frame frame;
  enum video_colorspace colorspace = VIDEO_CS_DEFAULT;
  enum video_range_type range = VIDEO_RANGE_DEFAULT;
  g_autoptr (GstSample) sample = NULL;
  obs_xdg_data *xdg = user_data;
  GstVideoInfo video_info;
  GstMapInfo info;
  GstBuffer *buffer;
  GstCaps *caps;

  sample = gst_app_sink_pull_sample (appsink);
  buffer = gst_sample_get_buffer (sample);
  caps = gst_sample_get_caps (sample);

  gst_buffer_ref (buffer);

  gst_video_info_from_caps (&video_info, caps);
  gst_buffer_map (buffer, &info, GST_MAP_READ);

  frame = (struct obs_source_frame) {
    .width = video_info.width,
    .height = video_info.height,
    .linesize[0] = video_info.stride[0],
    .linesize[1] = video_info.stride[1],
    .linesize[2] = video_info.stride[2],
    .data[0] = info.data + video_info.offset[0],
    .data[1] = info.data + video_info.offset[1],
    .data[2] = info.data + video_info.offset[2],
    .timestamp = GST_BUFFER_DTS_OR_PTS (buffer),
  };

  switch (video_info.colorimetry.range)
    {
    case GST_VIDEO_COLOR_RANGE_0_255:
      range = VIDEO_RANGE_FULL;
      frame.full_range = 1;
      break;

    case GST_VIDEO_COLOR_RANGE_16_235:
      range = VIDEO_RANGE_PARTIAL;
      break;

    default:
      break;
    }

  switch (video_info.colorimetry.matrix)
    {
    case GST_VIDEO_COLOR_MATRIX_BT709:
      colorspace = VIDEO_CS_709;
      break;

    case GST_VIDEO_COLOR_MATRIX_BT601:
      colorspace = VIDEO_CS_601;
      break;

    default:
      break;
    }

  video_format_get_parameters (colorspace,
                               range,
                               frame.color_matrix,
                               frame.color_range_min,
                               frame.color_range_max);

  switch (video_info.finfo->format)
    {
    case GST_VIDEO_FORMAT_I420:
      frame.format = VIDEO_FORMAT_I420;
      break;

    case GST_VIDEO_FORMAT_NV12:
      frame.format = VIDEO_FORMAT_NV12;
      break;

    case GST_VIDEO_FORMAT_BGRx:
      frame.format = VIDEO_FORMAT_BGRX;
      break;

    case GST_VIDEO_FORMAT_BGRA:
      frame.format = VIDEO_FORMAT_BGRA;
      break;

    case GST_VIDEO_FORMAT_RGBx:
    case GST_VIDEO_FORMAT_RGBA:
      frame.format = VIDEO_FORMAT_RGBA;
      break;

    case GST_VIDEO_FORMAT_UYVY:
      frame.format = VIDEO_FORMAT_UYVY;
      break;

    case GST_VIDEO_FORMAT_YUY2:
      frame.format = VIDEO_FORMAT_YUY2;
      break;

    case GST_VIDEO_FORMAT_YVYU:
      frame.format = VIDEO_FORMAT_YVYU;
      break;

    default:
      frame.format = VIDEO_FORMAT_NONE;
      blog (LOG_ERROR, "[OBS XDG] Unknown video format: %s", video_info.finfo->name);
      break;
    }

  obs_source_output_video (xdg->source, &frame);

  gst_buffer_unmap (buffer, &info);

  return GST_FLOW_OK;
}

static gboolean
bus_watch_cb (GstBus     *bus,
              GstMessage *message,
              gpointer    user_data)
{
  g_autoptr (GError) error = NULL;
  obs_xdg_data *xdg = user_data;

  switch (GST_MESSAGE_TYPE (message))
    {
    case GST_MESSAGE_EOS:
      obs_source_output_video (xdg->source, NULL);
      break;

    case GST_MESSAGE_ERROR:
      gst_message_parse_error (message, &error, NULL);
      blog (LOG_ERROR, "[OBS XDG] GStreamer bus error: %s", error->message);

      gst_element_set_state (xdg->gst_element, GST_STATE_NULL);
      obs_source_output_video (xdg->source, NULL);
      break;

    default:
      break;
    }

  return TRUE;
}

static void
play_pipewire_stream (obs_xdg_data *xdg)
{
  g_autoptr (GstElement) appsink = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GstBus) bus = NULL;
  g_autofree char *pipeline = NULL;

  pipeline = g_strdup_printf ("pipewiresrc client-name=obs-studio fd=%d path=%u ! "
                              "video/x-raw ! "
                              "appsink max-buffers=2 drop=true sync=false name=appsink",
                              xdg->pipewire_fd,
                              xdg->pipewire_node);

  xdg->gst_element = gst_parse_launch (pipeline, &error);
  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        blog (LOG_ERROR, "[OBS XDG] Error setting up GStreamer pipeline: %s", error->message);
      return;
    }

  appsink = gst_bin_get_by_name (GST_BIN (xdg->gst_element), "appsink");
  gst_app_sink_set_callbacks (GST_APP_SINK (appsink),
                              &(GstAppSinkCallbacks)
                                {
                                  NULL,
                                  NULL,
                                  new_appsink_sample_cb,
                                },
                              xdg,
                              NULL);

  bus = gst_element_get_bus (xdg->gst_element);
  gst_bus_add_watch (bus, bus_watch_cb, xdg);

  blog (LOG_INFO, "[OBS XDG] Starting monitor screencast…");

  gst_element_set_state (xdg->gst_element, GST_STATE_PLAYING);
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
  obs_xdg_data *xdg = user_data;
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
open_pipewire_remote (obs_xdg_data *xdg)
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
  obs_xdg_data *xdg = call->xdg;
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
start (obs_xdg_data *xdg)
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
  obs_xdg_data *xdg = call->xdg;
  uint32_t response;

  blog (LOG_DEBUG, "[OBS XDG] Response to select source received");

  g_clear_signal_handler (&call->cancelled_id, xdg->cancellable);
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
select_source (obs_xdg_data *xdg)
{
  g_autofree char *request_token = NULL;
  g_autofree char *request_path = NULL;
  GVariantBuilder builder;
  dbus_call_data *call;

  new_request_path (xdg, &request_path, &request_token);

  call = subscribe_to_signal (xdg, request_path, on_select_source_response_received_cb);

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&builder, "{sv}", "types", g_variant_new_uint32 (3));
  g_variant_builder_add (&builder, "{sv}", "multiple", g_variant_new_boolean (FALSE));
  g_variant_builder_add (&builder, "{sv}", "cursor_mode", g_variant_new_uint32 (2));
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
  obs_xdg_data *xdg = call->xdg;
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
create_session (obs_xdg_data *xdg)
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
init_obs_xdg (obs_xdg_data *xdg)
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

static const char *
obs_xdg_get_name (void *type_data)
{
        return "Desktop Screencast (Wayland / X11)";
}

static void *
obs_xdg_create(obs_data_t   *settings,
               obs_source_t *source)
{
        obs_xdg_data *xdg = g_new0 (obs_xdg_data, 1);

        xdg->source = source;
        xdg->settings = settings;

  if (!init_obs_xdg (xdg))
    g_clear_pointer (&xdg, g_free);

        return xdg;
}

static void
obs_xdg_destroy(void *data)
{
  obs_xdg_data *xdg = data;

  if (!xdg)
    return;

  if (xdg->gst_element)
    {
      gst_element_set_state (xdg->gst_element, GST_STATE_NULL);
      g_clear_object (&xdg->gst_element);
    }

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

  g_cancellable_cancel (xdg->cancellable);
  g_clear_object (&xdg->cancellable);
  g_clear_object (&xdg->connection);
  g_clear_pointer (&xdg->sender_name, g_free);
  g_free (xdg);
}

static void
obs_xdg_get_defaults (obs_data_t *settings)
{
}

static obs_properties_t *
obs_xdg_get_properties (void *data)
{
  return NULL;
}

static void
obs_xdg_update (void       *data,
                obs_data_t *settings)
{
}

static void
obs_xdg_show (void *data)
{
  obs_xdg_data *xdg = data;

  if (xdg->gst_element)
    gst_element_set_state (xdg->gst_element, GST_STATE_PLAYING);
}

static void
obs_xdg_hide (void *data)
{
  obs_xdg_data *xdg = data;

  if (xdg->gst_element)
    gst_element_set_state (xdg->gst_element, GST_STATE_PAUSED);
}

bool obs_module_load (void)
{
  struct obs_source_info info = {
    .id = "obs-xdg-source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
    .get_name = obs_xdg_get_name,
    .create = obs_xdg_create,
    .destroy = obs_xdg_destroy,
    .get_defaults = obs_xdg_get_defaults,
    .get_properties = obs_xdg_get_properties,
    .update = obs_xdg_update,
    .show = obs_xdg_show,
    .hide = obs_xdg_hide,
  };

  obs_register_source(&info);

  gst_init (NULL, NULL);

  return true;
}
