#include "funasr_ws_transport.h"
#include "funasr_audio.h"
#include "funasr_json.h"
#include "apt_log.h"

#include <apr_base64.h>
#include <apr_general.h>
#include <apr_poll.h>
#include <apr_sha1.h>
#include <apr_strings.h>
#include <apr_thread_cond.h>
#include <apr_thread_proc.h>
#include <apr_thread_mutex.h>
#include <stdlib.h>
#include <string.h>

#define FUNASR_WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define FUNASR_CALL_ID_LIMIT 255U
#define FUNASR_HANDSHAKE_BUFFER_SIZE 4096U
#define FUNASR_WORKER_READ_BUFFER_SIZE 4096U
#define FUNASR_WS_FRAME_OVERHEAD 14U
#define FUNASR_METRICS_LOG_INTERVAL_US APR_USEC_PER_SEC

typedef struct funasr_default_io_t {
    apr_pool_t *pool;
    const char *session_id;
    apr_thread_mutex_t *mutex;
    apr_socket_t *socket;
    apr_pollset_t *pollset;
    apr_pollfd_t pollfd;
    apr_int16_t registered_events;
    apt_bool_t wakeable;
} funasr_default_io_t;

struct funasr_transport_t {
    apr_pool_t *pool;
    funasr_transport_id_t id;
    funasr_transport_config_t config;
    char *host;
    char *path;
    const funasr_transport_io_vtable_t *io_vtable;
    void *io_obj;
    funasr_default_io_t default_io;
    apr_thread_mutex_t *mutex;
    apr_thread_cond_t *condition;
    apr_thread_t *thread;
    funasr_tx_ring_t *ring;
    funasr_transport_event_t *close_event;
    funasr_generation_t generation;
    funasr_audio_format_t format;
    char call_id[FUNASR_CALL_ID_LIMIT + 1U];
    funasr_transport_metrics_t metrics;
    apr_int64_t generation_started_us;
    apr_int64_t last_media_us;
    apr_int64_t last_metrics_report_us;
    apr_size_t ring_limit;
    apr_size_t chunk_size;
    funasr_transport_failure_e media_failure;
    apt_bool_t media_failure_latched;
    apt_bool_t active;
    apt_bool_t finishing;
    apt_bool_t cancel_requested;
    apt_bool_t close_requested;
    apt_bool_t thread_started;
    apt_bool_t worker_closed;
    apt_bool_t close_fence_failed;
    apt_bool_t joined;
};

static apr_status_t funasr_transport_pool_cleanup(void *obj)
{
    funasr_transport_t *transport = obj;
    free(transport->close_event);
    transport->close_event = NULL;
    return APR_SUCCESS;
}

struct funasr_tx_ring_t {
    unsigned char *data;
    apr_size_t capacity;
    apr_size_t limit;
    apr_size_t read_offset;
    apr_size_t write_offset;
    apr_size_t size;
    apr_size_t overrun_bytes;
    apr_size_t overrun_events;
    funasr_generation_t generation;
    apt_bool_t active;
    apt_bool_t closed;
    apr_thread_mutex_t *mutex;
};

static apt_bool_t funasr_ascii_equal_ci(
    unsigned char first,
    unsigned char second)
{
    if (first >= 'A' && first <= 'Z') {
        first = (unsigned char)(first - 'A' + 'a');
    }
    if (second >= 'A' && second <= 'Z') {
        second = (unsigned char)(second - 'A' + 'a');
    }
    return first == second;
}

static apt_bool_t funasr_span_equal_ci(
    const unsigned char *data,
    apr_size_t size,
    const char *expected)
{
    apr_size_t expected_size;
    apr_size_t index;

    expected_size = strlen(expected);
    if (size != expected_size) {
        return FALSE;
    }
    for (index = 0; index < size; ++index) {
        if (!funasr_ascii_equal_ci(
                data[index],
                (unsigned char)expected[index])) {
            return FALSE;
        }
    }
    return TRUE;
}

static apt_bool_t funasr_header_has_token(
    const unsigned char *value,
    apr_size_t value_size,
    const char *token)
{
    apr_size_t start;

    start = 0;
    while (start < value_size) {
        apr_size_t end;
        apr_size_t trimmed_start;
        apr_size_t trimmed_end;

        end = start;
        while (end < value_size && value[end] != ',') {
            ++end;
        }
        trimmed_start = start;
        while (trimmed_start < end &&
               (value[trimmed_start] == ' ' ||
                value[trimmed_start] == '\t')) {
            ++trimmed_start;
        }
        trimmed_end = end;
        while (trimmed_end > trimmed_start &&
               (value[trimmed_end - 1] == ' ' ||
                value[trimmed_end - 1] == '\t')) {
            --trimmed_end;
        }
        if (funasr_span_equal_ci(
                value + trimmed_start,
                trimmed_end - trimmed_start,
                token)) {
            return TRUE;
        }
        start = end + 1;
    }
    return FALSE;
}

static apt_bool_t funasr_http_header_valid(
    const unsigned char *header,
    apr_size_t size)
{
    static const char status_10[] = "HTTP/1.0 101";
    static const char status_11[] = "HTTP/1.1 101";
    apt_bool_t status_valid;
    apt_bool_t has_upgrade;
    apt_bool_t has_connection_upgrade;
    apr_size_t line_start;

    status_valid =
        (size >= sizeof(status_10) - 1 &&
         memcmp(header, status_10, sizeof(status_10) - 1) == 0) ||
        (size >= sizeof(status_11) - 1 &&
         memcmp(header, status_11, sizeof(status_11) - 1) == 0);
    if (!status_valid) {
        return FALSE;
    }
    if (size <= sizeof(status_10) - 1 ||
        (header[sizeof(status_10) - 1] != ' ' &&
         header[sizeof(status_10) - 1] != '\r')) {
        return FALSE;
    }

    line_start = 0;
    while (line_start + 1 < size &&
           !(header[line_start] == '\r' &&
             header[line_start + 1] == '\n')) {
        ++line_start;
    }
    if (line_start + 1 >= size) {
        return FALSE;
    }
    line_start += 2;
    has_upgrade = FALSE;
    has_connection_upgrade = FALSE;
    while (line_start + 1 < size) {
        apr_size_t line_end;
        apr_size_t colon;
        apr_size_t value_start;

        if (header[line_start] == '\r' &&
            header[line_start + 1] == '\n') {
            break;
        }
        line_end = line_start;
        while (line_end + 1 < size &&
               !(header[line_end] == '\r' &&
                 header[line_end + 1] == '\n')) {
            ++line_end;
        }
        if (line_end + 1 >= size) {
            return FALSE;
        }
        colon = line_start;
        while (colon < line_end && header[colon] != ':') {
            ++colon;
        }
        if (colon == line_end) {
            return FALSE;
        }
        value_start = colon + 1;
        if (funasr_span_equal_ci(
                header + line_start,
                colon - line_start,
                "upgrade") &&
            funasr_header_has_token(
                header + value_start,
                line_end - value_start,
                "websocket")) {
            has_upgrade = TRUE;
        }
        if (funasr_span_equal_ci(
                header + line_start,
                colon - line_start,
                "connection") &&
            funasr_header_has_token(
                header + value_start,
                line_end - value_start,
                "upgrade")) {
            has_connection_upgrade = TRUE;
        }
        line_start = line_end + 2;
    }

    return has_upgrade && has_connection_upgrade;
}

void funasr_http_decoder_init(
    funasr_http_decoder_t *decoder,
    void *storage,
    apr_size_t capacity)
{
    if (!decoder) {
        return;
    }

    memset(decoder, 0, sizeof(*decoder));
    decoder->storage = (unsigned char *)storage;
    decoder->capacity = capacity;
    if (!storage || capacity == 0) {
        decoder->failed = TRUE;
    }
}

funasr_http_status_e funasr_http_decoder_feed(
    funasr_http_decoder_t *decoder,
    const void *data,
    apr_size_t size,
    apr_size_t *consumed)
{
    const unsigned char *bytes;

    if (consumed) {
        *consumed = 0;
    }
    if (!decoder || !consumed || (!data && size != 0) || decoder->failed) {
        return FUNASR_HTTP_PROTOCOL_ERROR;
    }
    if (decoder->complete) {
        return FUNASR_HTTP_COMPLETE;
    }

    bytes = (const unsigned char *)data;
    while (*consumed < size) {
        if (decoder->size >= decoder->capacity) {
            decoder->failed = TRUE;
            return FUNASR_HTTP_LIMIT_EXCEEDED;
        }

        decoder->storage[decoder->size++] = bytes[*consumed];
        ++(*consumed);

        if (decoder->size >= 4 &&
            memcmp(
                decoder->storage + decoder->size - 4,
                "\r\n\r\n",
                4) == 0) {
            if (!funasr_http_header_valid(
                    decoder->storage,
                    decoder->size)) {
                decoder->failed = TRUE;
                return FUNASR_HTTP_PROTOCOL_ERROR;
            }
            decoder->complete = TRUE;
            return FUNASR_HTTP_COMPLETE;
        }
    }

    return FUNASR_HTTP_NEED_MORE;
}

void funasr_ws_decoder_init(
    funasr_ws_decoder_t *decoder,
    void *frame_storage,
    apr_size_t frame_storage_capacity,
    void *message_storage,
    apr_size_t message_storage_capacity,
    apr_size_t frame_limit,
    apr_size_t message_limit)
{
    if (!decoder) {
        return;
    }

    memset(decoder, 0, sizeof(*decoder));
    decoder->frame_storage = (unsigned char *)frame_storage;
    decoder->frame_storage_capacity = frame_storage_capacity;
    decoder->message_storage = (unsigned char *)message_storage;
    decoder->message_storage_capacity = message_storage_capacity;
    decoder->frame_limit = frame_limit;
    decoder->message_limit = message_limit;
    if (!frame_storage || !message_storage ||
        frame_storage_capacity == 0 || message_storage_capacity == 0 ||
        frame_limit == 0 || message_limit == 0) {
        decoder->failed = TRUE;
    }
}

void funasr_ws_decoder_reset(funasr_ws_decoder_t *decoder)
{
    if (!decoder) {
        return;
    }

    decoder->frame_storage_size = 0;
    decoder->message_size = 0;
    decoder->fragment_opcode = FUNASR_WS_OPCODE_CONTINUATION;
    decoder->fragment_active = FALSE;
    decoder->failed = FALSE;
}

