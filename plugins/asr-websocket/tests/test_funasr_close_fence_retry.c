#include "funasr_close_fence_retry.h"

#include <apr_general.h>
#include <apr_pools.h>
#include <apr_thread_cond.h>
#include <apr_thread_mutex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct retry_msg_t { funasr_transport_event_t *event; } retry_msg_t;
typedef struct retry_test_t {
    apr_pool_t *pool;
    apr_thread_mutex_t *mutex;
    apr_thread_cond_t *condition;
    apt_consumer_task_t *consumer;
    apt_task_t *task;
    int requeues;
    int releases;
    int fallbacks;
    int retains;
    int allocations;
    int notifications;
    apt_bool_t joined;
    apt_bool_t reject_allocation;
    apt_bool_t reject_requeue;
    funasr_transport_event_t *event;
} retry_test_t;

static int failures;
#define CHECK(label, expression) do { if (!(expression)) { fprintf(stderr, "FAIL %s\n", label); failures++; } } while (0)
#define CHECK_STATE(state, label, expression) do { \
    apt_bool_t state_ok; \
    apr_thread_mutex_lock((state)->mutex); \
    state_ok = (expression); \
    apr_thread_mutex_unlock((state)->mutex); \
    CHECK(label, state_ok); \
} while (0)

static apt_bool_t process(apt_task_t *task, apt_task_msg_t *msg)
{
    apt_consumer_task_t *consumer = apt_task_object_get(task);
    retry_test_t *state = apt_consumer_task_object_get(consumer);
    retry_msg_t *payload = (retry_msg_t *)msg->data;

    apr_thread_mutex_lock(state->mutex);
    state->requeues++;
    state->notifications++;
    state->event = payload->event;
    apr_thread_cond_signal(state->condition);
    apr_thread_mutex_unlock(state->mutex);
    return TRUE;
}

static apt_bool_t requeue(void *obj, funasr_transport_event_t *event)
{
    retry_test_t *state = obj;
    apt_task_msg_t *msg;
    retry_msg_t *payload;
    apt_bool_t reject;
    apr_thread_mutex_lock(state->mutex);
    reject = state->reject_requeue;
    apr_thread_mutex_unlock(state->mutex);
    if (reject || !(msg = apt_task_msg_get(state->task))) return FALSE;
    msg->type = TASK_MSG_USER;
    payload = (retry_msg_t *)msg->data;
    payload->event = event;
    return apt_task_msg_signal(state->task, msg);
}
static void release(void *obj, funasr_transport_event_t *event)
{
    retry_test_t *state = obj;
    apr_thread_mutex_lock(state->mutex);
    state->releases++;
    apr_thread_mutex_unlock(state->mutex);
    free(event);
}
static apt_bool_t joined(void *obj, funasr_transport_event_t *event)
{
    retry_test_t *state = obj;
    apt_bool_t result;
    (void)event;
    apr_thread_mutex_lock(state->mutex);
    result = state->joined;
    apr_thread_mutex_unlock(state->mutex);
    return result;
}
static void fallback(void *obj, funasr_transport_event_t *event)
{
    retry_test_t *state = obj;
    (void)event;
    apr_thread_mutex_lock(state->mutex);
    state->fallbacks++;
    apr_thread_mutex_unlock(state->mutex);
}
static void retain(void *obj, funasr_transport_event_t *event)
{
    retry_test_t *state = obj;
    (void)event;
    apr_thread_mutex_lock(state->mutex);
    state->retains++;
    apr_thread_mutex_unlock(state->mutex);
}
static void *allocate(void *obj, apr_pool_t *pool, apr_size_t size)
{
    retry_test_t *state = obj;
    apt_bool_t reject;
    apr_thread_mutex_lock(state->mutex);
    state->allocations++;
    reject = state->reject_allocation;
    apr_thread_mutex_unlock(state->mutex);
    return reject ? NULL : apr_pcalloc(pool, size);
}
static const funasr_close_fence_retry_vtable_t vtable = {
    requeue, release, joined, fallback, retain, allocate
};

