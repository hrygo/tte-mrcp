#include "mrcp_recog_engine.h"
#include "apt_consumer_task.h"
#include "apt_log.h"
#include "funasr_audio.h"
#include "funasr_clock.h"
#include "funasr_close_fence_retry.h"
#include "funasr_control.h"
#include "funasr_timeout_config.h"
#include "funasr_ws_transport.h"

#include <apr_strings.h>
#include <apr_uuid.h>
#include <stdlib.h>
#include <string.h>

#define RECOG_ENGINE_TASK_NAME "ASR WebSocket Engine"
#define FUNASR_SERVER_HOST "40.20.85.37"
#define FUNASR_SERVER_PORT 8888
#define FUNASR_SERVER_PATH "/ws/audio"
#define FUNASR_RESAMPLE_WINDOW_MS 200U

typedef struct funasr_engine_t funasr_engine_t;
typedef struct funasr_channel_t funasr_channel_t;
typedef struct funasr_registry_entry_t funasr_registry_entry_t;
typedef struct funasr_event_bridge_t funasr_event_bridge_t;
typedef struct funasr_msg_t funasr_msg_t;

#define LOG_WITH_SID(channel, priority, format, ...) \
    apt_log(APT_LOG_MARK, priority, "asr_websocket: [session_id=%s] " format, \
        (channel) && (channel)->session_id ? (channel)->session_id : "N/A", \
        ##__VA_ARGS__)

typedef enum funasr_msg_type_e {
    FUNASR_MSG_OPEN_CHANNEL,
    FUNASR_MSG_CLOSE_CHANNEL,
    FUNASR_MSG_REQUEST_PROCESS,
    FUNASR_MSG_ENGINE_CLOSE,
    FUNASR_MSG_TRANSPORT_EVENT
} funasr_msg_type_e;

struct funasr_registry_entry_t {
    funasr_transport_id_t id;
    funasr_transport_t *transport;
    funasr_channel_t *channel;
    funasr_registry_entry_t *next;
};

struct funasr_engine_t {
    apr_pool_t *pool;
    mrcp_engine_t *engine;
    apt_consumer_task_t *task;
    char *server_host;
    apr_port_t server_port;
    char *server_path;
    apr_interval_time_t first_audio_result_timeout_us;
    apr_interval_time_t last_speech_result_timeout_us;
    funasr_transport_id_t next_transport_id;
    funasr_registry_entry_t *registry;
    apt_bool_t quiescing;
    apt_bool_t close_responded;
};

struct funasr_channel_t {
    apr_pool_t *pool;
    funasr_engine_t *engine;
    mrcp_engine_channel_t *channel;
    funasr_registry_entry_t *registry_entry;
    funasr_transport_t *transport;
    funasr_control_t control;
    funasr_generation_t next_generation;
    mrcp_message_t *recog_request;
    mrcp_message_t *stop_response;
    unsigned char *scratch;
    apr_size_t scratch_capacity;
    funasr_resample_state_t resample;
    funasr_clock_t clock;
    char *session_id;
    apt_bool_t close_response_pending;
};

struct funasr_event_bridge_t {
    funasr_engine_t *engine;
    apt_task_msg_t *close_fence_msg;
};

struct funasr_msg_t {
    funasr_msg_type_e type;
    mrcp_engine_channel_t *channel;
    mrcp_message_t *request;
    funasr_transport_event_t *event;
};

static apt_bool_t funasr_engine_destroy(mrcp_engine_t *engine);
static apt_bool_t funasr_engine_open(mrcp_engine_t *engine);
static apt_bool_t funasr_engine_close(mrcp_engine_t *engine);
static mrcp_engine_channel_t *funasr_engine_channel_create(
    mrcp_engine_t *engine,
    apr_pool_t *pool);
static apt_bool_t funasr_channel_destroy(mrcp_engine_channel_t *channel);
static apt_bool_t funasr_channel_open(mrcp_engine_channel_t *channel);
static apt_bool_t funasr_channel_close(mrcp_engine_channel_t *channel);
static apt_bool_t funasr_channel_request_process(
    mrcp_engine_channel_t *channel,
    mrcp_message_t *request);
static apt_bool_t funasr_stream_destroy(mpf_audio_stream_t *stream);
static apt_bool_t funasr_stream_open(
    mpf_audio_stream_t *stream,
    mpf_codec_t *codec);
static apt_bool_t funasr_stream_close(mpf_audio_stream_t *stream);
static apt_bool_t funasr_stream_write(
    mpf_audio_stream_t *stream,
    const mpf_frame_t *frame);
static apt_bool_t funasr_msg_process(apt_task_t *task, apt_task_msg_t *msg);
static void funasr_engine_maybe_close_respond(funasr_engine_t *engine);
static funasr_registry_entry_t *funasr_registry_find(
    funasr_engine_t *engine,
    funasr_transport_id_t id);
static void funasr_registry_remove(
    funasr_engine_t *engine,
    funasr_registry_entry_t *target);

static const mrcp_engine_method_vtable_t engine_vtable = {
    funasr_engine_destroy,
    funasr_engine_open,
    funasr_engine_close,
    funasr_engine_channel_create
};

static const mrcp_engine_channel_method_vtable_t channel_vtable = {
    funasr_channel_destroy,
    funasr_channel_open,
    funasr_channel_close,
    funasr_channel_request_process
};

static const mpf_audio_stream_vtable_t audio_stream_vtable = {
    funasr_stream_destroy,
    NULL,
    NULL,
    NULL,
    funasr_stream_open,
    funasr_stream_close,
    funasr_stream_write,
    NULL
};

static apt_bool_t funasr_task_signal(
    funasr_engine_t *engine,
    funasr_msg_type_e type,
    mrcp_engine_channel_t *channel,
    mrcp_message_t *request,
    funasr_transport_event_t *event)
{
    apt_task_t *task;
    apt_task_msg_t *msg;
    funasr_msg_t *payload;

    if (!engine || !engine->task) {
        return FALSE;
    }
    task = apt_consumer_task_base_get(engine->task);
    msg = apt_task_msg_get(task);
    if (!msg) {
        return FALSE;
    }
    msg->type = TASK_MSG_USER;
    payload = (funasr_msg_t *)msg->data;
    payload->type = type;
    payload->channel = channel;
    payload->request = request;
    payload->event = event;
    return apt_task_msg_signal(task, msg);
}

static void funasr_close_fence_abort(
    funasr_engine_t *engine,
    funasr_registry_entry_t *entry,
    funasr_transport_event_t *event)
{
    funasr_channel_t *channel = entry->channel;

    if (!channel || !channel->control.worker_joined) {
        apt_log(
            APT_LOG_MARK,
            APT_PRIO_ERROR,
            "asr_websocket: refusing unsafe STOP close fence fallback before worker join");
        return;
    }
    LOG_WITH_SID(
        channel,
        APT_PRIO_ERROR,
        "STOP close fence retries exhausted generation=%lu retries=%u; "
        "releasing joined transport without a STOP response",
        (unsigned long)event->generation,
        (unsigned int)event->close_fence_retry_count);
    funasr_registry_remove(engine, entry);
    channel->transport = NULL;
    channel->stop_response = NULL;
    channel->recog_request = NULL;
    channel->control.active = FALSE;
    channel->control.terminal = TRUE;
    channel->control.stop_pending = FALSE;
    channel->control.close_pending = FALSE;
    channel->control.accepting_media = FALSE;
    channel->control.worker_closed = TRUE;
    if (channel->close_response_pending) {
        channel->close_response_pending = FALSE;
        if (!mrcp_engine_channel_close_respond(channel->channel)) {
            LOG_WITH_SID(
                channel,
                APT_PRIO_ERROR,
                "forced channel close response failed after STOP fence exhaustion");
        }
    }
    funasr_engine_maybe_close_respond(engine);
}

static apt_bool_t funasr_close_fence_requeue(void *obj, funasr_transport_event_t *event)
{
    return funasr_task_signal(obj, FUNASR_MSG_TRANSPORT_EVENT, NULL, NULL, event);
}
static void funasr_close_fence_release(void *obj, funasr_transport_event_t *event)
{
    (void)obj;
    funasr_transport_event_destroy(event);
}
static apt_bool_t funasr_close_fence_joined(void *obj, funasr_transport_event_t *event)
{
    funasr_engine_t *engine = obj;
    funasr_registry_entry_t *entry;
    entry = funasr_registry_find(engine, event->transport_id);
    return entry && entry->channel && entry->channel->control.worker_joined;
}
static void funasr_close_fence_fallback(void *obj, funasr_transport_event_t *event)
{
    funasr_engine_t *engine = obj;
    funasr_registry_entry_t *entry = funasr_registry_find(engine, event->transport_id);
    if (entry) funasr_close_fence_abort(engine, entry, event);
}
static void funasr_close_fence_retain(void *obj, funasr_transport_event_t *event)
{
    (void)obj;
    apt_log(APT_LOG_MARK, APT_PRIO_ERROR, "asr_websocket: retaining registry after unjoined close fence transport=%lu", (unsigned long)event->transport_id);
}
static void *funasr_close_fence_retry_allocate(
    void *obj,
    apr_pool_t *pool,
    apr_size_t size)
{
    (void)obj;
    return apr_pcalloc(pool, size);
}
static const funasr_close_fence_retry_vtable_t funasr_close_fence_retry_vtable = {
    funasr_close_fence_requeue, funasr_close_fence_release, funasr_close_fence_joined,
    funasr_close_fence_fallback, funasr_close_fence_retain,
    funasr_close_fence_retry_allocate
};

static apt_bool_t funasr_transport_event_sink(
    void *obj,
    funasr_transport_event_t *event)
{
    funasr_event_bridge_t *bridge;
    apt_task_msg_t *msg;
    funasr_msg_t *payload;

    bridge = obj;
    if (event->type != FUNASR_EVENT_WORKER_CLOSED) {
        return funasr_task_signal(
            bridge->engine,
            FUNASR_MSG_TRANSPORT_EVENT,
            NULL,
            NULL,
            event);
    }
    msg = bridge->close_fence_msg;
    bridge->close_fence_msg = NULL;
    if (!msg) {
        return FALSE;
    }
    msg->type = TASK_MSG_USER;
    payload = (funasr_msg_t *)msg->data;
    payload->type = FUNASR_MSG_TRANSPORT_EVENT;
    payload->channel = NULL;
    payload->request = NULL;
    payload->event = event;
    return apt_task_msg_signal(
        apt_consumer_task_base_get(bridge->engine->task),
        msg);
}

static funasr_registry_entry_t *funasr_registry_find(
    funasr_engine_t *engine,
    funasr_transport_id_t id)
{
    funasr_registry_entry_t *entry;

    for (entry = engine->registry; entry; entry = entry->next) {
        if (entry->id == id) {
            return entry;
        }
    }
    return NULL;
}

static void funasr_registry_remove(
    funasr_engine_t *engine,
    funasr_registry_entry_t *target)
{
    funasr_registry_entry_t **link;

    link = &engine->registry;
    while (*link) {
        if (*link == target) {
            *link = target->next;
            target->channel = NULL;
            target->next = NULL;
            return;
        }
        link = &(*link)->next;
    }
}

static void funasr_engine_maybe_close_respond(funasr_engine_t *engine)
{
    if (engine->quiescing && !engine->registry && !engine->close_responded) {
        engine->close_responded = TRUE;
        mrcp_engine_close_respond(engine->engine);
    }
}

MRCP_PLUGIN_DECLARE(mrcp_engine_t *) mrcp_plugin_create(apr_pool_t *pool)
{
    funasr_engine_t *funasr_engine;
    apt_task_msg_pool_t *msg_pool;
    apt_task_t *task;
    apt_task_vtable_t *vtable;

    funasr_engine = apr_pcalloc(pool, sizeof(*funasr_engine));
    funasr_engine->pool = pool;
    funasr_engine->next_transport_id = 1;
    msg_pool = apt_task_msg_pool_create_dynamic(sizeof(funasr_msg_t), pool);
    funasr_engine->task = apt_consumer_task_create(
        funasr_engine,
        msg_pool,
        pool);
    if (!funasr_engine->task) {
        return NULL;
    }
    task = apt_consumer_task_base_get(funasr_engine->task);
    apt_task_name_set(task, RECOG_ENGINE_TASK_NAME);
    vtable = apt_task_vtable_get(task);
    if (!vtable) {
        return NULL;
    }
    vtable->process_msg = funasr_msg_process;
    funasr_engine->engine = mrcp_engine_create(
        MRCP_RECOGNIZER_RESOURCE,
        funasr_engine,
        &engine_vtable,
        pool);
    return funasr_engine->engine;
}

static apt_bool_t funasr_engine_destroy(mrcp_engine_t *engine)
{
    funasr_engine_t *funasr_engine;

    funasr_engine = engine->obj;
    if (funasr_engine->registry) {
        apt_log(
            APT_LOG_MARK,
            APT_PRIO_ERROR,
            "asr_websocket: refusing clean destroy with live transports");
        return FALSE;
    }
    if (funasr_engine->task) {
        apt_task_destroy(apt_consumer_task_base_get(funasr_engine->task));
        funasr_engine->task = NULL;
    }
    return TRUE;
}

static apr_interval_time_t funasr_engine_timeout_param_get(
    mrcp_engine_t *engine,
    const char *name,
    apr_interval_time_t fallback)
{
    const char *value;
    funasr_timeout_value_e status;
    apr_interval_time_t timeout;

    value = mrcp_engine_param_get(engine, name);
    timeout = funasr_timeout_ms_parse(value, fallback, &status);
    if (status == FUNASR_TIMEOUT_VALUE_INVALID) {
        apt_log(
            APT_LOG_MARK,
            APT_PRIO_WARNING,
            "asr_websocket: invalid %s; using default timeout_ms=%ld",
            name,
            (long)(fallback / 1000));
    }
    return timeout;
}

static apt_bool_t funasr_engine_open(mrcp_engine_t *engine)
{
    funasr_engine_t *funasr_engine;
    const char *host;
    const char *port;
    const char *path;

    funasr_engine = engine->obj;
    host = mrcp_engine_param_get(engine, "funasr-host");
    port = mrcp_engine_param_get(engine, "funasr-port");
    path = mrcp_engine_param_get(engine, "funasr-path");
    funasr_engine->server_host = apr_pstrdup(
        funasr_engine->pool,
        host ? host : FUNASR_SERVER_HOST);
    funasr_engine->server_port = port ?
        (apr_port_t)atoi(port) : FUNASR_SERVER_PORT;
    funasr_engine->server_path = apr_pstrdup(
        funasr_engine->pool,
        path ? path : FUNASR_SERVER_PATH);
    funasr_engine->first_audio_result_timeout_us =
        funasr_engine_timeout_param_get(
            engine,
            "first-audio-result-timeout-ms",
            FUNASR_FIRST_AUDIO_RESULT_TIMEOUT_US);
    funasr_engine->last_speech_result_timeout_us =
        funasr_engine_timeout_param_get(
            engine,
            "last-speech-result-timeout-ms",
            FUNASR_LAST_SPEECH_RESULT_TIMEOUT_US);
    apt_task_start(apt_consumer_task_base_get(funasr_engine->task));
    apt_log(
        APT_LOG_MARK,
        APT_PRIO_INFO,
        "asr_websocket: transport worker endpoint configured host=%s port=%u path=%s first_audio_result_timeout_ms=%ld last_speech_result_timeout_ms=%ld",
        funasr_engine->server_host,
        (unsigned int)funasr_engine->server_port,
        funasr_engine->server_path,
        (long)(funasr_engine->first_audio_result_timeout_us / 1000),
        (long)(funasr_engine->last_speech_result_timeout_us / 1000));
    return mrcp_engine_open_respond(engine, TRUE);
}

static apt_bool_t funasr_engine_close(mrcp_engine_t *engine)
{
    funasr_engine_t *funasr_engine = engine->obj;
    return funasr_task_signal(
        funasr_engine,
        FUNASR_MSG_ENGINE_CLOSE,
        NULL,
        NULL,
        NULL);
}

static mrcp_engine_channel_t *funasr_engine_channel_create(
    mrcp_engine_t *engine,
    apr_pool_t *pool)
{
    funasr_channel_t *channel;
    mpf_stream_capabilities_t *capabilities;
    mpf_termination_t *termination;
    apr_uuid_t uuid;

    channel = apr_pcalloc(pool, sizeof(*channel));
    channel->pool = pool;
    channel->engine = engine->obj;
    channel->next_generation = 1;
    funasr_control_init(&channel->control);
    funasr_clock_default(&channel->clock);
    apr_uuid_get(&uuid);
    channel->session_id = apr_palloc(pool, 64);
    apr_snprintf(
        channel->session_id,
        64,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        uuid.data[0], uuid.data[1], uuid.data[2], uuid.data[3],
        uuid.data[4], uuid.data[5], uuid.data[6], uuid.data[7],
        uuid.data[8], uuid.data[9], uuid.data[10], uuid.data[11],
        uuid.data[12], uuid.data[13], uuid.data[14], uuid.data[15]);

    capabilities = mpf_sink_stream_capabilities_create(pool);
    mpf_codec_capabilities_add(
        &capabilities->codecs,
        MPF_SAMPLE_RATE_8000 | MPF_SAMPLE_RATE_16000,
        "LPCM");
    termination = mrcp_engine_audio_termination_create(
        channel,
        &audio_stream_vtable,
        capabilities,
        pool);
    channel->channel = mrcp_engine_channel_create(
        engine,
        &channel_vtable,
        channel,
        termination,
        pool);
    return channel->channel;
}

static apt_bool_t funasr_channel_destroy(mrcp_engine_channel_t *channel)
{
    funasr_channel_t *recog_channel = channel->method_obj;
    if (recog_channel->registry_entry &&
        recog_channel->registry_entry->channel) {
        LOG_WITH_SID(
            recog_channel,
            APT_PRIO_ERROR,
            "channel destroy rejected before worker close fence");
        return FALSE;
    }
    return TRUE;
}

static apt_bool_t funasr_channel_open(mrcp_engine_channel_t *channel)
{
    funasr_channel_t *recog_channel = channel->method_obj;
    return funasr_task_signal(
        recog_channel->engine,
        FUNASR_MSG_OPEN_CHANNEL,
        channel,
        NULL,
        NULL);
}

static apt_bool_t funasr_channel_close(mrcp_engine_channel_t *channel)
{
    funasr_channel_t *recog_channel = channel->method_obj;
    return funasr_task_signal(
        recog_channel->engine,
        FUNASR_MSG_CLOSE_CHANNEL,
        channel,
        NULL,
        NULL);
}

static apt_bool_t funasr_channel_request_process(
    mrcp_engine_channel_t *channel,
    mrcp_message_t *request)
{
    funasr_channel_t *recog_channel = channel->method_obj;
    return funasr_task_signal(
        recog_channel->engine,
        FUNASR_MSG_REQUEST_PROCESS,
        channel,
        request,
        NULL);
}

static apt_bool_t funasr_start_of_input(funasr_channel_t *channel)
{
    mrcp_message_t *message;

    if (!channel->recog_request) {
        return FALSE;
    }
    message = mrcp_event_create(
        channel->recog_request,
        RECOGNIZER_START_OF_INPUT,
        channel->recog_request->pool);
    if (!message) {
        return FALSE;
    }
    message->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
    return mrcp_engine_channel_message_send(channel->channel, message);
}

static apt_bool_t funasr_recognition_complete(
    funasr_channel_t *channel,
    mrcp_recog_completion_cause_e cause,
    const char *result)
{
    mrcp_message_t *message;
    mrcp_recog_header_t *recog_header;

    if (!channel->recog_request) {
        return FALSE;
    }
    message = mrcp_event_create(
        channel->recog_request,
        RECOGNIZER_RECOGNITION_COMPLETE,
        channel->recog_request->pool);
    if (!message) {
        return FALSE;
    }
    recog_header = mrcp_resource_header_prepare(message);
    if (recog_header) {
        recog_header->completion_cause = cause;
        mrcp_resource_header_property_add(
            message,
            RECOGNIZER_HEADER_COMPLETION_CAUSE);
    }
    message->start_line.request_state = MRCP_REQUEST_STATE_COMPLETE;
    if (result && cause == RECOGNIZER_COMPLETION_CAUSE_SUCCESS) {
        mrcp_generic_header_t *generic_header;
        char *result_with_suffix;
        char *nlsml;

        result_with_suffix = apr_psprintf(
            message->pool,
            "%s@%s.wav",
            result,
            channel->session_id);
        nlsml = apr_psprintf(
            message->pool,
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<result>\n"
            "  <interpretation grammar=\"session:%s\" confidence=\"1\">\n"
            "    <instance><result>%s</result></instance>\n"
            "    <input mode=\"speech\">%s</input>\n"
            "  </interpretation>\n"
            "</result>",
            channel->session_id,
            result_with_suffix,
            result);
        apt_string_assign(&message->body, nlsml, message->pool);
        generic_header = mrcp_generic_header_prepare(message);
        if (generic_header) {
            apt_string_assign(
                &generic_header->content_type,
                "application/x-nlsml",
                message->pool);
            mrcp_generic_header_property_add(
                message,
                GENERIC_HEADER_CONTENT_TYPE);
        }
    }
    channel->recog_request = NULL;
    LOG_WITH_SID(
        channel,
        APT_PRIO_INFO,
        "recognition terminal cause=%d generation=%lu",
        (int)cause,
        (unsigned long)channel->control.generation);
    return mrcp_engine_channel_message_send(channel->channel, message);
}

static apt_bool_t funasr_control_send_start(
    void *obj,
    funasr_generation_t generation)
{
    funasr_channel_t *channel = obj;
    (void)generation;
    return funasr_start_of_input(channel);
}

static apt_bool_t funasr_control_send_complete(
    void *obj,
    funasr_generation_t generation,
    mrcp_recog_completion_cause_e cause,
    const char *text)
{
    funasr_channel_t *channel = obj;
    (void)generation;
    return funasr_recognition_complete(channel, cause, text);
}

static apt_bool_t funasr_control_send_stop(
    void *obj,
    funasr_generation_t generation)
{
    funasr_channel_t *channel = obj;
    mrcp_message_t *response = channel->stop_response;
    (void)generation;
    if (!response) {
        return FALSE;
    }
    if (!mrcp_engine_channel_message_send(channel->channel, response)) {
        return FALSE;
    }
    channel->stop_response = NULL;
    channel->recog_request = NULL;
    return TRUE;
}

static apt_bool_t funasr_control_send_close(void *obj)
{
    funasr_channel_t *channel = obj;
    funasr_engine_t *engine = channel->engine;

    if (channel->close_response_pending) {
        if (!mrcp_engine_channel_close_respond(channel->channel)) {
            return FALSE;
        }
        channel->close_response_pending = FALSE;
    }
    if (channel->registry_entry) {
        funasr_registry_remove(engine, channel->registry_entry);
    }
    channel->transport = NULL;
    funasr_engine_maybe_close_respond(engine);
    return TRUE;
}

static apt_bool_t funasr_control_cancel(
    void *obj,
    funasr_generation_t generation)
{
    funasr_channel_t *channel = obj;
    return funasr_transport_cancel_generation(
        channel->transport,
        generation);
}

static apt_bool_t funasr_control_close(void *obj)
{
    funasr_channel_t *channel = obj;
    return funasr_transport_request_close(channel->transport);
}

static apr_status_t funasr_control_join(void *obj)
{
    funasr_channel_t *channel = obj;
    return funasr_transport_join_closed(channel->transport);
}

static const funasr_control_vtable_t control_vtable = {
    funasr_control_send_start,
    funasr_control_send_complete,
    funasr_control_send_stop,
    funasr_control_send_close,
    funasr_control_cancel,
    funasr_control_close,
    funasr_control_join
};

static apt_bool_t funasr_channel_recognize(
    mrcp_engine_channel_t *base,
    mrcp_message_t *request,
    mrcp_message_t *response)
{
    funasr_channel_t *channel;
    const mpf_codec_descriptor_t *descriptor;
    funasr_audio_format_t format;
    apr_size_t scratch_capacity;
    funasr_generation_t generation;

    channel = base->method_obj;
    descriptor = mrcp_engine_sink_stream_codec_get(base);
    if (channel->engine->quiescing || !channel->transport || !descriptor ||
        (descriptor->sampling_rate != 8000 &&
         descriptor->sampling_rate != FUNASR_OUTPUT_SAMPLE_RATE) ||
        descriptor->channel_count == 0 ||
        descriptor->channel_count > FUNASR_MAX_CHANNELS) {
        response->start_line.status_code = MRCP_STATUS_CODE_METHOD_FAILED;
        return FALSE;
    }
    if (request->channel_id.session_id.buf &&
        request->channel_id.session_id.length != 0) {
        channel->session_id = apr_pstrndup(
            channel->pool,
            request->channel_id.session_id.buf,
            request->channel_id.session_id.length);
    }
    scratch_capacity = funasr_pcm_bytes_for_ms(
        FUNASR_OUTPUT_SAMPLE_RATE,
        descriptor->channel_count,
        FUNASR_SAMPLE_WIDTH_BYTES,
        FUNASR_RESAMPLE_WINDOW_MS);
    if (scratch_capacity > channel->scratch_capacity) {
        channel->scratch = apr_palloc(channel->pool, scratch_capacity);
        channel->scratch_capacity = scratch_capacity;
    }
    memset(&channel->resample, 0, sizeof(channel->resample));
    generation = channel->next_generation++;
    if (generation == 0) {
        generation = channel->next_generation++;
    }
    if (!funasr_control_begin_generation(&channel->control, generation)) {
        response->start_line.status_code = MRCP_STATUS_CODE_METHOD_FAILED;
        return FALSE;
    }
    memset(&format, 0, sizeof(format));
    format.input_sample_rate = descriptor->sampling_rate;
    format.output_sample_rate = FUNASR_OUTPUT_SAMPLE_RATE;
    format.channel_count = descriptor->channel_count;
    format.sample_width = FUNASR_SAMPLE_WIDTH_BYTES;
    format.call_id = channel->session_id;
    if (!funasr_transport_begin_generation(
            channel->transport,
            generation,
            &format)) {
        funasr_control_init(&channel->control);
        response->start_line.status_code = MRCP_STATUS_CODE_METHOD_FAILED;
        return FALSE;
    }
    channel->recog_request = request;
    response->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
    LOG_WITH_SID(
        channel,
        APT_PRIO_INFO,
        "recognition generation=%lu input_rate=%u channels=%u ring_bytes=%lu chunk_bytes=%lu",
        (unsigned long)generation,
        (unsigned int)descriptor->sampling_rate,
        (unsigned int)descriptor->channel_count,
        (unsigned long)funasr_pcm_bytes_for_ms(
            FUNASR_OUTPUT_SAMPLE_RATE,
            descriptor->channel_count,
            FUNASR_SAMPLE_WIDTH_BYTES,
            FUNASR_TX_RING_DURATION_MS),
        (unsigned long)funasr_pcm_bytes_for_ms(
            FUNASR_OUTPUT_SAMPLE_RATE,
            descriptor->channel_count,
            FUNASR_SAMPLE_WIDTH_BYTES,
            FUNASR_WS_CHUNK_DURATION_MS));
    return mrcp_engine_channel_message_send(base, response);
}

static apt_bool_t funasr_channel_stop(
    mrcp_engine_channel_t *base,
    mrcp_message_t *request,
    mrcp_message_t *response)
{
    funasr_channel_t *channel = base->method_obj;
    (void)request;
    channel->stop_response = response;
    if (!funasr_control_request_stop(
            &channel->control,
            &control_vtable,
            channel)) {
        channel->stop_response = NULL;
        response->start_line.status_code = MRCP_STATUS_CODE_METHOD_FAILED;
        return FALSE;
    }
    return TRUE;
}

static apt_bool_t funasr_channel_request_dispatch(
    mrcp_engine_channel_t *channel,
    mrcp_message_t *request)
{
    mrcp_message_t *response;
    apt_bool_t processed;

    response = mrcp_response_create(request, request->pool);
    processed = FALSE;
    switch (request->start_line.method_id) {
        case RECOGNIZER_RECOGNIZE:
            processed = funasr_channel_recognize(
                channel,
                request,
                response);
            break;
        case RECOGNIZER_STOP:
            processed = funasr_channel_stop(channel, request, response);
            break;
        case RECOGNIZER_START_INPUT_TIMERS:
            processed = mrcp_engine_channel_message_send(channel, response);
            break;
        default:
            break;
    }
    if (!processed) {
        return mrcp_engine_channel_message_send(channel, response);
    }
    return TRUE;
}

static apt_bool_t funasr_stream_destroy(mpf_audio_stream_t *stream)
{
    (void)stream;
    return TRUE;
}

static apt_bool_t funasr_stream_open(
    mpf_audio_stream_t *stream,
    mpf_codec_t *codec)
{
    (void)stream;
    (void)codec;
    return TRUE;
}

static apt_bool_t funasr_stream_close(mpf_audio_stream_t *stream)
{
    (void)stream;
    return TRUE;
}

static apt_bool_t funasr_stream_write(
    mpf_audio_stream_t *stream,
    const mpf_frame_t *frame)
{
    funasr_channel_t *channel;
    funasr_media_snapshot_t media;
    const void *data;
    apr_size_t size;
    apr_int64_t now_us;

    if (!stream || !frame ||
        !(frame->type & MEDIA_FRAME_TYPE_AUDIO) ||
        frame->codec_frame.size == 0) {
        return TRUE;
    }
    channel = stream->obj;
    if (!channel || !channel->transport ||
        !funasr_transport_media_snapshot(channel->transport, &media)) {
        return TRUE;
    }
    data = frame->codec_frame.buffer;
    size = frame->codec_frame.size;
    LOG_WITH_SID(channel, APT_PRIO_DEBUG,
        "received client media audio frame, size=%" APR_SIZE_T_FMT " bytes",
        frame->codec_frame.size);
    if (media.input_sample_rate == 8000) {
        if (!funasr_resample_8k_to_16k_into(
                &channel->resample,
                data,
                size,
                media.channel_count,
                channel->scratch,
                channel->scratch_capacity,
                &size)) {
            funasr_transport_latch_media_failure(
                channel->transport,
                media.generation,
                FUNASR_FAILURE_INTERNAL);
            return TRUE;
        }
        data = channel->scratch;
    }
    now_us = funasr_clock_now_us(&channel->clock);
    funasr_transport_enqueue_pcm(
        channel->transport,
        media.generation,
        data,
        size,
        now_us);
    return TRUE;
}

static apt_bool_t funasr_open_channel_on_task(funasr_channel_t *channel)
{
    funasr_transport_config_t config;
    funasr_registry_entry_t *entry;
    funasr_transport_id_t id;
    funasr_event_bridge_t *bridge;
    apt_task_t *task;

    if (channel->engine->quiescing) {
        return mrcp_engine_channel_open_respond(channel->channel, FALSE);
    }
    id = channel->engine->next_transport_id++;
    if (id == 0) {
        id = channel->engine->next_transport_id++;
    }
    funasr_transport_config_init(&config);
    task = apt_consumer_task_base_get(channel->engine->task);
    bridge = apr_pcalloc(channel->engine->pool, sizeof(*bridge));
    bridge->engine = channel->engine;
    bridge->close_fence_msg = apt_task_msg_get(task);
    if (!bridge->close_fence_msg) {
        return mrcp_engine_channel_open_respond(channel->channel, FALSE);
    }
    config.host = channel->engine->server_host;
    config.port = channel->engine->server_port;
    config.path = channel->engine->server_path;
    config.first_audio_result_timeout_us =
        channel->engine->first_audio_result_timeout_us;
    config.last_speech_result_timeout_us =
        channel->engine->last_speech_result_timeout_us;
    config.clock = channel->clock;
    config.event_sink = funasr_transport_event_sink;
    config.event_sink_obj = bridge;
    channel->transport = funasr_transport_create(
        channel->engine->pool,
        id,
        &config);
    if (!channel->transport) {
        return mrcp_engine_channel_open_respond(channel->channel, FALSE);
    }
    entry = apr_pcalloc(channel->engine->pool, sizeof(*entry));
    entry->id = id;
    entry->transport = channel->transport;
    entry->channel = channel;
    entry->next = channel->engine->registry;
    channel->engine->registry = entry;
    channel->registry_entry = entry;
    return mrcp_engine_channel_open_respond(channel->channel, TRUE);
}

static void funasr_close_channel_on_task(funasr_channel_t *channel)
{
    if (!channel->transport) {
        mrcp_engine_channel_close_respond(channel->channel);
        return;
    }
    channel->close_response_pending = TRUE;
    if (!funasr_control_request_close(
            &channel->control,
            &control_vtable,
            channel)) {
        LOG_WITH_SID(channel, APT_PRIO_ERROR, "failed to request worker close");
    }
}

static void funasr_engine_close_on_task(funasr_engine_t *engine)
{
    funasr_registry_entry_t *entry;

    engine->quiescing = TRUE;
    for (entry = engine->registry; entry; entry = entry->next) {
        if (entry->channel && !entry->channel->control.close_pending &&
            !entry->channel->control.worker_closed) {
            funasr_control_request_close(
                &entry->channel->control,
                &control_vtable,
                entry->channel);
        }
    }
    funasr_engine_maybe_close_respond(engine);
}

static void funasr_gap_histogram_format(
    const funasr_transport_metrics_t *metrics,
    char *buffer,
    apr_size_t capacity)
{
    apr_size_t offset;
    apr_size_t bucket;

    if (!buffer || capacity == 0) {
        return;
    }
    offset = 0;
    buffer[0] = '\0';
    for (bucket = 0;
         bucket < FUNASR_MEDIA_GAP_HISTOGRAM_BUCKETS;
         ++bucket) {
        int written;

        if (metrics->media_gap_histogram[bucket] == 0) {
            continue;
        }
        written = apr_snprintf(
            buffer + offset,
            capacity - offset,
            "%s%lu:%lu",
            offset == 0 ? "" : ",",
            (unsigned long)bucket,
            (unsigned long)metrics->media_gap_histogram[bucket]);
        if (written < 0 || (apr_size_t)written >= capacity - offset) {
            buffer[capacity - 1U] = '\0';
            return;
        }
        offset += (apr_size_t)written;
    }
    if (offset == 0 && capacity > 1U) {
        buffer[0] = '-';
        buffer[1] = '\0';
    }
}

static void funasr_transport_event_on_task(
    funasr_engine_t *engine,
    funasr_transport_event_t *event)
{
    funasr_registry_entry_t *entry;
    char gap_histogram[4096];
    apt_bool_t handled;

    entry = funasr_registry_find(engine, event->transport_id);
    if (!entry || !entry->channel) {
        funasr_transport_event_destroy(event);
        return;
    }
    if (event->type == FUNASR_EVENT_TRANSPORT_METRICS) {
        funasr_gap_histogram_format(
            &event->metrics,
            gap_histogram,
            sizeof(gap_histogram));
        LOG_WITH_SID(
            entry->channel,
            APT_PRIO_INFO,
            "audio transport generation=%lu audio_rx_frames=%lu audio_rx_bytes=%lu audio_rx_gap_samples=%lu audio_rx_gap_hist_ms=%s audio_rx_gap_p99_ms=%ld audio_rx_gap_max_ms=%ld audio_tx_frames=%lu audio_tx_bytes=%lu audio_tx_last_us=%" APR_INT64_T_FMT " audio_tx_gap_last_ms=%ld audio_tx_gap_max_ms=%ld ring_high_water=%lu overrun_bytes=%lu overrun_events=%lu first_send_ms=%ld write_wait_max_ms=%ld abnormal_closes=%lu partial_reads=%lu rx_messages=%lu completion_failure=%d",
            (unsigned long)event->generation,
            (unsigned long)event->metrics.media_frames,
            (unsigned long)event->metrics.valid_audio_bytes,
            (unsigned long)event->metrics.media_gap_samples,
            gap_histogram,
            (long)(event->metrics.media_gap_p99_us / 1000),
            (long)(event->metrics.media_gap_max_us / 1000),
            (unsigned long)event->metrics.ws_audio_frames,
            (unsigned long)event->metrics.ws_audio_bytes,
            event->metrics.ws_audio_last_send_us,
            (long)(event->metrics.ws_audio_gap_last_us / 1000),
            (long)(event->metrics.ws_audio_gap_max_us / 1000),
            (unsigned long)event->metrics.tx_ring_high_water_bytes,
            (unsigned long)event->metrics.tx_ring_overrun_bytes,
            (unsigned long)event->metrics.tx_ring_overrun_events,
            (long)event->metrics.ws_first_send_ms,
            (long)event->metrics.ws_write_wait_max_ms,
            (unsigned long)event->metrics.abnormal_closes,
            (unsigned long)event->metrics.ws_rx_partial_reads,
            (unsigned long)event->metrics.ws_rx_messages,
            (int)event->metrics.completion_failure);
    }
    if (event->type == FUNASR_EVENT_TRANSPORT_FAILED &&
        entry->channel->control.active &&
        entry->channel->control.generation == event->generation) {
        if (event->failure == FUNASR_FAILURE_FIRST_AUDIO_RESULT_TIMEOUT) {
            LOG_WITH_SID(
                entry->channel,
                APT_PRIO_WARNING,
                "asr first audio result timeout generation=%lu timeout_ms=%ld",
                (unsigned long)event->generation,
                (long)(entry->channel->engine->
                    first_audio_result_timeout_us / 1000));
        } else if (event->failure == FUNASR_FAILURE_NO_RESULT_TIMEOUT) {
            LOG_WITH_SID(
                entry->channel,
                APT_PRIO_WARNING,
                "asr not response resut generation=%lu timeout_ms=%ld",
                (unsigned long)event->generation,
                (long)(entry->channel->engine->
                    last_speech_result_timeout_us / 1000));
        }
    }
    handled = FALSE;
    handled = funasr_control_handle_event(
        &entry->channel->control,
        event,
        &control_vtable,
        entry->channel);
    if (!handled && event->type == FUNASR_EVENT_WORKER_CLOSED) {
        if (funasr_close_fence_retry_attempt(
                engine->task, engine->pool, event,
                &funasr_close_fence_retry_vtable, engine)) return;
        return;
    } else if (!handled) {
        LOG_WITH_SID(
            entry->channel,
            APT_PRIO_WARNING,
            "transport event handling failed generation=%lu type=%d",
            (unsigned long)event->generation,
            (int)event->type);
    }
    funasr_transport_event_destroy(event);
}

static apt_bool_t funasr_msg_process(apt_task_t *task, apt_task_msg_t *msg)
{
    funasr_engine_t *engine;
    apt_consumer_task_t *consumer_task;
    funasr_msg_t *payload;
    funasr_channel_t *channel;

    consumer_task = apt_task_object_get(task);
    engine = apt_consumer_task_object_get(consumer_task);
    payload = (funasr_msg_t *)msg->data;
    switch (payload->type) {
        case FUNASR_MSG_OPEN_CHANNEL:
            channel = payload->channel->method_obj;
            funasr_open_channel_on_task(channel);
            break;
        case FUNASR_MSG_CLOSE_CHANNEL:
            channel = payload->channel->method_obj;
            funasr_close_channel_on_task(channel);
            break;
        case FUNASR_MSG_REQUEST_PROCESS:
            funasr_channel_request_dispatch(
                payload->channel,
                payload->request);
            break;
        case FUNASR_MSG_ENGINE_CLOSE:
            funasr_engine_close_on_task(engine);
            break;
        case FUNASR_MSG_TRANSPORT_EVENT:
            funasr_transport_event_on_task(engine, payload->event);
            payload->event = NULL;
            break;
    }
    return TRUE;
}

MRCP_PLUGIN_VERSION_DECLARE
MRCP_PLUGIN_LOG_SOURCE_IMPLEMENT(RECOG_PLUGIN, "FUNASR-PLUGIN")
#define RECOG_LOG_MARK APT_LOG_MARK_DECLARE(RECOG_PLUGIN)
