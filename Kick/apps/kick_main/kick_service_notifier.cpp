/*
 * Copyright (c) 2026 Dylan Corrales
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

#include "kick_service_notifier.h"

#if JUCE_LINUX
 #include <cstddef>
 #include <cstdlib>
 #include <cstring>
 #include <sys/socket.h>
 #include <sys/un.h>
 #include <unistd.h>
#endif

namespace kick
{
    namespace main
    {
        KickServiceNotifier::KickServiceNotifier()
        {
#if JUCE_LINUX
            // The protocol names either a filesystem socket ("/run/...") or one
            // in the abstract namespace ("@..."); ignore anything else.
            const char* address = std::getenv( "NOTIFY_SOCKET" );
            if( address != nullptr && ( address[0] == '/' || address[0] == '@' ) )
                socketAddress = address;
#endif
        }

        bool KickServiceNotifier::notify( const juce::String& state ) const
        {
#if JUCE_LINUX
            if( socketAddress.empty() )
                return false;

            sockaddr_un address{};
            if( socketAddress.size() >= sizeof( address.sun_path ) )
                return false;

            address.sun_family = AF_UNIX;
            std::memcpy( address.sun_path, socketAddress.data(), socketAddress.size() );

            // An abstract address starts with a NUL byte instead of the '@'
            // and is exactly as long as its name; a path keeps its terminator.
            const bool abstract = address.sun_path[0] == '@';
            if( abstract )
                address.sun_path[0] = '\0';
            const socklen_t addressLength =
                static_cast<socklen_t>( offsetof( sockaddr_un, sun_path ) + socketAddress.size() + ( abstract ? 0 : 1 ) );

            const int fd = socket( AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0 );
            if( fd < 0 )
                return false;

            const char*   message = state.toRawUTF8();
            const size_t  size    = std::strlen( message );
            const ssize_t sent    = sendto( fd, message, size, MSG_NOSIGNAL,
                                            reinterpret_cast<const sockaddr*>( &address ), addressLength );
            close( fd );
            return sent == static_cast<ssize_t>( size );
#else
            juce::ignoreUnused( state );
            return false;
#endif
        }
    }
}
