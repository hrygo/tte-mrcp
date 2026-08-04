# git diff 变更分析报告

- 分析对象：`git diff HEAD`（HEAD = `c8901b0` tts：0710上线版本）
- 范围：已跟踪文件的暂存 + 未暂存改动；未跟踪文件（`docs/`、`shell/`、`tools/`、构建产物等）不在本报告范围
- 变更规模：93 个文件，+20592 / -45669 行（含 `Makefile.in`、`configure` 等生成文件）

## 1. 变更主线：TTS 插件重命名 `demo-synth` → `tts-websocket`

正式 TTS 插件从旧示例命名整体迁移为正式命名，是全量 diff 的主体。

### 1.1 文件重命名

| 旧路径 | 新路径 |
|---|---|
| `plugins/demo-synth/`（整目录） | `plugins/tts-websocket/` |
| `plugins/demo-synth/src/demo_synth_engine.c` | `plugins/tts-websocket/src/tts_websocket_engine.c` |
| `plugins/demo-synth/tests/test_http_parse.c` | `plugins/tts-websocket/tests/test_tts_websocket_http_parse.c` |
| `plugins/demo-synth/demosynth.vcproj` | `plugins/tts-websocket/tts_websocket.vcproj` |
| `plugins/demo-synth/demosynth.vcxproj` | `plugins/tts-websocket/tts_websocket.vcxproj` |
| `plugins/demo-synth/demosynth.vcxproj.filters` | `plugins/tts-websocket/tts_websocket.vcxproj.filters` |
| `plugins/demo-synth/CMakeLists.txt` | `plugins/tts-websocket/CMakeLists.txt` |
| `data/demo-16kHz.pcm` | `data/tts_websocket-16kHz.pcm` |
| `data/demo-8kHz.pcm` | `data/tts_websocket-8kHz.pcm` |

### 1.2 构建与配置引用同步

- `configure.ac`：`UNI_PLUGIN_ENABLED(demosynth)` → `UNI_PLUGIN_ENABLED(tts_websocket)`，AM_CONDITIONAL 改为 `TTS_WEBSOCKET_PLUGIN`，`AC_CONFIG_FILES` 指向 `plugins/tts-websocket/Makefile`，配置摘要输出同步。
- 根 `CMakeLists.txt`：`ENABLE_DEMOSYNTH_PLUGIN` → `ENABLE_TTS_WEBSOCKET_PLUGIN`，`add_subdirectory(plugins/demo-synth)` → `plugins/tts-websocket`。
- `plugins/Makefile.am`：`DEMOSYNTH_PLUGIN`/`demo-synth` → `TTS_WEBSOCKET_PLUGIN`/`tts-websocket`。
- `plugins/tts-websocket/Makefile.am`（新增）：目标 `tts_websocket.la`，源码 `src/tts_websocket_engine.c`。
- `data/Makefile.am`：`DATAFILES` 使用新文件名与条件宏。
- `conf/unimrcpserver.xml`：`<engine id="Demo-Synth-1" name="demosynth">` → `<engine id="TTS-WebSocket-1" name="tts_websocket">`；`<resource id="speechsynth" engine="Demo-Synth-1">` → `engine="TTS-WebSocket-1"`。
- `tests/mpftest/src/mpf_suite.c`：引用文件 `demo-8kHz.pcm` → `tts_websocket-8kHz.pcm`。
- `diagnose.sh`：插件检查 `./plugin/demosynth.so` → `./plugin/tts_websocket.so`。
- Visual Studio 工程文件：项目名/RootNamespace/源文件路径同步改名，vcxproj 整体重写。

## 2. TTS 引擎核心逻辑修改（`plugins/tts-websocket/src/tts_websocket_engine.c`）

重命名基础上实质性改动 +1488 / -533 行。除标识符 `demo_synth_*`/`demo_tts_*` → `tts_websocket_*`、日志前缀 `zybTTS` → `zyTTS` 外，主要功能变更如下。

### 2.1 新增输出状态机与完成时序控制

- 新增枚举 `tts_websocket_output_state_e`：`IDLE / STREAMING / POSTROLL / RTP_DRAIN / COMPLETE_READY`。
- 新增 `tts_websocket_output_finish_begin()`：EOF 后按配置进入 post-roll（默认 600ms，上限 5000ms，配置参数 `completion-postroll-ms`）→ RTP drain（按 `rtp-ptime-ms` 折算 tick 数）→ COMPLETE_READY，把最后一帧 AUDIO、RTP 打包器 drain、SPEAK-COMPLETE 分离到不同 MPF tick，避免控制通道 SPEAK-COMPLETE 超车尾音 RTP 包。
- `completion_cause` 在 error 时置 `SYNTHESIZER_COMPLETION_CAUSE_ERROR`，否则 NORMAL。

