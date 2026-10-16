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

// Tests of the front-panel MCU frame parser (kick_McuFrame.h): frames split
// across reads, noise and corrupted frames, and a stream joined out of step,
// where a false header must not hold up the frames after it. Build and run
// it from the Kick directory, with AddressSanitizer:
//
//   g++ -std=c++17 -g -fsanitize=address,undefined -I modules/input_events
//       Tests/McuFrameTest.cpp -o McuFrameTest
//   ./McuFrameTest

#include "kick_McuFrame.h"

#include <cstdio>
#include <cstdlib>
#include <random>

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

    // CPktMgr::CrcGen(): CRC-8, polynomial 0x31, starting from 0xff.
    uint8_t crcGen( std::vector<char>& bytes )
    {
        uint8_t crc = 0xff;
        for( char byte : bytes )
        {
            crc ^= (uint8_t)byte;
            for( int bit = 0; bit < 8; ++bit )
                crc = ( crc & 0x80 ) ? (uint8_t)( ( crc << 1 ) ^ 0x31 ) : (uint8_t)( crc << 1 );
        }
        return crc;
    }

    std::vector<uint8_t> makeFrame( uint8_t type, uint8_t direction, uint8_t command, const std::vector<uint8_t>& data )
    {
        std::vector<uint8_t> frame = { 'M', 'T', type, direction, command, (uint8_t)( data.size() + 1 ) };
        frame.insert( frame.end(), data.begin(), data.end() );
        std::vector<char> covered( frame.begin(), frame.end() );
        frame.push_back( crcGen( covered ) );
        return frame;
    }

    bool sameFrame( const Controls::McuFrame& frame, const std::vector<uint8_t>& bytes )
    {
        return frame.type == bytes[2] && frame.direction == bytes[3] && frame.command == bytes[4]
               && frame.data == std::vector<uint8_t>( bytes.begin() + Controls::mcu::headerSize, bytes.end() - 1 );
    }

    /** What input_events.cpp does with each read: appends it, takes the
        frames out, and drops a leftover longer than any frame. */
    struct Receiver
    {
        std::vector<uint8_t>               pending;
        std::vector<Controls::McuFrame>    frames;

        std::size_t receive( const std::vector<uint8_t>& bytes )
        {
            pending.insert( pending.end(), bytes.begin(), bytes.end() );
            auto got = Controls::mcu::extractFrames( pending, crcGen );
            if( pending.size() > Controls::mcu::headerSize + Controls::mcu::maxDataSize + 1 )
                pending.clear();
            for( auto& frame : got )
                frames.push_back( std::move( frame ) );
            return got.size();
        }
    };

    std::vector<uint8_t> join( std::initializer_list<std::vector<uint8_t>> parts )
    {
        std::vector<uint8_t> all;
        for( const auto& part : parts )
            all.insert( all.end(), part.begin(), part.end() );
        return all;
    }

    // A button press and release, as the panel sends them.
    const auto press   = makeFrame( 1, 0, 0x10, { 0x05, 0x01 } );
    const auto release = makeFrame( 1, 0, 0x10, { 0x05, 0x00 } );
    const auto knob    = makeFrame( 3, 0, 0x22, { 0x02, 0x7f, 0x01 } );

    void testWholeFrames()
    {
        Receiver r;
        CHECK( r.receive( join( { press, release, knob } ) ) == 3 );
        CHECK( r.frames.size() == 3 && sameFrame( r.frames[0], press ) && sameFrame( r.frames[1], release ) && sameFrame( r.frames[2], knob ) );
        CHECK( r.pending.empty() );
    }

    void testSplitFrames()
    {
        // One byte at a time.
        Receiver   r;
        const auto stream = join( { press, knob, release } );
        for( uint8_t byte : stream )
            r.receive( { byte } );
        CHECK( r.frames.size() == 3 && sameFrame( r.frames[0], press ) && sameFrame( r.frames[1], knob ) && sameFrame( r.frames[2], release ) );
        CHECK( r.pending.empty() );

        // Every split of two frames in two reads.
        const auto two = join( { press, knob } );
        for( std::size_t split = 0; split <= two.size(); ++split )
        {
            Receiver s;
            s.receive( std::vector<uint8_t>( two.begin(), two.begin() + (std::ptrdiff_t)split ) );
            s.receive( std::vector<uint8_t>( two.begin() + (std::ptrdiff_t)split, two.end() ) );
            CHECK( s.frames.size() == 2 && sameFrame( s.frames[0], press ) && sameFrame( s.frames[1], knob ) );
            CHECK( s.pending.empty() );
        }
    }

    void testNoiseAndCrcFailures()
    {
        // Noise, a frame with a wrong CRC, a frame with a flipped data byte,
        // and the bytes of a frame header that is not one.
        auto badCrc = press;
        badCrc.back() ^= 0x01;
        auto badData = knob;
        badData[7] ^= 0x40;

        Receiver r;
        CHECK( r.receive( join( { { 0x00, 'M', 0x13, 'T', 0xff }, badCrc, release, badData, { 'M', 'T', 9, 0, 0, 1 }, press } ) ) == 2 );
        CHECK( r.frames.size() == 2 && sameFrame( r.frames[0], release ) && sameFrame( r.frames[1], press ) );
        CHECK( r.pending.empty() );

        // A corrupted frame whose 'M' 'T' is followed by more frames, in
        // pieces.
        Receiver   s;
        const auto stream = join( { badCrc, knob, release } );
        s.receive( std::vector<uint8_t>( stream.begin(), stream.begin() + 4 ) );
        s.receive( std::vector<uint8_t>( stream.begin() + 4, stream.begin() + 15 ) );
        s.receive( std::vector<uint8_t>( stream.begin() + 15, stream.end() ) );
        CHECK( s.frames.size() == 2 && sameFrame( s.frames[0], knob ) && sameFrame( s.frames[1], release ) );
    }

    void testFalseHeader()
    {
        // Joined in the middle of a frame whose data holds 'M' 'T' and what
        // looks like a header asking for 255 more bytes: the frames after it
        // come out as soon as they are complete.
        const std::vector<uint8_t> falseHeader = { 'M', 'T', 0x01, 0x00, 0x10, 0xff, 0x33 };

        Receiver r;
        CHECK( r.receive( falseHeader ) == 0 );
        CHECK( r.receive( press ) == 1 );
        CHECK( r.receive( release ) == 1 );
        CHECK( r.frames.size() == 2 && sameFrame( r.frames[0], press ) && sameFrame( r.frames[1], release ) );
        CHECK( r.pending.empty() );

        // The same, with the real frame arriving one byte at a time: it is
        // waited for, and not dropped with the false header.
        Receiver s;
        s.receive( falseHeader );
        for( uint8_t byte : join( { knob, press } ) )
            s.receive( { byte } );
        CHECK( s.frames.size() == 2 && sameFrame( s.frames[0], knob ) && sameFrame( s.frames[1], press ) );
        CHECK( s.pending.empty() );

        // A frame that lost a byte: the next frame starts where its last
        // bytes should have been.
        auto cut = release;
        cut.erase( cut.begin() + 6 );
        Receiver t;
        CHECK( t.receive( join( { cut, press } ) ) == 1 );
        CHECK( t.receive( knob ) == 1 );
        CHECK( t.frames.size() == 2 && sameFrame( t.frames[0], press ) && sameFrame( t.frames[1], knob ) );

        // A real frame that is incomplete is still waited for, though its
        // data holds a complete header of its own that is not a frame.
        auto notFrame = makeFrame( 1, 0, 0x02, { 0x00 } );
        notFrame.back() ^= 0x01;
        notFrame.push_back( 0x00 );
        const auto big = makeFrame( 2, 0, 0x01, notFrame );
        Receiver   u;
        u.receive( std::vector<uint8_t>( big.begin(), big.end() - 2 ) );
        CHECK( u.frames.empty() );
        u.receive( std::vector<uint8_t>( big.end() - 2, big.end() ) );
        CHECK( u.frames.size() == 1 && sameFrame( u.frames[0], big ) );
    }

    void testRandomStreams()
    {
        // Random frames (with many 'M' and 'T' data bytes), joined at a
        // random point and delivered in random pieces. In step from the
        // start, every frame comes out; joined in the middle of the first
        // one, the frames after it do, except when the bytes after the join
        // happen to form a frame with a valid CRC (about one time in 256).
        std::mt19937 random( 1234 );
        auto         pick = [&]( int lo, int hi ) { return std::uniform_int_distribution<int>( lo, hi )( random ); };

        int trials = 0;
        int missed = 0;
        for( int trial = 0; trial < 20000; ++trial )
        {
            std::vector<std::vector<uint8_t>> sent;
            std::vector<uint8_t>              stream;
            const int                         count = pick( 2, 8 );
            for( int i = 0; i < count; ++i )
            {
                std::vector<uint8_t> data( (std::size_t)pick( 0, i == 0 ? 60 : 12 ) );
                for( auto& byte : data )
                {
                    const int kind = pick( 0, 3 );
                    byte           = kind == 0 ? 'M' : kind == 1 ? 'T' : (uint8_t)pick( 0, 255 );
                }
                // Often, what looks like a header in the first frame's data.
                if( i == 0 && pick( 0, 1 ) == 0 )
                {
                    const std::vector<uint8_t> header = { 'M', 'T', (uint8_t)pick( 0, 7 ), (uint8_t)pick( 0, 1 ), (uint8_t)pick( 0, 255 ), (uint8_t)pick( 1, 255 ) };
                    data.insert( data.begin() + pick( 0, (int)data.size() ), header.begin(), header.end() );
                }
                sent.push_back( makeFrame( (uint8_t)pick( 0, 7 ), (uint8_t)pick( 0, 1 ), (uint8_t)pick( 0, 255 ), data ) );
                stream.insert( stream.end(), sent.back().begin(), sent.back().end() );
            }

            const bool        inStep = trial % 4 == 0;
            const std::size_t start  = inStep ? 0 : (std::size_t)pick( 1, (int)sent[0].size() - 1 );

            Receiver r;
            for( std::size_t pos = start; pos < stream.size(); )
            {
                const std::size_t n = std::min<std::size_t>( (std::size_t)pick( 1, 20 ), stream.size() - pos );
                r.receive( std::vector<uint8_t>( stream.begin() + (std::ptrdiff_t)pos, stream.begin() + (std::ptrdiff_t)( pos + n ) ) );
                pos += n;
            }

            const std::size_t first = inStep ? 0 : 1;
            bool              all   = r.frames.size() == sent.size() - first;
            for( std::size_t i = 0; all && i < r.frames.size(); ++i )
                all = sameFrame( r.frames[i], sent[first + i] );

            if( inStep )
                CHECK( all && r.pending.empty() );
            else
            {
                ++trials;
                missed += all ? 0 : 1;
            }
        }

        std::printf( "Joined out of step: %d of %d streams lost a frame\n", missed, trials );
        CHECK( missed * 50 < trials );
    }
}

int main()
{
    testWholeFrames();
    testSplitFrames();
    testNoiseAndCrcFailures();
    testFalseHeader();
    testRandomStreams();

    if( failures == 0 )
        std::printf( "All tests passed\n" );
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
