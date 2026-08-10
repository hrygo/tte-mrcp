/*
 * Copyright 2008-2015 Arsen Chaloyan
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* 
 * Mandatory rules concerning plugin implementation.
 * 1. Each plugin MUST implement a plugin/engine creator function
 *    with the exact signature and name (the main entry point)
 *        MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
 * 2. Each plugin MUST declare its version number
 *        MRCP_PLUGIN_VERSION_DECLARE
 * 3. One and only one response MUST be sent back to the received request.
 * 4. Methods (callbacks) of the MRCP engine channel MUST not block.
 *   (asynchronous response can be sent from the context of other thread)
 * 5. Methods (callbacks) of the MPF engine stream MUST not block.
 */

#include "mrcp_synth_engine.h"
#include "tts_websocket_pcm.h"
#include "tts_websocket_lifecycle.h"
#include "tts_websocket_thread.h"
#include "tts_websocket_ws.h"
#include "apt_consumer_task.h"
#include "apt_log.h"
#include <apr_network_io.h>
#include <apr_errno.h>
#include <apr_thread_proc.h>
#include <apr_thread_cond.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <iconv.h>
#include <arpa/inet.h>
#include <errno.h>
#include <strings.h>
#include <unistd.h>

typedef struct tts_websocket_engine_t tts_websocket_engine_t;
typedef struct tts_websocket_channel_t tts_websocket_channel_t;
typedef struct tts_websocket_msg_t tts_websocket_msg_t;
typedef struct websocket_connection_t websocket_connection_t;

typedef enum {
	TTS_WEBSOCKET_OUTPUT_IDLE = 0,
	TTS_WEBSOCKET_OUTPUT_STREAMING,
	TTS_WEBSOCKET_OUTPUT_POSTROLL,
	TTS_WEBSOCKET_OUTPUT_RTP_DRAIN,
	TTS_WEBSOCKET_OUTPUT_COMPLETE_READY
} tts_websocket_output_state_e;

/** Declaration of synthesizer engine methods */
static apt_bool_t tts_websocket_engine_destroy(mrcp_engine_t *engine);
static apt_bool_t tts_websocket_engine_open(mrcp_engine_t *engine);
static apt_bool_t tts_websocket_engine_close(mrcp_engine_t *engine);
static mrcp_engine_channel_t* tts_websocket_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool);

static const struct mrcp_engine_method_vtable_t engine_vtable = {
	tts_websocket_engine_destroy,
	tts_websocket_engine_open,
	tts_websocket_engine_close,
	tts_websocket_engine_channel_create
};


/** Declaration of synthesizer channel methods */
static apt_bool_t tts_websocket_channel_destroy(mrcp_engine_channel_t *channel);
static apt_bool_t tts_websocket_channel_open(mrcp_engine_channel_t *channel);
static apt_bool_t tts_websocket_channel_close(mrcp_engine_channel_t *channel);
static apt_bool_t tts_websocket_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request);

static const struct mrcp_engine_channel_method_vtable_t channel_vtable = {
	tts_websocket_channel_destroy,
	tts_websocket_channel_open,
	tts_websocket_channel_close,
	tts_websocket_channel_request_process
};

/** Declaration of synthesizer audio stream methods */
static apt_bool_t tts_websocket_stream_destroy(mpf_audio_stream_t *stream);
static apt_bool_t tts_websocket_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec);
static apt_bool_t tts_websocket_stream_close(mpf_audio_stream_t *stream);
static apt_bool_t tts_websocket_stream_read_safe(mpf_audio_stream_t *stream, mpf_frame_t *frame);

static const mpf_audio_stream_vtable_t audio_stream_vtable = {
	tts_websocket_stream_destroy,
	tts_websocket_stream_open,
	tts_websocket_stream_close,
	tts_websocket_stream_read_safe,
	NULL,
	NULL,
	NULL,
	NULL
};

/** Declaration of TTS WebSocket engine */
struct tts_websocket_engine_t {
	apt_consumer_task_t    *task;
	/* TTS server config */
	char                   *tts_server_host;
	apr_port_t             tts_server_port;
	/* ========== 录音开关 ========== */
	/** 是否启用录音保存（通过配置参数 recording-enabled 控制） */
	apt_bool_t             recording_enabled;
	/** SPEAK-COMPLETE 前发送的尾部静音时长 */
	apr_uint32_t           completion_postroll_ms;
	/** RTP packetization time，需与 RTP settings 的 ptime 保持一致 */
	apr_uint32_t           rtp_ptime_ms;
};

struct websocket_connection_t {
	apr_socket_t *sock;
	tts_websocket_ws_decoder_t decoder;
};

/** Declaration of TTS WebSocket channel */
struct tts_websocket_channel_t {
	/** Back pointer to engine */
	tts_websocket_engine_t   *tts_engine;
	/** Engine channel base */
	mrcp_engine_channel_t *channel;

	/** Active (in-progress) speak request */
	mrcp_message_t        *speak_request;
	/** Pending stop response */
	mrcp_message_t        *stop_response;
	/** Estimated time to complete */
	apr_size_t             time_to_complete;
	/** Is paused */
	apt_bool_t             paused;
	/** Speech source (used instead of actual synthesis) */
	FILE                  *audio_file;

	/* ========== 流式TTS处理相关字段 ========== */
	/** 流式接收线程 */
	apr_thread_t          *stream_thread;
	/** 流式接收线程退出标志 */
	volatile apr_uint32_t  stream_stop_requested;
	/** 流式接收socket */
	apr_socket_t          *stream_socket;
	/** WebSocket 有状态解码上下文（握手 surplus、分片消息状态） */
	websocket_connection_t *stream_ws;
	/** 当前 SPEAK 的可销毁内存池 */
	apr_pool_t             *stream_pool;
	/** MPF read callback 与 SPEAK 清理之间的生命周期屏障 */
	tts_websocket_stream_lifecycle_t stream_lifecycle;
	/** 流式环形缓冲区 */
	char                  *stream_buffer;
	/** 环形缓冲区大小（必须是2的幂） */
	apr_size_t             stream_buffer_size;
	/** 环形缓冲区写位置 */
	volatile apr_size_t    stream_write_pos;
	/** 环形缓冲区读位置 */
	volatile apr_size_t    stream_read_pos;
	/** 缓冲区同步条件变量（用于流控） */
	apr_thread_cond_t     *stream_buffer_cond;
	/** 缓冲区同步互斥锁 */
	apr_thread_mutex_t    *stream_buffer_mutex;
	/** 流式接收是否完成 */
	volatile apr_uint32_t  stream_complete;
	/** 流式接收错误标志 */
	volatile apr_uint32_t  stream_error;
	/** 是否开始接收音频数据（收到 audio.start 消息） */
	volatile apr_uint32_t  stream_audio_started;
	/** 上次警告时间（用于限制日志频率） */
	volatile apr_time_t stream_last_warn_time;
		/* ========== 时序诊断字段（排查音频卡顿问题） ========== */
		/** 流式接收启动时间（input.done 发送后） */
		apr_time_t             stream_start_time;
		/** 首帧音频到达时间 */
		volatile apr_time_t    stream_first_audio_time;
		/** 上一帧音频到达时间（用于计算帧间隔） */
		volatile apr_time_t    stream_last_audio_time;
		/** 已接收的原始PCM音频总字节数 */
		volatile apr_size_t    stream_total_bytes_received;
		/** 已接收的二进制音频帧数量 */
		volatile apr_uint32_t  stream_audio_frame_count;
		/** 音频帧之间的最大间隔（微秒） */
		volatile apr_interval_time_t stream_max_inter_arrival_us;
		/** reader 读到空缓冲区的次数 */
		volatile apr_uint32_t  stream_buffer_empty_count;
		/** reader 因缓冲区为空发送静音帧的次数 */
		volatile apr_uint32_t  stream_silence_frame_count;
		/* ========== 丢音诊断计数器（定位"录音完整但客户端丢尾音"问题） ==========
		 * 8k 录音的写盘点之前在 WS 接收线程（写环形缓冲区之前），只能证明
		 * 数据到达了插件，无法覆盖 ring→RTP 之间的丢失。以下计数器配合
		 * read_safe 发帧点的录音和 SPEAK-COMPLETE 时的 [DIAG] 汇总日志，
		 * 可精确定位丢失环节：
		 *   ring_written == ring_read 且 dropped==0 但客户端丢音
		 *     → 音频已全部交给 MPF，丢失在 RTP 发送/网络/客户端侧；
		 *   ring_written > ring_read
		 *     → reader 未排干缓冲区，检查 EOF/完成时序；
		 *   dropped > 0
		 *     → stop 路径丢弃了尾部数据（查 [CLEANUP]/STOP/barge-in 日志）。 */
		/** 已写入环形缓冲区的总字节数（8k PCMU） */
		volatile apr_size_t    stream_ring_bytes_written;
		/** 已从环形缓冲区读出的总字节数 */
		volatile apr_size_t    stream_ring_bytes_read;
		/** stop 请求导致未能写入而被丢弃的字节数 */
		volatile apr_size_t    stream_ring_bytes_dropped;
		/** reader trylock 失败（writer 持锁）被迫发静音帧的次数 */
		volatile apr_uint32_t  stream_trylock_fail_count;
		/** 缓冲区数据不足一帧且流未完成而跳过本次读取的次数 */
		volatile apr_uint32_t  stream_partial_wait_count;
		/* ========== 时序诊断字段结束 ========== */
		/* ========== 预缓冲策略：避免音频开头和句子间出现underrun ========== */
		/** 是否已完成预缓冲（缓冲区达到过最低水位） */
		volatile apr_uint32_t  stream_prebuffered;
		/** 预缓冲最低水位（字节数，默认取buffer_size的25%） */
		apr_size_t             stream_prebuffer_min_fill;
	/** MPF bridge 初始化的固定 codec frame 大小，不允许被短尾帧修改 */
	apr_size_t             stream_codec_frame_size;
	/** 尾音输出、post-roll、RTP drain 和完成事件状态 */
	tts_websocket_output_state_e stream_output_state;
	apr_uint32_t           stream_postroll_ticks_left;
	apr_uint32_t           stream_drain_ticks_left;
	mrcp_synth_completion_cause_e stream_completion_cause;
	/** 原始PCM累积缓冲区（跨帧对齐到3采样边界，避免帧边界爆音） */
	char     pcm_accum[6];  /* 6字节防御性预留：PCM帧恒为偶数，process_len%6∈{0,2,4} */
	/** 累积缓冲区中的字节数 (0-5) */
	int      pcm_accum_len;

	/* ========== 录音保存相关字段 ========== */
	/** 录音输出文件（最终8kHz μ-law格式，即MRCP客户端收到的格式） */
	FILE    *record_file;
	/** 录音输出文件（原始24kHz PCM格式，TTS服务端原始返回） */
	FILE    *record_file_orig;

};
typedef enum {
	TTS_WEBSOCKET_MSG_OPEN_CHANNEL,
	TTS_WEBSOCKET_MSG_CLOSE_CHANNEL,
	TTS_WEBSOCKET_MSG_REQUEST_PROCESS
} tts_websocket_msg_type_e;

/** Declaration of TTS WebSocket task message */
struct tts_websocket_msg_t {
	tts_websocket_msg_type_e  type;
	mrcp_engine_channel_t *channel; 
	mrcp_message_t        *request;
};


static apt_bool_t tts_websocket_msg_signal(tts_websocket_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request);
static apt_bool_t tts_websocket_msg_process(apt_task_t *task, apt_task_msg_t *msg);
static void hex_dump(const char *label, const char *data, apr_size_t len);
static char* gbk_to_utf8(const char *gbk_str, apr_size_t gbk_len, apr_size_t *utf8_len, apr_pool_t *pool);
static char* resample_pcm_to_8k(const char *input_pcm, apr_size_t input_size, apr_size_t *output_size, apr_pool_t *pool);
static char* resample_ulaw_to_8k(const char *input_ulaw, apr_size_t input_size, apr_size_t *output_size, apr_pool_t *pool);
static char* convert_16bit_to_ulaw(const char *input_pcm, apr_size_t input_size, apr_size_t *output_size, apr_pool_t *pool);

/* ========== 流式TTS处理函数声明 ========== */
static void* APR_THREAD_FUNC tts_websocket_stream_thread(apr_thread_t *thd, void *data);
static apt_bool_t tts_websocket_start_streaming(tts_websocket_channel_t *synth_channel, const char *text, apr_size_t text_size, const char *voice_name);
static apr_size_t tts_websocket_stream_read_audio(tts_websocket_channel_t *synth_channel, char *buffer, apr_size_t size, apt_bool_t *eof);
static apt_bool_t tts_websocket_stream_write_audio(tts_websocket_channel_t *synth_channel, const char *data, apr_size_t size);
static void tts_websocket_stream_pool_destroy(tts_websocket_channel_t *synth_channel);
static char* json_escape(const char *str, apr_size_t len, apr_pool_t *pool);
static const char* json_get_type(const char *json, apr_size_t len, char *type_buf, apr_size_t type_buf_size);

/* ========== WebSocket 相关函数声明 ========== */
static apt_bool_t websocket_handshake(websocket_connection_t *connection, const char *host, apr_port_t port, const char *path, apr_pool_t *pool);
static apt_bool_t websocket_send_text(apr_socket_t *sock, const char *text, apr_size_t len, apr_pool_t *pool);
static apt_bool_t websocket_send_close(apr_socket_t *sock, apr_pool_t *pool);
static apr_socket_t *tts_websocket_detach_stream_socket(
	tts_websocket_channel_t *synth_channel,
	apr_socket_t *expected);
static apr_ssize_t websocket_recv_message(websocket_connection_t *connection, char *buffer, apr_size_t buffer_size, apt_bool_t *is_text_frame);
static apr_status_t websocket_socket_read(void *context, char *buffer, apr_size_t *size);
static apt_bool_t websocket_socket_send_control(void *context, unsigned char opcode, const unsigned char *payload, apr_size_t payload_len);
static char* base64_encode(const unsigned char *input, apr_size_t len, apr_pool_t *pool);
static char* generate_websocket_key(apr_pool_t *pool);

/* ========== 录音保存函数声明 ========== */
static apt_bool_t tts_websocket_recording_open(tts_websocket_channel_t *synth_channel, const char *session_id);
static void tts_websocket_recording_write_final(tts_websocket_channel_t *synth_channel, const char *data, apr_size_t size);
static void tts_websocket_recording_write_orig(tts_websocket_channel_t *synth_channel, const char *data, apr_size_t size);
static void tts_websocket_recording_close(tts_websocket_channel_t *synth_channel);

/** Declare this macro to set plugin version */
MRCP_PLUGIN_VERSION_DECLARE

/**
 * Declare this macro to use log routine of the server, plugin is loaded from.
 * Enable/add the corresponding entry in logger.xml to set a cutsom log source priority.
 *    <source name="SYNTH-PLUGIN" priority="DEBUG" masking="NONE"/>
 */
MRCP_PLUGIN_LOG_SOURCE_IMPLEMENT(SYNTH_PLUGIN,"SYNTH-PLUGIN")

/** Use custom log source mark */
#define SYNTH_LOG_MARK   APT_LOG_MARK_DECLARE(SYNTH_PLUGIN)
#define TTS_WEBSOCKET_DEFAULT_POSTROLL_MS 600
#define TTS_WEBSOCKET_DEFAULT_RTP_PTIME_MS 20
#define TTS_WEBSOCKET_MAX_POSTROLL_MS 5000

/* Log macros with session_id */
#define LOG_WITH_SID(synth_channel, prio, fmt, ...) \
    do { \
        const char *_sid = "N/A"; \
        const char *_res = "N/A"; \
        if ((synth_channel) && (synth_channel)->speak_request) { \
            _sid = (synth_channel)->speak_request->channel_id.session_id.buf; \
            _res = (synth_channel)->speak_request->channel_id.resource_name.buf; \
        } \
        apt_log(SYNTH_LOG_MARK, prio, "zyTTS: [session_id=%s] " fmt, _sid, ##__VA_ARGS__); \
    } while(0)



/* ========== 环形缓冲区辅助函数 ========== */

/**
 * @brief 向环形缓冲区写入音频数据（带流控机制）
 * @param synth_channel synthesizer channel
 * @param data 数据指针
 * @param size 数据大小
 * @return 成功返回TRUE，失败返回FALSE
 */
static apt_bool_t tts_websocket_stream_write_audio(tts_websocket_channel_t *synth_channel, const char *data, apr_size_t size)
{
	apr_size_t buffer_size = synth_channel->stream_buffer_size;
	apr_size_t write_pos;
	apr_size_t read_pos;
	apr_size_t available;
	apr_size_t to_write;
	apr_size_t first_chunk;
	apr_size_t total_written = 0;
	apr_size_t data_offset = 0;
	apr_size_t low_watermark = buffer_size / 10;  /* 低水位：10% */
	apr_size_t high_watermark = buffer_size * 9 / 10;  /* 高水位：90% */

	/* 等待直到有足够空间写入全部数据 */
	while(total_written < size) {
		/* ========== 修复：检查停止请求（在加锁前，避免死锁） ==========
		 * 若已被请求停止，尝试最后一次非阻塞写入（不等待条件变量），
		 * 尽可能将当前批次数据写入缓冲区后再返回 FALSE。
		 * 直接丢弃未写入数据会导致句子末尾音频丢失。 */
		if(synth_channel->stream_stop_requested) {
			apr_thread_mutex_lock(synth_channel->stream_buffer_mutex);
			write_pos = synth_channel->stream_write_pos;
			read_pos = synth_channel->stream_read_pos;
			available = (read_pos + buffer_size - write_pos - 1) % buffer_size;
			if(available > 0) {
				to_write = size - total_written;
				if(to_write > available) to_write = available;
				first_chunk = buffer_size - write_pos;
				if(first_chunk > to_write) first_chunk = to_write;
				memcpy(synth_channel->stream_buffer + write_pos, data + data_offset, first_chunk);
				if(to_write > first_chunk) {
					apr_size_t second_chunk = to_write - first_chunk;
					memcpy(synth_channel->stream_buffer, data + data_offset + first_chunk, second_chunk);
				}
				synth_channel->stream_write_pos = (write_pos + to_write) % buffer_size;
				total_written += to_write;
			}
			apr_thread_mutex_unlock(synth_channel->stream_buffer_mutex);
			/* 诊断：stop 路径丢弃的字节会计入 dropped（8k 录音在此之前已落盘，
			 * 因此录音完整但 ring 缺尾时此计数器 > 0） */
			synth_channel->stream_ring_bytes_written += total_written;
			synth_channel->stream_ring_bytes_dropped += (size - total_written);
			LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[STREAM] Write interrupted by stop request, %"APR_SIZE_T_FMT"/%"APR_SIZE_T_FMT" bytes written, cumulative dropped=%"APR_SIZE_T_FMT" bytes",
				total_written, size, synth_channel->stream_ring_bytes_dropped);
			return FALSE;
		}

		/* 加锁读取位置 */
		apr_thread_mutex_lock(synth_channel->stream_buffer_mutex);
		write_pos = synth_channel->stream_write_pos;
		read_pos = synth_channel->stream_read_pos;

		/* 计算可用空间 */
		/* 修复：加上buffer_size确保结果非负 */
		available = (read_pos + buffer_size - write_pos - 1) % buffer_size;

		/* 如果没有空间，等待消费者读取数据（最多等待100ms） */
		if(available == 0) {
			apr_interval_time_t timeout = 100000;  /* 100ms */
			LOG_WITH_SID(synth_channel, APT_PRIO_DEBUG, "[STREAM] Buffer full, waiting for space...");
			apr_thread_cond_timedwait(synth_channel->stream_buffer_cond,
				synth_channel->stream_buffer_mutex, timeout);
			apr_thread_mutex_unlock(synth_channel->stream_buffer_mutex);
			/* stream_stop_requested 检查已移至 while 循环顶部，在此只需 continue 回到顶部 */
			continue;
		}

		/* 计算本次要写入的大小 */
		to_write = size - total_written;
		if(to_write > available) {
			to_write = available;
		}

		/* 计算第一次可以写入的大小（到缓冲区末尾） */
		first_chunk = buffer_size - write_pos;
		if(first_chunk > to_write) {
			first_chunk = to_write;
		}

		/* 写入第一块 */
		memcpy(synth_channel->stream_buffer + write_pos, data + data_offset, first_chunk);

		/* 如果有剩余数据，从缓冲区开头继续写 */
		if(to_write > first_chunk) {
			apr_size_t second_chunk = to_write - first_chunk;
			memcpy(synth_channel->stream_buffer, data + data_offset + first_chunk, second_chunk);
		}

		/* 更新写位置 */
		synth_channel->stream_write_pos = (write_pos + to_write) % buffer_size;

		total_written += to_write;
		data_offset += to_write;

		/* 记录缓冲区使用情况 */
		/* 修复：加上buffer_size确保结果非负 */
		apr_size_t used = (synth_channel->stream_write_pos + buffer_size - synth_channel->stream_read_pos) % buffer_size;
		if(used >= high_watermark) {
			apr_time_t now = apr_time_now();
			if(now - synth_channel->stream_last_warn_time > 1000000) {  /* 最多每秒警告一次 */
				LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[STREAM] Buffer high watermark: %.1f%% (%zu/%zu KB)",
					(used * 100.0) / buffer_size, used / 1024, buffer_size / 1024);
				synth_channel->stream_last_warn_time = now;
			}
		}

		/* ========== 修复：写入数据后通知reader有数据可读 ==========
		 * 之前缺少这个信号，reader在缓冲区为空时无法被唤醒，
		 * 只能依赖MPF的周期性回调轮询。高并发下writer线程CPU调度延迟时，
		 * reader反复读到空缓冲区 → 发送静音帧 → 客户端感知为音频丢失。 */
		apr_thread_cond_signal(synth_channel->stream_buffer_cond);
		/* 释放锁 */
		apr_thread_mutex_unlock(synth_channel->stream_buffer_mutex);
	}

	/* 诊断：累计写入环形缓冲区的字节数（仅 WS writer 线程更新，无需加锁） */
	synth_channel->stream_ring_bytes_written += total_written;

	return TRUE;
}

