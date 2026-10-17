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

    // Where a panel panelSize layout units in size shows in an area that
    // shows the layout: scaled as the layout is, with its bottom-right
    // corner margin.x layout units in from the right edge of the area and
    // margin.y up from its bottom edge (not of the letterboxed layout: of
    // the window, the display in kiosk mode), on whole pixels. The
    // transform maps the panel's bounds, at the origin, to that place.
    juce::AffineTransform placeInBottomRight( juce::Rectangle<int> area, juce::Rectangle<int> layout,
                                              juce::Point<int> panelSize, juce::Point<int> margin );

    // A saved window position and size: "x y width height". The position
    // is the one to ask for (the frame's top-left corner, as
    // chooseWindowBounds() gives it; see findPlacementOffset()), the size
    // the client area's.
    juce::String                        formatWindowBounds( juce::Rectangle<int> bounds );
    std::optional<juce::Rectangle<int>> parseWindowBounds( const juce::String& text );

    // How far from the position a desktop window asked for (its frame's
    // top-left corner, as chooseWindowBounds() gives it) the window manager
    // put its client area, from where it was once placed. ICCCM window
    // managers put the frame's top-left corner there, so that the client
    // area is the frame's left and top borders in. Weston's X window
    // manager puts the visible part of its frame there, but the frame it
    // publishes (_NET_FRAME_EXTENTS) includes the shadow around it (32
    // pixels on each side), so the client area is 32 pixels less far in
    // than that frame says: subtracting that frame on saving moved the
    // window 32 pixels up and to the left on every start.
    // An offset no frame can make (beyond the frame's left or top border,
    // or the assumed frame's when it is larger; or negative) means the
    // window manager put the window somewhere else (or the user moved it
    // first): the frame's top-left corner is taken instead.
    juce::Point<int> findPlacementOffset( juce::Point<int> requested, juce::Point<int> placed, juce::BorderSize<int> frame );

    // The bounds to save for a window with this client area, placed with
    // this offset: the position to ask for to get the same client area.
    juce::Rectangle<int> windowBoundsToSave( juce::Rectangle<int> clientArea, juce::Point<int> placementOffset );

    // When a desktop window's bounds are saved: once they have not changed
    // for a while (moves and resizes come in bursts, and each save writes
    // the settings file), and whatever is still waiting when the window
    // goes. Bounds equal to the last ones saved are not saved again.
    // Times are in milliseconds, from any counter that wraps (such as
    // juce::Time::getMillisecondCounter()).
    class BoundsSaving
    {
    public:
        explicit BoundsSaving( int delayInMS );

        // The bounds saved before, if any.
        void setSaved( const juce::String& bounds );

        // The window's bounds are now these.
        void changed( const juce::String& bounds, uint32_t now );

        // The bounds to save now, if they have been waiting for the delay.
        std::optional<juce::String> takeDue( uint32_t now );

        // The bounds to save now that the window goes, if any are waiting.
        std::optional<juce::String> flush();

        bool isWaiting() const { return waiting; }

        // How long until the waiting bounds are due (0: they are).
        int getTimeUntilDue( uint32_t now ) const;

    private:
        const int    delay;
        juce::String saved, pending;
        bool         waiting{ false };
        uint32_t     changedAt{ 0 };
    };

    // The room the window manager's frame takes around a desktop window's
    // client area (the title bar at the top). The real frame is only known
    // once the window is shown, so windows are placed as if they had this
    // one.
    inline const juce::BorderSize<int> assumedWindowFrame{ 40, 4, 4, 4 };

    // Kept free at the top of a display for a panel, such as the one of
    // Weston's desktop shell: on Linux JUCE's user area of a display is all
    // of it, and Weston publishes no work area, so a window at the top of
    // the display would have its title bar under the panel, out of reach.
    constexpr int displayTopReserve = 32;

    // Whether a desktop window's client area covers a whole display: the
    // shell made it full screen (Weston's kiosk shell does that to every
    // window). Such bounds are not the window's own and are not kept.
    bool fillsADisplay( juce::Rectangle<int>                      bounds,
                        const juce::Array<juce::Rectangle<int>>& displayAreas );

    // The bounds a desktop window should have after the window manager
    // resized it from previous to bounds: the layout's aspect ratio, within
    // the minimum and maximum size. The dimension the user changed more
    // (relative to the layout's) is kept, and so are the edges the user
    // did not drag (the bottom and right ones when the top or left one
    // moved alone). Bounds within a pixel of the aspect ratio are returned
    // as they are.
    juce::Rectangle<int> keepAspectRatio( juce::Rectangle<int> bounds,
                                          juce::Rectangle<int> previous,
                                          juce::Point<int>     layoutSize,
                                          juce::Point<int>     minimumSize,
                                          juce::Point<int>     maximumSize );

    // Where a desktop window showing the layout opens. Window managers put
    // the top-left corner of a window's frame where the window asks to be,
    // so the result is that corner, with the client area's size; the frame
    // (assumedWindowFrame) is kept inside the display, below
    // displayTopReserve, so that the title bar can be reached.
    // At the saved bounds (with the layout's aspect ratio) when they are
    // large enough and on a display, made smaller if they do not fit on it
    // and moved onto it; otherwise at the layout's size, or 90% of what
    // fits if the display is smaller, centred in preferredArea (the display
    // under the mouse).
    juce::Rectangle<int> chooseWindowBounds( std::optional<juce::Rectangle<int>> saved,
                                             const juce::Array<juce::Rectangle<int>>& displayAreas,
                                             juce::Rectangle<int>                      preferredArea,
                                             juce::Point<int>                          layoutSize,
                                             juce::Point<int>                          minimumSize );
}
