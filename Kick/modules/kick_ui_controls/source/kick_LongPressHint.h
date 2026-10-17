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
    // Shows a component's tooltip in a bubble while the component is pressed
    // and held: a touch screen has no hover for juce::TooltipWindow to show
    // tooltips on. The bubble appears after holdTimeMs without moving more
    // than moveTolerance pixels, and goes away on release.
    //
    // The component forwards its mouse events. mouseUp() tells whether the
    // bubble was shown during a press held for holdTimeMs: the press was
    // then a request for the hint, and the release should not also click.
    class LongPressHint : private juce::Timer
    {
    public:
        static constexpr int holdTimeMs    = 500;
        static constexpr int moveTolerance = 8;

        LongPressHint() = default;
        ~LongPressHint() override;

        // Starts waiting, if the component has a tooltip.
        void mouseDown( juce::Component& Target, const juce::MouseEvent& Event );
        // Stops waiting if the press moved too far.
        void mouseDrag( const juce::MouseEvent& Event );
        // Stops waiting, hides the bubble; true if it was shown and the
        // press lasted holdTimeMs.
        bool mouseUp( const juce::MouseEvent& Event );

        bool isShowing() const noexcept { return bubble != nullptr; }

    private:
        void timerCallback() override;
        void hide();

        juce::Component::SafePointer<juce::Component> target;
        juce::String                                  text;
        std::unique_ptr<juce::Component>              bubble;

        JUCE_DECLARE_NON_COPYABLE( LongPressHint )
    };
}
