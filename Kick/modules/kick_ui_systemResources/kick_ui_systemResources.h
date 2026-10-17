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

/*******************************************************************************
  The block below describes the properties of this module, and is read by
  the Projucer to automatically generate project code that uses it.
  For details about the syntax and how to create or use a module, see the
  JUCE Module Format.txt file.

  BEGIN_JUCE_MODULE_DECLARATION

   ID:                 kick_ui
   vendor:             MusicTribe
   version:            0.1.0
   name:               MusicTribe ui
   description:        Classes aand interfaces for DAW
   website:            http://www.musictribe.com/
   license:            AGPLv3

   dependencies:       kick_ui_controls

  END_JUCE_MODULE_DECLARATION

 *******************************************************************************/

#pragma once

#include <kick_ui_controls/kick_ui_controls.h>
#include <hardware/source/kick_RemovableMedia.h>

// Writable state; the build system can override it (see CMakeLists.txt).
#ifndef KICK_STATE_DIR
    #define KICK_STATE_DIR "/home/Kick"
#endif

namespace kick_ui
{
    class KSystemResources
        : public juce::Component
        , private juce::Timer
    {
    public:
        static KSystemResources& Instance();
        KSystemResources();
        void start();
        void stop();

    private:
        void timerCallback() override;
        void paint( juce::Graphics& g ) override;
        void resized() override;
        void visibilityChanged() override;

    private:
        ui::KickLabel storage{ "STORAGE" };

        ui::KickSlider cpu{ "CPU" };
        ui::KickSlider ram{ "RAM" };
        ui::KickSlider flash{ "INTERNAL MEMORY" };
        ui::KickSlider sdcard{ "SD CARD" };
        ui::KickSlider usb{ "USB DRIVE" };

        // Turns the performance overlay on and off (the setting it shows
        // follows), which stays when this window closes.
        ui::KickLabel        overlayLabel{ "PERFORMANCE OVERLAY" };
        ui::KickToggleButton overlay{ "Performance overlay", "On", "Off" };

        ui::KickImageButton purgeAll{ "Purge All", "purge" };
        ui::KickImageButton purgeUnused{ "Purge Unused", "purgeused" };
        ui::KickImageButton ok{ "OK", "ok_resource" };
        std::string         internalPath = KICK_STATE_DIR;
        std::string         sdCardPath;
        std::string         usbDiskPath;
    };

    class KickSysResources
        : public KSystemResources
        , public IScreen
    {
    public:
        KickSysResources()
            : IScreen( Screen::SystemResources ){};

    private:
    };

}
