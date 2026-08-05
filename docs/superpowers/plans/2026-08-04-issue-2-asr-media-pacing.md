# Issue #2 ASR Media Pacing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 FunASR WebSocket 网络 I/O、协议解析和 STOP 收尾完整移出 MPF media callback，使约 20 并发下任一慢会话都不能阻塞其他会话的 20 ms 媒体节拍，同时保持现有 MRCP、音频格式和 FunASR 空二进制结束帧协议。

**Architecture:** 每个 MRCP channel 使用一个可跨 generation 复用的 transport worker 和一个 1 秒有界 SPSC PCM ring。media callback 只做格式校验、8 kHz→16 kHz 重采样、指标更新和非阻塞入队；worker 独占 socket、pollset、HTTP/WebSocket decoder、发送聚合器和 deadline；FunASR consumer task 通过 engine-lifetime transport id 处理 MRCP 状态、STOP、close 和 worker 事件。

**Tech Stack:** C、APR/APR-util、UniMRCP MRCP Engine/MPF、Autotools/Libtool、CMake/CTest、Visual Studio Win32/x64、Python 3 WebSocket fixture、Bash 压测脚本、codebase-memory-mcp。

## Global Constraints

- Issue 为 `#2`；实现分支必须为 `fix/issue-2-asr-media-pacing`，不得在 `main`/`master` 提交或直接推送。
- 所有本机 Shell 命令使用 `rtk` 前缀；源码发现先用 codebase-memory-mcp，只有字符串、配置、非代码文件或图谱不足时才使用 `rtk rg`。
- 不修改 `libs/mpf`、`libs/mrcp*`、`modules/*`、MRCP profile/engine id、FunASR URL/PCM 格式或空 binary end-frame 协议。
- 不修改当前含重复 `Demo-Recog-1` 的 `conf/unimrcpserver.xml`；受控实验从它生成 `/tmp` 隔离配置并证明只有一个目标 engine。重复 id 治理由独立 Issue 处理。
- `funasr_stream_write` 内禁止 DNS/connect/handshake、socket send/recv/close、poll、thread join、APR pool/heap 分配、`apt_task_msg_signal`、MRCP API、`fwrite(stderr)` 和逐帧 INFO/DEBUG 日志。
- media callback 只允许短 ring 临界区；queue full 时锁存 overrun 并唤醒 worker，不能阻塞、静默丢音或直接投递 consumer task。
- transport worker 不保存 channel-pool 指针，不调用 MRCP API；事件只携带稳定 transport id、generation、定长快照和有上限的自有 heap payload。
- consumer task 是 `recog_request`、pending STOP/close response、generation、terminal 状态和 MRCP 事件的唯一写入者。
- pending STOP 时只允许一次 STOP response，禁止 `RECOGNITION-COMPLETE`；close response 必须晚于 `WORKER_CLOSED`、join、解绑和注销。
- 每个平台的结论分别记录为“已验证 / 阻断 / 未验证”；macOS 或 fixture 通过不能替代 Windows、Linux 或生产灰度。

## Fixed Implementation Contracts

### Timing and buffer constants

```c
#define FUNASR_OUTPUT_SAMPLE_RATE          16000U
#define FUNASR_SAMPLE_WIDTH_BYTES          2U
#define FUNASR_MAX_CHANNELS                 2U
#define FUNASR_TX_RING_DURATION_MS       1000U
#define FUNASR_WS_CHUNK_DURATION_MS       200U
#define FUNASR_POLL_TIMEOUT_MS             20U
#define FUNASR_CONNECT_TIMEOUT_US     5000000LL
#define FUNASR_HANDSHAKE_TIMEOUT_US   5000000LL
#define FUNASR_WRITE_STALL_TIMEOUT_US 5000000LL
#define FUNASR_STOP_DRAIN_TIMEOUT_US  5000000LL
#define FUNASR_NO_RESULT_TIMEOUT_US  10000000LL
#define FUNASR_HTTP_HEADER_LIMIT       (16U * 1024U)
#define FUNASR_WS_FRAME_LIMIT          (1U * 1024U * 1024U)
#define FUNASR_WS_MESSAGE_LIMIT        (1U * 1024U * 1024U)
```

`funasr_pcm_bytes_for_ms(16000, channels, 2, 1000)` 是 ring payload 容量，mono/stereo 分别为 32,000/64,000 bytes；同一函数以 200 ms 计算 send chunk，mono/stereo 分别为 6,400/12,800 bytes。任何测试和实现都不得复制字节公式。

### Monotonic clock and production helpers

```c
typedef struct funasr_clock_t {
    apr_int64_t (*now_us)(void *obj);
    void *obj;
} funasr_clock_t;

typedef struct funasr_resample_state_t {
    unsigned char partial[2 * FUNASR_MAX_CHANNELS];
    apr_size_t partial_size;
    int16_t previous[FUNASR_MAX_CHANNELS];
    apt_bool_t previous_valid;
} funasr_resample_state_t;

apr_int64_t funasr_clock_monotonic_now_us(void *obj);
apr_size_t funasr_pcm_bytes_for_ms(apr_uint32_t rate, apr_uint16_t channels,
                                   apr_uint16_t width, apr_uint32_t duration_ms);
apr_size_t funasr_resample_output_capacity(apr_size_t input_bytes,
                                           apr_uint16_t channels);
apt_bool_t funasr_resample_8k_to_16k_into(
    funasr_resample_state_t *state,
    const void *input, apr_size_t input_bytes, apr_uint16_t channels,
    void *output, apr_size_t output_capacity, apr_size_t *output_bytes);
```

`funasr_clock.c` 的平台分支限定为 Windows `QueryPerformanceCounter`、macOS `mach_absolute_time`、Linux `clock_gettime(CLOCK_MONOTONIC)`；Linux 构建显式处理旧 glibc 的 `librt`。

JSON API 使用长度而不是 NUL 终止假设，结果由 heap 所有：

