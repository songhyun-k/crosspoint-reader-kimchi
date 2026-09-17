get_target_property(INPUT_RENDERER_SOURCES SyntheticBoldTest SOURCES)
list(FILTER INPUT_RENDERER_SOURCES INCLUDE REGEX "^/")
get_target_property(INPUT_I18N_INCLUDES I18nTest INCLUDE_DIRECTORIES)
add_executable(InputFramesTest InputFramesTest.cpp TaskBoundary.cpp
  ${REPO_ROOT}/src/MappedInputManager.cpp
  ${REPO_ROOT}/src/activities/reader/EpubPageTurns.cpp
  ${INPUT_RENDERER_SOURCES}
  ${REPO_ROOT}/lib/hal/HalGPIOInput.cpp
  ${REPO_ROOT}/lib/hal/HalTiltSensor.cpp
  ${REPO_ROOT}/freeink-sdk/libs/hardware/InputManager/src/InputManager.cpp)
target_include_directories(InputFramesTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/test/xtc_memory/stubs
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/freeink-sdk/libs/hardware/InputManager/include
  ${REPO_ROOT}/freeink-sdk/libs/hardware/Imu/include
  ${REPO_ROOT}/freeink-sdk/libs/hardware/BoardConfig/include
  ${REPO_ROOT}/freeink-sdk/libs/hardware/FrontlightManager/include
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include
  ${INPUT_I18N_INCLUDES}
  ${REPO_ROOT}/src ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/GfxRenderer ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8 ${REPO_ROOT}/lib/MiniBidi ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/InflateReader ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs)
target_compile_definitions(InputFramesTest PRIVATE FREEINK_DEVICE_X4=1 FREEINK_DEVICE_X3=1 TEST_EXTERNAL_CLOCK=1)
target_link_libraries(InputFramesTest PRIVATE crosspoint_test_common GTest::gtest_main)
add_dependencies(InputFramesTest I18nTest)
target_compile_options(InputFramesTest PRIVATE -ffunction-sections -fdata-sections)
if(APPLE)
  target_link_options(InputFramesTest PRIVATE -Wl,-dead_strip)
else()
  target_link_options(InputFramesTest PRIVATE -Wl,--gc-sections)
endif()
gtest_discover_tests(InputFramesTest)
