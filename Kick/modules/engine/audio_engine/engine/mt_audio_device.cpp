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

// The audio device type and device the engine plays through; the build
// system can override them (see CMakeLists.txt).
#ifndef KICK_AUDIO_DEVICE_TYPE
    #define KICK_AUDIO_DEVICE_TYPE "ALSA"
#endif
#ifndef KICK_AUDIO_DEVICE
    #define KICK_AUDIO_DEVICE ""
#endif

namespace mt
{
    namespace
    {
        // The watchdog's period; a device that has not called back for the
        // stall time is closed and opened again, first after the shortest
        // reopen delay and then after twice the previous one, up to the
        // longest. A device that ran for the stable time starts over.
        constexpr int watchdogIntervalMs = 500;
        constexpr int stallTimeMs        = 2000;
        constexpr int minReopenDelayMs   = 1000;
        constexpr int maxReopenDelayMs   = 30000;
        constexpr int stableTimeMs       = 60000;

        void logAudioDevice( const juce::String& Message )
        {
            juce::Logger::writeToLog( "AudioDevice: " + Message );
        }

        // Finds the listed device Wanted names: the name itself, a name that
        // matches it as a wildcard pattern, or one that contains it.
        juce::String findDeviceName( const juce::StringArray& Names, const juce::String& Wanted )
        {
            if( Wanted.isEmpty() )
                return {};

            if( Names.contains( Wanted ) )
                return Wanted;

            for( const auto& name : Names )
                if( name.matchesWildcard( Wanted, true ) )
                    return name;

            for( const auto& name : Names )
                if( name.containsIgnoreCase( Wanted ) )
                    return name;

            return {};
        }

        juce::String defaultDeviceName( juce::AudioIODeviceType& Type, bool Input )
        {
            const auto names = Type.getDeviceNames( Input );
            return names[Type.getDefaultDeviceIndex( Input )];
        }

        // Whether a device is a sound server or the type's default device,
        // which mix or convert what they are given rather than play it on
        // as many hardware channels.
        bool isSharedDevice( juce::AudioIODeviceType& Type, const juce::String& Name )
        {
            return Name == defaultDeviceName( Type, false )
                   || Name.containsIgnoreCase( "default" )
                   || Name.containsIgnoreCase( "pipewire" )
                   || Name.containsIgnoreCase( "pulse" )
                   || Name.containsIgnoreCase( "dmix" );
        }
    } // namespace

    ///
    /// AudioDevice::JuceDeviceManager
    ///

    void AudioDevice::JuceDeviceManager::createAudioDeviceTypes( juce::OwnedArray<juce::AudioIODeviceType>& Types )
    {
        juce::AudioDeviceManager::createAudioDeviceTypes( Types );

        for( int i = 0; i < Types.size(); ++i )
        {
            if( Types[i]->getTypeName() == preferredType )
            {
                Types.move( i, 0 );
                return;
            }
        }

        logAudioDevice( "device type \"" + preferredType + "\" is not available" );
    }

    ///
    /// AudioDevice
    ///

    AudioDevice::AudioDevice( int NumInputs, int NumOutputs, const juce::String& DeviceName )
        : IAudioDevice( (uint32_t)juce::jmax( 0, NumInputs ), (uint32_t)juce::jmax( 0, NumOutputs ) )
        , numInputs( juce::jmax( 0, NumInputs ) )
        , numOutputs( juce::jmax( 0, NumOutputs ) )
        , deviceName( DeviceName.isNotEmpty() ? DeviceName : juce::String( KICK_AUDIO_DEVICE ) )
        , deviceManager( KICK_AUDIO_DEVICE_TYPE )
    {
    }

    AudioDevice::~AudioDevice()
    {
        stop();
    }

