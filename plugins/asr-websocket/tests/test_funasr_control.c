#include "funasr_control.h"

#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER) && _MSC_VER < 1900
#define snprintf _snprintf
#endif

typedef struct fake_sink_t {
    int starts;
    int completions;
    int stop_responses;
    int close_responses;
    int cancels;
    int close_requests;
    int joins;
    apt_bool_t reject_cancel;
    apt_bool_t reject_join;
    apt_bool_t reject_stop;
    apt_bool_t reject_close;
    mrcp_recog_completion_cause_e last_cause;
    char last_text[64];
    char call_order[16];
    size_t call_order_length;
} fake_sink_t;

static int failures;

static void fake_sink_record_call(fake_sink_t *sink, char call)
{
    if (sink->call_order_length + 1 < sizeof(sink->call_order)) {
        sink->call_order[sink->call_order_length++] = call;
        sink->call_order[sink->call_order_length] = '\0';
    }
}

#define CHECK_TRUE(label, expression) \
    do { \
        if (!(expression)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, label); \
            failures++; \
        } \
    } while (0)

static apt_bool_t fake_start(void *obj, funasr_generation_t generation)
{
    fake_sink_t *sink = obj;
    (void)generation;
    sink->starts++;
    return TRUE;
}

static apt_bool_t fake_complete(
    void *obj,
    funasr_generation_t generation,
    mrcp_recog_completion_cause_e cause,
    const char *text)
{
    fake_sink_t *sink = obj;
    (void)generation;
    sink->completions++;
    sink->last_cause = cause;
    if (text) {
        snprintf(sink->last_text, sizeof(sink->last_text), "%s", text);
    }
    return TRUE;
}

static apt_bool_t fake_stop(void *obj, funasr_generation_t generation)
{
    fake_sink_t *sink = obj;
    (void)generation;
    sink->stop_responses++;
    fake_sink_record_call(sink, 'S');
    return sink->reject_stop ? FALSE : TRUE;
}

static apt_bool_t fake_close_response(void *obj)
{
    fake_sink_t *sink = obj;
    sink->close_responses++;
    fake_sink_record_call(sink, 'C');
    return sink->reject_close ? FALSE : TRUE;
}

static apt_bool_t fake_cancel(void *obj, funasr_generation_t generation)
{
    fake_sink_t *sink = obj;
    (void)generation;
    sink->cancels++;
    return sink->reject_cancel ? FALSE : TRUE;
}

static apt_bool_t fake_close(void *obj)
{
    fake_sink_t *sink = obj;
    sink->close_requests++;
    return TRUE;
}

static apr_status_t fake_join(void *obj)
{
    fake_sink_t *sink = obj;
    sink->joins++;
    fake_sink_record_call(sink, 'J');
    return sink->reject_join ? APR_EGENERAL : APR_SUCCESS;
}

static const funasr_control_vtable_t fake_vtable = {
    fake_start,
    fake_complete,
    fake_stop,
    fake_close_response,
    fake_cancel,
    fake_close,
    fake_join
};

static funasr_transport_event_t event_make(
    funasr_generation_t generation,
    funasr_transport_event_type_e type)
{
    funasr_transport_event_t event;
    memset(&event, 0, sizeof(event));
    event.generation = generation;
    event.type = type;
    return event;
}

static void test_final_before_stop(void)
{
    funasr_control_t control;
    fake_sink_t sink;
    funasr_transport_event_t event;

    memset(&sink, 0, sizeof(sink));
    funasr_control_init(&control);
    CHECK_TRUE("generation starts", funasr_control_begin_generation(&control, 1));
    event = event_make(1, FUNASR_EVENT_INPUT_STARTED);
    CHECK_TRUE("input event handled",
               funasr_control_handle_event(&control, &event, &fake_vtable, &sink));
    CHECK_TRUE("start sent once", sink.starts == 1);
    CHECK_TRUE("duplicate input handled",
               funasr_control_handle_event(&control, &event, &fake_vtable, &sink));
    CHECK_TRUE("duplicate start suppressed", sink.starts == 1);

    event = event_make(1, FUNASR_EVENT_FINAL_RESULT);
    event.text = "hello";
    CHECK_TRUE("final handled",
               funasr_control_handle_event(&control, &event, &fake_vtable, &sink));
    CHECK_TRUE("one completion", sink.completions == 1);
    CHECK_TRUE("success cause",
               sink.last_cause == RECOGNIZER_COMPLETION_CAUSE_SUCCESS);
    CHECK_TRUE("text forwarded", strcmp(sink.last_text, "hello") == 0);
    CHECK_TRUE("STOP after terminal responds immediately",
               funasr_control_request_stop(&control, &fake_vtable, &sink));
    CHECK_TRUE("STOP after final has one response", sink.stop_responses == 1);
    CHECK_TRUE("STOP after final does not cancel", sink.cancels == 0);
    CHECK_TRUE("no completion plus STOP duplication", sink.completions == 1);
}

