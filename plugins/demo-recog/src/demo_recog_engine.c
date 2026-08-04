#include "mrcp_recog_engine.h"
#include "mpf_activity_detector.h"
#include "apt_consumer_task.h"
#include "apt_log.h"
#include <apr_network_io.h>
#include <apr_errno.h>
#include <apr_uuid.h>
#include <apr_time.h>
#include <apr_base64.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#define RECOG_ENGINE_TASK_NAME "FunASR Recog Engine"
#define FUNASR_SERVER_HOST "40.20.85.37"
#define FUNASR_SERVER_PORT 8888
#define FUNASR_SERVER_PATH "/ws/audio"
#define FUNASR_MAX_AUDIO_SIZE (10 * 1024 * 1024)  /* 10MB */

/* Log macros with session_id */
#define LOG_WITH_SID(channel, prio, fmt, ...) \
    apt_log(APT_LOG_MARK, prio, "zyASR: [session_id=%s] " fmt, \
            (channel) && (channel)->session_id ? (channel)->session_id : "N/A", ##__VA_ARGS__)

/* Audio parameters for new ASR interface */
#define FUNASR_SAMPLE_RATE 16000
#define FUNASR_N_CHANNELS 1
#define FUNASR_SAMPLE_WIDTH 2

/* WebSocket send buffer size for 200ms chunks
 * 200ms at 16kHz/16bit/mono = 200ms * 16000 samples/s * 2 bytes/sample = 6400 bytes
 */
#define FUNASR_WS_SEND_CHUNK_SIZE (200 * FUNASR_SAMPLE_RATE * FUNASR_SAMPLE_WIDTH / 1000)  /* 6400 bytes */

typedef struct funasr_engine_t funasr_engine_t;
typedef struct funasr_channel_t funasr_channel_t;
typedef struct funasr_msg_t funasr_msg_t;

typedef enum {
    FUNASR_MSG_OPEN_CHANNEL,
    FUNASR_MSG_CLOSE_CHANNEL,
    FUNASR_MSG_REQUEST_PROCESS
} funasr_msg_type_e;

/* Declaration of recognizer engine methods */
static apt_bool_t funasr_engine_destroy(mrcp_engine_t *engine);
static apt_bool_t funasr_engine_open(mrcp_engine_t *engine);
static apt_bool_t funasr_engine_close(mrcp_engine_t *engine);
static mrcp_engine_channel_t* funasr_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool);

/* Declaration of recognizer channel methods */
static apt_bool_t funasr_channel_destroy(mrcp_engine_channel_t *channel);
static apt_bool_t funasr_channel_open(mrcp_engine_channel_t *channel);
static apt_bool_t funasr_channel_close(mrcp_engine_channel_t *channel);
static apt_bool_t funasr_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request);

/* Declaration of recognizer audio stream methods */
static apt_bool_t funasr_stream_destroy(mpf_audio_stream_t *stream);
static apt_bool_t funasr_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec);
static apt_bool_t funasr_stream_close(mpf_audio_stream_t *stream);
static apt_bool_t funasr_stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame);

/* Message handling */
static apt_bool_t funasr_msg_signal(funasr_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request);
static apt_bool_t funasr_msg_process(apt_task_t *task, apt_task_msg_t *msg);

/* WebSocket client functions */
static apt_bool_t funasr_websocket_connect(funasr_channel_t *channel);
static void funasr_websocket_disconnect(funasr_channel_t *channel);
static apt_bool_t funasr_websocket_send_frame(funasr_channel_t *channel, const char *data, apr_size_t size, int opcode);
static apt_bool_t funasr_websocket_send(funasr_channel_t *channel, const char *data, apr_size_t size);
static char* funasr_websocket_recv(funasr_channel_t *channel, apr_size_t *size);
static char* funasr_websocket_recv_nonblock(funasr_channel_t *channel, apr_size_t *size);
static void funasr_ws_send_flush(funasr_channel_t *recog_channel);
static void funasr_ws_send_buffered(funasr_channel_t *recog_channel, const char *data, apr_size_t size);
static apt_bool_t funasr_ws_send_end_frame(funasr_channel_t *recog_channel);
/* Resample 8k->16k helper forward declaration (exported for tests)
 * prev_samples[2] / state_valid: carryover state for cross-frame continuity.
 * On first call set *state_valid=FALSE; the function will set *state_valid=TRUE
 * and populate prev_samples with the last sample of each channel. */
char* funasr_resample_8k_to_16k(apr_pool_t *pool, const char *in_buf, apr_size_t in_size, apr_size_t *out_size, int channels,
                                 int16_t prev_samples[2], apt_bool_t *state_valid);

/* JSON helper functions */
static apt_bool_t funasr_json_get_int(const char *json, const char *key, int *value);
static char* funasr_json_get_string(apr_pool_t *pool, const char *json, const char *key);
static char* funasr_json_unescape_string(apr_pool_t *pool, const char *str);

/* Event functions */
static apt_bool_t funasr_start_of_input(funasr_channel_t *recog_channel);
static apt_bool_t funasr_recognition_complete(funasr_channel_t *recog_channel, mrcp_recog_completion_cause_e cause, const char *result);
static apt_bool_t funasr_send_intermediate_result(funasr_channel_t *recog_channel, const char *result);
static void funasr_process_realtime_response(funasr_channel_t *recog_channel);

static const mpf_audio_stream_vtable_t audio_stream_vtable = {
    funasr_stream_destroy,
    NULL,
    NULL,
    NULL,
    funasr_stream_open,
    funasr_stream_close,
    funasr_stream_write,
    NULL
};

/** Declaration of FunASR recognizer engine */
struct funasr_engine_t {
    apt_consumer_task_t *task;
    char *server_host;
    apr_port_t server_port;
    char *server_path;
};

/** Declaration of FunASR recognizer channel */
struct funasr_channel_t {
    funasr_engine_t *funasr_engine;
    mrcp_engine_channel_t *channel;
    mrcp_message_t *recog_request;
    mrcp_message_t *stop_response;
    apt_bool_t timers_started;
    mpf_activity_detector_t *detector;
    char *audio_buffer;
    apr_size_t audio_size;
    apr_size_t audio_capacity;
    /* WebSocket connection */
    apr_socket_t *ws_socket;
    apt_bool_t ws_connected;
    apr_pool_t *ws_pool;
    /* WebSocket send buffer for 200ms chunks (6400 bytes at 16kHz/16bit/mono) */
    char *ws_send_buffer;
    apr_size_t ws_send_size;
    /* Audio info logging flag */
    apt_bool_t audio_info_logged;
    /* Session ID for NLSML result */
    char *session_id;
    /* Final text collected during real-time processing */
    char *realtime_final_text;
    /* Actual audio format parameters (for WebSocket URL) */
    int actual_sample_rate;
    int actual_n_channels;
    int actual_sample_width;
    /* Timestamp of last valid WebSocket frame (for timeout detection) */
    apr_time_t last_valid_frame_time;
    /* Timestamp of request start (for total timeout detection) */
    apr_time_t request_start_time;
    /* Flag indicating current recognition session is complete */
    apt_bool_t recognition_complete;
    /* Timestamp of last audio frame sent (for debugging) */
    apr_time_t last_audio_send_time;
    /* Resample state for cross-frame continuity (prevents clicks at frame boundaries) */
    int16_t resample_prev_samples[2];  /* Last sample per channel from previous frame */
    apt_bool_t resample_state_valid;   /* TRUE when prev_samples contain valid data */
};

/** Declaration of FunASR recognizer task message */
struct funasr_msg_t {
    funasr_msg_type_e type;
    mrcp_engine_channel_t *channel;
    mrcp_message_t *request;
};

/* ---------- engine vtable ---------- */
static const mrcp_engine_method_vtable_t engine_vtable = {
    funasr_engine_destroy,
    funasr_engine_open,
    funasr_engine_close,
    funasr_engine_channel_create
};

/* ---------- channel vtable ---------- */
static const mrcp_engine_channel_method_vtable_t funasr_channel_vtable = {
    funasr_channel_destroy,
    funasr_channel_open,
    funasr_channel_close,
    funasr_channel_request_process
};

/* ---------- plugin entry ---------- */
/**
 * @brief 创建FunASR识别引擎插件
 * @param pool APR内存池，用于分配内存
 * @return 返回MRCP引擎对象，失败返回NULL
 * 说明：
 *   1. 初始化FunASR引擎结构体
 *   2. 创建消费者任务队列
 *   3. 设置消息处理函数
 *   4. 创建MRCP引擎对象并返回
 */
MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
{
    funasr_engine_t *funasr_engine = apr_palloc(pool, sizeof(funasr_engine_t));
    apt_task_t *task;
    apt_task_vtable_t *vtable;
    apt_task_msg_pool_t *msg_pool;

    msg_pool = apt_task_msg_pool_create_dynamic(sizeof(funasr_msg_t), pool);
    funasr_engine->task = apt_consumer_task_create(funasr_engine, msg_pool, pool);
    if (!funasr_engine->task) {
        return NULL;
    }
    task = apt_consumer_task_base_get(funasr_engine->task);
    apt_task_name_set(task, RECOG_ENGINE_TASK_NAME);
    vtable = apt_task_vtable_get(task);
    if (vtable) {
        vtable->process_msg = funasr_msg_process;
    }

    /* create engine base */
    return mrcp_engine_create(
        MRCP_RECOGNIZER_RESOURCE,  /* MRCP resource identifier */
        funasr_engine,             /* object to associate */
        &engine_vtable,             /* virtual methods table of engine */
        pool);                     /* pool to allocate memory from */
}

/* ---------- engine destroy ---------- */
/**
 * @brief 销毁FunASR识别引擎
 * @param engine MRCP引擎对象
 * @return 成功返回TRUE
 * 说明：
 *   1. 获取FunASR引擎对象
 *   2. 停止并销毁消费者任务
 *   3. 释放相关资源
 */
static apt_bool_t funasr_engine_destroy(mrcp_engine_t *engine)
{
    funasr_engine_t *funasr_engine = engine->obj;
    if (funasr_engine->task) {
        apt_task_t *task = apt_consumer_task_base_get(funasr_engine->task);
        apt_task_destroy(task);
        funasr_engine->task = NULL;
    }
    return TRUE;
}

/* ---------- engine open ---------- */
/**
 * @brief 打开FunASR识别引擎
 * @param engine MRCP引擎对象
 * @return 成功返回TRUE
 * 说明：
 *   1. 获取FunASR引擎对象
 *   2. 从配置文件读取服务器地址和端口
 *   3. 启动消费者任务线程
 *   4. 向MRCP核心发送打开响应
 */
static apt_bool_t funasr_engine_open(mrcp_engine_t *engine)
{
    funasr_engine_t *funasr_engine = engine->obj;
    const char *host;
    const char *port;
    const char *path;

    /* Read configuration from engine params */
    apt_log(APT_LOG_MARK, APT_PRIO_INFO, "zyASR: Start read engine config...");

    host = mrcp_engine_param_get(engine, "funasr-host");
    port = mrcp_engine_param_get(engine, "funasr-port");
    path = mrcp_engine_param_get(engine, "funasr-path");

    apt_log(APT_LOG_MARK, APT_PRIO_INFO,
            "zyASR: Config result\n"
            "      funasr-host: %s\n"
            "      funasr-port: %s\n"
            "      funasr-path: %s",
            host ? host : "(null)",
            port ? port : "(null)",
            path ? path : "(null)");

    /* Set default values if not configured */
    funasr_engine->server_host = host ? (char*)host : FUNASR_SERVER_HOST;
    funasr_engine->server_port = port ? (apr_port_t)atoi(port) : FUNASR_SERVER_PORT;
    funasr_engine->server_path = path ? (char*)path : FUNASR_SERVER_PATH;

    apt_log(APT_LOG_MARK, APT_PRIO_INFO,
            "zyASR: Config load OK\n"
            "      Server: %s\n"
            "      Port: %d\n"
            "      Path: %s",
            funasr_engine->server_host,
            funasr_engine->server_port,
            funasr_engine->server_path);

    if (funasr_engine->task) {
        apt_task_t *task = apt_consumer_task_base_get(funasr_engine->task);
        apt_task_start(task);
    }
    return mrcp_engine_open_respond(engine, TRUE);
}

/* ---------- engine close ---------- */
/**
 * @brief 关闭FunASR识别引擎
 * @param engine MRCP引擎对象
 * @return 成功返回TRUE
 * 说明：
 *   1. 获取FunASR引擎对象
 *   2. 终止消费者任务线程
 *   3. 向MRCP核心发送关闭响应
 */
static apt_bool_t funasr_engine_close(mrcp_engine_t *engine)
{
    funasr_engine_t *funasr_engine = engine->obj;
    if (funasr_engine->task) {
        apt_task_t *task = apt_consumer_task_base_get(funasr_engine->task);
        apt_task_terminate(task, TRUE);
    }
    return mrcp_engine_close_respond(engine);
}

/* ---------- channel create ---------- */
/**
 * @brief 创建FunASR识别通道
 * @param engine MRCP引擎对象
 * @param pool APR内存池，用于分配内存
 * @return 返回MRCP引擎通道对象
 * 说明：
 *   1. 分配FunASR通道结构体
 *   2. 初始化语音活动检测器
 *   3. 分配音频缓冲区（最大10MB）
 *   4. 设置支持8000Hz和16000Hz采样率、LPCM编解码器
 *   5. 创建音频流终止点
 *   6. 创建MRCP引擎通道对象
 */
