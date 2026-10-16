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
// It passes when the sanitizer reports nothing and the memory pool holds
// only the stand-in device's buffers once the design is gone.

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
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>

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
} // namespace

int main( int argc, char** argv )
{
    const int seconds = ( argc > 1 ) ? std::atoi( argv[1] ) : 5;

    AudIO::MemoryPool<float>::get().initialize( poolMemory, POOL, BLOCK );

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
    const auto            deadline = std::chrono::steady_clock::now() + std::chrono::seconds( seconds );

    while( std::chrono::steady_clock::now() < deadline )
    {
        // Keep within the memory pool: remove when there are many objects.
        const auto operation = ( objects.size() < 300 ) ? rng() % 9 : 3;
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
            {
                auto id = design->createInsert( chain, "TestGain" );
                design->addInsert( chain, id, rng() % 3 );
                if( rng() & 1 )
                    design->removeInsert( chain, id );
                design->deleteInsert( chain, id );
                break;
            }
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
