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

#include <iostream>

#if( JUCE_LINUX || __linux__ )
    #include <dirent.h>
    #include <sys/stat.h>
#endif

#include "kick_NetInfo.h"
#include "kick_WpaCtrl.h"

// Wi-Fi is wpa_supplicant's job: the system runs it for the interface, and
// the system's network manager configures the address once it is
// associated. The application only talks to it through its control
// interface, so it needs no privileges beyond access to the control
// interface's socket directory, and passphrases never appear on a command
// line.
#ifndef KICK_WPA_CTRL_DIR
    #define KICK_WPA_CTRL_DIR "/var/run/wpa_supplicant"
#endif

namespace mt
{
    namespace
    {
        // How long a scan may take before its results are read anyway.
        constexpr int scanTimeoutMs = 10000;

        /** A name for an SSID to show: its text if it is UTF-8, or else the
            SSID with the other bytes written as \xNN. */
        juce::String ssidToDisplayName( const std::string& ssid )
        {
            if( juce::CharPointer_UTF8::isValidString( ssid.data(), (int)ssid.size() ) )
                return juce::String::fromUTF8( ssid.data(), (int)ssid.size() );

            juce::String name;
            for( char byte : ssid )
            {
                const auto c = static_cast<unsigned char>( byte );
                if( c >= 0x20 && c < 0x7f )
                    name += (juce::juce_wchar)c;
                else
                    name += "\\x" + juce::String::toHexString( (int)c ).paddedLeft( '0', 2 );
            }
            return name;
        }

        bool setNetwork( wpa::WpaCtrl& ctrl, int id, const std::string& variable, const std::string& value )
        {
            return ctrl.command( "SET_NETWORK " + std::to_string( id ) + " " + variable + " " + value );
        }
    }

    WifiManager::WifiManager()
    {
    }

    WifiManager::~WifiManager()
    {
    }

    std::string WifiManager::controlPath()
    {
        const std::string dir   = KICK_WPA_CTRL_DIR;
        std::string       iface = net::wifiInterface();

#if( JUCE_LINUX || __linux__ )
        // No Wi-Fi interface found: use whichever one wpa_supplicant has a
        // control socket for.
        if( iface.empty() )
        {
            if( DIR* d = opendir( dir.c_str() ) )
            {
                while( const dirent* entry = readdir( d ) )
                {
                    struct stat st {};
                    const std::string name = entry->d_name;
                    if( name != "." && name != ".." && stat( ( dir + "/" + name ).c_str(), &st ) == 0 && S_ISSOCK( st.st_mode ) )
                    {
                        iface = name;
                        break;
                    }
                }
                closedir( d );
            }
        }
#endif

        return iface.empty() ? std::string() : dir + "/" + iface;
    }

    int WifiManager::findNetwork( wpa::WpaCtrl& ctrl, const std::string& ssid )
    {
        for( const auto& network : wpa::listNetworks( ctrl ) )
            if( network.ssid == ssid )
                return network.id;

        return -1;
    }

    const std::vector<juce::String> WifiManager::scanWiFiNetworks()
    {
        std::vector<juce::String> _networks;

        DBG( "-------------------------scanning-----------------------" );
        const std::string path = controlPath();
        wpa::WpaCtrl      ctrl;
        if( !ctrl.open( path ) )
        {
            DBG( "Cannot connect to wpa_supplicant at " << path );
            return _networks;
        }

        // The scan runs in the background, and wpa_supplicant tells the
        // clients that listen for events when its results are ready. If a
        // scan is already running ("FAIL-BUSY"), its results will do.
        wpa::WpaCtrl events;
        const bool   listening = events.open( path ) && events.attach();
        const auto   reply     = ctrl.request( "SCAN" );

        if( reply == "OK\n" || reply == "FAIL-BUSY\n" )
        {
            if( listening )
            {
                const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32)scanTimeoutMs;
                std::string message;
                while( juce::Time::getMillisecondCounter() < deadline
                       && events.receive( message, (int)( deadline - juce::Time::getMillisecondCounter() ) ) )
                {
                    const auto event = wpa::WpaCtrl::eventText( message );
                    if( event.rfind( "CTRL-EVENT-SCAN-RESULTS", 0 ) == 0 || event.rfind( "CTRL-EVENT-SCAN-FAILED", 0 ) == 0 )
                        break;
                }
            }
            else
                juce::Thread::sleep( 3000 );
        }
        events.close();

        // One entry per network name, for the access point heard best.
        std::map<juce::String, wpa::ScanResult> found;
        for( const auto& result : wpa::parseScanResults( ctrl.request( "SCAN_RESULTS" ) ) )
        {
            const auto name = ssidToDisplayName( result.ssid );
            auto       it   = found.find( name );
            if( it == found.end() || result.signal > it->second.signal )
                found[name] = result;
        }

        for( const auto& entry : found )
        {
            _networks.push_back( entry.first );
            DBG( "SSID: " + entry.first + " (" + juce::String( entry.second.flags ) + ", " + juce::String( entry.second.frequency ) + " MHz, "
                 + juce::String( entry.second.signal ) + " dBm)" );
        }

