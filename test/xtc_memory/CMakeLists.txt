add_executable(XtcMemoryTest
  SdCardFontLifetimeTest.cpp
  XtcStreamingTest.cpp
  ${REPO_ROOT}/lib/Xtc/Xtc/XtcParser.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(XtcMemoryTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Xtc
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/test/fs_helpers/stubs
)
target_link_libraries(XtcMemoryTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(XtcMemoryTest)
