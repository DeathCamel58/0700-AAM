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

// The system's time zone, as systemd's timedated sets it: /etc/localtime, a
// link to the zone's file in the time zone database.

#include <cstring>
#include <string>

#include "kick_SysFile.h"

#if defined( __linux__ )
    #include <climits>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace mt
{
    namespace tz
    {
        /** The zone the time is shown in when nothing says otherwise. */
        constexpr const char* defaultZone = "UTC";

        /** A zone under the name the zone list uses: the database's other
            names for UTC ("Universal", "Etc/UTC", "Zulu", ... and GMT's,
            which differ from it in name only) become "UTC". */
        inline std::string canonical( const std::string& zone )
        {
            for( const char* alias : { "UTC", "UCT", "Universal", "Zulu", "GMT", "GMT0", "GMT+0", "GMT-0", "Greenwich",
                                       "Etc/UTC", "Etc/UCT", "Etc/Universal", "Etc/Zulu", "Etc/GMT", "Etc/GMT0",
                                       "Etc/GMT+0", "Etc/GMT-0", "Etc/Greenwich" } )
                if( zone == alias )
                    return defaultZone;
            return zone;
        }

        /** The zone a link to the time zone database names
            (".../zoneinfo/Europe/Berlin" is "Europe/Berlin"), or an empty
            string if it does not point into it. */
        inline std::string zoneOfLink( const std::string& target )
        {
            const auto pos = target.find( "zoneinfo/" );
            if( pos == std::string::npos )
                return {};

            std::string zone = target.substr( pos + 9 );
            for( const char* prefix : { "posix/", "right/" } )
                if( zone.rfind( prefix, 0 ) == 0 )
                    zone = zone.substr( std::strlen( prefix ) );
            return canonical( zone );
        }

        /** The system's time zone, or an empty string if it cannot be
            told. root prefixes the paths, for tests.

            Without /etc/localtime the C library uses UTC, so that is the
            zone then. A copy of a zone file instead of a link does not say
            which zone it is; /etc/timezone (Debian's way, also written by
            some builds) may. */
        inline std::string systemZone( const std::string& root = "" )
        {
#if defined( __linux__ )
            const std::string localtime = root + "/etc/localtime";

            char          target[PATH_MAX];
            const ssize_t n = readlink( localtime.c_str(), target, sizeof( target ) - 1 );
            if( n > 0 )
            {
                const auto zone = zoneOfLink( std::string( target, (size_t)n ) );
                if( !zone.empty() )
                    return zone;
            }

            const auto named = canonical( sysfile::readLine( root + "/etc/timezone" ) );
            if( !named.empty() )
                return named;

            struct stat st {};
            if( lstat( localtime.c_str(), &st ) != 0 )
                return defaultZone;
#else
            (void)root;
#endif
            return {};
        }
    }
}