        {
            std::lock_guard<std::mutex> lock( scanLock );
            lastScan = std::move( found );
        }

        displayWiFiNetworks( _networks );
        return _networks;
    }

    bool WifiManager::enableWiFi()
    {
        wpa::WpaCtrl ctrl;
        return ctrl.open( controlPath() ) && ctrl.command( "RECONNECT" );
    }

    bool WifiManager::disableWiFi()
    {
        wpa::WpaCtrl ctrl;
        return ctrl.open( controlPath() ) && ctrl.command( "DISCONNECT" );
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

        // A control character would end up in the stored configuration.
        for( auto c : password )
            if( c < 0x20 || c == 0x7f )
                return false;

        // The SSID's bytes and security, as the last scan saw them; a name
        // the scan did not return is taken as UTF-8 text, and as a hidden
        // network.
        std::string ssidBytes = ssid.toStdString();
        bool        seen      = false;
        bool        open      = password.isEmpty();
        {
            std::lock_guard<std::mutex> lock( scanLock );
            const auto                  it = lastScan.find( ssid );
            if( it != lastScan.end() )
            {
                ssidBytes = it->second.ssid;
                seen      = true;
                open      = it->second.isOpen();
            }
        }
        if( ssidBytes.empty() || ssidBytes.size() > 32 )
            return false;

        const std::string passphrase = password.toStdString();
        const bool        rawPsk     = passphrase.size() == 64 && passphrase.find_first_not_of( "0123456789abcdefABCDEF" ) == std::string::npos;
        if( !open && !password.isEmpty() && !rawPsk && ( passphrase.size() < 8 || passphrase.size() > 63 ) )
        {
            DBG( "Wi-Fi passphrases are 8 to 63 characters long" );
            return false;
        }

        wpa::WpaCtrl ctrl;
        if( !ctrl.open( controlPath() ) )
            return false;

        // wpa_supplicant stores the networks, so a network it already knows
        // keeps its passphrase unless a new one is given.
        int        id    = findNetwork( ctrl, ssidBytes );
        const bool known = id >= 0;
        if( !known && !open && password.isEmpty() )
        {
            DBG( "A passphrase is needed to join " << ssid );
            return false;
        }
        if( !known )
        {
            const auto reply = ctrl.request( "ADD_NETWORK" );
            if( !wpa::isNetworkId( reply ) )
                return false;
            id = std::atoi( reply.c_str() );
        }

        // The SSID in hexadecimal, so that no byte in it needs quoting.
        bool ok = setNetwork( ctrl, id, "ssid", wpa::toHex( ssidBytes ) );

        if( !password.isEmpty() && !open )
        {
            if( rawPsk )
            {
                // A raw PSK only works with WPA2-Personal.
                ok = ok && setNetwork( ctrl, id, "psk", passphrase );
                ok = ok && setNetwork( ctrl, id, "key_mgmt", "WPA-PSK" );
            }
            else
            {
                // A passphrase works for WPA2-Personal and WPA3-Personal
                // (SAE), and for networks in the transition mode between
                // the two. WPA3 needs management frame protection, which is
                // optional for the others. A wpa_supplicant without SAE
                // refuses it: then WPA2 it is.
                ok = ok && setNetwork( ctrl, id, "psk", "\"" + passphrase + "\"" );
                ok = ok && ( setNetwork( ctrl, id, "key_mgmt", "WPA-PSK WPA-PSK-SHA256 SAE" ) || setNetwork( ctrl, id, "key_mgmt", "WPA-PSK" ) );
                setNetwork( ctrl, id, "ieee80211w", "1" );
            }
        }
        else if( open && ( !known || seen ) )
            ok = ok && setNetwork( ctrl, id, "key_mgmt", "NONE" );

        // A network that does not answer broadcast scans has to be probed
        // for by name.
        if( !known && !seen )
            ok = ok && setNetwork( ctrl, id, "scan_ssid", "1" );

        // The other networks stay enabled, to be joined when this one is
        // out of reach; this one comes first.
        ok = ok && setNetwork( ctrl, id, "priority", std::to_string( std::max( priority, wpa::highestPriority( ctrl, id ) + 1 ) ) );

        if( !ok )
        {
            // Leave no half-configured network behind.
            if( !known )
                ctrl.command( "REMOVE_NETWORK " + std::to_string( id ) );
            return false;
        }

        ok = wpa::selectNetwork( ctrl, id );
        return ctrl.command( "SAVE_CONFIG" ) && ok;
    }

    bool WifiManager::disconnectWiFi()
    {
        wpa::WpaCtrl ctrl;
        return ctrl.open( controlPath() ) && ctrl.command( "DISCONNECT" );
    }

    bool WifiManager::removeAllNetworks()
    {
        // No Wi-Fi, nothing to forget.
        const std::string path = controlPath();
        if( path.empty() )
            return true;

        wpa::WpaCtrl ctrl;
        if( !ctrl.open( path ) )
            return false;

        const bool removed = ctrl.command( "REMOVE_NETWORK all" );
        return ctrl.command( "SAVE_CONFIG" ) && removed;
    }
}
