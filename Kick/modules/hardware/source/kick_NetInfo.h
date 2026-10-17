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

// The state of the network interfaces, read from the kernel: the interface
// names from sysfs, the addresses with getifaddrs(3) and the default
// gateway from /proc/net/route; and the DNS servers, which the kernel does
// not know, from systemd-networkd and systemd-resolved's state files.
// Nothing here changes the configuration, which is the system's network
// manager's job.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <istream>
#include <sstream>
#include <string>
#include <vector>

#include "kick_SysFile.h"

#if defined( __linux__ )
    #include <arpa/inet.h>
    #include <dirent.h>
    #include <ifaddrs.h>
    #include <net/if_arp.h>
    #include <net/route.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

// The Ethernet and Wi-Fi interfaces; the build system can name them (see
// CMakeLists.txt). Empty: the first one of each kind the kernel has.
#ifndef KICK_ETH_IFACE
    #define KICK_ETH_IFACE ""
#endif
#ifndef KICK_WIFI_IFACE
    #define KICK_WIFI_IFACE ""
#endif

namespace mt
{
    namespace net
    {
        struct Address
        {
            std::string address = "0.0.0.0";
            std::string netmask = "0.0.0.0";
        };

        inline std::string readSysfsValue( const std::string& path )
        {
            return sysfile::readLine( path );
        }

        inline bool pathExists( const std::string& path )
        {
#if defined( __linux__ )
            return access( path.c_str(), F_OK ) == 0;
#else
            return false;
#endif
        }

        /** The network interfaces, sorted by name. */
        inline std::vector<std::string> interfaces()
        {
            std::vector<std::string> names;
#if defined( __linux__ )
            if( DIR* dir = opendir( "/sys/class/net" ) )
            {
                while( const dirent* entry = readdir( dir ) )
                {
                    const std::string name = entry->d_name;
                    if( name != "." && name != ".." )
                        names.push_back( name );
                }
                closedir( dir );
            }
            std::sort( names.begin(), names.end() );
#endif
            return names;
        }

        /** Whether an interface is a Wi-Fi (cfg80211) one. */
        inline bool isWireless( const std::string& name )
        {
            const std::string dir = "/sys/class/net/" + name;
            return pathExists( dir + "/wireless" ) || pathExists( dir + "/phy80211" );
        }

        /** Whether an interface is an Ethernet one: of the Ethernet hardware
            type and not Wi-Fi, which has the same type. */
        inline bool isEthernet( const std::string& name )
        {
#if defined( __linux__ )
            return readSysfsValue( "/sys/class/net/" + name + "/type" ) == std::to_string( ARPHRD_ETHER ) && !isWireless( name );
#else
            return false;
#endif
        }

        /** The Wi-Fi interface: KICK_WIFI_IFACE, or the first Wi-Fi interface
            there is. Empty if there is none. */
        inline std::string wifiInterface()
        {
            if( std::string( KICK_WIFI_IFACE ).size() > 0 )
                return KICK_WIFI_IFACE;

            for( const auto& name : interfaces() )
                if( isWireless( name ) )
                    return name;

            return {};
        }

        /** The Ethernet interface: KICK_ETH_IFACE, or the first Ethernet
            interface backed by a device (so not a bridge, veth or other
            virtual one), or else the first Ethernet interface there is.
            Empty if there is none. */
        inline std::string ethernetInterface()
        {
            if( std::string( KICK_ETH_IFACE ).size() > 0 )
                return KICK_ETH_IFACE;

            std::string fallback;
            for( const auto& name : interfaces() )
            {
                if( !isEthernet( name ) )
                    continue;
                if( pathExists( "/sys/class/net/" + name + "/device" ) )
                    return name;
                if( fallback.empty() )
                    fallback = name;
            }

            return fallback;
        }

        /** Whether the interface's operational state is up. */
        inline bool isUp( const std::string& name )
        {
            return !name.empty() && readSysfsValue( "/sys/class/net/" + name + "/operstate" ) == "up";
        }

        /** Whether the interface has a carrier (a cable, for Ethernet). */
        inline bool hasCarrier( const std::string& name )
        {
            return !name.empty() && readSysfsValue( "/sys/class/net/" + name + "/carrier" ) == "1";
        }

        inline std::string ipv4ToString( uint32_t networkOrder )
        {
#if defined( __linux__ )
            char           text[INET_ADDRSTRLEN] = {};
            struct in_addr addr {};
            addr.s_addr = networkOrder;
            if( inet_ntop( AF_INET, &addr, text, sizeof( text ) ) != nullptr )
                return text;
#endif
            return "0.0.0.0";
        }

        /** The interface's first IPv4 address and its netmask. */
        inline Address ipv4Address( const std::string& name )
        {
            Address result;
#if defined( __linux__ )
            struct ifaddrs* list = nullptr;
            if( name.empty() || getifaddrs( &list ) != 0 )
                return result;

            for( const struct ifaddrs* ifa = list; ifa != nullptr; ifa = ifa->ifa_next )
            {
                if( ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET || name != ifa->ifa_name )
                    continue;

                result.address = ipv4ToString( reinterpret_cast<const sockaddr_in*>( ifa->ifa_addr )->sin_addr.s_addr );
                if( ifa->ifa_netmask != nullptr )
                    result.netmask = ipv4ToString( reinterpret_cast<const sockaddr_in*>( ifa->ifa_netmask )->sin_addr.s_addr );
                break;
            }

            freeifaddrs( list );
#endif
            return result;
        }

