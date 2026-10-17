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

// Tests that front-panel events handed to the UI through
// DclContractsHelper::dispatchHardwareInput() keep isShiftPressed() up to
// date and still reach DclSignals::hardwareInputTriggered. Built with
// -DKICK_BUILD_TESTS=ON; it needs no arguments, and opens no window.

#include "contracts/dcl/dcl_contracts.h"
#include "contracts/dcl/dcl_contracts_helper.h"
#include "contracts/dcl/dcl_signals.h"

#include <cstdio>
#include <cstdlib>

namespace
{
    int failures = 0;

    void check( bool ok, const char* what )
    {
        if( !ok )
        {
            std::printf( "FAIL: %s\n", what );
            ++failures;
        }
    }

    CPktMgr::HWMsg_t buttonMessage( int button, int state )
    {
        CPktMgr::HWMsg_t msg;
        msg.pktType = CPktMgr::enumPacketType::EN_PKT_TYPE_BUTTON;
        msg.pktCmd  = contracts::DeviceControlContracts::BtnCmd::EN_BTN_STATUS;
        msg.data    = { button, state };
        return msg;
    }
}

int main()
{
    using contracts::DclContractsHelper;
    using DCC = contracts::DeviceControlContracts;

    int received = 0;
    contracts::DclSignals::hardwareInputTriggered.connect( [&]( CPktMgr::HWMsg_t& ) { ++received; } );

    check( !DclContractsHelper::isShiftPressed(), "Shift starts released" );

    auto shiftDown = buttonMessage( DCC::EN_SHIFT, DCC::BTN_PRESSED );
    DclContractsHelper::dispatchHardwareInput( shiftDown );
    check( DclContractsHelper::isShiftPressed(), "Shift pressed is seen" );

    // Another button's events leave the Shift state alone.
    auto otherUp = buttonMessage( DCC::EN_KNOB_ENTER, DCC::BTN_RELEASED );
    DclContractsHelper::dispatchHardwareInput( otherUp );
    check( DclContractsHelper::isShiftPressed(), "another button does not release Shift" );

    // A Shift message without its state byte is passed on but ignored.
    CPktMgr::HWMsg_t truncated = buttonMessage( DCC::EN_SHIFT, DCC::BTN_RELEASED );
    truncated.data.resize( 1 );
    DclContractsHelper::dispatchHardwareInput( truncated );
    check( DclContractsHelper::isShiftPressed(), "a truncated message changes nothing" );

    // A non-button message with Shift's code in its first byte is not Shift.
    CPktMgr::HWMsg_t pad = buttonMessage( DCC::EN_SHIFT, DCC::BTN_RELEASED );
    pad.pktType = CPktMgr::enumPacketType::EN_PKT_TYPE_PAD;
    DclContractsHelper::dispatchHardwareInput( pad );
    check( DclContractsHelper::isShiftPressed(), "a pad message is not Shift" );

    auto shiftUp = buttonMessage( DCC::EN_SHIFT, DCC::BTN_RELEASED );
    DclContractsHelper::dispatchHardwareInput( shiftUp );
    check( !DclContractsHelper::isShiftPressed(), "Shift released is seen" );

    check( received == 5, "every event reaches hardwareInputTriggered" );

    if( failures == 0 )
        std::printf( "All tests passed\n" );
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
