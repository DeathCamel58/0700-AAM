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

// A client of wpa_supplicant's control interface: what wpa_cli and
// libwpa_client's wpa_ctrl do, without either.
//
// The control interface is a UNIX datagram socket per network interface,
// <ctrl dir>/<interface>. A client sends a command as one datagram and gets
// the reply as one datagram, sent back to the address the command came
// from. A client that sends ATTACH also gets the events ("<level>EVENT ...")
// on that address until it sends DETACH or goes away.
//
// The client's address is an abstract one the kernel picks (autobind), so it
// needs no directory to create a socket file in and leaves nothing behind.
// The only permission needed is write access to the interface's socket, which
// wpa_supplicant gives to the group named in its ctrl_interface setting.

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#if defined( __linux__ )
    #include <poll.h>
    #include <sys/socket.h>
    #include <sys/un.h>
    #include <time.h>
    #include <unistd.h>
#endif

namespace mt
{
    namespace wpa
    {
        class WpaCtrl
        {
        public:
            WpaCtrl() = default;
            ~WpaCtrl() { close(); }

            WpaCtrl( const WpaCtrl& )            = delete;
            WpaCtrl& operator=( const WpaCtrl& ) = delete;

            /** Connects to the control socket at path. */
            bool open( const std::string& path )
            {
                close();
#if defined( __linux__ )
                struct sockaddr_un remote {};
                if( path.empty() || path.size() >= sizeof( remote.sun_path ) )
                    return false;

                fd = socket( AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0 );
                if( fd < 0 )
                    return false;

                // Binding only the address family makes the kernel pick a
                // unique abstract address.
                struct sockaddr_un local {};
                local.sun_family = AF_UNIX;
                remote.sun_family = AF_UNIX;
                std::memcpy( remote.sun_path, path.c_str(), path.size() );

                if( bind( fd, (struct sockaddr*)&local, sizeof( sa_family_t ) ) != 0
                    || connect( fd, (struct sockaddr*)&remote, sizeof( remote ) ) != 0 )
                {
                    close();
                    return false;
                }
                return true;
#else
                return false;
#endif
            }

            void close()
            {
#if defined( __linux__ )
                if( fd >= 0 )
                {
                    if( attached )
                    {
                        std::string reply;
                        request( "DETACH", reply, 1000 );
                    }
                    ::close( fd );
                }
#endif
                fd       = -1;
                attached = false;
            }

            bool isOpen() const { return fd >= 0; }

            /** Sends a command and waits up to timeoutMs for its reply.
                Events that arrive in the meantime are skipped. */
            bool request( const std::string& command, std::string& reply, int timeoutMs = 10000 )
            {
                reply.clear();
#if defined( __linux__ )
                if( fd < 0 )
                    return false;

                ssize_t sent;
                do
                    sent = send( fd, command.data(), command.size(), MSG_NOSIGNAL );
                while( sent < 0 && errno == EINTR );
                if( sent != (ssize_t)command.size() )
                    return false;

                const int64_t deadline = nowMs() + timeoutMs;
                for( ;; )
                {
                    if( !receive( reply, (int)std::max<int64_t>( 0, deadline - nowMs() ) ) )
                        return false;
                    if( !isEvent( reply ) )
                        return true;
                }
#else
                (void)command;
                (void)timeoutMs;
                return false;
#endif
            }

            /** The reply to a command, or an empty string if there was none. */
            std::string request( const std::string& command, int timeoutMs = 10000 )
            {
                std::string reply;
                request( command, reply, timeoutMs );
                return reply;
            }

            /** Sends a command that answers "OK". */
            bool command( const std::string& command )
            {
                return request( command ) == "OK\n";
            }

            /** Asks for the events to be sent to this connection. */
            bool attach()
            {
                attached = command( "ATTACH" );
                return attached;
            }

            /** Waits up to timeoutMs for a datagram (an event, once attached). */
            bool receive( std::string& message, int timeoutMs )
            {
                message.clear();
#if defined( __linux__ )
                if( fd < 0 )
                    return false;

                struct pollfd pfd {};
                pfd.fd     = fd;
                pfd.events = POLLIN;

                const int64_t deadline = nowMs() + timeoutMs;
                for( ;; )
                {
                    const int ready = poll( &pfd, 1, (int)std::max<int64_t>( 0, deadline - nowMs() ) );
                    if( ready < 0 && errno == EINTR )
                        continue;
                    if( ready <= 0 )
                        return false;

                    // Replies are at most a few kilobytes (scan results are
                    // the longest); a longer one is truncated.
                    char          buffer[16384];
                    const ssize_t n = recv( fd, buffer, sizeof( buffer ), 0 );
                    if( n < 0 && ( errno == EINTR || errno == EAGAIN ) )
                        continue;
                    if( n < 0 )
                        return false;

                    message.assign( buffer, (size_t)n );
                    return true;
                }
#else
                (void)timeoutMs;
                return false;
#endif
            }

