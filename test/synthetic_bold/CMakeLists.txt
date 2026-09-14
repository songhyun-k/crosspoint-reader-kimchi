add_executable(SyntheticBoldTest
  SyntheticBoldTest.cpp
  ${REPO_ROOT}/lib/GfxRenderer/GfxRenderer.cpp
  ${REPO_ROOT}/lib/GfxRenderer/FontCacheManager.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
)
target_include_directories(SyntheticBoldTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/test/xtc_memory/stubs
  ${REPO_ROOT}/lib/GfxRenderer ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8 ${REPO_ROOT}/lib/MiniBidi ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/InflateReader ${REPO_ROOT}/lib/uzlib/src
)
target_link_libraries(SyntheticBoldTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_compile_options(SyntheticBoldTest PRIVATE -ffunction-sections -fdata-sections)
if(APPLE)
  target_link_options(SyntheticBoldTest PRIVATE -Wl,-dead_strip)
else()
  target_link_options(SyntheticBoldTest PRIVATE -Wl,--gc-sections)
endif()
gtest_discover_tests(SyntheticBoldTest)
