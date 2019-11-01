cmake_minimum_required(VERSION 3.14)
project(epoll-shim-tests LANGUAGES C)

find_package(epoll-shim REQUIRED)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_executable(epoll-test epoll-test.c)
target_link_libraries(epoll-test
                      PRIVATE epoll-shim::epoll-shim Threads::Threads)
add_test(NAME epoll-test COMMAND epoll-test)

add_executable(expire-five expire-five.c)
target_link_libraries(expire-five PRIVATE epoll-shim::epoll-shim)
add_test(NAME expire-five COMMAND expire-five)

add_executable(kqueue-state kqueue-state.c)
target_include_directories(kqueue-state
                           PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../include")
add_test(NAME kqueue-state COMMAND kqueue-state)

add_executable(many-timers many-timers.c)
target_link_libraries(many-timers PRIVATE epoll-shim::epoll-shim)
add_test(NAME many-timers COMMAND many-timers)

add_executable(tst-eventfd tst-eventfd.c)
target_link_libraries(tst-eventfd
                      PRIVATE epoll-shim::epoll-shim Threads::Threads)
add_test(NAME tst-eventfd COMMAND tst-eventfd)