    void AudioDevice::start( uint32_t samplerate, uint32_t blocksize )
    {
        if( opened )
            stop();

        // AudIO processes whole blocks of its buffer pool's size.
        blockSize = (int)_buffer.getBlockSize();
        jassert( blockSize > 0 && (uint32_t)blockSize == blocksize );
        if( blockSize <= 0 )
            blockSize = (int)blocksize;

        pendingOutputs.setSize( juce::jmax( 1, numOutputs ), blockSize );
        pendingOutputs.clear();
        fifoPos = 0;

        requestedRate = samplerate;
        sampleRate    = samplerate;
        reopenDelayMs = minReopenDelayMs;

        const auto hasDevice = openDevice( samplerate, (uint32_t)blockSize );
        if( hasDevice )
        {
            if( auto* device = deviceManager.getCurrentAudioDevice() )
            {
                const auto rate = (uint32_t)juce::roundToInt( device->getCurrentSampleRate() );
                if( rate > 0 && rate != samplerate )
                {
                    logAudioDevice( "\"" + device->getName() + "\" runs at " + juce::String( rate )
                                    + " Hz instead of " + juce::String( samplerate ) + " Hz" );
                    sampleRate = rate;
                }
            }
        }

        _buffer.clearInputs();
        _buffer.clearOutputs();
        prepareToPlay( sampleRate, (uint32_t)blockSize );

        setState( AudIO::AudioDeviceState::stopped );
        opened = true;

        // Callbacks run from here on, but play silence until resume().
        deviceManager.addAudioCallback( this );

        if( !hasDevice )
        {
            dummyClock.startThread();
            scheduleReopen();
        }

        startTimer( watchdogIntervalMs );
    }

    void AudioDevice::resume()
    {
        if( opened )
            setState( AudIO::AudioDeviceState::playing );
    }

    void AudioDevice::pause()
    {
        if( opened )
            setState( AudIO::AudioDeviceState::paused );
    }

    void AudioDevice::stop()
    {
        if( !opened )
            return;

        stopTimer();
        setState( AudIO::AudioDeviceState::stopped );
        dummyClock.stopThread( 2000 );
        deviceManager.removeAudioCallback( this );
        deviceManager.closeAudioDevice();
        opened = false;

        _buffer.clearOutputs();
        teardown();
        setState( AudIO::AudioDeviceState::uninitialized );
    }

    bool AudioDevice::openDevice( uint32_t SampleRate, uint32_t BlockSize )
    {
        // Creates and scans the device types, the configured one first.
        const auto& types = deviceManager.getAvailableDeviceTypes();

        juce::AudioIODeviceType* type = nullptr;
        juce::String             output;
        juce::String             input;

        if( deviceName.isNotEmpty() )
        {
            for( auto* t : types )
            {
                output = findDeviceName( t->getDeviceNames( false ), deviceName );
                input  = findDeviceName( t->getDeviceNames( true ), deviceName );

                if( output.isNotEmpty() || input.isNotEmpty() )
                {
                    type = t;
                    break;
                }
            }

            if( type == nullptr )
                logAudioDevice( "no device matches \"" + deviceName + "\", using the default device" );
        }

        juce::String error;

        if( type != nullptr )
        {
            error = openDevice( *type, output, input, SampleRate, BlockSize );

            if( deviceManager.getCurrentAudioDevice() == nullptr )
                logAudioDevice( "cannot open \"" + ( output.isNotEmpty() ? output : input ) + "\": " + error + ", using the default device" );
        }

        // Fall back to the default devices of the first type that has any.
        if( deviceManager.getCurrentAudioDevice() == nullptr )
        {
            type = nullptr;
            for( auto* t : types )
            {
                if( !t->getDeviceNames( false ).isEmpty() || !t->getDeviceNames( true ).isEmpty() )
                {
                    type = t;
                    break;
                }
            }

            if( type != nullptr )
            {
                output = numOutputs > 0 ? defaultDeviceName( *type, false ) : juce::String();
                input  = numInputs > 0 && type->hasSeparateInputsAndOutputs() ? defaultDeviceName( *type, true ) : output;

                error = openDevice( *type, output, input, SampleRate, BlockSize );
            }
        }

        auto* device = deviceManager.getCurrentAudioDevice();
        if( device == nullptr )
        {
            logAudioDevice( "no audio device could be opened" + ( error.isNotEmpty() ? ": " + error : juce::String() ) );
            return false;
        }

        logAudioDevice( "opened " + device->getTypeName() + " device \"" + device->getName() + "\" at "
                        + juce::String( device->getCurrentSampleRate() ) + " Hz, "
                        + juce::String( device->getCurrentBufferSizeSamples() ) + " samples, "
                        + juce::String( device->getActiveInputChannels().countNumberOfSetBits() ) + " in / "
                        + juce::String( device->getActiveOutputChannels().countNumberOfSetBits() ) + " out" );
        return true;
    }

