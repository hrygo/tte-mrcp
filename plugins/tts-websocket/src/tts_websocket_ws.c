#include "tts_websocket_ws.h"

#include <apr_errno.h>
#include <string.h>

static apt_bool_t tts_websocket_ws_utf8_valid(
	const unsigned char *data, apr_size_t size)
{
	apr_size_t i = 0;

	while(i < size) {
		unsigned char c = data[i];
		if(c <= 0x7F) {
			i++;
		} else if(c >= 0xC2 && c <= 0xDF) {
			if(i + 1 >= size || (data[i + 1] & 0xC0) != 0x80) {
				return FALSE;
			}
			i += 2;
		} else if(c == 0xE0) {
			if(i + 2 >= size || data[i + 1] < 0xA0 ||
				data[i + 1] > 0xBF || (data[i + 2] & 0xC0) != 0x80) {
				return FALSE;
			}
			i += 3;
		} else if((c >= 0xE1 && c <= 0xEC) ||
			(c >= 0xEE && c <= 0xEF)) {
			if(i + 2 >= size || (data[i + 1] & 0xC0) != 0x80 ||
				(data[i + 2] & 0xC0) != 0x80) {
				return FALSE;
			}
			i += 3;
		} else if(c == 0xED) {
			if(i + 2 >= size || data[i + 1] < 0x80 ||
				data[i + 1] > 0x9F || (data[i + 2] & 0xC0) != 0x80) {
				return FALSE;
			}
			i += 3;
		} else if(c == 0xF0) {
			if(i + 3 >= size || data[i + 1] < 0x90 ||
				data[i + 1] > 0xBF || (data[i + 2] & 0xC0) != 0x80 ||
				(data[i + 3] & 0xC0) != 0x80) {
				return FALSE;
			}
			i += 4;
		} else if(c >= 0xF1 && c <= 0xF3) {
			if(i + 3 >= size || (data[i + 1] & 0xC0) != 0x80 ||
				(data[i + 2] & 0xC0) != 0x80 ||
				(data[i + 3] & 0xC0) != 0x80) {
				return FALSE;
			}
			i += 4;
		} else if(c == 0xF4) {
			if(i + 3 >= size || data[i + 1] < 0x80 ||
				data[i + 1] > 0x8F || (data[i + 2] & 0xC0) != 0x80 ||
				(data[i + 3] & 0xC0) != 0x80) {
				return FALSE;
			}
			i += 4;
		} else {
			return FALSE;
		}
	}
	return TRUE;
}

static apt_bool_t tts_websocket_ws_close_payload_valid(
	const unsigned char *payload, apr_size_t payload_len)
{
	unsigned int status_code;

	if(payload_len == 0) {
		return TRUE;
	}
	/* RFC 6455: a non-empty Close payload must contain a 2-byte status code. */
	if(payload_len == 1 || !payload) {
		return FALSE;
	}
	status_code = ((unsigned int)payload[0] << 8) | payload[1];
	/* Accept normal, registered, and private-use codes only. */
	if(!(status_code == 1000 ||
		(status_code >= 1001 && status_code <= 1003) ||
		(status_code >= 1007 && status_code <= 1014) ||
		(status_code >= 3000 && status_code <= 4999))) {
		return FALSE;
	}
	return tts_websocket_ws_utf8_valid(payload + 2, payload_len - 2);
}

static apt_bool_t tts_websocket_ws_read_exact(
	tts_websocket_ws_decoder_t *decoder, unsigned char *buffer,
	apr_size_t size)
{
	apr_size_t offset = 0;

	if(!decoder || !decoder->read_fn || (!buffer && size > 0)) {
		return FALSE;
	}
	while(offset < size) {
		if(decoder->pending_pos < decoder->pending_len) {
			apr_size_t available = decoder->pending_len - decoder->pending_pos;
			apr_size_t copied = size - offset;
			if(copied > available) {
				copied = available;
			}
			memcpy(buffer + offset, decoder->pending + decoder->pending_pos, copied);
			decoder->pending_pos += copied;
			offset += copied;
			continue;
		}

		{
			apr_size_t received = size - offset;
			apr_status_t rv = decoder->read_fn(
				decoder->io_context, (char *)buffer + offset, &received);
			if(received > 0) {
				offset += received;
			}
			if(rv != APR_SUCCESS) {
				if(APR_STATUS_IS_EINTR(rv)) {
					continue;
				}
				decoder->last_status = rv;
				return FALSE;
			}
			if(received == 0) {
				decoder->last_status = APR_EOF;
				return FALSE;
			}
		}
	}
	return TRUE;
}

void tts_websocket_ws_decoder_init(
	tts_websocket_ws_decoder_t *decoder,
	tts_websocket_ws_read_fn read_fn,
	tts_websocket_ws_send_control_fn send_control_fn,
	void *io_context,
	apr_size_t max_message_size)
{
	if(!decoder) {
		return;
	}
	memset(decoder, 0, sizeof(*decoder));
	decoder->read_fn = read_fn;
	decoder->send_control_fn = send_control_fn;
	decoder->io_context = io_context;
	decoder->max_message_size = max_message_size;
}

