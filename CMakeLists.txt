cmake_minimum_required(VERSION 2.8) 

set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_COLOR_MAKEFILE   ON)

project(can2udp C)
ADD_DEFINITIONS(--std=gnu11 -O2 -pedantic -Wall -Werror -Wmissing-declarations)

add_executable(can2udp main.c)
set_property(TARGET can2udp PROPERTY C_STANDARD 11)
target_link_libraries(can2udp pthread)
