#include "funasr_clock.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

apr_int64_t funasr_clock_monotonic_now_us(void *obj)
{
    (void)obj;

#if defined(_WIN32)
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;

    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&counter) ||
        frequency.QuadPart <= 0) {
        return 0;
    }
    return (apr_int64_t)(
        (counter.QuadPart / frequency.QuadPart) * 1000000LL +
        ((counter.QuadPart % frequency.QuadPart) * 1000000LL) /
            frequency.QuadPart);
#elif defined(__APPLE__)
    mach_timebase_info_data_t timebase;
    uint64_t ticks;
    uint64_t whole;
    uint64_t remainder;
    uint64_t nanoseconds;

    if (mach_timebase_info(&timebase) != KERN_SUCCESS ||
        timebase.denom == 0) {
        return 0;
    }

    ticks = mach_absolute_time();
    whole = ticks / timebase.denom;
    remainder = ticks % timebase.denom;
    nanoseconds = whole * timebase.numer +
        (remainder * timebase.numer) / timebase.denom;
    return (apr_int64_t)(nanoseconds / 1000U);
#else
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0;
    }
    return (apr_int64_t)value.tv_sec * 1000000LL +
        (apr_int64_t)(value.tv_nsec / 1000L);
#endif
}

apr_int64_t funasr_clock_now_us(const funasr_clock_t *clock)
{
    if (!clock || !clock->now_us) {
        return 0;
    }
    return clock->now_us(clock->obj);
}

void funasr_clock_default(funasr_clock_t *clock)
{
    if (!clock) {
        return;
    }
    clock->now_us = funasr_clock_monotonic_now_us;
    clock->obj = NULL;
}
