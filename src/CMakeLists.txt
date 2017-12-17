set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_library(epoll-shim SHARED epoll.c timerfd.c signalfd.c common.c)
target_link_libraries(epoll-shim PUBLIC Threads::Threads rt)
target_include_directories(epoll-shim PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../include")

set_target_properties(epoll-shim PROPERTIES
  LINK_FLAGS "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/../Version.map")
set_target_properties(epoll-shim PROPERTIES SOVERSION 0)
