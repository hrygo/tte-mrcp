# Issue #46 8 kHz PCM 审查修复实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 `executing-plans` 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 让 `audio.start` 只读取真实 JSON `sample_rate` 键，并确保 8 kHz PCM 的完整句尾采样不会因 24 kHz 对齐规则丢失。

**架构：** 新增纯 JSON 数字字段解析模块和采样率到 PCM 对齐大小的纯函数。stream thread 使用这两个模块选择 2/6 字节边界；8 kHz `audio.done` 在清空 carry 前编码完整偶数字节，24 kHz 保持只处理完整 3-sample 组。

**技术栈：** C99、APR/UniMRCP、CMake/CTest、Autotools 源码清单。

---

## 文件职责

| 文件 | 职责 |
| --- | --- |
| `plugins/tts-websocket/src/tts_websocket_json.c/.h` | 以长度约束扫描顶层 JSON 数字字段，跳过字符串和转义内容。 |
| `plugins/tts-websocket/src/tts_websocket_pcm.c/.h` | 提供 8000/24000 Hz 对应的 PCM 对齐字节数。 |
| `plugins/tts-websocket/src/tts_websocket_engine.c` | 使用结构化采样率结果和采样率特定对齐，句尾保留 8 kHz 完整采样。 |
| `plugins/tts-websocket/tests/test_tts_websocket_json.c` | 回归 JSON 字段遮蔽、转义和无效值。 |
| `plugins/tts-websocket/tests/test_tts_websocket_pcm.c` | 回归 8 kHz 的 2/4 字节尾块完整保留。 |
| `plugins/tts-websocket/CMakeLists.txt`、`plugins/tts-websocket/Makefile.am` | 编译新源文件并注册 CMake 单测。 |

### 任务 1：以失败单测定义 JSON 与 PCM 对齐契约

**文件：**
- 创建：`plugins/tts-websocket/tests/test_tts_websocket_json.c`
- 修改：`plugins/tts-websocket/tests/test_tts_websocket_pcm.c`
- 修改：`plugins/tts-websocket/CMakeLists.txt`

- [x] **步骤 1：编写失败测试（RED）**

在 JSON 测试中声明 `tts_websocket_json_get_uint()` 并断言：

```c
const char json[] = "{\\\"type\\\":\\\"audio.start\\\",\\\"sentence_text\\\":\\\"sample_rate:24000\\\",\\\"sample_rate\\\":8000}";
assert(tts_websocket_json_get_uint(json, sizeof(json) - 1,
    "sample_rate", &rate) && rate == 8000);
```

再覆盖带转义引号的字符串、缺少字段和非数字字段。PCM 测试断言 `tts_websocket_pcm_alignment(8000) == 2`、`tts_websocket_pcm_alignment(24000) == 6`，并用 4 字节 8 kHz PCM 经 `accumulate(..., alignment=2)` 与 `pcm_to_ulaw()` 得到两个输出字节。

- [x] **步骤 2：运行测试确认失败（RED）**

运行：

```bash
cmake -S . -B /tmp/tte-mrcp-issue46 -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build /tmp/tte-mrcp-issue46 --target test_tts_websocket_json test_tts_websocket_pcm -j2
```

预期：JSON 和 PCM 对齐符号尚未实现而编译或链接失败。

- [x] **步骤 3：实现最小纯函数（GREEN）**

创建 `tts_websocket_json_get_uint()`，仅接受对象键位置的精确字段名、冒号后的十进制无符号整数，并跳过 JSON 字符串及其反斜杠转义。向 PCM 模块添加：

```c
size_t tts_websocket_pcm_alignment(unsigned int input_rate)
{
    return input_rate == 8000 ? 2 : input_rate == 24000 ? 6 : 0;
}
```

将模块和测试目标加入 CMake；将 JSON 源文件加入 Autotools 插件源清单。

- [x] **步骤 4：运行测试确认通过（GREEN）**

运行：

```bash
cmake --build /tmp/tte-mrcp-issue46 --target test_tts_websocket_json test_tts_websocket_pcm -j2
ctest --test-dir /tmp/tte-mrcp-issue46 -R '^tts_websocket_(json|pcm)$' --output-on-failure
```

预期：两个目标编译成功，两个测试通过。

### 任务 2：将 stream thread 接到两个纯函数

**文件：**
- 修改：`plugins/tts-websocket/src/tts_websocket_engine.c`
- 测试：`plugins/tts-websocket/tests/test_tts_websocket_json.c`
- 测试：`plugins/tts-websocket/tests/test_tts_websocket_pcm.c`

- [x] **步骤 1：实现最小运行时改动（GREEN）**

