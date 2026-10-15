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
#include "kick_Process.h"

namespace mt
{
    void SystemResource::cpuInfo( double& cpuUsage )
    {
#if( JUCE_LINUX || __linux__ )
        std::ifstream statFile( "/proc/stat" );
        std::string   line;

        // Read the first line of /proc/stat, which contains total CPU time information
        std::getline( statFile, line );

        // Parse the line to extract CPU time values
        std::istringstream iss( line );
        std::string        cpuLabel;
        long               user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;

        iss >> cpuLabel >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal >> guest >> guest_nice;

        // Calculate the total CPU time
        long totalTime = user + nice + system + idle + iowait + irq + softirq + steal + guest + guest_nice;

        // Calculate CPU usage as a percentage
        cpuUsage = static_cast<double>( totalTime - idle ) / totalTime * 100.0;
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
        info.status = false;
#if( JUCE_LINUX || __linux__ )
        juce::File fileToCheck( path );
        if( fileToCheck.exists() )
        {
            info.status = true;

            // Run `df -h <path>` (without a shell: path can be a mount
            // point named after a removable disk's label) and capture its
            // output
            juce::String output;
            if( process::run( { "df", "-h", path }, &output ) < 0 )
            {
                std::cerr << "Error executing command." << std::endl;
                return;
            }

            std::istringstream lines( output.toStdString() );
            std::string        filesystem, size, used, available, usePercentage, mountPoint;
            for( std::string line; std::getline( lines, line ); )
            {
                if( line.find( path ) != std::string::npos )
                {
                    std::istringstream iss( line );
                    iss >> filesystem >> size >> used >> available >> usePercentage >> mountPoint;
                    DBG( " " << filesystem << "Size -> " << size << "Used" << used << "Availd" << available << "Used Per" << usePercentage );
                    info.avaSize   = juce::String( available ).getIntValue();
                    info.usedSize  = juce::String( used ).getIntValue();
                    info.totalSize = juce::String( size ).getIntValue();
                    info.perSize   = juce::String( usePercentage ).getIntValue();
                }
            }
        }
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
        struct sysinfo memInfo;
        sysinfo( &memInfo );

        // Calculate total RAM, used RAM, and available RAM
        unsigned long long totalRAM = memInfo.totalram * memInfo.mem_unit;
        unsigned long long freeRAM  = memInfo.freeram * memInfo.mem_unit;
        unsigned long long usedRAM  = totalRAM - freeRAM;

        unsigned long long availableRAM = freeRAM;
        info.totalRam                   = totalRAM / ( 1024 * 1024 );
        info.usedRam                    = usedRAM / ( 1024 * 1024 );
        info.freeRam                    = availableRAM / ( 1024 * 1024 );
        info.perRam                     = ( ( info.usedRam * 100 ) / info.totalRam );
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
#if( JUCE_LINUX || __linux__ )
        const char* folderPath = path.c_str();

        // Run du -sh on the specified folder (without a shell) and capture
        // its output
        juce::String output;
        if( process::run( { "du", "-sh", path }, &output ) < 0 )
        {
            std::cerr << "Error executing du command." << std::endl;
            return;
        }
        std::string result = output.toStdString();

        // Parse the result to extract the size
        char sizeStr[16];
        if( sscanf( result.c_str(), "%s", sizeStr ) == 1 )
        {
            std::cout << "Size of folder '" << folderPath << "': " << sizeStr << std::endl;
        }
        else
        {
            std::cerr << "Error parsing du command output." << std::endl;
            return;
        }
#endif
    }
}
