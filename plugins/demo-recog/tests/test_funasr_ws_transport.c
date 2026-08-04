#include "funasr_clock.h"
#include "funasr_ws_transport.h"

#include <apr_general.h>
#include <apr_pools.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    apr_pool_destroy(pool);
    if (failures != 0) {
        fprintf(stderr, "%d transport assertion(s) failed\n", failures);
        return 1;
    }

    printf("PASS test_funasr_ws_transport\n");
    return 0;
}
