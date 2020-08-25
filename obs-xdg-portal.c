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

#include "pipewire.h"

OBS_DECLARE_MODULE()

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
  return obs_pipewire_create (settings, source);
}

static void
obs_xdg_destroy(void *data)
{
  obs_pipewire_destroy (data);
}

static void
obs_xdg_get_defaults (obs_data_t *settings)
{
  obs_pipewire_get_defaults (settings);
}

static obs_properties_t *
obs_xdg_get_properties (void *data)
{
  return obs_pipewire_get_properties (data);
}

static void
obs_xdg_update (void       *data,
                obs_data_t *settings)
{
  obs_pipewire_update (data, settings);
}

static void
obs_xdg_show (void *data)
{
  obs_pipewire_show (data);
}

static void
obs_xdg_hide (void *data)
{
  obs_pipewire_hide (data);
}

static uint32_t
obs_xdg_get_width (void *data)
{
  return obs_pipewire_get_width (data);
}

static uint32_t
obs_xdg_get_height (void *data)
{
  return obs_pipewire_get_height (data);
}

static void
obs_xdg_video_render (void        *data,
                      gs_effect_t *effect)
{
  obs_pipewire_video_render (data, effect);
}

bool obs_module_load (void)
{
  struct obs_source_info info = {
    .id = "obs-xdg-source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_VIDEO,
    .get_name = obs_xdg_get_name,
    .create = obs_xdg_create,
    .destroy = obs_xdg_destroy,
    .get_defaults = obs_xdg_get_defaults,
    .get_properties = obs_xdg_get_properties,
    .update = obs_xdg_update,
    .show = obs_xdg_show,
    .hide = obs_xdg_hide,
    .get_width = obs_xdg_get_width,
    .get_height = obs_xdg_get_height,
    .video_render = obs_xdg_video_render,
    .icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE,
  };

  obs_register_source(&info);

  obs_pipewire_load ();

  return true;
}
