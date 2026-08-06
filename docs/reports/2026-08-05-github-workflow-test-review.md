# GitHub Workflow 测试目标与场景完整性复核报告

日期：2026-08-05
审查范围：`.github/workflows/build-linux.yml` 及其直接调用的测试、诊断、场景和构建输入
审查方式：当前源码静态复核、调用关系复核、测试注册关系复核；不把历史报告作为行为证据

## 1. 结论摘要

当前 workflow 的真实定位是：

```text
两个 Linux ABI 矩阵目标
  -> 目标容器内 Autotools 构建、安装、ELF/GLIBC/依赖检查
  -> RHEL7 行执行 demo-recog Autotools 单测
  -> AArch64 行执行 demo-recog + tts-websocket CMake/CTest 单测
  -> 两个目标均解包后启动服务
  -> loopback FunASR WebSocket + 20 并发 ASR 场景 + pacing 阈值
```

它不是全量测试，也没有证明正式 TTS 的完整运行链路。当前最重要的问题如下：

| 等级 | 问题 | 影响 |
|---|---|---|
| P0 | `tools/diagnostics/funasr_ws_fixture.py:393-404` 调用的 `parse_server_metrics()` 当前函数体只有 `return None`；workflow 随后把 `server.log` 传给 `--aggregate`。`aggregate()` 会因没有 metrics 直接抛错 | runtime verify 的 pacing 汇总按当前源码无法成功，CI 不能完成其声明的运行时验收 |
| P1 | TTS 单测只在 `kylinv10-aarch64` 的 CMake gate 执行；RHEL7 行完全不执行 TTS 单测 | RHEL7 ABI 目标的正式 TTS 协议、PCM、WebSocket 和生命周期回归未被验证 |
| P1 | runtime verify 的端到端场景只配置 `Demo-Recog-1 -> loopback fixture`，没有配置 `TTS-WebSocket-1` 的 loopback backend，也没有发起 `SPEAK` | TTS 的 MRCP、WebSocket 服务交互、PCM 到 PCMU、RTP 输出、post-roll 和 `SPEAK-COMPLETE` 未被 CI 证明 |
| P1 | RHEL7 包在 `rockylinux:8` verify image 中运行；该 image 与构建 ABI 基线不一致 | 只能证明在 Rocky 8 运行容器可启动，不能严格证明 RHEL7 运行时兼容性 |
| P2 | workflow 只检查日志中的 ready/load 错误和 ASR pacing；没有将包内配置、插件加载、RTP 收包和 TTS 完成原因建立成独立断言 | 测试失败时可能只得到“进程启动/日志出现”级别的信号，定位和覆盖完整性不足 |

## 2. Workflow 实际定义的测试目标

### 2.1 触发与矩阵

文件 `.github/workflows/build-linux.yml:3-41` 定义：

- 触发：面向 `main` 的 pull request、推送到 `main`、手动触发。
- `fail-fast: false`：两个目标独立完成，便于同时观察失败。
- `rhel7-x86_64`：CentOS 7 构建容器，目标 GLIBC 上限 2.17，要求 GCC 4.8.5；关闭 CMake gate。
- `kylinv10-aarch64`：Rocky Linux 8、ARM runner，目标 GLIBC 上限 2.28，开启 CMake gate。
- 两行都声明 `verify_image: rockylinux:8`，但该字段只在 verify job 的矩阵中使用。

矩阵设计能验证两个目标架构和不同 GLIBC 上限，但测试方法不对称：RHEL7 目标没有执行 TTS CMake 测试，且两个目标的 runtime verify 使用相同的 Rocky 8 容器。

### 2.2 构建与包结构测试

`.github/workflows/build-linux.yml:173-207` 的目标和证据：

1. 重新生成 Autotools 输入：`./bootstrap`。
2. 固定 sofia-sip tag 和 commit，并从源码构建安装。
3. 目标容器内执行 `configure`、`make`、`make install DESTDIR`。
4. 检查 `unimrcpserver`、`umc`、`unimrcpclient`、`asrclient` 可执行文件和 `lib/plugin/conf/data` 目录存在。
5. 设置包内 `LD_LIBRARY_PATH`，复制非系统依赖到包内 `lib`。
6. 使用 `readelf` 检查 ELF machine 和 GLIBC 版本上限。
7. 使用 `ldd` 检查 `not found`。
8. 打包 ZIP，并列出 ZIP 内容。

这些是有价值的交付物和 ABI 检查，但它们主要证明“编译、安装、依赖可解析、包形态正确”，不是业务行为测试。特别是 `test -d plugin`、`test -d conf` 等不等于 plugin factory 已成功加载每个目标插件。

