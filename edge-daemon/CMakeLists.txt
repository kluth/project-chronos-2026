cmake_minimum_required(VERSION 3.10)
project(EdgeDaemon)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

find_package(SQLite3 REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gio-2.0)
pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk+-3.0)

include_directories(src)

add_library(core_lib src/differential_privacy.cpp)
target_link_libraries(core_lib PUBLIC SQLite::SQLite3 PkgConfig::GLIB)

add_executable(edge-daemon src/main.cpp)
target_link_libraries(edge-daemon core_lib PkgConfig::GLIB pthread)

add_executable(chronos-applet src/applet.cpp)
target_link_libraries(chronos-applet PkgConfig::GTK PkgConfig::GLIB)

enable_testing()
add_executable(edge-daemon-tests tests/differential_privacy_test.cpp)
target_link_libraries(edge-daemon-tests core_lib PkgConfig::GLIB)
add_test(NAME PropertyBasedDPTests COMMAND edge-daemon-tests)

