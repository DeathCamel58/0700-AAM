
juce_add_modules(
    dywapitchtrack
    fpm
    magic_enum
    melatonin_parameters
    moodycamel
    signal
    waveform
)

# juce_serialport is not in this repository. Only KickDeviceControl needs
# it (through input_events), so register it when it has been added and let
# everything else configure and build without it.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/juce_serialport/juce_serialport.h")
    juce_add_module(juce_serialport)
endif()

if(USE_VALUE_TREE_DEBUGGER)
juce_add_modules(
    juce-toys/jcf_debug
    juce-toys/jcf_advanced_leak_detector
    juce-toys/multithreading
)
endif()