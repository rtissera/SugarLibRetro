cmake_minimum_required(VERSION 3.16)

# Nom du projet
project(SugarLibRetro)

set (USE_SHARED_CRT OFF)


set (CPCCORE			${PROJECT_SOURCE_DIR}/CPCCore/CPCCoreEmu 		CACHE STRING "CPCCoreEmu folder"		FORCE)
set (ZLIB				${PROJECT_SOURCE_DIR}/CPCCore/zlib 		CACHE STRING "zlib folder"		FORCE)

# Remove Romantic Robot's Multiface II firmware from the build before
# CPCCoreEmu is configured. CPCCoreEmu globs every *.cpp, so MultifaceII.cpp --
# and the ROM byte array it #includes -- ends up compiled into our binary even
# though the Multiface is not wired to anything. MIT cannot relicense somebody
# else's firmware and the buildbot publishes binaries. See the script header.
find_package(Python3 COMPONENTS Interpreter)
if (Python3_Interpreter_FOUND)
   execute_process(
      COMMAND ${Python3_EXECUTABLE}
              ${PROJECT_SOURCE_DIR}/SugarLibRetro/tools/blank_multiface_rom.py
              --cpccore ${CPCCORE}
      RESULT_VARIABLE blank_multiface_result
      OUTPUT_VARIABLE blank_multiface_output)
   message(STATUS "Multiface ROM: ${blank_multiface_output}")
   if (NOT blank_multiface_result EQUAL 0)
      message(FATAL_ERROR "Failed to blank the bundled Multiface II firmware")
   endif()
else()
   message(WARNING
      "Python 3 not found: the bundled Multiface II firmware will be compiled "
      "into the binary. Run SugarLibRetro/tools/blank_multiface_rom.py manually.")
endif()

# This core writes the CPC key matrix itself (see kKeyMap in libretro.cpp), so
# it never reads CPCCoreEmu's host scancode tables. Skipping them drops the
# per-OS Keyboards/ data files from what has to ship, which otherwise means a
# different file per target -- 101_keyboard_win, 101_keyboard_linux, and one
# more for anything else.
set(CPCCORE_NO_HOST_KEYBOARD ON CACHE BOOL "" FORCE)

# zlib registers its own CTest suite, which turns up in our results the
# moment enable_testing() is called at the top level. We do not test zlib.
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

add_subdirectory(${CPCCORE})
add_subdirectory(${ZLIB})

add_subdirectory(SugarLibRetro)



# Tests are opt-in: the REG-Linux package build and the libretro buildbot only
# want the core, and configuring googletest for them would be pure cost.
#   cmake -DSUGARLIBRETRO_BUILD_TESTS=ON && ctest
option(SUGARLIBRETRO_BUILD_TESTS "Build the unit tests" OFF)

if (SUGARLIBRETRO_BUILD_TESTS)
   # enable_testing() has to be called at the TOP level or ctest finds no
   # tests to run from the build root, however many gtest_discover_tests()
   # calls the subdirectories make.
   enable_testing()

   # This project rewrites /MD to /MT (see SugarLibRetro/CMakeLists.txt), so
   # googletest must be built against the static runtime too or MSVC fails to
   # link with LNK2038. Must be set BEFORE the subdirectory is added.
   set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
   set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
   set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
   add_subdirectory(${PROJECT_SOURCE_DIR}/CPCCore/googletest googletest)

   add_subdirectory(SugarLibRetro/tests)
endif()
