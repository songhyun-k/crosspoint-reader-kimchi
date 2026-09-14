find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(GLOB UZLIB_SOURCES "${REPO_ROOT}/lib/uzlib/src/*.c")
add_executable(KoreanFontsTest
  KoreanFontsTest.cpp
  ${REPO_ROOT}/src/ReaderFontSizes.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/I18n/I18n.cpp
  ${CMAKE_BINARY_DIR}/i18n/generated/I18nStrings.cpp
  ${UZLIB_SOURCES}
)
add_dependencies(KoreanFontsTest I18nTest)
target_include_directories(KoreanFontsTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/test/xtc_memory/stubs
  ${CMAKE_BINARY_DIR}/i18n/generated
  ${REPO_ROOT}/src ${REPO_ROOT}/lib/I18n
  ${REPO_ROOT}/lib/EpdFont ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/InflateReader ${REPO_ROOT}/lib/uzlib/src
)
target_link_libraries(KoreanFontsTest PRIVATE crosspoint_test_common GTest::gtest_main)
# The firmware links only uzlib's raw inflater; checksum wrappers are deliberately
# absent from its trimmed library. Discard those unused functions on the host too.
target_compile_options(KoreanFontsTest PRIVATE -ffunction-sections -fdata-sections)
if(APPLE)
  target_link_options(KoreanFontsTest PRIVATE -Wl,-dead_strip)
else()
  target_link_options(KoreanFontsTest PRIVATE -Wl,--gc-sections)
endif()
gtest_discover_tests(KoreanFontsTest)
add_test(NAME KoreanFontCatalogTest COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_catalog.py)
