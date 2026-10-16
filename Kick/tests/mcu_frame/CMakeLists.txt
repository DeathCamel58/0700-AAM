## Unit test of the front-panel MCU frame parser (see McuFrameTest.cpp).
## -DKICK_TEST_SANITIZER=address (or undefined, thread) builds it with that
## sanitizer.

project(McuFrameTest VERSION 1.0.0)

add_executable(McuFrameTest McuFrameTest.cpp)

set_target_properties(McuFrameTest PROPERTIES CXX_STANDARD 17 RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(McuFrameTest PRIVATE ${CMAKE_SOURCE_DIR}/modules/input_events)

if(KICK_TEST_SANITIZER)
    target_compile_options(McuFrameTest PRIVATE -fsanitize=${KICK_TEST_SANITIZER} -fno-omit-frame-pointer)
    target_link_options(McuFrameTest PRIVATE -fsanitize=${KICK_TEST_SANITIZER})
endif()