```c
typedef enum {
    FUNASR_JSON_OK,
    FUNASR_JSON_NOT_FOUND,
    FUNASR_JSON_INVALID,
    FUNASR_JSON_LIMIT_EXCEEDED,
    FUNASR_JSON_NO_MEMORY
} funasr_json_status_e;

funasr_json_status_e funasr_json_get_int(
    const char *json, apr_size_t json_size, const char *key, int *value);
funasr_json_status_e funasr_json_get_string_heap(
    const char *json, apr_size_t json_size, const char *key,
    apr_size_t output_limit, char **value, apr_size_t *value_size);
void funasr_json_heap_free(void *value);
```

### Transport and event ownership

```c
typedef apr_uint64_t funasr_transport_id_t;
typedef apr_uint64_t funasr_generation_t;

typedef enum {
    FUNASR_EVENT_INPUT_STARTED,
    FUNASR_EVENT_FINAL_RESULT,
    FUNASR_EVENT_TRANSPORT_FAILED,
    FUNASR_EVENT_GENERATION_DRAINED,
    FUNASR_EVENT_TRANSPORT_METRICS,
    FUNASR_EVENT_WORKER_CLOSED
} funasr_transport_event_type_e;

typedef enum {
    FUNASR_FAILURE_CONNECT,
    FUNASR_FAILURE_HANDSHAKE,
    FUNASR_FAILURE_PROTOCOL,
    FUNASR_FAILURE_QUEUE_OVERRUN,
    FUNASR_FAILURE_WRITE_STALL,
    FUNASR_FAILURE_NO_RESULT_TIMEOUT,
    FUNASR_FAILURE_EOF,
    FUNASR_FAILURE_INTERNAL
} funasr_transport_failure_e;

typedef enum {
    FUNASR_ENQUEUE_ACCEPTED,
    FUNASR_ENQUEUE_STALE_GENERATION,
    FUNASR_ENQUEUE_NOT_STREAMING,
    FUNASR_ENQUEUE_QUEUE_FULL,
    FUNASR_ENQUEUE_CLOSED
} funasr_enqueue_status_e;

typedef struct funasr_media_snapshot_t {
    funasr_generation_t generation;
    apr_uint32_t input_sample_rate;
    apr_uint16_t channel_count;
} funasr_media_snapshot_t;
```

`funasr_transport_event_t` 含 `transport_id`、`generation`、type、failure/metrics 和可选 `text/text_size`。事件与 text 均由 transport heap 分配；event sink 返回 `TRUE` 时把唯一所有权转给 consumer，返回 `FALSE` 时仍由 worker 调用 `funasr_transport_event_destroy()`。consumer 在正常、陈旧 generation、未知 id、shutdown 丢弃四条路径上都恰好销毁一次。

```c
typedef apt_bool_t (*funasr_transport_event_sink_f)(
    void *obj, funasr_transport_event_t *event);

funasr_transport_t *funasr_transport_create(
    apr_pool_t *engine_pool, funasr_transport_id_t id,
    const funasr_transport_config_t *config);
apt_bool_t funasr_transport_begin_generation(
    funasr_transport_t *transport, funasr_generation_t generation,
    const funasr_audio_format_t *format);
apt_bool_t funasr_transport_media_snapshot(
    funasr_transport_t *transport, funasr_media_snapshot_t *snapshot);
funasr_enqueue_status_e funasr_transport_enqueue_pcm(
    funasr_transport_t *transport, funasr_generation_t generation,
    const void *data, apr_size_t size, apr_int64_t now_us);
apt_bool_t funasr_transport_latch_media_failure(
    funasr_transport_t *transport, funasr_generation_t generation,
    funasr_transport_failure_e failure);
apt_bool_t funasr_transport_cancel_generation(
    funasr_transport_t *transport, funasr_generation_t generation);
apt_bool_t funasr_transport_request_close(funasr_transport_t *transport);
apr_status_t funasr_transport_join_closed(funasr_transport_t *transport);
void funasr_transport_event_destroy(funasr_transport_event_t *event);
```

这里用 engine pool + stable id 精化 spec 中的 `create(channel, engine, config)` 设计简写：engine 注册表保存 `id → transport/channel` 映射，但 transport 本身不保存 channel 指针。

---

### Task 0: Establish the Issue branch and protect existing work

**Files:**
- Verify only: current worktree and Issue #2
- Preserve: `docs/superpowers/specs/2026-08-04-issue-2-asr-media-pacing.md`
- Preserve: unrelated user-owned untracked/modified files

**Interfaces:**
- Consumes: Issue #2, current `main`, current untracked spec/plan files.
- Produces: isolated `fix/issue-2-asr-media-pacing` worktree/branch without moving or overwriting user changes.

- [ ] **Step 1: Inspect the worktree and confirm the Issue**

Run: `rtk git status --short`

Run: `rtk git branch --show-current`

Run: `rtk gh issue view 2 --json number,state,title,url`

Expected: Issue #2 exists; all pre-existing changes are recorded before implementation.

- [ ] **Step 2: Enter the required Issue branch using the `using-git-worktrees` skill**

If the current branch is not already `fix/issue-2-asr-media-pacing`, create a dedicated worktree from current `origin/main` while leaving this dirty worktree untouched. Do not use `git reset`, `git checkout --`, stash unrelated changes, or switch a dirty worktree in place.

Run in the new worktree: `rtk git branch --show-current`

Expected: exactly `fix/issue-2-asr-media-pacing`.

- [ ] **Step 3: Copy only the reviewed Issue #2 spec and this plan into the Issue worktree**

Expected: no unrelated untracked file is introduced into the Issue branch; `rtk git status --short` lists only Issue #2 documents before code work begins.

### Task 1: Extract production audio and JSON helpers under tests

**Files:**
- Create: `plugins/demo-recog/src/funasr_audio.h`
- Create: `plugins/demo-recog/src/funasr_audio.c`
- Create: `plugins/demo-recog/src/funasr_json.h`
- Create: `plugins/demo-recog/src/funasr_json.c`
- Modify: `plugins/demo-recog/tests/test_resample.c`
- Modify: `plugins/demo-recog/tests/test_json_unescape.c`
- Modify: `plugins/demo-recog/CMakeLists.txt`

