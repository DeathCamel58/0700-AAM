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

#include "kick_Resorces.h"
#include "kick_SysFile.h"

#if( JUCE_LINUX || __linux__ )
    #include <sys/statvfs.h>
#endif

#include <filesystem>

namespace mt
{
    void SystemResource::cpuInfo( double& cpuUsage )
    {
#if( JUCE_LINUX || __linux__ )
        // The first line of /proc/stat holds the time all CPUs have spent
        // in each state since boot; the usage is the share of non-idle time
        // since the previous call.
        std::string stat;
        sysfile::read( "/proc/stat", stat );
        std::istringstream iss( stat.substr( 0, stat.find( '\n' ) ) );

        std::string cpuLabel;
        long long   user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
        iss >> cpuLabel >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

        // guest and guest_nice are already counted in user and nice.
        const long long totalTime = user + nice + system + idle + iowait + irq + softirq + steal;
        const long long idleTime  = idle + iowait;

        static long long lastTotal = 0, lastIdle = 0;
        const long long  total     = totalTime - lastTotal;
        const long long  idled     = idleTime - lastIdle;
        lastTotal                  = totalTime;
        lastIdle                   = idleTime;

        cpuUsage = total > 0 ? static_cast<double>( total - idled ) / static_cast<double>( total ) * 100.0 : 0.0;
#elif( defined( _WIN32 ) || defined( _WIN64 ) )
        // Dummy data for Windows
        static double dummyCpuUsage = 0.0;
        dummyCpuUsage += 5.0;
        if( dummyCpuUsage > 100.0 )
        {
            dummyCpuUsage = 0.0;
        }
        cpuUsage = dummyCpuUsage;
#else
        // Handle other platforms if necessary
        cpuUsage = 0.0;
#endif
    }

    void SystemResource::sizeInfo( std::string& path, sysSizeInfo& info )
    {
        info.status    = false;
        info.avaSize   = 0;
        info.usedSize  = 0;
        info.totalSize = 0;
        info.perSize   = 0;
#if( JUCE_LINUX || __linux__ )
        // The sizes of the file system path is on, in whole MiB, as df counts
        // them: available is what an unprivileged user can still write,
        // which leaves out the blocks reserved for root, and the percentage
        // used is of the space users can have.
        struct statvfs fs {};
        if( path.empty() || statvfs( path.c_str(), &fs ) != 0 || fs.f_blocks == 0 )
            return;

        const auto mebibytes = [&fs]( unsigned long long blocks ) { return (double)( blocks * fs.f_frsize / ( 1024 * 1024 ) ); };
        const auto used      = fs.f_blocks - fs.f_bfree;

        info.status    = true;
        info.totalSize = mebibytes( fs.f_blocks );
        info.usedSize  = mebibytes( used );
        info.avaSize   = mebibytes( fs.f_bavail );

        const auto usable = used + fs.f_bavail;
        info.perSize      = usable > 0 ? (int)( ( used * 100 + usable - 1 ) / usable ) : 0;
        DBG( path << ": " << info.totalSize << " MiB, " << info.usedSize << " used, " << info.avaSize << " available, " << info.perSize << "%" );
#elif( defined( _WIN32 ) || defined( _WIN64 ) )
        // Dummy data for Windows
        info.status    = true;
        info.avaSize   = 500; // Dummy available size in MB
        info.usedSize  = 250; // Dummy used size in MB
        info.totalSize = 750; // Dummy total size in MB
        info.perSize   = 33; // Dummy percentage used
#endif
    }

    void SystemResource::ramInfo( sysRamInfo& info )
    {
#if( JUCE_LINUX || __linux__ )
        // MemAvailable is the kernel's estimate of what can be allocated
        // without swapping, page cache and reclaimable memory included;
        // MemFree leaves those out, so memory looked nearly full whenever
        // files had been read.
        std::string meminfo;
        sysfile::read( "/proc/meminfo", meminfo );

        unsigned long long totalKiB = 0, availableKiB = 0;
        std::istringstream lines( meminfo );
        for( std::string line; std::getline( lines, line ); )
        {
            std::istringstream fields( line );
            std::string        name;
            unsigned long long value = 0;
            if( !( fields >> name >> value ) )
                continue;
            if( name == "MemTotal:" )
                totalKiB = value;
            else if( name == "MemAvailable:" )
                availableKiB = value;
        }

        info.totalRam = (double)( totalKiB / 1024 );
        info.freeRam  = (double)( availableKiB / 1024 );
        info.usedRam  = (double)( ( totalKiB - std::min( availableKiB, totalKiB ) ) / 1024 );
        info.perRam   = totalKiB > 0 ? (int)( ( totalKiB - std::min( availableKiB, totalKiB ) ) * 100 / totalKiB ) : 0;
#elif( defined( _WIN32 ) || defined( _WIN64 ) )
        // Dummy data for Windows
        info.totalRam = 100;
        info.usedRam  = 50;
        info.freeRam  = 50;
        info.perRam   = 50;
#endif
    }

    void SystemResource::folderSize( std::string path )
    {
        // The size of the files in the folder and below it (what du counts,
        // less the space directories themselves take).
        namespace fs = std::filesystem;

        std::error_code ec;
        std::uintmax_t  size = 0;
        for( fs::recursive_directory_iterator it( path, fs::directory_options::skip_permission_denied, ec ), end; !ec && it != end; it.increment( ec ) )
        {
            if( it->is_regular_file( ec ) && !it->is_symlink( ec ) )
            {
                const auto fileSize = it->file_size( ec );
                if( !ec )
                    size += fileSize;
            }
            ec.clear();
        }

        std::cout << "Size of folder '" << path << "': " << size << " bytes" << std::endl;
    }
}
