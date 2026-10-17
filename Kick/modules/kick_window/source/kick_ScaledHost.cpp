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

namespace kick::window
{
    ScaledHost::FrameStart::FrameStart( FrameTiming& timing )
        : frameTiming( timing )
    {
        setInterceptsMouseClicks( false, false );
        setAccessible( false );
    }

    void ScaledHost::FrameStart::paint( juce::Graphics& )
    {
        frameTiming.paintStarted();
    }

    ScaledHost::ScaledHost( juce::Component& contentToShow )
        : content( contentToShow ),
          layout( 0, 0, contentToShow.getWidth(), contentToShow.getHeight() )
    {
        setOpaque( true );
        addAndMakeVisible( frameStart );
        content.setTransform( {} );
        content.setBounds( layout );
        addAndMakeVisible( content );
    }

    ScaledHost::~ScaledHost()
    {
        setOverlay( nullptr );
        removeChildComponent( &content );
        content.setTransform( {} );
    }

    void ScaledHost::setOverlay( juce::Component* newOverlay, juce::Point<int> margin )
    {
        if( overlay == newOverlay && overlayCornerMargin == margin )
            return;

        overlayCornerMargin = margin;
        if( overlay == newOverlay )
        {
            placeOverlay();
            return;
        }

        if( overlay != nullptr )
        {
            removeChildComponent( overlay );
            overlay->setTransform( {} );
        }

        overlay = newOverlay;
        if( overlay == nullptr )
            return;

        // Above the content, whatever is brought to the front in there.
        overlay->setAlwaysOnTop( true );
        addChildComponent( overlay );
        placeOverlay();
    }

    void ScaledHost::paint( juce::Graphics& g )
    {
        // The bars around the layout.
        g.fillAll( juce::Colours::black );
    }

    void ScaledHost::paintOverChildren( juce::Graphics& )
    {
        // A frame is timed from the first child painted to here. JUCE leaves
        // out paint() when opaque components in the content cover all that
        // is repainted, but paints the children it does not cover (the
        // transparent frame start, behind everything) and then always
        // paintOverChildren(), with either renderer.
        frameTiming.paintFinished();
    }

    void ScaledHost::resized()
    {
        frameStart.setBounds( getLocalBounds() );
        content.setTransform( computeLetterbox( getLocalBounds(), layout ).transform );
        placeOverlay();
    }

    void ScaledHost::childBoundsChanged( juce::Component* child )
    {
        if( child != nullptr && child == overlay.getComponent() )
            placeOverlay();
    }

    void ScaledHost::placeOverlay()
    {
        if( overlay == nullptr )
            return;

        // The overlay is sized in layout units, at the origin; the transform
        // puts it in the corner. (Moving it back to the origin calls
        // childBoundsChanged() again, which then changes nothing.)
        if( overlay->getPosition() != juce::Point<int>() )
            overlay->setTopLeftPosition( 0, 0 );
        overlay->setTransform( placeInBottomRight( getLocalBounds(), layout,
                                                   { overlay->getWidth(), overlay->getHeight() }, overlayCornerMargin ) );
    }
}
