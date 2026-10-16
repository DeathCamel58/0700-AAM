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

#include <fstream>
#include <sstream>
#include <string>

// The directory removable disks are mounted below; the build system can
// override it (see CMakeLists.txt).
#ifndef KICK_MEDIA_DIR
    #define KICK_MEDIA_DIR "/mnt"
#endif

namespace mt
{
    enum class RemovableMedia
    {
        SdCard, // an MMC/SD block device, /dev/mmcblk*
        UsbDisk // a USB mass storage device, /dev/sd*
    };

    /** Where an SD card or a USB disk is mounted: the first one of that kind
        mounted below KICK_MEDIA_DIR, or an empty string if none is.

        It is looked up in /proc/mounts, so it does not matter what the mount
        point is named after (a fixed name, the device or the file system's
        label), only that it is below KICK_MEDIA_DIR, where the system's
        automounter puts removable disks. */
    inline std::string getRemovableMediaPath( RemovableMedia media )
    {
        const std::string base   = std::string( KICK_MEDIA_DIR ) + "/";
        const std::string device = media == RemovableMedia::SdCard ? "/dev/mmcblk" : "/dev/sd";

        std::ifstream mounts( "/proc/mounts" );
        for( std::string line; std::getline( mounts, line ); )
        {
            std::istringstream fields( line );
            std::string        source, escaped;
            if( !( fields >> source >> escaped ) || source.rfind( device, 0 ) != 0 )
                continue;

            // /proc/mounts writes space, tab, newline and backslash in mount
            // points as octal escapes (\040 and so on).
            std::string target;
            for( std::size_t i = 0; i < escaped.size(); ++i )
            {
                const bool octal = escaped[i] == '\\' && i + 3 < escaped.size()
                                   && escaped.find_first_not_of( "01234567", i + 1 ) >= i + 4;
                if( octal )
                {
                    target += (char)std::stoi( escaped.substr( i + 1, 3 ), nullptr, 8 );
                    i += 3;
                }
                else
                    target += escaped[i];
            }

            if( target.rfind( base, 0 ) == 0 )
                return target;
        }

        return {};
    }
}