    juce::String AudioDevice::openDevice( juce::AudioIODeviceType& Type, const juce::String& Output, const juce::String& Input, uint32_t SampleRate, uint32_t BlockSize )
    {
        const auto outputs = Output.isNotEmpty() ? numOutputChannelsFor( Type, Output ) : 0;
        const auto inputs  = Input.isNotEmpty() ? numInputs : 0;

        auto error = initialiseDevice( Type, Output, Input, inputs, outputs, SampleRate, BlockSize );

        // A device that cannot record (a busy or missing input) still plays.
        if( deviceManager.getCurrentAudioDevice() == nullptr && inputs > 0 && Output.isNotEmpty() )
        {
            logAudioDevice( "cannot open \"" + Input + "\" for input: " + error + ", opening the output only" );
            error = initialiseDevice( Type, Output, {}, 0, outputs, SampleRate, BlockSize );
        }

        return error;
    }

    juce::String AudioDevice::initialiseDevice( juce::AudioIODeviceType& Type, const juce::String& Output, const juce::String& Input, int Inputs, int Outputs, uint32_t SampleRate, uint32_t BlockSize )
    {
        // initialise() takes the device type only from a saved setup; any
        // other way of choosing it opens the type's default device first.
        juce::BigInteger inputChannels;
        juce::BigInteger outputChannels;
        inputChannels.setRange( 0, Inputs, true );
        outputChannels.setRange( 0, Outputs, true );

        juce::XmlElement state( "DEVICESETUP" );
        state.setAttribute( "deviceType", Type.getTypeName() );
        state.setAttribute( "audioOutputDeviceName", Output );
        state.setAttribute( "audioInputDeviceName", Inputs > 0 ? Input : juce::String() );
        state.setAttribute( "audioDeviceInChans", inputChannels.toString( 2 ) );
        state.setAttribute( "audioDeviceOutChans", outputChannels.toString( 2 ) );

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.sampleRate = (double)SampleRate;
        setup.bufferSize = (int)BlockSize;

        auto error = deviceManager.initialise( Inputs, Outputs, &state, false, juce::String(), &setup );

        if( error.isEmpty() && deviceManager.getCurrentAudioDevice() == nullptr )
            error = "the device did not open";

        return error;
    }

    int AudioDevice::numOutputChannelsFor( juce::AudioIODeviceType& Type, const juce::String& Output ) const
    {
        // The engine's first two outputs are its main mix. The others are
        // only opened on a device that has them: a mixing or converting
        // PCM (ALSA "default", PipeWire, PulseAudio) would take 8 channels
        // and mix them down, so outputs 3-8 would play in the main mix.
        const auto stereo = juce::jmin( 2, numOutputs );
        if( numOutputs <= stereo || isSharedDevice( Type, Output ) )
            return stereo;

        std::unique_ptr<juce::AudioIODevice> probe( Type.createDevice( Output, juce::String() ) );
        const auto channels = probe != nullptr ? probe->getOutputChannelNames().size() : 0;

        // A PCM that converts or mixes reports the most channels it
        // accepts rather than what is behind it (PipeWire 128, PulseAudio
        // 32, plug up to JUCE's limit of 256).
        if( channels <= stereo || channels >= 32 )
            return stereo;

        logAudioDevice( "\"" + Output + "\" has " + juce::String( channels ) + " outputs" );
        return juce::jmin( channels, numOutputs );
    }

