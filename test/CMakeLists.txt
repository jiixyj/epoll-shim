add_executable(epoll-test epoll-test.c)
target_link_libraries(epoll-test PRIVATE epoll-shim)

add_executable(expire-five expire-five.c)
target_link_libraries(expire-five PRIVATE epoll-shim)
