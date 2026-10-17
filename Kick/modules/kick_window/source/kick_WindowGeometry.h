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

#include <juce_graphics/juce_graphics.h>

#include <optional>

namespace kick::window
{
    // How a fixed-size layout is shown in an area of another size: scaled
    // uniformly to the largest size that fits, and centred, leaving bars
    // at the sides or at the top and bottom (letterboxing).
    struct Letterbox
    {
        float                  scale{ 1.0f };
        juce::Point<float>     offset;
        juce::AffineTransform  transform;   // layout -> area
        juce::Rectangle<float> contentArea; // the layout in the area
    };

    // The offset is rounded to whole pixels, and a scale this close to 1
    // is 1, so that an area of the layout's size shows it unchanged.
    Letterbox computeLetterbox( juce::Rectangle<int> area, juce::Rectangle<int> layout );

    // A saved window position and size: "x y width height".
    juce::String                        formatWindowBounds( juce::Rectangle<int> bounds );
    std::optional<juce::Rectangle<int>> parseWindowBounds( const juce::String& text );

    // Where a desktop window showing the layout opens: at its saved bounds
    // (with the layout's aspect ratio) when they are large enough and on a
    // display; otherwise at the layout's size, smaller if the display is,
    // centred in preferredArea (the display under the mouse).
    juce::Rectangle<int> chooseWindowBounds( std::optional<juce::Rectangle<int>> saved,
                                             const juce::Array<juce::Rectangle<int>>& displayAreas,
                                             juce::Rectangle<int>                      preferredArea,
                                             juce::Point<int>                          layoutSize,
                                             juce::Point<int>                          minimumSize );
}
