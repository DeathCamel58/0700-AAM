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

// Tests of WifiManager (kick_WifiFirmware.cpp) against a stand-in for
// wpa_supplicant's control interface, which answers as wpa_supplicant 2.10
// does, replies of at most 4096 bytes included, and records the commands.
// Its socket is in ./wifi-test-ctrl, so the test needs no wpa_supplicant and
// leaves the system's alone. Build it from the Kick directory with
// AddressSanitizer, JUCE's modules directory as JUCE, and run it:
//
//   g++ -std=c++17 -g -fsanitize=address,undefined -I $JUCE -I modules/hardware/source
//       -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -DJUCE_USE_CURL=0
//       Tests/WifiManagerTest.cpp $JUCE/juce_core/juce_core.cpp -o WifiManagerTest -ldl -lpthread
//   ./WifiManagerTest

#define KICK_WIFI_IFACE    "wlan-test"
#define KICK_WPA_CTRL_DIR  "wifi-test-ctrl"

#include <juce_core/juce_core.h>

#include "kick_WifiFirmware.h"
#include "kick_WifiFirmware.cpp"

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <thread>

namespace
{
    int failures = 0;

#define CHECK( condition )                                                        \
    do                                                                            \
    {                                                                             \
        if( !( condition ) )                                                      \
        {                                                                         \
            std::printf( "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #condition ); \
            ++failures;                                                           \
        }                                                                         \
    } while( false )

    // wpa_supplicant's reply buffer; snprintf() needs room for a NUL too.
    constexpr std::size_t replySize = 4096;

    /** printf_encode(), as wpa_supplicant writes SSIDs in its replies. */
    std::string encode( const std::string& bytes )
    {
        std::string text;
        for( char byte : bytes )
        {
            const auto c = (unsigned char)byte;
            if( c == '\\' || c == '"' )
                text += std::string( "\\" ) + (char)c;
            else if( c >= 32 && c < 127 )
                text += (char)c;
            else
            {
                char hex[8];
                std::snprintf( hex, sizeof( hex ), "\\x%02x", c );
                text += hex;
            }
        }
        return text;
    }

    /** Adds an entry to a reply if all of it fits, as wpa_supplicant's
        lists do. */
    bool append( std::string& reply, const std::string& entry )
    {
        if( reply.size() + entry.size() >= replySize )
            return false;
        reply += entry;
        return true;
    }

    class FakeWpaSupplicant
    {
    public:
        struct Network
        {
            int         id = 0;
            std::string ssid;
            int         disabled = 1;
            int         priority = 0;
            std::map<std::string, std::string> variables;
        };

        struct Bss
        {
            unsigned    id = 0;
            std::string bssid;
            int         frequency = 2412;
            int         level     = -60;
            std::string flags;
            std::string ssid;
        };

        std::vector<Network>     networks;
        std::vector<Network>     saved;
        std::vector<Bss>         table;
        std::vector<std::string> log;
        bool                     bssRanges = true;
        int                      nextId    = 0;
        int                      current   = -1;

        FakeWpaSupplicant()
        {
            mkdir( KICK_WPA_CTRL_DIR, 0700 );
            path = std::string( KICK_WPA_CTRL_DIR ) + "/" + KICK_WIFI_IFACE;
            unlink( path.c_str() );

            fd = socket( AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0 );
            struct sockaddr_un address {};
            address.sun_family = AF_UNIX;
            std::snprintf( address.sun_path, sizeof( address.sun_path ), "%s", path.c_str() );
            if( fd < 0 || bind( fd, (struct sockaddr*)&address, sizeof( address ) ) != 0 )
            {
                std::perror( path.c_str() );
                std::exit( 1 );
            }
            thread = std::thread( [this] { serve(); } );
        }

        ~FakeWpaSupplicant()
        {
            stop = true;
            thread.join();
            close( fd );
            unlink( path.c_str() );
            rmdir( KICK_WPA_CTRL_DIR );
        }

        Network& addNetwork( const std::string& ssid, int disabled, int priority )
        {
            networks.push_back( {} );
            auto& network    = networks.back();
            network.id       = nextId++;
            network.ssid     = ssid;
            network.disabled = disabled;
            network.priority = priority;
            return network;
        }

        void addBss( const std::string& ssid, int level, const std::string& flags )
        {
            Bss bss;
            bss.id = (unsigned)table.size() + 100;
            char bssid[32];
            std::snprintf( bssid, sizeof( bssid ), "02:00:00:00:%02x:%02x", ( bss.id >> 8 ) & 0xff, bss.id & 0xff );
            bss.bssid = bssid;
            bss.level = level;
            bss.flags = flags;
            bss.ssid  = ssid;
            table.push_back( bss );
        }

        /** What wpa_supplicant would join after its next scan: the network
            of the highest priority that is enabled and in reach. */
        int pick()
        {
            std::lock_guard<std::mutex> lock( mutex );
            const Network*              best = nullptr;
            for( const auto& network : networks )
            {
                bool inReach = false;
                for( const auto& bss : table )
                    inReach = inReach || bss.ssid == network.ssid;
                if( network.disabled == 0 && inReach && ( !best || network.priority > best->priority ) )
                    best = &network;
            }
            return best ? best->id : -1;
        }

        Network* find( std::vector<Network>& list, int id )
        {
            for( auto& network : list )
                if( network.id == id )
                    return &network;
            return nullptr;
        }

        std::vector<std::string> commands()
        {
            std::lock_guard<std::mutex> lock( mutex );
            return log;
        }

    private:
        std::string        path;
        int                fd = -1;
        std::thread        thread;
        std::atomic<bool>  stop{ false };
        std::mutex         mutex;
        std::vector<std::pair<struct sockaddr_un, socklen_t>> attached;

        void serve()
        {
            while( !stop )
            {
                struct pollfd pfd {};
                pfd.fd     = fd;
                pfd.events = POLLIN;
                if( poll( &pfd, 1, 50 ) <= 0 )
                    continue;

                char               buffer[4097];
                struct sockaddr_un from {};
                socklen_t          fromLength = sizeof( from );
                const ssize_t      n          = recvfrom( fd, buffer, sizeof( buffer ), 0, (struct sockaddr*)&from, &fromLength );
                if( n < 0 )
                    continue;

                const std::string command( buffer, (std::size_t)n );
                std::string       reply;
                bool              scanned = false;
                {
                    std::lock_guard<std::mutex> lock( mutex );
                    log.push_back( command );
                    if( command == "ATTACH" )
                    {
                        attached.push_back( { from, fromLength } );
                        reply = "OK\n";
                    }
                    else if( command == "DETACH" )
                        reply = "OK\n";
                    else if( command == "SCAN" )
                    {
                        reply   = "OK\n";
                        scanned = true;
                    }
                    else
                        reply = handle( command );
                }
                if( reply.size() >= replySize )
                {
                    std::printf( "Reply to %s too long\n", command.c_str() );
                    ++failures;
                }
                sendto( fd, reply.data(), reply.size(), 0, (struct sockaddr*)&from, fromLength );

                if( scanned )
                    for( const auto& client : attached )
                    {
                        const std::string event = "<2>CTRL-EVENT-SCAN-RESULTS ";
                        sendto( fd, event.data(), event.size(), 0, (const struct sockaddr*)&client.first, client.second );
                    }
            }
        }

        static std::vector<std::string> words( const std::string& text )
        {
            std::vector<std::string> list;
            std::istringstream       stream( text );
            std::string              word;
            while( stream >> word )
                list.push_back( word );
            return list;
        }

        std::string handle( const std::string& command )
        {
            const auto w = words( command );
            if( w.empty() )
                return "UNKNOWN COMMAND\n";

            if( w[0] == "LIST_NETWORKS" )
            {
                int last = -1;
                if( w.size() > 1 && w[1].rfind( "LAST_ID=", 0 ) == 0 )
                    last = std::atoi( w[1].c_str() + 8 );
                std::string reply = "network id / ssid / bssid / flags\n";
                for( const auto& network : networks )
                {
                    if( network.id <= last )
                        continue;
                    std::string flags = network.id == current ? "[CURRENT]" : "";
                    flags += network.disabled ? "[DISABLED]" : "";
                    flags += network.disabled == 2 ? "[P2P-PERSISTENT]" : "";
                    if( !append( reply, std::to_string( network.id ) + "\t" + encode( network.ssid ) + "\tany\t" + flags + "\n" ) )
                        break;
                }
                return reply;
            }
            if( w[0] == "ADD_NETWORK" )
                return std::to_string( addNetwork( "", 1, 0 ).id ) + "\n";
            if( w[0] == "SET_NETWORK" && w.size() >= 4 )
            {
                auto* network = find( networks, std::atoi( w[1].c_str() ) );
                if( !network )
                    return "FAIL\n";
                const auto value = command.substr( command.find( w[2] + " " ) + w[2].size() + 1 );
                if( w[2] == "ssid" )
                {
                    std::string bytes;
                    for( std::size_t i = 0; i + 1 < value.size(); i += 2 )
                        bytes += (char)std::stoi( value.substr( i, 2 ), nullptr, 16 );
                    network->ssid = bytes;
                }
                else if( w[2] == "priority" )
                    network->priority = std::atoi( value.c_str() );
                else
                    network->variables[w[2]] = value;
                return "OK\n";
            }
            if( w[0] == "GET_NETWORK" && w.size() == 3 )
            {
                auto* network = find( networks, std::atoi( w[1].c_str() ) );
                if( !network )
                    return "FAIL\n";
                if( w[2] == "priority" )
                    return std::to_string( network->priority );
                return network->variables.count( w[2] ) ? network->variables[w[2]] : "FAIL\n";
            }
            if( w[0] == "REMOVE_NETWORK" && w.size() == 2 )
            {
                for( auto it = networks.begin(); it != networks.end(); ++it )
                    if( w[1] == "all" || it->id == std::atoi( w[1].c_str() ) )
                    {
                        networks.erase( it );
                        return "OK\n";
                    }
                return "FAIL\n";
            }
            if( w[0] == "SELECT_NETWORK" && w.size() == 2 )
            {
                // wpa_supplicant_select_network()
                const int id = std::atoi( w[1].c_str() );
                if( !find( networks, id ) )
                    return "FAIL\n";
                for( auto& network : networks )
                    if( network.disabled != 2 )
                        network.disabled = network.id != id;
                current = id;
                return "OK\n";
            }
            if( w[0] == "ENABLE_NETWORK" && w.size() >= 2 )
            {
                // wpa_supplicant_ctrl_iface_enable_network()
                for( auto& network : networks )
                {
                    if( w[1] != "all" && network.id != std::atoi( w[1].c_str() ) )
                        continue;
                    if( w[1] != "all" && network.disabled == 2 )
                        return "FAIL\n";
                    if( network.disabled != 2 )
                        network.disabled = 0;
                }
                return "OK\n";
            }
            if( w[0] == "SAVE_CONFIG" )
            {
                saved = networks;
                return "OK\n";
            }
            if( w[0] == "SCAN_RESULTS" )
            {
                std::string reply = "bssid / frequency / signal level / flags / ssid\n";
                for( const auto& bss : table )
                    if( !append( reply, bss.bssid + "\t" + std::to_string( bss.frequency ) + "\t" + std::to_string( bss.level ) + "\t" + bss.flags + "\t" + encode( bss.ssid ) + "\n" ) )
                        break;
                return reply;
            }
            if( w[0] == "BSS" && bssRanges && w.size() == 3 && w[1].rfind( "RANGE=", 0 ) == 0 && w[2] == std::string( "MASK=" ) + mt::wpa::bssMask )
            {
                // wpa_supplicant_ctrl_iface_bss(), with RANGE=N- and the
                // mask's fields.
                const unsigned first = (unsigned)std::atoi( w[1].c_str() + 6 );
                std::string    reply;
                for( std::size_t i = 0; i < table.size(); ++i )
                {
                    const auto& bss = table[i];
                    if( bss.id < first )
                        continue;
                    std::string entry = "id=" + std::to_string( bss.id ) + "\nbssid=" + bss.bssid + "\nfreq=" + std::to_string( bss.frequency )
                                        + "\nlevel=" + std::to_string( bss.level ) + "\nflags=" + bss.flags + "\nssid=" + encode( bss.ssid ) + "\n====\n";
                    if( !append( reply, entry ) )
                        break;
                    if( i + 1 == table.size() )
                        reply.replace( reply.size() - 5, 5, "####\n" );
                }
                return reply;
            }
            return "UNKNOWN COMMAND\n";
        }
    };

    std::string longName( int i )
    {
        // As long as an SSID can be.
        char name[40];
        std::snprintf( name, sizeof( name ), "Access point number %03d", i );
        return std::string( name ) + std::string( 32 - std::strlen( name ), '.' );
    }

    void testScan()
    {
        FakeWpaSupplicant wpa;
        for( int i = 0; i < 150; ++i )
            wpa.addBss( longName( i ), -40 - i % 50, "[WPA2-PSK-CCMP][ESS]" );
        wpa.addBss( longName( 7 ), -30, "[WPA2-PSK-CCMP][ESS]" ); // a second access point
        wpa.addBss( "", -50, "[WPA2-PSK-CCMP][ESS]" );           // a hidden network
        wpa.addBss( "Caf\xe9\t\"x\"", -70, "[ESS]" );             // not UTF-8, and quoted bytes
        wpa.addBss( "Last one", -80, "[WPA2-PSK-CCMP][ESS]" );

        // What one SCAN_RESULTS reply holds.
        {
            mt::wpa::WpaCtrl ctrl;
            CHECK( ctrl.open( std::string( KICK_WPA_CTRL_DIR ) + "/" + KICK_WIFI_IFACE ) );
            const auto truncated = mt::wpa::parseScanResults( ctrl.request( "SCAN_RESULTS" ) );
            std::printf( "One SCAN_RESULTS reply: %d of %d access points\n", (int)truncated.size(), (int)wpa.table.size() );
            CHECK( truncated.size() < 100 );
        }

        mt::WifiManager manager;
        const auto      names = manager.scanWiFiNetworks();
        CHECK( names.size() == 152 );
        CHECK( std::find( names.begin(), names.end(), juce::String( "Last one" ) ) != names.end() );
        CHECK( std::find( names.begin(), names.end(), juce::String( "Caf\\xe9\\x09\"x\"" ) ) != names.end() );
        for( int i = 0; i < 150; ++i )
            CHECK( std::find( names.begin(), names.end(), juce::String( longName( i ) ) ) != names.end() );

        int pages = 0;
        for( const auto& command : wpa.commands() )
            pages += command.rfind( "BSS RANGE=", 0 ) == 0;
        std::printf( "Read the BSS table in %d replies\n", pages );
        CHECK( pages > 1 );

        // The network's bytes are what it is joined by.
        CHECK( manager.connectToWiFi( "Caf\\xe9\\x09\"x\"", {} ) );
        CHECK( wpa.saved.size() == 1 && wpa.saved[0].ssid == "Caf\xe9\t\"x\"" && wpa.saved[0].variables["key_mgmt"] == "NONE" );
    }

    void testScanWithoutRanges()
    {
        FakeWpaSupplicant wpa;
        wpa.bssRanges = false;
        for( int i = 0; i < 20; ++i )
            wpa.addBss( longName( i ), -50, "[WPA2-PSK-CCMP][ESS]" );

        mt::WifiManager manager;
        CHECK( manager.scanWiFiNetworks().size() == 20 );
    }

    void testConnect( bool known )
    {
        // Many networks, more than one LIST_NETWORKS reply holds: one left
        // disabled (by an earlier SELECT_NETWORK that was saved), one of
        // high priority, and a P2P group, which stays as it is.
        FakeWpaSupplicant wpa;
        for( int i = 0; i < 120; ++i )
            wpa.addNetwork( longName( i ), 0, 5 );
        wpa.networks[3].disabled  = 1;
        wpa.networks[9].priority  = 9;
        wpa.networks[11].disabled = 2;
        wpa.saved                 = wpa.networks;

        // The chosen network, and stronger known ones, in reach.
        wpa.addBss( longName( 3 ), -30, "[WPA2-PSK-CCMP][ESS]" );
        wpa.addBss( longName( 9 ), -35, "[WPA2-PSK-CCMP][ESS]" );
        wpa.addBss( longName( 50 ), -40, "[WPA2-PSK-CCMP][ESS]" );
        wpa.addBss( "Studio", -75, "[WPA2-PSK-CCMP][ESS]" );

        int chosen = 50;
        if( !known )
        {
            chosen = 120;
            mt::WifiManager manager;
            CHECK( manager.scanWiFiNetworks().size() == 4 );
            CHECK( manager.connectToWiFi( "Studio", "a passphrase" ) );
        }
        else
        {
            mt::WifiManager manager;
            CHECK( manager.connectToWiFi( juce::String( longName( 50 ) ), {} ) );
        }

        auto* network = wpa.find( wpa.saved, chosen );
        CHECK( network != nullptr );
        if( network )
        {
            CHECK( network->disabled == 0 );
            CHECK( network->priority == 10 );
            if( !known )
                CHECK( network->ssid == "Studio" && network->variables["psk"] == "\"a passphrase\"" );
        }

        // Saved with every network enabled, but for the P2P group.
        int disabled = 0;
        for( const auto& n : wpa.saved )
            disabled += n.disabled != 0;
        CHECK( disabled == 1 && wpa.find( wpa.saved, 11 )->disabled == 2 );
        CHECK( wpa.saved.size() == ( known ? 120u : 121u ) );

        // Joined now, though stronger networks are in reach.
        CHECK( wpa.current == chosen );
        CHECK( wpa.pick() == chosen );

        // SELECT_NETWORK, then the others enabled without a new connection,
        // then the configuration saved.
        const auto commands = wpa.commands();
        std::size_t select = 0, save = 0, enables = 0;
        bool        order  = true;
        for( std::size_t i = 0; i < commands.size(); ++i )
        {
            if( commands[i] == "SELECT_NETWORK " + std::to_string( chosen ) )
                select = i;
            else if( commands[i] == "SAVE_CONFIG" )
                save = i;
            else if( commands[i].rfind( "ENABLE_NETWORK", 0 ) == 0 )
            {
                ++enables;
                order = order && select > 0 && commands[i].find( " no-connect" ) != std::string::npos;
            }
        }
        CHECK( select > 0 && save > select && order );
        CHECK( enables == ( known ? 118u : 119u ) );
    }
}

int main()
{
    testScan();
    testScanWithoutRanges();
    testConnect( false );
    testConnect( true );

    if( failures == 0 )
        std::printf( "All tests passed\n" );
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
