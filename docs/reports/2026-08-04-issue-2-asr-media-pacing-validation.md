# Issue #2 ASR 媒体节拍修复验证记录

日期：2026-08-04  
分支：`fix/issue-2-asr-media-pacing`  
Issue：[#2](https://github.com/hrygo/tte-mrcp/issues/2)

## 1. 验证结论

本变更已完成源码、插件级构建、协议单测、故障注入工具和静态调用链验证。媒体回调只执行输入校验、必要的 8 kHz→16 kHz 重采样、单调时钟取值和有界队列入队；WebSocket 连接、握手、收发、解析、Ping/Pong、STOP drain 和关闭 fence 均由每通道 transport worker 负责。

macOS 上 CMake 与 Autotools 的插件本体和 4 个插件测试均实际编译，测试 4/4 通过。新增的 20-worker 回归覆盖 20 个独立 transport、每通道 50 个 20 ms PCM 帧、最终结果和关闭 fence，全部通道零队列溢出。

完整 UMC/Server 构建受仓库既有公共层问题阻断，因此未执行真实的 20 个 UMC 会话实验，也没有把 transport 单测冒充为端到端结果。Windows、Linux 和生产灰度均标记为未验证。

## 2. 源码事实

- `funasr_stream_write` 不再执行 socket、WebSocket 解码、JSON 解析、MRCP 事件发送、日志输出或堆分配。
- 每个 channel 拥有独立 APR transport subpool、worker、非阻塞 socket/pollset、1 秒有界 PCM ring 和 generation 状态。
- 16 kHz、mono、16-bit 的 200 ms WebSocket 聚合块为 6,400 bytes；STOP 先发送剩余尾部，再发送一个空 binary frame。
- HTTP 和 WebSocket 解码支持 short read、握手后粘包、fragment、Ping/Pong、Close、扩展长度和大小上限。
- Ping 在音频帧部分发送期间到达时进入 worker 内部待发控制槽，音频发送完成后仍发送一次 Pong。
- STOP 立即关闭生产者入队；worker 在 finishing 阶段先冻结并发布本代 metrics，再独占发布 drained/final/failure 终态，下一代不会重置或污染旧代统计。
- WebSocket 握手期间的 STOP 使用 `stop_drain_timeout` 限制等待，正常快速握手仍可发送队尾与空结束帧；channel close 立即中断握手。
- channel/engine close 使用 consumer task 上的 `WORKER_CLOSED` fence；worker 未退出时不释放 channel/engine pool。
- 每会话汇总包含媒体帧数、有效音频字节、1 ms 分桶的 gap histogram 与 p99/max、ring high-water、overrun、首发延迟、最大写等待、异常关闭、partial read、消息数和完成失败原因。
- 自然输入结束（真实 UMC 播完音频文件且不发 STOP）由 transport worker 在 `input_idle_timeout_us`（默认 1 s）无新音频后判定：冲刷尾部字节并发送空 binary 结束帧，然后等待服务端 final；该路径与 STOP 的空帧路径共用同一协议语义，使真实 UMC 的 20 并发 loopback 能够正常完成。

## 3. 测试与实验事实

### 3.1 CMake 插件门禁（通过）

环境通过 `tools/dev/setup_macos.sh` 检查，APR 1.7.6、APR-util 1.6.3、Sofia-SIP 1.13.17 均可用。使用显式 Homebrew `PKG_CONFIG_PATH`：

```sh
rtk env PKG_CONFIG_PATH=/opt/homebrew/opt/apr/lib/pkgconfig:/opt/homebrew/opt/apr-util/lib/pkgconfig:/opt/homebrew/opt/sofia-sip/lib/pkgconfig \
  cmake -S . -B /tmp/tte-mrcp-issue-2-cmake-werror \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_DEMORECOG_TESTS=ON \
  '-DCMAKE_C_FLAGS=-Wall -Wextra -Werror -Wno-missing-field-initializers'
rtk cmake --build /tmp/tte-mrcp-issue-2-cmake-werror \
  --target test_resample test_json_unescape test_funasr_ws_transport test_funasr_control -j4
rtk ctest --test-dir /tmp/tte-mrcp-issue-2-cmake-werror \
  --output-on-failure -R '^demorecog_'
```

结果：开启 warnings-as-errors 后 4/4 通过；transport/control 另做 5 次连续重复，均通过。

### 3.2 Autotools 插件门禁（通过）

```sh
rtk env PATH=/opt/homebrew/opt/apr/bin:/opt/homebrew/opt/apr-util/bin:$PATH \
  ./configure \
  --with-apr=/opt/homebrew/opt/apr/bin/apr-1-config \
  --with-apr-util=/opt/homebrew/opt/apr-util/bin/apu-1-config \
  --with-sofia-sip=/opt/homebrew/opt/sofia-sip
rtk make -C plugins/demo-recog check
```

结果：`demorecog.la` 与 4 个测试程序实际编译；4/4 通过，0 skip、0 fail、0 error。

### 3.3 transport、控制与音频回归（通过）

覆盖项包括：

- HTTP 逐字节读取、非法/超限 header、握手后同包 WebSocket frame；
- WebSocket masked/extended frame、fragment、Ping/Pong、Close、协议和大小错误；worker 明确拒绝服务端 masked frame；
- 8 kHz→16 kHz mono/stereo、奇数字节、跨帧 carry，以及分块/一次性重采样结果一致性；
- 1-byte RX、精确 PCM 字节顺序、STOP 尾部和唯一空 binary frame；
- ring generation wrap、满队列显式失败、fake-clock write stall、无结果超时；
- final/STOP 先后顺序、终态提交窗口 late STOP、陈旧 generation、重复 terminal、close fence；
- 握手写入停滞时 STOP 在 drain deadline 内终止，并且 metrics 先于唯一 drained 终态；
- 音频部分发送期间的多个 Ping 合并为最新 payload，并在音频之后发送唯一 Pong；
- 20 个独立 transport worker，每通道 50 帧、有效音频、最终结果和关闭 fence，零 overrun。

### 3.4 loopback fixture（工具验证通过，UMC 集成阻断）

```sh
rtk python3 tools/diagnostics/funasr_ws_fixture.py --self-test
rtk python3 -m py_compile tools/diagnostics/funasr_ws_fixture.py
rtk bash -n tools/stress/stress_test_improved.sh
rtk bash tools/stress/stress_test_improved.sh -h
```

结果：fixture 8/8 自测通过；Python 编译与 Shell 语法通过；旧参数和默认值保留，新参数仅为增量。聚合器按 `call_id/session_id` 关联 fixture 与插件记录；显式指定的 server log 不含 transport metrics 时会失败，不会静默生成全零报告。

生成 `/tmp/tte-mrcp-issue-2-unimrcpserver-v1.xml` 后使用独立 XML 解析确认：恰好一个 `Demo-Recog-1`，且 `funasr-host=127.0.0.1`、`funasr-port=8022`、`funasr-path=/ws/audio`。fixture 默认拒绝非回环监听、拒绝覆盖源文件或已有目标文件。

未生成正式 `/tmp/tte-mrcp-issue-2-pacing.json`：缺少可运行的当前 UMC/Server 二进制，受下述公共层构建问题阻断。该状态是“阻断”，不是“通过”。

## 4. 已知阻断与边界

| 门禁 | 状态 | 证据 |
|---|---|---|
| macOS 插件 CMake 构建与 CTest | 已验证 | 插件测试 4/4 通过 |
| macOS 插件 Autotools 构建与 `make check` | 已验证 | 插件本体编译，测试 4/4 通过 |
| macOS 完整 CMake build | 阻断 | 公共 `libs/apr-toolkit` / `libs/mpf` 未获得 APR-util include，`apr_xml.h` not found；Issue #2 插件目标尚未进入失败点 |
| macOS 完整 Autotools `make check` | 阻断 | 已知公共层 `JB_TRACE/RTP_TRACE` 向 `mpf_null_trace()` 传参导致编译失败 |
| 20 个真实 UMC ASR 会话 | CI 已定义，首跑待回填 | 由 `.github/workflows/build-linux.yml` 的 `verify-linux` job 在 Rocky Linux 8 容器内以 1 次预热 + 20 并发 split-payload loopback 执行；绿色运行后回填 pacing JSON 与阈值断言结果 |
| Windows Win32/x64 | 未验证 | 当前环境无 Windows/MSBuild；工程 XML 已解析，solution 已接入新源文件和测试项目 |
| Linux 目标发行版 | CI 已定义，首跑待回填 | `build-linux` 在 ABI 基线容器（RHEL 7 / Rocky Linux 8）完成 Autotools 构建、`make check`（kylin 行含 CMake/CTest）与 ELF/GLIBC/ldd 审计；`verify-linux` 在 Rocky Linux 8 容器执行插件加载、RTP/MRCP 建链冒烟和 20 并发 ASR loopback，断言零 overrun、非故障 p99 `<100 ms`、最大值 `<250 ms`；Actions 首次绿色运行后回填证据 |
| 生产灰度 | 线上未验证 | 未提供真实灰度流量与服务端日志 |

上述两个完整构建阻断均位于未修改的公共层，不归因于本 Issue 的插件实现。本变更没有为绕过门禁而扩大到公共库修复。

## 5. 卫生与图谱证据

- `rtk git diff --check`：通过。
- `tools/` 下全部 Shell 脚本逐个 `rtk bash -n`：通过。
- Visual Studio `.vcproj`/`.vcxproj`/`.filters` 共 9 个 XML 文件解析通过。
- 活动插件源码未发现 `Recv audio`、`Sending WebSocket chunk`、`fwrite(stderr` 或 `hex dump` 默认逐帧/载荷日志。
- codebase-memory fast index：项目 `tte-mrcp-issue-2`，5,792 nodes、33,740 edges。
- canonical `funasr_transport_*` 节点共 25 个；`funasr_transport_enqueue_pcm`、`funasr_transport_enter_finishing` 和 `funasr_transport_worker` 均已索引。
- `funasr_stream_write` outbound 一跳仅到媒体 snapshot、resample、monotonic clock、enqueue 和失败 latch；没有 socket send/recv、WebSocket parser、JSON parser 或 MRCP event API。

## 6. 后续上线门禁

合并前仍需在受支持的 Windows builder 上完成对应 solution 构建和插件加载；Linux 门禁已定义为 `.github/workflows/build-linux.yml`：

- `build-linux`：在 ABI 基线容器（RHEL 7 / Rocky Linux 8）内完成 Autotools 构建与 `make check`（kylin 行另跑 CMake/CTest 插件测试），并对打包产物执行 ELF/GLIBC 与 `ldd` 审计。
- `verify-linux`：在 Rocky Linux 8 容器内启动 loopback fixture（split-payload 故障注入）与打包后的 `unimrcpserver`，确认 MRCPv2 profile 就绪、插件加载无失败，然后运行 1 次预热 + 20 个并发 UMC 会话，以 fixture JSONL 与插件结构化 summary 生成 pacing JSON。
- 只有 Actions 首次绿色运行满足零 overrun、非故障样本 gap p99 `<100 ms`、最大值 `<250 ms`，且故障会话在 5 秒 deadline 内终止，才把 Linux 与端到端项改为通过。
