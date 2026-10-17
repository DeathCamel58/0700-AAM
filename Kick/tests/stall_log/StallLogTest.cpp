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

// Tests of the user interface's stall check (kick_StallCheck.h): from the
// times its timer calls back, how long the message thread was kept busy,
// and which line it logs, with and without input debugging. Built with
// -DKICK_BUILD_TESTS=ON; it needs no arguments, creates no window and runs
// no message loop.

#include <juce_core/juce_core.h>

#include "kick_ui_controls/source/kick_StallCheck.h"

#include <cstdio>

using ui::stallLog::StallCheck;

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

    void testOnTime()
    {
        StallCheck c( 1000.0 );
        // On time, and a little late, as timers are: nothing.
        for( double t = 1100.0; t <= 2000.0; t += 100.0 )
            CHECK( c.check( t, true ).line.isEmpty() );
        CHECK( c.check( 2349.0, true ).line.isEmpty() ); // 249 ms late
    }

    void testThresholds()
    {
        StallCheck c( 0.0 );
        // 250 ms late: only with input debugging on.
        auto r = c.check( 350.0, false );
        CHECK( r.line.isEmpty() );
        StallCheck d( 0.0 );
        r = d.check( 350.0, true );
        CHECK( r.line == "UI: message thread stalled 250 ms, no named step" );
        CHECK( !r.error );

        // 1 s late: always, as an error; without debugging, no detail.
        StallCheck e( 0.0 );
        r = e.check( 1100.0, false );
        CHECK( r.line == "UI: message thread stalled 1000 ms" );
        CHECK( r.error );
        // Measured from the previous callback.
        r = e.check( 1200.0, false );
        CHECK( r.line.isEmpty() );
    }

    void testPhases()
    {
        StallCheck c( 0.0 );
        c.phaseEnded( "creating screen", "AppGrid", 300.0 );
        c.phaseEnded( "creating screen", "AppMainmode", 13900.0 );
        c.phaseEnded( "showing screen", nullptr, 20.0 );
        const auto r = c.check( 14100.0, true, []( const char* name ) { return juce::String( "kick_ui::" ) + name; } );
        CHECK( r.error );
        CHECK( r.line == "UI: message thread stalled 14000 ms, longest step: creating screen kick_ui::AppMainmode (13900 ms)" );

        // The steps are forgotten after each check.
        const auto next = c.check( 14600.0, true );
        CHECK( next.line == "UI: message thread stalled 400 ms, no named step" );

        // Without a type name.
        c.phaseEnded( "loading", nullptr, 700.0 );
        CHECK( c.check( 15400.0, true ).line == "UI: message thread stalled 700 ms, longest step: loading (700 ms)" );
    }
}

int main()
{
    testOnTime();
    testThresholds();
    testPhases();

    if( failures == 0 )
        std::printf( "StallLogTest: all tests passed\n" );
    return failures == 0 ? 0 : 1;
}
