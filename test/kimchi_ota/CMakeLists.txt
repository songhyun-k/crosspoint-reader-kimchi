file(STRINGS "${REPO_ROOT}/platformio.ini" KIMCHI_VERSION_LINE REGEX "^version = ")
string(REPLACE "version = " "" KIMCHI_VERSION "${KIMCHI_VERSION_LINE}")
if(NOT KIMCHI_VERSION MATCHES "^([0-9]+)\.([0-9]+)\.([0-9]+)-kimchi\.([0-9]+)$")
  message(FATAL_ERROR "Unexpected Kimchi version format: ${KIMCHI_VERSION}")
endif()
set(KIMCHI_MAJOR "${CMAKE_MATCH_1}")
set(KIMCHI_MINOR "${CMAKE_MATCH_2}")
set(KIMCHI_PATCH "${CMAKE_MATCH_3}")
set(KIMCHI_REVISION "${CMAKE_MATCH_4}")
math(EXPR KIMCHI_TEST_REVISION "${KIMCHI_REVISION} + 1")
set(KIMCHI_TEST_LATEST_VERSION
    "v${KIMCHI_MAJOR}.${KIMCHI_MINOR}.${KIMCHI_PATCH}-kimchi.${KIMCHI_TEST_REVISION}")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/latest.json.in"
  "${CMAKE_CURRENT_BINARY_DIR}/latest.json"
  @ONLY
)

foreach(BOARD IN ITEMS x4 sticky x4pro papermono)
  string(TOUPPER "${BOARD}" BOARD_FLAG)
  set(TARGET "KimchiOtaTest_${BOARD}")
  add_executable(${TARGET}
    KimchiOtaTest.cpp
    ${REPO_ROOT}/src/network/KimchiRelease.cpp
    ${REPO_ROOT}/src/network/OtaUpdater.cpp
    ${REPO_ROOT}/src/network/FirmwareBoardTag.cpp
    ${REPO_ROOT}/lib/JsonParser/ReleaseJsonParser.cpp
    ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  )
  target_include_directories(${TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${REPO_ROOT}/test/xtc_memory/stubs
    ${REPO_ROOT}/src/network
    ${REPO_ROOT}/lib/JsonParser
  )
  target_compile_definitions(${TARGET} PRIVATE
    FREEINK_DEVICE_${BOARD_FLAG}=1
    CROSSPOINT_RELEASE_VERSION="${KIMCHI_VERSION}"
    CROSSPOINT_VERSION="${KIMCHI_VERSION}-dev-test-${BOARD}"
    OTA_FIXTURE_PATH="${CMAKE_CURRENT_BINARY_DIR}/latest.json"
    TEST_BOARD_NAME="${BOARD}"
    TEST_CURRENT_VERSION="${KIMCHI_VERSION}"
    TEST_LATEST_VERSION="${KIMCHI_TEST_LATEST_VERSION}"
  )
  target_link_libraries(${TARGET} PRIVATE crosspoint_test_common GTest::gtest_main)
  gtest_discover_tests(${TARGET} TEST_PREFIX "${BOARD}.")
endforeach()
