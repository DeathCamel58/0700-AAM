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

#include "Looper.h"

namespace AudIO
{
    Looper::Looper( Design& D, const std::string& Name )
        : Recorder( D, Name )
    {
        // Changing the number of bars keeps what has been recorded.
        keepContentOnResize_ = true;
        setLooping( true );
        undo_.setSize( 2, 0 );
    }

    //
    // Modes...
    //

    void Looper::setRecord( bool Record )
    {
        const juce::SpinLock::ScopedLockType sl( lock_ );
        if( Record )
        {
            overdubMode_.store( false );
            record_.store( true );
            passWritten_ = 0;
            // Already running: punch in now.
            if( running_.load() )
                beginPassLocked( true );
        }
        else if( record_.exchange( false ) )
        {
            endPassLocked();
        }
    }

    void Looper::setOverdub( bool Overdub )
    {
        const juce::SpinLock::ScopedLockType sl( lock_ );
        if( Overdub )
        {
            record_.store( false );
            overdubMode_.store( true );
            if( running_.load() )
                beginPassLocked( true );
        }
        else if( overdubMode_.exchange( false ) )
        {
            endPassLocked();
        }
    }

    void Looper::setPlay()
    {
        const juce::SpinLock::ScopedLockType sl( lock_ );
        record_.store( false );
        overdubMode_.store( false );
        endPassLocked();
        if( !empty_.load() && 0 < buffer_.getNumSamples() )
            running_.store( true );
    }

    //
    // Transport...
    //

    void Looper::start( bool FromBeginning, bool SkipThresholdDetection )
    {
        {
            const juce::SpinLock::ScopedLockType sl( lock_ );
            const auto                           length = buffer_.getNumSamples();
            if( 0 == length )
                return;

            if( FromBeginning )
                setHeadLocked( isReverse() ? length - 1 : 0 );
            if( record_.load() || overdubMode_.load() )
            {
                passWritten_ = 0;
                beginPassLocked( SkipThresholdDetection );
            }

            if( !pass_.load() && empty_.load() )
                return; // Nothing to record or play.
            running_.store( true );
        }
        sendEvent( EV_STARTED );
    }

    void Looper::play( double FromTimeInSeconds )
    {
        {
            const juce::SpinLock::ScopedLockType sl( lock_ );
            record_.store( false );
            overdubMode_.store( false );
            endPassLocked();
            if( empty_.load() || 0 == buffer_.getNumSamples() )
                return;
            setHeadLocked( samplesFor( utils::max( 0.0, FromTimeInSeconds ) ) );
            running_.store( true );
        }
        sendEvent( EV_PREVIEW_STARTED );
    }

    void Looper::stop()
    {
        bool wasRunning;
        {
            const juce::SpinLock::ScopedLockType sl( lock_ );
            wasRunning = running_.exchange( false );
            record_.store( false );
            overdubMode_.store( false );
            endPassLocked();
            setHeadLocked( isReverse() ? buffer_.getNumSamples() - 1 : 0 );
        }
        if( wasRunning )
            sendEvent( EV_STOPPED );
    }

    void Looper::clear()
    {
        stop();

        const juce::SpinLock::ScopedLockType sl( lock_ );
        buffer_.clear();
        undo_.clear();
        empty_.store( true );
        undoEmpty_ = true;
        hasUndo_.store( false );
        hasRedo_.store( false );
        setHeadLocked( 0 );
    }

    void Looper::undo()
    {
        const juce::SpinLock::ScopedLockType sl( lock_ );
        if( !hasUndo_.load() )
            return;

        record_.store( false );
        overdubMode_.store( false );
        endPassLocked();

        std::swap( buffer_, undo_ );
        const auto wasEmpty = empty_.load();
        empty_.store( undoEmpty_ );
        undoEmpty_ = wasEmpty;
        hasUndo_.store( false );
        hasRedo_.store( true );

        if( empty_.load() )
            running_.store( false );
    }

    void Looper::redo()
    {
        const juce::SpinLock::ScopedLockType sl( lock_ );
        if( !hasRedo_.load() )
            return;

        record_.store( false );
        overdubMode_.store( false );
        endPassLocked();

        std::swap( buffer_, undo_ );
        const auto wasEmpty = empty_.load();
        empty_.store( undoEmpty_ );
        undoEmpty_ = wasEmpty;
        hasRedo_.store( false );
        hasUndo_.store( true );

        if( empty_.load() )
            running_.store( false );
    }

