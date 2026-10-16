/*
 * Copyright (c) 2021 MUSIC Tribe
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

#if( JUCE_LINUX || __linux__ )
    #include <iostream>
    #include <fcntl.h>
    #include <linux/i2c-dev.h>
    #include <sys/ioctl.h>
    #include <unistd.h>
#endif

#include "kick_Process.h"

// Wi-Fi is wpa_supplicant's job: the system runs it for the interface, and
// the system's network manager configures the address once it is
// associated. The application only talks to it through its control
// interface, with wpa_cli, so it needs no privileges beyond access to the
// control interface's socket directory.
#ifndef KICK_WPA_CTRL_DIR
    #define KICK_WPA_CTRL_DIR "/var/run/wpa_supplicant"
#endif
#define WIFI_INTERFACE "wlan0"

namespace mt
{
    WifiManager::WifiManager()
    {
    }

    WifiManager::~WifiManager()
    {
    }

    juce::String WifiManager::wpaCli( const juce::StringArray& args, bool* ok )
    {
        juce::StringArray command { "wpa_cli", "-p", KICK_WPA_CTRL_DIR, "-i", WIFI_INTERFACE };
        command.addArray( args );

        juce::String output;
        const int    status = process::run( command, &output );
        output              = output.trim();

        if( ok != nullptr )
            *ok = status == 0 && !output.startsWith( "FAIL" );

        return output;
    }

    int WifiManager::findNetwork( const juce::String& ssid )
    {
        // "network id / ssid / bssid / flags", then one line per network
        juce::StringArray lines;
        lines.addLines( wpaCli( { "list_networks" } ) );

        for( int i = 1; i < lines.size(); ++i )
        {
            const auto fields = juce::StringArray::fromTokens( lines[i], "\t", "" );
            if( fields.size() >= 2 && fields[1] == ssid && fields[0].containsOnly( "0123456789" ) )
                return fields[0].getIntValue();
        }

        return -1;
    }

    const std::vector<juce::String> WifiManager::scanWiFiNetworks()
    {
        std::map<juce::String, std::vector<juce::String>> wifiNetworks;
        std::vector<juce::String>                         _networks;

        DBG( "-------------------------scanning-----------------------" );
        // The scan runs in the background; its results replace the ones
        // scan_results returns once it is done, which takes a few seconds.
        wpaCli( { "scan" } );
        juce::Thread::sleep( 3000 );

        // "bssid / frequency / signal level / flags / ssid", then one line
        // per access point
        juce::StringArray scanResults;
        scanResults.addLines( wpaCli( { "scan_results" } ) );

        for( int i = 1; i < scanResults.size(); ++i )
        {
            const auto fields = juce::StringArray::fromTokens( scanResults[i], "\t", "" );
            if( fields.size() >= 5 && fields[4].isNotEmpty() )
                wifiNetworks[fields[4]].push_back( fields[1] );
        }

        for( const auto& entry : wifiNetworks )
        {
            juce::StringArray frequencyArray;

            // Manually add elements from the vector to the StringArray
            for( const auto& freq : entry.second )
            {
                frequencyArray.add( freq );
            }

            _networks.push_back( entry.first );
            DBG( "SSID: " + entry.first );
            DBG( "Frequencies: " + frequencyArray.joinIntoString( ", " ) );
            DBG( "-------------------------" );
        }
        displayWiFiNetworks( _networks );
        return _networks;
    }

    bool WifiManager::enableWiFi()
    {
        bool ok = false;
        wpaCli( { "reconnect" }, &ok );
        return ok;
    }

    bool WifiManager::disableWiFi()
    {
        bool ok = false;
        wpaCli( { "disconnect" }, &ok );
        return ok;
    }

    void WifiManager::displayWiFiNetworks( const std::vector<juce::String>& Networks )
    {
        for( const auto& network : Networks )
        {
            std::cout << "Network: " << network << std::endl;
        }
    }

    bool WifiManager::connectToWiFi( const juce::String& ssid, const juce::String& password, int priority )
    {
        std::cout << "Network: connecting to wifi SSID: " << ssid << std::endl;

        // wpa_supplicant stores the networks, so a network it already knows
        // keeps its passphrase unless a new one is given.
        bool known = true;
        int  id    = findNetwork( ssid );
        if( id < 0 )
        {
            known                   = false;
            const juce::String reply = wpaCli( { "add_network" } );
            if( !reply.containsOnly( "0123456789" ) || reply.isEmpty() )
                return false;
            id = reply.getIntValue();
        }

        // A control character would end up in the stored configuration.
        for( auto c : password )
            if( c < 0x20 || c == 0x7f )
                return false;

        const juce::String network = juce::String( id );
        bool               ok      = true;
        auto               set     = [&]( const juce::StringArray& args ) {
            bool done = false;
            wpaCli( args, &done );
            ok = ok && done;
        };

        // The SSID in hexadecimal, so that no character in it needs quoting.
        set( { "set_network", network, "ssid", juce::String::toHexString( ssid.toRawUTF8(), (int)ssid.getNumBytesAsUTF8(), 0 ) } );
        if( password.isNotEmpty() )
            set( { "set_network", network, "psk", "\"" + password + "\"" } );
        else if( !known )
            set( { "set_network", network, "key_mgmt", "NONE" } );
        set( { "set_network", network, "priority", juce::String( priority ) } );
        set( { "select_network", network } );
        set( { "save_config" } );

        return ok;
    }

    bool WifiManager::disconnectWiFi()
    {
        bool ok = false;
        wpaCli( { "disconnect" }, &ok );
        return ok;
    }
}
