add_executable(epoll-test epoll-test.c)
target_link_libraries(epoll-test PRIVATE epoll-shim)

add_executable(expire-five expire-five.c)
target_link_libraries(expire-five PRIVATE epoll-shim)

add_executable(kqueue-state kqueue-state.c)
target_include_directories(kqueue-state PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../include")
