set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

set(INCLUDES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../include")

add_library(epoll-shim SHARED epoll.c timerfd.c signalfd.c common.c)
target_link_libraries(epoll-shim PUBLIC Threads::Threads rt)
target_include_directories(epoll-shim PUBLIC "${INCLUDES_DIR}")

set_target_properties(epoll-shim PROPERTIES
  LINK_FLAGS "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/../Version.map")
set_target_properties(epoll-shim PROPERTIES SOVERSION 0)


install(TARGETS epoll-shim LIBRARY             DESTINATION lib)
install(FILES "${INCLUDES_DIR}/sys/epoll.h"    DESTINATION include/libepoll-shim/sys)
install(FILES "${INCLUDES_DIR}/sys/signalfd.h" DESTINATION include/libepoll-shim/sys)
install(FILES "${INCLUDES_DIR}/sys/timerfd.h"  DESTINATION include/libepoll-shim/sys)
