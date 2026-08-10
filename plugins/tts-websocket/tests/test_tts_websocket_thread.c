#include "tts_websocket_thread.h"

#include <stdio.h>

int main(void)
{
    if (tts_websocket_thread_id_current() == 0) {
        fprintf(stderr, "current APR thread id must be nonzero\n");
        return 1;
    }
    printf("PASS test_tts_websocket_thread\n");
    return 0;
}
