/*
 BEGIN_JUCE_MODULE_DECLARATION

  ID:                 AudIO_juce
  vendor:             MusicTribe
  version:            0.1.0
  name:               AudIO as a JUCE module
  description:        Wraps the public AudIO release (AudIO/AudIO.h) under the module name Kick expects
  license:            AGPLv3
  dependencies:       juce_audio_basics, juce_audio_devices, juce_audio_formats, juce_audio_utils, juce_dsp

 END_JUCE_MODULE_DECLARATION
*/
#pragma once

// Kick's engine modules reach JUCE's audio classes only through this
// module, so the real one must have brought them in. Which modules it
// declared is a guess from what the engine uses.
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>

#include "../AudIO/AudIO.h"
