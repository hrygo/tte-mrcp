# Issue #64 ASR 双结果超时实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 `executing-plans` 逐任务实现本计划。步骤使用复选框（`- [ ]`）语法跟踪进度。

**目标：** 每个 ASR `RECOGNIZE` generation 在首帧后 20 秒、或末次非静音语音后 10 秒仍无最终结果时，返回空 body 的 `NO-INPUT-TIMEOUT`。

**架构：** timeout 状态仅存在于 transport worker 的一个 generation 局部作用域；首帧 deadline 不刷新，末次语音 deadline 仅由完整写出的非静音 PCM WebSocket frame 刷新。worker 投递可区分 failure，控制层以现有链路生成一次 MRCP completion，engine task 记录 WARN。

**技术栈：** C99、APR、UniMRCP、现有 fake I/O/fake clock C 测试、CMake、Autotools。

---

## 文件职责

- 创建 `plugins/asr-websocket/src/funasr_timeout_config.[ch]` 和 `tests/test_funasr_timeout_config.c`：无 MRCP 依赖的毫秒配置解析及其单测。
- 修改 `funasr_ws_transport.[ch]`、`test_funasr_ws_transport.c`：双 deadline、PCM 声音活动、generation 重置。
- 修改 `asr_websocket_engine.c`、`funasr_control.c`、`test_funasr_control.c`：XML 注入、WARN、MRCP cause。
- 修改 `plugins/asr-websocket/{CMakeLists.txt,Makefile.am}`、`conf/unimrcpserver.xml`、`plugins/asr-websocket/README.md`：构建与发布契约。

### 任务 1：先以纯函数解析配置值

**文件：**

- 创建：`plugins/asr-websocket/src/funasr_timeout_config.h`
- 创建：`plugins/asr-websocket/src/funasr_timeout_config.c`
- 创建：`plugins/asr-websocket/tests/test_funasr_timeout_config.c`
- 修改：`plugins/asr-websocket/CMakeLists.txt`
- 修改：`plugins/asr-websocket/Makefile.am`

- [ ] **步骤 1：编写失败的单测**

```c
CHECK_EQ("default", FUNASR_FIRST_AUDIO_RESULT_TIMEOUT_US,
    funasr_timeout_ms_parse(NULL, FUNASR_FIRST_AUDIO_RESULT_TIMEOUT_US, &status));
CHECK_EQ("valid", 7000000LL,
    funasr_timeout_ms_parse("7000", FUNASR_LAST_SPEECH_RESULT_TIMEOUT_US, &status));
CHECK_EQ("zero fallback", FUNASR_LAST_SPEECH_RESULT_TIMEOUT_US,
    funasr_timeout_ms_parse("0", FUNASR_LAST_SPEECH_RESULT_TIMEOUT_US, &status));
CHECK_TRUE("invalid status", status == FUNASR_TIMEOUT_VALUE_INVALID);
```

- [ ] **步骤 2：运行并确认红灯**

运行：`cmake --build /tmp/tte-mrcp-issue64 -j2 --target test_funasr_timeout_config`

预期：失败，提示缺少 `funasr_timeout_config.h` 或 `funasr_timeout_ms_parse`。

- [ ] **步骤 3：实现最小解析器并登记双构建系统**

```c
apr_interval_time_t funasr_timeout_ms_parse(
    const char *value, apr_interval_time_t fallback,
    funasr_timeout_value_e *status)
{
    char *end = NULL;
    long milliseconds;
    if (!value || !*value) { *status = FUNASR_TIMEOUT_VALUE_DEFAULT; return fallback; }
    errno = 0; milliseconds = strtol(value, &end, 10);
    if (errno || end == value || *end || milliseconds <= 0 ||
        (apr_int64_t)milliseconds > APR_INT64_MAX / 1000) {
        *status = FUNASR_TIMEOUT_VALUE_INVALID; return fallback;
    }
    *status = FUNASR_TIMEOUT_VALUE_CONFIGURED;
    return (apr_interval_time_t)milliseconds * 1000;
}
```

为 CMake 和 `Makefile.am` 同时登记 plugin source 与 `test_funasr_timeout_config`。

- [ ] **步骤 4：运行并确认绿灯**

运行：`cmake --build /tmp/tte-mrcp-issue64 -j2 --target test_funasr_timeout_config && ctest --test-dir /tmp/tte-mrcp-issue64 -R '^asr_websocket_funasr_timeout_config$' --output-on-failure`

预期：1/1 PASS。

- [ ] **步骤 5：提交**