void funasr_ws_decoder_reject_masked(
    funasr_ws_decoder_t *decoder,
    apt_bool_t reject_masked)
{
    if (decoder) {
        decoder->reject_masked = reject_masked;
    }
}

static apr_uint64_t funasr_ws_payload_length(
    const unsigned char *data,
    apr_size_t bytes)
{
    apr_uint64_t length;
    apr_size_t index;

    length = 0;
    for (index = 0; index < bytes; ++index) {
        length = (length << 8) | data[index];
    }
    return length;
}

static void funasr_ws_consume_frame(
    funasr_ws_decoder_t *decoder,
    apr_size_t frame_size)
{
    apr_size_t remaining;

    remaining = decoder->frame_storage_size - frame_size;
    if (remaining != 0) {
        memmove(
            decoder->frame_storage,
            decoder->frame_storage + frame_size,
            remaining);
    }
    decoder->frame_storage_size = remaining;
}

static void funasr_ws_copy_payload(
    const unsigned char *payload,
    apr_size_t payload_size,
    apt_bool_t masked,
    const unsigned char *mask,
    unsigned char *output)
{
    apr_size_t index;

    for (index = 0; index < payload_size; ++index) {
        output[index] = masked ?
            (unsigned char)(payload[index] ^ mask[index % 4]) :
            payload[index];
    }
}

static funasr_ws_status_e funasr_ws_parse_buffered(
    funasr_ws_decoder_t *decoder,
    funasr_ws_event_t *event)
{
    while (decoder->frame_storage_size >= 2) {
        const unsigned char *frame;
        const unsigned char *mask;
        const unsigned char *payload;
        apr_uint64_t payload_length_64;
        apr_size_t payload_length;
        apr_size_t header_size;
        apr_size_t frame_size;
        apr_size_t length_bytes;
        funasr_ws_opcode_e opcode;
        apt_bool_t final_frame;
        apt_bool_t masked;
        apt_bool_t control;

        frame = decoder->frame_storage;
        final_frame = (frame[0] & 0x80U) != 0;
        if ((frame[0] & 0x70U) != 0) {
            decoder->failed = TRUE;
            return FUNASR_WS_PROTOCOL_ERROR;
        }

        opcode = (funasr_ws_opcode_e)(frame[0] & 0x0FU);
        masked = (frame[1] & 0x80U) != 0;
        if (masked && decoder->reject_masked) {
            decoder->failed = TRUE;
            return FUNASR_WS_PROTOCOL_ERROR;
        }
        payload_length_64 = (apr_uint64_t)(frame[1] & 0x7FU);
        header_size = 2;
        length_bytes = 0;
        if (payload_length_64 == 126U) {
            length_bytes = 2;
        } else if (payload_length_64 == 127U) {
            length_bytes = 8;
        }
        if (decoder->frame_storage_size < header_size + length_bytes) {
            return FUNASR_WS_NEED_MORE;
        }
        if (length_bytes != 0) {
            payload_length_64 = funasr_ws_payload_length(
                frame + header_size,
                length_bytes);
            if (length_bytes == 8 && (frame[2] & 0x80U) != 0) {
                decoder->failed = TRUE;
                return FUNASR_WS_PROTOCOL_ERROR;
            }
            header_size += length_bytes;
        }

        control = opcode >= FUNASR_WS_OPCODE_CLOSE;
        if (control && (!final_frame || payload_length_64 > 125U)) {
            decoder->failed = TRUE;
            return FUNASR_WS_PROTOCOL_ERROR;
        }
        if (payload_length_64 > decoder->frame_limit ||
            payload_length_64 > (apr_uint64_t)((apr_size_t)-1)) {
            decoder->failed = TRUE;
            return FUNASR_WS_LIMIT_EXCEEDED;
        }
        payload_length = (apr_size_t)payload_length_64;

        if (masked) {
            header_size += 4;
        }
        if (header_size > decoder->frame_storage_capacity ||
            payload_length > decoder->frame_storage_capacity - header_size) {
            decoder->failed = TRUE;
            return FUNASR_WS_LIMIT_EXCEEDED;
        }
        frame_size = header_size + payload_length;
        if (decoder->frame_storage_size < frame_size) {
            return FUNASR_WS_NEED_MORE;
        }

        mask = masked ? frame + header_size - 4 : NULL;
        payload = frame + header_size;

        if (opcode == FUNASR_WS_OPCODE_TEXT ||
            opcode == FUNASR_WS_OPCODE_BINARY) {
            if (decoder->fragment_active) {
                decoder->failed = TRUE;
                return FUNASR_WS_PROTOCOL_ERROR;
            }
            if (payload_length > decoder->message_limit ||
                payload_length > decoder->message_storage_capacity) {
                decoder->failed = TRUE;
                return FUNASR_WS_LIMIT_EXCEEDED;
            }
            funasr_ws_copy_payload(
                payload,
                payload_length,
                masked,
                mask,
                decoder->message_storage);
            decoder->message_size = payload_length;
            if (final_frame) {
                event->type = FUNASR_WS_EVENT_MESSAGE;
                event->opcode = opcode;
                event->data = decoder->message_storage;
                event->size = decoder->message_size;
                decoder->message_size = 0;
                funasr_ws_consume_frame(decoder, frame_size);
                return FUNASR_WS_EVENT_READY;
            }
            decoder->fragment_active = TRUE;
            decoder->fragment_opcode = opcode;
            funasr_ws_consume_frame(decoder, frame_size);
            continue;
        }

        if (opcode == FUNASR_WS_OPCODE_CONTINUATION) {
            if (!decoder->fragment_active) {
                decoder->failed = TRUE;
                return FUNASR_WS_PROTOCOL_ERROR;
            }
            if (payload_length > decoder->message_limit -
                    decoder->message_size ||
                payload_length > decoder->message_storage_capacity -
                    decoder->message_size) {
                decoder->failed = TRUE;
                return FUNASR_WS_LIMIT_EXCEEDED;
            }
            funasr_ws_copy_payload(
                payload,
                payload_length,
                masked,
                mask,
                decoder->message_storage + decoder->message_size);
            decoder->message_size += payload_length;
            funasr_ws_consume_frame(decoder, frame_size);
            if (final_frame) {
                event->type = FUNASR_WS_EVENT_MESSAGE;
                event->opcode = decoder->fragment_opcode;
                event->data = decoder->message_storage;
                event->size = decoder->message_size;
                decoder->message_size = 0;
                decoder->fragment_active = FALSE;
                decoder->fragment_opcode = FUNASR_WS_OPCODE_CONTINUATION;
                return FUNASR_WS_EVENT_READY;
            }
            continue;
        }

        if (opcode != FUNASR_WS_OPCODE_PING &&
            opcode != FUNASR_WS_OPCODE_PONG &&
            opcode != FUNASR_WS_OPCODE_CLOSE) {
            decoder->failed = TRUE;
            return FUNASR_WS_PROTOCOL_ERROR;
        }

        funasr_ws_copy_payload(
            payload,
            payload_length,
            masked,
            mask,
            decoder->control_storage);
        event->type = opcode == FUNASR_WS_OPCODE_PING ?
            FUNASR_WS_EVENT_PING :
            (opcode == FUNASR_WS_OPCODE_PONG ?
                FUNASR_WS_EVENT_PONG :
                FUNASR_WS_EVENT_CLOSE);
        event->opcode = opcode;
        event->data = decoder->control_storage;
        event->size = payload_length;
        funasr_ws_consume_frame(decoder, frame_size);
        return FUNASR_WS_EVENT_READY;
    }

    return FUNASR_WS_NEED_MORE;
}

funasr_ws_status_e funasr_ws_decoder_feed(
    funasr_ws_decoder_t *decoder,
    const void *data,
    apr_size_t size,
    funasr_ws_event_t *event)
{
    if (!decoder || !event || (!data && size != 0) || decoder->failed) {
        return FUNASR_WS_PROTOCOL_ERROR;
    }

    memset(event, 0, sizeof(*event));
    if (size > decoder->frame_storage_capacity -
            decoder->frame_storage_size) {
        decoder->failed = TRUE;
        return FUNASR_WS_LIMIT_EXCEEDED;
    }
    if (size != 0) {
        memcpy(
            decoder->frame_storage + decoder->frame_storage_size,
            data,
            size);
        decoder->frame_storage_size += size;
    }

    return funasr_ws_parse_buffered(decoder, event);
}

funasr_tx_ring_t *funasr_tx_ring_create(
    apr_pool_t *pool,
    apr_size_t capacity)
{
    funasr_tx_ring_t *ring;

    if (!pool || capacity == 0) {
        return NULL;
    }

    ring = apr_pcalloc(pool, sizeof(*ring));
    ring->data = apr_palloc(pool, capacity);
    ring->capacity = capacity;
    ring->limit = capacity;
    if (!ring->data ||
        apr_thread_mutex_create(
            &ring->mutex,
            APR_THREAD_MUTEX_DEFAULT,
            pool) != APR_SUCCESS) {
        return NULL;
    }
    return ring;
}

apt_bool_t funasr_tx_ring_begin(
    funasr_tx_ring_t *ring,
    funasr_generation_t generation)
{
    if (!ring || generation == 0) {
        return FALSE;
    }

    apr_thread_mutex_lock(ring->mutex);
    ring->read_offset = 0;
    ring->write_offset = 0;
    ring->size = 0;
    ring->overrun_bytes = 0;
    ring->overrun_events = 0;
    ring->generation = generation;
    ring->active = TRUE;
    ring->closed = FALSE;
    apr_thread_mutex_unlock(ring->mutex);
    return TRUE;
}

funasr_enqueue_status_e funasr_tx_ring_enqueue(
    funasr_tx_ring_t *ring,
    funasr_generation_t generation,
    const void *data,
    apr_size_t size)
{
    funasr_enqueue_status_e status;
    apr_size_t first_part;

    if (!ring || (!data && size != 0)) {
        return FUNASR_ENQUEUE_NOT_STREAMING;
    }

    apr_thread_mutex_lock(ring->mutex);
    if (ring->closed) {
        status = FUNASR_ENQUEUE_CLOSED;
    } else if (!ring->active) {
        status = FUNASR_ENQUEUE_NOT_STREAMING;
    } else if (ring->generation != generation) {
        status = FUNASR_ENQUEUE_STALE_GENERATION;
    } else if (size > ring->limit - ring->size) {
        ring->overrun_bytes += size;
        ring->overrun_events += 1;
        status = FUNASR_ENQUEUE_QUEUE_FULL;
    } else {
        first_part = size;
        if (first_part > ring->capacity - ring->write_offset) {
            first_part = ring->capacity - ring->write_offset;
        }
        if (first_part != 0) {
            memcpy(ring->data + ring->write_offset, data, first_part);
        }
        if (size > first_part) {
            memcpy(
                ring->data,
                (const unsigned char *)data + first_part,
                size - first_part);
        }
        ring->write_offset =
            (ring->write_offset + size) % ring->capacity;
        ring->size += size;
        status = FUNASR_ENQUEUE_ACCEPTED;
    }
    apr_thread_mutex_unlock(ring->mutex);
    return status;
}

