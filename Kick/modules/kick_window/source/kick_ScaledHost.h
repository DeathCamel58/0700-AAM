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

namespace kick::window
{
    // Shows a component laid out at a fixed size (Kick's 1024x600 layout)
    // in an area of any size: the component keeps its size and is scaled
    // with an AffineTransform, letterboxed and centred. JUCE maps mouse and
    // touch positions through the transform, so input needs nothing else.
    //
    // The component is not owned; it is taken out of this host and its
    // transform reset when the host is destroyed.
    //
    // It can show an overlay above the content: a component sized in
    // layout units, scaled as the layout is, in the host's bottom-right
    // corner (placeInBottomRight()), above everything the content shows,
    // its pop-ups included (they are the content's children). It is not
    // owned either.
    //
    // The host times the frames it paints (FrameTiming), while that is
    // enabled.
    class ScaledHost : public juce::Component
    {
    public:
        explicit ScaledHost( juce::Component& content );
        ~ScaledHost() override;

        juce::Rectangle<int> getLayoutBounds() const { return layout; }

        // Replaces the overlay; nullptr removes it. Whether it is visible is
        // up to the overlay.
        void setOverlay( juce::Component* overlay );

        FrameTiming& getFrameTiming() { return frameTiming; }

        void paint( juce::Graphics& g ) override;
        void paintOverChildren( juce::Graphics& g ) override;
        void resized() override;
        void childBoundsChanged( juce::Component* child ) override;

        // The overlay's distance from the corner, in layout units.
        static constexpr int overlayMargin = 6;

    private:
        // Behind the content, painted first in every frame (see paint()).
        class FrameStart : public juce::Component
        {
        public:
            explicit FrameStart( FrameTiming& timing );
            void paint( juce::Graphics& ) override;

        private:
            FrameTiming& frameTiming;
        };

        void placeOverlay();

        juce::Component&     content;
        juce::Rectangle<int> layout;

        juce::Component::SafePointer<juce::Component> overlay;
        FrameTiming                                   frameTiming;
        FrameStart                                    frameStart{ frameTiming };

        JUCE_DECLARE_NON_COPYABLE( ScaledHost )
    };
}
