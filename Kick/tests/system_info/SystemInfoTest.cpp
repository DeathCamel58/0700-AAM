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

// Tests of the system information the preferences and the system resources
// window show: CPU and memory use parsed from /proc. The parsers are fed
// fixture copies of the files, so the results do not depend on this
// machine. Built with -DKICK_BUILD_TESTS=ON; it needs no arguments, creates
// no window and changes nothing on the system.

#include <juce_core/juce_core.h>

#include "hardware/source/kick_Resorces.cpp"

#include <cstdio>

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

    bool near( double a, double b )
    {
        return std::abs( a - b ) < 1.0e-6;
    }

    void testCpu()
    {
        using mt::SystemResource;

        mt::cpuTimes earlier, later;
        CHECK( SystemResource::parseCpuTimes( "cpu  100 0 100 700 100 0 0 0 0 0\ncpu0 50 0 50 350 50 0 0 0 0 0\n", earlier ) );
        CHECK( earlier.total == 1000 );
        CHECK( earlier.idle == 800 );

        // 400 more ticks, 100 of them idle: 75 % busy.
        CHECK( SystemResource::parseCpuTimes( "cpu  250 0 250 790 110 0 0 0 0 0\n", later ) );
        CHECK( near( SystemResource::cpuUsageBetween( earlier, later ), 75.0 ) );

        // No time passed, or the counters went back: no figure, not a
        // division by zero.
        CHECK( near( SystemResource::cpuUsageBetween( later, later ), 0.0 ) );
        CHECK( near( SystemResource::cpuUsageBetween( later, earlier ), 0.0 ) );

        // An old kernel's four columns.
        CHECK( SystemResource::parseCpuTimes( "cpu 10 20 30 40\n", later ) );
        CHECK( later.total == 100 && later.idle == 40 );

        // Not /proc/stat.
        CHECK( !SystemResource::parseCpuTimes( "", later ) );
        CHECK( !SystemResource::parseCpuTimes( "intr 1 2 3\n", later ) );

        // The first reading only takes a baseline; the next one compares.
        SystemResource resources;
        double         usage = -1.0;
        CHECK( !resources.cpuInfo( usage ) );
        CHECK( near( usage, 0.0 ) );
        resources.startCpuMeasurement();
        volatile double spin = 0.0;
        for( int i = 0; i < 50000000; ++i )
            spin = spin + 1.0;
        CHECK( resources.cpuInfo( usage ) );
        CHECK( usage >= 0.0 && usage <= 100.0 );
        std::printf( "CPU in use here: %.1f %%\n", usage );
    }

    void testMemory()
    {
        using mt::SystemResource;

        const char* meminfo = "MemTotal:        2048000 kB\n"
                              "MemFree:          100000 kB\n"
                              "MemAvailable:    1536000 kB\n"
                              "Buffers:           20000 kB\n"
                              "Cached:          1200000 kB\n";
        mt::sysRamInfo info;
        CHECK( SystemResource::parseMemInfo( meminfo, info ) );
        CHECK( near( info.totalRam, 2000.0 ) );
        CHECK( near( info.freeRam, 1500.0 ) );
        CHECK( near( info.usedRam, 500.0 ) );
        CHECK( info.perRam == 25 );

        // Nothing read: nothing known, and all zeros, not leftovers.
        info.perRam = 77;
        CHECK( !SystemResource::parseMemInfo( "", info ) );
        CHECK( info.perRam == 0 && near( info.totalRam, 0.0 ) );
        CHECK( !SystemResource::parseMemInfo( "MemTotal: 1000 kB\n", info ) );

        // This machine's own file parses, with a believable figure.
        mt::sysRamInfo here;
        SystemResource().ramInfo( here );
        CHECK( here.totalRam > 0 );
        CHECK( here.perRam > 0 && here.perRam < 100 );
        std::printf( "RAM in use here: %d %% of %.0f MiB\n", here.perRam, here.totalRam );
    }

    void testStorage()
    {
        mt::sysSizeInfo info;
        std::string     root = "/";
        mt::SystemResource().sizeInfo( root, info );
        CHECK( info.status );
        CHECK( info.totalSize > 0 && info.perSize >= 0 && info.perSize <= 100 );

        std::string missing = "/nonexistent/kick/test";
        mt::SystemResource().sizeInfo( missing, info );
        CHECK( !info.status );
        CHECK( info.perSize == 0 );

        std::string empty;
        mt::SystemResource().sizeInfo( empty, info );
        CHECK( !info.status );
    }
}

int main()
{
    testCpu();
    testMemory();
    testStorage();

    if( failures != 0 )
    {
        std::printf( "%d check(s) failed\n", failures );
        return 1;
    }

    std::printf( "All system information tests passed\n" );
    return 0;
}
