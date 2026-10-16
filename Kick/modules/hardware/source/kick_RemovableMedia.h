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

#include <cstdlib>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined( __linux__ )
    #include <dirent.h>
    #include <sys/stat.h>
    #include <sys/sysmacros.h>
    #include <unistd.h>
#endif

#include "kick_SysFile.h"

// The directory removable disks are mounted below; the build system can
// override it (see CMakeLists.txt).
#ifndef KICK_MEDIA_DIR
    #define KICK_MEDIA_DIR "/mnt"
#endif

namespace mt
{
    enum class RemovableMedia
    {
        SdCard, // an SD card, in a card slot on the MMC bus
        UsbDisk // a USB mass storage device
    };

    namespace media
    {
        struct Mount
        {
            std::string source;
            std::string target;
        };

        /** A field of /proc/mounts or /proc/self/mountinfo, which write
            space, tab, newline and backslash as octal escapes (\040 and so
            on). */
        inline std::string unescapeMountField( const std::string& escaped )
        {
            std::string text;
            for( std::size_t i = 0; i < escaped.size(); ++i )
            {
                const bool octal = escaped[i] == '\\' && i + 3 < escaped.size()
                                   && escaped.find_first_not_of( "01234567", i + 1 ) >= i + 4;
                if( octal )
                {
                    text += (char)std::stoi( escaped.substr( i + 1, 3 ), nullptr, 8 );
                    i += 3;
                }
                else
                    text += escaped[i];
            }
            return text;
        }

        /** The mounts listed in /proc/mounts' format. */
        inline std::vector<Mount> parseMounts( const std::string& contents )
        {
            std::vector<Mount> mounts;
            std::istringstream lines( contents );
            for( std::string line; std::getline( lines, line ); )
            {
                std::istringstream fields( line );
                std::string        source, target;
                if( fields >> source >> target )
                    mounts.push_back( { unescapeMountField( source ), unescapeMountField( target ) } );
            }
            return mounts;
        }

        inline std::vector<Mount> mounts()
        {
            std::string contents;
            sysfile::read( "/proc/mounts", contents );
            return parseMounts( contents );
        }

        /** The device number ("major:minor") of the file system mounted on
            "/", from the contents of /proc/self/mountinfo. /proc/mounts does
            not tell: it names the root device /dev/root, which does not
            exist. */
        inline std::string parseRootDevice( const std::string& mountinfo )
        {
            std::string        device;
            std::istringstream lines( mountinfo );
            for( std::string line; std::getline( lines, line ); )
            {
                // mount ID, parent ID, major:minor, root, mount point, ...
                std::istringstream fields( line );
                std::string        id, parent, number, root, target;
                if( fields >> id >> parent >> number >> root >> target && target == "/" )
                    device = number; // the last one is the one on top
            }
            return device;
        }

        inline std::string realPath( const std::string& path )
        {
#if defined( __linux__ )
            if( char* resolved = ::realpath( path.c_str(), nullptr ) )
            {
                std::string result = resolved;
                std::free( resolved );
                return result;
            }
#endif
            return {};
        }

        /** The sysfs directory of a block device node, or an empty string if
            the path is not one. */
        inline std::string sysfsOfDeviceNode( const std::string& node )
        {
#if defined( __linux__ )
            struct stat st {};
            if( node.rfind( "/dev/", 0 ) == 0 && stat( node.c_str(), &st ) == 0 && S_ISBLK( st.st_mode ) )
                return realPath( "/sys/dev/block/" + std::to_string( major( st.st_rdev ) ) + ":" + std::to_string( minor( st.st_rdev ) ) );
#endif
            return {};
        }

        /** The sysfs directory of the disk a block device (a partition or
            the disk itself) is on. */
        inline std::string diskOf( const std::string& block )
        {
            if( block.empty() )
                return {};
#if defined( __linux__ )
            if( access( ( block + "/partition" ).c_str(), F_OK ) == 0 )
                return realPath( block + "/.." );
#endif
            return block;
        }

