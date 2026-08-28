# Issue #68 ASR WebSocket 异常日志实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 在不改变识别语义的前提下，让 ASR WebSocket 握手、音频发送和服务端 JSON 结果具备可关联的诊断日志。

**架构：** transport 层提供统一的错误日志辅助函数，握手与发送路径在拥有 APR 状态的地方记录失败。WebSocket 文本帧在 JSON 解析前以单行、最大 2KB 的形式记录，因此服务端非零 `code` 不再无迹可寻。

**技术栈：** C、APR、UniMRCP APT logging、CMake/CTest。

---

### 任务 1：日志约束回归测试

**文件：**
- 创建：`plugins/asr-websocket/tests/test_asr_error_logs.cmake`
- 修改：`plugins/asr-websocket/CMakeLists.txt:120-124`
- 测试：`plugins/asr-websocket/tests/test_asr_error_logs.cmake`

- [ ] **步骤 1：编写失败的测试**

创建 CMake 脚本，读取 `src/funasr_ws_transport.c`，断言存在：

```cmake
"ASR WebSocket handshake failed"
"ASR WebSocket audio write failed"
"ASR WebSocket result JSON"
"APT_PRIO_ERROR"
"FUNASR_LOG_JSON_MAX_BYTES"
```

并在 `CMakeLists.txt` 中加入：

```cmake
add_test(NAME asr_websocket_error_logs
    COMMAND ${CMAKE_COMMAND}
        -DASR_WEBSOCKET_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_asr_error_logs.cmake)
```

- [ ] **步骤 2：运行测试验证失败**

运行：

```sh
cmake -DASR_WEBSOCKET_SOURCE_DIR=plugins/asr-websocket -P plugins/asr-websocket/tests/test_asr_error_logs.cmake
```

预期：失败并说明缺少握手、音频发送和 JSON 结果日志。

### 任务 2：最小化 transport 诊断日志实现

**文件：**
- 修改：`plugins/asr-websocket/src/funasr_ws_transport.c:1352-1640`
- 测试：`plugins/asr-websocket/tests/test_asr_error_logs.cmake`

- [ ] **步骤 1：实现统一错误日志与受限 JSON 日志**

实现两个内部辅助函数：

```c
static void funasr_transport_log_error(
    funasr_transport_t *transport,
    funasr_generation_t generation,
    const char *stage,
    apr_status_t status);

static void funasr_transport_log_result_json(
    funasr_transport_t *transport,
    funasr_generation_t generation,
    const unsigned char *json,
    apr_size_t json_size);
```

错误日志记录 session ID、generation、endpoint、阶段和 APR 状态；JSON 日志只复制
至多 `FUNASR_LOG_JSON_MAX_BYTES` 字节，并将控制字符替换为空格。

- [ ] **步骤 2：在握手、音频发送和文本结果分支接入日志**

在 `funasr_worker_handshake` 的非可重试 poll/write/read、无效 accept、无效 HTTP
响应和超时分支记录 `ERROR`；在 `funasr_worker_write_pending` 的不可重试发送失败
记录 `ERROR`，阶段为 `audio write`；在 `funasr_worker_handle_ws_event` 的文本帧
解析前调用 JSON 日志函数。

- [ ] **步骤 3：运行日志约束测试验证通过**

运行：

```sh
cmake -DASR_WEBSOCKET_SOURCE_DIR=plugins/asr-websocket -P plugins/asr-websocket/tests/test_asr_error_logs.cmake
```

预期：成功退出。

### 任务 3：编译与回归验证

**文件：**
- 修改：`plugins/asr-websocket/src/funasr_ws_transport.c`
- 测试：`plugins/asr-websocket/tests/test_funasr_ws_transport.c`

- [ ] **步骤 1：配置并构建定向测试**

运行：

```sh
cmake -S . -B /tmp/tte-mrcp-asr-log-test -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build /tmp/tte-mrcp-asr-log-test --target test_funasr_ws_transport
```

- [ ] **步骤 2：运行回归测试**

运行：

```sh
ctest --test-dir /tmp/tte-mrcp-asr-log-test -R 'asr_websocket_(funasr_ws_transport|error_logs|frame_debug_logs)' --output-on-failure
git diff --check
```

预期：全部通过。

- [ ] **步骤 3：重新索引并复核调用链**

运行 codebase-memory 的 `index_repository`，并检查 `funasr_worker_handshake`、
`funasr_worker_write_pending` 和 `funasr_worker_handle_ws_event` 的定义和调用关系。

- [ ] **步骤 4：提交 Issue 分支**

```sh
git add docs/superpowers/specs/2026-08-28-asr-websocket-error-observability-design.md \
  docs/superpowers/plans/2026-08-28-issue-68-asr-websocket-error-logs.md \
  plugins/asr-websocket/CMakeLists.txt \
  plugins/asr-websocket/src/funasr_ws_transport.c \
  plugins/asr-websocket/tests/test_asr_error_logs.cmake
git commit -m "fix(asr): log websocket transport failures" -m "Refs #68"
```
