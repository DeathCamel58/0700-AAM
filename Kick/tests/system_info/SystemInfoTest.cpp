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

// Tests of the system information the preferences and the system resources
// window show: CPU and memory use parsed from /proc, and the DNS server
// from systemd-networkd's and systemd-resolved's state files. The parsers
// and lookups are fed
// fixture copies of the files, so the results do not depend on this
// machine. Built with -DKICK_BUILD_TESTS=ON; it needs no arguments, creates
// no window and changes nothing on the system.

#include <juce_core/juce_core.h>

#include "hardware/source/kick_Resorces.cpp"
#include "hardware/source/kick_NetInfo.h"

#include <cstdio>

namespace
{
    int failures = 0;

#define CHECK( condition )                                                      \
    do                                                                          \
    {                                                                           \
        if( !( condition ) )                                                    \
        {                                                                       \
            std::printf( "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #condition ); \
            ++failures;                                                         \
        }                                                                       \
    } while( false )

    bool near( double a, double b )
    {
        return std::abs( a - b ) < 1.0e-6;
    }

    void testCpu()
    {
        using mt::SystemResource;

        mt::cpuTimes earlier, later;
        CHECK( SystemResource::parseCpuTimes( "cpu  100 0 100 700 100 0 0 0 0 0\ncpu0 50 0 50 350 50 0 0 0 0 0\n", earlier ) );
        CHECK( earlier.total == 1000 );
        CHECK( earlier.idle == 800 );

        // 400 more ticks, 100 of them idle: 75 % busy.
        CHECK( SystemResource::parseCpuTimes( "cpu  250 0 250 790 110 0 0 0 0 0\n", later ) );
        CHECK( near( SystemResource::cpuUsageBetween( earlier, later ), 75.0 ) );

        // No time passed, or the counters went back: no figure, not a
        // division by zero.
        CHECK( near( SystemResource::cpuUsageBetween( later, later ), 0.0 ) );
        CHECK( near( SystemResource::cpuUsageBetween( later, earlier ), 0.0 ) );

        // An old kernel's four columns.
        CHECK( SystemResource::parseCpuTimes( "cpu 10 20 30 40\n", later ) );
        CHECK( later.total == 100 && later.idle == 40 );

        // Not /proc/stat.
        CHECK( !SystemResource::parseCpuTimes( "", later ) );
        CHECK( !SystemResource::parseCpuTimes( "intr 1 2 3\n", later ) );

        // The first reading only takes a baseline; the next one compares.
        SystemResource resources;
        double         usage = -1.0;
        CHECK( !resources.cpuInfo( usage ) );
        CHECK( near( usage, 0.0 ) );
        resources.startCpuMeasurement();
        volatile double spin = 0.0;
        for( int i = 0; i < 50000000; ++i )
            spin = spin + 1.0;
        CHECK( resources.cpuInfo( usage ) );
        CHECK( usage >= 0.0 && usage <= 100.0 );
        std::printf( "CPU in use here: %.1f %%\n", usage );
    }

    void testMemory()
    {
        using mt::SystemResource;

        const char* meminfo = "MemTotal:        2048000 kB\n"
                              "MemFree:          100000 kB\n"
                              "MemAvailable:    1536000 kB\n"
                              "Buffers:           20000 kB\n"
                              "Cached:          1200000 kB\n";
        mt::sysRamInfo info;
        CHECK( SystemResource::parseMemInfo( meminfo, info ) );
        CHECK( near( info.totalRam, 2000.0 ) );
        CHECK( near( info.freeRam, 1500.0 ) );
        CHECK( near( info.usedRam, 500.0 ) );
        CHECK( info.perRam == 25 );

        // Nothing read: nothing known, and all zeros, not leftovers.
        info.perRam = 77;
        CHECK( !SystemResource::parseMemInfo( "", info ) );
        CHECK( info.perRam == 0 && near( info.totalRam, 0.0 ) );
        CHECK( !SystemResource::parseMemInfo( "MemTotal: 1000 kB\n", info ) );

        // This machine's own file parses, with a believable figure.
        mt::sysRamInfo here;
        SystemResource().ramInfo( here );
        CHECK( here.totalRam > 0 );
        CHECK( here.perRam > 0 && here.perRam < 100 );
        std::printf( "RAM in use here: %d %% of %.0f MiB\n", here.perRam, here.totalRam );
    }

    void testStorage()
    {
        mt::sysSizeInfo info;
        std::string     root = "/";
        mt::SystemResource().sizeInfo( root, info );
        CHECK( info.status );
        CHECK( info.totalSize > 0 && info.perSize >= 0 && info.perSize <= 100 );

        std::string missing = "/nonexistent/kick/test";
        mt::SystemResource().sizeInfo( missing, info );
        CHECK( !info.status );
        CHECK( info.perSize == 0 );

        std::string empty;
        mt::SystemResource().sizeInfo( empty, info );
        CHECK( !info.status );
    }

    // Writes a fixture file below root, making its directories.
    void writeFixture( const juce::File& root, const juce::String& path, const juce::String& contents )
    {
        const auto file = root.getChildFile( path.substring( 1 ) );
        file.getParentDirectory().createDirectory();
        CHECK( file.replaceWithText( contents, false, false, "\n" ) );
    }

    void testDns()
    {
        namespace net = mt::net;

        CHECK( net::ipv4OfServer( "10.0.2.3" ) == "10.0.2.3" );
        CHECK( net::ipv4OfServer( "1.1.1.1:53" ) == "1.1.1.1" );
        CHECK( net::ipv4OfServer( "9.9.9.9%2#dns.quad9.net" ) == "9.9.9.9" );
        CHECK( net::ipv4OfServer( "8.8.8.8#dns.google" ) == "8.8.8.8" );
        CHECK( net::ipv4OfServer( "fe80::1" ).empty() );
        CHECK( net::ipv4OfServer( "[2001:db8::1]:53" ).empty() );
        CHECK( net::ipv4OfServer( "" ).empty() );

        // networkd's link state file (systemd 255).
        const char* link = "# This is private data. Do not parse.\n"
                           "ADMIN_STATE=configured\n"
                           "OPER_STATE=routable\n"
                           "REQUIRED_FOR_ONLINE=yes\n"
                           "NETWORK_FILE=/usr/lib/systemd/network/80-wired.network\n"
                           "DNS=fe80::1%2 10.0.2.3 192.168.1.1:53\n"
                           "NTP=\n"
                           "DOMAINS=\n";
        const auto  linkServers = net::parseNetworkdDns( link );
        CHECK( linkServers.size() == 2 );
        CHECK( linkServers.size() == 2 && linkServers[0] == "10.0.2.3" && linkServers[1] == "192.168.1.1" );
        CHECK( net::parseNetworkdDns( "ADMIN_STATE=configured\nDNS=\n" ).empty() );
        CHECK( net::parseNetworkdDns( "LLMNR=yes\nDNSSEC=no\n" ).empty() );

        // The stub resolver's /etc/resolv.conf, and resolved's list of the
        // real servers.
        const char* stub = "# This is /run/systemd/resolve/stub-resolv.conf managed by man:systemd-resolved(8).\n"
                           "nameserver 127.0.0.53\n"
                           "options edns0 trust-ad\n"
                           "search .\n";
        CHECK( net::parseResolvConf( stub ).empty() );
        const char* upstream = "# This is /run/systemd/resolve/resolv.conf managed by man:systemd-resolved(8).\n"
                               "nameserver 2001:db8::53\n"
                               "nameserver 10.0.2.3\n"
                               "nameserver 1.1.1.1\n";
        const auto  resolved = net::parseResolvConf( upstream );
        CHECK( resolved.size() == 2 && resolved[0] == "10.0.2.3" );
        CHECK( net::parseResolvConf( "; comment\n#nameserver 1.2.3.4\nnameserver\n" ).empty() );

        // The lookup order, on a fixture tree: the link's own servers, then
        // resolved's, then /etc/resolv.conf.
        const juce::TemporaryFile temporary;
        const auto                root = temporary.getFile();
        CHECK( root.createDirectory() );
        const std::string rootPath = root.getFullPathName().toStdString();

        CHECK( net::dnsServer( "eth0", rootPath ).empty() );

        writeFixture( root, "/etc/resolv.conf", "nameserver 192.168.0.1\n" );
        CHECK( net::dnsServer( "eth0", rootPath ) == "192.168.0.1" );

        writeFixture( root, "/etc/resolv.conf", stub );
        CHECK( net::dnsServer( "eth0", rootPath ).empty() );

        writeFixture( root, "/run/systemd/resolve/resolv.conf", upstream );
        CHECK( net::dnsServer( "eth0", rootPath ) == "10.0.2.3" );

        writeFixture( root, "/sys/class/net/eth0/ifindex", "2\n" );
        writeFixture( root, "/run/systemd/netif/links/2", "ADMIN_STATE=configured\nDNS=172.16.0.1\n" );
        CHECK( net::dnsServer( "eth0", rootPath ) == "172.16.0.1" );

        // A link networkd has no servers for falls back to resolved's.
        writeFixture( root, "/sys/class/net/wlan0/ifindex", "3\n" );
        writeFixture( root, "/run/systemd/netif/links/3", "ADMIN_STATE=configured\nDNS=\n" );
        CHECK( net::dnsServer( "wlan0", rootPath ) == "10.0.2.3" );
        CHECK( net::dnsServer( "", rootPath ) == "10.0.2.3" );

        root.deleteRecursively();
    }
}

int main()
{
    testCpu();
    testMemory();
    testStorage();
    testDns();

    if( failures != 0 )
    {
        std::printf( "%d check(s) failed\n", failures );
        return 1;
    }

    std::printf( "All system information tests passed\n" );
    return 0;
}
