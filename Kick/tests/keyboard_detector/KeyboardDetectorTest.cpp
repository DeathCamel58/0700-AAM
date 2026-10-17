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

// Tests the computer keyboard's front-panel emulation
// (contracts::keyPressedDetector): which events it sends as keys go down
// and up, and what it leaves to a text editor that has the focus. The key
// states are made up, so it reads no real keyboard. Built with
// -DKICK_BUILD_TESTS=ON; it needs no arguments, and opens no window.

#include "contracts/utils/keyboard/keyboard_detector.h"
#include "contracts/utils/keyboard/keyboard_signals.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

namespace
{
    using DCC = contracts::DeviceControlContracts;

    int failures = 0;

    void check( bool ok, const char* what )
    {
        if( !ok )
        {
            std::printf( "FAIL: %s\n", what );
            ++failures;
        }
    }

    std::vector<CPktMgr::HWMsg_t> sent;
    std::set<int>                 down;

    void scan( bool textInputHasFocus = false )
    {
        sent.clear();
        contracts::getKeypressDetector().scanKeys( []( int keyCode ) { return down.count( keyCode ) != 0; }, textInputHasFocus );
    }

    bool sentOnly( int id, int state )
    {
        return sent.size() == 1 && sent[0].data.size() >= 2 && sent[0].data[0] == id && sent[0].data[1] == state;
    }
}

int main()
{
    contracts::KeyboardSignals::KeyboardEventTriggered.connect( []( CPktMgr::HWMsg_t& msg ) { sent.push_back( msg ); } );

    scan();
    check( sent.empty(), "nothing is sent while no key is down" );

    // A pad: pressed and released.
    down.insert( 'Z' );
    scan();
    check( sentOnly( DCC::EN_PAD1, DCC::BTN_PRESSED ), "Z presses pad 1" );
    check( sent.size() == 1 && sent[0].pktType == CPktMgr::EN_PKT_TYPE_PAD, "a pad is sent as a pad packet" );
    scan();
    check( sent.empty(), "a key held down is sent once" );
    down.erase( 'Z' );
    scan();
    check( sentOnly( DCC::EN_PAD1, DCC::BTN_RELEASED ), "Z released releases pad 1" );

    // A button sends its press only.
    down.insert( 'T' );
    scan();
    check( sentOnly( DCC::EN_MAINMODE, DCC::BTN_PRESSED ), "T presses MAIN MODE" );
    down.erase( 'T' );
    scan();
    check( sent.empty(), "a button's release is not sent" );

    // While a text editor has the focus, keys going down are typed.
    down.insert( 'A' );
    scan( true );
    check( sent.empty(), "a key typed into a text editor presses nothing" );
    down.erase( 'A' );
    scan( true );
    check( sent.empty(), "nor does releasing it" );

    // A pad held when the editor got the focus is still released.
    down.insert( 'X' );
    scan();
    check( sentOnly( DCC::EN_PAD2, DCC::BTN_PRESSED ), "X presses pad 2" );
    down.erase( 'X' );
    scan( true );
    check( sentOnly( DCC::EN_PAD2, DCC::BTN_RELEASED ), "a pad held before typing is released" );

    // A key typed into the editor and still down when the editor loses the
    // focus is not pressed late.
    down.insert( 'S' );
    scan( true );
    scan();
    check( sent.empty(), "a key typed and still down presses nothing later" );
    down.erase( 'S' );
    scan();
    check( sent.empty(), "nor does its release" );
    down.insert( 'S' );
    scan();
    check( sentOnly( DCC::EN_PAD6, DCC::BTN_PRESSED ), "the next press of that key is a pad again" );
    down.erase( 'S' );
    scan();

    check( !contracts::keyPressedDetector::isTextInput( nullptr ), "no focus is no text input" );

    if( failures == 0 )
        std::printf( "All tests passed\n" );
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
