#include "funasr_timeout_config.h"

#include <errno.h>
#include <stdlib.h>

static void funasr_timeout_status_set(
    funasr_timeout_value_e *status,
    funasr_timeout_value_e value)
{
    if (status) {
        *status = value;
    }
}

apr_interval_time_t funasr_timeout_ms_parse(
    const char *value,
    apr_interval_time_t fallback,
    funasr_timeout_value_e *status)
{
    char *end = NULL;
    long milliseconds;

    if (!value) {
        funasr_timeout_status_set(status, FUNASR_TIMEOUT_VALUE_DEFAULT);
        return fallback;
    }
    if (!*value) {
        funasr_timeout_status_set(status, FUNASR_TIMEOUT_VALUE_INVALID);
        return fallback;
    }

    errno = 0;
    milliseconds = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || milliseconds <= 0 ||
        (apr_int64_t)milliseconds > APR_INT64_MAX / 1000) {
        funasr_timeout_status_set(status, FUNASR_TIMEOUT_VALUE_INVALID);
        return fallback;
    }

    funasr_timeout_status_set(status, FUNASR_TIMEOUT_VALUE_CONFIGURED);
    return (apr_interval_time_t)milliseconds * 1000;
}