**Interfaces:**
- Implements the fixed audio/JSON contracts above.
- Tests link the production `.c` files directly; neither test may contain a copied implementation or mock APR pool.

- [ ] **Step 1: Rewrite the tests first**

Add resample cases for split odd bytes, split interleaved stereo frames, continuity across two calls, insufficient output capacity, invalid channel count, and exact mono/stereo ring/chunk formulas. Add JSON cases for explicit input length, invalid hex, unmatched surrogate, valid surrogate pair, missing key, embedded escape sequences, output limit and heap cleanup.

- [ ] **Step 2: Register both test executables before implementing the helpers**

```cmake
add_executable(test_resample tests/test_resample.c src/funasr_audio.c)
add_executable(test_json_unescape tests/test_json_unescape.c src/funasr_json.c)
add_test(NAME demorecog_resample COMMAND test_resample)
add_test(NAME demorecog_json_unescape COMMAND test_json_unescape)
```

Run: `rtk cmake -S . -B /tmp/tte-mrcp-issue-2 -DCMAKE_POLICY_VERSION_MINIMUM=3.5`

Run: `rtk cmake --build /tmp/tte-mrcp-issue-2 --target test_resample test_json_unescape`

Expected: compile/link failure because production APIs do not exist yet, proving the tests are red for the intended reason.

- [ ] **Step 3: Implement caller-buffer resampling and bounded JSON decoding**

Preserve the current 8→16 kHz interpolation behavior, but carry partial PCM bytes and the previous sample per channel across calls. Reject channels outside `1..FUNASR_MAX_CHANNELS`. JSON escape decoding must validate every hex digit and UTF-16 surrogate pair, enforce `output_limit` before allocation/write, and never allocate from an APR channel pool.

- [ ] **Step 4: Run the focused tests**

Run: `rtk cmake --build /tmp/tte-mrcp-issue-2 --target test_resample test_json_unescape`

Run: `rtk ctest --test-dir /tmp/tte-mrcp-issue-2 -R '^demorecog_(resample|json_unescape)$' --output-on-failure`

Expected: both tests pass; `rtk rg -n 'funasr_resample_8k_to_16k\(|funasr_json_unescape_string\(' plugins/demo-recog/tests` returns no copied implementation.

- [ ] **Step 5: Commit the production-helper extraction**

Run: `rtk git add plugins/demo-recog/src/funasr_audio.h plugins/demo-recog/src/funasr_audio.c plugins/demo-recog/src/funasr_json.h plugins/demo-recog/src/funasr_json.c plugins/demo-recog/tests/test_resample.c plugins/demo-recog/tests/test_json_unescape.c plugins/demo-recog/CMakeLists.txt`

Run: `rtk git commit -m "refactor(demorecog): extract audio and json helpers" -m "Refs #2"`

### Task 2: Build the deterministic ring, HTTP/WS decoder, and monotonic clock

**Files:**
- Create: `plugins/demo-recog/src/funasr_clock.h`
- Create: `plugins/demo-recog/src/funasr_clock.c`
- Create: `plugins/demo-recog/src/funasr_ws_transport.h`
- Create: `plugins/demo-recog/src/funasr_ws_transport.c`
- Create: `plugins/demo-recog/tests/test_funasr_ws_transport.c`
- Modify: `plugins/demo-recog/CMakeLists.txt`

**Interfaces:**
- Implements the fixed clock, event, ring and decoder contracts.
- Exposes decoder feed/reset and ring enqueue/dequeue operations from `funasr_ws_transport.h` so the test links production functions; these helpers remain transport-owned and are not called by `demo_recog_engine.c`.

- [ ] **Step 1: Write red decoder and ring tests**

Cover HTTP 101 one byte at a time, HTTP header plus first WS frame in one read, invalid status, 16 KiB boundary and overflow. Cover 1-byte WS header/extended-length/mask/payload reads, fragmented text/binary with interleaved Ping/Pong, Close, empty binary frame, invalid continuation/opcode/control fragmentation, EOF, `EAGAIN`, frame limit and reassembled-message limit. Cover ring empty/full/wraparound, generation mismatch, queue-overrun latch, mono/stereo capacity and no overwrite.

Add a fake clock whose `now_us` returns a test-owned counter; test deadline comparisons at `limit-1`, `limit`, and `limit+1` without sleeping.

- [ ] **Step 2: Register and run the failing transport target**

```cmake
add_executable(test_funasr_ws_transport
    tests/test_funasr_ws_transport.c
    src/funasr_clock.c
    src/funasr_json.c
    src/funasr_ws_transport.c)
set_target_properties(test_funasr_ws_transport PROPERTIES
    COMPILE_DEFINITIONS "FUNASR_TRANSPORT_TESTING=1")
add_test(NAME demorecog_funasr_ws_transport COMMAND test_funasr_ws_transport)
```

Run: `rtk cmake --build /tmp/tte-mrcp-issue-2 --target test_funasr_ws_transport`

Expected: failure on missing transport types/functions, not an unrelated include-path error.

- [ ] **Step 3: Implement the SPSC ring and incremental parsers**

Ring mutex sections may only copy bytes/update indices/latches/signal the worker condition. HTTP completion must return the unconsumed suffix to the WS decoder. The WS parser must retain partial header/payload state across feeds and deliver only complete messages. It must answer Ping with Pong through a worker-owned outbound control queue and emit one protocol failure per generation.

- [ ] **Step 4: Implement monotonic platform adapters**

Use conditional compilation only in `funasr_clock.c`; convert native ticks to microseconds without overflow. Fake clocks are passed through `funasr_transport_config_t`, never selected by a global variable.

- [ ] **Step 5: Run data-structure and parser tests**

Run: `rtk cmake --build /tmp/tte-mrcp-issue-2 --target test_funasr_ws_transport`

