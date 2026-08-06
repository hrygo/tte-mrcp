cmake_minimum_required (VERSION 2.8)
project (UniMRCP C)
enable_testing()

# CMake options
option (ENABLE_CLIENT_LIB          "Enable UniMRCP client library"                  ON)
option (ENABLE_CLIENT_APP          "Enable sample UniMRCP client application"       ON)
option (ENABLE_UMC_APP             "Enable sample UniMRCP client C++ application"   ON)
option (ENABLE_ASR_CLIENT          "Enable misc ASR client library and application" ON)

option (ENABLE_SERVER_LIB          "Enable UniMRCP server library"                  ON)
option (ENABLE_SERVER_APP          "Enable UniMRCP server application"              ON)

option (ENABLE_TTS_WEBSOCKET_PLUGIN    "Enable TTS WebSocket plugin"                 ON)
option (ENABLE_ASR_WEBSOCKET_PLUGIN "Enable ASR WebSocket plugin"                 ON)
option (ENABLE_DEMOVERIFIER_PLUGIN "Enable demo verifier plugin"                    ON)
option (ENABLE_RECORDER_PLUGIN     "Enable recorder plugin"                         ON)

option (ENABLE_AMR_CODEC           "Enable AMR Codec"                               OFF)

option (ENABLE_TINGYUN_APM         "Enable TingYun APM instrumentation"             ON)

option (ENABLE_TEST_SUITES         "Enable test suites"                             OFF)

# Organize targets into folders
set_property (GLOBAL PROPERTY USE_FOLDERS ON)

# Set source directories for dependencies
set (APR_SOURCE_DIR ${CMAKE_SOURCE_DIR}/libs/apr CACHE PATH "Path to APR source directory")
set (APU_SOURCE_DIR ${CMAKE_SOURCE_DIR}/libs/apr-util CACHE PATH "Path to APR-util source directory")
set (SOFIA_SOURCE_DIR ${CMAKE_SOURCE_DIR}/libs/sofia-sip CACHE PATH "Path to SofiaSIP source directory")

# Set CMake module path
set (CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/build/cmake)

set (APR_SOURCE_DIR ${CMAKE_SOURCE_DIR}/libs/apr CACHE PATH "Path to APR source directory")
set (APU_SOURCE_DIR ${CMAKE_SOURCE_DIR}/libs/apr-util CACHE PATH "Path to APR-util source directory")
set (SOFIA_SOURCE_DIR ${CMAKE_SOURCE_DIR}/libs/sofia-sip CACHE PATH "Path to SofiaSIP source directory")

# Set CMake module path
set (CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/build/cmake)

# Find dependencies through pkg-config. The development environment script
# supplies Homebrew's keg-only paths on macOS arm64.
find_package (PkgConfig REQUIRED)
pkg_check_modules (APR REQUIRED apr-1)
pkg_check_modules (APRUTIL REQUIRED apr-util-1)
pkg_check_modules (SOFIA REQUIRED sofia-sip-ua)

# Keep the historical APU_* names consumed by UniMRCP subprojects while
# sourcing the dependency data from the APR-util pkg-config probe above.
set (APU_INCLUDE_DIRS ${APRUTIL_INCLUDE_DIRS})
set (APU_LIBRARIES ${APRUTIL_LIBRARIES})
set (APU_DEFINES ${APRUTIL_CFLAGS_OTHER})

# Keep the compatibility variable names consumed by existing UniMRCP modules.
set (SofiaSIP_FOUND ${SOFIA_FOUND})
set (SofiaSIP_INCLUDE_DIRS ${SOFIA_INCLUDE_DIRS})
set (SofiaSIP_LIBRARIES ${SOFIA_LIBRARIES})
if (ENABLE_AMR_CODEC)
	find_package (OpenCoreAMR REQUIRED)
	find_package (VoAMRWBEnc REQUIRED)
endif()

# TingYun APM C SDK (auto-disabled on unsupported platforms).
# libs/tingyun/lib/ currently ships static libraries for:
#   x86_64  + glibc -> libtingyun_sdk.a
#   x86     + glibc -> libtingyun_sdk.x86.a
#   x86_64  + musl  -> libtingyun_musl.x64.a
set (TINGYUN_FOUND FALSE)
set (TINGYUN_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/libs/tingyun/include")
set (TINGYUN_LIB_DIR     "${CMAKE_SOURCE_DIR}/libs/tingyun/lib")
if (ENABLE_TINGYUN_APM)
	set (_tingyun_lib "")
	if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
		if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
			# Prefer musl build only when the compiler targets musl.
			# Default path uses glibc; users can override by editing this block.
			set (_tingyun_lib "libtingyun_sdk.a")
		elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "i.86|x86")
			set (_tingyun_lib "libtingyun_sdk.x86.a")
		endif ()
	endif ()
	if (_tingyun_lib AND EXISTS "${TINGYUN_INCLUDE_DIR}/tingyun.h"
			AND EXISTS "${TINGYUN_LIB_DIR}/${_tingyun_lib}")
		set (TINGYUN_FOUND TRUE)
		set (TINGYUN_LIBRARY "${TINGYUN_LIB_DIR}/${_tingyun_lib}")
		# Propagate preprocessor symbol to every target in this directory scope.
		add_compile_definitions (TINGYUN_ENABLED=1)
		message (STATUS "TingYun APM: enabled (${_tingyun_lib})")
	elseif (ENABLE_TINGYUN_APM)
		message (STATUS "TingYun APM: requested but SDK files not found for "
				"${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}; disabling")
		set (ENABLE_TINGYUN_APM OFF)
	endif ()
	unset (_tingyun_lib)
