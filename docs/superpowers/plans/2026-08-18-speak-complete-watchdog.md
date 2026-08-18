# TTS SPEAK-COMPLETE 有界完成实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 superpowers:subagent-driven-development（推荐）或 superpowers:executing-plans 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 在 MPF 完成回调停止推进时，以 error `SPEAK-COMPLETE` 有界终结请求，同时保持正常音频排空和 completion exactly-once。

**架构：** 新建一个无 APR 依赖的 completion 状态模块，负责 generation、终态所有权和 deadline 计算；TTS engine 仍由 MPF callback 完成正常路径，WebSocket worker 在 `session.done` 后以现有 mutex/condition 做有界等待并承担 watchdog 兜底。STOP、cleanup、MPF 和 watchdog 共享同一状态仲裁。

**技术栈：** C99、APR thread mutex/condition、UniMRCP MRCP engine API、CMake/CTest、Autotools、Visual Studio 工程。

---

## 文件结构

- 创建 `plugins/tts-websocket/src/tts_websocket_completion.h`：定义 completion generation、状态、owner 和 deadline 纯函数接口。
- 创建 `plugins/tts-websocket/src/tts_websocket_completion.c`：实现无锁的纯状态转换；调用方必须持有 channel buffer mutex。
- 创建 `plugins/tts-websocket/tests/test_tts_websocket_completion.c`：覆盖 generation、exactly-once、STOP 失效和 deadline 边界。
- 修改 `plugins/tts-websocket/src/tts_websocket_engine.c`：把纯状态模块接入 SPEAK、MPF、watchdog、STOP 和 cleanup。
- 修改 `plugins/tts-websocket/CMakeLists.txt`、`Makefile.am`、`tts_websocket.vcxproj`、`tts_websocket.vcxproj.filters`：三平台构建纳入新模块，CMake/Autotools 纳入测试。

### 任务 1：Completion 纯状态模块

**文件：**
- 创建：`plugins/tts-websocket/src/tts_websocket_completion.h`
- 创建：`plugins/tts-websocket/src/tts_websocket_completion.c`
- 创建：`plugins/tts-websocket/tests/test_tts_websocket_completion.c`
- 修改：`plugins/tts-websocket/CMakeLists.txt`

- [ ] **步骤 1：编写 generation 与 exactly-once 失败测试**

测试依次调用以下接口，断言同一 generation 只有第一个 owner 能 claim，旧 generation 不能完成新请求，STOP 后 watchdog claim 失败：

```c
tts_websocket_completion_begin(&state);
generation = state.generation;
assert(tts_websocket_completion_claim(&state, generation,
    TTS_WEBSOCKET_COMPLETION_OWNER_MPF) == 1);
assert(tts_websocket_completion_claim(&state, generation,
    TTS_WEBSOCKET_COMPLETION_OWNER_WATCHDOG) == 0);
```

- [ ] **步骤 2：编写 deadline 失败测试**

断言 8000 PCMU 字节对应 1000 ms，并累加 post-roll、grace 和最小等待：

```c
assert(tts_websocket_completion_timeout_ms(8000, 200, 1500, 500) == 2700);
assert(tts_websocket_completion_timeout_ms(0, 0, 200, 500) == 500);
```

- [ ] **步骤 3：运行测试确认红灯**

运行：

```sh
cmake -S . -B /tmp/tte-mrcp-issue57 -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build /tmp/tte-mrcp-issue57 --target test_tts_websocket_completion
```

预期：FAIL，新头文件/target 尚不存在。

- [ ] **步骤 4：实现最小纯状态模块**

状态包含 `generation`、`active`、`owner`。`begin` 递增 generation 并激活；`claim` 只允许匹配 generation 的 active 状态转换一次；`cancel` 使当前 generation 失效；timeout 使用上溢安全的整数运算：

```c
uint64_t audio_ms = remaining_pcmu_bytes > UINT64_MAX / 1000
    ? UINT64_MAX : (remaining_pcmu_bytes * 1000 + 7999) / 8000;
uint64_t timeout_ms = saturating_add(audio_ms, postroll_ms, grace_ms);
return timeout_ms < minimum_ms ? minimum_ms : timeout_ms;
```

- [ ] **步骤 5：运行测试确认绿灯**

运行：`cmake --build /tmp/tte-mrcp-issue57 --target test_tts_websocket_completion && ctest --test-dir /tmp/tte-mrcp-issue57 -R tts_websocket_completion --output-on-failure`

预期：completion 测试全部 PASS。

- [ ] **步骤 6：提交纯状态模块**

```sh
git add plugins/tts-websocket/src/tts_websocket_completion.[ch] \
  plugins/tts-websocket/tests/test_tts_websocket_completion.c \
  plugins/tts-websocket/CMakeLists.txt
git commit -m "test(tts): define bounded completion state"
```

### 任务 2：接入正常完成与 watchdog

**文件：**
- 修改：`plugins/tts-websocket/src/tts_websocket_engine.c`
- 修改：`plugins/tts-websocket/tests/test_tts_websocket_completion.c`

- [ ] **步骤 1：增加 watchdog deadline/竞态失败测试**

增加纯状态场景：MPF 在 deadline 前 claim 后 watchdog 必须失败；watchdog 先 claim 后 MPF 必须失败；下一代 SPEAK 不受旧 watchdog 影响。

- [ ] **步骤 2：运行测试验证新增断言失败**

运行：`cmake --build /tmp/tte-mrcp-issue57 --target test_tts_websocket_completion && ctest --test-dir /tmp/tte-mrcp-issue57 -R tts_websocket_completion --output-on-failure`