static mrcp_engine_channel_t* funasr_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool)
{
    mpf_stream_capabilities_t *capabilities;
    mpf_termination_t *termination;
    apr_uuid_t uuid;

    /* create funasr channel */
    funasr_channel_t *recog_channel = apr_palloc(pool, sizeof(funasr_channel_t));
    recog_channel->funasr_engine = engine->obj;
    recog_channel->recog_request = NULL;
    recog_channel->stop_response = NULL;
    recog_channel->detector = mpf_activity_detector_create(pool);
    recog_channel->timers_started = TRUE;
    recog_channel->audio_buffer = apr_palloc(pool, FUNASR_MAX_AUDIO_SIZE);
    recog_channel->audio_size = 0;
    recog_channel->audio_capacity = FUNASR_MAX_AUDIO_SIZE;
    /* Initialize WebSocket */
    recog_channel->ws_socket = NULL;
    recog_channel->ws_connected = FALSE;
    recog_channel->ws_pool = pool;
    /* Initialize WebSocket send buffer */
    recog_channel->ws_send_buffer = apr_palloc(pool, FUNASR_WS_SEND_CHUNK_SIZE);
    recog_channel->ws_send_size = 0;
    /* Initialize audio info logging flag */
    recog_channel->audio_info_logged = FALSE;
    /* Initialize real-time processing fields */
    recog_channel->realtime_final_text = NULL;
    /* Initialize actual audio format parameters (will be set during RECOGNIZE) */
    recog_channel->actual_sample_rate = 0;
    recog_channel->actual_n_channels = 0;
    recog_channel->actual_sample_width = 0;
    /* Initialize recognition complete flag */
    recog_channel->recognition_complete = FALSE;
    /* Initialize resample state for cross-frame continuity */
    recog_channel->resample_state_valid = FALSE;
    memset(recog_channel->resample_prev_samples, 0, sizeof(recog_channel->resample_prev_samples));

    /* Generate fallback session ID using UUID (used until first RECOGNIZE request) */
    apr_uuid_get(&uuid);
    recog_channel->session_id = apr_palloc(pool, 64);
    apr_snprintf(recog_channel->session_id, 64,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        uuid.data[0], uuid.data[1], uuid.data[2], uuid.data[3],
        uuid.data[4], uuid.data[5], uuid.data[6], uuid.data[7],
        uuid.data[8], uuid.data[9], uuid.data[10], uuid.data[11],
        uuid.data[12], uuid.data[13], uuid.data[14], uuid.data[15]);

    capabilities = mpf_sink_stream_capabilities_create(pool);
    mpf_codec_capabilities_add(
        &capabilities->codecs,
        MPF_SAMPLE_RATE_8000 | MPF_SAMPLE_RATE_16000,
        "LPCM");

    /* create media termination */
    termination = mrcp_engine_audio_termination_create(
        recog_channel,        /* object to associate */
        &audio_stream_vtable, /* virtual methods table of audio stream */
        capabilities,         /* stream capabilities */
        pool);                /* pool to allocate memory from */

    /* create engine channel base */
    recog_channel->channel = mrcp_engine_channel_create(
        engine,               /* engine */
        &funasr_channel_vtable, /* virtual methods table of engine channel */
        recog_channel,        /* object to associate */
        termination,          /* associated media termination */
        pool);                /* pool to allocate memory from */

    return recog_channel->channel;
}

/**
 * @brief 销毁FunASR识别通道
 * @param channel MRCP引擎通道对象
 * @return 成功返回TRUE
 * 说明：
 *   1. 关闭 WebSocket 连接（如果仍然存在）
 *   2. 其他资源由APR池自动管理
 */
static apt_bool_t funasr_channel_destroy(mrcp_engine_channel_t *channel)
{
    /* Close WebSocket connection if still active */
    funasr_channel_t *recog_channel = channel->method_obj;
    if (recog_channel && recog_channel->ws_connected) {
        LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "Closing WebSocket connection in channel destroy");
        funasr_websocket_disconnect(recog_channel);
    }
    return TRUE;
}

/**
 * @brief 打开FunASR识别通道（异步）
 * @param channel MRCP引擎通道对象
 * @return 成功返回TRUE
 * 说明：
 *   1. 将OPEN_CHANNEL消息放入队列
 *   2. 在消费者任务线程中异步处理
 */
static apt_bool_t funasr_channel_open(mrcp_engine_channel_t *channel)
{
    return funasr_msg_signal(FUNASR_MSG_OPEN_CHANNEL, channel, NULL);
}

/**
 * @brief 关闭FunASR识别通道（异步）
 * @param channel MRCP引擎通道对象
 * @return 成功返回TRUE
 * 说明：
 *   1. 将CLOSE_CHANNEL消息放入队列
 *   2. 在消费者任务线程中异步处理
 */
static apt_bool_t funasr_channel_close(mrcp_engine_channel_t *channel)
{
    return funasr_msg_signal(FUNASR_MSG_CLOSE_CHANNEL, channel, NULL);
}

/**
 * @brief 处理MRCP通道请求（异步）
 * @param channel MRCP引擎通道对象
 * @param request MRCP消息对象
 * @return 成功返回TRUE
 * 说明：
 *   1. 将REQUEST_PROCESS消息放入队列
 *   2. 在消费者任务线程中异步分发处理
 */
static apt_bool_t funasr_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
    return funasr_msg_signal(FUNASR_MSG_REQUEST_PROCESS, channel, request);
}

/**
 * @brief 处理RECOGNIZE识别请求
 * @param channel MRCP引擎通道对象
 * @param request RECOGNIZE请求消息
 * @param response 响应消息对象
 * @return 成功返回TRUE，失败返回FALSE
 * 说明：
 *   1. 获取编解码器信息，失败则返回错误
 *   2. 记录日志：收到识别请求
 *   3. 重置音频缓冲区
 *   4. 解析请求头参数（超时设置等）
 *   5. 将请求状态设为INPROGRESS
 *   6. 发送异步响应
 *   7. 保存请求对象用于后续处理
 */
static apt_bool_t funasr_channel_recognize(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
    mrcp_recog_header_t *recog_header;
    funasr_channel_t *recog_channel = channel->method_obj;
    const mpf_codec_descriptor_t *descriptor = mrcp_engine_sink_stream_codec_get(channel);

    /* Log when ASR request is received */
    LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "Recv RECOGNIZE request engine=%p", channel->engine);

    /* Update session_id from MRCP Channel-Identifier to correlate with upstream systems */
    if (request->channel_id.session_id.buf && request->channel_id.session_id.length > 0) {
        char *mrcp_session_id = apr_pstrndup(request->pool,
            request->channel_id.session_id.buf, request->channel_id.session_id.length);
        LOG_WITH_SID(recog_channel, APT_PRIO_INFO,
            "Updating session_id from MRCP Channel-Identifier: %s -> %s",
            recog_channel->session_id, mrcp_session_id);
        recog_channel->session_id = mrcp_session_id;
    }

    if (!descriptor) {
        LOG_WITH_SID(recog_channel, APT_PRIO_WARNING, "Failed to Get Codec Descriptor");
        response->start_line.status_code = MRCP_STATUS_CODE_METHOD_FAILED;
        return FALSE;
    }

    /* Save actual audio format parameters for WebSocket URL */
    /* Always use 16kHz for ASR server (resample 8kHz input if needed) */
    recog_channel->actual_sample_rate = 16000;
    recog_channel->actual_n_channels = descriptor->channel_count > 0 ? descriptor->channel_count : 1;
    recog_channel->actual_sample_width = 2;  /* 16-bit = 2 bytes */

    LOG_WITH_SID(recog_channel, APT_PRIO_INFO,
            "Audio format for WebSocket - sample_rate=%d Hz, channels=%d, sample_width=%d bytes",
            recog_channel->actual_sample_rate,
            recog_channel->actual_n_channels,
            recog_channel->actual_sample_width);

    recog_channel->timers_started = TRUE;
    recog_channel->audio_size = 0;
    recog_channel->audio_info_logged = FALSE;
    recog_channel->realtime_final_text = NULL;
    recog_channel->ws_send_size = 0;  /* Reset WebSocket send buffer */
    recog_channel->recognition_complete = FALSE;  /* Reset recognition complete flag */
    recog_channel->resample_state_valid = FALSE;  /* Reset resample state for new session */

    /* Reset timestamps for new recognition session */
    recog_channel->request_start_time = apr_time_now();
    recog_channel->last_valid_frame_time = apr_time_now();
    recog_channel->last_audio_send_time = 0;  /* No audio sent yet */
    LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "Recognition session timestamps reset");

    /* Reset activity detector for new recognition session */
    mpf_activity_detector_reset(recog_channel->detector);

    /* 设置服务端默认 VAD 参数（客户端参数可覆盖这些默认值）
     * VAD（语音活动检测）参数说明：
     * - level_threshold: 能量阈值（0-255），音频能量低于此值认为是静音
     *   - 值越低：越敏感，容易把噪声误判为语音
     *   - 值越高：越不敏感，需要更大音量才能检测到语音
     *   - 框架默认值是2，电话场景音频能量较低，使用默认值2
     * - speech_timeout: 语音开始确认时间（毫秒），需要持续超过阈值这么久才确认语音开始
     * - silence_timeout: 静音确认时间（毫秒），需要持续低于阈值这么久才确认语音结束
     */
    mpf_activity_detector_level_set(recog_channel->detector, 2);         /* 能量阈值: 2 (范围0-255)，电话场景音频能量较低 */
    mpf_activity_detector_speech_timeout_set(recog_channel->detector, 200);   /* 语音开始确认: 200ms，需要持续超过阈值这么久才确认语音开始 */
    mpf_activity_detector_silence_timeout_set(recog_channel->detector, 1000); /* 静音确认: 1000ms，语音结束后1秒无声音则结束识别 */
    LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "VAD defaults set - level=2, speech_timeout=200ms, silence_timeout=1000ms");

    /* get recognizer header */
    recog_header = mrcp_resource_header_get(request);
    if (recog_header) {
        if (mrcp_resource_header_property_check(request, RECOGNIZER_HEADER_START_INPUT_TIMERS) == TRUE) {
            recog_channel->timers_started = recog_header->start_input_timers;
        }
        if (mrcp_resource_header_property_check(request, RECOGNIZER_HEADER_NO_INPUT_TIMEOUT) == TRUE) {
            mpf_activity_detector_noinput_timeout_set(recog_channel->detector, recog_header->no_input_timeout);
        }
        if (mrcp_resource_header_property_check(request, RECOGNIZER_HEADER_SPEECH_COMPLETE_TIMEOUT) == TRUE) {
            mpf_activity_detector_silence_timeout_set(recog_channel->detector, recog_header->speech_complete_timeout);
        }
        /* Set VAD level threshold from Sensitivity-Level (0.0-1.0 maps to 0-255) */
        if (mrcp_resource_header_property_check(request, RECOGNIZER_HEADER_SENSITIVITY_LEVEL) == TRUE) {
            /* sensitivity_level: 0.0 = least sensitive (high threshold), 1.0 = most sensitive (low threshold)
             * We invert it: higher sensitivity -> lower threshold */
            apr_size_t level_threshold = (apr_size_t)((1.0 - recog_header->sensitivity_level) * 255);
            if (level_threshold > 255) level_threshold = 255;
            mpf_activity_detector_level_set(recog_channel->detector, level_threshold);
            LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "VAD level_threshold set to %d (sensitivity_level=%.2f)",
                    (int)level_threshold, recog_header->sensitivity_level);
        }
        /* Set speech timeout - time required to confirm speech start */
        if (mrcp_resource_header_property_check(request, RECOGNIZER_HEADER_SPEECH_INCOMPLETE_TIMEOUT) == TRUE) {
            mpf_activity_detector_speech_timeout_set(recog_channel->detector, recog_header->speech_incomplete_timeout);
            LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "VAD speech_timeout set to %d ms",
                    (int)recog_header->speech_incomplete_timeout);
        }
    }

    response->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
    /* send asynchronous response */
    mrcp_engine_channel_message_send(channel, response);
    recog_channel->recog_request = request;
    return TRUE;
}

/**
 * @brief 处理STOP停止请求
 * @param channel MRCP引擎通道对象
 * @param request STOP请求消息
 * @param response 响应消息对象
 * @return 成功返回TRUE
 * 说明：
 *   1. 记录日志：收到停止请求
 *   2. 保存停止响应对象
 *   3. 在stream_write中检测到停止响应时发送
 */
static apt_bool_t funasr_channel_stop(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
    funasr_channel_t *recog_channel = channel->method_obj;

    /* Log when STOP request is received */
    LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "Recv STOP request");

    recog_channel->stop_response = response;
    return TRUE;
}

/**
 * @brief 处理START-INPUT-TIMERS启动输入定时器请求
 * @param channel MRCP引擎通道对象
 * @param request START-INPUT-TIMERS请求消息
 * @param response 响应消息对象
 * @return 成功返回TRUE
 * 说明：
 *   1. 设置timers_started标志为TRUE
 *   2. 启动语音活动检测定时器
 *   3. 发送响应
 */
static apt_bool_t funasr_channel_timers_start(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
    funasr_channel_t *recog_channel = channel->method_obj;
    recog_channel->timers_started = TRUE;
    return mrcp_engine_channel_message_send(channel, response);
}

/**
 * @brief 分发MRCP请求到对应的处理函数
 * @param channel MRCP引擎通道对象
 * @param request MRCP请求消息
 * @return 成功返回TRUE
 * 说明：
 *   1. 根据请求方法ID分发到不同处理函数：
 *      - SET_PARAMS: 参数设置
 *      - GET_PARAMS: 参数获取
 *      - DEFINE_GRAMMAR: 语法定义
 *      - RECOGNIZE: 语音识别
 *      - GET_RESULT: 获取结果
 *      - START_INPUT_TIMERS: 启动定时器
 *      - STOP: 停止识别
 *   2. 未处理的请求直接发送响应
 */
static apt_bool_t funasr_channel_request_dispatch(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
    apt_bool_t processed = FALSE;
    mrcp_message_t *response = mrcp_response_create(request, request->pool);
    switch (request->start_line.method_id) {
        case RECOGNIZER_SET_PARAMS:
            break;
        case RECOGNIZER_GET_PARAMS:
            break;
        case RECOGNIZER_DEFINE_GRAMMAR:
            break;
        case RECOGNIZER_RECOGNIZE:
            processed = funasr_channel_recognize(channel, request, response);
            break;
        case RECOGNIZER_GET_RESULT:
            break;
        case RECOGNIZER_START_INPUT_TIMERS:
            processed = funasr_channel_timers_start(channel, request, response);
            break;
        case RECOGNIZER_STOP:
            processed = funasr_channel_stop(channel, request, response);
            break;
        default:
            break;
    }
    if (processed == FALSE) {
        /* send asynchronous response for not handled request */
        mrcp_engine_channel_message_send(channel, response);
    }
    return TRUE;
}

/* ---------- stream functions ---------- */

/**
 * @brief 销毁识别音频流
 * @param stream MPF音频流对象
 * @return 成功返回TRUE
 * 说明：无需释放资源，所有资源由APR池自动管理
 */
