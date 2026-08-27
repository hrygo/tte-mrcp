# Issue #64 ASR 双结果超时设计

> 版本：v1.0 · 最后更新：2026-08-27 · 负责人：Codex · 评审：Approved

## 背景

ASR 通道在收到 `RECOGNIZE` 后持续接收 RTP 音频。客户未说话时，媒体层仍可能持续提供静音 PCM；FunASR 服务若不返回最终结果，现有逻辑会用每次音频发送的时间刷新无结果超时，导致上游的 `RECOGNIZE` 长时间保持 `IN-PROGRESS`。

超时必须按每个 `RECOGNIZE` generation 计算。多轮对话的下一轮 generation 不能继承上一轮的计时状态；它不是从接通电话或 channel 创建时开始累计。

## 决策

在 `asr_websocket` transport 的每个 generation 中维护两个独立 deadline，并使用现有 transport event → `funasr_control` → `funasr_recognition_complete` 链路发送 MRCP 事件。不得手工拼接 MRCP 字符串。

| XML 参数 | 默认值 | 起点与刷新规则 | 到期行为 |
| --- | ---: | --- | --- |
| `first-audio-result-timeout-ms` | 20,000 ms | 首个 WebSocket 音频二进制帧完整写入后开始；后续帧不得刷新。 | `NO-INPUT-TIMEOUT`、空 body、WARN。 |
| `last-speech-result-timeout-ms` | 10,000 ms | 首个音频帧完整写入后开始；每个非静音 PCM 帧完整写入后刷新。静音 PCM 不刷新。 | `NO-INPUT-TIMEOUT`、空 body、WARN，日志包含 `asr not response resut`。 |

两条 deadline 都在收到 FunASR 的**最终**识别结果前有效。先到期者终结 generation；最终结果、`STOP`、channel close 与超时竞争时，复用 `funasr_control` 的现有单终结语义，保证至多发送一次终结结果。

### 静音定义

媒体输入为线性 PCM。插件在入队到 WebSocket 前以帧的峰值绝对采样幅度判断声音活动：峰值高于一个内部、保守的固定阈值时视为非静音并刷新 10 秒 deadline。静音帧仍照常发送给 FunASR，但不刷新该 deadline。阈值不作为本 Issue 的运营配置，避免无需求的配置面扩张；测试使用零值 PCM 和明显非零 PCM 证明两类行为。

### MRCP 完成事件

任一 deadline 到期都投递 transport failure，并映射为 `RECOGNIZER_COMPLETION_CAUSE_NO_INPUT_TIMEOUT`。现有 recognizer completion 构造器将产生与以下语义等价的事件：

```mrcp
MRCP/2.0 501 RECOGNITION-COMPLETE 2
Channel-Identifier: <本次请求的 channel id>
Completion-Cause: NO-INPUT-TIMEOUT
Content-Length: 0
```

实际 request-id、channel id 和 wire framing 由 UniMRCP 的现有序列化器生成；超时分支不伪造或硬编码这些协议字段。

### 事件与日志

首帧 deadline 与末次语音 deadline 使用可区分的 transport failure 原因，但都映射到 `NO-INPUT-TIMEOUT` 和空识别文本。两个路径都记录 `APT_PRIO_WARNING`，其中末次语音 deadline 的日志必须包含精确文本 `asr not response resut`。日志仅记录 session ID、generation、超时类型及配置毫秒数，禁止记录 PCM 或识别文本。

## 替代方案

1. 仅按最后一个 RTP/PCM 帧刷新 10 秒计时：放弃。持续静音会无限刷新 deadline。
2. 仅依赖 FunASR 的服务端端点检测：放弃。服务端不响应时插件仍不能向上游有界完成。
3. 在插件中做完整 VAD 并增加阈值配置：放弃。当前问题只需分辨静音与明显声音；复杂 VAD、额外依赖和运营参数超出范围。

## 验收

- `conf/unimrcpserver.xml` 配置两个参数，缺省值分别为 20,000 ms 和 10,000 ms；非法或非正数值必须回退到安全默认值并记录明确日志。
- 使用 fake clock 的 transport 测试证明：持续发送非静音帧仍在首帧后 20 秒失败；首帧后持续静音在 10 秒失败；每次非静音帧只刷新 10 秒 deadline，不影响 20 秒 absolute deadline。
- 最终结果先到、`STOP` 先到和 close 先到时，不得额外产生 `RECOGNITION-COMPLETE`；两个 generation 连续运行时计时状态必须独立。
- engine/control 测试验证失败只返回一次 `NO-INPUT-TIMEOUT` completion 且结果 body 长度为 0。
- 两个超时路径均产生 WARN；10 秒路径的日志包含 `asr not response resut`。

## 非目标

- 不修改 UniMRCP 公共库、MRCP wire serializer 或外部 FunASR 服务。
- 不将音频内容、完整识别文本或密钥写入日志。
- 不改变正常最终识别、STOP、PAUSE/RESUME 或 WebSocket framing 行为。
