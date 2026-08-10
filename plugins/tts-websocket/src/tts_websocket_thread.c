#include "tts_websocket_thread.h"

#include <apr_portable.h>

unsigned long tts_websocket_thread_id_current(void)
{
    return (unsigned long)apr_os_thread_current();
}
