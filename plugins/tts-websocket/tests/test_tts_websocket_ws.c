#include "tts_websocket_ws.h"

#include <apr_errno.h>
#include <string.h>
#include <stdio.h>

typedef struct test_reader_t {
	const unsigned char *data;
	apr_size_t size;
	apr_size_t pos;
	apr_size_t max_chunk;
} test_reader_t;

typedef struct test_sender_t {
	unsigned char opcode;
	unsigned char payload[125];
	apr_size_t payload_len;
	int calls;
} test_sender_t;

typedef struct test_connection_t {
	test_reader_t reader;
	test_sender_t sender;
} test_connection_t;

static apr_status_t test_read(void *context, char *buffer, apr_size_t *size)
{
	test_reader_t *reader = &((test_connection_t*)context)->reader;
	apr_size_t available;
	apr_size_t amount;

	if(reader->pos == reader->size) {
		*size = 0;
		return APR_EOF;
	}
	available = reader->size - reader->pos;
	amount = *size < available ? *size : available;
	if(reader->max_chunk > 0 && amount > reader->max_chunk) {
		amount = reader->max_chunk;
	}
	memcpy(buffer, reader->data + reader->pos, amount);
	reader->pos += amount;
	*size = amount;
	return APR_SUCCESS;
}

static apt_bool_t test_send(void *context, unsigned char opcode,
	const unsigned char *payload, apr_size_t payload_len)
{
	test_sender_t *sender = &((test_connection_t*)context)->sender;
	if(payload_len > sizeof(sender->payload)) {
		return FALSE;
	}
	sender->opcode = opcode;
	sender->payload_len = payload_len;
	if(payload_len > 0) {
		memcpy(sender->payload, payload, payload_len);
	}
	sender->calls++;
	return TRUE;
}

static int test_short_reads(void)
{
	static const unsigned char frame[] = {0x81, 0x05, 'h', 'e', 'l', 'l', 'o'};
	test_connection_t connection = {{frame, sizeof(frame), 0, 1}, {0, {0}, 0, 0}};
	tts_websocket_ws_decoder_t decoder;
	char message[16];
	apt_bool_t is_text = FALSE;
	apr_ssize_t length;

	tts_websocket_ws_decoder_init(&decoder, test_read, test_send, &connection, 15);
	length = tts_websocket_ws_decoder_recv_message(
		&decoder, message, sizeof(message), &is_text);
	return length == 5 && is_text && memcmp(message, "hello", 5) == 0;
}

static int test_handshake_surplus(void)
{
	static const unsigned char frame[] = {0x82, 0x03, 0x01, 0x02, 0x03};
	test_connection_t connection = {{NULL, 0, 0, 0}, {0, {0}, 0, 0}};
	tts_websocket_ws_decoder_t decoder;
	char message[8];
	apt_bool_t is_text = TRUE;
	apr_ssize_t length;

	tts_websocket_ws_decoder_init(&decoder, test_read, test_send, &connection, 7);
	if(!tts_websocket_ws_decoder_set_pending(&decoder, frame, sizeof(frame))) {
		return 0;
	}
	length = tts_websocket_ws_decoder_recv_message(
		&decoder, message, sizeof(message), &is_text);
	return length == 3 && !is_text &&
		memcmp(message, "\x01\x02\x03", 3) == 0;
}

static int test_fragmentation_and_ping(void)
{
	static const unsigned char frames[] = {
		0x01, 0x03, 'h', 'e', 'l',
		0x89, 0x01, 'x',
		0x80, 0x02, 'l', 'o'
	};
	test_connection_t connection = {{frames, sizeof(frames), 0, 1}, {0, {0}, 0, 0}};
	tts_websocket_ws_decoder_t decoder;
	char message[16];
	apt_bool_t is_text = FALSE;
	apr_ssize_t length;

	tts_websocket_ws_decoder_init(&decoder, test_read, test_send, &connection, 15);
	length = tts_websocket_ws_decoder_recv_message(
		&decoder, message, sizeof(message), &is_text);
	return length == 5 && is_text && memcmp(message, "hello", 5) == 0 &&
		connection.sender.calls == 1 && connection.sender.opcode == 0x0A &&
		connection.sender.payload_len == 1 && connection.sender.payload[0] == 'x';
}

