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
        // Stop the clock first: its thread's onTick() copies transportAlive,
        // and stopping waits for a tick it is running.
        close();
        midiClock.onTick = []( uint32_t ) {};

        if( tickProcessingThread )
        {
            stopTickProcessing.store( true );
            tickProcessingThread->join();
            delete tickProcessingThread;
            tickProcessingThread = nullptr;
        }

        // Song ticks still waiting in the message queue must not run.
        transportAlive.reset();
    }

    void MTAudioTransportManager::close()
    {
        stop();
        {
            const std::lock_guard<std::mutex> sl( switchLock );
            playingSequence = nullptr;
            nextBarSwitch   = {};
            endSwitch       = {};
            songSwitch      = {};
            songSwitchArmed = false;
        }
        waitForTickThread();
    }

    void MTAudioTransportManager::detachSequence( MTSequence* Sequence )
    {
        if( !Sequence )
            return;

        bool isPlaying;
        {
            // Once it is not armed, the tick thread cannot switch to it.
            const std::lock_guard<std::mutex> sl( switchLock );
            for( auto* pending : { &nextBarSwitch, &endSwitch, &songSwitch } )
                if( pending->sequence == Sequence )
                    *pending = {};
            isPlaying = ( Sequence == playingSequence.load() );
        }

        if( isPlaying )
        {
            stop();
            const std::lock_guard<std::mutex> sl( switchLock );
            playingSequence = nullptr;
        }
        waitForTickThread(); // It may have been playing until just now.
    }

    void MTAudioTransportManager::waitForTickThread()
    {
        // The tick thread sets threadIsProcessingTick before it reads
        // playingSequence, so once the tick it is processing now is done it
        // cannot be using a sequence it read before playingSequence was
        // cleared. The caller frees that next, so wait for as long as it
        // takes: the tick thread never waits for this thread, and the
        // callers hold none of the locks it takes.
        jassert( std::this_thread::get_id() != tickProcessingThread->get_id() );

        const auto processed = ticksProcessed.load();
        const auto start     = juce::Time::getMillisecondCounter();
        auto       reported  = false;
        while( threadIsProcessingTick.load() && processed == ticksProcessed.load() )
        {
            tickProcessed.wait( 100 );
            if( !reported && juce::Time::getMillisecondCounter() - start >= 1000 )
            {
                reported = true;
                juce::Logger::writeToLog( "MTAudioTransportManager: the tick thread has been busy with one tick for over a second, still waiting for it" );
            }
        }
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
            const auto resume = isPaused();
            {
                const std::lock_guard<std::mutex> sl( switchLock );
                startSequence( s, resume );
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
        int retVal = -1;
        {
            const std::lock_guard<std::mutex> sl( switchLock );
            midiClock.stop();
            if( auto playing = playingSequence.load() )
            {
                playing->pause();
                retVal = 0;
            }
        }
        setTransportState( TransportState::Paused );
        return retVal;
    }

    int MTAudioTransportManager::stop()
    {
        int retVal = -1;
        {
            const std::lock_guard<std::mutex> sl( switchLock );
            midiClock.stop();
            if( auto playing = playingSequence.load() )
            {
                playing->stop();
                retVal = 0;
            }
        }
        setTransportState( TransportState::Stopped );
        return retVal;
    }

    int MTAudioTransportManager::play( int Seq, bool FromBeginning, bool Now )
    {
        if( Now )
            return FromBeginning ? play( Seq ) : resume( Seq );

        // At the bar the transport is playing, so starting the sequence
        // from the beginning and resuming it (play()) come to the same.
        if( getSequence( Seq ) )
            armSwitch( nextBarSwitch, Seq );
        return -1;
    }

    void MTAudioTransportManager::seek( int posInTicks )
    {
        const std::lock_guard<std::mutex> sl( switchLock );
        if( auto playing = playingSequence.load() )
        {
            playing->seek( posInTicks );
        }
    }

    // Caller holds switchLock.
    void MTAudioTransportManager::startSequence( MTSequence* Sequence, bool Resume )
    {
        if( auto playing = playingSequence.load(); playing && ( Sequence != playing ) )
            playing->stop();
        playingSequence = Sequence;

        if( Resume )
            Sequence->resume();
        else
            Sequence->play();
    }

    // Message thread.
    void MTAudioTransportManager::armSwitch( PendingSwitch& Switch, int Index )
    {
        // The tick thread cannot read the project: bring the sequence's
        // notes up to date for it now.
        auto sequence = ( 0 <= Index ) ? getSequence( Index ) : nullptr;
        if( sequence )
            sequence->prepareToPlay();

        const std::lock_guard<std::mutex> sl( switchLock );
        Switch = { sequence, sequence ? Index : -1 };
    }

    // Message thread: arm the switch to the song step at the front of the
    // queue.
    void MTAudioTransportManager::armSongStep()
    {
        PendingSwitch next;
        unsigned int  after = 0;
        const auto    armed = !playQueue.empty();
        if( armed )
        {
            const auto& step = playQueue.front();
            if( auto sequence = step.isEnabled ? getSequence( step.seq ) : nullptr )
            {
                sequence->prepareToPlay();
                next = { sequence, step.seq };
            }
            after = (unsigned int)juce::jmax( 0, step.timeInTicks );
        }

        const std::lock_guard<std::mutex> sl( switchLock );
        songSwitch           = next;
        songSwitchArmed      = armed;
        songSwitchTicks      = after;
        songSwitchGeneration = songGeneration;
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

        // Song mode's bookkeeping tells the UI: do that on the message
        // thread. Ticks that come before it gets to them are taken together.
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
            // The tick thread starts the step when it is due (sequenceSwitched()).
            if( ( playQueue.front().timeInTicks > playTime ) && ( nextSequenceUpdateTicks <= playTime ) && ( nextSequenceUpdateTicks >= 0 ) )
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
    }

    // Tick thread, after a tick: make the switch armed for it. Starting a
    // sequence there, at the tick, keeps the previous one from playing on
    // until the message thread gets to it; the message thread then does
    // the rest (song state, selection, telling the UI).
    void MTAudioTransportManager::processPendingSwitches( unsigned int Tick, unsigned int PreviousTick )
    {
        const auto atBar = std::exchange( barReached, false );

        const std::lock_guard<std::mutex> sl( switchLock );

        // Nothing plays, or it was paused or stopped since the tick.
        auto playing = playingSequence.load();
        if( !playing || midiClock.isStopped() )
            return;

        // The clock restarts at 0 when a sequence plays from the start.
        const auto previous = ( Tick >= PreviousTick ) ? PreviousTick : 0u;
        songTicks += Tick - previous;

        if( songSwitchArmed && songTicks >= songSwitchTicks )
        {
            songSwitchArmed = false;
            songTicks       = 0;
            switchSequence( SwitchKind::SongStep, std::exchange( songSwitch, {} ) );
            return;
        }

        // Next sequence: when the playing sequence reaches its end (a
        // multiple of its length), even if that tick was taken together
        // with others.
        if( endSwitch.sequence )
        {
            auto length        = playing->isLooping.get() ? playing->loopEnd.get() - playing->loopStart.get() : (int)playing->length.get();
            auto lengthInTicks = (unsigned int)( length * playing->getTicksPerBar() );
            if( 0 < lengthInTicks && ( Tick / lengthInTicks ) != ( previous / lengthInTicks ) )
            {
                switchSequence( SwitchKind::NextSequence, std::exchange( endSwitch, {} ) );
                return;
            }
        }

        if( atBar && nextBarSwitch.sequence )
            switchSequence( SwitchKind::NextBar, std::exchange( nextBarSwitch, {} ) );
    }

    // Tick thread, holding switchLock.
    void MTAudioTransportManager::switchSequence( SwitchKind Kind, const PendingSwitch& Switch )
    {
        if( Switch.sequence )
            startSequence( Switch.sequence, false );

        juce::MessageManager::callAsync( [this, Kind, index = Switch.index, generation = songSwitchGeneration, alive = std::weak_ptr<bool>( transportAlive )]() {
            if( !alive.expired() )
                sequenceSwitched( Kind, index, generation );
        } );
    }

    // Message thread, after the tick thread switched to sequence Index.
    void MTAudioTransportManager::sequenceSwitched( SwitchKind Kind, int Index, uint32_t Generation )
    {
        switch( Kind )
        {
            case SwitchKind::SongStep:
                // A song set up since has its own steps.
                if( Generation != songGeneration || playQueue.empty() )
                    return;

                ++stepIndex;
                resetSong();
                if( playQueue.front().isEnabled )
                    setSelectedSequence( playQueue.front().seq );
                playQueue.pop();
                armSongStep();
                if( auto playing = playingSequence.load() )
                    signals::onSongSequencePlay( playing->getIndex(), -1, stepIndex );
                break;

            case SwitchKind::NextSequence:
                if( nextSequenceToPlay == Index )
                    nextSequenceToPlay = -1;
                signals::onNextSequencePlay( Index );
                break;

            case SwitchKind::NextBar:
                signals::onNextSequencePlay( Index );
                break;
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

                const auto tick     = curTick.load();
                const auto previous = prevTick.exchange( tick );

                if( auto playing = playingSequence.load() )
                {
                    playing->onTick( tick );
                }

                processPendingSwitches( tick, previous );
            } catch( ... )
            {
                DBG( "Exception While tick Processing..." );
            }
            ++ticksProcessed;
            tickProcessed.signal();
        }
    }

    void MTAudioTransportManager::updatePlayheads()
    {
        if( auto playing = playingSequence.load() )
        {
            playing->playheadLocalToGlobal();

            if( MTTrack* pTrack = playing->getSelectedTrackPtr() )
                pTrack->copyPlayheadTo( MTAudioSequenceManager::getTrackPlayhead() );
        }
    }

    void MTAudioTransportManager::onBeat()
    {
        if( auto playing = playingSequence.load() )
            playing->onBeat();
    }

    // Called on the tick thread, at a bar, inside the playing sequence's
    // onTick(): the switch armed for the bar is made once that returns.
    void MTAudioTransportManager::onBar()
    {
        barReached = true;
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

            // Steps of a song set up before must not be taken for these.
            ++songGeneration;
            {
                const std::lock_guard<std::mutex> sl( switchLock );
                songTicks = 0;
            }
            armSongStep();

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
        nextSequenceToPlay = NextSequence;
        armSwitch( endSwitch, NextSequence );
    }

    void MTAudioTransportManager::setNextBarFlag( bool NextBarFlag )
    {
        auto       playing = playingSequence.load();
        const auto seq     = nextSequenceToPlay == -1 ? ( playing ? playing->getIndex() : -1 ) : nextSequenceToPlay;
        armSwitch( nextBarSwitch, NextBarFlag ? seq : -1 );
    }
}
