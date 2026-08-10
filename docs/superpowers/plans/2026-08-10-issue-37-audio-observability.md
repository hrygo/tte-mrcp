# Issue #37 音频链路可观测性实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 `subagent-driven-development`（推荐）或 `executing-plans` 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 为 ASR 会话提供节流、可关联的上游/下游音频统计日志，并在 TTS SPEAK 日志中加入跨平台线程标识。

**架构：** 在 ASR transport 中记录实际成功入队和实际 WebSocket 音频写入的独立统计；仅在 metrics 事件的固定窗口或终态，由 engine 在 consumer-task 路径格式化并输出关联汇总。TTS 仅增强 SPEAK 的既有会话日志，不改变请求处理。

**技术栈：** C、APR/UniMRCP、现有 FunASR transport fake I/O 单测、Autotools/CMake。

---

## 文件职责

| 文件 | 职责 |
| --- | --- |
| `plugins/asr-websocket/src/funasr_ws_transport.h` | 定义收发统计、节流快照和读取 API。 |
| `plugins/asr-websocket/src/funasr_ws_transport.c` | 在入队/实际写入处维护计数，并在锁下产出一致快照。 |
| `plugins/asr-websocket/src/asr_websocket_engine.c` | 根据 session/channel 和 generation 输出节流或终态的结构化汇总。 |
| `plugins/asr-websocket/tests/test_funasr_ws_transport.c` | 用 fake clock/I/O 验证收发计数、阻塞与终态快照。 |
| `plugins/tts-websocket/src/tts_websocket_engine.c` | 用 APR 的跨平台线程 API 增强 SPEAK 入口日志。 |
| `plugins/tts-websocket/src/tts_websocket_thread.[ch]` | 将 APR 线程标识转换隔离为可复用、可单测的跨平台适配。 |
| `plugins/tts-websocket/tests/test_tts_websocket_thread.c` | 验证当前 APR 线程标识可用。 |
| `docs/superpowers/plans/2026-08-10-issue-37-audio-observability.md` | 记录并追踪本计划。 |

### 任务 1：扩展 ASR transport 收发快照

**文件：**
- 修改：`plugins/asr-websocket/src/funasr_ws_transport.h`
- 修改：`plugins/asr-websocket/src/funasr_ws_transport.c`
- 测试：`plugins/asr-websocket/tests/test_funasr_ws_transport.c`

- [x] **步骤 1：编写失败的 transport 测试**

在 fake I/O 已完成 binary audio write 的测试中，断言 metrics 快照同时包含上游 `media_frames`/`valid_audio_bytes` 与下游 `ws_audio_frames`/`ws_audio_bytes`。该 fixture 当前仅覆盖完整 write；短写握手语义不在本 Issue 范围内。

- [x] **步骤 2：运行测试验证失败**

运行：`cmake --build /tmp/tte-mrcp-issue37 --target test_funasr_ws_transport && ctest --test-dir /tmp/tte-mrcp-issue37 -R '^asr_websocket_funasr_ws_transport$' --output-on-failure`

预期：失败，原因是下游音频统计字段或断言支持尚不存在。

- [x] **步骤 3：实现最小统计与快照**

在 `funasr_transport_metrics_t` 增加 `ws_audio_frames`、`ws_audio_bytes`、最近成功写入时间和下游相邻帧间隔；只有 worker 成功写完整个 `tx_audio` WebSocket frame 时更新它们。新增带 mutex 的 metrics snapshot API，保留现有字段和 WebSocket/MRCP 行为。

- [x] **步骤 4：运行测试验证通过**

运行步骤 2 的命令。

预期：目标测试通过，且原有 transport 测试继续通过。

- [x] **步骤 5：重构并回归**

将时间差计算抽为 transport 内私有 helper，保证上游与下游均以同一个单调 clock 计算，不在音频回调中做日志格式化。

- [x] **步骤 6：提交**

```bash
git add plugins/asr-websocket/src/funasr_ws_transport.h plugins/asr-websocket/src/funasr_ws_transport.c plugins/asr-websocket/tests/test_funasr_ws_transport.c docs/superpowers/plans/2026-08-10-issue-37-audio-observability.md
git commit -m "feat(asr): track websocket audio delivery metrics" -m "Refs #37"
```

### 任务 2：输出关联、节流的 ASR 汇总日志

**文件：**
- 修改：`plugins/asr-websocket/src/asr_websocket_engine.c`
- 修改：`plugins/asr-websocket/src/funasr_ws_transport.[ch]`
- 测试：`plugins/asr-websocket/tests/test_funasr_ws_transport.c`

- [x] **步骤 1：编写失败的节流/终态测试**

使用 fake clock 产生连续 metrics 快照，断言窗口未到期时不请求输出，到期时产生一次进度快照；队列溢出、write-stall、final、关闭时不受窗口限制而产生终态快照。

- [x] **步骤 2：运行测试验证失败**

