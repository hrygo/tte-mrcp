# Issue #56 ASR rapid STOP 竞态修复设计

## 背景

在识别 generation 已收到 `STOP` 且等待 `GENERATION_DRAINED` 时，channel 可能开始关闭并使 transport worker 直接发出 `WORKER_CLOSED`。现有控制器在该 close fence 上清除 `stop_pending` 并只发送 channel close 响应，因此待处理的 MRCP STOP 响应会永久丢失。调用方随后超时并可能继续销毁仍由 registry 引用的 channel。

## 决策

保持现有 `funasr_control` 的单线程事件状态机和 worker close fence。处理 `WORKER_CLOSED` 时：

1. 先 join worker，确认没有后台线程还能访问 transport/channel；
2. 若存在待处理 STOP，先发送该 STOP 响应并记录 `stop_responded`；close response 失败后的同一 fence 重投只会重试 close，不会再次使用已经清空的 MRCP STOP message；
3. 再发送 channel close 响应。只有该响应成功后，engine 才删除 registry、清空 transport 并清除 `close_response_pending`，从而保留失败后的可重试 fence。

engine 对失败的 `WORKER_CLOSED` 使用专用 retry lifecycle：成功 `funasr_task_signal` 时立即把同一个 event 的所有权移交给 consumer task，计数上限为 3。重入队或 allocator 失败均有明确的单次 event release：已成功 join 的 worker 在耗尽时才允许 fallback 释放 registry/channel；未 join 时记录错误、保留 registry 而不 fallback。fallback 是安全 teardown，不保证仍能送达 STOP response。

不修改 UniMRCP 公共库、不延长压力测试超时，也不改变正常 `GENERATION_DRAINED` 路径。

## 验收

- STOP 后直接收到 `WORKER_CLOSED` 时，恰好发送一次 STOP 响应和一次 close 响应；close response 首次失败后，重投不得重发 STOP。
- 已有 final、drain、重复 close-fence 和 stale-event 语义保持不变。
- retry task 测试以 APR mutex/condition 等待真实 consumer callback，避免无锁忙轮询。

## 验证（2026-08-18）

- `cmake --build /tmp/tte-mrcp-issue56-cmake-homebrew --target asr_websocket test_funasr_control test_funasr_close_fence_retry -j2` 通过。
- `ctest --test-dir /tmp/tte-mrcp-issue56-cmake-homebrew --output-on-failure -R '^asr_websocket_'` 通过，6/6。
- `./bootstrap` 成功生成 `plugins/asr-websocket/Makefile.in`；隔离 configure 使用 `--with-apr=/opt/homebrew/opt/apr --with-apr-util=/opt/homebrew/opt/apr-util` 成功。Autotools 的 `test_funasr_control` 和 `test_funasr_close_fence_retry` 编译、运行通过。
- 完整 Autotools `make -j2` 在既有 `libs/mpf` 的 `JB_TRACE/RTP_TRACE` 与 `mpf_null_trace()` 宏签名错误停止，未到 ASR plugin 链接步骤；该基线阻断不归因于 Issue #56。
- Visual Studio 工程已登记 retry source/header，但本机没有 MSBuild，Windows 编译未验证。RHEL 7 / Kylin rapid-stop 运行时与外部 FunASR 服务均未验证。
