#include "funasr_ws_transport.h"

#include <apr_thread_mutex.h>
#include <string.h>

struct funasr_tx_ring_t {
    unsigned char *data;
    apr_size_t capacity;
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
    } else if (size > ring->capacity - ring->size) {
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
