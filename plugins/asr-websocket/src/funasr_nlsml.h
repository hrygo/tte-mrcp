#ifndef FUNASR_NLSML_H
#define FUNASR_NLSML_H

#include <apr_pools.h>

#include <stdint.h>

char *funasr_nlsml_result_create(
    apr_pool_t *pool,
    const char *session_id,
    const char *result,
    uint64_t *success_count);

#endif
