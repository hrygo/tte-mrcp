#ifndef FUNASR_CLOCK_H
#define FUNASR_CLOCK_H

#include <apr.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct funasr_clock_t {
    apr_int64_t (*now_us)(void *obj);
    void *obj;
} funasr_clock_t;

apr_int64_t funasr_clock_monotonic_now_us(void *obj);
apr_int64_t funasr_clock_now_us(const funasr_clock_t *clock);
void funasr_clock_default(funasr_clock_t *clock);

#ifdef __cplusplus
}
#endif

#endif