Run: `rtk ctest --test-dir /tmp/tte-mrcp-issue-2 -R '^demorecog_funasr_ws_transport$' --output-on-failure`

Expected: all deterministic ring/HTTP/WS/clock cases pass without real sleep or network access.

- [ ] **Step 6: Commit the transport core**

Run: `rtk git add plugins/demo-recog/src/funasr_clock.h plugins/demo-recog/src/funasr_clock.c plugins/demo-recog/src/funasr_ws_transport.h plugins/demo-recog/src/funasr_ws_transport.c plugins/demo-recog/tests/test_funasr_ws_transport.c plugins/demo-recog/CMakeLists.txt`

Run: `rtk git commit -m "feat(demorecog): add bounded websocket transport core" -m "Refs #2"`

### Task 3: Add the nonblocking per-channel transport worker

**Files:**
- Modify: `plugins/demo-recog/src/funasr_ws_transport.h`
- Modify: `plugins/demo-recog/src/funasr_ws_transport.c`
- Modify: `plugins/demo-recog/tests/test_funasr_ws_transport.c`

**Interfaces:**
- Completes `create/begin_generation/enqueue_pcm/cancel_generation/request_close/join_closed`.
- `funasr_transport_config_t` contains host/port/path, limits/deadlines, monotonic clock, event sink, and an optional I/O vtable used only by tests; `NULL` selects APR socket/pollset I/O.

- [ ] **Step 1: Add failing worker and fault-injection tests**

Use a fake I/O vtable to drive short send/recv, `EAGAIN`, sticky handshake bytes, partial payload delayed by 3 fake seconds, 5-second connect/handshake/write-stall/STOP deadlines, slow reader, poll wake and timed-poll fallback. Enqueue 50 frames at 20 ms fake-clock intervals while a payload is incomplete; assert all calls return without waiting on I/O and the worker later reconstructs the exact PCM order.

Add tests for `APR_POLLOUT` registration only while bytes are pending, per-poll read/write byte budgets, overrun→one failure event, one `INPUT_STARTED` per generation, one `GENERATION_DRAINED` on all STOP exits, `WORKER_CLOSED` as the final event, mandatory join, dynamic event-allocation failure, and final close-fence signal failure without freeing engine/channel pools.

- [ ] **Step 2: Run the new cases red**

Run: `rtk cmake --build /tmp/tte-mrcp-issue-2 --target test_funasr_ws_transport`

Run: `rtk ctest --test-dir /tmp/tte-mrcp-issue-2 -R '^demorecog_funasr_ws_transport$' --output-on-failure`

Expected: failures identify unimplemented worker lifecycle/deadline behavior.

- [ ] **Step 3: Implement the worker loop and APR wake fallback**

Create one dedicated APR subpool and thread per transport before returning from `begin_generation`; only the worker uses its socket/pollset/decoder/send aggregator. Compile the wakeable path only under `#ifdef APR_POLLSET_WAKEABLE`; on `APR_ENOTIMPL`, record one downgrade and use a maximum 20 ms timed poll. `EAGAIN` never closes the socket or advances `last_write_progress_us`.

On STOP, stop accepting the generation, flush queued tail, send one empty binary frame, and emit exactly one drained event on success/failure/deadline. On close, stop normal event production, close worker-owned I/O, emit metrics, then emit the final close fence. Reserve the close-fence task message before the worker starts so ordinary heap/message-pool exhaustion cannot suppress the fence; if task signaling itself fails, latch `close_fence_failed`, leave engine/channel pools alive, and make fatal shutdown join the already-closing worker before reporting failure.

- [ ] **Step 4: Prove callback-facing operations are nonblocking**

The enqueue path may lock/copy/update/signal only. Add a test sink whose consumer queue is full: worker event delivery may fail and must free the event; media enqueue must remain independent of that sink.

Run: `rtk ctest --test-dir /tmp/tte-mrcp-issue-2 -R '^demorecog_funasr_ws_transport$' --output-on-failure`

Expected: all worker, ownership, deadline and race tests pass; no real 3- or 5-second sleep exists in the test.

- [ ] **Step 5: Commit the worker**

Run: `rtk git add plugins/demo-recog/src/funasr_ws_transport.h plugins/demo-recog/src/funasr_ws_transport.c plugins/demo-recog/tests/test_funasr_ws_transport.c`

Run: `rtk git commit -m "feat(demorecog): isolate websocket io per channel" -m "Refs #2"`

### Task 4: Implement the consumer-task control state machine

**Files:**
- Create: `plugins/demo-recog/src/funasr_control.h`
- Create: `plugins/demo-recog/src/funasr_control.c`
- Create: `plugins/demo-recog/tests/test_funasr_control.c`
- Modify: `plugins/demo-recog/CMakeLists.txt`

**Interfaces:**
- `funasr_control_t` owns generation, terminal, input-started, STOP/close pending and accepting-media state.
- A vtable adapts pure transitions to transport begin/cancel/close/join and fake/real MRCP sends; tests use a counting fake sink.

```c
typedef struct funasr_control_vtable_t {
    apt_bool_t (*send_start_of_input)(void *obj, funasr_generation_t generation);
    apt_bool_t (*send_recognition_complete)(void *obj,
        funasr_generation_t generation, mrcp_recog_completion_cause_e cause,
        const char *text);
    apt_bool_t (*send_stop_response)(void *obj, funasr_generation_t generation);
    apt_bool_t (*send_close_response)(void *obj);
    apt_bool_t (*cancel_generation)(void *obj, funasr_generation_t generation);
    apt_bool_t (*request_close)(void *obj);
    apr_status_t (*join_closed)(void *obj);
} funasr_control_vtable_t;

void funasr_control_init(funasr_control_t *control);
apt_bool_t funasr_control_begin_generation(
    funasr_control_t *control, funasr_generation_t generation);
apt_bool_t funasr_control_request_stop(
    funasr_control_t *control, const funasr_control_vtable_t *vtable, void *obj);
apt_bool_t funasr_control_request_close(
    funasr_control_t *control, const funasr_control_vtable_t *vtable, void *obj);
apt_bool_t funasr_control_handle_event(
    funasr_control_t *control, const funasr_transport_event_t *event,
    const funasr_control_vtable_t *vtable, void *obj);
```

