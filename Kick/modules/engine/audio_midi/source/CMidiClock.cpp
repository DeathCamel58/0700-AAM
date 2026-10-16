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

#include "CMidiClock.h"

namespace mt
{

    double CMidiClock::sTickCounter   = 0;
    double CMidiClock::sTickFactor    = 1.0;
    bool   CMidiClock::isTimerRunning = false;

    //######################################
    // Global MIDI Clock Instance
    //######################################
    CMidiClock& getMidiClock() noexcept
    {
        static CMidiClock gMidiClock{};
        return gMidiClock;
    }

    //######################################
    // Platform specific timer setup/control
    //######################################
    inline void CMidiClock::initPlatformTimer()
    { //
    }

    inline void CMidiClock::stopPlatformTimer()
    { //
        if( isTimerRunning )
        {
            getMidiClock().stopTimer();
            isTimerRunning = false;
        }
    }

    inline void CMidiClock::startPlatformTimer( [[maybe_unused]] uint32_t microSeconds )
    {
        setPlatformTimerInterval( microSeconds );
        getMidiClock().startTimer( 1 );
        isTimerRunning = true;
    }

    inline void CMidiClock::setPlatformTimerInterval( [[maybe_unused]] float microSeconds )
    { // Compute tick multiplier if desired interval is not possible to meet on the platform.
        if( microSeconds <= 0.f )
            return;

        uint32_t mills = 1;
        sTickFactor    = ( (float)( mills * 1000 ) / (float)microSeconds ) * getMidiClock().getInternalPPQN();
    }

    inline void CMidiClock::handlePlatformTimer()
    { // send tick to interval clock.
        getMidiClock().tickInternal();
    }

    //######################################
    // Call CMidiClock
    //######################################
    CMidiClock::CMidiClock( PPQNResolution PPQN )
    {
        setInternalPPQN( PPQN );
        init();
    }

    CMidiClock::~CMidiClock()
    {
        stop();
    }

    // Clock control API's
    void CMidiClock::init() noexcept
    {
        const std::lock_guard<std::recursive_mutex> sl( controlLock );
        initPlatformTimer();
        setTempo( tempo.getCurrentValue() );
    }

    void CMidiClock::start( bool ClearCounters ) noexcept
    {
        const std::lock_guard<std::recursive_mutex> sl( controlLock );

        // Restarting a running clock: stopping waits for a tick the timer
        // thread is running, which reads the counters reset below.
        stopPlatformTimer();
        takeTempoRequest( false );

        clockCounter       = 0;
        intClockSyncPulses = 0;

        if( ClearCounters )
            resetCounters();

        if( isExternalClockSync() )
        {
            resetCounters();
            resetExternalClockCounters();
        }

        state = isInternalClockSync() ? ClockState::STARTED : ClockState::STARTING;
        onClockStart();

        prevTime = juce::Time::getMillisecondCounterHiRes();
        startPlatformTimer( microsPerQuarterNote );
    }

    void CMidiClock::stop() noexcept
    {
        const std::lock_guard<std::recursive_mutex> sl( controlLock );
        stopPlatformTimer();

        if( isExternalClockSync() )
        {
            resetCounters();
            resetExternalClockCounters();
        }

        state = ClockState::STOPPED;
        onClockStop();
    }

    // PPQN factors...
    void CMidiClock::setInternalPPQN( PPQNResolution resolution )
    {
        const std::lock_guard<std::recursive_mutex> sl( controlLock );
        stop();
        intPPQN = resolution;
        recomputePPQNFactor();
    }

    void CMidiClock::setExternalPPQN( PPQNResolution resolution )
    {
        const std::lock_guard<std::recursive_mutex> sl( controlLock );
        stop();
        extPPQN = resolution;
        recomputePPQNFactor();
    }

    // Tempo API's
    float CMidiClock::getTempo()
    {
        if( isExternalClockSync() )
        {
            auto averageMicros = averageOfExternalIntervals();
            return ( 0 == averageMicros ) ? requestedTempo.load() : microsToTempo( averageMicros );
        }
        // The tempo set last: a running clock ramps to it within a
        // millisecond or so.
        return requestedTempo.load();
    }

    void CMidiClock::setTempo( float Bpm, double RampLengthInSeconds )
    {
        const std::lock_guard<std::recursive_mutex> sl( controlLock );

        auto numSteps = isInternalClockSync() ? ( RampLengthInSeconds / 0.001 ) : 1;
        requestedTempo.store( Bpm, std::memory_order_relaxed );
        requestedRampSteps.store( (int)numSteps, std::memory_order_relaxed );
        tempoRequest.fetch_add( 1, std::memory_order_release );

        // A running timer takes it up at its next tick.
        if( !isTimerRunning )
            takeTempoRequest( false );
    }

    // Timer thread while the timer runs; otherwise a control function.
    void CMidiClock::takeTempoRequest( bool UpdateInterval )
    {
        const auto request = tempoRequest.load( std::memory_order_acquire );
        if( request == tempoRequestTaken )
            return;
        tempoRequestTaken = request;
        applyTempo( requestedTempo.load( std::memory_order_relaxed ), requestedRampSteps.load( std::memory_order_relaxed ), UpdateInterval );
    }