### 2.3 Autotools 单测 gate

`.github/workflows/build-linux.yml:209-217` 只运行：

```sh
make -C build/ci/plugins/demo-recog check
```

该目标来自 `plugins/demo-recog/Makefile.am:17-20`，注册 4 个测试：

- `test_resample`
- `test_json_unescape`
- `test_funasr_ws_transport`
- `test_funasr_control`

注意：`plugins/tts-websocket/Makefile.am:1-8` 没有 `check_PROGRAMS` 或 `TESTS`，所以 Autotools gate 当前不会执行正式 TTS 单测。workflow 中的注释“Autotools gate: demorecog check_PROGRAMS (4 tests)”与当前行为一致，但也明确暴露了 TTS 的 Autotools 测试缺口。

### 2.4 CMake/CTest gate

`.github/workflows/build-linux.yml:223-249` 仅在 AArch64 目标执行，显式开启：

- `BUILD_DEMORECOG_TESTS=ON`
- `BUILD_TTS_WEBSOCKET_TESTS=ON`

显式构建 8 个测试目标，并用 `ctest -R '^(demorecog_|tts_websocket_)'` 执行注册测试。注册关系来自：

- `plugins/demo-recog/CMakeLists.txt:70-105`：4 个 demorecog 测试。
- `plugins/tts-websocket/CMakeLists.txt:62-92`：4 个 TTS 测试。

该 gate 对 AArch64 的 TTS 测试覆盖是完整的，但有两个边界：

1. CTest 只证明测试 executable 的行为，不证明安装到 package-root 后的 `tts_websocket.so` 能被 server 运行时加载。
2. RHEL7 行关闭 gate，因此同一套 TTS 测试没有在 GLIBC 2.17/GCC 4.8.5 目标上执行。

## 3. 单元测试目标与覆盖路径

### 3.1 正式 TTS WebSocket 插件

| 测试 | 直接覆盖 | 已覆盖的关键路径 | 未覆盖/不足 |
|---|---|---|---|
| `test_tts_websocket_http_parse` | HTTP response status、header/body separator、Content-Length、chunk-like header、binary body、large body、buffer growth、empty body | HTTP 解析辅助逻辑的边界和短数据 | 从真实 socket 完成 HTTP handshake、错误响应到 MRCP completion 的联动 |
| `test_tts_websocket_pcm` | odd chunks、1-byte chunks、carry 保留 | `tts_websocket_pcm_accumulate()` 的跨输入边界字节完整性 | 24 kHz 到 8 kHz 重采样、PCMU 编码、codec frame 对齐、真实 stream read 输出 |
| `test_tts_websocket_lifecycle` | close 后拒绝 callback、close 等待 active callback | `tts_websocket_stream_lifecycle_*()` 的原子计数和关闭门 | worker/socket/ring/pool 的完整销毁顺序；没有真实 MPF callback 竞态 |
| `test_tts_websocket_ws` | short read、handshake surplus、fragmentation、Ping/Pong | `tts_websocket_ws_decoder_recv_message()` 的帧头、粘包、控制帧、fragment | close reason、masked/unmasked 组合的全面矩阵、超大 64-bit length、实际 TLS/HTTP handshake |

TTS 源码的关键生产路径由图谱定位为：

```text
tts_websocket_channel_request_dispatch
  -> tts_websocket_channel_speak
  -> tts_websocket_stream_thread
  -> tts_websocket_stream_write_audio
  -> tts_websocket_stream_read_safe
  -> MPF/RTP
  -> SPEAK-COMPLETE
```

其中 `tts_websocket_stream_thread` 复杂度高、包含循环和读写状态机；`tts_websocket_stream_read_safe` 负责固定 PCMU frame、silence/post-roll/drain 和 completion。现有 TTS 单测没有直接触达这两个 engine 函数，也没有测试 MRCP `SPEAK` 到 RTP 输出的完整行为。因此“协议辅助逻辑已测”不能等价为“正式 TTS 已验收”。

### 3.2 Demo-recog/FunASR transport

该部分是当前 CI 覆盖最强的区域：

- `test_resample`：mono/stereo 时长公式、插值连续性、奇数字节 carry、分割 stereo frame、非法参数/容量失败不修改状态、分块与一次性结果一致。
- `test_json_unescape`：有界整数、无 NUL buffer、字符串和 Unicode、非法/受限字符串。
- `test_funasr_ws_transport`：HTTP bytewise header、sticky frame、大小写不敏感 token、WebSocket short read/mask、extended length、fragment + interleaved ping、协议/size error、ring wrap/generation/overrun、partial RX 时保持 media enqueue、Pong 排队、STOP drain、natural endpoint、20 worker 独立性、sticky frame、queue overrun、write stall、STOP 中断 stalled handshake、close fence。
- `test_funasr_control`：final-before-stop、stop-before-final、timeout、失败映射、stale generation、STOP/failure race、terminal commit、close fence。

