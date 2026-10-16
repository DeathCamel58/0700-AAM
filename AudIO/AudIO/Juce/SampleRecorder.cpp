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

#include "SampleRecorder.h"

namespace AudIO
{
    // Layout of the buffer: once a recording is finalized it occupies
    // [0, recorded_). While a REVERSE recording is made it grows from the end
    // of the buffer down, occupying [capacity - recorded_, capacity), and it
    // is moved to the start when it is finalized.

    SampleRecorder::SampleRecorder( Design& D, const std::string& Name )
        : Recorder( D, Name )
    {
        const juce::SpinLock::ScopedLockType sl( lock_ );
        updateViewLocked();
    }

    //
    // Control...
    //

    void SampleRecorder::reset()
    {
        uint32_t events = 0;
        {
            const juce::SpinLock::ScopedLockType sl( lock_ );
            if( recording_.load() || paused_.load() )
                events |= EV_STOPPED;
            if( previewing_.load() )
                events |= EV_PREVIEW_STOPPED;
            buffer_.clear();
            bufferResized();
        }
        sendEvent( events );
    }

    void SampleRecorder::bufferResized()
    {
        recording_.store( false );
        paused_.store( false );
        capturing_.store( false );
        previewing_.store( false );
        finalizePending_.store( false );
        progress_.store( 0.f );
        recorded_.store( 0 );

        writePos_  = 0;
        readPos_   = 0;
        wrapped_   = false;
        finalized_ = true;
        reverse_   = false;
        updateViewLocked();
    }

    void SampleRecorder::startRecording( bool FromBeginning )
    {
        finalizeIfPending();

        uint32_t events = 0;
        {
            const juce::SpinLock::ScopedLockType sl( lock_ );
            const auto                           capacity = buffer_.getNumSamples();
            if( 0 == capacity )
                return;

            if( previewing_.exchange( false ) )
                events |= EV_PREVIEW_STOPPED;

            const auto resume = !FromBeginning && ( 0 < recorded_.load() );
            if( !resume )
            {
                reverse_     = isReverse();
                numChannels_ = isStereoMode( getRecordingMode() ) ? 2 : 1;
                wrapped_     = false;
                if( !isOverdubbing() )
                {
                    buffer_.clear();
                    recorded_.store( 0 );
                }
                // Overdub a reversed recording where it was recorded.
                if( reverse_ && 0 < recorded_.load() )
                    moveRecordingLocked( true );
                writePos_ = reverse_ ? capacity : 0;
            }
            else
            {
                // Carry on from the end of the recording, in its direction.
                if( reverse_ )
                {
                    if( finalized_ )
                        moveRecordingLocked( true );
                    writePos_ = capacity - recorded_.load();
                }
                else
                {
                    writePos_ = wrapped_ ? writePos_ : recorded_.load();
                }
            }

            finalized_ = false;
            capturing_.store( resume );
            paused_.store( false );
            recording_.store( true );
            progress_.store( (float)( reverse_ ? capacity - writePos_ : writePos_ ) / (float)capacity );
            updateViewLocked();
        }
        events |= FromBeginning ? EV_STARTED : EV_RESUMED;
        sendEvent( events );
    }

    void SampleRecorder::stopRecording()
    {
        finalizeIfPending();
        {
            const juce::SpinLock::ScopedLockType sl( lock_ );
            if( !recording_.load() && !paused_.load() )
                return;
            recording_.store( false );
            paused_.store( false );
            finalizeLocked();
        }
        sendEvent( EV_STOPPED | EV_LENGTH_CHANGED );
    }

    void SampleRecorder::pauseRecording()
    {
        {
            const juce::SpinLock::ScopedLockType sl( lock_ );
            if( !recording_.load() )
                return;
            recording_.store( false );
            paused_.store( true );
        }
        sendEvent( EV_PAUSED );
    }

    void SampleRecorder::resumeRecording()
    {
        {
            const juce::SpinLock::ScopedLockType sl( lock_ );
            if( !paused_.load() )
                return;
            paused_.store( false );
            recording_.store( true );
        }
        sendEvent( EV_RESUMED );
    }

    void SampleRecorder::stop()
    {
        stopRecording();
        stopPreview();
    }

    void SampleRecorder::startPreview( double FromStartTimeInSeconds )
    {
        finalizeIfPending();
        {
            const juce::SpinLock::ScopedLockType sl( lock_ );
            const auto                           length = recorded_.load();
            if( recording_.load() || paused_.load() || 0 >= length )
                return;
            readPos_ = utils::clamp( samplesFor( utils::max( 0.0, FromStartTimeInSeconds ) ), 0, length - 1 );
            previewing_.store( true );
        }
        sendEvent( EV_PREVIEW_STARTED );
    }

    void SampleRecorder::stopPreview()
    {
        if( previewing_.exchange( false ) )
            sendEvent( EV_PREVIEW_STOPPED );
    }

    bool SampleRecorder::saveRecording( const std::string& FilePath )
    {
        stopRecording();
        stopPreview();
        finalizeIfPending();

        const juce::SpinLock::ScopedLockType sl( lock_ );
        return writeWavFile( juce::String( FilePath ), buffer_.getArrayOfReadPointers(), numChannels_, recorded_.load(), getSampleRate() );
    }

    //
    // Getters...
    //

    double SampleRecorder::getRecordedLength() const noexcept
    {
        return (double)recorded_.load() / getSampleRate();
    }

    double SampleRecorder::reportedLength() const
    {
        return ( 0 < recorded_.load() && !recording_.load() ) ? getRecordedLength() : getLength();
    }

