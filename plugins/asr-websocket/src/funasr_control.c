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
    control->stop_responded = FALSE;
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
        apt_bool_t result;
        control->active = FALSE;
        result = vtable->send_stop_response(obj, control->generation);
        if (result) control->stop_responded = TRUE;
        return result;
    }
    control->stop_pending = TRUE;
    if (vtable->cancel_generation(obj, control->generation)) {
        return TRUE;
    }
    control->stop_pending = FALSE;
    control->active = FALSE;
    control->terminal = TRUE;
    if (!vtable->send_stop_response(obj, control->generation)) {
        return FALSE;
    }
    control->stop_responded = TRUE;
    return TRUE;
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
    if (failure == FUNASR_FAILURE_FIRST_AUDIO_RESULT_TIMEOUT ||
        failure == FUNASR_FAILURE_NO_RESULT_TIMEOUT) {
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
            apt_bool_t result;

            if (control->close_responded) {
                return TRUE;
            }
            /* Commit the state before the callback.  A successful close
             * response may release the channel pool on another task. */
            control->close_responded = TRUE;
            result = vtable->send_close_response(obj);
            if (!result) {
                control->close_responded = FALSE;
            }
            return result;
        }
        if (vtable->join_closed(obj) != APR_SUCCESS) {
            return FALSE;
        }
        control->worker_joined = TRUE;
        if (control->stop_pending && !control->stop_responded) {
            if (!vtable->send_stop_response(obj, control->generation)) {
                return FALSE;
            }
            control->stop_responded = TRUE;
        }
        control->worker_closed = TRUE;
        control->close_pending = FALSE;
        control->active = FALSE;
        if (control->stop_pending) {
            control->stop_pending = FALSE;
            control->terminal = TRUE;
            control->accepting_media = FALSE;
        }
        /* This callback must be the final channel operation on success. */
        control->close_responded = TRUE;
        if (!vtable->send_close_response(obj)) {
            control->close_responded = FALSE;
            return FALSE;
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
            if (!vtable->send_stop_response(obj, control->generation)) {
                return FALSE;
            }
            control->stop_responded = TRUE;
            control->stop_pending = FALSE;
            control->active = FALSE;
            control->terminal = TRUE;
            control->accepting_media = FALSE;
            return TRUE;

        case FUNASR_EVENT_TRANSPORT_METRICS:
        case FUNASR_EVENT_WORKER_CLOSED:
            return TRUE;
    }
    return FALSE;
}
