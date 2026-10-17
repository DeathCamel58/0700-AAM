## Unit tests of the user interface's stall check (see StallLogTest.cpp): how
## late the check's timer came and what is logged for it, with made-up
## times. No window, no message loop. -DKICK_TEST_SANITIZER=address (or
## undefined) builds them with that sanitizer.

project(StallLogTest VERSION 1.0.0)

juce_add_console_app(StallLogTest PRODUCT_NAME StallLogTest)

set_target_properties(StallLogTest PROPERTIES CXX_STANDARD 17)

target_sources(StallLogTest PRIVATE StallLogTest.cpp)
target_include_directories(StallLogTest PRIVATE ${CMAKE_SOURCE_DIR}/modules)

target_compile_definitions(StallLogTest
    PRIVATE
        JUCE_USE_CURL=0
)

if(KICK_TEST_SANITIZER)
    target_compile_options(StallLogTest PRIVATE -fsanitize=${KICK_TEST_SANITIZER} -fno-omit-frame-pointer)
    target_link_options(StallLogTest PRIVATE -fsanitize=${KICK_TEST_SANITIZER})
endif()

target_link_libraries(StallLogTest
    PRIVATE
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
