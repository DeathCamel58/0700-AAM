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
    /// Records the input into memory, up to the length set with setLength(),
    /// to make a sample of it.
    ///
    /// - Gain and pan are applied to the signal as it is recorded.
    /// - Recording waits for the input to reach the threshold.
    /// - FORWARD records from the start of the buffer, REVERSE from its end
    ///   backwards, so that the recording plays back reversed.
    /// - With looping, recording carries on from the other end of the buffer
    ///   when it is full; otherwise it stops there. With overdub, the input is
    ///   added to what is already recorded instead of replacing it.
    /// - The mono modes record one channel, the stereo modes two.
    /// - The recording can be previewed (played to the output) and saved as
    ///   a WAV file.
    class SampleRecorder : public Recorder
    {
    public:
        explicit SampleRecorder( Design& D, const std::string& Name = "SampleRecorder" );
        ~SampleRecorder() override = default;

        /// Stop and erase the recording. The length is kept.
        void reset();

        void startRecording( bool FromBeginning = true );
        void stopRecording();
        void pauseRecording();
        void resumeRecording();

        void startPreview( double FromStartTimeInSeconds = 0.0 );
        void stopPreview();

        /// Writes the recording as a WAV file. False if there is nothing to
        /// save or the file cannot be written.
        bool saveRecording( const std::string& FilePath );

        bool isRecording() const noexcept { return recording_.load(); }
        bool isPaused() const noexcept { return paused_.load(); }
        bool isPreviewing() const noexcept { return previewing_.load(); }

        /// Recorded part of the buffer, 0 to 1 (1 when it is full).
        float readProgress() const noexcept { return progress_.load(); }

        /// Length of the recording in seconds.
        double getRecordedLength() const noexcept;

        /// The recording: as many channels as the recording mode records, and
        /// the recorded length once stopped (the whole buffer while recording).
        juce::AudioBuffer<float>& getAudioBuffer();

        void stop() override;

    private:
        void   processRecorder( const float* const* Source, float* const* Out, int NumSamples ) override;
        void   bufferResized() override;
        void   dispatchEvents( uint32_t Events ) override;
        double reportedLength() const override;

        void finalizeLocked(); // Lock held.
        void finalizeIfPending();
        void updateViewLocked(); // Lock held.
        void moveRecordingLocked( bool ToEnd ); // Lock held.

        std::atomic<bool>  recording_{ false };
        std::atomic<bool>  paused_{ false };
        std::atomic<bool>  capturing_{ false }; // Threshold reached.
        std::atomic<bool>  previewing_{ false };
        std::atomic<bool>  finalizePending_{ false };
        std::atomic<float> progress_{ 0.f };
        std::atomic<int>   recorded_{ 0 }; // Samples recorded; written under lock_.

        // Guarded by lock_.
        int  writePos_{ 0 };
        int  readPos_{ 0 };
        int  numChannels_{ 2 }; // Channels the recording has.
        bool wrapped_{ false };
        bool finalized_{ true };
        bool reverse_{ false }; // Direction of the current recording.

        GainRamp gainL_, gainR_;

        juce::AudioBuffer<float> view_; // Refers to buffer_.
    };

} // namespace AudIO
