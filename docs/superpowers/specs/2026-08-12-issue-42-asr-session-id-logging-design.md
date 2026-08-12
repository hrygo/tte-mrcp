# Issue #42：ASR 逐包 DEBUG 日志会话标识设计

## 目标

为 Issue #37 / 提交 `e76c3ce74eee3ed1d96bdc9c08b44aa6b63bd414` 新增的两类 ASR 逐包 DEBUG 日志使用同一个 session ID 前缀，使客户端媒体包与其对应的 FunASR WebSocket 实际写入日志可直接关联。

## 当前事实与边界

现行链路为：

```text
MPF input → funasr_stream_write → funasr_transport_enqueue_pcm → TX ring
          → transport worker → funasr_default_io_write → WebSocket
```

`funasr_stream_write` 已通过 `LOG_WITH_SID(channel, ...)` 输出：

```text
asr_websocket: [session_id=<id>] 接收客户端媒体流音频数据包，大小=<原始帧字节数> 字节
```

`funasr_default_io_write` 位于 transport 的默认 socket I/O 对象中，只能取得 `io`、数据指针和实际写入长度；它目前不含 session ID。`funasr_channel_recognize` 会在每个 generation 前将 MRCP request 的 session ID 复制到 `channel->session_id`，并将该值作为 `format.call_id` 传给 `funasr_transport_begin_generation`。transport 已在该函数中把 `format.call_id` 复制到受 mutex 保护的固定 `transport->call_id` 缓冲区，这是 worker 使用 WebSocket handshake `call_id` 的稳定所有权边界。

## 决策

在 `funasr_transport_begin_generation` 完成 `format.call_id` 到 `transport->call_id` 的既有复制后，把默认 I/O 的 `session_id` 指向这个 transport 自有缓冲区。该同步点与媒体日志使用的 `channel->session_id` 属于同一个 MRCP request，且发生在 worker 被唤醒之前。`funasr_default_io_write` 使用完全一致的结构化前缀：

```text
asr_websocket: [session_id=<id>] 发送 ASR WebSocket 网络数据包，大小=<实际写入字节数> 字节
```

缺少 ID 时使用与 `LOG_WITH_SID` 相同的 `N/A` 回退值。

不传递 `funasr_channel_t *` 到 transport worker，也不在 socket 写入时访问 channel：这样避免扩大会话与 worker 的生命周期/并发耦合。

## 不变条件

- `apr_socket_send()` 仍只调用一次，函数返回原始 `apr_status_t`。
- 仅在 `status == APR_SUCCESS && *size != 0` 后输出网络写入日志；`*size` 仍为实际写入长度，短写语义不变。
- 客户端媒体日志仍位于有效帧校验后、重采样前，仍使用 `frame->codec_frame.size`。
- 不输出 PCM、WebSocket payload、识别文本、hex dump 或指针地址。
- 不修改 MRCP、PCM、WebSocket 帧、队列或配置行为。

## 文件范围

| 文件 | 变更 |
| --- | --- |
| `plugins/asr-websocket/src/funasr_ws_transport.c` | 在 generation 开始时同步 transport 自有的 call ID 到默认 I/O，并在既有成功写入日志加入一致前缀。 |
| `plugins/asr-websocket/tests/test_asr_frame_debug_logs.cmake` | 以源码契约检查格式、数据流和不变条件。 |

## 验证

先更新源码契约，使当前实现因网络日志缺少 session ID 而失败；再实施最小改动并验证通过。执行 CMake 注册的目标测试、`git diff --check` 和 codebase-memory 重新索引。macOS 以本机测试结果记录；Windows 与 Linux 本次不运行则明确标为未验证。
