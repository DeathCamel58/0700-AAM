/*
 * Copyright (c) 2026 Dylan Corrales
 *
 * This file is part of AudIO.
 *
 * AudIO is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * AudIO is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with AudIO. If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

// Stress test of the design's thread-safety: a stand-in audio thread
// processes a design in a loop while a control thread changes the graph
// (creates, routes, meters and removes objects, adds and deletes chain
// inserts, swaps insert effects). Build it with ThreadSanitizer, or with
// AddressSanitizer, from the AudIO directory, and run it for a number of
// seconds:
//
//   g++ -std=c++17 -g -O1 -fsanitize=thread -DAUDIO_ONLY_CORE -I AudIO \
//       AudIO/Tests/GraphStress.cpp -o GraphStress -lpthread
//   ./GraphStress 30
//
// It first checks a few behaviors on their own (chain inserts, meters,
// sub-designs, batched removal, parameter changes). It passes when those checks pass, the sanitizer reports
// nothing and the memory pool holds only the stand-in device's buffers once
// the design is gone.

#include "AudIO.h"

#include "Core/Design.cpp"
#include "Core/DesignImpl.cpp"
#include "Core/Chain.cpp"
#include "Core/Bus.cpp"
#include "Core/MeterEngine.cpp"
#include "Objects/LevelMeter.cpp"

namespace AudIO
{
    const std::string _emptyStr{ "" }; // Defined by DawEngine.cpp in a full build.
}
#include "Daw/DawInserts.cpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

namespace
{
    constexpr uint32_t BLOCK = 480;
    constexpr uint32_t POOL  = 2500;
    float              poolMemory[POOL * BLOCK];

    // A source: writes a constant to its outputs.
    struct Source : AudIO::Object
    {
        Source()
            : Object( 0, 2, 0 ) {}

        void process( AudIO::Buffer<float>& B ) override
        {
            for( uint32_t ch = 0; ch < B.getNumOutputChannels(); ++ch )
                if( auto out = B.getOutputBuffer( ch ) )
                    for( uint32_t i = 0; i < B.getBlockSize(); ++i )
                        out[i] = 0.25f;
        }
    };

    // An effect: scales its input into its outputs.
    struct Gain : AudIO::Object
    {
        Gain()
            : Object( 2, 2, 0 ) {}

        void process( AudIO::Buffer<float>& B ) override { B.copyInputsToOutputs( 0.5f ); }
    };
    REGISTER_OBJECT( "TestGain", Gain );

    // An object with insert effects, as Kick's programs have.
    struct Host : AudIO::Object
        , AudIO::DawInserts
    {
        Host()
            : Object( 0, 2, 0 ) {}

        void prepareToPlay( uint32_t Samplerate, uint32_t Blocksize ) override
        {
            Object::prepareToPlay( Samplerate, Blocksize );
            DawInserts::prepareToPlay( Samplerate, Blocksize );
        }

        void process( AudIO::Buffer<float>& B ) override { DawInserts::process( B ); }
    };

    // A stand-in audio thread for the checks below. Look at the device's
    // outputs only while it is stopped.
    struct Player
    {
        Player( AudIO::Design& D, AudIO::Buffer<float>& Device )
            : design( D ), device( Device ) {}

        ~Player() { stop(); }

        void start()
        {
            running.store( true );
            thread = std::thread( [this] {
                while( running.load() )
                {
                    design.process( device );
                    ++blocks;
                    std::this_thread::sleep_for( pause );
                }
            } );
        }

        void stop()
        {
            running.store( false );
            if( thread.joinable() )
                thread.join();
        }

        void waitBlocks( long Count )
        {
            const auto target = blocks.load() + Count;
            while( blocks.load() < target )
                std::this_thread::sleep_for( std::chrono::microseconds( 100 ) );
        }

        AudIO::Design&        design;
        AudIO::Buffer<float>& device;
        std::atomic<bool>     running{ false };
        std::atomic<long>     blocks{ 0 };
        std::thread           thread;

        std::chrono::microseconds pause{ 200 }; // Between blocks.
    };

    bool check( bool Condition, const char* What )
    {
        if( !Condition )
            std::printf( "FAILED: %s\n", What );
        return Condition;
    }

    float outputLevel( AudIO::Buffer<float>& Device )
    {
        auto out = Device.getOutputBuffer( 0 );
        return out ? out[Device.getBlockSize() - 1] : 0.f;
    }

    // A chain's inserts, added at the head, the middle and the tail (several
    // times), before and while it plays, must all be wired in: every Gain
    // halves the level. Burst: change the chain several times between two
    // blocks.
    bool checkChainInserts( bool Burst )
    {
        auto design = std::make_unique<AudIO::Design>( 2, 2 );
        auto chain  = design->createChain( true );
        auto source = design->createObject( std::make_unique<Source>() );
        design->patch( source, chain );
        design->setDesignOutput( chain, true );

        AudIO::Buffer<float> device( 2, true, 2, true );
        Player               player( *design, device );
        bool                 ok = true;

        std::vector<uint32_t> inserts;
        auto                  settle = [&] {
            if( player.running.load() && !Burst )
                player.waitBlocks( 2 );
        };
        auto add = [&]( uint32_t Pos ) {
            auto id = design->createInsert( chain, "TestGain" );
            ok &= check( design->addInsert( chain, id, Pos ), "adding an insert" );
            inserts.push_back( id );
            settle();
        };
        auto remove = [&]( uint32_t Pos ) {
            ok &= check( design->removeInsertByPos( chain, Pos ), "removing an insert" );
            settle();
        };
        auto expect = [&]( int Gains, const char* What ) {
            player.waitBlocks( 4 );
            player.stop();
            ok &= check( ( 0.25f / (float)( 1 << Gains ) ) == outputLevel( device ), What );
            player.start();
        };

        // Not playing yet: the patches are made at once.
        add( AudIO::POS_END );
        add( 1 ); // The tail.

        design->prepareToPlay( 48000, BLOCK );
        player.start();
        expect( 2, "two inserts each halve the level" );

        add( 2 ); // The tail.
        add( AudIO::POS_END );
        add( 0 ); // The head.
        add( 2 ); // The middle.
        add( (uint32_t)inserts.size() ); // The tail.
        expect( 7, "seven inserts each halve the level" );

        remove( 6 ); // The tail.
        remove( 0 ); // The head.
        remove( 2 ); // The middle.
        expect( 4, "four inserts left each halve the level" );

        player.stop();
        design->teardown();
        return ok;
    }

    // The design's meters run round-robin, a few per block: they must keep
    // coming round as meters are added and removed.
    bool checkMeterRoundRobin()
    {
        auto design = std::make_unique<AudIO::Design>( 2, 2 );
        auto out    = design->createBus();
        design->setDesignOutput( out, true );

        AudIO::Buffer<float> device( 2, true, 2, true );
        design->prepareToPlay( 48000, BLOCK );
        Player player( *design, device );
        player.start();

        auto addMetered = [&] {
            auto id = design->createObject( std::make_unique<Source>() );
            design->sendToBus( out, id, 1.f );
            design->addMeter( id );
            return id;
        };
        auto metered = [&]( const std::vector<uint32_t>& Ids ) {
            player.waitBlocks( 50 );
            player.stop();
            bool all = true;
            for( auto id : Ids )
            {
                // A Source peaks at 0.25 (-12 dB); a meter that never ran
                // reads otherwise.
                const auto& data = design->readMeter( id );
                all &= !data.empty() && ( std::fabs( data[0].peak + 12.04f ) < 0.1f );
            }
            player.start();
            return all;
        };

        std::vector<uint32_t> first;
        for( int i = 0; i < 4; ++i )
            first.push_back( addMetered() );
        bool ok = check( metered( first ), "four meters are updated" );

        // Fewer meters than the round-robin has been through.
        for( int i = 0; i < 3; ++i )
            design->removeObject( first[(size_t)i] );
        std::vector<uint32_t> second{ first[3], addMetered(), addMetered() };
        ok &= check( metered( second ), "meters added after removals are updated" );

        std::vector<uint32_t> third{ addMetered(), addMetered(), addMetered() };
        ok &= check( metered( third ), "meters added later are updated" );

        player.stop();
        design->teardown();
        return ok;
    }

    // A chain's gain, changed while it plays, ramps to the new value; a
    // forced change of the design's output gain applies at once.
    bool checkParameterChanges()
    {
        auto design = std::make_unique<AudIO::Design>( 2, 2 );
        auto chain  = design->createChain( true );
        auto source = design->createObject( std::make_unique<Source>() );
        design->patch( source, chain );
        design->setDesignOutput( chain, true );

        AudIO::Buffer<float> device( 2, true, 2, true );
        design->prepareToPlay( 48000, BLOCK );
        Player player( *design, device );
        player.start();

        bool ok    = true;
        auto level = [&]( long Blocks ) {
            player.waitBlocks( Blocks );
            player.stop();
            const auto l = outputLevel( device );
            player.start();
            return l;
        };

        for( int i = 0; i < 50; ++i ) // Changes faster than the ramps.
            design->setGain( chain, ( i & 1 ) ? 0.25f : 1.f );
        design->setGain( chain, 0.5f );
        ok &= check( 0.5f == design->getGain( chain ), "a chain's gain reads back at once" );
        ok &= check( 0.125f == level( 40 ), "a chain's gain ramps to its new value" );

        design->setMute( chain, true );
        ok &= check( 0.f == level( 40 ), "a muted chain is silent" );
        design->setMute( chain, false );
        ok &= check( 0.125f == level( 40 ), "an unmuted chain plays again" );

        design->clearDesignOutput( true );
        ok &= check( 0.f == level( 4 ), "a forced output gain change is immediate" );

        player.stop();
        design->teardown();
        return ok;
    }

    // A sub-design inserted and torn down while the engine plays must not
    // start or stop the patch queue, which only the audio thread empties.
    bool checkSubDesign()
    {
        auto design = std::make_unique<AudIO::Design>( 2, 2 );
        auto out    = design->createBus();
        design->setDesignOutput( out, true );

        AudIO::Buffer<float> device( 2, true, 2, true );
        design->prepareToPlay( 48000, BLOCK );
        Player player( *design, device );
        player.start();

        auto& queue = AudIO::PatchQueue<float>::get();
        bool  ok    = true;
        for( int i = 0; i < 20; ++i )
        {
            auto sub    = std::make_shared<AudIO::Design>( 0, 2 );
            auto source = sub->createObject( std::make_unique<Source>() );
            sub->setDesignOutput( source, true );
            auto id = design->insertDesign( sub );
            ok &= check( 0 != id, "inserting a sub-design" );
            design->sendToBus( out, id, 1.f );
            player.waitBlocks( 2 );

            sub->teardown();
            ok &= check( queue.isRunning(), "a sub-design's teardown leaves the patch queue running" );
            design->removeObject( id );
        }

        player.stop();
        design->teardown();
        ok &= check( !queue.isRunning(), "the top-level design's teardown stops the patch queue" );
        return ok;
    }

    // Removing many objects at once, while playing, waits for the audio
    // thread once rather than once per object, and frees them all.
    bool checkBatchedRemoval()
    {
        auto design = std::make_unique<AudIO::Design>( 2, 2 );
        auto out    = design->createBus();
        design->setDesignOutput( out, true );

        AudIO::Buffer<float> device( 2, true, 2, true );
        design->prepareToPlay( 48000, BLOCK );
        Player player( *design, device );
        player.pause = std::chrono::microseconds( 2000 ); // Blocks as a device runs them.
        player.start();

        const auto usedBefore = AudIO::MemoryPool<float>::get().getUsed();
        auto       create     = [&] {
            std::vector<uint32_t> ids;
            for( int i = 0; i < 30; ++i )
            {
                auto id = design->createObject( std::make_unique<Source>() );
                design->sendToBus( out, id, 1.f );
                design->addMeter( id );
                ids.push_back( id );
            }
            return ids;
        };
        auto timed = [&]( auto&& Remove ) {
            const auto start = std::chrono::steady_clock::now();
            Remove();
            return std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - start ).count();
        };

        auto       ids = create();
        const auto one = timed( [&] {
            for( auto id : ids )
                design->removeObject( id );
        } );

        ids             = create();
        auto       ok   = true;
        const auto many = timed( [&] { ok &= check( design->removeObjects( ids ), "removing objects together" ); } );

        ids              = create();
        const auto scope = timed( [&] {
            AudIO::Design::RemovalBatch batch{ *design };
            for( auto id : ids )
                design->removeObject( id );
        } );

        player.waitBlocks( 2 );
        player.stop();
        std::printf( "Removing 30 objects: %.1f ms one by one, %.1f ms together, %.1f ms in a batch\n", one, many, scope );
        ok &= check( ( many < one ) && ( scope < one ), "removing objects together is faster" );
        ok &= check( usedBefore == AudIO::MemoryPool<float>::get().getUsed(), "objects removed together are freed" );

        design->teardown();
        return ok;
    }
} // namespace

int main( int argc, char** argv )
{
    const int seconds = ( argc > 1 ) ? std::atoi( argv[1] ) : 5;

    AudIO::MemoryPool<float>::get().initialize( poolMemory, POOL, BLOCK );

    if( !checkChainInserts( false ) || !checkChainInserts( true ) || !checkMeterRoundRobin() || !checkSubDesign() || !checkBatchedRemoval() || !checkParameterChanges() )
        return 1;

    auto design = std::make_unique<AudIO::Design>( 2, 2 );
    auto out    = design->createBus();
    auto sub    = design->createBus();
    auto chain  = design->createChain( true );
    design->sendToBus( out, sub, 1.f );
    design->setDesignInput( chain );
    design->setDesignOutput( out );
    design->addMeter( out );

    auto host   = std::make_unique<Host>();
    auto hostId = host->getId();
    auto pHost  = host.get();
    design->createObject( std::move( host ) );
    design->sendToBus( out, hostId, 1.f );

    AudIO::Buffer<float> device( 2, true, 2, true );
    design->prepareToPlay( 48000, BLOCK );

    std::atomic<bool> running{ true };
    std::atomic<long> blocks{ 0 };

    std::thread audio( [&] {
        while( running.load() )
        {
            design->process( device );
            ++blocks;
            if( 0 == ( blocks.load() % 16 ) )
                std::this_thread::sleep_for( std::chrono::microseconds( 100 ) );
        }
    } );

    std::mt19937          rng( 1234 );
    long                  operations = 0;
    std::vector<uint32_t> objects;
    std::vector<uint32_t> chainInserts;
    const auto            deadline = std::chrono::steady_clock::now() + std::chrono::seconds( seconds );

    while( std::chrono::steady_clock::now() < deadline )
    {
        // Keep within the memory pool: remove many when there are many objects.
        const auto operation = ( objects.size() < 300 ) ? rng() % 10 : 10;
        switch( operation )
        {
            case 0:
            case 1:
            {
                auto id = design->createObject( std::make_unique<Source>() );
                design->sendToBus( ( rng() & 1 ) ? out : sub, id, 0.5f );
                if( rng() & 1 )
                    design->addMeter( id );
                objects.push_back( id );
                break;
            }
            case 2:
            {
                auto id = design->createObject( std::make_unique<Gain>() );
                if( !objects.empty() )
                    design->patch( objects[rng() % objects.size()], id );
                design->sendToBus( sub, id, 0.7f );
                objects.push_back( id );
                break;
            }
            case 3:
            case 4:
                if( !objects.empty() )
                {
                    auto index = rng() % objects.size();
                    auto id    = objects[index];
                    if( rng() & 1 ) // Otherwise it is removed while still routed.
                    {
                        design->removeFromBus( out, id );
                        design->removeFromBus( sub, id );
                        design->removeMeter( id );
                    }
                    design->removeObject( id );
                    objects.erase( objects.begin() + (long)index );
                }
                break;
            case 5:
                // Up to a few inserts, added anywhere (the tail included)
                // and deleted in any order.
                if( ( chainInserts.size() < 4 ) && ( rng() & 1 ) )
                {
                    auto id = design->createInsert( chain, "TestGain" );
                    design->addInsert( chain, id, rng() % ( chainInserts.size() + 2 ) );
                    chainInserts.push_back( id );
                }
                else if( !chainInserts.empty() )
                {
                    auto index = rng() % chainInserts.size();
                    auto id    = chainInserts[index];
                    if( rng() & 1 )
                        design->removeInsert( chain, id );
                    design->deleteInsert( chain, id );
                    chainInserts.erase( chainInserts.begin() + (long)index );
                }
                break;
            case 6:
                pHost->addEffect( "TestGain", rng() % AudIO::DawInserts::MAX_INSERTS );
                break;
            case 7:
                pHost->removeEffect( (unsigned int)( rng() % AudIO::DawInserts::MAX_INSERTS ) );
                break;
            case 8:
                if( !objects.empty() )
                    design->setBusInputGain( out, objects[rng() % objects.size()], 0.3f );
                break;
            case 9:
            {
                // Parameters, which the audio thread ramps.
                const auto target = ( rng() & 1 ) ? chain : ( ( rng() & 1 ) ? sub : out );
                switch( rng() % 6 )
                {
                    case 0: design->setGain( target, 0.5f + 0.5f * (float)( rng() & 1 ) ); break;
                    case 1: design->setPan( target, ( rng() & 1 ) ? -0.5f : 0.f ); break;
                    case 2: design->setLevelAndPan( target, 1.f, 0.f ); break;
                    case 3: design->setMute( target, 0 == rng() % 4 ); break;
                    case 4: design->setBypass( target, 0 == rng() % 4 ); break;
                    case 5: design->setBypass( 0 == rng() % 8 ); break;
                }
                break;
            }
            case 10:
            {
                // Many at once, together or in a batch.
                std::vector<uint32_t> ids;
                for( auto n = 1 + rng() % 100; n && !objects.empty(); --n )
                {
                    auto index = rng() % objects.size();
                    ids.push_back( objects[index] );
                    objects.erase( objects.begin() + (long)index );
                }
                if( rng() & 1 )
                {
                    design->removeObjects( ids );
                }
                else
                {
                    AudIO::Design::RemovalBatch batch{ *design };
                    for( auto id : ids )
                    {
                        if( rng() & 1 )
                            design->removeFromBus( out, id );
                        design->removeObject( id );
                    }
                }
                break;
            }
        }
        ++operations;
    }

    running.store( false );
    audio.join();
    design->teardown();

    std::printf( "%ld blocks, %ld graph changes, %zu objects left, %u pool blocks used\n", blocks.load(), operations, objects.size(), AudIO::MemoryPool<float>::get().getUsed() );
    design.reset();

    const auto used = AudIO::MemoryPool<float>::get().getUsed();
    std::printf( "%u pool blocks used without the design (the device's: 4)\n", used );
    return ( 4 == used ) ? 0 : 1;
}
