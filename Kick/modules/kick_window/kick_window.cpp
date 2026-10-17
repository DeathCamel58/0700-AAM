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

#ifdef KICK_WINDOW_INCLUDE
    #error "Incorrect use of JUCE cpp file"
#endif

// JUCE's X11 declarations, for the OpenGL check (kick_OpenGLSupport.cpp).
#define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1

#include "kick_window.h"
#include "source/kick_OpenGLSupport.h"

#include "source/kick_OpenGLSupport.cpp"
#include "source/kick_WindowGeometry.cpp"
#include "source/kick_FrameTiming.cpp"
#include "source/kick_ScaledHost.cpp"
#include "source/kick_MainWindow.cpp"