    void CMidiClock::applyTempo( float Bpm, int RampSteps, bool UpdateInterval )
    {
        tempo.reset( RampSteps );
        tempo.setTargetValue( Bpm );
        microsPerQuarterNote = tempoToMicros( Bpm ); // In Micro-Seconds.

        if( UpdateInterval )
            setPlatformTimerInterval( microsPerQuarterNote );
    }

    void CMidiClock::setTempoByTime( uint32_t MicrosPerQuarterNote )
    {
        if( 0 == MicrosPerQuarterNote )
            return;

        auto targetTempo = microsToTempo( MicrosPerQuarterNote );
        setTempo( targetTempo );
    }

    // Normally expected to be called at 24PPQN
    void CMidiClock::tickExternal()
    {
        if( isInternalClockSync() )
            return;

        const std::lock_guard<std::recursive_mutex> sl( controlLock );

        switch( state )
        {
            default: break; // ClockState::STOPPED:

            case ClockState::STARTING:
                state                 = ClockState::STARTED;
                ts                    = juce::Time::getMillisecondCounterHiRes();
                extClockFirstInterval = true;
                break;

            case ClockState::STARTED:
            {
                te = juce::Time::getMillisecondCounterHiRes();

                // Increment external ticks count!
                ++extClockSyncPulses;

                // Accumulate intervals between ticks, in microseconds (the
                // timer counts milliseconds).
                const auto intervalUs        = ( te - ts ) * 1000.0;
                ts                           = te;
                extIntervals[extIntervalIdx] = static_cast<uint32_t>( juce::jlimit( 0.0, (double)std::numeric_limits<uint32_t>::max(), intervalUs ) );
                if( ++extIntervalIdx >= MAX_EXT_INTERVALS )
                {
                    extIntervalIdx = 0;
                }

                if( extClockFirstInterval )
                {
                    extClockFirstInterval = false;
                    // Synchronize tempo (setTempo() sets microsPerQuarterNote)
                    setTempoByTime( averageOfExternalIntervals() );
                }

                if( ++extCounter >= TEMPO_SYNC_INTERVAL )
                {
                    extCounter = 0;
                    syncTempo  = true;
                }
                break;
            }
        }
    }

    void CMidiClock::tickInternal()
    {
        takeTempoRequest( true );

        if( tempo.isSmoothing() )
        {
            microsPerQuarterNote = static_cast<uint32_t>( tempoToMicros( tempo.getNextValue() ) ); // In Micro-Seconds.
            setPlatformTimerInterval( microsPerQuarterNote );
        }

        auto currTime  = juce::Time::getMillisecondCounterHiRes();
        auto diffTime  = ( currTime - prevTime );
        auto diffTicks = ( sTickFactor * diffTime );

        if( diffTicks < 1.0 )
            return;
        else
            prevTime = currTime;

        sTickCounter += diffTicks;

        //sTickCounter += sTickFactor;
        //if( sTickCounter < 1.0 )
        //    return;
        //sTickCounter = sTickCounter - 1.0;

        if( ++clockCounter >= intClockPulsesPerExtClockPulse )
        {
            clockCounter = 0;
            // increase internal clock pulses at tempo synchronization rate as per extPPQN.
            ++intClockSyncPulses;

            if( isExternalClockSync() && syncTempo.exchange( false ) )
            {
                // sync tick position with external tick clock
                const uint32_t extPulses = extClockSyncPulses.load();
                if( ( intClockSyncPulses < extPulses ) || ( intClockSyncPulses > ( extPulses + 1 ) ) )
                {
                    intClockSyncPulses = extPulses;
                    ticks              = extPulses * intClockPulsesPerExtClockPulse;
                    clockCounter       = ticks % intClockPulsesPerExtClockPulse;
                }

                // Synchronize tempo (here, on the timer thread, directly:
                // setTempo() would take the control lock).
                if( uint32_t avgIntervalInMicros = averageOfExternalIntervals() )
                    applyTempo( microsToTempo( (float)avgIntervalInMicros ), 1, true );
            }

            if( ++syncCounter >= (uint32_t)extPPQN )
            {
                syncCounter = 0;
                onSync24Callback( intClockSyncPulses, microsPerQuarterNote );
            }
        }

        //// tick me
        //++ticks;

        // Callbacks
        onTick( (uint32_t)sTickCounter );
    }

    void CMidiClock::resetCounters()
    {
        sTickCounter = 0;
        ticks        = 0;

        syncTempo   = false;
        syncCounter = 0;
    }

    void CMidiClock::resetExternalClockCounters()
    {
        extClockFirstInterval = false;
        extCounter            = 0;
        extClockSyncPulses    = 0;
        extIntervalIdx        = 0;
        for( auto& interval : extIntervals )
            interval.store( 0 );
    }

    uint32_t CMidiClock::averageOfExternalIntervals() const noexcept
    {
        // Microseconds per quarter note from the intervals between pulses;
        // 0 until there is one.
        uint64_t count = 0;
        uint64_t total = 0;
        for( const auto& interval : extIntervals )
        {
            const uint64_t val = interval.load();
            if( val != 0 )
            {
                total += val;
                ++count;
            }
        }

        if( 0 == count )
            return 0;

        return static_cast<uint32_t>( juce::jmin( ( total * (uint64_t)extPPQN ) / count, (uint64_t)std::numeric_limits<uint32_t>::max() ) );
    }
}