apt_bool_t tts_websocket_ws_decoder_set_pending(
	tts_websocket_ws_decoder_t *decoder,
	const unsigned char *data,
	apr_size_t size)
{
	if(!decoder || (!data && size > 0) ||
		size > TTS_WEBSOCKET_WS_PENDING_CAPACITY) {
		return FALSE;
	}
	if(size > 0) {
		memcpy(decoder->pending, data, size);
	}
	decoder->pending_len = size;
	decoder->pending_pos = 0;
	return TRUE;
}

apr_ssize_t tts_websocket_ws_decoder_recv_message(
	tts_websocket_ws_decoder_t *decoder,
	char *buffer,
	apr_size_t buffer_size,
	apt_bool_t *is_text_frame)
{
	apr_size_t total = 0;
	unsigned char message_opcode = 0;

	if(!decoder || !buffer || buffer_size == 0 || !is_text_frame) {
		return -1;
	}
	/* Structural parse failures retain APR_EGENERAL; read failures replace it
	 * with the socket status so callers can identify an intentional timeout. */
	decoder->last_status = APR_EGENERAL;
	if(decoder->max_message_size == 0 ||
	   decoder->max_message_size > buffer_size) {
		decoder->max_message_size = buffer_size;
	}
	*is_text_frame = FALSE;
	for(;;) {
		unsigned char header[2];
		unsigned char ext[8];
		unsigned char mask[4] = {0};
		unsigned char opcode;
		apt_bool_t fin;
		apt_bool_t masked;
		apr_uint64_t payload_len = 0;
		apr_size_t ext_len = 0;
		apr_size_t offset;

		if(!tts_websocket_ws_read_exact(decoder, header, sizeof(header))) {
			return -1;
		}
		if(header[0] & 0x70) {
			return -1;
		}
		fin = (header[0] & 0x80) != 0;
		opcode = header[0] & 0x0F;
		masked = (header[1] & 0x80) != 0;
		payload_len = header[1] & 0x7F;
		if(payload_len == 126) {
			ext_len = 2;
		} else if(payload_len == 127) {
			ext_len = 8;
		}
		if(ext_len > 0 && !tts_websocket_ws_read_exact(decoder, ext, ext_len)) {
			return -1;
		}
		if(ext_len == 2) {
			payload_len = ((apr_uint64_t)ext[0] << 8) | ext[1];
		} else if(ext_len == 8) {
			apr_size_t i;
			payload_len = 0;
			for(i = 0; i < 8; ++i) {
				payload_len = (payload_len << 8) | ext[i];
			}
		}
		if(masked && !tts_websocket_ws_read_exact(decoder, mask, sizeof(mask))) {
			return -1;
		}

		if(opcode >= 0x8) {
			unsigned char control[125];
			if(!fin || payload_len > sizeof(control)) {
				return -1;
			}
			if(!tts_websocket_ws_read_exact(decoder, control, (apr_size_t)payload_len)) {
				return -1;
			}
			if(masked) {
				for(offset = 0; offset < (apr_size_t)payload_len; ++offset) {
					control[offset] ^= mask[offset % 4];
				}
			}
			if(opcode == 0x09) {
				if(!decoder->send_control_fn ||
				   !decoder->send_control_fn(decoder->io_context, 0x0A,
					control, (apr_size_t)payload_len)) {
					return -1;
				}
				continue;
			}
			if(opcode == 0x0A) {
				continue;
			}
			if(opcode == 0x08) {
				if(!tts_websocket_ws_close_payload_valid(
					control, (apr_size_t)payload_len)) {
					return -1;
				}
				decoder->close_received = TRUE;
				if(decoder->send_control_fn) {
					(void)decoder->send_control_fn(decoder->io_context, 0x08,
						control, (apr_size_t)payload_len);
				}
				return -1;
			}
			return -1;
		}

		if(opcode == 0x00) {
			if(!decoder->fragmented_message_open) {
				return -1;
			}
			opcode = message_opcode = decoder->fragmented_opcode;
		} else if(opcode == 0x01 || opcode == 0x02) {
			if(decoder->fragmented_message_open || total != 0) {
				return -1;
			}
			message_opcode = opcode;
		} else {
			return -1;
		}

		if(payload_len > buffer_size - total ||
		   payload_len > decoder->max_message_size - total) {
			return -1;
		}
		if(!tts_websocket_ws_read_exact(decoder,
			(unsigned char *)buffer + total, (apr_size_t)payload_len)) {
			return -1;
		}
		if(masked) {
			for(offset = 0; offset < (apr_size_t)payload_len; ++offset) {
				((unsigned char *)buffer)[total + offset] ^= mask[offset % 4];
			}
		}
		total += (apr_size_t)payload_len;

		if(!fin) {
			decoder->fragmented_message_open = TRUE;
			decoder->fragmented_opcode = message_opcode;
			continue;
		}
		decoder->fragmented_message_open = FALSE;
		decoder->fragmented_opcode = 0;
		*is_text_frame = message_opcode == 0x01 ? TRUE : FALSE;
		return (apr_ssize_t)total;
	}
}