/**
 * @brief 从环形缓冲区读取音频数据（带同步通知）
 * @param synth_channel synthesizer channel
 * @param buffer 输出缓冲区
 * @param size 请求读取的大小
 * @return 实际读取的字节数
 */
/**
 * @brief 从环形缓冲区读取音频数据（带同步通知和竞态修复）
 * @param synth_channel synthesizer channel
 * @param buffer 输出缓冲区
 * @param size 请求读取的大小
 * @param eof 输出参数：TRUE表示流已完成且缓冲区已空，调用方应发送SPEAK-COMPLETE
 * @return 实际读取的字节数（可能小于size仅当eof为TRUE时）
 *
 * 修复说明：
 * 1. "数据不足等待"的检查和 stream_complete 检查现在统一在 mutex 保护下完成，
 *    消除了之前 tts_websocket_stream_read 中无锁预检与实际读取之间的竞态窗口。
 * 2. 调用方在进入本函数前已经持有 stream_buffer_mutex；本函数不再二次 trylock，
 *    由调用方在整个“状态检查+读取+推进索引”临界区结束后统一解锁。
 * 3. 条件变量信号改为每次读取后无条件发送，避免低水位场景下写线程只能靠100ms超时轮询。
 */
static apr_size_t tts_websocket_stream_read_audio(tts_websocket_channel_t *synth_channel, char *buffer, apr_size_t size, apt_bool_t *eof)
{
	apr_size_t buffer_size = synth_channel->stream_buffer_size;
	apr_size_t write_pos;
	apr_size_t read_pos;
	apr_size_t available;
	apr_size_t to_read;
	apr_size_t first_chunk;

	*eof = FALSE;

	write_pos = synth_channel->stream_write_pos;
	read_pos = synth_channel->stream_read_pos;

	/* 计算可读数据量 */
	available = (write_pos + buffer_size - read_pos) % buffer_size;

	/* 空且完成才是 EOF；空但 producer 仍运行时立即返回。 */
	if(available == 0) {
		if(synth_channel->stream_complete) {
			*eof = TRUE;
		}
		return 0;
	}

	/* 活跃 producer 的不足帧留在 ring 中；仅 EOF 尾帧允许短读并由调用方补静音。
	 * 否则一次 1..79 字节短读会永久污染 MPF 复用 frame 的 size。 */
	if(available < size && !synth_channel->stream_complete) {
		/* 诊断：缓冲区有数据但不足一帧，保留等待凑整（调用方本帧发静音） */
		synth_channel->stream_partial_wait_count++;
		return 0;
	}
	to_read = available < size ? available : size;

	/* 计算第一次可以读取的大小（到缓冲区末尾） */
	first_chunk = buffer_size - read_pos;
	if(first_chunk > to_read) {
		first_chunk = to_read;
	}

	/* 读取第一块 */
	memcpy(buffer, synth_channel->stream_buffer + read_pos, first_chunk);

	/* 如果有剩余数据，从缓冲区开头继续读 */
	if(to_read > first_chunk) {
		apr_size_t second_chunk = to_read - first_chunk;
		memcpy(buffer + first_chunk, synth_channel->stream_buffer, second_chunk);
	}

	/* 更新读位置 */
	synth_channel->stream_read_pos = (read_pos + to_read) % buffer_size;
	/* 诊断：累计从环形缓冲区读出的字节数（仅 MPF reader 线程更新） */
	synth_channel->stream_ring_bytes_read += to_read;

	/* ========== 修复：在 mutex 内部判断 EOF，消除 TOCTOU 竞态 ==========
	 * 之前的实现在释放 mutex 后检查 stream_complete（无锁），
	 * 而 writer 线程也在 mutex 之外设置 stream_complete=1（第1013行）。
	 * 这导致：
	 *   1. 内存排序问题：reader 可能先看到 stream_complete=1，
	 *      再看到旧的 stream_write_pos（buffer 写入不可见），
	 *      误判 available==0 而提前返回 EOF。
	 *   2. 时间窗口竞态：reader 在 "check stream_complete" 和
	 *      "lock+check available" 之间，writer 可能完成 flush 并
	 *      设置 stream_complete=1，导致 reader 看到 EOF 但 buffer
	 *      中实际还有未读数据。
	 *
	 * 修复：在 mutex 保护下同时读取 stream_complete、write_pos、
	 * read_pos 三个变量，原子性地判断 "buffer为空 && 流已完成"，
	 * 彻底消除上述竞态窗口。 */
	if(synth_channel->stream_complete) {
		apr_size_t remaining = (synth_channel->stream_write_pos + buffer_size
			- synth_channel->stream_read_pos) % buffer_size;
		*eof = (remaining == 0);
	}

	if(synth_channel->stream_buffer_cond) {
		apr_thread_cond_signal(synth_channel->stream_buffer_cond);
	}

	return to_read;
}

/**
 * @brief 流式TTS接收线程主函数（WebSocket模式）
 * @param thd 线程对象
 * @param data tts_websocket_channel_t指针
 * @return NULL
 */
static void* APR_THREAD_FUNC tts_websocket_stream_thread(apr_thread_t *thd, void *data)
{
	tts_websocket_channel_t *synth_channel = (tts_websocket_channel_t*)data;
	apr_socket_t *sock;
	char *buffer;  /* 接收缓冲区 - 从堆上分配2MB，避免线程栈溢出（增大缓冲区防止大帧数据被丢弃） */
	apr_ssize_t len;
	apt_bool_t is_text_frame;
	apt_bool_t receiving_audio = FALSE;
	int sentence_index = 0;
	apr_time_t start_time;
	apr_time_t current_time;
	apr_time_t last_data_time;  /* 上次收到数据的时间 */
	apr_interval_time_t timeout = 30 * 1000000; /* 30秒超时，适应长文本TTS处理 */
	apt_bool_t session_done_received = FALSE;
	apr_pool_t *pool;       /* channel pool — 用于长生命周期分配（buffer/socket） */
	apr_pool_t *frame_pool = NULL; /* 线程私有临时pool — 每帧处理后清空，避免内存泄漏 */

	LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Stream thread starting...");

	/* 立即复制socket指针，避免竞态条件 */
	/* ========== 修复：检查channel有效性 ========== */
	if(!synth_channel->channel || !synth_channel->channel->pool) {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Channel or pool is NULL, thread exiting");
		synth_channel->stream_complete = 1;
		synth_channel->stream_error = 1;
		apr_thread_exit(thd, 0);
		return NULL;
	}
	sock = synth_channel->stream_socket;
	if(!sock) {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Stream socket is NULL, thread exiting");
		synth_channel->stream_complete = 1;
		apr_thread_exit(thd, 0);
		return NULL;
	}

	/* 保存 pool 指针，避免后续访问时出现问题 */
	pool = synth_channel->stream_pool
		? synth_channel->stream_pool : synth_channel->channel->pool;

	/* ========== 修复：从堆上分配2MB缓冲区，避免线程栈溢出和帧数据截断 ========== */
	buffer = (char*)apr_palloc(pool, 2097152);
	if(!buffer) {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Failed to allocate receive buffer, thread exiting");
		synth_channel->stream_complete = 1;
		synth_channel->stream_error = 1;
		apr_thread_exit(thd, 0);
		return NULL;
	}

	/* ========== 修复：创建线程私有临时pool，避免每帧apr_palloc导致channel pool无限增长 ==========
	 * 之前 resample_pcm_to_8k / convert_16bit_to_ulaw / combined buffer 都用 channel pool，
	 * 分配的内存永不释放，长时间session持续累积 → OOM → 分配失败丢帧。
	 * 现在用 frame_pool 子池，每轮循环后 apr_pool_clear 释放本轮临时分配。 */
	if(apr_pool_create(&frame_pool, pool) != APR_SUCCESS) {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Failed to create frame pool, thread exiting");
		synth_channel->stream_complete = 1;
		synth_channel->stream_error = 1;
		apr_thread_exit(thd, 0);
		return NULL;
	}

	/* 记录开始时间（发送input.done后的时间） */
	start_time = apr_time_now();
	last_data_time = start_time;  /* 初始化最后收到数据的时间 */
	synth_channel->stream_start_time = start_time;
	synth_channel->stream_total_bytes_received = 0;
	synth_channel->stream_audio_frame_count = 0;
	synth_channel->stream_max_inter_arrival_us = 0;
	synth_channel->stream_buffer_empty_count = 0;
	synth_channel->stream_silence_frame_count = 0;
	synth_channel->stream_first_audio_time = 0;
	synth_channel->stream_last_audio_time = 0;
	synth_channel->stream_prebuffered = 0;

	LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Stream thread started, waiting for server response (30s timeout or session.done)...");

	/* 接收WebSocket消息 */
	while(!synth_channel->stream_stop_requested && !session_done_received) {
		/* 清空上一轮迭代的临时分配（resample/ulaw/combined buffer），防止内存无限累积 */
		apr_pool_clear(frame_pool);

		/* 检查超时：从input.done发送后开始计算4秒 */
		current_time = apr_time_now();
		if(current_time - last_data_time > timeout) {
			LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Timeout: No data received for 30 seconds, actively closing connection");
			synth_channel->stream_error = 1;
			break;
		}

		LOG_WITH_SID(synth_channel, APT_PRIO_DEBUG, "[WS] Calling websocket_recv_message...");
		len = websocket_recv_message(synth_channel->stream_ws, buffer, 2097152, &is_text_frame);
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] websocket_recv_message returned: len=%d, is_text_frame=%d", (int)len, is_text_frame);

		if(len < 0) {
			/* 连接关闭或出错 */
			LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Connection closed or error detected, len=%d, actively closing connection", (int)len);
			if(!synth_channel->stream_stop_requested) {
				synth_channel->stream_error = 1;
			}
			break;
		}

		if(len == 0) {
			/* 空帧或控制帧，继续 */
			continue;
		}

		/* 更新最后收到数据的时间 */
		last_data_time = apr_time_now();

		/* 安全地对文本帧做终止符处理，避免越界 */
		if(is_text_frame) {
			if(len < (apr_ssize_t)2097152) {
				buffer[len] = '\0';
			}
			else {
				/* 截断并确保末尾为0，以避免后续字符串操作越界 */
				buffer[2097152-1] = '\0';
			}
		}

		if(is_text_frame) {
			/* JSON 文本消息 */
			LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Received JSON message (len=%d): %.*s%s", (int)len,
				(int)len > 200 ? 200 : (int)len, buffer, (int)len > 200 ? "..." : "");

			/* 解析消息类型 - 支持带空格和不带空格的格式 */
			/* 格式: {"type": "audio.start", ...} 或 {"type":"audio.start", ...} */

			/* ========== 修复：使用 json_get_type 精确提取 type 字段值，替代 strstr 子串匹配 ==========
			 * strstr 子串匹配存在严重缺陷：若消息中包含 "audio_start_time" 等字段名，
			 * 会误匹配 "audio.start" 子串，导致 receiving_audio 状态错乱、音频数据被丢弃。 */
			char msg_type[64];
			if(json_get_type(buffer, (apr_size_t)len, msg_type, sizeof(msg_type))) {
				LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Message type: %s", msg_type);
			} else {
				LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Failed to extract message type from JSON");
				msg_type[0] = '\0';
			}

			if(strcmp(msg_type, "audio.start") == 0) {
				receiving_audio = TRUE;
				synth_channel->stream_audio_started = 1;  /* 标记音频已开始 */
				/* synth_channel->stream_prebuffered = 0;  已移除每句预缓冲：TTS gap仅20-27ms，
				 * 句子间flush的残留数据已在环形缓冲中，新句子音频到来后可直接播放，
				 * 每句重新预缓冲反而导致每句开头大量静音帧（压测silence_frame_count普遍40） */
				/* 丢弃上一句末尾不足3采样(6字节)的残留PCM字节。
				 * 残留量最多4字节 = 2个16-bit采样 = 0.083ms@24kHz，完全不可闻。
				 * 零填充flush会引入波形跳变 → 句子间爆音，直接丢弃远优于补零。 */
				if(synth_channel->pcm_accum_len > 0) {
					LOG_WITH_SID(synth_channel, APT_PRIO_DEBUG, "[WS] Discarding %d residual accum bytes at audio.start (inaudible)",
						synth_channel->pcm_accum_len);
				}
				synth_channel->pcm_accum_len = 0;
				LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] ========== Audio.start received, expecting binary PCM data ==========");

				/* 提取句子索引 */
				char *p = strstr(buffer, "sentence_index");
				if(p) {
					/* 跳过 "sentence_index" 和可能的空格/引号/冒号 */
					p += 14; /* strlen("sentence_index") */
					while(*p == ' ' || *p == ':' || *p == '"' || *p == '\'') p++;
					if(*p >= '0' && *p <= '9') {
						sentence_index = atoi(p);
						LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Audio started for sentence %d", sentence_index);
					}
				}

				/* 提取并记录句子文本（如果有） */
				p = strstr(buffer, "sentence_text");
				if(p) {
					p += 13; /* strlen("sentence_text") */
					/* 查找第一个引号后的内容 */
					char *start = strchr(p, '"');
					if(start) {
						start++; /* 跳过引号 */
						char *end = strchr(start, '"');
						if(end) {
							*end = '\0'; /* 临时终止以记录 */
							LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Sentence text: %s", start);
							*end = '"'; /* 恢复 */
						}
					}
				}

				/* 提取并记录音频格式信息 */
				p = strstr(buffer, "format");
				if(p) {
					char *format_start = strchr(p + 6, '"');
					if(format_start) {
						format_start++;
						char *format_end = strchr(format_start, '"');
						if(format_end) {
							char format[16];
						 apr_size_t format_len = format_end - format_start;
							if(format_len < sizeof(format) - 1) {
								memcpy(format, format_start, format_len);
								format[format_len] = '\0';
								LOG_WITH_SID(synth_channel, APT_PRIO_DEBUG, "[WS] Audio format: %s", format);
							}
						}
					}
				}

				p = strstr(buffer, "sample_rate");
				if(p) {
					p += 11; /* strlen("sample_rate") */
					while(*p == ' ' || *p == ':' || *p == '"' || *p == '\'') p++;
					int sample_rate = atoi(p);
					LOG_WITH_SID(synth_channel, APT_PRIO_DEBUG, "[WS] Sample rate: %d Hz", sample_rate);
				}
			}
			else if(strcmp(msg_type, "audio.done") == 0) {
				LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] ========== Audio.done received ==========");

				/* 提取当前句子索引 */
				char *p = strstr(buffer, "sentence_index");
				if(p) {
					p += 14; /* strlen("sentence_index") */
					while(*p == ' ' || *p == ':' || *p == '"' || *p == '\'') p++;
					if(*p >= '0' && *p <= '9') {
						int done_index = atoi(p);
						LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Audio done for sentence %d", done_index);
					}
				}

				/* 提取并记录总字节数 */
				p = strstr(buffer, "total_bytes");
				if(p) {
					p += 11; /* strlen("total_bytes") */
					while(*p == ' ' || *p == ':' || *p == '"' || *p == '\'') p++;
					if(*p >= '0' && *p <= '9') {
						int total_bytes = atoi(p);
						LOG_WITH_SID(synth_channel, APT_PRIO_DEBUG, "[WS] Total bytes for sentence: %d", total_bytes);
					}
				}

			/* 检查是否有错误 */
			p = strstr(buffer, "error");
			if(p) {
				p += 5; /* strlen("error") */
				while(*p == ' ' || *p == ':' || *p == '"' || *p == '\'') p++;
				if(strncmp(p, "true", 4) == 0) {
					LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Audio generation error detected for sentence");
					synth_channel->stream_error = 1;
				}
			}

			/* 丢弃句子末尾不足3采样(6字节)的残留PCM字节（最多4字节，
			 * = 0.083ms@24kHz），直接丢弃避免补零flush造成的波形跳变/爆音。 */
			if(synth_channel->pcm_accum_len > 0) {
				LOG_WITH_SID(synth_channel, APT_PRIO_DEBUG, "[WS] Discarding %d residual accum bytes at audio.done (inaudible)",
					synth_channel->pcm_accum_len);
			}
			synth_channel->pcm_accum_len = 0;

			receiving_audio = FALSE;
		}
			else if(strcmp(msg_type, "session.done") == 0) {
				LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] ========== Session.done received, all audio complete, actively closing connection ==========");

				/* 提取句子总数 */
				char *p = strstr(buffer, "total_sentences");
				if(p) {
					p += 16; /* strlen("total_sentences") */
					while(*p == ' ' || *p == ':' || *p == '"' || *p == '\'') p++;
					if(*p >= '0' && *p <= '9') {
						int total_sentences = atoi(p);
						LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Session done, total sentences: %d", total_sentences);
					}
				} else {
					LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Session done, total sentences received");
				}

				/* ========== 修复：先标记 session.done 已接收，但不设置 stream_complete ==========
				 * stream_complete 必须在 pcm_accum flush（循环结束后）之后再设置。
				 * 否则 reader 线程可能看到 stream_complete==1 且环形缓冲区为空，
				 * 提前发送 SPEAK-COMPLETE，导致 flush 写入的残余数据被丢弃。
				 * 现在仅设置 session_done_received，退出循环后再 flush → set stream_complete。 */
				session_done_received = TRUE;
				break;
			}
			else if(strcmp(msg_type, "error") == 0 || strcmp(msg_type, "Error") == 0) {
				LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Error message received: %s, actively closing connection", buffer);
				synth_channel->stream_error = 1;
				break;
			}
			else {
				/* 未知的消息类型，记录但不中断 */
				LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Received unknown message type: %s", buffer);
			}
		} else {
			/* 二进制音频数据 */
			if(receiving_audio) {
				/* ========== 时序诊断：记录帧间隔（排查TTS服务器流卡顿） ========== */
				{
					apr_time_t now = apr_time_now();
					synth_channel->stream_total_bytes_received += (apr_size_t)len;
					synth_channel->stream_audio_frame_count++;
					if(synth_channel->stream_first_audio_time == 0) {
						synth_channel->stream_first_audio_time = now;
						apr_interval_time_t first_delay = now - synth_channel->stream_start_time;
						LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[TIMING] First audio frame arrived: delay=%"APR_TIME_T_FMT"us (%.1fms) from input.done",
							first_delay, first_delay / 1000.0);
					} else {
						apr_interval_time_t gap = now - synth_channel->stream_last_audio_time;
						if(gap > synth_channel->stream_max_inter_arrival_us) {
							synth_channel->stream_max_inter_arrival_us = gap;
						}
						/* 超过50ms间隔打WARNING（24kHz PCM，正常每帧约20-40ms音频） */
						if(gap > 50000) {
							LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[TIMING] Audio frame gap > 50ms: gap=%"APR_TIME_T_FMT"us (%.1fms), frame=%u",
								gap, gap / 1000.0, synth_channel->stream_audio_frame_count);
						} else {
							LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[TIMING] Audio frame #%u: %"APR_SIZE_T_FMT" bytes, gap=%"APR_TIME_T_FMT"us (%.1fms)",
								synth_channel->stream_audio_frame_count, len, gap, gap / 1000.0);
						}
					}
					synth_channel->stream_last_audio_time = now;
				}
				/* ========== 时序诊断结束 ========== */

				LOG_WITH_SID(synth_channel, APT_PRIO_DEBUG, "[WS] Received binary audio frame: %d bytes", (int)len);

				/* 网络分块边界不等于 PCM 采样边界。保留所有奇数字节，
				 * 由 pcm_accum 跨帧拼接，禁止在这里截断最后一个字节。 */
				if(len <= 0) {
					continue;
				}

				/* 保存原始TTS服务端返回的24kHz PCM（在重采样和格式转换之前） */
				tts_websocket_recording_write_orig(synth_channel, buffer, (apr_size_t)len);

				/* 服务端返回的是 24kHz 16-bit little-endian PCM
				 * ("response_format":"pcm" 在标准x86服务器上即为LE)
				 * 不再使用启发式字节序检测（该检测在LSB接近0时会误判为big-endian导致偶发噪声）
				 * 直接按 little-endian 处理 */

				/* 重采样：24kHz -> 8kHz。网络块可在任意字节处分割，
				 * accumulate 会把未达到 6 字节（3 个 16-bit 采样）的尾部
				 * 保留到下一块，保证输入字节流不丢失、不重排。 */
				{
					apr_size_t combined_capacity = (apr_size_t)synth_channel->pcm_accum_len + (apr_size_t)len;
					char *combined = apr_palloc(frame_pool, combined_capacity > 0 ? combined_capacity : 1);
					apr_size_t aligned_len;
					size_t carry_len = (size_t)synth_channel->pcm_accum_len;

					if(!combined) {
						LOG_WITH_SID(synth_channel, APT_PRIO_WARNING,
							"[WS] Failed to allocate PCM accumulation buffer, terminating stream without dropping bytes");
						synth_channel->stream_error = 1;
						break;
					}

					aligned_len = tts_websocket_pcm_accumulate(
						(unsigned char *)synth_channel->pcm_accum,
						&carry_len,
						(const unsigned char *)buffer, (apr_size_t)len,
						(unsigned char *)combined, combined_capacity, 6);
					synth_channel->pcm_accum_len = (int)carry_len;

					/* 处理对齐部分 */
					if(aligned_len >= 6) {
						apr_size_t resampled_size = 0;
						char *resampled_data = resample_pcm_to_8k(combined, aligned_len, &resampled_size, frame_pool);
						if(!resampled_data) {
							LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Resampling failed, dropping frame");
							continue;
						}
						LOG_WITH_SID(synth_channel, APT_PRIO_DEBUG, "[WS] Resampled: %"APR_SIZE_T_FMT" bytes -> %"APR_SIZE_T_FMT" bytes (accum=%d)",
							aligned_len, resampled_size, synth_channel->pcm_accum_len);

						/* 2. 格式转换：16-bit PCM -> 8-bit μ-law (PCMU) */
						apr_size_t ulaw_size = 0;
						char *ulaw_data = convert_16bit_to_ulaw(resampled_data, resampled_size, &ulaw_size, frame_pool);
						if(!ulaw_data) {
							LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] μ-law conversion failed, dropping frame");
							continue;
						}
						LOG_WITH_SID(synth_channel, APT_PRIO_DEBUG, "[WS] Converted to μ-law: %d bytes", (int)ulaw_size);

						/* 8k 录音已移至 MPF 发帧点（tts_websocket_stream_read_safe），
						 * 录制内容与客户端实际收到的 RTP 载荷流一致，
						 * 用于定位 ring→RTP 之间的丢音；此处不再落盘。 */

						/* 3. 写入环形缓冲区 */
						if(!tts_websocket_stream_write_audio(synth_channel, ulaw_data, ulaw_size)) {
							LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Write interrupted by stop request, %d bytes not written",
								(int)ulaw_size);
						} else {
							LOG_WITH_SID(synth_channel, APT_PRIO_DEBUG, "[WS] Written to ring buffer: %d bytes", (int)ulaw_size);
						}
					} else {
						LOG_WITH_SID(synth_channel, APT_PRIO_DEBUG, "[WS] Accumulated %d bytes (total accum=%d), waiting for more",
							(int)len, synth_channel->pcm_accum_len);
					}
				}
			} else {
				LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Received binary data outside audio frame, ignoring");
			}
		}
	}

	/* 丢弃session末尾不足3采样(6字节)的残留PCM字节（最多4字节=0.083ms），
	 * 直接丢弃避免补零flush造成的波形跳变/爆音。 */
	if(session_done_received && synth_channel->pcm_accum_len > 0) {
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Discarding %d residual accum bytes at session.done (inaudible)",
			synth_channel->pcm_accum_len);
		synth_channel->pcm_accum_len = 0;
	}

	/* ========== 时序诊断：打印session汇总统计 ========== */
	{
		apr_time_t now = apr_time_now();
		apr_interval_time_t total_time = now - synth_channel->stream_start_time;
		apr_interval_time_t first_audio_delay = synth_channel->stream_first_audio_time > 0
			? (synth_channel->stream_first_audio_time - synth_channel->stream_start_time) : 0;
		apr_size_t total_kb = synth_channel->stream_total_bytes_received / 1024;
		double throughput_kbps = total_time > 0
			? (synth_channel->stream_total_bytes_received * 1000000.0) / (total_time * 1024.0) : 0;
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[TIMING] ========== Session Summary ==========");
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[TIMING] Total session time: %"APR_TIME_T_FMT"us (%.1fms)",
			total_time, total_time / 1000.0);
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[TIMING] First audio delay: %"APR_TIME_T_FMT"us (%.1fms) from input.done",
			first_audio_delay, first_audio_delay / 1000.0);
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[TIMING] Audio frames received: %u frames, %"APR_SIZE_T_FMT" KB raw PCM",
			synth_channel->stream_audio_frame_count, total_kb);
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[TIMING] Raw audio throughput: %.1f KB/s", throughput_kbps);
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[TIMING] Max inter-frame gap: %"APR_TIME_T_FMT"us (%.1fms) [>50ms indicates TTS server streaming stall]",
			synth_channel->stream_max_inter_arrival_us, synth_channel->stream_max_inter_arrival_us / 1000.0);
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[TIMING] Ring buffer empty count: %u (reader waited for data)",
			synth_channel->stream_buffer_empty_count);
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[TIMING] Silence frames sent (buffer empty): %u",
			synth_channel->stream_silence_frame_count);
		/* 注意：此处 reader 尚未排干缓冲区，ring_read 为部分值；
		 * 最终对比以 SPEAK-COMPLETE 时的 [DIAG] 汇总日志为准 */
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[TIMING] Ring buffer: written=%"APR_SIZE_T_FMT"B, dropped_stop=%"APR_SIZE_T_FMT"B, trylock_fail=%u, partial_wait=%u",
			synth_channel->stream_ring_bytes_written,
			synth_channel->stream_ring_bytes_dropped,
			(unsigned)synth_channel->stream_trylock_fail_count,
			(unsigned)synth_channel->stream_partial_wait_count);
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[TIMING] ==========================================");
	}
	/* ========== 时序诊断结束 ========== */

	/* 标记接收完成（stream_complete 在 pcm_accum flush 之后再设置，避免 reader 提前看到 EOF） */
	if(session_done_received) {
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] ========== Streaming completed successfully (session.done received) ==========");
		/* ========== 修复：在 mutex 保护下设置 stream_complete 并 broadcast ==========
		 * 之前 stream_complete 在 mutex 之外设置（无 release barrier），
		 * reader 的 EOF 检测也在 mutex 之外读取 stream_complete（无 acquire barrier），
		 * 高并发下导致 reader 可能先观测到 stream_complete=1 再看到旧的 write_pos，
		 * 误判缓冲区为空而提前发送 SPEAK-COMPLETE → 后半段音频丢失。
		 *
		 * 现在在 buffer mutex 内设置 stream_complete 并通过 broadcast 唤醒 reader，
		 * mutex 的 release/acquire 语义保证 reader 同时看到 stream_complete 和
		 * 最新的 write_pos/read_pos，消除内存排序导致的 TOCTOU 竞态。
		 *
		 * 注意：pcm_accum flush（上面的 tts_websocket_stream_write_audio 调用）
		 * 已完成所有环形缓冲区写入，此处仅设置完成标志。 */
		apr_thread_mutex_lock(synth_channel->stream_buffer_mutex);
		synth_channel->stream_complete = 1;
		apr_thread_cond_broadcast(synth_channel->stream_buffer_cond);
		apr_thread_mutex_unlock(synth_channel->stream_buffer_mutex);
	} else if(synth_channel->stream_error) {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] ========== Streaming terminated with error (timeout or connection error) ==========");
		/* 错误情况下也通过 mutex 设置 stream_complete */
		apr_thread_mutex_lock(synth_channel->stream_buffer_mutex);
		synth_channel->stream_complete = 1;
		apr_thread_cond_broadcast(synth_channel->stream_buffer_cond);
		apr_thread_mutex_unlock(synth_channel->stream_buffer_mutex);
	} else {
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] ========== Streaming stopped (stop requested) ==========");
		/* 停止请求情况下也通过 mutex 设置 stream_complete */
		apr_thread_mutex_lock(synth_channel->stream_buffer_mutex);
		synth_channel->stream_complete = 1;
		apr_thread_cond_broadcast(synth_channel->stream_buffer_cond);
		apr_thread_mutex_unlock(synth_channel->stream_buffer_mutex);
	}

	/* 只有成功摘取 socket 的路径拥有关闭权。cleanup 可能已经关闭并
	 * 摘取了 socket 来中断 recv；不再通过裸指针检查与它竞争发送/关闭。 */
	if(tts_websocket_detach_stream_socket(synth_channel, sock)) {
		/* ========== 修复：发送 WebSocket 关闭帧，优雅地关闭连接 ========== */
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] ========== Closing WebSocket connection (stream thread finishing) ==========");
		websocket_send_close(sock, pool);

		/* 短暂延迟，让关闭帧发送出去 */
		apr_sleep(10000);  /* 10ms */

		apr_socket_close(sock);
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] WebSocket connection closed by stream thread");
	} else if(sock) {
		/* cleanup 已抢先关闭 socket，无需重复操作 */
		LOG_WITH_SID(synth_channel, APT_PRIO_DEBUG, "[WS] Socket already closed by cleanup, skipping");
	}

	/* 本线程仍是 24k 原始录音（record_file_orig）的唯一 writer，退出前 flush/close。
	 * 8k 最终录音（record_file）的 writer 已改为 MPF 发帧线程（read_safe），
	 * 必须保持打开直至 post-roll 结束、SPEAK-COMPLETE 发出，
	 * 由 read_safe 在播放完成时关闭；异常/停止路径由 cleanup_audio 幂等关闭。
	 * 若在此提前关闭，reader 后续排出的尾部音频帧将无法落盘。 */
	if(synth_channel->record_file_orig) {
		fflush(synth_channel->record_file_orig);
		fclose(synth_channel->record_file_orig);
		synth_channel->record_file_orig = NULL;
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[RECORD] Original audio file closed");
	}
	LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Stream thread exiting, stream_complete=%d", synth_channel->stream_complete);
	/* 销毁线程私有临时pool，释放所有本轮session累积的临时分配 */
	if(frame_pool) {
		apr_pool_destroy(frame_pool);
		frame_pool = NULL;
	}
	apr_thread_exit(thd, 0);
	return NULL;
}

