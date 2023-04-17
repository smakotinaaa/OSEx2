cmake_minimum_required(VERSION 3.22)
project(ex2)

set(CMAKE_CXX_STANDARD 11)

add_executable(ex2 demo_itimer.c uthreads.cpp uthreads.h)