static void test_stop_before_final_and_timeout(void)
{
    funasr_control_t control;
    fake_sink_t sink;
    funasr_transport_event_t event;

    memset(&sink, 0, sizeof(sink));
    funasr_control_init(&control);
    funasr_control_begin_generation(&control, 2);
    CHECK_TRUE("STOP requests transport cancel",
               funasr_control_request_stop(&control, &fake_vtable, &sink));
    CHECK_TRUE("one cancel", sink.cancels == 1);
    event = event_make(2, FUNASR_EVENT_FINAL_RESULT);
    event.text = "late";
    funasr_control_handle_event(&control, &event, &fake_vtable, &sink);
    CHECK_TRUE("late final suppressed", sink.completions == 0);
    CHECK_TRUE("STOP response waits for drain", sink.stop_responses == 0);
    event = event_make(2, FUNASR_EVENT_GENERATION_DRAINED);
    event.failure = FUNASR_FAILURE_WRITE_STALL;
    funasr_control_handle_event(&control, &event, &fake_vtable, &sink);
    CHECK_TRUE("drain releases one STOP response", sink.stop_responses == 1);
    funasr_control_handle_event(&control, &event, &fake_vtable, &sink);
    CHECK_TRUE("duplicate drain suppressed", sink.stop_responses == 1);
}

static void test_failure_mapping_and_stale_generation(void)
{
    funasr_control_t control;
    fake_sink_t sink;
    funasr_transport_event_t event;

    memset(&sink, 0, sizeof(sink));
    funasr_control_init(&control);
    funasr_control_begin_generation(&control, 3);
    event = event_make(99, FUNASR_EVENT_FINAL_RESULT);
    funasr_control_handle_event(&control, &event, &fake_vtable, &sink);
    CHECK_TRUE("stale generation dropped", sink.completions == 0);
    CHECK_TRUE("stale generation counted", control.stale_events == 1);

    event = event_make(3, FUNASR_EVENT_TRANSPORT_FAILED);
    event.failure = FUNASR_FAILURE_NO_RESULT_TIMEOUT;
    funasr_control_handle_event(&control, &event, &fake_vtable, &sink);
    CHECK_TRUE("timeout completes once", sink.completions == 1);
    CHECK_TRUE("timeout cause mapped",
               sink.last_cause == RECOGNIZER_COMPLETION_CAUSE_NO_INPUT_TIMEOUT);
    event.failure = FUNASR_FAILURE_QUEUE_OVERRUN;
    funasr_control_handle_event(&control, &event, &fake_vtable, &sink);
    CHECK_TRUE("second terminal suppressed", sink.completions == 1);
    CHECK_TRUE("duplicate terminal counted",
               control.duplicate_terminal_events == 1);
}

static void test_stop_wins_failure_race(void)
{
    funasr_control_t control;
    fake_sink_t sink;
    funasr_transport_event_t event;

    memset(&sink, 0, sizeof(sink));
    funasr_control_init(&control);
    funasr_control_begin_generation(&control, 4);
    funasr_control_request_stop(&control, &fake_vtable, &sink);
    event = event_make(4, FUNASR_EVENT_TRANSPORT_FAILED);
    event.failure = FUNASR_FAILURE_QUEUE_OVERRUN;
    funasr_control_handle_event(&control, &event, &fake_vtable, &sink);
    CHECK_TRUE("pending STOP suppresses failure completion", sink.completions == 0);
    event = event_make(4, FUNASR_EVENT_GENERATION_DRAINED);
    funasr_control_handle_event(&control, &event, &fake_vtable, &sink);
    CHECK_TRUE("pending STOP produces one response", sink.stop_responses == 1);
    CHECK_TRUE("next generation now allowed",
               funasr_control_begin_generation(&control, 5));
    CHECK_TRUE("new generation accepts media", control.accepting_media);
}

static void test_stop_during_terminal_commit_responds(void)
{
    funasr_control_t control;
    fake_sink_t sink;
    funasr_transport_event_t event;

    memset(&sink, 0, sizeof(sink));
    sink.reject_cancel = TRUE;
    funasr_control_init(&control);
    funasr_control_begin_generation(&control, 8);
    CHECK_TRUE("late STOP falls back to an immediate response",
               funasr_control_request_stop(&control, &fake_vtable, &sink));
    CHECK_TRUE("late STOP attempted one transport cancel", sink.cancels == 1);
    CHECK_TRUE("late STOP receives one response", sink.stop_responses == 1);
    event = event_make(8, FUNASR_EVENT_FINAL_RESULT);
    event.text = "already-committed";
    funasr_control_handle_event(&control, &event, &fake_vtable, &sink);
    CHECK_TRUE("committed final after STOP is suppressed",
               sink.completions == 0);
    CHECK_TRUE("committed final is counted as duplicate terminal",
               control.duplicate_terminal_events == 1);
}

