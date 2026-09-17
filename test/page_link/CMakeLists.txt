add_executable(PageLinkTest PageLinkTest.cpp)
target_include_directories(PageLinkTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/test/korean_layout/stubs
  ${REPO_ROOT}/test/xtc_memory/stubs
  ${REPO_ROOT}/lib/Epub)
target_link_libraries(PageLinkTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PageLinkTest)