/* The MPF read callback can outlive the WebSocket worker.  The worker join
 * therefore is not sufficient to make stream_pool safe to destroy: the
 * callback must first leave its lifecycle section. */
static void tts_websocket_stream_pool_destroy(tts_websocket_channel_t *synth_channel)
{
	if(!synth_channel) {
		return;
	}
	tts_websocket_stream_lifecycle_begin_close(&synth_channel->stream_lifecycle);
	if(!synth_channel->stream_pool) {
		return;
	}
	tts_websocket_stream_lifecycle_wait(&synth_channel->stream_lifecycle);
	apr_pool_destroy(synth_channel->stream_pool);
	synth_channel->stream_pool = NULL;
	synth_channel->stream_ws = NULL;
}

/* Detach the socket under the buffer mutex so cleanup and the worker have a
 * single owner for the send-close/close sequence. */
static apr_socket_t *tts_websocket_detach_stream_socket(
	tts_websocket_channel_t *synth_channel,
	apr_socket_t *expected)
{
	apr_socket_t *sock = NULL;

	if(!synth_channel || !synth_channel->stream_buffer_mutex) {
		return NULL;
	}
	apr_thread_mutex_lock(synth_channel->stream_buffer_mutex);
	if(synth_channel->stream_socket &&
		(!expected || synth_channel->stream_socket == expected)) {
		sock = synth_channel->stream_socket;
		synth_channel->stream_socket = NULL;
		if(synth_channel->stream_ws && synth_channel->stream_ws->sock == sock) {
			synth_channel->stream_ws->sock = NULL;
		}
	}
	apr_thread_mutex_unlock(synth_channel->stream_buffer_mutex);
	return sock;
}

/**
 * @brief 启动流式TTS接收（WebSocket模式）
 * @param synth_channel synthesizer channel
 * @param text 要转换的文本
 * @param text_size 文本长度
 * @return 成功返回TRUE
 */
static apt_bool_t tts_websocket_start_streaming(tts_websocket_channel_t *synth_channel, const char *text, apr_size_t text_size, const char *voice_name)
{
	tts_websocket_engine_t *tts_engine;
	apr_pool_t *channel_pool;
	apr_pool_t *pool;
	apr_socket_t *sock;
	apr_status_t rv;
	apr_sockaddr_t *sa;
	char *json_body = NULL;
	char *escaped_text = NULL;

	LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] tts_websocket_start_streaming ENTER");

	if(!synth_channel || !text || text_size == 0) {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Early validation FAILED");
		return FALSE;
	}

	tts_engine = synth_channel->tts_engine;
	channel_pool = synth_channel->channel->pool;
	pool = channel_pool;

	LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Pointers initialized OK");

	/* ========== 修复：检查并等待之前的线程完成 ========== */
	if(synth_channel->stream_thread) {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Previous stream thread still exists, waiting for it to complete...");
		/* 等待之前的线程完成（重要：不要强制停止，让它自然完成） */
		apr_status_t rv;
		apr_status_t thread_retval;
		rv = apr_thread_join(&thread_retval, synth_channel->stream_thread);
		if(rv != APR_SUCCESS) {
			LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Previous thread join failed with status %d", rv);
		}
		synth_channel->stream_thread = NULL;
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Previous stream thread completed");
	}

	/* ========== 修复：关闭之前的 socket（如果存在） ========== */
	/* 之前的线程应该已经关闭了 socket，但为了安全起见，再次检查 */
	if(synth_channel->stream_socket) {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Previous socket still exists, closing...");
		apr_socket_close(synth_channel->stream_socket);
		synth_channel->stream_socket = NULL;
	}
	tts_websocket_stream_pool_destroy(synth_channel);
	if(apr_pool_create(&synth_channel->stream_pool, channel_pool) != APR_SUCCESS) {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Failed to create SPEAK stream pool");
		return FALSE;
	}
	pool = synth_channel->stream_pool;

	/* 初始化流式接收状态 */
	synth_channel->stream_stop_requested = 0;
	synth_channel->stream_complete = 0;
	synth_channel->stream_error = 0;
	synth_channel->stream_audio_started = 0;  /* 尚未收到 audio.start */
	synth_channel->stream_last_warn_time = 0;
	synth_channel->stream_write_pos = 0;
	synth_channel->stream_read_pos = 0;
	/* 重置时序诊断字段 */
	synth_channel->stream_start_time = 0;
	synth_channel->stream_first_audio_time = 0;
	synth_channel->stream_last_audio_time = 0;
	synth_channel->stream_total_bytes_received = 0;
	synth_channel->stream_audio_frame_count = 0;
	synth_channel->stream_max_inter_arrival_us = 0;
	synth_channel->stream_buffer_empty_count = 0;
	synth_channel->stream_silence_frame_count = 0;
	synth_channel->stream_prebuffered = 0;
	/* 重置丢音诊断计数器 */
	synth_channel->stream_ring_bytes_written = 0;
	synth_channel->stream_ring_bytes_read = 0;
	synth_channel->stream_ring_bytes_dropped = 0;
	synth_channel->stream_trylock_fail_count = 0;
	synth_channel->stream_partial_wait_count = 0;
	synth_channel->stream_output_state = TTS_WEBSOCKET_OUTPUT_STREAMING;
	synth_channel->stream_postroll_ticks_left = 0;
	synth_channel->stream_drain_ticks_left = 0;
	synth_channel->stream_completion_cause = SYNTHESIZER_COMPLETION_CAUSE_NORMAL;
	/* 重置PCM累积缓冲区 */
	synth_channel->pcm_accum_len = 0;

	/* 分配环形缓冲区（512KB，高并发下TTS服务响应可能变慢，需要更大缓冲防止句子间underrun） */
	synth_channel->stream_buffer_size = 512 * 1024;
	synth_channel->stream_prebuffer_min_fill = 640;   /* 640B = ~80ms of audio at 8KB/s, 降低首次延迟同时提供最小缓冲 */
	synth_channel->stream_buffer = apr_palloc(pool, synth_channel->stream_buffer_size);
	LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Buffer allocated, size: %"APR_SIZE_T_FMT" bytes",
		synth_channel->stream_buffer_size);
	if(!synth_channel->stream_buffer) {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Failed to allocate ring buffer");
		return FALSE;
	}

	/* 创建缓冲区同步条件变量和互斥锁（如果尚未创建） */
	apt_bool_t mutex_created = FALSE, cond_created = FALSE;
	LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Before mutex/cond creation");
	if(!synth_channel->stream_buffer_mutex) {
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Creating mutex...");
		if(apr_thread_mutex_create(&synth_channel->stream_buffer_mutex, APR_THREAD_MUTEX_DEFAULT, channel_pool) != APR_SUCCESS) {
			LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Failed to create buffer mutex");
			return FALSE;
		}
		mutex_created = TRUE;
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Mutex created");
	}
	if(!synth_channel->stream_buffer_cond) {
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Creating cond...");
			if(apr_thread_cond_create(&synth_channel->stream_buffer_cond, channel_pool) != APR_SUCCESS) {
			LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Failed to create buffer condition");
			/* ========== 修复：销毁已创建的互斥锁，避免资源泄漏 ========== */
			if(mutex_created) {
				apr_thread_mutex_destroy(synth_channel->stream_buffer_mutex);
				synth_channel->stream_buffer_mutex = NULL;
			}
			return FALSE;
		}
		cond_created = TRUE;
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Cond created");
	}
	LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Step 1: After mutex/cond creation");
	LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Step 2: Before ring buffer log");
	LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Step 3: Ring buffer initialized");
	LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Step 4: After all logs");

	/* MPF read 回调的入口屏障：仅在 ring buffer 与 mutex/cond 全部就绪后才
	 * 放行回调，避免 setup 窗口内回调看到 NULL buffer/mutex（trylock(NULL)）
	 * 或落入遗留分支提前触发 finish_begin（过早 SPEAK-COMPLETE + 丢音）。 */
	tts_websocket_stream_lifecycle_reopen(&synth_channel->stream_lifecycle);

	/* 转换编码 */
	{
		apt_bool_t likely_gbk = FALSE;
		apr_size_t i;
		for(i = 0; i < text_size && i < 10; i++) {
			if((unsigned char)text[i] >= 0x81) {
				likely_gbk = TRUE;
				break;
			}
		}

		if(likely_gbk) {
			apr_size_t utf8_len = 0;
			char *utf8_text = gbk_to_utf8(text, text_size, &utf8_len, pool);
			if(utf8_text && utf8_len > 0) {
				text = utf8_text;
				text_size = utf8_len;
			}
		}
	}

	/* 转义文本用于JSON */
	escaped_text = json_escape(text, text_size, pool);
	if(!escaped_text) {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] JSON escape failed");
		return FALSE;
	}

	LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Starting WebSocket TTS request, text length: %"APR_SIZE_T_FMT, text_size);

	/* 创建socket */
	rv = apr_socket_create(&sock, APR_INET, SOCK_STREAM, APR_PROTO_TCP, pool);
	if(rv != APR_SUCCESS) {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Failed to create socket");
		return FALSE;
	}
	/* 设置30秒超时，与应用层超时匹配 */
	apr_socket_timeout_set(sock, 30 * 1000000);

	/* 连接服务器 */
	rv = apr_sockaddr_info_get(&sa, tts_engine->tts_server_host, APR_INET,
		tts_engine->tts_server_port, 0, pool);
	if(rv != APR_SUCCESS) {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Failed to resolve address");
		apr_socket_close(sock);
		return FALSE;
	}

	rv = apr_socket_connect(sock, sa);
	if(rv != APR_SUCCESS) {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Failed to connect to %s:%d",
			tts_engine->tts_server_host, tts_engine->tts_server_port);
		apr_socket_close(sock);
		return FALSE;
	}

	/* 执行 WebSocket 握手。连接上下文保留 HTTP 101 之后同一次 recv
	 * 读到的首个 WebSocket 字节，避免首帧被握手逻辑吞掉。 */
	synth_channel->stream_ws = apr_pcalloc(pool, sizeof(*synth_channel->stream_ws));
	if(!synth_channel->stream_ws) {
		apr_socket_close(sock);
		return FALSE;
	}
	synth_channel->stream_ws->sock = sock;
	tts_websocket_ws_decoder_init(
		&synth_channel->stream_ws->decoder,
		websocket_socket_read,
		websocket_socket_send_control,
		synth_channel->stream_ws,
		2097152);
	if(!websocket_handshake(synth_channel->stream_ws, tts_engine->tts_server_host, tts_engine->tts_server_port, "/v1/audio/speech/stream", pool)) {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] WebSocket handshake failed");
		apr_socket_close(sock);
		return FALSE;
	}

	/* 发送 session.config 消息 */
	if(!voice_name || voice_name[0] == '\0') {
		voice_name = "yamei_fangyan";
	}
	json_body = apr_psprintf(pool, "{\"type\":\"session.config\",\"voice\":\"%s\",\"task_type\":\"CustomVoice\",\"language\":\"Auto\",\"split_granularity\":\"sentence\",\"stream_audio\":true,\"response_format\":\"pcm\",\"system_id\":\"ncc\",\"scene_id\":\"outcall\",\"call_id\":\"%s\"}", voice_name, (synth_channel && synth_channel->speak_request) ? synth_channel->speak_request->channel_id.session_id.buf : "");
	if(!websocket_send_text(sock, json_body, strlen(json_body), pool)) {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Failed to send session.config");
		apr_socket_close(sock);
		return FALSE;
	}
	LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Sent session.config: %s", json_body);

	/* 发送 input.text 消息（包含完整文本） */
	json_body = apr_psprintf(pool, "{\"type\":\"input.text\",\"text\":\"%s\"}", escaped_text);
	/* 打印WebSocket请求参数信息 */
	LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] ========== Sending input.text to TTS Server ==========");
	LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Server: %s:%d", tts_engine->tts_server_host, tts_engine->tts_server_port);
	LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Request body: %s", json_body);
	LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Original text: %s", text);
	LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Text length: %zu", strlen(text));
	if(!json_body || !websocket_send_text(sock, json_body, strlen(json_body), pool)) {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Failed to send input.text");
		apr_socket_close(sock);
		return FALSE;
	}
	LOG_WITH_SID(synth_channel, APT_PRIO_DEBUG, "[WS] Sent input.text");

	/* 发送 input.done 消息 */
	json_body = "{\"type\":\"input.done\"}";
	if(!websocket_send_text(sock, json_body, strlen(json_body), pool)) {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Failed to send input.done");
		apr_socket_close(sock);
		return FALSE;
	}
	LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] ========== input.done sent, waiting for TTS service response (30s timeout or session.done) ==========");

	/* 保存socket供线程使用 - 必须在创建线程之前完成 */
	synth_channel->stream_socket = sock;

	/* 创建接收线程 */
	rv = apr_thread_create(&synth_channel->stream_thread, NULL, tts_websocket_stream_thread, synth_channel, pool);
	if(rv != APR_SUCCESS) {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[WS] Failed to create stream thread");
		apr_socket_close(sock);
		synth_channel->stream_socket = NULL;
		return FALSE;
	}

	LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[WS] Receive thread created, waiting for TTS service response");

	return TRUE;
}

