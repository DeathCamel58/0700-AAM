/*
 * Copyright (c) 2026 Dylan Corrales
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

// Stress test of the sequencer's threads, on the engine as KickAudioEngine
// runs it (built without audio device types, so it runs on its fallback
// clock):
//
// - note queue: two threads post note ons and offs to a program's note
//   queue while a stand-in audio thread drains it, and every so often
//   stops draining it for a while (a stalled device, or a program the
//   design skips). Posting must never block, and no voice may be left
//   playing once every note on has had its note off, also when the note
//   offs come while the queue is full.
// - switches: with the message thread blocked, the tick thread must
//   switch to the next sequence, at the next bar and to the next song
//   step on its own.
// - transport: the message thread plays, pauses, stops, seeks and
//   switches sequences, plays songs, edits notes, and deletes and
//   recreates sequences and programs, and closes and reloads the
//   project, while the MIDI clock and the tick thread run.
//
// Configure Kick with -DKICK_BUILD_TESTS=ON (and
// -DKICK_TEST_SANITIZER=thread or address), build the SequencerStress
// target and run it for a number of seconds, with a scratch directory
// for the project, and optionally only one part (notes, switches or
// transport):
//
//   SequencerStress 30 /tmp/sequencer-stress
//
// It passes when it prints PASS and the sanitizer reports nothing.

#include <audio_engine/audio_engine.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>

namespace
{
    using Clock = std::chrono::steady_clock;

    int failures = 0;

    void check( bool Condition, const char* What )
    {
        if( !Condition )
        {
            ++failures;
            std::printf( "FAIL: %s\n", What );
        }
    }

    void runMessageLoop( int Milliseconds )
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil( Milliseconds );
    }

    //
    // Note queue.
    //

    // A voice that only keeps its state.
    struct TestVoice : mt::ISynthVoice
    {
        explicit TestVoice( const mt::MTIAtomicProgramMaster& Master )
            : ISynthVoice( Master ) {}

        void noteOn( int Note, int ) override
        {
            noteNo = Note;
            state  = State::RUNNING;
        }
        void noteOff() override { makeFree(); }
        void process( juce::AudioBuffer<float>& ) override {}
    };

    struct TestSynth : mt::ISynthObject
    {
        explicit TestSynth( AudIO::Design& D )
            : ISynthObject( D, 4 )
        {
            for( auto i = 0; i < 32; ++i )
                ownVoices.push_back( std::make_unique<TestVoice>( master ) );
            addVoices( ownVoices );
        }

        void on( int Note ) { postNoteOn( Note, 100, 4, nullptr ); }

        std::vector<std::unique_ptr<mt::ISynthVoice>> ownVoices;
    };

    void testNoteQueue( int Seconds )
    {
        AudIO::Design design( 2, 2 );
        TestSynth     synth( design );
        auto&         object = static_cast<AudIO::Object&>( synth );
        object.prepareToPlay( 48000, AUDIO_BLOCK_SIZE );

        AudIO::Buffer<float> buffer( 0, false, 2, true );

        std::atomic<bool> producing{ true };
        std::atomic<bool> draining{ true };
        std::atomic<bool> stalled{ false };
        std::atomic<bool> idle{ false };
        std::atomic<long> blocks{ 0 };

        std::thread audio( [&] {
            std::mt19937 rng( 1 );
            while( draining.load() )
            {
                idle = stalled.load();
                if( idle.load() )
                {
                    std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
                    continue;
                }

                // Stall now and then, as long as a few hundred blocks.
                if( producing.load() && 0 == rng() % 400 )
                    std::this_thread::sleep_for( std::chrono::milliseconds( 20 + rng() % 200 ) );
                object.process( buffer );
                ++blocks;
                std::this_thread::sleep_for( std::chrono::microseconds( 500 ) );
            }
        } );

        std::atomic<long>                    posts{ 0 };
        std::atomic<long long>               slowest{ 0 };
        std::vector<std::thread>             producers;
        const auto                           deadline = Clock::now() + std::chrono::seconds( Seconds );
        for( auto p = 0; p < 2; ++p )
        {
            producers.emplace_back( [&, p] {
                std::mt19937 rng( 100 + p );
                auto         timed = [&]( auto&& Post ) {
                    const auto start = Clock::now();
                    Post();
                    const auto took = std::chrono::duration_cast<std::chrono::microseconds>( Clock::now() - start ).count();
                    auto       prev = slowest.load();
                    while( took > prev && !slowest.compare_exchange_weak( prev, took ) ) {}
                    ++posts;
                };

                // Each producer has its own notes, so its on and off of a
                // note are ordered.
                while( Clock::now() < deadline )
                {
                    const auto note  = p * 64 + (int)( rng() % 64 );
                    const auto burst = 1 + (int)( rng() % 64 );
                    for( auto i = 0; i < burst; ++i )
                        timed( [&] { synth.on( note ); } );
                    if( 0 == rng() % 8 )
                        std::this_thread::sleep_for( std::chrono::microseconds( rng() % 2000 ) );
                    timed( [&] { synth.noteOff( note ); } );
                }
            } );
        }
        for( auto& it : producers )
            it.join();
        producing = false;

        auto waitForBlocks = [&]( long Count ) {
            const auto until = blocks.load() + Count;
            while( blocks.load() < until )
                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        };
        auto numPlaying = [&] {
            // The audio thread is stalled or done: the voices are ours.
            auto playing = 0;
            for( auto note = 0; note < 128; ++note )
                playing += synth.getNumPlaying( note );
            return playing;
        };

        auto stall = [&] {
            stalled = true;
            while( !idle.load() )
                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        };

        // Let the audio thread catch up.
        waitForBlocks( 8 );
        stall();
        const auto stuck = numPlaying();

        // Then, with notes playing, the audio thread stops draining the
        // queue and it fills up: the note offs that come after must not be
        // lost.
        stalled = false;
        idle    = false;
        for( auto note = 0; note < 16; ++note )
            synth.on( note );
        waitForBlocks( 8 );
        stall();
        const auto started = numPlaying();
        for( auto i = 0; i < 1100; ++i )
            synth.on( 100 );
        for( auto note = 0; note < 16; ++note )
            synth.noteOff( note );
        synth.noteOff( 100 );
        stalled = false;
        idle    = false;
        waitForBlocks( 8 );
        draining = false;
        audio.join();
        const auto hanging = numPlaying();

        std::printf( "note queue: %ld posts, slowest %lld us, %d voices left playing; %d of %d left playing after a full queue\n",
                     posts.load(), slowest.load(), stuck, hanging, started );
        check( 0 == stuck, "a note off was lost: voices are left playing" );
        check( 0 < started && 0 == hanging, "a note off was lost when the queue was full: voices are left playing" );
        check( slowest.load() < 20000, "posting a note event blocked" );
    }

    //
    // Sequencer.
    //

    constexpr int NUM_SEQUENCES = 4;
    constexpr int NUM_TRACKS    = 4;

    void fillSequence( mt::MTAudioManager& M, int Sequence, std::mt19937& Rng )
    {
        if( auto s = M.getSequence( Sequence ) )
        {
            s->setLength( 1 );
            while( s->getNumTracks() < NUM_TRACKS )
                s->createTrack( juce::String( "T" ) + juce::String( s->getNumTracks() ) );

            for( auto t = 0; t < s->getNumTracks(); ++t )
            {
                if( auto track = s->getTrack( t ) )
                {
                    if( auto program = M.getProgram( (int)( Rng() % (unsigned)juce::jmax( 1, (int)M.getPrograms().size() ) ) ) )
                        track->setProgram( program );
                    for( auto start = 0; start < 384; start += 24 )
                        track->addOrModifyNote( 36 + (int)( Rng() % 16 ), start + (int)( Rng() % 4 ), 12, 100, 100, 1 + (int)( Rng() % 3 ) );
                }
            }
        }
    }

    void setUpProject( mt::MTAudioManager& M, juce::File& Project, std::mt19937& Rng )
    {
        check( M.loadProject( Project ), "load the project" );

        while( M.getPrograms().size() < 3 )
            M.createProgram( mt::MTProgramType::Drum, "P" + juce::String( (int)M.getPrograms().size() ) );
        while( M.getNumSequences() < NUM_SEQUENCES )
            M.createSequence( "S" + juce::String( M.getNumSequences() ) );
        for( auto i = 0; i < M.getNumSequences(); ++i )
            fillSequence( M, i, Rng );

        if( !M.isSongExists( "Song" ) )
        {
            M.createSong( "Song" );
            for( auto i = 0; i < NUM_SEQUENCES; ++i )
            {
                mt::MTSongStepInfo step;
                step.seqNo       = i;
                step.repeatCount = 1;
                step.bars        = 1;
                M.addSongStep( "Song", step );
            }
        }
    }

    // Blocks the message thread for up to Milliseconds and returns whether
    // the playing sequence became Expected meanwhile.
    bool switchesWithoutMessageThread( mt::MTAudioManager& M, mt::MTSequence* Expected, int Milliseconds )
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds( Milliseconds );
        while( Clock::now() < deadline )
        {
            if( M.getPlayingSequence() == Expected )
                return true;
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
        std::printf( "playing sequence %d, expected %d\n", M.getSequenceIndex( M.getPlayingSequence() ), M.getSequenceIndex( Expected ) );
        return false;
    }

    void testSwitches( mt::MTAudioManager& M )
    {
        // Let the engine start (its audio device) first.
        runMessageLoop( 500 );

        // A sequence is a bar long, about a second.
        M.play( 0 );
        runMessageLoop( 100 );
        M.setNextSequence( 1 );
        check( switchesWithoutMessageThread( M, M.getSequence( 1 ), 2500 ), "switch to the next sequence at the end of the playing one" );
        runMessageLoop( 100 );

        M.play( 2, true, false );
        check( switchesWithoutMessageThread( M, M.getSequence( 2 ), 2500 ), "switch to a sequence at the next bar" );
        runMessageLoop( 100 );

        M.playSong( "Song", 0 );
        runMessageLoop( 100 );
        check( M.getPlayingSequence() == M.getSequence( 0 ), "play the song's first step" );
        check( switchesWithoutMessageThread( M, M.getSequence( 1 ), 2500 ), "switch to the song's next step" );
        runMessageLoop( 100 ); // Arms the third step.
        check( switchesWithoutMessageThread( M, M.getSequence( 2 ), 2500 ), "switch to the song's third step" );

        M.stop();
        runMessageLoop( 50 );
    }

    void testTransport( mt::MTAudioManager& M, juce::File& Project, int Seconds, std::mt19937& Rng )
    {
        long       operations = 0;
        const auto deadline   = Clock::now() + std::chrono::seconds( Seconds );
        while( Clock::now() < deadline )
        {
            const auto seq = (int)( Rng() % NUM_SEQUENCES );
            switch( Rng() % 16 )
            {
                case 0:
                case 1: M.play( seq ); break;
                case 2: M.play( seq, true, false ); break;
                case 3: M.setNextSequence( seq ); break;
                case 4: M.setNextBarFlag( true ); break;
                case 5: M.stop(); break;
                case 6: M.pause(); break;
                case 7: M.resume( seq ); break;
                case 8: M.seek( (int)( Rng() % 400 ) ); break;
                case 9: M.playSong( "Song", (int)( Rng() % NUM_SEQUENCES ) ); break;
                case 10:
                    if( auto track = M.getTrack( seq, (int)( Rng() % NUM_TRACKS ) ) )
                        track->addOrModifyNote( 40, (int)( Rng() % 384 ), 6 );
                    break;
                case 11:
                    M.deleteSequence( seq );
                    M.createSequence( "S" + juce::String( (int)Rng() % 1000 ) );
                    fillSequence( M, M.getNumSequences() - 1, Rng );
                    break;
                case 12:
                    M.deleteProgram( (int)( Rng() % M.getPrograms().size() ) );
                    M.createProgram( mt::MTProgramType::Drum, "P" + juce::String( (int)Rng() % 1000 ) );
                    fillSequence( M, seq, Rng );
                    break;
                case 13:
                    if( 0 == Rng() % 8 )
                    {
                        M.closeProject();
                        setUpProject( M, Project, Rng );
                    }
                    break;
                default: break;
            }
            ++operations;
            runMessageLoop( (int)( Rng() % 20 ) );
        }
        M.stop();
        runMessageLoop( 50 );
        std::printf( "transport: %ld operations\n", operations );
    }
} // namespace

int main( int argc, char** argv )
{
    const int seconds = ( argc > 1 ) ? std::atoi( argv[1] ) : 10;
    if( argc < 3 )
    {
        std::printf( "usage: %s <seconds> <scratch directory> [notes|switches|transport]\n", argv[0] );
        return 2;
    }

    juce::MessageManager::getInstance();
    {
        juce::File dir( juce::File::getCurrentWorkingDirectory().getChildFile( argv[2] ) );
        dir.createDirectory();
        auto project = dir.getChildFile( "Stress.kick" );

        // The engine's own instance: parts of it (songs) use it.
        std::mt19937 rng( 1234 );
        auto*        manager = mt::getAudioInterface().getManager();
        setUpProject( *manager, project, rng );

        const juce::String only = ( argc > 3 ) ? argv[3] : "";
        if( only.isEmpty() || only == "notes" )
            testNoteQueue( juce::jmax( 1, seconds / 3 ) );
        if( only.isEmpty() || only == "switches" )
            testSwitches( *manager );
        if( only.isEmpty() || only == "transport" )
            testTransport( *manager, project, juce::jmax( 1, seconds - seconds / 3 ), rng );

        manager->closeProject();
        runMessageLoop( 50 );
    }
    juce::DeletedAtShutdown::deleteAll();
    juce::MessageManager::deleteInstance();

    std::printf( "%s\n", 0 == failures ? "PASS" : "FAIL" );
    return 0 == failures ? 0 : 1;
}