apt_bool_t funasr_tx_ring_dequeue(
    funasr_tx_ring_t *ring,
    void *output,
    apr_size_t *size)
{
    apr_size_t requested;
    apr_size_t first_part;

    if (!ring || !output || !size) {
        return FALSE;
    }

    apr_thread_mutex_lock(ring->mutex);
    requested = *size;
    if (requested > ring->size) {
        requested = ring->size;
    }
    if (requested == 0) {
        *size = 0;
        apr_thread_mutex_unlock(ring->mutex);
        return FALSE;
    }

    first_part = requested;
    if (first_part > ring->capacity - ring->read_offset) {
        first_part = ring->capacity - ring->read_offset;
    }
    memcpy(output, ring->data + ring->read_offset, first_part);
    if (requested > first_part) {
        memcpy(
            (unsigned char *)output + first_part,
            ring->data,
            requested - first_part);
    }
    ring->read_offset = (ring->read_offset + requested) % ring->capacity;
    ring->size -= requested;
    *size = requested;
    apr_thread_mutex_unlock(ring->mutex);
    return TRUE;
}

static apr_size_t funasr_tx_ring_metric(
    funasr_tx_ring_t *ring,
    int metric)
{
    apr_size_t value;

    if (!ring) {
        return 0;
    }

    apr_thread_mutex_lock(ring->mutex);
    if (metric == 0) {
        value = ring->size;
    } else if (metric == 1) {
        value = ring->overrun_bytes;
    } else {
        value = ring->overrun_events;
    }
    apr_thread_mutex_unlock(ring->mutex);
    return value;
}

apr_size_t funasr_tx_ring_size(funasr_tx_ring_t *ring)
{
    return funasr_tx_ring_metric(ring, 0);
}

apr_size_t funasr_tx_ring_overrun_bytes(funasr_tx_ring_t *ring)
{
    return funasr_tx_ring_metric(ring, 1);
}

apr_size_t funasr_tx_ring_overrun_events(funasr_tx_ring_t *ring)
{
    return funasr_tx_ring_metric(ring, 2);
}

void funasr_tx_ring_close(funasr_tx_ring_t *ring)
{
    if (!ring) {
        return;
    }

    apr_thread_mutex_lock(ring->mutex);
    ring->active = FALSE;
    ring->closed = TRUE;
    apr_thread_mutex_unlock(ring->mutex);
}

static void funasr_tx_ring_stop_enqueue(funasr_tx_ring_t *ring)
{
    if (!ring) {
        return;
    }
    apr_thread_mutex_lock(ring->mutex);
    ring->active = FALSE;
    apr_thread_mutex_unlock(ring->mutex);
}

static void funasr_tx_ring_set_limit(
    funasr_tx_ring_t *ring,
    apr_size_t limit)
{
    if (!ring) {
        return;
    }
    apr_thread_mutex_lock(ring->mutex);
    ring->limit = limit <= ring->capacity ? limit : ring->capacity;
    apr_thread_mutex_unlock(ring->mutex);
}

void funasr_transport_event_destroy(funasr_transport_event_t *event)
{
    if (!event) {
        return;
    }
    free(event->text);
    free(event);
}

void funasr_transport_config_init(funasr_transport_config_t *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->connect_timeout_us = FUNASR_CONNECT_TIMEOUT_US;
    config->handshake_timeout_us = FUNASR_HANDSHAKE_TIMEOUT_US;
    config->write_stall_timeout_us = FUNASR_WRITE_STALL_TIMEOUT_US;
    config->stop_drain_timeout_us = FUNASR_STOP_DRAIN_TIMEOUT_US;
    config->no_result_timeout_us = FUNASR_NO_RESULT_TIMEOUT_US;
    config->input_idle_timeout_us = FUNASR_INPUT_IDLE_TIMEOUT_US;
    config->poll_timeout_us = FUNASR_POLL_TIMEOUT_MS * 1000;
    funasr_clock_default(&config->clock);
}

apt_bool_t funasr_ws_accept_compute(
    const char *client_key,
    char *output,
    apr_size_t output_capacity)
{
    apr_sha1_ctx_t sha1;
    unsigned char digest[APR_SHA1_DIGESTSIZE];
    int encoded_size;
    apr_size_t key_size;

    if (!client_key || !output) {
        return FALSE;
    }
    key_size = strlen(client_key);
    if (key_size > 1024U || output_capacity < 29U) {
        return FALSE;
    }
    apr_sha1_init(&sha1);
    apr_sha1_update(&sha1, client_key, (unsigned int)key_size);
    apr_sha1_update(
        &sha1,
        FUNASR_WS_GUID,
        (unsigned int)(sizeof(FUNASR_WS_GUID) - 1U));
    apr_sha1_final(digest, &sha1);
    encoded_size = apr_base64_encode_binary(
        output,
        digest,
        APR_SHA1_DIGESTSIZE);
    return encoded_size > 0 && (apr_size_t)encoded_size <= output_capacity;
}

static apr_status_t funasr_default_io_open(
    void *obj,
    const char *host,
    apr_port_t port,
    apr_interval_time_t timeout)
{
    funasr_default_io_t *io;
    apr_sockaddr_t *address;
    apr_socket_t *socket;
    apr_pollset_t *pollset;
    apr_pollfd_t pollfd;
    apr_status_t status;
    apr_uint32_t poll_flags;
    apt_bool_t wakeable;

    io = (funasr_default_io_t *)obj;
    socket = NULL;
    pollset = NULL;
    wakeable = FALSE;
    apr_thread_mutex_lock(io->mutex);
    io->socket = NULL;
    io->pollset = NULL;
    io->registered_events = 0;
    io->wakeable = FALSE;
    apr_thread_mutex_unlock(io->mutex);
    status = apr_sockaddr_info_get(
        &address,
        host,
        APR_UNSPEC,
        port,
        0,
        io->pool);
    if (status != APR_SUCCESS) {
        return status;
    }
    status = apr_socket_create(
        &socket,
        address->family,
        SOCK_STREAM,
        APR_PROTO_TCP,
        io->pool);
    if (status != APR_SUCCESS) {
        return status;
    }
    apr_socket_timeout_set(socket, timeout);
    status = apr_socket_connect(socket, address);
    if (status != APR_SUCCESS) {
        apr_socket_close(socket);
        return status;
    }
    apr_socket_timeout_set(socket, 0);

    poll_flags = 0;
#ifdef APR_POLLSET_WAKEABLE
    poll_flags = APR_POLLSET_WAKEABLE;
#endif
    status = apr_pollset_create(&pollset, 1, io->pool, poll_flags);
    if (status == APR_ENOTIMPL && poll_flags != 0) {
        status = apr_pollset_create(&pollset, 1, io->pool, 0);
    } else if (status == APR_SUCCESS && poll_flags != 0) {
        wakeable = TRUE;
    }
    if (status != APR_SUCCESS) {
        apr_socket_close(socket);
        return status;
    }
    memset(&pollfd, 0, sizeof(pollfd));
    pollfd.p = io->pool;
    pollfd.desc_type = APR_POLL_SOCKET;
    pollfd.desc.s = socket;
    apr_thread_mutex_lock(io->mutex);
    io->socket = socket;
    io->pollset = pollset;
    io->pollfd = pollfd;
    io->registered_events = 0;
    io->wakeable = wakeable;
    apr_thread_mutex_unlock(io->mutex);
    return APR_SUCCESS;
}

static apr_status_t funasr_default_io_poll(
    void *obj,
    apr_interval_time_t timeout,
    apt_bool_t want_write,
    apr_int16_t *events)
{
    funasr_default_io_t *io;
    apr_int16_t wanted;
    apr_status_t status;
    apr_int32_t count;
    const apr_pollfd_t *descriptors;

    io = (funasr_default_io_t *)obj;
    if (!io->pollset || !io->socket || !events) {
        return APR_EINVAL;
    }
    wanted = APR_POLLIN;
    if (want_write) {
        wanted |= APR_POLLOUT;
    }
    if (wanted != io->registered_events) {
        if (io->registered_events != 0) {
            apr_pollset_remove(io->pollset, &io->pollfd);
        }
        io->pollfd.reqevents = wanted;
        status = apr_pollset_add(io->pollset, &io->pollfd);
        if (status != APR_SUCCESS) {
            return status;
        }
        io->registered_events = wanted;
    }
    *events = 0;
    status = apr_pollset_poll(
        io->pollset,
        timeout,
        &count,
        &descriptors);
    if (status == APR_SUCCESS && count > 0) {
        if ((descriptors[0].rtnevents & APR_POLLIN) != 0) {
            *events |= FUNASR_IO_READABLE;
        }
        if ((descriptors[0].rtnevents & APR_POLLOUT) != 0) {
            *events |= FUNASR_IO_WRITABLE;
        }
        if ((descriptors[0].rtnevents &
             (APR_POLLERR | APR_POLLHUP | APR_POLLNVAL)) != 0 &&
            (*events & FUNASR_IO_READABLE) == 0) {
            return APR_EOF;
        }
    }
    return status;
}

static apr_status_t funasr_default_io_read(
    void *obj,
    void *data,
    apr_size_t *size)
{
    funasr_default_io_t *io = (funasr_default_io_t *)obj;
    return apr_socket_recv(io->socket, (char *)data, size);
}

static apr_status_t funasr_default_io_write(
    void *obj,
    const void *data,
    apr_size_t *size)
{
    funasr_default_io_t *io = (funasr_default_io_t *)obj;
    apr_status_t status;

    status = apr_socket_send(io->socket, (const char *)data, size);
    if (status == APR_SUCCESS && *size != 0) {
        apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,
            "asr_websocket: [session_id=%s] 发送 ASR WebSocket 网络数据包，大小=%" APR_SIZE_T_FMT " 字节",
            io->session_id ? io->session_id : "N/A",
            *size);
    }
    return status;
}

