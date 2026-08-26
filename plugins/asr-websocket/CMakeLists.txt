cmake_minimum_required (VERSION 2.8)
project (asr_websocket)

# Set source files
set (ASR_WEBSOCKET_SOURCES
	src/asr_websocket_engine.c
	src/funasr_audio.c
	src/funasr_clock.c
	src/funasr_close_fence_retry.c
	src/funasr_control.c
	src/funasr_json.c
	src/funasr_nlsml.c
	src/funasr_ws_transport.c
)
source_group ("src" FILES ${ASR_WEBSOCKET_SOURCES})

# Plug-in declaration
add_library (${PROJECT_NAME} MODULE ${ASR_WEBSOCKET_SOURCES}
	$<TARGET_OBJECTS:mrcpengine>
	$<TARGET_OBJECTS:mrcp>
	$<TARGET_OBJECTS:mpf>
	$<TARGET_OBJECTS:aprtoolkit>
)
set_target_properties (${PROJECT_NAME} PROPERTIES FOLDER "plugins")

# Input libraries
target_link_libraries(${PROJECT_NAME}
	${APU_LIBRARIES}
	${APR_LIBRARIES}
)
# Input system libraries
if (WIN32)
	target_link_libraries(${PROJECT_NAME} ws2_32 winmm)
elseif (UNIX)
	target_link_libraries(${PROJECT_NAME} m)
	if (NOT APPLE)
		target_link_libraries(${PROJECT_NAME} rt)
	endif ()
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
option(BUILD_ASR_WEBSOCKET_TESTS "Build ASR WebSocket tests" ON)
if(BUILD_ASR_WEBSOCKET_TESTS)
	set(ASR_WEBSOCKET_TEST_INCLUDE_DIRS
		${PROJECT_SOURCE_DIR}/src
		${MRCP_ENGINE_INCLUDE_DIRS}
		${MRCP_INCLUDE_DIRS}
		${MPF_INCLUDE_DIRS}
		${APR_TOOLKIT_INCLUDE_DIRS}
		${APR_INCLUDE_DIRS}
		${APU_INCLUDE_DIRS})
	add_executable(test_resample tests/test_resample.c src/funasr_audio.c)
	target_include_directories(test_resample PRIVATE ${ASR_WEBSOCKET_TEST_INCLUDE_DIRS})
	set_target_properties(test_resample PROPERTIES FOLDER "tests")
	add_test(NAME asr_websocket_resample COMMAND test_resample)

	add_executable(test_json_unescape tests/test_json_unescape.c src/funasr_json.c)
	target_include_directories(test_json_unescape PRIVATE ${ASR_WEBSOCKET_TEST_INCLUDE_DIRS})
	set_target_properties(test_json_unescape PROPERTIES FOLDER "tests")
	add_test(NAME asr_websocket_json_unescape COMMAND test_json_unescape)

	add_executable(test_funasr_nlsml
		tests/test_funasr_nlsml.c
		src/funasr_nlsml.c)
	target_include_directories(test_funasr_nlsml PRIVATE ${ASR_WEBSOCKET_TEST_INCLUDE_DIRS})
	target_link_libraries(test_funasr_nlsml ${APR_LIBRARIES})
	set_target_properties(test_funasr_nlsml PROPERTIES FOLDER "tests")
	add_test(NAME asr_websocket_nlsml COMMAND test_funasr_nlsml)

	add_executable(test_funasr_ws_transport
		tests/test_funasr_ws_transport.c
		src/funasr_audio.c
		src/funasr_clock.c
		src/funasr_json.c
		src/funasr_ws_transport.c
		$<TARGET_OBJECTS:aprtoolkit>)
	target_include_directories(test_funasr_ws_transport PRIVATE ${ASR_WEBSOCKET_TEST_INCLUDE_DIRS})
	target_link_libraries(test_funasr_ws_transport
		${APU_LIBRARIES}
		${APR_LIBRARIES})
	if (WIN32)
		target_link_libraries(test_funasr_ws_transport ws2_32 winmm)
	elseif (UNIX)
		target_link_libraries(test_funasr_ws_transport m)
		if (NOT APPLE)
			target_link_libraries(test_funasr_ws_transport rt)
		endif ()
	endif ()
	set_target_properties(test_funasr_ws_transport PROPERTIES FOLDER "tests")
	add_test(NAME asr_websocket_funasr_ws_transport COMMAND test_funasr_ws_transport)

	add_executable(test_funasr_control
		tests/test_funasr_control.c
		src/funasr_control.c)
	target_include_directories(test_funasr_control PRIVATE ${ASR_WEBSOCKET_TEST_INCLUDE_DIRS})
	set_target_properties(test_funasr_control PROPERTIES FOLDER "tests")
	add_test(NAME asr_websocket_funasr_control COMMAND test_funasr_control)

	add_executable(test_funasr_close_fence_retry
		tests/test_funasr_close_fence_retry.c
		src/funasr_close_fence_retry.c
		$<TARGET_OBJECTS:aprtoolkit>)
	target_include_directories(test_funasr_close_fence_retry PRIVATE ${ASR_WEBSOCKET_TEST_INCLUDE_DIRS})
	target_link_libraries(test_funasr_close_fence_retry ${APU_LIBRARIES} ${APR_LIBRARIES})
	set_target_properties(test_funasr_close_fence_retry PROPERTIES FOLDER "tests")
	add_test(NAME asr_websocket_close_fence_retry COMMAND test_funasr_close_fence_retry)

	add_test(NAME asr_websocket_frame_debug_logs
		COMMAND ${CMAKE_COMMAND}
			-DASR_WEBSOCKET_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
			-P ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_asr_frame_debug_logs.cmake)
endif()