static apt_bool_t funasr_stream_destroy(mpf_audio_stream_t *stream)
{
    return TRUE;
}

/**
 * @brief 打开识别音频流
 * @param stream MPF音频流对象
 * @param codec 音频编解码器信息
 * @return 成功返回TRUE
 * 说明：音频流打开时无需特殊处理，直接返回成功
 */
static apt_bool_t funasr_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec)
{
    return TRUE;
}

/**
 * @brief 关闭识别音频流
 * @param stream MPF音频流对象
 * @return 成功返回TRUE
 * 说明：音频流关闭时无需特殊处理，直接返回成功
 */
static apt_bool_t funasr_stream_close(mpf_audio_stream_t *stream)
{
    return TRUE;
}

/**
 * @brief 处理实时ASR响应（在发送音频时调用）
 * @param recog_channel FunASR识别通道对象
 * 说明：
 *   非阻塞地尝试接收ASR服务器的响应
 *   新接口返回即为 final 结果，没有中间结果
 */
static void funasr_process_realtime_response(funasr_channel_t *recog_channel)
{
    apr_size_t i;
    const char *text_start;
    const char *text_end;
    int code;

    if (!recog_channel->ws_connected) {
        return;
    }

    /* Try to receive response (non-blocking) */
    apr_size_t response_size;
    char *response = funasr_websocket_recv_nonblock(recog_channel, &response_size);

    if (!response) {
        return;
    }

    /* Debug: Print raw response bytes (first 200 bytes) */
    if (response_size > 0) {
        apr_size_t debug_len = response_size < 200 ? response_size : 200;
        char *hex_debug = apr_palloc(recog_channel->channel->pool, debug_len * 3 + 1);
        for (i = 0; i < debug_len; i++) {
            sprintf(hex_debug + i * 3, "%02x ", (unsigned char)response[i]);
        }
        LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "Raw response bytes: %s", hex_debug);
    }

    /* Debug: Use write() to bypass apt_log encoding issues */
    LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "Realtime response (via write):");
    if (response_size > 0 && response) {
        /* Write directly to stderr to avoid encoding conversions */
        fwrite(response, 1, response_size, stderr);
        fwrite("\n", 1, 1, stderr);
        fflush(stderr);
    }

    /* Debug: Verify the exact bytes of the "text" field value */
    /* Find the "text" field in JSON */
    text_start = strstr(response, "\"text\":\"");
    if (text_start) {
        text_start += 8;  /* Skip past "text":" */
        text_end = strchr(text_start, '"');
        if (text_end) {
            apr_size_t text_len = text_end - text_start;
            LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "Text field length = %d bytes", (int)text_len);

            /* Print hex of text field */
            char *text_hex = apr_palloc(recog_channel->channel->pool, text_len * 3 + 1);
            for (i = 0; i < text_len; i++) {
                sprintf(text_hex + i * 3, "%02x ", (unsigned char)text_start[i]);
            }
            LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "Text field hex: %s", text_hex);

            /* Check if it starts with backslash u (Unicode escape) or UTF-8 */
            if (text_len >= 6 && text_start[0] == '\\' && text_start[1] == 'u') {
                LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "Text field contains Unicode escape sequences (\\uXXXX)");
            } else if (text_len >= 3 && (unsigned char)text_start[0] >= 0xE0) {
                LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "Text field contains raw UTF-8 bytes");
            }
        }
    }

    /* Check for error in response */
    code = 0;
    if (funasr_json_get_int(response, "code", &code) == TRUE && code != 0) {
        char *msg = funasr_json_get_string(recog_channel->channel->pool, response, "message");
        LOG_WITH_SID(recog_channel, APT_PRIO_WARNING, "ASR error code=%d message=%s",
                code, msg ? msg : "(null)");
        return;
    }

    /* New interface: response is always final */
    char *text = funasr_json_get_string(recog_channel->channel->pool, response, "text");
    if (text) {
        /* Final result: print and save for later */
        LOG_WITH_SID(recog_channel, APT_PRIO_INFO,
                "========== Realtime FINAL result ==========\n"
                "      Text: %s\n"
                "=====================================================",
                text);
        recog_channel->realtime_final_text = text;

        /* Send recognition complete message to upstream when ASR returns text */
        LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "ASR returned text, sending RECOGNITION-COMPLETE event");

        /* Mark recognition as complete - this recognition session is done
         * Any subsequent audio will be ignored until next RECOGNIZE request */
        recog_channel->recognition_complete = TRUE;

        /* Send recognition complete message to upstream */
        funasr_recognition_complete(recog_channel, RECOGNIZER_COMPLETION_CAUSE_SUCCESS, text);

        /* Send end marker immediately so the ASR server resets its state for the
         * next recognition round.  Without this the server may treat audio from
         * the next RECOGNIZE request as continuation of the current round. */
        if (recog_channel->ws_connected) {
            recog_channel->ws_send_size = 0;  /* clear stale buffered audio */
            funasr_ws_send_end_frame(recog_channel);

            /* Detect if the server closed the connection after the end marker. */
            apr_size_t check_size;
            if (funasr_websocket_recv_nonblock(recog_channel, &check_size) == NULL &&
                !recog_channel->ws_connected) {
                LOG_WITH_SID(recog_channel, APT_PRIO_INFO,
                    "ASR server closed WebSocket after end marker, will reconnect on next session");
            } else {
                LOG_WITH_SID(recog_channel, APT_PRIO_INFO,
                    "WebSocket connection kept alive for next recognition session");
            }
        }
    }
}

/**
 * @brief 写入音频流数据
 * @param stream MPF音频流对象
 * @param frame 音频帧对象
 * @return 成功返回TRUE
 * 说明：
 *   1. 记录日志：收到音频数据（INFO级别，包含累计大小）
 *   2. 检查是否有待发送的停止响应
 *   3. 如果有识别请求，使用语音活动检测器处理音频帧：
 *      - ACTIVITY: 检测到语音活动，发送START-OF-INPUT事件
 *      - INACTIVITY: 检测到语音不活动，发送音频到FunASR服务
 *      - NOINPUT: 无输入超时，发送识别完成事件（超时原因）
 *   4. 将音频数据保存到缓冲区（最大10MB）
 */
static apt_bool_t funasr_stream_write(mpf_audio_stream_t *stream, const mpf_frame_t *frame)
{
    funasr_channel_t *recog_channel = stream->obj;
    const mpf_codec_descriptor_t *tx_descriptor = NULL;
    const mpf_codec_descriptor_t *codec_descriptor = NULL;
    apt_bool_t need_resample = FALSE;

    /* Log when audio data is received */
    if (frame && frame->codec_frame.size && frame->type & MEDIA_FRAME_TYPE_AUDIO) {
        /* Log audio format info on first audio frame */
        if (!recog_channel->audio_info_logged) {
            codec_descriptor = mrcp_engine_sink_stream_codec_get(recog_channel->channel);
            if (codec_descriptor) {
                LOG_WITH_SID(recog_channel, APT_PRIO_INFO,
                        "========== Audio Stream Info ==========\n"
                        "      Format: %s\n"
                        "      Sample Rate: %d Hz\n"
                        "      Channels: %d\n"
                        "      Frame Size: %d bytes\n"
                        "==============================================",
                        codec_descriptor->name.buf ? codec_descriptor->name.buf : "(unknown)",
                        codec_descriptor->sampling_rate,
                        codec_descriptor->channel_count,
                        (int)frame->codec_frame.size);
            }
            recog_channel->audio_info_logged = TRUE;
        }
        LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "Recv audio size=%d bytes, total=%d bytes",
                (int)frame->codec_frame.size, (int)recog_channel->audio_size);
    }

    if (recog_channel->stop_response) {
        LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "Processing STOP response (recog_request=%p, ws_connected=%d, recognition_complete=%d)",
                recog_channel->recog_request, recog_channel->ws_connected, recog_channel->recognition_complete);

        if (recog_channel->recognition_complete) {
            /* Recognition already completed via realtime path - result and end marker
             * were already sent. Just check if the server closed the connection. */
            if (recog_channel->ws_connected) {
                LOG_WITH_SID(recog_channel, APT_PRIO_INFO,
                    "Recognition already completed, checking server connection state");
                apr_size_t check_size;
                if (funasr_websocket_recv_nonblock(recog_channel, &check_size) == NULL &&
                    !recog_channel->ws_connected) {
                    LOG_WITH_SID(recog_channel, APT_PRIO_INFO,
                        "Server closed WebSocket, will reconnect on next session");
                }
            }
        } else if (recog_channel->ws_connected && recog_channel->audio_size > 0) {
            /* WebSocket connected with audio sent but no result yet - try to get final result */
            LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "STOP with active audio session, trying to get final result");

            /* Flush remaining buffered audio and send end marker */
            if (!funasr_ws_send_end_frame(recog_channel)) {
                LOG_WITH_SID(recog_channel, APT_PRIO_WARNING,
                        "Failed to send audio end marker on STOP, will still try to receive response");
            }

            /* Temporarily shorten socket timeout to 5s for STOP's blocking recv.
             * The server may not return a result (e.g. no speech detected), and we must
             * not block the audio thread while the MRCP client is waiting for STOP response. */
            apr_interval_time_t saved_timeout;
            apr_socket_timeout_get(recog_channel->ws_socket, &saved_timeout);
            apr_socket_timeout_set(recog_channel->ws_socket, 5 * 1000000);

            /* Try to receive final result (blocking, up to 5s timeout) */
            apr_size_t response_size;
            char *response = funasr_websocket_recv(recog_channel, &response_size);
            if (response) {
                LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "WebSocket response on STOP: %s", response);

                int code = 0;
                if (funasr_json_get_int(response, "code", &code) == TRUE && code == 0) {
                    char *final_text = funasr_json_get_string(recog_channel->channel->pool, response, "text");
                    if (final_text) {
                        LOG_WITH_SID(recog_channel, APT_PRIO_INFO,
                                "========== Got result on STOP ==========\n"
                                "      Text: %s\n"
                                "=====================================================",
                                final_text);
                        /* Send the result BEFORE sending STOP response */
                        funasr_recognition_complete(recog_channel, RECOGNIZER_COMPLETION_CAUSE_SUCCESS, final_text);
                    } else {
                        LOG_WITH_SID(recog_channel, APT_PRIO_WARNING, "WebSocket response has no text field");
                    }
                } else {
                    LOG_WITH_SID(recog_channel, APT_PRIO_WARNING, "WebSocket returned error code=%d on STOP", code);
                }
            } else {
                LOG_WITH_SID(recog_channel, APT_PRIO_WARNING,
                    "No response from WebSocket on STOP within 5 seconds, giving up");
            }

            /* Restore original socket timeout */
            if (recog_channel->ws_socket) {
                apr_socket_timeout_set(recog_channel->ws_socket, saved_timeout);
            }

            LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "STOP request processed, keeping WebSocket connection alive for next recognition session");
        } else {
            /* No audio was sent in this recognition session */
            LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "STOP with no audio sent, keeping WebSocket connection alive");
        }

        /* Send STOP response */
        /* Per MRCP spec, STOP must be followed by RECOGNITION-COMPLETE to inform
         * the client of the final recognition state.  If the realtime path already
         * sent it (recognition_complete=TRUE before STOP), skip. */
        if (!recog_channel->recognition_complete && recog_channel->recog_request) {
            LOG_WITH_SID(recog_channel, APT_PRIO_WARNING,
                "STOP without recognition result, sending RECOGNITION-COMPLETE with ERROR cause");
            funasr_recognition_complete(recog_channel,
                RECOGNIZER_COMPLETION_CAUSE_ERROR, NULL);
        }

        mrcp_engine_channel_message_send(recog_channel->channel, recog_channel->stop_response);
        recog_channel->stop_response = NULL;
        recog_channel->recog_request = NULL;
        if (!recog_channel->recognition_complete) {
            recog_channel->recognition_complete = TRUE;
        }
        LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "STOP response sent");
        return TRUE;
    }

    if (recog_channel->recog_request) {
        /* determine codec sampling rate once per frame */
        tx_descriptor = mrcp_engine_sink_stream_codec_get(recog_channel->channel);
        if (tx_descriptor && tx_descriptor->sampling_rate == 8000) {
            need_resample = TRUE;
        }
        /* VAD功能已关闭 - 直接发送音频到WebSocket
         * 当收到音频帧时立即连接WebSocket并发送，不等待VAD检测
         * 识别结束由STOP请求触发
         * 说明：
         *   - 不再使用音频暂停检测，避免误判正常说话停顿
         *   - 每次识别由明确的STOP请求结束
         *   - WebSocket连接会复用，但每轮识别需要新的RECOGNIZE请求
         */
        if (frame && frame->codec_frame.size && frame->type & MEDIA_FRAME_TYPE_AUDIO) {
            /* Skip audio processing if recognition is already complete (client explicitly stopped) */
            if (recog_channel->recognition_complete) {
                LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "Recognition already complete (client stopped), ignoring audio data");
                return TRUE;
            }

            /* Check no-result timeout: 10s since last audio packet was sent with no ASR response.
             * Timeout starts from when the last audio chunk was actually sent over the network.
             * Without this guard the MRCP session and WebSocket connection would stay open indefinitely. */
            {
                apr_time_t timeout_base = recog_channel->last_audio_send_time > 0
                    ? recog_channel->last_audio_send_time : recog_channel->request_start_time;
                apr_interval_time_t elapsed = apr_time_now() - timeout_base;
                if (elapsed > 10 * 1000000) {
                    LOG_WITH_SID(recog_channel, APT_PRIO_ERROR,
                        "========== NO-RESULT TIMEOUT (10 seconds since last audio sent) ==========\n"
                        "      No ASR result received in %d seconds, aborting recognition.\n"
                        "      Audio sent: %d bytes\n"
                        "      Closing WebSocket and completing recognition.\n"
                        "====================================================",
                        (int)(elapsed / 1000000), (int)recog_channel->audio_size);
                    if (recog_channel->ws_connected) {
                        funasr_websocket_disconnect(recog_channel);
                    }
                    recog_channel->recognition_complete = TRUE;
                    funasr_recognition_complete(recog_channel,
                        RECOGNIZER_COMPLETION_CAUSE_NO_INPUT_TIMEOUT, NULL);
                    return TRUE;
                }
            }

            /* Connect to WebSocket server if not connected */
            if (!recog_channel->ws_connected) {
                LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "========== Starting new recognition session, connecting WebSocket ==========");
                funasr_start_of_input(recog_channel);
                apt_bool_t connect_result = funasr_websocket_connect(recog_channel);
                if (!connect_result) {
                    LOG_WITH_SID(recog_channel, APT_PRIO_WARNING, "Failed to connect to WebSocket server, cannot send audio");
                    /* Don't send audio if connection failed */
                    return TRUE;
                }
                LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "WebSocket connected successfully, ready to send audio");
            } else {
                /* WebSocket already connected, reusing for new recognition session */
                if (recog_channel->audio_size == 0) {
                    LOG_WITH_SID(recog_channel, APT_PRIO_INFO,
                        "Reusing existing WebSocket connection for new recognition session");
                }
            }
            /* Buffer and send audio data via WebSocket */
            if (recog_channel->ws_connected) {
                if (need_resample) {
                    /* Save resample state so the storage path below sees the same
                     * previous-frame state (the first call updates it in-place). */
                    int16_t saved_prev[2];
                    apt_bool_t saved_valid;
                    memcpy(saved_prev, recog_channel->resample_prev_samples, sizeof(saved_prev));
                    saved_valid = recog_channel->resample_state_valid;

                    LOG_WITH_SID(recog_channel, APT_PRIO_DEBUG, "Original sample rate=%d Hz, channels=%d",
                            tx_descriptor->sampling_rate, tx_descriptor->channel_count ? tx_descriptor->channel_count : 1);
                    apr_size_t out_size = 0;
                    char *out_buf = funasr_resample_8k_to_16k(recog_channel->channel->pool,
                                                            (const char*)frame->codec_frame.buffer,
                                                            frame->codec_frame.size,
                                                            &out_size,
                                                            tx_descriptor->channel_count ? tx_descriptor->channel_count : 1,
                                                            recog_channel->resample_prev_samples,
                                                            &recog_channel->resample_state_valid);
                    if (out_buf && out_size > 0) {
                        LOG_WITH_SID(recog_channel, APT_PRIO_DEBUG, "Resampled audio 8000->16000, size=%d bytes", (int)out_size);
                        funasr_ws_send_buffered(recog_channel, out_buf, out_size);
                        /* Note: last_audio_send_time is updated inside funasr_ws_send_buffered/flush
                         * when data is actually sent over network, not here */
                    }

                    /* Restore pre-call state so the storage-path call below starts
                     * from the same previous-frame boundary. */
                    memcpy(recog_channel->resample_prev_samples, saved_prev, sizeof(saved_prev));
                    recog_channel->resample_state_valid = saved_valid;
                } else {
                    funasr_ws_send_buffered(recog_channel, (const char*)frame->codec_frame.buffer, frame->codec_frame.size);
                    /* Note: last_audio_send_time is updated inside funasr_ws_send_buffered/flush
                     * when data is actually sent over network, not here */
                }

                /* After sending audio, try to receive any available response */
                funasr_process_realtime_response(recog_channel);
            }
        }

        if (recog_channel->recog_request) {
            if (frame->codec_frame.size && frame->type & MEDIA_FRAME_TYPE_AUDIO) {
                /* Skip storing audio if recognition is already complete (client explicitly stopped) */
                if (recog_channel->recognition_complete) {
                    LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "Recognition complete, skipping audio storage");
                    return TRUE;
                }
                /* Store resampled audio if needed */
                    if (need_resample) {
                        LOG_WITH_SID(recog_channel, APT_PRIO_DEBUG, "Original sample rate=%d Hz, channels=%d (for storage)",
                                tx_descriptor->sampling_rate, tx_descriptor->channel_count ? tx_descriptor->channel_count : 1);
                        apr_size_t out_size = 0;
                        char *out_buf = funasr_resample_8k_to_16k(recog_channel->channel->pool,
                                                                (const char*)frame->codec_frame.buffer,
                                                                frame->codec_frame.size,
                                                                &out_size,
                                                                tx_descriptor->channel_count ? tx_descriptor->channel_count : 1,
                                                                recog_channel->resample_prev_samples,
                                                                &recog_channel->resample_state_valid);
                        if (out_buf && out_size > 0) {
                            LOG_WITH_SID(recog_channel, APT_PRIO_DEBUG, "Resampled audio 8000->16000 for storage, size=%d bytes", (int)out_size);
                            if (recog_channel->audio_size + out_size <= recog_channel->audio_capacity) {
                                memcpy(recog_channel->audio_buffer + recog_channel->audio_size, out_buf, out_size);
                                recog_channel->audio_size += out_size;
                            } else {
                                LOG_WITH_SID(recog_channel, APT_PRIO_WARNING, "Audio buffer full, dropping data");
                            }
                        }
                    } else {
                    if (recog_channel->audio_size + frame->codec_frame.size <= recog_channel->audio_capacity) {
                        memcpy(recog_channel->audio_buffer + recog_channel->audio_size,
                               frame->codec_frame.buffer,
                               frame->codec_frame.size);
                        recog_channel->audio_size += frame->codec_frame.size;
                    } else {
                        LOG_WITH_SID(recog_channel, APT_PRIO_WARNING, "Audio buffer full, dropping data");
                    }
                }
            }
        }
    }

    return TRUE;
}

