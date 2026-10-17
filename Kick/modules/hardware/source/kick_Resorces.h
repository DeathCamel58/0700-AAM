/*
 * Copyright (c) 2021 MUSIC Tribe
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
#include <string>
#if( JUCE_LINUX || __linux__ )
    #include <iostream>
    #include <sys/sysinfo.h>
    #include <chrono>
    #include <cstdio>
    #include <cstring>
    #include <thread>
    #include <fstream>
    #include <sstream>
    #include <unistd.h>
#endif
namespace mt
{

    struct sysSizeInfo
    {
        bool   status    = false;
        double avaSize   = 0;
        double usedSize  = 0;
        double totalSize = 0;
        int    perSize   = 0;
    };
    struct sysRamInfo
    {
        double totalRam = 0;
        double usedRam  = 0;
        double freeRam  = 0;
        int    perRam   = 0;
    };
    /** The time all CPUs have spent since boot, from /proc/stat's first
        line, in clock ticks. */
    struct cpuTimes
    {
        long long total = 0;
        long long idle  = 0;
    };
    class SystemResource
    {
    public:
        SystemResource() = default;

        void ramInfo( sysRamInfo& info );
        void sizeInfo( std::string& path, sysSizeInfo& info );
        /** The share of CPU time in use since the previous call (or the
            last startCpuMeasurement()), in percent. False, and 0, if there
            was no previous reading to compare with. */
        bool cpuInfo( double& cpuUsage );
        /** Takes the reading the next cpuInfo() compares with, so that a
            stale one from long ago is not used. */
        void startCpuMeasurement();

        // The parsing, apart from the files, so it can be tested.
        static bool   parseCpuTimes( const std::string& procStat, cpuTimes& times );
        static double cpuUsageBetween( const cpuTimes& earlier, const cpuTimes& later );
        static bool   parseMemInfo( const std::string& procMeminfo, sysRamInfo& info );

        void                   folderSize( std::string folderPath );
        static SystemResource& getResourceInterface()
        {
            static SystemResource _sysResource;
            return _sysResource;
        }

    private:
        cpuTimes lastCpu;
        bool     haveLastCpu = false;
    };
}
