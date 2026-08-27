AM_CPPFLAGS                = $(UNIMRCP_PLUGIN_INCLUDES) -I$(srcdir)/src

plugin_LTLIBRARIES         = asr_websocket.la

asr_websocket_la_SOURCES   = \
                             src/asr_websocket_engine.c \
                             src/funasr_audio.c src/funasr_audio.h \
                             src/funasr_clock.c src/funasr_clock.h \
                             src/funasr_close_fence_retry.c src/funasr_close_fence_retry.h \
                             src/funasr_control.c src/funasr_control.h \
                             src/funasr_json.c src/funasr_json.h \
                             src/funasr_nlsml.c src/funasr_nlsml.h \
                             src/funasr_timeout_config.c src/funasr_timeout_config.h \
                             src/funasr_ws_transport.c src/funasr_ws_transport.h
asr_websocket_la_LDFLAGS   = $(UNIMRCP_PLUGIN_OPTS)

check_PROGRAMS             = test_resample test_json_unescape test_funasr_nlsml \
                             test_funasr_ws_transport test_funasr_control \
                             test_funasr_close_fence_retry test_funasr_timeout_config
TESTS                      = $(check_PROGRAMS)

test_resample_SOURCES      = tests/test_resample.c src/funasr_audio.c
test_resample_CPPFLAGS     = $(AM_CPPFLAGS)
test_json_unescape_SOURCES = tests/test_json_unescape.c src/funasr_json.c
test_json_unescape_CPPFLAGS = $(AM_CPPFLAGS)
test_funasr_nlsml_SOURCES = tests/test_funasr_nlsml.c src/funasr_nlsml.c
test_funasr_nlsml_LDADD = $(UNIMRCP_APR_LIBS)
test_funasr_nlsml_CPPFLAGS = $(AM_CPPFLAGS)
test_funasr_ws_transport_SOURCES = \
                             tests/test_funasr_ws_transport.c \
                             src/funasr_audio.c src/funasr_clock.c \
                             src/funasr_json.c src/funasr_ws_transport.c
test_funasr_ws_transport_LDADD = \
                             $(top_builddir)/libs/apr-toolkit/libaprtoolkit.la \
                             $(UNIMRCP_APR_LIBS)
test_funasr_ws_transport_CPPFLAGS = $(AM_CPPFLAGS)
test_funasr_control_SOURCES = tests/test_funasr_control.c src/funasr_control.c
test_funasr_control_CPPFLAGS = $(AM_CPPFLAGS)
test_funasr_timeout_config_SOURCES = \
                             tests/test_funasr_timeout_config.c \
                             src/funasr_timeout_config.c
test_funasr_timeout_config_CPPFLAGS = $(AM_CPPFLAGS)
test_funasr_close_fence_retry_SOURCES = tests/test_funasr_close_fence_retry.c src/funasr_close_fence_retry.c
test_funasr_close_fence_retry_LDADD = $(top_builddir)/libs/apr-toolkit/libaprtoolkit.la $(UNIMRCP_APR_LIBS)
test_funasr_close_fence_retry_CPPFLAGS = $(AM_CPPFLAGS)

include $(top_srcdir)/build/rules/uniplugin.am