```bash
git add plugins/asr-websocket/src/funasr_timeout_config.[ch] \
  plugins/asr-websocket/tests/test_funasr_timeout_config.c \
  plugins/asr-websocket/CMakeLists.txt plugins/asr-websocket/Makefile.am
git commit -m "feat(asr): parse result timeout configuration" -m "Refs #64"
```

### 任务 2：以 fake clock 固定双 deadline 与静音语义

**文件：**

- 修改：`plugins/asr-websocket/src/funasr_ws_transport.h`
- 修改：`plugins/asr-websocket/src/funasr_ws_transport.c`
- 修改：`plugins/asr-websocket/tests/test_funasr_ws_transport.c`

- [ ] **步骤 1：把现有单 timeout 测试拆成两个失败测试**

```c
config.first_audio_result_timeout_us = 20000;
config.last_speech_result_timeout_us = 90000;
/* 每 9 ms 入队/发出一个 voiced frame；仍应在首帧后 20 ms 失败。 */
CHECK_FAILURE("absolute timeout", FUNASR_FAILURE_FIRST_AUDIO_RESULT_TIMEOUT);

config.first_audio_result_timeout_us = 90000;
config.last_speech_result_timeout_us = 10000;
/* 先发 voiced PCM，随后只发全零 PCM；全零帧不得刷新 10 ms deadline。 */
CHECK_FAILURE("last speech timeout", FUNASR_FAILURE_NO_RESULT_TIMEOUT);
```

每个测试都要在 failure 后启动下一 generation，并断言新 generation 仅在自身 deadline 后失败。

- [ ] **步骤 2：运行并确认红灯**

运行：`cmake --build /tmp/tte-mrcp-issue64 -j2 --target test_funasr_ws_transport && /tmp/tte-mrcp-issue64/plugins/asr-websocket/test_funasr_ws_transport`

预期：失败，新的 config 字段和 `FUNASR_FAILURE_FIRST_AUDIO_RESULT_TIMEOUT` 不存在。

- [ ] **步骤 3：实现 worker 局部状态**

```c
apr_int64_t first_audio_sent_us = 0;
apr_int64_t last_speech_sent_us = 0;
/* tx_audio_has_speech 来自刚 dequeue 的 16-bit little-endian PCM peak。 */
if (tx_audio && tx_offset == tx_size) {
    if (first_audio_sent_us == 0) first_audio_sent_us = last_write_progress_us;
    if (tx_audio_has_speech) last_speech_sent_us = last_write_progress_us;
}
if (first_audio_sent_us != 0 &&
    now_us - first_audio_sent_us >= transport->config.first_audio_result_timeout_us) {
    terminal_failure = FUNASR_FAILURE_FIRST_AUDIO_RESULT_TIMEOUT;
    generation_failed = TRUE;
} else if (last_speech_sent_us != 0 &&
           now_us - last_speech_sent_us >= transport->config.last_speech_result_timeout_us) {
    terminal_failure = FUNASR_FAILURE_NO_RESULT_TIMEOUT;
    generation_failed = TRUE;
}
```

新增默认值 `20,000,000 us`、`10,000,000 us` 和峰值阈值 `256`；扫描 PCM 时拒绝越界读取并安全忽略最后一个奇数字节。保留 `last_media_us` 原有自然 EOS 逻辑，不能让它刷新结果 deadline。

- [ ] **步骤 4：运行并确认绿灯**

运行：`cmake --build /tmp/tte-mrcp-issue64 -j2 --target test_funasr_ws_transport && ctest --test-dir /tmp/tte-mrcp-issue64 -R '^asr_websocket_funasr_ws_transport$' --output-on-failure`

预期：1/1 PASS；连续 voiced frame 不延长 20 秒、全零 PCM 不延长 10 秒、下一 generation 独立。

- [ ] **步骤 5：提交**

```bash
git add plugins/asr-websocket/src/funasr_ws_transport.c \
  plugins/asr-websocket/src/funasr_ws_transport.h \
  plugins/asr-websocket/tests/test_funasr_ws_transport.c
git commit -m "feat(asr): enforce per-generation result deadlines" -m "Refs #64"
```

### 任务 3：以现有 MRCP event 构造器返回空结果并记录 WARN

**文件：**

- 修改：`plugins/asr-websocket/src/asr_websocket_engine.c`
- 修改：`plugins/asr-websocket/src/funasr_control.c`
- 修改：`plugins/asr-websocket/tests/test_funasr_control.c`

- [ ] **步骤 1：扩展失败的 control 单测**

