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

// How values are written for the user, the same way on every screen. Only
// juce_core is needed, so the formats can be tested without a display.

namespace mt
{
    namespace display
    {
        /** A build's date and time as "2026-10-17 02:28", from the
            compiler's __DATE__ ("Oct 17 2026") and __TIME__ ("02:28:15"),
            or the text as it is if it is not in that form. */
        inline juce::String buildDate( const juce::String& date, const juce::String& time )
        {
            static const char* const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

            // A day below 10 is padded with a space: "Oct  7 2026".
            auto parts = juce::StringArray::fromTokens( date, " ", "" );
            parts.removeEmptyStrings();
            int month = 0;
            for( int i = 0; i < 12 && parts.size() == 3; ++i )
                if( parts[0] == months[i] )
                    month = i + 1;

            const auto clock = juce::StringArray::fromTokens( time, ":", "" );
            if( month == 0 || clock.size() < 2 || !parts[1].containsOnly( "0123456789" ) || !parts[2].containsOnly( "0123456789" ) )
                return ( date + " " + time ).trim();

            return juce::String::formatted( "%04d-%02d-%02d %02d:%02d", parts[2].getIntValue(), month, parts[1].getIntValue(),
                                            clock[0].getIntValue(), clock[1].getIntValue() );
        }

        /** A build time in the timestamp library's form ("20261017022815")
            as "2026-10-17 02:28", or the text as it is. */
        inline juce::String buildTimestamp( const juce::String& stamp )
        {
            if( stamp.length() < 12 || !stamp.containsOnly( "0123456789" ) )
                return stamp;
            return stamp.substring( 0, 4 ) + "-" + stamp.substring( 4, 6 ) + "-" + stamp.substring( 6, 8 ) + " "
                   + stamp.substring( 8, 10 ) + ":" + stamp.substring( 10, 12 );
        }

        /** What an unknown value reads as. */
        inline juce::String unavailable()
        {
            return "Unavailable";
        }

        /** A serial number, or unavailable() if there is none: empty, or
            all zeros, which is what an unprogrammed one reads. */
        inline juce::String serialNumber( const juce::String& serial )
        {
            const auto trimmed = serial.trim();
            return trimmed.isEmpty() || trimmed.containsOnly( "0" ) ? unavailable() : trimmed;
        }

        /** A version, or unavailable() if it is not known. */
        inline juce::String version( const juce::String& text )
        {
            const auto trimmed = text.trim();
            return trimmed.isEmpty() ? unavailable() : trimmed;
        }
    }
}
