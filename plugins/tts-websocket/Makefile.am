AM_CPPFLAGS                = $(UNIMRCP_PLUGIN_INCLUDES)

plugin_LTLIBRARIES         = tts_websocket.la

tts_websocket_la_SOURCES       = src/tts_websocket_engine.c src/tts_websocket_pcm.c src/tts_websocket_lifecycle.c src/tts_websocket_ws.c src/tts_websocket_resampler.c
tts_websocket_la_CPPFLAGS      = $(AM_CPPFLAGS) $(SPEEXDSP_CFLAGS)
tts_websocket_la_LIBADD        = $(SPEEXDSP_LIBS)
tts_websocket_la_LDFLAGS       = $(UNIMRCP_PLUGIN_OPTS)

check_PROGRAMS = test_tts_websocket_resampler
TESTS = $(check_PROGRAMS)
test_tts_websocket_resampler_SOURCES = \
	tests/test_tts_websocket_resampler.c \
	src/tts_websocket_resampler.c \
	src/tts_websocket_pcm.c
test_tts_websocket_resampler_CPPFLAGS = $(AM_CPPFLAGS) $(SPEEXDSP_CFLAGS)
test_tts_websocket_resampler_LDADD = $(SPEEXDSP_LIBS) -lm

include $(top_srcdir)/build/rules/uniplugin.am