/**
 * @brief 发送START-OF-INPUT事件
 * @param recog_channel FunASR识别通道对象
 * @return 成功返回TRUE，失败返回FALSE
 * 说明：
 *   1. 创建MRCP事件消息
 *   2. 设置事件类型为START-OF-INPUT
 *   3. 设置请求状态为INPROGRESS
 *   4. 通过MRCP引擎通道发送事件
 */
static apt_bool_t funasr_start_of_input(funasr_channel_t *recog_channel)
{
    mrcp_message_t *message = mrcp_event_create(
        recog_channel->recog_request,
        RECOGNIZER_START_OF_INPUT,
        recog_channel->recog_request->pool);
    if (!message) {
        return FALSE;
    }

    message->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
    return mrcp_engine_channel_message_send(recog_channel->channel, message);
}

/**
 * @brief 发送RECOGNITION-COMPLETE事件（识别完成）
 * @param recog_channel FunASR识别通道对象
 * @param cause 识别完成原因（成功/超时/无匹配/错误）
 * @param result 识别结果文本，成功时有效
 * @return 成功返回TRUE，失败返回FALSE
 * 说明：
 *   1. 创建MRCP事件消息
 *   2. 设置事件类型为RECOGNITION-COMPLETE
 *   3. 设置完成原因到消息头
 *   4. 设置请求状态为COMPLETE
 *   5. 如果成功且有结果，设置消息体和Content-Type为text/plain
 *   6. 清除recog_request引用
 *   7. 通过MRCP引擎通道发送事件
 */
static apt_bool_t funasr_recognition_complete(funasr_channel_t *recog_channel, mrcp_recog_completion_cause_e cause, const char *result)
{
    mrcp_recog_header_t *recog_header;

    /* Check if recog_request is still valid */
    if (!recog_channel->recog_request) {
        LOG_WITH_SID(recog_channel, APT_PRIO_WARNING, "recog_request is NULL in funasr_recognition_complete, cannot send result");
        return FALSE;
    }

    mrcp_message_t *message = mrcp_event_create(
        recog_channel->recog_request,
        RECOGNIZER_RECOGNITION_COMPLETE,
        recog_channel->recog_request->pool);
    if (!message) {
        LOG_WITH_SID(recog_channel, APT_PRIO_WARNING, "Failed to create RECOGNITION-COMPLETE event message");
        return FALSE;
    }

    /* Log recognition complete */
    const char *cause_str = "unknown";
    switch (cause) {
        case RECOGNIZER_COMPLETION_CAUSE_SUCCESS:
            cause_str = "success";
            break;
        case RECOGNIZER_COMPLETION_CAUSE_NO_INPUT_TIMEOUT:
            cause_str = "no_input_timeout";
            break;
        case RECOGNIZER_COMPLETION_CAUSE_NO_MATCH:
            cause_str = "no_match";
            break;
        case RECOGNIZER_COMPLETION_CAUSE_ERROR:
            cause_str = "error";
            break;
        default:
            break;
    }

    LOG_WITH_SID(recog_channel, APT_PRIO_INFO,
            "========== Recognition complete ==========\n"
            "      Cause: %s (code=%d)\n"
            "      Audio size: %d bytes\n"
            "      Result: %s\n"
            "=====================================================",
            cause_str, cause, (int)recog_channel->audio_size,
            result ? result : "(none)");

    /* get/allocate recognizer header */
    recog_header = mrcp_resource_header_prepare(message);
    if (recog_header) {
        /* set completion cause */
        recog_header->completion_cause = cause;
        mrcp_resource_header_property_add(message, RECOGNIZER_HEADER_COMPLETION_CAUSE);
    }
    message->start_line.request_state = MRCP_REQUEST_STATE_COMPLETE;

    if (result && (cause == RECOGNIZER_COMPLETION_CAUSE_SUCCESS ||
                   cause == RECOGNIZER_COMPLETION_CAUSE_ERROR ||
                   cause == RECOGNIZER_COMPLETION_CAUSE_NO_MATCH ||
                   cause == RECOGNIZER_COMPLETION_CAUSE_NO_INPUT_TIMEOUT)) {
        /* Convert plain text result to NLSML format */
        char *nlsml_result;
        char *result_with_suffix = apr_psprintf(message->pool, "%s@%s.wav", result, recog_channel->session_id);
        nlsml_result = apr_psprintf(message->pool,
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<result>\n"
            "  <interpretation grammar=\"session:%s\" confidence=\"1\">\n"
            "    <instance>\n"
            "      <result>%s</result>\n"
            "    </instance>\n"
            "    <input mode=\"speech\">%s</input>\n"
            "  </interpretation>\n"
            "</result>",
            recog_channel->session_id, result_with_suffix, result);

        apt_string_assign(&message->body, nlsml_result, message->pool);

        LOG_WITH_SID(recog_channel, APT_PRIO_INFO,
                "========== Sending RECOGNITION-COMPLETE ==========\n"
                "      Body length: %d\n"
                "      Content-Type: application/x-nlsml\n"
                "      Body: %s\n"
                "=====================================================",
                (int)message->body.length, nlsml_result);

        /* get/allocate generic header */
        mrcp_generic_header_t *generic_header;
        generic_header = mrcp_generic_header_prepare(message);
        if (generic_header) {
            apt_string_assign(&generic_header->content_type, "application/x-nlsml", message->pool);
            mrcp_generic_header_property_add(message, GENERIC_HEADER_CONTENT_TYPE);
        }
    } else {
        LOG_WITH_SID(recog_channel, APT_PRIO_WARNING, "Not setting message body - result=%p, cause=%d", result, cause);
    }

    LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "Sending MRCP event message...");
    recog_channel->recog_request = NULL;
    apt_bool_t send_result = mrcp_engine_channel_message_send(recog_channel->channel, message);
    LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "MRCP message send %s", send_result ? "SUCCESS" : "FAILED");
    return send_result;
}

/**
 * @brief 发送中间识别结果（实时识别场景）
 * @param recog_channel FunASR识别通道对象
 * @param result 中间识别结果文本
 * @return 成功返回TRUE，失败返回FALSE
 * 说明：
 *   1. 创建 MRCP 事件消息
 *   2. 设置事件类型为 RECOGNITION-COMPLETE（但状态为 INPROGRESS）
 *   3. 设置消息体和 Content-Type
 *   4. 通过 MRCP 引擎通道发送事件
 *   5. 不清空 recog_request，识别会话继续
 */
static apt_bool_t funasr_send_intermediate_result(funasr_channel_t *recog_channel, const char *result)
{
    mrcp_message_t *message;
    char *nlsml_result;

    if (!recog_channel->recog_request) {
        return FALSE;
    }

    message = mrcp_event_create(
        recog_channel->recog_request,
        RECOGNIZER_RECOGNITION_COMPLETE,
        recog_channel->recog_request->pool);
    if (!message) {
        return FALSE;
    }

    LOG_WITH_SID(recog_channel, APT_PRIO_INFO,
            "========== Intermediate result (real-time) ==========\n"
            "      Result: %s\n"
            "      Status: INPROGRESS (recognition continues)"
            "=============================================================",
            result ? result : "(none)");

    /* Set request state to INPROGRESS to indicate recognition is ongoing */
    message->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;

    if (result) {
        /* Convert plain text result to NLSML format */
        char *result_with_suffix = apr_psprintf(message->pool, "%s@%s.wav", result, recog_channel->session_id);
        nlsml_result = apr_psprintf(message->pool,
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<result>\n"
            "  <interpretation grammar=\"session:%s\" confidence=\"1\">\n"
            "    <instance>\n"
            "      <result>%s</result>\n"
            "    </instance>\n"
            "    <input mode=\"speech\">%s</input>\n"
            "  </interpretation>\n"
            "</result>",
            recog_channel->session_id, result_with_suffix, result);

        apt_string_assign(&message->body, nlsml_result, message->pool);

        /* Set Content-Type header */
        mrcp_generic_header_t *generic_header;
        generic_header = mrcp_generic_header_prepare(message);
        if (generic_header) {
            apt_string_assign(&generic_header->content_type, "application/x-nlsml", message->pool);
            mrcp_generic_header_property_add(message, GENERIC_HEADER_CONTENT_TYPE);
        }
    }

    /* Do NOT clear recog_request - recognition session continues */
    return mrcp_engine_channel_message_send(recog_channel->channel, message);
}

/* JSON helper functions */
static apt_bool_t funasr_json_get_int(const char *json, const char *key, int *value)
{
    char pattern[64];
    apr_snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        return FALSE;
    }
    p = strchr(p, ':');
    if (!p) {
        return FALSE;
    }
    p++;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    *value = (int)strtol(p, NULL, 10);
    return TRUE;
}

static char* funasr_json_get_string(apr_pool_t *pool, const char *json, const char *key)
{
    char pattern[64];
    apr_snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        return NULL;
    }
    p = strchr(p, ':');
    if (!p) {
        return NULL;
    }
    p++;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != '"') {
        return NULL;
    }
    p++;
    const char *end = strchr(p, '"');
    if (!end) {
        return NULL;
    }
    /* ASR service returns UTF-8 encoded text directly, no need to decode Unicode escapes */
    return apr_pstrndup(pool, p, end - p);
}