这些测试对纯函数、协议状态机和 fake I/O 的内部行为覆盖充分；但它们是 mock/fake transport 场景，不能独立证明真实 `unimrcpserver`、SIP/MRCP、MPF/RTP 和动态插件加载。

## 4. Runtime verify 场景复核

### 4.1 设计意图

`.github/workflows/build-linux.yml:289-291` 的注释声明 runtime verify 包含：

- packaged tree plugin load；
- RTP/MRCP link smoke；
- Issue #2 20-session loopback fixture 和 pacing thresholds。

实际步骤是：

1. 解包 artifact。
2. 执行 fixture `--self-test`。
3. 对包内 ELF 和 plugin 做依赖检查。
4. 用 `emit-config` 把 `Demo-Recog-1` 指向 `127.0.0.1:8022/ws/asr`。
5. 启动 split-payload fixture，并让 connection 2 走 fault mode。
6. 从包内启动 `unimrcpserver`，等待 `Create MRCPv2 Profile`。
7. 检查 server.log 中没有两个配置/插件加载错误。
8. 创建包内 `umc` wrapper，调用 `stress_test_improved.sh -t asr -c 20 -i 1 --warmup=1`。
9. 汇总 pacing JSON，检查 requests、overrun、abnormal close、successes、p99 和 max gap。

### 4.2 实际证明的路径

```text
unimrcpserver packaged binary
  -> generated config
  -> Demo-Recog plugin
  -> MRCPv2 profile / speechrecog resource map
  -> UMC ASR request
  -> RTP input from UMC
  -> FunASR WebSocket loopback fixture
  -> fake final result
  -> transport metrics / pacing aggregation
```

它有效覆盖：服务进程可启动、配置可解析到 profile 就绪、Demo-Recog WebSocket 可以 loopback、20 个 ASR session 的并发独立性、split payload、指定连接故障、queue overrun/abnormal close/pacing 指标的意图。

### 4.3 不能证明的路径

当前 `emit_config()` 只删除/重建 `Demo-Recog-1`，没有把 `TTS-WebSocket-1` 指向 loopback fixture；runtime stress 使用 `-t asr`，所以以下路径未被端到端执行：

```text
SPEAK
  -> tts_websocket_channel_request_dispatch
  -> tts_websocket_channel_speak
  -> WebSocket handshake/send text
  -> receive 24 kHz PCM
  -> resample 8 kHz / PCMU
  -> tts_websocket_stream_read_safe
  -> RTP output
  -> post-roll/RTP drain
  -> SPEAK-COMPLETE
```

此外，runtime verify 没有通过 RTP 接收器或音频断言检查“有效音频字节/静音/丢帧”的实际内容；pacing report 的字段主要来自 ASR transport metrics。

## 5. 当前源码中的 CI 阻断与方法问题

### P0：pacing aggregation 当前实现不可用

位置：`tools/diagnostics/funasr_ws_fixture.py:393-404`。

`aggregate()` 执行：

```python
metrics = parse_server_metrics(server_log)
if server_log is not None and not metrics:
    raise ValueError(f"no transport metrics found in {server_log}")
```

但当前 `parse_server_metrics()` 的函数体为 `return None`。workflow 在 `.github/workflows/build-linux.yml:469-473` 传入 `--server-log=server.log`，脚本在 `.github/workflows/build-linux.yml:475-506` 立即执行聚合断言。因此按当前源码实际运行，20 并发请求即使全部完成，也会在 pacing 汇总阶段失败。

建议：实现并测试 server metrics parser；至少增加一个带真实日志行的 self-test，验证 `server_log -> metrics -> pacing.json` 全链路，而不仅是“无关日志应失败”的反例测试。

### P1：TTS 测试目标跨矩阵不完整

位置：`.github/workflows/build-linux.yml:29-40`、`:211-249`。

RHEL7 行关闭 CMake gate，同时 Autotools 未注册 TTS tests。结果是 RHEL7 ABI 目标只执行 demo-recog 的 4 个测试，正式 TTS 的 4 个测试完全缺席。对于 TTS 插件位于正式业务边界的项目，这不能作为两个 Linux 目标均通过 TTS 回归的证据。

