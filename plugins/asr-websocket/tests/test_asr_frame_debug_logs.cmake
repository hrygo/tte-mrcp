if(NOT DEFINED ASR_WEBSOCKET_SOURCE_DIR)
    message(FATAL_ERROR "缺少 ASR_WEBSOCKET_SOURCE_DIR")
endif()

file(READ
    "${ASR_WEBSOCKET_SOURCE_DIR}/src/asr_websocket_engine.c"
    engine_source)
string(FIND "${engine_source}" "data = frame->codec_frame.buffer;" stream_start)
if(stream_start EQUAL -1)
    message(FATAL_ERROR "未找到客户端媒体流入口函数")
endif()
string(SUBSTRING "${engine_source}" ${stream_start} 3000 stream_write_source)
string(FIND "${stream_write_source}"
    "LOG_WITH_SID(channel, APT_PRIO_DEBUG," client_media_log_start)
if(client_media_log_start EQUAL -1)
    message(FATAL_ERROR
        "客户端媒体流逐包日志必须使用 LOG_WITH_SID 和 APT_PRIO_DEBUG")
endif()
string(SUBSTRING "${stream_write_source}" ${client_media_log_start} 500
    client_media_log)
string(FIND "${client_media_log}" ");" client_media_log_end)
if(client_media_log_end EQUAL -1)
    message(FATAL_ERROR "客户端媒体流逐包日志调用不完整")
endif()
string(SUBSTRING "${client_media_log}" 0 ${client_media_log_end}
    client_media_log)
if(NOT "${client_media_log}" MATCHES
        "接收客户端媒体流音频数据包，大小=%")
    message(FATAL_ERROR "客户端媒体流入口缺少逐包 DEBUG 日志")
endif()
if(NOT "${client_media_log}" MATCHES "frame->codec_frame.size")
    message(FATAL_ERROR
        "客户端媒体流逐包日志必须记录 frame->codec_frame.size")
endif()
string(FIND "${client_media_log}" "data" client_media_log_data)
string(FIND "${client_media_log}" "buffer" client_media_log_buffer)
if(NOT client_media_log_data EQUAL -1 OR
        NOT client_media_log_buffer EQUAL -1)
    message(FATAL_ERROR "客户端媒体流逐包日志不得记录音频 payload 指针")
endif()
string(FIND "${stream_write_source}"
    "if (media.input_sample_rate == 8000)" resample_branch_start)
if(resample_branch_start EQUAL -1)
    message(FATAL_ERROR "未找到客户端媒体流重采样分支")
endif()
if(client_media_log_start GREATER_EQUAL resample_branch_start)
    message(FATAL_ERROR "客户端媒体流逐包日志必须位于重采样分支之前")
endif()

file(READ
    "${ASR_WEBSOCKET_SOURCE_DIR}/src/funasr_ws_transport.c"
    transport_source)
string(FIND "${transport_source}" "static apr_status_t funasr_default_io_write(" write_start)
if(write_start EQUAL -1)
    message(FATAL_ERROR "未找到 ASR WebSocket 网络发送函数")
endif()
string(SUBSTRING "${transport_source}" ${write_start} 1200 write_source)
if(NOT "${write_source}" MATCHES "status = apr_socket_send")
    message(FATAL_ERROR "网络发送函数未保存 apr_socket_send 返回状态")
endif()
if(NOT "${write_source}" MATCHES "status == APR_SUCCESS && \\*size != 0")
    message(FATAL_ERROR "网络发送日志没有限定成功且非空写入")
endif()
string(FIND "${write_source}"
    "apt_log(APT_LOG_MARK, APT_PRIO_DEBUG," network_send_log_start)
if(network_send_log_start EQUAL -1)
    message(FATAL_ERROR "未找到 ASR WebSocket 网络发送日志调用")
endif()
string(SUBSTRING "${write_source}" ${network_send_log_start} 500
    network_send_log)
string(FIND "${network_send_log}" ");" network_send_log_end)
if(network_send_log_end EQUAL -1)
    message(FATAL_ERROR "ASR WebSocket 网络发送日志调用不完整")
endif()
string(SUBSTRING "${network_send_log}" 0 ${network_send_log_end}
    network_send_log)
if(NOT "${network_send_log}" MATCHES
        "发送 ASR WebSocket 网络数据包，大小=%")
    message(FATAL_ERROR "ASR WebSocket 网络发送出口缺少逐包 DEBUG 日志")
endif()
if(NOT "${network_send_log}" MATCHES "\\*size")
    message(FATAL_ERROR "ASR WebSocket 网络发送日志必须记录 *size")
endif()
string(FIND "${network_send_log}" "data" network_send_log_data)
string(FIND "${network_send_log}" "buffer" network_send_log_buffer)
if(NOT network_send_log_data EQUAL -1 OR
        NOT network_send_log_buffer EQUAL -1)
    message(FATAL_ERROR "ASR WebSocket 网络发送日志不得记录 payload 指针")
endif()
string(FIND "${write_source}" "return status;" network_return_start)
if(network_return_start EQUAL -1)
    message(FATAL_ERROR "ASR WebSocket 网络发送函数未返回 status")
endif()
math(EXPR network_send_log_end_in_source
    "${network_send_log_start} + ${network_send_log_end}")
if(network_return_start LESS_EQUAL network_send_log_end_in_source)
    message(FATAL_ERROR "ASR WebSocket 网络发送日志必须位于 return status; 之前")
endif()
