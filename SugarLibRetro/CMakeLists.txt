cmake_minimum_required(VERSION 3.0)
project(SugarLibRetro)

file( GLOB SRCS
	"*.cpp"
	)

file( GLOB HEADERS
	"*.h"
)

add_library(SugarLibRetro SHARED ${SRCS} ${HEADERS}  )

# The CPC firmware and the Plus/GX4000 system cartridge are compiled in by
# default so the core runs with an empty system directory. Turn this off to
# leave them out (about 400 KB smaller); the files must then be supplied under
# <system directory>/amstradcpc/ROM/.
option(SUGARBOX_EMBED_ROMS "Compile the CPC firmware into the core" ON)
if(SUGARBOX_EMBED_ROMS)
   target_compile_definitions(SugarLibRetro PRIVATE SUGARBOX_EMBED_ROMS=1)
else()
   target_compile_definitions(SugarLibRetro PRIVATE SUGARBOX_EMBED_ROMS=0)
endif()
target_include_directories( SugarLibRetro PUBLIC ${CPCCORE})
set_target_properties(${LIBRARY_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(CompilerFlags
        CMAKE_CXX_FLAGS
        CMAKE_CXX_FLAGS_DEBUG
        CMAKE_CXX_FLAGS_RELEASE
        CMAKE_CXX_FLAGS_RELWITHDEBINFO
        CMAKE_C_FLAGS
        CMAKE_C_FLAGS_DEBUG
        CMAKE_C_FLAGS_RELEASE
        )

set(CMAKE_CXX_STANDARD 17)
 
IF(MSVC)
   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /std:c++17")  
ENDIF(MSVC)

if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	set ( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --std=c++1z" )
endif()

foreach(CompilerFlag ${CompilerFlags})
	string(REPLACE "/MD" "/MT" ${CompilerFlag} "${${CompilerFlag}}")
endforeach()

IF(CMAKE_BUILD_TYPE MATCHES "Debug")
include_directories (SugarLibRetro
					 ${ZLIBDIR}/lib
                ${CpcCoreEmu_BINARY_DIR}/Debug
					 )
ELSE(CMAKE_BUILD_TYPE MATCHES "Release")
include_directories (SugarLibRetro
					 ${ZLIBDIR}/lib
                ${CpcCoreEmu_BINARY_DIR}/Release
					 )
ENDIF(CMAKE_BUILD_TYPE MATCHES "Debug")

if(MSVC)
	target_link_libraries ( SugarLibRetro zlibstatic CPCCoreEmu)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /std:c++17")  
	ADD_DEFINITIONS(/arch:SSE2)
else ()
   if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      target_link_libraries ( SugarLibRetro  m CPCCoreEmu zlibstatic)
   else()
      target_link_libraries ( SugarLibRetro  m stdc++fs CPCCoreEmu zlibstatic)
   endif()
endif()

