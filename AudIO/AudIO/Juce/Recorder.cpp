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

#include "Recorder.h"

namespace AudIO
{
    Recorder::Recorder( Design& D, const std::string& Name )
        : Object( 2, 2, 0 )
        , DawRouting( D, BusType::Object, BusWidth::Stereo )
    {
        setName( Name );
        DawRouting::setID( getId() );
        buffer_.setSize( 2, 0 );
    }

    Recorder::~Recorder()
    {
        cancelPendingUpdate();
    }

    //
    // Settings...
    //

    void Recorder::setMonitorGain( float Gain ) noexcept
    {
        monitorGain_.store( utils::clamp( Gain, 0.f, 1.f ) );
    }

    void Recorder::setGain( float Gain ) noexcept
    {
        gain_.store( utils::clamp( Gain, 0.f, utils::logToLinear( MAX_GAIN_DB ) ) );
    }

    void Recorder::setGainDB( float GainDB ) noexcept
    {
        setGain( ( GainDB <= MIN_DB ) ? 0.f : utils::logToLinear( GainDB ) );
    }

    void Recorder::setPan( float Pan ) noexcept
    {
        pan_.store( utils::clamp( Pan, -1.f, 1.f ) );
    }

    void Recorder::setGainAndPan( float Gain, float Pan, bool Force ) noexcept
    {
        setGain( Gain );
        setPan( Pan );
        if( Force )
            snapGain_.store( true );
    }

    void Recorder::setThreshold( float Gain ) noexcept
    {
        threshold_.store( utils::clamp( Gain, 0.f, 1.f ) );
    }

    void Recorder::setThresholdDB( float DB ) noexcept
    {
        setThreshold( ( DB <= MIN_DB ) ? 0.f : utils::logToLinear( DB ) );
    }

    void Recorder::setRecordingMode( Mode M ) noexcept
    {
        mode_.store( (Mode)utils::clamp<int>( (int)M, (int)L_MONO, (int)LR_STEREO ) );
    }

    void Recorder::getGainAndPanTargets( float& L, float& R ) const noexcept
    {
        // Balance law: the far side is attenuated, the near side kept at unity.
        const auto gain = gain_.load();
        const auto pan  = pan_.load();
        L               = gain * ( ( 0.f < pan ) ? ( 1.f - pan ) : 1.f );
        R               = gain * ( ( 0.f > pan ) ? ( 1.f + pan ) : 1.f );
    }

    bool Recorder::thresholdReached( const float* const* Source, int NumSamples ) const noexcept
    {
        const auto threshold = threshold_.load();
        if( 0.f >= threshold )
            return true;

        for( int ch = 0; ch < 2; ++ch )
        {
            const auto range = juce::FloatVectorOperations::findMinAndMax( Source[ch], NumSamples );
            if( threshold <= utils::max( -range.getStart(), range.getEnd() ) )
                return true;
        }
        return false;
    }

    //
    // Buffer...
    //

    int Recorder::samplesFor( double Seconds ) const noexcept
    {
        const auto samples = std::llround( Seconds * getSampleRate() );
        return (int)utils::clamp<long long>( samples, 0, std::numeric_limits<int>::max() );
    }

    void Recorder::setLength( double TimeInSeconds )
    {
        lengthSeconds_.store( utils::clamp( TimeInSeconds, 0.0, MAX_LENGTH_SECONDS ) );
        allocateBuffer( keepContentOnResize_ );
        sendEvent( EV_LENGTH_CHANGED );
    }

    void Recorder::allocateBuffer( bool KeepContent )
    {
        const auto numSamples = samplesFor( lengthSeconds_.load() );

        const juce::SpinLock::ScopedLockType sl( lock_ );
        if( numSamples != buffer_.getNumSamples() || 2 != buffer_.getNumChannels() )
            buffer_.setSize( 2, numSamples, KeepContent, true, false );
        if( !KeepContent )
            buffer_.clear();
        bufferResized();
    }

    bool Recorder::writeWavFile( const juce::String& Path, const float* const* Channels, int NumChannels, int NumSamples, double SampleRate )
    {
        if( Path.isEmpty() || 0 >= NumChannels || 0 >= NumSamples || 0.0 >= SampleRate )
            return false;

        auto file = juce::File::getCurrentWorkingDirectory().getChildFile( Path );
        if( !file.getParentDirectory().createDirectory() )
            return false;
        file.deleteFile();

        std::unique_ptr<juce::OutputStream> stream( file.createOutputStream() );
        if( !stream )
            return false;

        juce::WavAudioFormat                     wav;
        std::unique_ptr<juce::AudioFormatWriter> writer( wav.createWriterFor( stream.get(), SampleRate, (unsigned int)NumChannels, 24, {}, 0 ) );
        if( !writer )
            return false;
        stream.release(); // Owned by the writer now.

        return writer->writeFromFloatArrays( Channels, NumChannels, NumSamples );
    }

