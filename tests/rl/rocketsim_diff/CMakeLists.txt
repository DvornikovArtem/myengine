# Standalone build of the generator of the reference traces; not part of the engine build (see README.md).
#   cmake -S tests/rl/rocketsim_diff -B <build dir> -DRS_DIR=<path to a checkout of RocketSim>
#   cmake --build <build dir> --config Release --target rsgen
#   <build dir>/Release/rsgen <output directory>
cmake_minimum_required(VERSION 3.15)
project(rsgen CXX)
set(CMAKE_CXX_STANDARD 20)
if(NOT RS_DIR)
  message(FATAL_ERROR "pass -DRS_DIR=<path to a checkout of github.com/ZealanL/RocketSim>")
endif()
add_subdirectory(${RS_DIR} rocketsim EXCLUDE_FROM_ALL)
add_executable(rsgen gen.cpp)
target_link_libraries(rsgen RocketSim)
target_include_directories(rsgen PRIVATE ${RS_DIR}/src)
target_compile_definitions(rsgen PRIVATE RS_DONT_LOG)
