cmake_minimum_required(VERSION 3.16)
project(SugarLibRetroTests)

include(GoogleTest)

# display_geometry.h is header-only and pure, so the tests need no core, no
# frontend callbacks and no CPCCoreEmu -- just the header.
add_executable(sugarlibretro_tests test_display_geometry.cpp)
target_include_directories(sugarlibretro_tests PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR}/..
   ${gtest_SOURCE_DIR}/include
   ${gtest_SOURCE_DIR})
target_link_libraries(sugarlibretro_tests gtest gtest_main)
set_property(TARGET sugarlibretro_tests PROPERTY CXX_STANDARD 17)

gtest_discover_tests(sugarlibretro_tests)