static void funasr_default_io_close(void *obj)
{
    funasr_default_io_t *io = (funasr_default_io_t *)obj;
    apr_thread_mutex_lock(io->mutex);
    if (io->pollset) {
        apr_pollset_destroy(io->pollset);
        io->pollset = NULL;
    }
    if (io->socket) {
        apr_socket_close(io->socket);
        io->socket = NULL;
    }
    io->registered_events = 0;
    io->wakeable = FALSE;
    apr_thread_mutex_unlock(io->mutex);
}

static apr_status_t funasr_default_io_wake(void *obj)
{
    funasr_default_io_t *io = (funasr_default_io_t *)obj;
    apr_status_t status;

    status = APR_ENOTIMPL;
    apr_thread_mutex_lock(io->mutex);
#ifdef APR_POLLSET_WAKEABLE
    if (io->pollset && io->wakeable) {
        status = apr_pollset_wakeup(io->pollset);
    }
#endif
    apr_thread_mutex_unlock(io->mutex);
    return status;
}

static const funasr_transport_io_vtable_t funasr_default_io_vtable = {
    funasr_default_io_open,
    funasr_default_io_poll,
    funasr_default_io_read,
    funasr_default_io_write,
    funasr_default_io_close,
    funasr_default_io_wake
};

static apt_bool_t funasr_status_retryable(apr_status_t status)
{
    return APR_STATUS_IS_EAGAIN(status) ||
        APR_STATUS_IS_EINTR(status) ||
        APR_STATUS_IS_TIMEUP(status);
}

static void funasr_metrics_refresh_locked(funasr_transport_t *transport)
{
    apr_uint64_t threshold;
    apr_uint64_t accumulated;
    apr_size_t bucket;

    if (transport->metrics.media_gap_samples == 0) {
        transport->metrics.media_gap_p99_us = 0;
        return;
    }
    threshold = (transport->metrics.media_gap_samples * 99U + 99U) / 100U;
    accumulated = 0;
    for (bucket = 0;
         bucket < FUNASR_MEDIA_GAP_HISTOGRAM_BUCKETS;
         ++bucket) {
        accumulated += transport->metrics.media_gap_histogram[bucket];
        if (accumulated >= threshold) {
            transport->metrics.media_gap_p99_us =
                (apr_int64_t)bucket * 1000;
            return;
        }
    }
}

static apt_bool_t funasr_emit_event(
    funasr_transport_t *transport,
    funasr_generation_t generation,
    funasr_transport_event_type_e type,
    funasr_transport_failure_e failure,
    char *text,
    apr_size_t text_size)
{
    funasr_transport_event_t *event;
    apt_bool_t accepted;

    event = (funasr_transport_event_t *)calloc(1, sizeof(*event));
    if (!event) {
        free(text);
        return FALSE;
    }
    event->transport_id = transport->id;
    event->generation = generation;
    event->type = type;
    event->failure = failure;
    event->text = text;
    event->text_size = text_size;
    apr_thread_mutex_lock(transport->mutex);
    funasr_metrics_refresh_locked(transport);
    event->metrics = transport->metrics;
    apr_thread_mutex_unlock(transport->mutex);

    accepted = FALSE;
    if (transport->config.event_sink) {
        accepted = transport->config.event_sink(
            transport->config.event_sink_obj,
            event);
    }
    if (!accepted) {
        funasr_transport_event_destroy(event);
    }
    return accepted;
}

static apt_bool_t funasr_emit_close_fence(
    funasr_transport_t *transport)
{
    funasr_transport_event_t *event;
    apt_bool_t accepted;

    event = transport->close_event;
    transport->close_event = NULL;
    if (!event) {
        apr_thread_mutex_lock(transport->mutex);
        transport->close_fence_failed = TRUE;
        apr_thread_mutex_unlock(transport->mutex);
        return FALSE;
    }
    event->transport_id = transport->id;
    event->generation = transport->generation;
    event->type = FUNASR_EVENT_WORKER_CLOSED;
    apr_thread_mutex_lock(transport->mutex);
    funasr_metrics_refresh_locked(transport);
    event->metrics = transport->metrics;
    apr_thread_mutex_unlock(transport->mutex);
    accepted = transport->config.event_sink(
        transport->config.event_sink_obj,
        event);
    if (!accepted) {
        funasr_transport_event_destroy(event);
        apr_thread_mutex_lock(transport->mutex);
        transport->close_fence_failed = TRUE;
        apr_thread_mutex_unlock(transport->mutex);
    }
    return accepted;
}

static apt_bool_t funasr_http_header_value_equal(
    const unsigned char *header,
    apr_size_t size,
    const char *name,
    const char *expected)
{
    apr_size_t start;

    start = 0;
    while (start + 1U < size) {
        apr_size_t end;
        apr_size_t colon;
        apr_size_t value_start;
        apr_size_t value_end;

        end = start;
        while (end + 1U < size &&
               !(header[end] == '\r' && header[end + 1U] == '\n')) {
            ++end;
        }
        colon = start;
        while (colon < end && header[colon] != ':') {
            ++colon;
        }
        if (colon < end &&
            funasr_span_equal_ci(header + start, colon - start, name)) {
            value_start = colon + 1U;
            while (value_start < end &&
                   (header[value_start] == ' ' ||
                    header[value_start] == '\t')) {
                ++value_start;
            }
            value_end = end;
            while (value_end > value_start &&
                   (header[value_end - 1U] == ' ' ||
                    header[value_end - 1U] == '\t')) {
                --value_end;
            }
            return funasr_span_equal_ci(
                header + value_start,
                value_end - value_start,
                expected);
        }
        if (end + 2U > size) {
            break;
        }
        start = end + 2U;
    }
    return FALSE;
}

static apr_size_t funasr_ws_frame_encode(
    funasr_transport_t *transport,
    funasr_ws_opcode_e opcode,
    const unsigned char *payload,
    apr_size_t payload_size,
    unsigned char *output,
    apr_size_t output_capacity)
{
    unsigned char mask[4];
    apr_size_t header_size;
    apr_size_t index;

    if (!output || payload_size > FUNASR_WS_FRAME_LIMIT) {
        return 0;
    }
    header_size = 2U;
    if (payload_size >= 126U && payload_size <= 65535U) {
        header_size += 2U;
    } else if (payload_size > 65535U) {
        header_size += 8U;
    }
    header_size += 4U;
    if (payload_size > output_capacity - header_size) {
        return 0;
    }
    output[0] = (unsigned char)(0x80U | (unsigned char)opcode);
    if (payload_size < 126U) {
        output[1] = (unsigned char)(0x80U | payload_size);
        header_size = 2U;
    } else if (payload_size <= 65535U) {
        output[1] = 0xFEU;
        output[2] = (unsigned char)((payload_size >> 8) & 0xFFU);
        output[3] = (unsigned char)(payload_size & 0xFFU);
        header_size = 4U;
    } else {
        apr_uint64_t length = payload_size;
        output[1] = 0xFFU;
        for (index = 0; index < 8U; ++index) {
            output[2U + index] =
                (unsigned char)((length >> (56U - index * 8U)) & 0xFFU);
        }
        header_size = 10U;
    }
    if (apr_generate_random_bytes(mask, sizeof(mask)) != APR_SUCCESS) {
        apr_uint64_t seed = transport->id ^ transport->generation;
        for (index = 0; index < sizeof(mask); ++index) {
            mask[index] = (unsigned char)(seed >> (index * 8U));
        }
    }
    memcpy(output + header_size, mask, sizeof(mask));
    header_size += sizeof(mask);
    for (index = 0; index < payload_size; ++index) {
        output[header_size + index] =
            (unsigned char)(payload[index] ^ mask[index % 4U]);
    }
    return header_size + payload_size;
}

static apt_bool_t funasr_transport_generation_state(
    funasr_transport_t *transport,
    funasr_generation_t generation,
    apt_bool_t *cancel,
    apt_bool_t *close,
    apt_bool_t *failed,
    funasr_transport_failure_e *failure)
{
    apt_bool_t current;

    apr_thread_mutex_lock(transport->mutex);
    current = transport->active && transport->generation == generation;
    *cancel = transport->cancel_requested;
    *close = transport->close_requested;
    *failed = transport->media_failure_latched;
    *failure = transport->media_failure;
    apr_thread_mutex_unlock(transport->mutex);
    return current;
}

static apt_bool_t funasr_transport_enter_finishing(
    funasr_transport_t *transport,
    funasr_generation_t generation)
{
    apt_bool_t entered;

    apr_thread_mutex_lock(transport->mutex);
    entered = transport->active && transport->generation == generation;
    if (entered) {
        transport->active = FALSE;
        transport->finishing = TRUE;
    }
    apr_thread_mutex_unlock(transport->mutex);
    if (entered) {
        funasr_tx_ring_stop_enqueue(transport->ring);
    }
    return entered;
}

static apt_bool_t funasr_transport_commit_terminal(
    funasr_transport_t *transport,
    funasr_generation_t generation)
{
    apt_bool_t cancel;

    cancel = FALSE;
    apr_thread_mutex_lock(transport->mutex);
    if (transport->finishing && transport->generation == generation) {
        cancel = transport->cancel_requested;
        transport->cancel_requested = FALSE;
        transport->finishing = FALSE;
    }
    apr_thread_mutex_unlock(transport->mutex);
    return cancel;
}

static apt_bool_t funasr_worker_write_pending(
    funasr_transport_t *transport,
    unsigned char *buffer,
    apr_size_t size,
    apr_size_t *offset,
    apr_int64_t *last_progress_us)
{
    apr_size_t amount;
    apr_status_t status;

    amount = size - *offset;
    status = transport->io_vtable->write(
        transport->io_obj,
        buffer + *offset,
        &amount);
    if (status == APR_SUCCESS && amount != 0) {
        *offset += amount;
        *last_progress_us = funasr_clock_now_us(&transport->config.clock);
        return TRUE;
    }
    if (funasr_status_retryable(status)) {
        return TRUE;
    }
    return FALSE;
}

static apt_bool_t funasr_worker_handle_ws_event(
    funasr_transport_t *transport,
    funasr_generation_t generation,
    const funasr_ws_event_t *ws_event,
    unsigned char *tx_buffer,
    apr_size_t tx_capacity,
    apr_size_t *tx_size,
    apr_size_t *tx_offset,
    unsigned char *pong_payload,
    apr_size_t *pong_size,
    apt_bool_t *pong_pending,
    char **final_text,
    apr_size_t *final_text_size,
    apt_bool_t *final_received);

