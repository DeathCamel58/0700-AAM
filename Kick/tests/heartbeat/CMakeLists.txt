## Unit tests of KickMain's heartbeat bookkeeping (see HeartBeatTest.cpp): when
## a process counts as starting, missing or silent, with made-up times. No
## process is started or signalled. -DKICK_TEST_SANITIZER=address (or
## undefined) builds them with that sanitizer.

project(HeartBeatTest VERSION 1.0.0)

juce_add_console_app(HeartBeatTest PRODUCT_NAME HeartBeatTest)

set_target_properties(HeartBeatTest PROPERTIES CXX_STANDARD 17)

target_sources(HeartBeatTest PRIVATE HeartBeatTest.cpp)
target_include_directories(HeartBeatTest PRIVATE ${CMAKE_SOURCE_DIR}/apps/kick_main)

target_compile_definitions(HeartBeatTest
    PRIVATE
        JUCE_USE_CURL=0
)

if(KICK_TEST_SANITIZER)
    target_compile_options(HeartBeatTest PRIVATE -fsanitize=${KICK_TEST_SANITIZER} -fno-omit-frame-pointer)
    target_link_options(HeartBeatTest PRIVATE -fsanitize=${KICK_TEST_SANITIZER})
endif()

target_link_libraries(HeartBeatTest
    PRIVATE
        juce::juce_core
        juce::juce_events
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
