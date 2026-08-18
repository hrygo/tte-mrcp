#ifndef FUNASR_WS_TRANSPORT_H
#define FUNASR_WS_TRANSPORT_H

#include "apt.h"
#include "funasr_clock.h"

#include <apr.h>
#include <apr_network_io.h>
#include <apr_pools.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FUNASR_HTTP_HEADER_LIMIT (16U * 1024U)
#define FUNASR_WS_FRAME_LIMIT (1U * 1024U * 1024U)
#define FUNASR_WS_MESSAGE_LIMIT (1U * 1024U * 1024U)

typedef apr_uint64_t funasr_generation_t;
typedef apr_uint64_t funasr_transport_id_t;

#define FUNASR_OUTPUT_SAMPLE_RATE          16000U
#define FUNASR_SAMPLE_WIDTH_BYTES              2U
#define FUNASR_MAX_CHANNELS                    2U
#define FUNASR_TX_RING_DURATION_MS          1000U
#define FUNASR_WS_CHUNK_DURATION_MS          200U
#define FUNASR_POLL_TIMEOUT_MS                20U
#define FUNASR_CONNECT_TIMEOUT_US        5000000LL
#define FUNASR_HANDSHAKE_TIMEOUT_US      5000000LL
#define FUNASR_WRITE_STALL_TIMEOUT_US    5000000LL
#define FUNASR_STOP_DRAIN_TIMEOUT_US     5000000LL
#define FUNASR_NO_RESULT_TIMEOUT_US     10000000LL
#define FUNASR_INPUT_IDLE_TIMEOUT_US     1000000LL
#define FUNASR_MEDIA_GAP_HISTOGRAM_BUCKETS 256U

#define FUNASR_IO_READABLE 0x01
#define FUNASR_IO_WRITABLE 0x02

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
    apt_bool_t reject_masked;
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
void funasr_ws_decoder_reject_masked(
    funasr_ws_decoder_t *decoder,
    apt_bool_t reject_masked);

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

typedef enum funasr_transport_event_type_e {
    FUNASR_EVENT_INPUT_STARTED,
    FUNASR_EVENT_FINAL_RESULT,
    FUNASR_EVENT_TRANSPORT_FAILED,
    FUNASR_EVENT_GENERATION_DRAINED,
    FUNASR_EVENT_TRANSPORT_METRICS,
    FUNASR_EVENT_WORKER_CLOSED
} funasr_transport_event_type_e;

typedef enum funasr_transport_failure_e {
    FUNASR_FAILURE_NONE,
    FUNASR_FAILURE_CONNECT,
    FUNASR_FAILURE_HANDSHAKE,
    FUNASR_FAILURE_PROTOCOL,
    FUNASR_FAILURE_QUEUE_OVERRUN,
    FUNASR_FAILURE_WRITE_STALL,
    FUNASR_FAILURE_NO_RESULT_TIMEOUT,
    FUNASR_FAILURE_EOF,
    FUNASR_FAILURE_INTERNAL
} funasr_transport_failure_e;

typedef struct funasr_transport_metrics_t {
    apr_uint64_t media_frames;
    apr_uint64_t valid_audio_bytes;
    apr_uint64_t media_gap_samples;
    apr_uint32_t media_gap_histogram[FUNASR_MEDIA_GAP_HISTOGRAM_BUCKETS];
    apr_int64_t media_gap_p99_us;
    apr_int64_t media_gap_max_us;
    apr_int64_t enqueue_max_us;
    apr_int64_t ws_first_send_ms;
    apr_int64_t ws_write_wait_max_ms;
    apr_uint64_t ws_audio_frames;
    apr_uint64_t ws_audio_bytes;
    apr_int64_t ws_audio_last_send_us;
    apr_int64_t ws_audio_gap_last_us;
    apr_int64_t ws_audio_gap_max_us;
    apr_size_t tx_ring_high_water_bytes;
    apr_size_t tx_ring_overrun_bytes;
    apr_size_t tx_ring_overrun_events;
    apr_uint64_t abnormal_closes;
    apr_uint64_t ws_rx_partial_reads;
    apr_uint64_t ws_rx_messages;
    funasr_transport_failure_e completion_failure;
} funasr_transport_metrics_t;