建议：为 TTS 建立 Autotools `check_PROGRAMS/TESTS`，或在 RHEL7 行使用兼容的独立编译/测试入口；至少把 TTS 四个测试 executable 在 RHEL7 容器中构建并执行。

### P1：TTS 端到端场景缺失

位置：`.github/workflows/build-linux.yml:385-400`、`:463-473`。

运行时配置只保留 `Demo-Recog-1`，压测参数固定为 `-t asr`。因此 workflow 的“RTP/MRCP link smoke”实际是 ASR 方向的 loopback，不是 TTS 验收。TTS backend 仍然来自默认配置中的外部地址时，也没有被替换为 loopback fixture；不会形成可重复、无外部依赖的 TTS CI 场景。

建议：增加 loopback TTS fixture，支持短 PCM、分块、延迟、Ping/Pong、Close、空帧和异常结束；生成只保留 `TTS-WebSocket-1` 的 verify 配置；运行 UMC synth，并分别断言 SPEAK-COMPLETE、RTP 收包、音频字节数、静音/丢帧和 completion cause。

### P1：RHEL7 构建包没有在 RHEL7 runtime image 验证

位置：`.github/workflows/build-linux.yml:21-31`、`:300-309`、`:340-365`。

RHEL7 目标用 CentOS 7 构建，但 verify matrix 将它与 AArch64 一样放入 `rockylinux:8`。这会掩盖 RHEL7 的 loader、libcrypt、libfreebl、系统库版本和实际动态加载差异。当前注释只说明对 RHEL7 包做了兼容依赖处理，并不能把 Rocky 8 变成 RHEL7。

建议：RHEL7 使用仍可获取的 RHEL7/CentOS 7 vault runtime image，AArch64 使用与目标发行版匹配的 runtime image；如果 GitHub runner 无法原生运行目标架构，应明确把“容器中执行”与“目标机执行”分成两种证据，不要合并成同一 runtime 结论。

### P2：运行时断言没有覆盖插件加载和音频结果的全部证据

位置：`.github/workflows/build-linux.yml:417-451`、`:475-506`。

`Create MRCPv2 Profile` 只能说明 profile 创建日志出现；错误 grep 只覆盖两个字符串。没有断言：

- 每个预期 plugin 的 factory load 成功；
- resource-engine-map 的实际映射；
- SIP/MRCP 请求收到 200/成功 response；
- RTP 端口实际收发；
- TTS/ASR 完成事件与 completion cause 的一一对应；
- 每个 session 都有 metrics，且 metrics session id 与 fixture call id 成功关联。

建议：把启动、插件加载、协议请求、媒体收发、完成状态分别设为独立 gate，并上传对应日志/抓取摘要；不要用单条 ready 日志代表整条链路。

## 6. 测试完整性矩阵

| 能力/路径 | 测试代码存在 | AArch64 CI | RHEL7 CI | packaged runtime | 结论 |
|---|---:|---:|---:|---:|---|
| TTS HTTP parser | 是 | 是 | 否 | 否 | 仅 AArch64 单元覆盖 |
| TTS PCM carry | 是 | 是 | 否 | 否 | 未覆盖重采样/PCMU/RTP |
| TTS WebSocket short-read/fragment/control | 是 | 是 | 否 | 否 | 未覆盖真实握手和 engine 联动 |
| TTS lifecycle | 是 | 是 | 否 | 否 | 未覆盖完整 channel cleanup 竞态 |
| FunASR resample/JSON | 是 | 是 | 是 | 间接 | 有单测，未覆盖所有包内构建变体 |
| FunASR WebSocket/ring/worker | 是 | 是 | 是 | 是 | 当前最强覆盖区域，但受 metrics parser 阻断 |
| FunASR control race | 是 | 是 | 是 | 是 | fake transport + loopback runtime 两层 |
| server 配置解析/profile ready | 否（无专门断言） | 间接 | 间接 | 是 | 只有日志级 smoke |
| 动态 plugin load | 构建产物存在 | 间接 | 间接 | 部分 | 未按 plugin/resource 逐项断言 |
| MRCP/SIP 建链 | 场景脚本存在 | 是（ASR） | 是（ASR） | 是（ASR） | 只验证 ASR 方向 |
| RTP 媒体收发 | 场景配置存在 | 间接 | 间接 | 未独立断言 | 没有 RTP receiver/内容断言 |
| TTS SPEAK -> RTP -> COMPLETE | 代码存在 | 否 | 否 | 否 | 主要完整性缺口 |
| MRCPv1 / recorder / verifier / mrcptest | 测试资产存在 | 否 | 否 | 否 | 不属于当前 workflow 的有效 gate |
| Windows / macOS | 各自工程/脚本存在 | 否 | 否 | 否 | 本 workflow 不提供跨平台证据 |

