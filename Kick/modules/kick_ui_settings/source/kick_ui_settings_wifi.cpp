/*
 * Copyright (c) 2024 MUSIC Tribe
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

#include "kick_ui_settings_wifi.h"
#include "../modules/hardware/source/kick_Hwpref.h"
#include "../modules/hardware/source/kick_WifiFirmware.h"

namespace kick_ui
{

    ///////////////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////  KickChannelMixer
    ///////////////////////////////////////////////////////////////////////////////////////////////////////

    SettingsWifi::SettingsWifi()
        : SettingsInterface( 6, 3, true )
    {
        ui::KickGrid::setBackgroundColour( juce::Colours::transparentBlack );
        setRowTitles( { "", "WIFI SETTINGS", "SSID", "PASSWORD", "CONNECTION", "NETWORK" } );

        addComponent( enabled, 1, 0 );
        addComponent( scanWifi, 2, 0 );
        addComponent( wifiList, 2, 1 );
        addComponent( password, 3, 0 );
        addComponent( connect, 4, 0 );
        // The connection's addresses, titled, on a row of their own (the
        // subnet mask was placed in a fourth column of three, never shown).
        addComponent( wifIP, 5, 0 );
        addComponent( subnet, 5, 1 );
        addComponent( gateway, 5, 2 );
        wifIP.setTitle( TRANS( "IP Address" ) );
        subnet.setTitle( TRANS( "Subnet Mask" ) );
        gateway.setTitle( TRANS( "Gateway" ) );
        for( auto* value : { &wifIP, &subnet, &gateway } )
        {
            value->showTitle( true );
            value->setTitleColour( ui::Theme::textColour( 1 ) );
            value->setTitleJustification( juce::Justification::centred );
            value->setValueJustification( juce::Justification::centred );
        }

        password.setEditable( false, true );
        // A box like the other fields: on the page's black it was invisible.
        password.setValueBackgroundColour( ui::Theme::backgroundColour( 3 ) );
        password.setValueJustification( juce::Justification::centred );
        enabled.setSelected( settings.wifi.enabled() ? 1 : 0, juce::dontSendNotification );
        enabled.onSelectionChanged = [&]( int index, const juce::String& ) {
            settings.wifi.enabled = ( index == 1 );
        };

        // Scan is an action, not a state: every tap scans (as a toggle,
        // every other tap only switched it off).
        scanWifi.setClickingTogglesState( false );
        scanWifi.onClick = [&]() {
            if( !scanning )
            {
                DBG( "Scanning Wifi" );
                scanning = true;

                // A scan takes seconds: wait for it on a thread of its own,
                // and show the result if this page is still there.
                juce::Component::SafePointer<SettingsWifi> self( this );
                juce::Thread::launch( [self]() {
                    auto list = mt::WifiManager::getWifiInterface()->scanWiFiNetworks();
                    juce::MessageManager::callAsync( [self, list]() {
                        if( self != nullptr )
                        {
                            self->wifiList.setListOptions( list );
                            self->scanning = false;
                        }
                    } );
                } );
            }
        };

        // The passphrase is only kept until it is handed to wpa_supplicant,
        // which stores it with the network; it is not a preference.
        password.onTextChange = [&]() {
            passphrase = password.getText();
        };

        wifiList.onSelectionChanged = [&]( int value, juce::String str ) {
            settings.wifi.ssid = ( str );
        };

        connect.onClick = [&]() {
            connectWifi( connect.getToggleState() );
        };
    }

    void SettingsWifi::connectWifi( bool Connect )
    {
        if( Connect )
        {
            if( mt::WifiManager::getWifiInterface()->connectToWiFi( wifiList.getSelectedValue(), passphrase ) )
                passphrase.clear();
        }
        else
            mt::WifiManager::getWifiInterface()->disconnectWiFi();
    }

    void SettingsWifi::handleEnabledClick()
    {
        bool status = enabled.getSelected() == 1;
        if( status )
        {
            mt::WifiManager::getWifiInterface()->enableWiFi();
        }
        else
            mt::WifiManager::getWifiInterface()->disableWiFi();
    }

    void SettingsWifi::refresh()
    {
        mt::sysEthInfo info;
        mt::HwPref::getHwPref().getIP( mt::net::wifiInterface(), info );

        wifIP.setText( info.IP, juce::dontSendNotification );
        subnet.setText( info.subnet, juce::dontSendNotification );
        gateway.setText( info.gateway, juce::dontSendNotification );
#if 0
        // Assuming wifiList is a custom UI component or structure managing the list of networks
        juce::String previouslySelectedSSID = wifiList.getSelected(); // Step 1: Store the currently selected SSID

        std::vector<juce::String> list = mt::WifiManager().scanWiFiNetworks(); // Refresh the list of networks

        wifiList.setListOptions( list ); // Update the UI or list with the new set of networks

        // Step 2: Reselect the previously selected SSID if it's still available
        if( std::find( list.begin(), list.end(), previouslySelectedSSID ) != list.end() )
        {
            wifiList.setSelected( previouslySelectedSSID );
        }
        else
        {
            // Optionally, handle the case where the previously selected SSID is no longer available
            wifiList.setSelected( "" ); // or any default behavior
        }
#endif
    }

    void SettingsWifi::setupHeader( kick_ui::KickModeHeader& Hdr ) {}

    void SettingsWifi::handleCommand( CPktMgr::HWMsg_t& ) {}

}
