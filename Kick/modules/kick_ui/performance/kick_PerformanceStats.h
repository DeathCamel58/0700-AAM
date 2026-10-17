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

#pragma once

#include <juce_core/juce_core.h>

#include <optional>
#include <string>
#include <vector>

// What the performance overlay shows, apart from showing it: the rolling
// histories it draws, the figures read from /proc, and how values are
// written. Plain code, tested in tests/performance_overlay.
namespace kick_ui::perf
{
    // The latest values, up to a fixed number, oldest first. Pushing never
    // allocates.
    class History
    {
    public:
        explicit History( int capacity );

        void  push( float value );
        void  clear();
        int   size() const { return count; }
        int   capacity() const { return (int)values.size(); }
        float operator[]( int index ) const; // 0: the oldest
        float latest() const;                // 0 when empty

    private:
        std::vector<float> values;
        int                next{ 0 };
        int                count{ 0 };
    };

    // The window's frames over the last few periods the overlay sampled
    // (FrameTiming::Period): what its texts show. A period alone is too
    // short when frames come slower than the overlay samples (the OpenGL
    // renderer on a slow GPU, or a software one): most periods then hold
    // no frame, and the overlay's own repaint, the frame that would show
    // a period with one, is painted only after the next, empty, period was
    // sampled, so that the texts always read "0 fps, paint 0.0 ms" while
    // frames were painted.
    class RecentFrames
    {
    public:
        explicit RecentFrames( int periods );

        void add( double lengthMs, int frames, double longestPaintMs, double longestTickGapMs );
        void clear();

        double framesPerSecond() const; // 0 without time
        double longestPaintMs() const;
        double longestTickGapMs() const;

    private:
        struct Period
        {
            double lengthMs{ 0.0 };
            int    frames{ 0 };
            double longestPaintMs{ 0.0 };
            double longestTickGapMs{ 0.0 };
        };

        std::vector<Period> periods;
        int                 next{ 0 };
        int                 count{ 0 };
    };

    // The time all CPUs have spent since boot, from the "cpu" line of
    // /proc/stat, in clock ticks.
    struct CpuTimes
    {
        juce::uint64 total{ 0 };
        juce::uint64 idle{ 0 }; // idle and waiting for I/O
    };

    std::optional<CpuTimes>     parseProcStat( const std::string& procStat );
    // The CPU time of a process, user and system, from /proc/<pid>/stat,
    // in the same clock ticks.
    std::optional<juce::uint64> parseProcessCpuTicks( const std::string& procPidStat );
    // The resident set of a process, in pages, from /proc/<pid>/statm.
    std::optional<juce::uint64> parseResidentPages( const std::string& procPidStatm );

    // The share of all CPUs that was busy between two readings, and the
    // share of all CPUs a process used (the same scale: a process keeping
    // one of four cores busy is at 25%). Percent; 0 without time between.
    double busyPercent( CpuTimes before, CpuTimes after );
    double processPercent( juce::uint64 processTicksBefore, juce::uint64 processTicksAfter, CpuTimes before, CpuTimes after );

    // The xruns since the previous count. A count lower than before is a
    // device opened again (its count starts over); below zero, the device
    // does not count them.
    int xrunsSince( int previousCount, int count );

    juce::String formatPercent( double percent );       // "34%"
    juce::String formatMilliseconds( double ms );       // "3.2 ms", "17 ms"
    juce::String formatMegabytes( double mebibytes );   // "812 MB", "1.9 GB"
    juce::String formatFramesPerSecond( double rate );  // "24 fps", "2.5 fps"
    juce::String formatSampleRate( double hz );         // "48 kHz", "44.1 kHz"
}
