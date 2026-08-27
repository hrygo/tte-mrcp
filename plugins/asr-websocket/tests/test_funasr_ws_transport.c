#include "funasr_clock.h"
#include "funasr_ws_transport.h"

#include <apr_general.h>
#include <apr_pools.h>
#include <apr_thread_mutex.h>
#include <apr_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) && _MSC_VER < 1900
#define snprintf _snprintf
#endif

static int failures = 0;

#define CHECK_TRUE(label, expression) \
    do { \
        if (!(expression)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, label); \
            failures++; \
        } \
    } while (0)

#define CHECK_SIZE(label, actual, expected) \
    do { \
        apr_size_t actual_value = (actual); \
        apr_size_t expected_value = (expected); \
        if (actual_value != expected_value) { \
            fprintf(stderr, "FAIL %s:%d: %s actual=%lu expected=%lu\n", \
                    __FILE__, __LINE__, label, \
                    (unsigned long)actual_value, (unsigned long)expected_value); \
            failures++; \
        } \
    } while (0)

static apr_int64_t fake_now_us(void *obj)
{
    return *(apr_int64_t *)obj;
}

static void test_fake_clock_contract(void)
{
    apr_int64_t value = 4999999;
    funasr_clock_t clock;

    clock.now_us = fake_now_us;
    clock.obj = &value;
    CHECK_TRUE("clock returns caller-owned monotonic value",
               funasr_clock_now_us(&clock) == 4999999);
    value = 5000000;
    CHECK_TRUE("clock boundary is exact",
               funasr_clock_now_us(&clock) == 5000000);
}

static void test_http_bytewise_and_sticky_frame(void)
{
    static const char response[] =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "\r\n"
        "\x81\x02ok";
    unsigned char storage[FUNASR_HTTP_HEADER_LIMIT];
    funasr_http_decoder_t decoder;
    funasr_http_status_e status;
    apr_size_t index;
    apr_size_t consumed;

    funasr_http_decoder_init(&decoder, storage, sizeof(storage));
    status = FUNASR_HTTP_NEED_MORE;
    for (index = 0; index < sizeof(response) - 5; ++index) {
        consumed = 0;
        status = funasr_http_decoder_feed(
            &decoder,
            response + index,
            1,
            &consumed);
        CHECK_SIZE("bytewise HTTP consumes one byte", consumed, 1);
        if (index + 1 < sizeof(response) - 5) {
            CHECK_TRUE("HTTP remains partial before terminator",
                       status == FUNASR_HTTP_NEED_MORE);
        }
    }
    CHECK_TRUE("HTTP 101 completes bytewise", status == FUNASR_HTTP_COMPLETE);

    funasr_http_decoder_init(&decoder, storage, sizeof(storage));
    consumed = 0;
    status = funasr_http_decoder_feed(
        &decoder,
        response,
        sizeof(response) - 1,
        &consumed);
    CHECK_TRUE("HTTP 101 accepts sticky frame", status == FUNASR_HTTP_COMPLETE);
    CHECK_SIZE("sticky WS bytes stay unconsumed",
               (sizeof(response) - 1) - consumed,
               4);
    CHECK_TRUE("sticky WS prefix is preserved",
               memcmp(response + consumed, "\x81\x02ok", 4) == 0);
}

static void test_http_rejects_invalid_and_oversized_headers(void)
{
    unsigned char storage[64];
    unsigned char oversized[65];
    funasr_http_decoder_t decoder;
    funasr_http_status_e status;
    apr_size_t consumed;

    funasr_http_decoder_init(&decoder, storage, sizeof(storage));
    consumed = 0;
    status = funasr_http_decoder_feed(
        &decoder,
        "HTTP/1.1 200 OK\r\n\r\n",
        sizeof("HTTP/1.1 200 OK\r\n\r\n") - 1,
        &consumed);
    CHECK_TRUE("non-101 handshake rejected", status == FUNASR_HTTP_PROTOCOL_ERROR);

    memset(oversized, 'A', sizeof(oversized));
    funasr_http_decoder_init(&decoder, storage, sizeof(storage));
    consumed = 0;
    status = funasr_http_decoder_feed(
        &decoder,
        oversized,
        sizeof(oversized),
        &consumed);
    CHECK_TRUE("header limit enforced", status == FUNASR_HTTP_LIMIT_EXCEEDED);
}

static void test_http_header_names_and_tokens_are_case_insensitive(void)
{
    static const char response[] =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "uPgRaDe: WebSocket\r\n"
        "cOnNeCtIoN: keep-alive, Upgrade\r\n"
        "\r\n";
    unsigned char storage[256];
    funasr_http_decoder_t decoder;
    apr_size_t consumed;

    funasr_http_decoder_init(&decoder, storage, sizeof(storage));
    consumed = 0;
    CHECK_TRUE("HTTP token lists are accepted",
               funasr_http_decoder_feed(
                   &decoder,
                   response,
                   sizeof(response) - 1,
                   &consumed) == FUNASR_HTTP_COMPLETE);
    CHECK_SIZE("token-list response fully consumed",
               consumed,
               sizeof(response) - 1);
}

static void test_ws_short_read_and_mask(void)
{
    static const unsigned char frame[] = {
        0x81, 0x85, 0x11, 0x22, 0x33, 0x44,
        (unsigned char)('h' ^ 0x11),
        (unsigned char)('e' ^ 0x22),
        (unsigned char)('l' ^ 0x33),
        (unsigned char)('l' ^ 0x44),
        (unsigned char)('o' ^ 0x11)
    };
    unsigned char frame_storage[64];
    unsigned char message_storage[64];
    funasr_ws_decoder_t decoder;
    funasr_ws_event_t event;
    funasr_ws_status_e status;
    apr_size_t index;

    funasr_ws_decoder_init(
        &decoder,
        frame_storage,
        sizeof(frame_storage),
        message_storage,
        sizeof(message_storage),
        sizeof(frame_storage),
        sizeof(message_storage));

    memset(&event, 0, sizeof(event));
    status = FUNASR_WS_NEED_MORE;
    for (index = 0; index < sizeof(frame); ++index) {
        status = funasr_ws_decoder_feed(
            &decoder,
            frame + index,
            1,
            &event);
        if (index + 1 < sizeof(frame)) {
            CHECK_TRUE("masked frame remains partial",
                       status == FUNASR_WS_NEED_MORE);
        }
    }
    CHECK_TRUE("masked text completes", status == FUNASR_WS_EVENT_READY);
    CHECK_TRUE("masked text event type", event.type == FUNASR_WS_EVENT_MESSAGE);
    CHECK_TRUE("masked text opcode", event.opcode == FUNASR_WS_OPCODE_TEXT);
    CHECK_SIZE("masked text length", event.size, 5);
    CHECK_TRUE("masked payload decoded", memcmp(event.data, "hello", 5) == 0);

    funasr_ws_decoder_reset(&decoder);
    funasr_ws_decoder_reject_masked(&decoder, TRUE);
    status = funasr_ws_decoder_feed(&decoder, frame, sizeof(frame), &event);
    CHECK_TRUE("client mode rejects masked server frame",
               status == FUNASR_WS_PROTOCOL_ERROR);
}

static void test_ws_extended_length_and_fragmented_control(void)
{
    unsigned char frame[134];
    unsigned char frame_storage[256];
    unsigned char message_storage[256];
    funasr_ws_decoder_t decoder;
    funasr_ws_event_t event;
    funasr_ws_status_e status;
    apr_size_t index;

    frame[0] = 0x82;
    frame[1] = 126;
    frame[2] = 0;
    frame[3] = 130;
    for (index = 0; index < 130; ++index) {
        frame[4 + index] = (unsigned char)(index & 0xff);
    }

    funasr_ws_decoder_init(
        &decoder,
        frame_storage,
        sizeof(frame_storage),
        message_storage,
        sizeof(message_storage),
        130,
        200);
    status = funasr_ws_decoder_feed(
        &decoder,
        frame,
        sizeof(frame),
        &event);
    CHECK_TRUE("extended payload completes", status == FUNASR_WS_EVENT_READY);
    CHECK_SIZE("extended payload size", event.size, 130);
    CHECK_TRUE("extended payload content",
               event.data[0] == 0 && event.data[129] == 129);

    funasr_ws_decoder_reset(&decoder);
    status = funasr_ws_decoder_feed(
        &decoder,
        "\x01\x03hel",
        5,
        &event);
    CHECK_TRUE("text fragment starts", status == FUNASR_WS_NEED_MORE);
    status = funasr_ws_decoder_feed(
        &decoder,
        "\x89\x01!",
        3,
        &event);
    CHECK_TRUE("interleaved ping delivered", status == FUNASR_WS_EVENT_READY);
    CHECK_TRUE("ping event type", event.type == FUNASR_WS_EVENT_PING);
    CHECK_SIZE("ping payload size", event.size, 1);
    status = funasr_ws_decoder_feed(
        &decoder,
        "\x80\x02lo",
        4,
        &event);
    CHECK_TRUE("fragmented text completes", status == FUNASR_WS_EVENT_READY);
    CHECK_TRUE("fragmented event is message",
               event.type == FUNASR_WS_EVENT_MESSAGE);
    CHECK_TRUE("fragmented opcode remains text",
               event.opcode == FUNASR_WS_OPCODE_TEXT);
    CHECK_SIZE("fragmented message size", event.size, 5);
    CHECK_TRUE("fragmented payload reassembled",
               memcmp(event.data, "hello", 5) == 0);
}

static void test_ws_protocol_and_size_errors(void)
{
    static const unsigned char oversized_frame[] = {
        0x82, 0x09, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i'
    };
    unsigned char frame_storage[32];
    unsigned char message_storage[32];
    funasr_ws_decoder_t decoder;
    funasr_ws_event_t event;

    funasr_ws_decoder_init(
        &decoder,
        frame_storage,
        sizeof(frame_storage),
        message_storage,
        sizeof(message_storage),
        8,
        8);
    CHECK_TRUE("continuation without fragment rejected",
               funasr_ws_decoder_feed(
                   &decoder,
                   "\x80\x00",
                   2,
                   &event) == FUNASR_WS_PROTOCOL_ERROR);

    funasr_ws_decoder_reset(&decoder);
    CHECK_TRUE("unknown opcode rejected",
               funasr_ws_decoder_feed(
                   &decoder,
                   "\x83\x00",
                   2,
                   &event) == FUNASR_WS_PROTOCOL_ERROR);

    funasr_ws_decoder_reset(&decoder);
    CHECK_TRUE("fragmented control rejected",
               funasr_ws_decoder_feed(
                   &decoder,
                   "\x09\x00",
                   2,
                   &event) == FUNASR_WS_PROTOCOL_ERROR);

    funasr_ws_decoder_reset(&decoder);
    CHECK_TRUE("frame limit enforced",
               funasr_ws_decoder_feed(
                   &decoder,
                   oversized_frame,
                   sizeof(oversized_frame),
                   &event) == FUNASR_WS_LIMIT_EXCEEDED);
}