static void test_close_fence_is_only_close_release(void)
{
    funasr_control_t control;
    fake_sink_t sink;
    funasr_transport_event_t event;

    memset(&sink, 0, sizeof(sink));
    funasr_control_init(&control);
    funasr_control_begin_generation(&control, 6);
    CHECK_TRUE("close requested",
               funasr_control_request_close(&control, &fake_vtable, &sink));
    CHECK_TRUE("worker close requested once", sink.close_requests == 1);
    event = event_make(6, FUNASR_EVENT_FINAL_RESULT);
    funasr_control_handle_event(&control, &event, &fake_vtable, &sink);
    CHECK_TRUE("ordinary event after close suppressed", sink.completions == 0);
    CHECK_TRUE("close not acknowledged early", sink.close_responses == 0);
    event = event_make(6, FUNASR_EVENT_WORKER_CLOSED);
    CHECK_TRUE("close fence handled",
               funasr_control_handle_event(&control, &event, &fake_vtable, &sink));
    CHECK_TRUE("join happens once", sink.joins == 1);
    CHECK_TRUE("close response after join", sink.close_responses == 1);
    funasr_control_handle_event(&control, &event, &fake_vtable, &sink);
    CHECK_TRUE("duplicate fence suppressed", sink.joins == 1);
}

static void test_worker_close_settles_pending_stop(void)
{
    funasr_control_t control;
    fake_sink_t sink;
    funasr_transport_event_t event;

    memset(&sink, 0, sizeof(sink));
    funasr_control_init(&control);
    funasr_control_begin_generation(&control, 9);
    CHECK_TRUE("STOP requests transport cancel",
               funasr_control_request_stop(&control, &fake_vtable, &sink));
    event = event_make(9, FUNASR_EVENT_WORKER_CLOSED);
    CHECK_TRUE("worker close settles pending STOP",
               funasr_control_handle_event(&control, &event, &fake_vtable, &sink));
    CHECK_TRUE("worker close joins once", sink.joins == 1);
    CHECK_TRUE("worker close sends STOP response", sink.stop_responses == 1);
    CHECK_TRUE("worker close sends close response", sink.close_responses == 1);
    CHECK_TRUE("worker close responses are ordered after join",
               strcmp(sink.call_order, "JSC") == 0);
    CHECK_TRUE("worker close commits closed state", control.worker_closed);
    CHECK_TRUE("worker close records joined worker", control.worker_joined);
    CHECK_TRUE("worker close settles STOP state", !control.stop_pending);
    CHECK_TRUE("worker close commits terminal state", control.terminal);
}

static void test_worker_close_retries_stop_after_send_failure(void)
{
    funasr_control_t control;
    fake_sink_t sink;
    funasr_transport_event_t event;

    memset(&sink, 0, sizeof(sink));
    sink.reject_stop = TRUE;
    funasr_control_init(&control);
    funasr_control_begin_generation(&control, 10);
    CHECK_TRUE("STOP requests transport cancel",
               funasr_control_request_stop(&control, &fake_vtable, &sink));
    event = event_make(10, FUNASR_EVENT_WORKER_CLOSED);
    CHECK_TRUE("worker close reports rejected STOP response",
               !funasr_control_handle_event(&control, &event, &fake_vtable, &sink));
    CHECK_TRUE("rejected STOP joins once", sink.joins == 1);
    CHECK_TRUE("rejected STOP sends no close response", sink.close_responses == 0);
    CHECK_TRUE("rejected STOP leaves fence uncommitted", !control.worker_closed);
    CHECK_TRUE("rejected STOP stays pending", control.stop_pending);
    CHECK_TRUE("rejected STOP leaves terminal uncommitted", !control.terminal);

    sink.reject_stop = FALSE;
    CHECK_TRUE("worker close retries STOP response",
               funasr_control_handle_event(&control, &event, &fake_vtable, &sink));
    CHECK_TRUE("retried close joins again", sink.joins == 2);
    CHECK_TRUE("retried close attempts STOP twice", sink.stop_responses == 2);
    CHECK_TRUE("retried STOP is followed by close response", sink.close_responses == 1);
    CHECK_TRUE("retried close preserves response order",
               strcmp(sink.call_order, "JSJSC") == 0);
    CHECK_TRUE("retried close commits closed state", control.worker_closed);
    CHECK_TRUE("retried close settles STOP state", !control.stop_pending);
    CHECK_TRUE("retried close commits terminal state", control.terminal);
}

