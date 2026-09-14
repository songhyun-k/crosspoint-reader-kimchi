find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(GLOB TRANSLATION_SOURCES CONFIGURE_DEPENDS "${REPO_ROOT}/lib/I18n/translations/*.yaml")
set(I18N_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${I18N_GENERATED_DIR}")
set(I18N_GENERATED_FILES
  "${I18N_GENERATED_DIR}/I18nKeys.h"
  "${I18N_GENERATED_DIR}/I18nStrings.h"
  "${I18N_GENERATED_DIR}/I18nStrings.cpp"
)
add_custom_command(OUTPUT ${I18N_GENERATED_FILES}
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/gen_i18n.py
    ${REPO_ROOT}/lib/I18n/translations ${I18N_GENERATED_DIR}
    --src-dirs ${REPO_ROOT}/src ${REPO_ROOT}/lib --strip-unused
  DEPENDS ${REPO_ROOT}/scripts/gen_i18n.py ${TRANSLATION_SOURCES}
  VERBATIM
)
add_executable(I18nTest I18nTest.cpp ${REPO_ROOT}/lib/I18n/I18n.cpp ${I18N_GENERATED_FILES})
target_include_directories(I18nTest BEFORE PRIVATE ${I18N_GENERATED_DIR} ${REPO_ROOT}/lib/I18n)
target_link_libraries(I18nTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(I18nTest)
add_test(NAME KoreanTranslationsTest COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_translations.py)
