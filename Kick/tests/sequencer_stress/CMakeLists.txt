## Stress test of the sequencer's threads (see SequencerStress.cpp).
## -DKICK_TEST_SANITIZER=thread or address builds it, JUCE and the engine
## modules included, with that sanitizer.
set(KICK_TEST_SANITIZER "" CACHE STRING "Sanitizer for the stress tests (thread, address or empty)")

project(SequencerStress VERSION 1.0.0)

configure_file(${CMAKE_SOURCE_DIR}/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config.h)

juce_add_console_app(SequencerStress PRODUCT_NAME SequencerStress)

set_target_properties(SequencerStress PROPERTIES CXX_STANDARD 17)

target_sources(SequencerStress PRIVATE SequencerStress.cpp)
target_include_directories(SequencerStress PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_compile_definitions(SequencerStress
    PRIVATE
        JUCE_APPLICATION_NAME_STRING="SequencerStress"
        JUCE_APPLICATION_VERSION_STRING="1.0.0"
        # No audio device types: the engine runs on its fallback clock.
        JUCE_ALSA=0
        JUCE_JACK=0
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_PLUGINHOST_VST3=0
        JUCE_PLUGINHOST_AU=0
        THPAR_SQLITE=1
)

if(KICK_TEST_SANITIZER)
    target_compile_options(SequencerStress PRIVATE -fsanitize=${KICK_TEST_SANITIZER} -fno-omit-frame-pointer)
    target_link_options(SequencerStress PRIVATE -fsanitize=${KICK_TEST_SANITIZER})
endif()

target_link_libraries(SequencerStress
    PRIVATE
        "-latomic"
        audio_engine
        hardware
        contracts
        ipc
        juce::juce_audio_devices
        juce::juce_audio_processors
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