/**
 * @brief Decode JSON string escape sequences including Unicode \uXXXX
 * @param pool APR memory pool
 * @param str Raw JSON string value (may contain \uXXXX escapes)
 * @return Decoded string with proper UTF-8 encoding
 *
 * Handles:
 * - Unicode escapes: \uXXXX -> UTF-8 character
 * - Simple escapes: \\, \", \/, \b, \f, \n, \r, \t
 */
static char* funasr_json_unescape_string(apr_pool_t *pool, const char *str)
{
    size_t i;
    int j;

    if (!str) {
        return NULL;
    }

    /* UTF-8 output can be up to 3x longer for worst case (ASCII to Chinese) */
    size_t len = strlen(str);
    char *out = apr_palloc(pool, len * 3 + 1);
    size_t out_idx = 0;

    for (i = 0; i < len; ) {
        if (str[i] == '\\' && i + 1 < len) {
            switch (str[i + 1]) {
                case '"':  out[out_idx++] = '"';  i += 2; break;
                case '\\': out[out_idx++] = '\\'; i += 2; break;
                case '/':  out[out_idx++] = '/';  i += 2; break;
                case 'b':  out[out_idx++] = '\b'; i += 2; break;
                case 'f':  out[out_idx++] = '\f'; i += 2; break;
                case 'n':  out[out_idx++] = '\n'; i += 2; break;
                case 'r':  out[out_idx++] = '\r'; i += 2; break;
                case 't':  out[out_idx++] = '\t'; i += 2; break;
                case 'u':
                    /* Unicode escape \uXXXX */
                    if (i + 5 < len) {
                        /* Parse 4 hex digits */
                        unsigned int codepoint = 0;
                        for (j = 0; j < 4; j++) {
                            char c = str[i + 2 + j];
                            codepoint <<= 4;
                            if (c >= '0' && c <= '9') {
                                codepoint |= (c - '0');
                            } else if (c >= 'a' && c <= 'f') {
                                codepoint |= (c - 'a' + 10);
                            } else if (c >= 'A' && c <= 'F') {
                                codepoint |= (c - 'A' + 10);
                            }
                        }
                        i += 6;  /* Skip all 6 chars of \uXXXX */

                        /* Convert Unicode codepoint to UTF-8 */
                        if (codepoint <= 0x7F) {
                            /* 1 byte: 0xxxxxxx */
                            out[out_idx++] = (char)codepoint;
                        } else if (codepoint <= 0x7FF) {
                            /* 2 bytes: 110xxxxx 10xxxxxx */
                            out[out_idx++] = (char)(0xC0 | (codepoint >> 6));
                            out[out_idx++] = (char)(0x80 | (codepoint & 0x3F));
                        } else if (codepoint <= 0xFFFF) {
                            /* 3 bytes: 1110xxxx 10xxxxxx 10xxxxxx */
                            out[out_idx++] = (char)(0xE0 | (codepoint >> 12));
                            out[out_idx++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                            out[out_idx++] = (char)(0x80 | (codepoint & 0x3F));
                        } else {
                            /* Surrogate pairs for characters beyond BMP (not handled here) */
                            out[out_idx++] = '?';
                        }
                    } else {
                        /* Incomplete escape, just copy as-is */
                        out[out_idx++] = str[i++];
                    }
                    break;
                default:
                    /* Unknown escape, just copy the backslash */
                    out[out_idx++] = str[i++];
                    break;
            }
        } else {
            out[out_idx++] = str[i++];
        }
    }

    out[out_idx] = '\0';
    return out;
}

/* WebSocket client implementation */

static const char funasr_ws_b64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char* funasr_base64_encode(apr_pool_t *pool, const unsigned char *data, size_t len)
{
    int out_len = apr_base64_encode_len((int)len);
    char *encoded = apr_palloc(pool, out_len);
    apr_base64_encode(encoded, (const char*)data, (int)len);
    return encoded;
}

/**
 * @brief Generate Sec-WebSocket-Key for handshake
 * @param pool APR memory pool
 * @return WebSocket key string
 */
static char* funasr_websocket_generate_key(apr_pool_t *pool)
{
    unsigned char key[16];
    apr_uuid_t uuid;

    /* Initialize key array to zero to avoid undefined behavior */
    memset(key, 0, sizeof(key));
    
    apr_uuid_get(&uuid);
    memcpy(key, uuid.data, sizeof(key));

    char *ws_key = funasr_base64_encode(pool, key, sizeof(key));
    apt_log(APT_LOG_MARK, APT_PRIO_DEBUG, "zyASR: Generated Sec-WebSocket-Key: %s", ws_key ? ws_key : "(null)");
    return ws_key;
}

/* Minimal SHA1 implementation */
typedef struct {
    uint32_t state[5];
    uint64_t count;
    unsigned char buffer[64];
} funasr_sha1_ctx_t;

static void funasr_sha1_transform(uint32_t state[5], const unsigned char buffer[64])
{
    uint32_t a, b, c, d, e, t;
    uint32_t w[80];
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)buffer[i * 4] << 24) |
               ((uint32_t)buffer[i * 4 + 1] << 16) |
               ((uint32_t)buffer[i * 4 + 2] << 8) |
               ((uint32_t)buffer[i * 4 + 3]);
    }
    for (i = 16; i < 80; i++) {
        t = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
        w[i] = (t << 1) | (t >> 31);
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];

    for (i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d;
        d = c;
        c = (b << 30) | (b >> 2);
        b = a;
        a = t;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

static void funasr_sha1_init(funasr_sha1_ctx_t *ctx)
{
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count = 0;
}

static void funasr_sha1_update(funasr_sha1_ctx_t *ctx, const unsigned char *data, size_t len)
{
    size_t i = 0;
    size_t j = (size_t)(ctx->count & 63);
    ctx->count += (uint64_t)len;

    if ((j + len) > 63) {
        size_t part_len = 64 - j;
        memcpy(ctx->buffer + j, data, part_len);
        funasr_sha1_transform(ctx->state, ctx->buffer);
        for (i = part_len; i + 63 < len; i += 64) {
            funasr_sha1_transform(ctx->state, data + i);
        }
        j = 0;
    }

    if (i < len) {
        memcpy(ctx->buffer + j, data + i, len - i);
    }
}

static void funasr_sha1_final(funasr_sha1_ctx_t *ctx, unsigned char digest[20])
{
    unsigned char finalcount[8];
    unsigned char c = 0x80;
    int i;

    for (i = 0; i < 8; i++) {
        finalcount[i] = (unsigned char)((ctx->count * 8) >> ((7 - i) * 8));
    }

    funasr_sha1_update(ctx, &c, 1);
    while ((ctx->count & 63) != 56) {
        c = 0x00;
        funasr_sha1_update(ctx, &c, 1);
    }
    funasr_sha1_update(ctx, finalcount, 8);

    for (i = 0; i < 20; i++) {
        digest[i] = (unsigned char)(ctx->state[i >> 2] >> ((3 - (i & 3)) * 8));
    }
}

static char* funasr_websocket_compute_accept(apr_pool_t *pool, const char *key)
{
    static const char ws_guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    funasr_sha1_ctx_t ctx;
    unsigned char digest[20];
    char *tmp = apr_pstrcat(pool, key, ws_guid, NULL);

    funasr_sha1_init(&ctx);
    funasr_sha1_update(&ctx, (const unsigned char*)tmp, strlen(tmp));
    funasr_sha1_final(&ctx, digest);

    return funasr_base64_encode(pool, digest, sizeof(digest));
}

static const char* funasr_ws_strcasestr(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !*needle) {
        return haystack;
    }
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        if (tolower((unsigned char)*haystack) == tolower((unsigned char)*needle)) {
            if (strncasecmp(haystack, needle, nlen) == 0) {
                return haystack;
            }
        }
    }
    return NULL;
}

static char* funasr_ws_get_header_value(apr_pool_t *pool, const char *response, const char *header)
{
    const char *p;
    const char *end;

    p = funasr_ws_strcasestr(response, header);
    if (!p) {
        return NULL;
    }
    p = strchr(p, ':');
    if (!p) {
        return NULL;
    }
    p++;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    end = p;
    while (*end && *end != '\r' && *end != '\n') {
        end++;
    }
    if (end == p) {
        return NULL;
    }
    return apr_pstrndup(pool, p, end - p);
}

/**
 * @brief Connect to WebSocket server and perform handshake
 * @param channel FunASR recognition channel
 * @return Success returns TRUE
 */
