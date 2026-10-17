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

#include "kick_PerformanceStats.h"

#include <sstream>

namespace kick_ui::perf
{
    History::History( int capacity )
        : values( (size_t)juce::jmax( 1, capacity ), 0.0f )
    {
    }

    void History::push( float value )
    {
        values[(size_t)next] = value;
        next                 = ( next + 1 ) % capacity();
        count                = juce::jmin( count + 1, capacity() );
    }

    void History::clear()
    {
        next  = 0;
        count = 0;
    }

    float History::operator[]( int index ) const
    {
        jassert( index >= 0 && index < count );
        const auto oldest = ( next - count + capacity() ) % capacity();
        return values[(size_t)( ( oldest + index ) % capacity() )];
    }

    float History::latest() const
    {
        return count > 0 ? ( *this )[count - 1] : 0.0f;
    }

    std::optional<CpuTimes> parseProcStat( const std::string& procStat )
    {
        std::istringstream line( procStat.substr( 0, procStat.find( '\n' ) ) );

        std::string  label;
        juce::uint64 user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
        if( !( line >> label >> user >> nice >> system >> idle ) || label != "cpu" )
            return std::nullopt;

        // Kernels before 2.6 stop here; later ones have all of them.
        line >> iowait >> irq >> softirq >> steal;

        // guest and guest_nice are counted in user and nice already.
        CpuTimes times;
        times.total = user + nice + system + idle + iowait + irq + softirq + steal;
        times.idle  = idle + iowait;
        return times;
    }

    std::optional<juce::uint64> parseProcessCpuTicks( const std::string& procPidStat )
    {
        // "pid (comm) state ppid ...": the name can hold spaces and
        // parentheses, so the fields are counted from its last ')'. utime
        // and stime are fields 14 and 15; state, the first after it, is 3.
        const auto nameEnd = procPidStat.rfind( ')' );
        if( nameEnd == std::string::npos )
            return std::nullopt;

        std::istringstream fields( procPidStat.substr( nameEnd + 1 ) );
        std::string        field;
        for( int number = 3; number < 14; ++number )
            if( !( fields >> field ) )
                return std::nullopt;

        juce::uint64 utime = 0, stime = 0;
        if( !( fields >> utime >> stime ) )
            return std::nullopt;
        return utime + stime;
    }

    std::optional<juce::uint64> parseResidentPages( const std::string& procPidStatm )
    {
        std::istringstream fields( procPidStatm );
        juce::uint64       size = 0, resident = 0;
        if( !( fields >> size >> resident ) )
            return std::nullopt;
        return resident;
    }

    namespace
    {
        // Counters only grow; a smaller one (wrapped, or another source)
        // counts as no time.
        juce::uint64 elapsed( juce::uint64 before, juce::uint64 after )
        {
            return after > before ? after - before : 0;
        }
    }

    double busyPercent( CpuTimes before, CpuTimes after )
    {
        const auto total = elapsed( before.total, after.total );
        const auto idle  = juce::jmin( elapsed( before.idle, after.idle ), total );
        return total > 0 ? (double)( total - idle ) * 100.0 / (double)total : 0.0;
    }

    double processPercent( juce::uint64 processTicksBefore, juce::uint64 processTicksAfter, CpuTimes before, CpuTimes after )
    {
        const auto total = elapsed( before.total, after.total );
        if( total == 0 )
            return 0.0;
        return juce::jmin( 100.0, (double)elapsed( processTicksBefore, processTicksAfter ) * 100.0 / (double)total );
    }

    int xrunsSince( int previousCount, int count )
    {
        if( count < 0 )
            return 0;
        if( count < previousCount )
            return count;
        return count - juce::jmax( 0, previousCount );
    }

    juce::String formatPercent( double percent )
    {
        return juce::String( juce::roundToInt( juce::jmax( 0.0, percent ) ) ) + "%";
    }

    juce::String formatMilliseconds( double ms )
    {
        ms = juce::jmax( 0.0, ms );
        // Tenths while they matter.
        if( ms < 9.95 )
            return juce::String( ms, 1 ) + " ms";
        return juce::String( juce::roundToInt( ms ) ) + " ms";
    }

    juce::String formatMegabytes( double mebibytes )
    {
        mebibytes = juce::jmax( 0.0, mebibytes );
        if( mebibytes < 1023.5 )
            return juce::String( juce::roundToInt( mebibytes ) ) + " MB";
        return juce::String( mebibytes / 1024.0, 1 ) + " GB";
    }

    juce::String formatFramesPerSecond( double rate )
    {
        return juce::String( juce::roundToInt( juce::jmax( 0.0, rate ) ) ) + " fps";
    }

    juce::String formatSampleRate( double hz )
    {
        const auto kilohertz = juce::roundToInt( hz / 100.0 ) / 10.0;
        if( juce::exactlyEqual( kilohertz, std::floor( kilohertz ) ) )
            return juce::String( (int)kilohertz ) + " kHz";
        return juce::String( kilohertz, 1 ) + " kHz";
    }
}