- [ ] **Step 1: Write the control tests first**

Cover final-before-STOP; STOP-before-final; STOP drain timeout; failure/overrun vs STOP in both consumer processing orders; close-before-final; ordinary event after close request; engine close with live channels; stale id/generation; payload released once; pending STOP queues the next RECOGNIZE until response; new generation has an empty ring; one input-start event; no-result-timeout→`NO_INPUT_TIMEOUT`; pending STOP suppresses that completion.

The fake MRCP expectations must match `libs/mrcp-engine/src/mrcp_recog_state_machine.c`: pending STOP never expects completion plus STOP response.

- [ ] **Step 2: Register and run the failing control target**

```cmake
add_executable(test_funasr_control
    tests/test_funasr_control.c
    src/funasr_control.c)
add_test(NAME demorecog_funasr_control COMMAND test_funasr_control)
```

Run: `rtk cmake --build /tmp/tte-mrcp-issue-2 --target test_funasr_control`

Expected: missing transition implementation or failing ordering assertions.

- [ ] **Step 3: Implement deterministic transitions**

`FINAL_RESULT` maps to `SUCCESS`; `NO_RESULT_TIMEOUT` maps to `NO_INPUT_TIMEOUT`; all other transport failures map to `ERROR`. The first terminal event wins unless STOP is already pending; stale events only increment/drop metrics. `funasr_control_handle_event()` never frees its input; its caller destroys every event exactly once after the transition returns. `GENERATION_DRAINED` is the only transition that releases a pending STOP response. `WORKER_CLOSED` is the only transition that permits join/unregister/close response.

- [ ] **Step 4: Run control and transport tests together**

Run: `rtk cmake --build /tmp/tte-mrcp-issue-2 --target test_funasr_ws_transport test_funasr_control`

Run: `rtk ctest --test-dir /tmp/tte-mrcp-issue-2 -R '^demorecog_(funasr_ws_transport|funasr_control)$' --output-on-failure`

Expected: both targets pass; event counts show no completion+STOP double terminal outcome.

- [ ] **Step 5: Commit the control plane**

Run: `rtk git add plugins/demo-recog/src/funasr_control.h plugins/demo-recog/src/funasr_control.c plugins/demo-recog/tests/test_funasr_control.c plugins/demo-recog/CMakeLists.txt`

Run: `rtk git commit -m "feat(demorecog): add deterministic recognition control" -m "Refs #2"`

### Task 5: Integrate transport/control into the MRCP engine and simplify the media callback

**Files:**
- Modify: `plugins/demo-recog/src/demo_recog_engine.c`
- Modify: `plugins/demo-recog/src/funasr_control.h`
- Modify: `plugins/demo-recog/src/funasr_control.c`
- Modify: `plugins/demo-recog/src/funasr_ws_transport.h`
- Modify: `plugins/demo-recog/src/funasr_ws_transport.c`
- Modify: `plugins/demo-recog/CMakeLists.txt`
- Modify: `plugins/demo-recog/tests/test_funasr_control.c`

**Interfaces:**
- Engine registry is consumer-task-only and stores stable entries `{id, transport, channel_or_null, next}` allocated from the plugin-create pool.
- `funasr_msg_t` gains a transport-event case carrying only `funasr_transport_event_t *`; the signal adapter owns the event only after successful `apt_task_msg_signal`.
- Each transport reserves its final close-fence task message before thread start; failure to deliver that fence is a fatal close state, never permission to respond or free a pool.
- Channel stores a stable transport pointer, control state, preallocated resample scratch/capacity and resample state; socket/decoder/send fields and the unused 10 MiB audio retention buffer are removed.

- [ ] **Step 1: Add integration assertions to the control test**

Exercise registry lookup, unknown id, signal failure ownership, close fence ordering, engine `QUIESCING`, multiple live transports, registry empty before task termination, and `channel_or_null` clearing before channel close response.

- [ ] **Step 2: Add engine lifecycle state and registry**

Store the plugin-create pool in `funasr_engine_t`, allocate monotonically increasing nonzero transport ids, reject new generation starts while quiescing, and mutate the registry only in `funasr_msg_process`. `funasr_engine_close` snapshots/requests all closes and responds only after the last close fence is processed.

- [ ] **Step 3: Replace RECOGNIZE/STOP/close paths**

On RECOGNIZE, validate 1–2 LPCM channels, compute scratch capacity from the negotiated descriptor/frame duration, allocate/resize scratch on the consumer task before accepting media, increment generation, reset control/metrics/resampler, begin the worker, then send IN-PROGRESS. On STOP, mark canceling, save response and call cancel without waiting. On channel close, invalidate generation, save close response and request close without waiting.

- [ ] **Step 4: Reduce `funasr_stream_write` to local processing**

The final callback shape is:

```c
if (!frame || !(frame->type & MEDIA_FRAME_TYPE_AUDIO) || !frame->codec_frame.size)
    return TRUE;
if (!funasr_transport_media_snapshot(channel->transport, &media))
    return TRUE;
now_us = channel->clock.now_us(channel->clock.obj);
data = frame->codec_frame.buffer;
size = frame->codec_frame.size;
generation = media.generation;
if (media.input_sample_rate == 8000) {
    if (!funasr_resample_8k_to_16k_into(&channel->resample, data, size,
            media.channel_count, channel->scratch,
            channel->scratch_capacity, &size))
        return funasr_transport_latch_media_failure(channel->transport,
            generation, FUNASR_FAILURE_INTERNAL);
    data = channel->scratch;
}
funasr_transport_enqueue_pcm(channel->transport, generation, data, size, now_us);
return TRUE;
```

Handle `QUEUE_FULL` by the transport's latched failure; do not signal the consumer directly. Remove callback socket calls, blocking receive, per-frame log/hex dump/stderr, duplicate resample calls, audio retention copies, STOP polling and 10-second wall-clock timeout.

