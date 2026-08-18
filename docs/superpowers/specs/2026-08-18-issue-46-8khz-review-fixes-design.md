# Issue #46：8 kHz PCM 直通审查修复设计

## 目标

修复 PR #49 审查发现的两项数据正确性问题：`audio.start` 的 `sample_rate` 不得从 JSON 字符串值中误取，且 8 kHz PCM 在 WebSocket 帧边界及 `audio.done` 边界不得丢弃完整采样。

## 当前事实与边界

`tts_websocket_stream_thread` 在收到 `audio.start` 后记录输入采样率，并在二进制帧到来时调用 `tts_websocket_pcm_accumulate` 和 `tts_websocket_pcm_to_ulaw`。24 kHz PCM 每 3 个 16-bit 采样产生一个 8 kHz PCMU 字节，因此需要 6 字节对齐；8 kHz PCM 每个完整 16-bit 采样都可直接编码，因此只需要 2 字节对齐。

现有 `strstr(buffer, "sample_rate")` 不区分 JSON 键、字符串内容或嵌套值。音频句子文本可包含 `sample_rate:24000`，从而遮蔽实际的 `"sample_rate":8000` 字段。现有无条件 6 字节对齐会使 8 kHz 句尾的 2 或 4 字节完整 PCM 在 `audio.done` 被清空。

## 决策

在 TTS 插件内新增一个受长度约束的 JSON 数字字段提取辅助函数。该函数按 JSON token 扫描：跳过字符串（包括反斜杠转义），仅接受对象键位置中名称精确为 `sample_rate` 的键，再读取冒号后的十进制无符号值。仅 8000 和 24000 继续被调用方接受，其他值维持现有告警与回退行为。

二进制 PCM 处理根据 `input_sample_rate` 选择对齐大小：8000 Hz 为 2 字节，24000 Hz 为 6 字节。`audio.done` 时保留 24 kHz 的不完整 3-sample 组丢弃行为；8 kHz 中若 carry 是完整偶数字节，先编码并写入 ring buffer，只有奇数字节才视为不完整采样并丢弃。

不引入 JSON 库、不修改 MRCP/RTP codec 协商、不改变 24 kHz 的移动平均降采样算法，也不扩大 stream thread、ring buffer 或 socket 生命周期边界。

## 测试策略

- 为 JSON helper 加入独立单元测试：字符串值内的 `sample_rate:24000` 不能遮蔽顶层 8000；含转义引号的字符串也不能遮蔽实际字段；无字段或非数字字段返回失败。
- 为 PCM 累积/转换路径增加 8 kHz 两字节和四字节尾块测试，证明完整样本不会因 6 字节分组被滞留。
- 保留 24 kHz 非 6 字节输入拒绝测试，证明旧路径不变。
- 运行受影响 CMake/CTest 目标、`git diff --check` 和插件 CMake 构建；Windows 与 Linux 若未运行，明确标记为未验证。

## 文件范围

| 文件 | 变更 |
| --- | --- |
| `plugins/tts-websocket/src/tts_websocket_engine.c` | 结构化解析采样率、按采样率对齐并在 `audio.done` 保留 8 kHz 完整尾样本。 |
| `plugins/tts-websocket/src/tts_websocket_pcm.c` | 不变更 G.711 编码逻辑；仅复用现有 2/6 字节输入契约。 |
| `plugins/tts-websocket/tests/test_tts_websocket_pcm.c` | 覆盖 8 kHz 尾块不丢样本与 24 kHz 对齐不变量。 |
| `plugins/tts-websocket/tests/test_tts_websocket_sample_rate.c` | 新增 JSON 采样率提取单元测试。 |
| `plugins/tts-websocket/CMakeLists.txt` | 注册新增纯函数测试目标。 |