将 `strstr`/`atoi` 采样率读取替换为：

```c
unsigned int sample_rate;
if (tts_websocket_json_get_uint(buffer, (apr_size_t)len,
        "sample_rate", &sample_rate) &&
    (sample_rate == 8000 || sample_rate == 24000)) {
    synth_channel->input_sample_rate = sample_rate;
}
```

在每个二进制帧使用 `tts_websocket_pcm_alignment(synth_channel->input_sample_rate)` 作为 `tts_websocket_pcm_accumulate()` 的对齐参数，并以该对齐值决定是否可编码。`audio.done` 在 8 kHz carry 为偶数字节时执行同一 PCM→PCMU→ring 写入路径，随后才清空 carry；24 kHz 不完整组仍仅记录并丢弃。

- [x] **步骤 2：运行插件范围回归（GREEN）**

运行：

```bash
cmake --build /tmp/tte-mrcp-issue46 --target tts_websocket test_tts_websocket_json test_tts_websocket_pcm -j2
ctest --test-dir /tmp/tte-mrcp-issue46 -R '^tts_websocket_(http_parse|json|pcm|lifecycle|ws|thread|connection_log)$' --output-on-failure
```

预期：插件和已注册的 TTS 测试全部通过。

- [x] **步骤 3：重构并回归（REFACTOR）**

仅提取 stream thread 内复用的 PCM→PCMU→ring 写入帮助函数；不得改变 ring buffer、MRCP 完成状态或 24 kHz 降采样语义。运行：

```bash
git diff --check
find tools -type f -name '*.sh' -print0 | xargs -0 bash -n
```

预期：两条命令退出码均为 0。

- [x] **步骤 4：提交**

运行：

```bash
git add plugins/tts-websocket/src/tts_websocket_engine.c plugins/tts-websocket/src/tts_websocket_json.c plugins/tts-websocket/src/tts_websocket_json.h plugins/tts-websocket/src/tts_websocket_pcm.c plugins/tts-websocket/src/tts_websocket_pcm.h plugins/tts-websocket/tests/test_tts_websocket_json.c plugins/tts-websocket/tests/test_tts_websocket_pcm.c plugins/tts-websocket/CMakeLists.txt plugins/tts-websocket/Makefile.am docs/superpowers/plans/2026-08-18-issue-46-8khz-review-fixes.md
git commit -m "fix(tts): preserve 8khz PCM tail samples" -m "Refs #46"
```

### 任务 3：记录执行证据

**文件：**
- 修改：`docs/superpowers/plans/2026-08-18-issue-46-8khz-review-fixes.md`

- [x] **步骤 1：重新索引并检查调用关系**

重新索引仓库，确认 `tts_websocket_stream_thread` 调用 PCM 累积与采样率解析边界。

- [x] **步骤 2：记录验证结果并提交**

追加 CTest、构建、静态检查和未验证平台结果；单独提交该记录。

## 执行结果（2026-08-18）

- RED：新增 JSON 与 PCM 对齐符号在旧实现链接失败；键名转义、畸形容器、尾随畸形成员、65 层嵌套和溢出回归均在对应旧提交上失败。
- GREEN：CMake `tts_websocket_json` 9/9、`tts_websocket_pcm` 7/7、`tts_websocket_http_parse` 通过；ASan/UBSan 下 JSON 与 PCM 纯函数测试也通过。
- Autotools：`Makefile.am`、生成的 `Makefile.in` 与 `configure` 已由 `./bootstrap` 同步；在带 Homebrew APR/APU 环境的早期验证中，`make -C plugins/tts-websocket check` 运行 connection-log、JSON、PCM 3/3 通过。最终本机配置尝试受 APU 参数校验和 CMake APR 头文件探测影响，未将完整插件构建标记为通过。
- 卫生：`git diff --check`、`./configure --help` 与 `find tools -type f -name '*.sh' -print0 | xargs -0 bash -n` 通过。
- 图谱：已重新索引 `Users-hosea-work-git-other-tte-mrcp`（5,949 nodes / 35,046 edges）。`tts_websocket_stream_thread` 调用 `tts_websocket_json_get_uint`、`tts_websocket_pcm_alignment` 和 PCM→μ-law→ring 写入边界；新增函数的测试调用关系已解析。
- 平台：macOS 纯函数测试已验证；Windows 编译/DLL 加载和 Linux 目标环境插件加载、RTP/MRCP 联调未验证。
- 审查：规格符合性与最终代码质量复审均通过；实现提交为 `5f169f0`、`61288c9`、`a34155a`、`7ca4fb3`、`a75101c`、`a8f4bed`、`7a6900b`、`48bd290`。
