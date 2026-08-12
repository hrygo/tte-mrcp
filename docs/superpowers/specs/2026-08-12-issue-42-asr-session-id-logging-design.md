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

`funasr_default_io_write` 位于 transport 的默认 socket I/O 对象中，只能取得 `io`、数据指针和实际写入长度；它目前不含 session ID。`funasr_channel_recognize` 会在每个 generation 前将 MRCP request 的 session ID 复制到 `channel->session_id`，因此在创建 transport 的 channel-open 路径复制这个稳定字符串可避免跨线程反查 channel。

## 决策

在 `funasr_transport_config_t` 增加只读 `session_id` 字段；`funasr_open_channel_on_task` 传入 `channel->session_id`；`funasr_transport_create` 将该字符串复制到 transport 所属 APR pool，并把副本赋给默认 I/O 对象。`funasr_default_io_write` 使用完全一致的结构化前缀：

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
| `plugins/asr-websocket/src/funasr_ws_transport.h` | 为 transport 配置和默认 I/O 对象声明 session ID。 |
| `plugins/asr-websocket/src/funasr_ws_transport.c` | 复制 session ID，并在既有成功写入日志加入一致前缀。 |
| `plugins/asr-websocket/src/asr_websocket_engine.c` | 在打开 channel 时将当前 session ID 交给 transport 配置。 |
| `plugins/asr-websocket/tests/test_asr_frame_debug_logs.cmake` | 以源码契约检查格式、数据流和不变条件。 |

## 验证

先更新源码契约，使当前实现因网络日志缺少 session ID 而失败；再实施最小改动并验证通过。执行 CMake 注册的目标测试、`git diff --check` 和 codebase-memory 重新索引。macOS 以本机测试结果记录；Windows 与 Linux 本次不运行则明确标为未验证。
