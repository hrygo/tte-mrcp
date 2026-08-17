cmake_minimum_required (VERSION 2.8)
project (unimrcpserver-app)

set (PROJECT_OUTPUT_NAME unimrcpserver)

# Set source files
set (UNIMRCP_SERVER_SOURCES
	src/main.c
	src/uni_cmdline.c
	src/uni_daemon.c
	src/unimrcp_server_signal.c
	src/unimrcp_server_signal.h
)
if (WIN32)
	set (UNIMRCP_SERVER_SOURCES ${UNIMRCP_SERVER_SOURCES} src/uni_service.c)
endif (WIN32)
source_group ("src" FILES ${UNIMRCP_SERVER_SOURCES})

# Application declaration
add_executable (${PROJECT_NAME} ${UNIMRCP_SERVER_SOURCES})
set_target_properties (${PROJECT_NAME} PROPERTIES FOLDER "platforms")
set_target_properties (${PROJECT_NAME} PROPERTIES OUTPUT_NAME ${PROJECT_OUTPUT_NAME})

# Input libraries
target_link_libraries(${PROJECT_NAME} unimrcpserver)

option(BUILD_UNIMRCP_SERVER_TESTS "Build unimrcpserver tests" ON)
if(BUILD_UNIMRCP_SERVER_TESTS AND NOT WIN32)
	add_executable(test_unimrcp_server_signal
		tests/test_unimrcp_server_signal.c
		src/unimrcp_server_signal.c)
	target_include_directories(test_unimrcp_server_signal PRIVATE
		${PROJECT_SOURCE_DIR}/src
		${APR_INCLUDE_DIRS})
	target_link_libraries(test_unimrcp_server_signal ${APR_LIBRARIES})
	set_target_properties(test_unimrcp_server_signal PROPERTIES FOLDER "tests")
	add_test(NAME unimrcp_server_signal COMMAND test_unimrcp_server_signal)
	set_tests_properties(unimrcp_server_signal PROPERTIES TIMEOUT 10)
endif()

# Preprocessor definitions
add_definitions (
	${MRCP_DEFINES}
	${MPF_DEFINES}
	${APR_TOOLKIT_DEFINES} 
	${APR_DEFINES} 
	${APU_DEFINES}
)

# Include directories
include_directories (
	${PROJECT_SOURCE_DIR}/include
	${VERSION_INCLUDE_DIRS}
	${UNIMRCP_SERVER_INCLUDE_DIRS}
	${MRCP_SERVER_INCLUDE_DIRS}
	${MRCP_ENGINE_INCLUDE_DIRS}
	${MRCP_SIGNALING_INCLUDE_DIRS}
	${MRCPv2_TRANSPORT_INCLUDE_DIRS}
	${MRCP_INCLUDE_DIRS}
	${MPF_INCLUDE_DIRS}
	${APR_TOOLKIT_INCLUDE_DIRS}
	${APR_INCLUDE_DIRS}
	${APU_INCLUDE_DIRS}
)

# Installation directives
install (TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
if (MSVC)
	install (FILES ${PROJECT_BINARY_DIR}/Debug/${PROJECT_OUTPUT_NAME}.pdb DESTINATION bin CONFIGURATIONS Debug)
	install (FILES ${PROJECT_BINARY_DIR}/RelWithDebInfo/${PROJECT_OUTPUT_NAME}.pdb DESTINATION bin CONFIGURATIONS RelWithDebInfo)
endif (MSVC)