    void AudioDevice::audioDeviceAboutToStart( juce::AudioIODevice* Device )
    {
        // The device calls the engine from here on, so the clock must not.
        dummyClock.stopThread( 2000 );

        // Callbacks are not running yet: safe to reset the re-blocking state,
        // and to prepare the engine for a device that runs at another rate.
        const auto rate = Device != nullptr ? (uint32_t)juce::roundToInt( Device->getCurrentSampleRate() ) : 0;
        if( opened && rate > 0 && rate != sampleRate )
        {
            logAudioDevice( "preparing the engine for " + juce::String( rate ) + " Hz instead of " + juce::String( sampleRate ) + " Hz" );
            sampleRate = rate;
            prepareToPlay( sampleRate, (uint32_t)blockSize );
        }

        lastNumCallbacks = numCallbacks.load();
        lastCallbackTime = deviceStartTime = juce::Time::getMillisecondCounterHiRes();
        lastXRunCount    = Device != nullptr ? juce::jmax( 0, Device->getXRunCount() ) : 0;
        stuckLogged      = false;

        const auto deviceBlock = Device != nullptr ? Device->getCurrentBufferSizeSamples() : 0;

        inPlace = blockSize > 0 && deviceBlock > 0 && 0 == ( deviceBlock % blockSize );
        fifoPos = 0;
        pendingOutputs.clear();

        if( !inPlace )
            logAudioDevice( "device buffer of " + juce::String( deviceBlock ) + " samples is re-blocked to "
                            + juce::String( blockSize ) + " samples" );
    }

    void AudioDevice::audioDeviceStopped()
    {
        fifoPos = 0;
    }

    void AudioDevice::audioDeviceError( const juce::String& ErrorMessage )
    {
        logAudioDevice( "device error: " + ErrorMessage );
        deviceError = true;
    }

    void AudioDevice::timerCallback()
    {
        if( !opened )
            return;

        const auto now    = juce::Time::getMillisecondCounterHiRes();
        auto*      device = deviceManager.getCurrentAudioDevice();

        if( device == nullptr )
        {
            if( now >= nextReopenTime )
                reopenDevice();
            return;
        }

        const auto count = numCallbacks.load();
        if( count != lastNumCallbacks )
        {
            lastNumCallbacks = count;
            lastCallbackTime = now;
        }

        if( deviceError.exchange( false ) )
        {
            closeLostDevice( "it reported an error" );
            return;
        }

        if( !device->isPlaying() )
        {
            closeLostDevice( "it stopped" );
            return;
        }

        if( now - lastCallbackTime > stallTimeMs )
        {
            // Closing the device would kill a thread that is stuck in the
            // engine, not one that waits for the device.
            if( inCallback )
            {
                if( !stuckLogged )
                    logAudioDevice( "the audio callback has not returned for " + juce::String( juce::roundToInt( now - lastCallbackTime ) ) + " ms" );
                stuckLogged = true;
                return;
            }

            closeLostDevice( "it has not called back for " + juce::String( juce::roundToInt( now - lastCallbackTime ) ) + " ms" );
            return;
        }

        const auto xruns = juce::jmax( 0, device->getXRunCount() );
        if( xruns != lastXRunCount )
        {
            logAudioDevice( juce::String( xruns - lastXRunCount ) + " xrun(s), " + juce::String( xruns ) + " since the device opened" );
            lastXRunCount = xruns;
        }
    }

    void AudioDevice::closeLostDevice( const juce::String& Reason )
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        if( now - deviceStartTime >= stableTimeMs )
            reopenDelayMs = minReopenDelayMs;

        auto* device = deviceManager.getCurrentAudioDevice();
        logAudioDevice( "closing \"" + ( device != nullptr ? device->getName() : juce::String() ) + "\": " + Reason
                        + "; opening it again in " + juce::String( reopenDelayMs / 1000.0, 1 ) + " s" );

        // The callback stays registered with the device manager, and is
        // called again once a device opens.
        deviceManager.closeAudioDevice();
        deviceError = false;

