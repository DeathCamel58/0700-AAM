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

// Tests of what the performance overlay shows (kick_PerformanceStats.h): the
// rolling histories its graphs draw, the CPU, memory and xrun figures and
// how they are read from /proc, and how values are written. And of the
// setting that keeps it on (Window/windowperfoverlay), through the same
// ValueTree attachment the preferences use. Built with
// -DKICK_BUILD_TESTS=ON; it needs no arguments and creates no window (no
// juce::Desktop, no display).

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include "common/source/kick_atomics.h"
#include "kick_ui/performance/kick_PerformanceStats.cpp"

#include <cstdio>
#include <fstream>
#include <sstream>

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

    namespace perf = kick_ui::perf;

    bool near( double a, double b )
    {
        return std::abs( a - b ) < 1.0e-6;
    }

    void testHistory()
    {
        perf::History history( 4 );
        CHECK( history.capacity() == 4 );
        CHECK( history.size() == 0 );
        CHECK( juce::exactlyEqual( history.latest(), 0.0f ) );

        history.push( 1.0f );
        history.push( 2.0f );
        CHECK( history.size() == 2 );
        CHECK( juce::exactlyEqual( history[0], 1.0f ) );
        CHECK( juce::exactlyEqual( history[1], 2.0f ) );
        CHECK( juce::exactlyEqual( history.latest(), 2.0f ) );

        // Full: the oldest go first, the order stays oldest first.
        for( auto value : { 3.0f, 4.0f, 5.0f, 6.0f, 7.0f } )
            history.push( value );
        CHECK( history.size() == 4 );
        CHECK( juce::exactlyEqual( history[0], 4.0f ) );
        CHECK( juce::exactlyEqual( history[1], 5.0f ) );
        CHECK( juce::exactlyEqual( history[2], 6.0f ) );
        CHECK( juce::exactlyEqual( history[3], 7.0f ) );
        CHECK( juce::exactlyEqual( history.latest(), 7.0f ) );

        // Around the end many times over.
        for( int i = 0; i < 1001; ++i )
            history.push( (float)i );
        CHECK( juce::exactlyEqual( history[0], 997.0f ) );
        CHECK( juce::exactlyEqual( history.latest(), 1000.0f ) );

        history.clear();
        CHECK( history.size() == 0 );
        history.push( 9.0f );
        CHECK( history.size() == 1 );
        CHECK( juce::exactlyEqual( history[0], 9.0f ) );

        // A capacity below one is one.
        perf::History one( 0 );
        CHECK( one.capacity() == 1 );
        one.push( 1.0f );
        one.push( 2.0f );
        CHECK( one.size() == 1 && juce::exactlyEqual( one.latest(), 2.0f ) );
    }

    void testProcStat()
    {
        const std::string stat = "cpu  100 20 30 800 50 5 5 10 7 0\n"
                                 "cpu0 50 10 15 400 25 2 3 5 0 0\n"
                                 "intr 12345\n";
        auto times = perf::parseProcStat( stat );
        CHECK( times.has_value() );
        CHECK( times->total == 100 + 20 + 30 + 800 + 50 + 5 + 5 + 10 ); // guest is in user
        CHECK( times->idle == 800 + 50 );

        // An old kernel's four fields.
        times = perf::parseProcStat( "cpu 1 2 3 4\n" );
        CHECK( times.has_value() && times->total == 10 && times->idle == 4 );

        CHECK( !perf::parseProcStat( "" ).has_value() );
        CHECK( !perf::parseProcStat( "cpu0 1 2 3 4\n" ).has_value() );
        CHECK( !perf::parseProcStat( "cpu x y\n" ).has_value() );

        // Half busy over the period, then nothing at all elapsed.
        perf::CpuTimes before{ 1000, 600 }, after{ 1200, 700 };
        CHECK( near( perf::busyPercent( before, after ), 50.0 ) );
        CHECK( near( perf::busyPercent( after, after ), 0.0 ) );
        CHECK( near( perf::busyPercent( after, before ), 0.0 ) ); // backwards: no time
        CHECK( near( perf::busyPercent( { 0, 0 }, { 100, 100 } ), 0.0 ) );
        CHECK( near( perf::busyPercent( { 0, 0 }, { 100, 0 } ), 100.0 ) );
    }

    void testProcessCpu()
    {
        // utime 1500 and stime 250 are fields 14 and 15; the name has
        // spaces and parentheses.
        const std::string stat = "4242 (Kick (UI) x) S 1 4242 4242 0 -1 4194560 9000 0 3 0 1500 250 0 0 20 0 12 0 "
                                 "500 300000000 20000 18446744073709551615 1 1 0 0 0 0 0 4096 0 0 0 0 17 2 0 0 0 0 0\n";
        auto ticks = perf::parseProcessCpuTicks( stat );
        CHECK( ticks.has_value() && *ticks == 1750 );

        CHECK( !perf::parseProcessCpuTicks( "" ).has_value() );
        CHECK( !perf::parseProcessCpuTicks( "4242 (Kick) S 1 2 3" ).has_value() );
        CHECK( !perf::parseProcessCpuTicks( "4242 Kick S 1 2 3 4 5 6 7 8 9 10 11 12 13" ).has_value() );

        // One core of four busy with Kick: 25% of all of them.
        perf::CpuTimes before{ 0, 0 }, after{ 400, 300 };
        CHECK( near( perf::processPercent( 1000, 1100, before, after ), 25.0 ) );
        CHECK( near( perf::processPercent( 1000, 1000, before, after ), 0.0 ) );
        CHECK( near( perf::processPercent( 1100, 1000, before, after ), 0.0 ) );
        CHECK( near( perf::processPercent( 0, 100, after, after ), 0.0 ) );
        CHECK( near( perf::processPercent( 0, 1000, before, after ), 100.0 ) ); // never above all of them

        CHECK( perf::parseResidentPages( "51200 38000 2000 100 0 30000 0\n" ) == juce::uint64( 38000 ) );
        CHECK( !perf::parseResidentPages( "51200" ).has_value() );
    }

    std::string readFile( const char* path )
    {
        std::ifstream     file( path );
        std::stringstream contents;
        contents << file.rdbuf();
        return contents.str();
    }

    void testThisMachine()
    {
        // The parsers read what this kernel writes.
        const auto stat = perf::parseProcStat( readFile( "/proc/stat" ) );
        CHECK( stat.has_value() && stat->total > 0 && stat->idle <= stat->total );
        const auto ticks = perf::parseProcessCpuTicks( readFile( "/proc/self/stat" ) );
        CHECK( ticks.has_value() );
        const auto pages = perf::parseResidentPages( readFile( "/proc/self/statm" ) );
        CHECK( pages.has_value() && *pages > 0 );
    }

    void testXRuns()
    {
        CHECK( perf::xrunsSince( 0, 0 ) == 0 );
        CHECK( perf::xrunsSince( 3, 5 ) == 2 );
        CHECK( perf::xrunsSince( 5, 2 ) == 2 );  // opened again: its count started over
        CHECK( perf::xrunsSince( 5, -1 ) == 0 ); // does not count them
        CHECK( perf::xrunsSince( -1, 4 ) == 4 ); // counted from nothing
    }

    void testRecentFrames()
    {
        // Five periods of 200 ms: the last second.
        perf::RecentFrames recent( 5 );
        CHECK( near( recent.framesPerSecond(), 0.0 ) );
        CHECK( near( recent.longestPaintMs(), 0.0 ) );

        // The OpenGL renderer in QEMU: a frame of 120 ms every other period.
        // Each period alone read 0 fps and 0.0 ms whenever the overlay's
        // texts were painted; over the last second, the frames show.
        for( int i = 0; i < 10; ++i )
        {
            const bool painted = i % 2 == 0;
            recent.add( 200.0, painted ? 1 : 0, painted ? 120.0 : 0.0, painted ? 130.0 : 14.0 );
        }
        CHECK( near( recent.framesPerSecond(), 2.0 ) ); // 2 frames in the last 5 periods
        CHECK( near( recent.longestPaintMs(), 120.0 ) );
        CHECK( near( recent.longestTickGapMs(), 130.0 ) );
        CHECK( perf::formatFramesPerSecond( recent.framesPerSecond() ) == "2.0 fps" );

        // The software renderer at the display's rate: as a single period.
        for( int i = 0; i < 5; ++i )
            recent.add( 200.0, 12, 3.0 + i, 17.0 );
        CHECK( near( recent.framesPerSecond(), 60.0 ) );
        CHECK( near( recent.longestPaintMs(), 7.0 ) );
        CHECK( near( recent.longestTickGapMs(), 17.0 ) );

        // Periods of other lengths (a late timer) are weighed by length.
        recent.clear();
        recent.add( 100.0, 6, 1.0, 16.0 );
        recent.add( 300.0, 18, 2.0, 16.0 );
        CHECK( near( recent.framesPerSecond(), 60.0 ) );

        // Nothing painted: no frames, no division by zero.
        recent.clear();
        recent.add( 0.0, 0, 0.0, 0.0 );
        CHECK( near( recent.framesPerSecond(), 0.0 ) );
    }

    void testFormatting()
    {
        CHECK( perf::formatPercent( 0.0 ) == "0%" );
        CHECK( perf::formatPercent( 33.4 ) == "33%" );
        CHECK( perf::formatPercent( 99.6 ) == "100%" );
        CHECK( perf::formatPercent( -3.0 ) == "0%" );
        CHECK( perf::formatPercent( 140.0 ) == "140%" ); // an audio callback that overran

        CHECK( perf::formatMilliseconds( 0.0 ) == "0.0 ms" );
        CHECK( perf::formatMilliseconds( 3.14 ) == "3.1 ms" );
        CHECK( perf::formatMilliseconds( 9.9 ) == "9.9 ms" );
        CHECK( perf::formatMilliseconds( 9.97 ) == "10 ms" );
        CHECK( perf::formatMilliseconds( 16.67 ) == "17 ms" );
        CHECK( perf::formatMilliseconds( 1234.0 ) == "1234 ms" );
        CHECK( perf::formatMilliseconds( -1.0 ) == "0.0 ms" );

        CHECK( perf::formatMegabytes( 0.0 ) == "0 MB" );
        CHECK( perf::formatMegabytes( 812.4 ) == "812 MB" );
        CHECK( perf::formatMegabytes( 1023.0 ) == "1023 MB" );
        CHECK( perf::formatMegabytes( 1024.0 ) == "1.0 GB" );
        CHECK( perf::formatMegabytes( 1987.0 ) == "1.9 GB" );

        CHECK( perf::formatFramesPerSecond( 0.0 ) == "0 fps" );
        CHECK( perf::formatFramesPerSecond( 59.7 ) == "60 fps" );
        CHECK( perf::formatFramesPerSecond( 2.5 ) == "2.5 fps" );
        CHECK( perf::formatFramesPerSecond( 9.96 ) == "10 fps" );
        CHECK( perf::formatFramesPerSecond( -1.0 ) == "0 fps" );

        CHECK( perf::formatSampleRate( 48000.0 ) == "48 kHz" );
        CHECK( perf::formatSampleRate( 44100.0 ) == "44.1 kHz" );
        CHECK( perf::formatSampleRate( 96000.0 ) == "96 kHz" );
    }

    void testSetting()
    {
        // As the preferences attach it (KickWindow::setup()).
        const juce::Identifier window( "WINDOW" ), property( "windowperfoverlay" );
        juce::ValueTree        preferences( "PREFERENCES" );
        auto                   windowTree = preferences.getOrCreateChildWithName( window, nullptr );

        mt::Atomic<bool> overlay;
        overlay.attachTo( windowTree, property, nullptr, false );

        // Off in preferences written before it existed.
        CHECK( !overlay.get() );
        CHECK( !windowTree.hasProperty( property ) );

        // The toggle's state and the overlay refer to the same value.
        juce::Value toggle, shown;
        toggle.referTo( overlay.getValue() );
        shown.referTo( overlay.getValue() );
        CHECK( !(bool)toggle.getValue() );

        toggle = true;
        CHECK( overlay.get() );
        CHECK( (bool)shown.getValue() );
        CHECK( (bool)windowTree[property] );

        // Saved and read back: still on.
        auto xml = preferences.createXml();
        CHECK( xml != nullptr );
        auto restored = juce::ValueTree::fromXml( xml->toString() );
        auto restoredWindow = restored.getOrCreateChildWithName( window, nullptr );

        mt::Atomic<bool> restoredOverlay;
        restoredOverlay.attachTo( restoredWindow, property, nullptr, false );
        CHECK( restoredOverlay.get() );

        // Turned off: saved off.
        toggle = false;
        CHECK( !overlay.get() );
        CHECK( !(bool)shown.getValue() );
        restored = juce::ValueTree::fromXml( preferences.createXml()->toString() );
        restoredWindow = restored.getOrCreateChildWithName( window, nullptr );
        mt::Atomic<bool> restoredAgain;
        restoredAgain.attachTo( restoredWindow, property, nullptr, false );
        CHECK( !restoredAgain.get() );
    }
}

int main()
{
    testHistory();
    testProcStat();
    testProcessCpu();
    testThisMachine();
    testXRuns();
    testRecentFrames();
    testFormatting();
    testSetting();

    if( failures != 0 )
    {
        std::printf( "%d check(s) failed\n", failures );
        return 1;
    }

    std::printf( "All performance overlay tests passed\n" );
    return 0;
}
