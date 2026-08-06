cmake_minimum_required (VERSION 2.8)
project (tts_websocket)

# Set source files
set (TTS_WEBSOCKET_SOURCES
	src/tts_websocket_engine.c
	src/tts_websocket_pcm.c
	src/tts_websocket_lifecycle.c
	src/tts_websocket_ws.c
)
source_group ("src" FILES ${TTS_WEBSOCKET_SOURCES})

# Plug-in declaration
add_library (${PROJECT_NAME} MODULE ${TTS_WEBSOCKET_SOURCES}
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
	target_link_libraries(${PROJECT_NAME} m iconv)
endif ()

# TingYun APM static library (conditional).
if (ENABLE_TINGYUN_APM AND TINGYUN_FOUND)
	target_include_directories(${PROJECT_NAME} PRIVATE ${TINGYUN_INCLUDE_DIR})
	target_link_libraries(${PROJECT_NAME} ${TINGYUN_LIBRARY})
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
	${MRCP_ENGINE_INCLUDE_DIRS}
	${MRCP_INCLUDE_DIRS}
	${MPF_INCLUDE_DIRS}
	${APR_TOOLKIT_INCLUDE_DIRS}
	${APR_INCLUDE_DIRS}
	${APU_INCLUDE_DIRS}
)

# Installation directives
install (TARGETS ${PROJECT_NAME} LIBRARY DESTINATION plugin)
if (MSVC)
	install (FILES ${PROJECT_BINARY_DIR}/Debug/${PROJECT_NAME}.pdb DESTINATION plugin CONFIGURATIONS Debug)
	install (FILES ${PROJECT_BINARY_DIR}/RelWithDebInfo/${PROJECT_NAME}.pdb DESTINATION plugin CONFIGURATIONS RelWithDebInfo)
endif (MSVC)

# -- Unit tests for the plugin (optional) ---------------------------------
option(BUILD_TTS_WEBSOCKET_TESTS "Build tts_websocket tests" ON)
if(BUILD_TTS_WEBSOCKET_TESTS)
	enable_testing()
	add_executable(test_tts_websocket_http_parse tests/test_tts_websocket_http_parse.c)
	set_target_properties(test_tts_websocket_http_parse PROPERTIES FOLDER "tests")
	add_test(NAME tts_websocket_http_parse COMMAND test_tts_websocket_http_parse)
	add_executable(test_tts_websocket_pcm
		tests/test_tts_websocket_pcm.c
		src/tts_websocket_pcm.c)
	set_target_properties(test_tts_websocket_pcm PROPERTIES FOLDER "tests")
	add_test(NAME tts_websocket_pcm COMMAND test_tts_websocket_pcm)
	add_executable(test_tts_websocket_lifecycle
		tests/test_tts_websocket_lifecycle.c
		src/tts_websocket_lifecycle.c)
	target_include_directories(test_tts_websocket_lifecycle PRIVATE
		${PROJECT_SOURCE_DIR}/src
		${APR_TOOLKIT_INCLUDE_DIRS}
		${APR_INCLUDE_DIRS})
	target_link_libraries(test_tts_websocket_lifecycle ${APR_LIBRARIES})
	set_target_properties(test_tts_websocket_lifecycle PROPERTIES FOLDER "tests")
	add_test(NAME tts_websocket_lifecycle COMMAND test_tts_websocket_lifecycle)
	add_executable(test_tts_websocket_ws
		tests/test_tts_websocket_ws.c
		src/tts_websocket_ws.c)
	target_include_directories(test_tts_websocket_ws PRIVATE
		${PROJECT_SOURCE_DIR}/src
		${APR_TOOLKIT_INCLUDE_DIRS}
		${APR_INCLUDE_DIRS})
	target_link_libraries(test_tts_websocket_ws ${APR_LIBRARIES})
	set_target_properties(test_tts_websocket_ws PROPERTIES FOLDER "tests")
	add_test(NAME tts_websocket_ws COMMAND test_tts_websocket_ws)
endif()
