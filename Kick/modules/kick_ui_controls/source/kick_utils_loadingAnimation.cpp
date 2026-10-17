/*
 * Copyright (c) 2021 MUSIC Tribe
 *
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

///
/// KickUtils...
///

namespace mt
{
    namespace utils
    {
        std::vector<juce::Image> LoadAnimation::gifImages;
        LoadAnimation::LoadAnimation()
        {
            ui::themeChanged.connect( [&]() {
                gifImages.clear();
                loadImages();
            } );

            loadImages();
        }

        LoadAnimation ::~LoadAnimation()
        {
            DBG( "LoadAnimation Destructor" );
            stopTimer();
        }

        void LoadAnimation::loadImages()
        {
            if( 0 == gifImages.size() )
            {
                gifImages.reserve( 14 );

                for( auto idx = 1; idx < 14; ++idx )
                {
                    auto text = juce::String( idx ).paddedLeft( '0', 2 );
                    gifImages.push_back( juce::ImageCache::getFromFile( ui::Theme::getAssetsDirectory().getChildFile( "animation/Load Animation B - " + text + ".png" ) ) );
                }
            }
        }

        void LoadAnimation::startAnimation()
        {
            id          = 0;
            isAnimating = true;

            // Already showing: a second pop-up would take the animation
            // from the first, which would then never close, and stay
            // modal over the whole screen.
            if( popup == nullptr )
            {
                popup = showPopup( this, { 0, 0, 80, 80 }, []( int R ) {} );
                // Not shown (it could not be seen): the pop-up is free for
                // others, and closing it later could close one of theirs.
                if( !popup->isInUse )
                    popup = nullptr;
            }

            startTimerHz( 22 );
        }

        void LoadAnimation::stopAnimation()
        {
            isAnimating = false;
            close();
        }

        void LoadAnimation::stopAnimationAndFree()
        {
            stopAnimation();
            if( this )
                delete this;
        }

        void LoadAnimation::paint( juce::Graphics& g )
        {
            if( isAnimating )
                g.drawImage( gifImages[id], getLocalBounds().toFloat() );
        }

        void LoadAnimation::timerCallback()
        {
            ++id;
            if( id >= gifImages.size() )
                id = 0;

            repaint();
        }

        void LoadAnimation::close()
        {
            stopTimer();
            if( popup )
            {
                popup->close();
                popup = nullptr;
            }
        }

    } // namespace utils

} // namespace mt