static apt_bool_t funasr_websocket_connect(funasr_channel_t *channel)
{
    apr_socket_t *sock;
    apr_status_t rv;
    apr_sockaddr_t *sa;
    char request[1024];
    char buffer[4096];
    apr_size_t len;
    char *ws_key;
    const char *host = channel->funasr_engine->server_host;
    apr_port_t port = channel->funasr_engine->server_port;
    const char *path = channel->funasr_engine->server_path;
    apr_pool_t *pool = channel->ws_pool;

    /* Use dynamic audio parameters (set during RECOGNIZE) */
    int sample_rate = channel->actual_sample_rate > 0 ? channel->actual_sample_rate : FUNASR_SAMPLE_RATE;
    int n_channels = channel->actual_n_channels > 0 ? channel->actual_n_channels : FUNASR_N_CHANNELS;
    int sample_width = channel->actual_sample_width > 0 ? channel->actual_sample_width : FUNASR_SAMPLE_WIDTH;

    /* Ensure ws_connected is FALSE at the start for consistent state */
    channel->ws_connected = FALSE;

    LOG_WITH_SID(channel, APT_PRIO_INFO, "Connecting to WebSocket server %s:%d%s", host, port, path ? path : "");

    /* Create socket */
    rv = apr_socket_create(&sock, APR_INET, SOCK_STREAM, APR_PROTO_TCP, pool);
    if (rv != APR_SUCCESS) {
        LOG_WITH_SID(channel, APT_PRIO_WARNING, "Failed to create socket: %d", rv);
        return FALSE;
    }

    /* Seed PRNG used for WebSocket masks */
    srand((unsigned int)apr_time_now());

    /* Set socket options */
    apr_socket_timeout_set(sock, 5 * 1000000);  /* 5 seconds timeout for ASR response */

    /* Enable TCP keepalive to detect broken connections */
    apr_socket_opt_set(sock, APR_SO_KEEPALIVE, 1);

    /* Set send buffer size */
    apr_socket_opt_set(sock, APR_SO_SNDBUF, 128 * 1024);  /* 128KB send buffer */

    /* Resolve address */
    rv = apr_sockaddr_info_get(&sa, host, APR_INET, port, 0, pool);
    if (rv != APR_SUCCESS) {
        LOG_WITH_SID(channel, APT_PRIO_WARNING, "Failed to resolve address: %d", rv);
        apr_socket_close(sock);
        return FALSE;
    }

    /* Connect */
    rv = apr_socket_connect(sock, sa);
    if (rv != APR_SUCCESS) {
        LOG_WITH_SID(channel, APT_PRIO_WARNING, "Failed to connect to %s:%d: %d", host, port, rv);
        apr_socket_close(sock);
        return FALSE;
    }

    /* Generate WebSocket key */
    ws_key = funasr_websocket_generate_key(pool);

    /* Log WebSocket request parameters */
    LOG_WITH_SID(channel, APT_PRIO_INFO,
            "========== WebSocket Request Parameters ==========\n"
            "      URL: ws://%s:%d%s?system_id=%s&scene_id=%s&call_id=%s&sample_rate=%d&n_channels=%d&sample_width=%d\n"
            "      Sample Rate: %d Hz\n"
            "      Channels: %d\n"
            "      Sample Width: %d bytes\n"
            "      Sec-WebSocket-Key: %s\n"
            "=======================================================",
            host, port, (path && *path) ? path : "/", "ncc", "outcall", channel->session_id, sample_rate, n_channels, sample_width,
            sample_rate, n_channels, sample_width, ws_key ? ws_key : "(null)");

    /* Send WebSocket handshake request with dynamic audio parameters */
    len = apr_snprintf(request, sizeof(request),
        "GET %s?system_id=%s&scene_id=%s&call_id=%s&sample_rate=%d&n_channels=%d&sample_width=%d HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        (path && *path) ? path : "/", "ncc", "outcall", channel->session_id, sample_rate, n_channels, sample_width, host, port, ws_key);

    rv = apr_socket_send(sock, request, &len);
    if (rv != APR_SUCCESS) {
        LOG_WITH_SID(channel, APT_PRIO_WARNING, "Failed to send WebSocket handshake");
        apr_socket_close(sock);
        return FALSE;
    }

    LOG_WITH_SID(channel, APT_PRIO_INFO, "WebSocket handshake sent");

    /* Receive handshake response (leave room for terminating NUL) */
    len = 0;
    while (len < sizeof(buffer) - 1) {
        apr_size_t chunk = sizeof(buffer) - 1 - len;
        rv = apr_socket_recv(sock, buffer + len, &chunk);
        if (rv != APR_SUCCESS || chunk == 0) {
            LOG_WITH_SID(channel, APT_PRIO_WARNING, "Failed to receive WebSocket handshake response");
            apr_socket_close(sock);
            return FALSE;
        }

        len += chunk;
        buffer[len] = '\0';
        if (strstr(buffer, "\r\n\r\n")) {
            break;
        }
    }

    if (!strstr(buffer, "\r\n\r\n")) {
        LOG_WITH_SID(channel, APT_PRIO_WARNING, "WebSocket handshake response too large");
        apr_socket_close(sock);
        return FALSE;
    }

    LOG_WITH_SID(channel, APT_PRIO_INFO, "WebSocket handshake response: %s", buffer);

    /* Check response (case-insensitive) */
    if ((!funasr_ws_strcasestr(buffer, "HTTP/1.1 101") && !funasr_ws_strcasestr(buffer, "101 Switching Protocols")) ||
        !funasr_ws_strcasestr(buffer, "upgrade: websocket") ||
        !funasr_ws_strcasestr(buffer, "connection: upgrade") ||
        !funasr_ws_strcasestr(buffer, "sec-websocket-accept")) {
        LOG_WITH_SID(channel, APT_PRIO_WARNING, "WebSocket handshake failed");
        apr_socket_close(sock);
        return FALSE;
    }

    /* Validate Sec-WebSocket-Accept */
    {
        char *accept_val = funasr_ws_get_header_value(pool, buffer, "Sec-WebSocket-Accept");
        char *expected = funasr_websocket_compute_accept(pool, ws_key);
        if (!accept_val || !expected || strcasecmp(accept_val, expected) != 0) {
            LOG_WITH_SID(channel, APT_PRIO_WARNING, "WebSocket accept mismatch");
            apr_socket_close(sock);
            return FALSE;
        }
    }

    channel->ws_socket = sock;
    channel->ws_connected = TRUE;
    /* Initialize timestamps for timeout detection */
    channel->last_valid_frame_time = apr_time_now();
    channel->request_start_time = apr_time_now();
    LOG_WITH_SID(channel, APT_PRIO_INFO, "WebSocket connected successfully");

    return TRUE;
}

/**
 * @brief Disconnect from WebSocket server
 * @param channel FunASR recognition channel
 */
static void funasr_websocket_disconnect(funasr_channel_t *channel)
{
    if (channel->ws_socket) {
        /* Send close frame */
        funasr_websocket_send_frame(channel, NULL, 0, 0x8);

        apr_socket_close(channel->ws_socket);
        channel->ws_socket = NULL;
        channel->ws_connected = FALSE;
        LOG_WITH_SID(channel, APT_PRIO_INFO, "WebSocket disconnected");
    }
}

/**
 * @brief Send data frame via WebSocket
 * @param channel FunASR recognition channel
 * @param data Data buffer
 * @param size Data size
 * @param opcode WebSocket opcode (1=text, 2=binary)
 * @return Success returns TRUE
 */
static apt_bool_t funasr_websocket_send_frame(funasr_channel_t *channel, const char *data, apr_size_t size, int opcode)
{
    apr_status_t rv;
    apr_size_t len;
    unsigned char header[14];
    unsigned char mask[4];
    int header_len = 2;
    int i;

    if (!channel->ws_socket || !channel->ws_connected) {
        return FALSE;
    }

    /* Build frame header */
    header[0] = 0x80 | (opcode & 0x0F);

    if (size < 126) {
        header[1] = 0x80 | (unsigned char)size;
        header_len = 2;
    } else if (size < 65536) {
        header[1] = 0x80 | 126;
        header[2] = (size >> 8) & 0xFF;
        header[3] = size & 0xFF;
        header_len = 4;
    } else {
        header[1] = 0x80 | 127;
        for (i = 0; i < 8; i++) {
            header[2 + i] = (size >> (56 - i * 8)) & 0xFF;
        }
        header_len = 10;
    }

    /* Generate mask */
    for (i = 0; i < 4; i++) {
        mask[i] = (unsigned char)(rand() & 0xFF);
    }

    /* Send header */
    len = header_len;
    rv = apr_socket_send(channel->ws_socket, (const char*)header, &len);
    if (rv != APR_SUCCESS || len != (apr_size_t)header_len) {
        char err_buf[256];
        apr_strerror(rv, err_buf, sizeof(err_buf));
        LOG_WITH_SID(channel, APT_PRIO_WARNING, "Failed to send WebSocket frame header: %s (rv=%d, sent=%d/%d), closing socket",
                err_buf, rv, (int)len, header_len);
        /* Close socket to prevent resource leak */
        apr_socket_close(channel->ws_socket);
        channel->ws_socket = NULL;
        channel->ws_connected = FALSE;
        return FALSE;
    }

    /* Send mask */
    len = 4;
    rv = apr_socket_send(channel->ws_socket, (const char*)mask, &len);
    if (rv != APR_SUCCESS || len != 4) {
        char err_buf[256];
        apr_strerror(rv, err_buf, sizeof(err_buf));
        LOG_WITH_SID(channel, APT_PRIO_WARNING, "Failed to send WebSocket frame mask: %s (rv=%d), closing socket", err_buf, rv);
        /* Close socket to prevent resource leak */
        apr_socket_close(channel->ws_socket);
        channel->ws_socket = NULL;
        channel->ws_connected = FALSE;
        return FALSE;
    }

    /* Send masked payload */
    if (data && size > 0) {
        unsigned char temp[4096];
        apr_size_t sent = 0;
        int chunk_count = 0;
        LOG_WITH_SID(channel, APT_PRIO_INFO, "Starting to send WebSocket audio frame, total_size=%d bytes", (int)size);
        while (sent < size) {
            apr_size_t chunk = size - sent;
            if (chunk > sizeof(temp)) {
                chunk = sizeof(temp);
            }
            apr_size_t i;
            for ( i = 0; i < chunk; i++) {
                temp[i] = ((const unsigned char*)data)[sent + i] ^ mask[(sent + i) % 4];
            }
            len = chunk;
            rv = apr_socket_send(channel->ws_socket, (const char*)temp, &len);
            chunk_count++;
            if (rv != APR_SUCCESS || len != chunk) {
                char err_buf[256];
                apr_strerror(rv, err_buf, sizeof(err_buf));
                LOG_WITH_SID(channel, APT_PRIO_WARNING, "Failed to send WebSocket frame payload chunk %d: %s (rv=%d, expected=%d, sent=%d, total_sent=%d/%d), closing socket",
                        chunk_count, err_buf, rv, (int)chunk, (int)len, (int)sent, (int)size);
                /* Close socket to prevent resource leak */
                apr_socket_close(channel->ws_socket);
                channel->ws_socket = NULL;
                channel->ws_connected = FALSE;
                return FALSE;
            }
            sent += len;  /* Use actual len sent, not chunk */
            LOG_WITH_SID(channel, APT_PRIO_DEBUG, "Sent chunk %d: %d bytes (total: %d/%d)", chunk_count, (int)len, (int)sent, (int)size);
        }
        LOG_WITH_SID(channel, APT_PRIO_INFO, "Successfully sent WebSocket audio frame to ASR server, size=%d bytes in %d chunks",
                (int)sent, chunk_count);

        /* Verify socket is still connected after send */
        if (!channel->ws_socket || !channel->ws_connected) {
            LOG_WITH_SID(channel, APT_PRIO_WARNING, "WebSocket connection state invalid after send (socket=%p, connected=%d)",
                    channel->ws_socket, channel->ws_connected);
            return FALSE;
        }

        /* Try to flush data to network - use TCP_NODELAY or send small probe */
        /* Check if socket has any errors */
        apr_sockaddr_t *sock_addr = NULL;
        if (apr_socket_addr_get(&sock_addr, APR_REMOTE, channel->ws_socket) != APR_SUCCESS) {
            LOG_WITH_SID(channel, APT_PRIO_WARNING, "Failed to get socket address after send, connection may be broken");
            return FALSE;
        }


    }

    return TRUE;
}

static apt_bool_t funasr_websocket_send(funasr_channel_t *channel, const char *data, apr_size_t size)
{
    return funasr_websocket_send_frame(channel, data, size, 2);
}

/**
 * @brief Flush WebSocket send buffer (send accumulated data)
 * @param recog_channel FunASR recognition channel
 * 说明：
 *   发送缓冲区中累积的音频数据到ASR服务器
 *   用于确保每个chunk至少为200ms（6400字节）
 */
static void funasr_ws_send_flush(funasr_channel_t *recog_channel)
{
    if (!recog_channel->ws_connected || recog_channel->ws_send_size == 0) {
        return;
    }

    LOG_WITH_SID(recog_channel, APT_PRIO_INFO,
            "Flushing WebSocket send buffer size=%d bytes (%d ms)",
            (int)recog_channel->ws_send_size,
            (int)(recog_channel->ws_send_size * 1000 / (FUNASR_SAMPLE_RATE * FUNASR_SAMPLE_WIDTH)));

    /* Send buffered data */
    apt_bool_t send_result = funasr_websocket_send(recog_channel, recog_channel->ws_send_buffer, recog_channel->ws_send_size);

    /* Only reset buffer if send was successful */
    if (send_result) {
        recog_channel->ws_send_size = 0;
        /* Update last audio send time ONLY when actually sent over network */
        recog_channel->last_audio_send_time = apr_time_now();
    } else {
        LOG_WITH_SID(recog_channel, APT_PRIO_WARNING, "Failed to flush WebSocket send buffer, keeping data in buffer for retry");
    }
}

/**
 * @brief Send audio end marker to ASR server
 * @param recog_channel FunASR recognition channel
 * @return TRUE if sent successfully, FALSE otherwise
 * 说明：
 *   发送音频结束标记，告诉ASR服务器当前轮次的音频已发送完毕
 *   服务器收到此标记后应该开始识别并返回结果
 *   发送一个空的二进制帧（opcode 0x2, payload length 0）
 */
static apt_bool_t funasr_ws_send_end_frame(funasr_channel_t *recog_channel)
{
    if (!recog_channel->ws_connected) {
        LOG_WITH_SID(recog_channel, APT_PRIO_WARNING, "WebSocket not connected, cannot send end frame");
        return FALSE;
    }

    /* First flush any remaining buffered audio */
    funasr_ws_send_flush(recog_channel);

    /* Send empty binary frame as end marker (opcode 0x2, payload length 0) */
    LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "========== Sending audio end marker to ASR server ==========");

    /* Reuse existing frame send function to avoid code duplication */
    apt_bool_t result = funasr_websocket_send_frame(recog_channel, NULL, 0, 0x2);

    if (result) {
        LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "Audio end marker sent successfully");
    } else {
        LOG_WITH_SID(recog_channel, APT_PRIO_WARNING, "Failed to send audio end marker");
    }

    return result;
}

/**
 * @brief Buffer and send audio data in 200ms chunks
 * @param recog_channel FunASR recognition channel
 * @param data Audio data to send
 * @param size Size of audio data
 * 说明：
 *   将音频数据累积到缓冲区，当缓冲区达到200ms（6400字节）时发送
 *   确保每个发送的chunk满足ASR服务器的要求
 */
static void funasr_ws_send_buffered(funasr_channel_t *recog_channel, const char *data, apr_size_t size)
{
    if (!recog_channel->ws_connected || !data || size == 0) {
        return;
    }

    apr_size_t remaining = size;
    apr_size_t offset = 0;

    while (remaining > 0) {
        apr_size_t available = FUNASR_WS_SEND_CHUNK_SIZE - recog_channel->ws_send_size;
        apr_size_t to_copy = (remaining < available) ? remaining : available;

        /* Copy data to buffer */
        memcpy(recog_channel->ws_send_buffer + recog_channel->ws_send_size,
               data + offset,
               to_copy);
        recog_channel->ws_send_size += to_copy;
        offset += to_copy;
        remaining -= to_copy;

        /* Send buffer if full (200ms chunk) */
        if (recog_channel->ws_send_size >= FUNASR_WS_SEND_CHUNK_SIZE) {
            LOG_WITH_SID(recog_channel, APT_PRIO_INFO,
                    "Sending WebSocket chunk size=%d bytes (200ms)",
                    (int)recog_channel->ws_send_size);
            apt_bool_t send_result = funasr_websocket_send(recog_channel, recog_channel->ws_send_buffer, recog_channel->ws_send_size);
            if (send_result) {
                recog_channel->ws_send_size = 0;
                /* Update last audio send time ONLY when actually sent over network */
                recog_channel->last_audio_send_time = apr_time_now();
            } else {
                LOG_WITH_SID(recog_channel, APT_PRIO_WARNING, "Failed to send WebSocket chunk, aborting further sends");
                /* Don't reset buffer - keep data for potential retry, but stop processing */
                break;
            }
        }
    }
}

/* Resample 16-bit PCM mono/stereo from 8k to 16k (simple linear interpolation)
 * Returns newly allocated buffer in pool and sets out_size. Channels must be >=1.
 *
 * Cross-frame continuity: prev_samples[2] / *state_valid carry the last sample
 * of each channel from the previous frame.  On first call set *state_valid=FALSE;
 * the function will set *state_valid=TRUE and populate prev_samples for the next
 * call.  This eliminates the click/pop that would otherwise occur at every frame
 * boundary when resampling is done per-frame instead of on the complete audio.
 */