        /** The default IPv4 gateway through the interface, from the contents
            of /proc/net/route, or an empty string if there is none. Of
            several, the one with the lowest metric. */
        inline std::string parseDefaultGateway( std::istream& routes, const std::string& name )
        {
            std::string   gateway;
            unsigned long bestMetric = 0;

            std::string line;
            std::getline( routes, line ); // the column titles
            while( std::getline( routes, line ) )
            {
                // Iface Destination Gateway Flags RefCnt Use Metric Mask ...
                std::istringstream fields( line );
                std::string        iface, destination, via, flags, refCount, use, metric, mask;
                if( !( fields >> iface >> destination >> via >> flags >> refCount >> use >> metric >> mask ) )
                    continue;

                // The addresses are the kernel's network order values,
                // printed as hexadecimal numbers.
                const unsigned long flagBits = std::strtoul( flags.c_str(), nullptr, 16 );
                if( iface != name || std::strtoul( destination.c_str(), nullptr, 16 ) != 0 || std::strtoul( mask.c_str(), nullptr, 16 ) != 0
                    || ( flagBits & 0x1 ) == 0 /* RTF_UP */ || ( flagBits & 0x2 ) == 0 /* RTF_GATEWAY */ )
                    continue;

                const unsigned long routeMetric = std::strtoul( metric.c_str(), nullptr, 10 );
                if( gateway.empty() || routeMetric < bestMetric )
                {
                    gateway    = ipv4ToString( (uint32_t)std::strtoul( via.c_str(), nullptr, 16 ) );
                    bestMetric = routeMetric;
                }
            }

            return gateway;
        }

        /** The interface's default IPv4 gateway, or an empty string. */
        inline std::string defaultGateway( const std::string& name )
        {
            std::string contents;
            sysfile::read( "/proc/net/route", contents );
            std::istringstream routes( contents );
            return parseDefaultGateway( routes, name );
        }
    
        /** An IPv4 address as networkd and resolved write a DNS server
            ("1.2.3.4", "1.2.3.4:53", "1.2.3.4%2#dns.example"), without the
            port, interface and server name; empty if it is not IPv4. */
        inline std::string ipv4OfServer( const std::string& server )
        {
            const std::string address = server.substr( 0, server.find_first_of( ":%#" ) );
#if defined( __linux__ )
            struct in_addr parsed {};
            if( !address.empty() && inet_pton( AF_INET, address.c_str(), &parsed ) == 1 )
                return address;
#endif
            return {};
        }

        /** The IPv4 DNS servers of a link, from the contents of networkd's
            state file for it (/run/systemd/netif/links/<ifindex>): its
            DNS= line has the configured and the DHCP-acquired ones. */
        inline std::vector<std::string> parseNetworkdDns( const std::string& contents )
        {
            std::vector<std::string> servers;
            std::istringstream       lines( contents );
            for( std::string line; std::getline( lines, line ); )
            {
                if( line.rfind( "DNS=", 0 ) != 0 )
                    continue;
                std::istringstream fields( line.substr( 4 ) );
                for( std::string server; fields >> server; )
                {
                    const auto address = ipv4OfServer( server );
                    if( !address.empty() )
                        servers.push_back( address );
                }
            }
            return servers;
        }

        /** The IPv4 name servers in a resolv.conf, but for the local stub
            resolver of systemd-resolved (127.0.0.53, 127.0.0.54), which
            says nothing about the network's servers. */
        inline std::vector<std::string> parseResolvConf( const std::string& contents )
        {
            std::vector<std::string> servers;
            std::istringstream       lines( contents );
            for( std::string line; std::getline( lines, line ); )
            {
                std::istringstream fields( line );
                std::string        keyword, server;
                if( !( fields >> keyword >> server ) || keyword != "nameserver" )
                    continue;
                const auto address = ipv4OfServer( server );
                if( !address.empty() && address != "127.0.0.53" && address != "127.0.0.54" )
                    servers.push_back( address );
            }
            return servers;
        }

        /** The interface's first IPv4 DNS server, or an empty string. The
            link's own servers come first, from networkd's state file; then
            the servers resolved uses (/run/systemd/resolve/resolv.conf, the
            real ones even when /etc/resolv.conf points at its stub); then
            /etc/resolv.conf, for a system without either. root prefixes
            every path, for tests. */
        inline std::string dnsServer( const std::string& name, const std::string& root = "" )
        {
            std::string contents;

            const std::string index = sysfile::readLine( root + "/sys/class/net/" + name + "/ifindex" );
            if( !name.empty() && !index.empty() && sysfile::read( root + "/run/systemd/netif/links/" + index, contents ) )
            {
                const auto servers = parseNetworkdDns( contents );
                if( !servers.empty() )
                    return servers.front();
            }

            for( const char* file : { "/run/systemd/resolve/resolv.conf", "/etc/resolv.conf" } )
            {
                if( !sysfile::read( root + file, contents ) )
                    continue;
                const auto servers = parseResolvConf( contents );
                if( !servers.empty() )
                    return servers.front();
            }

            return {};
        }
    }
}
