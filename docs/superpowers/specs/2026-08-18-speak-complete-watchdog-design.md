# Issue #57：TTS SPEAK-COMPLETE 有界完成设计

## 背景与问题

生产异常会话已经收到 TTS WebSocket 服务的 `session.done`，流线程也设置
`stream_complete=1` 并退出，但没有出现 `[DIAG] Playback finishing`，MRCP 上游最终因
未收到 `SPEAK-COMPLETE` 主动断链。

当前 `session.done` 只标记 producer 完成。`SPEAK-COMPLETE` 只能由后续 MPF read
callback 在环形缓冲区排空、post-roll 和 RTP drain 后发送。因此，只要 MPF callback
不再触发或完成状态无法推进，SPEAK 请求就没有有界终态。

## 目标与边界

目标：

- 正常路径仍在真实音频、post-roll 和 RTP drain 完成后发送 normal
  `SPEAK-COMPLETE`。
- MPF 完成链停止推进时，在有界时间内发送 error `SPEAK-COMPLETE`。
- 正常完成、watchdog、STOP、BARGE-IN 和 cleanup 竞争时，完成事件至多发送一次。
- 日志可区分正常排空、watchdog 超时和上游提前关闭。

不在本次范围：

- 修改 UniMRCP 公共 MPF 调度机制。
- 在收到 `session.done` 时立即报告正常完成。
- 改变 TTS WebSocket 协议或 RTP 编码格式。

## 方案比较

### 方案 A：收到 `session.done` 立即发送正常完成

实现简单，但 MRCP 控制消息可能越过尚未发送的尾部 RTP，破坏现有音频完整性保证。
不采用。

### 方案 B：只增加 MPF callback 日志

可以进一步确认现场触发条件，但不能消除请求永久悬挂。只作为诊断增强，不作为修复。

### 方案 C：有界排空与 exactly-once watchdog（采用）

保留 MPF 正常完成链，同时在 `session.done` 后启动有界 watchdog。正常路径先完成时取消
watchdog；截止时间到达而状态仍未完成时，发送 error completion。所有终态入口共享
同一仲裁状态，防止重复响应。

## 设计

### 完成仲裁

为 channel 增加每次 SPEAK 的 completion generation 和原子化终态标记。所有发送
`SPEAK-COMPLETE` 的路径必须先在 channel mutex 下验证：

1. 当前仍是同一代 SPEAK 请求；
2. 请求尚未完成或停止；
3. 当前入口成功取得终态所有权。

取得所有权后保存请求指针、清空 active request 并设置终态，再在锁外构造和发送事件。
STOP/BARGE-IN 保持其协议语义，但必须使 watchdog 失效，不能再补发完成事件。

### Watchdog 生命周期

每次成功启动 SPEAK 时生成新的 generation。WebSocket worker 收到 `session.done`、完成
PCM carry flush 并设置 `stream_complete` 后，计算 deadline 并等待 completion condition：

- 剩余音频时长 = ring buffer 未读 PCMU 字节数 / 8000 bytes/s；
- 加上配置的 completion post-roll；
- 加上固定调度宽限；
- 设置合理的最小宽限，覆盖短音频和 MPF/RTP 调度抖动。

正常 MPF callback 完成、STOP 或 cleanup 会广播 condition。若等待超时且 generation
仍匹配、请求仍 active，watchdog 取得终态所有权并发送 completion cause error。

复用当前每次 SPEAK 已存在的 WebSocket worker，避免额外创建每请求线程。worker 在关闭
WebSocket socket 后执行有界等待；channel cleanup 先唤醒并 join worker，再销毁 session
pool，从而保持请求与 pool 生命周期有效。

### 正常完成路径

MPF read callback 保持现有状态链：

`STREAMING -> POSTROLL -> RTP_DRAIN -> COMPLETE_READY`

到达 `COMPLETE_READY` 后，通过统一完成仲裁发送 normal completion。watchdog 被唤醒，
发现请求已终态后直接退出。不得缩短正常排空时间，也不得在 ring 尚有数据时发送 normal
completion。

### 超时路径

超时表示媒体输出未能在根据剩余音频计算出的期限内完成。此时：

- completion cause 使用 error；
- 日志记录 generation、deadline、最后一次 MPF tick、输出状态、ring written/read/available
  和 lifecycle 状态；
- 不伪报音频完整；
- 清理仍走现有幂等 cleanup，不从 watchdog 直接销毁 channel 或 pool。

### 可观测性

增加限频或终态日志：

- `session.done` 时间和 watchdog deadline；
- 最近一次 MPF read tick 时间；
- 输出状态迁移；
- completion 所有者（MPF、watchdog、STOP/cleanup）；
- event send 结果与 completion cause。

日志不得输出完整合成文本或音频内容。

## 测试设计

先增加独立、可单测的 completion 状态模块，覆盖：

1. 正常 MPF 完成先取得终态，watchdog 不重复发送；
2. deadline 到期时 active 请求转为 error completion；
3. deadline 前 MPF tick 持续推进时不误超时；
4. STOP/BARGE-IN 使对应 generation 的 watchdog 失效；
5. 旧 generation watchdog 不能完成新的 SPEAK；
6. lifecycle close 与 watchdog 同时发生时至多一个终态所有者；
7. deadline 由剩余 8 kHz PCMU 字节数、post-roll 和宽限正确计算；
8. 长音频 burst 允许按真实播放时长排空，不使用从 `session.done` 起的过短固定超时。

插件级测试继续验证 ring 全量排空后 normal completion；新增模拟 MPF callback 停止的测试，
验证在有界时间内返回 error completion。

## 验收标准

- 每个收到 `session.done` 的 SPEAK 最终关联唯一 completion，或有明确 STOP/上游断链记录。
- 正常路径满足 `ring_written == ring_read` 后再发 normal `SPEAK-COMPLETE`。
- MPF callback 停止时在计算出的 deadline 内发 error `SPEAK-COMPLETE`。
- 所有竞态测试证明 completion exactly once。
- TTS 插件相关单测、构建检查和 `git diff --check` 通过。
- 重新索引 codebase-memory，并确认 canonical 完成链节点。
