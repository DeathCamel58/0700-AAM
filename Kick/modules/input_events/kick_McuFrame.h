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

// Splitting the front-panel MCU's serial byte stream into frames.
//
// A frame (CPktMgr's format) is
//   'M' 'T' type direction command size data[size - 1] crc
// where size counts the data and the CRC, and the CRC (CPktMgr::CrcGen())
// covers everything before it. Bytes that are not part of a valid frame,
// such as noise on the line or a frame cut short, are skipped by looking
// for the next 'M' 'T'.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Controls
{
    struct McuFrame
    {
        uint8_t              type      = 0;
        uint8_t              direction = 0;
        uint8_t              command   = 0;
        std::vector<uint8_t> data;
    };

    namespace mcu
    {
        constexpr std::size_t headerSize = 6; // 'M' 'T' type direction command size
        constexpr std::size_t maxDataSize = 254; // the size byte counts the CRC too
        constexpr uint8_t     maxType      = 7; // CPktMgr::EN_PKT_TYPE_SLIDER
        constexpr uint8_t     maxDirection = 1; // CPktMgr::EN_PKT_RESPONSE

        /** Takes the complete frames out of the received bytes, which keep
            whatever may still be the start of a frame. crc is called with a
            frame's bytes up to its CRC, as a std::vector<char>&, and returns
            the CRC they should have. */
        template <typename Crc>
        std::vector<McuFrame> extractFrames( std::vector<uint8_t>& received, Crc crc )
        {
            std::vector<McuFrame> frames;
            std::size_t           pos = 0;
            const std::size_t     end = received.size();

            while( pos < end )
            {
                // The next 'M' 'T', or an 'M' at the very end, which may be
                // the start of one.
                while( pos < end && !( received[pos] == 'M' && ( pos + 1 == end || received[pos + 1] == 'T' ) ) )
                    ++pos;
                if( end - pos < headerSize )
                    break;

                const uint8_t type      = received[pos + 2];
                const uint8_t direction = received[pos + 3];
                const uint8_t size      = received[pos + 5];
                if( type > maxType || direction > maxDirection || size == 0 )
                {
                    ++pos; // not a frame header
                    continue;
                }

                const std::size_t length = headerSize + size; // with the data and the CRC
                if( end - pos < length )
                    break;

                std::vector<char> covered( received.begin() + (std::ptrdiff_t)pos, received.begin() + (std::ptrdiff_t)( pos + length - 1 ) );
                if( (uint8_t)crc( covered ) != received[pos + length - 1] )
                {
                    ++pos; // a corrupted frame, or 'M' 'T' inside another one
                    continue;
                }

                McuFrame frame;
                frame.type      = type;
                frame.direction = direction;
                frame.command   = received[pos + 4];
                frame.data.assign( received.begin() + (std::ptrdiff_t)( pos + headerSize ), received.begin() + (std::ptrdiff_t)( pos + length - 1 ) );
                frames.push_back( std::move( frame ) );

                pos += length;
            }

            received.erase( received.begin(), received.begin() + (std::ptrdiff_t)pos );
            return frames;
        }
    }
}
