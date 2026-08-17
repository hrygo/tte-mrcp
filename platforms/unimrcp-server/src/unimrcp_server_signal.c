#include "unimrcp_server_signal.h"

#if defined(WIN32) || defined(_WIN32)

int unimrcp_server_signals_init(void)
{
	return 0;
}

#else

#include <signal.h>

int unimrcp_server_signals_init(void)
{
	return signal(SIGPIPE, SIG_IGN) == SIG_ERR ? -1 : 0;
}

#endif