- [ ] **Step 5: Route all worker events on the consumer task**

`INPUT_STARTED` calls the existing start-of-input helper once. Final/failure/drained/metrics/closed go through `funasr_control`. NLSML remains allocated from the channel/request pool only after id+generation validation. Log lifecycle/terminal metrics without final text or payload dumps.

- [ ] **Step 6: Delete obsolete inline networking/parser/helper functions**

Remove `funasr_websocket_connect/disconnect/send/recv`, buffered send state, inline JSON helpers and the old pool-allocating resampler from `demo_recog_engine.c`; production implementations now live only in the new modules.

- [ ] **Step 7: Build and run all four CMake tests**

Run: `rtk cmake --build /tmp/tte-mrcp-issue-2 --target test_resample test_json_unescape test_funasr_ws_transport test_funasr_control`

Run: `rtk ctest --test-dir /tmp/tte-mrcp-issue-2 -R '^demorecog_' --output-on-failure`

Expected: four tests pass; control tests exercise actual production control/transport code.

- [ ] **Step 8: Check the callback boundary**

Run: `rtk rg -n 'apr_socket_|apr_pollset_|apr_thread_join|apt_task_msg_signal|mrcp_engine_channel_message_send|fwrite|funasr_websocket_' plugins/demo-recog/src/demo_recog_engine.c`

Expected: any remaining matches are outside `funasr_stream_write`; manual inspection confirms the callback contains none of them and no allocation/logging call.

- [ ] **Step 9: Commit engine integration**

Run: `rtk git add plugins/demo-recog/src/demo_recog_engine.c plugins/demo-recog/src/funasr_control.h plugins/demo-recog/src/funasr_control.c plugins/demo-recog/src/funasr_ws_transport.h plugins/demo-recog/src/funasr_ws_transport.c plugins/demo-recog/CMakeLists.txt plugins/demo-recog/tests/test_funasr_control.c`

Run: `rtk git commit -m "fix(demorecog): remove network io from media callback" -m "Refs #2"`

### Task 6: Synchronize Autotools, CMake, and Visual Studio build entry points

**Files:**
- Modify: `configure.ac`
- Modify: `configure` only through `bootstrap` if tracked generation changes it
- Modify: `plugins/demo-recog/src/Makefile.am`
- Modify: `plugins/demo-recog/Makefile.in` only through Automake
- Modify: `plugins/demo-recog/CMakeLists.txt`
- Modify: `plugins/demo-recog/demorecog.vcxproj`
- Modify: `plugins/demo-recog/demorecog.vcxproj.filters`
- Modify: `plugins/demo-recog/demorecog.vcproj`
- Create: `plugins/demo-recog/tests/test_funasr_ws_transport.vcxproj`
- Create: `plugins/demo-recog/tests/test_funasr_ws_transport.vcxproj.filters`
- Create: `plugins/demo-recog/tests/test_funasr_ws_transport.vcproj`
- Create: `plugins/demo-recog/tests/test_funasr_control.vcxproj`
- Create: `plugins/demo-recog/tests/test_funasr_control.vcxproj.filters`
- Create: `plugins/demo-recog/tests/test_funasr_control.vcproj`
- Modify: `unimrcp.sln`
- Modify: `unimrcp-2010.sln`

**Interfaces:**
- Plugin target compiles all five production modules plus `demo_recog_engine.c`.
- CTest and `make check` expose exactly `demorecog_resample`, `demorecog_json_unescape`, `demorecog_funasr_ws_transport`, `demorecog_funasr_control`.
- Windows test `main` functions live only in independent console projects, never in the DLL project.

- [ ] **Step 1: Complete CMake source/link wiring**

Add all new production `.c` files to `DEMO_RECOG_SOURCES`. Link Linux targets with `rt` when required; keep Windows `ws2_32 winmm` and Unix `m`. Ensure all four tests inherit APR/UniMRCP include paths and link only the libraries their production sources require.

- [ ] **Step 2: Add Autotools production and check targets**

Add all sources/headers to `demorecog_la_SOURCES`; define four `check_PROGRAMS` and `TESTS`, with each test compiling the same production module list as CMake. Add `AC_SEARCH_LIBS([clock_gettime], [rt])` in `configure.ac` for Linux monotonic clock linkage.

- [ ] **Step 3: Regenerate tracked Autotools outputs**

Run: `rtk ./bootstrap`

Run: `rtk ./configure --help`

Expected: generation succeeds; `Makefile.in`/`configure` changes are tool-generated and reviewable. Do not hand-edit either generated file.

- [ ] **Step 4: Add production files and independent tests to Visual Studio**

Add `.c/.h` entries and filters to the plugin projects. Create two Console Application test projects with unique GUIDs, Win32/x64 Debug/Release configurations, inherited UniMRCP property sheets, the required production `.c` files, and project references to APR Toolkit/MRCP Engine only where needed. Add both projects under the `tests` solution folder in both solutions with complete configuration mappings.

- [ ] **Step 5: Validate generated/project structure locally**

Run: `rtk make check`

Run: `rtk cmake -S . -B /tmp/tte-mrcp-issue-2 -DCMAKE_POLICY_VERSION_MINIMUM=3.5`

Run: `rtk cmake --build /tmp/tte-mrcp-issue-2 --target test_resample test_json_unescape test_funasr_ws_transport test_funasr_control`

Run: `rtk ctest --test-dir /tmp/tte-mrcp-issue-2 -R '^demorecog_' --output-on-failure`

Expected: Autotools check and all four CTest entries pass, or any unrelated known baseline blocker is recorded verbatim and separated from focused-test results.

- [ ] **Step 6: Commit build-system parity**

