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

#pragma once

#include <map>
#include <mutex>
#include <string>

#include "kick_WpaCtrl.h"

namespace mt
{
    class WifiManager
    {
    public:
        WifiManager();
        ~WifiManager();
        bool enableWiFi();
        bool disableWiFi();

        /** Scans for networks and returns their names. It waits for the scan
            to finish, which takes seconds: call it off the message thread. */
        const std::vector<juce::String> scanWiFiNetworks();
        void                            displayWiFiNetworks( const std::vector<juce::String>& networks );

        /** Connects to a network, adding it to wpa_supplicant's configuration
            if it is not there, and saves the configuration. An empty
            password keeps a known network's passphrase, and makes a new
            network an open one. */
        bool connectToWiFi( const juce::String& ssid, const juce::String& password, int priority = 5 );
        bool disconnectWiFi();

        /** Removes every network from wpa_supplicant's configuration, and
            saves it: part of a factory reset. */
        bool removeAllNetworks();

        // Used from the scanning thread too, so created thread-safely.
        static WifiManager* getWifiInterface()
        {
            static WifiManager _wifiInterface;
            return &_wifiInterface;
        }

    private:
        // The networks the last scan found, by the names it returned.
        std::mutex                            scanLock;
        std::map<juce::String, wpa::ScanResult> lastScan;

        static std::string controlPath();
        static int         findNetwork( wpa::WpaCtrl& ctrl, const std::string& ssid );
    };
}
