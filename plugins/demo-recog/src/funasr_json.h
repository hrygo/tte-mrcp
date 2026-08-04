#ifndef FUNASR_JSON_H
#define FUNASR_JSON_H

#include <apr.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum funasr_json_status_e {
    FUNASR_JSON_OK,
    FUNASR_JSON_NOT_FOUND,
    FUNASR_JSON_INVALID,
    FUNASR_JSON_LIMIT_EXCEEDED,
    FUNASR_JSON_NO_MEMORY
} funasr_json_status_e;

funasr_json_status_e funasr_json_get_int(
    const char *json,
    apr_size_t json_size,
    const char *key,
    int *value);

funasr_json_status_e funasr_json_get_string_heap(
    const char *json,
    apr_size_t json_size,
    const char *key,
    apr_size_t output_limit,
    char **value,
    apr_size_t *value_size);

void funasr_json_heap_free(void *value);

#ifdef __cplusplus
}
#endif

#endif
