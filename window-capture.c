/* window-capture.c
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


/* obs_source_info methods */

static const char *
window_capture_get_name (void *type_data)
{
  return obs_module_text ("WindowCapture");
}

static void *
window_capture_create (obs_data_t   *settings,
                       obs_source_t *source)
{
  return obs_pipewire_create (WINDOW_CAPTURE, settings, source);
}

static void
window_capture_destroy(void *data)
{
  obs_pipewire_destroy (data);
}

static void
window_capture_get_defaults (obs_data_t *settings)
{
  obs_pipewire_get_defaults (settings);
}

static obs_properties_t *
window_capture_get_properties (void *data)
{
  return obs_pipewire_get_properties (data, "SelectWindow");
}

static void
window_capture_update (void       *data,
                       obs_data_t *settings)
{
  obs_pipewire_update (data, settings);
}

static void
window_capture_show (void *data)
{
  obs_pipewire_show (data);
}

static void
window_capture_hide (void *data)
{
  obs_pipewire_hide (data);
}

static uint32_t
window_capture_get_width (void *data)
{
  return obs_pipewire_get_width (data);
}

static uint32_t
window_capture_get_height (void *data)
{
  return obs_pipewire_get_height (data);
}

static void
window_capture_video_render (void        *data,
                             gs_effect_t *effect)
{
  obs_pipewire_video_render (data, effect);
}

void
window_capture_register_source (void)
{
  struct obs_source_info info = {
    .id = "obs-xdg-window-capture",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_VIDEO,
    .get_name = window_capture_get_name,
    .create = window_capture_create,
    .destroy = window_capture_destroy,
    .get_defaults = window_capture_get_defaults,
    .get_properties = window_capture_get_properties,
    .update = window_capture_update,
    .show = window_capture_show,
    .hide = window_capture_hide,
    .get_width = window_capture_get_width,
    .get_height = window_capture_get_height,
    .video_render = window_capture_video_render,
    .icon_type = OBS_ICON_TYPE_WINDOW_CAPTURE,
  };

  obs_register_source (&info);
}
