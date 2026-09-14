file(STRINGS "${REPO_ROOT}/platformio.ini" KIMCHI_VERSION_LINE REGEX "^version = ")
string(REPLACE "version = " "" KIMCHI_VERSION "${KIMCHI_VERSION_LINE}")

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
    OTA_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/latest.json"
    TEST_BOARD_NAME="${BOARD}"
  )
  target_link_libraries(${TARGET} PRIVATE crosspoint_test_common GTest::gtest_main)
  gtest_discover_tests(${TARGET} TEST_PREFIX "${BOARD}.")
endforeach()
