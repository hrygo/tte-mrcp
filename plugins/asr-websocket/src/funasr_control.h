#ifndef FUNASR_CONTROL_H
#define FUNASR_CONTROL_H

#include "funasr_ws_transport.h"
#include "mrcp_recog_header.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct funasr_control_t {
    funasr_generation_t generation;
    apr_uint64_t stale_events;
    apr_uint64_t duplicate_terminal_events;
    apt_bool_t active;
    apt_bool_t terminal;
    apt_bool_t input_started;
    apt_bool_t stop_pending;
    apt_bool_t close_pending;
    apt_bool_t accepting_media;
    apt_bool_t worker_closed;
    apt_bool_t worker_joined;
} funasr_control_t;

typedef struct funasr_control_vtable_t {
    apt_bool_t (*send_start_of_input)(
        void *obj,
        funasr_generation_t generation);
    apt_bool_t (*send_recognition_complete)(
        void *obj,
        funasr_generation_t generation,
        mrcp_recog_completion_cause_e cause,
        const char *text);
    apt_bool_t (*send_stop_response)(
        void *obj,
        funasr_generation_t generation);
    apt_bool_t (*send_close_response)(void *obj);
    apt_bool_t (*cancel_generation)(
        void *obj,
        funasr_generation_t generation);
    apt_bool_t (*request_close)(void *obj);
    apr_status_t (*join_closed)(void *obj);
} funasr_control_vtable_t;

void funasr_control_init(funasr_control_t *control);

apt_bool_t funasr_control_begin_generation(
    funasr_control_t *control,
    funasr_generation_t generation);

apt_bool_t funasr_control_request_stop(
    funasr_control_t *control,
    const funasr_control_vtable_t *vtable,
    void *obj);

apt_bool_t funasr_control_request_close(
    funasr_control_t *control,
    const funasr_control_vtable_t *vtable,
    void *obj);

apt_bool_t funasr_control_handle_event(
    funasr_control_t *control,
    const funasr_transport_event_t *event,
    const funasr_control_vtable_t *vtable,
    void *obj);

#ifdef __cplusplus
}
#endif

#endif
