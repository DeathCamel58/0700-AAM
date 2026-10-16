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

#pragma once

namespace mt
{
    ///
    /// AudioDevice
    ///
    /// The engine's master audio device: an AudIO::IAudioDevice that plays
    /// the engine through a juce::AudioDeviceManager.
    ///
    /// The device type and device come from the build (KICK_AUDIO_DEVICE_TYPE
    /// and KICK_AUDIO_DEVICE, see CMakeLists.txt), unless DeviceName names a
    /// device. A device name is matched against the names JUCE lists for the
    /// type: exactly, as a wildcard pattern, or as a part of the name; an
    /// empty name selects the type's default device, which for ALSA is the
    /// "default" PCM of /etc/asound.conf. If the type is not available (JUCE
    /// built without it) the first type that has devices is used, and if the
    /// device cannot be opened, the type's default device.
    ///
    /// The engine processes fixed blocks of the AudIO buffer pool's block
    /// size. The device is asked for that buffer size; when it delivers
    /// whole multiples of it the blocks are processed in place, otherwise
    /// they are re-blocked through a FIFO, which adds one block of latency.
    ///
    class AudioDevice
        : public AudIO::IAudioDevice
        , private juce::AudioIODeviceCallback
    {
    public:
        AudioDevice( int NumInputs, int NumOutputs, const juce::String& DeviceName );
        ~AudioDevice() override;

        // AudIO::IAudioDevice
        void start( uint32_t samplerate, uint32_t blocksize ) override;
        void resume() override;
        void pause() override;
        void stop() override;

        // The device manager that owns the open device; other players
        // (MTSamplePlayer) add their callbacks to it.
        juce::AudioDeviceManager& getJuceAudioDeviceMgr() noexcept { return deviceManager; }

    private:
        // juce::AudioIODeviceCallback
        void audioDeviceIOCallbackWithContext( const float* const* InputData, int NumInputChannels, float* const* OutputData, int NumOutputChannels, int NumSamples, const juce::AudioIODeviceCallbackContext& Context ) override;
        void audioDeviceAboutToStart( juce::AudioIODevice* Device ) override;
        void audioDeviceStopped() override;
        void audioDeviceError( const juce::String& ErrorMessage ) override;

        bool openDevice( uint32_t SampleRate, uint32_t BlockSize );
        void readInputs( const float* const* InputData, int NumInputChannels, int SrcOffset, int DstOffset, int NumSamples );

        // A device manager that offers the configured device type first, so
        // that it is the one initialise() picks.
        class JuceDeviceManager final : public juce::AudioDeviceManager
        {
        public:
            explicit JuceDeviceManager( const juce::String& PreferredType )
                : preferredType( PreferredType ) {}

            void createAudioDeviceTypes( juce::OwnedArray<juce::AudioIODeviceType>& Types ) override;

        private:
            const juce::String preferredType;
        };

    private:
        const int          numInputs;
        const int          numOutputs;
        const juce::String deviceName;

        JuceDeviceManager deviceManager;

        // Engine output waiting to be played, when the device's buffer size
        // is not a multiple of the engine's block size.
        juce::AudioBuffer<float> pendingOutputs;
        int                      blockSize{ 0 };
        int                      fifoPos{ 0 };
        bool                     inPlace{ true };
        bool                     opened{ false };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR( AudioDevice )
    };
} // namespace mt
