## 快速构建

服务器使用 GitHub Release/Actions Artifact 部署和绿灯测试见 [Release Artifacts 部署与绿灯测试](docs/deployment/release-artifacts.md)。

Agent 进行后续排查、优化和开发时，请先阅读 [AGENTS.md](AGENTS.md)；其中的架构边界、SSOT 规则和验证门禁以当前源码为准。

```sh
./tools/dev/setup_macos.sh
. ./tools/dev/env-macos.sh
./bootstrap
rm -rf build/local
mkdir -p build/local
cd build/local
../../configure --prefix="$PWD/install"
make -j"$(sysctl -n hw.ncpu)"
```

如果使用 CMake 4.x，需要允许当前工程的旧 CMake policy：

```sh
. ./tools/dev/env-macos.sh
cmake -S . -B /tmp/tte-mrcp-cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5
```

配置运行时请从构建目录启动，并将源码根目录作为 `-r` 参数：

```sh
cd build/local
./platforms/unimrcp-server/unimrcpserver -r ../.. -l debug
./platforms/umc/umc -r ../.. -l 6 -o 1
```

默认 SIP 端口为 8060，默认 RTSP 端口为 1544。诊断入口：

```sh
./tools/diagnostics/diagnose.sh
```

0724:性能压测
3. `tools/stress/stress_test_improved.sh` — 主要增强（根目录 `stress_test_improved.sh` 仍保留兼容入口）
新增参数：

参数	说明	默认值
-a <dir>	ASR 音频源目录	$ROOT_DIR/data
-o <file>	ASR 结果 CSV	$ROOT_DIR/stress_results/asr_results.csv
-s <rate>	采样率 (8000/16000)	16000
--tts-save	保留 TTS 录音	默认清空
--tts-var-dir=<dir>	TTS 输出目录	$ROOT_DIR/var
关键逻辑：

轮询选择音频：按迭代次数从音频目录中 index = (iter-1) % file_count 轮询
symlink 机制：data/stress_test_input.pcm → 目标音频（recogsession.cpp 通过 XML 中的 audio-source 找到该文件）
采样率检测：从文件名 *-{rate}kHz.pcm 自动识别
CSV 结果：timestamp, iteration, audio_file, sample_rate, status, recognized_text, log_file
TTS 录音管理：默认每次清空 var/，--tts-save 保留历史录音
使用示例

# TTS 压测（录音保存到 var/）
./tools/stress/stress_test_improved.sh -t tts -i 10 --tts-save

# ASR 压测（使用默认 data/ 目录下的音频）
./tools/stress/stress_test_improved.sh -t asr -i 20

# ASR 压测（自定义音频文件夹 + 结果输出）
./tools/stress/stress_test_improved.sh -t asr -i 30 -a /path/to/audio_folder -o /tmp/results.csv

# 完整压测
./tools/stress/stress_test_improved.sh -t all -i 20 -a /path/to/audio --tts-save

## 本机环境（2026-08-04）

本目录当前在 Apple Silicon macOS 上维护：Darwin 25.5.0、macOS 26.5.1、arm64。已确认的工具和版本：

| 工具 | 本机状态 |
|---|---|
| GNU Make | 3.81 (`/usr/bin/make`) |
| C 编译器 | Apple clang 21.0.0（`gcc` 和 `clang` 命令均指向 Apple clang） |
| Python | 3.14.6（Homebrew 环境加载后） |
| Expect | `/opt/homebrew/bin/expect` |
| pkg-config | 3.0.5；可发现 `apr-1` 1.7.6、`apr-util-1` 1.6.3、`sofia-sip-ua` 1.13.17 |
| CMake | 4.4.2 (`/opt/homebrew/bin/cmake`) |
| Autoconf/Automake | 2.73 / 1.18.1 |
| Libtool | `glibtoolize` 2.5.4 |
| Sofia-SIP | 已安装并可由 `pkg-config` 发现 |

本机已通过 `./tools/dev/setup_macos.sh`、Autotools 配置和 CMake 配置解析。完整 Autotools 编译目前在既有 `libs/mpf` 的 `JB_TRACE`/`RTP_TRACE` 宏与当前 clang 兼容性处失败；这属于既有源码构建问题，不是插件改名或依赖探测失败。

配置本机开发环境：