Run: `rtk git add configure.ac configure plugins/demo-recog/src/Makefile.am plugins/demo-recog/Makefile.in plugins/demo-recog/CMakeLists.txt plugins/demo-recog/demorecog.vcxproj plugins/demo-recog/demorecog.vcxproj.filters plugins/demo-recog/demorecog.vcproj plugins/demo-recog/tests/test_funasr_ws_transport.vcxproj plugins/demo-recog/tests/test_funasr_ws_transport.vcxproj.filters plugins/demo-recog/tests/test_funasr_ws_transport.vcproj plugins/demo-recog/tests/test_funasr_control.vcxproj plugins/demo-recog/tests/test_funasr_control.vcxproj.filters plugins/demo-recog/tests/test_funasr_control.vcproj unimrcp.sln unimrcp-2010.sln`

If `configure` is not tracked or unchanged, omit it from the exact add list.

Run: `rtk git commit -m "build(demorecog): wire transport tests across platforms" -m "Refs #2"`

### Task 7: Add a controllable FunASR fixture and pacing evidence output

**Files:**
- Create: `tools/diagnostics/funasr_ws_fixture.py`
- Modify: `tools/stress/stress_test_improved.sh`
- Create: `docs/reports/2026-08-04-issue-2-asr-media-pacing-validation.md`

**Interfaces:**
- Fixture modes: normal, slow-read, bytewise HTTP header, split WS payload, Ping interleave, delayed final, Close and connection-specific fault injection.
- Fixture emits JSON Lines with connection id, monotonic-relative timestamps, wall-clock start/end, bytes read/written, injected pauses and final outcome.
- Stress script preserves all existing flags/defaults and adds `--server-log=`, `--fixture-report=`, `--pacing-json=`, and `--warmup=`.

- [ ] **Step 1: Write fixture protocol self-tests before the server implementation**

Use Python `unittest` in the same file behind `--self-test` to verify HTTP upgrade response generation, frame encoding, split schedule, Ping/Close behavior, connection-id selection and JSONL schema. Do not add third-party Python dependencies.

Run: `rtk python3 tools/diagnostics/funasr_ws_fixture.py --self-test`

Expected: red until handlers are implemented, then all self-tests pass.

- [ ] **Step 2: Implement the local-only fixture**

Default bind is `127.0.0.1`; require an explicit flag for non-loopback. Add `--emit-config SOURCE TARGET --host 127.0.0.1 --port PORT --path /ws/audio` that parses XML, removes duplicate recognizer engine entries in the target copy only, inserts exactly one `Demo-Recog-1` pointing to the fixture, and refuses to overwrite `SOURCE` or any path outside the caller-supplied target.

- [ ] **Step 3: Add machine-readable pacing aggregation**

Parse structured per-session plugin summaries and fixture JSONL. Report warm-up, sample window, injection connection id, requests, successes, normal completions, STOP cancellations, valid audio bytes, ring high water, overrun bytes/events, abnormal closes, first-send latency, all nonfault frame-gap samples, p99 and maximum. Use each process's monotonic deltas and wall-clock correlation; never subtract monotonic values from different processes.

- [ ] **Step 4: Preserve legacy stress behavior**

Run: `rtk bash -n tools/stress/stress_test_improved.sh`

Run: `rtk bash tools/stress/stress_test_improved.sh -h`

Expected: existing help/default flags remain; new options are additive and missing pacing inputs do not change legacy runs.

- [ ] **Step 5: Run a loopback fixture smoke test**

Run: `rtk python3 tools/diagnostics/funasr_ws_fixture.py --self-test`

Run: `rtk python3 -m py_compile tools/diagnostics/funasr_ws_fixture.py`

Expected: syntax and self-tests pass; no external FunASR endpoint is contacted.

- [ ] **Step 6: Execute the controlled 20-session experiment when local binaries are available**

Generate `/tmp/tte-mrcp-issue-2-unimrcpserver.xml`, parse it independently, and prove exactly one `Demo-Recog-1` points at loopback before starting the server. Then run one warm-up followed by 20 concurrent ASR sessions with exactly one slow/split connection.

Run: `rtk bash tools/stress/stress_test_improved.sh -t asr -c 20 -i 1 -r /tmp/tte-mrcp-issue-2-runtime -a data --server-log=/tmp/tte-mrcp-issue-2-server.log --fixture-report=/tmp/tte-mrcp-issue-2-fixture.jsonl --pacing-json=/tmp/tte-mrcp-issue-2-pacing.json --warmup=1`

Expected acceptance:

- all 20 sessions have `media_frames > 0` and zero ring overrun;
- pooled nonfault gap samples, excluding first frame and declared source pause, have p99 `<100 ms` and maximum `<250 ms`;
- the injected session reaches terminal within its 5-second transport deadline and does not cause another session to exceed 250 ms;
- Ping/split/Close produces neither false close nor duplicate completion.

If binaries are blocked by the known MPF baseline, record the exact blocker and mark controlled integration “阻断”; do not fabricate results from unit tests.

- [ ] **Step 7: Write the validation report with actual evidence**

Record source/test/live evidence separately, exact commands and exit codes, fixture parameters, JSON artifact paths, threshold calculations, known blockers, and a platform matrix. Mark production gray “线上未验证” unless real gray data is supplied by the release process.

- [ ] **Step 8: Commit fixture, stress metrics and evidence**

Run: `rtk git add tools/diagnostics/funasr_ws_fixture.py tools/stress/stress_test_improved.sh docs/reports/2026-08-04-issue-2-asr-media-pacing-validation.md`

Run: `rtk git commit -m "test(demorecog): add media pacing fault injection" -m "Refs #2"`

### Task 8: Run platform gates and refresh the code graph

**Files:**
- Verify: all Issue #2 source/build/test files
- Modify: `docs/reports/2026-08-04-issue-2-asr-media-pacing-validation.md` with actual results
- Modify: persisted codebase-memory artifact only if the indexer writes one

**Interfaces:**
- Produces source, unit, build, controlled-integration and platform evidence without conflating them.

- [ ] **Step 1: Run the full macOS CMake gate**

Run: `rtk cmake -S . -B /tmp/tte-mrcp-cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5`

Run: `rtk cmake --build /tmp/tte-mrcp-cmake`

