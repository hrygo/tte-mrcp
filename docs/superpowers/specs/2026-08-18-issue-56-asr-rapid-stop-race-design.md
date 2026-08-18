# Issue #56 ASR rapid STOP 竞态修复设计

## 背景

在识别 generation 已收到 `STOP` 且等待 `GENERATION_DRAINED` 时，channel 可能开始关闭并使 transport worker 直接发出 `WORKER_CLOSED`。现有控制器在该 close fence 上清除 `stop_pending` 并只发送 channel close 响应，因此待处理的 MRCP STOP 响应会永久丢失。调用方随后超时并可能继续销毁仍由 registry 引用的 channel。

## 决策

保持现有 `funasr_control` 的单线程事件状态机和 worker close fence。处理 `WORKER_CLOSED` 时：

1. 先 join worker，确认没有后台线程还能访问 transport/channel；
2. 若存在待处理 STOP，先发送该 STOP 响应并将 generation 标为终止；
3. 再发送 channel close 响应，以便 registry 删除和资源释放只发生在 worker 已关闭之后。

不修改 UniMRCP 公共库、不延长压力测试超时，也不改变正常 `GENERATION_DRAINED` 路径。

## 验收

- STOP 后直接收到 `WORKER_CLOSED` 时，恰好发送一次 STOP 响应和一次 close 响应，且 join 恰好一次。
- 已有 final、drain、重复 close-fence 和 stale-event 语义保持不变。
- ASR control/transport 单测通过；可用环境中 rapid-stop ASR 门禁不再出现 `STOP_TIMEOUT` 或服务崩溃。
