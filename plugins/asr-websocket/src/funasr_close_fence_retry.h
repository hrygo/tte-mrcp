#ifndef FUNASR_CLOSE_FENCE_RETRY_H
#define FUNASR_CLOSE_FENCE_RETRY_H

#include "apt_consumer_task.h"
#include "funasr_ws_transport.h"

typedef void *(*funasr_close_fence_retry_alloc_fn)(
    void *obj,
    apr_pool_t *pool,
    apr_size_t size);

typedef struct funasr_close_fence_retry_vtable_t {
    apt_bool_t (*requeue)(void *obj, funasr_transport_event_t *event);
    void (*release)(void *obj, funasr_transport_event_t *event);
    apt_bool_t (*worker_joined)(void *obj, funasr_transport_event_t *event);
    void (*fallback)(void *obj, funasr_transport_event_t *event);
    void (*retain)(void *obj, funasr_transport_event_t *event);
    funasr_close_fence_retry_alloc_fn allocate;
} funasr_close_fence_retry_vtable_t;

/* A valid call always consumes event: TRUE transfers it to requeue; FALSE releases it. */
apt_bool_t funasr_close_fence_retry_attempt(
    apt_consumer_task_t *task,
    apr_pool_t *pool,
    funasr_transport_event_t *event,
    const funasr_close_fence_retry_vtable_t *vtable,
    void *obj);

#endif