static funasr_transport_event_t *event_create(void)
{
    funasr_transport_event_t *event = calloc(1, sizeof(*event));
    if (event) event->type = FUNASR_EVENT_WORKER_CLOSED;
    return event;
}
static apt_bool_t wait_for(retry_test_t *state, int count)
{
    apr_status_t status = APR_SUCCESS;
    apr_time_t timeout = apr_time_from_msec(200);
    apt_bool_t complete;

    apr_thread_mutex_lock(state->mutex);
    while (state->notifications < count && state->releases == 0 &&
           status == APR_SUCCESS) {
        status = apr_thread_cond_timedwait(
            state->condition,
            state->mutex,
            timeout);
    }
    complete = state->notifications >= count || state->releases != 0;
    apr_thread_mutex_unlock(state->mutex);
    return complete;
}
static void setup(retry_test_t *state)
{
    apt_task_vtable_t *task_vtable;
    memset(state, 0, sizeof(*state));
    CHECK("pool", apr_pool_create(&state->pool, NULL) == APR_SUCCESS);
    CHECK("mutex", apr_thread_mutex_create(
        &state->mutex, APR_THREAD_MUTEX_DEFAULT, state->pool) == APR_SUCCESS);
    CHECK("condition", apr_thread_cond_create(
        &state->condition, state->pool) == APR_SUCCESS);
    state->consumer = apt_consumer_task_create(
        state, apt_task_msg_pool_create_dynamic(sizeof(retry_msg_t), state->pool), state->pool);
    state->task = apt_consumer_task_base_get(state->consumer);
    task_vtable = apt_task_vtable_get(state->task);
    task_vtable->process_msg = process;
    CHECK("start", apt_task_start(state->task));
}
static void teardown(retry_test_t *state)
{
    apt_task_terminate(state->task, TRUE);
    apt_task_destroy(state->task);
    apr_pool_destroy(state->pool);
}
static void test_transfer(void)
{
    retry_test_t state; funasr_transport_event_t *event;
    setup(&state); event = event_create();
    CHECK("schedule", funasr_close_fence_retry_attempt(state.consumer, state.pool, event, &vtable, &state));
    CHECK("requeued", wait_for(&state, 1));
    CHECK_STATE(&state, "not released", state.releases == 0 && state.event == event);
    release(&state, event); CHECK_STATE(&state, "released by new owner", state.releases == 1); teardown(&state);
}
static void test_requeue_failure_once(void)
{
    retry_test_t state; funasr_transport_event_t *event;
    setup(&state); state.joined = TRUE; state.reject_requeue = TRUE; event = event_create();
    CHECK("requeue failure is terminal", !funasr_close_fence_retry_attempt(state.consumer, state.pool, event, &vtable, &state));
    CHECK("failure observed", wait_for(&state, 1));
    CHECK_STATE(&state, "fallback and one release", state.fallbacks == 1 && state.releases == 1); teardown(&state);
}
static void test_three_failures_joined(void)
{
    retry_test_t state; funasr_transport_event_t *event;
    setup(&state); state.joined = TRUE; event = event_create();
    CHECK("first", funasr_close_fence_retry_attempt(state.consumer,state.pool,event,&vtable,&state)); CHECK("first arrives", wait_for(&state,1));
    CHECK("second", funasr_close_fence_retry_attempt(state.consumer,state.pool,event,&vtable,&state)); CHECK("second arrives", wait_for(&state,2));
    CHECK("third terminal", !funasr_close_fence_retry_attempt(state.consumer,state.pool,event,&vtable,&state));
    CHECK_STATE(&state, "joined fallback once", state.fallbacks == 1 && state.releases == 1); teardown(&state);
}
static void test_three_failures_unjoined(void)
{
    retry_test_t state; funasr_transport_event_t *event;
    setup(&state); event = event_create(); event->close_fence_retry_count = 2;
    CHECK("third terminal", !funasr_close_fence_retry_attempt(state.consumer,state.pool,event,&vtable,&state));
    CHECK_STATE(&state, "unjoined retained", state.fallbacks == 0 && state.retains == 1 && state.releases == 1); teardown(&state);
}
static void test_allocation_failure_joined(void)
{
    retry_test_t state; funasr_transport_event_t *event;
    setup(&state); state.joined = TRUE; state.reject_allocation = TRUE; event = event_create();
    CHECK("allocation failure terminal", !funasr_close_fence_retry_attempt(state.consumer,state.pool,event,&vtable,&state));
    CHECK_STATE(&state, "joined allocation failure consumed once", state.allocations == 1 && state.requeues == 0 && state.fallbacks == 1 && state.retains == 0 && state.releases == 1); teardown(&state);
}
static void test_allocation_failure_unjoined(void)
{
    retry_test_t state; funasr_transport_event_t *event;
    setup(&state); state.reject_allocation = TRUE; event = event_create();
    CHECK("allocation failure terminal", !funasr_close_fence_retry_attempt(state.consumer,state.pool,event,&vtable,&state));
    CHECK_STATE(&state, "unjoined allocation failure retained once", state.allocations == 1 && state.requeues == 0 && state.fallbacks == 0 && state.retains == 1 && state.releases == 1); teardown(&state);
}
int main(void)
{
    apr_initialize(); test_transfer(); test_requeue_failure_once(); test_three_failures_joined(); test_three_failures_unjoined(); test_allocation_failure_joined(); test_allocation_failure_unjoined(); apr_terminate();
    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    printf("PASS test_funasr_close_fence_retry\n"); return 0;
}
