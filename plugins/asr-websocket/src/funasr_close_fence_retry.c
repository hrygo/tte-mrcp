#include "funasr_close_fence_retry.h"

#define FUNASR_CLOSE_FENCE_RETRY_LIMIT 3U
typedef struct funasr_close_fence_retry_t {
    funasr_transport_event_t *event;
    const funasr_close_fence_retry_vtable_t *vtable;
    void *obj;
} funasr_close_fence_retry_t;

static void funasr_close_fence_retry_finish(
    const funasr_close_fence_retry_vtable_t *vtable,
    void *obj,
    funasr_transport_event_t *event)
{
    if (vtable->worker_joined(obj, event)) {
        vtable->fallback(obj, event);
    } else {
        vtable->retain(obj, event);
    }
    vtable->release(obj, event);
}

apt_bool_t funasr_close_fence_retry_attempt(
    apt_consumer_task_t *task,
    apr_pool_t *pool,
    funasr_transport_event_t *event,
    const funasr_close_fence_retry_vtable_t *vtable,
    void *obj)
{
    funasr_close_fence_retry_t *retry;

    if (!task || !pool || !event || !vtable || !vtable->requeue ||
        !vtable->release || !vtable->worker_joined || !vtable->fallback ||
        !vtable->retain || !vtable->allocate) {
        return FALSE;
    }
    event->close_fence_retry_count++;
    if (event->close_fence_retry_count >= FUNASR_CLOSE_FENCE_RETRY_LIMIT) {
        funasr_close_fence_retry_finish(vtable, obj, event);
        return FALSE;
    }
    retry = vtable->allocate(obj, pool, sizeof(*retry));
    if (!retry) {
        funasr_close_fence_retry_finish(vtable, obj, event);
        return FALSE;
    }
    retry->event = event;
    retry->vtable = vtable;
    retry->obj = obj;
    if (!retry->vtable->requeue(retry->obj, retry->event)) {
        funasr_close_fence_retry_finish(
            retry->vtable,
            retry->obj,
            retry->event);
        retry->event = NULL;
        return FALSE;
    }
    retry->event = NULL;
    return TRUE;
}