```sh
./tools/dev/setup_macos.sh
. ./tools/dev/env-macos.sh
```

依赖清单、Homebrew 前缀处理和构建入口见 [tools/dev/README.md](tools/dev/README.md)。环境脚本不会写入用户全局 shell 配置。

## 跨平台部署基线

本项目按“macOS/Windows 开发、Linux 生产”组织验证：

| 环境 | 用途 | 构建/验证入口 |
|---|---|---|
| macOS Apple Silicon | 本地开发、协议测试和问题复现 | `tools/dev/setup_macos.sh`、Autotools、CMake |
| Windows Win32/x64 | Visual Studio 开发和兼容性验证 | `unimrcp.sln`、`unimrcp-2010.sln`、MSBuild/CTest |
| Linux 目标发行版 | 生产构建、部署和运行 | Autotools 为主，验证 `.so` 加载、RTP/MRCP 建链和 TTS 冒烟 |

macOS Homebrew 路径只允许出现在本机开发脚本和本地生成目录中，不得进入共享源码、CMake、Autotools 或运行配置。跨平台修复必须分别验证源码可移植性、构建可移植性和运行时可部署性；macOS 通过不代表 Linux 生产或 Windows 开发通过。

当前本机会话已实际验证 macOS 环境；Windows 编译和 Linux 生产运行仍需在对应环境或等价构建环境中执行，不能将其标记为已验证。

## 正式 TTS 插件

当前正式 TTS WebSocket 插件名称为 `tts-websocket` / `tts_websocket`：

- 源码目录：`plugins/tts-websocket/`
- 插件动态库：`tts_websocket.so`
- 配置引擎：`TTS-WebSocket-1`，注册名 `tts_websocket`
- 主要源码：`plugins/tts-websocket/src/tts_websocket_engine.c`

`demo_*` 仅用于 UniMRCP 示例客户端，不用于正式插件。

## 正式 ASR WebSocket 插件

当前正式 ASR 插件名称为 `asr-websocket` / `asr_websocket`，负责将 MRCP `speechrecog` 资源接入 FunASR WebSocket 服务：

- 源码目录：`plugins/asr-websocket/`
- 插件动态库：`asr_websocket.so`（Windows 为对应 DLL）
- 配置引擎：`ASR-WebSocket-1`，注册名 `asr_websocket`
- endpoint 参数：`funasr-host`、`funasr-port`、`funasr-path`
- 诊断入口：`tools/diagnostics/diagnose.sh`

历史配置 `Demo-Recog-1` / `demorecog` 迁移为 `ASR-WebSocket-1` / `asr_websocket`；FunASR endpoint 参数不变。当前发布不提供旧动态库或 engine 的运行时别名：部署升级必须原子替换 XML 与插件库，不能用旧 XML 加载新安装。兼容窗口仅覆盖 loopback fixture 的旧输入迁移；旧名称从本发布版本起退出正式构建，不延长到后续 release。真实生产灰度和外部服务运行验收需由部署方人工确认，macOS 本地通过不代表 Windows/Linux 运行时已验证。

## 目录约定

- `libs/`、`modules/`、`plugins/`、`platforms/`：运行时源码和框架模块；插件源码只从各自的 `src/` 编译。
- `plugins/tts-websocket/`：正式 TTS WebSocket 插件；其构建目标和注册名均为 `tts_websocket`。
- `plugins/asr-websocket/`：正式 ASR WebSocket 插件；其构建目标和注册名均为 `asr_websocket`。
- `tests/`：框架测试和协议测试；`tests/integration/` 存放依赖已编译 UMC 的 Expect 集成测试。
- `tools/stress/`：ASR/TTS 压测脚本；根目录同名脚本仅为兼容包装。
- `tools/diagnostics/`：本机服务、端口和插件诊断脚本。
- `conf/`、`data/`：可审查的运行配置和测试输入数据。
- `shell/`：服务启停、健康检查和日志维护脚本。
- `docs/reports/`：源码分析、故障排查等项目报告。
- `docs/superpowers/`：整理和重构的设计、执行计划。
- `build/local/`、`plugin/`、`var/`、`.libs/`、`.deps/`、对象文件和运行日志：本地生成或运行产物，不进入版本库；`build/` 根目录保留 Autotools 必需的源码输入；历史副本见 `.archive/`。
