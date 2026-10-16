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
    /// The main mix plays on the first two outputs. The engine's other
    /// outputs are only opened on a device that has that many channels of
    /// its own, never on a sound server or a converting PCM, which would
    /// mix them into the main outputs. A device whose input cannot be
    /// opened is opened for output only.
    ///
    /// The engine processes fixed blocks of the AudIO buffer pool's block
    /// size. The device is asked for that buffer size; when it delivers
    /// whole multiples of it the blocks are processed in place, otherwise
    /// they are re-blocked through a FIFO, which adds one block of latency.
    ///
    /// A watchdog on the message thread notices a device that stops calling
    /// back (JUCE's ALSA thread just ends on a read or write error) or
    /// reports an error, closes it and opens it again, waiting longer after
    /// each failure. While there is no device, a clock thread runs the
    /// engine on silence at the engine's rate, so that the engine still
    /// takes what the message thread queues for its audio thread. The
    /// engine is prepared again when a device opens at another rate.
    ///
    class AudioDevice
        : public AudIO::IAudioDevice
        , private juce::AudioIODeviceCallback
        , private juce::Timer
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

        // juce::Timer: the device watchdog.
        void timerCallback() override;

        void closeLostDevice( const juce::String& Reason );
        void reopenDevice();
        void scheduleReopen();
        void processWithoutDevice();

        bool         openDevice( uint32_t SampleRate, uint32_t BlockSize );
        juce::String openDevice( juce::AudioIODeviceType& Type, const juce::String& Output, const juce::String& Input, uint32_t SampleRate, uint32_t BlockSize );
        juce::String initialiseDevice( juce::AudioIODeviceType& Type, const juce::String& Output, const juce::String& Input, int Inputs, int Outputs, uint32_t SampleRate, uint32_t BlockSize );
        int          numOutputChannelsFor( juce::AudioIODeviceType& Type, const juce::String& Output ) const;
        void readInputs( const float* const* InputData, int NumInputChannels, int SrcOffset, int DstOffset, int NumSamples );

        // Calls the engine at its rate while no device does.
        class DummyClock final : public juce::Thread
        {
        public:
            explicit DummyClock( AudioDevice& Owner )
                : juce::Thread( "Kick audio clock" ), owner( Owner ) {}

            void run() override;

        private:
            AudioDevice& owner;
        };

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

        // The rate the engine is asked for, and the rate it is prepared at.
        uint32_t requestedRate{ 0 };
        uint32_t sampleRate{ 0 };

        DummyClock dummyClock{ *this };

        // Watchdog state; the counter and flags are set by the audio thread.
        std::atomic<uint32_t> numCallbacks{ 0 };
        std::atomic<bool>     inCallback{ false };
        std::atomic<bool>     deviceError{ false };
        uint32_t              lastNumCallbacks{ 0 };
        double                lastCallbackTime{ 0 };
        double                deviceStartTime{ 0 };
        double                nextReopenTime{ 0 };
        int                   reopenDelayMs{ 0 };
        int                   lastXRunCount{ 0 };
        bool                  stuckLogged{ false };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR( AudioDevice )
    };
} // namespace mt