char* funasr_resample_8k_to_16k(apr_pool_t *pool, const char *in_buf, apr_size_t in_size, apr_size_t *out_size, int channels,
                                 int16_t prev_samples[2], apt_bool_t *state_valid)
{
    const char *data_ptr;
    const char *chunk;
    const unsigned char *fmt;
    const int16_t *in_samples;
    apr_size_t data_size;
    apr_size_t offset;
    apr_size_t bytes_per_sample;
    apr_size_t frame_samples;
    apr_size_t out_frame_samples;
    apr_size_t total_out_samples;
    apr_size_t buf_size;
    apr_size_t n;
    apr_size_t in_idx;
    apr_size_t out_idx1;
    apr_size_t out_idx2;
    int used_channels;
    int ch;
    uint32_t chunk_size;
    uint16_t audio_format;
    uint16_t wav_channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    int16_t *out_samples;
    int16_t s;
    int16_t s_next;
    char *out;

    /* Validate input parameters - pool must be valid for memory management */
    if (!pool || !in_buf || in_size == 0 || channels <= 0) {
        *out_size = 0;
        return NULL;
    }

    /* Support raw 16-bit PCM or WAV (RIFF/WAVE) input. If WAV, find "data" chunk and fmt info. */
    data_ptr = in_buf;
    data_size = in_size;
    used_channels = channels;

    if (in_size >= 12 && memcmp(in_buf, "RIFF", 4) == 0 && memcmp(in_buf + 8, "WAVE", 4) == 0) {
        offset = 12;
        while (offset + 8 <= in_size) {
            chunk = in_buf + offset;
            chunk_size = (uint32_t)((unsigned char)chunk[4] | ((unsigned char)chunk[5] << 8) | ((unsigned char)chunk[6] << 16) | ((unsigned char)chunk[7] << 24));
            if (offset + 8 + chunk_size > in_size) break;

            if (memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
                fmt = (const unsigned char*)(chunk + 8);
                audio_format = (uint16_t)(fmt[0] | (fmt[1] << 8));
                wav_channels = (uint16_t)(fmt[2] | (fmt[3] << 8));
                sample_rate = (uint32_t)(fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24));
                bits_per_sample = (uint16_t)(fmt[14] | (fmt[15] << 8));
                /* Only support PCM 16-bit here */
                if (audio_format != 1 || bits_per_sample != 16) {
                    *out_size = 0;
                    return NULL;
                }
                used_channels = (int)wav_channels;
                (void)sample_rate; /* sample_rate validated by caller context if needed */
            } else if (memcmp(chunk, "data", 4) == 0) {
                data_ptr = chunk + 8;
                data_size = (apr_size_t)chunk_size;
                break;
            }

            offset += 8 + chunk_size;
            if (chunk_size & 1) offset++; /* pad */
        }
    }

    if (data_size == 0) {
        *out_size = 0;
        return NULL;
    }

    in_samples = (const int16_t*)data_ptr;
    bytes_per_sample = sizeof(int16_t);
    frame_samples = data_size / (bytes_per_sample * used_channels);
    out_frame_samples = frame_samples * 2; /* double sample rate */
    total_out_samples = out_frame_samples * used_channels;
    buf_size = total_out_samples * bytes_per_sample;

    /* Allocate output buffer from APR pool - managed automatically */
    out = apr_palloc(pool, buf_size);
    if (!out) {
        *out_size = 0;
        return NULL;
    }
    out_samples = (int16_t*)out;

    for (n = 0; n < frame_samples; n++) {
        for (ch = 0; ch < used_channels; ch++) {
            in_idx = n * used_channels + ch;
            out_idx1 = (n * 2) * used_channels + ch;
            out_idx2 = (n * 2 + 1) * used_channels + ch;
            s = in_samples[in_idx];
            s_next = s;
            if (n + 1 < frame_samples) {
                s_next = in_samples[(n + 1) * used_channels + ch];
            }
            out_samples[out_idx1] = s;
            /* Round-to-nearest average for the interpolated sample (fixes DC bias
             * from truncation-toward-zero on negative values). */
            out_samples[out_idx2] = (int16_t)(((int)s + (int)s_next + 1) >> 1);
        }
    }

    /* Cross-frame continuity: if we have a valid previous-frame state,
     * replace the first interpolated sample of each channel with an average
     * of the previous frame's last sample and this frame's first sample.
     * This eliminates the discontinuity at frame boundaries. */
    if (*state_valid) {
        for (ch = 0; ch < used_channels; ch++) {
            int16_t first_sample = in_samples[ch];  /* first sample of this frame, channel ch */
            int16_t prev_last = prev_samples[ch];    /* last sample of previous frame */
            out_samples[ch] = (int16_t)(((int)prev_last + (int)first_sample + 1) >> 1);
        }
    }

    /* Save last sample of each channel for next frame's cross-frame interpolation */
    for (ch = 0; ch < used_channels && ch < 2; ch++) {
        prev_samples[ch] = in_samples[(frame_samples - 1) * used_channels + ch];
    }
    *state_valid = TRUE;

    *out_size = buf_size;
    return out;
}

/**
 * @brief Receive data frame via WebSocket
 * @param channel FunASR recognition channel
 * @param size Output parameter, received data size
 * @return Success returns response data
 */
static char* funasr_websocket_recv(funasr_channel_t *channel, apr_size_t *size)
{
    if (!channel->ws_socket || !channel->ws_connected) {
        return NULL;
    }

    char *message = NULL;
    apr_size_t message_len = 0;

    while (1) {
        unsigned char header[14];
        char *payload = NULL;
        apr_size_t len = 2;
        apr_uint64_t payload_len = 0;
        int fin;
        int opcode;
        int masked;
        int i;
        unsigned char mask[4];

        /* Read first 2 bytes of header */
        if (apr_socket_recv(channel->ws_socket, (char*)header, &len) != APR_SUCCESS || len != 2) {
            LOG_WITH_SID(channel, APT_PRIO_WARNING, "Failed to read WebSocket frame header, closing socket");
            apr_socket_close(channel->ws_socket);
            channel->ws_socket = NULL;
            channel->ws_connected = FALSE;
            return NULL;
        }

        fin = (header[0] >> 7) & 0x01;
        opcode = header[0] & 0x0F;
        masked = (header[1] >> 7) & 0x01;
        payload_len = header[1] & 0x7F;

        LOG_WITH_SID(channel, APT_PRIO_DEBUG, "WS frame - FIN=%d, Opcode=%d, PayloadLen=%d",
                fin, opcode, (int)payload_len);

        /* Handle extended payload length */
        if (payload_len == 126) {
            len = 2;
            if (apr_socket_recv(channel->ws_socket, (char*)header + 2, &len) != APR_SUCCESS || len != 2) {
                LOG_WITH_SID(channel, APT_PRIO_WARNING, "Failed to read extended payload length, closing socket");
                apr_socket_close(channel->ws_socket);
                channel->ws_socket = NULL;
                channel->ws_connected = FALSE;
                return NULL;
            }
            payload_len = ((unsigned char)header[2] << 8) | (unsigned char)header[3];
        } else if (payload_len == 127) {
            len = 8;
            if (apr_socket_recv(channel->ws_socket, (char*)header + 2, &len) != APR_SUCCESS || len != 8) {
                LOG_WITH_SID(channel, APT_PRIO_WARNING, "Failed to read extended payload length, closing socket");
                apr_socket_close(channel->ws_socket);
                channel->ws_socket = NULL;
                channel->ws_connected = FALSE;
                return NULL;
            }
            payload_len = 0;
            for (i = 0; i < 8; i++) {
                payload_len = (payload_len << 8) | (unsigned char)header[2 + i];
            }
        }

        /* Read mask if present */
        if (masked) {
            len = 4;
            if (apr_socket_recv(channel->ws_socket, (char*)mask, &len) != APR_SUCCESS || len != 4) {
                LOG_WITH_SID(channel, APT_PRIO_WARNING, "Failed to read mask, closing socket");
                apr_socket_close(channel->ws_socket);
                channel->ws_socket = NULL;
                channel->ws_connected = FALSE;
                return NULL;
            }
        }

        /* Read payload */
        if (payload_len > 0) {
            payload = apr_palloc(channel->channel->pool, (apr_size_t)payload_len + 1);
            apr_size_t total_read = 0;
            while (total_read < (apr_size_t)payload_len) {
                len = (apr_size_t)payload_len - total_read;
                apr_status_t rv = apr_socket_recv(channel->ws_socket, payload + total_read, &len);
                if (rv != APR_SUCCESS || len == 0) {
                    LOG_WITH_SID(channel, APT_PRIO_WARNING, "Failed to read WebSocket payload, closing socket");
                    apr_socket_close(channel->ws_socket);
                    channel->ws_socket = NULL;
                    channel->ws_connected = FALSE;
                    return NULL;
                }
                total_read += len;
            }

            /* Unmask if needed */
            if (masked) {
                apr_size_t i;
                for ( i = 0; i < (apr_size_t)payload_len; i++) {
                    payload[i] ^= mask[i % 4];
                }
            }

            payload[payload_len] = '\0';

            /* Debug: Print raw ASR response bytes only for data frames */
            if (opcode == 0x1 || opcode == 0x2 || opcode == 0x0) {
                LOG_WITH_SID(channel, APT_PRIO_INFO, "========== Raw ASR Response (blocking recv) ==========");
                LOG_WITH_SID(channel, APT_PRIO_INFO, "Payload size: %d bytes", (int)payload_len);
                apr_size_t hex_len = payload_len < 500 ? payload_len : 500;
                char *hex_dump = apr_palloc(channel->channel->pool, hex_len * 3 + 1);
                for (i = 0; i < hex_len; i++) {
                    sprintf(hex_dump + i * 3, "%02x ", (unsigned char)payload[i]);
                }
                LOG_WITH_SID(channel, APT_PRIO_INFO, "Raw bytes (hex): %s", hex_dump);
                LOG_WITH_SID(channel, APT_PRIO_INFO, "Raw bytes (as text): %s", payload);
                LOG_WITH_SID(channel, APT_PRIO_INFO, "===============================================================");
            }
            LOG_WITH_SID(channel, APT_PRIO_INFO, "===============================================================");
        }

        /* Check if this is a valid opcode frame */
        apt_bool_t is_valid_frame = FALSE;
        if (opcode == 0x8) {
            funasr_websocket_send_frame(channel, payload, (apr_size_t)payload_len, 0x8);
            return NULL;
        } else if (opcode == 0x9) {
            funasr_websocket_send_frame(channel, payload, (apr_size_t)payload_len, 0xA);
            is_valid_frame = TRUE;
        } else if (opcode == 0xA) {
            is_valid_frame = TRUE;
        } else if (opcode == 0x1 || opcode == 0x2) {
            /* Start a new message (text or binary) */
            message_len = 0;
            if (payload_len > 0) {
                message = apr_palloc(channel->channel->pool, (apr_size_t)payload_len + 1);
                memcpy(message, payload, (apr_size_t)payload_len);
                message_len = (apr_size_t)payload_len;
                message[message_len] = '\0';
            } else {
                message = apr_palloc(channel->channel->pool, 1);
                message[0] = '\0';
            }
            is_valid_frame = TRUE;
        } else if (opcode == 0x0) {
            if (!message) {
                LOG_WITH_SID(channel, APT_PRIO_WARNING, "Unexpected continuation frame");
                return NULL;
            }
            if (payload_len > 0) {
                char *new_buf = apr_palloc(channel->channel->pool, message_len + (apr_size_t)payload_len + 1);
                memcpy(new_buf, message, message_len);
                memcpy(new_buf + message_len, payload, (apr_size_t)payload_len);
                message_len += (apr_size_t)payload_len;
                new_buf[message_len] = '\0';
                message = new_buf;
            }
            is_valid_frame = TRUE;
        } else {
            /* Unknown opcode - log warning */
            LOG_WITH_SID(channel, APT_PRIO_WARNING, "Unknown WebSocket opcode=%d, header=%02x %02x",
                    opcode, header[0], header[1]);
        }

        /* Check total timeout (60 seconds from request start) */
        apr_interval_time_t total_elapsed = apr_time_now() - channel->request_start_time;
        if (total_elapsed > 60 * 1000000) {
            LOG_WITH_SID(channel, APT_PRIO_ERROR,
                    "========== TOTAL TIMEOUT (60 seconds) - ASR server not responding ==========\n"
                    "      Elapsed time: %d seconds\n"
                    "      Disconnecting WebSocket connection\n"
                    "===========================================================================",
                    (int)(total_elapsed / 1000000));
            funasr_websocket_disconnect(channel);
            return NULL;
        }

        /* Update valid frame timestamp for known opcodes */
        if (is_valid_frame) {
            channel->last_valid_frame_time = apr_time_now();
        } else {
            /* Check timeout: disconnect if 30 seconds without valid frames */
            apr_interval_time_t elapsed = apr_time_now() - channel->last_valid_frame_time;
            if (elapsed > 30 * 1000000) {
                LOG_WITH_SID(channel, APT_PRIO_ERROR,
                        "========== NO VALID FRAMES TIMEOUT (30 seconds) ==========\n"
                        "      Elapsed time since last valid frame: %d seconds\n"
                        "      Total elapsed time: %d seconds\n"
                        "      Disconnecting WebSocket connection\n"
                        "===================================================================",
                        (int)(elapsed / 1000000), (int)(total_elapsed / 1000000));
                funasr_websocket_disconnect(channel);
                return NULL;
            }
            /* Continue to next frame */
            continue;
        }

        if (message && fin == 1) {
            *size = message_len;
            return message;
        }
    }
}

/**
 * @brief Non-blocking receive data frame via WebSocket
 * @param channel FunASR recognition channel
 * @param size Output parameter, received data size
 * @return Success returns response data, NULL if no data available or error
 */
