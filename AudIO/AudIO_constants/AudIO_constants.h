/*
 BEGIN_JUCE_MODULE_DECLARATION

  ID:                 AudIO_constants
  vendor:             MusicTribe
  version:            0.1.0
  name:               AudIO_constants
  description:        Parameter ranges (MIDI, MIDI_CH, POLYPHONY, GAIN, PAN, DB) and
                      audio engine settings shared by Kick's engine and UI
  license:            AGPLv3
  dependencies:       juce_core, juce_audio_basics

 END_JUCE_MODULE_DECLARATION
*/
/*
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

// The AudIO_constants module Music Tribe ships with Kick is not part of the
// public release. This is a clean-room replacement: the names and their
// shapes are those Kick's sources use, and every value comes from the Kick
// and AudIO sources in this tree or from the MIDI specification, as noted
// beside it. Values marked GUESS have no direct evidence in the tree and
// were chosen to keep the code that uses them working; they may differ from
// the values on the hardware.
//
// The namespaces follow Kick's own kick_Constants.h (BPM, SEMI, FINE, ...):
// MIN/MAX/DEFAULT/STEP, int where Kick passes them as template arguments
// (RangeConstrainer in kick_ValueTreeConstrainers.h), float where the value
// is continuous.

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

///
/// Audio engine.
///

// The AudIO DAW engine is built for 48 kHz in blocks of 480 samples (10 ms):
// AudIO/Daw/DawEngine.cpp sizes its buffer pool with BLOCK_SIZE 480, the DAW
// example starts the device with startAudioDevice( 48000, 480 ), and Kick's
// SamplerVoice and ISynthVoice default to sampleRate 48000, blockSize 480.
static constexpr int AUDIO_SAMPLE_RATE{ 48000 };
static constexpr int AUDIO_BLOCK_SIZE{ 480 };

// Channel counts, given to AudIO::DawEngine as mono ports.
// Inputs:  the UI offers "Input 1" and "Input 2" (kick_IOComponent.cpp,
//          kick_PadMixer*Program.h).
// Outputs: outMonoList has "Out 1" to "Out 8", outputList the pairs
//          "Out 1,2" to "Out 7,8" (kick_common_defs.h).
// Sends/returns: four of each, "Send 1".."Send 4" (kick_EffectsEdit.h),
//          sg0..sg3 (mt_audio_interface_types.cpp), "Return 1".."Return 4"
//          (kick_MidiControlTabs.cpp).
// Submixes: "Submix 1".."Submix 8" (kick_MidiControlTabs.cpp).
// Aux:     no aux bus appears in the UI, and DawEngine ignores the count
//          (its default is 0). GUESS.
static constexpr int AUDIO_NUM_INPUTS{ 2 };
static constexpr int AUDIO_NUM_OUTPUTS{ 8 };
static constexpr int AUDIO_NUM_SEND_RETURNS{ 4 };
static constexpr int AUDIO_NUM_SUBMIX{ 8 };
static constexpr int AUDIO_NUM_AUX{ 0 };

// Placeholder held by a UI attachment that is not attached to any value
// (kick_attachments.h). -1 is JUCE's "nothing selected" for list indices,
// which the list and toggle attachments pass it to. GUESS.
static constexpr int UNKNOWN_VALUE{ -1 };

///
/// Parameter ranges.
///

namespace MIDI
{
    // MIDI data bytes are 7-bit: note numbers, velocities and controller
    // values run 0..127 (MIDI 1.0 specification).
    static constexpr int MIN{ 0 };
    static constexpr int MAX{ 127 };
    static constexpr int STEP{ 1 };
    // Number of distinct values, used as the bound of loops and arrays over
    // all notes/pads (mt_audio_iprogram.cpp, mt_audio_interface_types.cpp).
    static constexpr int NUMERIC_MAX{ MAX + 1 };
    // Converts a 0..127 value to 0..1; kick_common_utils.cpp documents it
    // as "(MidiLevel / 127 )", and dbToMidi() is its inverse with MAX.
    static constexpr float SCALER{ 1.f / (float)MAX };
    // Default for MIDI-ranged amounts (LFO and velocity modulation depths in
    // mt_audio_interface_types.cpp), which are off unless set. GUESS.
    static constexpr int DEFAULT{ 0 };
    // Note 60 is middle C (MIDI specification); it is the default key note
    // of a sample.
    static constexpr int MIDDLE_C{ 60 };
    // Default number of a sequence note (MTNote::number). Kick's pads map
    // directly to notes (pad + bank * 16), so 0 is the first pad. GUESS.
    static constexpr int NOTE_DEFAULT{ MIN };
    // Default level/velocity of programs, tracks and notes, also used for the
    // screen brightness, whose slider runs 0..100 (kick_ui_settings_hardware
    // .cpp). Pad velocity and aftertouch default to MAX explicitly, so this is
    // a different value; 100 fits the brightness range. GUESS.
    static constexpr int LEVEL_DEFAULT{ 100 };
}

namespace MIDI_CH
{
    // MIDI channels are 1..16 (MIDI 1.0 specification; juce::MidiMessage
    // numbers them the same way). A track's input channel also takes 0 for
    // "All" (mt::Kick::midiChannels in kick_common_defs.h, and
    // MTTrack::isMidiInAllowed() accepts any channel when it is <= 0), and
    // MidiChannelConstrainer must not clip that away, so MIN is 0.
    static constexpr int MIN{ 0 };
    static constexpr int MAX{ 16 };
    static constexpr int STEP{ 1 };
    // Programs and track outputs pass the channel straight to
    // juce::MidiMessage, which needs 1..16, and the MIDI program view shows
    // it as midiCh - 1 in a list of 1..16 (kick_AppProgramComponent.cpp).
    static constexpr int DEFAULT{ 1 };
}

namespace POLYPHONY
{
    // Every synth object pre-allocates POLYPHONY::MAX voices
    // (DrumProgram.cpp, KeygroupProgram.cpp, clipProgram.cpp); the samplers
    // use MAX_POLYPHONY_PER_OBJECT, 16 (ISampler.h), for the same purpose.
    static constexpr int MIN{ 1 };
    static constexpr int MAX{ 16 };
    static constexpr int STEP{ 1 };
    // Default polyphony of a program and of each drum pad. Voice stealing is
    // not implemented (ISynthObject::getFreeVoice()), so a lower limit makes
    // a pad retriggered while it still sounds play nothing. GUESS.
    static constexpr int DEFAULT{ MAX };
}

namespace GAIN
{
    // Linear gain, 0..1: AudIO::Panner takes it as a factor, and the code
    // that uses these values clips gains to 0..1 (ASoundSlice::setGain(),
    // ClipSampler.cpp, the metronome level in kick_modeHeader.cpp, the
    // insert parameter knobs in kick_KnobGrid.cpp).
    static constexpr float MIN{ 0.f };
    static constexpr float MAX{ 1.f };
    // Unity gain. GUESS.
    static constexpr float DEFAULT{ 1.f };
    // Slider resolution; kick_KnobGrid.cpp uses 0.01 for the same range.
    static constexpr float STEP{ 0.01f };
    // -3 dB, 1/sqrt(2). Default gain of the metronome (kick_settings.cpp),
    // which must lie in 0..1 (kick_modeHeader.cpp clips it there), so this
    // is the reciprocal of the square root of two rather than sqrt(2)
    // itself. GUESS.
    static constexpr float ROOT_2{ 0.70710678f };
}

namespace PAN
{
    // -1 (left) .. 1 (right), 0 centre: AudioPreview::setPan(),
    // ASoundSlice::setPan() and ClipSampler.cpp all clip pan to -1..1, and
    // the pan controls are drawn centred.
    static constexpr float MIN{ -1.f };
    static constexpr float MAX{ 1.f };
    static constexpr float DEFAULT{ 0.f };
    // Slider resolution, matching GAIN::STEP. GUESS.
    static constexpr float STEP{ 0.01f };
}

namespace DB
{
    // Floor of the level meters and of the dB conversions in
    // kick_common_utils.cpp: the meters, sliders and the MIDI meter range
    // all bottom out at -100 dB (kick_levelMeter.cpp, kick_GainSlider.h,
    // mt_midi_meter.h).
    static constexpr float MIN{ -100.f };
}

///
/// Empty objects returned by reference when there is nothing to return.
/// (Kick's kick_Constants.h adds EMPTY::VTState to this namespace.)
///

namespace EMPTY
{
    static const juce::String             String{};
    static const juce::AudioBuffer<float> SampleBuffer{};
}
