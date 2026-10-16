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

// The operating system's identification, os-release(5).

#include <map>
#include <sstream>
#include <string>

#include "kick_SysFile.h"

namespace mt
{
    namespace osrelease
    {
        /** Parses os-release's format: KEY=value lines, the value optionally
            in single quotes (taken literally) or double quotes (where a
            backslash escapes \, ", $ and `), and # comments. */
        inline std::map<std::string, std::string> parse( const std::string& contents )
        {
            std::map<std::string, std::string> fields;
            std::istringstream                 lines( contents );
            for( std::string line; std::getline( lines, line ); )
            {
                const auto start = line.find_first_not_of( " \t" );
                if( start == std::string::npos || line[start] == '#' )
                    continue;

                const auto equals = line.find( '=', start );
                if( equals == std::string::npos )
                    continue;

                const std::string key = line.substr( start, equals - start );
                std::string       raw = line.substr( equals + 1 );
                raw.erase( raw.find_last_not_of( " \t\r" ) + 1 );

                std::string value;
                if( !raw.empty() && ( raw[0] == '"' || raw[0] == '\'' ) )
                {
                    const char quote = raw[0];
                    for( std::size_t i = 1; i < raw.size() && raw[i] != quote; ++i )
                    {
                        if( quote == '"' && raw[i] == '\\' && i + 1 < raw.size() && std::string( "\\\"$`" ).find( raw[i + 1] ) != std::string::npos )
                            ++i;
                        value += raw[i];
                    }
                }
                else
                    value = raw;

                fields[key] = value;
            }
            return fields;
        }

        /** The fields of the image's os-release. /usr/lib/os-release comes
            first, unlike os-release(5)'s order: it is part of the image,
            while /etc may be a writable overlay. */
        inline std::map<std::string, std::string> read()
        {
            std::string contents;
            if( sysfile::read( "/usr/lib/os-release", contents ) || sysfile::read( "/etc/os-release", contents ) )
                return parse( contents );
            return {};
        }

        /** The version to show for the system: the image's version
            (IMAGE_VERSION), or else the distribution's (VERSION_ID); empty
            if os-release has neither. */
        inline std::string displayVersion( const std::map<std::string, std::string>& fields )
        {
            for( const char* key : { "IMAGE_VERSION", "VERSION_ID" } )
            {
                const auto it = fields.find( key );
                if( it != fields.end() && !it->second.empty() )
                    return it->second;
            }
            return {};
        }
    }
}
