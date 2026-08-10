AM_CPPFLAGS                = $(UNIMRCP_PLUGIN_INCLUDES)

plugin_LTLIBRARIES         = tts_websocket.la

tts_websocket_la_SOURCES       = src/tts_websocket_engine.c src/tts_websocket_pcm.c src/tts_websocket_lifecycle.c src/tts_websocket_ws.c src/tts_websocket_resampler.c src/tts_websocket_stream_pipeline.c
tts_websocket_la_CPPFLAGS      = $(AM_CPPFLAGS) $(SPEEXDSP_CFLAGS)
tts_websocket_la_LIBADD        = $(SPEEXDSP_LIBS)
tts_websocket_la_LDFLAGS       = $(UNIMRCP_PLUGIN_OPTS)

check_PROGRAMS = test_tts_websocket_resampler test_tts_websocket_stream_pipeline
TESTS = $(check_PROGRAMS)
test_tts_websocket_resampler_SOURCES = \
	tests/test_tts_websocket_resampler.c \
	src/tts_websocket_resampler.c \
	src/tts_websocket_pcm.c
test_tts_websocket_resampler_CPPFLAGS = $(AM_CPPFLAGS) $(SPEEXDSP_CFLAGS)
test_tts_websocket_resampler_LDADD = $(SPEEXDSP_LIBS) -lm
test_tts_websocket_stream_pipeline_SOURCES = \
	tests/test_tts_websocket_stream_pipeline.c \
	src/tts_websocket_resampler.c \
	src/tts_websocket_pcm.c \
	src/tts_websocket_stream_pipeline.c
test_tts_websocket_stream_pipeline_CPPFLAGS = $(AM_CPPFLAGS) $(SPEEXDSP_CFLAGS)
test_tts_websocket_stream_pipeline_LDADD = $(SPEEXDSP_LIBS) $(UNIMRCP_APR_LIBS) -lm

include $(top_srcdir)/build/rules/uniplugin.am
