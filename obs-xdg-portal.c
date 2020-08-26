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

#include <obs/obs-nix-platform.h>

#include "desktop-capture.h"
#include "pipewire.h"
#include "window-capture.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-xdg-portal", "en-US")

bool obs_module_load (void)
{
  if (obs_get_nix_platform () == OBS_NIX_PLATFORM_X11_GLX)
    {
      blog (LOG_INFO, "obs-xdg-portal cannot run on X11/GLX, disabling…");
      return false;
    }

  desktop_capture_register_source ();
  window_capture_register_source ();

  obs_pipewire_load ();

  return true;
}
