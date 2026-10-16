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

#pragma once

#include <atomic>
#include <mutex>

namespace mt
{
    constexpr int MAX_EXT_INTERVALS   = 24;
    constexpr int TEMPO_SYNC_INTERVAL = 24;

    constexpr float SECONDS_IN_MINUTE = 60.f;
    constexpr float MILLS_IN_MINUTE   = ( SECONDS_IN_MINUTE * 1000.f );
    constexpr float MICROS_IN_MINUTE  = ( MILLS_IN_MINUTE * 1000.f );

    class ICMidiClockListener
    {
    public:
        virtual ~ICMidiClockListener() = default;
        virtual void onTick( unsigned int ) {} // Current Tick
        virtual void onStart( unsigned int ) {} // Tick count at Play/Resume
        virtual void onResume( unsigned int ) {} // Tick count at Play/Resume
        virtual void onStop( unsigned int ) {} // Tick count at Stop
        virtual void onPause( unsigned int ) {} // Tick count at Pause
    };

    //
    // CMidiClock: the sequencer's clock. Its HighResolutionTimer thread runs
    // tickInternal(); the tick thread and the message thread start, stop and
    // set the tempo; a MIDI input thread may run tickExternal().
    //
    // The control functions (start, stop, tempo, PPQN, sync mode,
    // tickExternal) are serialized by a lock, which the timer thread never
    // takes: stopping waits for the timer's tick, so the counters the tick
    // uses are only reset while the timer is stopped. A new tempo reaches a
    // running timer as a request it takes up at its next tick, so that the
    // tempo ramp, the tick factor and the period are the timer thread's
    // alone while it runs. The state, the sync mode and what tickExternal()
    // shares with the timer are atomics.
    //
    class CMidiClock : private juce::HighResolutionTimer
    {
    public:
        enum class SyncMode
        {
            INTERNAL_CLOCK = 0,
            EXTERNAL_CLOCK
        };

        enum class ClockState
        {
            STOPPED = 0,
            STARTING,
            STARTED
        };

        enum PPQNResolution
        {
            PPQN_24  = 24,
            PPQN_48  = 48,
            PPQN_96  = 96,
            PPQN_384 = 384,
            PPQN_480 = 480,
            PPQN_960 = 960
        };

        std::function<void( uint32_t tick )> onTick       = []( uint32_t ) {};
        std::function<void()>                onClockStart = []() {};
        std::function<void()>                onClockStop  = []() {};

        std::function<void( uint32_t tick, uint32_t microsPerQN )> onSync24Callback = []( uint32_t, uint32_t ) {};

        explicit CMidiClock( PPQNResolution PPQN = PPQN_96 );
        ~CMidiClock();

        // Clock control API's
        void init() noexcept;
        void start( bool ClearCounters ) noexcept;
        void stop() noexcept;

        // Clock state API's...
        inline bool isRunning() const noexcept { return state.load() != ClockState::STOPPED; }
        inline bool isStopped() const noexcept { return state.load() == ClockState::STOPPED; }

        // PPQN factors...
        void           setInternalPPQN( PPQNResolution resolution );
        void           setExternalPPQN( PPQNResolution resolution );
        PPQNResolution getInternalPPQN() const noexcept { return intPPQN; }
        PPQNResolution getExternalPPQN() const noexcept { return extPPQN; }

        // Tempo API's
        float getTempo();
        void  setTempo( float Tempo, double RampLengthInSeconds = 0.001 );
        void  setTempoByTime( uint32_t MicrosPerQuarterNote );

        // Sync API's
        void tickExternal();
        void tickInternal();

        // Clock Synchronization mode.
        void        setClockSyncMode( SyncMode tempo_mode ) noexcept { syncMode.store( tempo_mode ); }
        SyncMode    getClockSyncMode() const noexcept { return syncMode.load(); };
        inline bool isInternalClockSync() const noexcept { return syncMode.load() == SyncMode::INTERNAL_CLOCK; };
        inline bool isExternalClockSync() const noexcept { return syncMode.load() == SyncMode::EXTERNAL_CLOCK; };

        // Calculations...
        static float tempoToMicros( float Tempo ) noexcept { return ( MICROS_IN_MINUTE / Tempo ); }
        static float microsToTempo( float Micros ) noexcept { return ( MICROS_IN_MINUTE / Micros ); }
        static float tempoToPPQNMicros( float Tempo, int PPQN ) noexcept { return ( tempoToMicros( Tempo ) / (float)PPQN ); }
        static float PPQNMicrosToTempo( float Micros, int PPQN ) noexcept { return microsToTempo( Micros * (float)PPQN ); }

    private:
        CMidiClock( const CMidiClock& )            = delete;
        CMidiClock& operator=( const CMidiClock& ) = delete;

        static void initPlatformTimer();
        static void stopPlatformTimer();
        static void startPlatformTimer( [[maybe_unused]] uint32_t microSeconds );
        static void setPlatformTimerInterval( [[maybe_unused]] float microSeconds );
        static void handlePlatformTimer();

        void     takeTempoRequest( bool UpdateInterval );
        void     applyTempo( float Bpm, int RampSteps, bool UpdateInterval );
        void     resetCounters();
        void     resetExternalClockCounters();
        uint32_t averageOfExternalIntervals() const noexcept;

        inline void hiResTimerCallback() final { handlePlatformTimer(); }
        inline void recomputePPQNFactor() noexcept { intClockPulsesPerExtClockPulse = intPPQN / extPPQN; };

    private:
        static double sTickCounter;
        static double sTickFactor;
        static bool   isTimerRunning;

        // Defaults...
        PPQNResolution intPPQN = PPQN_96;
        PPQNResolution extPPQN = PPQN_24;

        // Serializes the control functions; never taken by the timer thread.
        std::recursive_mutex controlLock;

        // External clock control
        bool              extClockFirstInterval{ false };
        std::atomic<bool> syncTempo{ false };

        std::atomic<ClockState> state{ ClockState::STOPPED };
        std::atomic<SyncMode>   syncMode{ SyncMode::INTERNAL_CLOCK };

        // The tempo last asked for, taken up by the timer thread (or by
        // start(), with the timer stopped).
        std::atomic<float>    requestedTempo{ 1.f };
        std::atomic<int>      requestedRampSteps{ 1 };
        std::atomic<uint32_t> tempoRequest{ 0 };
        uint32_t              tempoRequestTaken{ 0 };

        uint32_t              microsPerQuarterNote{ 0 };
        uint32_t              ticks{ 0 };
        uint32_t              intClockSyncPulses{ 0 };
        std::atomic<uint32_t> extClockSyncPulses{ 0 };
        uint32_t intClockPulsesPerExtClockPulse{ 0 };
        uint32_t clockCounter{ 0 };
        uint32_t extCounter{ 0 };
        uint32_t syncCounter{ 0 };

        double ts{ 0 };
        double te{ 0 };
        double prevTime{ 0 };

        juce::SmoothedValue<float> tempo{ (float)1.0 };

        uint32_t                                extIntervalIdx{ 0 };
        std::array<std::atomic<uint32_t>, MAX_EXT_INTERVALS> extIntervals{};
    };

    // Global MIDI Clock Instance
    CMidiClock& getMidiClock() noexcept;
} // namespace mt
