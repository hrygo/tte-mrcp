# ASR English Debug Logging 实现计划

**目标：** 将默认日志等级设为 DEBUG，并确保 ASR 运行与测试日志文本均使用英文。

**范围：** 修改 `conf/logger.xml`、两处 ASR WebSocket DEBUG 日志，以及
`test_asr_frame_debug_logs.cmake` 的断言和诊断；不改中文文档或注释。

## 实施与验证

1. 先将静态测试断言改为英文并执行，确认旧中文生产日志导致预期失败。
2. 将默认 `<priority>` 从 `INFO` 改为 `DEBUG`。
3. 保持日志级别、参数和控制流不变，只将两条 ASR 运行时文字改为英文。
4. 将所有 ASR 静态测试诊断翻译为英文，并重新运行测试。
5. 解析 XML、扫描活动 ASR 源码与测试中的中文字符，并运行 `git diff --check`。
