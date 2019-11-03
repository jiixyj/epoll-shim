set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_library(epoll-shim
            epoll_shim_ctx.c
            epoll.c
            epollfd_ctx.c
            timerfd.c
            timerfd_ctx.c
            signalfd.c
            signalfd_ctx.c
            eventfd.c
            eventfd_ctx.c)
target_link_libraries(epoll-shim PRIVATE Threads::Threads rt)
target_include_directories(
  epoll-shim
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

target_link_options(epoll-shim PRIVATE
                    "LINKER:--version-script=${PROJECT_SOURCE_DIR}/Version.map")
set_target_properties(epoll-shim PROPERTIES SOVERSION 0)