static void test_ring_generation_wrap_and_overrun(apr_pool_t *pool)
{
    funasr_tx_ring_t *ring;
    unsigned char output[8];
    apr_size_t output_size;
    funasr_enqueue_status_e enqueue_status;

    ring = funasr_tx_ring_create(pool, 8);
    CHECK_TRUE("ring created", ring != NULL);
    CHECK_TRUE("generation starts", funasr_tx_ring_begin(ring, 7) == TRUE);

    enqueue_status = funasr_tx_ring_enqueue(
        ring,
        7,
        "abcdef",
        6);
    CHECK_TRUE("first enqueue accepted",
               enqueue_status == FUNASR_ENQUEUE_ACCEPTED);
    CHECK_SIZE("ring size after first enqueue", funasr_tx_ring_size(ring), 6);

    output_size = 4;
    CHECK_TRUE("ring dequeue succeeds",
               funasr_tx_ring_dequeue(ring, output, &output_size) == TRUE);
    CHECK_SIZE("ring dequeues requested bytes", output_size, 4);
    CHECK_TRUE("ring preserves first bytes", memcmp(output, "abcd", 4) == 0);

    CHECK_TRUE("wrap enqueue accepted",
               funasr_tx_ring_enqueue(
                   ring,
                   7,
                   "WXYZ",
                   4) == FUNASR_ENQUEUE_ACCEPTED);
    output_size = sizeof(output);
    CHECK_TRUE("wrapped dequeue succeeds",
               funasr_tx_ring_dequeue(ring, output, &output_size) == TRUE);
    CHECK_SIZE("wrapped dequeue size", output_size, 6);
    CHECK_TRUE("wrapped byte order", memcmp(output, "efWXYZ", 6) == 0);

    CHECK_TRUE("stale generation rejected",
               funasr_tx_ring_enqueue(
                   ring,
                   6,
                   "x",
                   1) == FUNASR_ENQUEUE_STALE_GENERATION);
    CHECK_TRUE("capacity fill accepted",
               funasr_tx_ring_enqueue(
                   ring,
                   7,
                   "12345678",
                   8) == FUNASR_ENQUEUE_ACCEPTED);
    CHECK_TRUE("overrun is explicit",
               funasr_tx_ring_enqueue(
                   ring,
                   7,
                   "9",
                   1) == FUNASR_ENQUEUE_QUEUE_FULL);
    CHECK_SIZE("overrun bytes counted",
               funasr_tx_ring_overrun_bytes(ring),
               1);
    CHECK_SIZE("overrun events counted",
               funasr_tx_ring_overrun_events(ring),
               1);

    funasr_tx_ring_close(ring);
    CHECK_TRUE("closed ring rejects enqueue",
               funasr_tx_ring_enqueue(
                   ring,
                   7,
                   "x",
                   1) == FUNASR_ENQUEUE_CLOSED);
}

typedef struct fake_io_t {
    apr_thread_mutex_t *mutex;
    unsigned char handshake_request[4096];
    apr_size_t handshake_request_size;
    unsigned char inbound[8192];
    apr_size_t inbound_size;
    apr_size_t inbound_offset;
    unsigned char outbound[65536];
    apr_size_t outbound_size;
    apr_size_t read_chunk;
    apr_size_t write_chunk;
    apt_bool_t opened;
    int open_count;
    int close_count;
    apt_bool_t handshake_ready;
    apt_bool_t stall_writes;
    int poll_want_write;
    int poll_without_write;
    apt_bool_t eof_with_readable;
    int wake_count;
    unsigned char handshake_suffix[256];
    apr_size_t handshake_suffix_size;
} fake_io_t;

typedef struct event_collector_t {
    apr_thread_mutex_t *mutex;
    int input_started;
    int final_results;
    int failures;
    int drained;
    int metrics;
    int closed;
    funasr_transport_metrics_t last_metrics;
    funasr_transport_failure_e last_failure;
    char final_text[64];
    funasr_transport_event_type_e sequence[32];
    int sequence_size;
    apt_bool_t reject_events;
} event_collector_t;

static apr_status_t fake_io_open(
    void *obj,
    const char *host,
    apr_port_t port,
    apr_interval_time_t timeout)
{
    fake_io_t *io = obj;

    (void)host;
    (void)port;
    (void)timeout;
    apr_thread_mutex_lock(io->mutex);
    io->opened = TRUE;
    io->open_count++;
    io->handshake_request_size = 0;
    io->inbound_size = 0;
    io->inbound_offset = 0;
    io->handshake_ready = FALSE;
    apr_thread_mutex_unlock(io->mutex);
    return APR_SUCCESS;
}

static apr_status_t fake_io_poll(
    void *obj,
    apr_interval_time_t timeout,
    apt_bool_t want_write,
    apr_int16_t *events)
{
    fake_io_t *io = obj;

    (void)timeout;
    *events = 0;
    apr_thread_mutex_lock(io->mutex);
    if (want_write) {
        io->poll_want_write++;
    } else {
        io->poll_without_write++;
    }
    if (io->inbound_offset < io->inbound_size) {
        *events |= FUNASR_IO_READABLE;
    }
    if (want_write && !io->stall_writes) {
        *events |= FUNASR_IO_WRITABLE;
    }
    apr_thread_mutex_unlock(io->mutex);
    if (*events == 0) {
        apr_sleep(1000);
        return APR_TIMEUP;
    }
    if (io->eof_with_readable && (*events & FUNASR_IO_READABLE) != 0) {
        return APR_EOF;
    }
    return APR_SUCCESS;
}

static apt_bool_t fake_find_client_key(
    const unsigned char *request,
    apr_size_t request_size,
    char *key,
    apr_size_t key_capacity)
{
    static const char header[] = "Sec-WebSocket-Key: ";
    apr_size_t offset;

    for (offset = 0; offset + sizeof(header) - 1 < request_size; ++offset) {
        apr_size_t end;
        apr_size_t key_size;

        if (memcmp(request + offset, header, sizeof(header) - 1) != 0) {
            continue;
        }
        offset += sizeof(header) - 1;
        end = offset;
        while (end < request_size && request[end] != '\r') {
            ++end;
        }
        key_size = end - offset;
        if (end >= request_size || key_size + 1 > key_capacity) {
            return FALSE;
        }
        memcpy(key, request + offset, key_size);
        key[key_size] = '\0';
        return TRUE;
    }
    return FALSE;
}

