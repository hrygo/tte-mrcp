#ifndef FUNASR_TIMEOUT_CONFIG_H
#define FUNASR_TIMEOUT_CONFIG_H

#include <apr.h>
#include <apr_time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FUNASR_FIRST_AUDIO_RESULT_TIMEOUT_US 20000000LL
#define FUNASR_LAST_SPEECH_RESULT_TIMEOUT_US 10000000LL

typedef enum funasr_timeout_value_e {
    FUNASR_TIMEOUT_VALUE_DEFAULT,
    FUNASR_TIMEOUT_VALUE_CONFIGURED,
    FUNASR_TIMEOUT_VALUE_INVALID
} funasr_timeout_value_e;

apr_interval_time_t funasr_timeout_ms_parse(
    const char *value,
    apr_interval_time_t fallback,
    funasr_timeout_value_e *status);

#ifdef __cplusplus
}
#endif

#endif