static apt_bool_t funasr_worker_handshake(
    funasr_transport_t *transport,
    funasr_generation_t generation,
    funasr_ws_decoder_t *ws_decoder,
    funasr_ws_event_t *sticky_event,
    apt_bool_t *sticky_ready)
{
    unsigned char nonce[16];
    char key[32];
    char accept[32];
    char request[FUNASR_HANDSHAKE_BUFFER_SIZE];
    unsigned char header_storage[FUNASR_HTTP_HEADER_LIMIT];
    unsigned char input[FUNASR_WORKER_READ_BUFFER_SIZE];
    funasr_http_decoder_t http_decoder;
    apr_int64_t deadline;
    apr_int64_t stop_deadline;
    apr_size_t request_size;
    apr_size_t request_offset;
    apr_size_t index;
    int result;

    *sticky_ready = FALSE;
    memset(sticky_event, 0, sizeof(*sticky_event));
    if (apr_generate_random_bytes(nonce, sizeof(nonce)) != APR_SUCCESS) {
        for (index = 0; index < sizeof(nonce); ++index) {
            nonce[index] = (unsigned char)(generation >> ((index % 8U) * 8U));
        }
    }
    if (apr_base64_encode_binary(key, nonce, sizeof(nonce)) <= 0 ||
        !funasr_ws_accept_compute(key, accept, sizeof(accept))) {
        return FALSE;
    }
    result = apr_snprintf(
        request,
        sizeof(request),
        "GET %s?system_id=ncc&scene_id=outcall&call_id=%s&sample_rate=%u&n_channels=%u&sample_width=%u HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n",
        transport->path,
        transport->call_id,
        (unsigned int)transport->format.output_sample_rate,
        (unsigned int)transport->format.channel_count,
        (unsigned int)transport->format.sample_width,
        transport->host,
        (unsigned int)transport->config.port,
        key);
    if (result <= 0 || (apr_size_t)result >= sizeof(request)) {
        return FALSE;
    }
    request_size = (apr_size_t)result;
    request_offset = 0;
    funasr_http_decoder_init(
        &http_decoder,
        header_storage,
        sizeof(header_storage));
    deadline = funasr_clock_now_us(&transport->config.clock) +
        transport->config.handshake_timeout_us;
    stop_deadline = 0;

    while (funasr_clock_now_us(&transport->config.clock) < deadline) {
        apt_bool_t cancel;
        apt_bool_t close;
        apt_bool_t media_failed;
        funasr_transport_failure_e media_failure;
        apr_int16_t events;
        apr_status_t status;

        if (!funasr_transport_generation_state(
                transport,
                generation,
                &cancel,
                &close,
                &media_failed,
                &media_failure) || close || media_failed) {
            return FALSE;
        }
        if (cancel) {
            apr_int64_t now_us =
                funasr_clock_now_us(&transport->config.clock);
            if (stop_deadline == 0) {
                stop_deadline = now_us +
                    transport->config.stop_drain_timeout_us;
            } else if (now_us >= stop_deadline) {
                return FALSE;
            }
        }
        status = transport->io_vtable->poll(
            transport->io_obj,
            transport->config.poll_timeout_us,
            request_offset < request_size,
            &events);
        if (status != APR_SUCCESS && !funasr_status_retryable(status)) {
            return FALSE;
        }
        if ((events & FUNASR_IO_WRITABLE) != 0 &&
            request_offset < request_size) {
            apr_size_t amount = request_size - request_offset;
            status = transport->io_vtable->write(
                transport->io_obj,
                request + request_offset,
                &amount);
            if (status == APR_SUCCESS) {
                request_offset += amount;
            } else if (!funasr_status_retryable(status)) {
                return FALSE;
            }
        }
        if ((events & FUNASR_IO_READABLE) != 0) {
            apr_size_t amount = sizeof(input);
            apr_size_t consumed = 0;
            funasr_http_status_e http_status;

            status = transport->io_vtable->read(
                transport->io_obj,
                input,
                &amount);
            if (status == APR_EOF || (status == APR_SUCCESS && amount == 0)) {
                return FALSE;
            }
            if (status != APR_SUCCESS) {
                if (funasr_status_retryable(status)) {
                    continue;
                }
                return FALSE;
            }
            http_status = funasr_http_decoder_feed(
                &http_decoder,
                input,
                amount,
                &consumed);
            if (http_status == FUNASR_HTTP_COMPLETE) {
                if (!funasr_http_header_value_equal(
                        http_decoder.storage,
                        http_decoder.size,
                        "sec-websocket-accept",
                        accept)) {
                    return FALSE;
                }
                if (consumed < amount) {
                    funasr_ws_status_e sticky_status;
                    sticky_status = funasr_ws_decoder_feed(
                            ws_decoder,
                            input + consumed,
                            amount - consumed,
                            sticky_event);
                    if (sticky_status == FUNASR_WS_EVENT_READY) {
                        *sticky_ready = TRUE;
                    } else if (sticky_status == FUNASR_WS_PROTOCOL_ERROR ||
                               sticky_status == FUNASR_WS_LIMIT_EXCEEDED) {
                        return FALSE;
                    }
                }
                return TRUE;
            }
            if (http_status != FUNASR_HTTP_NEED_MORE) {
                return FALSE;
            }
        }
    }
    return FALSE;
}

static apt_bool_t funasr_worker_handle_ws_event(
    funasr_transport_t *transport,
    funasr_generation_t generation,
    const funasr_ws_event_t *ws_event,
    unsigned char *tx_buffer,
    apr_size_t tx_capacity,
    apr_size_t *tx_size,
    apr_size_t *tx_offset,
    unsigned char *pong_payload,
    apr_size_t *pong_size,
    apt_bool_t *pong_pending,
    char **final_text,
    apr_size_t *final_text_size,
    apt_bool_t *final_received)
{
    (void)generation;
    if (ws_event->type == FUNASR_WS_EVENT_MESSAGE &&
        ws_event->opcode == FUNASR_WS_OPCODE_TEXT) {
        int code;
        char *text_value;
        apr_size_t text_size;

        apr_thread_mutex_lock(transport->mutex);
        transport->metrics.ws_rx_messages++;
        apr_thread_mutex_unlock(transport->mutex);
        if (funasr_json_get_int(
                (const char *)ws_event->data,
                ws_event->size,
                "code",
                &code) != FUNASR_JSON_OK || code != 0) {
            return FALSE;
        }
        text_value = NULL;
        text_size = 0;
        if (funasr_json_get_string_heap(
                (const char *)ws_event->data,
                ws_event->size,
                "text",
                FUNASR_WS_MESSAGE_LIMIT,
                &text_value,
                &text_size) != FUNASR_JSON_OK) {
            return FALSE;
        }
        *final_text = text_value;
        *final_text_size = text_size;
        *final_received = TRUE;
        return TRUE;
    }
    if (ws_event->type == FUNASR_WS_EVENT_PING) {
        if (*tx_size == *tx_offset) {
            *tx_size = funasr_ws_frame_encode(
                transport,
                FUNASR_WS_OPCODE_PONG,
                ws_event->data,
                ws_event->size,
                tx_buffer,
                tx_capacity);
            *tx_offset = 0;
            return *tx_size != 0;
        }
        if (ws_event->size > 125U) {
            return FALSE;
        }
        memcpy(pong_payload, ws_event->data, ws_event->size);
        *pong_size = ws_event->size;
        *pong_pending = TRUE;
        return TRUE;
    }
    if (ws_event->type == FUNASR_WS_EVENT_CLOSE) {
        return FALSE;
    }
    return TRUE;
}

static apt_bool_t funasr_worker_read_available(
    funasr_transport_t *transport,
    funasr_generation_t generation,
    funasr_ws_decoder_t *decoder,
    unsigned char *tx_buffer,
    apr_size_t tx_capacity,
    apr_size_t *tx_size,
    apr_size_t *tx_offset,
    unsigned char *pong_payload,
    apr_size_t *pong_size,
    apt_bool_t *pong_pending,
    char **final_text,
    apr_size_t *final_text_size,
    apt_bool_t *final_received)
{
    unsigned char input[FUNASR_WORKER_READ_BUFFER_SIZE];
    apr_size_t budget;

    budget = 64U * 1024U;
    while (budget != 0) {
        apr_size_t amount = sizeof(input);
        apr_status_t status;
        funasr_ws_status_e ws_status;
        funasr_ws_event_t ws_event;

        if (amount > budget) {
            amount = budget;
        }
        status = transport->io_vtable->read(
            transport->io_obj,
            input,
            &amount);
        if (status == APR_EOF || (status == APR_SUCCESS && amount == 0)) {
            return FALSE;
        }
        if (status != APR_SUCCESS) {
            return funasr_status_retryable(status);
        }
        budget -= amount;
        apr_thread_mutex_lock(transport->mutex);
        if (amount < sizeof(input)) {
            transport->metrics.ws_rx_partial_reads++;
        }
        apr_thread_mutex_unlock(transport->mutex);
        ws_status = funasr_ws_decoder_feed(
            decoder,
            input,
            amount,
            &ws_event);
        while (ws_status == FUNASR_WS_EVENT_READY) {
            if (!funasr_worker_handle_ws_event(
                    transport,
                    generation,
                    &ws_event,
                    tx_buffer,
                    tx_capacity,
                    tx_size,
                    tx_offset,
                    pong_payload,
                    pong_size,
                    pong_pending,
                    final_text,
                    final_text_size,
                    final_received)) {
                return FALSE;
            }
            if (*final_received) {
                return TRUE;
            }
            ws_status = funasr_ws_decoder_feed(
                decoder,
                NULL,
                0,
                &ws_event);
        }
        if (ws_status == FUNASR_WS_PROTOCOL_ERROR ||
            ws_status == FUNASR_WS_LIMIT_EXCEEDED) {
            return FALSE;
        }
        if (amount < sizeof(input)) {
            return TRUE;
        }
    }
    return TRUE;
}

