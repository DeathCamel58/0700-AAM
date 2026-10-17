## Unit test of the computer keyboard's front-panel emulation (see KeyboardDetectorTest.cpp).
## -DKICK_TEST_SANITIZER=address (or undefined, thread) builds it, JUCE and the
## Kick modules included, with that sanitizer.

project(KeyboardDetectorTest VERSION 1.0.0)

configure_file(${CMAKE_SOURCE_DIR}/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config.h)

juce_add_console_app(KeyboardDetectorTest PRODUCT_NAME KeyboardDetectorTest)

set_target_properties(KeyboardDetectorTest PROPERTIES CXX_STANDARD 17)

target_sources(KeyboardDetectorTest PRIVATE KeyboardDetectorTest.cpp)
target_include_directories(KeyboardDetectorTest PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_compile_definitions(KeyboardDetectorTest
    PRIVATE
        JUCE_APPLICATION_NAME_STRING="KeyboardDetectorTest"
        JUCE_APPLICATION_VERSION_STRING="1.0.0"
        JUCE_ALSA=0
        JUCE_JACK=0
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        THPAR_SQLITE=1
        JUCE_PLUGINHOST_AU=0
        JUCE_PLUGINHOST_VST3=0
        JUCE_MODAL_LOOPS_PERMITTED=1
)

if(KICK_TEST_SANITIZER)
    target_compile_options(KeyboardDetectorTest PRIVATE -fsanitize=${KICK_TEST_SANITIZER} -fno-omit-frame-pointer)
    target_link_options(KeyboardDetectorTest PRIVATE -fsanitize=${KICK_TEST_SANITIZER})
endif()

target_link_libraries(KeyboardDetectorTest
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
