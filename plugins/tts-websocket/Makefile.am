AM_CPPFLAGS                = $(UNIMRCP_PLUGIN_INCLUDES)

plugin_LTLIBRARIES         = tts_websocket.la

tts_websocket_la_SOURCES       = src/tts_websocket_engine.c src/tts_websocket_pcm.c src/tts_websocket_lifecycle.c src/tts_websocket_ws.c
tts_websocket_la_LDFLAGS       = $(UNIMRCP_PLUGIN_OPTS)
tts_websocket_la_LIBADD        = $(UNIMRCP_PLUGIN_TINGYUN_LIBADD)

include $(top_srcdir)/build/rules/uniplugin.am
