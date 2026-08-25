# Issue #46：缺失采样率默认 8 kHz 实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 当 TTS `audio.start` 未提供 `sample_rate` 时，插件按 8 kHz PCM 直转 PCMU；显式 `24000` 仍执行 3:1 重采样。

**架构：** 在已有的 JSON 纯函数模块中增加三态读取 API（有效、缺失、无效），使 stream thread 只把真正缺失的字段按 8 kHz 处理。每个 SPEAK 在启动时将 `input_sample_rate` 初始化为 8000；每个 `audio.start` 缺失字段也会回落到 8000。已有 PCM 模块继续按 channel 的有效采样率选择 2-byte 或 6-byte 对齐及转换路径。

**技术栈：** C99、APR/UniMRCP、现有 `tts_websocket_json` 和 `tts_websocket_pcm` 单元测试、Autotools/CMake。

---

## 文件职责

| 文件 | 职责 |
| --- | --- |
| `plugins/tts-websocket/src/tts_websocket_json.h` | 声明可区分有效、缺失和无效字段的 JSON 数字读取 API。 |
| `plugins/tts-websocket/src/tts_websocket_json.c` | 实现严格三态解析和独立的采样率决策纯函数。 |
| `plugins/tts-websocket/tests/test_tts_websocket_json.c` | 验证显式 8/24 kHz、缺失字段回落、无效/溢出字段和 24 kHz → 缺失序列。 |
| `plugins/tts-websocket/src/tts_websocket_engine.c` | 初始化每条 SPEAK 的 8 kHz 默认值，并按三态结果处理 `audio.start`。 |
| `plugins/tts-websocket/tests/test_tts_websocket_pcm.c` | 维持 8 kHz 直通和 24 kHz 3:1 转换的字节数回归。 |

### 任务 1：为三态采样率解析写失败的 JSON 单元测试

**文件：**
- 修改：`plugins/tts-websocket/tests/test_tts_websocket_json.c`
- 修改：`plugins/tts-websocket/src/tts_websocket_json.h`

- [ ] **步骤 1：声明待测 API 并编写失败测试**

在头文件声明：

```c
int tts_websocket_json_get_uint_status(
    const char *json, size_t len, const char *key, unsigned int *value);
```

在 `test_tts_websocket_json.c` 添加：

```c
static int test_missing_sample_rate_uses_8khz_default(void)
{
    const char json[] = "{\"type\":\"audio.start\"}";
    unsigned int rate = 24000;

    return tts_websocket_json_get_uint_status(
        json, sizeof(json) - 1, "sample_rate", &rate) == 0 && rate == 24000;
}
```

并在 `main()` 中调用该测试，失败时打印 `test_missing_sample_rate_uses_8khz_default failed`。

- [ ] **步骤 2：运行测试，确认因缺少实现而失败**

运行：

```sh
cc -std=c99 -Wall -Wextra -Werror -Iplugins/tts-websocket/src \
  plugins/tts-websocket/tests/test_tts_websocket_json.c \
  plugins/tts-websocket/src/tts_websocket_json.c \
  -o /tmp/tte-mrcp-issue46-json && /tmp/tte-mrcp-issue46-json
```

预期：编译失败，提示 `tts_websocket_json_get_uint_status` 未定义；这是新行为尚未实现，而不是测试夹具错误。

### 任务 2：实现 JSON 三态解析并验证显式字段不变

**文件：**
- 修改：`plugins/tts-websocket/src/tts_websocket_json.c`
- 修改：`plugins/tts-websocket/tests/test_tts_websocket_json.c`

- [ ] **步骤 1：实现最小包装器**

实现 `tts_websocket_json_get_uint_status()`，返回：有效值为 `1`、完整 JSON 中字段缺失为 `0`、非数字/溢出/JSON 结构错误为 `-1`。缺失或无效时不得修改调用方传入的值。

```c
实现纯函数 `tts_websocket_sample_rate_resolve()`：缺失返回 8000；有效的 8000/24000 返回该值；无效或不支持的显式值保留前一个采样率。
```

- [ ] **步骤 2：补充显式值断言**

在同一测试文件中添加：显式 8000/24000、字符串/溢出值为无效，以及 24000 后的缺失字段回落到 8000。复用现有 JSON 测试的 `main()` 风格。

- [ ] **步骤 3：运行 JSON 测试，确认通过**

运行任务 1 的编译命令。

预期：退出码 0，现有 JSON 测试及新增缺失/显式采样率测试全部通过。

- [ ] **步骤 4：提交 JSON 测试与实现**

```sh
git add plugins/tts-websocket/src/tts_websocket_json.c \
  plugins/tts-websocket/src/tts_websocket_json.h \
  plugins/tts-websocket/tests/test_tts_websocket_json.c
git commit -m "fix(tts): default missing sample rate to 8khz" -m "Refs: #46"
```

### 任务 3：把 stream thread 的缺失字段路径接到 8 kHz 默认值

**文件：**
- 修改：`plugins/tts-websocket/src/tts_websocket_engine.c:audio.start handling and tts_websocket_start_streaming`
- 测试：`plugins/tts-websocket/tests/test_tts_websocket_json.c`
- 测试：`plugins/tts-websocket/tests/test_tts_websocket_pcm.c`