#define SYNTH_ENGINE_TASK_NAME "TTS WebSocket Engine"

typedef struct tts_websocket_engine_t tts_websocket_engine_t;
typedef struct tts_websocket_channel_t tts_websocket_channel_t;
typedef struct tts_websocket_msg_t tts_websocket_msg_t;

/* ---------- plugin entry ---------- */
/**
 * @brief 创建 TTS WebSocket 合成引擎插件
 * @param pool APR 内存池，用于分配内存
 * @return 返回 MRCP 引擎对象，失败返回 NULL
 * 说明：
 *   1. 初始化 tts_websocket_engine_t 结构体
 *   2. 创建消费者任务队列并设置消息处理函数
 *   3. 创建并返回 MRCP 引擎对象
 */
MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
{
	/* ========== 添加插件加载日志 ========== */
	apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "");
	apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "========================================");
	apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "ZyTTS Plugin: Loading TTS WebSocket Engine");
	apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "========================================");

	/* create TTS WebSocket engine */
	tts_websocket_engine_t *tts_engine = apr_palloc(pool,sizeof(tts_websocket_engine_t));
	/* 默认关闭录音功能（通过 recording-enabled 配置参数开启） */
	tts_engine->recording_enabled = FALSE;
	tts_engine->completion_postroll_ms = TTS_WEBSOCKET_DEFAULT_POSTROLL_MS;
	tts_engine->rtp_ptime_ms = TTS_WEBSOCKET_DEFAULT_RTP_PTIME_MS;
	apt_task_t *task;
	apt_task_vtable_t *vtable;
	apt_task_msg_pool_t *msg_pool;

	/* create task/thread to run TTS WebSocket engine in the context of this task */
	msg_pool = apt_task_msg_pool_create_dynamic(sizeof(tts_websocket_msg_t),pool);
	tts_engine->task = apt_consumer_task_create(tts_engine,msg_pool,pool);
	if(!tts_engine->task) {
		apt_log(SYNTH_LOG_MARK, APT_PRIO_WARNING, "ZyTTS Plugin: Failed to create consumer task");
		return NULL;
	}
	task = apt_consumer_task_base_get(tts_engine->task);
	apt_task_name_set(task,SYNTH_ENGINE_TASK_NAME);
	vtable = apt_task_vtable_get(task);
	if(vtable) {
		vtable->process_msg = tts_websocket_msg_process;
	}

	apt_log(SYNTH_LOG_MARK, APT_PRIO_INFO, "ZyTTS Plugin: Consumer task created successfully");

	/* create engine base */
	mrcp_engine_t *engine = mrcp_engine_create(
				MRCP_SYNTHESIZER_RESOURCE, /* MRCP resource identifier */
				tts_engine,               /* object to associate */
				&engine_vtable,            /* virtual methods table of engine */
				pool);                     /* pool to allocate memory from */

	if(engine) {
		apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "ZyTTS Plugin: Engine created successfully [Resource: speechsynth]");
		apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "========================================");
	} else {
		apt_log(SYNTH_LOG_MARK, APT_PRIO_WARNING, "ZyTTS Plugin: Failed to create engine");
	}

	return engine;
}

/* ---------- engine destroy ---------- */
/**
 * @brief 销毁 TTS WebSocket 合成引擎
 * @param engine MRCP 引擎对象
 * @return 成功返回 TRUE
 * 说明：
 *   1. 获取 tts_websocket_engine_t 对象
 *   2. 停止并销毁消费者任务
 *   3. 清理相关资源
 */
static apt_bool_t tts_websocket_engine_destroy(mrcp_engine_t *engine)
{
	tts_websocket_engine_t *tts_engine = engine->obj;
	if(tts_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(tts_engine->task);
		apt_task_destroy(task);
		tts_engine->task = NULL;
	}
	return TRUE;
}

/* ---------- engine open ---------- */
/**
 * @brief 打开 TTS WebSocket 合成引擎
 * @param engine MRCP 引擎对象
 * @return 成功返回 TRUE
 * 说明：
 *   1. 从引擎参数读取 TTS 主机和端口配置
 *   2. 设置默认值（如未配置）
 *   3. 启动消费者任务线程
 *   4. 向 MRCP 核心发送打开响应
 */
static apt_bool_t tts_websocket_engine_open(mrcp_engine_t *engine)
{
	tts_websocket_engine_t *tts_engine = engine->obj;
	const char *host;
	const char *port;

	/* ========== 添加TTS服务器配置日志 ========== */
	apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "");
	apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "========================================");
	apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "ZyTTS Engine: Opening Synthesizer Engine");
	apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "========================================");

	/* Read TTS server host and port from config */
	apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"ZyTTS Engine: Reading configuration from config file...");

	host = mrcp_engine_param_get(engine, "tts-host");
	port = mrcp_engine_param_get(engine, "tts-port");

	apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"ZyTTS Engine: Config params - tts-host=%s, tts-port=%s",
		host?host:"(not set)", port?port:"(not set)");

	/* Set default values */
	tts_engine->tts_server_host = host ? (char*)host : "127.0.0.1";
	tts_engine->tts_server_port = port ? (apr_port_t)atoi(port) : 8000;

	/* Read recording-enabled flag (default: false/off) */
	{
		const char *rec_enabled = mrcp_engine_param_get(engine, "recording-enabled");
		if (rec_enabled && (strcasecmp(rec_enabled, "true") == 0 || strcasecmp(rec_enabled, "yes") == 0 || strcmp(rec_enabled, "1") == 0)) {
			tts_engine->recording_enabled = TRUE;
		} else {
			tts_engine->recording_enabled = FALSE;
		}
	}
	{
		const char *postroll = mrcp_engine_param_get(engine, "completion-postroll-ms");
		const char *rtp_ptime = mrcp_engine_param_get(engine, "rtp-ptime-ms");
		char *end = NULL;
		unsigned long value;

		if(postroll && postroll[0] != '\0') {
			errno = 0;
			value = strtoul(postroll, &end, 10);
			if(errno == 0 && end && *end == '\0' && value <= TTS_WEBSOCKET_MAX_POSTROLL_MS) {
				tts_engine->completion_postroll_ms = (apr_uint32_t)value;
			} else {
				apt_log(SYNTH_LOG_MARK, APT_PRIO_WARNING,
					"ZyTTS Engine: Invalid completion-postroll-ms [%s], using %u",
					postroll, (unsigned)tts_engine->completion_postroll_ms);
			}
		}

		end = NULL;
		if(rtp_ptime && rtp_ptime[0] != '\0') {
			errno = 0;
			value = strtoul(rtp_ptime, &end, 10);
			if(errno == 0 && end && *end == '\0' && value >= 10 && value <= 200) {
				tts_engine->rtp_ptime_ms = (apr_uint32_t)value;
			} else {
				apt_log(SYNTH_LOG_MARK, APT_PRIO_WARNING,
					"ZyTTS Engine: Invalid rtp-ptime-ms [%s], using %u",
					rtp_ptime, (unsigned)tts_engine->rtp_ptime_ms);
			}
		}
	}


	/* ========== 打印TTS服务器配置信息 ========== */
	apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "ZyTTS Engine: TTS Server Configuration:");
	apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "ZyTTS Engine:   -> Host: %s", tts_engine->tts_server_host);
	apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "ZyTTS Engine:   -> Port: %d", tts_engine->tts_server_port);
	apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "ZyTTS Engine:   -> Path: /v1/audio/speech/stream");
	apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "ZyTTS Engine:   -> Voice: Vivian (CustomVoice)");
	apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "ZyTTS Engine:   -> Recording: %s",
		tts_engine->recording_enabled ? "enabled" : "disabled");
	apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE,
		"ZyTTS Engine:   -> Completion post-roll: %u ms, RTP ptime: %u ms",
		(unsigned)tts_engine->completion_postroll_ms,
		(unsigned)tts_engine->rtp_ptime_ms);
	apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "========================================");

	if(tts_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(tts_engine->task);
		apt_task_start(task);
		apt_log(SYNTH_LOG_MARK, APT_PRIO_INFO, "ZyTTS Engine: Consumer task started");
	}

	apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "ZyTTS Engine: Engine opened successfully, ready to process requests");
	return mrcp_engine_open_respond(engine,TRUE);
}

/* ---------- engine close ---------- */
/**
 * @brief 关闭 TTS WebSocket 合成引擎
 * @param engine MRCP 引擎对象
 * @return 成功返回 TRUE
 * 说明：
 *   1. 终止消费者任务线程
 *   2. 向 MRCP 核心发送关闭响应
 */
static apt_bool_t tts_websocket_engine_close(mrcp_engine_t *engine)
{
	tts_websocket_engine_t *tts_engine = engine->obj;
	if(tts_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(tts_engine->task);
		apt_task_terminate(task,TRUE);
	}
	return mrcp_engine_close_respond(engine);
}

/* ---------- channel create ---------- */
/**
 * @brief 创建 TTS WebSocket 合成通道
 * @param engine MRCP 引擎对象
 * @param pool APR 内存池，用于分配内存
 * @return 返回 MRCP 引擎通道对象
 * 说明：
 *   1. 分配并初始化 tts_websocket_channel_t
 *   2. 设置媒体能力并创建音频终端
 *   3. 创建并返回 engine channel 基础
 */
static mrcp_engine_channel_t* tts_websocket_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool)
{
	mpf_stream_capabilities_t *capabilities;
	mpf_termination_t *termination; 

	/* create TTS WebSocket channel */
	tts_websocket_channel_t *synth_channel = apr_palloc(pool,sizeof(tts_websocket_channel_t));
	synth_channel->tts_engine = engine->obj;
	synth_channel->speak_request = NULL;
	synth_channel->stop_response = NULL;
	synth_channel->time_to_complete = 0;
	synth_channel->paused = FALSE;
	synth_channel->audio_file = NULL;

	/* ========== 打印通道创建日志和TTS服务器配置 ========== */
	apt_log(SYNTH_LOG_MARK, APT_PRIO_INFO, "ZyTTS: Creating new synth channel");
	apt_log(SYNTH_LOG_MARK, APT_PRIO_INFO, "ZyTTS: Channel will connect to TTS Server: %s:%d",
		synth_channel->tts_engine->tts_server_host,
		synth_channel->tts_engine->tts_server_port);

	/* ========== 初始化流式处理相关字段 ========== */
	synth_channel->stream_thread = NULL;
	synth_channel->stream_stop_requested = 0;
	synth_channel->stream_socket = NULL;
	synth_channel->stream_ws = NULL;
	synth_channel->stream_pool = NULL;
	tts_websocket_stream_lifecycle_init(&synth_channel->stream_lifecycle);
	synth_channel->stream_buffer = NULL;
	synth_channel->stream_buffer_size = 0;
	synth_channel->stream_write_pos = 0;
	synth_channel->stream_read_pos = 0;
	synth_channel->stream_buffer_mutex = NULL;  /* 修复：显式初始化为NULL */
	synth_channel->stream_buffer_cond = NULL;   /* 修复：显式初始化为NULL */
	synth_channel->stream_complete = 0;
	synth_channel->stream_error = 0;
	synth_channel->stream_codec_frame_size = 0;
	synth_channel->stream_output_state = TTS_WEBSOCKET_OUTPUT_IDLE;
	synth_channel->stream_postroll_ticks_left = 0;
	synth_channel->stream_drain_ticks_left = 0;
	synth_channel->stream_completion_cause = SYNTHESIZER_COMPLETION_CAUSE_NORMAL;
	/* 初始化录音文件指针 */
	synth_channel->record_file = NULL;
	synth_channel->record_file_orig = NULL;

	capabilities = mpf_source_stream_capabilities_create(pool);
	mpf_codec_capabilities_add(
			&capabilities->codecs,
			MPF_SAMPLE_RATE_8000,
			"PCMU");  /* 当前转换链固定输出 G.711 μ-law / 8 kHz */

	/* create media termination */
	termination = mrcp_engine_audio_termination_create(
			synth_channel,        /* object to associate */
			&audio_stream_vtable, /* virtual methods table of audio stream */
			capabilities,         /* stream capabilities */
			pool);                /* pool to allocate memory from */

	/* create engine channel base */
	synth_channel->channel = mrcp_engine_channel_create(
			engine,               /* engine */
			&channel_vtable,      /* virtual methods table of engine channel */
			synth_channel,        /* object to associate */
			termination,          /* associated media termination */
			pool);                /* pool to allocate memory from */

	return synth_channel->channel;
}

/* ========== 录音保存函数 ========== */

/**
 * @brief 打开录音文件（在 SPEAK 请求开始时调用）
 * @param synth_channel TTS WebSocket 合成通道对象
 * @param session_id MRCP session ID，用于文件命名
 * @param codec_name 编解码器名称（如 "PCMU"），用于文件后缀
 * @return 成功返回 TRUE
 *
 * 说明：
 *   1. 受 engine->recording_enabled 开关控制，关闭时不创建文件
 *   2. 同时打开两个文件：
 *      - record_file:     最终输出（8kHz μ-law），MRCP 客户端收到的格式
 *      - record_file_orig: 原始返回（24kHz PCM），TTS 服务端原始返回
 *   3. 文件名格式: tts-{fmt}-{session_id}-{timestamp}.{ext}
 *   4. 使用 sync 模式确保数据完整写入磁盘
 */
static apt_bool_t tts_websocket_recording_open(tts_websocket_channel_t *synth_channel,
                                            const char *session_id)
{
	tts_websocket_engine_t *tts_engine;
	const apt_dir_layout_t *dir_layout;
	apr_pool_t *pool;
	char *file_name_final;
	char *file_path_final;
	char *file_name_orig;
	char *file_path_orig;
	apr_time_t now;

	if (!synth_channel || !synth_channel->channel || !synth_channel->tts_engine) {
		return FALSE;
	}

	tts_engine = synth_channel->tts_engine;

	/* 录音开关：未启用时直接返回 */
	if (!tts_engine->recording_enabled) {
		LOG_WITH_SID(synth_channel, APT_PRIO_DEBUG, "[RECORD] Recording disabled, skip file creation");
		return TRUE;
	}

	dir_layout = synth_channel->channel->engine->dir_layout;
	pool = synth_channel->channel->pool;
	now = apr_time_now();

	/* 防止重复打开 */
	if (synth_channel->record_file || synth_channel->record_file_orig) {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[RECORD] Recording files already open, closing first");
		tts_websocket_recording_close(synth_channel);
	}

	/* --- 打开最终输出文件（8kHz μ-law） --- */
	file_name_final = apr_psprintf(pool, "tts-final-8kHz-%s-%"APR_TIME_T_FMT".pcmu",
		session_id ? session_id : "unknown", now);
	file_path_final = apt_vardir_filepath_get(dir_layout, file_name_final, pool);
	if (file_path_final) {
		synth_channel->record_file = fopen(file_path_final, "wb");
		if (synth_channel->record_file) {
			LOG_WITH_SID(synth_channel, APT_PRIO_NOTICE, "[RECORD] Final audio file opened: %s", file_path_final);
		} else {
			LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[RECORD] Failed to open final audio file: %s (errno=%d)",
				file_path_final, errno);
		}
	} else {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[RECORD] Failed to compose final audio file path");
	}

	/* --- 打开原始输出文件（24kHz PCM） --- */
	file_name_orig = apr_psprintf(pool, "tts-orig-24kHz-%s-%"APR_TIME_T_FMT".pcm",
		session_id ? session_id : "unknown", now);
	file_path_orig = apt_vardir_filepath_get(dir_layout, file_name_orig, pool);
	if (file_path_orig) {
		synth_channel->record_file_orig = fopen(file_path_orig, "wb");
		if (synth_channel->record_file_orig) {
			LOG_WITH_SID(synth_channel, APT_PRIO_NOTICE, "[RECORD] Original audio file opened: %s", file_path_orig);
		} else {
			LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[RECORD] Failed to open original audio file: %s (errno=%d)",
				file_path_orig, errno);
		}
	} else {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[RECORD] Failed to compose original audio file path");
	}

	return TRUE;
}

/**
 * @brief 写入最终输出音频数据（8kHz μ-law，MRCP 客户端收到的格式）
 * @param synth_channel TTS WebSocket 合成通道对象
 * @param data 音频数据指针
 * @param size 数据大小（字节数）
 *
 * 说明：
 *   在 tts_websocket_stream_read_safe() 的 MPF 回调上下文中调用，
 *   录制每一帧实际交给 MPF 的 AUDIO 帧（真实音频 + 静音填充帧 + post-roll），
 *   因此录音文件与客户端应收到的 RTP 载荷流逐帧一致，用于定位
 *   ring→RTP 之间的丢音。
 *   线程安全：仅由 MPF 调度线程调用（含 post-roll 分支），单线程写；
 *   文件在播放完成时由 read_safe 关闭，异常/停止路径由 cleanup_audio 幂等关闭。
 */
static void tts_websocket_recording_write_final(tts_websocket_channel_t *synth_channel,
                                             const char *data, apr_size_t size)
{
	if (!synth_channel || !synth_channel->record_file || !data || size == 0) {
		return;
	}

	if (fwrite(data, 1, size, synth_channel->record_file) != size) {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[RECORD] Failed to write final audio data (errno=%d)", errno);
	}
}

/**
 * @brief 写入原始音频数据（24kHz PCM，TTS 服务端原始返回）
 * @param synth_channel TTS WebSocket 合成通道对象
 * @param data 音频数据指针
 * @param size 数据大小（字节数）
 *
 * 说明：
 *   在 tts_websocket_stream_thread() 中调用，写入 TTS 服务端通过 WebSocket
 *   返回的原始 PCM 音频帧数据（重采样和格式转换之前）。
 *   线程安全：在独立接收线程中调用，与 MPF 主线程不冲突。
 */
static void tts_websocket_recording_write_orig(tts_websocket_channel_t *synth_channel,
                                            const char *data, apr_size_t size)
{
	if (!synth_channel || !synth_channel->record_file_orig || !data || size == 0) {
		return;
	}

	if (fwrite(data, 1, size, synth_channel->record_file_orig) != size) {
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[RECORD] Failed to write original audio data (errno=%d)", errno);
	}
}

/**
 * @brief 关闭录音文件并 flush 到磁盘
 * @param synth_channel TTS WebSocket 合成通道对象
 *
 * 说明：
 *   在 SPEAK-COMPLETE 或 channel cleanup 时调用。
 *   可重复调用（幂等），调用后 record_file 和 record_file_orig 置为 NULL。
 */
static void tts_websocket_recording_close(tts_websocket_channel_t *synth_channel)
{
	if (!synth_channel) {
		return;
	}

	if (synth_channel->record_file) {
		fflush(synth_channel->record_file);
		fclose(synth_channel->record_file);
		synth_channel->record_file = NULL;
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[RECORD] Final audio file closed");
	}

	if (synth_channel->record_file_orig) {
		fflush(synth_channel->record_file_orig);
		fclose(synth_channel->record_file_orig);
		synth_channel->record_file_orig = NULL;
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[RECORD] Original audio file closed");
	}
}

/* ---------- channel audio cleanup ---------- */
/**
 * @brief 清理通道音频资源
 * @param synth_channel TTS WebSocket 合成通道对象
 * 说明：
 *   关闭音频文件、重置内存缓冲区指针和位置
 *   修复：确保在清理资源之前，先等待接收线程完成
 */
