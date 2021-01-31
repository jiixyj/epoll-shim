cmake_minimum_required(VERSION 3.10)
project(epoll-shim-tests LANGUAGES C)

#

include(CTest)

#

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(epoll-shim REQUIRED)
else()
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../external/microatf" microatf)
  add_library(epoll-shim INTERFACE)
  add_library(epoll-shim::epoll-shim ALIAS epoll-shim)
endif()

find_package(microatf REQUIRED)
include(ATFTest)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

#

macro(atf_test _testname)
  add_executable("${_testname}" "${_testname}.c")
  target_link_libraries(
    "${_testname}" PRIVATE epoll-shim::epoll-shim Threads::Threads
                           microatf::microatf-c)
  atf_discover_tests("${_testname}" ${ARGN})
endmacro()

#

atf_test(epoll-test)
target_sources(epoll-test PRIVATE real_close.c)
atf_test(timerfd-test)
atf_test(timerfd-mock-test)
atf_test(signalfd-test)
atf_test(perf-many-fds)
atf_test(atf-test)
atf_test(eventfd-ctx-test)
atf_test(pipe-test)
atf_test(socketpair-test)
atf_test(malloc-fail-test)
target_link_libraries(malloc-fail-test PRIVATE ${CMAKE_DL_LIBS})
atf_test(tst-epoll)
atf_test(tst-timerfd)
add_executable(epoll-include-test epoll-include-test.c)
target_link_libraries(epoll-include-test PRIVATE epoll-shim::epoll-shim)
set_target_properties(
  epoll-include-test
  PROPERTIES C_STANDARD 99 #
             C_STANDARD_REQUIRED YES #
             C_EXTENSIONS NO)
add_test(NAME epoll-include-test COMMAND epoll-include-test)
