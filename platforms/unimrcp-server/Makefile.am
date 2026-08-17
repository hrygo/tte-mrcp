AM_CPPFLAGS            = -I$(top_srcdir)/platforms/unimrcp-server/include \
                         $(UNIMRCP_SERVERAPP_INCLUDES)

bin_PROGRAMS           = unimrcpserver
unimrcpserver_SOURCES  = src/main.c src/uni_cmdline.c src/uni_daemon.c \
                         src/unimrcp_server_signal.c src/unimrcp_server_signal.h
unimrcpserver_LDADD    = $(UNIMRCP_SERVERAPP_LIBS)
unimrcpserver_LDFLAGS  = $(UNIMRCP_SERVERAPP_OPTS)

check_PROGRAMS = test_unimrcp_server_signal
TESTS = $(check_PROGRAMS)
test_unimrcp_server_signal_SOURCES = \
                         tests/test_unimrcp_server_signal.c \
                         src/unimrcp_server_signal.c
test_unimrcp_server_signal_CPPFLAGS = -I$(top_srcdir)/platforms/unimrcp-server/src

include $(top_srcdir)/build/rules/uniserverapp.am
