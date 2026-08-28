#ifndef TTS_WEBSOCKET_WS_H
#define TTS_WEBSOCKET_WS_H

#include <apr.h>
#include <apr_errno.h>
#include <apt.h>

#define TTS_WEBSOCKET_WS_PENDING_CAPACITY 8192

typedef apr_status_t (*tts_websocket_ws_read_fn)(
	void *context, char *buffer, apr_size_t *size);
typedef apt_bool_t (*tts_websocket_ws_send_control_fn)(
	void *context, unsigned char opcode, const unsigned char *payload,
	apr_size_t payload_len);

typedef struct tts_websocket_ws_decoder_t {
	tts_websocket_ws_read_fn read_fn;
	tts_websocket_ws_send_control_fn send_control_fn;
	void *io_context;
	unsigned char pending[TTS_WEBSOCKET_WS_PENDING_CAPACITY];
	apr_size_t pending_len;
	apr_size_t pending_pos;
	apr_size_t max_message_size;
	apt_bool_t fragmented_message_open;
	unsigned char fragmented_opcode;
	/* Set when a valid peer Close control frame is decoded. */
	apt_bool_t close_received;
	/* Last receive failure, used to distinguish grace timeout from EOF/protocol errors. */
	apr_status_t last_status;
} tts_websocket_ws_decoder_t;

void tts_websocket_ws_decoder_init(
	tts_websocket_ws_decoder_t *decoder,
	tts_websocket_ws_read_fn read_fn,
	tts_websocket_ws_send_control_fn send_control_fn,
	void *io_context,
	apr_size_t max_message_size);
apt_bool_t tts_websocket_ws_decoder_set_pending(
	tts_websocket_ws_decoder_t *decoder,
	const unsigned char *data,
	apr_size_t size);
apr_ssize_t tts_websocket_ws_decoder_recv_message(
	tts_websocket_ws_decoder_t *decoder,
	char *buffer,
	apr_size_t buffer_size,
	apt_bool_t *is_text_frame);

#endif