        dummyClock.startThread();
        scheduleReopen();
    }

    void AudioDevice::reopenDevice()
    {
        dummyClock.stopThread( 2000 );

        if( openDevice( requestedRate, (uint32_t)blockSize ) )
            return;

        dummyClock.startThread();
        scheduleReopen();
    }

    void AudioDevice::scheduleReopen()
    {
        nextReopenTime = juce::Time::getMillisecondCounterHiRes() + reopenDelayMs;
        reopenDelayMs  = juce::jmin( reopenDelayMs * 2, maxReopenDelayMs );
    }

    void AudioDevice::processWithoutDevice()
    {
        if( !isPlaying() || blockSize <= 0 )
            return;

        juce::ScopedNoDenormals noDenormals;

        _buffer.clearInputs();
        process( _buffer );
    }

    void AudioDevice::DummyClock::run()
    {
        const auto period = 1000.0 * owner.blockSize / juce::jmax( 1u, owner.sampleRate );
        auto       next   = juce::Time::getMillisecondCounterHiRes();

        while( !threadShouldExit() )
        {
            owner.processWithoutDevice();

            next += period;
            const auto now = juce::Time::getMillisecondCounterHiRes();

            // After a long stall, carry on from now rather than catch up.
            if( next < now - 100 * period )
                next = now;

            if( next > now )
                wait( next - now );
        }
    }

    void AudioDevice::readInputs( const float* const* InputData, int NumInputChannels, int SrcOffset, int DstOffset, int NumSamples )
    {
        for( int ch = 0; ch < numInputs; ++ch )
        {
            if( auto dst = _buffer.getInputBuffer( (uint32_t)ch ) )
            {
                if( ch < NumInputChannels && InputData[ch] != nullptr )
                    juce::FloatVectorOperations::copy( dst + DstOffset, InputData[ch] + SrcOffset, NumSamples );
                else
                    juce::FloatVectorOperations::clear( dst + DstOffset, NumSamples );
            }
        }
    }

    void AudioDevice::audioDeviceIOCallbackWithContext( const float* const* InputData, int NumInputChannels, float* const* OutputData, int NumOutputChannels, int NumSamples, const juce::AudioIODeviceCallbackContext& Context )
    {
        juce::ignoreUnused( Context );
        juce::ScopedNoDenormals noDenormals;

        numCallbacks.fetch_add( 1, std::memory_order_relaxed );

        if( !isPlaying() || blockSize <= 0 )
        {
            for( int ch = 0; ch < NumOutputChannels; ++ch )
                if( OutputData[ch] != nullptr )
                    juce::FloatVectorOperations::clear( OutputData[ch], NumSamples );
            return;
        }

        inCallback.store( true, std::memory_order_relaxed );

        int done = 0;
        while( done < NumSamples )
        {
            if( inPlace && 0 == fifoPos && NumSamples - done >= blockSize )
            {
                // A whole block lines up with the device buffer.
                readInputs( InputData, NumInputChannels, done, 0, blockSize );
                process( _buffer );

                for( int ch = 0; ch < NumOutputChannels; ++ch )
                {
                    if( OutputData[ch] == nullptr )
                        continue;

                    if( auto src = ( ch < numOutputs ) ? _buffer.getOutputBuffer( (uint32_t)ch ) : nullptr )
                        juce::FloatVectorOperations::copy( OutputData[ch] + done, src, blockSize );
                    else
                        juce::FloatVectorOperations::clear( OutputData[ch] + done, blockSize );
                }

                done += blockSize;
                continue;
            }

            // Re-block: collect input up to a whole block, and play the
            // previous block's output meanwhile.
            const int count = juce::jmin( blockSize - fifoPos, NumSamples - done );

            readInputs( InputData, NumInputChannels, done, fifoPos, count );

            for( int ch = 0; ch < NumOutputChannels; ++ch )
            {
                if( OutputData[ch] == nullptr )
                    continue;

                if( ch < numOutputs )
                    juce::FloatVectorOperations::copy( OutputData[ch] + done, pendingOutputs.getReadPointer( ch, fifoPos ), count );
                else
                    juce::FloatVectorOperations::clear( OutputData[ch] + done, count );
            }

            fifoPos += count;
            done += count;

            if( fifoPos == blockSize )
            {
                process( _buffer );

                for( int ch = 0; ch < numOutputs; ++ch )
                {
                    if( auto src = _buffer.getOutputBuffer( (uint32_t)ch ) )
                        pendingOutputs.copyFrom( ch, 0, src, blockSize );
                    else
                        pendingOutputs.clear( ch, 0, blockSize );
                }

                fifoPos = 0;
            }
        }

        inCallback.store( false, std::memory_order_relaxed );
    }
} // namespace mt
