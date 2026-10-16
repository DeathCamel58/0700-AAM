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

#include "Recorder.h"

namespace AudIO
{
    /// A loop recorder: a stereo loop of the length set with setLength()
    /// (Kick sets it from a number of bars), recorded from the input, played
    /// back round and round and overdubbed.
    ///
    /// - setRecord( true ) selects record (replace): the input replaces the
    ///   loop. After one full pass the looper goes on playing what it
    ///   recorded.
    /// - setOverdub( true ) selects overdub: the input is added to the loop,
    ///   pass after pass, while the loop plays.
    /// - setPlay() leaves record or overdub and plays the loop.
    /// - start() runs the loop, recording if record or overdub is selected.
    ///   Recording waits for the input to reach the threshold unless told to
    ///   skip that (for instance when it is synced to the sequencer).
    /// - Gain and pan apply to the loop's playback; the monitor gain to the
    ///   input heard through the looper. Inserts apply to the whole output.
    /// - The direction is that of the play head: REVERSE plays (and
    ///   overdubs) the loop backwards.
    /// - Without looping, the looper stops at the end of the loop.
    /// - undo() puts back the loop as it was before the last record or
    ///   overdub, redo() takes that back again.
    class Looper : public Recorder
    {
    public:
        explicit Looper( Design& D, const std::string& Name = "Looper" );
        ~Looper() override = default;

        // Modes...
        void setRecord( bool Record );
        void setOverdub( bool Overdub );
        void setPlay();

        // Transport...
        void start( bool FromBeginning = true, bool SkipThresholdDetection = false );
        void play( double FromTimeInSeconds = 0.0 );
        void stop() override;
        void clear();
        void undo();
        void redo();

        /// Writes the loop as a stereo WAV file. False if it is empty or the
        /// file cannot be written.
        bool save( const juce::String& FilePath );

        bool isRecording() const noexcept { return record_.load(); }
        bool isOverdubbing() const noexcept { return overdubMode_.load(); }
        bool isPreviewing() const noexcept { return running_.load() && !record_.load() && !overdubMode_.load(); }
        bool isRunning() const noexcept { return running_.load(); }
        bool isEmpty() const noexcept { return empty_.load(); }
        bool isWaitingForThreshold() const noexcept { return armed_.load(); }
        bool canUndo() const noexcept { return hasUndo_.load(); }
        bool canRedo() const noexcept { return hasRedo_.load(); }

        /// Play head position, 0 to 1. Updated by the audio thread.
        double& getProgress() noexcept { return progress_; }

        /// The loop, two channels of the loop's length.
        juce::AudioBuffer<float>& getAudioBuffer() noexcept { return buffer_; }

    private:
        void processRecorder( const float* const* Source, float* const* Out, int NumSamples ) override;
        void bufferResized() override;

        void beginPassLocked( bool SkipThreshold ); // Lock held.
        void endPassLocked(); // Lock held.
        void setHeadLocked( int Position ); // Lock held.

        std::atomic<bool> record_{ false };
        std::atomic<bool> overdubMode_{ false };
        std::atomic<bool> running_{ false };
        std::atomic<bool> armed_{ false }; // Waiting for the threshold.
        std::atomic<bool> pass_{ false }; // A record or overdub pass is under way.
        std::atomic<bool> empty_{ true };
        std::atomic<bool> hasUndo_{ false };
        std::atomic<bool> hasRedo_{ false };

        // Guarded by lock_.
        int                      head_{ 0 };
        int                      passWritten_{ 0 }; // Samples replaced in this pass.
        bool                     undoEmpty_{ true };
        juce::AudioBuffer<float> undo_; // The other version of the loop.

        double progress_{ 0.0 };

        GainRamp gainL_, gainR_;
    };

} // namespace AudIO
