# This is loosely based on `GoogleTestAddTests.cmake` from CMake.
#
# See `Copyright.txt` for license details.

set(script "set(_counter 1)")

function(add_test_to_script
         _name
         _executable
         _test)

  set(_testscript "
set(_wd \"${BINARY_DIR}/${_name}.\${_counter}\")
add_test(${_name}.setup    ${CMAKE_COMMAND} -E   make_directory \"\${_wd}\")
add_test(${_name}.teardown ${CMAKE_COMMAND} -E remove_directory \"\${_wd}\")
add_test(${_name}          ${CMAKE_COMMAND} -E env
         --unset=LANG --unset=LC_ALL --unset=LC_COLLATE --unset=LC_CTYPE
         --unset=LC_MESSAGES --unset=LC_MONETARY --unset=LC_NUMERIC
         --unset=LC_TIME
         HOME=\"\${_wd}\" TMPDIR=\"\${_wd}\" TZ=UTC ${_executable} ${_test})
set_tests_properties(${_name} PROPERTIES WORKING_DIRECTORY \"\${_wd}\")

set_tests_properties(${_name}.setup     PROPERTIES FIXTURES_SETUP    ${_name}.f)
set_tests_properties(${_name}.teardown  PROPERTIES FIXTURES_CLEANUP  ${_name}.f)
set_tests_properties(${_name}           PROPERTIES FIXTURES_REQUIRED ${_name}.f)

math(EXPR _counter \"\${_counter}+1\")
")

  set(script "${script}${_testscript}" PARENT_SCOPE)
endfunction()

if(NOT EXISTS "${TEST_EXECUTABLE}")
  message(FATAL_ERROR "Specified test executable does not exist.\n"
                      "  Path: '${TEST_EXECUTABLE}'")
endif()

execute_process(COMMAND "${TEST_EXECUTABLE}" -l
                WORKING_DIRECTORY "${TEST_WORKING_DIR}"
                OUTPUT_VARIABLE output
                RESULT_VARIABLE result)

if(NOT ${result} EQUAL 0)
  string(REPLACE "\n"
                 "\n    "
                 output
                 "${output}")
  message(FATAL_ERROR "Error running test executable.\n"
                      "  Path: '${TEST_EXECUTABLE}'\n"
                      "  Result: ${result}\n"
                      "  Output:\n"
                      "    ${output}\n")
endif()

string(REPLACE "\n"
               ";"
               output
               "${output}")

foreach(line ${output})
  if(line MATCHES "^ident: ")
    string(REGEX
           REPLACE "^ident: "
                   ""
                   test
                   "${line}")
    add_test_to_script("${TEST_TARGET}.${test}" "${TEST_EXECUTABLE}" "${test}")
  endif()
endforeach()

file(WRITE "${CTEST_FILE}" "${script}")
