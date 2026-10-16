/*******************************************************************************
 The block below describes the properties of this module, and is read by
 the Projucer to automatically generate project code that uses it.
 For details about the syntax and how to create or use a module, see the
 JUCE Module Format.txt file.

 BEGIN_JUCE_MODULE_DECLARATION

  ID:                 audio_interface
  vendor:             MUSIC Tribe, MY
  version:            0.1.0
  name:               MusicTribe Common definitions
  description:        Classes and interfaces containing common definitions
  website:
  license:            AGPLv3

  dependencies:       AudIO_juce

 END_JUCE_MODULE_DECLARATION

*******************************************************************************/

#pragma once
#ifndef AUDIO_INTERFACE_INCLUDE
    #define AUDIO_INTERFACE_INCLUDE

    // Dependent module includes.
    #include <AudIO_juce/AudIO_juce.h>

// Module include files.

namespace mt
{
    // The engine and the UI name AudIO's recording objects unqualified in mt.
    using AudIO::Looper;
    using AudIO::Recorder;
    using AudIO::SampleRecorder;
} // namespace mt

#endif // AUDIO_INTERFACE_INCLUDE
