/*
 * This file is part of Kick.
 *
 * Kick is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Kick is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with Kick. If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*******************************************************************************
 BEGIN_JUCE_MODULE_DECLARATION

  ID:                 kick_window
  vendor:             MUSIC Tribe
  version:            0.1.0
  name:               Kick main window
  description:        The window Kick's user interface is shown in
  website:
  license:            AGPLv3

  dependencies:       kick_window_options, juce_gui_basics, juce_opengl

 END_JUCE_MODULE_DECLARATION
*******************************************************************************/

#pragma once
#ifndef KICK_WINDOW_INCLUDE
    #define KICK_WINDOW_INCLUDE

    #include <kick_window_options/kick_window_options.h>
    #include <juce_gui_basics/juce_gui_basics.h>
    #include <juce_opengl/juce_opengl.h>

    #include "source/kick_WindowGeometry.h"
    #include "source/kick_FrameTiming.h"
    #include "source/kick_ScaledHost.h"
    #include "source/kick_MainWindow.h"

#endif