## 7. 测试方法评价

### 优点

1. **目标 ABI 构建与单元测试同容器执行。** 这避免只在宿主机编译、在目标容器里运行的错位，ELF machine 和 GLIBC 上限检查也有明确失败条件。
2. **外部 ASR 服务被 loopback fixture 替代。** 运行时场景可重复，且 fixture 支持 split payload、指定连接故障、JSONL 记录。
3. **并发测试有真实的 worker 隔离设计。** `stress_test_improved.sh` 为 worker 准备独立 SIP/RTP 端口和运行目录，避免把端口冲突误判为业务并发问题。
4. **测试分层较清晰。** C 单测验证内部协议/状态，fake I/O 验证并发和时序，打包后 runtime verify 验证部署形态。
5. **错误指标方向正确。** overrun、abnormal close、p99/max gap、valid audio bytes 等指标比只检查进程退出码更接近媒体系统质量。

### 方法缺陷

1. **测试注册和 CI 调度不对称。** CMake 注册了 TTS 测试，但 Autotools 没有；矩阵开关又导致 RHEL7 完全跳过 TTS。
2. **结果汇总器缺少正向 parser 测试。** self-test 只有无关日志应失败的反例，不能证明生产日志能被解析。
3. **“成功”语义仍分散。** expect 脚本、stress shell、fixture aggregate 各自解析完成状态；没有统一的 session-level schema 来同时绑定 MRCP completion、RTP 音频和 WebSocket outcome。
4. **端到端场景偏向 ASR。** 对正式 TTS 业务最关键的输出媒体链路没有对应的可控 backend 和验收断言。
5. **运行时环境证据不匹配。** build ABI、verify image、runner 架构和实际生产发行版没有一一对应的矩阵标签。
6. **完整性依赖日志文本。** 例如等待 `Create MRCPv2 Profile` 和 grep 少量错误字符串，容易受到日志级别、文案变化和异步输出顺序影响。

## 8. 建议的整改顺序

### P0：先恢复 CI 的真实可判定性

1. 实现 `parse_server_metrics()`，支持当前 `METRICS_RE` 对应的 metrics 日志格式。
2. 为 parser 和 aggregate 添加正向 fixture 测试：多 session、warmup、fault connection、缺失 session、重复 session、空 histogram。
3. 在 workflow 中增加 `test -s pacing.json` 和 `plugin_metric_records == requests` 等完整性断言，避免只看 p99。

### P1：补齐正式 TTS 的两个门

1. 在两个 ABI 目标执行 TTS 四个单测；优先补 Autotools test registration，减少 workflow 对构建系统的偏置。
2. 增加 loopback TTS WebSocket fixture，并提供正常、split PCM、短帧/奇数字节、延迟、Ping、Close、服务端错误等模式。
3. 运行 packaged `umc synth`，将验收拆成：MRCP completion、RTP packet/audio bytes、静音/丢帧、post-roll/drain、completion reason。

### P1：修正运行时镜像矩阵

1. RHEL7 artifact 在 RHEL7-compatible image 中启动和加载 plugin。
2. AArch64 artifact 在 AArch64 compatible image 中启动；保留当前 runner 只在其能真正执行目标 ELF 时使用。
3. 在报告和 workflow summary 中分别标注 build ABI、runtime image、runner architecture、是否为真实目标机。

### P2：把运行时断言结构化

1. 为 server 输出结构化的 plugin-load/profile-ready 事件，避免依赖单条自然语言日志。
2. 为每个请求输出 session id、MRCP completion cause、RTP packet/audio byte summary 和 fixture outcome。
3. 将 smoke、单元、并发、音频质量、包部署分别命名为独立 job/step，报告中不要把它们合称为“runtime verify”。

## 9. 复核边界与未执行项

- 本次复核没有修改 workflow、测试代码或配置。
- 未在本机重建两个 Linux 容器，也未执行真实外部 TTS 服务调用；因此本报告不宣称线上或目标发行版运行通过。
- 代码图谱已用于定位 TTS canonical 节点、测试模块和关键调用关系；workflow、Makefile/CMake、XML、shell/Python 场景使用当前文件系统文本作为事实来源。
- 当前报告结论以源码事实为主；“P0 阻断”来自 `parse_server_metrics()` 的当前实现与 `aggregate()` 的直接调用关系，而不是历史运行日志推断。
