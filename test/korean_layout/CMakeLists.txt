# Share the real renderer sources and its memory-only HAL from F03. Text layout,
# TextBlock arenas, XML parsing and CSS are real production implementations.
get_target_property(RENDERER_TEST_SOURCES SyntheticBoldTest SOURCES)
list(FILTER RENDERER_TEST_SOURCES INCLUDE REGEX "^/")
get_target_property(RENDERER_TEST_INCLUDES SyntheticBoldTest INCLUDE_DIRECTORIES)
add_executable(KoreanLayoutTest KoreanLayoutTest.cpp ParserIoStubs.cpp
  ${RENDERER_TEST_SOURCES}
  ${REPO_ROOT}/src/activities/reader/EpubPageTurns.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Section.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Page.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageDimsProbe.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/Hyphenator.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp
)
target_include_directories(KoreanLayoutTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${RENDERER_TEST_INCLUDES}
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
  ${REPO_ROOT}/test/input_frames/stubs
  ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/lib/Serialization ${REPO_ROOT}/lib/XmlParserUtils
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation ${REPO_ROOT}/src
)
target_compile_definitions(KoreanLayoutTest PRIVATE CP_HYPHENATION_LANGS=0 XML_GE=0 XML_CONTEXT_BYTES=1024)
target_link_libraries(KoreanLayoutTest PRIVATE crosspoint_test_common GTest::gtest_main expat)
target_compile_options(KoreanLayoutTest PRIVATE -ffunction-sections -fdata-sections)
if(APPLE)
  target_link_options(KoreanLayoutTest PRIVATE -Wl,-dead_strip)
else()
  target_link_options(KoreanLayoutTest PRIVATE -Wl,--gc-sections)
endif()
gtest_discover_tests(KoreanLayoutTest)
