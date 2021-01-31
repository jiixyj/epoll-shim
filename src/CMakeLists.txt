set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

find_package(tree-macros REQUIRED)
find_package(queue-macros REQUIRED)

add_library(
  epoll-shim
  epoll_shim_ctx.c
  epoll.c
  epollfd_ctx.c
  kqueue_event.c
  timerfd.c
  timerfd_ctx.c
  signalfd.c
  signalfd_ctx.c
  timespec_util.c)
if(NOT HAVE_EVENTFD)
  target_sources(epoll-shim PRIVATE eventfd.c eventfd_ctx.c)
endif()
include(GenerateExportHeader)
generate_export_header(epoll-shim BASE_NAME epoll_shim)
target_link_libraries(
  epoll-shim PRIVATE Threads::Threads #
                     queue-macros::queue-macros tree-macros::tree-macros)
target_include_directories(
  epoll-shim
  PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
if(NOT HAVE_EVENTFD)
  target_include_directories(
    epoll-shim PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/eventfd>)
endif()

set_target_properties(epoll-shim PROPERTIES SOVERSION 0)
