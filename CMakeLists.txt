cmake_minimum_required(VERSION 3.9)
project(epoll-shim C)

include(CTest)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS ON)

add_subdirectory(src)

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