- [ ] **步骤 1：以测试确定 8 kHz 直通及 24 kHz 回归预期**

保留并运行 PCM 测试中的以下真实转换：

```c
produced = tts_websocket_pcm_to_ulaw(input, sizeof(input), 8000,
    output, sizeof(output));
/* 4 个 16-bit 输入样本必须产生 4 个 PCMU 字节。 */
if (produced != 4) return 0;

produced = tts_websocket_pcm_to_ulaw(input_24k, sizeof(input_24k), 24000,
    output, sizeof(output));
/* 6 个 16-bit 输入样本必须产生 2 个 PCMU 字节。 */
if (produced != 2) return 0;
```

- [ ] **步骤 2：运行 PCM 测试，确认当前纯转换基线通过**

运行：

```sh
cc -std=c99 -Wall -Wextra -Werror -Iplugins/tts-websocket/src \
  plugins/tts-websocket/tests/test_tts_websocket_pcm.c \
  plugins/tts-websocket/src/tts_websocket_pcm.c \
  -o /tmp/tte-mrcp-issue46-pcm && /tmp/tte-mrcp-issue46-pcm
```

预期：退出码 0，报告 `7/7 PCM streaming tests passed` 或添加断言后的对应总数。

- [ ] **步骤 3：改用 8 kHz 默认值并保留显式字段优先级**

在 `tts_websocket_start_streaming()` 中替换初始化：

```c
synth_channel->input_sample_rate = 8000;
synth_channel->input_sample_rate_warned = FALSE;
```

在 `audio.start` 处理处使用三态 API：缺失时设置 8000 并只记录一次告警；无效值仅记录告警并保持前一个采样率。

```c
int sample_rate_status = tts_websocket_json_get_uint_status(
    buffer, (size_t)len, "sample_rate", &sample_rate);
if (sample_rate_status == TTS_WEBSOCKET_JSON_UINT_MISSING) {
    synth_channel->input_sample_rate = 8000;
    if (!synth_channel->input_sample_rate_warned) {
        LOG_WITH_SID(synth_channel, APT_PRIO_WARNING,
            "[WS] sample_rate missing in audio.start, using 8000 Hz fallback");
        synth_channel->input_sample_rate_warned = TRUE;
    }
} else if (sample_rate_status == TTS_WEBSOCKET_JSON_UINT_FOUND &&
           (sample_rate == 8000 || sample_rate == 24000)) {
    synth_channel->input_sample_rate = sample_rate;
} else if (sample_rate_status == TTS_WEBSOCKET_JSON_UINT_FOUND) {
    LOG_WITH_SID(synth_channel, APT_PRIO_WARNING,
        "[WS] Unsupported sample rate %u, keeping previous %u",
        sample_rate, synth_channel->input_sample_rate);
} else {
    LOG_WITH_SID(synth_channel, APT_PRIO_WARNING,
        "[WS] Invalid sample_rate in audio.start, keeping previous %u",
        synth_channel->input_sample_rate);
}
```

Do not alter `tts_websocket_pcm_alignment()` or `tts_websocket_pcm_to_ulaw()`; the existing `input_sample_rate` selects their 2-byte/6-byte behavior.

- [ ] **步骤 4：运行受影响单元测试，确认通过**

运行任务 2 的 JSON 命令和任务 3 的 PCM 命令。

预期：两条命令均退出码 0；8 kHz 输入不缩短，24 kHz 输入仍为 3:1。

- [ ] **步骤 5：提交流处理改动**

```sh
git add plugins/tts-websocket/src/tts_websocket_engine.c \
  plugins/tts-websocket/tests/test_tts_websocket_pcm.c
git commit -m "fix(tts): bypass resampling when sample rate is absent" -m "Closes #46"
```

### 任务 4：构建配置与变更卫生验证

**文件：**
- 不修改文件；验证任务 1–3 的变更。

- [ ] **步骤 1：运行完整插件 CMake 测试目标**

运行：

```sh
cmake -S . -B /tmp/tte-mrcp-issue46-cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build /tmp/tte-mrcp-issue46-cmake --target test_tts_websocket_json test_tts_websocket_pcm
ctest --test-dir /tmp/tte-mrcp-issue46-cmake --output-on-failure -R 'tts_websocket_(json|pcm)'
```

预期：CMake 配置与两个测试目标成功；CTest 报告 JSON 和 PCM 测试均通过。若现有项目级依赖阻断配置，记录精确错误，不能归因于本次采样率改动。

- [ ] **步骤 2：运行静态卫生检查并复核图谱**

运行：

```sh
git diff origin/main...HEAD --check
git status --short
```

然后重新索引仓库，并查询 `tts_websocket_start_streaming`、`tts_websocket_stream_thread` 与 `tts_websocket_json_get_uint_status`，确认新默认值只影响 TTS 插件输入采样率决策。

预期：diff check 无输出；状态仅包含本 Issue 的预期变更；图谱节点和调用链可解析。

- [ ] **步骤 3：提交验证记录（仅在需要新增记录时）**

如验证不需要新增受跟踪文件，不创建空提交。最终 PR 描述必须记录：本机 JSON/PCM 结果、CMake/CTest 结果、macOS 已验证与 Windows/Linux 运行时未验证状态。
