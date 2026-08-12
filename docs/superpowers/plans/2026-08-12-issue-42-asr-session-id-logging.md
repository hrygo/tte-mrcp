# Issue #42 ASR 逐包 DEBUG 日志会话标识实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 `subagent-driven-development`（推荐）或 `executing-plans` 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 使 ASR 客户端媒体包与 WebSocket 实际网络写入的逐包 DEBUG 日志采用相同的 `[session_id=<id>]` 前缀。

**架构：** `funasr_channel_recognize` 已把当前 session ID 作为 `format.call_id` 传入 generation 开始边界；transport 将此值复制到自有 `call_id` 后同步给默认 socket I/O。写入日志仅使用这个稳定副本，不访问 channel，也不改变写入语义。

**技术栈：** C、APR/UniMRCP、CMake 源码契约测试、codebase-memory。

---

## 文件职责

| 文件 | 职责 |
| --- | --- |
| `plugins/asr-websocket/src/funasr_ws_transport.c` | 在 generation 边界同步 ID，并输出具有关联前缀的实际 socket 写入日志。 |
| `plugins/asr-websocket/tests/test_asr_frame_debug_logs.cmake` | 验证逐包日志边界、格式与不变条件。 |

### 任务 1：以源码契约驱动 session ID 传递与日志格式

**文件：**
- 修改：`plugins/asr-websocket/tests/test_asr_frame_debug_logs.cmake`
- 修改：`plugins/asr-websocket/src/funasr_ws_transport.c`

- [x] **步骤 1：编写失败的源码契约测试（RED）**

在 `test_asr_frame_debug_logs.cmake` 增加以下断言：

```cmake
string(FIND "${transport_source}"
    "asr_websocket: [session_id=%s] 发送 ASR WebSocket 网络数据包"
    socket_session_log_start)
if(socket_session_log_start EQUAL -1)
    message(FATAL_ERROR "网络写入日志必须使用 session_id 前缀")
endif()
```

同时断言 `funasr_transport_begin_generation` 在复制 `format->call_id` 到 transport 自有 `call_id` 后，以 `transport->default_io.session_id = transport->call_id;` 同步默认 I/O。保留既有关于 `APR_SUCCESS && *size != 0`、实际 `*size`、媒体日志位置、中文文本和不记录地址/payload 的断言。

- [x] **步骤 2：运行测试确认失败（RED）**

运行：

```bash
cmake -S . -B /tmp/tte-mrcp-issue42 -DCMAKE_POLICY_VERSION_MINIMUM=3.5
ctest --test-dir /tmp/tte-mrcp-issue42 -R '^asr_websocket_frame_debug_logs$' --output-on-failure
```

预期：失败，提示网络写入日志缺少 session ID 前缀或 transport session ID 传递尚不存在。

- [x] **步骤 3：实现最小 session ID 传递与日志改动（GREEN）**

在 `funasr_transport_begin_generation` 中、将 `format->call_id` 复制到 `transport->call_id` 后添加：

```c
transport->default_io.session_id = transport->call_id;
```

然后将 `funasr_default_io_write` 的日志改为：

```c
apt_log(APT_LOG_MARK, APT_PRIO_DEBUG,
    "asr_websocket: [session_id=%s] 发送 ASR WebSocket 网络数据包，大小=%" APR_SIZE_T_FMT " 字节",
    io->session_id ? io->session_id : "N/A",
    *size);
```

不得移动 `apr_socket_send()`、条件 `status == APR_SUCCESS && *size != 0` 或返回语句。

- [x] **步骤 4：运行测试确认通过（GREEN）**

运行步骤 2 的两条命令。

预期：`asr_websocket_frame_debug_logs` 通过。

- [x] **步骤 5：重构并回归（REFACTOR）**

检查 ID 回退值仅存在于 transport 创建或日志读取边界；保留最小字段和赋值，不新增 channel 指针、锁、I/O 或协议改动。运行：

```bash
git diff --check
```

预期：退出码 0。

- [x] **步骤 6：提交**

```bash
git add plugins/asr-websocket/tests/test_asr_frame_debug_logs.cmake plugins/asr-websocket/src/funasr_ws_transport.h plugins/asr-websocket/src/funasr_ws_transport.c plugins/asr-websocket/src/asr_websocket_engine.c docs/superpowers/plans/2026-08-12-issue-42-asr-session-id-logging.md
git commit -m "fix(asr): correlate packet logs with session id" -m "Refs #42"
```

### 任务 2：最终验证与图谱证据

**文件：**
- 修改：`docs/superpowers/plans/2026-08-12-issue-42-asr-session-id-logging.md`

- [x] **步骤 1：运行范围验证**

运行：

```bash
ctest --test-dir /tmp/tte-mrcp-issue42 -R '^asr_websocket_frame_debug_logs$' --output-on-failure
git diff --check
```

预期：两条命令退出码均为 0。

- [x] **步骤 2：重新索引并验证关键调用链**

重新索引仓库，并检查：`funasr_open_channel_on_task` 调用 `funasr_transport_create`，`funasr_default_io_write` 保持在默认 socket I/O 写入边界。

- [x] **步骤 3：记录结果并提交**

在本计划附加执行结果，列出 CMake 测试、`git diff --check`、图谱查询结果，以及 Windows/Linux 为“未验证”。然后运行：

```bash
git add docs/superpowers/plans/2026-08-12-issue-42-asr-session-id-logging.md
git commit -m "test(asr): record issue 42 verification" -m "Refs #42"
```

## 执行结果（2026-08-12）

- RED：扩展后的 `asr_websocket_frame_debug_logs` 在旧实现上失败，原因是网络写入日志缺少 session ID 前缀。
- macOS：加载 `tools/dev/env-macos.sh` 后，`cmake -S . -B /tmp/tte-mrcp-issue42-macos -DCMAKE_POLICY_VERSION_MINIMUM=3.5`、`cmake --build /tmp/tte-mrcp-issue42-macos --target test_funasr_ws_transport asr_websocket -j2` 均通过。
- 测试：`asr_websocket_funasr_ws_transport` 与 `asr_websocket_frame_debug_logs` 均通过（2/2）。
- 卫生：`git diff --check` 与所有 `tools/**/*.sh` 的 `bash -n` 通过。
- 图谱：`tte-mrcp-issue-42` 已重新索引（5,912 nodes / 34,470 edges）；`funasr_transport_begin_generation` 由 `funasr_channel_recognize` 调用，默认 socket 写入边界为 `funasr_default_io_write`。
- Windows、Linux：未验证；本次未执行 MSBuild/DLL 加载或 Linux 动态插件加载/服务端冒烟。
