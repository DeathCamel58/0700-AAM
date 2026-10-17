## Unit tests of what the performance overlay shows (see
## PerformanceOverlayTest.cpp): its histories, the /proc figures, the
## formatting, and the setting that turns it on.
## They create no window: no display is needed. -DKICK_TEST_SANITIZER=address
## (or undefined) builds them with that sanitizer.

project(PerformanceOverlayTest VERSION 1.0.0)

juce_add_console_app(PerformanceOverlayTest PRODUCT_NAME PerformanceOverlayTest)

set_target_properties(PerformanceOverlayTest PROPERTIES CXX_STANDARD 17)

# The module sources are compiled into the test (PerformanceOverlayTest.cpp
# includes them): the module system marks them as headers.
target_sources(PerformanceOverlayTest PRIVATE PerformanceOverlayTest.cpp)
target_include_directories(PerformanceOverlayTest PRIVATE ${CMAKE_SOURCE_DIR}/modules)

target_compile_definitions(PerformanceOverlayTest
    PRIVATE
        JUCE_USE_CURL=0
)

if(KICK_TEST_SANITIZER)
    target_compile_options(PerformanceOverlayTest PRIVATE -fsanitize=${KICK_TEST_SANITIZER} -fno-omit-frame-pointer)
    target_link_options(PerformanceOverlayTest PRIVATE -fsanitize=${KICK_TEST_SANITIZER})
endif()

target_link_libraries(PerformanceOverlayTest
    PRIVATE
        juce::juce_core
        juce::juce_data_structures
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
