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

#pragma once

#include "juce_core/juce_core.h"

#if JUCE_LINUX
 #include <arpa/inet.h>
 #include <cstdlib>
 #include <dirent.h>
 #include <fcntl.h>
 #include <netinet/in.h>
 #include <sys/socket.h>
 #include <sys/stat.h>
#endif

namespace ipc
{
    /*
     * JUCE opens its sockets without SOCK_CLOEXEC, so every program a Kick
     * process runs (wpa_cli, amixer, swupdate-client, ...) inherited its IPC
     * connections and listening socket, and a long-running one kept the
     * port bound after Kick had exited. These mark them close-on-exec once
     * JUCE has created them.
     */
    inline void setCloseOnExec( int fd )
    {
#if JUCE_LINUX
        if( fd < 0 )
            return;
        const int flags = ::fcntl( fd, F_GETFD );
        if( flags >= 0 )
            ::fcntl( fd, F_SETFD, flags | FD_CLOEXEC );
#else
        juce::ignoreUnused( fd );
#endif
    }

    // juce::InterprocessConnectionServer does not expose its listening
    // socket: find the one listening on the port among the open descriptors.
    inline void setListeningSocketCloseOnExec( int port )
    {
#if JUCE_LINUX
        DIR* directory = ::opendir( "/proc/self/fd" );
        if( directory == nullptr )
            return;

        while( const dirent* entry = ::readdir( directory ) )
        {
            if( entry->d_name[0] < '0' || entry->d_name[0] > '9' )
                continue;
            const int fd = std::atoi( entry->d_name );

            struct stat st;
            if( ::fstat( fd, &st ) != 0 || !S_ISSOCK( st.st_mode ) )
                continue;

            int       listening = 0;
            socklen_t length    = sizeof( listening );
            if( ::getsockopt( fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &length ) != 0 || !listening )
                continue;

            sockaddr_storage address{};
            socklen_t        addressLength = sizeof( address );
            if( ::getsockname( fd, reinterpret_cast<sockaddr*>( &address ), &addressLength ) != 0 )
                continue;

            int boundPort = -1;
            if( address.ss_family == AF_INET )
                boundPort = ntohs( reinterpret_cast<const sockaddr_in&>( address ).sin_port );
            else if( address.ss_family == AF_INET6 )
                boundPort = ntohs( reinterpret_cast<const sockaddr_in6&>( address ).sin6_port );

            if( boundPort == port )
                setCloseOnExec( fd );
        }
        ::closedir( directory );
#else
        juce::ignoreUnused( port );
#endif
    }
}