static void tts_websocket_channel_cleanup_audio(tts_websocket_channel_t *synth_channel)
{
	tts_websocket_stream_lifecycle_begin_close(&synth_channel->stream_lifecycle);
	if(synth_channel->audio_file) {
		fclose(synth_channel->audio_file);
		synth_channel->audio_file = NULL;
	}
	synth_channel->time_to_complete = 0;
	synth_channel->speak_request = NULL;
	synth_channel->stop_response = NULL;
	synth_channel->paused = FALSE;

	/* ========== 修复：先通知线程停止 + 关闭 socket → 再 join ==========
	 * 之前先 join 再关闭 socket，但 WS 线程可能阻塞在 apr_socket_recv
	 * （30s 超时），导致 join 阻塞长达 30 秒，高并发下大量线程堆积。
	 * 现在先关闭 socket（使 recv 立即返回错误），线程快速退出，join 立即返回。 */
	if(synth_channel->stream_thread) {
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[CLEANUP] Stream thread still running, requesting stop...");

		/* 1. 设置停止标志 */
		synth_channel->stream_stop_requested = 1;

		/* 2. 通知条件变量（如果线程在等待缓冲区空间） */
		if(synth_channel->stream_buffer_cond) {
			apr_thread_cond_broadcast(synth_channel->stream_buffer_cond);
		}

		/* 3. 先关闭 socket — 使 WS 线程的 apr_socket_recv 立即返回错误，
		 *    线程快速退出，避免 join 长时间阻塞 */
		{
			apr_socket_t *sock = tts_websocket_detach_stream_socket(
				synth_channel, NULL);
			if(sock) {
				LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[CLEANUP] Closing socket to interrupt WS thread recv...");
				if(synth_channel->channel && synth_channel->channel->pool) {
					websocket_send_close(sock, synth_channel->channel->pool);
				}
				apr_sleep(10000);  /* 10ms — 让关闭帧发送出去 */
				apr_socket_close(sock);
			}
		}

		/* 4. 等待线程完成 — 此时 socket 已关闭，recv 已中断，join 立即返回 */
		apr_status_t rv;
		apr_status_t thread_retval;
		rv = apr_thread_join(&thread_retval, synth_channel->stream_thread);
		if(rv != APR_SUCCESS) {
			LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[CLEANUP] Thread join failed with status %d", rv);
		}
		synth_channel->stream_thread = NULL;
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[CLEANUP] Stream thread completed");
	}

	/* socket 已在上面关闭，此处仅防御性检查 */
	{
		apr_socket_t *sock = tts_websocket_detach_stream_socket(
			synth_channel, NULL);
		if(sock) {
			LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[CLEANUP] Socket still exists (unexpected), closing...");
			if(synth_channel->channel && synth_channel->channel->pool) {
				websocket_send_close(sock, synth_channel->channel->pool);
			}
			apr_sleep(10000);
			apr_socket_close(sock);
		}
	}

	/* worker 已退出，此处仅做失败路径的幂等兜底关闭。 */
	tts_websocket_recording_close(synth_channel);
	/* worker 已 join 且 socket 已关闭，当前 SPEAK 的大块缓冲、握手 surplus
	 * 和临时 JSON 均可一次性回收，避免通道复用导致内存池线性增长。 */
	tts_websocket_stream_pool_destroy(synth_channel);

	/* ========== 修复：不销毁同步对象，因为它们是从channel pool分配的，可以重用 ==========
	 * channel销毁时会自动清理这些对象
	 * 在这里销毁会导致第二次请求时重新创建，可能引发APR内部状态问题导致崩溃
	 */

	/* 重置缓冲区指针 */
	synth_channel->stream_buffer = NULL;
	synth_channel->stream_buffer_size = 0;
	synth_channel->stream_write_pos = 0;
	synth_channel->stream_read_pos = 0;
	synth_channel->stream_complete = 0;
	synth_channel->stream_error = 0;
	synth_channel->stream_output_state = TTS_WEBSOCKET_OUTPUT_IDLE;
	synth_channel->stream_postroll_ticks_left = 0;
	synth_channel->stream_drain_ticks_left = 0;
	synth_channel->stream_completion_cause = SYNTHESIZER_COMPLETION_CAUSE_NORMAL;
}

/* ---------- channel destroy ---------- */
/**
 * @brief 销毁引擎通道
 * @param channel MRCP 引擎通道对象
 * @return 成功返回 TRUE
 * 说明：
 *   清理音频资源后销毁通道
 */
static apt_bool_t tts_websocket_channel_destroy(mrcp_engine_channel_t *channel)
{
	tts_websocket_channel_t *synth_channel = channel->method_obj;
	tts_websocket_channel_cleanup_audio(synth_channel);
	return TRUE;
}

/* ---------- channel open ---------- */
/**
 * @brief 打开引擎通道（异步）
 * @param channel MRCP 引擎通道对象
 * @return 成功返回 TRUE
 * 说明：
 *   将打开请求封装为任务消息并发送到消费者任务，由任务线程完成异步响应
 */
static apt_bool_t tts_websocket_channel_open(mrcp_engine_channel_t *channel)
{
	return tts_websocket_msg_signal(TTS_WEBSOCKET_MSG_OPEN_CHANNEL,channel,NULL);
}

/* ---------- channel close ---------- */
/**
 * @brief 关闭引擎通道（异步）
 * @param channel MRCP 引擎通道对象
 * @return 成功返回 TRUE
 * 说明：
 *   将关闭请求封装为任务消息并发送到消费者任务，由任务线程完成异步响应
 */
static apt_bool_t tts_websocket_channel_close(mrcp_engine_channel_t *channel)
{
	return tts_websocket_msg_signal(TTS_WEBSOCKET_MSG_CLOSE_CHANNEL,channel,NULL);
}

/* ---------- channel request process ---------- */
/**
 * @brief 处理 MRCP 通道请求（异步）
 * @param channel MRCP 引擎通道对象
 * @param request MRCP 请求消息
 * @return 成功返回 TRUE
 * 说明：
 *   将 MRCP 请求封装为任务消息并发送到消费者任务，由任务线程完成具体处理
 */
static apt_bool_t tts_websocket_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	return tts_websocket_msg_signal(TTS_WEBSOCKET_MSG_REQUEST_PROCESS,channel,request);
}

/* ---------- audio resample function ---------- */
/**
 * @brief 将24kHz PCM音频降采样到8kHz（带抗混叠滤波）
 * @param input_pcm 输入PCM数据（16位单声道）
 * @param input_size 输入数据大小（字节数）
 * @param output_size 输出参数，返回输出数据大小
 * @param pool APR内存池
 * @return 返回重采样后的PCM数据，失败返回NULL
 * 说明：
 *   1. 输入为24kHz采样率（TTS服务返回）
 *   2. 使用3:1降采样，先进行移动平均滤波再抽取
 *   3. 输出为8kHz采样率，16位单声道PCM
 * @deprecated 流式场景请使用 resample_pcm_to_8k_stateful 避免帧边界爆音
 */
static char* resample_pcm_to_8k(const char *input_pcm, apr_size_t input_size, apr_size_t *output_size, apr_pool_t *pool)
{
	/* 降采样比例 = 24000 / 8000 = 3 */
	const int decimation_factor = 3;
	apr_size_t input_samples = input_size / 2;  /* 16位采样，2字节/采样 */
	apr_size_t output_samples = input_samples / decimation_factor;
	apr_size_t output_bytes = output_samples * 2;
	unsigned char *input_bytes = (unsigned char*)input_pcm;
	short *output;
	apr_size_t i;

	if(!input_pcm || input_size == 0 || !output_size || !pool) {
		return NULL;
	}

	/* ========== 修复：防御性检查 — 输入大小必须是6字节（3个16-bit采样）的整数倍 ==========
	 * 非对齐输入会导致末尾采样被整数除法静默丢弃。
	 * 虽然调用方通过 pcm_accum 机制保证了 6 字节对齐，但此检查可防止
	 * 未来代码变更引入非对齐调用导致的数据丢失。 */
	if(input_size % 6 != 0) {
		apt_log(SYNTH_LOG_MARK, APT_PRIO_WARNING,
			"zyTTS: resample_pcm_to_8k: input_size %"APR_SIZE_T_FMT" is not a multiple of 6, "
			"last %"APR_SIZE_T_FMT" bytes will be dropped",
			input_size, input_size % 6);
	}

	output = apr_palloc(pool, output_bytes);
	if(!output) {
		apt_log(SYNTH_LOG_MARK,APT_PRIO_WARNING,"zyTTS: Failed to allocate memory for resampled audio");
		return NULL;
	}

	/* 调试：打印原始前几个字节 */
	apt_log(SYNTH_LOG_MARK,APT_PRIO_DEBUG,"zyTTS: Resample input: first 10 bytes = %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
		input_bytes[0], input_bytes[1], input_bytes[2], input_bytes[3], input_bytes[4],
		input_bytes[5], input_bytes[6], input_bytes[7], input_bytes[8], input_bytes[9]);

	/* 降采样：使用移动平均滤波来减少混叠失真
	 * 每3个采样点取平均值，而不是简单抽取
	 * 注意：TTS服务返回的音频通常是little-endian格式
	 */
	for(i = 0; i < output_samples; i++) {
		int sum = 0;
		/* 对每3个采样点求平均 */
		apr_size_t j;
		for(j = 0; j < decimation_factor; j++) {
			/* 从little-endian字节序读取16位采样值 */
			apr_size_t sample_idx = (i * decimation_factor + j) * 2;
			short sample = (short)((input_bytes[sample_idx + 1] << 8) | input_bytes[sample_idx]);
			sum += sample;
		}
		output[i] = (short)(sum / decimation_factor);
	}

	/* 调试：打印输出前几个采样值 */
	apt_log(SYNTH_LOG_MARK,APT_PRIO_DEBUG,"zyTTS: Resample output: first 5 samples = %d, %d, %d, %d, %d",
		output[0], output[1], output[2], output[3], output[4]);

	*output_size = output_bytes;
	apt_log(SYNTH_LOG_MARK,APT_PRIO_DEBUG,"zyTTS: Resampled audio: %d samples (%d bytes) -> %d samples (%d bytes)",
		(int)input_samples, (int)input_size, (int)output_samples, (int)output_bytes);

	return (char*)output;
}

/* ---------- audio bit depth conversion function (16-bit linear to 8-bit mu-law) ---------- */
/**
 * @brief μ-law编码查找表 (ITU-T G.711标准)
 * 将14位有符号数 (-8159 到 8159) 转换为8位μ-law编码
 * @return 8位μ-law编码字节
 *
 * 使用标准ITU-T G.711 μ-law编码算法
 */
static inline unsigned char linear_to_ulaw(short sample)
{
	int magnitude;
	int exponent;
	int mantissa;
	int exponent_mask;
	unsigned char ulaw_byte;

	/* 处理符号和幅度，处理-32768边界情况 */
	/* 注意：C标准规定带符号负数的右移是实现定义行为，改用比较判断避免移植风险 */
	int sign = (sample < 0) ? 0x80 : 0;
	if (sign) {
		magnitude = (sample == -32768) ? 32767 : -sample;
	} else {
		magnitude = sample;
	}

	/* 添加偏移量 BIAS = 0x84 = 132 */
	magnitude += 0x84;
	/* 限制幅度在15位范围内 (0-32767)，防止溢出 */
	if (magnitude > 32767) {
		magnitude = 32767;
	}

	/* 计算指数 (3位) */
	exponent = 7;
	for(exponent_mask = 0x4000; !(magnitude & exponent_mask); exponent_mask >>= 1) {
		exponent--;
	}

	/* 提取尾数 (4位) */
	mantissa = (magnitude >> (exponent + 3)) & 0x0F;

	/* 组合: S EEEE MMMM */
	ulaw_byte = (unsigned char)(sign | (exponent << 4) | mantissa);

	/* 按位取反 (μ-law特性) */
	return ~ulaw_byte;
}

/**
 * @brief 将16位线性PCM音频批量转换为8位μ-law音频 (PCMU/G.711)
 * @param input_pcm 输入PCM数据（16位单声道线性）
 * @param input_size 输入数据大小（字节数）
 * @param output_size 输出参数，返回输出数据大小
 * @param pool APR内存池
 * @return 返回8位μ-law数据，失败返回NULL
 * 说明：
 *   1. 输入为16位有符号线性PCM（每个采样2字节）
 *   2. 输出为8位μ-law压缩编码（每个采样1字节）
 *   3. μ-law是ITU-T G.711标准，用于电话通信，PCMU格式
 */
static char* convert_16bit_to_ulaw(const char *input_pcm, apr_size_t input_size, apr_size_t *output_size, apr_pool_t *pool)
{
	apr_size_t input_samples = input_size / 2;  /* 16位采样，2字节/采样 */
	apr_size_t output_bytes = input_samples;    /* 8位μ-law采样，1字节/采样 */
	short *input = (short*)input_pcm;
	unsigned char *output;

	if(!input_pcm || input_size == 0 || !output_size || !pool) {
		return NULL;
	}

	output = apr_palloc(pool, output_bytes);
	if(!output) {
		apt_log(SYNTH_LOG_MARK,APT_PRIO_WARNING,"zyTTS: Failed to allocate memory for μ-law audio");
		return NULL;
	}

	/* 批量转换: 16-bit linear -> 8-bit μ-law */
	apr_size_t i;
	for(i = 0; i < input_samples; i++) {
		output[i] = linear_to_ulaw(input[i]);
	}

	*output_size = output_bytes;
	apt_log(SYNTH_LOG_MARK,APT_PRIO_DEBUG,"zyTTS: μ-law conversion: %d samples (%d bytes 16bit linear) -> %d samples (%d bytes 8bit μ-law/PCMU)",
		(int)input_samples, (int)input_size, (int)input_samples, (int)output_bytes);

	return (char*)output;
}

/* ---------- audio resample function for 8-bit μ-law ---------- */
/**
 * @brief 将24kHz μ-law音频降采样到8kHz
 * @param input_ulaw 输入μ-law数据（8位单声道）
 * @param input_size 输入数据大小（字节数）
 * @param output_size 输出参数，返回输出数据大小
 * @param pool APR内存池
 * @return 返回重采样后的μ-law数据，失败返回NULL
 * 说明：
 *   1. 输入为24kHz采样率的8bit μ-law数据
 *   2. 使用3:1降采样，简单抽取
 *   3. 输出为8kHz采样率的8bit μ-law数据
 */
static char* resample_ulaw_to_8k(const char *input_ulaw, apr_size_t input_size, apr_size_t *output_size, apr_pool_t *pool)
{
	/* 降采样比例 = 24000 / 8000 = 3 */
	const int decimation_factor = 3;
	apr_size_t input_samples = input_size;  /* 8位采样，1字节/采样 */
	apr_size_t output_samples = input_samples / decimation_factor;
	apr_size_t output_bytes = output_samples;
	unsigned char *output;

	if(!input_ulaw || input_size == 0 || !output_size || !pool) {
		return NULL;
	}

	output = apr_palloc(pool, output_bytes);
	if(!output) {
		apt_log(SYNTH_LOG_MARK,APT_PRIO_WARNING,"zyTTS: Failed to allocate memory for resampled μ-law audio");
		return NULL;
	}

	/* 调试：打印原始前几个字节 */
	apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"zyTTS: Resample μ-law input: first 10 bytes = %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
		(unsigned char)input_ulaw[0], (unsigned char)input_ulaw[1], (unsigned char)input_ulaw[2],
		(unsigned char)input_ulaw[3], (unsigned char)input_ulaw[4], (unsigned char)input_ulaw[5],
		(unsigned char)input_ulaw[6], (unsigned char)input_ulaw[7], (unsigned char)input_ulaw[8],
		(unsigned char)input_ulaw[9]);

	/* 降采样：简单抽取，每3个采样点取1个 */
	apr_size_t i;
	for(i = 0; i < output_samples; i++) {
		output[i] = input_ulaw[i * decimation_factor];
	}

	/* 调试：打印输出前几个采样值 */
	apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"zyTTS: Resample μ-law output: first 5 samples = 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X",
		output[0], output[1], output[2], output[3], output[4]);

	*output_size = output_bytes;
	apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"zyTTS: Resampled μ-law audio: %d samples (%d bytes) -> %d samples (%d bytes)",
		(int)input_samples, (int)input_size, (int)output_samples, (int)output_bytes);

	return (char*)output;
}

/* ---------- request: SPEAK ---------- */
/**
 * @brief 处理 SPEAK 请求
 * @param channel MRCP 引擎通道对象
 * @param request SPEAK 请求消息
 * @param response 为该请求准备的响应消息
 * @return 成功返回 TRUE，失败返回 FALSE
 * 说明：
 *   1. 清理旧的音频资源
 *   2. 获取目标音频源（优先查找本地音频文件）
 *   3. 若本地文件不可用：
 *      a) 尝试调用外部 TTS WebSocket 服务
 *      b) 若 TTS 调用失败或无请求体，根据头信息估算完成时间
 *   4. 将请求标记为 INPROGRESS 并发送异步响应
 */
static apt_bool_t tts_websocket_channel_speak(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"Received SPEAK request thread_id=%lu " APT_SIDRES_FMT,
		tts_websocket_thread_id_current(), MRCP_MESSAGE_SIDRES(request));
	/* char *file_path = NULL;  // 已注释：本地文件回退功能已禁用 */
	tts_websocket_channel_t *synth_channel = channel->method_obj;
	const mpf_codec_descriptor_t *descriptor = mrcp_engine_source_stream_codec_get(channel);

	/* 清理之前可能残留的音频资源 */
	tts_websocket_channel_cleanup_audio(synth_channel);

	if(!descriptor || descriptor->sampling_rate != 8000) {
		apt_log(SYNTH_LOG_MARK,APT_PRIO_WARNING,"Failed to Get Codec Descriptor " APT_SIDRES_FMT, MRCP_MESSAGE_SIDRES(request));
		response->start_line.status_code = MRCP_STATUS_CODE_METHOD_FAILED;
		mrcp_engine_channel_message_send(channel,response);
		return FALSE;
	}

	/* Priority 1: Try TTS service first */
	if(request->body.buf && request->body.length > 0) {
		tts_websocket_engine_t *tts_engine = synth_channel->tts_engine;
		apr_size_t resp_size = 0;
		mrcp_generic_header_t *generic_header = mrcp_generic_header_get(request);
		const char *content_type = generic_header && generic_header->content_type.buf ?
			generic_header->content_type.buf : "(not set)";

		LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "Attempting to call TTS service %s:%d",
			tts_engine->tts_server_host, tts_engine->tts_server_port);
		LOG_WITH_SID(synth_channel, APT_PRIO_DEBUG, "Before TTS service request, text length=%"APR_SIZE_T_FMT" content_type=%s",
			request->body.length, content_type);

		/* 打印原始请求体的十六进制转储，用于调试编码问题 */
		hex_dump("MRCP request body", request->body.buf, request->body.length);

		/* ========== 使用流式TTS接收 ========== */
		const char *voice_name = NULL;
		mrcp_synth_header_t *synth_header = mrcp_resource_header_get(request);
		if(synth_header && mrcp_resource_header_property_check(request, SYNTHESIZER_HEADER_VOICE_NAME) == TRUE) {
			voice_name = synth_header->voice_param.name.buf;
		}

		/* 打开录音文件（在启动流式接收之前，确保线程启动后即可写入） */
		tts_websocket_recording_open(synth_channel, request->channel_id.session_id.buf);

		/* 在建连/握手前发布本次不可变请求上下文，保证 session.config
		 * 的 call_id 与日志使用当前 SPEAK，而不是上一次会话或空值。 */
		synth_channel->speak_request = request;
		apt_bool_t stream_started = tts_websocket_start_streaming(synth_channel, request->body.buf, request->body.length, voice_name);

		if(stream_started) {
			/* 流式接收已启动，立即返回响应 */
			LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[STREAM] Streaming started successfully");

			/* 发送SPEAK响应 */
				response->start_line.status_code = MRCP_STATUS_CODE_SUCCESS;
				response->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
				mrcp_engine_channel_message_send(channel,response);
				return TRUE;
		} else {
			/* 流式启动失败：直接返回错误给MRCP客户端，不回退到时间估算或本地文件 */
			LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "[STREAM] Failed to start streaming, returning error to MRCP client");
			/* 关闭已打开的录音文件 */
			tts_websocket_recording_close(synth_channel);
			/* 启动失败也回收本次 SPEAK 子池和 socket 状态。 */
			tts_websocket_channel_cleanup_audio(synth_channel);
			/* 清空speak_request */
			synth_channel->speak_request = NULL;
			/* 发送错误响应 */
			response->start_line.status_code = MRCP_STATUS_CODE_METHOD_FAILED;
			response->start_line.request_state = MRCP_REQUEST_STATE_COMPLETE;
			mrcp_engine_channel_message_send(channel,response);
			return TRUE;
		}
	}
	else {
		/* No request body: 返回错误 */
		LOG_WITH_SID(synth_channel, APT_PRIO_WARNING, "SPEAK request has no body content, returning error to MRCP client");
		synth_channel->speak_request = NULL;
		response->start_line.status_code = MRCP_STATUS_CODE_METHOD_FAILED;
		response->start_line.request_state = MRCP_REQUEST_STATE_COMPLETE;
		mrcp_engine_channel_message_send(channel,response);
		return TRUE;
	}

	response->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	synth_channel->speak_request = request;
	return TRUE;
}

/* ---------- request: STOP ---------- */
/**
 * @brief 处理 STOP 请求
 * @param channel MRCP 引擎通道对象
 * @param request STOP 请求消息
 * @param response 为该请求准备的响应消息
 * @return 成功返回 TRUE
 * 说明：
 *   1. 保存 STOP 响应，实际发送将在确保播放停止后由音频流处理逻辑完成
 */
static apt_bool_t tts_websocket_channel_stop(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	tts_websocket_channel_t *synth_channel = channel->method_obj;
	/* store the request, make sure there is no more activity and only then send the response */
	synth_channel->stop_response = response;
	return TRUE;
}

/* ---------- request: PAUSE ---------- */
/**
 * @brief 处理 PAUSE 请求
 * @param channel MRCP 引擎通道对象
 * @param request PAUSE 请求消息
 * @param response 为该请求准备的响应消息
 * @return 成功返回 TRUE
 * 说明：
 *   将通道置为暂停状态并发送异步响应，音频流读取在暂停期间会被阻塞
 */
static apt_bool_t tts_websocket_channel_pause(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	tts_websocket_channel_t *synth_channel = channel->method_obj;
	synth_channel->paused = TRUE;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	return TRUE;
}

/* ---------- request: RESUME ---------- */
/**
 * @brief 处理 RESUME 请求
 * @param channel MRCP 引擎通道对象
 * @param request RESUME 请求消息
 * @param response 为该请求准备的响应消息
 * @return 成功返回 TRUE
 * 说明：
 *   清除暂停状态并发送异步响应，恢复音频播放
 */
static apt_bool_t tts_websocket_channel_resume(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	tts_websocket_channel_t *synth_channel = channel->method_obj;
	synth_channel->paused = FALSE;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	return TRUE;
}

