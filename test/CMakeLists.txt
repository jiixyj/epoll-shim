cmake_minimum_required(VERSION 3.10)
project(epoll-shim-tests LANGUAGES C)

include(CTest)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(epoll-shim REQUIRED)
else()
  add_library(epoll-shim INTERFACE)
  add_library(epoll-shim::epoll-shim ALIAS epoll-shim)
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_executable(epoll-test epoll-test.c)
target_link_libraries(epoll-test
                      PRIVATE epoll-shim::epoll-shim Threads::Threads)
add_test(NAME epoll-test COMMAND epoll-test)

add_executable(expire-five expire-five.c)
target_link_libraries(expire-five PRIVATE epoll-shim::epoll-shim)
add_test(NAME expire-five COMMAND expire-five)

add_executable(many-timers many-timers.c)
target_link_libraries(many-timers PRIVATE epoll-shim::epoll-shim)
add_test(NAME many-timers COMMAND many-timers)

add_executable(tst-eventfd tst-eventfd.c)
target_link_libraries(tst-eventfd
                      PRIVATE epoll-shim::epoll-shim Threads::Threads)
add_test(NAME tst-eventfd COMMAND tst-eventfd)

add_executable(perf-many-fds perf-many-fds.c)
target_link_libraries(perf-many-fds PRIVATE epoll-shim::epoll-shim)
add_test(NAME perf-many-fds COMMAND perf-many-fds)

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
