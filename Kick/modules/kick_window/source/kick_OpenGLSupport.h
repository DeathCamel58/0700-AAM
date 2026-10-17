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

#pragma once

namespace kick::window::detail
{
    // Why JUCE's OpenGL context (GLX) would fail on this display, or an
    // empty string when it can be created.
    //
    // On Linux, JUCE 7's OpenGLContext reports success however it goes: it
    // dereferences a missing frame buffer configuration (a crash) and, when
    // the context cannot be created, leaves the window black. So the same
    // steps are tried here first, on JUCE's X display: the GLX extension, a
    // frame buffer configuration with JUCE's default pixel format, and a
    // context.
    juce::String findOpenGLProblem();
}
