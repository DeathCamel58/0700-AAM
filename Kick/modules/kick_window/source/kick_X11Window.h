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
    // Asks the window manager to open the window where its bounds say: its
    // frame's top-left corner at the window's position (ICCCM's default
    // north-west gravity), the client area at its size.
    //
    // Call it once the window is on the desktop, before it is shown. JUCE
    // creates the window at the screen's origin and then moves it, which
    // does not work with every window manager:
    // - Setting a constrainer makes JUCE replace the window's size hints
    //   with the minimum and maximum size alone, so the window manager does
    //   not know the window asked for a position.
    // - Weston's X window manager ignores the position in a window's
    //   requests to move (and moves the window to a place of its own), and
    //   opens it where the window was when it was mapped.
    // So the position is set in the size hints again, and the window is
    // moved while the window manager does not handle its requests
    // (override-redirect, while it is not mapped).
    void askForPosition( juce::ResizableWindow& window );

    // Shows an arrow, or nothing, where Kick's components ask for JUCE's
    // normal mouse cursor.
    //
    // JUCE sets no cursor on its window for the normal cursor, so the
    // window shows its parent's: the window manager's frame, or the root
    // window. Weston's X window manager sets the root window's (and its
    // frames') from the Xcursor theme; without a theme on the system they
    // keep the X server's default, an "X". So the root window's cursor is
    // set here: in kiosk mode, where Kick is all there is on the display,
    // and under Xwayland, where the root window is not seen and is not the
    // user's desktop. Components that ask for another cursor (a text
    // field's I-beam, say) still show it.
    void setDefaultCursor( bool show, bool kiosk );
}
