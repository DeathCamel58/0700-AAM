/*
 * Copyright (c) 2026 Dylan Corrales
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

// Stress test of the MIDI clock's threads: while the clock's timer thread
// ticks, a stand-in tick thread changes the tempo and restarts the clock
// (as a sequence switch at a bar does) and a stand-in message thread asks
// whether it runs, reads the tempo and stops and starts it. First it checks
// that the clock ticks at the tempo it is set to, also when the tempo
// changes while it runs.
//
// Configure Kick with -DKICK_BUILD_TESTS=ON (and
// -DKICK_TEST_SANITIZER=thread or address), build the ClockStress target
// and run it for a number of seconds:
//
//   ClockStress 10
//
// It passes when it prints PASS and the sanitizer reports nothing.

#include <audio_engine/audio_engine.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>

namespace
{
    std::atomic<uint32_t> lastTick{ 0 };

    bool check( bool Condition, const char* What )
    {
        if( !Condition )
            std::printf( "FAILED: %s\n", What );
        return Condition;
    }

    // Ticks per second while the clock runs for a second.
    double tickRate()
    {
        const auto first = lastTick.load();
        std::this_thread::sleep_for( std::chrono::seconds( 1 ) );
        return (double)( lastTick.load() - first );
    }

    bool checkTempo( mt::CMidiClock& Clock )
    {
        const double ppqn = Clock.getInternalPPQN();
        bool         ok   = true;

        Clock.setTempo( 120.f );
        Clock.start( true );
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
        auto rate = tickRate();
        std::printf( "120 BPM: %.0f ticks per second (%.0f expected)\n", rate, 2.0 * ppqn );
        ok &= check( std::abs( rate - 2.0 * ppqn ) < 0.1 * 2.0 * ppqn, "the clock ticks at 120 BPM" );

        // While it runs.
        Clock.setTempo( 60.f );
        ok &= check( 60.f == Clock.getTempo(), "the new tempo reads back at once" );
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
        rate = tickRate();
        std::printf( "60 BPM: %.0f ticks per second (%.0f expected)\n", rate, ppqn );
        ok &= check( std::abs( rate - ppqn ) < 0.1 * ppqn, "the clock ticks at 60 BPM after a change while running" );

        Clock.stop();
        ok &= check( Clock.isStopped(), "the clock stops" );
        return ok;
    }
} // namespace

int main( int argc, char** argv )
{
    const int seconds = ( argc > 1 ) ? std::atoi( argv[1] ) : 10;

    auto& clock   = mt::getMidiClock();
    clock.onTick  = []( uint32_t Tick ) { lastTick.store( Tick ); };
    bool ok       = checkTempo( clock );
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( seconds );

    std::atomic<bool> running{ true };
    std::atomic<long> changes{ 0 };

    // The tick thread: tempo changes at bars, restarts on sequence switches.
    std::thread tickThread( [&] {
        std::mt19937 rng( 1 );
        while( running.load() )
        {
            clock.setTempo( 60.f + (float)( rng() % 140 ) );
            if( 0 == rng() % 4 )
                clock.start( rng() & 1 );
            ++changes;
            std::this_thread::sleep_for( std::chrono::microseconds( 500 + rng() % 2000 ) );
        }
    } );

    // The message thread: looks at the clock, and stops and plays.
    std::thread messageThread( [&] {
        std::mt19937 rng( 2 );
        while( running.load() )
        {
            static_cast<void>( clock.isStopped() );
            static_cast<void>( clock.isRunning() );
            static_cast<void>( clock.getTempo() );
            if( 0 == rng() % 16 )
                clock.stop();
            else if( 0 == rng() % 16 )
                clock.start( true );
            std::this_thread::sleep_for( std::chrono::microseconds( 300 + rng() % 1000 ) );
        }
    } );

    while( std::chrono::steady_clock::now() < deadline )
        std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
    running.store( false );
    tickThread.join();
    messageThread.join();

    clock.setTempo( 90.f );
    ok &= check( 90.f == clock.getTempo(), "the last tempo set reads back" );
    clock.stop();
    clock.onTick = []( uint32_t ) {};

    std::printf( "%ld tempo changes, last tick %u\n", changes.load(), lastTick.load() );
    std::printf( "%s\n", ok ? "PASS" : "FAIL" );
    return ok ? 0 : 1;
}
