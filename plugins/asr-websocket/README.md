# ASR WebSocket 插件

`asr-websocket` 是 UniMRCP 的正式 ASR 业务插件，将 MRCP `speechrecog` 资源接入 FunASR WebSocket 服务。它与 `tts-websocket` 同级，承载生产代码、协议测试、并发控制、日志和跨平台构建交付；`funasr_*` 文件名表示协议适配实现，不代表插件仍是示例代码。

## Canonical contract

| 项目 | 正式值 |
|---|---|
| 源码目录 | `plugins/asr-websocket/` |
| Autotools/CMake 目标 | `asr_websocket` |
| 动态库 | `asr_websocket.so`（Windows 为对应 DLL） |
| XML engine | `id="ASR-WebSocket-1" name="asr_websocket"` |
| MRCP 资源 | `speechrecog` |
| 端点参数 | `funasr-host`、`funasr-port`、`funasr-path` |

运行链路为：MRCP `RECOGNIZE` → 音频写入 → 有界 FunASR transport worker → HTTP/WebSocket 解码 → PCM 重采样/队列 → MRCP 识别事件和完成响应。transport 的 generation、STOP/close fence、队列满/空、超时、异常关闭和多会话隔离由插件级单元测试覆盖。

## Configuration migration

旧配置中的 `Demo-Recog-1` / `demorecog` 应迁移为 `ASR-WebSocket-1` / `asr_websocket`；`funasr-host`、`funasr-port`、`funasr-path` 参数保持不变。当前发布不提供旧动态库或 engine 的运行时别名：部署升级必须原子替换 XML 与插件库，不能用旧 XML 加载新安装。兼容窗口仅覆盖 loopback fixture 的旧输入迁移；旧名称从本发布版本起退出正式构建，不延长到后续 release。`tools/diagnostics/funasr_ws_fixture.py` 会移除旧或重复 engine 项并生成一个 canonical 项，避免测试配置继续静默保留旧命名。自定义部署配置应在升级时完成同样的替换，并确认 `speechrecog` 只有一个 engine 映射。

默认日志记录 session、generation、首包/帧时序、音频字节/帧、队列溢出、超时、异常关闭和完成原因；默认不记录完整识别文本、二进制帧或逐帧 INFO 噪声。真实生产灰度和外部 FunASR 服务验证仍是部署方的人工运行验收，不由本地协议测试替代。

## Tests and platform boundary

```sh
cmake -S . -B /tmp/tte-mrcp-asr-websocket \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build /tmp/tte-mrcp-asr-websocket \
  --target test_funasr_ws_transport test_funasr_control test_resample test_json_unescape
ctest --test-dir /tmp/tte-mrcp-asr-websocket -R '^asr_websocket_' --output-on-failure
```

| 平台 | 当前交付边界 |
|---|---|
| macOS Apple Silicon | 源码、协议单测和 CMake/Autotools 配置可在本地验证 |
| Windows Win32/x64 | Visual Studio 工程已同步；本次环境运行时 DLL 加载和 MSBuild 结果未验证 |
| Linux 生产目标 | Autotools 目标和插件命名已同步；本次环境动态库加载、RTP/MRCP 建链和外部 FunASR 冒烟未验证 |
