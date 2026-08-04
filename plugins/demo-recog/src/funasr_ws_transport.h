#ifndef FUNASR_WS_TRANSPORT_H
#define FUNASR_WS_TRANSPORT_H

#include "apt.h"

#include <apr.h>
#include <apr_pools.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FUNASR_HTTP_HEADER_LIMIT (16U * 1024U)
#define FUNASR_WS_FRAME_LIMIT (1U * 1024U * 1024U)
#define FUNASR_WS_MESSAGE_LIMIT (1U * 1024U * 1024U)

typedef apr_uint64_t funasr_generation_t;

typedef enum funasr_enqueue_status_e {
    FUNASR_ENQUEUE_ACCEPTED,
    FUNASR_ENQUEUE_STALE_GENERATION,
    FUNASR_ENQUEUE_NOT_STREAMING,
    FUNASR_ENQUEUE_QUEUE_FULL,
    FUNASR_ENQUEUE_CLOSED
} funasr_enqueue_status_e;

typedef enum funasr_http_status_e {
    FUNASR_HTTP_NEED_MORE,
    FUNASR_HTTP_COMPLETE,
    FUNASR_HTTP_PROTOCOL_ERROR,
    FUNASR_HTTP_LIMIT_EXCEEDED
} funasr_http_status_e;

typedef struct funasr_http_decoder_t {
    unsigned char *storage;
    apr_size_t size;
    apr_size_t capacity;
    apt_bool_t complete;
    apt_bool_t failed;
} funasr_http_decoder_t;

void funasr_http_decoder_init(
    funasr_http_decoder_t *decoder,
    void *storage,
    apr_size_t capacity);

funasr_http_status_e funasr_http_decoder_feed(
    funasr_http_decoder_t *decoder,
    const void *data,
    apr_size_t size,
    apr_size_t *consumed);

typedef enum funasr_ws_opcode_e {
    FUNASR_WS_OPCODE_CONTINUATION = 0x0,
    FUNASR_WS_OPCODE_TEXT = 0x1,
    FUNASR_WS_OPCODE_BINARY = 0x2,
    FUNASR_WS_OPCODE_CLOSE = 0x8,
    FUNASR_WS_OPCODE_PING = 0x9,
    FUNASR_WS_OPCODE_PONG = 0xA
} funasr_ws_opcode_e;

typedef enum funasr_ws_event_type_e {
    FUNASR_WS_EVENT_NONE,
    FUNASR_WS_EVENT_MESSAGE,
    FUNASR_WS_EVENT_PING,
    FUNASR_WS_EVENT_PONG,
    FUNASR_WS_EVENT_CLOSE
} funasr_ws_event_type_e;

typedef struct funasr_ws_event_t {
    funasr_ws_event_type_e type;
    funasr_ws_opcode_e opcode;
    const unsigned char *data;
    apr_size_t size;
} funasr_ws_event_t;

typedef enum funasr_ws_status_e {
    FUNASR_WS_NEED_MORE,
    FUNASR_WS_EVENT_READY,
    FUNASR_WS_PROTOCOL_ERROR,
    FUNASR_WS_LIMIT_EXCEEDED
} funasr_ws_status_e;

typedef struct funasr_ws_decoder_t {
    unsigned char *frame_storage;
    apr_size_t frame_storage_size;
    apr_size_t frame_storage_capacity;
    unsigned char *message_storage;
    apr_size_t message_size;
    apr_size_t message_storage_capacity;
    unsigned char control_storage[125];
    apr_size_t frame_limit;
    apr_size_t message_limit;
    funasr_ws_opcode_e fragment_opcode;
    apt_bool_t fragment_active;
    apt_bool_t failed;
} funasr_ws_decoder_t;

void funasr_ws_decoder_init(
    funasr_ws_decoder_t *decoder,
    void *frame_storage,
    apr_size_t frame_storage_capacity,
    void *message_storage,
    apr_size_t message_storage_capacity,
    apr_size_t frame_limit,
    apr_size_t message_limit);

void funasr_ws_decoder_reset(funasr_ws_decoder_t *decoder);

funasr_ws_status_e funasr_ws_decoder_feed(
    funasr_ws_decoder_t *decoder,
    const void *data,
    apr_size_t size,
    funasr_ws_event_t *event);

typedef struct funasr_tx_ring_t funasr_tx_ring_t;

funasr_tx_ring_t *funasr_tx_ring_create(
    apr_pool_t *pool,
    apr_size_t capacity);

apt_bool_t funasr_tx_ring_begin(
    funasr_tx_ring_t *ring,
    funasr_generation_t generation);

funasr_enqueue_status_e funasr_tx_ring_enqueue(
    funasr_tx_ring_t *ring,
    funasr_generation_t generation,
    const void *data,
    apr_size_t size);

apt_bool_t funasr_tx_ring_dequeue(
    funasr_tx_ring_t *ring,
    void *output,
    apr_size_t *size);

apr_size_t funasr_tx_ring_size(funasr_tx_ring_t *ring);
apr_size_t funasr_tx_ring_overrun_bytes(funasr_tx_ring_t *ring);
apr_size_t funasr_tx_ring_overrun_events(funasr_tx_ring_t *ring);
void funasr_tx_ring_close(funasr_tx_ring_t *ring);

#ifdef __cplusplus
}
#endif

#endif
