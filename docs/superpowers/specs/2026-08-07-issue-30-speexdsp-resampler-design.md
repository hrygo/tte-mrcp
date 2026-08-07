# Issue #30：SpeexDSP 流式重采样修复设计

## 背景与根因

Issue #30 的问题音频在 35–37.56 秒出现颤音和电流感。用户已确认同一时段在电话实时播放时即可听到异常，并进一步确认 TTS WebSocket 返回的原始 24 kHz、16-bit、单声道 PCM 正常。因此，客户端下载阶段的 24 kbps MP3 只能放大异常，不能作为起始根因；故障范围收敛到服务端的 24 kHz PCM → 8 kHz PCM → PCMU → RTP 链路。

当前生产代码在 `tts_websocket_engine.c` 中对每三个输入采样求平均后直接抽取。三点移动平均不是满足电话语音带宽要求的抗混叠滤波器：通带存在明显下垂，4 kHz 以上阻带衰减不足，输入中超过 4 kHz 的成分会折叠到 0–4 kHz 电话频带。该失真与语音内容有关，因此可以只在低能量、低基频、谐波性下降的片段表现为颤音或金属感。

本修复使用成熟的 SpeexDSP 有状态流式重采样器替换三点移动平均。修复不改变 TTS WebSocket 协议、PCMU 编码、ring buffer、MPF/RTP 帧长及 SPEAK-COMPLETE 状态机。

## 目标与非目标

### 目标

1. 将 24 kHz PCM S16LE 单声道稳定转换为 8 kHz PCM S16LE 单声道，消除当前抽取算法的内容相关混叠。
2. 使用 SpeexDSP 1.2.1、`quality=10` 和整数 PCM 流式接口。
3. 对任意 WebSocket 分块保持输出一致：连续输入、逐字节输入和随机分块必须产生相同的有效 PCM。
4. 每个 SPEAK 使用独立重采样状态；正常完成时排出滤波器尾部，STOP、错误和 cleanup 路径不泄漏状态。
5. 产物可部署到现有 RHEL 7.9 x86_64 和银河麒麟 V10 aarch64 基线，并将所需 `libspeexdsp.so` 纳入部署包。
6. 保留原始 24 kHz 与最终 8 kHz PCMU 录音能力，用于同会话回归比对。

### 非目标

- 不修改 TTS 模型、voice、WebSocket 消息格式或文本切分策略。
- 不修改 G.711 μ-law 编码算法、RTP packetization、ring buffer 和完成状态机。
- 不以最终 MP3 文件作为修复验收源，音质验收使用原始 PCM、PCMU 解码 PCM 和电话实听。
- 本次部署验收仅覆盖 Linux x86_64 与 Linux aarch64；macOS 和 Windows 标记为未验证，不作为发布门禁。

## 方案选择

### 采用方案：SpeexDSP 1.2.1

生产代码使用 `speex_resampler_process_int()`，每个流创建单声道、24000→8000 Hz、质量等级 10 的 `SpeexResamplerState`。选择理由：

- API 原生支持有状态流式输入、16-bit 整数 PCM、延迟查询、状态重置和错误码。
- 算法面向实时语音，CPU、内存和延迟适合高并发 TTS。
- Linux x86_64 与 aarch64 可使用同一源码版本构建。
- BSD 风格许可适合随服务器安装包分发动态库。

固定版本为 `SpeexDSP-1.2.1`，对应提交 `1b28a0f61bc31162979e1f26f3981fc3637095c8`。CI 从该 tag 构建到 `/opt/tte-mrcp`，校验提交后再参与 TTE-MRCP 构建；部署包继续使用现有非系统共享库收集逻辑携带 `libspeexdsp.so`。

### 未采用方案

1. **继续修补三点移动平均**：无法提供足够阻带衰减，拒绝采用。
2. **项目内自研 FIR/polyphase**：可达到目标，但系数设计、定点溢出、边界 flush 和双架构一致性均需自行长期维护。
3. **SoXR**：实验室指标更高，但 FFT 路径可能引入更高实时延迟，且 LGPL 依赖对最终 8 kHz PCMU 没有足够收益。
4. **libsamplerate**：通用高质量转换可行，但以浮点通用场景为主，对本项目实时语音链路没有超过 SpeexDSP 的综合优势。

## 代码边界与接口

新增独立组件：