            /** Whether a message is an event: "<level>" and the event. */
            static bool isEvent( const std::string& message ) { return !message.empty() && message[0] == '<'; }

            /** An event without its "<level>" prefix. */
            static std::string eventText( const std::string& message )
            {
                const auto end = message.find( '>' );
                return isEvent( message ) && end != std::string::npos ? message.substr( end + 1 ) : message;
            }

        private:
            static int64_t nowMs()
            {
#if defined( __linux__ )
                struct timespec ts {};
                clock_gettime( CLOCK_MONOTONIC, &ts );
                return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#else
                return 0;
#endif
            }

            int  fd       = -1;
            bool attached = false;
        };

        inline int hexDigit( char c )
        {
            if( c >= '0' && c <= '9' )
                return c - '0';
            if( c >= 'a' && c <= 'f' )
                return c - 'a' + 10;
            if( c >= 'A' && c <= 'F' )
                return c - 'A' + 10;
            return -1;
        }

        /** Undoes the escaping wpa_supplicant applies to SSIDs in its replies
            (printf_encode(): \\, \", \e, \n, \r, \t and \xNN for any other
            byte outside printable ASCII), giving the SSID's bytes. */
        inline std::string decodeSsid( const std::string& text )
        {
            std::string bytes;
            for( std::size_t i = 0; i < text.size(); ++i )
            {
                if( text[i] != '\\' || i + 1 >= text.size() )
                {
                    bytes += text[i];
                    continue;
                }

                const char c = text[++i];
                switch( c )
                {
                    case '\\': bytes += '\\'; break;
                    case '"': bytes += '"'; break;
                    case 'n': bytes += '\n'; break;
                    case 'r': bytes += '\r'; break;
                    case 't': bytes += '\t'; break;
                    case 'e': bytes += '\033'; break;
                    case 'x':
                    {
                        int value  = 0;
                        int digits = 0;
                        while( digits < 2 && i + 1 < text.size() && hexDigit( text[i + 1] ) >= 0 )
                        {
                            value = value * 16 + hexDigit( text[++i] );
                            ++digits;
                        }
                        if( digits > 0 )
                            bytes += (char)value;
                        break;
                    }
                    default:
                        if( c >= '0' && c <= '7' )
                        {
                            int value  = c - '0';
                            int digits = 1;
                            while( digits < 3 && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '7' )
                            {
                                value = value * 8 + ( text[++i] - '0' );
                                ++digits;
                            }
                            bytes += (char)value;
                        }
                        break;
                }
            }
            return bytes;
        }

        /** Bytes as lowercase hexadecimal, how SSIDs are given to SET_NETWORK
            so that no byte in them needs quoting. */
        inline std::string toHex( const std::string& bytes )
        {
            static const char digits[] = "0123456789abcdef";
            std::string       hex;
            for( char byte : bytes )
            {
                const auto c = static_cast<unsigned char>( byte );
                hex += digits[c >> 4];
                hex += digits[c & 0xf];
            }
            return hex;
        }

        inline std::vector<std::string> splitTabs( const std::string& line )
        {
            std::vector<std::string> fields;
            std::size_t              start = 0;
            for( ;; )
            {
                const auto tab = line.find( '\t', start );
                fields.push_back( line.substr( start, tab - start ) );
                if( tab == std::string::npos )
                    return fields;
                start = tab + 1;
            }
        }

        struct ScanResult
        {
            std::string bssid;
            int         frequency = 0;
            int         signal    = 0;
            std::string flags;
            std::string ssid; // the SSID's bytes

            bool isOpen() const { return flags.find( "WPA" ) == std::string::npos && flags.find( "RSN" ) == std::string::npos && flags.find( "WEP" ) == std::string::npos; }
            bool hasSae() const { return flags.find( "SAE" ) != std::string::npos; }
            bool hasPsk() const { return flags.find( "PSK" ) != std::string::npos; }
        };

