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

#include <juce_gui_basics/juce_gui_basics.h>
#include "contracts/dcl/dcl_contracts.h"
#include "PktMgr/PktMgr.hpp"



namespace contracts
{
    

    // Emulates the front panel with a computer keyboard: keys stand for
    // the pads, buttons and the centre knob (see the key map in the .cpp).
    // The state of every key is read with juce::KeyPress::isKeyCurrentlyDown().
    struct keyPressedDetector : public juce::KeyListener
    {
        juce::Array<int> _keys;

        struct cmdInfo
        {
            int                     id{ contracts::DeviceControlContracts::EN_INVALID_ID };
            bool                    releaseEventRequired{ false };
            CPktMgr::enumPacketType pktType{ CPktMgr::EN_PKT_TYPE_BUTTON };
            int                     pktCmd{ 0 };
        };

        bool isValidCmd( const cmdInfo& Cmd ) { return contracts::DeviceControlContracts::EN_INVALID_ID != Cmd.id; }
        void detectForKeyCode( int KeyCode, const cmdInfo& Info );

        bool              keyPressed( const juce::KeyPress& key, juce::Component* originatingComponent ) override;
        bool              keyStateChanged( bool isKeyDown, juce::Component* originatingComponent ) override;
        std::atomic<bool> shiftBtnState;

        // Listens to the keys that reach Target: nullptr stops. Give it the
        // window. JUCE hands a key to the focused component and then to its
        // parents up to the window, or to the window alone while nothing
        // has the focus, so only there are the keys seen wherever the focus
        // is (a pop-up, a screen, nothing at all).
        void listenTo( juce::Component* Target );

    private:
        juce::Component::SafePointer<juce::Component> target;
    };
    keyPressedDetector& getKeypressDetector();
}
