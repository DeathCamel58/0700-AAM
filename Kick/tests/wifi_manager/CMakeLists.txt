## Test of WifiManager against a stand-in wpa_supplicant (see
## WifiManagerTest.cpp). -DKICK_TEST_SANITIZER=address (or undefined, thread)
## builds it, juce_core included, with that sanitizer.

project(WifiManagerTest VERSION 1.0.0)

juce_add_console_app(WifiManagerTest PRODUCT_NAME WifiManagerTest)

set_target_properties(WifiManagerTest PROPERTIES CXX_STANDARD 17)

target_sources(WifiManagerTest PRIVATE WifiManagerTest.cpp)
target_include_directories(WifiManagerTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/hardware/source)

target_compile_definitions(WifiManagerTest
    PRIVATE
        JUCE_USE_CURL=0
)

if(KICK_TEST_SANITIZER)
    target_compile_options(WifiManagerTest PRIVATE -fsanitize=${KICK_TEST_SANITIZER} -fno-omit-frame-pointer)
    target_link_options(WifiManagerTest PRIVATE -fsanitize=${KICK_TEST_SANITIZER})
endif()

target_link_libraries(WifiManagerTest
    PRIVATE
        juce::juce_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)
