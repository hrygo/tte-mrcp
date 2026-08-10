# Issue #37：ASR 音频链路可观测性设计

## 目标

为每个 FunASR 识别会话提供可关联、低噪声的上游音频接收与下游 WebSocket 发送观测；同时在 TTS `SPEAK` 入口记录当前执行线程。变更只增加可观测性，不改变 PCM、WebSocket、MRCP 状态或实时路径的既有行为。

## 当前路径与边界

```text
MPF input → funasr_stream_write → funasr_transport_enqueue_pcm → TX ring
       → transport worker → WebSocket binary write → FunASR
```

上游的 `funasr_transport_enqueue_pcm` 已维护帧数、有效音频字节、最大帧间隔、队列高水位及溢出统计；下游 worker 已维护首发延迟与最大写等待。日志改动复用这些统计，不新增逐帧格式化、I/O 或锁竞争。

## 决策

采用会话级、固定窗口的汇总日志，而不是逐帧 DEBUG 日志或新增诊断线程：

- 每个 ASR generation 使用已有 session/channel 关联字段输出相同的关联键。
- 在固定窗口到期时输出低频进度汇总；正常完成、上游停顿、队列溢出、下游写阻塞、关闭或失败时强制输出终态汇总。
- 汇总包含：上游/下游累计字节与帧数、最近帧时间与间隔、队列当前/高水位、丢弃或异常、首发延迟、下游写等待及完成原因。
- 默认日志绝不包含 PCM、WebSocket payload、识别文本或 hex dump。
- `tts_websocket_channel_speak` 在现有 session/channel 关联日志中加入项目跨平台 APR 抽象取得的当前线程标识。

## 实现范围

| 文件 | 变更 |
| --- | --- |
| `plugins/asr-websocket/src/funasr_ws_transport.[ch]` | 扩展可快照的收发统计和节流状态；在 worker 的实际成功写入路径累计下游字节/帧与时间；提供受测的汇总快照/格式化边界。 |
| `plugins/asr-websocket/src/asr_websocket_engine.c` | 在现有 transport metrics 事件消费处按窗口或终态输出结构化关联日志。 |
| `plugins/asr-websocket/tests/test_funasr_ws_transport.c` | 先验证统计、节流、发送阻塞与终态快照；不发送或比较音频内容。 |
| `plugins/tts-websocket/src/tts_websocket_engine.c` | 只增强 SPEAK 起始日志的线程标识和既有会话关联信息。 |

不修改 FunASR 服务端、MRCP/MPF 公共库、协议帧或运行配置。

## 验证

1. 新增单测先失败，分别覆盖收发累计量、帧间隔、窗口节流和异常终态强制汇总。
2. 使用 fake clock/fake I/O 验证上游停顿、下游部分写入或写阻塞时，快照可区分上游与下游滞后且不改变排队数据。
3. 检查日志格式不包含原始音频、识别文本或 hex dump；TTS SPEAK 日志包含线程标识和 session/channel 字段。
4. 运行相应插件单测、`git diff --check`，并重新索引 codebase-memory；macOS、Windows、Linux 结果分别记录，未运行的平台明确标为未验证。

## 风险与完成定义

风险是日志本身影响音频热路径。通过仅在已有统计边界更新计数、将格式化/输出限制在节流或终态路径，并复用 APR 锁与时间抽象来控制。完成不以 MRCP 成功码单独判断：单测和诊断输出必须能用同一关联键区分“上游未到达”“已入队但下游未发送”“下游写入受阻”三类状态，同时保持既有音频数据与 MRCP 结果。

## 追加需求：逐帧 DEBUG 日志

Issue #37 评论明确要求在特殊诊断场景下逐帧对照数据，因此补充两条中文 `DEBUG` 日志；它们与既有节流汇总日志并存，且不输出 PCM 内容：

1. `funasr_stream_write`：每次收到有效客户端媒体帧后、重采样前，记录“接收客户端音频数据包，大小=<原始帧字节数>”。这反映 MPF 实际交给插件的输入大小。
2. `funasr_default_io_write`：每次 `apr_socket_send()` 成功返回后，记录“发送 ASR WebSocket 网络数据包，大小=<本次实际写入字节数>”。这反映底层 socket 的实际写出量；一个 WebSocket 帧在 short write 时可对应多条日志，日志语义是网络写入而不是逻辑帧。

日志采用现有 `apt_log`/APR 抽象，不增加锁、队列、协议字段或网络 I/O。对空帧和失败发送不输出“成功发送”日志；现有音频字节、WebSocket 帧、MRCP 状态与错误处理保持不变。