static void *APR_THREAD_FUNC funasr_transport_worker(
    apr_thread_t *thread,
    void *obj)
{
    funasr_transport_t *transport;
    unsigned char *frame_payload;
    unsigned char *tx_buffer;
    unsigned char *frame_storage;
    unsigned char *message_storage;
    apr_size_t max_chunk;

    (void)thread;
    transport = (funasr_transport_t *)obj;
    max_chunk = funasr_pcm_bytes_for_ms(
        FUNASR_OUTPUT_SAMPLE_RATE,
        FUNASR_MAX_CHANNELS,
        FUNASR_SAMPLE_WIDTH_BYTES,
        FUNASR_WS_CHUNK_DURATION_MS);
    frame_payload = apr_palloc(transport->pool, max_chunk);
    tx_buffer = apr_palloc(
        transport->pool,
        max_chunk + FUNASR_WS_FRAME_OVERHEAD);
    frame_storage = apr_palloc(
        transport->pool,
        FUNASR_WS_FRAME_LIMIT + FUNASR_WS_FRAME_OVERHEAD);
    message_storage = apr_palloc(
        transport->pool,
        FUNASR_WS_MESSAGE_LIMIT);

    for (;;) {
        funasr_generation_t generation;
        apt_bool_t close_requested;

        apr_thread_mutex_lock(transport->mutex);
        while (!transport->active && !transport->close_requested) {
            apr_thread_cond_timedwait(
                transport->condition,
                transport->mutex,
                transport->config.poll_timeout_us);
        }
        close_requested = transport->close_requested;
        generation = transport->generation;
        apr_thread_mutex_unlock(transport->mutex);
        if (close_requested) {
            break;
        }

        {
            funasr_ws_decoder_t decoder;
            apr_status_t open_status;
            apr_size_t tx_size;
            apr_size_t tx_offset;
            apr_int64_t last_write_progress_us;
            apr_int64_t write_wait_started_us;
            apr_int64_t last_audio_send_us;
            apr_int64_t stop_started_us;
            unsigned char pong_payload[125];
            apr_size_t pong_size;
            apt_bool_t pong_pending;
            char *final_text;
            apr_size_t final_text_size;
            apt_bool_t input_started;
            apt_bool_t end_frame_queued;
            apt_bool_t idle_endpoint_pending;
            apt_bool_t final_received;
            apt_bool_t generation_failed;
            apt_bool_t generation_drained;
            apt_bool_t tx_audio;
            apr_size_t tx_audio_bytes;
            apt_bool_t sticky_ready;
            funasr_ws_event_t sticky_event;
            funasr_transport_failure_e terminal_failure;

            tx_size = 0;
            tx_offset = 0;
            last_write_progress_us =
                funasr_clock_now_us(&transport->config.clock);
            write_wait_started_us = 0;
            last_audio_send_us = 0;
            stop_started_us = 0;
            pong_size = 0;
            pong_pending = FALSE;
            final_text = NULL;
            final_text_size = 0;
            input_started = FALSE;
            end_frame_queued = FALSE;
            idle_endpoint_pending = FALSE;
            final_received = FALSE;
            generation_failed = FALSE;
            generation_drained = FALSE;
            tx_audio = FALSE;
            tx_audio_bytes = 0;
            terminal_failure = FUNASR_FAILURE_NONE;
            funasr_ws_decoder_init(
                &decoder,
                frame_storage,
                FUNASR_WS_FRAME_LIMIT + FUNASR_WS_FRAME_OVERHEAD,
                message_storage,
                FUNASR_WS_MESSAGE_LIMIT,
                FUNASR_WS_FRAME_LIMIT,
                FUNASR_WS_MESSAGE_LIMIT);
            funasr_ws_decoder_reject_masked(&decoder, TRUE);

            open_status = transport->io_vtable->open(
                transport->io_obj,
                transport->host,
                transport->config.port,
                transport->config.connect_timeout_us);
            if (open_status != APR_SUCCESS) {
                terminal_failure = FUNASR_FAILURE_CONNECT;
                generation_failed = TRUE;
            } else if (!funasr_worker_handshake(
                           transport,
                           generation,
                           &decoder,
                           &sticky_event,
                           &sticky_ready)) {
                apt_bool_t cancel;
                apt_bool_t close;
                apt_bool_t media_failed;
                funasr_transport_failure_e media_failure;

                funasr_transport_generation_state(
                    transport,
                    generation,
                    &cancel,
                    &close,
                    &media_failed,
                    &media_failure);
                if (cancel) {
                    generation_drained = TRUE;
                } else if (!close) {
                    terminal_failure = media_failed ?
                        media_failure : FUNASR_FAILURE_HANDSHAKE;
                    generation_failed = TRUE;
                }
            }
            if (!generation_failed && sticky_ready &&
                !funasr_worker_handle_ws_event(
                    transport,
                    generation,
                    &sticky_event,
                    tx_buffer,
                    max_chunk + FUNASR_WS_FRAME_OVERHEAD,
                    &tx_size,
                    &tx_offset,
                    pong_payload,
                    &pong_size,
                    &pong_pending,
                    &final_text,
                    &final_text_size,
                    &final_received)) {
                terminal_failure = FUNASR_FAILURE_PROTOCOL;
                generation_failed = TRUE;
            }

            while (!generation_failed && !final_received &&
                   !generation_drained) {
                apt_bool_t cancel;
                apt_bool_t close;
                apt_bool_t media_failed;
                funasr_transport_failure_e media_failure;
                apr_int64_t now_us;
                apr_int64_t last_media_us;
                apr_int16_t events;
                apr_status_t poll_status;
                apr_size_t ring_size;

                if (!funasr_transport_generation_state(
                        transport,
                        generation,
                        &cancel,
                        &close,
                        &media_failed,
                        &media_failure)) {
                    break;
                }
                if (close) {
                    break;
                }
                now_us = funasr_clock_now_us(&transport->config.clock);
                apr_thread_mutex_lock(transport->mutex);
                last_media_us = transport->last_media_us;
                apr_thread_mutex_unlock(transport->mutex);
                if (media_failed) {
                    terminal_failure = media_failure;
                    generation_failed = TRUE;
                    break;
                }
                if (cancel && stop_started_us == 0) {
                    stop_started_us = now_us;
                }

                ring_size = funasr_tx_ring_size(transport->ring);
                if (ring_size != 0 && !input_started) {
                    funasr_emit_event(
                        transport,
                        generation,
                        FUNASR_EVENT_INPUT_STARTED,
                        FUNASR_FAILURE_INTERNAL,
                        NULL,
                        0);
                    input_started = TRUE;
                }
                if (tx_size == tx_offset && pong_pending) {
                    tx_size = funasr_ws_frame_encode(
                        transport,
                        FUNASR_WS_OPCODE_PONG,
                        pong_payload,
                        pong_size,
                        tx_buffer,
                        max_chunk + FUNASR_WS_FRAME_OVERHEAD);
                    tx_offset = 0;
                    pong_size = 0;
                    pong_pending = FALSE;
                    tx_audio = FALSE;
                    tx_audio_bytes = 0;
                    write_wait_started_us = now_us;
                } else if (tx_size == tx_offset &&
                    !end_frame_queued &&
                    (ring_size >= transport->chunk_size ||
                     (cancel && ring_size != 0))) {
                    apr_size_t amount = transport->chunk_size;
                    if (amount > ring_size) {
                        amount = ring_size;
                    }
                    if (funasr_tx_ring_dequeue(
                            transport->ring,
                            frame_payload,
                            &amount)) {
                        tx_size = funasr_ws_frame_encode(
                            transport,
                            FUNASR_WS_OPCODE_BINARY,
                            frame_payload,
                            amount,
                            tx_buffer,
                            max_chunk + FUNASR_WS_FRAME_OVERHEAD);
                        tx_offset = 0;
                        tx_audio = TRUE;
                        tx_audio_bytes = amount;
                        write_wait_started_us = now_us;
                    }
                }
                if (cancel && tx_size == tx_offset &&
                    funasr_tx_ring_size(transport->ring) == 0 &&
                    !end_frame_queued) {
                    tx_size = funasr_ws_frame_encode(
                        transport,
                        FUNASR_WS_OPCODE_BINARY,
                        NULL,
                        0,
                        tx_buffer,
                        max_chunk + FUNASR_WS_FRAME_OVERHEAD);
                    tx_offset = 0;
                    tx_audio = FALSE;
                    tx_audio_bytes = 0;
                    write_wait_started_us = now_us;
                    end_frame_queued = TRUE;
                }
                /* Natural end of input: once no audio has been sent for
                   input_idle_timeout_us, latch the endpoint decision, flush
                   any trailing bytes and send the empty binary end-of-stream
                   frame, then keep waiting for the final result. */
                if (!cancel && input_started &&
                    !idle_endpoint_pending &&
                    tx_size == tx_offset &&
                    !end_frame_queued &&
                    last_media_us != 0 &&
                    now_us - last_media_us >=
                        transport->config.input_idle_timeout_us) {
                    idle_endpoint_pending = TRUE;
                    funasr_tx_ring_stop_enqueue(transport->ring);
                }
                if (!cancel && idle_endpoint_pending &&
                    !end_frame_queued &&
                    tx_size == tx_offset &&
                    funasr_tx_ring_size(transport->ring) != 0) {
                    apr_size_t amount = funasr_tx_ring_size(
                        transport->ring);
                    if (amount > max_chunk) {
                        amount = max_chunk;
                    }
                    if (funasr_tx_ring_dequeue(
                            transport->ring,
                            frame_payload,
                            &amount)) {
                        tx_size = funasr_ws_frame_encode(
                            transport,
                            FUNASR_WS_OPCODE_BINARY,
                            frame_payload,
                            amount,
                            tx_buffer,
                            max_chunk + FUNASR_WS_FRAME_OVERHEAD);
                        tx_offset = 0;
                        tx_audio = TRUE;
                        tx_audio_bytes = amount;
                        write_wait_started_us = now_us;
                    }
                }
                if (!cancel && idle_endpoint_pending &&
                    !end_frame_queued &&
                    tx_size == tx_offset &&
                    funasr_tx_ring_size(transport->ring) == 0) {
                    tx_size = funasr_ws_frame_encode(
                        transport,
                        FUNASR_WS_OPCODE_BINARY,
                        NULL,
                        0,
                        tx_buffer,
                        max_chunk + FUNASR_WS_FRAME_OVERHEAD);
                    tx_offset = 0;
                    tx_audio = FALSE;
                    tx_audio_bytes = 0;
                    write_wait_started_us = now_us;
                    end_frame_queued = TRUE;
                }
                if (cancel && end_frame_queued &&
                    tx_size == tx_offset) {
                    generation_drained = TRUE;
                    break;
                }

                poll_status = transport->io_vtable->poll(
                    transport->io_obj,
                    transport->config.poll_timeout_us,
                    tx_size != tx_offset,
                    &events);
                if (poll_status != APR_SUCCESS &&
                    !funasr_status_retryable(poll_status) &&
                    (events & FUNASR_IO_READABLE) == 0) {
                    terminal_failure = FUNASR_FAILURE_EOF;
                    generation_failed = TRUE;
                    break;
                }
                if ((events & FUNASR_IO_WRITABLE) != 0 &&
                    tx_size != tx_offset) {
                    apr_size_t before = tx_offset;
                    apt_bool_t metrics_due = FALSE;
                    if (!funasr_worker_write_pending(
                            transport,
                            tx_buffer,
                            tx_size,
                            &tx_offset,
                            &last_write_progress_us)) {
                        terminal_failure = FUNASR_FAILURE_EOF;
                        generation_failed = TRUE;
                        break;
                    }
                    if (tx_offset > before) {
                        apr_int64_t write_wait_ms =
                            write_wait_started_us == 0 ? 0 :
                            (last_write_progress_us - write_wait_started_us) /
                                1000;

                        apr_thread_mutex_lock(transport->mutex);
                        if (write_wait_ms >
                                transport->metrics.ws_write_wait_max_ms) {
                            transport->metrics.ws_write_wait_max_ms =
                                write_wait_ms;
                        }
                        if (tx_audio) {
                            last_audio_send_us = last_write_progress_us;
                            if (transport->metrics.ws_first_send_ms < 0) {
                                transport->metrics.ws_first_send_ms =
                                    (last_write_progress_us -
                                     transport->generation_started_us) / 1000;
                            }
                            if (tx_offset == tx_size) {
                                apr_int64_t gap =
                                    transport->metrics.ws_audio_last_send_us == 0 ?
                                    0 : last_write_progress_us -
                                        transport->metrics.ws_audio_last_send_us;

                                transport->metrics.ws_audio_frames++;
                                transport->metrics.ws_audio_bytes +=
                                    tx_audio_bytes;
                                transport->metrics.ws_audio_last_send_us =
                                    last_write_progress_us;
                                transport->metrics.ws_audio_gap_last_us = gap;
                                if (gap >
                                        transport->metrics.ws_audio_gap_max_us) {
                                    transport->metrics.ws_audio_gap_max_us = gap;
                                }
                                if (transport->last_metrics_report_us == 0 ||
                                    last_write_progress_us -
                                        transport->last_metrics_report_us >=
                                        FUNASR_METRICS_LOG_INTERVAL_US) {
                                    transport->last_metrics_report_us =
                                        last_write_progress_us;
                                    metrics_due = TRUE;
                                }
                            }
                        }
                        apr_thread_mutex_unlock(transport->mutex);
                        write_wait_started_us = last_write_progress_us;
                        if (metrics_due) {
                            funasr_emit_event(
                                transport,
                                generation,
                                FUNASR_EVENT_TRANSPORT_METRICS,
                                FUNASR_FAILURE_NONE,
                                NULL,
                                0);
                        }
                    }
                }
                if ((events & FUNASR_IO_READABLE) != 0 &&
                    !final_received) {
                    apt_bool_t had_pending = tx_size != tx_offset;
                    if (!funasr_worker_read_available(
                            transport,
                            generation,
                            &decoder,
                            tx_buffer,
                            max_chunk + FUNASR_WS_FRAME_OVERHEAD,
                            &tx_size,
                            &tx_offset,
                            pong_payload,
                            &pong_size,
                            &pong_pending,
                            &final_text,
                            &final_text_size,
                            &final_received)) {
                        terminal_failure = FUNASR_FAILURE_PROTOCOL;
                        generation_failed = TRUE;
                        break;
                    }
                    if (!had_pending && tx_size != tx_offset) {
                        tx_audio = FALSE;
                        tx_audio_bytes = 0;
                        write_wait_started_us = now_us;
                    }
                }
                now_us = funasr_clock_now_us(&transport->config.clock);
                if (tx_size != tx_offset &&
                    write_wait_started_us != 0 &&
                    now_us - write_wait_started_us >=
                        transport->config.write_stall_timeout_us) {
                    terminal_failure = FUNASR_FAILURE_WRITE_STALL;
                    generation_failed = TRUE;
                    break;
                }
                if (cancel && stop_started_us != 0 &&
                    now_us - stop_started_us >=
                        transport->config.stop_drain_timeout_us) {
                    terminal_failure = FUNASR_FAILURE_WRITE_STALL;
                    generation_drained = TRUE;
                    break;
                }
                if (!cancel && last_audio_send_us != 0 &&
                    now_us - last_audio_send_us >=
                        transport->config.no_result_timeout_us) {
                    terminal_failure = FUNASR_FAILURE_NO_RESULT_TIMEOUT;
                    generation_failed = TRUE;
                    break;
                }
            }
            transport->io_vtable->close(transport->io_obj);
            {
                apt_bool_t cancel;
                apt_bool_t close;
                apt_bool_t ignored_failed;
                funasr_transport_failure_e ignored_failure;

                funasr_transport_generation_state(
                    transport,
                    generation,
                    &cancel,
                    &close,
                    &ignored_failed,
                    &ignored_failure);
                apr_thread_mutex_lock(transport->mutex);
                transport->metrics.completion_failure = terminal_failure;
                if (generation_failed && !cancel && !close) {
                    transport->metrics.abnormal_closes++;
                }
                apr_thread_mutex_unlock(transport->mutex);
                funasr_transport_enter_finishing(transport, generation);
                funasr_emit_event(
                    transport,
                    generation,
                    FUNASR_EVENT_TRANSPORT_METRICS,
                    terminal_failure,
                    NULL,
                    0);
                cancel = funasr_transport_commit_terminal(
                    transport,
                    generation);
                if (cancel || generation_drained) {
                    free(final_text);
                    final_text = NULL;
                    funasr_emit_event(
                        transport,
                        generation,
                        FUNASR_EVENT_GENERATION_DRAINED,
                        terminal_failure,
                        NULL,
                        0);
                } else if (final_received) {
                    funasr_emit_event(
                        transport,
                        generation,
                        FUNASR_EVENT_FINAL_RESULT,
                        FUNASR_FAILURE_INTERNAL,
                        final_text,
                        final_text_size);
                    final_text = NULL;
                } else if (generation_failed && !close) {
                    funasr_emit_event(
                        transport,
                        generation,
                        FUNASR_EVENT_TRANSPORT_FAILED,
                        terminal_failure,
                        NULL,
                        0);
                }
                free(final_text);
            }
        }
    }

    transport->io_vtable->close(transport->io_obj);
    funasr_emit_close_fence(transport);
    apr_thread_mutex_lock(transport->mutex);
    transport->worker_closed = TRUE;
    apr_thread_cond_broadcast(transport->condition);
    apr_thread_mutex_unlock(transport->mutex);
    return NULL;
}

