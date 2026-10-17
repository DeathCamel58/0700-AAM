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

// Tests of KickMain's heartbeat bookkeeping (kick_heart_beat_data.h): which
// processes count as starting, missing (holding up the watchdog) or silent
// (to be restarted), fed made-up times. Built with -DKICK_BUILD_TESTS=ON; it
// needs no arguments and starts no process.

#include "kick_heart_beat_data.h"

#include <cstdio>

using kick::main::KickHeartBeatData;

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

    // KickMain's timing: a heartbeat counts for 10 s, a start gets 60 s.
    constexpr juce::int64 maxAge = 10000;
    constexpr juce::int64 limit  = 60000;

    bool has( const juce::Array<int>& ids, int id ) { return ids.contains( id ); }

    struct State
    {
        bool starting, missing, silent;
    };

    State stateOf( const KickHeartBeatData& d, int id, juce::int64 t )
    {
        const auto v = d.verify( t, maxAge, limit );
        return { has( v.startingHostIds, id ), has( v.missingHostIds, id ), has( v.silentHostIds, id ) };
    }

    bool isOk( const KickHeartBeatData& d, int id, juce::int64 t )
    {
        const auto s = stateOf( d, id, t );
        return !s.starting && !s.missing && !s.silent;
    }

    bool isStarting( const KickHeartBeatData& d, int id, juce::int64 t )
    {
        const auto s = stateOf( d, id, t );
        return s.starting && !s.missing && !s.silent;
    }

    bool isSilent( const KickHeartBeatData& d, int id, juce::int64 t )
    {
        const auto s = stateOf( d, id, t );
        return !s.starting && s.missing && s.silent;
    }

    // Missing, but left to the service manager's watchdog.
    bool isMissingOnly( const KickHeartBeatData& d, int id, juce::int64 t )
    {
        const auto s = stateOf( d, id, t );
        return !s.starting && s.missing && !s.silent;
    }

    // The case seen under emulation: the user interface sends one heartbeat
    // soon after it starts, then builds its screens for 14 s. It was
    // restarted 10 s after that heartbeat, again and again.
    void testBusyAfterFirstHeartBeat()
    {
        KickHeartBeatData d;
        d.expectHeartBeatsFrom( { 4 } );

        // Not started yet: missing, not restarted (KickMain is starting).
        CHECK( isMissingOnly( d, 4, 0 ) );

        d.hostStarted( 4, 1000 );
        CHECK( isStarting( d, 4, 1000 ) );
        d.registerHeartBeat( 4, 4000 );
        CHECK( isOk( d, 4, 5000 ) );
        // Busy from 4 s to 18 s: still starting, so neither restarted nor
        // holding up the watchdog.
        CHECK( isStarting( d, 4, 15000 ) );
        CHECK( !d.beginRestartIfSilent( 4, 15000, maxAge, limit ) );
        CHECK( isStarting( d, 4, 18000 ) );
        d.registerHeartBeat( 4, 18000 );
        CHECK( isOk( d, 4, 19000 ) );

        // After the window, the normal timeout applies.
        d.registerHeartBeat( 4, 70000 );
        CHECK( isOk( d, 4, 80000 ) );
        CHECK( isSilent( d, 4, 80001 ) );
    }

    // A first start that sends nothing is restarted once the window ends,
    // and its successor gets no window: a process that keeps hanging on
    // startup is left to the watchdog.
    void testHangOnFirstStart()
    {
        KickHeartBeatData d;
        d.expectHeartBeatsFrom( { 1 } );
        d.hostStarted( 1, 0 );
        CHECK( isStarting( d, 1, 60000 ) );
        CHECK( !d.beginRestartIfSilent( 1, 60000, maxAge, limit ) );
        CHECK( isSilent( d, 1, 60001 ) );
        CHECK( d.beginRestartIfSilent( 1, 60001, maxAge, limit ) );
        CHECK( !d.beginRestartIfSilent( 1, 60002, maxAge, limit ) ); // asked only once
        CHECK( isMissingOnly( d, 1, 60001 ) );

        // SIGTERM ignored, SIGKILL 5 s later, started again.
        d.hostExited( 1, 66000 );
        d.hostStarted( 1, 66000 );
        CHECK( isMissingOnly( d, 1, 67000 ) );
        CHECK( isMissingOnly( d, 1, 500000 ) );
        CHECK( !d.beginRestartIfSilent( 1, 500000, maxAge, limit ) );

        // A late heartbeat brings it back, and its next start gets a window
        // again once it has come up.
        d.registerHeartBeat( 1, 500000 );
        CHECK( isOk( d, 1, 500001 ) );
        d.hostExited( 1, 501000 );
        d.hostStarted( 1, 501000 );
        CHECK( isStarting( d, 1, 520000 ) );
    }

    // A process that sends one heartbeat and then hangs within its window
    // is restarted when the window ends, and its successor gets none.
    void testHangAfterFirstHeartBeat()
    {
        KickHeartBeatData d;
        d.expectHeartBeatsFrom( { 2 } );
        d.hostStarted( 2, 0 );
        d.registerHeartBeat( 2, 3000 );
        CHECK( isStarting( d, 2, 59000 ) );
        CHECK( isSilent( d, 2, 60001 ) );
        CHECK( d.beginRestartIfSilent( 2, 60001, maxAge, limit ) );
        CHECK( isMissingOnly( d, 2, 60001 ) );
        d.hostExited( 2, 66000 );
        d.hostStarted( 2, 66000 );
        CHECK( isMissingOnly( d, 2, 67000 ) );
    }

    // A process that has been up for a while and hangs is restarted 10 s
    // after its last heartbeat; its successor gets a window, from the
    // restart on, which its first heartbeat does not end.
    void testHangWhenUp()
    {
        KickHeartBeatData d;
        d.expectHeartBeatsFrom( { 3, 5 } );
        d.hostStarted( 3, 0 );
        d.hostStarted( 5, 0 );
        for( juce::int64 t = 1000; t <= 100000; t += 1000 )
        {
            d.registerHeartBeat( 3, t );
            d.registerHeartBeat( 5, t );
        }
        d.registerHeartBeat( 5, 120000 );
        CHECK( isOk( d, 3, 110000 ) );
        CHECK( isSilent( d, 3, 110001 ) );
        CHECK( d.beginRestartIfSilent( 3, 110001, maxAge, limit ) );
        CHECK( isStarting( d, 3, 110001 ) );
        // The restart: SIGKILL 5 s after SIGTERM, the exit seen 1 s later.
        d.hostExited( 3, 116000 );
        CHECK( isStarting( d, 3, 116000 ) );
        d.hostStarted( 3, 116500 );
        d.registerHeartBeat( 3, 118000 );
        CHECK( isOk( d, 3, 119000 ) );
        CHECK( isStarting( d, 3, 170000 ) );
        CHECK( !d.beginRestartIfSilent( 3, 170000, maxAge, limit ) );
        CHECK( isStarting( d, 3, 176500 ) );
        CHECK( isSilent( d, 3, 176501 ) );
    }

    // A crash: the old heartbeat no longer counts, the successor gets a
    // window from the exit, renewed when it starts.
    void testCrash()
    {
        KickHeartBeatData d;
        d.expectHeartBeatsFrom( { 6 } );
        d.hostStarted( 6, 0 );
        d.registerHeartBeat( 6, 70000 );
        d.hostExited( 6, 70500 );
        CHECK( isStarting( d, 6, 70500 ) );
        d.hostStarted( 6, 71000 );
        CHECK( isStarting( d, 6, 131000 ) );
        CHECK( isSilent( d, 6, 131001 ) );

        // Crashes during start-up keep a window; repeated crashes are the
        // process restarter's limit's job.
        d.hostExited( 6, 80000 );
        d.hostStarted( 6, 80000 );
        d.hostExited( 6, 82000 );
        CHECK( isStarting( d, 6, 82000 ) );
    }

    // A host that recovers between the check and the restart is not
    // restarted.
    void testRecovery()
    {
        KickHeartBeatData d;
        d.registerHeartBeat( 7, 0 );
        CHECK( d.verify( 11000, maxAge, limit ).silentHostIds.contains( 7 ) );
        d.registerHeartBeat( 7, 11000 );
        CHECK( !d.beginRestartIfSilent( 7, 11000, maxAge, limit ) );
    }

    // An unexpected host's heartbeat is tracked but holds up nothing.
    void testUnexpected()
    {
        KickHeartBeatData d;
        d.expectHeartBeatsFrom( { 1 } );
        d.registerHeartBeat( 1, 0 );
        d.registerHeartBeat( 9, 0 );
        const auto v = d.verify( 5000, maxAge, limit );
        CHECK( v.missingHostIds.isEmpty() && v.startingHostIds.isEmpty() && v.silentHostIds.isEmpty() );
    }
}

int main()
{
    testBusyAfterFirstHeartBeat();
    testHangOnFirstStart();
    testHangAfterFirstHeartBeat();
    testHangWhenUp();
    testCrash();
    testRecovery();
    testUnexpected();

    if( failures == 0 )
        std::printf( "HeartBeatTest: all tests passed\n" );
    return failures == 0 ? 0 : 1;
}
