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

// Stress test of playing pads while they are edited, on the engine as
// KickAudioEngine runs it (built without audio device types, so it renders
// on its fallback clock):
//
// - the sequencer's tick thread plays tracks of a drum, a clip and a
//   keygroup program, and a second thread (standing in for MIDI input)
//   sends the same programs note ons and offs;
// - meanwhile the message thread assigns and clears the pads' samples,
//   removes pads, changes their mute groups, simultaneous plays and mutes,
//   solos and mutes them and changes how they pick a layer, and mutes,
//   solos and sets the level of the programs;
// - with "assets", it also removes sample files from the project and adds
//   them again, so that the last reference to a slice can go while a voice
//   still plays it or a note on for it is still queued;
// - with "cycles", the pads' simultaneous plays may form loops.
//
// Configure Kick with -DKICK_BUILD_TESTS=ON (and
// -DKICK_TEST_SANITIZER=thread or address), build the PadStress target and
// run it for a number of seconds with a scratch directory:
//
//   PadStress 30 /tmp/pad-stress assets cycles
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

    constexpr int FIRST_PAD = 36;
    constexpr int NUM_PADS  = 16;

    void runMessageLoop( int Milliseconds )
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil( Milliseconds );
    }

    juce::String writeWav( const juce::File& Dir, const juce::String& Name, float Freq )
    {
        auto file = Dir.getChildFile( Name );
        file.deleteFile();
        juce::WavAudioFormat                     wav;
        std::unique_ptr<juce::AudioFormatWriter> writer( wav.createWriterFor( new juce::FileOutputStream( file ), 48000.0, 2, 16, {}, 0 ) );
        juce::AudioBuffer<float>                 b( 2, 24000 );
        for( int i = 0; i < b.getNumSamples(); ++i )
        {
            const auto v = 0.2f * std::sin( Freq * (float)i / 48000.f * juce::MathConstants<float>::twoPi );
            b.setSample( 0, i, v );
            b.setSample( 1, i, v );
        }
        writer->writeFromAudioSampleBuffer( b, 0, b.getNumSamples() );
        return file.getFullPathName();
    }
} // namespace

