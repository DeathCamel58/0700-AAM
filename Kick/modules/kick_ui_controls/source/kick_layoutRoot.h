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

namespace ui
{
    // The component the fixed layout (ui::layout, 1024x600) is laid out in:
    // the home screen. Pop-ups, dialogs, notifications and menus become its
    // children rather than windows of their own, so that they appear in
    // Kick's window wherever it is, scaled with it, and the window stays the
    // only one Kick has.
    //
    // Layout coordinates are coordinates in this component. Without one
    // (a program that has no home screen), they are screen coordinates.
    void             setLayoutRoot( juce::Component* root );
    juce::Component* getLayoutRoot();

    // The top-left of the component, in layout coordinates.
    juce::Point<int> getLayoutPosition( juce::Component& component );

    // A point on the screen, in layout coordinates.
    juce::Point<int> screenToLayout( juce::Point<int> screenPosition );
}
