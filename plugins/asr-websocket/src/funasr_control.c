#include "funasr_control.h"

#include <string.h>

static apt_bool_t funasr_control_vtable_valid(
    const funasr_control_vtable_t *vtable)
{
    return vtable &&
        vtable->send_start_of_input &&
        vtable->send_recognition_complete &&
        vtable->send_stop_response &&
        vtable->send_close_response &&
        vtable->cancel_generation &&
        vtable->request_close &&
        vtable->join_closed;
}

void funasr_control_init(funasr_control_t *control)
{
    if (control) {
        memset(control, 0, sizeof(*control));
    }
}

apt_bool_t funasr_control_begin_generation(
    funasr_control_t *control,
    funasr_generation_t generation)
{
    if (!control || generation == 0 || control->active ||
        control->stop_pending || control->close_pending) {
        return FALSE;
    }
    control->generation = generation;
    control->active = TRUE;
    control->terminal = FALSE;
    control->input_started = FALSE;
    control->accepting_media = TRUE;
    return TRUE;
}

apt_bool_t funasr_control_request_stop(
    funasr_control_t *control,
    const funasr_control_vtable_t *vtable,
    void *obj)
{
    if (!control || !funasr_control_vtable_valid(vtable) ||
        control->close_pending || control->stop_pending) {
        return FALSE;
    }
    control->accepting_media = FALSE;
    if (!control->active || control->terminal) {
        control->active = FALSE;
        return vtable->send_stop_response(obj, control->generation);
    }
    control->stop_pending = TRUE;
    if (vtable->cancel_generation(obj, control->generation)) {
        return TRUE;
    }
    control->stop_pending = FALSE;
    control->active = FALSE;
    control->terminal = TRUE;
    return vtable->send_stop_response(obj, control->generation);
}

apt_bool_t funasr_control_request_close(
    funasr_control_t *control,
    const funasr_control_vtable_t *vtable,
    void *obj)
{
    if (!control || !funasr_control_vtable_valid(vtable) ||
        control->close_pending || control->worker_closed) {
        return FALSE;
    }
    control->close_pending = TRUE;
    control->accepting_media = FALSE;
    control->active = FALSE;
    return vtable->request_close(obj);
}

static mrcp_recog_completion_cause_e funasr_control_failure_cause(
    funasr_transport_failure_e failure)
{
    if (failure == FUNASR_FAILURE_NO_RESULT_TIMEOUT) {
        return RECOGNIZER_COMPLETION_CAUSE_NO_INPUT_TIMEOUT;
    }
    return RECOGNIZER_COMPLETION_CAUSE_ERROR;
}

apt_bool_t funasr_control_handle_event(
    funasr_control_t *control,
    const funasr_transport_event_t *event,
    const funasr_control_vtable_t *vtable,
    void *obj)
{
    if (!control || !event || !funasr_control_vtable_valid(vtable)) {
        return FALSE;
    }

    if (event->type == FUNASR_EVENT_WORKER_CLOSED) {
        if (control->worker_closed) {
            return TRUE;
        }
        if (vtable->join_closed(obj) != APR_SUCCESS) {
            return FALSE;
        }
        if (control->stop_pending) {
            if (!vtable->send_stop_response(obj, control->generation)) {
                return FALSE;
            }
        }
        if (!vtable->send_close_response(obj)) {
            return FALSE;
        }
        control->worker_closed = TRUE;
        control->close_pending = FALSE;
        control->active = FALSE;
        if (control->stop_pending) {
            control->stop_pending = FALSE;
            control->terminal = TRUE;
            control->accepting_media = FALSE;
        }
        return TRUE;
    }

    if (event->generation != control->generation) {
        control->stale_events++;
        return TRUE;
    }
    if (event->type == FUNASR_EVENT_TRANSPORT_METRICS) {
        return TRUE;
    }
    if (control->close_pending || control->worker_closed) {
        return TRUE;
    }

    switch (event->type) {
        case FUNASR_EVENT_INPUT_STARTED:
            if (!control->active || control->terminal ||
                control->stop_pending || control->input_started) {
                return TRUE;
            }
            control->input_started = TRUE;
            return vtable->send_start_of_input(obj, control->generation);

        case FUNASR_EVENT_FINAL_RESULT:
        case FUNASR_EVENT_TRANSPORT_FAILED:
            if (control->terminal) {
                control->duplicate_terminal_events++;
                return TRUE;
            }
            control->terminal = TRUE;
            control->accepting_media = FALSE;
            if (control->stop_pending) {
                return TRUE;
            }
            control->active = FALSE;
            if (event->type == FUNASR_EVENT_FINAL_RESULT) {
                return vtable->send_recognition_complete(
                    obj,
                    control->generation,
                    RECOGNIZER_COMPLETION_CAUSE_SUCCESS,
                    event->text);
            }
            return vtable->send_recognition_complete(
                obj,
                control->generation,
                funasr_control_failure_cause(event->failure),
                NULL);

        case FUNASR_EVENT_GENERATION_DRAINED:
            if (!control->stop_pending) {
                return TRUE;
            }
            control->stop_pending = FALSE;
            control->active = FALSE;
            control->terminal = TRUE;
            control->accepting_media = FALSE;
            return vtable->send_stop_response(obj, control->generation);

        case FUNASR_EVENT_TRANSPORT_METRICS:
        case FUNASR_EVENT_WORKER_CLOSED:
            return TRUE;
    }
    return FALSE;
}
