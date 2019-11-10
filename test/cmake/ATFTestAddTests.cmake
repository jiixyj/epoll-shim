# This is loosely based on `GoogleTestAddTests.cmake` from CMake.
#
# See `Copyright.txt` for license details.

set(script)

function(add_command _name)
  set(_args "")
  foreach(_arg ${ARGN})
    set(_args "${_args} ${_arg}")
  endforeach()
  set(script "${script}${_name}(${_args})\n" PARENT_SCOPE)
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
    add_command(add_test
                "${TEST_TARGET}.${test}"
                "${TEST_EXECUTABLE}"
                "${test}")
  endif()
endforeach()

file(WRITE "${CTEST_FILE}" "${script}")