        /** Parses a SCAN_RESULTS reply: a title line, then
            "bssid \t frequency \t signal level \t flags \t ssid" per access
            point. Hidden networks (an empty SSID) are left out. */
        inline std::vector<ScanResult> parseScanResults( const std::string& reply )
        {
            std::vector<ScanResult> results;
            std::istringstream      lines( reply );
            std::string             line;
            std::getline( lines, line );
            while( std::getline( lines, line ) )
            {
                const auto fields = splitTabs( line );
                if( fields.size() < 5 || fields[4].empty() )
                    continue;

                ScanResult result;
                result.bssid     = fields[0];
                result.frequency = std::atoi( fields[1].c_str() );
                result.signal    = std::atoi( fields[2].c_str() );
                result.flags     = fields[3];
                result.ssid      = decodeSsid( fields[4] );
                if( !result.ssid.empty() )
                    results.push_back( result );
            }
            return results;
        }

        struct ConfiguredNetwork
        {
            int         id = -1;
            std::string ssid; // the SSID's bytes
            std::string flags;

            bool isDisabled() const { return flags.find( "[DISABLED]" ) != std::string::npos; }
            bool isP2pGroup() const { return flags.find( "[P2P-PERSISTENT]" ) != std::string::npos; }
        };

        /** Parses a LIST_NETWORKS reply: a title line, then
            "network id \t ssid \t bssid \t flags" per network. */
        inline std::vector<ConfiguredNetwork> parseNetworkList( const std::string& reply )
        {
            std::vector<ConfiguredNetwork> networks;
            std::istringstream             lines( reply );
            std::string                    line;
            std::getline( lines, line );
            while( std::getline( lines, line ) )
            {
                const auto fields = splitTabs( line );
                if( fields.size() < 2 || fields[0].empty() || fields[0].find_first_not_of( "0123456789" ) != std::string::npos )
                    continue;

                networks.push_back( { std::atoi( fields[0].c_str() ), decodeSsid( fields[1] ), fields.size() > 3 ? fields[3] : std::string() } );
            }
            return networks;
        }

        /** Whether a reply is a network id, as ADD_NETWORK answers. */
        inline bool isNetworkId( const std::string& reply )
        {
            const auto id = reply.substr( 0, reply.find( '\n' ) );
            return !id.empty() && id.find_first_not_of( "0123456789" ) == std::string::npos;
        }

        // wpa_supplicant's replies are at most 4096 bytes, and what does not
        // fit is left out. Lists that can be longer are read a piece at a
        // time.

        /** The networks in wpa_supplicant's configuration. */
        inline std::vector<ConfiguredNetwork> listNetworks( WpaCtrl& ctrl )
        {
            std::vector<ConfiguredNetwork> networks;
            std::string                    command = "LIST_NETWORKS";
            for( ;; )
            {
                const auto page = parseNetworkList( ctrl.request( command ) );
                if( page.empty() || ( !networks.empty() && page.front().id <= networks.back().id ) )
                    return networks;

                networks.insert( networks.end(), page.begin(), page.end() );
                command = "LIST_NETWORKS LAST_ID=" + std::to_string( networks.back().id );
            }
        }

        /** The highest priority of the configured networks other than
            except, or 0 if there are none. */
        inline int highestPriority( WpaCtrl& ctrl, int except )
        {
            int highest = 0;
            for( const auto& network : listNetworks( ctrl ) )
                if( network.id != except )
                    highest = std::max( highest, std::atoi( ctrl.request( "GET_NETWORK " + std::to_string( network.id ) + " priority" ).c_str() ) );
            return highest;
        }

        /** Connects to the network id now, and keeps the others enabled for
            joining automatically. SELECT_NETWORK disables every other network,
            and a later SAVE_CONFIG would store them so, leaving them disabled
            for good. Enabling them with "no-connect" changes nothing else: the
            connection to id goes on, as long as its priority is the highest
            of them all. */
        inline bool selectNetwork( WpaCtrl& ctrl, int id )
        {
            const bool selected = ctrl.command( "SELECT_NETWORK " + std::to_string( id ) );
            for( const auto& network : listNetworks( ctrl ) )
                if( network.id != id && network.isDisabled() && !network.isP2pGroup() )
                    ctrl.command( "ENABLE_NETWORK " + std::to_string( network.id ) + " no-connect" );
            return selected;
        }
    }
}
