cmake_minimum_required(VERSION 3.9)
project(epoll-shim C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS ON)

add_subdirectory(src)
add_subdirectory(test)
