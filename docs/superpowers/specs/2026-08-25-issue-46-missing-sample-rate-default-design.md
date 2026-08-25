# Issue #46：缺失采样率按 8 kHz 处理

## 目标

修复生产环境中 TTS 服务的 `audio.start` 不携带 `sample_rate` 时，插件默认按 24 kHz 对 8 kHz PCM 再做 3:1 重采样，从而令电话侧语音变为约三倍速的问题。

## 已确认的协议与边界

- TTS 二进制负载为 16-bit little-endian 单声道 PCM；裸 PCM 本身不携带采样率。
- 服务端当前可以发送 `{"type":"audio.start"}`，但无法保证携带 `sample_rate`。
- RTP/MRCP 下游保持 `PCMU/8000`，不修改 SDP、MPF codec、ring buffer 或生命周期协议。
- 不新增 `resampling-enabled` 或任何其他 XML 配置开关。

服务端后续若能补充字段，推荐的控制帧格式为：

```json
{"type":"audio.start","sample_rate":8000,"format":"pcm","channels":1,"sample_width":16}
```

24 kHz 旧服务应发送相同结构并将 `sample_rate` 设为 `24000`。二进制 PCM 帧和 `audio.done` / `session.done` 语义不变。

## 决策

`audio.start` 的有效 `sample_rate` 仍是采样率的唯一显式依据：

| 收到的字段 | 输入处理 | 对齐 |
| --- | --- | --- |
| `8000` | 直接 PCM → PCMU | 2 字节 |
| `24000` | 24 kHz PCM 3:1 重采样，再转 PCMU | 6 字节 |
| 缺失 | 每个 `audio.start` 回落到 8 kHz；每条 SPEAK 记录一次警告；PCM → PCMU | 2 字节 |

无效、溢出或不支持的已显式字段继续走既有的安全处理，不得被当作缺失字段静默转换。每次 SPEAK 启动时将默认输入采样率初始化为 8000，后续有效字段可以覆盖该默认值。

## 数据流

```text
audio.start
  ├─ sample_rate=24000 → 6-byte accumulate → 3:1 resample → PCMU/8000
  ├─ sample_rate=8000  → 2-byte accumulate → PCMU/8000
  └─ 字段缺失           → warn once → 2-byte accumulate → PCMU/8000
```

这使生产日志中 `222720 B` 的 8 kHz、16-bit PCM 输出为 `111360 B` PCMU，而不是错误重采样后的 `37120 B`。

## 测试与验收

- 新增/调整采样率状态测试：缺失字段将输入采样率设为 8000，并且日志告警至多一次。
- PCM 测试证明缺失字段选择的 8 kHz 路径保留每个完整 16-bit 采样；24 kHz 路径仍保留 3:1 行为。
- 覆盖跨 WebSocket 帧的奇数字节、8 kHz 的完整 2/4-byte 尾样本和 24 kHz 非完整 6-byte 尾块。
- 运行插件 PCM 与 sample-rate 单元测试、CMake/CTest 相关目标及 `git diff --check`。
- Windows、Linux 运行时部署验证不在本次本地变更中伪造为已验证；生产验证应检查最终 PCMU 字节数、RTP payload 为 160 B/20 ms，以及不存在“missing … fallback 24000”日志。

## 文件范围

| 文件 | 变更 |
| --- | --- |
| `plugins/tts-websocket/src/tts_websocket_engine.c` | 把缺失 `sample_rate` 的初始化/回退值由 24000 改为 8000，并同步诊断文本。 |
| `plugins/tts-websocket/tests/*sample_rate*.c` | 断言缺失字段的默认值与一次告警语义。 |
| `plugins/tts-websocket/tests/test_tts_websocket_pcm.c` | 回归 8 kHz 直通与 24 kHz 重采样的字节数、对齐和尾样本。 |

不修改服务端、XML 配置、MRCP/RTP 框架或公共媒体库。
