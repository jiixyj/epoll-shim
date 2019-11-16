cmake_minimum_required(VERSION 3.10)
project(epoll-shim-tests LANGUAGES C)

#

option(USE_SYSTEM_PRIVATE_ATF "" OFF)

#

include(CTest)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(ATFTest)

#

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(epoll-shim REQUIRED)
else()
  add_library(epoll-shim INTERFACE)
  add_library(epoll-shim::epoll-shim ALIAS epoll-shim)
endif()

add_library(atf-c INTERFACE)
add_library(atf::atf-c ALIAS atf-c)
if(USE_SYSTEM_PRIVATE_ATF)
  target_link_libraries(atf-c INTERFACE privateatf-c)
else()
  target_include_directories(atf-c
                             INTERFACE "${CMAKE_CURRENT_LIST_DIR}/microatf")
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_executable(epoll-test epoll-test.c)
target_link_libraries(epoll-test PRIVATE epoll-shim::epoll-shim
                                         Threads::Threads)
add_test(NAME epoll-test COMMAND epoll-test)

add_executable(timerfd-test timerfd-test.c)
target_link_libraries(timerfd-test PRIVATE epoll-shim::epoll-shim atf::atf-c)
atf_discover_tests(timerfd-test)

add_executable(tst-eventfd tst-eventfd.c)
target_link_libraries(tst-eventfd PRIVATE epoll-shim::epoll-shim
                                          Threads::Threads)
add_test(NAME tst-eventfd COMMAND tst-eventfd)
if(ENABLE_COMPILER_WARNINGS)
  target_compile_options(tst-eventfd PRIVATE "-w")
endif()

add_executable(perf-many-fds perf-many-fds.c)
target_link_libraries(perf-many-fds PRIVATE epoll-shim::epoll-shim atf::atf-c)
atf_discover_tests(perf-many-fds)

add_executable(epoll-test-atf epoll-test-atf.c)
target_link_libraries(epoll-test-atf PRIVATE epoll-shim::epoll-shim atf::atf-c)
atf_discover_tests(epoll-test-atf)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(kqueue-state kqueue-state.c)
  target_include_directories(kqueue-state
                             PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../include")
  target_link_options(kqueue-state PRIVATE
                      "LINKER:--unresolved-symbols=ignore-all")
  add_test(NAME kqueue-state COMMAND kqueue-state)

  add_executable(eventfd-ctx-test eventfd-ctx-test.c)
  target_link_libraries(eventfd-ctx-test PRIVATE Threads::Threads)
  target_include_directories(eventfd-ctx-test
                             PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
  add_test(NAME eventfd-ctx-test COMMAND eventfd-ctx-test)
endif()
