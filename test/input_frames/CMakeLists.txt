add_executable(InputFramesTest InputFramesTest.cpp
  ${REPO_ROOT}/lib/hal/HalGPIOInput.cpp
  ${REPO_ROOT}/freeink-sdk/libs/hardware/InputManager/src/InputManager.cpp)
target_include_directories(InputFramesTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/freeink-sdk/libs/hardware/InputManager/include
  ${REPO_ROOT}/freeink-sdk/libs/hardware/BoardConfig/include)
target_compile_definitions(InputFramesTest PRIVATE FREEINK_DEVICE_X4=1 FREEINK_DEVICE_X3=1)
target_link_libraries(InputFramesTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(InputFramesTest)
