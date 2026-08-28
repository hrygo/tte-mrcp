if(NOT DEFINED ASR_WEBSOCKET_SOURCE_DIR)
    message(FATAL_ERROR "ASR_WEBSOCKET_SOURCE_DIR is required")
endif()

file(READ
    "${ASR_WEBSOCKET_SOURCE_DIR}/src/funasr_ws_transport.c"
    transport_source)

foreach(required_text
        "ASR WebSocket handshake failed"
        "ASR WebSocket audio write failed"
        "ASR WebSocket result JSON"
        "FUNASR_LOG_JSON_MAX_BYTES")
    string(FIND "${transport_source}" "${required_text}" match_position)
    if(match_position EQUAL -1)
        message(FATAL_ERROR "Missing required ASR WebSocket log: ${required_text}")
    endif()
endforeach()

string(FIND "${transport_source}"
    "APT_PRIO_ERROR"
    error_priority_position)
if(error_priority_position EQUAL -1)
    message(FATAL_ERROR "ASR WebSocket failures must use ERROR priority")
endif()