funasr_transport_t *funasr_transport_create(
    apr_pool_t *engine_pool,
    funasr_transport_id_t id,
    const funasr_transport_config_t *config)
{
    funasr_transport_t *transport;
    apr_pool_t *pool;
    apr_size_t max_ring;

    if (!engine_pool || !config || id == 0 || !config->host ||
        !config->path || !config->event_sink) {
        return NULL;
    }
    if (apr_pool_create(&pool, engine_pool) != APR_SUCCESS) {
        return NULL;
    }
    transport = apr_pcalloc(pool, sizeof(*transport));
    transport->pool = pool;
    transport->id = id;
    transport->config = *config;
    transport->host = apr_pstrdup(pool, config->host);
    transport->path = apr_pstrdup(pool, config->path);
    transport->config.host = transport->host;
    transport->config.path = transport->path;
    if (!transport->config.clock.now_us) {
        funasr_clock_default(&transport->config.clock);
    }
    if (transport->config.poll_timeout_us <= 0) {
        transport->config.poll_timeout_us = FUNASR_POLL_TIMEOUT_MS * 1000;
    }
    if (transport->config.connect_timeout_us <= 0) {
        transport->config.connect_timeout_us = FUNASR_CONNECT_TIMEOUT_US;
    }
    if (transport->config.handshake_timeout_us <= 0) {
        transport->config.handshake_timeout_us = FUNASR_HANDSHAKE_TIMEOUT_US;
    }
    if (transport->config.write_stall_timeout_us <= 0) {
        transport->config.write_stall_timeout_us =
            FUNASR_WRITE_STALL_TIMEOUT_US;
    }
    if (transport->config.stop_drain_timeout_us <= 0) {
        transport->config.stop_drain_timeout_us =
            FUNASR_STOP_DRAIN_TIMEOUT_US;
    }
    if (transport->config.no_result_timeout_us <= 0) {
        transport->config.no_result_timeout_us =
            FUNASR_NO_RESULT_TIMEOUT_US;
    }
    if (transport->config.input_idle_timeout_us <= 0) {
        transport->config.input_idle_timeout_us =
            FUNASR_INPUT_IDLE_TIMEOUT_US;
    }
    if (apr_thread_mutex_create(
            &transport->mutex,
            APR_THREAD_MUTEX_DEFAULT,
            pool) != APR_SUCCESS ||
        apr_thread_cond_create(&transport->condition, pool) != APR_SUCCESS ||
        apr_thread_mutex_create(
            &transport->default_io.mutex,
            APR_THREAD_MUTEX_DEFAULT,
            pool) != APR_SUCCESS) {
        apr_pool_destroy(pool);
        return NULL;
    }
    max_ring = funasr_pcm_bytes_for_ms(
        FUNASR_OUTPUT_SAMPLE_RATE,
        FUNASR_MAX_CHANNELS,
        FUNASR_SAMPLE_WIDTH_BYTES,
        FUNASR_TX_RING_DURATION_MS);
    transport->ring = funasr_tx_ring_create(pool, max_ring);
    transport->close_event =
        (funasr_transport_event_t *)calloc(1, sizeof(*transport->close_event));
    if (!transport->ring || !transport->close_event) {
        free(transport->close_event);
        apr_pool_destroy(pool);
        return NULL;
    }
    if (config->io_vtable) {
        transport->io_vtable = config->io_vtable;
        transport->io_obj = config->io_obj;
    } else {
        transport->default_io.pool = pool;
        transport->io_vtable = &funasr_default_io_vtable;
        transport->io_obj = &transport->default_io;
    }
    apr_pool_cleanup_register(
        pool,
        transport,
        funasr_transport_pool_cleanup,
        apr_pool_cleanup_null);
    return transport;
}

