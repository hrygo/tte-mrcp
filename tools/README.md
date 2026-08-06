# TTE-MRCP 工具目录

## 压测

`tools/stress/` 包含四套现有压测入口：

- `stress_test_improved.sh`：当前推荐的 ASR/TTS 分轮并发压测。
- `stress_test.sh`：基础 Bash 压测。
- `stress_test_simple.sh`：简化压测。
- `stress_test.py`：Python 并发压测。

这些脚本默认把脚本所在仓库作为根目录，也支持显式 `-r`/`--root-dir` 覆盖。压测入口统一从 `tools/stress/` 调用。

## 诊断

`tools/diagnostics/diagnose.sh` 检查 UniMRCP server、TTS/ASR 后端端口、`tts_websocket.so`/`asr_websocket.so` 插件文件和 SIP 单次连通性。ASR endpoint 从 `conf/unimrcpserver.xml` 的 `funasr-host`、`funasr-port`、`funasr-path` 配置读取；可通过 `PLUGIN_DIR` 指定实际插件部署目录。

## 集成测试

`tests/integration/umc_test.exp` 是由压测脚本调用的 Expect 流程，不应与框架单元测试混放。

## 本机边界

`tools/dev/setup_macos.sh` 负责安装或检查 APR、APR-util、Sofia-SIP、CMake、Autotools、pkg-config 和 Expect；`tools/dev/env-macos.sh` 负责加载 Homebrew arm64 路径。工具不会修改用户全局 shell 配置。

完整构建前先执行：

```sh
./tools/dev/setup_macos.sh
. ./tools/dev/env-macos.sh
```
