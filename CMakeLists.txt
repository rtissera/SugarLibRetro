cmake_minimum_required(VERSION 3.0)

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

add_subdirectory(${CPCCORE})
add_subdirectory(${ZLIB})

add_subdirectory(SugarLibRetro)


