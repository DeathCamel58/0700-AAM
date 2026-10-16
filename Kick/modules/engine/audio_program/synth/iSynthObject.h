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
#pragma once

#include "iSynthVoice.h"

namespace mt
{
    class MTIProgram;

    class ISynthObject : public AudIO::Object
        , public AudIO::DawRouting
        , public AudIO::DawInserts
        , public IMidiMsgProcessor
    {
    public:
        ISynthObject( AudIO::Design& D, int Polyphony, AudIO::BusType Type = AudIO::BusType::Object );
        ~ISynthObject();

        // Overall program control API's
        inline bool  isOutMeterEnabled() { return outMeterEnabled_.load(); }
        inline bool  isBypassed() { return bypass_.load(); }
        inline float isMuted() { return master.isMuted(); }

        inline void setBypass( bool Bypass ) { bypass_ = Bypass; }

        // Metering.
        inline void  enableOutputMetering( bool Enable ) { outMeterEnabled_ = true; };
        MTMeterData& readOutMeter();

        // Voices.
        void       addVoices( std::vector<std::unique_ptr<ISynthVoice>>& Voices );
        inline int getPolyphony() const { return numPolyphony; }

        // Virtual functions.
        virtual void noteProcess( juce::Array<juce::MidiMessage>& ){};

        // The note functions below can be called from any thread but the
        // audio thread (the sequencer's tick thread, the message thread).
        // They only queue the change: the voices belong to the audio thread,
        // which applies queued changes at the start of the next block.

        virtual void noteOff( int Note );
        virtual void noteOff( std::vector<int>& Notes );
        virtual void noteOffAll();
        virtual void noteMute( int Note, bool Mute );
        virtual void noteReset( int Note );
        virtual void noteReset( std::vector<int>& Notes );
        virtual void noteLoop( int Note, bool Loop );

        // If you are overriding this, make sure to call this base method.
        virtual void setProgram( MTIProgram* Program );

        // Functions related to processing.
        virtual void initialize( uint32_t samplerate, uint32_t blocksize ){};
        virtual void shutdown(){};
        virtual void preProcess( juce::AudioBuffer<float>& buffer );
        virtual void doProcess( juce::AudioBuffer<float>& buffer );
        virtual void postProcess( juce::AudioBuffer<float>& buffer );

        // Audio thread only (they look at the voices).
        bool isPlaying( int Note );
        int  getNumPlaying( int Note ) const;

        ISynthVoice* isPlayback( int Note );
        ISynthVoice* getFreeVoice( int Note, int Polyphony );
        void         freeVoice( ISynthVoice* Voice );
        void         playVoice( ISynthVoice* Voice );
        void         addVoiceForPlayback( ISynthVoice* Voice );

    protected:
        // A change to the voices, queued for the audio thread.
        struct NoteEvent
        {
            enum class Type
            {
                NoteOn, // Start a free voice on slice (if Polyphony allows).
                ClipNoteOn, // Restart the voice playing Note, or start a free one.
                NoteOff,
                NoteOffAll,
                NoteMute,
                NoteReset,
            };

            Type         type{ Type::NoteOff };
            int          note{ -1 }; // Note the voice is allocated for.
            int          voiceNote{ -1 }; // Note the voice is started with.
            int          velocity{ 0 };
            int          polyphony{ 1 };
            bool         flag{ false }; // NoteMute: mute; ClipNoteOn: note off first.
            ASoundSlice* slice{ nullptr };
        };

        void postNoteOn( int Note, int Velocity, int Polyphony, ASoundSlice* Slice );
        void postNoteOn( int Note, int VoiceNote, int Velocity, int Polyphony, ASoundSlice* Slice );
        void postNoteEvent( const NoteEvent& Event );

    private:
        void process( AudIO::Buffer<float>& buffer );
        void prepareToPlay( uint32_t samplerate, uint32_t blocksize );
        void teardown();

        void processNoteEvents(); // Audio thread.
        void handleNoteEvent( const NoteEvent& Event ); // Audio thread.

    protected:
        MTIAtomicProgramMaster master;

    private:
        std::atomic<bool>                         bypass_{ true };
        std::atomic<bool>                         outMeterEnabled_{ true };
        std::vector<std::unique_ptr<ISynthVoice>> voices;

        // Single consumer (the audio thread); the producers take the lock.
        static constexpr int                  NOTE_QUEUE_SIZE{ 1024 };
        juce::AbstractFifo                    noteFifo{ NOTE_QUEUE_SIZE };
        std::array<NoteEvent, NOTE_QUEUE_SIZE> noteQueue;
        std::mutex                            noteQueueLock;

        AudIO::MeterData  outMeterData;
        AudIO::LevelMeter meter;

        int numPolyphony;

        AudIO::Design& AudioDesign;
        MTIProgram*    program{ nullptr };
    };

};