- `plugins/tts-websocket/src/tts_websocket_resampler.h`
- `plugins/tts-websocket/src/tts_websocket_resampler.c`
- `plugins/tts-websocket/tests/test_tts_websocket_resampler.c`

该组件只负责 PCM 样本率转换，不依赖 APR、MRCP、WebSocket、ring buffer 或文件系统。建议接口为：

```c
typedef struct tts_websocket_resampler_t tts_websocket_resampler_t;

tts_websocket_resampler_t *tts_websocket_resampler_create(
    unsigned int input_rate,
    unsigned int output_rate,
    unsigned int channels,
    int quality,
    int *error_code);

int tts_websocket_resampler_process(
    tts_websocket_resampler_t *resampler,
    const short *input,
    size_t *input_samples,
    short *output,
    size_t *output_samples);

int tts_websocket_resampler_finish(
    tts_websocket_resampler_t *resampler,
    short *output,
    size_t *output_samples);

size_t tts_websocket_resampler_output_bound(
    const tts_websocket_resampler_t *resampler,
    size_t input_samples);

void tts_websocket_resampler_destroy(
    tts_websocket_resampler_t *resampler);
```

封装层负责：

- 调用 SpeexDSP 并把库错误码映射为稳定的插件返回值；
- 记录累计输入/输出样本数；
- 在正常 EOF 时补入有限零样本排出 FIR 尾部，并把总输出严格限制为 `floor(total_input_samples * 8000 / 24000)`；
- 防止输入未完全消费、输出容量不足和重复 finish 被静默忽略；
- `destroy(NULL)` 安全，所有错误路径可幂等清理。

`tts_websocket_engine.c` 只进行字节流到 `short` 样本的组装、调用封装层、PCM→PCMU 转换和 ring buffer 写入，不直接访问 `SpeexResamplerState`。

## 流式数据与生命周期

```text
WebSocket binary PCM bytes
  → 保留跨消息的单个奇数字节
  → PCM S16LE samples
  → SpeexDSP stateful 24 kHz→8 kHz, quality=10
  → PCM S16LE 8 kHz
  → G.711 μ-law
  → ring buffer
  → MPF 20 ms PCMU frame
  → RTP
```

1. 收到 `audio.start` 后，WebSocket worker 创建本次 SPEAK 的重采样器。
2. 网络分块只要求 16-bit 样本对齐；原有 `pcm_accum` 从 6 字节相位对齐改为保留最多 1 个奇数字节。SpeexDSP 自身维护 3:1 比例相位和滤波历史。
3. 每次 process 必须循环到输入全部消费；输出空间不足时扩展临时输出或继续调用，不能丢弃剩余输入。
4. 收到正常 `session.done` 后调用 finish，排出滤波尾部，再设置 `stream_complete`。
5. WebSocket 错误、STOP 或 cleanup 不生成额外尾部音频；先停止并 join worker，再销毁重采样器，避免和 read callback 交叉释放。
6. 新 SPEAK 不复用旧状态；每次创建或显式 reset，防止前一通话滤波历史污染下一通话。

## 错误策略

- SpeexDSP 初始化失败：本次 SPEAK 返回失败，不连接 TTS 服务，不回退到三点移动平均。
- process 未消费全部输入、返回非成功错误或计数不一致：设置 `stream_error`，终止本次流并以错误原因完成；不得把部分损坏音频伪装为正常完成。
- finish 失败：保留已成功写入的音频，标记错误完成，不发送未经初始化的数据。
- 构建时找不到 `speexdsp >= 1.2.1`：当 TTS WebSocket 插件启用时，Autotools 和 CMake 配置必须立即失败并打印明确依赖名和最低版本。
- 运行时找不到 `libspeexdsp.so`：部署验证中的插件加载检查失败，不允许生成可发布 artifact。

## 构建与交付

### Autotools

- `configure.ac` 仅在启用 `tts_websocket` 插件时通过 pkg-config 要求 `speexdsp >= 1.2.1`。
- `plugins/tts-websocket/Makefile.am` 加入新源文件、`SPEEXDSP_CFLAGS` 和 `SPEEXDSP_LIBS`。
- 运行 `./bootstrap` 重新生成 `configure`、`Makefile.in` 等受控生成文件，不手工编辑生成结果。

### CMake

- 通过现有 pkg-config 机制探测 `speexdsp >= 1.2.1`。
- 插件和重采样单测显式链接 `${SPEEXDSP_LIBRARIES}` 并使用对应 include/library 目录。

