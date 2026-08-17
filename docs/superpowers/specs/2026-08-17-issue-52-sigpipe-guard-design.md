# Issue #52 SIGPIPE 防护第一阶段设计

## 1. 背景与证据边界

TTE-MRCP v1.0.2 在 RHEL 7.9 x86、约 30 并发下出现 `unimrcpserver` 退出并被托管方拉起的现象。线上退出信号尚未取得，因此本设计不把事故归因视为定案。

当前源码可以确认：TTS WebSocket 发送最终调用 `apr_socket_send()`；目标环境 APR 1.4.8 的 Unix 实现使用 `write()`；服务端进程没有显式忽略 SIGPIPE。对端关闭后继续发送业务帧、Pong 或 Close 时，进程存在被 SIGPIPE 默认动作终止的确定风险。

本阶段修复这个已证实风险，同时改进相关诊断和回归测试。共享 consumer 异步化及 channel destroy 竞态不在本阶段实现范围内。

## 2. 目标

1. POSIX 平台上的 `unimrcpserver` 不因任意线程向已关闭 socket 写入而被 SIGPIPE 终止。
2. 写入失败继续由 APR/插件错误返回路径处理，不改变正常 WebSocket 数据流。
3. Windows 构建和运行行为保持不变。
4. 建立可重复的断连发送回归测试，证明防护前后的进程级行为差异。
5. 降低 TTS 逐帧 INFO 日志量，保留会话级诊断指标。

## 3. 非目标

- 不重构共享 consumer task，不在本阶段移动建连或握手线程。
- 不修改 MRCP session/channel 状态机。
- 不修改 APR、MRCP、MPF 公共库的 API 或 ABI。
- 不把尚未复现的 UAF、二次释放或 OOM 当作本次实现依据。
- 不宣称本次线上事故已经由 SIGPIPE 定案。

## 4. 方案选择

### 4.1 采用方案：服务端进程入口统一忽略 SIGPIPE

在 `platforms/unimrcp-server/src/main.c` 的 POSIX 初始化边界设置 SIGPIPE 为忽略。信号处置在工作线程和插件启动前完成，使整个服务端进程的行为明确且一致。

对已关闭连接执行 `write()` 时，内核不再以 SIGPIPE 终止进程；APR 返回对应错误，TTS WebSocket 发送函数返回失败，现有握手、发送、stream 或 cleanup 路径负责关闭本次连接并结束会话。

Windows 条件编译路径不引用 SIGPIPE，也不改变 Winsock 行为。

### 4.2 未采用方案

在 TTS 插件加载时忽略 SIGPIPE 仍会改变整个进程，只是把全局副作用隐藏在插件内部，而且无法保护 ASR 和框架其他 socket 写入。

绕过 APR、直接使用 `send(..., MSG_NOSIGNAL)` 可以缩小到 TTS，但会引入 Linux/macOS/Windows 分支，并可能改变 APR 已有的阻塞、超时和错误映射语义。

## 5. 组件与数据流

### 5.1 进程初始化

`unimrcpserver` 启动后、创建服务端和插件线程前，调用 `unimrcp_server_signals_init()`。该函数位于 `platforms/unimrcp-server/src/unimrcp_server_signal.c`，接口声明位于同目录头文件。POSIX 分支设置 SIGPIPE 忽略；失败时记录错误并停止启动，避免服务在保护未生效的情况下继续运行。Windows 分支返回成功。

### 5.2 WebSocket 发送

`websocket_send_all()` 继续使用 `apr_socket_send()`，不绕开 APR。非 EINTR 错误或零字节发送返回失败。新增或调整的日志只记录 session、发送阶段、APR 状态和字节统计，不记录请求正文、密钥或完整帧数据。

业务文本、Pong 和 Close 共用该底层语义，无需在各调用点分别安装信号处理。

### 5.3 日志

逐帧接收长度和逐帧 timing 从 INFO 降为 DEBUG。以下会话级信息继续保留在 INFO/NOTICE：首包延迟、累计音频字节、帧数、最大帧间隔、丢弃/静音统计和完成原因。

## 6. 错误处理与兼容性

- SIGPIPE 处置是进程级行为，覆盖服务端所有线程和模块。
- 正常写入行为不变；断连写入由“进程终止”变为“调用返回错误”。
- 各模块必须检查发送返回值；本阶段重点验证 TTS 路径，并对服务启动、SIP/RTSP、ASR/TTS 插件加载做冒烟检查。
- 不在信号 handler 内执行日志或非异步信号安全操作，因为采用的是忽略处置而非自定义 handler。
- 如果服务端未来 fork/exec 外部进程，需要单独审计被忽略信号的继承；当前实现不新增 fork/exec 行为。

## 7. 测试设计

实现遵循红—绿—重构：先提交能够暴露缺失防护的失败测试，再写生产代码。

### 7.1 进程级回归测试

测试使用本地 `socketpair()` 或 loopback TCP 和子进程，避免依赖外部 TTS 服务：

1. 对端关闭后，未执行服务端信号初始化的子进程写入，测试夹具确认基线会收到 SIGPIPE。
2. 执行服务端信号初始化后，对端关闭，子进程写入不再因 SIGPIPE 退出，并收到 EPIPE/等价 APR 错误。
3. 重复发送，确认防护持续有效且不会只保护第一次写入。

测试必须设置超时，避免 socket 行为异常时无限等待。

### 7.2 TTS 发送路径测试

使用断连 socket 覆盖共用发送底层，确认 `websocket_send_all()` 返回失败而非使测试进程退出。通过共用底层覆盖业务帧、Pong 和 Close 的关键风险，不重复测试 WebSocket 编码细节。

### 7.3 现有回归与构建

- TTS HTTP parser、WebSocket、lifecycle、thread、PCM 和 connection-log 测试。
- `git diff --check`。
- 与变更范围匹配的 Autotools/CMake configure。
- macOS 本机只能证明 macOS 源码与构建行为；Linux 运行时 SIGPIPE 结论需在 Linux CI、容器或目标环境验证。
- Windows 路径至少做源码/工程编译检查；无法执行时明确标为未验证。

## 8. 安全与可观测性

- 不新增密钥、凭据或外部网络访问。
- 断连错误日志不得包含完整合成文本、WebSocket 请求正文或音频 hex dump。
- 忽略 SIGPIPE 后，错误必须转化为明确的连接/会话失败，不能静默无限重试。
- 测试仅绑定 loopback 或使用本地 socket，不开放外部监听端口。

## 9. 变更范围

预计修改：

- `platforms/unimrcp-server/src/main.c`。
- `platforms/unimrcp-server/src/unimrcp_server_signal.c` 及其头文件。
- `platforms/unimrcp-server/tests/test_unimrcp_server_signal.c` 及 Autotools、CMake、Visual Studio 构建接入文件。
- `plugins/tts-websocket/src/tts_websocket_engine.c` 的发送失败诊断和逐帧日志等级。
- `plugins/tts-websocket/tests/` 下的断连发送回归测试及构建接入。

不修改 `libs/mrcp*`、`libs/mpf`、`libs/apr-toolkit` 的公共行为。

## 10. 交付边界

本阶段完成后，Issue #52 可以确认“已消除已知 SIGPIPE 进程退出风险”，但只有取得线上退出状态或完成与线上一致的故障复现后，才能确认原事故根因。

共享 consumer 的网络 I/O 异步化、队列反压指标和关闭状态机竞态审计作为后续 Issue 单独设计、实现和验收。