### 2.2 重写 MPF 回调：`tts_websocket_stream_read_safe`

旧 `tts_websocket_stream_read` 整体移入 `#if 0`，由新的实时安全回调取代：

- 永不等待 producer：用 `apr_thread_mutex_trylock`，抢锁失败发静音并统计 `trylock_fail_count`。
- 固定返回 codec frame size，尾帧不足一帧补 `0xFF`（不缩短 MPF 复用 frame 的 `size`）。
- 数据不足一帧且 producer 活跃时保留在 ring 中（`partial_wait_count`），不短读污染复用 frame；仅在 EOF 尾帧允许短读。
- STOP（`stop_response`）、PAUSE、无 `speak_request` 时的快速返回路径。
- SPEAK-COMPLETE 只发一次，同时关闭诊断录音文件。

### 2.3 并发/竞态修复

- `stream_complete` 由线程退出处改为在 `stream_buffer_mutex` 保护下设置并 `cond_broadcast`，消除 reader 无锁读取 `stream_complete` 与 writer 写 buffer 之间的 TOCTOU / 内存排序竞态（防止提前 EOF 导致尾音丢弃）。
- `stream_read_audio` 增加 `eof` 出参，空且完成才算 EOF；"数据不足"与"stream_complete"检查统一在 mutex 内完成。
- 写入后/读取后无条件 `cond_signal`，替代仅高水位才通知的旧逻辑。
- `cleanup_audio` 改为"先置停止标志 + 关闭 socket（10ms 后）→ join"，避免旧实现 join 阻塞最长 30 秒。
- 删除双重 `trylock` 失败静默返回路径，STOP 请求时最后一次非阻塞写入（`ring_bytes_dropped` 统计）。
- STOP 回调中设置 `stream_stop_requested` 并广播条件变量（防 WS 线程阻塞/泄漏）。

### 2.4 内存与帧处理修复

- 接收缓冲 512KB → 2MB；新增线程私有 `frame_pool`，每轮 `apr_pool_clear` 释放重采样/μ-law/合并缓冲，修复 channel pool 无限增长导致的长会话 OOM 丢帧。
- 防御奇数长度二进制帧：截断末字节保持 16-bit 采样对齐。
- 去除句子边界/会话尾"补零 flush"残留字节，改为直接丢弃（最多 4 字节不可闻），消除补零导致的波形跳变爆音。
- 移除每句预缓冲，改为 `stream_prebuffer_min_fill=640B`（约 80ms 音频）的最小缓冲；`audio.start` 到达时清零 `pcm_accum_len`。
- ring buffer 256KB → 512KB。
- `resample_pcm_to_8k` 增加输入大小非 6 字节倍数的防御性告警。

### 2.5 诊断与录音

- 新增时序诊断：首包延迟、逐帧间隔（>50ms 打 WARNING）、session 汇总（总时长/吞吐/最大帧间隔/静音帧数）与 `[DIAG]` 丢音定位日志（`ring_written/ring_read/dropped_stop/silence_frames/trylock_fail/partial_wait`）。
- 新增录音功能（引擎配置参数 `recording-enabled`，默认关闭）：
  - `tts-orig-24kHz-{sid}-{ts}.pcm`：WS 线程写入的原始 24kHz PCM（`recording_write_orig`）。
  - `tts-final-8kHz-{sid}-{ts}.pcmu`：MPF 发帧点写入的 8kHz μ-law，与客户端实际收到的 RTP 载荷逐帧一致（`recording_write_final`），用于区分"插件内部丢音"与"RTP/网络/客户端丢音"。
- 日志前缀 `zybTTS` → `zyTTS`（与 `zyASR` 对齐）。

### 2.6 WebSocket 协议修复

- 新增 `json_get_type()` 精确提取 JSON `type` 字段并 `strcmp` 比较，替代 `strstr("audio.start")` 子串匹配（旧实现会被 `audio_start_time` 等字段名误匹配，导致状态错乱）。
- `generate_websocket_key()` 与 `websocket_send_text()` 移除线程不安全的 `srand/rand` fallback，改用 `apr_generate_random_bytes`（失败时退回时间戳+pid 种子）。
- 握手发送校验完整发送长度（`len != strlen(handshake)` 判失败）。
- 超大 payload 丢弃日志明确标注 `DATA LOSS`。

