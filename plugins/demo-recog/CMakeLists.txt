cmake_minimum_required (VERSION 2.8)
project (demorecog)

# Set source files
set (DEMO_RECOG_SOURCES
	src/demo_recog_engine.c
	src/funasr_audio.c
	src/funasr_clock.c
	src/funasr_control.c
	src/funasr_json.c
	src/funasr_ws_transport.c
)
source_group ("src" FILES ${DEMO_RECOG_SOURCES})

# Plug-in declaration
add_library (${PROJECT_NAME} MODULE ${DEMO_RECOG_SOURCES}
	$<TARGET_OBJECTS:mrcpengine>
	$<TARGET_OBJECTS:mrcp>
	$<TARGET_OBJECTS:mpf>
	$<TARGET_OBJECTS:aprtoolkit>
)
set_target_properties (${PROJECT_NAME} PROPERTIES FOLDER "plugins")

# Input libraries
target_link_libraries(${PROJECT_NAME}
	${APU_LIBRARIES}
	${APRUTIL_LIBRARIES}
	${APR_LIBRARIES}
)
# Input system libraries
if (WIN32)
	target_link_libraries(${PROJECT_NAME} ws2_32 winmm)
elseif (UNIX)
	target_link_libraries(${PROJECT_NAME} m)
endif ()

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
	${PROJECT_SOURCE_DIR}/src
	${MRCP_ENGINE_INCLUDE_DIRS}
	${MRCP_INCLUDE_DIRS}
	${MPF_INCLUDE_DIRS}
	${APR_TOOLKIT_INCLUDE_DIRS}
	${APR_INCLUDE_DIRS}
	${APU_INCLUDE_DIRS}
	${APRUTIL_INCLUDE_DIRS}
)

# Installation directives
install (TARGETS ${PROJECT_NAME} LIBRARY DESTINATION plugin)
if (MSVC)
	install (FILES ${PROJECT_BINARY_DIR}/Debug/${PROJECT_NAME}.pdb DESTINATION plugin CONFIGURATIONS Debug)
	install (FILES ${PROJECT_BINARY_DIR}/RelWithDebInfo/${PROJECT_NAME}.pdb DESTINATION plugin CONFIGURATIONS RelWithDebInfo)
endif (MSVC)

# -- Unit tests for the plugin (optional) ---------------------------------
option(BUILD_DEMORECOG_TESTS "Build demorecog tests" ON)
if(BUILD_DEMORECOG_TESTS)
	add_executable(test_resample tests/test_resample.c src/funasr_audio.c)
	set_target_properties(test_resample PROPERTIES FOLDER "tests")
	add_test(NAME demorecog_resample COMMAND test_resample)

	add_executable(test_json_unescape tests/test_json_unescape.c src/funasr_json.c)
	set_target_properties(test_json_unescape PROPERTIES FOLDER "tests")
	add_test(NAME demorecog_json_unescape COMMAND test_json_unescape)

	add_executable(test_funasr_ws_transport
		tests/test_funasr_ws_transport.c
		src/funasr_audio.c
		src/funasr_clock.c
		src/funasr_json.c
		src/funasr_ws_transport.c)
	target_link_libraries(test_funasr_ws_transport
		${APU_LIBRARIES}
		${APRUTIL_LIBRARIES}
		${APR_LIBRARIES})
	set_target_properties(test_funasr_ws_transport PROPERTIES FOLDER "tests")
	add_test(NAME demorecog_funasr_ws_transport COMMAND test_funasr_ws_transport)

	add_executable(test_funasr_control
		tests/test_funasr_control.c
		src/funasr_control.c)
	set_target_properties(test_funasr_control PROPERTIES FOLDER "tests")
	add_test(NAME demorecog_funasr_control COMMAND test_funasr_control)
endif()
