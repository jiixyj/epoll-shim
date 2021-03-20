if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(epoll-shim INTERFACE)
  add_library(epoll-shim::epoll-shim ALIAS epoll-shim)
  return()
endif()

set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN 1)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

find_package(tree-macros REQUIRED)
find_package(queue-macros REQUIRED)


macro(add_compat_target _name _condition)
  add_library(compat_${_name} STATIC compat_${_name}.c)
  target_compile_options(
    compat_${_name}
    INTERFACE "SHELL:-include \"${CMAKE_CURRENT_LIST_DIR}/compat_${_name}.h\"")
  add_library(compat_enable_${_name} INTERFACE)
  if(${_condition})
    target_link_libraries(compat_enable_${_name} INTERFACE compat_${_name})
    string(TOUPPER "${_name}" _upper_name)
    target_compile_definitions(compat_enable_${_name}
                               INTERFACE COMPAT_ENABLE_${_upper_name})
  endif()
endmacro()

include(CheckSymbolExists)
check_symbol_exists(kqueue1 "sys/event.h;sys/time.h" HAVE_KQUEUE1)
add_compat_target(kqueue1 "NOT;HAVE_KQUEUE1")


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
                     queue-macros::queue-macros tree-macros::tree-macros #
                     compat_enable_kqueue1)
target_include_directories(
  epoll-shim
  PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
if(NOT HAVE_EVENTFD)
  target_include_directories(
    epoll-shim PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/eventfd>)
endif()

set_target_properties(epoll-shim PROPERTIES SOVERSION 0)
