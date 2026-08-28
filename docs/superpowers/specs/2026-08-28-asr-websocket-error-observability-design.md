# ASR WebSocket 异常可观测性设计

## 背景

会话 `32268b3ca27c11f1` 在首个媒体帧后约 2.27 秒返回
`006 recognizer-error`。传输指标显示 `completion_failure=3`，对应
`FUNASR_FAILURE_PROTOCOL`，并非本地无结果超时。当前实现遇到服务端 JSON
响应的非零 `code` 时直接失败，没有记录服务端状态码或失败阶段。

## 目标

在不输出二进制帧或凭据的前提下，为每个异常终止记录：

- MRCP session/call ID 和 generation；
- ASR endpoint（host、port、path）；
- 失败阶段（TCP connect、WebSocket handshake、响应帧、WebSocket Close、读写或轮询）；
- APR 状态、WebSocket close code（可得时）、ASR 非零 `code` 和相关字节数；
- 最终内部 failure 枚举。

按本次需求，所有服务端文本结果帧还要打印其 JSON。JSON 以单行形式输出，
每行最多 2KB；超出上限时输出前缀并标记 `truncated=1`。这项日志可能包含
识别文本，仅用于本次明确授权的内网诊断。

## 方案

在 `plugins/asr-websocket/src/funasr_ws_transport.c` 的 transport worker 中，
对每个已知失败分支写入结构一致的 `ERROR` 日志。WebSocket 握手失败和发送
音频帧失败的日志必须记录具体阶段与 APR 状态。非零 ASR `code` 的日志在解析
响应帧处产生，确保它在转化为 `FUNASR_FAILURE_PROTOCOL` 前保留下来。每个
ASR 服务端文本帧以 `INFO` 级别打印受限且单行化的 JSON。

不记录 HTTP/WebSocket 原始头、音频数据或任何凭据。现有完成语义、超时阈值
和 MRCP `Completion-Cause` 不修改。

## 验证

1. 先添加静态源代码测试，断言握手失败、音频发送失败均具有 `ERROR` 日志，且
   ASR 文本帧会以受限单行 JSON 形式输出。
2. 确认该测试在当前实现上失败。
3. 添加最小日志实现后运行 ASR WebSocket transport 单测和新增检查。
4. 运行 `git diff --check`，然后重新索引代码图谱并检查关键调用链。

## 非目标

- 不改变 20 秒、10 秒或任何其他超时配置；
- 不重试 ASR 请求；
- 不记录音频、HTTP/WebSocket 原始头或凭据；
- 不修改上游 UniMRCP 公共库。
