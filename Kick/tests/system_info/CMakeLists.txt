## Unit tests of the system information the preferences and the system
## resources window show (see SystemInfoTest.cpp), read from fixture copies
## of the /proc and /run files: no window, no display, and nothing of this
## machine's configuration is changed. -DKICK_TEST_SANITIZER=address (or
## undefined) builds them with that sanitizer.

project(SystemInfoTest VERSION 1.0.0)

juce_add_console_app(SystemInfoTest PRODUCT_NAME SystemInfoTest)

set_target_properties(SystemInfoTest PROPERTIES CXX_STANDARD 17)

# The module sources are compiled into the test (SystemInfoTest.cpp
# includes them): the module system marks them as headers.
target_sources(SystemInfoTest PRIVATE SystemInfoTest.cpp)
target_include_directories(SystemInfoTest PRIVATE ${CMAKE_SOURCE_DIR}/modules)

target_compile_definitions(SystemInfoTest
    PRIVATE
        JUCE_USE_CURL=0
)

if(KICK_TEST_SANITIZER)
    target_compile_options(SystemInfoTest PRIVATE -fsanitize=${KICK_TEST_SANITIZER} -fno-omit-frame-pointer)
    target_link_options(SystemInfoTest PRIVATE -fsanitize=${KICK_TEST_SANITIZER})
endif()

target_link_libraries(SystemInfoTest
    PRIVATE
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