    bool Looper::save( const juce::String& FilePath )
    {
        // The audio thread skips the looper while the lock is held; Kick
        // stops the looper before it saves.
        const juce::SpinLock::ScopedLockType sl( lock_ );
        if( empty_.load() )
            return false;
        return writeWavFile( FilePath, buffer_.getArrayOfReadPointers(), 2, buffer_.getNumSamples(), getSampleRate() );
    }

    //
    // Private...
    //

    void Looper::beginPassLocked( bool SkipThreshold )
    {
        if( !pass_.load() )
        {
            // Keep the loop as it is now, for undo. The two buffers have the
            // same size, so this copies without allocating.
            undo_.makeCopyOf( buffer_, true );
            undoEmpty_ = empty_.load();
            hasUndo_.store( true );
            hasRedo_.store( false );
            pass_.store( true );
        }
        armed_.store( !SkipThreshold && 0.f < getThreshold() );
    }

    void Looper::endPassLocked()
    {
        pass_.store( false );
        armed_.store( false );
        passWritten_ = 0;
    }

    void Looper::setHeadLocked( int Position )
    {
        const auto length = buffer_.getNumSamples();
        head_             = utils::clamp( Position, 0, utils::max( length - 1, 0 ) );
        progress_         = ( 0 < length ) ? (double)head_ / (double)length : 0.0;
    }

    void Looper::bufferResized()
    {
        const auto length = buffer_.getNumSamples();
        undo_.setSize( 2, length, true, true, false );
        if( 0 == length )
        {
            running_.store( false );
            record_.store( false );
            overdubMode_.store( false );
            endPassLocked();
            empty_.store( true );
            undoEmpty_ = true;
            hasUndo_.store( false );
            hasRedo_.store( false );
        }
        setHeadLocked( head_ );
    }

    void Looper::processRecorder( const float* const* Source, float* const* Out, int NumSamples )
    {
        const auto length = buffer_.getNumSamples();

        float targetL, targetR;
        getGainAndPanTargets( targetL, targetR );
        const auto snap = consumeGainSnap();
        gainL_.begin( targetL, NumSamples, snap );
        gainR_.begin( targetR, NumSamples, snap );

        if( running_.load() && 0 < length )
        {
            const auto replace = record_.load();
            auto       writing = pass_.load() && ( replace || overdubMode_.load() );

            if( writing && armed_.load() && thresholdReached( Source, NumSamples ) )
                armed_.store( false );
            writing = writing && !armed_.load();

            // An empty loop waits for the input before it starts.
            if( !( armed_.load() && empty_.load() ) )
            {
                auto       d0      = buffer_.getWritePointer( 0 );
                auto       d1      = buffer_.getWritePointer( 1 );
                const auto reverse = isReverse();
                auto       wrote   = false;

                for( int i = 0; i < NumSamples; ++i )
                {
                    const auto gl  = gainL_.next();
                    const auto gr  = gainR_.next();
                    const auto pos = head_;

                    // What is being replaced is not heard; what is overdubbed is.
                    if( !( writing && replace ) )
                    {
                        Out[0][i] += d0[pos] * gl;
                        Out[1][i] += d1[pos] * gr;
                    }

                    if( writing )
                    {
                        if( replace )
                        {
                            d0[pos] = Source[0][i];
                            d1[pos] = Source[1][i];
                            // One whole loop replaced: play it from now on.
                            if( length <= ++passWritten_ )
                            {
                                record_.store( false );
                                endPassLocked();
                                writing = false;
                            }
                        }
                        else
                        {
                            d0[pos] += Source[0][i];
                            d1[pos] += Source[1][i];
                        }
                        wrote = true;
                    }

                    auto wrapped = false;
                    if( !reverse )
                    {
                        if( length <= ++head_ )
                        {
                            head_   = 0;
                            wrapped = true;
                        }
                    }
                    else if( 0 > --head_ )
                    {
                        head_   = length - 1;
                        wrapped = true;
                    }

                    if( wrapped && !isLooping() )
                    {
                        running_.store( false );
                        record_.store( false );
                        overdubMode_.store( false );
                        endPassLocked();
                        postEvent( EV_STOPPED );
                        break;
                    }
                }

                if( wrote )
                    empty_.store( false );
            }
            progress_ = (double)head_ / (double)length;
        }

        gainL_.end( targetL );
        gainR_.end( targetR );
    }

} // namespace AudIO
