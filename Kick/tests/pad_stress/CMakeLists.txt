## Stress test of playing pads while they are edited (see PadStress.cpp).

project(PadStress VERSION 1.0.0)

configure_file(${CMAKE_SOURCE_DIR}/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config.h)

juce_add_console_app(PadStress PRODUCT_NAME PadStress)

set_target_properties(PadStress PROPERTIES CXX_STANDARD 17)

target_sources(PadStress PRIVATE PadStress.cpp)
target_include_directories(PadStress PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_compile_definitions(PadStress
    PRIVATE
        JUCE_APPLICATION_NAME_STRING="PadStress"
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
    target_compile_options(PadStress PRIVATE -fsanitize=${KICK_TEST_SANITIZER} -fno-omit-frame-pointer)
    target_link_options(PadStress PRIVATE -fsanitize=${KICK_TEST_SANITIZER})
endif()

target_link_libraries(PadStress
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
