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
	printf("%d/3 WebSocket decoder tests passed\n", passed);
	return passed == 3 ? 0 : 1;
}