    juce::AudioBuffer<float>& SampleRecorder::getAudioBuffer()
    {
        finalizeIfPending();
        return view_;
    }

    //
    // Private...
    //

    void SampleRecorder::moveRecordingLocked( bool ToEnd )
    {
        const auto capacity = buffer_.getNumSamples();
        const auto length   = recorded_.load();
        if( 0 >= length || length >= capacity )
            return;

        const auto from = ToEnd ? 0 : capacity - length;
        const auto to   = ToEnd ? capacity - length : 0;
        for( int ch = 0; ch < buffer_.getNumChannels(); ++ch )
        {
            auto data = buffer_.getWritePointer( ch );
            std::memmove( data + to, data + from, (size_t)length * sizeof( float ) );
        }
        if( ToEnd )
            buffer_.clear( 0, capacity - length );
        else
            buffer_.clear( length, capacity - length );
    }

    void SampleRecorder::finalizeLocked()
    {
        finalizePending_.store( false );
        if( finalized_ )
            return;

        const auto capacity = buffer_.getNumSamples();
        if( wrapped_ )
            recorded_.store( capacity );
        else if( reverse_ )
            moveRecordingLocked( false );

        finalized_ = true;
        capturing_.store( false );
        progress_.store( ( 0 < capacity ) ? (float)recorded_.load() / (float)capacity : 0.f );
        updateViewLocked();
    }

    void SampleRecorder::finalizeIfPending()
    {
        if( finalizePending_.load() )
        {
            const juce::SpinLock::ScopedLockType sl( lock_ );
            finalizeLocked();
        }
    }

    void SampleRecorder::updateViewLocked()
    {
        const auto capacity   = buffer_.getNumSamples();
        const auto length     = recorded_.load();
        const auto numSamples = ( finalized_ && 0 < length ) ? length : capacity;

        if( 0 < numSamples && numChannels_ <= buffer_.getNumChannels() )
            view_.setDataToReferTo( buffer_.getArrayOfWritePointers(), numChannels_, numSamples );
        else
            view_.setSize( numChannels_, 0 );
    }

    void SampleRecorder::dispatchEvents( uint32_t Events )
    {
        finalizeIfPending();
        Recorder::dispatchEvents( Events );
    }

    void SampleRecorder::processRecorder( const float* const* Source, float* const* Out, int NumSamples )
    {
        const auto capacity = buffer_.getNumSamples();

        // Gain and pan of the recorded signal; a mono recording takes the gain only.
        float targetL, targetR;
        getGainAndPanTargets( targetL, targetR );
        if( 1 == numChannels_ )
            targetL = targetR = getGain();
        const auto snap = consumeGainSnap();
        gainL_.begin( targetL, NumSamples, snap );
        gainR_.begin( targetR, NumSamples, snap );

        if( recording_.load() && 0 < capacity )
        {
            if( !capturing_.load() && thresholdReached( Source, NumSamples ) )
                capturing_.store( true );

            if( capturing_.load() )
            {
                auto       d0      = buffer_.getWritePointer( 0 );
                auto       d1      = buffer_.getWritePointer( 1 );
                const auto overdub = isOverdubbing();
                const auto looping = isLooping();
                auto       full    = false;

                for( int i = 0; i < NumSamples; ++i )
                {
                    const auto l = Source[0][i] * gainL_.next();
                    const auto r = Source[1][i] * gainR_.next();

                    int pos;
                    if( !reverse_ )
                    {
                        if( capacity <= writePos_ )
                        {
                            if( !looping )
                            {
                                full = true;
                                break;
                            }
                            writePos_ = 0;
                            wrapped_  = true;
                        }
                        pos = writePos_++;
                    }
                    else
                    {
                        if( 0 >= writePos_ )
                        {
                            if( !looping )
                            {
                                full = true;
                                break;
                            }
                            writePos_ = capacity;
                            wrapped_  = true;
                        }
                        pos = --writePos_;
                    }

                    if( overdub )
                    {
                        d0[pos] += l;
                        d1[pos] += r;
                    }
                    else
                    {
                        d0[pos] = l;
                        d1[pos] = r;
                    }
                }

                const auto done = reverse_ ? ( capacity - writePos_ ) : writePos_;
                recorded_.store( wrapped_ ? capacity : utils::max( recorded_.load(), done ) );
                full = full || ( !looping && capacity <= done );
                progress_.store( full ? 1.f : (float)done / (float)capacity );

                if( full )
                {
                    // Buffer full: stop, and let the message thread finalize.
                    recording_.store( false );
                    finalizePending_.store( true );
                    postEvent( EV_STOPPED | EV_LENGTH_CHANGED );
                }
            }
        }
        gainL_.end( targetL );
        gainR_.end( targetR );

        // Preview...
        if( previewing_.load() && finalized_ && !recording_.load() )
        {
            const auto length = recorded_.load();
            const auto s0     = buffer_.getReadPointer( 0 );
            const auto s1     = buffer_.getReadPointer( ( 2 == numChannels_ ) ? 1 : 0 );

            for( int i = 0; i < NumSamples; ++i )
            {
                if( length <= readPos_ )
                {
                    if( !isLooping() || 0 >= length )
                    {
                        previewing_.store( false );
                        readPos_ = 0;
                        postEvent( EV_PREVIEW_STOPPED );
                        break;
                    }
                    readPos_ = 0;
                }
                Out[0][i] += s0[readPos_];
                Out[1][i] += s1[readPos_];
                ++readPos_;
            }
        }
    }

} // namespace AudIO