### 2.7 SPEAK 方法行为收紧

- 强制 codec 采样率必须为 8000（`descriptor->sampling_rate != 8000` 直接 METHOD_FAILED），移除 16000 能力声明（`MPF_SAMPLE_RATE_8000 | MPF_SAMPLE_RATE_16000` → `MPF_SAMPLE_RATE_8000`）。
- 流式启动失败或请求无 body：直接返回 METHOD_FAILED/COMPLETE，删除"时间估算 + 本地文件回退"死代码。
- `speak_request` 改为在流式网络/缓冲初始化成功后才发布。

## 3. 其他插件与脚本

- `plugins/demo-recog/src/demo_recog_engine.c`：日志前缀 `zybASR` → `zyASR`；`funasr_websocket_send_frame` 移除每次发送后的 `apr_sleep(1000)`（1ms 延迟）。
- `plugins/demo-synth/tests/test_http_parse.c` → `test_tts_websocket_http_parse.c`：纯重命名 + 标识符规范化，无测试逻辑变化（13/13 用例基线保持）。
- `umc_test.exp`：日志级别 `-l 4` → `-l 6`；增加启动/超时处理；expect 改为两阶段——Phase 1 等 RECOGNITION/SPEAK-COMPLETE 并按 `Completion-Cause` 判定 SUCCESS/MRCP_ERROR（新增 SIP_ERROR 模式），Phase 2 单独捕获 `Interpretation[].instance[]` NLSML 文本并随 `RESULT:` 输出。
- `stress_test.sh` / `stress_test_simple.sh`：`ROOT_DIR` 硬编码路径 → 基于脚本自身位置的动态推导。
- `stress_test_improved.sh`：大规模增强（+734 行）——新增 ASR 音频轮询选源（`-a`）、ASR 结果 CSV 输出（`-o`）、采样率识别（`-s`）、`--tts-save`/`--tts-var-dir` 录音管理、`data/stress_test_input.pcm` symlink 机制、`-d` 轮间延迟等。
- `README.md`：标题及内容重写为编译/启动/验证/压测使用说明。

## 4. 文件清理

- 删除 `plugin/` 下 4 个预编译 `.so`（demorecog/demosynth/demoverifier/mrcprecorder）。
- 删除 `plugins/demo-synth/` 下 7 份 `.bak*` 备份、`.0525.c`、`.http.c`、`.deps/.dirstamp`、编译产物（`.o/.lo/.libs`）及脚本 `tts_play_with_timer.sh`、测试二进制 `test_http_parse`。
- 删除 `plugins/demo-recog/src/` 下备份文件 `.0525.c`、`.bak`、`.http.c` 及编译产物。

## 5. 生成/派生文件（非手改）

`configure`、`aclocal.m4`、各目录 `Makefile.in`、`config.status` 等为 Autotools 生成物，随 `configure.ac`/`Makefile.am` 变化重新生成，属派生变更。

## 6. 涉及的文件清单

- 构建：`configure.ac`、`CMakeLists.txt`、`plugins/Makefile.am`、`plugins/tts-websocket/Makefile.am`、`data/Makefile.am`、`plugins/tts-websocket/CMakeLists.txt`
- 配置：`conf/unimrcpserver.xml`、`conf/umc-scenarios/recognizer.xml`
- 源码：`plugins/tts-websocket/src/tts_websocket_engine.c`、`plugins/demo-recog/src/demo_recog_engine.c`、`tests/mpftest/src/mpf_suite.c`
- 测试/脚本：`plugins/tts-websocket/tests/test_tts_websocket_http_parse.c`、`umc_test.exp`、`diagnose.sh`、`stress_test*.sh`
- 文档/数据：`README.md`、`data/tts_websocket-*.pcm`
- 工程文件：`plugins/tts-websocket/tts_websocket.vcproj/.vcxproj/.filters`

## 7. 附注

- 未跟踪目录 `docs/reports/`、`docs/superpowers/`、`shell/`、`tools/`、`tests/integration/`、`__pycache__/`、`.vscode/` 及构建产物均不在本 diff 分析范围。
- 本报告基于 `git diff HEAD`（对照 `c8901b0`），行号与文件名以当前工作区为准；如需对照具体代码位置，结合 diff 中 `@@` 上下文行号回溯。
