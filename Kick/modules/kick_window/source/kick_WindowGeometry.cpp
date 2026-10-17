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

#include "kick_WindowGeometry.h"

namespace kick::window
{
    namespace
    {
        // The height that keeps the layout's aspect ratio at this width.
        int heightForWidth( int width, juce::Point<int> layoutSize )
        {
            return juce::roundToInt( (double)width * layoutSize.y / layoutSize.x );
        }

        // How much of the window must be on a display for it to be found
        // and moved again.
        constexpr int minimumVisible = 64;
    }

    Letterbox computeLetterbox( juce::Rectangle<int> area, juce::Rectangle<int> layout )
    {
        Letterbox result;
        if( area.isEmpty() || layout.isEmpty() )
        {
            result.contentArea = layout.toFloat();
            return result;
        }

        auto scale = juce::jmin( (float)area.getWidth() / (float)layout.getWidth(),
                                 (float)area.getHeight() / (float)layout.getHeight() );
        if( std::abs( scale - 1.0f ) < 1.0e-4f )
            scale = 1.0f;

        const auto width  = (float)layout.getWidth() * scale;
        const auto height = (float)layout.getHeight() * scale;

        result.scale  = scale;
        result.offset = { (float)area.getX() + std::round( ( (float)area.getWidth() - width ) / 2.0f ) - (float)layout.getX() * scale,
                          (float)area.getY() + std::round( ( (float)area.getHeight() - height ) / 2.0f ) - (float)layout.getY() * scale };
        result.transform   = juce::AffineTransform::scale( scale ).translated( result.offset );
        result.contentArea = layout.toFloat().transformedBy( result.transform );
        return result;
    }

    juce::AffineTransform placeInBottomRight( juce::Rectangle<int> area, juce::Rectangle<int> layout,
                                              juce::Point<int> panelSize, int margin )
    {
        const auto scale = computeLetterbox( area, layout ).scale;
        const auto x     = (float)area.getRight() - std::round( (float)( panelSize.x + margin ) * scale );
        const auto y     = (float)area.getBottom() - std::round( (float)( panelSize.y + margin ) * scale );
        return juce::AffineTransform::scale( scale ).translated( juce::jmax( x, (float)area.getX() ),
                                                                 juce::jmax( y, (float)area.getY() ) );
    }

    juce::String formatWindowBounds( juce::Rectangle<int> bounds )
    {
        return juce::String( bounds.getX() ) + " " + juce::String( bounds.getY() ) + " "
               + juce::String( bounds.getWidth() ) + " " + juce::String( bounds.getHeight() );
    }

    std::optional<juce::Rectangle<int>> parseWindowBounds( const juce::String& text )
    {
        auto tokens = juce::StringArray::fromTokens( text.trim(), " ", "" );
        tokens.removeEmptyStrings();
        if( tokens.size() != 4 )
            return std::nullopt;

        int values[4];
        for( int i = 0; i < 4; ++i )
        {
            const auto& token  = tokens[i];
            const auto  digits = token.startsWithChar( '-' ) ? token.substring( 1 ) : token;
            if( digits.isEmpty() || !digits.containsOnly( "0123456789" ) || digits.length() > 6 )
                return std::nullopt;
            values[i] = token.getIntValue();
        }

        if( values[2] <= 0 || values[3] <= 0 )
            return std::nullopt;
        return juce::Rectangle<int>( values[0], values[1], values[2], values[3] );
    }

    juce::Rectangle<int> chooseWindowBounds( std::optional<juce::Rectangle<int>> saved,
                                             const juce::Array<juce::Rectangle<int>>& displayAreas,
                                             juce::Rectangle<int>                      preferredArea,
                                             juce::Point<int>                          layoutSize,
                                             juce::Point<int>                          minimumSize )
    {
        if( layoutSize.x <= 0 || layoutSize.y <= 0 )
            return {};

        if( saved.has_value() && saved->getWidth() >= minimumSize.x )
        {
            const auto bounds = saved->withHeight( heightForWidth( saved->getWidth(), layoutSize ) );
            for( const auto& area : displayAreas )
            {
                const auto visible = bounds.getIntersection( area );
                if( visible.getWidth() >= minimumVisible && visible.getHeight() >= minimumVisible )
                    return bounds;
            }
        }

        // The layout's size, or what fits in 90% of the display.
        auto width = layoutSize.x;
        if( !preferredArea.isEmpty() )
        {
            width = juce::jmin( width,
                                preferredArea.getWidth() * 9 / 10,
                                (int)( (double)preferredArea.getHeight() * 0.9 * layoutSize.x / layoutSize.y ) );
        }
        width = juce::jmax( width, minimumSize.x );

        const auto size = juce::Rectangle<int>( width, heightForWidth( width, layoutSize ) );
        return preferredArea.isEmpty() ? size : size.withCentre( preferredArea.getCentre() );
    }
}