static int test_close_is_reported(void)
{
	static const unsigned char frame[] = {0x88, 0x02, 0x03, 0xE8};
	test_connection_t connection = {{frame, sizeof(frame), 0, 1}, {0, {0}, 0, 0}};
	tts_websocket_ws_decoder_t decoder;
	char message[8];
	apt_bool_t is_text = TRUE;

	tts_websocket_ws_decoder_init(&decoder, test_read, test_send, &connection, 7);
	return tts_websocket_ws_decoder_recv_message(
		&decoder, message, sizeof(message), &is_text) < 0 &&
		decoder.close_received && connection.sender.calls == 1 &&
		connection.sender.opcode == 0x08 && connection.sender.payload_len == 2;
}

static int test_malformed_close_is_rejected(void)
{
	static const unsigned char short_frame[] = {0x88, 0x01, 0x03};
	static const unsigned char reserved_code_frame[] = {0x88, 0x02, 0x03, 0xED};
	static const unsigned char invalid_utf8_frame[] = {0x88, 0x03, 0x03, 0xE8, 0xFF};
	test_connection_t short_connection = {{short_frame, sizeof(short_frame), 0, 1}, {0, {0}, 0, 0}};
	test_connection_t reserved_connection = {{reserved_code_frame, sizeof(reserved_code_frame), 0, 1}, {0, {0}, 0, 0}};
	test_connection_t invalid_utf8_connection = {{invalid_utf8_frame, sizeof(invalid_utf8_frame), 0, 1}, {0, {0}, 0, 0}};
	tts_websocket_ws_decoder_t short_decoder;
	tts_websocket_ws_decoder_t reserved_decoder;
	tts_websocket_ws_decoder_t invalid_utf8_decoder;
	char message[8];
	apt_bool_t is_text = FALSE;

	tts_websocket_ws_decoder_init(&short_decoder, test_read, test_send,
		&short_connection, 7);
	tts_websocket_ws_decoder_init(&reserved_decoder, test_read, test_send,
		&reserved_connection, 7);
	tts_websocket_ws_decoder_init(&invalid_utf8_decoder, test_read, test_send,
		&invalid_utf8_connection, 7);
	if(tts_websocket_ws_decoder_recv_message(&short_decoder, message,
		sizeof(message), &is_text) >= 0 || short_decoder.close_received) {
		return 0;
	}
	if(tts_websocket_ws_decoder_recv_message(&reserved_decoder, message,
		sizeof(message), &is_text) >= 0 || reserved_decoder.close_received) {
		return 0;
	}
	return tts_websocket_ws_decoder_recv_message(&invalid_utf8_decoder,
		message, sizeof(message), &is_text) < 0 &&
		!invalid_utf8_decoder.close_received;
}

int main(void)
{
	int passed = 0;
	if(test_short_reads()) {
		passed++;
	} else {
		fprintf(stderr, "short-read frame test failed\n");
	}
	if(test_handshake_surplus()) {
		passed++;
	} else {
		fprintf(stderr, "handshake surplus test failed\n");
	}
	if(test_fragmentation_and_ping()) {
		passed++;
	} else {
		fprintf(stderr, "fragmentation/control-frame test failed\n");
	}
	if(test_close_is_reported()) {
		passed++;
	} else {
		fprintf(stderr, "peer Close reporting test failed\n");
	}
	if(test_malformed_close_is_rejected()) {
		passed++;
	} else {
		fprintf(stderr, "malformed Close validation test failed\n");
	}
	printf("%d/5 WebSocket decoder tests passed\n", passed);
	return passed == 5 ? 0 : 1;
}
