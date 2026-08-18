#include "funasr_close_fence_retry.h"

#include <apr_general.h>
#include <apr_pools.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct retry_msg_t { funasr_transport_event_t *event; } retry_msg_t;
typedef struct retry_test_t {
    apr_pool_t *pool;
    apt_consumer_task_t *consumer;
    apt_task_t *task;
    int requeues;
    int releases;
    int fallbacks;
    int retains;
    apt_bool_t joined;
    apt_bool_t reject_requeue;
    funasr_transport_event_t *event;
} retry_test_t;

static int failures;
#define CHECK(label, expression) do { if (!(expression)) { fprintf(stderr, "FAIL %s\n", label); failures++; } } while (0)

static apt_bool_t process(apt_task_t *task, apt_task_msg_t *msg)
{
    apt_consumer_task_t *consumer = apt_task_object_get(task);
    retry_test_t *state = apt_consumer_task_object_get(consumer);
    retry_msg_t *payload = (retry_msg_t *)msg->data;
    state->requeues++;
    state->event = payload->event;
    return TRUE;
}

static apt_bool_t requeue(void *obj, funasr_transport_event_t *event)
{
    retry_test_t *state = obj;
    apt_task_msg_t *msg;
    retry_msg_t *payload;
    if (state->reject_requeue || !(msg = apt_task_msg_get(state->task))) return FALSE;
    msg->type = TASK_MSG_USER;
    payload = (retry_msg_t *)msg->data;
    payload->event = event;
    return apt_task_msg_signal(state->task, msg);
}
static void release(void *obj, funasr_transport_event_t *event) { ((retry_test_t *)obj)->releases++; free(event); }
static apt_bool_t joined(void *obj, funasr_transport_event_t *event) { (void)event; return ((retry_test_t *)obj)->joined; }
static void fallback(void *obj, funasr_transport_event_t *event) { (void)event; ((retry_test_t *)obj)->fallbacks++; }
static void retain(void *obj, funasr_transport_event_t *event) { (void)event; ((retry_test_t *)obj)->retains++; }
static const funasr_close_fence_retry_vtable_t vtable = { requeue, release, joined, fallback, retain };

static funasr_transport_event_t *event_create(void)
{
    funasr_transport_event_t *event = calloc(1, sizeof(*event));
    if (event) event->type = FUNASR_EVENT_WORKER_CLOSED;
    return event;
}
static apt_bool_t wait_for(retry_test_t *state, int count)
{
    int attempt;
    for (attempt = 0; attempt < 200; ++attempt) {
        if (state->requeues >= count || state->releases != 0) return TRUE;
        apr_sleep(1000);
    }
    return FALSE;
}
static void setup(retry_test_t *state)
{
    apt_task_vtable_t *task_vtable;
    memset(state, 0, sizeof(*state));
    CHECK("pool", apr_pool_create(&state->pool, NULL) == APR_SUCCESS);
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
    CHECK("not released", state.releases == 0 && state.event == event);
    release(&state, event); CHECK("released by new owner", state.releases == 1); teardown(&state);
}
static void test_requeue_failure_once(void)
{
    retry_test_t state; funasr_transport_event_t *event;
    setup(&state); state.joined = TRUE; state.reject_requeue = TRUE; event = event_create();
    CHECK("requeue failure is terminal", !funasr_close_fence_retry_attempt(state.consumer, state.pool, event, &vtable, &state));
    CHECK("failure observed", wait_for(&state, 1));
    CHECK("fallback and one release", state.fallbacks == 1 && state.releases == 1); teardown(&state);
}
static void test_three_failures_joined(void)
{
    retry_test_t state; funasr_transport_event_t *event;
    setup(&state); state.joined = TRUE; event = event_create();
    CHECK("first", funasr_close_fence_retry_attempt(state.consumer,state.pool,event,&vtable,&state)); CHECK("first arrives", wait_for(&state,1));
    CHECK("second", funasr_close_fence_retry_attempt(state.consumer,state.pool,event,&vtable,&state)); CHECK("second arrives", wait_for(&state,2));
    CHECK("third terminal", !funasr_close_fence_retry_attempt(state.consumer,state.pool,event,&vtable,&state));
    CHECK("joined fallback once", state.fallbacks == 1 && state.releases == 1); teardown(&state);
}
static void test_three_failures_unjoined(void)
{
    retry_test_t state; funasr_transport_event_t *event;
    setup(&state); event = event_create(); event->close_fence_retry_count = 2;
    CHECK("third terminal", !funasr_close_fence_retry_attempt(state.consumer,state.pool,event,&vtable,&state));
    CHECK("unjoined retained", state.fallbacks == 0 && state.retains == 1 && state.releases == 1); teardown(&state);
}
int main(void)
{
    apr_initialize(); test_transfer(); test_requeue_failure_once(); test_three_failures_joined(); test_three_failures_unjoined(); apr_terminate();
    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    printf("PASS test_funasr_close_fence_retry\n"); return 0;
}