        /** Adds a disk, and the disks under it if it is a device mapper or
            RAID device, to a set. */
        inline void addDisk( std::set<std::string>& disks, const std::string& disk, int depth = 0 )
        {
            if( disk.empty() || depth > 4 || !disks.insert( disk ).second )
                return;
#if defined( __linux__ )
            if( DIR* dir = opendir( ( disk + "/slaves" ).c_str() ) )
            {
                while( const dirent* entry = readdir( dir ) )
                {
                    const std::string name = entry->d_name;
                    if( name != "." && name != ".." )
                        addDisk( disks, diskOf( realPath( disk + "/slaves/" + name ) ), depth + 1 );
                }
                closedir( dir );
            }
#endif
        }

        /** The disks the system runs from: the one the root file system is
            on, and any with a file system mounted outside the removable
            media directory. */
        inline std::set<std::string> systemDisks( const std::vector<Mount>& mounted, const std::string& mediaDir )
        {
            std::set<std::string> disks;

            std::string mountinfo;
            sysfile::read( "/proc/self/mountinfo", mountinfo );
            const auto root = parseRootDevice( mountinfo );
            if( !root.empty() )
                addDisk( disks, diskOf( realPath( "/sys/dev/block/" + root ) ) );

            for( const auto& mount : mounted )
                if( mount.target.rfind( mediaDir, 0 ) != 0 )
                    addDisk( disks, diskOf( sysfsOfDeviceNode( mount.source ) ) );

            return disks;
        }

        /** Whether a disk is on USB: its sysfs path goes through a USB bus
            (".../usb1/1-1/..."). */
        inline bool isUsbDisk( const std::string& disk )
        {
            for( auto pos = disk.find( "/usb" ); pos != std::string::npos; pos = disk.find( "/usb", pos + 1 ) )
                if( pos + 4 < disk.size() && disk[pos + 4] >= '0' && disk[pos + 4] <= '9' )
                    return true;
            return false;
        }

        /** Whether a disk is an SD card: a card on the MMC bus of the SD
            type. An eMMC is on the same bus, but of the MMC type. */
        inline bool isSdCard( const std::string& disk )
        {
            const auto bus = realPath( disk + "/device/subsystem" );
            if( bus.size() < 4 || bus.compare( bus.size() - 4, 4, "/mmc" ) != 0 )
                return false;

            const auto type = sysfile::readLine( disk + "/device/type" );
            return type == "SD" || ( type.empty() && sysfile::readLine( disk + "/removable" ) == "1" );
        }
    }

    /** Where an SD card or a USB disk is mounted: the first one of that kind
        mounted below KICK_MEDIA_DIR, or an empty string if none is.

        It is looked up in /proc/mounts, so it does not matter what the mount
        point is named after (a fixed name, the device or the file system's
        label), only that it is below KICK_MEDIA_DIR, where the system's
        automounter puts removable disks. What kind of disk is mounted there
        is found in sysfs, by the bus it is on; a disk the system runs from
        is never one, even if the automounter mounted one of its partitions
        (such as the inactive one of two root file systems). */
    inline std::string getRemovableMediaPath( RemovableMedia media )
    {
        const std::string base    = std::string( KICK_MEDIA_DIR ) + "/";
        const auto        mounted = media::mounts();

        std::set<std::string> system;
        bool                  systemKnown = false;

        for( const auto& mount : mounted )
        {
            if( mount.target.rfind( base, 0 ) != 0 )
                continue;

            const auto disk = media::diskOf( media::sysfsOfDeviceNode( mount.source ) );
            if( disk.empty() )
                continue;

            if( !systemKnown )
            {
                system      = media::systemDisks( mounted, base );
                systemKnown = true;
            }
            if( system.count( disk ) > 0 )
                continue;

            if( media == RemovableMedia::SdCard ? media::isSdCard( disk ) : media::isUsbDisk( disk ) )
                return mount.target;
        }

        return {};
    }

    /** Whether something is mounted exactly at path. */
    inline bool isMountPoint( const std::string& path )
    {
        for( const auto& mount : media::mounts() )
            if( mount.target == path )
                return true;
        return false;
    }
}
