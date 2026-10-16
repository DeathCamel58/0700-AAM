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

// module include files...

namespace mt
{

    /// @brief
    ISynthObject::ISynthObject( AudIO::Design& D, int Polyphony, AudIO::BusType Type )
        : AudIO::Object( 0, 2, 0 )
        , AudIO::DawRouting( D, Type, AudIO::BusWidth::Stereo )
        , AudioDesign( D )
    {
        // Limits check...
        numPolyphony = AudIO::utils::limit<int>( 0, POLYPHONY::MAX, Polyphony );

        AudIO::DawRouting::setID( getId() );
    }

    void ISynthObject::addVoices( std::vector<std::unique_ptr<ISynthVoice>>& Voices )
    {
        voices.clear();
        for( auto& it : Voices )
        {
            voices.push_back( std::move( it ) );
        }
    }

    ISynthObject::~ISynthObject()
    {
        AudioDesign.removeMeter( getId() );
        setBypass( true );

        // Free Voices...
        voices.clear();
    }

    void ISynthObject::setProgram( MTIProgram* Program )
    {
        program = Program;
        master.attachTo( Program ? Program->getState() : juce::ValueTree{ IDs::PROGRAM } );
    }

    bool ISynthObject::isPlaying( int Note )
    {
        return nullptr != isPlayback( Note );
    }

    int ISynthObject::getNumPlaying( int Note ) const
    {
        int num = 0;
        if( CommonUtils::isValidMidiNote( Note ) )
            for( auto& it : voices )
                if( it->isPlayingNote( Note ) )
                    ++num;

        return num;
    }

    ISynthVoice* ISynthObject::isPlayback( int Note )
    {
        if( CommonUtils::isValidMidiNote( Note ) )
            for( auto& it : voices )
                if( it->isPlayingNote( Note ) )
                    return it.get();

        return nullptr;
    }

    ISynthVoice* ISynthObject::getFreeVoice( int Note, int Polyphony )
    {
        if( CommonUtils::isValidMidiNote( Note ) )
        {
            auto numPlaying = getNumPlaying( Note );
            if( numPlaying < Polyphony )
            { // Find free voice.
                for( auto& it : voices )
                    if( it->isFree() )
                        return it->changeToSettingupAndReturn();
            }
            else
            { // Steal voice.
            }
        }
        return nullptr;
    }

    void ISynthObject::freeVoice( ISynthVoice* Voice )
    {
        if( Voice )
            Voice->makeFree();
    }

    void ISynthObject::playVoice( ISynthVoice* Voice )
    {
        if( Voice )
            Voice->makeRunning();
    }

    void ISynthObject::addVoiceForPlayback( ISynthVoice* Voice )
    {
        playVoice( Voice );
    }

    void ISynthObject::noteMute( int Note, bool Mute )
    {
        if( CommonUtils::isValidMidiNote( Note ) )
        {
            NoteEvent event;
            event.type = NoteEvent::Type::NoteMute;
            event.note = Note;
            event.flag = Mute;
            postNoteEvent( event );
        }
    }

    void ISynthObject::noteReset( int Note )
    {
        if( CommonUtils::isValidMidiNote( Note ) )
        {
            NoteEvent event;
            event.type = NoteEvent::Type::NoteReset;
            event.note = Note;
            postNoteEvent( event );
        }
    }

    void ISynthObject::noteReset( std::vector<int>& Notes )
    {
        for( auto it : Notes )
            noteReset( it );
    }

    void ISynthObject::noteLoop( int Note, bool Loop )
    {
        //for( const auto& it : playingVoices )
        //    if( it->p.midiNote == Note )
        //        it->setLoop( Loop );
    }

    void ISynthObject::noteOff( int Note )
    {
        if( CommonUtils::isValidMidiNote( Note ) )
        {
            NoteEvent event;
            event.type = NoteEvent::Type::NoteOff;
            event.note = Note;
            postNoteEvent( event );
        }
    }

    void ISynthObject::noteOff( std::vector<int>& Notes )
    {
        for( auto it : Notes )
            noteOff( it );
    }

    void ISynthObject::noteOffAll()
    {
        NoteEvent event;
        event.type = NoteEvent::Type::NoteOffAll;
        postNoteEvent( event );
    }

    void ISynthObject::postNoteOn( int Note, int Velocity, int Polyphony, ASoundSlice* Slice )
    {
        postNoteOn( Note, Note, Velocity, Polyphony, Slice );
    }

    void ISynthObject::postNoteOn( int Note, int VoiceNote, int Velocity, int Polyphony, ASoundSlice* Slice )
    {
        NoteEvent event;
        event.type      = NoteEvent::Type::NoteOn;
        event.note      = Note;
        event.voiceNote = VoiceNote;
        event.velocity  = Velocity;
        event.polyphony = Polyphony;
        event.slice     = Slice;
        postNoteEvent( event );
    }

