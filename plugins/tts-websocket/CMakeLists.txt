cmake_minimum_required (VERSION 2.8)
project (tts_websocket)

pkg_check_modules (SPEEXDSP REQUIRED speexdsp>=1.2.1)
link_directories (${SPEEXDSP_LIBRARY_DIRS})

# Set source files
set (TTS_WEBSOCKET_SOURCES
	src/tts_websocket_engine.c
	src/tts_websocket_pcm.c
	src/tts_websocket_lifecycle.c
	src/tts_websocket_ws.c
	src/tts_websocket_resampler.c
	src/tts_websocket_stream_pipeline.c
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
	${SPEEXDSP_LIBRARIES}
)
# Input system libraries
if (WIN32)
	target_link_libraries(${PROJECT_NAME} ws2_32 winmm)
elseif (UNIX)
	target_link_libraries(${PROJECT_NAME} m iconv)
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
	${SPEEXDSP_INCLUDE_DIRS}
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
	add_executable(test_tts_websocket_resampler
		tests/test_tts_websocket_resampler.c
		src/tts_websocket_resampler.c
		src/tts_websocket_pcm.c)
	target_include_directories(test_tts_websocket_resampler PRIVATE
		${PROJECT_SOURCE_DIR}/src
		${SPEEXDSP_INCLUDE_DIRS})
	if (UNIX)
		target_link_libraries(test_tts_websocket_resampler
			${SPEEXDSP_LIBRARIES} m)
	else ()
		target_link_libraries(test_tts_websocket_resampler
			${SPEEXDSP_LIBRARIES})
	endif ()
	set_target_properties(test_tts_websocket_resampler PROPERTIES FOLDER "tests")
	add_test(NAME tts_websocket_resampler COMMAND test_tts_websocket_resampler)
	add_executable(test_tts_websocket_stream_pipeline
		tests/test_tts_websocket_stream_pipeline.c
		src/tts_websocket_resampler.c
		src/tts_websocket_pcm.c
		src/tts_websocket_stream_pipeline.c)
	target_include_directories(test_tts_websocket_stream_pipeline PRIVATE
		${PROJECT_SOURCE_DIR}/src
		${APR_TOOLKIT_INCLUDE_DIRS}
		${APR_INCLUDE_DIRS}
		${SPEEXDSP_INCLUDE_DIRS})
	if (UNIX)
		target_link_libraries(test_tts_websocket_stream_pipeline
			${SPEEXDSP_LIBRARIES} ${APR_LIBRARIES} m)
	else ()
		target_link_libraries(test_tts_websocket_stream_pipeline
			${SPEEXDSP_LIBRARIES} ${APR_LIBRARIES})
	endif ()
	set_target_properties(test_tts_websocket_stream_pipeline PROPERTIES FOLDER "tests")
	add_test(NAME tts_websocket_stream_pipeline COMMAND test_tts_websocket_stream_pipeline)
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