```c
event = event_make(3, FUNASR_EVENT_TRANSPORT_FAILED);
event.failure = FUNASR_FAILURE_FIRST_AUDIO_RESULT_TIMEOUT;
funasr_control_handle_event(&control, &event, &fake_vtable, &sink);
CHECK_TRUE("first deadline cause",
    sink.last_cause == RECOGNIZER_COMPLETION_CAUSE_NO_INPUT_TIMEOUT);
CHECK_TRUE("first deadline has no text", sink.last_text == NULL);
```

以新 generation 重复测试 `FUNASR_FAILURE_NO_RESULT_TIMEOUT`，并保留重复 terminal 不再完成的断言。

- [ ] **步骤 2：运行并确认红灯**

运行：`cmake --build /tmp/tte-mrcp-issue64 -j2 --target test_funasr_control && /tmp/tte-mrcp-issue64/plugins/asr-websocket/test_funasr_control`

预期：失败，首帧 timeout failure 未定义。

- [ ] **步骤 3：实现 XML 注入、cause 映射和告警**

```c
first = funasr_timeout_ms_parse(mrcp_engine_param_get(engine,
    "first-audio-result-timeout-ms"), FUNASR_FIRST_AUDIO_RESULT_TIMEOUT_US, &status);
last = funasr_timeout_ms_parse(mrcp_engine_param_get(engine,
    "last-speech-result-timeout-ms"), FUNASR_LAST_SPEECH_RESULT_TIMEOUT_US, &status);
config.first_audio_result_timeout_us = channel->engine->first_audio_result_timeout_us;
config.last_speech_result_timeout_us = channel->engine->last_speech_result_timeout_us;
```

engine 在非法参数时 WARN 并回退默认值；两个 failure 均映射为 `RECOGNIZER_COMPLETION_CAUSE_NO_INPUT_TIMEOUT`。在 `funasr_transport_event_on_task` 的 failure 被 control 消费前记录两条 WARN：10 秒路径包含精确文本 `asr not response resut`；20 秒路径记录 `asr first audio result timeout`。日志仅含 session、generation、timeout_ms。不得改动 `funasr_recognition_complete`，使其维持既有的 non-success 空 body 序列化。

- [ ] **步骤 4：运行并确认绿灯**

运行：`cmake --build /tmp/tte-mrcp-issue64 -j2 --target asr_websocket test_funasr_control && ctest --test-dir /tmp/tte-mrcp-issue64 -R '^asr_websocket_funasr_control$' --output-on-failure`

预期：1/1 PASS；plugin 链接成功。

- [ ] **步骤 5：提交**

```bash
git add plugins/asr-websocket/src/asr_websocket_engine.c \
  plugins/asr-websocket/src/funasr_control.c \
  plugins/asr-websocket/tests/test_funasr_control.c
git commit -m "feat(asr): complete timeout sessions with no input" -m "Refs #64"
```

### 任务 4：发布配置与验证

**文件：**

- 修改：`conf/unimrcpserver.xml`
- 修改：`plugins/asr-websocket/README.md`

- [ ] **步骤 1：添加生产默认配置并更新 README**

```xml
<param name="first-audio-result-timeout-ms" value="20000"/>
<param name="last-speech-result-timeout-ms" value="10000"/>
```

README 写明毫秒单位、静音 PCM 不刷新 10 秒、每个 generation 独立，以及两个超时都以 `NO-INPUT-TIMEOUT` 和零长度 body 完成。

- [ ] **步骤 2：运行范围内验证**

```bash
xmllint --noout conf/unimrcpserver.xml
cmake -S . -B /tmp/tte-mrcp-issue64 -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build /tmp/tte-mrcp-issue64 -j2 --target asr_websocket test_funasr_timeout_config test_funasr_ws_transport test_funasr_control
ctest --test-dir /tmp/tte-mrcp-issue64 -R '^asr_websocket_' --output-on-failure
git diff --check
```

预期：XML 与 CMake 成功，所有 ASR WebSocket CTest 通过，diff 检查无输出。若完整 Autotools 仍命中已知 `libs/mpf` 宏签名基线阻断，单独记录且不得归因于 Issue #64。

- [ ] **步骤 3：提交并复核调用链**

```bash
git add conf/unimrcpserver.xml plugins/asr-websocket/README.md \
  docs/superpowers/plans/2026-08-27-issue-64-asr-dual-result-timeouts.md
git commit -m "docs(asr): configure dual result timeouts" -m "Closes #64"
git diff origin/main...HEAD --check
```

重新索引并查询 `funasr_transport_worker → funasr_control_handle_event → funasr_recognition_complete`，确认两类 failure 只沿该链路产生一次 completion。
