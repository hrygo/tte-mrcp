AM_CPPFLAGS                = $(UNIMRCP_PLUGIN_INCLUDES) -I$(srcdir)/src

plugin_LTLIBRARIES         = tts_websocket.la

tts_websocket_la_SOURCES       = src/tts_websocket_engine.c src/tts_websocket_completion.c src/tts_websocket_completion.h src/tts_websocket_json.c src/tts_websocket_pcm.c src/tts_websocket_drain.c src/tts_websocket_drain.h src/tts_websocket_lifecycle.c src/tts_websocket_thread.c src/tts_websocket_ws.c

tts_websocket_la_LDFLAGS       = $(UNIMRCP_PLUGIN_OPTS)

check_PROGRAMS                 = test_tts_websocket_connection_log test_tts_websocket_json test_tts_websocket_pcm test_tts_websocket_drain
TESTS                           = $(check_PROGRAMS)
test_tts_websocket_connection_log_SOURCES = tests/test_tts_websocket_connection_log.c
test_tts_websocket_connection_log_LDADD = $(UNIMRCP_APR_LIBS)
test_tts_websocket_json_SOURCES = tests/test_tts_websocket_json.c src/tts_websocket_json.c
test_tts_websocket_pcm_SOURCES = tests/test_tts_websocket_pcm.c src/tts_websocket_pcm.c
test_tts_websocket_drain_SOURCES = tests/test_tts_websocket_drain.c src/tts_websocket_drain.c
# Target-specific flags force Automake to give unit-test objects distinct names
# from the same sources compiled into the libtool plug-in.
test_tts_websocket_json_CFLAGS = -DTEST_TTS_WEBSOCKET_JSON
test_tts_websocket_pcm_CFLAGS = -DTEST_TTS_WEBSOCKET_PCM
test_tts_websocket_drain_CFLAGS = -DTEST_TTS_WEBSOCKET_DRAIN

include $(top_srcdir)/build/rules/uniplugin.am