运行：`cmake --build /tmp/tte-mrcp-issue37 --target test_funasr_ws_transport && ctest --test-dir /tmp/tte-mrcp-issue37 -R '^asr_websocket_funasr_ws_transport$' --output-on-failure`

预期：失败，原因是节流状态或终态判定尚未实现。

- [x] **步骤 3：实现最小节流和日志集成**

实现固定窗口的 `funasr_transport_metrics_should_log` 判定，使用 generation 与 monotonic timestamp。`funasr_control_handle_event`/metrics 消费路径调用它，并输出单条不含 payload/text 的日志：session/channel、generation、RX/TX bytes/frames、RX/TX latest-gap、ring current/high-water、overrun、first-send、write-wait、completion reason。保留所有 MRCP 消息与 completion 分支。

- [x] **步骤 4：运行测试验证通过**

运行步骤 2 的命令。

预期：节流、终态和既有 transport 测试均通过。

- [x] **步骤 5：重构并回归**

统一日志字段命名为 `audio_rx_*` 与 `audio_tx_*`，并检查生产路径没有逐帧 `APT_PRIO_INFO`/payload/hex 输出。

- [x] **步骤 6：提交**

```bash
git add plugins/asr-websocket/src/funasr_ws_transport.h plugins/asr-websocket/src/funasr_ws_transport.c plugins/asr-websocket/src/asr_websocket_engine.c plugins/asr-websocket/tests/test_funasr_ws_transport.c docs/superpowers/plans/2026-08-10-issue-37-audio-observability.md
git commit -m "feat(asr): log correlated audio transport summaries" -m "Refs #37"
```

### 任务 3：记录 TTS SPEAK 调度线程并完成验证

**文件：**
- 修改：`plugins/tts-websocket/src/tts_websocket_engine.c`
- 创建：`plugins/tts-websocket/tests/test_tts_websocket_thread.c`
- 创建：`plugins/tts-websocket/src/tts_websocket_thread.[ch]`
- 修改：`plugins/tts-websocket/CMakeLists.txt`、`Makefile.am` 与 Visual Studio 工程文件
- 修改：`docs/superpowers/plans/2026-08-10-issue-37-audio-observability.md`

- [x] **步骤 1：编写失败的跨平台 API/日志验证**

在现有 TTS test harness 中增加一个最小断言，验证 SPEAK 入口日志格式含 `thread_id` 和 MRCP session/channel 格式；若 harness 无法构造 engine channel，则增加可独立编译的 helper 测试，禁止调用平台特有 pthread/Windows API。

- [x] **步骤 2：运行测试验证失败**

运行：`cmake --build /tmp/tte-mrcp-issue37 --target test_tts_websocket_ws && ctest --test-dir /tmp/tte-mrcp-issue37 -R '^tts_websocket_ws$' --output-on-failure`

预期：失败，原因是线程标识尚未进入 SPEAK 日志。

- [x] **步骤 3：实现最小 TTS 日志改动**

在 `tts_websocket_channel_speak` 入口使用 APR 的线程标识 API 取得当前线程号，并写入既有 `APT_SIDRES` 或 `LOG_WITH_SID` 关联日志。不得记录 `request->body`、不得引入 pthread/Win32 分支，也不得改变 response 或 streaming 路径。

- [x] **步骤 4：运行测试验证通过**

运行步骤 2 的命令；若该独立目标未注册，运行现有 TTS parser/WebSocket 单测并以编译检查记录该 harness 限制。

预期：目标验证通过，或明确记录不影响代码编译的既有测试目标限制。

- [x] **步骤 5：重构并回归**

确认格式化符与 APR 线程类型匹配；运行 ASR transport、TTS WebSocket/HTTP parser 测试及 `git diff --check`。

- [x] **步骤 6：提交**

```bash
git add plugins/tts-websocket/src/tts_websocket_engine.c plugins/tts-websocket/tests/test_tts_websocket_ws.c docs/superpowers/plans/2026-08-10-issue-37-audio-observability.md
git commit -m "feat(tts): include scheduler thread in speak logs" -m "Refs #37"
```

### 任务 4：最终验证与图谱证据

**文件：**
- 修改：`docs/superpowers/plans/2026-08-10-issue-37-audio-observability.md`

- [x] **步骤 1：运行最终验证**

运行：`git diff --check`、`find tools -type f -name '*.sh' -print0 | xargs -0 bash -n`、目标 ASR/TTS 单测、`cmake -S . -B /tmp/tte-mrcp-issue37 -DCMAKE_POLICY_VERSION_MINIMUM=3.5`。

预期：变更卫生与可执行目标通过；全量构建若命中已知 MPF 宏签名基线，则记录为独立阻断，不归因于本 Issue。

- [x] **步骤 2：重新索引并查询关键链路**

运行 codebase-memory `index_repository`，查询 `funasr_stream_write → funasr_transport_enqueue_pcm` 和 `tts_websocket_channel_speak`，将结果与未验证的平台状态记录在本计划的执行结果中。

