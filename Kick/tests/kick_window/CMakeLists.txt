## Unit tests of the window's start-up options (see KickWindowTest.cpp).
## They create no window: no display is needed. -DKICK_TEST_SANITIZER=address
## (or undefined) builds them with that sanitizer.

project(KickWindowTest VERSION 1.0.0)

juce_add_console_app(KickWindowTest PRODUCT_NAME KickWindowTest)

set_target_properties(KickWindowTest PROPERTIES CXX_STANDARD 17)

# The module sources are compiled into the test (KickWindowTest.cpp
# includes them): the module system marks them as headers.
target_sources(KickWindowTest PRIVATE KickWindowTest.cpp)
target_include_directories(KickWindowTest PRIVATE ${CMAKE_SOURCE_DIR}/modules)

target_compile_definitions(KickWindowTest
    PRIVATE
        JUCE_USE_CURL=0
)

if(KICK_TEST_SANITIZER)
    target_compile_options(KickWindowTest PRIVATE -fsanitize=${KICK_TEST_SANITIZER} -fno-omit-frame-pointer)
    target_link_options(KickWindowTest PRIVATE -fsanitize=${KICK_TEST_SANITIZER})
endif()

target_link_libraries(KickWindowTest
    PRIVATE
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
