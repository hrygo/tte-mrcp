#include "funasr_close_fence_retry.h"

#define FUNASR_CLOSE_FENCE_RETRY_LIMIT 3U
typedef struct funasr_close_fence_retry_t {
    funasr_transport_event_t *event;
    const funasr_close_fence_retry_vtable_t *vtable;
    void *obj;
} funasr_close_fence_retry_t;

static void funasr_close_fence_retry_finish(funasr_close_fence_retry_t *retry)
{
    if (retry->vtable->worker_joined(retry->obj, retry->event)) {
        retry->vtable->fallback(retry->obj, retry->event);
    } else {
        retry->vtable->retain(retry->obj, retry->event);
    }
    retry->vtable->release(retry->obj, retry->event);
    retry->event = NULL;
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
        !vtable->retain) {
        return FALSE;
    }
    event->close_fence_retry_count++;
    if (event->close_fence_retry_count >= FUNASR_CLOSE_FENCE_RETRY_LIMIT) {
        retry = apr_pcalloc(pool, sizeof(*retry));
        retry->event = event;
        retry->vtable = vtable;
        retry->obj = obj;
        funasr_close_fence_retry_finish(retry);
        return FALSE;
    }
    retry = apr_pcalloc(pool, sizeof(*retry));
    if (!retry) return FALSE;
    retry->event = event;
    retry->vtable = vtable;
    retry->obj = obj;
    if (!retry->vtable->requeue(retry->obj, retry->event)) {
        funasr_close_fence_retry_finish(retry);
        return FALSE;
    }
    retry->event = NULL;
    return TRUE;
}
