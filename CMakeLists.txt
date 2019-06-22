cmake_minimum_required(VERSION 3.14)
project(epoll-shim LANGUAGES C)

option(BUILD_SHARED_LIBS "build libepoll-shim as shared lib" ON)

include(CTest)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS ON)

add_subdirectory(src)

if(PROJECT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  if(BUILD_TESTING)
    add_subdirectory(test)
  endif()

  include(GNUInstallDirs)

  set(CMAKE_INSTALL_PKGCONFIGDIR
      "libdata/pkgconfig"
      CACHE PATH "Installation directory for pkgconfig (.pc) files")
  mark_as_advanced(CMAKE_INSTALL_PKGCONFIGDIR)

  install(TARGETS epoll-shim LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
  install(FILES "${PROJECT_SOURCE_DIR}/include/sys/epoll.h" #
                "${PROJECT_SOURCE_DIR}/include/sys/signalfd.h"
                "${PROJECT_SOURCE_DIR}/include/sys/timerfd.h"
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/libepoll-shim/sys")

  if(BUILD_SHARED_LIBS)
    configure_file("${PROJECT_SOURCE_DIR}/epoll-shim.pc.cmakein"
                   "${PROJECT_BINARY_DIR}/epoll-shim.pc" @ONLY)
    install(FILES "${PROJECT_BINARY_DIR}/epoll-shim.pc"
            DESTINATION "${CMAKE_INSTALL_PKGCONFIGDIR}")
  endif()
endif()