- [x] **步骤 3：提交最终验证记录**

```bash
git add docs/superpowers/plans/2026-08-10-issue-37-audio-observability.md
git commit -m "test: record issue 37 verification" -m "Refs #37"
```

## 执行结果（2026-08-10）

- macOS：`tools/dev/setup_macos.sh` 安装 Sofia-SIP 后，CMake 配置成功；`tts_websocket.so`、ASR transport 及 TTS 测试目标均构建成功。
- 测试：`asr_websocket_funasr_ws_transport`、`tts_websocket_http_parse`、`tts_websocket_ws`、`tts_websocket_thread` 共 4/4 通过。
- 构建卫生：`./configure --help`、全部 `tools/**/*.sh` 的 `bash -n` 和 `git diff --check` 通过。
- 图谱：`tte-mrcp-issue-37` 已重新索引（5,903 nodes / 34,434 edges）；`funasr_transport_enqueue_pcm` 仍受 transport 单测覆盖，`tts_websocket_channel_speak` 已直接调用 `tts_websocket_thread_id_current`。
- Windows、Linux：未验证。本次只同步了 Visual Studio 的 TTS 源文件登记；未执行 MSBuild/DLL 加载或 Linux 插件加载/冒烟。

### 任务 5：增加逐包中文 DEBUG 诊断日志

**文件：**
- 创建：`plugins/asr-websocket/tests/test_asr_frame_debug_logs.cmake`
- 修改：`plugins/asr-websocket/CMakeLists.txt`
- 修改：`plugins/asr-websocket/src/asr_websocket_engine.c:767-768`
- 修改：`plugins/asr-websocket/src/funasr_ws_transport.c:1018-1025`

- [ ] **步骤 1：编写失败的日志契约测试**

创建 `test_asr_frame_debug_logs.cmake`，通过 `file(READ ...)` 读取两个源码文件，并断言：媒体流入口存在 `APT_PRIO_DEBUG` 的中文“接收客户端媒体流音频数据包”日志且使用 `frame->codec_frame.size`；socket 写入函数保存 `apr_socket_send()` 的返回状态，并只在 `APR_SUCCESS` 且 `*size != 0` 时输出中文“发送 ASR WebSocket 网络数据包”日志。将 CMake 测试注册为：

```cmake
add_test(NAME asr_websocket_frame_debug_logs
    COMMAND ${CMAKE_COMMAND}
        -DASR_WEBSOCKET_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_asr_frame_debug_logs.cmake)
```

- [ ] **步骤 2：运行测试验证失败**

运行：

```bash
cmake -S . -B /tmp/tte-mrcp-issue37-debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5
ctest --test-dir /tmp/tte-mrcp-issue37-debug -R '^asr_websocket_frame_debug_logs$' --output-on-failure
```

预期：失败，提示客户端媒体流或 ASR WebSocket 逐包 DEBUG 日志不存在。

- [ ] **步骤 3：添加最小日志实现**

在 `funasr_stream_write` 中、`size = frame->codec_frame.size;` 后添加：

```c
LOG_WITH_SID(channel, APT_PRIO_DEBUG,
    "接收客户端媒体流音频数据包，大小=%" APR_SIZE_T_FMT " 字节",
    frame->codec_frame.size);
```

在 `funasr_ws_transport.c` 中包含 `apt_log.h`，将 `funasr_default_io_write` 改为保存 `apr_socket_send()` 的返回值，并仅在成功且实际写入非零时添加：

```c
apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,
    "asr_websocket: 发送 ASR WebSocket 网络数据包，大小=%" APR_SIZE_T_FMT " 字节",
    *size);
```

保留 `apr_socket_send()` 返回值、`*size` short-write 语义和所有现有错误路径。

- [ ] **步骤 4：运行日志契约与 transport 回归测试**

运行：

```bash
cmake --build /tmp/tte-mrcp-issue37-debug --target test_funasr_ws_transport
ctest --test-dir /tmp/tte-mrcp-issue37-debug -R '^(asr_websocket_frame_debug_logs|asr_websocket_funasr_ws_transport)$' --output-on-failure
```

预期：两个测试均通过；transport 测试继续覆盖握手、短写和音频发送路径。

- [ ] **步骤 5：完成检查并提交**

运行：

```bash
git diff --check
git add plugins/asr-websocket/src/asr_websocket_engine.c \
        plugins/asr-websocket/src/funasr_ws_transport.c \
        plugins/asr-websocket/CMakeLists.txt \
        plugins/asr-websocket/tests/test_asr_frame_debug_logs.cmake \
        docs/superpowers/plans/2026-08-10-issue-37-audio-observability.md
git commit -m "fix(asr): add per-packet debug logging" -m "Refs #37"
```

预期：仅提交逐包 DEBUG 日志、其跨平台 CMake 契约测试和计划更新。