/* ---------- request: SET-PARAMS ---------- */
/**
 * @brief 处理 SET-PARAMS 请求
 * @param channel MRCP 引擎通道对象
 * @param request SET-PARAMS 请求消息
 * @param response 为该请求准备的响应消息
 * @return 成功返回 TRUE
 * 说明：
 *   从请求头读取合成器参数（如 voice age/name）并记录或应用
 */
static apt_bool_t tts_websocket_channel_set_params(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	mrcp_synth_header_t *req_synth_header;
	/* get synthesizer header */
	req_synth_header = mrcp_resource_header_get(request);
	if(req_synth_header) {
		/* check voice age header */
		if(mrcp_resource_header_property_check(request,SYNTHESIZER_HEADER_VOICE_AGE) == TRUE) {
			apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"Set Voice Age [%"APR_SIZE_T_FMT"]",
				req_synth_header->voice_param.age);
		}
		/* check voice name header */
		if(mrcp_resource_header_property_check(request,SYNTHESIZER_HEADER_VOICE_NAME) == TRUE) {
			apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"Set Voice Name [%s]",
				req_synth_header->voice_param.name.buf);
		}
	}
	
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	return TRUE;
}

/* ---------- request: GET-PARAMS ---------- */
/**
 * @brief 处理 GET-PARAMS 请求
 * @param channel MRCP 引擎通道对象
 * @param request GET-PARAMS 请求消息
 * @param response 为该请求准备的响应消息
 * @return 成功返回 TRUE
 * 说明：
 *   准备并填写响应中的合成器参数（当前实现返回固定值，例如 voice name/age）
 */
static apt_bool_t tts_websocket_channel_get_params(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	mrcp_synth_header_t *req_synth_header;
	/* get synthesizer header */
	req_synth_header = mrcp_resource_header_get(request);
	if(req_synth_header) {
		mrcp_synth_header_t *res_synth_header = mrcp_resource_header_prepare(response);
		/* check voice age header */
		if(mrcp_resource_header_property_check(request,SYNTHESIZER_HEADER_VOICE_AGE) == TRUE) {
			res_synth_header->voice_param.age = 25;
			mrcp_resource_header_property_add(response,SYNTHESIZER_HEADER_VOICE_AGE);
		}
		/* check voice name header */
		if(mrcp_resource_header_property_check(request,SYNTHESIZER_HEADER_VOICE_NAME) == TRUE) {
			apt_string_set(&res_synth_header->voice_param.name,"David");
			mrcp_resource_header_property_add(response,SYNTHESIZER_HEADER_VOICE_NAME);
		}
	}

	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	return TRUE;
}

/* ---------- request dispatch ---------- */
/**
 * @brief 分发 MRCP 请求
 * @param channel MRCP 引擎通道对象
 * @param request MRCP 请求消息
 * @return 成功返回 TRUE
 * 说明：
 *   根据请求的 method_id 调用对应的处理函数，未处理的请求发送默认异步响应
 */
static apt_bool_t tts_websocket_channel_request_dispatch(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_bool_t processed = FALSE;
	mrcp_message_t *response = mrcp_response_create(request,request->pool);
	switch(request->start_line.method_id) {
		case SYNTHESIZER_SET_PARAMS:
			processed = tts_websocket_channel_set_params(channel,request,response);
			break;
		case SYNTHESIZER_GET_PARAMS:
			processed = tts_websocket_channel_get_params(channel,request,response);
			break;
		case SYNTHESIZER_SPEAK:
			processed = tts_websocket_channel_speak(channel,request,response);
			break;
		case SYNTHESIZER_STOP:
			processed = tts_websocket_channel_stop(channel,request,response);
			break;
		case SYNTHESIZER_PAUSE:
			processed = tts_websocket_channel_pause(channel,request,response);
			break;
		case SYNTHESIZER_RESUME:
			processed = tts_websocket_channel_resume(channel,request,response);
			break;
		case SYNTHESIZER_BARGE_IN_OCCURRED:
			processed = tts_websocket_channel_stop(channel,request,response);
			break;
		case SYNTHESIZER_CONTROL:
			break;
		case SYNTHESIZER_DEFINE_LEXICON:
			break;
		default:
			break;
	}
	if(processed == FALSE) {
		/* send asynchronous response for not handled request */
		mrcp_engine_channel_message_send(channel,response);
	}
	return TRUE;
}

/* ---------- stream destroy ---------- */
/**
 * @brief 销毁音频流回调（MPF 上下文）
 * @param stream MPF 音频流对象
 * @return 成功返回 TRUE
 * 说明：
 *   在 MPF 引擎上下文中被调用，用于释放音频流相关的额外数据（当前无额外操作）
 */
static apt_bool_t tts_websocket_stream_destroy(mpf_audio_stream_t *stream)
{
	return TRUE;
}

/* ---------- stream open ---------- */
/**
 * @brief 打开音频流回调（MPF 上下文）
 * @param stream MPF 音频流对象
 * @param codec 选定的编解码器
 * @return 成功返回 TRUE
 * 说明：
 *   在流打开前进行必要准备（当前无额外操作）
 */
static apt_bool_t tts_websocket_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec)
{
	tts_websocket_channel_t *synth_channel = stream ? stream->obj : NULL;
	if(synth_channel) {
		synth_channel->stream_codec_frame_size = 0;
	}
	return TRUE;
}

/* ---------- stream close ---------- */
/**
 * @brief 关闭音频流回调（MPF 上下文）
 * @param stream MPF 音频流对象
 * @return 成功返回 TRUE
 * 说明：
 *   在流关闭后进行必要清理（当前无额外操作）
 */
static apt_bool_t tts_websocket_stream_close(mpf_audio_stream_t *stream)
{
	tts_websocket_channel_t *synth_channel = stream ? stream->obj : NULL;
	if(synth_channel) {
		synth_channel->stream_codec_frame_size = 0;
	}
	return TRUE;
}

/* Start an explicit end-of-stream sequence.
 *
 * POSTROLL keeps the RTP timeline alive until a client-side jitter buffer has
 * played the real tail. RTP_DRAIN gives MPF enough callbacks to flush a
 * partial packet, without adding a new RTP packet of its own. The drain stage
 * is required because MPF calls the source before the RTP sink: an event sent
 * from the first empty EOF callback can otherwise overtake a partial final
 * RTP packet.
 */
static void tts_websocket_output_finish_begin(
	tts_websocket_channel_t *synth_channel,
	mpf_audio_stream_t *stream,
	apt_bool_t terminal_error)
{
	apr_uint32_t frame_duration_ms = 10;
	apr_uint32_t packet_frames;

	if(synth_channel->stream_output_state != TTS_WEBSOCKET_OUTPUT_STREAMING) {
		return;
	}
	if(stream && stream->rx_descriptor && stream->rx_descriptor->frame_duration > 0) {
		frame_duration_ms = stream->rx_descriptor->frame_duration;
	}

	synth_channel->stream_postroll_ticks_left =
		(synth_channel->tts_engine->completion_postroll_ms + frame_duration_ms - 1)
		/ frame_duration_ms;
	packet_frames = synth_channel->tts_engine->rtp_ptime_ms / frame_duration_ms;
	if(packet_frames == 0) {
		packet_frames = 1;
	}
	synth_channel->stream_drain_ticks_left = packet_frames - 1;
	synth_channel->stream_completion_cause = terminal_error
		? SYNTHESIZER_COMPLETION_CAUSE_ERROR
		: SYNTHESIZER_COMPLETION_CAUSE_NORMAL;

	if(synth_channel->stream_postroll_ticks_left > 0) {
		synth_channel->stream_output_state = TTS_WEBSOCKET_OUTPUT_POSTROLL;
	} else if(synth_channel->stream_drain_ticks_left > 0) {
		synth_channel->stream_output_state = TTS_WEBSOCKET_OUTPUT_RTP_DRAIN;
	} else {
		synth_channel->stream_output_state = TTS_WEBSOCKET_OUTPUT_COMPLETE_READY;
	}

	LOG_WITH_SID(synth_channel, APT_PRIO_DEBUG,
		"[STREAM] EOF: post-roll=%u ticks, RTP drain=%u ticks, cause=%d",
		(unsigned)synth_channel->stream_postroll_ticks_left,
		(unsigned)synth_channel->stream_drain_ticks_left,
		synth_channel->stream_completion_cause);
}

/* Real-time-safe MPF source callback.
 *
 * It never waits for the WebSocket producer, always returns fixed-size PCMU
 * frames, preserves incomplete producer chunks, and separates the last AUDIO
 * frame, RTP packetizer drain, and SPEAK-COMPLETE into distinct MPF ticks.
 */
static apt_bool_t tts_websocket_stream_read_safe(mpf_audio_stream_t *stream, mpf_frame_t *frame)
{
	tts_websocket_channel_t *synth_channel = stream ? stream->obj : NULL;
	mrcp_message_t *active_request;
	apr_size_t frame_size;
	apr_size_t bytes_read = 0;
	apr_size_t available = 0;
	apt_bool_t eof = FALSE;
	apt_bool_t completed = FALSE;
	apt_bool_t terminal_error = FALSE;
	apt_bool_t audio_started = FALSE;
	apt_bool_t stream_complete = FALSE;
	apt_bool_t read_audio = TRUE;
	apt_bool_t lifecycle_entered = FALSE;

	if(!synth_channel || !frame || !frame->codec_frame.buffer ||
	   frame->codec_frame.size == 0) {
		return TRUE;
	}
	/* MPF may reuse the frame object; a callback rejected during cleanup must
	 * not leak a previous AUDIO flag into the current RTP tick. */
	frame->type = MEDIA_FRAME_TYPE_NONE;
	if(!tts_websocket_stream_lifecycle_enter(&synth_channel->stream_lifecycle)) {
		return TRUE;
	}
	lifecycle_entered = TRUE;

	/* MPF bridge reuses this frame. Never let a short EOF tail shrink it. */
	if(synth_channel->stream_codec_frame_size == 0) {
		synth_channel->stream_codec_frame_size = frame->codec_frame.size;
	}
	frame_size = synth_channel->stream_codec_frame_size;
	frame->codec_frame.size = frame_size;
	/* NONE callbacks can still be consumed by MPF while flushing a partial
	 * packet. Keep the backing bytes deterministic even when no AUDIO frame is
	 * emitted. */
	tts_websocket_pcm_fill_silence(frame->codec_frame.buffer, frame_size);

	if(synth_channel->stop_response) {
		mrcp_engine_channel_message_send(
			synth_channel->channel, synth_channel->stop_response);
		synth_channel->stop_response = NULL;
		synth_channel->speak_request = NULL;
		synth_channel->paused = FALSE;
		synth_channel->stream_stop_requested = 1;
		synth_channel->stream_output_state = TTS_WEBSOCKET_OUTPUT_IDLE;
		synth_channel->stream_postroll_ticks_left = 0;
		synth_channel->stream_drain_ticks_left = 0;
		if(synth_channel->stream_buffer_cond) {
			apr_thread_cond_broadcast(synth_channel->stream_buffer_cond);
		}
		goto stream_read_done;
	}
	if(synth_channel->paused) {
		goto stream_read_done;
	}

	active_request = synth_channel->speak_request;
	if(!active_request) {
		goto stream_read_done;
	}

	/* Drain states must run before ordinary underrun handling. Keep the callback
	 * frame initialized because MPF copies codec_frame.buffer while flushing a
	 * pending packet, but leave its type as NONE so a drain tick cannot create a
	 * fresh RTP packet. */
	if(synth_channel->stream_output_state == TTS_WEBSOCKET_OUTPUT_POSTROLL) {
		memset(frame->codec_frame.buffer, 0xFF, frame_size);
		frame->type |= MEDIA_FRAME_TYPE_AUDIO;
		synth_channel->stream_silence_frame_count++;
		/* 诊断录音：post-roll 静音帧同样发给客户端，一并录制 */
		if(synth_channel->record_file) {
			tts_websocket_recording_write_final(synth_channel, frame->codec_frame.buffer, frame_size);
		}
		if(synth_channel->stream_postroll_ticks_left > 0) {
			synth_channel->stream_postroll_ticks_left--;
		}
		if(synth_channel->stream_postroll_ticks_left == 0) {
			synth_channel->stream_output_state =
				synth_channel->stream_drain_ticks_left > 0
				? TTS_WEBSOCKET_OUTPUT_RTP_DRAIN
				: TTS_WEBSOCKET_OUTPUT_COMPLETE_READY;
		}
		goto stream_read_done;
	}
	if(synth_channel->stream_output_state == TTS_WEBSOCKET_OUTPUT_RTP_DRAIN) {
		/* MPF's RTP packetizer may use a NONE frame to flush a partial packet,
		 * but it still copies codec_frame.buffer/size in that path. Supplying an
		 * initialized reserve prevents stale callback-buffer bytes from becoming
		 * RTP payload. */
		tts_websocket_pcm_fill_silence(frame->codec_frame.buffer, frame_size);
		if(synth_channel->stream_drain_ticks_left > 0) {
			synth_channel->stream_drain_ticks_left--;
		}
		if(synth_channel->stream_drain_ticks_left == 0) {
			synth_channel->stream_output_state = TTS_WEBSOCKET_OUTPUT_COMPLETE_READY;
		}
		goto stream_read_done;
	}
	if(synth_channel->stream_output_state == TTS_WEBSOCKET_OUTPUT_COMPLETE_READY) {
		completed = TRUE;
		terminal_error =
			synth_channel->stream_completion_cause == SYNTHESIZER_COMPLETION_CAUSE_ERROR;
	}

	if(!completed && synth_channel->audio_file) {
		bytes_read = fread(frame->codec_frame.buffer, 1, frame_size, synth_channel->audio_file);
		if(bytes_read > 0) {
			if(bytes_read < frame_size) {
				memset((char*)frame->codec_frame.buffer + bytes_read, 0xFF,
					frame_size - bytes_read);
				tts_websocket_output_finish_begin(synth_channel, stream, FALSE);
			}
			frame->type |= MEDIA_FRAME_TYPE_AUDIO;
		} else {
			tts_websocket_output_finish_begin(synth_channel, stream, FALSE);
		}
		} else if(!completed && synth_channel->stream_buffer &&
		          synth_channel->stream_buffer_size > 1) {
			if(!synth_channel->stream_buffer_mutex ||
			   apr_thread_mutex_trylock(synth_channel->stream_buffer_mutex) != APR_SUCCESS) {
				/* MPF 回调不得等待 producer。锁竞争时跳过本次回调，
				 * 不向 RTP 注入一整帧伪静音，避免把合法音频时间轴改写成静音。 */
				synth_channel->stream_trylock_fail_count++;
				goto stream_read_done;
			} else {
			audio_started = synth_channel->stream_audio_started ? TRUE : FALSE;
			stream_complete = synth_channel->stream_complete ? TRUE : FALSE;
			terminal_error = synth_channel->stream_error ? TRUE : FALSE;
			available = (synth_channel->stream_write_pos + synth_channel->stream_buffer_size
				- synth_channel->stream_read_pos) % synth_channel->stream_buffer_size;

			if(!audio_started) {
				if(stream_complete) {
					completed = TRUE;
					synth_channel->stream_completion_cause = terminal_error
						? SYNTHESIZER_COMPLETION_CAUSE_ERROR
						: SYNTHESIZER_COMPLETION_CAUSE_NORMAL;
				} else {
					/* Wait for the first real chunk instead of moving the RTP clock
					 * with synthetic silence. */
					read_audio = FALSE;
				}
			} else if(!synth_channel->stream_prebuffered) {
				if(stream_complete || available >= synth_channel->stream_prebuffer_min_fill) {
					synth_channel->stream_prebuffered = 1;
				} else {
					read_audio = FALSE;
				}
			}
				if(!completed && read_audio) {
					bytes_read = tts_websocket_stream_read_audio(
						synth_channel, frame->codec_frame.buffer, frame_size, &eof);
				}
				apr_thread_mutex_unlock(synth_channel->stream_buffer_mutex);

			if(!completed && read_audio) {
			if(bytes_read > 0) {
				if(bytes_read < frame_size) {
					memset((char*)frame->codec_frame.buffer + bytes_read, 0xFF,
						frame_size - bytes_read);
				}
				frame->type |= MEDIA_FRAME_TYPE_AUDIO;
				if(eof) {
					tts_websocket_output_finish_begin(
						synth_channel, stream, terminal_error);
				}
			} else if(eof) {
				/* This callback itself remains NONE and may flush the tail packet.
				 * COMPLETE is deliberately deferred to a later state/tick. */
				tts_websocket_output_finish_begin(
					synth_channel, stream, terminal_error);
				} else {
					/* Preserve any partial producer chunk and rebuild the reserve.
					 * A temporary underrun is not audio. Returning NONE lets the
					 * RTP sink keep its clock without inserting synthetic speech-time
					 * silence ahead of the next real chunk. */
					synth_channel->stream_prebuffered = 0;
					synth_channel->stream_buffer_empty_count++;
				}
			}
		}
		} else if(!completed) {
		if(stream->rx_descriptor &&
		   synth_channel->time_to_complete >= stream->rx_descriptor->frame_duration) {
			memset(frame->codec_frame.buffer, 0xFF, frame_size);
			frame->type |= MEDIA_FRAME_TYPE_AUDIO;
			synth_channel->time_to_complete -= stream->rx_descriptor->frame_duration;
		} else {
			tts_websocket_output_finish_begin(synth_channel, stream, FALSE);
		}
	}

	/* ========== 丢音诊断：在发帧点录制客户端实际收到的音频流 ==========
	 * 之前 8k 录音在 WS 线程写环形缓冲区之前落盘，只能证明数据到达插件，
	 * 无法覆盖 ring→RTP 之间的丢失。现在录制每一帧实际交给 MPF 的 AUDIO
	 * 帧（真实音频 + 静音填充帧 + post-roll），录音与客户端应收到的 RTP
	 * 载荷流逐帧一致：
	 *   - 录音里能听到尾音但客户端听不到 → 丢失在 RTP 发送/网络/客户端侧；
	 *   - 录音里尾音同样是静音 → 丢失在插件内部（结合 [DIAG] 计数器定位）。
	 * 注意：recording_enabled 默认关闭，仅在压测/排查时开启；
	 * fwrite 为缓冲写，正常场景开销可忽略，但生产环境不建议长期开启。 */
	if((frame->type & MEDIA_FRAME_TYPE_AUDIO) && synth_channel->record_file) {
		tts_websocket_recording_write_final(synth_channel, frame->codec_frame.buffer, frame_size);
	}

	if(completed) {
		/* ========== 丢音诊断汇总：定位丢失环节 ==========
		 *   ring_written == ring_read 且 dropped == 0
		 *     → 音频已全部交给 MPF，丢失在 RTP 发送/网络/客户端侧
		 *       （post-roll 不足或 SPEAK-COMPLETE 超车尾音包）；
		 *   ring_written > ring_read
		 *     → reader 未排干环形缓冲区，检查 EOF/完成时序；
		 *   dropped > 0
		 *     → stop 路径丢弃尾部数据（查 [CLEANUP]/STOP/barge-in 日志）；
		 *   silence_frames / trylock_fail / partial_wait 高
		 *     → 高并发下静音帧大量插队，尾音被推迟（客户端侧表现为丢尾）。 */
		LOG_WITH_SID(synth_channel, APT_PRIO_INFO,
			"[DIAG] Playback finishing: ring_written=%"APR_SIZE_T_FMT"B, ring_read=%"APR_SIZE_T_FMT"B, "
			"dropped_stop=%"APR_SIZE_T_FMT"B, silence_frames=%u, trylock_fail=%u, partial_wait=%u, "
			"buffer_empty=%u, cause=%d",
			synth_channel->stream_ring_bytes_written,
			synth_channel->stream_ring_bytes_read,
			synth_channel->stream_ring_bytes_dropped,
			(unsigned)synth_channel->stream_silence_frame_count,
			(unsigned)synth_channel->stream_trylock_fail_count,
			(unsigned)synth_channel->stream_partial_wait_count,
			(unsigned)synth_channel->stream_buffer_empty_count,
			terminal_error ? 1 : 0);
		mrcp_message_t *message = mrcp_event_create(
			active_request,
			SYNTHESIZER_SPEAK_COMPLETE,
			active_request->pool);
		if(message) {
			mrcp_synth_header_t *synth_header = mrcp_resource_header_prepare(message);
			if(synth_header) {
				synth_header->completion_cause = terminal_error
					? SYNTHESIZER_COMPLETION_CAUSE_ERROR
					: SYNTHESIZER_COMPLETION_CAUSE_NORMAL;
				mrcp_resource_header_property_add(
					message, SYNTHESIZER_HEADER_COMPLETION_CAUSE);
			}
			message->start_line.request_state = MRCP_REQUEST_STATE_COMPLETE;
			synth_channel->speak_request = NULL;
			synth_channel->stream_output_state = TTS_WEBSOCKET_OUTPUT_IDLE;
			if(synth_channel->audio_file) {
				fclose(synth_channel->audio_file);
				synth_channel->audio_file = NULL;
			}
			mrcp_engine_channel_message_send(synth_channel->channel, message);
		}
		/* 播放完成（含 post-roll 最后一帧），此处关闭诊断录音文件。
		 * 录音文件的唯一 writer 是本 MPF 回调线程。 */
		tts_websocket_recording_close(synth_channel);
	}
stream_read_done:
	if(lifecycle_entered) {
		tts_websocket_stream_lifecycle_leave(&synth_channel->stream_lifecycle);
	}
	return TRUE;
}

