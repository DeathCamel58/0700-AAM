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

// Reading and writing small kernel files (sysfs, procfs) with plain system
// calls. The descriptors are close-on-exec, unlike std::fstream's, so a
// program another thread starts at the same time does not inherit them.

#include <cerrno>
#include <string>

#if defined( __linux__ )
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace mt
{
    namespace sysfile
    {
        /** The whole contents of a file, or false if it cannot be read. */
        inline bool read( const std::string& path, std::string& contents )
        {
            contents.clear();
#if defined( __linux__ )
            const int fd = open( path.c_str(), O_RDONLY | O_CLOEXEC );
            if( fd < 0 )
                return false;

            char buffer[4096];
            for( ;; )
            {
                const ssize_t n = ::read( fd, buffer, sizeof( buffer ) );
                if( n > 0 )
                    contents.append( buffer, (size_t)n );
                else if( n == 0 || errno != EINTR )
                {
                    close( fd );
                    return n == 0;
                }
            }
#else
            return false;
#endif
        }

        /** The first line of a file without its line end, or an empty string
            if it cannot be read: how sysfs attributes are read. */
        inline std::string readLine( const std::string& path )
        {
            std::string contents;
            read( path, contents );
            return contents.substr( 0, contents.find( '\n' ) );
        }

        /** Writes a value to an existing file in one write(2), as sysfs
            attributes are written. */
        inline bool write( const std::string& path, const std::string& value )
        {
#if defined( __linux__ )
            const int fd = open( path.c_str(), O_WRONLY | O_CLOEXEC );
            if( fd < 0 )
                return false;

            ssize_t n;
            do
                n = ::write( fd, value.data(), value.size() );
            while( n < 0 && errno == EINTR );

            const bool closed = close( fd ) == 0;
            return n == (ssize_t)value.size() && closed;
#else
            (void)path;
            (void)value;
            return false;
#endif
        }
    }
}