Run: `rtk ctest --test-dir /tmp/tte-mrcp-cmake --output-on-failure`

If full build hits the known `JB_TRACE/RTP_TRACE` versus `mpf_null_trace()` blocker, save the original failure, mark plugin artifact “阻断/未验证”, then run:

Run: `rtk cmake --build /tmp/tte-mrcp-cmake --target test_resample test_json_unescape test_funasr_ws_transport test_funasr_control`

Run: `rtk ctest --test-dir /tmp/tte-mrcp-cmake -R '^demorecog_' --output-on-failure`

- [ ] **Step 2: Run the final Autotools gate**

Run: `rtk ./bootstrap`

Run: `rtk ./configure --help`

Run: `rtk make check`

Expected: generated inputs are synchronized and checks pass, or the exact unrelated blocker is recorded.

- [ ] **Step 3: Run or explicitly defer Windows gates**

On a Windows builder with repository dependencies configured, build `unimrcp-2010.sln` and `unimrcp.sln` for Debug/Release Win32/x64, run both new test executables, load `demorecog.dll`, and run the 20-session loopback fixture. If that environment is unavailable, record Windows as “未验证”, not passed.


- [x] **Step 4: Define Linux production-target gates as a GitHub Actions workflow**

`.github/workflows/build-linux.yml` 现在把 Linux 门禁定义为 CI：

- `build-linux` 在 ABI 基线容器（RHEL 7 / Rocky Linux 8）内完成 Autotools 构建、`make check`（kylin 行另跑 CMake/CTest 插件测试）与 ELF/GLIBC/`ldd` 审计。
- `verify-linux` 在 Rocky Linux 8 容器内完成 `demorecog.so`/`tts_websocket.so` 加载、RTP/MRCP 建链冒烟和 1 次预热 + 20 并发 split-payload loopback，并断言零 overrun、非故障 p99 `<100 ms`、最大值 `<250 ms`。
- [x] **Step 4a（已执行）**：Actions run 30967036638 首次绿色运行；rhel7-x86_64 与 kylinv10-aarch64 的 `verify-linux` 各 20/20 会话成功、零 overrun、非故障 gap p99/max 10–11 ms。pacing JSON、fixture JSONL、server log 与测试日志已作为证据回填到验证报告，Linux 与端到端项改为“已验证”。


- [ ] **Step 5: Run final hygiene checks**

Run: `rtk git diff --check`

Run: `rtk git status --short`

Run: `rtk rg -n 'Recv audio|Sending WebSocket chunk|fwrite\(stderr|hex dump' plugins/demo-recog/src`

Expected: no whitespace error, no build artifact/sensitive payload, and no default per-frame/payload logging.

- [ ] **Step 6: Re-index and query canonical nodes**

Run codebase-memory-mcp `index_repository(repo_path="/Users/huangzhonghui/tte-mrcp", mode="fast")`.

Run `search_graph(name_pattern=".*funasr_transport.*")` and `trace_path(function_name="funasr_stream_write", direction="outbound")`.

Expected: canonical transport/clock/control nodes exist; `funasr_stream_write` reaches `funasr_transport_enqueue_pcm` and no socket function. Re-query the MPF scheduler chain and preserve it as root-cause evidence, not as a modified area.

- [ ] **Step 7: Commit final evidence updates**

Run: `rtk git add docs/reports/2026-08-04-issue-2-asr-media-pacing-validation.md`

Run: `rtk git commit -m "docs(demorecog): record issue 2 validation evidence" -m "Refs #2"`

If the report did not change after Task 7, skip this empty commit.

### Task 9: Review and prepare the protected PR handoff

**Files:**
- Review: all commits and files on `fix/issue-2-asr-media-pacing`
- No new source file unless review finds a scoped defect

**Interfaces:**
- Produces a reviewable Issue branch and PR description; push/PR creation remains behind explicit user confirmation.

- [ ] **Step 1: Use the `requesting-code-review` skill**

Review correctness, race/lifetime safety, exactly-once payload release, timeout arithmetic, parser limits, callback prohibitions, Windows/Linux compile branches, test realism and spec coverage. Fix actionable findings with focused tests and a separate conventional commit.

- [ ] **Step 2: Inspect branch scope and commit history**

Run: `rtk git status --short`

Run: `rtk git log --oneline origin/main..HEAD`

Run: `rtk git diff --stat origin/main...HEAD`

Run: `rtk git diff --check origin/main...HEAD`

Expected: only Issue #2 docs, plugin implementation/tests/build entries, fixture, stress script and validation report are present.

- [ ] **Step 3: Prepare the PR body**

Include root cause, explicit non-goals, thread ownership, STOP/close semantics, callback proof, unit/fixture metrics, platform status, known blockers, rollback boundary and `Closes #2`. Do not state Windows/Linux/production passed unless evidence exists.

- [ ] **Step 4: Stop for external-operation confirmation**

Do not run `git push` or `gh pr create` until the user explicitly authorizes publication. After approval, push only `fix/issue-2-asr-media-pacing` and open a PR targeting `main`; never push `main` directly.

## Completion Checklist

- [ ] `funasr_stream_write` contains no network, wait, allocation, task-signal, MRCP or per-frame logging operation.
- [ ] Production helper tests no longer copy resample/JSON implementations.
- [ ] Transport tests cover HTTP/WS short reads, sticky bytes, fragmentation/control frames, limits, `EAGAIN`, ring full, slow reader, wake fallback, deadlines and close/join.
- [ ] Control tests cover `INPUT_STARTED`, final/STOP/failure races, no-result mapping, generation isolation, close fence, engine quiescing and exactly-once release.
- [ ] CMake, Autotools and both Visual Studio solution families reference the same production/test files.
- [ ] Controlled 20-session evidence meets thresholds or is honestly marked blocked with raw failure evidence.
- [ ] macOS/Windows/Linux and production gray each have an explicit independent status.
- [ ] `git diff --check` and codebase-memory re-index/query pass.
- [ ] Protected Issue branch is ready; publication waits for explicit confirmation.