apt_bool_t funasr_transport_begin_generation(
    funasr_transport_t *transport,
    funasr_generation_t generation,
    const funasr_audio_format_t *format)
{
    apr_size_t ring_limit;
    apr_size_t chunk_size;

    if (!transport || !format || generation == 0 ||
        format->output_sample_rate != FUNASR_OUTPUT_SAMPLE_RATE ||
        format->sample_width != FUNASR_SAMPLE_WIDTH_BYTES ||
        format->channel_count == 0 ||
        format->channel_count > FUNASR_MAX_CHANNELS ||
        !format->call_id || strlen(format->call_id) > FUNASR_CALL_ID_LIMIT) {
        return FALSE;
    }
    ring_limit = funasr_pcm_bytes_for_ms(
        format->output_sample_rate,
        format->channel_count,
        format->sample_width,
        FUNASR_TX_RING_DURATION_MS);
    chunk_size = funasr_pcm_bytes_for_ms(
        format->output_sample_rate,
        format->channel_count,
        format->sample_width,
        FUNASR_WS_CHUNK_DURATION_MS);
    if (ring_limit == 0 || chunk_size == 0) {
        return FALSE;
    }
    apr_thread_mutex_lock(transport->mutex);
    if (transport->active || transport->finishing ||
        transport->close_requested ||
        transport->worker_closed) {
        apr_thread_mutex_unlock(transport->mutex);
        return FALSE;
    }
    transport->generation = generation;
    transport->format = *format;
    memcpy(
        transport->call_id,
        format->call_id,
        strlen(format->call_id) + 1U);
    transport->format.call_id = transport->call_id;
    transport->default_io.session_id = transport->call_id;
    memset(&transport->metrics, 0, sizeof(transport->metrics));
    transport->metrics.ws_first_send_ms = -1;
    transport->generation_started_us =
        funasr_clock_now_us(&transport->config.clock);
    transport->last_media_us = 0;
    transport->last_metrics_report_us = 0;
    transport->ring_limit = ring_limit;
    transport->chunk_size = chunk_size;
    transport->media_failure_latched = FALSE;
    transport->cancel_requested = FALSE;
    transport->finishing = FALSE;
    transport->active = TRUE;
    funasr_tx_ring_set_limit(transport->ring, ring_limit);
    funasr_tx_ring_begin(transport->ring, generation);
    if (!transport->thread_started) {
        if (apr_thread_create(
                &transport->thread,
                NULL,
                funasr_transport_worker,
                transport,
                transport->pool) != APR_SUCCESS) {
            transport->active = FALSE;
            apr_thread_mutex_unlock(transport->mutex);
            return FALSE;
        }
        transport->thread_started = TRUE;
    }
    apr_thread_cond_signal(transport->condition);
    apr_thread_mutex_unlock(transport->mutex);
    return TRUE;
}

apt_bool_t funasr_transport_media_snapshot(
    funasr_transport_t *transport,
    funasr_media_snapshot_t *snapshot)
{
    if (!transport || !snapshot) {
        return FALSE;
    }
    apr_thread_mutex_lock(transport->mutex);
    if (!transport->active || transport->cancel_requested) {
        apr_thread_mutex_unlock(transport->mutex);
        return FALSE;
    }
    snapshot->generation = transport->generation;
    snapshot->input_sample_rate = transport->format.input_sample_rate;
    snapshot->channel_count = transport->format.channel_count;
    apr_thread_mutex_unlock(transport->mutex);
    return TRUE;
}

funasr_enqueue_status_e funasr_transport_enqueue_pcm(
    funasr_transport_t *transport,
    funasr_generation_t generation,
    const void *data,
    apr_size_t size,
    apr_int64_t now_us)
{
    funasr_enqueue_status_e status;
    apr_size_t ring_size;
    apr_int64_t enqueue_started_us;
    apr_int64_t enqueue_elapsed_us;

    if (!transport) {
        return FUNASR_ENQUEUE_NOT_STREAMING;
    }
    enqueue_started_us = funasr_clock_now_us(&transport->config.clock);
    status = funasr_tx_ring_enqueue(
        transport->ring,
        generation,
        data,
        size);
    apr_thread_mutex_lock(transport->mutex);
    if (status == FUNASR_ENQUEUE_ACCEPTED) {
        apr_int64_t gap = transport->last_media_us == 0 ?
            0 : now_us - transport->last_media_us;
        transport->metrics.media_frames++;
        transport->metrics.valid_audio_bytes += size;
        if (gap > transport->metrics.media_gap_max_us) {
            transport->metrics.media_gap_max_us = gap;
        }
        if (gap > 0) {
            apr_size_t bucket = (apr_size_t)(gap / 1000);
            if (bucket >= FUNASR_MEDIA_GAP_HISTOGRAM_BUCKETS) {
                bucket = FUNASR_MEDIA_GAP_HISTOGRAM_BUCKETS - 1U;
            }
            transport->metrics.media_gap_histogram[bucket]++;
            transport->metrics.media_gap_samples++;
        }
        transport->last_media_us = now_us;
        ring_size = funasr_tx_ring_size(transport->ring);
        if (ring_size > transport->metrics.tx_ring_high_water_bytes) {
            transport->metrics.tx_ring_high_water_bytes = ring_size;
        }
    } else if (status == FUNASR_ENQUEUE_QUEUE_FULL &&
               transport->generation == generation &&
               !transport->media_failure_latched) {
        transport->media_failure_latched = TRUE;
        transport->media_failure = FUNASR_FAILURE_QUEUE_OVERRUN;
        transport->metrics.tx_ring_overrun_bytes =
            funasr_tx_ring_overrun_bytes(transport->ring);
        transport->metrics.tx_ring_overrun_events =
            funasr_tx_ring_overrun_events(transport->ring);
    }
    enqueue_elapsed_us =
        funasr_clock_now_us(&transport->config.clock) - enqueue_started_us;
    if (enqueue_elapsed_us > transport->metrics.enqueue_max_us) {
        transport->metrics.enqueue_max_us = enqueue_elapsed_us;
    }
    apr_thread_cond_signal(transport->condition);
    apr_thread_mutex_unlock(transport->mutex);
    funasr_transport_wake(transport);
    return status;
}

apt_bool_t funasr_transport_latch_media_failure(
    funasr_transport_t *transport,
    funasr_generation_t generation,
    funasr_transport_failure_e failure)
{
    apt_bool_t latched;

    if (!transport) {
        return FALSE;
    }
    apr_thread_mutex_lock(transport->mutex);
    latched = transport->active &&
        transport->generation == generation &&
        !transport->media_failure_latched;
    if (latched) {
        transport->media_failure_latched = TRUE;
        transport->media_failure = failure;
        apr_thread_cond_signal(transport->condition);
    }
    apr_thread_mutex_unlock(transport->mutex);
    if (latched) {
        funasr_transport_wake(transport);
    }
    return latched;
}

apt_bool_t funasr_transport_cancel_generation(
    funasr_transport_t *transport,
    funasr_generation_t generation)
{
    apt_bool_t accepted;

    if (!transport) {
        return FALSE;
    }
    apr_thread_mutex_lock(transport->mutex);
    accepted = (transport->active || transport->finishing) &&
        !transport->close_requested &&
        transport->generation == generation;
    if (accepted) {
        transport->cancel_requested = TRUE;
        apr_thread_cond_signal(transport->condition);
    }
    apr_thread_mutex_unlock(transport->mutex);
    if (accepted) {
        funasr_tx_ring_stop_enqueue(transport->ring);
        funasr_transport_wake(transport);
    }
    return accepted;
}

apt_bool_t funasr_transport_wake(funasr_transport_t *transport)
{
    apr_status_t status;

    if (!transport) {
        return FALSE;
    }
    apr_thread_mutex_lock(transport->mutex);
    apr_thread_cond_signal(transport->condition);
    apr_thread_mutex_unlock(transport->mutex);
    status = transport->io_vtable->wake(transport->io_obj);
    return status == APR_SUCCESS || status == APR_ENOTIMPL;
}

apt_bool_t funasr_transport_request_close(funasr_transport_t *transport)
{
    apr_status_t start_status;

    if (!transport) {
        return FALSE;
    }
    apr_thread_mutex_lock(transport->mutex);
    if (transport->close_requested) {
        apr_thread_mutex_unlock(transport->mutex);
        return TRUE;
    }
    transport->close_requested = TRUE;
    transport->active = FALSE;
    start_status = APR_SUCCESS;
    if (!transport->thread_started) {
        start_status = apr_thread_create(
            &transport->thread,
            NULL,
            funasr_transport_worker,
            transport,
            transport->pool);
        if (start_status == APR_SUCCESS) {
            transport->thread_started = TRUE;
        }
    }
    apr_thread_cond_broadcast(transport->condition);
    apr_thread_mutex_unlock(transport->mutex);
    funasr_tx_ring_close(transport->ring);
    if (start_status != APR_SUCCESS) {
        return FALSE;
    }
    funasr_transport_wake(transport);
    return TRUE;
}

apr_status_t funasr_transport_join_closed(funasr_transport_t *transport)
{
    apr_status_t thread_status;
    apr_status_t status;

    if (!transport) {
        return APR_EINVAL;
    }
    apr_thread_mutex_lock(transport->mutex);
    if (!transport->thread_started) {
        transport->worker_closed = TRUE;
        transport->joined = TRUE;
        apr_thread_mutex_unlock(transport->mutex);
        return APR_SUCCESS;
    }
    if (transport->joined) {
        apr_thread_mutex_unlock(transport->mutex);
        return APR_SUCCESS;
    }
    apr_thread_mutex_unlock(transport->mutex);
    status = apr_thread_join(&thread_status, transport->thread);
    if (status == APR_SUCCESS) {
        apr_thread_mutex_lock(transport->mutex);
        transport->joined = TRUE;
        apr_thread_mutex_unlock(transport->mutex);
    }
    if (status != APR_SUCCESS) {
        return status;
    }
    if (thread_status != APR_SUCCESS) {
        return thread_status;
    }
    apr_thread_mutex_lock(transport->mutex);
    status = transport->close_fence_failed ? APR_EGENERAL : APR_SUCCESS;
    apr_thread_mutex_unlock(transport->mutex);
    return status;
}