#if 0
/* ---------- stream read ---------- */
/**
 * @brief 读取音频帧（MPF 回调）
 * @param stream MPF 音频流对象
 * @param frame 待填充的音频帧结构
 * @return 成功返回 TRUE
 * 说明：
 *   1. 处理 STOP/PAUSE 状态
 *   2. 从本地文件或内存缓冲读取音频数据到 frame
 *   3. 在播放完成时发送 SPEAK-COMPLETE 事件
 */
static apt_bool_t tts_websocket_stream_read(mpf_audio_stream_t *stream, mpf_frame_t *frame)
{
	tts_websocket_channel_t *synth_channel = stream->obj;
	/* check if STOP was requested */
		/* ========== 修复：检查stream->obj的有效性 ========== */
		if(!synth_channel) {
			apt_log(SYNTH_LOG_MARK, APT_PRIO_WARNING, "zyTTS: [STREAM] stream->obj is NULL");
			return TRUE;
		}
		/* ========== 修复：检查frame和codec_frame.buffer的有效性 ========== */
		if(!frame || !frame->codec_frame.buffer) {
			apt_log(SYNTH_LOG_MARK, APT_PRIO_WARNING, "zyTTS: [STREAM] Frame or codec_frame.buffer is NULL");
			return TRUE;
		}
	if(synth_channel->stop_response) {
		/* send asynchronous response to STOP request */
		mrcp_engine_channel_message_send(synth_channel->channel,synth_channel->stop_response);
		synth_channel->stop_response = NULL;
		synth_channel->speak_request = NULL;
		synth_channel->paused = FALSE;
		if(synth_channel->audio_file) {
			fclose(synth_channel->audio_file);
			synth_channel->audio_file = NULL;
		}
			/* ========== 修复：通知 WebSocket 线程停止，避免线程泄漏 ==========
			 * 之前 STOP 请求不设置 stream_stop_requested 也不唤醒条件变量，
			 * 导致 WS 线程在缓冲区满时可能阻塞长达 30 秒才超时退出。
			 * 高并发下大量僵尸线程占用资源，加剧调度延迟。 */
			synth_channel->stream_stop_requested = 1;
			if(synth_channel->stream_buffer_cond) {
				apr_thread_cond_broadcast(synth_channel->stream_buffer_cond);
			}
		return TRUE;
	}

	/* check if there is active SPEAK request and it isn't in paused state */
	if(synth_channel->speak_request && synth_channel->paused == FALSE) {
		/* normal processing */
		apt_bool_t completed = FALSE;
		if(synth_channel->audio_file) {
			/* read speech from file */
			apr_size_t size = frame->codec_frame.size;
			if(fread(frame->codec_frame.buffer,1,size,synth_channel->audio_file) == size) {
				frame->type |= MEDIA_FRAME_TYPE_AUDIO;
			}
			else {
				completed = TRUE;
						synth_channel->stream_buffer_empty_count++;
			}
		}
		/* ========== 流式TTS缓冲区读取（优先级最高）========== */
		else if(synth_channel->stream_buffer) {
			/* ========== 修复：检查是否发生错误 ========== */
			if(synth_channel->stream_error) {
				apt_log(SYNTH_LOG_MARK, APT_PRIO_WARNING, "zyTTS: [STREAM] Stream error detected, terminating playback " APT_SIDRES_FMT,
					APT_SIDRES_FMT, synth_channel->speak_request ? MRCP_MESSAGE_SIDRES(synth_channel->speak_request) : "(null)");
				completed = TRUE;
						synth_channel->stream_buffer_empty_count++;
			}
			else {
				/* ========== 修复：检查是否已开始接收音频 ========== */
				if(!synth_channel->stream_audio_started) {
					/* ========== 修复：流已完成但从未收到 audio.start ==========
					 * TTS 服务器可能直接关闭连接/返回错误而不发 audio.start。
					 * 此时 stream_complete 已置位但 stream_audio_started 仍为 0，
					 * 若不处理会导致 reader 永远 return TRUE 不发送 SPEAK-COMPLETE，
					 * MRCP 客户端将永久挂起等待完成信号。 */
					if(synth_channel->stream_complete) {
						completed = TRUE;
						/* ========== 修复：直接跳到completed处理，不再继续fall through ==========
						 * 否则后续 read 又会发现 EOF 再次置 completed=TRUE，
						 * 且可能产生重复的静音帧写入 frame buffer。 */
					} else {
						/* 等待 audio.start 期间不发送任何帧，让MPF自然跳过此间隔。
					 * 不注入静音帧：静音会覆盖后续到达的真实音频 → 丢音。
					 * RTP时间戳的短暂间隙由客户端jitter buffer自然处理。 */
					return TRUE;					}
				}

				/* 从环形缓冲区读取流式音频数据 */
				apr_size_t size = frame->codec_frame.size;

				/* ========== 预缓冲检查：避免缓冲区刚有少量数据就开始发送，导致频繁underrun ==========
				 * 高并发下TTS服务器响应可能变慢，首批音频帧到达后如果立即开始消费，
				 * 缓冲区水位可能迅速下降至零，导致后续静音帧填充 → 客户端感知为音频丢失。
				 * 预缓冲策略：首个音频帧到达后，等待缓冲区积累到最低水位（buffer_size的25%），
				 * 然后再开始发送真实音频。预缓冲期间发送静音帧（录音会记录以保持一致）。 */
				if(!synth_channel->stream_prebuffered && synth_channel->stream_audio_started) {
					/* ========== 修复：预缓冲期间检查流是否已结束或出错 ==========
					 * 如果 TTS 服务器在预缓冲期间完成(session.done)或出错，
					 * 直接跳过预缓冲让正常的 read+EOF 流程接管，
					 * 否则 reader 永远发送静音帧无法退出。 */
					if(synth_channel->stream_complete || synth_channel->stream_error) {
						synth_channel->stream_prebuffered = 1;
						LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[PREBUF] Stream completed/error during pre-buffering, skipping");
					} else {
						apr_thread_mutex_lock(synth_channel->stream_buffer_mutex);
						apr_size_t pre_available = (synth_channel->stream_write_pos + synth_channel->stream_buffer_size
							- synth_channel->stream_read_pos) % synth_channel->stream_buffer_size;
						apr_thread_mutex_unlock(synth_channel->stream_buffer_mutex);

						if(pre_available < synth_channel->stream_prebuffer_min_fill) {
							/* 缓冲区未达到最低水位，不发送音频帧，让MPF自然等待下一次回调。
							 * 不注入静音帧：静音会永久覆盖后续到达的真实音频 → 丢音/断断续续。 */
							/* memset + frame->type 已移除 — 返回TRUE但不设AUDIO标志，MPF跳过此间隔 */
							LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[PREBUF] Pre-buffering: %"APR_SIZE_T_FMT"/%"APR_SIZE_T_FMT" bytes (%.1f%%), waiting for %"APR_SIZE_T_FMT,
								pre_available, synth_channel->stream_buffer_size,
								(pre_available*100.0)/synth_channel->stream_buffer_size,
								synth_channel->stream_prebuffer_min_fill);
							return TRUE;
						}
						/* 缓冲区达到最低水位，标记预缓冲完成，开始发送真实音频 */
						synth_channel->stream_prebuffered = 1;
						LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[PREBUF] Pre-buffering complete: %"APR_SIZE_T_FMT"/%"APR_SIZE_T_FMT" bytes (%.1f%%)",
							pre_available, synth_channel->stream_buffer_size,
							(pre_available*100.0)/synth_channel->stream_buffer_size);
					}
				}
				/* ========== 预缓冲检查结束 ========== */

				/* ========== 修复：不再做无锁预检，统一在 tts_websocket_stream_read_audio 内部 mutex 保护下完成 ==========
				 * 之前的无锁 ring_buffer_read_available() 预检与实际读取之间存在竞态窗口：
				 * stream_complete 可能在预检后、读取前被置1，导致缓冲区中剩余数据被丢弃。 */
				apt_bool_t eof = FALSE;
				apr_size_t bytes_read = tts_websocket_stream_read_audio(synth_channel, frame->codec_frame.buffer, size, &eof);

				/* ========== 修复：最后一帧音频与 SPEAK-COMPLETE 分离，消除竞态 ==========
				 * 高并发场景下的核心问题：
				 *   之前 bytes_read>0 && eof==TRUE 时，在同一回调中既返回音频帧(RTP)
				 *   又发送 SPEAK-COMPLETE(MRCP控制通道TCP)。控制通道消息可能比RTP包
				 *   更快到达客户端，导致客户端提前关闭媒体流 → 最后一帧音频丢失。
				 *
				 * 修复策略：
				 *   读到末尾数据(eof==TRUE)时，只返回音频帧，不设置 completed。
				 *   下一轮MPF回调时缓冲区为空+eof==TRUE，再发送SPEAK-COMPLETE。
				 *   这保证最后一帧RTP音频先于SPEAK-COMPLETE到达客户端。 */
				if(bytes_read > 0) {
					frame->type |= MEDIA_FRAME_TYPE_AUDIO;
					if(bytes_read < size) {
						/* 尾帧不足一帧时：设置实际数据长度，避免用 0xFF 填充导致波形跳变杂音。
						 * MPF 框架会根据 frame->codec_frame.size 发送实际数据量，
						 * 不会发送填充字节，从而消除尾帧的波形不连续咔嗒声。 */
						frame->codec_frame.size = bytes_read;
					}
					/* 保存录音到文件 — 与 MRCP 客户端收到的数据完全一致 */
					tts_websocket_recording_write_final(synth_channel, frame->codec_frame.buffer, frame->codec_frame.size);
					/* ========== 修复：即使 eof==TRUE，也不在此回调中发送 SPEAK-COMPLETE ==========
					 * 当前回调已返回了一帧音频(RTP)，必须等MPF将这帧RTP发出后，
					 * 下一轮回调中(缓冲区为空+eof==TRUE)再发送SPEAK-COMPLETE。
					 * 否则控制通道的 SPEAK-COMPLETE 可能比 RTP 音频帧更早到达客户端，
					 * 导致高并发下客户端提前关闭流 → 最后一段语音丢失。 */
					if(eof) {
						apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"zyTTS: [STREAM] Last audio frame sent, deferring SPEAK-COMPLETE to next cycle " APT_SIDRES_FMT,
							APT_SIDRES_FMT, synth_channel->speak_request ? MRCP_MESSAGE_SIDRES(synth_channel->speak_request) : "(null)");
						/* 不设置 completed=TRUE，下一轮回调中缓冲区为空+eof==TRUE 时再发送 */
					}
				} else if(eof) {
					/* 流已完成且缓冲区为空 -> 真正的 EOF，此时安全发送 SPEAK-COMPLETE */
					completed = TRUE;
					synth_channel->stream_buffer_empty_count++;
					apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"zyTTS: [STREAM] Audio playback completed " APT_SIDRES_FMT,
						APT_SIDRES_FMT, synth_channel->speak_request ? MRCP_MESSAGE_SIDRES(synth_channel->speak_request) : "(null)");
				} else {
					/* 缓冲区临时为空且流未结束：不发送任何帧，让MPF自然跳过此间隔。
					 * 不注入静音帧：静音会永久覆盖后续到达的真实音频数据，
					 * 导致用户感知为丢音/断断续续。短暂RTP间隙由客户端jitter buffer处理。 */
					synth_channel->stream_buffer_empty_count++;
					{
						apr_thread_mutex_lock(synth_channel->stream_buffer_mutex);
						apr_size_t _used = (synth_channel->stream_write_pos + synth_channel->stream_buffer_size - synth_channel->stream_read_pos) % synth_channel->stream_buffer_size;
						apr_thread_mutex_unlock(synth_channel->stream_buffer_mutex);
						LOG_WITH_SID(synth_channel, APT_PRIO_INFO, "[BUF_UNDERRUN] Buffer empty, skipping frame: used=%"APR_SIZE_T_FMT"/%"APR_SIZE_T_FMT" bytes (%.1f%%), total empty count=%u",
							_used, synth_channel->stream_buffer_size, (_used*100.0)/synth_channel->stream_buffer_size, (unsigned int)synth_channel->stream_buffer_empty_count);
					}
				}
			}
		}
		else {
				/* ========== 修复：检查rx_descriptor的有效性 ========== */
				if(!stream->rx_descriptor) {
					apt_log(SYNTH_LOG_MARK, APT_PRIO_WARNING, "zyTTS: [STREAM] stream->rx_descriptor is NULL");
					completed = TRUE;
					synth_channel->stream_buffer_empty_count++;
				} else {
			/* fill with silence in case no file available */
			if(synth_channel->time_to_complete >= stream->rx_descriptor->frame_duration) {
				memset(frame->codec_frame.buffer, 0xFF, frame->codec_frame.size);
				frame->type |= MEDIA_FRAME_TYPE_AUDIO;
				synth_channel->time_to_complete -= stream->rx_descriptor->frame_duration;
			}
			else {
				completed = TRUE;
			}
		}
		
				}
		if(completed) {
				/* ========== 修复：检查speak_request是否仍然有效 ========== */
				if(!synth_channel->speak_request) {
					apt_log(SYNTH_LOG_MARK, APT_PRIO_WARNING, "zyTTS: [STREAM] speak_request is NULL, cannot send SPEAK-COMPLETE event");
					return TRUE;
				}
			/* raise SPEAK-COMPLETE event */
			mrcp_message_t *message = mrcp_event_create(
								synth_channel->speak_request,
								SYNTHESIZER_SPEAK_COMPLETE,
								synth_channel->speak_request->pool);
			if(message) {
				/* get/allocate synthesizer header */
				mrcp_synth_header_t *synth_header = mrcp_resource_header_prepare(message);
				if(synth_header) {
					/* set completion cause */
					synth_header->completion_cause = SYNTHESIZER_COMPLETION_CAUSE_NORMAL;
					mrcp_resource_header_property_add(message,SYNTHESIZER_HEADER_COMPLETION_CAUSE);
				}
				/* set request state */
				message->start_line.request_state = MRCP_REQUEST_STATE_COMPLETE;

				synth_channel->speak_request = NULL;
				if(synth_channel->audio_file) {
					fclose(synth_channel->audio_file);
					synth_channel->audio_file = NULL;
				}
				/* send asynch event */
				mrcp_engine_channel_message_send(synth_channel->channel,message);
			}
		}
	}
	return TRUE;
}
#endif

/* ---------- message signal ---------- */
/**
 * @brief 向后台任务发送消息
 * @param type 消息类型
 * @param channel 关联的 MRCP 引擎通道
 * @param request 可选的 MRCP 请求消息
 * @return 成功返回 TRUE
 * 说明：
 *   将类型、通道和请求封装为任务消息并信号化到消费者任务队列
 */
static apt_bool_t tts_websocket_msg_signal(tts_websocket_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
	apt_bool_t status = FALSE;
	tts_websocket_channel_t *tts_channel = channel->method_obj;
	tts_websocket_engine_t *tts_engine = tts_channel->tts_engine;
	apt_task_t *task = apt_consumer_task_base_get(tts_engine->task);
	apt_task_msg_t *msg = apt_task_msg_get(task);
	if(msg) {
		tts_websocket_msg_t *tts_msg;
		msg->type = TASK_MSG_USER;
		tts_msg = (tts_websocket_msg_t*) msg->data;

		tts_msg->type = type;
		tts_msg->channel = channel;
		tts_msg->request = request;
		status = apt_task_msg_signal(task,msg);
	}
	return status;
}

/* ---------- message process ---------- */
/**
 * @brief 任务消息处理函数
 * @param task 处理消息的任务
 * @param msg 接收到的任务消息
 * @return 成功返回 TRUE
 * 说明：
 *   在任务上下文中处理 OPEN/CLOSE/REQUEST_PROCESS 等消息，并发送相应的异步响应或分发请求
 */
static apt_bool_t tts_websocket_msg_process(apt_task_t *task, apt_task_msg_t *msg)
{
	tts_websocket_msg_t *tts_msg = (tts_websocket_msg_t*)msg->data;
	switch(tts_msg->type) {
		case TTS_WEBSOCKET_MSG_OPEN_CHANNEL:
			{
				/* 打印通道打开日志和TTS服务器配置 */
				tts_websocket_channel_t *synth_channel = tts_msg->channel->method_obj;
				apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "");
				apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "========================================");
				apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "ZyTTS: Opening Synth Channel");
				apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "ZyTTS: TTS Server: %s:%d",
					synth_channel->tts_engine->tts_server_host,
					synth_channel->tts_engine->tts_server_port);
				apt_log(SYNTH_LOG_MARK, APT_PRIO_NOTICE, "========================================");
				/* open channel and send asynch response */
				mrcp_engine_channel_open_respond(tts_msg->channel,TRUE);
			}
			break;
		case TTS_WEBSOCKET_MSG_CLOSE_CHANNEL:
			{
				/* cleanup audio resources before closing channel */
				tts_websocket_channel_t *synth_channel = tts_msg->channel->method_obj;
				tts_websocket_channel_cleanup_audio(synth_channel);
				/* close channel and send asynch response */
				mrcp_engine_channel_close_respond(tts_msg->channel);
			}
			break;
		case TTS_WEBSOCKET_MSG_REQUEST_PROCESS:
			tts_websocket_channel_request_dispatch(tts_msg->channel,tts_msg->request);
			break;
		default:
			break;
	}
	return TRUE;
}

/* ---------- url encode ---------- */
/**
 * @brief URL 编码字符串
 * @param input 待编码的字符串
 * @param input_len 输入长度
 * @param pool APR 内存池
 * @return 返回编码后的字符串（在 pool 中分配），失败返回 NULL
 * 说明：
 *   将特殊字符转换为 %XX 格式，用于 URL 参数编码
 */
static char* url_encode(const char *input, apr_size_t input_len, apr_pool_t *pool)
{
	static const char hex[] = "0123456789ABCDEF";
	apr_size_t i;
	apr_size_t encoded_len = 0;
	char *output, *p;

	/* 计算编码后需要的长度 */
	for(i = 0; i < input_len; i++) {
		unsigned char c = (unsigned char)input[i];
		if(isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			encoded_len++;
		} else {
			encoded_len += 3;
		}
	}

	output = apr_palloc(pool, encoded_len + 1);
	if(!output) return NULL;

	p = output;
	for(i = 0; i < input_len; i++) {
		unsigned char c = (unsigned char)input[i];
		if(isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			*p++ = c;
		} else {
			*p++ = '%';
			*p++ = hex[c >> 4];
			*p++ = hex[c & 15];
		}
	}
	*p = '\0';
	return output;
}

/* ---------- json escape ---------- */
/**
 * @brief JSON 字符串转义
 * @param input 待转义的字符串
 * @param input_len 输入长度
 * @param pool APR 内存池
 * @return 返回转义后的字符串（在 pool 中分配），失败返回 NULL
 * 说明：
 *   将特殊字符（引号、反斜杠、控制字符等）转义为 JSON 兼容格式
 *   保留 UTF-8 多字节字符（如中文）不被转义
 */
static char* json_escape(const char *input, apr_size_t input_len, apr_pool_t *pool)
{
	apr_size_t i;
	apr_size_t escaped_len = 0;
	char *output, *p;

	/* 计算转义后需要的长度 */
	for(i = 0; i < input_len; i++) {
		unsigned char c = (unsigned char)input[i];
		if(c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') {
			escaped_len += 2;
		} else if(c < 0x20) {
			escaped_len += 6;  /* \uXXXX */
		} else if(c >= 0x80) {
			/* UTF-8 多字节字符，保留原始字节（不转义） */
			escaped_len++;
		} else {
			escaped_len++;
		}
	}

	output = apr_palloc(pool, escaped_len + 1);
	if(!output) return NULL;

	p = output;
	for(i = 0; i < input_len; i++) {
		unsigned char c = (unsigned char)input[i];
		switch(c) {
			case '"':  *p++ = '\\'; *p++ = '"'; break;
			case '\\': *p++ = '\\'; *p++ = '\\'; break;
			case '\b': *p++ = '\\'; *p++ = 'b'; break;
			case '\f': *p++ = '\\'; *p++ = 'f'; break;
			case '\n': *p++ = '\\'; *p++ = 'n'; break;
			case '\r': *p++ = '\\'; *p++ = 'r'; break;
			case '\t': *p++ = '\\'; *p++ = 't'; break;
			default:
				if(c < 0x20) {
					p += sprintf(p, "\\u%04x", c);
				} else {
					/* 对于 0x20-0x7F 的 ASCII 和 >= 0x80 的 UTF-8 字节，直接保留 */
					*p++ = c;
				}
				break;
		}
	}
	*p = '\0';
	return output;
}

/* ---------- json get type ---------- */
/**
 * @brief 从 JSON 消息中提取 "type" 字段的值（精确匹配，替代 strstr 子串搜索）
 * @param json JSON 字符串
 * @param len JSON 字符串长度
 * @param type_buf 输出缓冲区，存放提取到的 type 值
 * @param type_buf_size 输出缓冲区大小
 * @return 成功返回 type_buf 指针，失败返回 NULL
 *
 * 修复说明：
 * 之前的实现使用 strstr(buffer, "audio.start") 做子串匹配来判断消息类型，
 * 存在严重缺陷：若 audio.done 消息中包含 "audio_start_time" 等字段名，
 * strstr 会误匹配 "audio.start" 子串，导致 receiving_audio 状态错乱。
 * 本函数精确提取 JSON "type" 字段的值，使用 strcmp 做精确比较。
 */
