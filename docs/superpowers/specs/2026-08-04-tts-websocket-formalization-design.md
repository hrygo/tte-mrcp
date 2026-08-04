# TTS WebSocket 正式插件化设计

## 目标

将当前承担正式 TTS WebSocket 业务的 `demo-synth` 全链路改为正式插件名称 `tts-websocket` / `tts_websocket`，同时补齐本机 macOS arm64 开发环境的可重复配置入口。

## SSOT 与命名边界

- 运行时源码、构建文件和 `conf/unimrcpserver.xml` 是插件行为与注册名的 SSOT。
- 正式插件目录：`plugins/tts-websocket/`。
- CMake/Autotools 目标和插件注册名：`tts_websocket`。
- 动态库产物：`tts_websocket.so`（Autotools 的中间目标为 `tts_websocket.la`）。
- 主要源码：`src/tts_websocket_engine.c`。
- 引擎配置 ID：`TTS-WebSocket-1`。
- `demo_*` 仅保留给 UniMRCP 示例客户端框架，不再用于正式 TTS 插件。

## 本机开发环境

使用 Homebrew 原生 arm64 安装 APR、APR-util、Sofia-SIP、CMake、Autoconf、Automake、Libtool、pkg-config 和 Expect。`tools/dev/env-macos.sh` 只负责根据 Homebrew 前缀导出 `PATH`、`PKG_CONFIG_PATH`、`CPPFLAGS` 和 `LDFLAGS`，不写入用户 shell 配置；`tools/dev/setup_macos.sh` 负责可重复执行依赖安装和环境检查。

构建系统优先支持现有 Autotools；CMake 配置移除 `/usr/local/opt` 硬编码，改用 pkg-config 和环境脚本提供的依赖路径。

## 迁移策略

1. 重命名插件目录、源码、测试、Visual Studio 工程和构建目标。
2. 更新 `configure.ac`、`Makefile.am/in`、顶层 CMake、解决方案文件、诊断脚本和 XML 配置。
3. 将 C 符号从 `demo_synth_*` 改为 `tts_websocket_*`，避免正式代码内部仍带示例语义。
4. 不同时构建旧、新两个插件，避免同一 MRCP 引擎重复注册；部署升级时直接替换旧动态库并使用新 XML 名称。
5. 通过文本引用检查、脚本语法检查、构建配置检查和插件名称图谱查询验收。

## 不在本次范围

- 不修改 WebSocket、PCM、并发或音频时序业务逻辑。
- 不重命名 UniMRCP 自带的 `platforms/unimrcp-client/src/demo_*` 示例客户端。
- 不清理与正式插件改名无关的历史文档内容；生成文档仅在必要时同步链接。