static apt_bool_t fake_request_complete(
    const unsigned char *request,
    apr_size_t request_size)
{
    apr_size_t offset;

    if (request_size < 4) {
        return FALSE;
    }
    for (offset = 0; offset + 4 <= request_size; ++offset) {
        if (memcmp(request + offset, "\r\n\r\n", 4) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static apr_status_t fake_io_write(
    void *obj,
    const void *data,
    apr_size_t *size)
{
    fake_io_t *io = obj;
    apr_size_t amount = *size;

    apr_thread_mutex_lock(io->mutex);
    if (io->stall_writes) {
        *size = 0;
        apr_thread_mutex_unlock(io->mutex);
        return APR_EAGAIN;
    }
    if (io->write_chunk != 0 && amount > io->write_chunk) {
        amount = io->write_chunk;
    }

    if (!io->handshake_ready) {
        char key[128];
        char accept[128];
        int response_size;

        if (amount > sizeof(io->handshake_request) -
                io->handshake_request_size) {
            apr_thread_mutex_unlock(io->mutex);
            return APR_ENOSPC;
        }
        memcpy(io->handshake_request + io->handshake_request_size,
               data,
               amount);
        io->handshake_request_size += amount;
        if (fake_request_complete(io->handshake_request,
                                  io->handshake_request_size)) {
            if (!fake_find_client_key(io->handshake_request,
                                      io->handshake_request_size,
                                      key,
                                      sizeof(key)) ||
                !funasr_ws_accept_compute(key, accept, sizeof(accept))) {
                apr_thread_mutex_unlock(io->mutex);
                return APR_EGENERAL;
            }
            response_size = snprintf(
                (char *)io->inbound,
                sizeof(io->inbound),
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: %s\r\n"
                "\r\n",
                accept);
            if (response_size <= 0 ||
                (apr_size_t)response_size >= sizeof(io->inbound)) {
                apr_thread_mutex_unlock(io->mutex);
                return APR_ENOSPC;
            }
            io->inbound_size = (apr_size_t)response_size;
            if (io->handshake_suffix_size >
                    sizeof(io->inbound) - io->inbound_size) {
                apr_thread_mutex_unlock(io->mutex);
                return APR_ENOSPC;
            }
            memcpy(io->inbound + io->inbound_size,
                   io->handshake_suffix,
                   io->handshake_suffix_size);
            io->inbound_size += io->handshake_suffix_size;
            io->inbound_offset = 0;
            io->handshake_ready = TRUE;
        }
    } else {
        if (amount > sizeof(io->outbound) - io->outbound_size) {
            apr_thread_mutex_unlock(io->mutex);
            return APR_ENOSPC;
        }
        memcpy(io->outbound + io->outbound_size, data, amount);
        io->outbound_size += amount;
    }
    apr_thread_mutex_unlock(io->mutex);
    return APR_SUCCESS;
}

static apr_status_t fake_io_read(
    void *obj,
    void *data,
    apr_size_t *size)
{
    fake_io_t *io = obj;
    apr_size_t available;
    apr_size_t amount;

    apr_thread_mutex_lock(io->mutex);
    available = io->inbound_size - io->inbound_offset;
    if (available == 0) {
        *size = 0;
        apr_thread_mutex_unlock(io->mutex);
        return APR_EAGAIN;
    }
    amount = *size;
    if (amount > available) {
        amount = available;
    }
    if (io->read_chunk != 0 && amount > io->read_chunk) {
        amount = io->read_chunk;
    }
    memcpy(data, io->inbound + io->inbound_offset, amount);
    io->inbound_offset += amount;
    *size = amount;
    apr_thread_mutex_unlock(io->mutex);
    return APR_SUCCESS;
}

static void fake_io_close(void *obj)
{
    fake_io_t *io = obj;

    apr_thread_mutex_lock(io->mutex);
    io->opened = FALSE;
    io->close_count++;
    apr_thread_mutex_unlock(io->mutex);
}

static apr_status_t fake_io_wake(void *obj)
{
    fake_io_t *io = obj;

    apr_thread_mutex_lock(io->mutex);
    io->wake_count++;
    apr_thread_mutex_unlock(io->mutex);
    return APR_SUCCESS;
}

static apt_bool_t collect_transport_event(
    void *obj,
    funasr_transport_event_t *event)
{
    event_collector_t *collector = obj;
    apt_bool_t reject;

    apr_thread_mutex_lock(collector->mutex);
    if (collector->sequence_size <
            (int)(sizeof(collector->sequence) /
                  sizeof(collector->sequence[0]))) {
        collector->sequence[collector->sequence_size++] = event->type;
    }
    switch (event->type) {
        case FUNASR_EVENT_INPUT_STARTED:
            collector->input_started++;
            break;
        case FUNASR_EVENT_FINAL_RESULT:
            collector->final_results++;
            if (event->text && event->text_size < sizeof(collector->final_text)) {
                memcpy(collector->final_text, event->text, event->text_size);
                collector->final_text[event->text_size] = '\0';
            }
            break;
        case FUNASR_EVENT_TRANSPORT_FAILED:
            collector->failures++;
            collector->last_failure = event->failure;
            break;
        case FUNASR_EVENT_GENERATION_DRAINED:
            collector->drained++;
            break;
        case FUNASR_EVENT_TRANSPORT_METRICS:
            collector->metrics++;
            collector->last_metrics = event->metrics;
            break;
        case FUNASR_EVENT_WORKER_CLOSED:
            collector->closed++;
            break;
    }
    reject = collector->reject_events;
    apr_thread_mutex_unlock(collector->mutex);
    if (!reject) {
        funasr_transport_event_destroy(event);
        return TRUE;
    }
    return FALSE;
}

static void fake_io_append_inbound(
    fake_io_t *io,
    const void *data,
    apr_size_t size)
{
    apr_thread_mutex_lock(io->mutex);
    CHECK_TRUE("fake inbound capacity",
               size <= sizeof(io->inbound) - io->inbound_size);
    if (size <= sizeof(io->inbound) - io->inbound_size) {
        memcpy(io->inbound + io->inbound_size, data, size);
        io->inbound_size += size;
    }
    apr_thread_mutex_unlock(io->mutex);
}

static apt_bool_t fake_io_wait_outbound(
    fake_io_t *io,
    apr_size_t minimum_size)
{
    int attempts;

    for (attempts = 0; attempts < 2000; ++attempts) {
        apr_size_t size;

        apr_thread_mutex_lock(io->mutex);
        size = io->outbound_size;
        apr_thread_mutex_unlock(io->mutex);
        if (size >= minimum_size) {
            return TRUE;
        }
        apr_sleep(1000);
    }
    return FALSE;
}

static apt_bool_t fake_io_wait_poll_write(fake_io_t *io, int minimum_count)
{
    int attempts;

    for (attempts = 0; attempts < 2000; ++attempts) {
        int count;

        apr_thread_mutex_lock(io->mutex);
        count = io->poll_want_write;
        apr_thread_mutex_unlock(io->mutex);
        if (count >= minimum_count) {
            return TRUE;
        }
        apr_sleep(1000);
    }
    return FALSE;
}

static apt_bool_t fake_io_wait_handshake(fake_io_t *io)
{
    int attempts;

    for (attempts = 0; attempts < 2000; ++attempts) {
        apt_bool_t ready;

        apr_thread_mutex_lock(io->mutex);
        ready = io->handshake_ready;
        apr_thread_mutex_unlock(io->mutex);
        if (ready) {
            return TRUE;
        }
        apr_sleep(1000);
    }
    return FALSE;
}

static apt_bool_t fake_io_wait_open_count(fake_io_t *io, int expected)
{
    int attempts;

    for (attempts = 0; attempts < 2000; ++attempts) {
        apt_bool_t ready;

        apr_thread_mutex_lock(io->mutex);
        ready = io->open_count >= expected && io->handshake_ready;
        apr_thread_mutex_unlock(io->mutex);
        if (ready) {
            return TRUE;
        }
        apr_sleep(1000);
    }
    return FALSE;
}

static void fake_io_set_stall(fake_io_t *io, apt_bool_t stalled)
{
    apr_thread_mutex_lock(io->mutex);
    io->stall_writes = stalled;
    apr_thread_mutex_unlock(io->mutex);
}

static apt_bool_t fake_io_decode_binary(
    fake_io_t *io,
    unsigned char *output,
    apr_size_t output_capacity,
    apr_size_t *output_size,
    int *empty_frames)
{
    unsigned char *wire;
    unsigned char *frame_storage;
    unsigned char *message_storage;
    apr_size_t wire_size;
    funasr_ws_decoder_t decoder;
    funasr_ws_event_t event;
    funasr_ws_status_e status;
    apt_bool_t ok;

    wire = malloc(sizeof(io->outbound));
    frame_storage = malloc(FUNASR_WS_FRAME_LIMIT + 14U);
    message_storage = malloc(FUNASR_WS_MESSAGE_LIMIT);
    if (!wire || !frame_storage || !message_storage) {
        free(wire);
        free(frame_storage);
        free(message_storage);
        return FALSE;
    }
    apr_thread_mutex_lock(io->mutex);
    wire_size = io->outbound_size;
    memcpy(wire, io->outbound, wire_size);
    apr_thread_mutex_unlock(io->mutex);

    *output_size = 0;
    *empty_frames = 0;
    ok = TRUE;
    funasr_ws_decoder_init(
        &decoder,
        frame_storage,
        FUNASR_WS_FRAME_LIMIT + 14U,
        message_storage,
        FUNASR_WS_MESSAGE_LIMIT,
        FUNASR_WS_FRAME_LIMIT,
        FUNASR_WS_MESSAGE_LIMIT);
    status = funasr_ws_decoder_feed(&decoder, wire, wire_size, &event);
    while (status == FUNASR_WS_EVENT_READY) {
        if (event.type == FUNASR_WS_EVENT_MESSAGE &&
            event.opcode == FUNASR_WS_OPCODE_BINARY) {
            if (event.size == 0) {
                ++(*empty_frames);
            } else if (event.size > output_capacity - *output_size) {
                ok = FALSE;
                break;
            } else {
                memcpy(output + *output_size, event.data, event.size);
                *output_size += event.size;
            }
        }
        status = funasr_ws_decoder_feed(&decoder, NULL, 0, &event);
    }
    if (status == FUNASR_WS_PROTOCOL_ERROR ||
        status == FUNASR_WS_LIMIT_EXCEEDED) {
        ok = FALSE;
    }
    free(wire);
    free(frame_storage);
    free(message_storage);
    return ok;
}

static int fake_io_count_opcode(fake_io_t *io, unsigned char opcode)
{
    unsigned char *wire;
    unsigned char *frame_storage;
    unsigned char *message_storage;
    apr_size_t wire_size;
    funasr_ws_decoder_t decoder;
    funasr_ws_event_t event;
    funasr_ws_status_e status;
    int count;

    wire = malloc(sizeof(io->outbound));
    frame_storage = malloc(FUNASR_WS_FRAME_LIMIT + 14U);
    message_storage = malloc(FUNASR_WS_MESSAGE_LIMIT);
    if (!wire || !frame_storage || !message_storage) {
        free(wire);
        free(frame_storage);
        free(message_storage);
        return -1;
    }
    apr_thread_mutex_lock(io->mutex);
    wire_size = io->outbound_size;
    memcpy(wire, io->outbound, wire_size);
    apr_thread_mutex_unlock(io->mutex);
    count = 0;
    funasr_ws_decoder_init(
        &decoder,
        frame_storage,
        FUNASR_WS_FRAME_LIMIT + 14U,
        message_storage,
        FUNASR_WS_MESSAGE_LIMIT,
        FUNASR_WS_FRAME_LIMIT,
        FUNASR_WS_MESSAGE_LIMIT);
    status = funasr_ws_decoder_feed(&decoder, wire, wire_size, &event);
    while (status == FUNASR_WS_EVENT_READY) {
        if (event.opcode == opcode) {
            ++count;
        }
        status = funasr_ws_decoder_feed(&decoder, NULL, 0, &event);
    }
    if (status == FUNASR_WS_PROTOCOL_ERROR ||
        status == FUNASR_WS_LIMIT_EXCEEDED) {
        count = -1;
    }
    free(wire);
    free(frame_storage);
    free(message_storage);
    return count;
}

static int collector_value(event_collector_t *collector, int which)
{
    int value;

    apr_thread_mutex_lock(collector->mutex);
    if (which == 0) {
        value = collector->final_results;
    } else if (which == 1) {
        value = collector->drained;
    } else if (which == 2) {
        value = collector->closed;
    } else if (which == 4) {
        value = collector->input_started;
    } else if (which == 5) {
        value = collector->metrics;
    } else {
        value = collector->failures;
    }
    apr_thread_mutex_unlock(collector->mutex);
    return value;
}

static apt_bool_t wait_for_collector(
    event_collector_t *collector,
    int which,
    int expected)
{
    int attempts;

    for (attempts = 0; attempts < 2000; ++attempts) {
        if (collector_value(collector, which) >= expected) {
            return TRUE;
        }
        apr_sleep(1000);
    }
    return FALSE;
}

static void test_worker_keeps_media_enqueue_independent_of_partial_rx(
    apr_pool_t *pool)
{
    static const funasr_transport_io_vtable_t fake_vtable = {
        fake_io_open,
        fake_io_poll,
        fake_io_read,
        fake_io_write,
        fake_io_close,
        fake_io_wake
    };
    funasr_transport_config_t config;
    funasr_audio_format_t format;
    funasr_transport_t *transport;
    funasr_media_snapshot_t snapshot;
    fake_io_t io;
    event_collector_t collector;
    apr_int64_t now_us = 1000000;
    unsigned char media_frame[640];
    unsigned char expected_media[50U * sizeof(media_frame)];
    unsigned char decoded_media[50U * sizeof(media_frame)];
    apr_size_t decoded_size;
    int empty_frames;
    unsigned char response_header[4];
    const char response_json[] = "{\"code\":0,\"text\":\"ok\"}";
    int index;

    memset(&io, 0, sizeof(io));
    memset(&collector, 0, sizeof(collector));
    CHECK_TRUE("fake I/O mutex",
               apr_thread_mutex_create(
                   &io.mutex,
                   APR_THREAD_MUTEX_DEFAULT,
                   pool) == APR_SUCCESS);
    CHECK_TRUE("collector mutex",
               apr_thread_mutex_create(
                   &collector.mutex,
                   APR_THREAD_MUTEX_DEFAULT,
                   pool) == APR_SUCCESS);
    io.read_chunk = 1;

    funasr_transport_config_init(&config);
    config.host = "127.0.0.1";
    config.port = 8888;
    config.path = "/ws/audio";
    config.clock.now_us = fake_now_us;
    config.clock.obj = &now_us;
    config.event_sink = collect_transport_event;
    config.event_sink_obj = &collector;
    config.io_vtable = &fake_vtable;
    config.io_obj = &io;

    transport = funasr_transport_create(pool, 42, &config);
    CHECK_TRUE("transport created", transport != NULL);
    memset(&format, 0, sizeof(format));
    format.input_sample_rate = 16000;
    format.output_sample_rate = 16000;
    format.channel_count = 1;
    format.sample_width = 2;
    format.call_id = "worker-test";
    CHECK_TRUE("generation begins",
               funasr_transport_begin_generation(
                   transport,
                   11,
                   &format) == TRUE);
    CHECK_TRUE("media snapshot published",
               funasr_transport_media_snapshot(
                   transport,
                   &snapshot) == TRUE);
    CHECK_TRUE("snapshot generation", snapshot.generation == 11);

    for (index = 0; index < 50; ++index) {
        int byte_index;

        for (byte_index = 0;
             byte_index < (int)sizeof(media_frame);
             ++byte_index) {
            media_frame[byte_index] =
                (unsigned char)(index + byte_index);
        }
        memcpy(expected_media + index * sizeof(media_frame),
               media_frame,
               sizeof(media_frame));
        CHECK_TRUE("20ms media enqueue accepted while RX is partial",
                   funasr_transport_enqueue_pcm(
                       transport,
                       11,
                       media_frame,
                       sizeof(media_frame),
                       now_us) == FUNASR_ENQUEUE_ACCEPTED);
        now_us += 20000;
    }

    CHECK_TRUE("worker completed handshake and observed input",
               wait_for_collector(&collector, 4, 1) == TRUE);
    CHECK_TRUE("worker sent all queued PCM",
               fake_io_wait_outbound(&io, sizeof(expected_media)) == TRUE);
    CHECK_TRUE("client frames decode",
               fake_io_decode_binary(
                   &io,
                   decoded_media,
                   sizeof(decoded_media),
                   &decoded_size,
                   &empty_frames) == TRUE);
    CHECK_SIZE("exact PCM byte count", decoded_size, sizeof(expected_media));
    CHECK_TRUE("exact PCM order",
               memcmp(decoded_media,
                      expected_media,
                      sizeof(expected_media)) == 0);
    CHECK_TRUE("no normal-stream end frame yet", empty_frames == 0);

    response_header[0] = 0x81;
    response_header[1] = 126;
    response_header[2] = 0;
    response_header[3] = (unsigned char)(sizeof(response_json) - 1);
    fake_io_append_inbound(&io, response_header, sizeof(response_header));
    now_us += 3000000;
    funasr_transport_wake(transport);
    fake_io_append_inbound(
        &io,
        response_json,
        sizeof(response_json) - 1);
    funasr_transport_wake(transport);

    CHECK_TRUE("final event arrives",
               wait_for_collector(&collector, 0, 1) == TRUE);
    CHECK_TRUE("metrics event arrives",
               wait_for_collector(&collector, 5, 1) == TRUE);
    CHECK_TRUE("one input-start event", collector.input_started == 1);
    CHECK_TRUE("final text preserved",
               strcmp(collector.final_text, "ok") == 0);
    CHECK_TRUE("no transport failure", collector.failures == 0);
    CHECK_TRUE("all media frames counted",
               collector.last_metrics.media_frames == 50);
    CHECK_TRUE("valid audio bytes counted",
               collector.last_metrics.valid_audio_bytes ==
                   sizeof(expected_media));
    CHECK_TRUE("websocket audio bytes counted after complete writes",
               collector.last_metrics.ws_audio_bytes ==
                   sizeof(expected_media));
    CHECK_TRUE("websocket audio frames counted after complete writes",
               collector.last_metrics.ws_audio_frames > 0);
    CHECK_TRUE("websocket audio send interval is non-negative",
               collector.last_metrics.ws_audio_gap_max_us >= 0);
    CHECK_TRUE("media gap p99 is 20ms",
               collector.last_metrics.media_gap_p99_us == 20000);
    CHECK_TRUE("successful metrics have the neutral completion reason",
               collector.last_metrics.completion_failure ==
                   FUNASR_FAILURE_NONE);

    format.call_id = "worker-test-next";
    CHECK_TRUE("next generation starts immediately after terminal event",
               funasr_transport_begin_generation(
                   transport,
                   12,
                   &format) == TRUE);
    memset(media_frame, 0x6c, sizeof(media_frame));
    for (index = 0; index < 10; ++index) {
        CHECK_TRUE("next generation media is accepted",
                   funasr_transport_enqueue_pcm(
                       transport,
                       12,
                       media_frame,
                       sizeof(media_frame),
                       now_us) == FUNASR_ENQUEUE_ACCEPTED);
        now_us += 20000;
    }
    CHECK_TRUE("next generation media uses existing socket",
               fake_io_wait_outbound(
                   &io,
                   sizeof(expected_media) + 10U * sizeof(media_frame)) == TRUE);
    response_header[0] = 0x81;
    response_header[1] = 126;
    response_header[2] = 0;
    response_header[3] = (unsigned char)(sizeof(response_json) - 1);
    fake_io_append_inbound(&io, response_header, sizeof(response_header));
    fake_io_append_inbound(
        &io,
        response_json,
        sizeof(response_json) - 1);
    funasr_transport_wake(transport);
    CHECK_TRUE("next generation final arrives",
               wait_for_collector(&collector, 0, 2) == TRUE);
    CHECK_TRUE("next generation metrics arrive",
               wait_for_collector(&collector, 5, 2) == TRUE);
    apr_thread_mutex_lock(io.mutex);
    CHECK_TRUE("two generations reuse one WebSocket open",
               io.open_count == 1);
    CHECK_TRUE("healthy WebSocket remains open between generations",
               io.close_count == 0 && io.opened == TRUE);
    apr_thread_mutex_unlock(io.mutex);

    CHECK_TRUE("close requested",
               funasr_transport_request_close(transport) == TRUE);
    CHECK_TRUE("close fence arrives",
               wait_for_collector(&collector, 2, 1) == TRUE);
    CHECK_TRUE("worker joins",
               funasr_transport_join_closed(transport) == APR_SUCCESS);
    apr_thread_mutex_lock(io.mutex);
    CHECK_TRUE("channel close closes the reused WebSocket once",
               io.close_count == 1 && io.opened == FALSE);
    apr_thread_mutex_unlock(io.mutex);
    CHECK_TRUE("each generation emits terminal metrics",
               collector.metrics >= 2);
    CHECK_TRUE("POLLOUT requested only for pending bytes",
               io.poll_want_write > 0 && io.poll_without_write > 0);
    CHECK_TRUE("enqueue wakes worker", io.wake_count > 0);
    CHECK_TRUE("WORKER_CLOSED is final",
               collector.sequence_size > 0 &&
               collector.sequence[collector.sequence_size - 1] ==
                   FUNASR_EVENT_WORKER_CLOSED);
}

static void test_worker_queues_pong_behind_pending_audio(
    apr_pool_t *pool)
{
    static const funasr_transport_io_vtable_t fake_vtable = {
        fake_io_open,
        fake_io_poll,
        fake_io_read,
        fake_io_write,
        fake_io_close,
        fake_io_wake
    };
    static const unsigned char ping_frames[] = {
        0x89, 0x01, 'q',
        0x89, 0x01, 'r'
    };
    static const char response_json[] = "{\"code\":0,\"text\":\"pong-ok\"}";
    unsigned char response_frame[2 + sizeof(response_json) - 1];
    unsigned char media_chunk[6400];
    funasr_transport_config_t config;
    funasr_audio_format_t format;
    funasr_transport_t *transport;
    fake_io_t io;
    event_collector_t collector;
    apr_int64_t now_us;

    memset(&io, 0, sizeof(io));
    memset(&collector, 0, sizeof(collector));
    apr_thread_mutex_create(&io.mutex, APR_THREAD_MUTEX_DEFAULT, pool);
    apr_thread_mutex_create(
        &collector.mutex,
        APR_THREAD_MUTEX_DEFAULT,
        pool);
    now_us = 3000000;
    funasr_transport_config_init(&config);
    config.host = "127.0.0.1";
    config.port = 8888;
    config.path = "/ws/audio";
    config.clock.now_us = fake_now_us;
    config.clock.obj = &now_us;
    config.event_sink = collect_transport_event;
    config.event_sink_obj = &collector;
    config.io_vtable = &fake_vtable;
    config.io_obj = &io;
    transport = funasr_transport_create(pool, 44, &config);

    memset(&format, 0, sizeof(format));
    format.input_sample_rate = 16000;
    format.output_sample_rate = 16000;
    format.channel_count = 1;
    format.sample_width = 2;
    format.call_id = "pong-test";
    CHECK_TRUE("Pong generation begins",
               funasr_transport_begin_generation(
                   transport,
                   31,
                   &format) == TRUE);
    CHECK_TRUE("Pong handshake completes",
               fake_io_wait_handshake(&io) == TRUE);
    fake_io_set_stall(&io, TRUE);
    memset(media_chunk, 0x4a, sizeof(media_chunk));
    CHECK_TRUE("Pong audio chunk enqueued",
               funasr_transport_enqueue_pcm(
                   transport,
                   31,
                   media_chunk,
                   sizeof(media_chunk),
                   now_us) == FUNASR_ENQUEUE_ACCEPTED);
    CHECK_TRUE("audio write becomes pending",
               fake_io_wait_poll_write(&io, 1) == TRUE);
    fake_io_append_inbound(&io, ping_frames, sizeof(ping_frames));
    funasr_transport_wake(transport);
    apr_sleep(10000);
    fake_io_set_stall(&io, FALSE);
    funasr_transport_wake(transport);
    CHECK_TRUE("audio and queued Pong are written",
               fake_io_wait_outbound(&io, 6415) == TRUE);
    CHECK_TRUE("latest pending Ping produces one Pong after audio",
               fake_io_count_opcode(&io, FUNASR_WS_OPCODE_PONG) == 1);

    response_frame[0] = 0x81;
    response_frame[1] = (unsigned char)(sizeof(response_json) - 1);
    memcpy(response_frame + 2, response_json, sizeof(response_json) - 1);
    fake_io_append_inbound(&io, response_frame, sizeof(response_frame));
    funasr_transport_wake(transport);
    CHECK_TRUE("Pong test final arrives",
               wait_for_collector(&collector, 0, 1) == TRUE);
    CHECK_TRUE("Pong test close requested",
               funasr_transport_request_close(transport) == TRUE);
    CHECK_TRUE("Pong test close fence arrives",
               wait_for_collector(&collector, 2, 1) == TRUE);
    CHECK_TRUE("Pong test worker joins",
               funasr_transport_join_closed(transport) == APR_SUCCESS);
}

static void test_worker_stop_drains_without_final(
    apr_pool_t *pool)
{
    static const funasr_transport_io_vtable_t fake_vtable = {
        fake_io_open,
        fake_io_poll,
        fake_io_read,
        fake_io_write,
        fake_io_close,
        fake_io_wake
    };
    funasr_transport_config_t config;
    funasr_audio_format_t format;
    funasr_transport_t *transport;
    funasr_media_snapshot_t snapshot;
    fake_io_t io;
    event_collector_t collector;
    apr_int64_t now_us = 2000000;
    unsigned char media_frame[640];
    unsigned char decoded_media[640];
    apr_size_t decoded_size;
    int empty_frames;

    memset(&io, 0, sizeof(io));
    memset(&collector, 0, sizeof(collector));
    apr_thread_mutex_create(
        &io.mutex,
        APR_THREAD_MUTEX_DEFAULT,
        pool);
    apr_thread_mutex_create(
        &collector.mutex,
        APR_THREAD_MUTEX_DEFAULT,
        pool);
    io.read_chunk = 7;

    funasr_transport_config_init(&config);
    config.host = "127.0.0.1";
    config.port = 8888;
    config.path = "/ws/audio";
    config.clock.now_us = fake_now_us;
    config.clock.obj = &now_us;
    config.event_sink = collect_transport_event;
    config.event_sink_obj = &collector;
    config.io_vtable = &fake_vtable;
    config.io_obj = &io;
    transport = funasr_transport_create(pool, 43, &config);

    memset(&format, 0, sizeof(format));
    format.input_sample_rate = 16000;
    format.output_sample_rate = 16000;
    format.channel_count = 1;
    format.sample_width = 2;
    format.call_id = "stop-test";
    CHECK_TRUE("STOP generation begins",
               funasr_transport_begin_generation(
                   transport,
                   21,
                   &format) == TRUE);
    memset(media_frame, 0x33, sizeof(media_frame));
    CHECK_TRUE("STOP tail enqueued",
               funasr_transport_enqueue_pcm(
                   transport,
                   21,
                   media_frame,
                   sizeof(media_frame),
                   now_us) == FUNASR_ENQUEUE_ACCEPTED);
    CHECK_TRUE("generation cancel requested",
               funasr_transport_cancel_generation(
                   transport,
                   21) == TRUE);
    CHECK_TRUE("cancel stops new media snapshots",
               funasr_transport_media_snapshot(
                   transport,
                   &snapshot) == FALSE);
    CHECK_TRUE("cancel rejects callback frames arriving after STOP",
               funasr_transport_enqueue_pcm(
                   transport,
                   21,
                   media_frame,
                   sizeof(media_frame),
                   now_us + 20000) == FUNASR_ENQUEUE_NOT_STREAMING);
    CHECK_TRUE("drain event arrives",
               wait_for_collector(&collector, 1, 1) == TRUE);
    CHECK_TRUE("STOP emits no final", collector.final_results == 0);
    CHECK_TRUE("STOP frames decode",
               fake_io_decode_binary(
                   &io,
                   decoded_media,
                   sizeof(decoded_media),
                   &decoded_size,
                   &empty_frames) == TRUE);
    CHECK_SIZE("STOP flushes tail", decoded_size, sizeof(media_frame));
    CHECK_TRUE("STOP preserves tail bytes",
               memcmp(decoded_media, media_frame, sizeof(media_frame)) == 0);
    CHECK_TRUE("STOP emits exactly one empty binary frame",
               empty_frames == 1);

    funasr_transport_request_close(transport);
    CHECK_TRUE("STOP worker closes",
               wait_for_collector(&collector, 2, 1) == TRUE);
    CHECK_TRUE("STOP worker joins",
    funasr_transport_join_closed(transport) == APR_SUCCESS);
}

static void test_worker_natural_endpoint_flushes_short_input(
    apr_pool_t *pool)
{
    static const funasr_transport_io_vtable_t fake_vtable = {
        fake_io_open,
        fake_io_poll,
        fake_io_read,
        fake_io_write,
        fake_io_close,
        fake_io_wake
    };
    static const char response_json[] = "{\"code\":0,\"text\":\"short\"}";
    unsigned char response_frame[2 + sizeof(response_json) - 1];
    unsigned char short_audio[100];
    unsigned char decoded_audio[100];
    funasr_transport_config_t config;
    funasr_audio_format_t format;
    funasr_transport_t *transport;
    fake_io_t io;
    event_collector_t collector;
    apr_int64_t now_us = 6000000;
    apr_size_t decoded_size;
    int empty_frames;

    memset(&io, 0, sizeof(io));
    memset(&collector, 0, sizeof(collector));
    apr_thread_mutex_create(&io.mutex, APR_THREAD_MUTEX_DEFAULT, pool);
    apr_thread_mutex_create(
        &collector.mutex,
        APR_THREAD_MUTEX_DEFAULT,
        pool);
    funasr_transport_config_init(&config);
    config.host = "127.0.0.1";
    config.port = 8888;
    config.path = "/ws/audio";
    config.clock.now_us = fake_now_us;
    config.clock.obj = &now_us;
    config.event_sink = collect_transport_event;
    config.event_sink_obj = &collector;
    config.io_vtable = &fake_vtable;
    config.io_obj = &io;
    transport = funasr_transport_create(pool, 46, &config);

    memset(&format, 0, sizeof(format));
    format.input_sample_rate = 16000;
    format.output_sample_rate = 16000;
    format.channel_count = 1;
    format.sample_width = 2;
    format.call_id = "natural-short-test";
    CHECK_TRUE("natural endpoint generation begins",
               funasr_transport_begin_generation(transport, 46, &format));
    memset(short_audio, 0x61, sizeof(short_audio));
    CHECK_TRUE("short input accepted",
               funasr_transport_enqueue_pcm(
                   transport,
                   46,
                   short_audio,
                   sizeof(short_audio),
                   now_us) == FUNASR_ENQUEUE_ACCEPTED);

    CHECK_TRUE("short input is observed",
               wait_for_collector(&collector, 4, 1));
    now_us += FUNASR_INPUT_IDLE_TIMEOUT_US;
    funasr_transport_wake(transport);
    CHECK_TRUE("short input reaches natural EOS",
               fake_io_wait_outbound(&io, sizeof(short_audio) + 4) == TRUE);
    CHECK_TRUE("short input emits audio and one EOS",
               fake_io_decode_binary(
                   &io,
                   decoded_audio,
                   sizeof(decoded_audio),
                   &decoded_size,
                   &empty_frames) == TRUE);
    CHECK_SIZE("short input is flushed", decoded_size, sizeof(short_audio));
    CHECK_TRUE("short input bytes are preserved",
               memcmp(decoded_audio, short_audio, sizeof(short_audio)) == 0);
    CHECK_TRUE("natural endpoint emits one EOS", empty_frames == 1);
    CHECK_TRUE("producer is stopped after natural EOS",
               funasr_transport_enqueue_pcm(
                   transport,
                   46,
                   short_audio,
                   sizeof(short_audio),
                   now_us) == FUNASR_ENQUEUE_NOT_STREAMING);

    response_frame[0] = 0x81;
    response_frame[1] = (unsigned char)(sizeof(response_json) - 1);
    memcpy(response_frame + 2, response_json, sizeof(response_json) - 1);
    fake_io_append_inbound(&io, response_frame, sizeof(response_frame));
    funasr_transport_wake(transport);
    CHECK_TRUE("natural endpoint final arrives",
               wait_for_collector(&collector, 0, 1));
    CHECK_TRUE("natural endpoint metrics arrive",
               wait_for_collector(&collector, 5, 1));
    CHECK_TRUE("successful natural endpoint has no failure",
               collector.last_metrics.completion_failure ==
                   FUNASR_FAILURE_NONE);

    funasr_transport_request_close(transport);
    CHECK_TRUE("natural endpoint worker closes",
               wait_for_collector(&collector, 2, 1));
    CHECK_TRUE("natural endpoint worker joins",
               funasr_transport_join_closed(transport) == APR_SUCCESS);
}

static void test_twenty_transport_workers_remain_independent(
    apr_pool_t *pool)
{
    static const funasr_transport_io_vtable_t fake_vtable = {
        fake_io_open,
        fake_io_poll,
        fake_io_read,
        fake_io_write,
        fake_io_close,
        fake_io_wake
    };
    static const char response_json[] = "{\"code\":0,\"text\":\"twenty-ok\"}";
    enum { TRANSPORT_COUNT = 20, FRAME_COUNT = 50 };
    funasr_transport_t **transports;
    fake_io_t *ios;
    event_collector_t *collectors;
    apr_int64_t *now_us;
    unsigned char response_frame[2 + sizeof(response_json) - 1];
    unsigned char media_frame[640];
    int index;
    int frame;

    transports = calloc(TRANSPORT_COUNT, sizeof(*transports));
    ios = calloc(TRANSPORT_COUNT, sizeof(*ios));
    collectors = calloc(TRANSPORT_COUNT, sizeof(*collectors));
    now_us = calloc(TRANSPORT_COUNT, sizeof(*now_us));
    CHECK_TRUE("20-worker allocations",
               transports && ios && collectors && now_us);
    if (!transports || !ios || !collectors || !now_us) {
        free(transports);
        free(ios);
        free(collectors);
        free(now_us);
        return;
    }
    response_frame[0] = 0x81;
    response_frame[1] = (unsigned char)(sizeof(response_json) - 1);
    memcpy(response_frame + 2, response_json, sizeof(response_json) - 1);
    memset(media_frame, 0x5b, sizeof(media_frame));

    for (index = 0; index < TRANSPORT_COUNT; ++index) {
        funasr_transport_config_t config;
        funasr_audio_format_t format;

        apr_thread_mutex_create(
            &ios[index].mutex,
            APR_THREAD_MUTEX_DEFAULT,
            pool);
        apr_thread_mutex_create(
            &collectors[index].mutex,
            APR_THREAD_MUTEX_DEFAULT,
            pool);
        now_us[index] = 4000000;
        funasr_transport_config_init(&config);
        config.host = "127.0.0.1";
        config.port = 8888;
        config.path = "/ws/audio";
        config.clock.now_us = fake_now_us;
        config.clock.obj = &now_us[index];
        config.event_sink = collect_transport_event;
        config.event_sink_obj = &collectors[index];
        config.io_vtable = &fake_vtable;
        config.io_obj = &ios[index];
        transports[index] = funasr_transport_create(
            pool,
            (funasr_transport_id_t)(100 + index),
            &config);
        memset(&format, 0, sizeof(format));
        format.input_sample_rate = 16000;
        format.output_sample_rate = 16000;
        format.channel_count = 1;
        format.sample_width = 2;
        format.call_id = "twenty-worker-test";
        CHECK_TRUE("20-worker generation begins",
                   transports[index] != NULL &&
                   funasr_transport_begin_generation(
                       transports[index],
                       (funasr_generation_t)(1000 + index),
                       &format) == TRUE);
    }
    for (frame = 0; frame < FRAME_COUNT; ++frame) {
        for (index = 0; index < TRANSPORT_COUNT; ++index) {
            CHECK_TRUE("20-worker media accepted",
                       funasr_transport_enqueue_pcm(
                           transports[index],
                           (funasr_generation_t)(1000 + index),
                           media_frame,
                           sizeof(media_frame),
                           now_us[index]) == FUNASR_ENQUEUE_ACCEPTED);
            now_us[index] += 20000;
        }
    }
    for (index = 0; index < TRANSPORT_COUNT; ++index) {
        CHECK_TRUE("20-worker exact audio sent",
                   fake_io_wait_outbound(
                       &ios[index],
                       FRAME_COUNT * sizeof(media_frame)) == TRUE);
        fake_io_append_inbound(
            &ios[index],
            response_frame,
            sizeof(response_frame));
        funasr_transport_wake(transports[index]);
    }
    for (index = 0; index < TRANSPORT_COUNT; ++index) {
        CHECK_TRUE("20-worker final arrives",
                   wait_for_collector(&collectors[index], 0, 1) == TRUE);
        CHECK_TRUE("20-worker metrics arrive",
                   wait_for_collector(&collectors[index], 5, 1) == TRUE);
        CHECK_TRUE("20-worker frame count",
                   collectors[index].last_metrics.media_frames == FRAME_COUNT);
        CHECK_TRUE("20-worker has no overrun",
                   collectors[index].last_metrics.tx_ring_overrun_events == 0);
        CHECK_TRUE("20-worker close requested",
                   funasr_transport_request_close(transports[index]) == TRUE);
    }
    for (index = 0; index < TRANSPORT_COUNT; ++index) {
        CHECK_TRUE("20-worker close fence",
                   wait_for_collector(&collectors[index], 2, 1) == TRUE);
        CHECK_TRUE("20-worker joins",
                   funasr_transport_join_closed(transports[index]) == APR_SUCCESS);
        CHECK_TRUE("20-worker emits progress and terminal metrics",
                   collectors[index].metrics == 2);
    }
    free(transports);
    free(ios);
    free(collectors);
    free(now_us);
}

static void test_worker_preserves_handshake_sticky_frame(apr_pool_t *pool)
{
    static const funasr_transport_io_vtable_t fake_vtable = {
        fake_io_open,
        fake_io_poll,
        fake_io_read,
        fake_io_write,
        fake_io_close,
        fake_io_wake
    };
    static const char response[] = "{\"code\":0,\"text\":\"sticky\"}";
    funasr_transport_config_t config;
    funasr_audio_format_t format;
    funasr_transport_t *transport;
    fake_io_t io;
    event_collector_t collector;
    apr_int64_t now_us = 2500000;

    memset(&io, 0, sizeof(io));
    memset(&collector, 0, sizeof(collector));
    apr_thread_mutex_create(&io.mutex, APR_THREAD_MUTEX_DEFAULT, pool);
    apr_thread_mutex_create(
        &collector.mutex,
        APR_THREAD_MUTEX_DEFAULT,
        pool);
    io.handshake_suffix[0] = 0x81;
    io.handshake_suffix[1] = (unsigned char)(sizeof(response) - 1U);
    memcpy(io.handshake_suffix + 2, response, sizeof(response) - 1U);
    io.handshake_suffix_size = sizeof(response) + 1U;

    funasr_transport_config_init(&config);
    config.host = "127.0.0.1";
    config.port = 8888;
    config.path = "/ws/audio";
    config.clock.now_us = fake_now_us;
    config.clock.obj = &now_us;
    config.event_sink = collect_transport_event;
    config.event_sink_obj = &collector;
    config.io_vtable = &fake_vtable;
    config.io_obj = &io;
    transport = funasr_transport_create(pool, 47, &config);
    memset(&format, 0, sizeof(format));
    format.input_sample_rate = 16000;
    format.output_sample_rate = 16000;
    format.channel_count = 1;
    format.sample_width = 2;
    format.call_id = "sticky-test";
    CHECK_TRUE("sticky generation begins",
               funasr_transport_begin_generation(transport, 61, &format));
    CHECK_TRUE("sticky final arrives",
               wait_for_collector(&collector, 0, 1));
    CHECK_TRUE("sticky final text preserved",
               strcmp(collector.final_text, "sticky") == 0);
    CHECK_TRUE("sticky frame has no protocol failure", collector.failures == 0);
    funasr_transport_request_close(transport);
    CHECK_TRUE("sticky worker closes",
               wait_for_collector(&collector, 2, 1));
    CHECK_TRUE("sticky worker joins",
               funasr_transport_join_closed(transport) == APR_SUCCESS);
}

static void test_worker_reads_data_when_poll_reports_eof_with_readable(
    apr_pool_t *pool)
{
    static const funasr_transport_io_vtable_t fake_vtable = {
        fake_io_open,
        fake_io_poll,
        fake_io_read,
        fake_io_write,
        fake_io_close,
        fake_io_wake
    };
    static const char response[] = "{\"code\":0,\"text\":\"hup\"}";
    funasr_transport_config_t config;
    funasr_audio_format_t format;
    funasr_transport_t *transport;
    fake_io_t io;
    event_collector_t collector;
    apr_int64_t now_us = 2600000;
    unsigned char frame[2 + sizeof(response) - 1];

    memset(&io, 0, sizeof(io));
    memset(&collector, 0, sizeof(collector));
    apr_thread_mutex_create(&io.mutex, APR_THREAD_MUTEX_DEFAULT, pool);
    apr_thread_mutex_create(
        &collector.mutex,
        APR_THREAD_MUTEX_DEFAULT,
        pool);
    funasr_transport_config_init(&config);
    config.host = "127.0.0.1";
    config.port = 8888;
    config.path = "/ws/audio";
    config.clock.now_us = fake_now_us;
    config.clock.obj = &now_us;
    config.event_sink = collect_transport_event;
    config.event_sink_obj = &collector;
    config.io_vtable = &fake_vtable;
    config.io_obj = &io;
    transport = funasr_transport_create(pool, 49, &config);
    memset(&format, 0, sizeof(format));
    format.input_sample_rate = 16000;
    format.output_sample_rate = 16000;
    format.channel_count = 1;
    format.sample_width = 2;
    format.call_id = "hup-test";

    CHECK_TRUE("HUP generation begins",
               funasr_transport_begin_generation(transport, 81, &format));
    CHECK_TRUE("HUP handshake completes", fake_io_wait_handshake(&io));
    frame[0] = 0x81;
    frame[1] = (unsigned char)(sizeof(response) - 1U);
    memcpy(frame + 2, response, sizeof(response) - 1U);
    fake_io_append_inbound(&io, frame, sizeof(frame));
    apr_thread_mutex_lock(io.mutex);
    io.eof_with_readable = TRUE;
    apr_thread_mutex_unlock(io.mutex);
    funasr_transport_wake(transport);

    CHECK_TRUE("data survives poll HUP", wait_for_collector(&collector, 0, 1));
    CHECK_TRUE("HUP final text preserved", strcmp(collector.final_text, "hup") == 0);
    CHECK_TRUE("HUP does not report transport failure", collector.failures == 0);
    apr_thread_mutex_lock(io.mutex);
    io.eof_with_readable = FALSE;
    apr_thread_mutex_unlock(io.mutex);
    format.call_id = "hup-test-next";
    CHECK_TRUE("generation after final HUP begins",
               funasr_transport_begin_generation(transport, 82, &format));
    CHECK_TRUE("final HUP forces next generation reconnect",
               fake_io_wait_open_count(&io, 2));
    CHECK_TRUE("generation after final HUP can stop",
               funasr_transport_cancel_generation(transport, 82));
    CHECK_TRUE("generation after final HUP drains",
               wait_for_collector(&collector, 1, 1));
    funasr_transport_request_close(transport);
    CHECK_TRUE("HUP worker closes", wait_for_collector(&collector, 2, 1));
    CHECK_TRUE("HUP worker joins", funasr_transport_join_closed(transport) == APR_SUCCESS);
}

static void test_worker_reconnects_before_late_idle_result(
    apr_pool_t *pool)
{
    static const funasr_transport_io_vtable_t fake_vtable = {
        fake_io_open,
        fake_io_poll,
        fake_io_read,
        fake_io_write,
        fake_io_close,
        fake_io_wake
    };
    static const char first_response[] = "{\"code\":0,\"text\":\"first\"}";
    static const char late_response[] = "{\"code\":0,\"text\":\"late\"}";
    funasr_transport_config_t config;
    funasr_audio_format_t format;
    funasr_transport_t *transport;
    fake_io_t io;
    event_collector_t collector;
    unsigned char frame[2 + sizeof(first_response) - 1];

    memset(&io, 0, sizeof(io));
    memset(&collector, 0, sizeof(collector));
    apr_thread_mutex_create(&io.mutex, APR_THREAD_MUTEX_DEFAULT, pool);
    apr_thread_mutex_create(
        &collector.mutex,
        APR_THREAD_MUTEX_DEFAULT,
        pool);
    funasr_transport_config_init(&config);
    config.host = "127.0.0.1";
    config.port = 8888;
    config.path = "/ws/audio";
    config.event_sink = collect_transport_event;
    config.event_sink_obj = &collector;
    config.io_vtable = &fake_vtable;
    config.io_obj = &io;
    transport = funasr_transport_create(pool, 53, &config);
    memset(&format, 0, sizeof(format));
    format.input_sample_rate = 16000;
    format.output_sample_rate = 16000;
    format.channel_count = 1;
    format.sample_width = 2;
    format.call_id = "late-result-test";

    CHECK_TRUE("late-result first generation begins",
               funasr_transport_begin_generation(transport, 101, &format));
    CHECK_TRUE("late-result handshake completes", fake_io_wait_handshake(&io));
    frame[0] = 0x81;
    frame[1] = (unsigned char)(sizeof(first_response) - 1U);
    memcpy(frame + 2, first_response, sizeof(first_response) - 1U);
    fake_io_append_inbound(&io, frame, sizeof(frame));
    funasr_transport_wake(transport);
    CHECK_TRUE("late-result first final arrives",
               wait_for_collector(&collector, 0, 1));

    frame[0] = 0x81;
    frame[1] = (unsigned char)(sizeof(late_response) - 1U);
    memcpy(frame + 2, late_response, sizeof(late_response) - 1U);
    fake_io_append_inbound(&io, frame, sizeof(frame));
    format.call_id = "late-result-test-next";
    CHECK_TRUE("generation after late idle result begins",
               funasr_transport_begin_generation(transport, 102, &format));
    CHECK_TRUE("late idle result forces reconnect",
               fake_io_wait_open_count(&io, 2));
    CHECK_TRUE("late idle result is not delivered to next generation",
               collector.final_results == 1 &&
               strcmp(collector.final_text, "first") == 0);
    CHECK_TRUE("late-result next generation can stop",
               funasr_transport_cancel_generation(transport, 102));
    CHECK_TRUE("late-result next generation drains",
               wait_for_collector(&collector, 1, 1));
    funasr_transport_request_close(transport);
    CHECK_TRUE("late-result worker closes",
               wait_for_collector(&collector, 2, 1));
    CHECK_TRUE("late-result worker joins",
               funasr_transport_join_closed(transport) == APR_SUCCESS);
}

static void test_worker_reconnects_after_final_with_trailing_close(
    apr_pool_t *pool)
{
    static const funasr_transport_io_vtable_t fake_vtable = {
        fake_io_open,
        fake_io_poll,
        fake_io_read,
        fake_io_write,
        fake_io_close,
        fake_io_wake
    };
    static const char response[] = "{\"code\":0,\"text\":\"final-close\"}";
    funasr_transport_config_t config;
    funasr_audio_format_t format;
    funasr_transport_t *transport;
    fake_io_t io;
    event_collector_t collector;
    unsigned char frames[2 + sizeof(response) - 1 + 2];

    memset(&io, 0, sizeof(io));
    memset(&collector, 0, sizeof(collector));
    apr_thread_mutex_create(&io.mutex, APR_THREAD_MUTEX_DEFAULT, pool);
    apr_thread_mutex_create(
        &collector.mutex,
        APR_THREAD_MUTEX_DEFAULT,
        pool);
    funasr_transport_config_init(&config);
    config.host = "127.0.0.1";
    config.port = 8888;
    config.path = "/ws/audio";
    config.event_sink = collect_transport_event;
    config.event_sink_obj = &collector;
    config.io_vtable = &fake_vtable;
    config.io_obj = &io;
    transport = funasr_transport_create(pool, 54, &config);
    memset(&format, 0, sizeof(format));
    format.input_sample_rate = 16000;
    format.output_sample_rate = 16000;
    format.channel_count = 1;
    format.sample_width = 2;
    format.call_id = "final-close-test";

    CHECK_TRUE("final-close generation begins",
               funasr_transport_begin_generation(transport, 111, &format));
    CHECK_TRUE("final-close handshake completes", fake_io_wait_handshake(&io));
    frames[0] = 0x81;
    frames[1] = (unsigned char)(sizeof(response) - 1U);
    memcpy(frames + 2, response, sizeof(response) - 1U);
    frames[sizeof(frames) - 2] = 0x88;
    frames[sizeof(frames) - 1] = 0;
    fake_io_append_inbound(&io, frames, sizeof(frames));
    funasr_transport_wake(transport);
    CHECK_TRUE("final before trailing close is delivered",
               wait_for_collector(&collector, 0, 1));

    format.call_id = "final-close-test-next";
    CHECK_TRUE("generation after trailing close begins",
               funasr_transport_begin_generation(transport, 112, &format));
    CHECK_TRUE("trailing close forces reconnect",
               fake_io_wait_open_count(&io, 2));
    CHECK_TRUE("generation after trailing close can stop",
               funasr_transport_cancel_generation(transport, 112));
    CHECK_TRUE("generation after trailing close drains",
               wait_for_collector(&collector, 1, 1));
    funasr_transport_request_close(transport);
    CHECK_TRUE("final-close worker closes",
               wait_for_collector(&collector, 2, 1));
    CHECK_TRUE("final-close worker joins",
               funasr_transport_join_closed(transport) == APR_SUCCESS);
}

static void test_worker_reports_one_queue_overrun(
    apr_pool_t *pool)
{
    static const funasr_transport_io_vtable_t fake_vtable = {
        fake_io_open,
        fake_io_poll,
        fake_io_read,
        fake_io_write,
        fake_io_close,
        fake_io_wake
    };
    funasr_transport_config_t config;
    funasr_audio_format_t format;
    funasr_transport_t *transport;
    fake_io_t io;
    event_collector_t collector;
    apr_int64_t now_us = 3000000;
    unsigned char chunk[6400];
    unsigned char frame[640];
    apt_bool_t queue_full;
    int index;

    memset(&io, 0, sizeof(io));
    memset(&collector, 0, sizeof(collector));
    apr_thread_mutex_create(&io.mutex, APR_THREAD_MUTEX_DEFAULT, pool);
    apr_thread_mutex_create(
        &collector.mutex,
        APR_THREAD_MUTEX_DEFAULT,
        pool);
    io.read_chunk = 8;
    funasr_transport_config_init(&config);
    config.host = "127.0.0.1";
    config.port = 8888;
    config.path = "/ws/audio";
    config.clock.now_us = fake_now_us;
    config.clock.obj = &now_us;
    config.event_sink = collect_transport_event;
    config.event_sink_obj = &collector;
    config.io_vtable = &fake_vtable;
    config.io_obj = &io;
    transport = funasr_transport_create(pool, 44, &config);

    memset(&format, 0, sizeof(format));
    format.input_sample_rate = 16000;
    format.output_sample_rate = 16000;
    format.channel_count = 1;
    format.sample_width = 2;
    format.call_id = "overrun-test";
    CHECK_TRUE("overrun generation begins",
               funasr_transport_begin_generation(transport, 31, &format));
    memset(chunk, 0x41, sizeof(chunk));
    CHECK_TRUE("initial chunk accepted",
               funasr_transport_enqueue_pcm(
                   transport,
                   31,
                   chunk,
                   sizeof(chunk),
                   now_us) == FUNASR_ENQUEUE_ACCEPTED);
    CHECK_TRUE("overrun worker observed input",
               wait_for_collector(&collector, 4, 1));
    apr_thread_mutex_lock(io.mutex);
    io.stall_writes = TRUE;
    apr_thread_mutex_unlock(io.mutex);

    memset(frame, 0x42, sizeof(frame));
    queue_full = FALSE;
    for (index = 0; index < 100; ++index) {
        funasr_enqueue_status_e status;

        status = funasr_transport_enqueue_pcm(
            transport,
            31,
            frame,
            sizeof(frame),
            now_us);
        now_us += 20000;
        if (status == FUNASR_ENQUEUE_QUEUE_FULL) {
            queue_full = TRUE;
        }
    }
    CHECK_TRUE("bounded ring reports queue full", queue_full);
    CHECK_TRUE("overrun failure arrives",
               wait_for_collector(&collector, 3, 1));
    CHECK_TRUE("overrun failure is unique", collector.failures == 1);
    CHECK_TRUE("overrun reason preserved",
               collector.last_failure == FUNASR_FAILURE_QUEUE_OVERRUN);

    funasr_transport_request_close(transport);
    CHECK_TRUE("overrun worker closes",
               wait_for_collector(&collector, 2, 1));
    CHECK_TRUE("overrun worker joins",
               funasr_transport_join_closed(transport) == APR_SUCCESS);
}

static void test_worker_write_stall_uses_fake_clock(
    apr_pool_t *pool)
{
    static const funasr_transport_io_vtable_t fake_vtable = {
        fake_io_open,
        fake_io_poll,
        fake_io_read,
        fake_io_write,
        fake_io_close,
        fake_io_wake
    };
    funasr_transport_config_t config;
    funasr_audio_format_t format;
    funasr_transport_t *transport;
    fake_io_t io;
    event_collector_t collector;
    apr_int64_t now_us = 4000000;
    unsigned char first[640];
    unsigned char remainder[5760];

    memset(&io, 0, sizeof(io));
    memset(&collector, 0, sizeof(collector));
    apr_thread_mutex_create(&io.mutex, APR_THREAD_MUTEX_DEFAULT, pool);
    apr_thread_mutex_create(
        &collector.mutex,
        APR_THREAD_MUTEX_DEFAULT,
        pool);
    funasr_transport_config_init(&config);
    config.host = "127.0.0.1";
    config.port = 8888;
    config.path = "/ws/audio";
    config.clock.now_us = fake_now_us;
    config.clock.obj = &now_us;
    config.event_sink = collect_transport_event;
    config.event_sink_obj = &collector;
    config.io_vtable = &fake_vtable;
    config.io_obj = &io;
    transport = funasr_transport_create(pool, 45, &config);

    memset(&format, 0, sizeof(format));
    format.input_sample_rate = 16000;
    format.output_sample_rate = 16000;
    format.channel_count = 1;
    format.sample_width = 2;
    format.call_id = "stall-test";
    CHECK_TRUE("stall generation begins",
               funasr_transport_begin_generation(transport, 41, &format));
    memset(first, 0x51, sizeof(first));
    CHECK_TRUE("stall first frame accepted",
               funasr_transport_enqueue_pcm(
                   transport,
                   41,
                   first,
                   sizeof(first),
                   now_us) == FUNASR_ENQUEUE_ACCEPTED);
    CHECK_TRUE("stall worker observed input",
               wait_for_collector(&collector, 4, 1));
    apr_thread_mutex_lock(io.mutex);
    io.stall_writes = TRUE;
    apr_thread_mutex_unlock(io.mutex);
    memset(remainder, 0x52, sizeof(remainder));
    CHECK_TRUE("stall chunk completed",
               funasr_transport_enqueue_pcm(
                   transport,
                   41,
                   remainder,
                   sizeof(remainder),
                   now_us) == FUNASR_ENQUEUE_ACCEPTED);
    CHECK_TRUE("worker is waiting for writable socket",
               fake_io_wait_poll_write(&io, 2));
    now_us += FUNASR_WRITE_STALL_TIMEOUT_US;
    funasr_transport_wake(transport);
    CHECK_TRUE("write-stall failure arrives",
               wait_for_collector(&collector, 3, 1));
    CHECK_TRUE("write-stall failure is unique", collector.failures == 1);
    CHECK_TRUE("write-stall reason preserved",
               collector.last_failure == FUNASR_FAILURE_WRITE_STALL);

    funasr_transport_request_close(transport);
    CHECK_TRUE("stall worker closes",
               wait_for_collector(&collector, 2, 1));
    CHECK_TRUE("stall worker joins",
               funasr_transport_join_closed(transport) == APR_SUCCESS);
}

static void test_worker_first_audio_timeout_ignores_continued_audio_and_reconnects(
    apr_pool_t *pool)
{
    static const funasr_transport_io_vtable_t fake_vtable = {
        fake_io_open,
        fake_io_poll,
        fake_io_read,
        fake_io_write,
        fake_io_close,
        fake_io_wake
    };
    funasr_transport_config_t config;
    funasr_audio_format_t format;
    funasr_transport_t *transport;
    fake_io_t io;
    event_collector_t collector;
    apr_int64_t now_us = 5000000;
    unsigned char audio[6400];

    memset(&io, 0, sizeof(io));
    memset(&collector, 0, sizeof(collector));
    apr_thread_mutex_create(&io.mutex, APR_THREAD_MUTEX_DEFAULT, pool);
    apr_thread_mutex_create(
        &collector.mutex,
        APR_THREAD_MUTEX_DEFAULT,
        pool);
    funasr_transport_config_init(&config);
    config.host = "127.0.0.1";
    config.port = 8888;
    config.path = "/ws/audio";
    config.first_audio_result_timeout_us = 20000;
    config.last_speech_result_timeout_us = 90000;
    config.clock.now_us = fake_now_us;
    config.clock.obj = &now_us;
    config.event_sink = collect_transport_event;
    config.event_sink_obj = &collector;
    config.io_vtable = &fake_vtable;
    config.io_obj = &io;
    transport = funasr_transport_create(pool, 52, &config);

    memset(&format, 0, sizeof(format));
    format.input_sample_rate = 16000;
    format.output_sample_rate = 16000;
    format.channel_count = 1;
    format.sample_width = 2;
    format.call_id = "timeout-test";
    CHECK_TRUE("timeout generation begins",
               funasr_transport_begin_generation(transport, 91, &format));
    memset(audio, 0x71, sizeof(audio));
    CHECK_TRUE("first-timeout audio accepted",
               funasr_transport_enqueue_pcm(
                   transport,
                   91,
                   audio,
                   sizeof(audio),
                   now_us) == FUNASR_ENQUEUE_ACCEPTED);
    CHECK_TRUE("first-timeout audio reaches socket",
               fake_io_wait_outbound(&io, sizeof(audio)));
    now_us += 10000;
    CHECK_TRUE("continued audio is accepted before first deadline",
               funasr_transport_enqueue_pcm(
                   transport,
                   91,
                   audio,
                   sizeof(audio),
                   now_us) == FUNASR_ENQUEUE_ACCEPTED);
    CHECK_TRUE("continued audio reaches socket",
               fake_io_wait_outbound(&io, 2U * sizeof(audio)));
    now_us += 10000;
    funasr_transport_wake(transport);
    CHECK_TRUE("first-audio timeout failure arrives",
               wait_for_collector(&collector, 3, 1));
    CHECK_TRUE("first-audio timeout reason preserved",
               collector.last_failure ==
                   FUNASR_FAILURE_FIRST_AUDIO_RESULT_TIMEOUT);
    apr_thread_mutex_lock(io.mutex);
    CHECK_TRUE("first-audio timeout closes WebSocket",
               io.open_count == 1 && io.close_count == 1 && !io.opened);
    apr_thread_mutex_unlock(io.mutex);

    format.call_id = "timeout-test-next";
    CHECK_TRUE("generation after timeout begins",
               funasr_transport_begin_generation(transport, 92, &format));
    CHECK_TRUE("generation after timeout reconnects",
               fake_io_wait_open_count(&io, 2));
    apr_thread_mutex_lock(io.mutex);
    CHECK_TRUE("timeout recovery opens a new WebSocket",
               io.open_count == 2 && io.opened);
    apr_thread_mutex_unlock(io.mutex);
    CHECK_TRUE("reconnected generation can stop",
               funasr_transport_cancel_generation(transport, 92));
    CHECK_TRUE("reconnected generation drains",
               wait_for_collector(&collector, 1, 1));
    funasr_transport_request_close(transport);
    CHECK_TRUE("timeout worker closes",
               wait_for_collector(&collector, 2, 1));
    CHECK_TRUE("timeout worker joins",
               funasr_transport_join_closed(transport) == APR_SUCCESS);
}

static void test_worker_last_speech_timeout_ignores_silence(
    apr_pool_t *pool)
{
    static const funasr_transport_io_vtable_t fake_vtable = {
        fake_io_open,
        fake_io_poll,
        fake_io_read,
        fake_io_write,
        fake_io_close,
        fake_io_wake
    };
    funasr_transport_config_t config;
    funasr_audio_format_t format;
    funasr_transport_t *transport;
    fake_io_t io;
    event_collector_t collector;
    apr_int64_t now_us = 5000000;
    unsigned char speech[6400];
    unsigned char silence[6400];

    memset(&io, 0, sizeof(io));
    memset(&collector, 0, sizeof(collector));
    apr_thread_mutex_create(&io.mutex, APR_THREAD_MUTEX_DEFAULT, pool);
    apr_thread_mutex_create(
        &collector.mutex,
        APR_THREAD_MUTEX_DEFAULT,
        pool);
    funasr_transport_config_init(&config);
    config.host = "127.0.0.1";
    config.port = 8888;
    config.path = "/ws/audio";
    config.first_audio_result_timeout_us = 90000;
    config.last_speech_result_timeout_us = 10000;
    config.clock.now_us = fake_now_us;
    config.clock.obj = &now_us;
    config.event_sink = collect_transport_event;
    config.event_sink_obj = &collector;
    config.io_vtable = &fake_vtable;
    config.io_obj = &io;
    transport = funasr_transport_create(pool, 53, &config);

    memset(&format, 0, sizeof(format));
    format.input_sample_rate = 16000;
    format.output_sample_rate = 16000;
    format.channel_count = 1;
    format.sample_width = 2;
    format.call_id = "last-speech-timeout-test";
    CHECK_TRUE("last-speech generation begins",
               funasr_transport_begin_generation(transport, 93, &format));
    memset(speech, 0x71, sizeof(speech));
    memset(silence, 0, sizeof(silence));
    CHECK_TRUE("speech before silence is accepted",
               funasr_transport_enqueue_pcm(
                   transport,
                   93,
                   speech,
                   sizeof(speech),
                   now_us) == FUNASR_ENQUEUE_ACCEPTED);
    CHECK_TRUE("speech reaches socket",
               fake_io_wait_outbound(&io, sizeof(speech)));
    now_us += 5000;
    CHECK_TRUE("silence after speech is accepted",
               funasr_transport_enqueue_pcm(
                   transport,
                   93,
                   silence,
                   sizeof(silence),
                   now_us) == FUNASR_ENQUEUE_ACCEPTED);
    CHECK_TRUE("silence reaches socket",
               fake_io_wait_outbound(&io, 2U * sizeof(speech)));
    now_us += 5000;
    funasr_transport_wake(transport);
    CHECK_TRUE("last-speech timeout failure arrives",
               wait_for_collector(&collector, 3, 1));
    CHECK_TRUE("silence does not reset last-speech deadline",
               collector.last_failure == FUNASR_FAILURE_NO_RESULT_TIMEOUT);

    funasr_transport_request_close(transport);
    CHECK_TRUE("last-speech timeout worker closes",
               wait_for_collector(&collector, 2, 1));
    CHECK_TRUE("last-speech timeout worker joins",
               funasr_transport_join_closed(transport) == APR_SUCCESS);
}

static void test_stop_interrupts_stalled_handshake(apr_pool_t *pool)
{
    static const funasr_transport_io_vtable_t fake_vtable = {
        fake_io_open,
        fake_io_poll,
        fake_io_read,
        fake_io_write,
        fake_io_close,
        fake_io_wake
    };
    funasr_transport_config_t config;
    funasr_audio_format_t format;
    funasr_transport_t *transport;
    fake_io_t io;
    event_collector_t collector;

    memset(&io, 0, sizeof(io));
    memset(&collector, 0, sizeof(collector));
    apr_thread_mutex_create(&io.mutex, APR_THREAD_MUTEX_DEFAULT, pool);
    apr_thread_mutex_create(
        &collector.mutex,
        APR_THREAD_MUTEX_DEFAULT,
        pool);
    io.stall_writes = TRUE;
    funasr_transport_config_init(&config);
    config.host = "127.0.0.1";
    config.port = 8888;
    config.path = "/ws/audio";
    config.stop_drain_timeout_us = 20000;
    config.event_sink = collect_transport_event;
    config.event_sink_obj = &collector;
    config.io_vtable = &fake_vtable;
    config.io_obj = &io;
    transport = funasr_transport_create(pool, 48, &config);

    memset(&format, 0, sizeof(format));
    format.input_sample_rate = 16000;
    format.output_sample_rate = 16000;
    format.channel_count = 1;
    format.sample_width = 2;
    format.call_id = "handshake-stop-test";
    CHECK_TRUE("handshake-stop generation begins",
               funasr_transport_begin_generation(transport, 71, &format));
    CHECK_TRUE("worker enters stalled handshake",
               fake_io_wait_poll_write(&io, 1));
    CHECK_TRUE("STOP accepted during handshake",
               funasr_transport_cancel_generation(transport, 71));
    CHECK_TRUE("stalled handshake drains promptly",
               wait_for_collector(&collector, 1, 1));
    CHECK_TRUE("handshake STOP emits metrics first",
               collector.sequence_size >= 2 &&
               collector.sequence[collector.sequence_size - 2] ==
                   FUNASR_EVENT_TRANSPORT_METRICS &&
               collector.sequence[collector.sequence_size - 1] ==
                   FUNASR_EVENT_GENERATION_DRAINED);
    CHECK_TRUE("handshake STOP is not a transport failure",
               collector.failures == 0);

    funasr_transport_request_close(transport);
    CHECK_TRUE("handshake-stop worker closes",
               wait_for_collector(&collector, 2, 1));
    CHECK_TRUE("handshake-stop worker joins",
               funasr_transport_join_closed(transport) == APR_SUCCESS);
}

static void test_worker_rejected_close_fence_is_reported(
    apr_pool_t *pool)
{
    static const funasr_transport_io_vtable_t fake_vtable = {
        fake_io_open,
        fake_io_poll,
        fake_io_read,
        fake_io_write,
        fake_io_close,
        fake_io_wake
    };
    funasr_transport_config_t config;
    funasr_audio_format_t format;
    funasr_transport_t *transport;
    fake_io_t io;
    event_collector_t collector;
    apr_int64_t now_us = 5000000;
    unsigned char frame[640];

    memset(&io, 0, sizeof(io));
    memset(&collector, 0, sizeof(collector));
    apr_thread_mutex_create(&io.mutex, APR_THREAD_MUTEX_DEFAULT, pool);
    apr_thread_mutex_create(
        &collector.mutex,
        APR_THREAD_MUTEX_DEFAULT,
        pool);
    funasr_transport_config_init(&config);
    config.host = "127.0.0.1";
    config.port = 8888;
    config.path = "/ws/audio";
    config.clock.now_us = fake_now_us;
    config.clock.obj = &now_us;
    config.event_sink = collect_transport_event;
    config.event_sink_obj = &collector;
    config.io_vtable = &fake_vtable;
    config.io_obj = &io;
    transport = funasr_transport_create(pool, 46, &config);

    memset(&format, 0, sizeof(format));
    format.input_sample_rate = 16000;
    format.output_sample_rate = 16000;
    format.channel_count = 1;
    format.sample_width = 2;
    format.call_id = "sink-reject-test";
    CHECK_TRUE("sink-reject generation begins",
               funasr_transport_begin_generation(transport, 51, &format));
    memset(frame, 0x61, sizeof(frame));
    CHECK_TRUE("sink-reject frame accepted",
               funasr_transport_enqueue_pcm(
                   transport,
                   51,
                   frame,
                   sizeof(frame),
                   now_us) == FUNASR_ENQUEUE_ACCEPTED);
    CHECK_TRUE("sink-reject worker observed input",
               wait_for_collector(&collector, 4, 1));
    apr_thread_mutex_lock(collector.mutex);
    collector.reject_events = TRUE;
    apr_thread_mutex_unlock(collector.mutex);
    funasr_transport_request_close(transport);
    CHECK_TRUE("rejected close fence attempted",
               wait_for_collector(&collector, 2, 1));
    CHECK_TRUE("rejected close fence makes join fail",
               funasr_transport_join_closed(transport) == APR_EGENERAL);
    CHECK_TRUE("rejected WORKER_CLOSED remains final attempt",
               collector.sequence_size > 0 &&
               collector.sequence[collector.sequence_size - 1] ==
                   FUNASR_EVENT_WORKER_CLOSED);
}

static void test_worker_close_without_generation_has_fence(apr_pool_t *pool)
{
    static const funasr_transport_io_vtable_t fake_vtable = {
        fake_io_open,
        fake_io_poll,
        fake_io_read,
        fake_io_write,
        fake_io_close,
        fake_io_wake
    };
    funasr_transport_config_t config;
    funasr_transport_t *transport;
    fake_io_t io;
    event_collector_t collector;

    memset(&io, 0, sizeof(io));
    memset(&collector, 0, sizeof(collector));
    apr_thread_mutex_create(&io.mutex, APR_THREAD_MUTEX_DEFAULT, pool);
    apr_thread_mutex_create(
        &collector.mutex,
        APR_THREAD_MUTEX_DEFAULT,
        pool);
    funasr_transport_config_init(&config);
    config.host = "127.0.0.1";
    config.port = 8888;
    config.path = "/ws/audio";
    config.event_sink = collect_transport_event;
    config.event_sink_obj = &collector;
    config.io_vtable = &fake_vtable;
    config.io_obj = &io;
    transport = funasr_transport_create(pool, 48, &config);
    CHECK_TRUE("idle transport created", transport != NULL);
    CHECK_TRUE("idle transport close requested",
               funasr_transport_request_close(transport));
    CHECK_TRUE("idle transport close fence arrives",
               wait_for_collector(&collector, 2, 1));
    CHECK_TRUE("idle transport joins",
               funasr_transport_join_closed(transport) == APR_SUCCESS);
    CHECK_TRUE("idle transport never opens socket", io.opened == FALSE);
    CHECK_TRUE("idle close does not invent generation metrics",
               collector.metrics == 0);
}

int main(void)
{
    apr_pool_t *pool = NULL;

    if (apr_initialize() != APR_SUCCESS) {
        fprintf(stderr, "FAIL apr_initialize\n");
        return 2;
    }
    atexit(apr_terminate);

    if (apr_pool_create(&pool, NULL) != APR_SUCCESS) {
        fprintf(stderr, "FAIL apr_pool_create\n");
        return 2;
    }

    test_fake_clock_contract();
    test_http_bytewise_and_sticky_frame();
    test_http_rejects_invalid_and_oversized_headers();
    test_http_header_names_and_tokens_are_case_insensitive();
    test_ws_short_read_and_mask();
    test_ws_extended_length_and_fragmented_control();
    test_ws_protocol_and_size_errors();
    test_ring_generation_wrap_and_overrun(pool);
    test_worker_keeps_media_enqueue_independent_of_partial_rx(pool);
    test_worker_queues_pong_behind_pending_audio(pool);
    test_worker_stop_drains_without_final(pool);
    test_worker_natural_endpoint_flushes_short_input(pool);
    test_twenty_transport_workers_remain_independent(pool);
    test_worker_preserves_handshake_sticky_frame(pool);
    test_worker_reads_data_when_poll_reports_eof_with_readable(pool);
    test_worker_reconnects_before_late_idle_result(pool);
    test_worker_reconnects_after_final_with_trailing_close(pool);
    test_worker_reports_one_queue_overrun(pool);
    test_worker_write_stall_uses_fake_clock(pool);
    test_worker_first_audio_timeout_ignores_continued_audio_and_reconnects(pool);
    test_worker_last_speech_timeout_ignores_silence(pool);
    test_stop_interrupts_stalled_handshake(pool);
    test_worker_rejected_close_fence_is_reported(pool);
    test_worker_close_without_generation_has_fence(pool);

    apr_pool_destroy(pool);
    if (failures != 0) {
        fprintf(stderr, "%d transport assertion(s) failed\n", failures);
        return 1;
    }

    printf("PASS test_funasr_ws_transport\n");
    return 0;
}