endif ()

# Set API definitions
set (APR_TOOLKIT_DEFINES -DAPT_STATIC_LIB)
set (MPF_DEFINES -DMPF_STATIC_LIB)
set (RTSP_DEFINES -DRTSP_STATIC_LIB)
set (MRCP_DEFINES -DMRCP_STATIC_LIB)


# Set API definitions
set (APR_TOOLKIT_DEFINES -DAPT_STATIC_LIB)
set (MPF_DEFINES -DMPF_STATIC_LIB)
set (RTSP_DEFINES -DRTSP_STATIC_LIB)
set (MRCP_DEFINES -DMRCP_STATIC_LIB)

# Set compiler flags
if (CMAKE_C_COMPILER_ID MATCHES MSVC)
	# Microsoft Visual Studio Compiler
	set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /W4 /WX /wd4100")
	set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4 /WX /wd4100")
elseif (CMAKE_C_COMPILER_ID MATCHES GNU)
	# GNU Compiler
	set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC -Wall -Werror")
	set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC -Wall -Werror")
endif ()

# Shared modules (plug-ins) should not have any prefix set
set (CMAKE_SHARED_MODULE_PREFIX "")

# Pertain RPATH upon installation 
set (CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# Add library directory to RPATH
set (CMAKE_INSTALL_RPATH ${CMAKE_INSTALL_PREFIX}/lib)

# Set default installation path
if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
	set (CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/installed" CACHE PATH "Default installation path" FORCE )
endif ()

# Set include directories
set (APR_TOOLKIT_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/libs/apr-toolkit/include)
set (MPF_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/libs/mpf/include)
set (RTSP_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/libs/uni-rtsp/include)
set (MRCP_INCLUDE_DIRS
	${CMAKE_SOURCE_DIR}/libs/mrcp/include
	${CMAKE_SOURCE_DIR}/libs/mrcp/control/include
	${CMAKE_SOURCE_DIR}/libs/mrcp/message/include
	${CMAKE_SOURCE_DIR}/libs/mrcp/resources/include)
set (MRCP_SIGNALING_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/libs/mrcp-signaling/include)
set (MRCPv2_TRANSPORT_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/libs/mrcpv2-transport/include)
set (MRCP_ENGINE_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/libs/mrcp-engine/include)
set (MRCP_CLIENT_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/libs/mrcp-client/include)
set (MRCP_SERVER_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/libs/mrcp-server/include)
set (MRCP_SOFIASIP_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/modules/mrcp-sofiasip/include)
set (MRCP_UNIRTSP_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/modules/mrcp-unirtsp/include)
set (UNIMRCP_CLIENT_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/platforms/libunimrcp-client/include)
set (UNIMRCP_SERVER_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/platforms/libunimrcp-server/include)
set (VERSION_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/build)

# Sub-projects: libraries
add_subdirectory (libs/apr-toolkit)
add_subdirectory (libs/mpf)
add_subdirectory (libs/uni-rtsp)
add_subdirectory (libs/mrcp)
add_subdirectory (libs/mrcp-signaling)
add_subdirectory (libs/mrcpv2-transport)
if (ENABLE_CLIENT_LIB)
add_subdirectory (libs/mrcp-client)
endif ()
if (ENABLE_SERVER_LIB)
add_subdirectory (libs/mrcp-engine)
add_subdirectory (libs/mrcp-server)
endif ()

# Sub-projects: modules
add_subdirectory (modules/mrcp-sofiasip)
add_subdirectory (modules/mrcp-unirtsp)

# Sub-projects: plug-ins
if (ENABLE_ASR_WEBSOCKET_PLUGIN)
add_subdirectory (plugins/asr-websocket)
endif ()
if (ENABLE_TTS_WEBSOCKET_PLUGIN)
add_subdirectory (plugins/tts-websocket)
endif ()
if (ENABLE_DEMOVERIFIER_PLUGIN)
add_subdirectory (plugins/demo-verifier)
endif ()
if (ENABLE_RECORDER_PLUGIN)
add_subdirectory (plugins/mrcp-recorder)
endif ()

# Sub-projects: platform libraries
if (ENABLE_CLIENT_LIB)
add_subdirectory (platforms/libunimrcp-client)
endif ()
if (ENABLE_SERVER_LIB)
add_subdirectory (platforms/libunimrcp-server)
endif ()

# Sub-projects: platform applications
if (ENABLE_CLIENT_LIB AND ENABLE_UMC_APP)
add_subdirectory (platforms/umc)
endif ()
if (ENABLE_CLIENT_LIB AND ENABLE_CLIENT_APP)
add_subdirectory (platforms/unimrcp-client)
endif ()
if (ENABLE_SERVER_LIB AND ENABLE_SERVER_APP)
add_subdirectory (platforms/unimrcp-server)
endif ()

# Sub-projects: asr-client
if (ENABLE_CLIENT_LIB AND ENABLE_ASR_CLIENT)
add_subdirectory (platforms/libasr-client)
add_subdirectory (platforms/asr-client)
endif ()

# Sub-projects: tests
if (ENABLE_TEST_SUITES)
add_subdirectory (tests/apttest)
add_subdirectory (tests/mpftest)
add_subdirectory (tests/mrcptest)
add_subdirectory (tests/rtsptest)
add_subdirectory (tests/strtablegen)
endif ()

# Installation directives
install (DIRECTORY DESTINATION log)
install (DIRECTORY DESTINATION var)
install (DIRECTORY data/ DESTINATION data PATTERN Makefile* EXCLUDE)
install (DIRECTORY conf/ DESTINATION conf PATTERN Makefile* EXCLUDE)
