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

#pragma once

#include "../Includes/AudioObject.h"
#include "../Objects/LevelMeter.h"
#include "../Daw/DawRouting.h"
#include "../Daw/DawInserts.h"

#include <atomic>
#include <functional>
#include <string>

namespace AudIO
{
    /// Base of the audio recording objects that capture the engine's input
    /// into memory: SampleRecorder (one-shot capture for the sampler) and
    /// Looper (loop recorder with overdub).
    ///
    /// A Recorder is an ordinary design object with two inputs and two
    /// outputs. It is routed like any other DAW object (DawRouting): its input
    /// is patched from an input bus with connectInputTo() and its output goes
    /// to the main out with initAudioRouting(). It carries a chain of insert
    /// effects (DawInserts), applied to what it outputs.
    ///
    /// Threading: the set/start/stop calls are made from a control thread
    /// (the message thread in Kick). Simple settings are atomics. Everything
    /// that touches the recorded audio or the play/record heads is done
    /// under a spin lock that the audio thread only ever try-locks, so the
    /// audio thread never waits: if a control call holds the lock (for
    /// instance while the buffer is being reallocated), the recorder outputs
    /// only its monitor signal for that block. Memory is allocated on the
    /// control thread only (setLength(), prepareToPlay()). Events raised on
    /// the audio thread (end of recording, end of preview) are delivered on
    /// the message thread through a juce::AsyncUpdater; events caused by a
    /// control call are delivered synchronously from that call.
    class Recorder
        : public Object
        , public DawRouting
        , public DawInserts
        , private juce::AsyncUpdater
    {
    public:
        enum Direction
        {
            FORWARD,
            REVERSE,
        };

        /// Which inputs are recorded, and into how many channels.
        enum Mode
        {
            L_MONO, ///< Input 1 into one channel.
            R_MONO, ///< Input 2 into one channel.
            LR_MONO, ///< Inputs 1 and 2 summed (averaged) into one channel.
            L_STEREO, ///< Input 1 into both channels.
            R_STEREO, ///< Input 2 into both channels.
            LR_STEREO, ///< Inputs 1 and 2 into channels 1 and 2.
        };

        static constexpr int    NUM_MODES{ 6 };
        static constexpr float  MIN_DB{ -100.f };
        static constexpr float  MAX_GAIN_DB{ 12.f };
        static constexpr double MAX_LENGTH_SECONDS{ 600.0 };

        explicit Recorder( Design& D, const std::string& Name );
        ~Recorder() override;

        // Listeners. Called on the message thread.
        std::function<void()>                       onRecorderStarted;
        std::function<void()>                       onRecorderStopped;
        std::function<void()>                       onRecorderPaused;
        std::function<void()>                       onRecorderResumed;
        std::function<void()>                       onRecorderPreviewStarted;
        std::function<void()>                       onRecorderPreviewStopped;
        std::function<void( double TimeInSeconds )> onRecorderLengthChanged;

        // Enable: a disabled recorder outputs silence and records nothing.
        void setEnable( bool Enable ) noexcept { enabled_.store( Enable ); }
        bool isEnabled() const noexcept { return enabled_.load(); }

        // Input monitoring.
        void  setMonitorOn( bool Monitor ) noexcept { monitor_.store( Monitor ); }
        bool  isMonitorOn() const noexcept { return monitor_.load(); }
        void  setMonitorGain( float Gain ) noexcept; // Linear, 0 to 1.
        float getMonitorGain() const noexcept { return monitorGain_.load(); }

        // Gain and pan (what they apply to is defined by the subclass).
        void  setGain( float Gain ) noexcept; // Linear, 0 to +12 dB.
        void  setGainDB( float GainDB ) noexcept;
        void  setPan( float Pan ) noexcept; // -1 (left) to 1 (right).
        void  setGainAndPan( float Gain, float Pan, bool Force = false ) noexcept;
        float getGain() const noexcept { return gain_.load(); }
        float getGainDB() const noexcept { return utils::linearTolog( gain_.load() ); }
        float getPan() const noexcept { return pan_.load(); }

        // Recording starts when the input peak reaches the threshold.
        // A threshold of 0 (-100 dB or less) starts recording at once.
        void  setThreshold( float Gain ) noexcept; // Linear, 0 to 1.
        void  setThresholdDB( float DB ) noexcept;
        float getThreshold() const noexcept { return threshold_.load(); }
        float getThresholdDB() const noexcept { return utils::linearTolog( threshold_.load() ); }

        // Direction in which the head moves through the buffer.
        void      setDirection( Direction Dir ) noexcept { direction_.store( Dir ); }
        Direction getDirection() const noexcept { return direction_.load(); }
        bool      isForward() const noexcept { return FORWARD == direction_.load(); }
        bool      isReverse() const noexcept { return REVERSE == direction_.load(); }

        void        setRecordingMode( Mode M ) noexcept;
        Mode        getRecordingMode() const noexcept { return mode_.load(); }
        static bool isStereoMode( Mode M ) noexcept { return L_STEREO <= M; }

        void setLooping( bool Looping ) noexcept { looping_.store( Looping ); }
        bool isLooping() const noexcept { return looping_.load(); }
        void setOverdub( bool Overdub ) noexcept { overdub_.store( Overdub ); }
        bool isOverdubbing() const noexcept { return overdub_.load(); }

        /// Length of the record buffer. Allocates; call from a control thread.
        void   setLength( double TimeInSeconds );
        double getLength() const noexcept { return lengthSeconds_.load(); }

        // Meters (input: before anything is done to it; output: after inserts).
        MeterData& readInputMeter() { return meter_.getMeterIn( inMeter_ ); }
        MeterData& readOutputMeter() { return meter_.getMeterOut( outMeter_ ); }

    protected:
        enum Event : uint32_t
        {
            EV_STARTED         = 1u << 0,
            EV_STOPPED         = 1u << 1,
            EV_PAUSED          = 1u << 2,
            EV_RESUMED         = 1u << 3,
            EV_PREVIEW_STARTED = 1u << 4,
            EV_PREVIEW_STOPPED = 1u << 5,
            EV_LENGTH_CHANGED  = 1u << 6,
        };

        /// Called with the lock held, from the audio thread. Source holds the
        /// two channels selected by the recording mode, without gain; Out
        /// already holds the monitor signal, add to it.
        virtual void processRecorder( const float* const* Source, float* const* Out, int NumSamples ) = 0;

        /// Called with the lock held after the buffer has been (re)allocated.
        virtual void bufferResized() {}

        /// Message-thread delivery of events. Overrides must call this.
        virtual void dispatchEvents( uint32_t Events );

        /// Raise an event from the audio thread.
        void postEvent( uint32_t Events ) noexcept;
        /// Raise an event from a control thread (delivered synchronously).
        void sendEvent( uint32_t Events )
        {
            if( Events )
                dispatchEvents( Events );
        }

        /// Write audio as a 24 bit WAV file. A relative path is taken from
        /// the current directory.
        static bool writeWavFile( const juce::String& Path, const float* const* Channels, int NumChannels, int NumSamples, double SampleRate );

        /// True when the peak of the block reaches the threshold.
        bool thresholdReached( const float* const* Source, int NumSamples ) const noexcept;

        /// Allocate the buffer for the current length and sample rate.
        void allocateBuffer( bool KeepContent );

        /// Samples per second as an integer count.
        int samplesFor( double Seconds ) const noexcept;

        /// Linear ramp of a gain over one block, owned by the audio thread.
        struct GainRamp
        {
            float current{ 0.f };
            float step{ 0.f };

            void begin( float Target, int NumSamples, bool Snap ) noexcept
            {
                if( Snap )
                    current = Target;
                step = ( Target - current ) / (float)utils::max( NumSamples, 1 );
            }
            float next() noexcept { return current += step; }
            void  end( float Target ) noexcept
            {
                current = Target;
                step    = 0.f;
            }
            bool isSilent( float Target ) const noexcept { return 0.f == current && 0.f == Target; }
        };

        /// True once after setGainAndPan( ..., true ): jump to the new gain.
        bool consumeGainSnap() noexcept { return snapGain_.exchange( false ); }

        /// Length reported by onRecorderLengthChanged.
        virtual double reportedLength() const { return getLength(); }

        /// Left and right gains of the current gain and pan (balance law).
        void getGainAndPanTargets( float& L, float& R ) const noexcept;

        juce::SpinLock           lock_;
        juce::AudioBuffer<float> buffer_; // Always two channels; guarded by lock_.
        bool                     keepContentOnResize_{ false };

    private:
        void prepareToPlay( uint32_t Samplerate, uint32_t Blocksize ) override;
        void process( Buffer<float>& Buffer ) override;
        void handleAsyncUpdate() override;

        std::atomic<bool>      enabled_{ true };
        std::atomic<bool>      monitor_{ false };
        std::atomic<bool>      looping_{ false };
        std::atomic<bool>      overdub_{ false };
        std::atomic<float>     monitorGain_{ 1.f };
        std::atomic<float>     gain_{ 1.f };
        std::atomic<float>     pan_{ 0.f };
        std::atomic<float>     threshold_{ 0.f };
        std::atomic<Direction> direction_{ FORWARD };
        std::atomic<Mode>      mode_{ LR_STEREO };
        std::atomic<double>    lengthSeconds_{ 0.0 };
        std::atomic<uint32_t>  pendingEvents_{ 0 };
        std::atomic<bool>      snapGain_{ true };

        GainRamp monitorRamp_;

        juce::AudioBuffer<float> source_; // Scratch, sized in prepareToPlay.
        juce::AudioBuffer<float> spare_; // Stands in for a missing output.

        LevelMeter meter_;
        MeterData  inMeter_;
        MeterData  outMeter_;

        AUD_IO_MAKE_NON_COPYABLE( Recorder );
    };

} // namespace AudIO
