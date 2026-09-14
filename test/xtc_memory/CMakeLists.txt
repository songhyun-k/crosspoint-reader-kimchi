get_target_property(RENDERER_TEST_SOURCES SyntheticBoldTest SOURCES)
list(FILTER RENDERER_TEST_SOURCES INCLUDE REGEX "^/")
get_target_property(RENDERER_TEST_INCLUDES SyntheticBoldTest INCLUDE_DIRECTORIES)
add_executable(XtcMemoryTest
  SdCardFontLifetimeTest.cpp
  XtcStreamingTest.cpp
  ${REPO_ROOT}/lib/Xtc/Xtc/XtcParser.cpp
  ${RENDERER_TEST_SOURCES}
)
target_include_directories(XtcMemoryTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${RENDERER_TEST_INCLUDES}
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Xtc
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/test/fs_helpers/stubs
)
target_link_libraries(XtcMemoryTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_compile_options(XtcMemoryTest PRIVATE -ffunction-sections -fdata-sections)
if(APPLE)
  target_link_options(XtcMemoryTest PRIVATE -Wl,-dead_strip)
else()
  target_link_options(XtcMemoryTest PRIVATE -Wl,--gc-sections)
endif()
gtest_discover_tests(XtcMemoryTest)
