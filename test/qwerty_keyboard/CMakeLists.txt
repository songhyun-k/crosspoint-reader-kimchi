add_executable(QwertyKeyboardTest
  QwertyKeyboardTest.cpp
  ${REPO_ROOT}/src/activities/util/KeyboardLayoutSet.cpp
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/src/FreeInkUI.cpp
)
target_include_directories(QwertyKeyboardTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/activities/util
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include
)
target_link_libraries(QwertyKeyboardTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(QwertyKeyboardTest)
