cmake_minimum_required(VERSION 3.8)
project(Maize)

set(CMAKE_CXX_STANDARD 20)

add_executable(maize "src/main.cpp" "src/cpu.cpp" "src/sys.cpp")
add_executable(mazm "src/mazm.cpp" "src/cpu.cpp" "src/sys.cpp")

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET maize PROPERTY CXX_STANDARD 20)
  set_property(TARGET mazm PROPERTY CXX_STANDARD 20)
endif()