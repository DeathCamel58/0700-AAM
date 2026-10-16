/*
 * Copyright (c) 2024 MUSIC Tribe
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
    class MTAudioTransportManager
        : public MTAudioSequenceManager
        , private ICMidiClockListener
    {
    public:
        std::function<void( TransportState State )> transportStateChanged;

        explicit MTAudioTransportManager( MTSelected& Selected, MTAudioManager& Manager );
        virtual ~MTAudioTransportManager();

        void close();
        void setup();

        // Getters.
        inline juce::ValueTree& getRuntimeState() noexcept { return runtimeVT; };
        inline MTSequence*      getPlayingSequence() { return playingSequence.load(); }

        // Transport control API's
        TransportState getTransportState();
        void           setTransportState( TransportState TState );

        bool isArmed();
        bool isRecording();

        bool isPlaying() { return TransportState::Playing == getTransportState(); }
        bool isNotPlaying() { return TransportState::Playing != getTransportState(); }
        bool isStopped() { return TransportState::Stopped == getTransportState(); }
        bool isPaused() { return TransportState::Paused == getTransportState(); }

        int  play( int Seq, bool FromBeginning, bool Now );
        int  play( int Seq );
        int  resume( int Seq );
        int  pause();
        int  stop();
        void seek( int posInTicks );
        void updatePlayheads();

        void setupSong( MTSongInfo, int );

        void playSequence( int Sequence );
        void stopSequence();
        void setNextSequence( int NextSequence );
        void setNextBarFlag( bool NextBarFlag );

        // Stops playback if Sequence is playing, and returns once the tick
        // thread no longer uses it: call before deleting a sequence.
        void detachSequence( MTSequence* Sequence );

        // Returns once the tick thread is done with the tick it may be
        // processing: call after taking a sequence, or a track's program,
        // away from it and before freeing that. Not on the tick thread, and
        // not while holding a lock it takes.
        void waitForTickThread();

    private:
        // A sequence the tick thread switches to at a tick, armed by the
        // message thread.
        struct PendingSwitch
        {
            MTSequence* sequence{ nullptr };
            int         index{ -1 };
        };

        enum class SwitchKind
        {
            SongStep,
            NextSequence,
            NextBar,
        };

        void armSwitch( PendingSwitch& Switch, int Index );
        void armSongStep();
        void startSequence( MTSequence* Sequence, bool Resume );
        void stopPlayingSequence( bool Pause );
        void processPendingSwitches( unsigned int Tick, unsigned int PreviousTick );
        void switchSequence( SwitchKind Kind, const PendingSwitch& Switch );
        void sequenceSwitched( SwitchKind Kind, int Index, uint32_t Generation );
        void handleSongTick();
        void onTick( unsigned int Ticks ) override;
        void onBeat();
        void onBar();
        void onStart( unsigned int ) override;
        void onLoopBegin( unsigned int );
        void onLoopEnd( unsigned int, unsigned int );
        void processTick();

        void resetSong();

    private:
        CMidiClock&     midiClock{ getMidiClock() };
        juce::ValueTree runtimeVT{ IDs::RUNTIME };

        // Changed under switchLock (by the message thread, or by the tick
        // thread when it switches sequences); the tick thread reads it.
        std::atomic<MTSequence*> playingSequence{ nullptr };

        // Guards starting and stopping the playing sequence (and the clock),
        // and the switches below. The tick thread takes it after a tick, never
        // while it holds a sequence's or a track's lock.
        std::mutex    switchLock;
        PendingSwitch nextBarSwitch; // At the playing sequence's next bar.
        PendingSwitch endSwitch; // When the playing sequence reaches its end.
        PendingSwitch songSwitch; // The next song step...
        bool          songSwitchArmed{ false };
        unsigned int  songSwitchTicks{ 0 }; // ...once the current one has played this long.
        unsigned int  songTicks{ 0 }; // How long the current step has played.
        uint32_t      songSwitchGeneration{ 0 };
        bool          barReached{ false }; // Tick thread only.

        struct PlayManager
        {
            int  timeInTicks{ 0 };
            int  seq{ -1 };
            int  repeatCount{ 1 };
            int  nextSeqUpdateTicks{ 0 };
            bool isEnabled{ true };
            bool fromBegin{ true };
        };

        // Song mode, on the message thread.
        uint32_t                songGeneration{ 0 };
        int                     stepIndex{ -1 };
        int                     lastSeqTicks{ 0 };
        int                     nextSequenceUpdateTicks{ -1 };
        int                     playTime{ 0 };
        unsigned int            lastSongTick{ 0 };
        std::queue<PlayManager> playQueue;

        // The clock's latest tick, for the message thread.
        std::atomic<unsigned int> songTick{ 0 };
        std::atomic<bool>         songTickPending{ false };
        std::shared_ptr<bool>     transportAlive{ std::make_shared<bool>( true ) };

        int                       nextSequenceToPlay{ -1 };
        std::atomic<bool>         stopTickProcessing{ false };
        std::atomic<bool>         threadIsProcessingTick{ false };
        std::atomic<uint32_t>     ticksProcessed{ 0 };
        juce::WaitableEvent       tickProcessed;
        std::atomic<unsigned int> prevTick{ 0 };
        std::atomic<unsigned int> curTick{ 0 };

        std::thread*        tickProcessingThread;
        juce::WaitableEvent onTickEvent;
    };

} // namespace MT