static char* funasr_websocket_recv_nonblock(funasr_channel_t *channel, apr_size_t *size)
{
    apr_size_t i;

    if (!channel->ws_socket || !channel->ws_connected) {
        return NULL;
    }

    /* Save current timeout and set to non-blocking (0) */
    apr_interval_time_t orig_timeout;
    apr_socket_timeout_get(channel->ws_socket, &orig_timeout);
    apr_socket_timeout_set(channel->ws_socket, 0);  /* Non-blocking */

    /* Try to read first 2 bytes of header */
    unsigned char header[2];
    apr_size_t len = 2;
    apr_status_t rv = apr_socket_recv(channel->ws_socket, (char*)header, &len);

    if (rv != APR_SUCCESS || len != 2) {
        /* No data available or error - restore timeout and return */
        apr_socket_timeout_set(channel->ws_socket, orig_timeout);
        return NULL;
    }

    /* Data available - restore timeout and use blocking recv for the rest */
    apr_socket_timeout_set(channel->ws_socket, orig_timeout);

    int fin = (header[0] >> 7) & 0x01;
    int opcode = header[0] & 0x0F;
    int masked = (header[1] >> 7) & 0x01;
    apr_uint64_t payload_len = header[1] & 0x7F;

    LOG_WITH_SID(channel, APT_PRIO_DEBUG, "Nonblock WS frame - FIN=%d, Opcode=%d, PayloadLen=%d",
            fin, opcode, (int)payload_len);

    /* Handle extended payload length */
    if (payload_len == 126) {
        len = 2;
        if (apr_socket_recv(channel->ws_socket, (char*)header, &len) != APR_SUCCESS || len != 2) {
            /* Failed to read extended length, close socket to prevent resource leak */
            LOG_WITH_SID(channel, APT_PRIO_WARNING, "Failed to read WebSocket extended payload length, closing socket");
            apr_socket_close(channel->ws_socket);
            channel->ws_socket = NULL;
            channel->ws_connected = FALSE;
            return NULL;
        }
        payload_len = ((unsigned char)header[0] << 8) | (unsigned char)header[1];
    } else if (payload_len == 127) {
        unsigned char ext_len[8];
        len = 8;
        if (apr_socket_recv(channel->ws_socket, (char*)ext_len, &len) != APR_SUCCESS || len != 8) {
            /* Failed to read extended length, close socket to prevent resource leak */
            LOG_WITH_SID(channel, APT_PRIO_WARNING, "Failed to read WebSocket extended payload length (8 bytes), closing socket");
            apr_socket_close(channel->ws_socket);
            channel->ws_socket = NULL;
            channel->ws_connected = FALSE;
            return NULL;
        }
        payload_len = 0;
        int i;
        for (i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | ext_len[i];
        }
    }

    /* Read mask if present */
    unsigned char mask[4] = {0};
    if (masked) {
        len = 4;
        if (apr_socket_recv(channel->ws_socket, (char*)mask, &len) != APR_SUCCESS || len != 4) {
            /* Failed to read mask, close socket to prevent resource leak */
            LOG_WITH_SID(channel, APT_PRIO_WARNING, "Failed to read WebSocket mask, closing socket");
            apr_socket_close(channel->ws_socket);
            channel->ws_socket = NULL;
            channel->ws_connected = FALSE;
            return NULL;
        }
    }

    /* Handle control frames and check for valid frames */
    apt_bool_t is_valid_frame = FALSE;
    if (opcode == 0x8) {
        /* Close frame - server initiated close, properly close our socket */
        LOG_WITH_SID(channel, APT_PRIO_INFO, "Received WebSocket close frame from server, closing connection");
        if (channel->ws_socket) {
            apr_socket_close(channel->ws_socket);
            channel->ws_socket = NULL;
        }
        channel->ws_connected = FALSE;
        return NULL;
    } else if (opcode == 0x9) {
        /* Ping - send pong */
        LOG_WITH_SID(channel, APT_PRIO_INFO, "Received WebSocket ping frame, payload_len=%d bytes", (int)payload_len);

        /* Ensure socket is in blocking mode for reading ping data */
        apr_interval_time_t current_timeout;
        int timeout_changed = 0; /* flag whether we changed timeout and need to restore */
        apr_socket_timeout_get(channel->ws_socket, &current_timeout);
        if (current_timeout == 0) {
            /* Socket is still in non-blocking mode, set to blocking */
            LOG_WITH_SID(channel, APT_PRIO_WARNING, "Socket still in non-blocking mode while reading ping, setting to blocking (timeout=30s)");
            apr_socket_timeout_set(channel->ws_socket, 30 * 1000000);
            timeout_changed = 1;  /* Remember to restore original timeout */
        }

        if (payload_len > 0) {
            char *ping_data = apr_palloc(channel->channel->pool, (apr_size_t)payload_len);
            apr_size_t total_read = 0;
            LOG_WITH_SID(channel, APT_PRIO_DEBUG, "Starting to read ping data, expected %d bytes", (int)payload_len);
            while (total_read < (apr_size_t)payload_len) {
                len = (apr_size_t)payload_len - total_read;
                apr_status_t ping_rv = apr_socket_recv(channel->ws_socket, ping_data + total_read, &len);
                if (ping_rv != APR_SUCCESS) {
                    /* Restore timeout before closing socket if we changed it */
                    if (timeout_changed && channel->ws_socket) {
                        apr_socket_timeout_set(channel->ws_socket, current_timeout);
                    }
                    /* Failed to read ping data, close socket to prevent resource leak */
                    LOG_WITH_SID(channel, APT_PRIO_WARNING, "Failed to read WebSocket ping data at offset %d/%d (rv=%d), closing socket",
                            (int)total_read, (int)payload_len, ping_rv);
                    apr_socket_close(channel->ws_socket);
                    channel->ws_socket = NULL;
                    channel->ws_connected = FALSE;
                    return NULL;
                }
                if (len == 0) {
                    /* Restore timeout before closing socket if we changed it */
                    if (timeout_changed && channel->ws_socket) {
                        apr_socket_timeout_set(channel->ws_socket, current_timeout);
                    }
                    /* Connection closed prematurely */
                    LOG_WITH_SID(channel, APT_PRIO_WARNING, "WebSocket connection closed while reading ping data (read %d/%d), closing socket",
                            (int)total_read, (int)payload_len);
                    apr_socket_close(channel->ws_socket);
                    channel->ws_socket = NULL;
                    channel->ws_connected = FALSE;
                    return NULL;
                }
                total_read += len;
                LOG_WITH_SID(channel, APT_PRIO_DEBUG, "Read ping chunk: %d bytes (total: %d/%d)", (int)len, (int)total_read, (int)payload_len);
            }
            LOG_WITH_SID(channel, APT_PRIO_INFO, "Successfully read ping data, size=%d bytes, sending pong", (int)payload_len);

            /* Restore original timeout before sending pong if we changed it */
            if (timeout_changed && channel->ws_socket) {
                apr_socket_timeout_set(channel->ws_socket, current_timeout);
            }

            funasr_websocket_send_frame(channel, ping_data, (apr_size_t)payload_len, 0xA);
        } else {
            LOG_WITH_SID(channel, APT_PRIO_INFO, "Received empty ping frame, sending empty pong");

            /* Restore original timeout before sending pong if we changed it */
            if (timeout_changed && channel->ws_socket) {
                apr_socket_timeout_set(channel->ws_socket, current_timeout);
            }

            funasr_websocket_send_frame(channel, NULL, 0, 0xA);
        }
        is_valid_frame = TRUE;
    } else if (opcode == 0xA) {
        /* Pong - ignore */
        is_valid_frame = TRUE;
    } else if (opcode != 0x0 && opcode != 0x1 && opcode != 0x2) {
        /* Unknown opcode - print hex dump for debugging */
        LOG_WITH_SID(channel, APT_PRIO_WARNING, "Unknown WebSocket opcode=%d (non-blocking), header bytes: %02x %02x",
                opcode, (unsigned char)header[0], (unsigned char)header[1]);
        /* Don't return immediately, try to read and discard payload to maintain sync */
        if (payload_len > 0 && payload_len < 1000000) {  /* Sanity check */
            LOG_WITH_SID(channel, APT_PRIO_WARNING, "Attempting to discard %d bytes of unknown frame payload", (int)payload_len);
            char *discard_buf = apr_palloc(channel->channel->pool, (apr_size_t)payload_len);
            apr_size_t total_discarded = 0;
            while (total_discarded < (apr_size_t)payload_len) {
                apr_size_t chunk = (apr_size_t)payload_len - total_discarded;
                apr_status_t discard_rv = apr_socket_recv(channel->ws_socket, discard_buf + total_discarded, &chunk);
                if (discard_rv != APR_SUCCESS || chunk == 0) {
                    LOG_WITH_SID(channel, APT_PRIO_WARNING, "Failed to discard unknown frame payload at offset %d/%d",
                            (int)total_discarded, (int)payload_len);
                    break;
                }
                total_discarded += chunk;
            }
            LOG_WITH_SID(channel, APT_PRIO_WARNING, "Discarded %d bytes of unknown frame payload", (int)total_discarded);
        }
    } else {
        /* Valid data frame (0x0, 0x1, 0x2) */
        is_valid_frame = TRUE;
    }

    /* Check total timeout (60 seconds from request start) */
    apr_interval_time_t total_elapsed = apr_time_now() - channel->request_start_time;
    if (total_elapsed > 60 * 1000000) {
        LOG_WITH_SID(channel, APT_PRIO_ERROR,
                "========== TOTAL TIMEOUT (60 seconds) - ASR server not responding (non-blocking) ==========\n"
                "      Elapsed time: %d seconds\n"
                "      Disconnecting WebSocket connection\n"
                "================================================================================================",
                (int)(total_elapsed / 1000000));
        funasr_websocket_disconnect(channel);
        return NULL;
    }

    /* Check timeout for unknown opcodes */
    if (!is_valid_frame) {
        apr_interval_time_t elapsed = apr_time_now() - channel->last_valid_frame_time;
        if (elapsed > 30 * 1000000) {
            LOG_WITH_SID(channel, APT_PRIO_ERROR,
                    "========== NO VALID FRAMES TIMEOUT (30 seconds) (non-blocking) ==========\n"
                    "      Elapsed time since last valid frame: %d seconds\n"
                    "      Total elapsed time: %d seconds\n"
                    "      Disconnecting WebSocket connection\n"
                    "===================================================================================",
                    (int)(elapsed / 1000000), (int)(total_elapsed / 1000000));
            funasr_websocket_disconnect(channel);
            return NULL;
        }
        return NULL;
    }

    /* Update valid frame timestamp */
    channel->last_valid_frame_time = apr_time_now();

    /* Read payload for data frames only (control frame payloads are already handled above) */
    char *payload = NULL;
    if (payload_len > 0 && (opcode == 0x0 || opcode == 0x1 || opcode == 0x2)) {
        payload = apr_palloc(channel->channel->pool, (apr_size_t)payload_len + 1);
        apr_size_t total_read = 0;
        while (total_read < (apr_size_t)payload_len) {
            len = (apr_size_t)payload_len - total_read;
            if (apr_socket_recv(channel->ws_socket, payload + total_read, &len) != APR_SUCCESS || len == 0) {
                /* Failed to read payload, close socket to prevent resource leak */
                LOG_WITH_SID(channel, APT_PRIO_WARNING, "Failed to read WebSocket payload, closing socket");
                apr_socket_close(channel->ws_socket);
                channel->ws_socket = NULL;
                channel->ws_connected = FALSE;
                return NULL;
            }
            total_read += len;
        }

        /* Unmask if needed */
        if (masked) {
            apr_size_t i;
            for (i = 0; i < (apr_size_t)payload_len; i++) {
                payload[i] ^= mask[i % 4];
            }
        }

        payload[payload_len] = '\0';

        /* Debug: Print raw ASR response bytes for format analysis (non-blocking recv) */
        LOG_WITH_SID(channel, APT_PRIO_INFO, "========== Raw ASR Response (non-blocking recv) ==========");
        LOG_WITH_SID(channel, APT_PRIO_INFO, "Payload size: %d bytes", (int)payload_len);
        /* Print hex dump (first 500 bytes) */
        apr_size_t hex_len = payload_len < 500 ? payload_len : 500;
        char *hex_dump = apr_palloc(channel->channel->pool, hex_len * 3 + 1);
        for (i = 0; i < hex_len; i++) {
            sprintf(hex_dump + i * 3, "%02x ", (unsigned char)payload[i]);
        }
        LOG_WITH_SID(channel, APT_PRIO_INFO, "Raw bytes (hex): %s", hex_dump);
        LOG_WITH_SID(channel, APT_PRIO_INFO, "Raw bytes (as text): %s", payload);
        LOG_WITH_SID(channel, APT_PRIO_INFO, "===============================================================");
    }

    /* Only return complete messages (fin == 1) for text/binary frames */
    if ((opcode == 0x1 || opcode == 0x2) && fin == 1) {
        *size = (apr_size_t)payload_len;
        return payload;
    }

    return NULL;
}

/**
 * @brief 发送消息到消费者任务队列
 * @param type 消息类型（打开通道/关闭通道/处理请求）
 * @param channel MRCP引擎通道对象
 * @param request MRCP请求消息（REQUEST_PROCESS时有效）
 * @return 成功返回TRUE，失败返回FALSE
 * 说明：
 *   1. 获取FunASR引擎和消费者任务
 *   2. 从任务池获取消息对象
 *   3. 设置消息类型和参数
 *   4. 通过apt_task_msg_signal发送到队列
 */
static apt_bool_t funasr_msg_signal(funasr_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
    apt_bool_t status = FALSE;
    funasr_channel_t *funasr_channel = channel->method_obj;
    funasr_engine_t *funasr_engine = funasr_channel->funasr_engine;
    apt_task_t *task = apt_consumer_task_base_get(funasr_engine->task);
    apt_task_msg_t *msg = apt_task_msg_get(task);

    if (msg) {
        funasr_msg_t *funasr_msg;
        msg->type = TASK_MSG_USER;
        funasr_msg = (funasr_msg_t*) msg->data;

        funasr_msg->type = type;
        funasr_msg->channel = channel;
        funasr_msg->request = request;
        status = apt_task_msg_signal(task, msg);
    }
    return status;
}

/**
 * @brief 处理消费者任务队列中的消息（在任务线程中运行）
 * @param task 任务对象
 * @param msg 消息对象
 * @return 成功返回TRUE
 * 说明：
 *   1. 记录日志：处理消息（DEBUG级别）
 *   2. 根据消息类型分发处理：
 *      - OPEN_CHANNEL: 发送通道打开响应
 *      - CLOSE_CHANNEL: 发送通道关闭响应
 *      - REQUEST_PROCESS: 调用请求分发函数
 *      - 未知类型: 记录警告日志
 */
static apt_bool_t funasr_msg_process(apt_task_t *task, apt_task_msg_t *msg)
{
    funasr_msg_t *funasr_msg = (funasr_msg_t*)msg->data;

    /* Log when message is processed */
    apt_log(APT_LOG_MARK, APT_PRIO_DEBUG, "zyASR: Process msg type=%d", funasr_msg->type);

    switch (funasr_msg->type) {
        case FUNASR_MSG_OPEN_CHANNEL:
            apt_log(APT_LOG_MARK, APT_PRIO_INFO, "zyASR: Open channel msg");
            mrcp_engine_channel_open_respond(funasr_msg->channel, TRUE);
            break;
        case FUNASR_MSG_CLOSE_CHANNEL:
            apt_log(APT_LOG_MARK, APT_PRIO_INFO, "zyASR: Close channel msg");
            /* Close WebSocket connection when MRCP channel is closed */
            funasr_channel_t *recog_channel = funasr_msg->channel->method_obj;
            if (recog_channel && recog_channel->ws_connected) {
                LOG_WITH_SID(recog_channel, APT_PRIO_INFO, "Closing WebSocket connection due to MRCP channel close");
                funasr_websocket_disconnect(recog_channel);
            }
            mrcp_engine_channel_close_respond(funasr_msg->channel);
            break;
        case FUNASR_MSG_REQUEST_PROCESS:
            apt_log(APT_LOG_MARK, APT_PRIO_INFO, "zyASR: Process request method=%d",
                    funasr_msg->request ? funasr_msg->request->start_line.method_id : -1);
            funasr_channel_request_dispatch(funasr_msg->channel, funasr_msg->request);
            break;
        default:
            apt_log(APT_LOG_MARK, APT_PRIO_WARNING, "zyASR: Unknown msg type=%d", funasr_msg->type);
            break;
    }
    return TRUE;
}

/* Plugin version */
MRCP_PLUGIN_VERSION_DECLARE

/* Log source */
MRCP_PLUGIN_LOG_SOURCE_IMPLEMENT(RECOG_PLUGIN, "FUNASR-PLUGIN")

/** Use custom log source mark */
#define RECOG_LOG_MARK   APT_LOG_MARK_DECLARE(RECOG_PLUGIN)