static void test_worker_close_join_failure_preserves_fence(void)
{
    funasr_control_t control;
    fake_sink_t sink;
    funasr_transport_event_t event;

    memset(&sink, 0, sizeof(sink));
    sink.reject_join = TRUE;
    funasr_control_init(&control);
    funasr_control_begin_generation(&control, 11);
    CHECK_TRUE("STOP requests transport cancel",
               funasr_control_request_stop(&control, &fake_vtable, &sink));
    event = event_make(11, FUNASR_EVENT_WORKER_CLOSED);
    CHECK_TRUE("worker close reports join failure",
               !funasr_control_handle_event(&control, &event, &fake_vtable, &sink));
    CHECK_TRUE("rejected join attempted once", sink.joins == 1);
    CHECK_TRUE("rejected join sends no STOP response", sink.stop_responses == 0);
    CHECK_TRUE("rejected join sends no close response", sink.close_responses == 0);
    CHECK_TRUE("rejected join leaves worker unjoined", !control.worker_joined);
    CHECK_TRUE("rejected join leaves fence uncommitted", !control.worker_closed);
    CHECK_TRUE("rejected join keeps STOP pending", control.stop_pending);
}

static void test_worker_close_retries_close_without_resending_stop(void)
{
    funasr_control_t control;
    fake_sink_t sink;
    funasr_transport_event_t event;

    memset(&sink, 0, sizeof(sink));
    sink.reject_close = TRUE;
    funasr_control_init(&control);
    funasr_control_begin_generation(&control, 12);
    CHECK_TRUE("STOP requests transport cancel",
               funasr_control_request_stop(&control, &fake_vtable, &sink));
    event = event_make(12, FUNASR_EVENT_WORKER_CLOSED);
    CHECK_TRUE("rejected close reports failed fence",
               !funasr_control_handle_event(&control, &event, &fake_vtable, &sink));
    CHECK_TRUE("first fence joins once", sink.joins == 1);
    CHECK_TRUE("first fence sends STOP once", sink.stop_responses == 1);
    CHECK_TRUE("first fence attempts close once", sink.close_responses == 1);
    CHECK_TRUE("first fence preserves STOP settlement", control.stop_responded);
    CHECK_TRUE("first fence keeps close uncommitted", !control.worker_closed);
    CHECK_TRUE("first fence keeps STOP pending", control.stop_pending);

    sink.reject_close = FALSE;
    CHECK_TRUE("same fence retries close",
               funasr_control_handle_event(&control, &event, &fake_vtable, &sink));
    CHECK_TRUE("retry joins only as needed", sink.joins == 2);
    CHECK_TRUE("retry does not resend STOP", sink.stop_responses == 1);
    CHECK_TRUE("retry sends close response", sink.close_responses == 2);
    CHECK_TRUE("retry order skips STOP", strcmp(sink.call_order, "JSCJC") == 0);
    CHECK_TRUE("retry commits worker close", control.worker_closed);
    CHECK_TRUE("retry settles STOP", !control.stop_pending && control.stop_responded);
    CHECK_TRUE("retry commits terminal", control.terminal);
}

static void test_unexpected_close_fence_releases_transport(void)
{
    funasr_control_t control;
    fake_sink_t sink;
    funasr_transport_event_t event;

    memset(&sink, 0, sizeof(sink));
    funasr_control_init(&control);
    funasr_control_begin_generation(&control, 7);
    event = event_make(7, FUNASR_EVENT_WORKER_CLOSED);

    CHECK_TRUE("unexpected close fence handled",
               funasr_control_handle_event(&control, &event, &fake_vtable, &sink));
    CHECK_TRUE("unexpected close joins worker", sink.joins == 1);
    CHECK_TRUE("unexpected close marks worker closed", control.worker_closed);
    CHECK_TRUE("unexpected close releases transport", sink.close_responses == 1);
    CHECK_TRUE("unexpected close does not request a second close",
               sink.close_requests == 0);
    CHECK_TRUE("duplicate unexpected close fence suppressed",
               funasr_control_handle_event(&control, &event, &fake_vtable, &sink));
    CHECK_TRUE("unexpected close joins once", sink.joins == 1);
    CHECK_TRUE("unexpected close releases once", sink.close_responses == 1);
}

int main(void)
{
    test_final_before_stop();
    test_stop_before_final_and_timeout();
    test_failure_mapping_and_stale_generation();
    test_stop_wins_failure_race();
    test_stop_during_terminal_commit_responds();
    test_close_fence_is_only_close_release();
    test_worker_close_settles_pending_stop();
    test_worker_close_retries_stop_after_send_failure();
    test_worker_close_join_failure_preserves_fence();
    test_worker_close_retries_close_without_resending_stop();
    test_unexpected_close_fence_releases_transport();
    if (failures != 0) {
        fprintf(stderr, "%d control assertion(s) failed\n", failures);
        return 1;
    }
    printf("PASS test_funasr_control\n");
    return 0;
}
