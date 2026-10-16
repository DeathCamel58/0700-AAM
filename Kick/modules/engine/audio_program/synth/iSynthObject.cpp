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

    void ISynthObject::postNoteOn( int Note, int Velocity, int Polyphony, std::shared_ptr<ASoundSlice> Slice )
    {
        postNoteOn( Note, Note, Velocity, Polyphony, std::move( Slice ) );
    }

    void ISynthObject::postNoteOn( int Note, int VoiceNote, int Velocity, int Polyphony, std::shared_ptr<ASoundSlice> Slice )
    {
        NoteEvent event;
        event.type      = NoteEvent::Type::NoteOn;
        event.note      = Note;
        event.voiceNote = VoiceNote;
        event.velocity  = Velocity;
        event.polyphony = Polyphony;
        event.slice     = std::move( Slice );
        postNoteEvent( event );
    }

    void ISynthObject::postNoteEvent( const NoteEvent& Event )
    {
        // Never wait for the audio thread here: the sequencer's tick thread
        // posts too, and the audio thread may not be draining the queue at
        // all (no device, or the program is not being processed).
        const std::lock_guard<std::mutex> lock( noteQueueLock );

        collectReleasedSlices();

        // The entry about to be overwritten may hold the last reference to
        // the slice of an event the audio thread has handled.
        auto write = [this, &Event]( int Index ) {
            auto& entry = noteQueue[(size_t)Index];
            if( entry.slice )
                releasedSlices.push_back( std::move( entry.slice ) );
            entry = Event;
        };

        if( !noteQueueFull.load() )
        {
            const auto scope = noteFifo.write( 1 );
            if( scope.blockSize1 > 0 )
            {
                write( scope.startIndex1 );
                return;
            }
            if( scope.blockSize2 > 0 )
            {
                write( scope.startIndex2 );
                return;
            }

            // Full. Until the audio thread has caught up, nothing more goes
            // into the queue, so what is kept applies after all of it.
            noteQueueFull.store( true );

            const auto id      = getId();
            const auto dropped = droppedNoteEvents.load();
            juce::MessageManager::callAsync( [id, dropped]() {
                juce::Logger::writeToLog( "ISynthObject " + juce::String( id ) + ": note queue full, dropping note ons until the audio thread drains it ("
                                          + juce::String( dropped ) + " note events dropped before)" );
            } );
        }

        keepNoteEvent( Event );
    }

    void ISynthObject::keepNoteEvent( const NoteEvent& Event )
    {
        // A note on can be lost, a note off cannot: the voice would hang.
        const auto note = Event.note;
        switch( Event.type )
        {
            case NoteEvent::Type::NoteOff:
                if( CommonUtils::isValidMidiNote( note ) )
                    keptNoteOffs.set( (size_t)note );
                break;

            case NoteEvent::Type::NoteReset:
                if( CommonUtils::isValidMidiNote( note ) )
                    keptNoteResets.set( (size_t)note );
                break;

            case NoteEvent::Type::NoteOffAll:
                keptNoteOffAll = true;
                break;

            default:
                droppedNoteEvents.fetch_add( 1 );
                break;
        }
    }

    void ISynthObject::collectReleasedSlices()
    {
        const auto ready = releaseFifo.getNumReady();
        if( 0 < ready )
        {
            const auto scope = releaseFifo.read( ready );
            for( auto i = 0; i < scope.blockSize1; ++i )
                releasedSlices.push_back( std::move( releaseQueue[(size_t)( scope.startIndex1 + i )] ) );
            for( auto i = 0; i < scope.blockSize2; ++i )
                releasedSlices.push_back( std::move( releaseQueue[(size_t)( scope.startIndex2 + i )] ) );
        }

        if( !releasedSlices.empty() )
            sliceReleaser.triggerAsyncUpdate();
    }

    void ISynthObject::freeReleasedSlices()
    {
        std::vector<std::shared_ptr<ASoundSlice>> slices;
        {
            const std::lock_guard<std::mutex> lock( noteQueueLock );
            collectReleasedSlices();
            std::swap( slices, releasedSlices );
        }
        // Freed here, on the message thread, outside the lock.
    }

    bool ISynthObject::startVoiceOnSlice( ISynthVoice* Voice, const std::shared_ptr<ASoundSlice>& Slice )
    {
        // Hand the voice's previous slice over to be freed off the audio
        // thread; there is always room (see releaseFifo), but if there were
        // not, the note would be dropped rather than the slice freed here.
        const auto scope = releaseFifo.write( 1 );
        auto*      entry = ( scope.blockSize1 > 0 ) ? &releaseQueue[(size_t)scope.startIndex1]
                                                    : ( ( scope.blockSize2 > 0 ) ? &releaseQueue[(size_t)scope.startIndex2] : nullptr );
        if( nullptr == entry )
            return false;

        *entry = Voice->exchangeSlice( Slice );
        return true;
    }

    void ISynthObject::processNoteEvents()
    {
        const auto ready = noteFifo.getNumReady();
        if( 0 < ready )
        {
            const auto scope = noteFifo.read( ready );
            for( auto i = 0; i < scope.blockSize1; ++i )
                handleNoteEvent( noteQueue[(size_t)( scope.startIndex1 + i )] );
            for( auto i = 0; i < scope.blockSize2; ++i )
                handleNoteEvent( noteQueue[(size_t)( scope.startIndex2 + i )] );
        }

        if( noteQueueFull.load() )
            processKeptNoteEvents();
    }

    void ISynthObject::processKeptNoteEvents()
    {
        NoteEvent        offAll;
        bool             isOffAll{ false };
        std::bitset<128> offs;
        std::bitset<128> resets;
        {
            // Never wait on the audio thread: if a producer is posting, try
            // again next block.
            const std::unique_lock<std::mutex> lock( noteQueueLock, std::try_to_lock );
            if( !lock.owns_lock() )
                return;

            // What was queued before the queue filled up must be handled
            // first; nothing was queued since.
            if( 0 < noteFifo.getNumReady() )
                return;

            isOffAll = std::exchange( keptNoteOffAll, false );
            offs     = std::exchange( keptNoteOffs, {} );
            resets   = std::exchange( keptNoteResets, {} );
            noteQueueFull.store( false );
        }

        if( isOffAll )
        {
            offAll.type = NoteEvent::Type::NoteOffAll;
            handleNoteEvent( offAll );
        }

        NoteEvent event;
        for( size_t note = 0; note < offs.size(); ++note )
        {
            event.note = (int)note;
            if( offs[note] )
            {
                event.type = NoteEvent::Type::NoteOff;
                handleNoteEvent( event );
            }
            if( resets[note] )
            {
                event.type = NoteEvent::Type::NoteReset;
                handleNoteEvent( event );
            }
        }
    }

    void ISynthObject::handleNoteEvent( const NoteEvent& Event )
    {
        const auto note = Event.note;
        switch( Event.type )
        {
            case NoteEvent::Type::NoteOn:
                if( auto voice = getFreeVoice( note, Event.polyphony ) )
                {
                    if( startVoiceOnSlice( voice, Event.slice ) )
                    {
                        voice->noteOn( Event.voiceNote, Event.velocity );
                        addVoiceForPlayback( voice );
                    }
                    else
                        freeVoice( voice );
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
                    if( startVoiceOnSlice( free, Event.slice ) )
                    {
                        free->noteOn( Event.voiceNote, Event.velocity );
                        addVoiceForPlayback( free );
                    }
                    else
                        freeVoice( free );
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
        keptNoteOffAll = false;
        keptNoteOffs.reset();
        keptNoteResets.reset();
        noteQueueFull.store( false );
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
