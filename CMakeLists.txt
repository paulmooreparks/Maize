cmake_minimum_required(VERSION 3.20)
project(Maize)

set(CMAKE_CXX_STANDARD 20)

add_executable(maize main.cpp cpu.cpp sys.cpp)