int main( int argc, char** argv )
{
    if( argc < 3 )
    {
        std::printf( "usage: %s <seconds> <scratch directory> [assets] [cycles]\n", argv[0] );
        return 2;
    }
    const int  seconds = juce::jmax( 1, std::atoi( argv[1] ) );
    const auto options = juce::StringArray( argv + 3, argc - 3 );
    const bool assets  = options.contains( "assets" );
    const bool cycles  = options.contains( "cycles" );

    bool passed = true;
    juce::MessageManager::getInstance();
    {
        juce::File dir( juce::File::getCurrentWorkingDirectory().getChildFile( argv[2] ) );
        dir.createDirectory();

        auto* manager = mt::getAudioInterface().getManager();
        auto project = dir.getChildFile( "PadStress.kick" );
        passed &= manager->loadProject( project );

        const juce::String files[] = { writeWav( dir, "a.wav", 220.f ), writeWav( dir, "b.wav", 330.f ), writeWav( dir, "c.wav", 440.f ) };
        for( const auto& f : files )
            static_cast<mt::MTAudioAssetManager*>( manager )->add( f );

        // The programs stay for the whole test: the stand-in MIDI input
        // thread keeps pointers to them.
        auto* drum = manager->createProgram( mt::MTProgramType::Drum, "Drum" );
        auto* clip = manager->createProgram( mt::MTProgramType::Clip, "Clip" );
        auto* keys = manager->createProgram( mt::MTProgramType::Keygroup, "Keys" );
        passed &= ( drum && clip && keys );
        mt::MTIProgram* programs[] = { drum, clip, keys };

        // A sequence a bar long, a track per program, a note every 16th.
        if( 0 == manager->getNumSequences() )
            manager->createSequence( "S" );
        if( auto s = manager->getSequence( 0 ) )
        {
            s->setLength( 1 );
            std::mt19937 rng( 7 );
            for( auto* p : programs )
            {
                if( auto track = s->getTrack( s->createTrack( p->getName() ) ) )
                {
                    track->setProgram( p );
                    for( auto start = 0; start < 384; start += 24 )
                        track->addOrModifyNote( FIRST_PAD + (int)( rng() % NUM_PADS ), start, 12, 100, 100, 1 + (int)( rng() % 3 ) );
                }
            }
        }

        // Let the engine start, then play.
        runMessageLoop( 300 );
        manager->play( 0 );

        std::atomic<bool> running{ true };
        std::atomic<long> notes{ 0 };
        std::thread       midiIn( [&] {
            std::mt19937 rng( 99 );
            while( running.load() )
            {
                juce::Array<juce::MidiMessage> msgs;
                for( int i = 0; i < 4; ++i )
                {
                    const int note = FIRST_PAD + (int)( rng() % NUM_PADS );
                    msgs.add( ( rng() & 1 ) ? juce::MidiMessage::noteOn( 1, note, (juce::uint8)( rng() % 127 + 1 ) ) : juce::MidiMessage::noteOff( 1, note ) );
                }
                for( auto* p : programs )
                {
                    auto copy = msgs;
                    p->noteProcess( copy );
                }
                notes += 12;
                std::this_thread::sleep_for( std::chrono::microseconds( 200 ) );
            }
        } );

        std::mt19937 rng( 1234 );
        long         edits    = 0;
        const auto   deadline = Clock::now() + std::chrono::seconds( seconds );
        while( Clock::now() < deadline )
        {
            const int note = FIRST_PAD + (int)( rng() % NUM_PADS );
            auto*     p    = programs[rng() % 3];
            switch( rng() % 13 )
            {
                case 0:
                case 1:
                    // Only layer 0: a sample has a ValueTree for one layer.
                    p->addSample( note, files[rng() % 3], 0, 0 );
                    break;
                case 2: p->removeSample( note ); break;
                case 3: p->removeSample( note, 0 ); break;
                case 4: p->setNoteMuteGroup( note, (int)( rng() % 3 ) - 1 ); break;
                case 5:
                    p->setSimultaneousNotePlay( note, { cycles ? FIRST_PAD + (int)( rng() % NUM_PADS ) : note + 1 + (int)( rng() % 3 ), -1 } );
                    break;
                case 6: p->setSimultaneousNoteMute( note, { FIRST_PAD + (int)( rng() % NUM_PADS ) } ); break;
                case 7: p->setNoteSolo( note, 0 == rng() % 4 ); break;
                case 8: p->setNoteMute( note, 0 == rng() % 3 ); break;
                case 9:
                    if( auto d = dynamic_cast<mt::MTDrumProgram*>( p ) )
                        d->setNoteLayerPlay( note, (mt::LayerPlayType)( rng() % 3 ) );
                    break;
                case 12:
                    // The program's own mute, solo and level.
                    switch( rng() % 3 )
                    {
                        case 0: p->setMute( 0 == rng() % 3 ); break;
                        case 1: p->setSolo( 0 == rng() % 3 ); break;
                        case 2: p->setLevel( (int)( rng() % 128 ) ); break;
                    }
                    break;
                case 10:
                case 11:
                    if( assets )
                    {
                        const auto& f = files[rng() % 3];
                        static_cast<mt::MTAudioAssetManager*>( manager )->remove( f );
                        static_cast<mt::MTAudioAssetManager*>( manager )->add( f );
                    }
                    break;
            }
            // Let the programs publish what changed now and then.
            if( 0 == ++edits % 8 )
                runMessageLoop( 1 );
        }

        running = false;
        midiIn.join();
        manager->stop();
        runMessageLoop( 100 );
        manager->closeProject();
        runMessageLoop( 100 );

        std::printf( "%ld note messages, %ld edits\n", notes.load(), edits );
        dir.deleteRecursively();
    }
    juce::DeletedAtShutdown::deleteAll();
    juce::MessageManager::deleteInstance();

    std::printf( "%s\n", passed ? "PASS" : "FAIL" );
    return passed ? 0 : 1;
}
