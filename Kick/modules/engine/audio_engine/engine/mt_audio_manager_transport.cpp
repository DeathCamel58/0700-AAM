/*
 * Copyright (c) 2024 MUSIC Tribe
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

namespace mt
{
    ///
    ///  MTAudioTransportManager
    ///

    template<typename T>
    class ScopedAtomicResetter
    {
    public:
        ScopedAtomicResetter( std::atomic<T>& val, T resetValue, T initialValue )
            : v( val ), rv( resetValue ) { v.store( initialValue ); }
        ~ScopedAtomicResetter() { v.store( rv ); };
        ScopedAtomicResetter( const ScopedAtomicResetter& )            = delete;
        ScopedAtomicResetter& operator=( const ScopedAtomicResetter& ) = delete;

    private:
        std::atomic<T>& v;
        T               rv;
    };

    MTAudioTransportManager::MTAudioTransportManager( MTSelected& Selected, MTAudioManager& Manager )
        : MTAudioSequenceManager( Selected, Manager )
    {
        // Defaults...
        runtimeVT = juce::ValueTree{ IDs::RUNTIME };
        runtimeVT.getOrCreateChildWithName( IDs::TRANSPORT, nullptr );
        runtimeVT.getOrCreateChildWithName( IDs::PLAYHEAD, nullptr );

        setTransportState( TransportState::Idle );

        /// Connect MIDI callbacks...
        midiClock.onTick           = [&]( uint32_t T ) { onTick( T ); };
        midiClock.onSync24Callback = [&]( uint32_t T, uint32_t U ) {};

        /// Connect sequence playback callbacks.
        signals::on1_2.connect( [&]() {} );
        signals::on1_4.connect( [&]() {} );
        signals::on1_8.connect( [&]() {} );
        signals::on1_16.connect( [&]() {} );

        signals::onBeat.connect( [&]( int ) {} );
        signals::onBar.connect( [&]( int ) { onBar(); } );

        auto tickProcessingFunc = [&]() { processTick(); };
        tickProcessingThread    = new std::thread( tickProcessingFunc );
    }

    MTAudioTransportManager::~MTAudioTransportManager()
    {
        // Song ticks still waiting in the message queue must not run.
        transportAlive.reset();

        close();

        if( tickProcessingThread )
        {
            stopTickProcessing.store( true );
            tickProcessingThread->join();
            delete tickProcessingThread;
            tickProcessingThread = nullptr;
        }
    }

    void MTAudioTransportManager::close()
    {
        stop();
        playingSequence = nullptr;
        waitForTickThread();
    }

    void MTAudioTransportManager::detachSequence( MTSequence* Sequence )
    {
        if( Sequence && Sequence == playingSequence.load() )
            close();
    }

    void MTAudioTransportManager::waitForTickThread()
    {
        // The tick thread sets threadIsProcessingTick before it reads
        // playingSequence, so once that is clear it cannot be using a
        // sequence it read before playingSequence was cleared.
        const auto deadline = juce::Time::getMillisecondCounter() + 1000;
        while( threadIsProcessingTick.load() && juce::Time::getMillisecondCounter() < deadline )
            std::this_thread::yield();
    }

    void MTAudioTransportManager::setup()
    {
        // Initialize transport state...
        setTransportState( TransportState::Idle );
    }

    ////////////////////////////
    //// Transport ...
    ////////////////////////////
    TransportState MTAudioTransportManager::getTransportState()
    {
        auto t = runtimeVT.getOrCreateChildWithName( IDs::TRANSPORT, nullptr );
        return (TransportState)( (int)t.getProperty( IDs::state, TransportState::Idle ) );
    }

    void MTAudioTransportManager::setTransportState( TransportState TState )
    {
        auto t        = runtimeVT.getOrCreateChildWithName( IDs::TRANSPORT, nullptr );
        auto oldState = (TransportState)( (int)t.getProperty( IDs::state, TransportState::Idle ) );

        if( oldState != TState )
        {
            // Set new transport state...
            t.setProperty( IDs::state, TState, nullptr );

            if( transportStateChanged )
                transportStateChanged( TState );
        }
    }

    bool MTAudioTransportManager::isArmed()
    {
        auto pSeq = getSequence();
        return pSeq ? pSeq->isArmed() : false;
    }

    bool MTAudioTransportManager::isRecording()
    {
        auto pSeq = getSequence();
        return pSeq ? pSeq->isRecording() : false;
    }

    // Playback API's

    int MTAudioTransportManager::play( int Seq )
    {
        int retVal = -1;

        if( auto s = getSequence( Seq ) )
        {
            if( auto playing = playingSequence.load(); playing && ( s != playing ) )
            {
                playing->stop();
            }
            playingSequence = s;
            //playingSequence->updatePlayhead(); // redundant call as playingSequence->play() updates the playhead.

            if( isPaused() )
            {
                s->resume();
            }
            else
            {
                s->play();
            }

            setTransportState( TransportState::Playing );
            retVal = 0;
        }

        return retVal;
    }

    int MTAudioTransportManager::resume( int Seq )
    {
        return play( Seq );
    }

    int MTAudioTransportManager::pause()
    {
        midiClock.stop();
        setTransportState( TransportState::Paused );

        int retVal = -1;
        if( auto playing = playingSequence.load() )
        {
            playing->pause();
            retVal = 0;
        }
        return retVal;
    }

    int MTAudioTransportManager::stop()
    {
        midiClock.stop();
        setTransportState( TransportState::Stopped );

        int retVal = -1;
        if( auto playing = playingSequence.load() )
        {
            playing->stop();
            retVal = 0;
        }
        return retVal;
    }

    int MTAudioTransportManager::play( int Seq, bool FromBeginning, bool Now )
    {
        if( Now )
            return FromBeginning ? play( Seq ) : resume( Seq );
        else
        {
            if( auto s = getSequence( Seq ) )
            {
                _nextBar.seq.store( Seq );
                _nextBar.fromBegin.store( FromBeginning );
                _nextBar.isEnabled.store( true );
            }
        }
        return -1;
    }

    void MTAudioTransportManager::seek( int posInTicks )
    {
        if( auto playing = playingSequence.load() )
        {
            playing->seek( posInTicks );
        }
    }

    /// Callback functions...
    void MTAudioTransportManager::onTick( unsigned int Ticks )
    {
        /*if( playingSequence )
        {
            playingSequence->onTick( Ticks );
        }
        updatePlayheads();*/

        /*juce::Logger::writeToLog( "is Tick Processing, " + juce::String( threadIsProcessingTick.load() ? "yes, " : "No, " ) +
            "Prev Tick, " + juce::String( prevTick.load() ) + ", CurTick, " + juce::String(curTick.load()) + ", Ticks" + juce::String(Ticks));*/

        if( threadIsProcessingTick )
            return;

        curTick = Ticks;
        onTickEvent.signal();

        // Song mode and the switch to the next sequence start and stop
        // sequences and tell the UI: do that on the message thread. Ticks
        // that come before it gets to them are taken together.
        songTick.store( Ticks );
        if( !songTickPending.exchange( true ) )
        {
            juce::MessageManager::callAsync( [this, alive = std::weak_ptr<bool>( transportAlive )]() {
                if( !alive.expired() )
                    handleSongTick();
            } );
        }
    }

    void MTAudioTransportManager::handleSongTick()
    {
        songTickPending.store( false );

        // A tick posted before the transport stopped is stale.
        if( midiClock.isStopped() )
            return;

        const auto tick = songTick.load();
        if( tick < lastSongTick )
            lastSongTick = 0;
        const auto previousTick = lastSongTick;
        lastSongTick            = tick;
        if( tick == previousTick )
            return;

        playTime += (int)( tick - previousTick );

        if( !playQueue.empty() )
        {
            if( nextSequenceUpdateTicks == -1 ) nextSequenceUpdateTicks = playQueue.size() ? playQueue.front().nextSeqUpdateTicks : -1;
            if( playQueue.front().timeInTicks <= playTime )
            {
                ++stepIndex;
                resetSong();
                if( playQueue.front().isEnabled )
                {
                    setSelectedSequence( playQueue.front().seq );
                    play( playQueue.front().seq );
                }
                playQueue.pop();
                if( auto playing = playingSequence.load() )
                    signals::onSongSequencePlay( playing->getIndex(), -1, stepIndex );
            }
            else if( ( nextSequenceUpdateTicks <= playTime ) && ( nextSequenceUpdateTicks >= 0 ) )
            {
                nextSequenceUpdateTicks = -2;
                signals::onNextSeqAboutToPlayInSong( playQueue.front().seq );
            }
        }
        else
        {
            if( lastSeqTicks <= playTime )
            {
                signals::onSongStopped();
                resetSong();
            }
        }

        //Next Sequence: when the playing sequence reaches its end (a
        // multiple of its length), even if that tick was taken together
        // with others.
        auto playing = playingSequence.load();
        if( playing && nextSequenceToPlay >= 0 )
        {
            auto length        = playing->isLooping.get() ? playing->loopEnd.get() - playing->loopStart.get() : (int)playing->length.get();
            auto lengthInTicks = (unsigned int)( length * playing->getTicksPerBar() );
            if( 0 < lengthInTicks && ( tick / lengthInTicks ) != ( previousTick / lengthInTicks ) )
            {
                play( nextSequenceToPlay );
                nextSequenceToPlay = -1;
                if( auto next = playingSequence.load() )
                    signals::onNextSequencePlay( next->getIndex() );
            }
        }
    }

    void MTAudioTransportManager::processTick()
    {
        while( !stopTickProcessing )
        {
            onTickEvent.wait( 1 );
            if( prevTick == curTick )
                continue;
            try
            {
                const ScopedAtomicResetter<bool> resetter( threadIsProcessingTick, false, true );

                prevTick.store( curTick );

                if( auto playing = playingSequence.load() )
                {
                    playing->onTick( curTick );
                }
            } catch( ... )
            {
                DBG( "Exception While tick Processing..." );
            }
        }
    }

    void MTAudioTransportManager::updatePlayheads()
    {
        if( auto playing = playingSequence.load() )
        {
            playing->playheadLocalToGlobal();

            if( MTTrack* pTrack = playing->getSelectedTrackPtr() )
                MTAudioSequenceManager::getTrackPlayhead() = pTrack->playhead();
        }
    }

    void MTAudioTransportManager::onBeat()
    {
        if( auto playing = playingSequence.load() )
            playing->onBeat();
    }

    // Called on the tick thread, at a bar.
    void MTAudioTransportManager::onBar()
    {
        if( playingSequence.load() )
        {
            if( _nextBar.isEnabled.exchange( false ) )
            {
                // Starting a sequence changes the transport and tells the UI:
                // do it on the message thread.
                const auto seq       = _nextBar.seq.load();
                const auto fromBegin = _nextBar.fromBegin.load();
                juce::MessageManager::callAsync( [this, seq, fromBegin, alive = std::weak_ptr<bool>( transportAlive )]() {
                    if( alive.expired() )
                        return;
                    fromBegin ? play( seq ) : resume( seq );
                    signals::onNextSequencePlay( seq );
                } );
            }
        }
    }

    void MTAudioTransportManager::onStart( unsigned int BeginTick )
    {
        if( auto playing = playingSequence.load() )
            playing->onStart( BeginTick );
    }

    void MTAudioTransportManager::onLoopBegin( unsigned int BeginTick )
    {
        if( auto playing = playingSequence.load() )
            playing->onLoopBegin( BeginTick );
    }

    void MTAudioTransportManager::onLoopEnd( unsigned int EndTick, unsigned int LoopCount )
    {
        if( auto playing = playingSequence.load() )
            playing->onLoopEnd( EndTick, LoopCount );
    }

    void MTAudioTransportManager::setupSong( MTSongInfo SongInfo, int Index )
    {
        if( SongInfo.name.isNotEmpty() && SongInfo.steps.size() )
        {
            auto index{ 0 };
            bool first{ true };
            int  firstSeq{ -1 };
            int  prevSeq{ -1 };
            int  repeat{ 1 };
            while( !playQueue.empty() )
                playQueue.pop();
            resetSong();
            for( auto& step : SongInfo.steps )
            {
                if( index >= Index )
                {
                    if( first )
                    {
                        firstSeq = step.seqNo;
                        first    = false;
                        prevSeq  = firstSeq;
                        repeat   = step.repeatCount;
                    }
                    else
                    {
                        auto pSeq   = getSequence( prevSeq );
                        auto length = pSeq->isLooping.get() ? pSeq->loopEnd.get() - pSeq->loopStart.get() : (int)pSeq->length.get();

                        PlayManager data;
                        data.timeInTicks        = pSeq->getTicksPerBar() * ( repeat * length );
                        data.nextSeqUpdateTicks = pSeq->getTicksPerBar() * ( ( repeat - 1 ) * length );
                        data.seq                = step.seqNo;
                        data.repeatCount        = step.repeatCount;
                        prevSeq                 = step.seqNo;
                        repeat                  = step.repeatCount;
                        playQueue.push( data );
                    }
                }
                ++index;
            }

            auto pSeq    = getSequence( playQueue.size() ? playQueue.back().seq : firstSeq );
            auto length  = pSeq->isLooping.get() ? pSeq->loopEnd.get() - pSeq->loopStart.get() : (int)pSeq->length.get();
            lastSeqTicks = pSeq->getTicksPerBar() * ( playQueue.size() ? playQueue.back().repeatCount * length : SongInfo.steps[Index].repeatCount * length );
            stepIndex    = Index;
            setSelectedSequence( firstSeq );
            play( firstSeq );

            //Update UI
            if( SongInfo.steps[Index].bars == 1 )
                signals::onSongSequencePlay( SongInfo.steps[Index].seqNo, Index + 1 < SongInfo.steps.size() ? SongInfo.steps[Index + 1].seqNo : -1, Index );
            else
                signals::onSongSequencePlay( SongInfo.steps[Index].seqNo, -1, Index );
            if( SongInfo.steps[Index].repeatCount == 1 )
            {
                if( Index + 1 < SongInfo.steps.size() )
                    signals::onNextSeqAboutToPlayInSong( SongInfo.steps[Index + 1].seqNo );
            }
        }
    }

    void MTAudioTransportManager::resetSong()
    {
        playTime                = 0;
        lastSongTick            = 0;
        nextSequenceUpdateTicks = -1;
    }

    void MTAudioTransportManager::playSequence( int Sequence )
    {
        play( Sequence );
    }

    void MTAudioTransportManager::stopSequence()
    {
        pause();
    }

    void MTAudioTransportManager::setNextSequence( int NextSequence )
    {
        if( nextSequenceToPlay != NextSequence )
            nextSequenceToPlay = NextSequence;
    }

    void MTAudioTransportManager::setNextBarFlag( bool NextBarFlag )
    {
        _nextBar.isEnabled = NextBarFlag;
        _nextBar.seq       = nextSequenceToPlay == -1 ? playingSequence.load()->getIndex() : nextSequenceToPlay;
    }
}
