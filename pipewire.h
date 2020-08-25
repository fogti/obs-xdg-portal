/* pipewire.h
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

#pragma once

#include <obs/obs-module.h>

typedef struct _obs_pipewire_data obs_pipewire_data;

typedef enum
{
  DESKTOP_CAPTURE = 1,
  WINDOW_CAPTURE = 2,
} obs_pw_capture_type;

void* obs_pipewire_create (obs_pw_capture_type  capture_type,
                           obs_data_t          *settings,
                           obs_source_t        *source);

void obs_pipewire_destroy (obs_pipewire_data *xdg);

void obs_pipewire_get_defaults (obs_data_t *settings);

obs_properties_t * obs_pipewire_get_properties (obs_pipewire_data *xdg);

void obs_pipewire_update (obs_pipewire_data *xdg,
                          obs_data_t        *settings);

void obs_pipewire_show (obs_pipewire_data *xdg);

void obs_pipewire_hide (obs_pipewire_data *xdg);
uint32_t obs_pipewire_get_width (obs_pipewire_data *xdg);
uint32_t obs_pipewire_get_height (obs_pipewire_data *xdg);
void obs_pipewire_video_render (obs_pipewire_data *xdg,
                                gs_effect_t       *effect);

void obs_pipewire_load (void);