    void ISynthObject::postNoteEvent( const NoteEvent& Event )
    {
        const std::lock_guard<std::mutex> lock( noteQueueLock );

        // The audio thread empties the queue every block, so it is only full
        // when the audio thread is not running (or a burst of over a
        // thousand events came within one block): wait for one block.
        if( 0 == noteFifo.getFreeSpace() )
            AudIO::AudioThreadSync::get().waitForBlock( std::chrono::milliseconds( 50 ) );

        const auto scope = noteFifo.write( 1 );
        if( scope.blockSize1 > 0 )
            noteQueue[(size_t)scope.startIndex1] = Event;
        else if( scope.blockSize2 > 0 )
            noteQueue[(size_t)scope.startIndex2] = Event;
        else
            DBG( "ISynthObject: note queue full, note event dropped" );
    }

    void ISynthObject::processNoteEvents()
    {
        const auto ready = noteFifo.getNumReady();
        if( 0 == ready )
            return;

        const auto scope = noteFifo.read( ready );
        for( auto i = 0; i < scope.blockSize1; ++i )
            handleNoteEvent( noteQueue[(size_t)( scope.startIndex1 + i )] );
        for( auto i = 0; i < scope.blockSize2; ++i )
            handleNoteEvent( noteQueue[(size_t)( scope.startIndex2 + i )] );
    }

    void ISynthObject::handleNoteEvent( const NoteEvent& Event )
    {
        const auto note = Event.note;
        switch( Event.type )
        {
            case NoteEvent::Type::NoteOn:
                if( auto voice = getFreeVoice( note, Event.polyphony ) )
                {
                    voice->setSlice( Event.slice );
                    voice->noteOn( Event.voiceNote, Event.velocity );
                    addVoiceForPlayback( voice );
                }
                break;

            case NoteEvent::Type::ClipNoteOn:
                if( auto voice = isPlayback( note ) )
                {
                    if( Event.flag )
                        voice->noteOff();
                    voice->reset();
                    voice->noteOn( Event.voiceNote, Event.velocity );
                }
                else if( auto free = getFreeVoice( note, Event.polyphony ) )
                {
                    free->setSlice( Event.slice );
                    free->noteOn( Event.voiceNote, Event.velocity );
                    addVoiceForPlayback( free );
                }
                break;

            case NoteEvent::Type::NoteOff:
                for( auto& it : voices )
                    it->noteOff( note );
                break;

            case NoteEvent::Type::NoteOffAll:
                for( auto& it : voices )
                    it->noteOff();
                break;

            case NoteEvent::Type::NoteMute:
                for( auto& it : voices )
                    it->noteMute( note, Event.flag );
                break;

            case NoteEvent::Type::NoteReset:
                for( auto& it : voices )
                    it->noteReset( note );
                break;
        }
    }

    void ISynthObject::prepareToPlay( uint32_t samplerate, uint32_t blocksize )
    {
        AudIO::Object::prepareToPlay( samplerate, blocksize );
        AudIO::DawInserts::prepareToPlay( samplerate, blocksize );
        initialize( samplerate, blocksize );

        // The audio thread is not running this object now. Drop what was
        // queued while nothing played it, rather than play it all at once.
        const std::lock_guard<std::mutex> lock( noteQueueLock );
        noteFifo.reset();
    }

    void ISynthObject::teardown()
    {
        AudIO::Object::teardown();
        shutdown();
    }

    void ISynthObject::process( AudIO::Buffer<float>& buffer )
    {
        juce::AudioBuffer<float> bufferToFill( (float* const*)buffer.getOutputs(), buffer.getNumOutputChannels(), buffer.getBlockSize() );
        bufferToFill.clear();

        processNoteEvents();

        if( isBypassed() )
            return;

        // Do pre-processing...
        preProcess( bufferToFill );

        for( auto& it : voices )
            it->process( bufferToFill );

        // Do processing if required...
        doProcess( bufferToFill );

        // Perform inserts...
        AudIO::DawInserts::process( buffer );

        // Do post-processing...
        postProcess( bufferToFill );

        if( outMeterEnabled_.load() )
            meter.process( buffer );
    }

    void ISynthObject::preProcess( juce::AudioBuffer<float>& buffer ){};

    void ISynthObject::doProcess( juce::AudioBuffer<float>& buffer ){};

    void ISynthObject::postProcess( juce::AudioBuffer<float>& buffer )
    {
        auto ns = buffer.getNumSamples();
        auto nc = buffer.getNumChannels();

        // Apply Master Gain and Pan.
        if( !juce::approximatelyEqual( master.gainL.load(), float( 1 ) ) )
            buffer.applyGain( 0, 0, ns, master.gainL.load() );
        if( 1 < nc && !juce::approximatelyEqual( master.gainR.load(), float( 1 ) ) )
            buffer.applyGain( 1, 0, ns, master.gainR.load() );
    };

    // Metering...
    MTMeterData& ISynthObject::readOutMeter()
    {
        return meter.getMeter( outMeterData );
    }
};
