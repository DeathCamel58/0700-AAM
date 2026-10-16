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

        auto sampleRate = samplerate;
        if( openDevice( samplerate, (uint32_t)blockSize ) )
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

        setState( AudIO::AudioDeviceState::stopped );
        deviceManager.removeAudioCallback( this );
        deviceManager.closeAudioDevice();
        opened = false;

        _buffer.clearOutputs();
        teardown();
        setState( AudIO::AudioDeviceState::uninitialized );
    }

    bool AudioDevice::openDevice( uint32_t SampleRate, uint32_t BlockSize )
    {
        // Creates the device types, the configured one first.
        deviceManager.getAvailableDeviceTypes();

        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.sampleRate               = (double)SampleRate;
        setup.bufferSize               = (int)BlockSize;
        setup.useDefaultInputChannels  = false;
        setup.useDefaultOutputChannels = false;
        setup.inputChannels.setRange( 0, numInputs, true );
        setup.outputChannels.setRange( 0, numOutputs, true );

        // initialise() picks the first type that has devices: the configured
        // one when it has any. Look the device up in the types in that order.
        if( deviceName.isNotEmpty() )
        {
            for( auto* type : deviceManager.getAvailableDeviceTypes() )
            {
                const auto output = findDeviceName( type->getDeviceNames( false ), deviceName );
                const auto input  = findDeviceName( type->getDeviceNames( true ), deviceName );

                if( output.isNotEmpty() || input.isNotEmpty() )
                {
                    setup.outputDeviceName = output;
                    setup.inputDeviceName  = input;
                    break;
                }
            }

            if( setup.outputDeviceName.isEmpty() && setup.inputDeviceName.isEmpty() )
                logAudioDevice( "no device matches \"" + deviceName + "\", using the default device" );
        }

        auto error = deviceManager.initialise( numInputs, numOutputs, nullptr, false, juce::String(), &setup );

        if( deviceManager.getCurrentAudioDevice() == nullptr )
        {
            if( error.isNotEmpty() )
                logAudioDevice( "cannot open the device: " + error );

            // Fall back to the default device of the type initialise() picked.
            if( auto* type = deviceManager.getCurrentDeviceTypeObject() )
            {
                type->scanForDevices();
                setup.outputDeviceName = numOutputs > 0 ? defaultDeviceName( *type, false ) : juce::String();
                setup.inputDeviceName  = numInputs > 0 && type->hasSeparateInputsAndOutputs() ? defaultDeviceName( *type, true ) : setup.outputDeviceName;

                error = deviceManager.setAudioDeviceSetup( setup, false );
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

    void AudioDevice::audioDeviceAboutToStart( juce::AudioIODevice* Device )
    {
        // Callbacks are not running yet: safe to reset the re-blocking state.
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

        if( !isPlaying() || blockSize <= 0 )
        {
            for( int ch = 0; ch < NumOutputChannels; ++ch )
                if( OutputData[ch] != nullptr )
                    juce::FloatVectorOperations::clear( OutputData[ch], NumSamples );
            return;
        }

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
    }
} // namespace mt
