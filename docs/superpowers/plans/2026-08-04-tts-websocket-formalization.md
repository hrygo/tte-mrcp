# TTS WebSocket Formalization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将正式 TTS WebSocket 插件从 `demo-synth/demosynth` 全链路迁移为 `tts-websocket/tts_websocket`，并提供可重复的 macOS arm64 本机开发环境配置。

**Architecture:** 保持 UniMRCP 运行时插件接口和音频业务逻辑不变，只迁移目录、构建目标、注册名、源码符号和配置引用。环境配置采用 Homebrew Bundle + sourceable 环境脚本，不修改用户 shell 全局配置。

**Tech Stack:** C, Autotools/Libtool, CMake, Bash, Homebrew, pkg-config, macOS arm64, codebase-memory-mcp.

## Global Constraints

- 当前源码和构建配置是 SSOT；不以旧生成文档覆盖实际配置。
- 不修改 WebSocket、PCM、并发和音频时序业务逻辑。
- 生产插件不得继续使用 `demo` 作为目录、目标、注册名或内部正式符号。
- 保留 UniMRCP 示例客户端中的 `demo_*` 命名，不扩大重命名范围。
- 所有新入口必须从仓库根路径推导路径，不能依赖 `/Users/wp` 或 `/usr/local/opt`。

---

### Task 1: 本机 macOS 开发环境配置

**Files:**
- Create: `tools/dev/Brewfile`
- Create: `tools/dev/env-macos.sh`
- Create: `tools/dev/setup_macos.sh`
- Create: `tools/dev/README.md`
- Create: `AGENTS.md`
- Modify: `.gitignore`

- [x] **Step 1:** 创建 Homebrew 依赖清单和可 source 的环境脚本。
- [x] **Step 2:** 创建幂等安装/检查脚本，覆盖 Homebrew、pkg-config、APR、APR-util、Sofia-SIP、CMake、Autotools 和 Expect。
- [x] **Step 3:** 更新仓库规则和 README，记录 setup、环境加载和构建命令。
- [x] **Step 4:** 在本机运行 setup/check，记录实际安装或失败原因。

### Task 2: 正式插件目录和源码符号迁移

**Files:**
- Move: `plugins/demo-synth/` → `plugins/tts-websocket/`
- Move: `src/demo_synth_engine.c` → `src/tts_websocket_engine.c`
- Move: plugin test and Visual Studio project files to formal names
- Modify: `plugins/tts-websocket/src/tts_websocket_engine.c`

- [x] **Step 1:** 移动目录和文件，保持权限与历史可追踪性。
- [x] **Step 2:** 将 `demo_synth_*`、`DEMO_SYNTH_*` 和 `demo_tts_*` 正式插件符号改为 `tts_websocket_*`、`TTS_WEBSOCKET_*`。
- [x] **Step 3:** 更新插件测试名称、注释和工程文件引用。

### Task 3: 构建、配置和运维入口迁移

**Files:**
- Modify: `configure.ac`, `CMakeLists.txt`, `plugins/Makefile.am`, `plugins/Makefile.in`
- Modify: `conf/unimrcpserver.xml`
- Modify: `tools/diagnostics/diagnose.sh`, `README.md`
- Modify: `unimrcp.sln`, `unimrcp-2010.sln`

- [x] **Step 1:** 将 Autotools/CMake target、条件变量和子目录改为 `tts_websocket` / `tts-websocket`。
- [x] **Step 2:** 将 XML engine ID/name 和插件文件检查改为正式名称。
- [x] **Step 3:** 修正 CMake 依赖发现，消除 `/usr/local/opt` 硬编码。
- [x] **Step 4:** 更新 README、诊断脚本和工程解决方案。

### Task 4: 验证与图库更新

**Files:**
- Verify: all active source/config/build references
- Update: codebase-memory graph for `/Users/huangzhonghui/tte-mrcp`

- [x] **Step 1:** 执行 shell/C 语法、XML、引用残留和 `git diff --check` 检查。
- [x] **Step 2:** 运行 `configure --help`、CMake 解析或实际构建；Autotools/CMake 配置均通过，完整 `make` 被既有 `libs/mpf` 中 `JB_TRACE/RTP_TRACE` 与 `mpf_null_trace()` 宏签名不兼容阻断，已在 README 记录。
- [x] **Step 3:** 使用 `index_repository` 重新索引并用 `search_graph` 验证新插件节点。
- [x] **Step 4:** 更新计划状态并报告验证证据和本机限制。
