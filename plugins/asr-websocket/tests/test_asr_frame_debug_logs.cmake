if(NOT DEFINED ASR_WEBSOCKET_SOURCE_DIR)
    message(FATAL_ERROR "ASR_WEBSOCKET_SOURCE_DIR is required")
endif()

file(READ
    "${ASR_WEBSOCKET_SOURCE_DIR}/src/asr_websocket_engine.c"
    engine_source)
string(FIND "${engine_source}" "data = frame->codec_frame.buffer;" stream_start)
if(stream_start EQUAL -1)
    message(FATAL_ERROR "Could not find the client media stream entry function")
endif()
string(SUBSTRING "${engine_source}" ${stream_start} 3000 stream_write_source)
string(FIND "${stream_write_source}"
    "LOG_WITH_SID(channel, APT_PRIO_DEBUG," client_media_log_start)
if(client_media_log_start EQUAL -1)
    message(FATAL_ERROR
        "Client media per-frame log must use LOG_WITH_SID and APT_PRIO_DEBUG")
endif()
string(SUBSTRING "${stream_write_source}" ${client_media_log_start} 500
    client_media_log)
string(FIND "${client_media_log}" ");" client_media_log_end)
if(client_media_log_end EQUAL -1)
    message(FATAL_ERROR "Client media per-frame log call is incomplete")
endif()
string(SUBSTRING "${client_media_log}" 0 ${client_media_log_end}
    client_media_log)
if(NOT "${client_media_log}" MATCHES
        "received client media audio frame, size=%")
    message(FATAL_ERROR "Client media entry lacks per-frame DEBUG logging")
endif()
if(NOT "${client_media_log}" MATCHES "frame->codec_frame.size")
    message(FATAL_ERROR
        "Client media per-frame log must record frame->codec_frame.size")
endif()
string(FIND "${client_media_log}" "data" client_media_log_data)
string(FIND "${client_media_log}" "buffer" client_media_log_buffer)
if(NOT client_media_log_data EQUAL -1 OR
    NOT client_media_log_buffer EQUAL -1)
    message(FATAL_ERROR "Client media per-frame log must not record an audio payload pointer")
endif()
string(FIND "${stream_write_source}"
    "if (media.input_sample_rate == 8000)" resample_branch_start)
if(resample_branch_start EQUAL -1)
    message(FATAL_ERROR "Could not find the client media resampling branch")
endif()
if(client_media_log_start GREATER_EQUAL resample_branch_start)
    message(FATAL_ERROR "Client media per-frame log must precede the resampling branch")
endif()

file(READ
    "${ASR_WEBSOCKET_SOURCE_DIR}/src/funasr_ws_transport.c"
    transport_source)
string(FIND "${transport_source}" "static apr_status_t funasr_default_io_write(" write_start)
if(write_start EQUAL -1)
    message(FATAL_ERROR "Could not find the ASR WebSocket network send function")
endif()
string(SUBSTRING "${transport_source}" ${write_start} 1200 write_source)
if(NOT "${write_source}" MATCHES "status = apr_socket_send")
    message(FATAL_ERROR "Network send function must retain the apr_socket_send status")
endif()
if(NOT "${write_source}" MATCHES "status == APR_SUCCESS && \\*size != 0")
    message(FATAL_ERROR "Network send log must require a successful non-empty write")
endif()
string(FIND "${write_source}"
    "apt_log(APT_LOG_MARK, APT_PRIO_DEBUG," network_send_log_start)
if(network_send_log_start EQUAL -1)
    message(FATAL_ERROR "Could not find the ASR WebSocket network send log call")
endif()
string(SUBSTRING "${write_source}" ${network_send_log_start} 500
    network_send_log)
string(FIND "${network_send_log}" ");" network_send_log_end)
if(network_send_log_end EQUAL -1)
    message(FATAL_ERROR "ASR WebSocket network send log call is incomplete")
endif()
string(SUBSTRING "${network_send_log}" 0 ${network_send_log_end}
    network_send_log)
if(NOT "${network_send_log}" MATCHES
        "sent ASR WebSocket packet, size=%")
    message(FATAL_ERROR "ASR WebSocket send path lacks per-packet DEBUG logging")
endif()
if(NOT "${network_send_log}" MATCHES
        "asr_websocket: \\[session_id=%s\\] sent ASR WebSocket packet")
    message(FATAL_ERROR "ASR WebSocket network send log must include the session_id prefix")
endif()
if(NOT "${network_send_log}" MATCHES "\\*size")
    message(FATAL_ERROR "ASR WebSocket network send log must record *size")
endif()
string(FIND "${network_send_log}" "data" network_send_log_data)
string(FIND "${network_send_log}" "buffer" network_send_log_buffer)
if(NOT network_send_log_data EQUAL -1 OR
    NOT network_send_log_buffer EQUAL -1)
    message(FATAL_ERROR "ASR WebSocket network send log must not record a payload pointer")
endif()
string(FIND "${write_source}" "return status;" network_return_start)
if(network_return_start EQUAL -1)
    message(FATAL_ERROR "ASR WebSocket network send function must return status")
endif()
math(EXPR network_send_log_end_in_source
    "${network_send_log_start} + ${network_send_log_end}")
if(network_return_start LESS_EQUAL network_send_log_end_in_source)
    message(FATAL_ERROR "ASR WebSocket network send log must precede return status;")
endif()

file(READ
    "${ASR_WEBSOCKET_SOURCE_DIR}/src/funasr_ws_transport.h"
    transport_header_source)
if(NOT "${transport_source}" MATCHES
        "default_io\\.session_id = transport->call_id;")
    message(FATAL_ERROR "A generation start must synchronize session_id to the default socket I/O")
endif()