static const char* json_get_type(const char *json, apr_size_t len, char *type_buf, apr_size_t type_buf_size)
{
	const char *p, *colon, *start, *end;
	apr_size_t val_len;

	if(!json || len == 0 || !type_buf || type_buf_size == 0) {
		return NULL;
	}

	/* 查找 "type" 键 */
	p = strstr(json, "\"type\"");
	if(!p) {
		return NULL;
	}

	/* 查找冒号分隔符 */
	colon = strchr(p + 6, ':');
	if(!colon) {
		return NULL;
	}
	colon++;

	/* 跳过空白字符 */
	while(*colon == ' ' || *colon == '\t') {
		colon++;
	}

	/* 查找值的起始引号 */
	if(*colon != '"') {
		return NULL;
	}
	start = colon + 1;

	/* 查找值的结束引号 */
	end = strchr(start, '"');
	if(!end) {
		return NULL;
	}

	/* 提取类型值 */
	val_len = end - start;
	if(val_len >= type_buf_size) {
		val_len = type_buf_size - 1;
	}
	memcpy(type_buf, start, val_len);
	type_buf[val_len] = '\0';

	return type_buf;
}

/* ---------- gbk to utf8 convert ---------- */
/**
 * @brief 将 GBK 编码的字符串转换为 UTF-8
 * @param gbk_str GBK 编码的输入字符串
 * @param gbk_len 输入字符串长度
 * @param utf8_len 输出参数，返回转换后的 UTF-8 字符串长度
 * @param pool APR 内存池
 * @return 返回 UTF-8 字符串（在 pool 中分配），失败返回 NULL
 */
static char* gbk_to_utf8(const char *gbk_str, apr_size_t gbk_len, apr_size_t *utf8_len, apr_pool_t *pool)
{
	iconv_t cd;
	char *utf8_str;
	char *in_ptr, *out_ptr;
	size_t in_bytes, out_bytes, result;

	if(!gbk_str || gbk_len == 0 || !pool) {
		if(utf8_len) *utf8_len = 0;
		return NULL;
	}

	/* 初始化 iconv，从 GBK 转换到 UTF-8 */
	cd = iconv_open("UTF-8", "GBK");
	if(cd == (iconv_t)-1) {
		apt_log(SYNTH_LOG_MARK,APT_PRIO_WARNING,"zyTTS: iconv_open failed: %s", strerror(errno));
		return NULL;
	}

	/* 分配输出缓冲区（GBK转UTF-8最多扩展到3倍） */
	size_t out_buf_size = gbk_len * 3 + 1;
	utf8_str = apr_palloc(pool, out_buf_size);
	if(!utf8_str) {
		iconv_close(cd);
		return NULL;
	}

	in_ptr = (char *)gbk_str;
	in_bytes = gbk_len;
	out_ptr = utf8_str;
	out_bytes = out_buf_size - 1;

	/* 执行编码转换 */
	result = iconv(cd, &in_ptr, &in_bytes, &out_ptr, &out_bytes);
	iconv_close(cd);

	if(result == (size_t)-1) {
		apt_log(SYNTH_LOG_MARK,APT_PRIO_WARNING,"zyTTS: iconv failed: %s", strerror(errno));
		return NULL;
	}

	/* 计算转换后的长度 */
	*out_ptr = '\0';
	if(utf8_len) {
		*utf8_len = out_buf_size - 1 - out_bytes;
	}

	return utf8_str;
}

/* ---------- hex dump ---------- */
/**
 * @brief 打印十六进制转储用于调试
 * @param data 数据指针
 * @param len 数据长度
 * 说明：
 *   以十六进制和ASCII格式打印数据，用于调试字符编码问题
 */
static void hex_dump(const char *label, const char *data, apr_size_t len)
{
	apr_size_t i;
	char hex_buf[128];
	char ascii_buf[64];
	int hex_pos = 0;
	int ascii_pos = 0;

	apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"zyTTS: Hex dump [%s] (%"APR_SIZE_T_FMT" bytes):", label, len);

	for(i = 0; i < len; i++) {
		/* 十六进制部分 */
		hex_pos += snprintf(hex_buf + hex_pos, sizeof(hex_buf) - hex_pos, "%02X ", (unsigned char)data[i]);

		/* ASCII部分 - 可打印字符直接显示，否则显示为点 */
		if(data[i] >= 32 && data[i] <= 126) {
			ascii_pos += snprintf(ascii_buf + ascii_pos, sizeof(ascii_buf) - ascii_pos, "%c", data[i]);
		} else {
			ascii_pos += snprintf(ascii_buf + ascii_pos, sizeof(ascii_buf) - ascii_pos, ".");
		}

		/* 每16字节打印一行 */
		if((i + 1) % 16 == 0 || i == len - 1) {
			/* 补齐对齐 */
			while((i + 1) % 16 != 0 && hex_pos < sizeof(hex_buf)) {
				hex_pos += snprintf(hex_buf + hex_pos, sizeof(hex_buf) - hex_pos, "   ");
			}
			apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"zyTTS:   %s  |  %s", hex_buf, ascii_buf);
			hex_pos = 0;
			ascii_pos = 0;
		}
	}
}

/* ========== WebSocket 协议实现 ========== */

/**
 * @brief Base64 编码
 * @param input 输入数据
 * @param len 输入数据长度
 * @param pool 内存池
 * @return Base64 编码后的字符串
 */
static char* base64_encode(const unsigned char *input, apr_size_t len, apr_pool_t *pool)
{
	static const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	apr_size_t i, j;
	apr_size_t out_len = ((len + 2) / 3) * 4;
	char *output = apr_palloc(pool, out_len + 1);
	unsigned int v;

	if(!output) return NULL;

	for(i = 0, j = 0; i < len; i += 3) {
		v = 0;
		v = input[i] << 16;
		if(i + 1 < len) v |= input[i + 1] << 8;
		if(i + 2 < len) v |= input[i + 2];

		output[j++] = base64_table[(v >> 18) & 0x3F];
		output[j++] = base64_table[(v >> 12) & 0x3F];
		if(i + 1 < len) output[j++] = base64_table[(v >> 6) & 0x3F];
		else output[j++] = '=';
		if(i + 2 < len) output[j++] = base64_table[v & 0x3F];
		else output[j++] = '=';
	}

	output[j] = '\0';
	return output;
}

/**
 * @brief 生成 WebSocket 握手所需的 Sec-WebSocket-Key
 * @param pool 内存池
 * @return 随机生成的 Base64 编码 key
 */
static char* generate_websocket_key(apr_pool_t *pool)
{
	unsigned char key_raw[16];
	apr_status_t rv;
	FILE *fp;
	size_t bytes_read;
	apr_size_t i;

	/* 首选：使用 APR 的跨平台随机数生成器 */
	rv = apr_generate_random_bytes(key_raw, sizeof(key_raw));
	if (rv == APR_SUCCESS) {
		apt_log(SYNTH_LOG_MARK, APT_PRIO_DEBUG, "zyTTS: [WS] Generated WebSocket key using apr_generate_random_bytes");
		return base64_encode(key_raw, 16, pool);
	}

	/* 次选：使用 /dev/urandom (Linux/macOS) */
	fp = fopen("/dev/urandom", "rb");
	if (fp != NULL) {
		bytes_read = fread(key_raw, 1, sizeof(key_raw), fp);
		fclose(fp);
		if (bytes_read == sizeof(key_raw)) {
			apt_log(SYNTH_LOG_MARK, APT_PRIO_DEBUG, "zyTTS: [WS] Generated WebSocket key using /dev/urandom");
			return base64_encode(key_raw, 16, pool);
		}
	}

	/* ========== 修复：移除非线程安全的 srand/rand fallback ==========
	 * 高并发下 srand()/rand() 的数据竞争会导致未定义行为，
	 * 可能生成损坏的 WebSocket key 导致握手失败。
	 * apr_generate_random_bytes 和 /dev/urandom 都是线程安全的。 */
	apt_log(SYNTH_LOG_MARK, APT_PRIO_WARNING, "zyTTS: [WS] Failed to generate WebSocket key (both APR random and /dev/urandom unavailable)");
	return NULL;
}

/* Minimal SHA1 implementation (copied/adapted from the recognizer plugin) */
typedef struct {
	uint32_t state[5];
	uint64_t count;
	unsigned char buffer[64];
} sha1_ctx_t;

static void sha1_transform(uint32_t state[5], const unsigned char buffer[64])
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

static void sha1_init(sha1_ctx_t *ctx)
{
	ctx->state[0] = 0x67452301;
	ctx->state[1] = 0xEFCDAB89;
	ctx->state[2] = 0x98BADCFE;
	ctx->state[3] = 0x10325476;
	ctx->state[4] = 0xC3D2E1F0;
	ctx->count = 0;
}

static void sha1_update(sha1_ctx_t *ctx, const unsigned char *data, size_t len)
{
	size_t i = 0;
	size_t j = (size_t)(ctx->count & 63);
	ctx->count += (uint64_t)len;

	if ((j + len) > 63) {
		size_t part_len = 64 - j;
		memcpy(ctx->buffer + j, data, part_len);
		sha1_transform(ctx->state, ctx->buffer);
		for (i = part_len; i + 63 < len; i += 64) {
			sha1_transform(ctx->state, data + i);
		}
		j = 0;
	}

	if (i < len) {
		memcpy(ctx->buffer + j, data + i, len - i);
	}
}

static void sha1_final(sha1_ctx_t *ctx, unsigned char digest[20])
{
	unsigned char finalcount[8];
	unsigned char c = 0x80;
	int i;

	for (i = 0; i < 8; i++) {
		finalcount[i] = (unsigned char)((ctx->count * 8) >> ((7 - i) * 8));
	}

	sha1_update(ctx, &c, 1);
	while ((ctx->count & 63) != 56) {
		c = 0x00;
		sha1_update(ctx, &c, 1);
	}
	sha1_update(ctx, finalcount, 8);

	for (i = 0; i < 20; i++) {
		digest[i] = (unsigned char)(ctx->state[i >> 2] >> ((3 - (i & 3)) * 8));
	}
}

static char* websocket_compute_accept(apr_pool_t *pool, const char *key)
{
	static const char ws_guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	sha1_ctx_t ctx;
	unsigned char digest[20];
	char *tmp = apr_pstrcat(pool, key, ws_guid, NULL);

	sha1_init(&ctx);
	sha1_update(&ctx, (const unsigned char*)tmp, strlen(tmp));
	sha1_final(&ctx, digest);

	return base64_encode(digest, sizeof(digest), pool);
}

/* Case-insensitive substring search */
static char* ws_strcasestr(const char *haystack, const char *needle)
{
	if(!haystack || !needle || !*needle) return NULL;
	size_t nlen = strlen(needle);
	const char *p;
	for(p = haystack; *p; p++) {
		if(strncasecmp(p, needle, nlen) == 0) return (char*)p;
	}
	return NULL;
}

static apt_bool_t websocket_send_all(
	apr_socket_t *sock, const unsigned char *data, apr_size_t size)
{
	apr_size_t offset = 0;

	while(offset < size) {
		apr_size_t sent = size - offset;
		apr_status_t rv = apr_socket_send(sock, (const char *)data + offset, &sent);
		if(sent > 0) {
			offset += sent;
		}
		if(rv != APR_SUCCESS) {
			if(APR_STATUS_IS_EINTR(rv)) {
				continue;
			}
			return FALSE;
		}
		if(sent == 0) {
			return FALSE;
		}
	}
	return TRUE;
}

static apt_bool_t websocket_send_masked_frame(
	apr_socket_t *sock, unsigned char opcode, const unsigned char *payload,
	apr_size_t payload_len)
{
	unsigned char header[14];
	unsigned char mask[4];
	unsigned char stack_masked[125];
	unsigned char *masked = stack_masked;
	apr_size_t header_len = 2;
	apr_size_t i;
	apt_bool_t ok;

	if(!sock || (!payload && payload_len > 0) || payload_len > 125) {
		return FALSE;
	}
	if(apr_generate_random_bytes(mask, sizeof(mask)) != APR_SUCCESS) {
		return FALSE;
	}
	header[0] = (unsigned char)(0x80 | (opcode & 0x0F));
	if(payload_len < 126) {
		header[1] = (unsigned char)(0x80 | payload_len);
	} else {
		return FALSE;
	}
	memcpy(header + header_len, mask, sizeof(mask));
	header_len += sizeof(mask);
	for(i = 0; i < payload_len; ++i) {
		masked[i] = payload[i] ^ mask[i % 4];
	}
	ok = websocket_send_all(sock, header, header_len);
	if(ok && payload_len > 0) {
		ok = websocket_send_all(sock, masked, payload_len);
	}
	return ok;
}

static apr_status_t websocket_socket_read(void *context, char *buffer, apr_size_t *size)
{
	websocket_connection_t *connection = (websocket_connection_t*)context;
	if(!connection || !connection->sock) {
		if(size) {
			*size = 0;
		}
		return APR_EOF;
	}
	return apr_socket_recv(connection->sock, buffer, size);
}

static apt_bool_t websocket_socket_send_control(
	void *context, unsigned char opcode, const unsigned char *payload,
	apr_size_t payload_len)
{
	websocket_connection_t *connection = (websocket_connection_t*)context;
	if(!connection || !connection->sock) {
		return FALSE;
	}
	return websocket_send_masked_frame(
		connection->sock, opcode, payload, payload_len);
}

static apt_bool_t websocket_handshake(
	websocket_connection_t *connection, const char *host, apr_port_t port,
	const char *path, apr_pool_t *pool)
{
	char *key;
	char handshake[1024];
	unsigned char response[8192];
	apr_size_t used = 0;
	apr_size_t header_end = 0;
	char *accept_key_ptr;
	char *expected;

	if(!connection || !connection->sock || !host || !pool) {
		return FALSE;
	}
	key = generate_websocket_key(pool);
	if(!key) {
		return FALSE;
	}
	{
		apr_size_t handshake_len = apr_snprintf(handshake, sizeof(handshake),
			"GET %s HTTP/1.1\r\n"
			"Host: %s:%d\r\n"
			"Upgrade: websocket\r\n"
			"Connection: Upgrade\r\n"
			"Sec-WebSocket-Key: %s\r\n"
			"Sec-WebSocket-Version: 13\r\n\r\n",
			path ? path : "/v1/audio/speech/stream", host, port, key);
		if(!websocket_send_all(connection->sock,
			(const unsigned char *)handshake, handshake_len)) {
			return FALSE;
		}
	}

	while(used + 1 < sizeof(response)) {
		apr_size_t received = sizeof(response) - used - 1;
		apr_status_t rv = apr_socket_recv(connection->sock,
			(char *)response + used, &received);
		if(received > 0) {
			used += received;
			response[used] = '\0';
		}
		if(rv != APR_SUCCESS) {
			if(APR_STATUS_IS_EINTR(rv)) {
				continue;
			}
			return FALSE;
		}
		if(received == 0) {
			return FALSE;
		}
		{
			char *marker = strstr((char *)response, "\r\n\r\n");
			if(marker) {
				header_end = (apr_size_t)(marker - (char *)response) + 4;
				break;
			}
		}
	}
	if(header_end == 0 ||
	   (strstr((char *)response, "HTTP/1.1 101") == NULL &&
	    strstr((char *)response, "HTTP/1.0 101") == NULL)) {
		return FALSE;
	}
	if(!ws_strcasestr((char *)response, "Upgrade:") ||
	   !ws_strcasestr((char *)response, "websocket")) {
		return FALSE;
	}
	accept_key_ptr = ws_strcasestr((char *)response, "Sec-WebSocket-Accept:");
	if(!accept_key_ptr) {
		return FALSE;
	}
	expected = websocket_compute_accept(pool, key);
	if(!expected) {
		return FALSE;
	}
	{
		const char *value = accept_key_ptr + strlen("Sec-WebSocket-Accept:");
		char accept_value[256];
		size_t i = 0;
		while(*value == ' ' || *value == '\t') {
			value++;
		}
		while(value[i] && value[i] != '\r' && value[i] != '\n' && i + 1 < sizeof(accept_value)) {
			accept_value[i] = value[i];
			i++;
		}
		accept_value[i] = '\0';
		if(strcmp(expected, accept_value) != 0) {
			return FALSE;
		}
	}

	if(used > header_end) {
		if(!tts_websocket_ws_decoder_set_pending(
			&connection->decoder,
			response + header_end,
			used - header_end)) {
			return FALSE;
		}
	}
	return TRUE;
}

/**
 * @brief 发送 WebSocket 文本帧（客户端模式，必须使用掩码）
 * @param sock WebSocket socket
 * @param text 要发送的文本
 * @param len 文本长度
 * @param pool 内存池
 * @return 成功返回 TRUE
 */
static apt_bool_t websocket_send_text(apr_socket_t *sock, const char *text, apr_size_t len, apr_pool_t *pool)
{
	char frame_header[16];
	unsigned char masking_key[4];
	char *masked_data;
	apr_size_t header_len;
	apr_size_t total_len;
	apr_status_t rv;
	apr_size_t i;

	if(!sock || !text) return FALSE;

	/* ========== 修复：使用线程安全的 apr_generate_random_bytes 替代 rand() ==========
	 * rand() 不是线程安全的，高并发下多个 channel 同时调用会导致数据竞争，
	 * 可能产生错误的 masking key，导致 WebSocket 帧内容损坏。 */
	rv = apr_generate_random_bytes(masking_key, sizeof(masking_key));
	if(rv != APR_SUCCESS) {
		/* 极端情况下的 fallback：使用时间戳和进程ID组合（不如随机数安全，但好过数据竞争） */
		apr_time_t now = apr_time_now();
		pid_t pid = getpid();
		unsigned int seed = (unsigned int)(now ^ (pid << 16));
		for(i = 0; i < 4; i++) {
			seed = seed * 1103515245 + 12345;
			masking_key[i] = (unsigned char)((seed >> 16) & 0xFF);
		}
	}

	/* 分配缓冲区用于存储掩码后的数据 */
	masked_data = apr_palloc(pool, len);
	if(!masked_data) {
		apt_log(SYNTH_LOG_MARK, APT_PRIO_WARNING, "zyTTS: [WS] Failed to allocate masked data buffer");
		return FALSE;
	}

	/* 对所有 payload 字节进行异或掩码运算 */
	for(i = 0; i < len; i++) {
		masked_data[i] = ((unsigned char)text[i]) ^ masking_key[i % 4];
	}

	/* 构建 WebSocket 帧（文本帧，FIN=1, MASK=1） */
	/* 字节0: FIN=1 (0x80), RSV1-3=0, Opcode=1 (文本帧) */
	frame_header[0] = 0x81;

	if(len < 126) {
		frame_header[1] = (unsigned char)(len | 0x80);  /* 设置 MASK=1 */
		header_len = 2;
	} else if(len < 65536) {
		frame_header[1] = (unsigned char)(126 | 0x80);  /* 设置 MASK=1 */
		frame_header[2] = (len >> 8) & 0xFF;
		frame_header[3] = len & 0xFF;
		header_len = 4;
	} else {
		/* 不支持超大帧 */
		apt_log(SYNTH_LOG_MARK, APT_PRIO_WARNING, "zyTTS: [WS] Message too large");
		return FALSE;
	}

	/* 添加掩码密钥到帧头 */
	memcpy(frame_header + header_len, masking_key, 4);
	header_len += 4;

	/* 发送帧头，处理合法短写 */
	if(!websocket_send_all(sock, (const unsigned char *)frame_header, header_len)) {
		apt_log(SYNTH_LOG_MARK, APT_PRIO_WARNING, "zyTTS: [WS] Failed to send frame header");
		return FALSE;
	}

	/* 发送掩码后的帧数据，处理合法短写 */
	if(!websocket_send_all(sock, (const unsigned char *)masked_data, len)) {
		apt_log(SYNTH_LOG_MARK, APT_PRIO_WARNING, "zyTTS: [WS] Failed to send frame data");
		return FALSE;
	}

	apt_log(SYNTH_LOG_MARK, APT_PRIO_DEBUG, "zyTTS: [WS] Sent text frame with masking: %.*s", (int)len, text);
	return TRUE;
}

/**
 * @brief 发送 WebSocket 关闭帧
 * @param sock WebSocket socket
 * @param pool 内存池
 * @return 成功返回 TRUE
 *
 * WebSocket 关闭帧格式：
 * - 字节0: FIN=1, Opcode=0x08 (关闭帧)
 * - 字节1: MASK=1, Payload length=2 (状态码)
 * - 字节2-5: 掩码和状态码 (1000 正常关闭)
 */
static apt_bool_t websocket_send_close(apr_socket_t *sock, apr_pool_t *pool)
{
	const unsigned char close_payload[2] = {0x03, 0xE8};
	(void)pool;

	if(!sock) return FALSE;

	/* 客户端关闭帧必须带 MASK，并且必须完整发送。 */
	if(!websocket_send_masked_frame(sock, 0x08, close_payload,
		sizeof(close_payload))) {
		apt_log(SYNTH_LOG_MARK, APT_PRIO_WARNING, "zyTTS: [WS] Failed to send close frame");
		return FALSE;
	}

	apt_log(SYNTH_LOG_MARK, APT_PRIO_INFO, "zyTTS: [WS] Sent close frame (status 1000)");
	return TRUE;
}

/**
 * @brief 接收 WebSocket 帧
 * @param connection WebSocket connection context
 * @param buffer 接收缓冲区
 * @param buffer_size 缓冲区大小
 * @param is_text_frame 输出参数，是否为文本帧
 * @return 接收到的数据长度，-1 表示错误
 */
static apr_ssize_t websocket_recv_message(
	websocket_connection_t *connection, char *buffer, apr_size_t buffer_size,
	apt_bool_t *is_text_frame)
{
	if(!connection) {
		return -1;
	}
	return tts_websocket_ws_decoder_recv_message(
		&connection->decoder, buffer, buffer_size, is_text_frame);
}
