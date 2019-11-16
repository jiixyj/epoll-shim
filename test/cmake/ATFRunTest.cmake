#[[
TEST_FOLDER_NAME
TEST_EXECUTABLE
TEST_NAME
BINARY_DIR
TIMEOUT
#]]

set(_wd "${BINARY_DIR}/${TEST_FOLDER_NAME}")

execute_process(COMMAND "${CMAKE_COMMAND}" -E remove_directory "${_wd}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E   make_directory "${_wd}/work")
execute_process(COMMAND "${CMAKE_COMMAND}" -E            touch "${_wd}/result")

execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -E env --unset=LANG --unset=LC_ALL --unset=LC_COLLATE
    --unset=LC_CTYPE --unset=LC_MESSAGES --unset=LC_MONETARY --unset=LC_NUMERIC
    --unset=LC_TIME "HOME=${_wd}/work" "TMPDIR=${_wd}/work" "TZ=UTC"
    "__RUNNING_INSIDE_ATF_RUN=internal-yes-value"
    "${TEST_EXECUTABLE}" -r "${_wd}/result" "${TEST_NAME}"
  WORKING_DIRECTORY "${_wd}/work"
  TIMEOUT "${TIMEOUT}"
  RESULT_VARIABLE _result
  ERROR_FILE "${_wd}/stderr")

file(STRINGS "${_wd}/result" _result_line)
file(STRINGS "${_wd}/stderr" _stderr_lines)

execute_process(COMMAND "${CMAKE_COMMAND}" -E remove_directory "${_wd}")

list(LENGTH _result_line _result_line_length)
if(_result_line_length GREATER 1)
  message(FATAL_ERROR "Result must not consist of multiple lines!")
endif()

message(STATUS "result: ${_result}, ${_result_line}")

#[[
"expected_death", -1, &formatted
"expected_exit", exitcode, &formatted
"expected_failure", -1, reason
"expected_signal", signo, &formatted
"expected_timeout", -1, &formatted
"failed", -1, reason
"passed", -1, NULL
"skipped", -1, reason
#]]

if(_result_line MATCHES "^passed$")
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "")
  endif()

elseif(_result_line MATCHES "^failed: (.*)$")
  message(FATAL_ERROR "${CMAKE_MATCH_1}")

elseif(_result_line MATCHES "^skipped: (.*)$")
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "")
  endif()

elseif(_result_line MATCHES "^expected_timeout: (.*)$")
  if(NOT _result STREQUAL "Process terminated due to timeout")
    message(FATAL_ERROR "")
  endif()

elseif(_result_line MATCHES "^expected_failure: (.*)$")
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "")
  endif()

elseif(_result_line MATCHES "^expected_death: (.*)$")

elseif(_result_line MATCHES "^expected_exit\\((.*)\\): (.*)$")
  if(NOT _result EQUAL "${CMAKE_MATCH_1}")
    message(FATAL_ERROR "")
  endif()

elseif(_result_line MATCHES "^expected_signal\\((.*)\\): (.*)$")
  if(NOT _result EQUAL 1)
    message(FATAL_ERROR "")
  endif()

  list(LENGTH _stderr_lines _stderr_lines_length)
  math(EXPR _last_index "${_stderr_lines_length} - 1")
  list(GET _stderr_lines _last_index _signal_line)

  if("${CMAKE_MATCH_1}" EQUAL 1) # SIGHUP
    if(NOT _signal_line STREQUAL "SIGHUP")
      message(FATAL_ERROR "")
    endif()
  else()
    # TODO(jan): Add other signals.
    message(FATAL_ERROR "")
  endif()

else()
  message(
    FATAL_ERROR
      "Unexpected result: \"${_result_line}\", process exited with: ${_result}")

endif()