typedef struct funasr_transport_event_t {
    funasr_transport_id_t transport_id;
    funasr_generation_t generation;
    funasr_transport_event_type_e type;
    apr_byte_t close_fence_retry_count;
    funasr_transport_failure_e failure;
    funasr_transport_metrics_t metrics;
    char *text;
    apr_size_t text_size;
} funasr_transport_event_t;

typedef apt_bool_t (*funasr_transport_event_sink_f)(
    void *obj,
    funasr_transport_event_t *event);

typedef struct funasr_transport_io_vtable_t {
    apr_status_t (*open)(
        void *obj,
        const char *host,
        apr_port_t port,
        apr_interval_time_t timeout);
    apr_status_t (*poll)(
        void *obj,
        apr_interval_time_t timeout,
        apt_bool_t want_write,
        apr_int16_t *events);
    apr_status_t (*read)(void *obj, void *data, apr_size_t *size);
    apr_status_t (*write)(void *obj, const void *data, apr_size_t *size);
    void (*close)(void *obj);
    apr_status_t (*wake)(void *obj);
} funasr_transport_io_vtable_t;

typedef struct funasr_transport_config_t {
    const char *host;
    apr_port_t port;
    const char *path;
    apr_interval_time_t connect_timeout_us;
    apr_interval_time_t handshake_timeout_us;
    apr_interval_time_t write_stall_timeout_us;
    apr_interval_time_t stop_drain_timeout_us;
    apr_interval_time_t no_result_timeout_us;
    apr_interval_time_t input_idle_timeout_us;
    apr_interval_time_t poll_timeout_us;
    funasr_clock_t clock;
    funasr_transport_event_sink_f event_sink;
    void *event_sink_obj;
    const funasr_transport_io_vtable_t *io_vtable;
    void *io_obj;
} funasr_transport_config_t;

typedef struct funasr_audio_format_t {
    apr_uint32_t input_sample_rate;
    apr_uint32_t output_sample_rate;
    apr_uint16_t channel_count;
    apr_uint16_t sample_width;
    const char *call_id;
} funasr_audio_format_t;

typedef struct funasr_media_snapshot_t {
    funasr_generation_t generation;
    apr_uint32_t input_sample_rate;
    apr_uint16_t channel_count;
} funasr_media_snapshot_t;

typedef struct funasr_transport_t funasr_transport_t;

void funasr_transport_config_init(funasr_transport_config_t *config);

apt_bool_t funasr_ws_accept_compute(
    const char *client_key,
    char *output,
    apr_size_t output_capacity);

funasr_transport_t *funasr_transport_create(
    apr_pool_t *engine_pool,
    funasr_transport_id_t id,
    const funasr_transport_config_t *config);

apt_bool_t funasr_transport_begin_generation(
    funasr_transport_t *transport,
    funasr_generation_t generation,
    const funasr_audio_format_t *format);

apt_bool_t funasr_transport_media_snapshot(
    funasr_transport_t *transport,
    funasr_media_snapshot_t *snapshot);

funasr_enqueue_status_e funasr_transport_enqueue_pcm(
    funasr_transport_t *transport,
    funasr_generation_t generation,
    const void *data,
    apr_size_t size,
    apr_int64_t now_us);

apt_bool_t funasr_transport_latch_media_failure(
    funasr_transport_t *transport,
    funasr_generation_t generation,
    funasr_transport_failure_e failure);

apt_bool_t funasr_transport_cancel_generation(
    funasr_transport_t *transport,
    funasr_generation_t generation);

apt_bool_t funasr_transport_wake(funasr_transport_t *transport);
apt_bool_t funasr_transport_request_close(funasr_transport_t *transport);
apr_status_t funasr_transport_join_closed(funasr_transport_t *transport);
void funasr_transport_event_destroy(funasr_transport_event_t *event);

#ifdef __cplusplus
}
#endif

#endif