### Linux 双架构 CI

- `.github/workflows/build-linux.yml` 新增 `install_speexdsp`，从固定 tag/commit 构建共享库到 `/opt/tte-mrcp`。
- x86_64 继续使用 CentOS 7、GCC 4.8.5、glibc 2.17 基线。
- aarch64 继续使用 Rocky Linux 8 原生 ARM runner、glibc 2.28 基线。
- 两个 artifact 都必须包含 SpeexDSP soname 链；`ldd`、ELF machine 和 GLIBC 上限审计覆盖该库及 `tts_websocket.so`。

## 测试设计

### 单元测试

1. **冲激响应**：输出稳定、无溢出，样本数符合精确 3:1 比例。
2. **直流与低频正弦**：300 Hz、1 kHz、3.4 kHz 通带幅度在验收阈值内。
3. **阻带正弦**：6 kHz、10 kHz 输入降采样后残留能量相对输入至少降低 60 dB。
4. **分块不变性**：整块、1 字节、1 样本和固定种子随机分块的输出逐样本一致。
5. **长度与尾部**：空输入、1–5 个样本、非 3 倍数长度和长流的输出样本数均为 `floor(N/3)`，最后一个有效语音样本不丢失。
6. **状态隔离**：两个连续 SPEAK 的第二段输出与全新重采样器输出一致。
7. **错误路径**：非法采样率、非法 quality、空指针、输出容量不足、重复 finish 和 destroy 均有确定行为。

### 质量回归

- 使用同一份正常 24 kHz 原始问题音频分别通过当前三点平均、SpeexDSP 和 SoXR 高质量参考进行离线转换。
- SpeexDSP 输出与 SoXR 参考比较通带幅度、阻带残留、总样本数和分段 SNR。
- 将 SpeexDSP 输出编码为 PCMU 后解码回 PCM，35–37.56 秒不得再出现原有颤音；试听采用盲测顺序，至少包含当前实现、修复实现和原始 24 kHz 三份样本。

### 集成与部署验证

- TTS WebSocket HTTP、WebSocket parser、PCM、生命周期和新 resampler 单测全部通过。
- 同一文本、voice 和原始 PCM 复现时，服务端 24 kHz 原始录音保持不变，8 kHz PCMU 录音无颤音。
- RHEL 7 x86_64 与麒麟 V10 aarch64 CI 均完成构建、插件动态加载、MRCP/RTP 冒烟和 artifact ABI 审计。
- `git diff --check`、Autotools configure、CMake configure 通过；codebase-memory 重新索引并确认新组件调用链。

## 可观测性

每个 SPEAK 只记录汇总信息，不逐帧打印 PCM：

- resampler 名称、输入/输出采样率、quality；
- 累计输入样本、输出样本、预计输出样本；
- process 调用次数、未消费输入次数、finish 输出样本；
- 初始化、process、finish 的错误码与 SpeexDSP 错误文本；
- 正常完成时断言输出样本数与精确 3:1 目标一致。

日志不得输出完整文本、PCM 内容或 hex dump。现有会打印文本和逐帧 INFO 的历史日志治理不在本修复范围，但新代码不得继续扩大该问题。

## 发布、回滚与验收

### 发布步骤

1. Issue #30 专用分支完成单元测试和本机配置验证。
2. PR 的 Linux 双架构构建、运行和 ABI 门禁全部通过。
3. 在受控环境用问题原文和相同 voice 完成同会话四层录音及电话实听。
4. 先部署单实例或小流量节点，观察初始化错误、输出样本计数和 TTS 完成率，再全量发布。

### 回滚

回滚整个插件和随包 SpeexDSP 动态库到上一已验证 artifact。代码不保留运行时切换回三点移动平均的开关，避免故障时静默恢复已知劣化算法。若 SpeexDSP 初始化失败，应失败可见，而不是输出已知可能失真的音频。

### 完成标准

同时满足以下条件才可关闭 Issue #30：

1. 原始 24 kHz PCM 与修复前一致且正常。
2. 修复后的 8 kHz PCM/PCMU 在 35–37.56 秒无可辨别颤音或电流感。
3. 同一输入任意分块输出一致，输入字节、输出样本和 ring 写入计数闭合。
4. Linux x86_64 与 aarch64 构建、动态加载和 MRCP/RTP 冒烟全部通过。
5. PR 合并后远程 `main` 包含修复提交，Issue 留存测试音频、指标和发布验证结论。