    //
    // Events...
    //

    void Recorder::postEvent( uint32_t Events ) noexcept
    {
        pendingEvents_.fetch_or( Events );
        triggerAsyncUpdate();
    }

    void Recorder::handleAsyncUpdate()
    {
        if( auto events = pendingEvents_.exchange( 0 ) )
            dispatchEvents( events );
    }

    void Recorder::dispatchEvents( uint32_t Events )
    {
        auto call = []( const std::function<void()>& F ) {
            if( F )
                F();
        };

        if( Events & EV_LENGTH_CHANGED )
            if( onRecorderLengthChanged )
                onRecorderLengthChanged( reportedLength() );
        if( Events & EV_STARTED )
            call( onRecorderStarted );
        if( Events & EV_RESUMED )
            call( onRecorderResumed );
        if( Events & EV_PAUSED )
            call( onRecorderPaused );
        if( Events & EV_STOPPED )
            call( onRecorderStopped );
        if( Events & EV_PREVIEW_STARTED )
            call( onRecorderPreviewStarted );
        if( Events & EV_PREVIEW_STOPPED )
            call( onRecorderPreviewStopped );
    }

    //
    // Processing...
    //

    void Recorder::prepareToPlay( uint32_t Samplerate, uint32_t Blocksize )
    {
        const auto rateChanged = ( Samplerate != sampleRate() );

        Object::prepareToPlay( Samplerate, Blocksize );
        DawInserts::prepareToPlay( Samplerate, Blocksize );

        source_.setSize( 2, (int)Blocksize, false, true, true );
        spare_.setSize( 1, (int)Blocksize, false, true, true );

        // The buffer holds a length in seconds; keep that length at the new rate.
        if( rateChanged )
            allocateBuffer( true );
    }

    void Recorder::process( Buffer<float>& buffer )
    {
        const juce::ScopedNoDenormals noDenormals;

        meter_.processIn( buffer );
        buffer.clearOutputs();

        const auto numSamples = (int)buffer.getBlockSize();
        float*     out[2]     = { buffer.getOutputBuffer( 0 ), buffer.getOutputBuffer( 1 ) };

        if( !enabled_.load() || 0 >= numSamples || numSamples > source_.getNumSamples() || nullptr == out[0] )
        {
            meter_.processOut( buffer );
            return;
        }
        if( nullptr == out[1] )
        {
            out[1] = spare_.getWritePointer( 0 );
            juce::FloatVectorOperations::clear( out[1], numSamples );
        }

        // Select the recorded signal. An input that is not patched is silent.
        float* src[2] = { source_.getWritePointer( 0 ), source_.getWritePointer( 1 ) };
        for( uint32_t ch = 0; ch < 2; ++ch )
        {
            if( auto in = buffer.getInputBuffer( ch ) )
                juce::FloatVectorOperations::copy( src[ch], in, numSamples );
            else
                juce::FloatVectorOperations::clear( src[ch], numSamples );
        }

        switch( mode_.load() )
        {
            case L_MONO:
            case L_STEREO:
                juce::FloatVectorOperations::copy( src[1], src[0], numSamples );
                break;
            case R_MONO:
            case R_STEREO:
                juce::FloatVectorOperations::copy( src[0], src[1], numSamples );
                break;
            case LR_MONO:
                juce::FloatVectorOperations::add( src[0], src[1], numSamples );
                juce::FloatVectorOperations::multiply( src[0], 0.5f, numSamples );
                juce::FloatVectorOperations::copy( src[1], src[0], numSamples );
                break;
            case LR_STEREO:
            default:
                break;
        }

        // Monitor...
        const auto monitorTarget = monitor_.load() ? monitorGain_.load() : 0.f;
        if( !monitorRamp_.isSilent( monitorTarget ) )
        {
            monitorRamp_.begin( monitorTarget, numSamples, false );
            for( int i = 0; i < numSamples; ++i )
            {
                const auto g = monitorRamp_.next();
                out[0][i]    = src[0][i] * g;
                out[1][i]    = src[1][i] * g;
            }
        }
        monitorRamp_.end( monitorTarget );

        // Record and play. Never wait for a control thread.
        {
            const juce::SpinLock::ScopedTryLockType sl( lock_ );
            if( sl.isLocked() )
                processRecorder( src, out, numSamples );
        }

        DawInserts::process( buffer );
        meter_.processOut( buffer );
    }

} // namespace AudIO