预期：FAIL，缺少 watchdog eligibility/owner 行为。

- [ ] **步骤 3：实现统一完成 helper**

在 engine 内新增只负责 MRCP 事件构造/发送的 helper。调用者先持 mutex 调用 completion claim，成功后捕获 `speak_request`、清空 active request、广播 condition，再在锁外发送；normal 使用 `SYNTHESIZER_COMPLETION_CAUSE_NORMAL`，watchdog 使用 `SYNTHESIZER_COMPLETION_CAUSE_ERROR`。

- [ ] **步骤 4：在 SPEAK/STOP/cleanup 接入 generation**

SPEAK 发布 request 时调用 `begin` 并保存 generation；STOP 和 cleanup 在 mutex 下 `cancel` 并 broadcast。启动失败也 cancel，确保 worker 不能引用失效请求。

- [ ] **步骤 5：在 MPF callback 接入 normal claim**

`COMPLETE_READY` 不再直接清空并发送；改用统一 helper 取得 MPF owner。若锁竞争则当前 tick 返回，下一 tick重试，不能阻塞实时回调。

- [ ] **步骤 6：在 WebSocket worker 接入 watchdog**

收到 `session.done`、设置 `stream_complete` 并关闭 socket 后，在 buffer mutex 上循环 timedwait。deadline 按剩余 ring PCMU 字节、post-roll、1500 ms grace 和 500 ms minimum 计算。正常完成/STOP/cleanup 唤醒后退出；超时且 generation 仍 active 时 claim watchdog owner并在锁外发送 error completion。

- [ ] **步骤 7：运行 completion 与插件测试**

运行：

```sh
cmake --build /tmp/tte-mrcp-issue57 --target test_tts_websocket_completion test_tts_websocket_lifecycle test_tts_websocket_pcm test_tts_websocket_ws test_tts_websocket_thread
ctest --test-dir /tmp/tte-mrcp-issue57 -R 'tts_websocket_(completion|lifecycle|pcm|ws|thread)' --output-on-failure
```

预期：全部 PASS。

- [ ] **步骤 8：提交 engine 集成**

```sh
git add plugins/tts-websocket/src/tts_websocket_engine.c \
  plugins/tts-websocket/tests/test_tts_websocket_completion.c
git commit -m "fix(tts): bound SPEAK completion after session done"
```

### 任务 3：同步跨平台构建入口并验证

**文件：**
- 修改：`plugins/tts-websocket/Makefile.am`
- 修改：`plugins/tts-websocket/tts_websocket.vcxproj`
- 修改：`plugins/tts-websocket/tts_websocket.vcxproj.filters`
- 生成：`plugins/tts-websocket/Makefile.in`（由 Autotools 生成，不手工编辑）

- [ ] **步骤 1：将 completion 源文件加入所有插件构建目标**

CMake 已在任务 1 纳入；Autotools 的 `tts_websocket_la_SOURCES` 加入 `.c/.h`，并新增 completion `check_PROGRAMS`；Visual Studio 工程加入 `ClCompile`、`ClInclude` 和对应 filter。

- [ ] **步骤 2：重新生成 Autotools 文件**

运行：`./bootstrap`

预期：`Makefile.in` 等生成文件与 `Makefile.am` 同步；只保留与本任务相关的生成差异。

- [ ] **步骤 3：运行最低验证门禁**

```sh
git diff --check
find tools -type f -name "*.sh" -print0 | xargs -0 bash -n
./configure --help >/tmp/tte-mrcp-issue57-configure-help.txt
cmake -S . -B /tmp/tte-mrcp-issue57 -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build /tmp/tte-mrcp-issue57 --target test_tts_websocket_completion test_tts_websocket_http_parse
ctest --test-dir /tmp/tte-mrcp-issue57 -R 'tts_websocket_' --output-on-failure
```

预期：所有可运行检查 PASS。Windows/Linux 真实构建若当前 macOS 环境无法执行，在 PR 中明确标记未验证，不以 macOS 结果替代。

- [ ] **步骤 4：重新索引并核查关键节点**

使用 codebase-memory `index_repository(mode="fast")`，再查询 `tts_websocket_completion_claim`、`tts_websocket_stream_thread` 和 `tts_websocket_stream_read_safe`，确认 canonical 节点与完成调用链存在。

- [ ] **步骤 5：提交构建与生成文件**

```sh
git add plugins/tts-websocket/Makefile.am plugins/tts-websocket/Makefile.in \
  plugins/tts-websocket/tts_websocket.vcxproj \
  plugins/tts-websocket/tts_websocket.vcxproj.filters
git commit -m "build(tts): include completion watchdog module"
```

### 任务 4：审查与发布

**文件：**
- 审查本分支相对 `origin/main` 的全部变更。

- [ ] **步骤 1：请求代码审查**

向 code-reviewer 提供 Issue #57、设计规格、计划、`origin/main` SHA 和 HEAD SHA；修复所有 Critical/Important 反馈并重跑相关测试。

- [ ] **步骤 2：检查提交和工作区**

```sh
git status -sb
git log --oneline origin/main..HEAD
git diff --stat origin/main...HEAD
git diff --check origin/main...HEAD
```

预期：工作区干净，提交仅覆盖 Issue #57。

- [ ] **步骤 3：推送 Issue 分支并创建草稿 PR**

```sh
git push -u origin fix/issue-57-speak-complete-watchdog
```

PR 目标为 `main`，标题使用 `fix(tts): guarantee bounded SPEAK completion`，正文包含根因、正常/超时语义、验证结果、Windows/Linux 未验证项，并使用 `Closes #57`。
