cmake_minimum_required(VERSION 3.10)
project(EdgeDaemon)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include_directories(src)

add_library(core_lib src/differential_privacy.cpp)

add_executable(edge-daemon src/main.cpp)
target_link_libraries(edge-daemon core_lib)

enable_testing()
add_executable(edge-daemon-tests tests/differential_privacy_test.cpp)
target_link_libraries(edge-daemon-tests core_lib)
add_test(NAME PropertyBasedDPTests COMMAND edge-daemon-tests)
